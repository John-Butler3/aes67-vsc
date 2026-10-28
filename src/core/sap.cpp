#include "sap.h"
#include "log.h"
#include <cstring>

namespace aes67 {

static const char* kSapGroup = "239.255.255.255";
static const uint16_t kSapPort = 9875;
static const int64_t kAnnounceInterval = 30LL * 1000000000LL;
static const int64_t kExpire = 120LL * 1000000000LL;

SapAgent::~SapAgent() { stop(); }

std::vector<uint8_t> SapAgent::build_packet(const std::string& origin_ip, uint16_t hash, const std::string& sdp, bool deletion) {
    std::vector<uint8_t> p;
    p.push_back((uint8_t)(0x20 | (deletion ? 0x04 : 0)));   // V=1, IPv4, T=deletion
    p.push_back(0);                                          // auth len
    p.push_back((uint8_t)(hash >> 8)); p.push_back((uint8_t)hash);
    in_addr a{}; inet_pton(AF_INET, origin_ip.c_str(), &a);
    const uint8_t* ab = (const uint8_t*)&a;
    p.insert(p.end(), ab, ab + 4);
    const char* mime = "application/sdp";
    p.insert(p.end(), mime, mime + strlen(mime) + 1);
    p.insert(p.end(), sdp.begin(), sdp.end());
    return p;
}

bool SapAgent::parse_packet(const uint8_t* b, size_t n, std::string& origin_ip, uint16_t& hash, std::string& sdp, bool& deletion) {
    if (n < 8) return false;
    uint8_t flags = b[0];
    if ((flags >> 5) != 1) return false;
    bool ipv6 = (flags & 0x10) != 0;
    deletion = (flags & 0x04) != 0;
    bool encrypted = (flags & 0x02) != 0;
    bool compressed = (flags & 0x01) != 0;
    if (encrypted || compressed) return false;
    size_t auth_len = (size_t)b[1] * 4;
    hash = (uint16_t)((b[2] << 8) | b[3]);
    size_t off = 4;
    if (ipv6) { if (n < 24) return false; origin_ip = "::"; off += 16; }
    else { char ip[32]; in_addr a; memcpy(&a, b + 4, 4); inet_ntop(AF_INET, &a, ip, sizeof ip); origin_ip = ip; off += 4; }
    off += auth_len;
    if (off >= n) return false;
    // Optional payload type string (NUL terminated) unless the payload starts with "v=0".
    if (!(n - off >= 3 && b[off] == 'v' && b[off + 1] == '=')) {
        size_t z = off;
        while (z < n && b[z] != 0) z++;
        if (z >= n) return false;
        std::string mime((const char*)b + off, z - off);
        if (mime.find("application/sdp") == std::string::npos) return false;
        off = z + 1;
    }
    sdp.assign((const char*)b + off, n - off);
    return true;
}

bool SapAgent::start(const std::string& iface_ip, bool listen, std::string& err) {
    stop();
    iface_ip_ = iface_ip; listen_ = listen;
    if (!sock_.open()) { err = "sap socket: " + net_error_string(net_last_error()); return false; }
    sock_.set_reuse_addr(true);
    if (listen) {
        if (!sock_.bind_any(kSapPort)) { err = "sap bind: " + net_error_string(net_last_error()); return false; }
        if (!sock_.join_multicast(kSapGroup, iface_ip)) { err = "sap join: " + net_error_string(net_last_error()); return false; }
    }
    sock_.set_multicast_if(iface_ip);
    sock_.set_multicast_ttl(32);
    sock_.set_multicast_loop(true);
    sock_.set_recv_timeout_ms(200);
    running_ = true;
    thread_ = std::thread([this] { thread_main(); });
    return true;
}

void SapAgent::stop() {
    if (running_) {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        if (!ann_sdp_.empty()) send_now(true);
    }
    sock_.close();
}

void SapAgent::set_announcement(const std::string& sdp) {
    std::lock_guard<std::mutex> g(mu_);
    if (sdp == ann_sdp_) return;
    if (!ann_sdp_.empty() && sock_.valid()) send_now(true);
    ann_sdp_ = sdp;
    // hash: stable per SDP text
    uint32_t h = 2166136261u;
    for (unsigned char c : sdp) { h ^= c; h *= 16777619u; }
    ann_hash_ = (uint16_t)(h & 0xFFFF);
    if (ann_hash_ == 0) ann_hash_ = 1;
    last_ann_mono_ = 0; // announce immediately
}

void SapAgent::send_now(bool deletion) {
    if (ann_sdp_.empty()) return;
    auto p = build_packet(iface_ip_, ann_hash_, ann_sdp_, deletion);
    sock_.send_to(p.data(), (int)p.size(), kSapGroup, kSapPort);
}

std::vector<SapEntry> SapAgent::entries() const {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<SapEntry> v;
    for (auto& kv : entries_) v.push_back(kv.second);
    return v;
}

void SapAgent::thread_main() {
    uint8_t buf[4096];
    while (running_) {
        if (listen_) {
            std::string from; uint16_t port; int64_t rx;
            int n = sock_.recv(buf, sizeof buf, &from, &port, &rx);
            if (n > 0) {
                std::string origin, sdp; uint16_t hash; bool del;
                if (parse_packet(buf, (size_t)n, origin, hash, sdp, del)) {
                    std::string key = origin + "#" + std::to_string(hash);
                    std::lock_guard<std::mutex> g(mu_);
                    bool ours = (origin == iface_ip_ && hash == ann_hash_);
                    if (del) entries_.erase(key);
                    else if (!ours) {
                        SapEntry& e = entries_[key];
                        e.origin_ip = origin; e.hash = hash; e.last_seen_mono = rx;
                        if (e.sdp != sdp) {
                            e.sdp = sdp;
                            std::string err;
                            e.parsed = sdp_parse(sdp, e.desc, err);
                        }
                    }
                }
            } else if (n < 0) {
                sleep_ms(100);
            }
        } else {
            sleep_ms(200);
        }
        int64_t now = mono_ns();
        std::lock_guard<std::mutex> g(mu_);
        if (!ann_sdp_.empty() && now - last_ann_mono_ >= kAnnounceInterval) { send_now(false); last_ann_mono_ = now; }
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (now - it->second.last_seen_mono > kExpire) it = entries_.erase(it); else ++it;
        }
    }
}

} // namespace aes67
