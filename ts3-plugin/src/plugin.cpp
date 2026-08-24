// plugin.cpp — RoN Tactical Radio TeamSpeak 3 client plugin.
// Reads game state from shared memory (gamelink.hpp) and applies proximity /
// radio DSP (radio_dsp.hpp) to incoming voice in onEditPlaybackVoiceDataEvent.
//
// Build against the official TS3 Client Plugin SDK (API 26):
//   https://github.com/teamspeak/ts3client-pluginsdk
// Install: %APPDATA%/TS3Client/plugins/ron_tactical_radio.dll

#define PLUGIN_VERSION "0.6.0" // keep in step with game-mod ModVersion / release tag
#define PLUGIN_NAME "RoN Tactical Radio"
#define PLUGIN_API_VERSION 26

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
#include "httpbridge.hpp"
#include "radio_dsp.hpp"
#include "settings.hpp"
#include "config_win.hpp"

#include <filesystem>

static struct TS3Functions ts3Functions;

#ifdef _WIN32
#define RTR_EXPORT __declspec(dllexport)
#else
#define RTR_EXPORT __attribute__((visibility("default")))
#endif

static char* s_pluginID = nullptr;
static rtr::GameLink s_link;
static rtr::HttpBridge s_http; // REST transport for other games (docs/HTTP-BRIDGE.md)
static bool s_httpRunning = false; // init + poll thread only (sequenced)
static std::thread s_pollThread;
static std::atomic<bool> s_running{false};

// Per-remote-client state, guarded by s_mtx (audio + poll threads touch it).
struct RemoteClient {
    std::string nickname;      // TS nickname (fallback matching)
    std::string gameName;      // from HELLO
    std::string pluginVersion; // from INFO (shown in the client info panel)
    std::string game;          // from INFO; "" = not connected to a game
    bool        infoKnown = false; // an INFO was received from this client
    uint32_t    txFreqKhz = 0; // nonzero while transmitting on radio
    rtr::RadioEffect radio;
    rtr::PanState pan;
    rtr::OcclusionState occl;  // wall muffling (proximity path only)
    rtr::CompressorState comp; // voice leveler (proximity path only)
    uint64_t    lastAudioLogMs = 0; // throttled diagnostics
    float       amp = 0.0f;         // smoothed loudness 0..1 (drives mouth anim)
    uint64_t    lastAudioMs = 0;    // last time we processed audio from them
};
static std::mutex s_mtx;
static std::map<anyID, RemoteClient> s_clients;
static uint64 s_sch = 0; // active server connection

// Live-tunable DSP settings (settings.hpp). Guarded by s_mtx: the audio
// callback copies them under its existing lock, the poll thread hot-reloads
// the ini (~1s), and /rtr chat commands set values from the UI thread.
static rtr::Settings s_settings;
static std::string s_settingsPath;                     // <TS config>/ron_tactical_radio.ini
static std::filesystem::file_time_type s_settingsMtime;

// Which game this client is currently linked to, for the INFO announce and
// our own info panel: "ready-or-not" while a native (shm/UDP) link is fresh,
// the REST bridge's game= id while HTTP owns the link, "" when no game.
// Guarded by s_mtx (poll thread writes, bridge + UI threads read/write).
static std::string s_infoGame;
static std::string s_bridgeGame; // last game= id accepted from the REST bridge

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

// Value of "key=" in a space-separated command ("INFO ver=0.5.0 game=x").
static std::string cmdToken(const std::string& cmd, const char* key)
{
    const size_t at = cmd.find(key);
    if (at == std::string::npos) return {};
    const size_t start = at + std::strlen(key);
    const size_t end = cmd.find(' ', start);
    return cmd.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// INFO announce: plugin version + current game, shown in other clients'
// info panel. Server-wide so version is visible across channels; sent on
// connect and whenever the game changes, plus once targeted back at each
// newly-seen plugin user (late-join discovery).
static std::string infoCmd(std::string game)
{
    std::string cmd = "INFO ver=" PLUGIN_VERSION;
    for (auto& c : game) if (std::isspace((unsigned char)c)) c = '-';
    if (!game.empty()) cmd += " game=" + game;
    return cmd;
}

static void sendInfoBroadcast(const std::string& game)
{
    if (!s_pluginID || !s_sch) return;
    ts3Functions.sendPluginCommand(s_sch, s_pluginID, infoCmd(game).c_str(),
                                   PluginCommandTarget_SERVER, nullptr, nullptr);
}

static void sendInfoTo(anyID client, const std::string& game)
{
    if (!s_pluginID || !s_sch) return;
    const anyID ids[2] = {client, 0};
    ts3Functions.sendPluginCommand(s_sch, s_pluginID, infoCmd(game).c_str(),
                                   PluginCommandTarget_CLIENT, ids, nullptr);
}

// ------------------------------------------------------- tunable settings ---

// Load (or create) the settings ini in the TS config directory.
static void initSettings()
{
    char cfgDir[512]{};
    ts3Functions.getConfigPath(cfgDir, sizeof(cfgDir));
    std::string dir = cfgDir;
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';
    s_settingsPath = dir + "ron_tactical_radio.ini";

    rtr::Settings loaded;
    if (rtr::settingsLoad(loaded, s_settingsPath)) {
        tsLog("settings loaded from " + s_settingsPath);
    } else {
        rtr::settingsSave(loaded, s_settingsPath); // write commented defaults
        tsLog("settings file created: " + s_settingsPath);
    }
    std::error_code ec;
    s_settingsMtime = std::filesystem::last_write_time(s_settingsPath, ec);
    std::lock_guard<std::mutex> lk(s_mtx);
    s_settings = loaded;
}

// Diagnostics (debug.log setting): dump the received game state every ~2s so
// transmitted positions can be checked against the game's own logs. Runs on
// the HTTP bridge thread (single-threaded, so the plain statics are fine).
static void maybeLogState(const RtrSharedState& st)
{
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        if (s_settings.debugLog < 0.5f) return;
    }
    static uint64_t lastMs = 0;
    const uint64_t now = nowMs();
    if (now - lastMs < 2000) return;
    lastMs = now;

    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "state: ingame=%d me='%s' lpos=(%.1f,%.1f,%.1f) fwd=(%.2f,%.2f,%.2f) players=%u",
                  int(st.inGame), st.localName,
                  st.listenerPos.x, st.listenerPos.y, st.listenerPos.z,
                  st.listenerFwd.x, st.listenerFwd.y, st.listenerFwd.z,
                  st.playerCount);
    tsLog(buf);
    for (uint32_t i = 0; i < st.playerCount && i < RTR_MAX_PLAYERS; ++i) {
        const RtrPlayer& p = st.players[i];
        const float dx = p.pos.x - st.listenerPos.x;
        const float dy = p.pos.y - st.listenerPos.y;
        const float dz = p.pos.z - st.listenerPos.z;
        std::snprintf(buf, sizeof(buf),
                      "  '%s' pos=(%.1f,%.1f,%.1f) dist=%.1fm alive=%d occl=%d",
                      p.name, p.pos.x, p.pos.y, p.pos.z,
                      std::sqrt(dx * dx + dy * dy + dz * dz),
                      int(p.alive), int(p.occlusion));
        tsLog(buf);
    }
}

// Persist current settings; refreshes the stored mtime so the hot reload
// doesn't re-trigger on our own write.
static bool saveSettingsToFile()
{
    rtr::Settings copy;
    { std::lock_guard<std::mutex> lk(s_mtx); copy = s_settings; }
    const bool ok = rtr::settingsSave(copy, s_settingsPath);
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(s_settingsPath, ec);
    { std::lock_guard<std::mutex> lk(s_mtx); s_settingsMtime = mtime; }
    return ok;
}

// Poll-thread: re-read the ini when its timestamp changes (edit + save while
// playing = new values within ~1s).
static void reloadSettingsIfChanged()
{
    if (s_settingsPath.empty()) return;
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(s_settingsPath, ec);
    if (ec) return;
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        if (mtime == s_settingsMtime) return;
        s_settingsMtime = mtime;
    }
    rtr::Settings loaded; // defaults + file so removed keys fall back cleanly
    if (!rtr::settingsLoad(loaded, s_settingsPath)) return;
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        s_settings = loaded;
    }
    tsLog("settings reloaded from " + s_settingsPath);
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

// Our own mic: onEditPostProcessVoiceDataEvent only sees REMOTE voices, so
// self-talk comes from TS's talk-status callback instead. The game uses it
// to light the native talking indicators for the local player.
static std::atomic<bool> s_selfTalking{false};

// Snapshot of who is audible right now (including ourselves). Shared by the
// UDP reverse channel (RoN) and the HTTP bridge response (Arma Reforger).
static RtrTalkMsg buildTalkMsg(uint64_t now)
{
    RtrTalkMsg msg{};
    msg.magic = RTR_MAGIC;
    msg.version = RTR_VERSION;
    std::lock_guard<std::mutex> lk(s_mtx);
    for (auto& [id, rc] : s_clients) {
        if (msg.count >= RTR_TALK_MAX) break;
        if (now - rc.lastAudioMs > 250) continue; // fading out
        const std::string& name = !rc.gameName.empty() ? rc.gameName : rc.nickname;
        if (name.empty()) continue;
        RtrTalkSpeaker& sp = msg.speakers[msg.count++];
        std::snprintf(sp.name, RTR_NAME_LEN, "%s", name.c_str());
        sp.amplitude = rc.amp;
    }
    if (s_selfTalking.load() && msg.count < RTR_TALK_MAX) {
        const RtrSharedState st = s_link.snapshot();
        if (st.localName[0] != '\0') {
            RtrTalkSpeaker& sp = msg.speakers[msg.count++];
            std::snprintf(sp.name, RTR_NAME_LEN, "%s", st.localName);
            sp.amplitude = 0.8f; // no self RMS available; fixed level is fine
        }
    }
    return msg;
}

// Called from the poll thread every 50ms. Owns the fade-out decay so the
// HTTP thread's buildTalkMsg calls don't double the decay rate.
static void publishTalkers(uint64_t now)
{
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        for (auto& [id, rc] : s_clients)
            if (now - rc.lastAudioMs > 250) rc.amp *= 0.8f;
    }
    sendTalkMsg(buildTalkMsg(now)); // sent even when empty so mouths close promptly
}

// Start/stop the REST listener to match the bridge.enable/bridge.port
// settings. Called from init and then the poll thread (~1s), so /rtr set,
// the ini hot-reload, and the config dialog all apply live; a port change
// restarts the listener; a busy port is retried at the same cadence
// (logged throttled).
static uint16_t s_httpPort = 0; // port the running listener is bound to
static void syncHttpBridge()
{
    bool want;
    uint16_t port;
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        want = s_settings.bridgeEnable >= 0.5f;
        port = uint16_t(s_settings.bridgePort + 0.5f);
    }
    if (s_httpRunning && want && s_httpPort != port) {
        s_http.stop();
        s_httpRunning = false;
        tsLog("http bridge restarting (bridge.port -> " + std::to_string(port) + ")");
    }
    if (want && !s_httpRunning) {
        s_httpRunning = s_http.start(port,
            [](const RtrSharedState& st, const std::string& gameId) {
                if (!s_link.injectState(st, nowMs())) {
                    static uint64_t lastIgnoreLogMs = 0; // bridge thread only
                    const uint64_t now = nowMs();
                    if (now - lastIgnoreLogMs > 10000) {
                        lastIgnoreLogMs = now;
                        tsLog("http state ignored: a native game link (shm/UDP) is active");
                    }
                    return;
                }
                {
                    std::lock_guard<std::mutex> lk(s_mtx);
                    s_bridgeGame = gameId;
                }
                maybeLogState(st);
            },
            [] { return buildTalkMsg(nowMs()); });
        if (s_httpRunning) {
            s_httpPort = port;
            tsLog("http bridge listening on 127.0.0.1:" + std::to_string(port));
        } else {
            static uint64_t lastFailLogMs = 0;
            const uint64_t now = nowMs();
            if (lastFailLogMs == 0 || now - lastFailLogMs > 30000) {
                lastFailLogMs = now;
                tsLog("http bridge could not bind 127.0.0.1:" +
                      std::to_string(port) + " (port in use?) - retrying");
            }
        }
    } else if (!want && s_httpRunning) {
        s_http.stop();
        s_httpRunning = false;
        tsLog("http bridge stopped (bridge.enable = 0)");
    }
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
    uint64_t lastActiveMs = 0; // last time the game link was active
    uint64 infoSch = 0;        // connection the last INFO announce went to
    std::string lastSentGame;  // game id in that announce

    int cfgTick = 0;
    while (s_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        if (++cfgTick >= 20) { cfgTick = 0; reloadSettingsIfChanged(); syncHttpBridge(); }

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
        if (ok) maybeLogState(st); // debug.log dumps for shm/UDP games too

        // The version gate rejects mismatched game mods SILENTLY by design
        // (clean passthrough) — but say so, or it looks like a dead plugin.
        static uint64_t lastVerWarnMs = 0;
        const uint32_t peerVer = s_link.mismatchedPeerVersion();
        if (peerVer != 0 && nowMs() - lastVerWarnMs > 10000) {
            lastVerWarnMs = nowMs();
            tsLog("game mod speaks protocol v" + std::to_string(peerVer) +
                  " but this plugin expects v" + std::to_string(RTR_VERSION) +
                  " - update the game mod (mod and plugin ship as a pair)");
        }

        // Current game for the INFO announce / own info panel. Runs before the
        // not-in-game bail-out: the announce must also go out when leaving.
        {
            std::string game;
            if (ok) {
                if (s_link.nativeFresh(nowMs())) game = "ready-or-not";
                else { std::lock_guard<std::mutex> lk(s_mtx); game = s_bridgeGame; }
            }
            { std::lock_guard<std::mutex> lk(s_mtx); s_infoGame = game; }
            if (s_sch != 0) {
                bool changed = game != lastSentGame;
                // Loading screens flap the link; hold the old game a while
                // (same hysteresis as the nickname restore).
                if (changed && game.empty() && lastActiveMs != 0 &&
                    nowMs() - lastActiveMs <= 15000)
                    changed = false;
                if (s_sch != infoSch || changed) {
                    sendInfoBroadcast(game);
                    infoSch = s_sch;
                    lastSentGame = game;
                }
            }
        }

        if (!ok || !st.inGame) {
            if (micApplied != -1) { setMicOpen(true); micApplied = -1; } // restore normal TS
            if (sentHello) tsLog("game link inactive (loading screen or menu)");
            // Hysteresis: loading screens toggle the link constantly - only
            // restore the nickname after we've been out of game for a while.
            if (lastActiveMs != 0 && nowMs() - lastActiveMs > 15000) restoreNickname();
            lastRadioPtt = false;
            sentHello = false;
            continue;
        }
        lastActiveMs = nowMs();

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

// "Settings" button in TeamSpeak's Plugins dialog. On Windows this opens a
// native slider window (config_win.hpp) whose changes apply live; elsewhere
// it opens the ini in the default editor (hot-reloaded ~1s by the poll
// thread). The /rtr chat command remains available on all platforms.
RTR_EXPORT int ts3plugin_offersConfigure() { return PLUGIN_OFFERS_CONFIGURE_NEW_THREAD; }

RTR_EXPORT void ts3plugin_configure(void* /*handle*/, void* /*qParentWidget*/)
{
#ifdef _WIN32
    rtrcfg::Host host;
    host.get = [] {
        std::lock_guard<std::mutex> lk(s_mtx);
        return s_settings;
    };
    host.set = [](const rtr::Settings& s) {
        std::lock_guard<std::mutex> lk(s_mtx);
        s_settings = s;
    };
    host.save = [] { return saveSettingsToFile(); };
    host.iniPath = s_settingsPath;
    rtrcfg::runDialog(std::move(host));
#else
    const std::string cmd = "xdg-open '" + s_settingsPath + "' &";
    if (std::system(cmd.c_str()) != 0)
        tsLog("could not open settings file: " + s_settingsPath);
#endif
}

RTR_EXPORT int ts3plugin_init()
{
    initSettings();
    const bool shm = s_link.open(); // ok to fail now; poll() retries via snapshot staleness
    s_http.setLogger([](const std::string& msg) { tsLog(msg); });
    s_http.setPluginVersion(PLUGIN_VERSION);
    syncHttpBridge(); // REST listener up-front if enabled; poll thread keeps it in sync
    s_running = true;
    s_pollThread = std::thread(pollLoop);
    tsLog(std::string("plugin loaded, shared memory ") +
          (shm ? "mapping present (game running, or another process still holds it)"
               : "not present yet (start RoN with the mod)"));
    return 0;
}

RTR_EXPORT void ts3plugin_shutdown()
{
    s_running = false;
    if (s_pollThread.joinable()) s_pollThread.join();
    s_http.stop();
    s_link.close();
    if (s_pluginID) { free(s_pluginID); s_pluginID = nullptr; }
}

RTR_EXPORT void ts3plugin_registerPluginID(const char* id)
{
    const size_t sz = strlen(id) + 1;
    s_pluginID = (char*)malloc(sz);
    memcpy(s_pluginID, id, sz);
}

// Own-mic talk state (remote voices go through the audio callback instead).
RTR_EXPORT void ts3plugin_onTalkStatusChangeEvent(uint64 sch, int status,
                                                  int /*isReceivedWhisper*/, anyID clientID)
{
    anyID myID = 0;
    if (ts3Functions.getClientID(sch, &myID) != ERROR_ok) return;
    if (clientID == myID)
        s_selfTalking = (status == STATUS_TALKING);
}

// Track which server connection is active.
RTR_EXPORT void ts3plugin_onConnectStatusChangeEvent(uint64 sch, int newStatus, unsigned int /*err*/)
{
    if (newStatus == STATUS_CONNECTION_ESTABLISHED) s_sch = sch;
    if (newStatus == STATUS_DISCONNECTED && s_sch == sch) {
        s_sch = 0;
        // Client IDs are per-connection; a reconnect reuses them for
        // different people, so stale entries would show wrong info.
        std::lock_guard<std::mutex> lk(s_mtx);
        s_clients.clear();
    }
}

// Forget clients that leave the server (newChannelID 0 = disconnected), so a
// reused client ID can't inherit the previous user's version/game.
RTR_EXPORT void ts3plugin_onClientMoveEvent(uint64 /*sch*/, anyID clientID, uint64 /*oldChannelID*/,
                                            uint64 newChannelID, int /*visibility*/, const char* /*moveMessage*/)
{
    if (newChannelID != 0) return;
    std::lock_guard<std::mutex> lk(s_mtx);
    s_clients.erase(clientID);
}

RTR_EXPORT void ts3plugin_onClientMoveTimeoutEvent(uint64 /*sch*/, anyID clientID, uint64 /*oldChannelID*/,
                                                   uint64 /*newChannelID*/, int /*visibility*/, const char* /*timeoutMessage*/)
{
    std::lock_guard<std::mutex> lk(s_mtx);
    s_clients.erase(clientID);
}

// ------------------------------------------------------------- info panel ---
// Right-hand info frame when a client is selected in the tree: shows whether
// they run this plugin, its version, and (optionally) the connected game.
// Data arrives via the INFO plugin command; freeMemory is required by the SDK
// for the info text to display at all.

RTR_EXPORT const char* ts3plugin_infoTitle() { return PLUGIN_NAME; }

RTR_EXPORT void ts3plugin_infoData(uint64 sch, uint64 id, enum PluginItemType type, char** data)
{
    if (type != PLUGIN_CLIENT) { *data = nullptr; return; }

    std::string text;
    anyID myID = 0;
    if (ts3Functions.getClientID(sch, &myID) == ERROR_ok && (anyID)id == myID) {
        std::lock_guard<std::mutex> lk(s_mtx);
        text = "plugin v" PLUGIN_VERSION;
        if (!s_infoGame.empty()) text += "\ngame: " + s_infoGame;
    } else {
        std::lock_guard<std::mutex> lk(s_mtx);
        const auto it = s_clients.find((anyID)id);
        if (it != s_clients.end() && it->second.infoKnown) {
            text = "plugin v" + it->second.pluginVersion;
            if (!it->second.game.empty()) text += "\ngame: " + it->second.game;
        } else {
            text = "plugin not detected";
        }
    }
    *data = (char*)malloc(text.size() + 1);
    if (*data) memcpy(*data, text.c_str(), text.size() + 1);
}

RTR_EXPORT void ts3plugin_freeMemory(void* data) { free(data); }

// ------------------------------------------------ live tuning chat command ---
// /rtr show | /rtr set <key> <value> | /rtr save | /rtr reload | /rtr reset
// Changes apply to the next audio frame; 'save' persists them to the ini.

RTR_EXPORT const char* ts3plugin_commandKeyword() { return "rtr"; }

RTR_EXPORT int ts3plugin_processCommand(uint64 /*sch*/, const char* command)
{
    const auto reply = [](const std::string& msg) {
        ts3Functions.printMessageToCurrentTab(("[RoNTacticalRadio] " + msg).c_str());
    };

    std::istringstream in(command ? command : "");
    std::string verb, key, val;
    in >> verb >> key >> val;
    verb = rtr::settingsLowerTrim(verb);

    if (verb == "show" || verb.empty()) {
        std::lock_guard<std::mutex> lk(s_mtx);
        reply("settings (" + s_settingsPath + "):\n" + rtr::settingsDescribe(s_settings));
    } else if (verb == "set" && !key.empty() && !val.empty()) {
        char* end = nullptr;
        const float f = std::strtof(val.c_str(), &end);
        if (end == val.c_str()) { reply("not a number: '" + val + "'"); return 0; }
        std::string err;
        std::lock_guard<std::mutex> lk(s_mtx);
        if (!rtr::settingsSet(s_settings, key, f, &err)) { reply(err); return 0; }
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s = %g (use '/rtr save' to keep)",
                      rtr::settingsLowerTrim(key).c_str(), double(f));
        reply(buf);
    } else if (verb == "save") {
        reply(saveSettingsToFile() ? "saved to " + s_settingsPath
                                   : "save FAILED: " + s_settingsPath);
    } else if (verb == "reload") {
        rtr::Settings loaded;
        if (rtr::settingsLoad(loaded, s_settingsPath)) {
            std::lock_guard<std::mutex> lk(s_mtx);
            s_settings = loaded;
            reply("reloaded from " + s_settingsPath);
        } else {
            reply("reload FAILED: " + s_settingsPath);
        }
    } else if (verb == "reset") {
        std::lock_guard<std::mutex> lk(s_mtx);
        s_settings = rtr::Settings{};
        reply("settings reset to defaults (in memory; '/rtr save' to persist)");
    } else {
        reply("usage: /rtr show | set <key> <value> | save | reload | reset");
    }
    return 0; // handled
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
    } else if (cmd.rfind("INFO", 0) == 0) {
        anyID myID = 0;
        if (ts3Functions.getClientID(sch, &myID) == ERROR_ok && invokerClientID == myID)
            return; // our own server-wide broadcast echoed back
        const bool firstInfo = !rc.infoKnown;
        rc.pluginVersion = cmdToken(cmd, "ver=");
        if (rc.pluginVersion.empty()) rc.pluginVersion = "unknown";
        rc.game = cmdToken(cmd, "game=");
        rc.infoKnown = true;
        // Late-join discovery: answer a newly-seen plugin user with our own
        // INFO so both sides populate. Second-generation INFOs hit an
        // already-known client and stop here — no echo loop.
        if (firstInfo) sendInfoTo(invokerClientID, s_infoGame);
        // Refresh the info panel if this client is currently selected.
        ts3Functions.requestInfoUpdate(sch, PLUGIN_CLIENT, invokerClientID);
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
    const rtr::Settings cfg = s_settings; // stable copy for this frame

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

    // Radio path? Union the ear routing of every matching slot: a speaker
    // heard on a left-ear AND a right-ear radio plays in both.
    bool radio = false, earL = false, earR = false;
    if (rc.txFreqKhz != 0)
        for (int s = 0; s < RTR_MAX_RADIOS; ++s) {
            if (st.radioFreqKhz[s] == 0 || st.radioFreqKhz[s] != rc.txFreqKhz) continue;
            radio = true;
            if (st.radioEars[s] == 1) earL = true;
            else if (st.radioEars[s] == 2) earR = true;
            else { earL = true; earR = true; } // 0 = both
        }

    float gain = 1.0f, pan = 0.0f;
    const uint64_t now = nowMs();
    if (radio) {
        rc.radio.processMono(mono.data(), sampleCount, cfg.radioDrive, cfg.radioNoise);
        // Ear routing via the pan stage: -1/+1 puts the constant-power writer
        // fully on one output channel; both ears keeps today's centered mix.
        if (earL != earR) pan = earL ? -1.0f : 1.0f;
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
        // TFAR-style leveler first, on the raw voice: mic loudness evens out
        // between speakers while distance attenuation below stays intact.
        if (cfg.voiceComp >= 0.5f) {
            rtr::CompressorParams cp;
            cp.threshold = cfg.voiceCompThresh;
            cp.ratio     = cfg.voiceCompRatio;
            cp.makeup    = cfg.voiceCompMakeup;
            rtr::applyCompressor(mono.data(), sampleCount, rc.comp, 48000.0f, cp);
        }
        gain = rtr::distanceGain(q.distM, {cfg.proxMaxDistM, cfg.proxRolloff});
        // Cap pan so a speaker dead to one side is still faintly audible in
        // the far ear (real heads leak sound around; full pan feels unnatural).
        pan = std::clamp(q.pan, -cfg.proxMaxPan, cfg.proxMaxPan);
        // Muffle through walls. Radio path skips this: radio exists to beat
        // walls. Occlusion has its own slow slew inside applyOcclusion.
        rtr::OcclusionParams op;
        op.minCutoffHz   = cfg.occlMinCutoffHz;
        op.bypassHz      = cfg.occlBypassHz;
        op.maxAtten      = cfg.occlMaxAtten;
        op.slewPerSample = 1.0f / (cfg.occlSlewMs * 0.001f * 48000.0f);
        rtr::applyOcclusion(mono.data(), sampleCount,
                            q.occlusion01 * cfg.occlStrength, rc.occl, 48000.0f, op);
        if (now - rc.lastAudioLogMs > 3000) {
            rc.lastAudioLogMs = now;
            char buf[160];
            std::snprintf(buf, sizeof(buf), "audio '%s': dist=%.1fm pan=%+.2f gain=%.2f occl=%.2f",
                          matchName.c_str(), q.distM, pan, gain, q.occlusion01);
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
    const float smooth = cfg.proxSmooth;
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
