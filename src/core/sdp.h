// SDP (RFC 4566) parse/generate with the AES67 / RAVENNA / Dante attributes we care about.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace aes67 {

enum class Encoding { L16, L24, Unknown };
const char* encoding_name(Encoding e);
Encoding encoding_from_name(const std::string& s);
int encoding_bytes(Encoding e);

struct StreamDesc {
    std::string name;            // s=
    std::string info;            // i=
    std::string origin_addr;     // o= address
    std::string session_id;      // o= session id
    std::string session_version; // o= version
    std::string dest;            // c= address (multicast group or unicast)
    int         dest_ttl = 32;   // c= /ttl
    std::string source;          // a=source-filter sender address (may be empty)
    uint16_t    port = 5004;     // m= port
    int         payload_type = 98;
    Encoding    encoding = Encoding::L24;
    int         sample_rate = 48000;
    int         channels = 2;
    double      ptime_ms = 1.0;      // a=ptime
    int         framecount = 0;      // a=framecount (0 = derive from ptime)
    std::string refclk;              // a=ts-refclk value, e.g. "ptp=IEEE1588-2008:00-..:0"
    std::string mediaclk;            // a=mediaclk value, e.g. "direct=0"
    int         ptp_domain = -1;     // from a=clock-domain or ts-refclk, -1 unknown
    std::string direction;           // sendonly / recvonly / sendrecv
    std::vector<std::string> extra;  // unrecognised attributes (kept for reference)

    int frames_per_packet() const;   // from framecount or ptime
    bool is_multicast() const;
    std::string summary() const;
};

// Parse SDP text. Returns false and fills err on a hard failure (no audio media, unsupported codec, ...).
bool sdp_parse(const std::string& text, StreamDesc& out, std::string& err);

// Generate AES67-style SDP for a stream we send.
std::string sdp_generate(const StreamDesc& d);

} // namespace aes67
