// aes67vsc: command line tool. Imports/exports SDP, edits the configuration, lists
// interfaces and discovered streams, shows live status, and can run the engine headless.
#include "../core/config.h"
#include "../core/sdp.h"
#include "../core/engine.h"
#include "../core/status.h"
#include "../core/log.h"
#include "../core/platform.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <iostream>
#include <csignal>
#include <atomic>

using namespace aes67;
static const double kPi = 3.14159265358979323846;

static std::atomic<bool> g_stop{false};
static void on_sigint(int) { g_stop = true; }

static void usage() {
    printf(
        "AES67 Virtual Sound Card - command line\n"
        "usage: aes67vsc <command> [args]\n\n"
        "  show                      show configuration and live status\n"
        "  status                    show live status only\n"
        "  import <file.sdp>         set the receive stream from an SDP file\n"
        "  parse <file.sdp>          parse an SDP file and print what was understood\n"
        "  export [file.sdp]         write the SDP for the transmit stream (stdout if no file)\n"
        "  set <section.key=value>.. change settings, e.g. set tx.dest=239.69.1.2 general.interface=192.168.1.10\n"
        "  interfaces                list IPv4 network interfaces\n"
        "  discover [seconds]        listen for SAP announcements (default 5 s)\n"
        "  run [seconds] [--tone|--loop|--silence]  run the engine headless (default: loop input to output)\n"
        "  selftest [seconds]        send a tone to the TX group and receive it back; reports PASS/FAIL\n"
        "  config-path               print the configuration file path\n"
        "  reset-config              restore default configuration\n"
        "\nSettings (section.key): general.interface general.sample_rate general.buffer_frames general.link_offset_ms\n"
        "  general.log_level ptp.mode(auto|slave|master|off) ptp.domain ptp.priority1 rx.enabled rx.dest rx.port\n"
        "  rx.payload_type rx.encoding(L16|L24) rx.channels rx.channel_offset tx.enabled tx.name tx.dest tx.port\n"
        "  tx.payload_type tx.encoding tx.ptime_ms tx.ttl tx.dscp tx.sap_announce\n");
}

static void print_desc(const StreamDesc& d) {
    printf("  name          : %s\n", d.name.c_str());
    if (!d.info.empty()) printf("  info          : %s\n", d.info.c_str());
    printf("  destination   : %s:%u%s\n", d.dest.c_str(), d.port, d.is_multicast() ? " (multicast)" : "");
    if (!d.source.empty()) printf("  source        : %s\n", d.source.c_str());
    printf("  format        : %s / %d Hz / %d ch, payload type %d\n", encoding_name(d.encoding), d.sample_rate, d.channels, d.payload_type);
    printf("  packet time   : %g ms (%d frames)\n", d.ptime_ms, d.frames_per_packet());
    if (!d.refclk.empty()) printf("  ref clock     : %s\n", d.refclk.c_str());
    if (d.ptp_domain >= 0) printf("  ptp domain    : %d\n", d.ptp_domain);
    if (!d.mediaclk.empty()) printf("  media clock   : %s\n", d.mediaclk.c_str());
    if (!d.direction.empty()) printf("  direction     : %s\n", d.direction.c_str());
}

static void print_status(const StatusBlock& s) {
    int64_t age = wall_ms() - s.updated_ms;
    if (!s.engine_running || age > 3000) { printf("Driver: not running (no audio application has the device open)\n"); return; }
    printf("Driver: running (pid %u, %u Hz, buffer %u frames, local IP %s)\n", s.pid, s.sample_rate, s.buffer_frames, s.local_ip);
    printf("  PTP     : %s, master %s, offset %+.1f us, path delay %.1f us\n", ptp_state_name(s.ptp_state), s.ptp_master, s.ptp_offset_ns / 1000.0, s.ptp_path_delay_ns / 1000.0);
    printf("  Receive : %s, %u packets, %u lost, %u resyncs, %u bad, arrival offset %d frames\n", s.rx_active ? "active" : "no data", s.rx_packets, s.rx_lost, s.rx_resyncs, s.rx_bad, s.rx_offset_frames);
    printf("  Transmit: %u packets, %u underruns\n", s.tx_packets, s.tx_underruns);
    printf("  Engine  : %u periods, %u late\n", s.periods, s.late_periods);
    if (s.message[0]) printf("  Message : %s\n", s.message);
}

static bool apply_setting(Config& c, const std::string& kv, std::string& err) {
    size_t eq = kv.find('=');
    if (eq == std::string::npos) { err = "expected section.key=value: " + kv; return false; }
    std::string key = kv.substr(0, eq), v = kv.substr(eq + 1);
    auto I = [&] { return atoi(v.c_str()); };
    auto B = [&] { return v == "1" || v == "true" || v == "on" || v == "yes"; };
    if (key == "general.interface") c.interface_ip = v.empty() ? "auto" : v;
    else if (key == "general.sample_rate") c.sample_rate = I();
    else if (key == "general.buffer_frames") c.buffer_frames = I();
    else if (key == "general.link_offset_ms") c.link_offset_ms = atof(v.c_str());
    else if (key == "general.log_level") c.log_level = I();
    else if (key == "general.spin_us") c.spin_us = I();
    else if (key == "ptp.mode") c.ptp_mode = ptp_mode_from_name(v);
    else if (key == "ptp.domain") c.ptp_domain = I();
    else if (key == "ptp.priority1") c.ptp_priority1 = I();
    else if (key == "rx.enabled") c.rx_enabled = B();
    else if (key == "rx.dest") { c.rx.dest = v; if (!v.empty()) c.rx_enabled = true; }
    else if (key == "rx.source") c.rx.source = v;
    else if (key == "rx.port") c.rx.port = (uint16_t)I();
    else if (key == "rx.payload_type") c.rx.payload_type = I();
    else if (key == "rx.encoding") c.rx.encoding = encoding_from_name(v);
    else if (key == "rx.channels") c.rx.channels = I();
    else if (key == "rx.sample_rate") c.rx.sample_rate = I();
    else if (key == "rx.channel_offset") c.rx_channel_offset = I();
    else if (key == "rx.name") c.rx.name = v;
    else if (key == "tx.enabled") c.tx_enabled = B();
    else if (key == "tx.name") c.tx_name = v;
    else if (key == "tx.dest") c.tx_dest = v;
    else if (key == "tx.port") c.tx_port = (uint16_t)I();
    else if (key == "tx.payload_type") c.tx_payload_type = I();
    else if (key == "tx.encoding") c.tx_encoding = encoding_from_name(v);
    else if (key == "tx.ptime_ms") c.tx_ptime_ms = atof(v.c_str());
    else if (key == "tx.ttl") c.tx_ttl = I();
    else if (key == "tx.dscp") c.tx_dscp = I();
    else if (key == "tx.sap_announce") c.sap_announce = B();
    else { err = "unknown setting: " + key; return false; }
    // re-validate through the INI path
    std::string e2; Config d; d.from_ini(c.to_ini(), e2); c = d;
    return true;
}

// Builds the TX SDP without a running engine (uses the status block for the current PTP master if present).
static std::string export_sdp(const Config& c) {
    NetInterface ni = pick_interface(c.interface_ip);
    std::string gm;
    StatusShare sh; StatusBlock s;
    if (sh.read(s) && s.engine_running && wall_ms() - s.updated_ms < 3000 && s.ptp_master[0]) gm = s.ptp_master;
    else {
        uint8_t id[8] = {0, 0, 0, 0xFF, 0xFE, 0, 0, 0};
        if (ni.has_mac) { id[0] = ni.mac[0]; id[1] = ni.mac[1]; id[2] = ni.mac[2]; id[5] = ni.mac[3]; id[6] = ni.mac[4]; id[7] = ni.mac[5]; }
        gm = ptp_identity_string(id);
    }
    std::string refclk = c.ptp_mode == PtpMode::Off ? "" : ("ptp=IEEE1588-2008:" + gm + ":" + std::to_string(c.ptp_domain));
    StreamDesc d = c.tx_stream(ni.ipv4, refclk);
    d.session_id = std::to_string((uint32_t)(wall_ms() / 1000)); d.session_version = d.session_id;
    return sdp_generate(d);
}

// ---- headless engine host -------------------------------------------------
struct CliHost : AudioCallback {
    enum Mode { Loop, Tone, Silence } mode = Loop;
    int rate = 48000;
    double phase = 0;
    // selftest analysis: expect the received signal to be a clean sine at 'tone_hz'
    bool analyze = false;
    int64_t analyzed = 0, discontinuities = 0, nonzero = 0;
    int32_t prev = 0; bool have_prev = false;
    double tone_hz = 1000.0;
    void process(const int32_t* const* in, int32_t* const* out, int n, int64_t, int64_t, uint64_t) override {
        if (analyze) {
            // A clean sine at 1 kHz / 48 kHz has a bounded sample-to-sample step; a dropout or slip shows as a jump.
            double max_step = 2.0 * kPi * tone_hz / rate * 0.5 * 2147483647.0 * 1.15 + 1e6;
            for (int i = 0; i < n; i++) {
                int32_t v = in[0][i];
                if (v != 0) nonzero++;
                if (have_prev && nonzero > 0) { if (std::fabs((double)v - prev) > max_step) discontinuities++; analyzed++; }
                prev = v; have_prev = true;
            }
        }
        for (int i = 0; i < n; i++) {
            int32_t v = 0;
            if (mode == Tone) { v = (int32_t)(std::sin(phase) * 0.5 * 2147483647.0); phase += 2.0 * kPi * tone_hz / rate; if (phase > 2 * kPi) phase -= 2 * kPi; }
            else if (mode == Loop) v = in[0][i];
            out[0][i] = v;
            out[1][i] = mode == Loop ? in[1][i] : v;
        }
    }
};

static int cmd_run(Config c, int seconds, CliHost::Mode mode, bool selftest) {
    CliHost host; host.mode = mode; host.rate = c.sample_rate;
    if (selftest) {
        host.mode = CliHost::Tone; host.analyze = true;
        c.tx_enabled = true; c.rx_enabled = true; c.sap_announce = false;
        if (c.tx_dest.empty()) c.tx_dest = "239.69.0.1";
        c.rx = c.tx_stream("0.0.0.0", ""); c.rx.source.clear();
        c.rx_channel_offset = 0;
    }
    Engine eng;
    eng.set_watch_config(false);
    std::string err;
    if (!eng.start(c, c.buffer_frames, &host, err)) { fprintf(stderr, "engine start failed: %s\n", err.c_str()); return 2; }
    printf("engine running on %s; press Ctrl-C to stop\n", eng.local_ip().c_str());
    signal(SIGINT, on_sigint);
    int64_t t0 = mono_ns();
    int last = -1; uint32_t lost0 = 0, resync0 = 0;
    while (!g_stop) {
        sleep_ms(100);
        int el = (int)((mono_ns() - t0) / 1000000000LL);
        if (el != last) {
            last = el;
            StatusBlock s = eng.snapshot();
            printf("[%3ds] ptp=%s off=%+.1fus rx=%u lost=%u resync=%u bad=%u arr=%d tx=%u under=%u late=%u\n", el, ptp_state_name(s.ptp_state), s.ptp_offset_ns / 1000.0,
                   s.rx_packets, s.rx_lost, s.rx_resyncs, s.rx_bad, s.rx_offset_frames, s.tx_packets, s.tx_underruns, s.late_periods);
            fflush(stdout);
            if (selftest) {
                // reset analysis after the first 2 s of settling
                if (el == 2) { host.analyzed = 0; host.discontinuities = 0; host.nonzero = 0; lost0 = s.rx_lost; resync0 = s.rx_resyncs; }
            }
        }
        if (seconds > 0 && mono_ns() - t0 > (int64_t)seconds * 1000000000LL) break;
    }
    StatusBlock s = eng.snapshot();
    eng.stop();
    if (selftest) {
        bool ok = s.rx_packets > 0 && host.nonzero > 0 && host.discontinuities == 0 && s.rx_lost == lost0 && s.rx_resyncs == resync0;
        printf("selftest: rx packets %u, lost %u, resyncs %u, analysed %lld samples, discontinuities %lld, tx underruns %u -> %s\n",
               s.rx_packets, s.rx_lost - lost0, s.rx_resyncs - resync0, (long long)host.analyzed, (long long)host.discontinuities, s.tx_underruns, ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 1; }
    std::string cmd = argv[1];
    Config c; c.load();
    if (cmd == "config-path") { printf("%s\n", Config::default_path().c_str()); return 0; }
    if (cmd == "reset-config") { Config d; return d.save() ? 0 : 2; }
    if (cmd == "interfaces") {
        for (auto& i : list_interfaces()) printf("%-16s %s%s\n", i.ipv4.c_str(), i.name.c_str(), i.ipv4 == pick_interface(c.interface_ip).ipv4 ? "   (selected)" : "");
        return 0;
    }
    if (cmd == "parse" || cmd == "import") {
        if (argc < 3) { usage(); return 1; }
        std::string text, err;
        if (!read_file(argv[2], text)) { fprintf(stderr, "cannot read %s\n", argv[2]); return 2; }
        StreamDesc d;
        if (!sdp_parse(text, d, err)) { fprintf(stderr, "SDP error: %s\n", err.c_str()); return 2; }
        print_desc(d);
        if (cmd == "import") {
            if (d.sample_rate != c.sample_rate) printf("note: stream rate %d differs from device rate %d; device rate set to %d\n", d.sample_rate, c.sample_rate, d.sample_rate), c.sample_rate = d.sample_rate;
            if (d.channels > 2) printf("note: stream has %d channels; inputs 1-2 map to stream channels %d-%d (rx.channel_offset)\n", d.channels, c.rx_channel_offset + 1, c.rx_channel_offset + 2);
            c.rx = d; c.rx_sdp_text = text; c.rx_enabled = true;
            if (!c.save()) { fprintf(stderr, "cannot write %s\n", Config::default_path().c_str()); return 2; }
            printf("receive stream saved to %s\n", Config::default_path().c_str());
        }
        return 0;
    }
    if (cmd == "export") {
        std::string sdp = export_sdp(c);
        if (argc >= 3) { if (!write_file(argv[2], sdp)) { fprintf(stderr, "cannot write %s\n", argv[2]); return 2; } printf("wrote %s\n", argv[2]); }
        else fputs(sdp.c_str(), stdout);
        return 0;
    }
    if (cmd == "set") {
        if (argc < 3) { usage(); return 1; }
        for (int i = 2; i < argc; i++) { std::string err; if (!apply_setting(c, argv[i], err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; } }
        if (!c.save()) { fprintf(stderr, "cannot write %s\n", Config::default_path().c_str()); return 2; }
        printf("saved %s\n", Config::default_path().c_str());
        return 0;
    }
    if (cmd == "show" || cmd == "status") {
        if (cmd == "show") {
            printf("Config: %s\n", Config::default_path().c_str());
            printf("  interface %s, %d Hz, buffer %d, link offset %g ms, PTP %s domain %d\n", c.interface_ip.c_str(), c.sample_rate, c.buffer_frames, c.link_offset_ms, ptp_mode_name(c.ptp_mode), c.ptp_domain);
            printf("Receive (%s):\n", c.rx_enabled && !c.rx.dest.empty() ? "enabled" : "disabled");
            if (!c.rx.dest.empty()) print_desc(c.rx);
            printf("Transmit (%s):\n", c.tx_enabled ? "enabled" : "disabled");
            print_desc(c.tx_stream(pick_interface(c.interface_ip).ipv4, ""));
        }
        StatusShare sh; StatusBlock s;
        if (sh.read(s)) print_status(s); else printf("Driver: not running\n");
        return 0;
    }
    if (cmd == "discover") {
        int secs = argc >= 3 ? atoi(argv[2]) : 5;
        SapAgent sap; std::string err;
        std::string ip = pick_interface(c.interface_ip).ipv4;
        if (!sap.start(ip, true, err)) { fprintf(stderr, "%s\n", err.c_str()); return 2; }
        printf("listening for SAP announcements on %s for %d s...\n", ip.c_str(), secs);
        sleep_ms(secs * 1000);
        auto e = sap.entries();
        sap.stop();
        if (e.empty()) printf("no streams announced\n");
        for (auto& x : e) {
            printf("from %s:\n", x.origin_ip.c_str());
            if (x.parsed) print_desc(x.desc); else printf("  (unparseable SDP)\n");
        }
        return 0;
    }
    if (cmd == "run" || cmd == "selftest") {
        int secs = 0; CliHost::Mode mode = CliHost::Loop;
        for (int i = 2; i < argc; i++) {
            std::string a = argv[i];
            if (a == "--tone") mode = CliHost::Tone; else if (a == "--loop") mode = CliHost::Loop; else if (a == "--silence") mode = CliHost::Silence;
            else if (a.rfind("--", 0) == 0) { std::string err; if (!apply_setting(c, a.substr(2), err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; } }
            else secs = atoi(a.c_str());
        }
        if (cmd == "selftest" && secs == 0) secs = 6;
        return cmd_run(c, secs, mode, cmd == "selftest");
    }
    usage();
    return 1;
}
