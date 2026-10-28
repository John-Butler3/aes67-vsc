// aes67vsc-bridge: console front end for the Windows audio bridge (see bridge_core.cpp).
#include "../core/platform.h"
#include <objbase.h>
#include "bridge_core.h"
#include "../core/log.h"
#include <cstdio>
#include <string>
#include <atomic>

using namespace aes67;
static std::atomic<bool> g_stop{false};
static BOOL WINAPI on_ctrl(DWORD) { g_stop = true; return TRUE; }

static void usage() {
    printf("aes67vsc-bridge: route the AES67 streams to/from Windows audio devices\n\n"
           "  aes67vsc-bridge --list\n"
           "  aes67vsc-bridge [--play <device|default|none>] [--capture <device|default|none>] [--loopback <device|default>]\n\n"
           "  --play      playback device on which the INCOMING stream (received from the network) is played\n"
           "  --capture   recording device whose audio becomes the OUTGOING stream (transmitted to the network)\n"
           "  --loopback  instead of --capture: transmit everything played on this playback device\n\n"
           "Without arguments the devices saved by the tray application are used.\n"
           "With VB-CABLE installed:  aes67vsc-bridge --play \"CABLE Input\" --loopback default\n"
           "then normal apps (Teams, browsers) record the AES67 audio from \"CABLE Output\".\n");
}

int main(int argc, char** argv) {
    Config cfg; cfg.load();
    std::string play = cfg.bridge_play, cap = cfg.bridge_source; bool loopback = cfg.bridge_loopback, list = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--list") list = true;
        else if (a == "--play") play = next();
        else if (a == "--capture") { cap = next(); loopback = false; }
        else if (a == "--loopback") { cap = next(); loopback = true; }
        else { usage(); return 1; }
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (list) {
        auto d = bridge_list_devices();
        printf("Playback devices:\n");  for (auto& x : d) if (x.render) printf("  %s\n", x.name.c_str());
        printf("Recording devices:\n"); for (auto& x : d) if (!x.render) printf("  %s\n", x.name.c_str());
        return 0;
    }
    log_set_file(config_dir() + "\\bridge.log");
    BridgeRunner br; std::string err;
    if (!br.start(cfg, play, cap, loopback, err)) { fprintf(stderr, "%s\n", err.c_str()); return 2; }
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    BridgeStats st = br.stats();
    printf("bridge running. incoming stream plays on: %s ; outgoing stream taken from: %s%s. Ctrl-C to stop.\n", st.play_name.empty() ? "(none)" : st.play_name.c_str(),
           st.source_name.empty() ? "(none)" : st.source_name.c_str(), loopback ? " (loopback)" : "");
    int n = 0;
    while (!g_stop) {
        sleep_ms(500);
        if (br.restart_needed()) {
            printf("configuration changed; restarting\n");
            cfg.load(); br.stop();
            if (!br.start(cfg, play, cap, loopback, err)) { fprintf(stderr, "%s\n", err.c_str()); return 2; }
        }
        if (++n % 10 == 0) {
            StatusBlock s = br.engine_status(); st = br.stats();
            printf("ptp=%s off=%+.1fus rx=%u lost=%u tx=%u | play fifo %zu under=%u | source fifo %zu under=%u%s%s\n",
                   ptp_state_name(s.ptp_state), s.ptp_offset_ns / 1000.0, s.rx_packets, s.rx_lost, s.tx_packets,
                   st.play_fifo, st.play_under, st.cap_fifo, st.cap_under, st.error.empty() ? "" : " | ERROR: ", st.error.c_str());
            fflush(stdout);
        }
    }
    br.stop();
    return 0;
}
