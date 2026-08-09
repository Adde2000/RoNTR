// RoNTacticalRadio game-side mod (UE4SS C++ mod).
// Polls player/camera state each tick and publishes it to shared memory
// for the TeamSpeak 3 plugin. See shared/RadioLink.h for the contract.
//
// Build: as a UE4SS C++ mod against RE-UE4SS (see CMakeLists.txt / README).
// Install: <game>/Binaries/Win64/ue4ss/Mods/RoNTacticalRadio/dlls/main.dll
//
// NOTE: RoN class/property names below are PLACEHOLDERS. Dump the SDK with
// UE4SS (UHT dump) for your game build and fix the names marked TODO(sdk).

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h> // must precede Windows.h
#include <ws2tcpip.h>
#include <Windows.h>
#pragma comment(lib, "ws2_32.lib")
#include <chrono>
#include <cmath>
#include <cstdio>
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
#include <Unreal/AActor.hpp>
#include <DynamicOutput/DynamicOutput.hpp>

#include "../../shared/RadioLink.h"

using namespace RC;
using namespace RC::Unreal;

namespace {

constexpr int kPttVKey       = VK_CAPITAL; // radio PTT (TODO: ini-configurable)
constexpr int kVoiceVKey     = 'V';        // proximity voice key
constexpr int kCycleVKey     = VK_OEM_6;   // ']' cycle radio channel

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
        ModVersion = STR("0.3.0");
        ModDescription = STR("Publishes player positions + radio state for the TS3 plugin");
        ModAuthors = STR("Ethan Baxter");
    }

    void on_unreal_init() override
    {
        m_shmOk = m_shm.Open();
        m_udpOk = m_udp.Open();
        m_talkRx.Open();
        Output::send<LogLevel::Verbose>(STR("[RoNTacticalRadio] shared memory {}, udp {}\n"),
                                        m_shmOk ? STR("ready") : STR("FAILED"),
                                        m_udpOk ? STR("ready") : STR("FAILED"));
    }

    void on_update() override
    {
        // Throttle to ~20 Hz; on_update runs every frame.
        const uint64_t now = NowMs();
        if (now - m_lastPublish < 50) return;
        m_lastPublish = now;
        if (!m_shmOk && !m_udpOk) return;

        RtrSharedState s{};
        s.timestampMs = now;
        s.radioPtt  = (GetAsyncKeyState(kPttVKey)   & 0x8000) ? 1 : 0;
        s.voicePtt  = (GetAsyncKeyState(kVoiceVKey) & 0x8000) ? 1 : 0;
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
        // TODO(config): load from ini. Defaults: two program slots.
        s.radioFreqKhz[0] = 246000;
        s.radioFreqKhz[1] = 247000;
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
        const bool down = (GetAsyncKeyState(kCycleVKey) & 0x8000) != 0;
        if (down && !m_cycleWasDown) m_activeRadio = (m_activeRadio + 1) % 2;
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

    // Reads camera + all player pawns from the local (client) world.
    // Class/function names confirmed against the RoN UHT dump:
    //   AReadyOrNotPlayerController -> PlayerCameraManager (prop, Engine)
    //   APlayerCameraManager::GetCameraLocation()/GetCameraRotation()
    //   AReadyOrNotPlayerState : APlayerState -> GetPlayerName(), GetPawn()
    //   AActor::K2_GetActorLocation()
    //   AReadyOrNotCharacter::IsDeadOrUnconscious()
    // First live (non-CDO) instance of the player controller. FindFirstOf can
    // hand back the class default object, whose camera manager is null.
    static UObject* FindLivePlayerController()
    {
        for (const auto* name : {STR("ReadyOrNotPlayerController"), STR("PlayerController")}) {
            std::vector<UObject*> pcs{};
            UObjectGlobals::FindAllOf(name, pcs);
            for (UObject* pc : pcs) {
                if (!pc) continue;
                if (pc->HasAnyFlags(Unreal::EObjectFlags::RF_ClassDefaultObject)) continue;
                return pc;
            }
        }
        return nullptr;
    }

    void CollectGameState(RtrSharedState& s)
    {
        UObject* pc = FindLivePlayerController();
        if (!pc) { s.inGame = 0; return; }
        s.inGame = 1;

        // --- Listener transform: camera manager, with controller fallbacks ---
        // Found as a live instance (like PlayerStates) rather than through the
        // PlayerCameraManager property: reflected function calls work reliably
        // here, but the property-by-name read returned null on live PCs.
        UObject* camMgr = nullptr;
        for (const auto* name : {STR("ReadyOrNotPlayerCameraManager"), STR("PlayerCameraManager")}) {
            std::vector<UObject*> mgrs{};
            UObjectGlobals::FindAllOf(name, mgrs);
            for (UObject* m : mgrs) {
                if (!m || m->HasAnyFlags(Unreal::EObjectFlags::RF_ClassDefaultObject)) continue;
                camMgr = m;
                break;
            }
            if (camMgr) break;
        }

        FVecD camLoc{}; FRotD camRot{};
        bool gotLoc = camMgr && CallVec(camMgr, STR("GetCameraLocation"), camLoc);
        bool gotRot = camMgr && CallRot(camMgr, STR("GetCameraRotation"), camRot);

        // Fallbacks: control rotation from the controller, location from the pawn.
        bool locFromPawn = false;
        if (!gotRot) gotRot = CallRot(pc, STR("GetControlRotation"), camRot);
        if (!gotLoc) {
            if (UObject* pawn = CallObj(pc, STR("K2_GetPawn"))) {
                gotLoc = CallVec(pawn, STR("K2_GetActorLocation"), camLoc);
                locFromPawn = gotLoc;
            }
        }

        if ((!gotLoc || !gotRot) && NowMs() - m_lastCamWarn > 5000) {
            m_lastCamWarn = NowMs();
            Output::send<LogLevel::Warning>(
                STR("[RoNTacticalRadio] listener read FAILED (camMgr={} loc={} rot={}) - positional audio will be wrong\n"),
                camMgr ? STR("live") : STR("null"),
                gotLoc ? STR("ok") : STR("MISSING"),
                gotRot ? STR("ok") : STR("MISSING"));
        }
        s.listenerPos = ToMeters(camLoc);
        if (locFromPawn) s.listenerPos.z += 1.6f; // pawn root -> approx head height
        // UE rotator (degrees) -> forward/up unit vectors (roll ignored).
        const float d2r = 3.14159265f / 180.0f;
        const float cp = std::cos(float(camRot.Pitch) * d2r), sp = std::sin(float(camRot.Pitch) * d2r);
        const float cy = std::cos(float(camRot.Yaw) * d2r),   sy = std::sin(float(camRot.Yaw) * d2r);
        s.listenerFwd = {cp * cy, cp * sy, sp};
        s.listenerUp  = {-sp * cy, -sp * sy, cp};

        // --- Players (all replicated PlayerStates, skip CDOs) ---
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
            m_pawns[p.name] = pawn;
            ++n;
        }
        s.playerCount = n;

        // --- Local player name ---
        UObject** psPtr = pc->GetValuePtrByPropertyNameInChain<UObject*>(STR("PlayerState"));
        if (psPtr && *psPtr)
            CallNameUtf8(*psPtr, STR("GetPlayerName"), s.localName, RTR_NAME_LEN);
    }

    // Face anim instance for a pawn (cached; resolved via
    // ReadyOrNotFaceAnimInstance -> GetOwningComponent -> GetOwner == pawn).
    UObject* FindFaceAnim(const std::string& name, UObject* pawn)
    {
        auto it = m_faceCache.find(name);
        if (it != m_faceCache.end()) return it->second;

        std::vector<UObject*> anims{};
        UObjectGlobals::FindAllOf(STR("ReadyOrNotFaceAnimInstance"), anims);
        for (UObject* anim : anims) {
            if (!anim || anim->HasAnyFlags(Unreal::EObjectFlags::RF_ClassDefaultObject)) continue;
            UObject* comp = CallObj(anim, STR("GetOwningComponent"));
            UObject* owner = comp ? CallObj(comp, STR("GetOwner")) : nullptr;
            if (owner == pawn) { m_faceCache[name] = anim; return anim; }
        }
        return nullptr;
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
    void PumpTalkState(bool inGame)
    {
        RtrTalkMsg msg{};
        if (m_talkRx.Poll(msg)) m_talk = msg;
        if (!inGame) { m_faceCache.clear(); m_activeMouths.clear(); return; }

        std::vector<std::string> nowTalking;
        for (uint32_t i = 0; i < m_talk.count && i < RTR_TALK_MAX; ++i) {
            const std::string name = m_talk.speakers[i].name;
            auto pw = m_pawns.find(name);
            if (pw == m_pawns.end() || !pw->second) continue;
            UObject* anim = FindFaceAnim(name, pw->second);
            if (!anim || !SetMouthAlpha(anim, std::min(1.0f, m_talk.speakers[i].amplitude))) {
                m_faceCache.erase(name); // stale cache entry; re-resolve next tick
                continue;
            }
            nowTalking.push_back(name);
        }

        // Close mouths that stopped talking.
        for (const std::string& name : m_activeMouths) {
            if (std::find(nowTalking.begin(), nowTalking.end(), name) != nowTalking.end()) continue;
            auto it = m_faceCache.find(name);
            if (it != m_faceCache.end() && it->second) SetMouthAlpha(it->second, 0.0f);
        }
        m_activeMouths = std::move(nowTalking);
    }

    SharedMemWriter m_shm{};
    UdpSender m_udp{};
    TalkReceiver m_talkRx{};
    RtrTalkMsg m_talk{};
    std::unordered_map<std::string, UObject*> m_pawns;     // this tick's players
    std::unordered_map<std::string, UObject*> m_faceCache; // name -> face anim
    std::vector<std::string> m_activeMouths;
    bool     m_shmOk = false;
    bool     m_udpOk = false;
    uint64_t m_lastPublish = 0;
    uint64_t m_lastLog = 0;
    uint8_t  m_prevRadioPtt = 0;
    uint8_t  m_prevVoicePtt = 0;
    uint64_t m_lastCamWarn = 0;
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
