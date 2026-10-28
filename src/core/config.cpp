#include "config.h"
#include "platform.h"
#include <sstream>
#include <cstdlib>
#include <cstring>

namespace aes67 {

const char* ptp_mode_name(PtpMode m) {
    switch (m) { case PtpMode::Auto: return "auto"; case PtpMode::Slave: return "slave"; case PtpMode::Master: return "master"; default: return "off"; }
}
PtpMode ptp_mode_from_name(const std::string& s) {
    if (s == "slave") return PtpMode::Slave;
    if (s == "master") return PtpMode::Master;
    if (s == "off") return PtpMode::Off;
    return PtpMode::Auto;
}

static std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '\n') o += "\\n"; else if (c == '\r') o += "\\r"; else if (c == '\\') o += "\\\\"; else o += c; }
    return o;
}
static std::string unesc(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) { char n = s[++i]; o += n == 'n' ? '\n' : n == 'r' ? '\r' : n; }
        else o += s[i];
    }
    return o;
}
static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::string Config::to_ini() const {
    std::ostringstream o;
    o << "; AES67 Virtual Sound Card configuration\n";
    o << "[general]\n";
    o << "interface=" << interface_ip << "\n";
    o << "sample_rate=" << sample_rate << "\n";
    o << "buffer_frames=" << buffer_frames << "\n";
    o << "link_offset_ms=" << link_offset_ms << "\n";
    o << "log_level=" << log_level << "\n";
    o << "spin_us=" << spin_us << "\n";
    o << "\n[ptp]\n";
    o << "mode=" << ptp_mode_name(ptp_mode) << "\n";
    o << "domain=" << ptp_domain << "\n";
    o << "priority1=" << ptp_priority1 << "\n";
    o << "sync_log_interval=" << ptp_sync_log_interval << "\n";
    o << "announce_log_interval=" << ptp_announce_log_interval << "\n";
    o << "\n[rx]\n";
    o << "enabled=" << (rx_enabled ? 1 : 0) << "\n";
    o << "name=" << esc(rx.name) << "\n";
    o << "dest=" << rx.dest << "\n";
    o << "source=" << rx.source << "\n";
    o << "port=" << rx.port << "\n";
    o << "payload_type=" << rx.payload_type << "\n";
    o << "encoding=" << encoding_name(rx.encoding) << "\n";
    o << "sample_rate=" << rx.sample_rate << "\n";
    o << "channels=" << rx.channels << "\n";
    o << "ptime_ms=" << rx.ptime_ms << "\n";
    o << "framecount=" << rx.framecount << "\n";
    o << "channel_offset=" << rx_channel_offset << "\n";
    o << "sdp=" << esc(rx_sdp_text) << "\n";
    o << "\n[tx]\n";
    o << "enabled=" << (tx_enabled ? 1 : 0) << "\n";
    o << "name=" << esc(tx_name) << "\n";
    o << "dest=" << tx_dest << "\n";
    o << "port=" << tx_port << "\n";
    o << "payload_type=" << tx_payload_type << "\n";
    o << "encoding=" << encoding_name(tx_encoding) << "\n";
    o << "ptime_ms=" << tx_ptime_ms << "\n";
    o << "ttl=" << tx_ttl << "\n";
    o << "dscp=" << tx_dscp << "\n";
    o << "sap_announce=" << (sap_announce ? 1 : 0) << "\n";
    o << "\n[bridge]\n";
    o << "autostart=" << (bridge_autostart ? 1 : 0) << "\n";
    o << "play=" << esc(bridge_play) << "\n";
    o << "source=" << esc(bridge_source) << "\n";
    o << "loopback=" << (bridge_loopback ? 1 : 0) << "\n";
    return o.str();
}

bool Config::from_ini(const std::string& text, std::string& err) {
    Config c; // defaults
    std::istringstream is(text);
    std::string line, sect;
    while (std::getline(is, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') { sect = line.substr(1, line.find(']') - 1); continue; }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        auto I = [&] { return atoi(v.c_str()); };
        auto F = [&] { return atof(v.c_str()); };
        if (sect == "general") {
            if (k == "interface") c.interface_ip = v.empty() ? "auto" : v;
            else if (k == "sample_rate") c.sample_rate = I();
            else if (k == "buffer_frames") c.buffer_frames = I();
            else if (k == "link_offset_ms") c.link_offset_ms = F();
            else if (k == "log_level") c.log_level = I();
            else if (k == "spin_us") c.spin_us = I();
        } else if (sect == "ptp") {
            if (k == "mode") c.ptp_mode = ptp_mode_from_name(v);
            else if (k == "domain") c.ptp_domain = I();
            else if (k == "priority1") c.ptp_priority1 = I();
            else if (k == "sync_log_interval") c.ptp_sync_log_interval = I();
            else if (k == "announce_log_interval") c.ptp_announce_log_interval = I();
        } else if (sect == "rx") {
            if (k == "enabled") c.rx_enabled = I() != 0;
            else if (k == "name") c.rx.name = unesc(v);
            else if (k == "dest") c.rx.dest = v;
            else if (k == "source") c.rx.source = v;
            else if (k == "port") c.rx.port = (uint16_t)I();
            else if (k == "payload_type") c.rx.payload_type = I();
            else if (k == "encoding") c.rx.encoding = encoding_from_name(v);
            else if (k == "sample_rate") c.rx.sample_rate = I();
            else if (k == "channels") c.rx.channels = I();
            else if (k == "ptime_ms") c.rx.ptime_ms = F();
            else if (k == "framecount") c.rx.framecount = I();
            else if (k == "channel_offset") c.rx_channel_offset = I();
            else if (k == "sdp") c.rx_sdp_text = unesc(v);
        } else if (sect == "bridge") {
            if (k == "autostart") c.bridge_autostart = I() != 0;
            else if (k == "play") c.bridge_play = unesc(v);
            else if (k == "source") c.bridge_source = unesc(v);
            else if (k == "loopback") c.bridge_loopback = I() != 0;
        } else if (sect == "tx") {
            if (k == "enabled") c.tx_enabled = I() != 0;
            else if (k == "name") c.tx_name = unesc(v);
            else if (k == "dest") c.tx_dest = v;
            else if (k == "port") c.tx_port = (uint16_t)I();
            else if (k == "payload_type") c.tx_payload_type = I();
            else if (k == "encoding") c.tx_encoding = encoding_from_name(v);
            else if (k == "ptime_ms") c.tx_ptime_ms = F();
            else if (k == "ttl") c.tx_ttl = I();
            else if (k == "dscp") c.tx_dscp = I();
            else if (k == "sap_announce") c.sap_announce = I() != 0;
        }
    }
    // sanity
    if (c.sample_rate != 44100 && c.sample_rate != 48000 && c.sample_rate != 96000) c.sample_rate = 48000;
    if (c.buffer_frames < 16 || c.buffer_frames > 8192) c.buffer_frames = 240;
    if (c.link_offset_ms < 0.5) c.link_offset_ms = 0.5;
    if (c.link_offset_ms > 500) c.link_offset_ms = 500;
    if (c.rx.encoding == Encoding::Unknown) c.rx.encoding = Encoding::L24;
    if (c.tx_encoding == Encoding::Unknown) c.tx_encoding = Encoding::L24;
    if (c.rx.dest.empty()) c.rx_enabled = false;
    if (c.tx_ptime_ms <= 0) c.tx_ptime_ms = 1.0;
    err.clear();
    *this = c;
    return true;
}

std::string Config::default_path() {
#ifdef _WIN32
    return config_dir() + "\\config.ini";
#else
    return config_dir() + "/config.ini";
#endif
}
bool Config::load(const std::string& path) {
    std::string p = path.empty() ? default_path() : path, text, err;
    if (!read_file(p, text)) return false;
    return from_ini(text, err);
}
bool Config::save(const std::string& path) const {
    std::string p = path.empty() ? default_path() : path;
    return write_file(p, to_ini());
}

StreamDesc Config::tx_stream(const std::string& local_ip, const std::string& refclk) const {
    StreamDesc d;
    d.name = tx_name;
    d.info = "2 channels: Left, Right";
    d.origin_addr = local_ip;
    d.source = local_ip;
    d.dest = tx_dest;
    d.dest_ttl = tx_ttl;
    d.port = tx_port;
    d.payload_type = tx_payload_type;
    d.encoding = tx_encoding;
    d.sample_rate = sample_rate;
    d.channels = 2;
    d.ptime_ms = tx_ptime_ms;
    d.framecount = 0;
    d.framecount = d.frames_per_packet();
    d.refclk = refclk;
    d.mediaclk = "direct=0";
    d.ptp_domain = ptp_mode == PtpMode::Off ? -1 : ptp_domain;
    d.direction = "recvonly";
    return d;
}

} // namespace aes67
