// SAP (RFC 2974) announcer and listener for AES67 stream discovery (239.255.255.255:9875).
#pragma once
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <mutex>
#include <atomic>
#include "platform.h"
#include "sdp.h"

namespace aes67 {

struct SapEntry {
    std::string origin_ip;
    uint16_t    hash = 0;
    std::string sdp;
    StreamDesc  desc;        // parsed (if parse succeeded)
    bool        parsed = false;
    int64_t     last_seen_mono = 0;
};

class SapAgent {
public:
    ~SapAgent();
    // Start listening (and optionally announcing). announce_sdp may be empty.
    bool start(const std::string& iface_ip, bool listen, std::string& err);
    void stop();
    void set_announcement(const std::string& sdp_text);   // empty = stop announcing (sends deletion)
    std::vector<SapEntry> entries() const;                 // discovered streams (excluding our own)
    static std::vector<uint8_t> build_packet(const std::string& origin_ip, uint16_t hash, const std::string& sdp, bool deletion);
    static bool parse_packet(const uint8_t* b, size_t n, std::string& origin_ip, uint16_t& hash, std::string& sdp, bool& deletion);
private:
    void thread_main();
    void send_now(bool deletion);
    UdpSocket sock_;
    std::string iface_ip_;
    bool listen_ = false;
    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::mutex mu_;
    std::string ann_sdp_;
    uint16_t ann_hash_ = 0;
    int64_t last_ann_mono_ = 0;
    std::map<std::string, SapEntry> entries_;
};

} // namespace aes67
