// Driver configuration (INI file in the per-user config directory).
#pragma once
#include <string>
#include <map>
#include "sdp.h"

namespace aes67 {

enum class PtpMode { Auto, Slave, Master, Off };
const char* ptp_mode_name(PtpMode m);
PtpMode ptp_mode_from_name(const std::string& s);

struct Config {
    // [general]
    std::string interface_ip = "auto";   // IPv4 of the NIC to use, or "auto"
    int sample_rate = 48000;             // 44100 / 48000 / 96000
    int channels = 2;                    // 2 = stereo 2x2 device and stream; 1 = mono 1x1 (sum), 1-channel transmit stream
    int buffer_frames = 240;             // preferred ASIO buffer size (multiple of the packet size)
    double link_offset_ms = 4.0;         // receive playout delay
    int log_level = 1;                   // 0 debug .. 3 error
    int spin_us = 150;                   // busy-wait window before each timer deadline
    std::string theme = "auto";          // panel appearance: auto (follow Windows) / light / dark

    // [ptp]
    PtpMode ptp_mode = PtpMode::Auto;
    int ptp_domain = 0;
    int ptp_priority1 = 200;             // when acting as master; real devices (128) win
    int ptp_sync_log_interval = -3;      // master: 8 syncs/s (AES67 media profile)
    int ptp_announce_log_interval = 1;   // master: every 2 s
    bool ptp_stack_timestamps = false;   // Windows: use SIO_TIMESTAMPING receive stamps (experimental); Linux always uses kernel stamps

    // [rx]
    bool rx_enabled = true;
    StreamDesc rx;                       // from SDP
    std::string rx_sdp_text;             // original SDP (kept verbatim)
    int rx_channel_offset = 0;           // first stream channel mapped to input 1

    // [tx]
    bool tx_enabled = true;
    std::string tx_name = "AES67 VSC";
    std::string tx_dest = "239.69.0.1";
    uint16_t tx_port = 5004;
    int tx_payload_type = 98;
    Encoding tx_encoding = Encoding::L24;
    double tx_ptime_ms = 1.0;
    int tx_ttl = 32;
    int tx_dscp = 34;                    // AF41 for media per AES67
    bool sap_announce = true;

    // [bridge] (Windows audio device bridge hosted by the tray application)
    bool bridge_autostart = false;         // start the bridge when the tray app starts
    std::string bridge_play = "default";   // playback device for the received stream ("none" to disable)
    std::string bridge_source = "default"; // recording device (or playback device when bridge_loopback) feeding the transmit stream
    bool bridge_loopback = false;

    std::string to_ini() const;
    bool from_ini(const std::string& text, std::string& err);
    static std::string default_path();   // <config_dir>/config.ini
    bool load(const std::string& path = "");
    bool save(const std::string& path = "") const;

    // Build the StreamDesc for our transmit stream.
    StreamDesc tx_stream(const std::string& local_ip, const std::string& refclk) const;
};

} // namespace aes67
