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
        if (!m_view) openShm();          // game may start after TS
        if (m_view) updated = pollShm();
#endif
        if (m_sock == INVALID_SOCKET) openUdp();
        if (pollUdp()) updated = true;

        if (updated) m_lastGoodMs = nowMs;
        if (m_lastGoodMs == 0 || nowMs - m_lastGoodMs > 1000) {
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
    void injectState(const RtrSharedState& s, uint64_t nowMs)
    {
        setState(s);
        m_lastGoodMs = nowMs;
    }

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
            if (!valid(local)) return false;
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
    SOCKET m_sock = INVALID_SOCKET;
#ifdef _WIN32
    HANDLE m_mapping{};
    const RtrSharedState* m_view{};
    uint32_t m_lastShmSeq = 0;
#endif
};

} // namespace rtr
