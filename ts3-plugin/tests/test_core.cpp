// Host-side tests for DSP + spatial logic (no TS3 or Windows deps).
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../src/radio_dsp.hpp"
#include "../src/gamelink.hpp"
#include "../src/httpbridge.hpp"
#include "../src/settings.hpp"

#include <sstream>

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

static void testContractV3()
{
    CHECK(RTR_VERSION == 3u);
    // occlusion consumed a pad byte: the packed layout must not have moved.
    CHECK(sizeof(RtrPlayer) == 80);
}

static float rmsOfSine(rtr::OnePoleLP& lp, float freqHz, float sampleRate, int n)
{
    double sum = 0.0;
    for (int i = 0; i < n; ++i) {
        const float y = lp.process(std::sin(2.0f * 3.14159265f * freqHz * i / sampleRate));
        if (i >= n / 2) sum += double(y) * double(y); // skip transient
    }
    return float(std::sqrt(sum / (n / 2)));
}

static void testOnePoleLowpass()
{
    const float sr = 48000.0f;
    const float sineRms = 0.7071f;
    rtr::OnePoleLP lo, hi;
    lo.setCutoff(sr, 500.0f);
    hi.setCutoff(sr, 500.0f);
    CHECK(rmsOfSine(lo, 100.0f, sr, 9600) > 0.9f * sineRms);  // below cutoff: passes
    CHECK(rmsOfSine(hi, 4000.0f, sr, 9600) < 0.35f * sineRms); // above: attenuated
    rtr::OnePoleLP dc; // default coefficient = passthrough
    CHECK(dc.process(0.5f) == 0.5f);
}

static void testOcclusionCutoffMapping()
{
    const rtr::OcclusionParams p{};
    CHECK(std::fabs(rtr::occlusionCutoffHz(0.0f) - p.bypassHz) < 1.0f);
    CHECK(std::fabs(rtr::occlusionCutoffHz(1.0f) - p.minCutoffHz) < 1.0f);
    const float mid = rtr::occlusionCutoffHz(0.5f);
    const float geo = std::sqrt(p.bypassHz * p.minCutoffHz); // log interp midpoint
    CHECK(std::fabs(mid - geo) < 0.01f * geo);
    CHECK(rtr::occlusionCutoffHz(0.25f) > mid && mid > rtr::occlusionCutoffHz(0.75f));
}

static void testOcclusionQuery()
{
    rtr::GameLink link;
    RtrSharedState st{};
    st.magic = RTR_MAGIC; st.version = RTR_VERSION;
    st.inGame = 1;
    st.listenerFwd = {1, 0, 0};
    st.listenerUp  = {0, 0, 1};
    st.playerCount = 2;
    std::strcpy(st.players[0].name, "alice");
    st.players[0].pos = {10, 0, 0};
    st.players[0].occlusion = 255;
    std::strcpy(st.players[1].name, "bob");
    st.players[1].pos = {5, 0, 0};
    st.players[1].occlusion = 85; // one wall
    link.setState(st);

    CHECK(std::fabs(link.query("alice").occlusion01 - 1.0f) < 1e-3f);
    CHECK(std::fabs(link.query("bob").occlusion01 - 85.0f / 255.0f) < 1e-3f);
}

static void testOcclusionSlewAndAtten()
{
    const rtr::OcclusionParams p{};
    rtr::OcclusionState st;
    std::vector<float> buf(480, 1.0f); // 10ms DC @ 48k

    // Warm up at o=0 so the filter has settled on the DC input (a live stream
    // ramps up from silence; a cold filter fed a DC step charges up audibly).
    rtr::applyOcclusion(buf.data(), 480, 0.0f, st);

    // Slew rate: after 10ms toward o=1, o has moved ~480 steps, not jumped.
    buf.assign(480, 1.0f);
    rtr::applyOcclusion(buf.data(), 480, 1.0f, st);
    CHECK(std::fabs(st.o - 480.0f * p.slewPerSample) < 1e-4f);
    // Click-free: no sudden jump between consecutive output samples.
    for (int i = 1; i < 480; ++i) CHECK(std::fabs(buf[i] - buf[i - 1]) < 0.02f);

    // Converged (>150ms): DC passes the lowpass at unity, so output settles
    // at the attenuation floor 1 - maxAtten.
    for (int f = 0; f < 40; ++f) {
        buf.assign(480, 1.0f);
        rtr::applyOcclusion(buf.data(), 480, 1.0f, st);
    }
    CHECK(std::fabs(st.o - 1.0f) < 1e-4f);
    CHECK(std::fabs(buf[479] - (1.0f - p.maxAtten)) < 0.01f);

    // o=0 stream stays untouched (bypass).
    rtr::OcclusionState clear;
    std::vector<float> dry(480, 0.25f);
    rtr::applyOcclusion(dry.data(), 480, 0.0f, clear);
    for (float v : dry) CHECK(v == 0.25f);
}

static void testHttpStateParse()
{
    const std::string body =
        "rtr=3\r\n"
        "ingame=1\n"
        "name=Alice\n"
        "lpos=1 2.5 3\n"
        "lfwd=0 0 1\n"
        "lup=0 1 0\n"
        "radioptt=1\n"
        "voiceptt=0\n"
        "freq=246000\n"
        "unknown=future stuff\n" // must be ignored, not rejected
        "player=Alice|1|2.5|3|1|0\n"
        "player=Bob Smith|4|5|6|0|170\n";
    RtrSharedState st{};
    CHECK(rtr::parseStateText(body, st) == rtr::ParseResult::Ok);
    CHECK(st.magic == RTR_MAGIC && st.version == RTR_VERSION);
    CHECK(st.inGame == 1 && st.radioPtt == 1 && st.voicePtt == 0);
    CHECK(std::strcmp(st.localName, "Alice") == 0);
    CHECK(st.listenerPos.y == 2.5f && st.listenerFwd.z == 1.0f && st.listenerUp.y == 1.0f);
    CHECK(st.radioFreqKhz[0] == 246000u);
    CHECK(st.playerCount == 2);
    CHECK(std::strcmp(st.players[1].name, "Bob Smith") == 0); // spaces survive
    CHECK(st.players[1].pos.x == 4.0f && st.players[1].alive == 0);
    CHECK(st.players[1].occlusion == 170);

    // The parsed state must satisfy the plugin's spatial query end to end.
    rtr::GameLink link;
    link.setState(st);
    auto q = link.query("Bob Smith");
    CHECK(q.inGame && q.found && std::fabs(q.occlusion01 - 170.0f / 255.0f) < 1e-3f);

    RtrSharedState bad{};
    CHECK(rtr::parseStateText("rtr=2\ningame=1\n", bad) == rtr::ParseResult::BadVersion);
    CHECK(rtr::parseStateText("ingame=1\n", bad) == rtr::ParseResult::BadVersion); // no version line
    CHECK(rtr::parseStateText("rtr=3\nlpos=not a vector\n", bad) == rtr::ParseResult::Malformed);
    CHECK(rtr::parseStateText("rtr=3\nplayer=NoCoords\n", bad) == rtr::ParseResult::Malformed);
}

static void testTransportArbitration()
{
    rtr::GameLink link;
    RtrSharedState native{};
    native.magic = RTR_MAGIC; native.version = RTR_VERSION;
    native.inGame = 1;
    std::strcpy(native.players[0].name, "ron-player");
    native.playerCount = 1;

    RtrSharedState http{};
    http.magic = RTR_MAGIC; http.version = RTR_VERSION;
    http.inGame = 0; // background game in menus

    // Native link fresh -> HTTP injection is refused, state untouched.
    link.setState(native);
    link.noteNativeUpdate(10000);
    CHECK(!link.injectState(http, 10500));
    CHECK(link.snapshot().inGame == 1);

    // Native stale (>1s) -> HTTP takes over.
    CHECK(link.injectState(http, 11500));
    CHECK(link.snapshot().inGame == 0);
}

static void testHttpMultiRadioAndGameId()
{
    RtrSharedState st{};
    std::string gameId;
    const std::string body =
        "rtr=3\n"
        "game=arma-reforger\n"
        "ingame=1\n"
        "freqs=42000,51000,63000\n"
        "activeradio=1\n"
        "ears=l,r,b\n";
    CHECK(rtr::parseStateText(body, st, &gameId) == rtr::ParseResult::Ok);
    CHECK(gameId == "arma-reforger");
    CHECK(st.radioFreqKhz[0] == 42000u && st.radioFreqKhz[1] == 51000u);
    CHECK(st.radioFreqKhz[2] == 63000u && st.radioFreqKhz[3] == 0u);
    CHECK(st.activeRadio == 1);
    CHECK(st.radioEars[0] == 1 && st.radioEars[1] == 2);
    CHECK(st.radioEars[2] == 0 && st.radioEars[3] == 0); // b + unsent = both

    // Digits work too; garbage falls back to "both"; no ears line = all both.
    RtrSharedState stEars{};
    CHECK(rtr::parseStateText("rtr=3\nears=1,2,x\n", stEars) == rtr::ParseResult::Ok);
    CHECK(stEars.radioEars[0] == 1 && stEars.radioEars[1] == 2 && stEars.radioEars[2] == 0);

    // activeradio clamps; freq= (singular) still fills slot 0
    RtrSharedState st2{};
    CHECK(rtr::parseStateText("rtr=3\nfreq=246000\nactiveradio=9\n", st2) == rtr::ParseResult::Ok);
    CHECK(st2.radioFreqKhz[0] == 246000u && st2.activeRadio == RTR_MAX_RADIOS - 1);

    CHECK(rtr::buildHealthText("0.5.0", "arma-reforger") ==
          "rtr=3\nplugin=0.5.0\ngame=arma-reforger\n");
    CHECK(rtr::buildHealthText("0.5.0", "") == "rtr=3\nplugin=0.5.0\n");
}

static void testHttpBodyNormalization()
{
    // Form-encoded wrapper: data=rtr%3D3%0Aingame%3D1 -> plain lines
    RtrSharedState st{};
    CHECK(rtr::parseStateText("data=rtr%3D3%0Aingame%3D1", st) == rtr::ParseResult::Ok);
    CHECK(st.inGame == 1);
    // Bare percent-encoding without the data= wrapper
    CHECK(rtr::parseStateText("rtr%3D3%0Aingame%3D1", st) == rtr::ParseResult::Ok);
    CHECK(st.inGame == 1);
    // Literal backslash-n instead of newlines
    CHECK(rtr::parseStateText("rtr=3\\ningame=1", st) == rtr::ParseResult::Ok);
    CHECK(st.inGame == 1);
    // Plain body must be untouched by normalization
    CHECK(rtr::parseStateText("rtr=3\ningame=1\n", st) == rtr::ParseResult::Ok);

    // Chunked transfer decoding
    std::string body;
    bool complete = false;
    CHECK(rtr::decodeChunked("6\r\nrtr=3\n\r\n9\r\ningame=1\n\r\n0\r\n\r\n", body, complete));
    CHECK(complete && body == "rtr=3\ningame=1\n");
    CHECK(!rtr::decodeChunked("6\r\nrtr=3\n\r\n9\r\ninga", body, complete)); // partial
    CHECK(!complete);
}

static void testTalkText()
{
    RtrTalkMsg msg{};
    msg.count = 2;
    std::strcpy(msg.speakers[0].name, "Alice");
    msg.speakers[0].amplitude = 0.83f;
    std::strcpy(msg.speakers[1].name, "Bob");
    msg.speakers[1].amplitude = 0.0f;
    CHECK(rtr::buildTalkText(msg) == "rtr=3\ntalk=Alice|0.83\ntalk=Bob|0.00\n");
    CHECK(rtr::buildTalkText(RtrTalkMsg{}) == "rtr=3\n"); // empty = nobody audible
}

static void testSettings()
{
    rtr::Settings s{};
    // set: case-insensitive key, clamped to range, unknown rejected
    CHECK(rtr::settingsSet(s, "OCCL.MaxAtten", 0.7f));
    CHECK(s.occlMaxAtten == 0.7f);
    CHECK(rtr::settingsSet(s, "occl.maxatten", 99.0f)); // clamps to 0.95
    CHECK(s.occlMaxAtten == 0.95f);
    std::string err;
    CHECK(!rtr::settingsSet(s, "no.such.key", 1.0f, &err) && !err.empty());

    // ini parse: comments/garbage skipped, values applied
    std::istringstream ini(
        "; comment\n"
        "prox.maxdist = 25   ; trailing comment\n"
        "occl.slewms=300\n"
        "garbage line\n"
        "occl.strength = notanumber\n");
    rtr::Settings p{};
    CHECK(rtr::settingsParseIni(p, ini) == 2);
    CHECK(p.proxMaxDistM == 25.0f && p.occlSlewMs == 300.0f);
    CHECK(p.occlStrength == rtr::Settings{}.occlStrength); // untouched

    // round trip: every key survives toIni -> parse
    rtr::Settings a{};
    for (const auto& d : rtr::settingsTable())
        a.*(d.field) = std::clamp(a.*(d.field) * 1.5f + 0.01f, d.min, d.max);
    std::istringstream back(rtr::settingsToIni(a));
    rtr::Settings b{};
    CHECK(rtr::settingsParseIni(b, back) == (int)rtr::settingsTable().size());
    for (const auto& d : rtr::settingsTable())
        CHECK(std::fabs(a.*(d.field) - b.*(d.field)) < 1e-4f * std::max(1.0f, a.*(d.field)));
}

static void testVoiceCompressor()
{
    const rtr::CompressorParams p{}; // thresh 0.20, ratio 3, makeup 1.4

    // Loud steady signal: env converges to the input level, output is pulled
    // well below input*makeup (0.9 * 1.4 = 1.26 uncompressed).
    rtr::CompressorState loudSt;
    std::vector<float> loud(48000, 0.9f);
    rtr::applyCompressor(loud.data(), int(loud.size()), loudSt, 48000.0f, p);
    CHECK(loud.back() > 0.4f && loud.back() < 0.75f);

    // Quiet signal below the threshold: only makeup gain applies.
    rtr::CompressorState quietSt;
    std::vector<float> quiet(48000, 0.1f);
    rtr::applyCompressor(quiet.data(), int(quiet.size()), quietSt, 48000.0f, p);
    CHECK(std::fabs(quiet.back() - 0.1f * p.makeup) < 0.01f);

    // Output never exceeds full scale, even on clipped input.
    rtr::CompressorState hotSt;
    std::vector<float> hot(4800, 1.0f);
    rtr::applyCompressor(hot.data(), int(hot.size()), hotSt, 48000.0f, p);
    for (float v : hot) CHECK(v <= 1.0f && v >= -1.0f);
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
    testContractV3();
    testOnePoleLowpass();
    testOcclusionCutoffMapping();
    testOcclusionQuery();
    testOcclusionSlewAndAtten();
    testHttpStateParse();
    testTransportArbitration();
    testHttpMultiRadioAndGameId();
    testHttpBodyNormalization();
    testTalkText();
    testSettings();
    testVoiceCompressor();
    testProximityMutesFar();
#ifndef _WIN32
    testUdpTransport();
#endif
    if (failures == 0) { std::printf("all tests passed\n"); return 0; }
    std::printf("%d check(s) failed\n", failures);
    return 1;
}
