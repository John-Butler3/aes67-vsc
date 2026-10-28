#include "sdp.h"
#include <sstream>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <algorithm>

namespace aes67 {

const char* encoding_name(Encoding e) {
    switch (e) { case Encoding::L16: return "L16"; case Encoding::L24: return "L24"; default: return "?"; }
}
Encoding encoding_from_name(const std::string& s) {
    std::string u; for (char c : s) u += (char)toupper((unsigned char)c);
    if (u == "L16") return Encoding::L16;
    if (u == "L24") return Encoding::L24;
    return Encoding::Unknown;
}
int encoding_bytes(Encoding e) { return e == Encoding::L16 ? 2 : (e == Encoding::L24 ? 3 : 0); }

int StreamDesc::frames_per_packet() const {
    if (framecount > 0) return framecount;
    double f = ptime_ms * sample_rate / 1000.0;
    int n = (int)(f + 0.5);
    // 44.1 kHz @ 1 ms is 44.1 frames; AES67 uses 48 frames (1.09 ms) there.
    if (sample_rate == 44100 && n == 44) n = 48;
    return n > 0 ? n : 48;
}
bool StreamDesc::is_multicast() const {
    int a = atoi(dest.c_str());
    return a >= 224 && a <= 239;
}
std::string StreamDesc::summary() const {
    char buf[256];
    snprintf(buf, sizeof buf, "%s:%u %s/%d/%dch %gms pt%d", dest.c_str(), port, encoding_name(encoding), sample_rate, channels, ptime_ms, payload_type);
    return buf;
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
static std::vector<std::string> split(const std::string& s, char d) {
    std::vector<std::string> v; std::string cur; std::istringstream is(s);
    while (std::getline(is, cur, d)) v.push_back(cur);
    return v;
}
static std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> v; std::istringstream is(s); std::string w;
    while (is >> w) v.push_back(w);
    return v;
}

bool sdp_parse(const std::string& text, StreamDesc& out, std::string& err) {
    StreamDesc d;
    std::string session_c;
    int session_ttl = 32;
    bool in_media = false, media_done = false, have_media = false;
    bool media_c = false;
    std::string session_refclk, session_mediaclk;
    int session_domain = -1;
    std::vector<std::string> session_attrs;

    std::istringstream is(text);
    std::string raw;
    auto parse_c = [&](const std::string& v, std::string& addr, int& ttl) {
        auto p = split_ws(v);
        if (p.size() >= 3 && p[0] == "IN" && p[1] == "IP4") {
            auto a = split(p[2], '/');
            addr = a[0];
            if (a.size() > 1) ttl = atoi(a[1].c_str());
            return true;
        }
        return false;
    };
    auto parse_attr = [&](const std::string& v, bool media) {
        std::string key = v, val;
        size_t c = v.find(':');
        if (c != std::string::npos) { key = v.substr(0, c); val = trim(v.substr(c + 1)); }
        if (key == "rtpmap" && media) {
            // rtpmap:98 L24/48000/2
            auto p = split_ws(val);
            if (p.size() >= 2) {
                int pt = atoi(p[0].c_str());
                if (pt == d.payload_type || d.encoding == Encoding::Unknown) {
                    auto f = split(p[1], '/');
                    Encoding e = encoding_from_name(f[0]);
                    if (pt == d.payload_type) {
                        d.encoding = e;
                        if (f.size() > 1) d.sample_rate = atoi(f[1].c_str());
                        d.channels = f.size() > 2 ? atoi(f[2].c_str()) : 1;
                        if (e == Encoding::Unknown) err = "unsupported encoding '" + f[0] + "' (only L16/L24 PCM is supported)";
                    }
                }
            }
        } else if (key == "ptime") { d.ptime_ms = atof(val.c_str()); }
        else if (key == "maxptime") { /* ignore */ }
        else if (key == "framecount") { d.framecount = atoi(val.c_str()); }
        else if (key == "ts-refclk") {
            if (media) d.refclk = val; else session_refclk = val;
            // ptp=IEEE1588-2008:00-1D-C1-FF-FE-12-34-56:0  -> domain after the last ':'
            if (val.rfind("ptp=", 0) == 0) {
                size_t last = val.rfind(':');
                if (last != std::string::npos && last > 4) {
                    std::string dom = val.substr(last + 1);
                    bool numeric = !dom.empty() && std::all_of(dom.begin(), dom.end(), ::isdigit);
                    if (numeric) { if (media) d.ptp_domain = atoi(dom.c_str()); else session_domain = atoi(dom.c_str()); }
                }
            }
        }
        else if (key == "mediaclk") { if (media) d.mediaclk = val; else session_mediaclk = val; }
        else if (key == "clock-domain") {
            // clock-domain:PTPv2 0
            auto p = split_ws(val);
            if (p.size() >= 2) { int dom = atoi(p[1].c_str()); if (media) d.ptp_domain = dom; else session_domain = dom; }
        }
        else if (key == "source-filter") {
            // source-filter: incl IN IP4 239.69.1.2 192.168.1.10
            auto p = split_ws(val);
            if (p.size() >= 5 && p[0] == "incl") d.source = p[4];
        }
        else if (key == "sendonly" || key == "recvonly" || key == "sendrecv" || key == "inactive") d.direction = key;
        else if (key == "sync-time" || key == "mid" || key == "control") { /* ignore */ }
        else (media ? d.extra : session_attrs).push_back(v);
    };

    while (std::getline(is, raw)) {
        std::string line = trim(raw);
        if (line.size() < 2 || line[1] != '=') continue;
        char type = line[0];
        std::string v = line.substr(2);
        if (type == 'm') {
            if (have_media) { media_done = true; in_media = false; continue; } // only first audio stream
            auto p = split_ws(v);
            if (p.size() >= 4 && p[0] == "audio") {
                have_media = true; in_media = true;
                d.port = (uint16_t)atoi(p[1].c_str());
                d.payload_type = atoi(p[3].c_str());
                d.encoding = Encoding::Unknown; // set from rtpmap
                // static payload types
                if (d.payload_type == 10) { d.encoding = Encoding::L16; d.sample_rate = 44100; d.channels = 2; }
                if (d.payload_type == 11) { d.encoding = Encoding::L16; d.sample_rate = 44100; d.channels = 1; }
            } else {
                in_media = false; // skip video etc.
            }
            continue;
        }
        if (media_done) continue;
        if (!in_media) {
            switch (type) {
                case 's': d.name = v; break;
                case 'i': d.info = v; break;
                case 'o': { auto p = split_ws(v); if (p.size() >= 6) { d.session_id = p[1]; d.session_version = p[2]; d.origin_addr = p[5]; } break; }
                case 'c': parse_c(v, session_c, session_ttl); break;
                case 'a': parse_attr(v, false); break;
                default: break;
            }
        } else {
            switch (type) {
                case 'i': if (d.info.empty()) d.info = v; break;
                case 'c': { std::string a; int ttl = 32; if (parse_c(v, a, ttl)) { d.dest = a; d.dest_ttl = ttl; media_c = true; } break; }
                case 'a': parse_attr(v, true); break;
                default: break;
            }
        }
    }
    if (!have_media) { err = "no audio media (m=audio) found"; return false; }
    if (!media_c) { d.dest = session_c; d.dest_ttl = session_ttl; }
    if (d.dest.empty()) { err = "no connection address (c=) found"; return false; }
    if (d.refclk.empty()) d.refclk = session_refclk;
    if (d.mediaclk.empty()) d.mediaclk = session_mediaclk;
    if (d.ptp_domain < 0) d.ptp_domain = session_domain;
    if (d.encoding == Encoding::Unknown) {
        if (err.empty()) err = "no rtpmap for payload type " + std::to_string(d.payload_type);
        return false;
    }
    if (d.channels < 1) d.channels = 1;
    if (d.sample_rate < 8000) { err = "bad sample rate"; return false; }
    err.clear();
    out = d;
    return true;
}

std::string sdp_generate(const StreamDesc& d) {
    std::ostringstream o;
    std::string sid = d.session_id.empty() ? "1" : d.session_id;
    std::string sver = d.session_version.empty() ? sid : d.session_version;
    std::string oaddr = d.origin_addr.empty() ? (d.source.empty() ? "0.0.0.0" : d.source) : d.origin_addr;
    o << "v=0\r\n";
    o << "o=- " << sid << " " << sver << " IN IP4 " << oaddr << "\r\n";
    o << "s=" << (d.name.empty() ? "AES67 Stream" : d.name) << "\r\n";
    if (!d.info.empty()) o << "i=" << d.info << "\r\n";
    o << "c=IN IP4 " << d.dest;
    if (d.is_multicast()) o << "/" << d.dest_ttl;
    o << "\r\n";
    o << "t=0 0\r\n";
    if (d.ptp_domain >= 0) o << "a=clock-domain:PTPv2 " << d.ptp_domain << "\r\n";
    o << "m=audio " << d.port << " RTP/AVP " << d.payload_type << "\r\n";
    o << "a=rtpmap:" << d.payload_type << " " << encoding_name(d.encoding) << "/" << d.sample_rate << "/" << d.channels << "\r\n";
    if (!d.source.empty() && d.is_multicast()) o << "a=source-filter: incl IN IP4 " << d.dest << " " << d.source << "\r\n";
    o << "a=sync-time:0\r\n";
    o << "a=framecount:" << d.frames_per_packet() << "\r\n";
    {
        char pt[32];
        double ms = d.ptime_ms;
        if (ms == (int)ms) snprintf(pt, sizeof pt, "%d", (int)ms); else snprintf(pt, sizeof pt, "%.3f", ms);
        o << "a=ptime:" << pt << "\r\n";
    }
    if (!d.refclk.empty()) o << "a=ts-refclk:" << d.refclk << "\r\n";
    o << "a=mediaclk:" << (d.mediaclk.empty() ? "direct=0" : d.mediaclk) << "\r\n";
    o << "a=" << (d.direction.empty() ? "recvonly" : d.direction) << "\r\n";
    return o.str();
}

} // namespace aes67
