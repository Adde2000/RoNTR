// plugin.cpp — RoN Tactical Radio TeamSpeak 3 client plugin.
// Reads game state from shared memory (gamelink.hpp) and applies proximity /
// radio DSP (radio_dsp.hpp) to incoming voice in onEditPlaybackVoiceDataEvent.
//
// Build against the official TS3 Client Plugin SDK (API 26):
//   https://github.com/teamspeak/ts3client-pluginsdk
// Install: %APPDATA%/TS3Client/plugins/ron_tactical_radio.dll

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "teamspeak/public_definitions.h"
#include "teamspeak/public_rare_definitions.h"
#include "teamspeak/public_errors.h"
#include "ts3_functions.h"
#include "plugin_definitions.h"

#include "gamelink.hpp"
#include "radio_dsp.hpp"

static struct TS3Functions ts3Functions;

#define PLUGIN_API_VERSION 26
#define PLUGIN_NAME "RoN Tactical Radio"
#define PLUGIN_VERSION "0.4.0"

#ifdef _WIN32
#define RTR_EXPORT __declspec(dllexport)
#else
#define RTR_EXPORT __attribute__((visibility("default")))
#endif

static char* s_pluginID = nullptr;
static rtr::GameLink s_link;
static std::thread s_pollThread;
static std::atomic<bool> s_running{false};

// Per-remote-client state, guarded by s_mtx (audio + poll threads touch it).
struct RemoteClient {
    std::string nickname;      // TS nickname (fallback matching)
    std::string gameName;      // from HELLO
    uint32_t    txFreqKhz = 0; // nonzero while transmitting on radio
    rtr::RadioEffect radio;
    rtr::PanState pan;
    uint64_t    lastAudioLogMs = 0; // throttled diagnostics
    float       amp = 0.0f;         // smoothed loudness 0..1 (drives mouth anim)
    uint64_t    lastAudioMs = 0;    // last time we processed audio from them
};
static std::mutex s_mtx;
static std::map<anyID, RemoteClient> s_clients;
static uint64 s_sch = 0; // active server connection

static uint64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------- helpers ---

static void tsLog(const std::string& msg)
{
    // Shows in TS3: Tools > Client Log (channel "RoNTacticalRadio").
    ts3Functions.logMessage(msg.c_str(), LogLevel_INFO, "RoNTacticalRadio", 0);
}

static std::string clientNickname(uint64 sch, anyID clientID)
{
    char* name = nullptr;
    if (ts3Functions.getClientVariableAsString(sch, clientID, CLIENT_NICKNAME, &name) != ERROR_ok)
        return {};
    std::string out = name ? name : "";
    ts3Functions.freeMemory(name);
    return out;
}

static void sendCmd(const std::string& cmd)
{
    if (!s_pluginID || !s_sch) return;
    ts3Functions.sendPluginCommand(s_sch, s_pluginID, cmd.c_str(),
                                   PluginCommandTarget_CURRENT_CHANNEL, nullptr, nullptr);
}

// Reverse channel: tell the game mod who is audible and how loud, so it can
// drive character mouth animation (VoipMouthAlpha) in sync with TS speech.
static SOCKET s_talkSock = INVALID_SOCKET;

static void sendTalkMsg(const RtrTalkMsg& msg)
{
    if (s_talkSock == INVALID_SOCKET) {
        s_talkSock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s_talkSock == INVALID_SOCKET) return;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(RTR_TALK_UDP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::sendto(s_talkSock, reinterpret_cast<const char*>(&msg), sizeof(msg), 0,
             reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
}

// Called from the poll thread every 50ms.
static void publishTalkers(uint64_t now)
{
    RtrTalkMsg msg{};
    msg.magic = RTR_MAGIC;
    msg.version = RTR_VERSION;
    std::lock_guard<std::mutex> lk(s_mtx);
    for (auto& [id, rc] : s_clients) {
        if (msg.count >= RTR_TALK_MAX) break;
        if (now - rc.lastAudioMs > 250) { rc.amp *= 0.8f; continue; } // fading out
        const std::string& name = !rc.gameName.empty() ? rc.gameName : rc.nickname;
        if (name.empty()) continue;
        RtrTalkSpeaker& sp = msg.speakers[msg.count++];
        std::snprintf(sp.name, RTR_NAME_LEN, "%s", name.c_str());
        sp.amplitude = rc.amp;
    }
    sendTalkMsg(msg); // sent even when empty so mouths close promptly
}

// Rename own TS nickname to the in-game name while playing (makes matching
// obvious for humans; the HELLO handshake already handles it for the plugin).
static std::string s_origNick; // restored when leaving the game

static void syncNickname(const char* gameName)
{
    if (!s_sch || !gameName || !gameName[0]) return;
    char* nick = nullptr;
    if (ts3Functions.getClientSelfVariableAsString(s_sch, CLIENT_NICKNAME, &nick) != ERROR_ok)
        return;
    const std::string current = nick ? nick : "";
    ts3Functions.freeMemory(nick);
    if (current == gameName) return;
    if (s_origNick.empty()) s_origNick = current;
    ts3Functions.setClientSelfVariableAsString(s_sch, CLIENT_NICKNAME, gameName);
    ts3Functions.flushClientSelfUpdates(s_sch, nullptr);
    tsLog("nickname set to in-game name '" + std::string(gameName) + "'");
}

static void restoreNickname()
{
    if (!s_sch || s_origNick.empty()) return;
    ts3Functions.setClientSelfVariableAsString(s_sch, CLIENT_NICKNAME, s_origNick.c_str());
    ts3Functions.flushClientSelfUpdates(s_sch, nullptr);
    tsLog("nickname restored to '" + s_origNick + "'");
    s_origNick.clear();
}

static void setMicOpen(bool open)
{
    if (!s_sch) return;
    ts3Functions.setClientSelfVariableAsInt(s_sch, CLIENT_INPUT_DEACTIVATED,
                                            open ? INPUT_ACTIVE : INPUT_DEACTIVATED);
    ts3Functions.flushClientSelfUpdates(s_sch, nullptr);
}

// Poll thread: pump shared memory, gate mic, broadcast radio state changes.
static void pollLoop()
{
    bool lastRadioPtt = false;
    bool sentHello = false;
    uint32_t lastFreq = 0;
    int micApplied = -1; // -1 = TS default (not gated), 0 = closed, 1 = open

    while (s_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // The plugin may load AFTER TS connected to a server, in which case no
        // connect event fires - resolve the active connection lazily.
        if (s_sch == 0) {
            uint64* list = nullptr;
            if (ts3Functions.getServerConnectionHandlerList(&list) == ERROR_ok && list) {
                for (int i = 0; list[i] != 0; ++i) {
                    int status = 0;
                    if (ts3Functions.getConnectionStatus(list[i], &status) == ERROR_ok && status > 0) {
                        s_sch = list[i];
                        tsLog("attached to existing server connection");
                        break;
                    }
                }
                ts3Functions.freeMemory(list);
            }
        }

        const bool ok = s_link.poll(nowMs());
        const RtrSharedState st = s_link.snapshot();
        publishTalkers(nowMs());

        if (!ok || !st.inGame) {
            if (micApplied != -1) { setMicOpen(true); micApplied = -1; } // restore normal TS
            if (sentHello) { tsLog("game link lost (menu or game closed)"); restoreNickname(); }
            lastRadioPtt = false;
            sentHello = false;
            continue;
        }

        if (!sentHello) {
            tsLog(std::string("game link active: local player '") + st.localName +
                  "', " + std::to_string(st.playerCount) + " player(s) in state");
            sendCmd(std::string("HELLO name=") + st.localName);
            syncNickname(st.localName);
            sentHello = true;
        }

        const uint32_t freq = st.radioFreqKhz[st.activeRadio % RTR_MAX_RADIOS];
        if (freq != lastFreq) {
            // (M3) broadcast full freq list; single active freq for now
            sendCmd("FREQ freqs=" + std::to_string(freq));
            lastFreq = freq;
        }

        const bool radioPtt = st.radioPtt != 0;
        if (radioPtt != lastRadioPtt) {
            sendCmd(radioPtt ? ("TX_START freq=" + std::to_string(freq)) : "TX_STOP");
            tsLog(radioPtt ? "radio PTT down (TX_START)" : "radio PTT up (TX_STOP)");
            lastRadioPtt = radioPtt;
        }

        // Local voice is always on: the mic is never force-closed (use TS's
        // Voice Activity Detection or continuous capture as you prefer).
        // Radio PTT just makes sure input is active while held.
        const int wantMic = 1;
        if (wantMic != micApplied) {
            setMicOpen(true); // also un-sticks installs of the old gating version
            micApplied = wantMic;
        }
    }
}

// ------------------------------------------------------- required exports ---

extern "C" {

RTR_EXPORT const char* ts3plugin_name()        { return PLUGIN_NAME; }
RTR_EXPORT const char* ts3plugin_version()     { return PLUGIN_VERSION; }
RTR_EXPORT int         ts3plugin_apiVersion()  { return PLUGIN_API_VERSION; }
RTR_EXPORT const char* ts3plugin_author()      { return "Ethan Baxter"; }
RTR_EXPORT const char* ts3plugin_description()
{
    return "TFAR-style proximity voice and radio for Ready or Not.";
}

RTR_EXPORT void ts3plugin_setFunctionPointers(const struct TS3Functions funcs)
{
    ts3Functions = funcs;
}

RTR_EXPORT int ts3plugin_init()
{
    const bool shm = s_link.open(); // ok to fail now; poll() retries via snapshot staleness
    s_running = true;
    s_pollThread = std::thread(pollLoop);
    tsLog(std::string("plugin loaded, shared memory ") +
          (shm ? "found (game already running)" : "not present yet (start RoN with the mod)"));
    return 0;
}

RTR_EXPORT void ts3plugin_shutdown()
{
    s_running = false;
    if (s_pollThread.joinable()) s_pollThread.join();
    s_link.close();
    if (s_pluginID) { free(s_pluginID); s_pluginID = nullptr; }
}

RTR_EXPORT void ts3plugin_registerPluginID(const char* id)
{
    const size_t sz = strlen(id) + 1;
    s_pluginID = (char*)malloc(sz);
    memcpy(s_pluginID, id, sz);
}

// Track which server connection is active.
RTR_EXPORT void ts3plugin_onConnectStatusChangeEvent(uint64 sch, int newStatus, unsigned int /*err*/)
{
    if (newStatus == STATUS_CONNECTION_ESTABLISHED) s_sch = sch;
    if (newStatus == STATUS_DISCONNECTED && s_sch == sch) s_sch = 0;
}

// ------------------------------------------------------------ radio sync ---

RTR_EXPORT void ts3plugin_onPluginCommandEvent(uint64 sch, const char* pluginName,
                                    const char* pluginCommand, anyID invokerClientID,
                                    const char* /*invokerName*/, const char* /*invokerUID*/)
{
    if (strcmp(pluginName, "ron_tactical_radio") != 0) return;
    std::string cmd = pluginCommand ? pluginCommand : "";
    std::lock_guard<std::mutex> lk(s_mtx);
    RemoteClient& rc = s_clients[invokerClientID];
    if (rc.nickname.empty()) rc.nickname = clientNickname(sch, invokerClientID);

    if (cmd.rfind("HELLO ", 0) == 0) {
        rc.gameName = cmd.substr(cmd.find("name=") + 5);
        tsLog("mapped TS client '" + rc.nickname + "' -> game player '" + rc.gameName + "'");
    } else if (cmd.rfind("TX_START ", 0) == 0) {
        rc.txFreqKhz = (uint32_t)std::stoul(cmd.substr(cmd.find("freq=") + 5));
    } else if (cmd.rfind("TX_STOP", 0) == 0) {
        rc.txFreqKhz = 0;
    }
}

// ---------------------------------------------------------- audio pipeline ---
// NOTE: processing happens in onEditPostProcessVoiceDataEvent, NOT
// onEditPlaybackVoiceDataEvent - the latter only carries the decoded MONO
// stream, so stereo panning there is a silent no-op. Here we get the output
// speaker layout (channelSpeakerArray) and can fill left/right differently.

RTR_EXPORT void ts3plugin_onEditPostProcessVoiceDataEvent(
    uint64 sch, anyID clientID, short* samples, int sampleCount, int channels,
    const unsigned int* channelSpeakerArray, unsigned int* channelFillMask)
{
    std::lock_guard<std::mutex> lk(s_mtx);
    RemoteClient& rc = s_clients[clientID];
    if (rc.nickname.empty()) rc.nickname = clientNickname(sch, clientID);

    const RtrSharedState st = s_link.snapshot();
    if (!st.inGame) return; // menus/lobby: untouched TS audio
    if (*channelFillMask == 0) return;

    // Mix the filled channels down to one mono float buffer in [-1, 1].
    static thread_local std::vector<float> mono;
    mono.assign(size_t(sampleCount), 0.0f);
    int filled = 0;
    for (int c = 0; c < channels; ++c) {
        if (!(*channelFillMask & (1u << c))) continue;
        ++filled;
        for (int i = 0; i < sampleCount; ++i)
            mono[size_t(i)] += samples[i * channels + c] / 32768.0f;
    }
    if (filled == 0) return;
    for (auto& v : mono) v /= float(filled);

    // Loudness for mouth animation: smoothed RMS of the incoming voice.
    {
        double sum = 0.0;
        for (float v : mono) sum += double(v) * double(v);
        const float rms = float(std::sqrt(sum / std::max(1, sampleCount)));
        rc.amp = 0.6f * rc.amp + 0.4f * std::min(1.0f, rms * 8.0f);
        rc.lastAudioMs = nowMs();
    }

    // Locate left/right output channels.
    int li = -1, ri = -1;
    for (int c = 0; c < channels; ++c) {
        const unsigned int spk = channelSpeakerArray[c];
        if (spk == SPEAKER_FRONT_LEFT  || spk == SPEAKER_HEADPHONES_LEFT)  li = c;
        if (spk == SPEAKER_FRONT_RIGHT || spk == SPEAKER_HEADPHONES_RIGHT) ri = c;
    }

    // Radio path?
    bool radio = false;
    if (rc.txFreqKhz != 0)
        for (uint32_t f : st.radioFreqKhz)
            if (f != 0 && f == rc.txFreqKhz) { radio = true; break; }

    float gain = 1.0f, pan = 0.0f;
    const uint64_t now = nowMs();
    if (radio) {
        rc.radio.processMono(mono.data(), sampleCount);
    } else {
        const std::string& matchName = !rc.gameName.empty() ? rc.gameName : rc.nickname;
        const rtr::SpeakerAudio q = s_link.query(matchName);
        if (!q.found) {
            if (now - rc.lastAudioLogMs > 3000) {
                rc.lastAudioLogMs = now;
                tsLog("no game match for TS client '" + rc.nickname + "' (passthrough)");
            }
            return; // unmodded player: normal TS audio
        }
        gain = rtr::distanceGain(q.distM);
        // Cap pan so a speaker dead to one side is still faintly audible in
        // the far ear (real heads leak sound around; full pan feels unnatural).
        constexpr float kMaxPan = 0.85f;
        pan = std::clamp(q.pan, -kMaxPan, kMaxPan);
        if (now - rc.lastAudioLogMs > 3000) {
            rc.lastAudioLogMs = now;
            char buf[160];
            std::snprintf(buf, sizeof(buf), "audio '%s': dist=%.1fm pan=%+.2f gain=%.2f",
                          matchName.c_str(), q.distM, pan, gain);
            tsLog(buf);
        }
    }

    // Write output: clear everything that was filled, then fill L/R (or all,
    // if no stereo pair is available) with smoothed gain/pan.
    for (int c = 0; c < channels; ++c) {
        if (*channelFillMask & (1u << c))
            for (int i = 0; i < sampleCount; ++i) samples[i * channels + c] = 0;
    }
    const bool stereo = (li >= 0 && ri >= 0 && li != ri);
    const float smooth = 0.002f;
    for (int i = 0; i < sampleCount; ++i) {
        rc.pan.gain += std::clamp(gain - rc.pan.gain, -smooth, smooth);
        rc.pan.pan  += std::clamp(pan  - rc.pan.pan,  -smooth, smooth);
        const float v = std::clamp(mono[size_t(i)], -1.0f, 1.0f);
        if (stereo) {
            const float theta = (rc.pan.pan + 1.0f) * 0.25f * 3.14159265f; // 0..pi/2
            samples[i * channels + li] = short(v * rc.pan.gain * std::cos(theta) * 32767.0f);
            samples[i * channels + ri] = short(v * rc.pan.gain * std::sin(theta) * 32767.0f);
        } else {
            for (int c = 0; c < channels; ++c)
                if (*channelFillMask & (1u << c))
                    samples[i * channels + c] = short(v * rc.pan.gain * 32767.0f);
        }
    }
    if (stereo) *channelFillMask |= (1u << li) | (1u << ri);
}

} // extern "C"
