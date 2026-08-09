// Host-side tests for DSP + spatial logic (no TS3 or Windows deps).
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../src/radio_dsp.hpp"
#include "../src/gamelink.hpp"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void testDistanceGain()
{
    CHECK(rtr::distanceGain(0.0f) == 1.0f);
    CHECK(rtr::distanceGain(40.0f) == 0.0f);
    CHECK(rtr::distanceGain(45.0f) == 0.0f);
    const float g10 = rtr::distanceGain(10.0f);
    const float g30 = rtr::distanceGain(30.0f);
    CHECK(g10 > g30 && g30 > 0.0f && g10 < 1.0f);
}

static void testRadioEffectBounded()
{
    rtr::RadioEffect fx;
    std::vector<int16_t> buf(960 * 2); // 20ms stereo @ 48k
    for (size_t i = 0; i < buf.size(); ++i)
        buf[i] = int16_t(20000.0 * std::sin(0.05 * double(i)));
    fx.process(buf.data(), 960, 2);
    for (int16_t s : buf) CHECK(s >= -32768 && s <= 32767);
    // mono in both ears
    for (int i = 0; i < 960; ++i) CHECK(buf[i * 2] == buf[i * 2 + 1]);
}

static void testSeqlockAndQuery()
{
    rtr::GameLink link;
    RtrSharedState st{};
    st.magic = RTR_MAGIC; st.version = RTR_VERSION;
    st.inGame = 1;
    st.listenerPos = {0, 0, 0};
    st.listenerFwd = {1, 0, 0};
    st.listenerUp  = {0, 0, 1};
    st.playerCount = 2;
    std::strcpy(st.players[0].name, "alice");
    st.players[0].pos = {10, 0, 0};   // 10 m dead ahead
    std::strcpy(st.players[1].name, "bob");
    st.players[1].pos = {0, -20, 0};  // UE: fwd=+X, right=+Y, so -Y is hard left
    link.setState(st);

    auto qa = link.query("alice");
    CHECK(qa.inGame && qa.found);
    CHECK(std::fabs(qa.distM - 10.0f) < 1e-3f);
    CHECK(std::fabs(qa.pan) < 1e-3f); // straight ahead -> centered

    auto qb = link.query("bob");
    CHECK(qb.found && std::fabs(qb.distM - 20.0f) < 1e-3f);
    CHECK(std::fabs(qb.pan) > 0.9f); // fully to one side

    auto qc = link.query("nobody");
    CHECK(qc.inGame && !qc.found);
}

static void testProximityMutesFar()
{
    std::vector<int16_t> buf(480 * 2, 10000);
    rtr::PanState st;
    // gain 0 target: with slew it decays; run several frames to converge
    for (int f = 0; f < 40; ++f)
        rtr::applyProximity(buf.data(), 480, 2, 0.0f, 0.0f, st);
    CHECK(std::abs(buf[0]) < 200);
}

#ifndef _WIN32
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
static void testUdpTransport()
{
    rtr::GameLink link;
    CHECK(link.open()); // binds 127.0.0.1:RTR_UDP_PORT

    RtrSharedState st{};
    st.magic = RTR_MAGIC; st.version = RTR_VERSION;
    st.inGame = 1;
    st.playerCount = 1;
    std::strcpy(st.players[0].name, "carol");
    st.players[0].pos = {5, 0, 0};
    st.listenerFwd = {1, 0, 0};
    st.listenerUp  = {0, 0, 1};

    int tx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(RTR_UDP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(sendto(tx, &st, sizeof(st), 0, (sockaddr*)&addr, sizeof(addr)) == (ssize_t)sizeof(st));
    usleep(50000);

    CHECK(link.poll(1000));
    auto q = link.query("carol");
    CHECK(q.inGame && q.found && std::fabs(q.distM - 5.0f) < 1e-3f);

    // Staleness: no packets for >1s -> link reports dead, state cleared.
    CHECK(!link.poll(5000));
    CHECK(!link.query("carol").inGame);
    ::close(tx);
    link.close();
}
#endif

int main()
{
    testDistanceGain();
    testRadioEffectBounded();
    testSeqlockAndQuery();
    testProximityMutesFar();
#ifndef _WIN32
    testUdpTransport();
#endif
    if (failures == 0) { std::printf("all tests passed\n"); return 0; }
    std::printf("%d check(s) failed\n", failures);
    return 1;
}
