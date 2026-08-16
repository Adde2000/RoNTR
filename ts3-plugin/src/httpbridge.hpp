// httpbridge.hpp — minimal localhost HTTP listener for games whose mods can't
// open sockets or shared memory (Arma Reforger: sandboxed EnforceScript, but
// RestApi can POST to localhost).
//
// Contract (see ArmaReforgerRadio/docs/DESIGN.md):
//   POST /state, body is "key=value" lines mirroring RtrSharedState:
//     rtr=3  ingame=1  name=<local>  lpos/lfwd/lup=x y z  radioptt/voicePtt
//     freq=<kHz>  player=Name|x|y|z|alive|occl (one per player)
//   Response 200 body: "rtr=3" + "talk=Name|amp" lines (reverse channel).
//   Version mismatch -> 409, parse failure -> 400. One request per
//   connection, single-threaded: fine at the mod's 10 Hz on loopback.
//
// Parsing/building are pure functions (host-testable); only HttpBridge
// touches sockets, following gamelink.hpp's platform idioms.
#pragma once
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

#include "gamelink.hpp" // socket includes + RadioLink.h, shared with GameLink

namespace rtr {

// ---- wire format ------------------------------------------------------------

inline bool parseVec3(const std::string& v, RtrVec3& out)
{
    return std::sscanf(v.c_str(), "%f %f %f", &out.x, &out.y, &out.z) == 3;
}

// "Name|x|y|z|alive|occl" -> RtrPlayer. Name is sanitized by the sender.
inline bool parsePlayerLine(const std::string& v, RtrPlayer& p)
{
    std::string fields[6];
    size_t start = 0;
    for (int i = 0; i < 6; ++i) {
        const size_t bar = v.find('|', start);
        if (bar == std::string::npos) {
            if (i != 5) return false;
            fields[i] = v.substr(start);
        } else {
            fields[i] = v.substr(start, bar - start);
            start = bar + 1;
        }
    }
    if (fields[0].empty()) return false;
    std::snprintf(p.name, RTR_NAME_LEN, "%s", fields[0].c_str());
    p.pos.x = std::strtof(fields[1].c_str(), nullptr);
    p.pos.y = std::strtof(fields[2].c_str(), nullptr);
    p.pos.z = std::strtof(fields[3].c_str(), nullptr);
    p.alive = fields[4] == "0" ? 0 : 1;
    const long occl = std::strtol(fields[5].c_str(), nullptr, 10);
    p.occlusion = uint8_t(occl < 0 ? 0 : (occl > 255 ? 255 : occl));
    return true;
}

enum class ParseResult { Ok, BadVersion, Malformed };

inline std::string percentDecode(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size() &&
            std::isxdigit((unsigned char)in[i + 1]) &&
            std::isxdigit((unsigned char)in[i + 2])) {
            out += char(std::strtol(in.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else if (in[i] == '+') {
            out += ' ';
        } else {
            out += in[i];
        }
    }
    return out;
}

// HTTP clients we don't control (Enfusion's RestApi wraps libcurl) may ship
// the payload form-encoded ("data=rtr%3D3%0A...") or with escapes intact.
// Normalize back to plain "key=value" lines before parsing.
inline std::string normalizeStateBody(std::string body)
{
    if (body.compare(0, 5, "data=") == 0)
        body = percentDecode(body.substr(5));
    else if (body.find("rtr=") == std::string::npos && body.find('%') != std::string::npos)
        body = percentDecode(body);
    // Literal backslash-n instead of newlines (escaping quirk): unescape.
    if (body.find('\n') == std::string::npos && body.find("\\n") != std::string::npos) {
        std::string out;
        out.reserve(body.size());
        for (size_t i = 0; i < body.size(); ++i) {
            if (body[i] == '\\' && i + 1 < body.size() && body[i + 1] == 'n') {
                out += '\n';
                ++i;
            } else {
                out += body[i];
            }
        }
        body = out;
    }
    return body;
}

// Transfer-Encoding: chunked -> raw body. Returns false while more input is
// needed; on true, `complete` says whether the terminating 0-chunk arrived.
inline bool decodeChunked(const std::string& in, std::string& out, bool& complete)
{
    out.clear();
    complete = false;
    size_t pos = 0;
    for (;;) {
        const size_t lineEnd = in.find("\r\n", pos);
        if (lineEnd == std::string::npos) return false; // need more data
        const size_t size = std::strtoul(in.substr(pos, lineEnd - pos).c_str(), nullptr, 16);
        pos = lineEnd + 2;
        if (size == 0) { complete = true; return true; }
        if (in.size() < pos + size + 2) return false; // need more data
        out.append(in, pos, size);
        pos += size + 2; // chunk data + trailing CRLF
    }
}

// Body -> RtrSharedState. Unknown keys are ignored (forward compatible);
// missing keys just leave zeros, which all fail safe (ingame=0 etc.).
// `gameId`, when non-null, receives the payload's game= identifier.
inline ParseResult parseStateText(const std::string& rawBody, RtrSharedState& out,
                                  std::string* gameId = nullptr)
{
    const std::string body = normalizeStateBody(rawBody);
    out = {};
    out.magic = RTR_MAGIC;
    out.version = RTR_VERSION;
    bool versionSeen = false;

    size_t pos = 0;
    while (pos < body.size()) {
        size_t end = body.find('\n', pos);
        if (end == std::string::npos) end = body.size();
        std::string line = body.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);

        if (key == "rtr") {
            if (std::strtoul(val.c_str(), nullptr, 10) != RTR_VERSION)
                return ParseResult::BadVersion;
            versionSeen = true;
        } else if (key == "game") {
            if (gameId) *gameId = val;
        } else if (key == "ingame") {
            out.inGame = val == "1" ? 1 : 0;
        } else if (key == "name") {
            std::snprintf(out.localName, RTR_NAME_LEN, "%s", val.c_str());
        } else if (key == "lpos") {
            if (!parseVec3(val, out.listenerPos)) return ParseResult::Malformed;
        } else if (key == "lfwd") {
            if (!parseVec3(val, out.listenerFwd)) return ParseResult::Malformed;
        } else if (key == "lup") {
            if (!parseVec3(val, out.listenerUp)) return ParseResult::Malformed;
        } else if (key == "radioptt") {
            out.radioPtt = val == "1" ? 1 : 0;
        } else if (key == "voiceptt") {
            out.voicePtt = val == "1" ? 1 : 0;
        } else if (key == "freq") {
            out.radioFreqKhz[0] = (uint32_t)std::strtoul(val.c_str(), nullptr, 10);
        } else if (key == "freqs") { // comma list, up to RTR_MAX_RADIOS slots
            uint32_t slot = 0;
            size_t p = 0;
            while (slot < RTR_MAX_RADIOS) {
                const size_t comma = val.find(',', p);
                out.radioFreqKhz[slot++] =
                    (uint32_t)std::strtoul(val.substr(p, comma - p).c_str(), nullptr, 10);
                if (comma == std::string::npos) break;
                p = comma + 1;
            }
        } else if (key == "activeradio") {
            const long a = std::strtol(val.c_str(), nullptr, 10);
            out.activeRadio = uint8_t(a < 0 ? 0 : (a >= RTR_MAX_RADIOS ? RTR_MAX_RADIOS - 1 : a));
        } else if (key == "player") {
            if (out.playerCount >= RTR_MAX_PLAYERS) continue;
            RtrPlayer p{};
            if (!parsePlayerLine(val, p)) return ParseResult::Malformed;
            out.players[out.playerCount++] = p;
        }
    }
    return versionSeen ? ParseResult::Ok : ParseResult::BadVersion;
}

// GET /health response: lets an integration confirm the plugin is present
// and protocol-compatible before streaming state.
inline std::string buildHealthText(const std::string& pluginVersion,
                                   const std::string& gameId)
{
    std::string out = "rtr=3\n";
    out += "plugin=" + pluginVersion + "\n";
    if (!gameId.empty()) out += "game=" + gameId + "\n";
    return out;
}

inline std::string buildTalkText(const RtrTalkMsg& msg)
{
    std::string out = "rtr=3\n";
    for (uint32_t i = 0; i < msg.count && i < RTR_TALK_MAX; ++i) {
        char line[RTR_NAME_LEN + 24];
        std::snprintf(line, sizeof(line), "talk=%s|%.2f\n",
                      msg.speakers[i].name, msg.speakers[i].amplitude);
        out += line;
    }
    return out;
}

// ---- listener ---------------------------------------------------------------

class HttpBridge {
public:
    using StateSink  = std::function<void(const RtrSharedState&)>;
    using TalkSource = std::function<RtrTalkMsg()>;
    using Logger     = std::function<void(const std::string&)>;

    // Optional diagnostics sink (e.g. the TS client log). Call before start().
    void setLogger(Logger log) { m_log = std::move(log); }

    // Reported by GET /health. Call before start().
    void setPluginVersion(std::string version) { m_pluginVersion = std::move(version); }

    bool start(uint16_t port, StateSink onState, TalkSource talk)
    {
        if (m_running.load()) return true;
        m_onState = std::move(onState);
        m_talk = std::move(talk);
#ifdef _WIN32
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
#endif
        m_listen = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_listen == INVALID_SOCKET) return false;
        const int one = 1;
#ifdef _WIN32
        // NOT SO_REUSEADDR: on Windows that allows silently double-binding an
        // in-use port (requests then go to an arbitrary listener). Fail loudly.
        ::setsockopt(m_listen, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                     reinterpret_cast<const char*>(&one), sizeof(one));
#else
        ::setsockopt(m_listen, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&one), sizeof(one));
#endif
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // localhost only
        if (::bind(m_listen, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(m_listen, 4) != 0) {
            closeSock(m_listen);
            return false;
        }
        m_running = true;
        m_thread = std::thread([this] { run(); });
        return true;
    }

    void stop()
    {
        m_running = false;
        closeSock(m_listen); // unblocks select/accept
        if (m_thread.joinable()) m_thread.join();
    }

    ~HttpBridge() { stop(); }

private:
    static void closeSock(SOCKET& s)
    {
        if (s == INVALID_SOCKET) return;
#ifdef _WIN32
        ::closesocket(s);
#else
        ::close(s);
#endif
        s = INVALID_SOCKET;
    }

    void run()
    {
        while (m_running.load()) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(m_listen, &fds);
            timeval tv{0, 200 * 1000}; // stop() responsiveness
            const int n = ::select(int(m_listen + 1), &fds, nullptr, nullptr, &tv);
            if (!m_running.load()) break;
            if (n <= 0) continue;
            SOCKET client = ::accept(m_listen, nullptr, nullptr);
            if (client == INVALID_SOCKET) continue;
            handle(client);
            closeSock(client);
        }
    }

    // One request per connection: read headers + body, parse, respond.
    void handle(SOCKET client)
    {
#ifdef _WIN32
        const DWORD timeoutMs = 2000;
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
#else
        timeval rto{2, 0};
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &rto, sizeof(rto));
#endif
        std::string req, body;
        size_t headerEnd = std::string::npos;
        size_t contentLen = 0;
        bool chunked = false;
        char buf[4096];
        while (req.size() < kMaxRequest) {
            const int got = int(::recv(client, buf, sizeof(buf), 0));
            if (got <= 0) return;
            req.append(buf, size_t(got));
            if (headerEnd == std::string::npos) {
                headerEnd = req.find("\r\n\r\n");
                if (headerEnd == std::string::npos) continue;
                std::string headers = req.substr(0, headerEnd);
                for (auto& c : headers) c = char(::tolower((unsigned char)c));
                chunked = headers.find("transfer-encoding:") != std::string::npos &&
                          headers.find("chunked") != std::string::npos;
                contentLen = parseContentLength(headers);
                if (contentLen > kMaxRequest) return;
                headerEnd += 4;
            }
            if (chunked) {
                bool complete = false;
                if (decodeChunked(req.substr(headerEnd), body, complete) && complete) break;
            } else if (req.size() >= headerEnd + contentLen) {
                body = req.substr(headerEnd, contentLen);
                break;
            }
        }
        if (headerEnd == std::string::npos) return;

        // "METHOD /path HTTP/1.1"
        std::string method, path;
        {
            const std::string line = req.substr(0, req.find("\r\n"));
            const size_t sp1 = line.find(' ');
            const size_t sp2 = line.find(' ', sp1 + 1);
            if (sp1 == std::string::npos || sp2 == std::string::npos) return;
            method = line.substr(0, sp1);
            path = line.substr(sp1 + 1, sp2 - sp1 - 1);
        }

        if (method == "GET" && (path == "/health" || path == "/")) {
            respond(client, "200 OK", buildHealthText(m_pluginVersion, m_gameId));
            return;
        }
        if (method != "POST" || path != "/state") {
            respond(client, "404 Not Found", "");
            return;
        }
        RtrSharedState st{};
        std::string gameId;
        switch (parseStateText(body, st, &gameId)) {
        case ParseResult::BadVersion:
            logReject("bad version", body);
            respond(client, "409 Conflict", "rtr=3\nerror=version mismatch\n");
            return;
        case ParseResult::Malformed:
            logReject("malformed", body);
            respond(client, "400 Bad Request", "rtr=3\nerror=malformed state\n");
            return;
        case ParseResult::Ok:
            break;
        }
        if (!gameId.empty() && gameId != m_gameId) {
            m_gameId = gameId;
            if (m_log) m_log("http bridge: game '" + gameId + "' connected");
        }
        if (m_onState) m_onState(st);
        respond(client, "200 OK", m_talk ? buildTalkText(m_talk()) : "rtr=3\n");
    }

    // Throttled: shows what a rejected body actually looked like, since the
    // sender is a client stack we don't control.
    void logReject(const char* why, const std::string& body)
    {
        if (!m_log) return;
        const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        if (now - m_lastRejectLogMs < 10000) return;
        m_lastRejectLogMs = now;
        std::string preview = body.substr(0, 200);
        for (auto& c : preview)
            if (!std::isprint((unsigned char)c)) c = '.';
        m_log(std::string("http bridge rejected state (") + why + "), body[" +
              std::to_string(body.size()) + "]: '" + preview + "'");
    }

    static size_t parseContentLength(const std::string& headers)
    {
        // case-insensitive scan for "content-length:"
        std::string lower = headers;
        for (auto& c : lower) c = char(::tolower((unsigned char)c));
        const size_t at = lower.find("content-length:");
        if (at == std::string::npos) return 0;
        return size_t(std::strtoul(lower.c_str() + at + 15, nullptr, 10));
    }

    static void respond(SOCKET client, const char* status, const std::string& body)
    {
        char header[160];
        std::snprintf(header, sizeof(header),
                      "HTTP/1.1 %s\r\nContent-Type: text/plain\r\n"
                      "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                      status, body.size());
        std::string out = header;
        out += body;
        ::send(client, out.c_str(), int(out.size()), 0);
        // Graceful shutdown: signal EOF, then drain until the peer closes.
        // A hard close with unread client bytes pending would turn into an
        // RST that kills the response mid-flight ("failure receiving data").
#ifdef _WIN32
        ::shutdown(client, SD_SEND);
#else
        ::shutdown(client, SHUT_WR);
#endif
        char sink[1024];
        while (::recv(client, sink, sizeof(sink), 0) > 0) {}
    }

    static constexpr size_t kMaxRequest = 64 * 1024;

    SOCKET m_listen = INVALID_SOCKET;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    StateSink m_onState;
    TalkSource m_talk;
    Logger m_log;
    std::string m_pluginVersion = "unknown";
    std::string m_gameId;           // bridge thread only
    uint64_t m_lastRejectLogMs = 0; // bridge thread only
};

} // namespace rtr
