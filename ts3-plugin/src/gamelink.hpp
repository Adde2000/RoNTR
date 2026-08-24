// gamelink.hpp — receives game state (shared memory on Windows, UDP loopback
// everywhere) and answers spatial queries for the audio callback.
//
// Transports:
//   - Windows shared memory (seqlock): game and TS both native Windows.
//   - UDP 127.0.0.1:RTR_UDP_PORT:  game under Proton/Wine + native Linux TS
//     (Windows shared memory cannot cross that boundary), or fallback.
// Staleness uses the RECEIVER's clock (last successful update), because the
// game's clock domain (Wine) may not match a native client's.
#pragma once
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#pragma comment(lib, "ws2_32.lib")
typedef int socklen_t;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define INVALID_SOCKET (-1)
typedef int SOCKET;
#endif

#include "../../shared/RadioLink.h"

namespace rtr {

struct Vec3 { float x{}, y{}, z{}; };

inline Vec3 sub(const RtrVec3& a, const RtrVec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline float len(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline Vec3 norm(const Vec3& v)
{
    const float l = len(v);
    return l > 1e-6f ? Vec3{v.x / l, v.y / l, v.z / l} : Vec3{};
}

// Result of a spatial query for one speaker.
struct SpeakerAudio {
    bool  inGame = false;   // false -> passthrough
    bool  found  = false;   // speaker matched to a game player
    float distM  = 0.0f;
    float pan    = 0.0f;    // -1 left .. +1 right
    float occlusion01 = 0.0f; // 0 = clear line of sight .. 1 = fully occluded
};

class GameLink {
public:
    bool open()
    {
        bool any = openUdp();
#ifdef _WIN32
        any = openShm() || any;
#endif
        return any;
    }

    void close()
    {
#ifdef _WIN32
        if (m_view)    { UnmapViewOfFile(m_view);  m_view = nullptr; }
        if (m_mapping) { CloseHandle(m_mapping);   m_mapping = nullptr; }
        if (m_sock != INVALID_SOCKET) { closesocket(m_sock); m_sock = INVALID_SOCKET; }
#else
        if (m_sock != INVALID_SOCKET) { ::close(m_sock); m_sock = INVALID_SOCKET; }
#endif
    }

    // Poll-thread: pull the freshest state from any transport.
    // Returns false when no live data (never opened, or nothing for >1s).
    bool poll(uint64_t nowMs)
    {
        bool updated = false;
#ifdef _WIN32
        if (!m_view && nowMs >= m_shmRetryAtMs) openShm(); // game may start after TS
        if (m_view) updated = pollShm();
#endif
        if (m_sock == INVALID_SOCKET) openUdp();
        if (pollUdp()) updated = true;

        if (updated) { m_lastGoodMs = nowMs; noteNativeUpdate(nowMs); }
        if (m_lastGoodMs == 0 || nowMs - m_lastGoodMs > 1000) {
#ifdef _WIN32
            // Release the mapping while the link is dead: a named mapping
            // stays alive while ANY process holds a handle, so keeping ours
            // open would preserve Local\RoNTacticalRadio after the game
            // exits (confusing every "is the game running?" check, ours
            // included). Reopen attempts are throttled to 1 Hz; the UDP
            // mirror still detects a restarted game within one poll.
            if (m_view) {
                UnmapViewOfFile(m_view);
                m_view = nullptr;
                CloseHandle(m_mapping);
                m_mapping = nullptr;
                m_lastShmSeq = 0;
            }
            if (m_shmRetryAtMs < nowMs) m_shmRetryAtMs = nowMs + 1000;
#endif
            setState({});                // stale -> behave as not-in-game
            return false;
        }
        return true;
    }

    // For tests / injection.
    void setState(const RtrSharedState& s)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_state = s;
    }

    // External transports (e.g. the HTTP bridge) push state here; marks the
    // link fresh so poll()'s staleness logic treats it like shm/UDP data.
    // Returns false when ignored: native transports (shm/UDP) own the link
    // while they are fresh, so a second game left running in the background
    // (posting ingame=0 over HTTP) cannot stomp an active session. HTTP
    // takes over once the native link has been stale for >1s.
    bool injectState(const RtrSharedState& s, uint64_t nowMs)
    {
        const uint64_t native = m_lastNativeMs.load();
        if (native != 0 && nowMs - native < 1000) return false;
        setState(s);
        m_lastGoodMs = nowMs;
        return true;
    }

    // poll() records native freshness here; public so tests can simulate it.
    void noteNativeUpdate(uint64_t nowMs) { m_lastNativeMs = nowMs; }

    // True while a native transport (shm/UDP) delivered data within the last
    // second — i.e. the current state comes from RoN, not an HTTP integration.
    bool nativeFresh(uint64_t nowMs) const
    {
        const uint64_t n = m_lastNativeMs.load();
        return n != 0 && nowMs - n < 1000;
    }

    // Nonzero when data with our magic but a DIFFERENT protocol version was
    // seen (game mod and plugin out of step). For diagnostics only.
    uint32_t mismatchedPeerVersion() const { return m_peerVersion.load(); }

    RtrSharedState snapshot() const
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_state;
    }

    // Audio callback: where is `speakerName` relative to the listener?
    SpeakerAudio query(const std::string& speakerName) const
    {
        RtrSharedState s = snapshot();
        SpeakerAudio out;
        out.inGame = s.inGame != 0;
        if (!out.inGame) return out;

        for (uint32_t i = 0; i < s.playerCount && i < RTR_MAX_PLAYERS; ++i) {
            if (speakerName != s.players[i].name) continue;
            out.found = true;
            const Vec3 to = sub(s.players[i].pos, s.listenerPos);
            out.distM = len(to);
            const Vec3 fwd   = {s.listenerFwd.x, s.listenerFwd.y, s.listenerFwd.z};
            const Vec3 up    = {s.listenerUp.x,  s.listenerUp.y,  s.listenerUp.z};
            const Vec3 right = norm(cross(up, fwd)); // UE left-handed: up x fwd = right
            out.pan = out.distM > 0.5f ? dot(norm(to), right) : 0.0f;
            out.occlusion01 = s.players[i].occlusion / 255.0f;
            return out;
        }
        return out;
    }

private:
    bool valid(const RtrSharedState& s) const
    {
        return s.magic == RTR_MAGIC && s.version == RTR_VERSION;
    }

    // ---- UDP transport (all platforms) ----
    bool openUdp()
    {
        if (m_sock != INVALID_SOCKET) return true;
#ifdef _WIN32
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
#endif
        m_sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (m_sock == INVALID_SOCKET) return false;
        int reuse = 1;
        setsockopt(m_sock, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#ifdef _WIN32
        u_long nonblock = 1;
        ioctlsocket(m_sock, FIONBIO, &nonblock);
#else
        fcntl(m_sock, F_SETFL, fcntl(m_sock, F_GETFL, 0) | O_NONBLOCK);
#endif
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(RTR_UDP_PORT);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(m_sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
#ifdef _WIN32
            closesocket(m_sock);
#else
            ::close(m_sock);
#endif
            m_sock = INVALID_SOCKET;
            return false;
        }
        return true;
    }

    bool pollUdp()
    {
        if (m_sock == INVALID_SOCKET) return false;
        bool got = false;
        RtrSharedState pkt{};
        for (;;) { // drain queue, keep newest complete datagram
            RtrSharedState tmp{};
            const auto n = ::recvfrom(m_sock, reinterpret_cast<char*>(&tmp), sizeof(tmp),
                                      0, nullptr, nullptr);
            if (n <= 0 || static_cast<size_t>(n) != sizeof(tmp)) break;
            if (valid(tmp)) { pkt = tmp; got = true; }
            else if (tmp.magic == RTR_MAGIC && tmp.version != RTR_VERSION)
                m_peerVersion = tmp.version; // right game, wrong protocol
        }
        if (got) setState(pkt);
        return got;
    }

#ifdef _WIN32
    // ---- Shared-memory transport (Windows only) ----
    bool openShm()
    {
        if (m_view) return true;
        m_mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, RTR_SHM_NAME);
        if (!m_mapping) return false;
        m_view = static_cast<const RtrSharedState*>(
            MapViewOfFile(m_mapping, FILE_MAP_READ, 0, 0, sizeof(RtrSharedState)));
        if (!m_view) { CloseHandle(m_mapping); m_mapping = nullptr; return false; }
        return true;
    }

    bool pollShm()
    {
        RtrSharedState local{};
        for (int attempt = 0; attempt < 8; ++attempt) {
            const uint32_t s1 = m_view->sequence;
            if (s1 & 1u) continue;
            std::memcpy(&local, (const void*)m_view, sizeof(local));
            const uint32_t s2 = m_view->sequence;
            if (s1 != s2) continue;
            if (!valid(local)) {
                if (local.magic == RTR_MAGIC && local.version != RTR_VERSION)
                    m_peerVersion = local.version; // right game, wrong protocol
                return false;
            }
            if (s2 == m_lastShmSeq) return false; // writer stalled -> not fresh
            m_lastShmSeq = s2;
            setState(local);
            return true;
        }
        return false;
    }
#endif

    mutable std::mutex m_mtx;
    RtrSharedState m_state{};
    // atomic: written by the poll thread AND injectState (HTTP bridge thread)
    std::atomic<uint64_t> m_lastGoodMs{0};
    std::atomic<uint64_t> m_lastNativeMs{0}; // last fresh shm/UDP update
    std::atomic<uint32_t> m_peerVersion{0};  // last mismatched protocol seen
    SOCKET m_sock = INVALID_SOCKET;
#ifdef _WIN32
    HANDLE m_mapping{};
    const RtrSharedState* m_view{};
    uint32_t m_lastShmSeq = 0;
    uint64_t m_shmRetryAtMs = 0; // poll thread only
#endif
};

} // namespace rtr
