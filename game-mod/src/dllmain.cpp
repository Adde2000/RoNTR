// RoNTacticalRadio game-side mod (UE4SS C++ mod).
// Polls player/camera state each tick and publishes it to shared memory
// for the TeamSpeak 3 plugin. See shared/RadioLink.h for the contract.
//
// Build: as a UE4SS C++ mod against RE-UE4SS (see CMakeLists.txt / README).
// Install: <game>/Binaries/Win64/ue4ss/Mods/RoNTacticalRadio/dlls/main.dll
//
// Class/property names verified against the RoN UHT dump (see docs/DESIGN.md).

#define RTR_MOD_VERSION L"0.5.1" // keep in step with ts3-plugin PLUGIN_VERSION / release tag

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h> // must precede Windows.h
#include <ws2tcpip.h>
#include <Windows.h>
#pragma comment(lib, "ws2_32.lib")
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#include <Mod/CppUserModBase.hpp>
#include <UE4SSProgram.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/FString.hpp>
#include <Unreal/FProperty.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/Property/FStructProperty.hpp>
#include <Unreal/AActor.hpp>
#include <DynamicOutput/DynamicOutput.hpp>

#include "../../shared/RadioLink.h"

using namespace RC;
using namespace RC::Unreal;

namespace {

// ------------------------------------------------------------- config ------
// Read from ue4ss/Mods/RoNTacticalRadio/config.ini (next to the mod folder).
// A commented default file is written on first run if none exists.

struct RtrConfig {
    int      pttKey   = VK_CAPITAL; // radio PTT
    int      voiceKey = 'V';        // reserved (local voice is always on)
    int      cycleKey = VK_OEM_6;   // ']' cycle radio channel
    uint32_t freqKhz[RTR_MAX_RADIOS] = {246000, 247000, 0, 0};
    uint32_t updateMs = 50;         // publish interval (default 20 Hz)
};

// Directory containing this DLL (…/Mods/RoNTacticalRadio/dlls/).
std::wstring ModuleDir()
{
    HMODULE mod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ModuleDir), &mod);
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(mod, path, MAX_PATH);
    std::wstring s = path;
    const size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? s : s.substr(0, slash);
}

// "CAPSLOCK" / "F5" / "MOUSE4" / "A" / "0x14" -> virtual-key code, -1 if unknown.
int ParseKeyName(std::string s)
{
    for (auto& c : s) c = (char)toupper((unsigned char)c);
    static const std::unordered_map<std::string, int> named = {
        {"CAPSLOCK", VK_CAPITAL}, {"TAB", VK_TAB}, {"SPACE", VK_SPACE},
        {"ENTER", VK_RETURN}, {"BACKSPACE", VK_BACK}, {"ESCAPE", VK_ESCAPE},
        {"SHIFT", VK_SHIFT}, {"LSHIFT", VK_LSHIFT}, {"RSHIFT", VK_RSHIFT},
        {"CTRL", VK_CONTROL}, {"LCTRL", VK_LCONTROL}, {"RCTRL", VK_RCONTROL},
        {"ALT", VK_MENU}, {"LALT", VK_LMENU}, {"RALT", VK_RMENU},
        {"LBRACKET", VK_OEM_4}, {"RBRACKET", VK_OEM_6},
        {"SEMICOLON", VK_OEM_1}, {"APOSTROPHE", VK_OEM_7}, {"GRAVE", VK_OEM_3},
        {"COMMA", VK_OEM_COMMA}, {"PERIOD", VK_OEM_PERIOD},
        {"SLASH", VK_OEM_2}, {"BACKSLASH", VK_OEM_5},
        {"MINUS", VK_OEM_MINUS}, {"EQUALS", VK_OEM_PLUS},
        {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT},
        {"INSERT", VK_INSERT}, {"DELETE", VK_DELETE}, {"HOME", VK_HOME},
        {"END", VK_END}, {"PAGEUP", VK_PRIOR}, {"PAGEDOWN", VK_NEXT},
        {"MOUSE3", VK_MBUTTON}, {"MOUSE4", VK_XBUTTON1}, {"MOUSE5", VK_XBUTTON2},
    };
    if (auto it = named.find(s); it != named.end()) return it->second;
    if (s.size() == 1 && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= '0' && s[0] <= '9')))
        return s[0];
    if (s.size() >= 2 && s[0] == 'F') { // F1-F24
        const int n = atoi(s.c_str() + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    if (s.rfind("NUMPAD", 0) == 0 && s.size() == 7 && isdigit((unsigned char)s[6]))
        return VK_NUMPAD0 + (s[6] - '0');
    if (s.rfind("0X", 0) == 0) return (int)strtol(s.c_str(), nullptr, 16);
    return -1;
}

const char* kDefaultConfig =
    "; RoN Tactical Radio - game mod configuration\n"
    "; Key names: A-Z, 0-9, F1-F24, CAPSLOCK, TAB, SPACE, ENTER, SHIFT, CTRL,\n"
    ";   ALT, LBRACKET, RBRACKET, SEMICOLON, APOSTROPHE, GRAVE, COMMA, PERIOD,\n"
    ";   SLASH, BACKSLASH, MINUS, EQUALS, arrows, NUMPAD0-9, MOUSE3/4/5,\n"
    ";   or a raw hex virtual-key code like 0x14.\n"
    "\n"
    "[Keybinds]\n"
    "RadioPTT = CAPSLOCK\n"
    "CycleChannel = RBRACKET\n"
    "\n"
    "[Radio]\n"
    "; Frequencies in kHz. 0 disables a slot. CycleChannel switches slots.\n"
    "Freq1 = 246000\n"
    "Freq2 = 247000\n"
    "Freq3 = 0\n"
    "Freq4 = 0\n"
    "\n"
    "[Advanced]\n"
    "; State publish rate in Hz (5-60).\n"
    "UpdateHz = 20\n";

RtrConfig LoadConfig()
{
    RtrConfig cfg{};
    // dlls/ -> parent folder (Mods/RoNTacticalRadio/config.ini)
    std::wstring dir = ModuleDir();
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir = dir.substr(0, slash);
    const std::wstring path = dir + L"\\config.ini";

    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(h, kDefaultConfig, (DWORD)strlen(kDefaultConfig), &written, nullptr);
            CloseHandle(h);
        }
    }

    wchar_t wbuf[64]{};
    auto readKey = [&](const wchar_t* key, int fallback) {
        GetPrivateProfileStringW(L"Keybinds", key, L"", wbuf, 64, path.c_str());
        char nbuf[64]{};
        WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, nbuf, sizeof(nbuf), nullptr, nullptr);
        const int vk = ParseKeyName(nbuf);
        return vk > 0 ? vk : fallback;
    };
    cfg.pttKey   = readKey(L"RadioPTT", cfg.pttKey);
    cfg.cycleKey = readKey(L"CycleChannel", cfg.cycleKey);

    const wchar_t* freqKeys[RTR_MAX_RADIOS] = {L"Freq1", L"Freq2", L"Freq3", L"Freq4"};
    for (int i = 0; i < RTR_MAX_RADIOS; ++i)
        cfg.freqKhz[i] = GetPrivateProfileIntW(L"Radio", freqKeys[i], cfg.freqKhz[i], path.c_str());

    uint32_t hz = GetPrivateProfileIntW(L"Advanced", L"UpdateHz", 20, path.c_str());
    hz = hz < 5 ? 5 : (hz > 60 ? 60 : hz);
    cfg.updateMs = 1000 / hz;
    return cfg;
}

uint64_t NowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

class SharedMemWriter
{
public:
    bool Open()
    {
        m_mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                       0, sizeof(RtrSharedState), RTR_SHM_NAME);
        if (!m_mapping) return false;
        m_view = static_cast<RtrSharedState*>(
            MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(RtrSharedState)));
        if (!m_view) { CloseHandle(m_mapping); m_mapping = nullptr; return false; }
        std::memset((void*)m_view, 0, sizeof(RtrSharedState));
        m_view->magic = RTR_MAGIC;
        m_view->version = RTR_VERSION;
        return true;
    }

    // Seqlock write: odd while writing, even when consistent.
    void Publish(const RtrSharedState& s)
    {
        if (!m_view) return;
        uint32_t seq = m_view->sequence + 1; // -> odd
        m_view->sequence = seq;
        MemoryBarrier();
        // copy everything after the header fields we manage here
        RtrSharedState tmp = s;
        tmp.magic = RTR_MAGIC;
        tmp.version = RTR_VERSION;
        tmp.sequence = seq;
        std::memcpy((void*)m_view, &tmp, sizeof(tmp));
        MemoryBarrier();
        m_view->sequence = seq + 1; // -> even
    }

    ~SharedMemWriter()
    {
        if (m_view) UnmapViewOfFile((void*)m_view);
        if (m_mapping) CloseHandle(m_mapping);
    }

private:
    HANDLE m_mapping{};
    RtrSharedState* m_view{};
};

// Listens for talk-state messages from the TS plugin (RTR_TALK_UDP_PORT) so
// the game can play mouth animation whenever the player speaks in TeamSpeak.
class TalkReceiver
{
public:
    bool Open()
    {
        m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (m_sock == INVALID_SOCKET) return false;
        u_long nonblock = 1;
        ioctlsocket(m_sock, FIONBIO, &nonblock);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(RTR_TALK_UDP_PORT);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(m_sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
            closesocket(m_sock); m_sock = INVALID_SOCKET;
            return false;
        }
        return true;
    }
    // Drains pending messages; returns true and fills `out` with the newest.
    bool Poll(RtrTalkMsg& out)
    {
        if (m_sock == INVALID_SOCKET) return false;
        bool got = false;
        for (;;) {
            RtrTalkMsg msg{};
            const int n = recvfrom(m_sock, reinterpret_cast<char*>(&msg), sizeof(msg), 0, nullptr, nullptr);
            if (n != (int)sizeof(msg)) break;
            if (msg.magic == RTR_MAGIC && msg.version == RTR_VERSION) {
                out = msg;
                got = true;
            }
        }
        return got;
    }
    ~TalkReceiver() { if (m_sock != INVALID_SOCKET) closesocket(m_sock); }
private:
    SOCKET m_sock = INVALID_SOCKET;
};

// UDP loopback mirror of the shared-memory state. Lets a native-Linux
// TeamSpeak client receive game data when the game runs under Proton.
class UdpSender
{
public:
    bool Open()
    {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (m_sock == INVALID_SOCKET) return false;
        m_addr.sin_family = AF_INET;
        m_addr.sin_port = htons(RTR_UDP_PORT);
        m_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return true;
    }
    void Send(const RtrSharedState& s)
    {
        if (m_sock == INVALID_SOCKET) return;
        sendto(m_sock, reinterpret_cast<const char*>(&s), sizeof(s), 0,
               reinterpret_cast<const sockaddr*>(&m_addr), sizeof(m_addr));
    }
    ~UdpSender() { if (m_sock != INVALID_SOCKET) closesocket(m_sock); }
private:
    SOCKET m_sock = INVALID_SOCKET;
    sockaddr_in m_addr{};
};

} // namespace

class RoNTacticalRadioMod : public CppUserModBase
{
public:
    RoNTacticalRadioMod()
    {
        ModName = STR("RoNTacticalRadio");
        ModVersion = RTR_MOD_VERSION;
        ModDescription = STR("Publishes player positions + radio state for the TS3 plugin");
        ModAuthors = STR("Ethan Baxter");
    }

    void on_unreal_init() override
    {
        m_cfg = LoadConfig();
        m_shmOk = m_shm.Open();
        m_udpOk = m_udp.Open();
        m_talkRx.Open();
        Output::send<LogLevel::Verbose>(STR("[RoNTacticalRadio] shared memory {}, udp {}\n"),
                                        m_shmOk ? STR("ready") : STR("FAILED"),
                                        m_udpOk ? STR("ready") : STR("FAILED"));
        Output::send<LogLevel::Verbose>(
            STR("[RoNTacticalRadio] config: ptt=0x{:X} cycle=0x{:X} freqs={},{},{},{} kHz rate={}ms\n"),
            m_cfg.pttKey, m_cfg.cycleKey,
            m_cfg.freqKhz[0], m_cfg.freqKhz[1], m_cfg.freqKhz[2], m_cfg.freqKhz[3],
            m_cfg.updateMs);
    }

    // on_update wraps the real tick in SEH: during level transitions,
    // reflected calls can touch objects that are mid-destruction (a much
    // wider window under Wine/Proton, where this crashed the game). A
    // faulted tick is skipped; the next tick runs against the settled world.
    void on_update() override
    {
        __try { TickBody(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { ++m_faults; }
    }

    void TickBody()
    {
        if (m_faults != m_faultsLogged) {
            m_faultsLogged = m_faults;
            Output::send<LogLevel::Warning>(
                STR("[RoNTacticalRadio] skipped {} faulted tick(s) during level transition\n"),
                m_faults);
        }

        // Throttle to ~20 Hz; on_update runs every frame.
        const uint64_t now = NowMs();
        if (now - m_lastPublish < m_cfg.updateMs) return;
        m_lastPublish = now;
        if (!m_shmOk && !m_udpOk) return;

        RtrSharedState s{};
        s.timestampMs = now;
        s.radioPtt  = (GetAsyncKeyState(m_cfg.pttKey)   & 0x8000) ? 1 : 0;
        s.voicePtt  = (GetAsyncKeyState(m_cfg.voiceKey) & 0x8000) ? 1 : 0;
        if (s.radioPtt != m_prevRadioPtt) {
            Output::send<LogLevel::Verbose>(STR("[RoNTacticalRadio] radio PTT {}\n"),
                                            s.radioPtt ? STR("DOWN") : STR("UP"));
            m_prevRadioPtt = s.radioPtt;
        }
        if (s.voicePtt != m_prevVoicePtt) {
            Output::send<LogLevel::Verbose>(STR("[RoNTacticalRadio] voice PTT {}\n"),
                                            s.voicePtt ? STR("DOWN") : STR("UP"));
            m_prevVoicePtt = s.voicePtt;
        }
        HandleChannelCycle(s);
        for (int i = 0; i < RTR_MAX_RADIOS; ++i) s.radioFreqKhz[i] = m_cfg.freqKhz[i];
        s.activeRadio = m_activeRadio;

        CollectGameState(s);
        if (m_shmOk) m_shm.Publish(s);
        if (m_udpOk) { s.magic = RTR_MAGIC; s.version = RTR_VERSION; m_udp.Send(s); }
        PumpTalkState(s.inGame != 0);

        // Heartbeat every 5s so the pipeline is visible in UE4SS.log / console.
        if (now - m_lastLog >= 5000) {
            m_lastLog = now;
            Output::send<LogLevel::Verbose>(
                STR("[RoNTacticalRadio] inGame={} players={} listener=({:.1f},{:.1f},{:.1f}) fwd=({:.2f},{:.2f},{:.2f}) radioPtt={} freq={}kHz\n"),
                (int)s.inGame, s.playerCount,
                s.listenerPos.x, s.listenerPos.y, s.listenerPos.z,
                s.listenerFwd.x, s.listenerFwd.y, s.listenerFwd.z,
                (int)s.radioPtt,
                s.radioFreqKhz[s.activeRadio % RTR_MAX_RADIOS]);
        }
    }

private:
    void HandleChannelCycle(RtrSharedState& s)
    {
        const bool down = (GetAsyncKeyState(m_cfg.cycleKey) & 0x8000) != 0;
        if (down && !m_cycleWasDown) {
            // Advance to the next enabled (nonzero) frequency slot.
            for (int i = 1; i <= RTR_MAX_RADIOS; ++i) {
                const uint8_t next = uint8_t((m_activeRadio + i) % RTR_MAX_RADIOS);
                if (m_cfg.freqKhz[next] != 0) { m_activeRadio = next; break; }
            }
        }
        m_cycleWasDown = down;
        (void)s;
    }

    // ---- UE 5.3 reflected-call helpers (LWC: FVector/FRotator are doubles) ----
    struct FVecD { double X{}, Y{}, Z{}; };
    struct FRotD { double Pitch{}, Yaw{}, Roll{}; };

    static UFunction* Fn(UObject* obj, const wchar_t* name)
    {
        return obj ? obj->GetFunctionByNameInChain(name) : nullptr;
    }

    static bool CallVec(UObject* obj, const wchar_t* name, FVecD& out)
    {
        UFunction* fn = Fn(obj, name);
        if (!fn) return false;
        struct { FVecD ReturnValue{}; } params{};
        obj->ProcessEvent(fn, &params);
        out = params.ReturnValue;
        return true;
    }

    static bool CallRot(UObject* obj, const wchar_t* name, FRotD& out)
    {
        UFunction* fn = Fn(obj, name);
        if (!fn) return false;
        struct { FRotD ReturnValue{}; } params{};
        obj->ProcessEvent(fn, &params);
        out = params.ReturnValue;
        return true;
    }

    static UObject* CallObj(UObject* obj, const wchar_t* name)
    {
        UFunction* fn = Fn(obj, name);
        if (!fn) return nullptr;
        struct { UObject* ReturnValue{}; } params{};
        obj->ProcessEvent(fn, &params);
        return params.ReturnValue;
    }

    static bool CallBool(UObject* obj, const wchar_t* name, bool defaultVal)
    {
        UFunction* fn = Fn(obj, name);
        if (!fn) return defaultVal;
        struct { bool ReturnValue{}; } params{};
        obj->ProcessEvent(fn, &params);
        return params.ReturnValue;
    }

    static void CallNameUtf8(UObject* obj, const wchar_t* name, char* out, size_t outLen)
    {
        out[0] = '\0';
        UFunction* fn = Fn(obj, name);
        if (!fn) return;
        struct { Unreal::FString ReturnValue{}; } params{};
        obj->ProcessEvent(fn, &params);
        auto chars = params.ReturnValue.GetCharArray(); // TArray<TCHAR>, NUL-terminated
        const wchar_t* w = chars.Num() > 0
            ? reinterpret_cast<const wchar_t*>(chars.GetData()) : nullptr;
        if (w) WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)outLen, nullptr, nullptr);
        out[outLen - 1] = '\0';
    }

    static RtrVec3 ToMeters(const FVecD& v)
    {
        return {float(v.X / 100.0), float(v.Y / 100.0), float(v.Z / 100.0)};
    }

    // ---- Occlusion: walls between camera and each speaker (docs/OCCLUSION.md)
    // UKismetSystemLibrary::LineTraceSingle called through ProcessEvent with a
    // hand-built param frame. Every field offset is resolved at runtime from
    // the UFunction's own property layout, so an engine update shifts values
    // instead of memory: a failed resolve disables occlusion, it never
    // corrupts the frame.

    // Camera farther than this from the possessed pawn = a menu/map camera
    // (loadout, mission select). First-person + camera shake never exceeds
    // a few meters; menu scenes are hundreds away.
    static constexpr double kCamDetachCm     = 1000.0; // 10 m

    static constexpr int    kMaxWallCount    = 4;      // trace cap per speaker
    static constexpr int    kOcclusionPerWall = 85;    // RtrPlayer::occlusion units
    static constexpr double kHeadOffsetCm    = 160.0;  // pawn root -> approx head
    static constexpr double kAdvanceCm       = 10.0;   // step past each hit
    static constexpr double kMaxTraceRangeCm = 4500.0; // past audible range: skip

    struct TraceCall {
        UObject*   ksl = nullptr; // KismetSystemLibrary CDO
        UFunction* fn  = nullptr;
        int32_t frameSize = 0;
        int32_t offCtx = -1, offStart = -1, offEnd = -1, offChannel = -1,
                offComplex = -1, offIgnore = -1, offDraw = -1, offOutHit = -1,
                offIgnoreSelf = -1, offRet = -1;
        int32_t offHitLoc = -1; // FHitResult::Location inside OutHit; -1 = binary mode
        bool ok = false;
    };

    // Raw view of the TArray<AActor*> param. The native thunk reads reference
    // params in place from the caller's frame; the engine never owns or frees
    // Data, so it can point at a stack array that outlives the call.
    struct FTArrayRaw { void* Data; int32_t Num; int32_t Max; };

    void ResolveTrace()
    {
        // CDO/function re-found every tick like the other reflected calls;
        // offsets only re-derived when the UFunction instance changes.
        UObject* ksl = UObjectGlobals::StaticFindObject<UObject*>(
            nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
        UFunction* fn = ksl ? ksl->GetFunctionByNameInChain(STR("LineTraceSingle")) : nullptr;
        if (!fn) { m_trace = {}; WarnTraceUnavailable(STR("function not found")); return; }
        if (m_trace.ok && m_trace.fn == fn) { m_trace.ksl = ksl; return; }

        TraceCall tc{};
        tc.ksl = ksl;
        tc.fn = fn;
        tc.frameSize = fn->GetParmsSize();
        auto off = [fn](const wchar_t* name) -> int32_t {
            FProperty* p = fn->FindProperty(FName(name, FNAME_Find));
            return p ? p->GetOffset_Internal() : -1;
        };
        tc.offCtx        = off(STR("WorldContextObject"));
        tc.offStart      = off(STR("Start"));
        tc.offEnd        = off(STR("End"));
        tc.offChannel    = off(STR("TraceChannel"));
        tc.offComplex    = off(STR("bTraceComplex"));
        tc.offIgnore     = off(STR("ActorsToIgnore"));
        tc.offDraw       = off(STR("DrawDebugType"));
        tc.offIgnoreSelf = off(STR("bIgnoreSelf"));
        tc.offRet        = off(STR("ReturnValue"));
        if (FProperty* hit = fn->FindProperty(FName(STR("OutHit"), FNAME_Find))) {
            tc.offOutHit = hit->GetOffset_Internal();
            // Explicit type: GetStruct() returns TObjectPtr<UScriptStruct> in
            // newer RE-UE4SS (raw pointer before) — both convert to this.
            if (UScriptStruct* hs = static_cast<FStructProperty*>(hit)->GetStruct())
                if (FProperty* loc = hs->FindProperty(FName(STR("Location"), FNAME_Find)))
                    tc.offHitLoc = loc->GetOffset_Internal(); // LWC FVector: 3 doubles
        }

        // Every field we write must resolve and fit inside the reported frame.
        const int32_t maxEnd = std::max({
            tc.offCtx + 8, tc.offStart + 24, tc.offEnd + 24, tc.offChannel + 1,
            tc.offComplex + 1, tc.offIgnore + (int32_t)sizeof(FTArrayRaw),
            tc.offDraw + 1, tc.offOutHit + 1, tc.offIgnoreSelf + 1, tc.offRet + 1});
        const int32_t minOff = std::min({tc.offCtx, tc.offStart, tc.offEnd,
            tc.offChannel, tc.offComplex, tc.offIgnore, tc.offDraw, tc.offOutHit,
            tc.offIgnoreSelf, tc.offRet});
        tc.ok = minOff >= 0 && tc.frameSize > 0 && tc.frameSize <= 4096 &&
                maxEnd <= tc.frameSize;
        if (tc.ok)
            m_traceBuf.assign(size_t(tc.frameSize) * 2, 0); // oversized on purpose
        else
            WarnTraceUnavailable(STR("param layout resolve failed"));
        m_trace = tc;
    }

    void WarnTraceUnavailable(const wchar_t* why)
    {
        if (NowMs() - m_lastTraceWarn < 30000) return;
        m_lastTraceWarn = NowMs();
        Output::send<LogLevel::Warning>(
            STR("[RoNTacticalRadio] occlusion disabled: LineTraceSingle {}\n"), why);
    }

    // One Visibility-channel trace. Requires m_trace.ok. Returns hit/no-hit;
    // fills hitLoc (UE cm) when the FHitResult::Location offset is known.
    bool TraceOnce(UObject* pc, const FVecD& start, const FVecD& end,
                   UObject** ignoreList, int32_t ignoreCount, FVecD* hitLoc)
    {
        const TraceCall& tc = m_trace;
        uint8_t* p = m_traceBuf.data();
        std::memset(p, 0, m_traceBuf.size());
        *reinterpret_cast<UObject**>(p + tc.offCtx) = pc;
        *reinterpret_cast<FVecD*>(p + tc.offStart)  = start;
        *reinterpret_cast<FVecD*>(p + tc.offEnd)    = end;
        p[tc.offChannel]    = 0; // ETraceTypeQuery::TraceTypeQuery1 = Visibility
        p[tc.offComplex]    = 0;
        p[tc.offDraw]       = 0; // EDrawDebugTrace::None
        p[tc.offIgnoreSelf] = 0; // ActorsToIgnore already covers both bodies
        auto* arr = reinterpret_cast<FTArrayRaw*>(p + tc.offIgnore);
        arr->Data = ignoreList;
        arr->Num  = ignoreCount;
        arr->Max  = ignoreCount;
        tc.ksl->ProcessEvent(tc.fn, p);
        if (hitLoc && tc.offHitLoc >= 0)
            std::memcpy(hitLoc, p + tc.offOutHit + tc.offHitLoc, sizeof(FVecD));
        return p[tc.offRet] != 0;
    }

    // Wall count between camera and a speaker's head (positions in UE cm):
    // trace, step past the hit, re-trace; capped at kMaxWallCount. Without a
    // usable hit location this degrades to binary blocked(1)/clear(0).
    int CountWalls(UObject* pc, FVecD start, const FVecD& end,
                   UObject* myPawn, UObject* speakerPawn)
    {
        UObject* ignore[2] = {myPawn, speakerPawn};
        const double dx = end.X - start.X, dy = end.Y - start.Y, dz = end.Z - start.Z;
        const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (len < 1.0) return 0;
        const FVecD dir{dx / len, dy / len, dz / len};

        int walls = 0;
        while (walls < kMaxWallCount) {
            FVecD hit{};
            if (!TraceOnce(pc, start, end, ignore, 2, &hit)) break;
            ++walls;
            if (m_trace.offHitLoc < 0) break; // binary mode: can't advance
            // Progress along the ray; a garbage/backwards location (possible
            // under layout drift) ends the loop with the count so far.
            const double t = (hit.X - start.X) * dir.X + (hit.Y - start.Y) * dir.Y +
                             (hit.Z - start.Z) * dir.Z;
            if (!std::isfinite(t) || t <= 0.0) break;
            start = {start.X + (t + kAdvanceCm) * dir.X,
                     start.Y + (t + kAdvanceCm) * dir.Y,
                     start.Z + (t + kAdvanceCm) * dir.Z};
            const double remaining = (end.X - start.X) * dir.X +
                                     (end.Y - start.Y) * dir.Y +
                                     (end.Z - start.Z) * dir.Z;
            if (remaining <= 1.0) break; // reached the speaker
        }
        return walls;
    }

    // Occlusion byte for one speaker; logs on change so the known-spots
    // checklist (same room 0, closed door 1, across the map 4) is visible
    // live in UE4SS.log while walking around.
    uint8_t ComputeOcclusion(UObject* pc, const FVecD& camCm, bool camOk,
                             const FVecD& pawnLocCm, UObject* myPawn,
                             UObject* pawn, const char* name)
    {
        int walls = 0;
        if (m_trace.ok && camOk && pawn != myPawn) {
            FVecD headCm = pawnLocCm;
            headCm.Z += kHeadOffsetCm;
            const double dx = headCm.X - camCm.X, dy = headCm.Y - camCm.Y,
                         dz = headCm.Z - camCm.Z;
            if (dx * dx + dy * dy + dz * dz <= kMaxTraceRangeCm * kMaxTraceRangeCm)
                walls = CountWalls(pc, camCm, headCm, myPawn, pawn);
        }
        auto it = m_lastOccl.find(name);
        if (it == m_lastOccl.end() || it->second != walls) {
            m_lastOccl[name] = walls;
            wchar_t wname[RTR_NAME_LEN]{};
            MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, RTR_NAME_LEN);
            Output::send<LogLevel::Verbose>(
                STR("[RoNTacticalRadio] occl '{}' = {} wall(s)\n"), wname, walls);
        }
        return uint8_t(std::min(255, walls * kOcclusionPerWall));
    }

    // Reads camera + all player pawns from the local (client) world.
    // Class/function names confirmed against the RoN UHT dump:
    //   AReadyOrNotPlayerController -> PlayerCameraManager (prop, Engine)
    //   APlayerCameraManager::GetCameraLocation()/GetCameraRotation()
    //   AReadyOrNotPlayerState : APlayerState -> GetPlayerName(), GetPawn()
    //   AActor::K2_GetActorLocation()
    //   AReadyOrNotCharacter::IsDeadOrUnconscious()
    // The LOCAL player's controller. Critical on a listen server (host):
    // the host's process contains a PlayerController for EVERY connected
    // player, and after level transitions their ordering changes - grabbing
    // "the first live one" sometimes returned a remote player's controller
    // (positions were then relative to THEIR head: flipped/wrong audio).
    static UObject* FindLocalPlayerController()
    {
        // Any live world object to use as world context for GameplayStatics.
        UObject* ctx = nullptr;
        std::vector<UObject*> states{};
        UObjectGlobals::FindAllOf(STR("ReadyOrNotPlayerState"), states);
        for (UObject* s : states) {
            if (s && !s->HasAnyFlags(Unreal::EObjectFlags::RF_ClassDefaultObject)) { ctx = s; break; }
        }
        if (ctx) {
            UObject* gs = UObjectGlobals::StaticFindObject<UObject*>(
                nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));
            if (gs) {
                if (UFunction* fn = gs->GetFunctionByNameInChain(STR("GetPlayerController"))) {
                    struct { UObject* Ctx; int32_t PlayerIndex; UObject* ReturnValue; }
                        p{ctx, 0, nullptr};
                    gs->ProcessEvent(fn, &p);
                    if (p.ReturnValue) return p.ReturnValue;
                }
            }
        }
        // Fallback: first live controller that claims to be local.
        for (const auto* name : {STR("ReadyOrNotPlayerController"), STR("PlayerController")}) {
            std::vector<UObject*> pcs{};
            UObjectGlobals::FindAllOf(name, pcs);
            for (UObject* pc : pcs) {
                if (!pc) continue;
                if (pc->HasAnyFlags(Unreal::EObjectFlags::RF_ClassDefaultObject)) continue;
                if (CallBool(pc, STR("IsLocalPlayerController"), true)) return pc;
            }
        }
        return nullptr;
    }

    void CollectGameState(RtrSharedState& s)
    {
        m_myPs = nullptr; // same-tick pointer, re-resolved every tick
        UObject* pc = FindLocalPlayerController();
        if (!pc) { s.inGame = 0; return; }

        // No possessed pawn = loading screen / menus / not spawned yet:
        // report not-in-game so TeamSpeak behaves like normal TS.
        UObject* myPawn = CallObj(pc, STR("K2_GetPawn"));
        if (!myPawn) { s.inGame = 0; return; }
        s.inGame = 1;

        // --- Listener transform: OUR camera manager, with fallbacks ---
        // Matched by owner: on a host there is a camera manager per player.
        UObject* camMgr = nullptr;
        for (const auto* name : {STR("ReadyOrNotPlayerCameraManager"), STR("PlayerCameraManager")}) {
            std::vector<UObject*> mgrs{};
            UObjectGlobals::FindAllOf(name, mgrs);
            for (UObject* m : mgrs) {
                if (!m || m->HasAnyFlags(Unreal::EObjectFlags::RF_ClassDefaultObject)) continue;
                if (CallObj(m, STR("GetOwningPlayerController")) == pc) { camMgr = m; break; }
            }
            if (camMgr) break;
        }

        FVecD camLoc{}; FRotD camRot{};
        bool gotLoc = camMgr && CallVec(camMgr, STR("GetCameraLocation"), camLoc);
        bool gotRot = camMgr && CallRot(camMgr, STR("GetCameraRotation"), camRot);

        // Fallbacks: control rotation from the controller, location from our pawn.
        bool locFromPawn = false;
        if (!gotRot) gotRot = CallRot(pc, STR("GetControlRotation"), camRot);
        if (!gotLoc) {
            gotLoc = CallVec(myPawn, STR("K2_GetActorLocation"), camLoc);
            locFromPawn = gotLoc;
        }

        if ((!gotLoc || !gotRot) && NowMs() - m_lastCamWarn > 5000) {
            m_lastCamWarn = NowMs();
            Output::send<LogLevel::Warning>(
                STR("[RoNTacticalRadio] listener read FAILED (camMgr={} loc={} rot={}) - positional audio will be wrong\n"),
                camMgr ? STR("live") : STR("null"),
                gotLoc ? STR("ok") : STR("MISSING"),
                gotRot ? STR("ok") : STR("MISSING"));
        }
        // Menu cameras (loadout, mission select) fly far from the pawn while
        // OTHER players still hear us at the pawn — so listen from the pawn
        // too, or the menu user loses all proximity voice (camera is past
        // the falloff range) while still being heard by everyone.
        FVecD myLoc{};
        if (gotLoc && !locFromPawn && CallVec(myPawn, STR("K2_GetActorLocation"), myLoc)) {
            const double dx = camLoc.X - myLoc.X, dy = camLoc.Y - myLoc.Y,
                         dz = camLoc.Z - myLoc.Z;
            const bool detached = dx * dx + dy * dy + dz * dz > kCamDetachCm * kCamDetachCm;
            if (detached) {
                camLoc = myLoc;
                camLoc.Z += kHeadOffsetCm;
                CallRot(myPawn, STR("K2_GetActorRotation"), camRot); // face with the body
            }
            if (detached != m_camDetached) {
                m_camDetached = detached;
                Output::send<LogLevel::Verbose>(
                    STR("[RoNTacticalRadio] listener {} pawn (menu camera {})\n"),
                    detached ? STR("snapped to") : STR("back on"),
                    detached ? STR("detected") : STR("closed"));
            }
        }

        s.listenerPos = ToMeters(camLoc);
        if (locFromPawn) s.listenerPos.z += 1.6f; // pawn root -> approx head height
        // Trace origin in UE cm, mirroring the head-height fixup above.
        FVecD camCm = camLoc;
        if (locFromPawn) camCm.Z += kHeadOffsetCm;
        // UE rotator (degrees) -> forward/up unit vectors (roll ignored).
        const float d2r = 3.14159265f / 180.0f;
        const float cp = std::cos(float(camRot.Pitch) * d2r), sp = std::sin(float(camRot.Pitch) * d2r);
        const float cy = std::cos(float(camRot.Yaw) * d2r),   sy = std::sin(float(camRot.Yaw) * d2r);
        s.listenerFwd = {cp * cy, cp * sy, sp};
        s.listenerUp  = {-sp * cy, -sp * sy, cp};

        // --- Players (all replicated PlayerStates, skip CDOs) ---
        ResolveTrace(); // occlusion trace call (CDO/function re-found per tick)
        m_pawns.clear();
        std::vector<UObject*> states{};
        UObjectGlobals::FindAllOf(STR("ReadyOrNotPlayerState"), states);
        uint32_t n = 0;
        for (UObject* ps : states)
        {
            if (n >= RTR_MAX_PLAYERS || !ps) continue;
            if (ps->HasAnyFlags(Unreal::EObjectFlags::RF_ClassDefaultObject)) continue;

            RtrPlayer& p = s.players[n];
            CallNameUtf8(ps, STR("GetPlayerName"), p.name, RTR_NAME_LEN);
            if (p.name[0] == '\0') continue;

            UObject* pawn = CallObj(ps, STR("GetPawn"));
            if (!pawn) continue; // not spawned (spectator / loading)
            FVecD loc{};
            if (!CallVec(pawn, STR("K2_GetActorLocation"), loc)) continue;
            p.pos = ToMeters(loc);
            p.alive = CallBool(pawn, STR("IsDeadOrUnconscious"), false) ? 0 : 1;
            p.occlusion = ComputeOcclusion(pc, camCm, gotLoc, loc, myPawn, pawn, p.name);
            m_pawns[p.name] = pawn;
            ++n;
        }
        s.playerCount = n;

        // --- Local player name + state (state pointer valid this tick only) ---
        UObject** psPtr = pc->GetValuePtrByPropertyNameInChain<UObject*>(STR("PlayerState"));
        if (psPtr && *psPtr) {
            CallNameUtf8(*psPtr, STR("GetPlayerName"), s.localName, RTR_NAME_LEN);
            m_myPs = *psPtr;
            m_localName = s.localName;
        }
    }

    static bool SetMouthAlpha(UObject* anim, float alpha)
    {
        float* p = anim->GetValuePtrByPropertyNameInChain<float>(STR("VoipMouthAlpha"));
        if (!p) return false;
        *p = alpha;
        return true;
    }

    // Drives VoipMouthAlpha on each speaking character from TS loudness.
    // m_pawns is rebuilt by CollectGameState each tick (name -> pawn).
    //
    // IMPORTANT: face anim instances are resolved FRESH each tick and never
    // cached across ticks - level transitions destroy them, and a stale
    // pointer crashes the game (caused crashes on mission load in 0.3/0.4.0).
    // Same-tick pointers are safe: GC doesn't run mid-tick on the game thread.
    void PumpTalkState(bool inGame)
    {
        RtrTalkMsg msg{};
        if (m_talkRx.Poll(msg)) m_talk = msg;
        if (!inGame) { m_activeMouths.clear(); m_selfTalkSent = -1; return; }

        // Native talking indicators: each client reports its OWN talk state
        // (Server_PushToTalk is an owning-client RPC). IsTalking replicates,
        // so the stock VOIP talker HUD lights up on every machine — this is
        // how "who is talking" shows in-game with TS carrying the voice.
        bool meTalking = false;
        for (uint32_t i = 0; i < m_talk.count && i < RTR_TALK_MAX; ++i)
            if (m_localName == m_talk.speakers[i].name) { meTalking = true; break; }

        if (m_talk.count == 0 && m_activeMouths.empty()) return; // idle: no work

        // pawn -> face anim map, valid for this tick only.
        std::unordered_map<UObject*, UObject*> faceByPawn;
        std::vector<UObject*> anims{};
        UObjectGlobals::FindAllOf(STR("ReadyOrNotFaceAnimInstance"), anims);
        for (UObject* anim : anims) {
            if (!anim || anim->HasAnyFlags(Unreal::EObjectFlags::RF_ClassDefaultObject)) continue;
            UObject* comp = CallObj(anim, STR("GetOwningComponent"));
            UObject* owner = comp ? CallObj(comp, STR("GetOwner")) : nullptr;
            if (owner) faceByPawn[owner] = anim;
        }
        auto faceFor = [&](const std::string& name) -> UObject* {
            auto pw = m_pawns.find(name);
            if (pw == m_pawns.end() || !pw->second) return nullptr;
            auto it = faceByPawn.find(pw->second);
            return it == faceByPawn.end() ? nullptr : it->second;
        };

        std::vector<std::string> nowTalking;
        for (uint32_t i = 0; i < m_talk.count && i < RTR_TALK_MAX; ++i) {
            const std::string name = m_talk.speakers[i].name;
            UObject* anim = faceFor(name);
            if (!anim) continue;
            if (!SetMouthAlpha(anim, std::min(1.0f, m_talk.speakers[i].amplitude))) continue;
            nowTalking.push_back(name);
        }

        // Close mouths that stopped talking (re-resolved this tick; if the
        // character no longer exists there is nothing to close).
        for (const std::string& name : m_activeMouths) {
            if (std::find(nowTalking.begin(), nowTalking.end(), name) != nowTalking.end()) continue;
            if (UObject* anim = faceFor(name)) SetMouthAlpha(anim, 0.0f);
        }
        m_activeMouths = std::move(nowTalking);
    }

    // Report our own talk state to the game once per change. m_myPs is a
    // same-tick pointer set by CollectGameState (never cached across ticks).
    void SyncSelfTalking(bool talking)
    {
        if (!m_myPs) return;
        const int8_t want = talking ? 1 : 0;
        if (m_selfTalkSent == want) return;
        UFunction* fn = Fn(m_myPs, STR("Server_PushToTalk"));
        if (!fn) return;
        struct { bool bPushToTalk; } p{talking};
        m_myPs->ProcessEvent(fn, &p);
        m_selfTalkSent = want;
    }

    RtrConfig m_cfg{};
    SharedMemWriter m_shm{};
    UdpSender m_udp{};
    TalkReceiver m_talkRx{};
    RtrTalkMsg m_talk{};
    std::unordered_map<std::string, UObject*> m_pawns; // this tick's players
    std::vector<std::string> m_activeMouths;
    UObject* m_myPs = nullptr;    // local PlayerState, THIS tick only
    std::string m_localName;      // local player's in-game name
    int8_t m_selfTalkSent = -1;   // last Server_PushToTalk sent; -1 unknown
    bool m_camDetached = false;   // menu camera far from pawn (log on change)
    bool     m_shmOk = false;
    bool     m_udpOk = false;
    uint64_t m_lastPublish = 0;
    uint64_t m_lastLog = 0;
    uint8_t  m_prevRadioPtt = 0;
    uint8_t  m_prevVoicePtt = 0;
    uint64_t m_lastCamWarn = 0;
    uint64_t m_lastTraceWarn = 0;
    TraceCall m_trace{};
    std::vector<uint8_t> m_traceBuf;              // LineTraceSingle param frame
    std::unordered_map<std::string, int> m_lastOccl; // wall count change log
    uint32_t m_faults = 0;
    uint32_t m_faultsLogged = 0;
    uint8_t  m_activeRadio = 0;
    bool     m_cycleWasDown = false;
};

#define UE4SS_MOD_EXPORTS
extern "C" __declspec(dllexport) CppUserModBase* start_mod()
{
    return new RoNTacticalRadioMod();
}

extern "C" __declspec(dllexport) void uninstall_mod(CppUserModBase* mod)
{
    delete mod;
}
