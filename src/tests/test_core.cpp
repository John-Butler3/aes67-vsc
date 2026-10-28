// Unit tests for the portable core. Build & run natively (no network needed except for the servo test).
#include "../core/sdp.h"
#include "../core/rtp.h"
#include "../core/ptp.h"
#include "../core/sap.h"
#include "../core/config.h"
#include "../core/platform.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { auto _a = (a); auto _b = (b); if (_a == _b) g_pass++; else { g_fail++; std::string sa = std::to_string(_a), sb = std::to_string(_b); fprintf(stderr, "FAIL %s:%d: %s == %s (%s vs %s)\n", __FILE__, __LINE__, #a, #b, sa.c_str(), sb.c_str()); } } while (0)
#define CHECK_STR(a, b) do { std::string _a = (a), _b = (b); if (_a == _b) g_pass++; else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s == %s ('%s' vs '%s')\n", __FILE__, __LINE__, #a, #b, _a.c_str(), _b.c_str()); } } while (0)

using namespace aes67;

static void test_sdp_dante() {
    // Typical Dante / AES67 flow SDP
    const char* sdp =
        "v=0\r\n"
        "o=- 1311738121 1311738121 IN IP4 192.168.1.50\r\n"
        "s=Dante-Device : 2 ch\r\n"
        "c=IN IP4 239.69.21.7/32\r\n"
        "t=0 0\r\n"
        "a=clock-domain:PTPv2 0\r\n"
        "m=audio 5004 RTP/AVP 98\r\n"
        "i=2 channels: 01, 02\r\n"
        "a=recvonly\r\n"
        "a=rtpmap:98 L24/48000/2\r\n"
        "a=ptime:1\r\n"
        "a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-50-A4-7B:0\r\n"
        "a=mediaclk:direct=0\r\n";
    StreamDesc d; std::string err;
    CHECK(sdp_parse(sdp, d, err));
    CHECK_STR(d.name, "Dante-Device : 2 ch");
    CHECK_STR(d.dest, "239.69.21.7");
    CHECK_EQ(d.dest_ttl, 32);
    CHECK_EQ((int)d.port, 5004);
    CHECK_EQ(d.payload_type, 98);
    CHECK(d.encoding == Encoding::L24);
    CHECK_EQ(d.sample_rate, 48000);
    CHECK_EQ(d.channels, 2);
    CHECK_EQ(d.frames_per_packet(), 48);
    CHECK_EQ(d.ptp_domain, 0);
    CHECK_STR(d.refclk, "ptp=IEEE1588-2008:00-1D-C1-FF-FE-50-A4-7B:0");
    CHECK_STR(d.origin_addr, "192.168.1.50");
    CHECK(d.is_multicast());
}

static void test_sdp_ravenna() {
    const char* sdp =
        "v=0\n"
        "o=- 5 0 IN IP4 10.0.0.7\n"
        "s=Ravenna Stream 8ch\n"
        "t=0 0\n"
        "a=ts-refclk:ptp=IEEE1588-2008:00-0B-72-FF-FE-A1-B2-C3:4\n"
        "a=mediaclk:direct=0\n"
        "m=audio 5004 RTP/AVP 97\n"
        "c=IN IP4 239.1.2.3/15\n"
        "a=rtpmap:97 L16/96000/8\n"
        "a=sync-time:0\n"
        "a=framecount:48\n"
        "a=ptime:0.5\n"
        "a=source-filter: incl IN IP4 239.1.2.3 10.0.0.7\n"
        "a=sendonly\n";
    StreamDesc d; std::string err;
    CHECK(sdp_parse(sdp, d, err));
    CHECK(d.encoding == Encoding::L16);
    CHECK_EQ(d.sample_rate, 96000);
    CHECK_EQ(d.channels, 8);
    CHECK_EQ(d.frames_per_packet(), 48);
    CHECK_EQ(d.dest_ttl, 15);
    CHECK_STR(d.source, "10.0.0.7");
    CHECK_EQ(d.ptp_domain, 4);
    CHECK_STR(d.direction, "sendonly");
    CHECK_EQ(d.payload_type, 97);
}

static void test_sdp_errors() {
    StreamDesc d; std::string err;
    CHECK(!sdp_parse("v=0\r\ns=x\r\nm=video 5004 RTP/AVP 96\r\n", d, err));
    CHECK(!sdp_parse("v=0\r\ns=x\r\nc=IN IP4 239.1.1.1\r\nm=audio 5004 RTP/AVP 96\r\na=rtpmap:96 AM824/48000/2\r\n", d, err));
    CHECK(err.find("unsupported") != std::string::npos);
    // 44.1 kHz @ 1 ms -> 48 frames per AES67 convention
    CHECK(sdp_parse("v=0\r\ns=x\r\nc=IN IP4 239.1.1.1\r\nm=audio 5004 RTP/AVP 96\r\na=rtpmap:96 L16/44100/2\r\na=ptime:1\r\n", d, err));
    CHECK_EQ(d.frames_per_packet(), 48);
    // static payload type 10 = L16/44100 stereo
    CHECK(sdp_parse("v=0\r\ns=x\r\nc=IN IP4 10.1.1.1\r\nm=audio 6000 RTP/AVP 10\r\n", d, err));
    CHECK(d.encoding == Encoding::L16 && d.sample_rate == 44100 && d.channels == 2 && !d.is_multicast());
}

static void test_sdp_roundtrip() {
    StreamDesc d;
    d.name = "AES67 VSC"; d.origin_addr = "192.168.10.5"; d.source = "192.168.10.5"; d.dest = "239.69.0.1"; d.dest_ttl = 32;
    d.port = 5004; d.payload_type = 98; d.encoding = Encoding::L24; d.sample_rate = 48000; d.channels = 2; d.ptime_ms = 1;
    d.refclk = "ptp=IEEE1588-2008:00-11-22-FF-FE-33-44-55:0"; d.mediaclk = "direct=0"; d.ptp_domain = 0; d.session_id = "42";
    std::string s = sdp_generate(d);
    CHECK(s.find("m=audio 5004 RTP/AVP 98\r\n") != std::string::npos);
    CHECK(s.find("a=rtpmap:98 L24/48000/2\r\n") != std::string::npos);
    CHECK(s.find("c=IN IP4 239.69.0.1/32\r\n") != std::string::npos);
    CHECK(s.find("a=source-filter: incl IN IP4 239.69.0.1 192.168.10.5\r\n") != std::string::npos);
    CHECK(s.find("a=ptime:1\r\n") != std::string::npos);
    CHECK(s.find("a=framecount:48\r\n") != std::string::npos);
    CHECK(s.find("a=clock-domain:PTPv2 0\r\n") != std::string::npos);
    StreamDesc e; std::string err;
    CHECK(sdp_parse(s, e, err));
    CHECK_STR(e.name, d.name); CHECK_STR(e.dest, d.dest); CHECK_EQ((int)e.port, (int)d.port);
    CHECK(e.encoding == d.encoding); CHECK_EQ(e.sample_rate, d.sample_rate); CHECK_EQ(e.channels, d.channels);
    CHECK_STR(e.refclk, d.refclk); CHECK_STR(e.source, d.source); CHECK_EQ(e.frames_per_packet(), 48);
}

static void test_rtp() {
    uint8_t buf[12 + 48 * 2 * 3];
    RtpHeader h; h.payload_type = 98; h.sequence = 0xFFFE; h.timestamp = 0xDEADBEEF; h.ssrc = 0x12345678; h.marker = true;
    rtp_write_header(buf, h);
    int32_t l[48], r[48];
    for (int i = 0; i < 48; i++) { l[i] = (int32_t)((i - 24) * 0x01000000); r[i] = (int32_t)(0x7FFFFF00 - i * 256); }
    const int32_t* src[2] = {l, r};
    pcm_pack(Encoding::L24, src, 48, 2, buf + 12);
    RtpHeader p; const uint8_t* pl; size_t pll;
    CHECK(rtp_parse(buf, sizeof buf, p, &pl, &pll));
    CHECK_EQ((int)p.payload_type, 98); CHECK_EQ((int)p.sequence, 0xFFFE); CHECK_EQ(p.timestamp, 0xDEADBEEFu); CHECK_EQ(p.ssrc, 0x12345678u);
    CHECK(p.marker); CHECK_EQ(pll, (size_t)(48 * 6)); CHECK_EQ(p.header_len, 12);
    int32_t ol[48], orr[48]; int32_t* dst[2] = {ol, orr};
    pcm_unpack(Encoding::L24, pl, 48, 2, 0, dst, 2);
    bool ok = true;
    for (int i = 0; i < 48; i++) { if (ol[i] != l[i] || orr[i] != (int32_t)(r[i] & 0xFFFFFF00)) ok = false; }
    CHECK(ok);
    // channel offset into an 8-channel L16 stream
    uint8_t b16[4 * 8 * 2]; memset(b16, 0, sizeof b16);
    b16[(0 * 8 + 3) * 2] = 0x12; b16[(0 * 8 + 3) * 2 + 1] = 0x34; // frame0 ch3
    b16[(0 * 8 + 4) * 2] = 0xAB; b16[(0 * 8 + 4) * 2 + 1] = 0xCD; // frame0 ch4
    int32_t a[4], bb[4]; int32_t* d2[2] = {a, bb};
    pcm_unpack(Encoding::L16, b16, 4, 8, 3, d2, 2);
    CHECK_EQ(a[0], (int32_t)0x12340000); CHECK_EQ(bb[0], (int32_t)0xABCD0000);
    // L16 pack rounds and saturates
    int32_t big[1] = {0x7FFFFFFF}; const int32_t* sb[1] = {big}; uint8_t o[2];
    pcm_pack(Encoding::L16, sb, 1, 1, o);
    CHECK(o[0] == 0x7F && o[1] == 0xFF);
    // malformed
    CHECK(!rtp_parse(buf, 5, p, &pl, &pll));
    buf[0] = 0x40; CHECK(!rtp_parse(buf, sizeof buf, p, &pl, &pll));
    // wrap-safe diffs
    CHECK_EQ(ts_diff(5, 0xFFFFFFFFu), 6); CHECK_EQ((int)seq_diff(2, 0xFFFF), 3);
}

static void test_ptp_ts() {
    uint8_t b[10];
    int64_t t = 1700000000LL * 1000000000LL + 123456789;
    ptp_write_timestamp(b, t);
    CHECK_EQ(ptp_read_timestamp(b), t);
    uint8_t id[8] = {0x00, 0x1D, 0xC1, 0xFF, 0xFE, 0x12, 0x34, 0x56};
    CHECK_STR(ptp_identity_string(id), "00-1D-C1-FF-FE-12-34-56");
}

static void test_servo() {
    // Simulated master: master = mono * (1 + 40ppm) + 12345678901 ns, with +-30 us timestamp noise.
    PtpClock c;
    c.servo_reset();
    int64_t m0 = mono_ns();
    const double drift = 40e-6; const int64_t off = 12345678901LL;
    auto master_at = [&](int64_t mono) { return (int64_t)std::llround((double)(mono - m0) * (1.0 + drift)) + m0 + off; };
    uint32_t seed = 1;
    int64_t worst_late = 0;
    for (int i = 0; i < 2000; i++) {
        int64_t mono = m0 + (int64_t)i * 125000000LL;
        seed = seed * 1664525u + 1013904223u;
        int64_t noise = (int64_t)(seed % 60001) - 30000;
        c.servo_update(master_at(mono) + noise, mono);
        if (i >= 1600) {
            int64_t probe = mono + 60000000LL;
            int64_t e = c.mono_to_ptp(probe) - master_at(probe);
            worst_late = std::max(worst_late, (int64_t)std::llabs(e));
        }
    }
    CHECK(worst_late < 60000);  // < 60 us steady-state error with 30 us noise
    if (worst_late >= 60000) fprintf(stderr, "servo steady-state error %lld ns\n", (long long)worst_late);
    // inverse mapping consistency
    int64_t x = mono_ns() + 123456789;
    CHECK(std::llabs(c.ptp_to_mono(c.mono_to_ptp(x)) - x) <= 1);
}

static void test_sap() {
    std::string sdp = "v=0\r\ns=test\r\n";
    auto p = SapAgent::build_packet("192.168.1.9", 0xBEEF, sdp, false);
    std::string origin, out; uint16_t hash; bool del;
    CHECK(SapAgent::parse_packet(p.data(), p.size(), origin, hash, out, del));
    CHECK_STR(origin, "192.168.1.9"); CHECK_EQ((int)hash, 0xBEEF); CHECK_STR(out, sdp); CHECK(!del);
    auto d = SapAgent::build_packet("10.0.0.1", 1, sdp, true);
    CHECK(SapAgent::parse_packet(d.data(), d.size(), origin, hash, out, del) && del);
    // packet without mime type
    std::vector<uint8_t> raw = {0x20, 0, 0, 7, 10, 0, 0, 2};
    raw.insert(raw.end(), sdp.begin(), sdp.end());
    CHECK(SapAgent::parse_packet(raw.data(), raw.size(), origin, hash, out, del) && out == sdp && origin == "10.0.0.2");
}

static void test_config() {
    Config c;
    c.interface_ip = "192.168.1.10"; c.sample_rate = 96000; c.buffer_frames = 192; c.link_offset_ms = 2.5;
    c.ptp_mode = PtpMode::Master; c.ptp_domain = 3;
    c.rx.name = "Some\nName"; c.rx.dest = "239.1.1.1"; c.rx.port = 5010; c.rx.encoding = Encoding::L16; c.rx.channels = 8; c.rx.sample_rate = 96000;
    c.rx_sdp_text = "v=0\r\ns=x\r\n"; c.rx_channel_offset = 2;
    c.tx_dest = "239.2.2.2"; c.tx_port = 5006; c.tx_name = "My TX"; c.tx_encoding = Encoding::L16; c.sap_announce = false;
    std::string ini = c.to_ini(), err;
    Config d;
    CHECK(d.from_ini(ini, err));
    CHECK_STR(d.interface_ip, c.interface_ip); CHECK_EQ(d.sample_rate, 96000); CHECK_EQ(d.buffer_frames, 192); CHECK(d.link_offset_ms == 2.5);
    CHECK(d.ptp_mode == PtpMode::Master); CHECK_EQ(d.ptp_domain, 3);
    CHECK_STR(d.rx.name, c.rx.name); CHECK_STR(d.rx.dest, c.rx.dest); CHECK_EQ((int)d.rx.port, 5010); CHECK(d.rx.encoding == Encoding::L16);
    CHECK_EQ(d.rx.channels, 8); CHECK_STR(d.rx_sdp_text, c.rx_sdp_text); CHECK_EQ(d.rx_channel_offset, 2);
    CHECK_STR(d.tx_dest, "239.2.2.2"); CHECK_EQ((int)d.tx_port, 5006); CHECK_STR(d.tx_name, "My TX"); CHECK(d.tx_encoding == Encoding::L16); CHECK(!d.sap_announce);
    StreamDesc t = d.tx_stream("192.168.1.10", "ptp=IEEE1588-2008:AA-BB-CC-FF-FE-DD-EE-FF:3");
    CHECK_EQ(t.frames_per_packet(), 96); CHECK_EQ(t.channels, 2); CHECK_STR(t.source, "192.168.1.10");
}

int main() {
    test_sdp_dante(); test_sdp_ravenna(); test_sdp_errors(); test_sdp_roundtrip();
    test_rtp(); test_ptp_ts(); test_servo(); test_sap(); test_config();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
