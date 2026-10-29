#include "ptp.h"
#include "log.h"
#include "status.h"
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <cmath>

namespace aes67 {

// PTP message types
enum : uint8_t { MT_SYNC = 0, MT_DELAY_REQ = 1, MT_FOLLOW_UP = 8, MT_DELAY_RESP = 9, MT_ANNOUNCE = 0xB };
static const char* kPtpGroup = "224.0.1.129";
static const int kHdr = 34;

static inline uint16_t rd16(const uint8_t* b) { return (uint16_t)((b[0] << 8) | b[1]); }
static inline uint32_t rd32(const uint8_t* b) { return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3]; }
static inline void wr16(uint8_t* b, uint16_t v) { b[0] = (uint8_t)(v >> 8); b[1] = (uint8_t)v; }
static inline void wr32(uint8_t* b, uint32_t v) { b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16); b[2] = (uint8_t)(v >> 8); b[3] = (uint8_t)v; }
static inline int64_t rd_corr(const uint8_t* b) { // correctionField: ns << 16, signed
    int64_t v = 0; for (int i = 0; i < 8; i++) v = (v << 8) | b[i];
    return v >> 16;
}

int64_t ptp_read_timestamp(const uint8_t* b) {
    uint64_t sec = ((uint64_t)rd16(b) << 32) | rd32(b + 2);
    uint32_t ns = rd32(b + 6);
    return (int64_t)sec * 1000000000LL + ns;
}
void ptp_write_timestamp(uint8_t* b, int64_t ns) {
    if (ns < 0) ns = 0;
    uint64_t sec = (uint64_t)(ns / 1000000000LL);
    uint32_t n = (uint32_t)(ns % 1000000000LL);
    wr16(b, (uint16_t)(sec >> 32)); wr32(b + 2, (uint32_t)sec); wr32(b + 6, n);
}
std::string ptp_identity_string(const uint8_t* id) {
    char buf[32];
    snprintf(buf, sizeof buf, "%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X", id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
    return buf;
}

PtpClock::PtpClock() {}
PtpClock::~PtpClock() { stop(); }

bool PtpClock::start(const PtpParams& p, std::string& err) {
    stop();
    p_ = p;
    // Clock identity: EUI-64 from MAC, or random.
    if (p.has_mac) {
        own_id_ = {p.mac[0], p.mac[1], p.mac[2], 0xFF, 0xFE, p.mac[3], p.mac[4], p.mac[5]};
    } else {
        uint64_t r = random_u64();
        for (int i = 0; i < 8; i++) own_id_[i] = (uint8_t)(r >> (8 * i));
    }
    memcpy(own_port_id_.data(), own_id_.data(), 8);
    own_port_id_[8] = 0; own_port_id_[9] = 1;

    servo_reset();
    start_mono_ = mono_ns();
    last_diag_log_mono_ = start_mono_; last_rejoin_mono_ = start_mono_; last_any_rx_mono_ = 0; seen_.clear();
    is_master_ = false; have_master_ = false; foreign_.clear();
    if (p.mode == PtpMode::Off) { set_state(PtpState::Off); return true; }

    auto open = [&](UdpSocket& s, uint16_t port) {
        if (!s.open()) { err = "socket: " + net_error_string(net_last_error()); return false; }
        s.set_reuse_addr(true);
        if (!s.bind_any(port)) { err = "bind udp " + std::to_string(port) + ": " + net_error_string(net_last_error()); return false; }
        if (!s.join_multicast(kPtpGroup, p.iface_ip)) { err = "join 224.0.1.129 on " + p.iface_ip + ": " + net_error_string(net_last_error()); return false; }
        s.set_multicast_if(p.iface_ip);
        s.set_multicast_ttl(p.ttl);
        s.set_multicast_loop(true);
        s.set_tos(46 << 2);
        s.set_recv_buffer(256 * 1024);
#ifdef _WIN32
        if (p.stack_timestamps) s.set_timestamping(true);
#else
        s.set_timestamping(true);
#endif
        return true;
    };
    if (!open(ev_, p.event_port) || !open(gen_, (uint16_t)(p.event_port + 1))) { ev_.close(); gen_.close(); return false; }
    running_ = true;
    set_state(PtpState::Listening);
    LOG_I("PTP: domain %d, mode %s, %s receive timestamps", p.domain, ptp_mode_name(p.mode), ev_.has_timestamping() ? "network-stack" : "user-mode");
    thread_ = std::thread([this] { thread_main(); });
    return true;
}

void PtpClock::stop() {
    if (running_) {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }
    ev_.close(); gen_.close();
    if (state_ != PtpState::Off) set_state(PtpState::Off);
}

void PtpClock::set_state(PtpState s) {
    if (state_.load() != s) {
        LOG_I("PTP state -> %s", ptp_state_name((uint32_t)s));
        state_ = s;
    }
}

// ---------------------------------------------------------------- servo
void PtpClock::servo_reset() {
    std::lock_guard<std::mutex> g(mu_);
    int64_t m = mono_ns();
    servo_.ref_mono = m;
    servo_.ref_ptp = wall_tai_ns();
    servo_.freq = 0.0;
    servo_.valid = true;
    delay_win_n_ = delay_win_i_ = 0;
    last_t1_ = last_t2_ = 0; jitter_ema_ = 0; outliers_in_row_ = 0; first_good_mono_ = last_good_mono_ = 0; ewin_n_ = ewin_i_ = 0; since_best_ = 0;
    last_servo_mono_ = 0;
}
int64_t PtpClock::mono_to_ptp(int64_t mono) const {
    std::lock_guard<std::mutex> g(mu_);
    double d = (double)(mono - servo_.ref_mono);
    return servo_.ref_ptp + (int64_t)std::llround(d * (1.0 + servo_.freq));
}
int64_t PtpClock::ptp_to_mono(int64_t ptp) const {
    std::lock_guard<std::mutex> g(mu_);
    double d = (double)(ptp - servo_.ref_ptp);
    return servo_.ref_mono + (int64_t)std::llround(d / (1.0 + servo_.freq));
}

// master_ns is the master's time corresponding to local mono time local_mono_ns.
void PtpClock::servo_update(int64_t master_ns, int64_t local_mono) {
    int64_t est = mono_to_ptp(local_mono);
    int64_t e = master_ns - est;   // positive: we are behind
    std::lock_guard<std::mutex> g(mu_);
    const int64_t kStep = 50 * 1000000LL;  // 50 ms
    if (std::llabs(e) > kStep || !servo_.valid) {
        // Hard step.
        servo_.ref_mono = local_mono;
        servo_.ref_ptp = master_ns;
        servo_.freq = 0.0;
        servo_.valid = true;
        last_servo_mono_ = local_mono;
        jitter_ema_ = 0; outliers_in_row_ = 0; first_good_mono_ = last_good_mono_ = 0;
        ewin_n_ = ewin_i_ = 0; since_best_ = 0;
        last_offset_ns_ = -e;
        return;
    }
    // Outlier gate: software timestamps occasionally arrive late by hundreds of microseconds.
    // Once we have a jitter estimate, ignore measurements far outside it (but never more than 8 in a row).
    double ae = (double)std::llabs(e);
    if (jitter_ema_ > 0 && ae > std::max(4.0 * jitter_ema_, 200000.0) && outliers_in_row_ < 8) {
        outliers_in_row_++; outlier_count_++;
        return;
    }
    outliers_in_row_ = 0;
    // Min-delay prefilter. Delay on the path (queuing, scheduling) always makes a measurement look
    // "earlier" (smaller e). The largest e in the recent window is therefore the cleanest sample:
    // use a new sample only if it is at least as good as the window, otherwise wait (at most 8 samples).
    ewin_[ewin_i_] = e; ewin_i_ = (ewin_i_ + 1) % 8; if (ewin_n_ < 8) ewin_n_++;
    int64_t best = ewin_[0]; for (int i = 1; i < ewin_n_; i++) best = std::max(best, ewin_[i]);
    bool recent = last_servo_mono_ && (local_mono - last_servo_mono_) < 1000000000LL;   // applied something within the last second
    if (ewin_n_ >= 4 && e < best - 20000 && since_best_ < 8 && recent) {
        // Skip this (more delayed) sample, but it still proves the clock is tracking.
        since_best_++;
        if (jitter_ema_ > 0 && ae < std::max(4.0 * jitter_ema_, 250000.0)) { if (!first_good_mono_) first_good_mono_ = local_mono; last_good_mono_ = local_mono; }
        return;
    }
    if (e < best && recent) e = best;     // apply the best of the window instead of a delayed sample
    since_best_ = 0;
    ae = (double)std::llabs(e);
    jitter_ema_ = jitter_ema_ > 0 ? jitter_ema_ * 0.9 + ae * 0.1 : ae;
    jitter_ns_ = (int64_t)jitter_ema_;
    last_offset_ns_ = -e;
    // Re-anchor at now (continuity), then apply PI correction.
    const double Kp = 0.1, Ki = 0.005;
    double dt = last_servo_mono_ ? (double)(local_mono - last_servo_mono_) : 1e9;
    if (dt < 1e6) dt = 1e6;
    servo_.ref_mono = local_mono;
    servo_.ref_ptp = est + (int64_t)std::llround(Kp * (double)e);
    servo_.freq += Ki * (double)e / dt;
    if (servo_.freq > 500e-6) servo_.freq = 500e-6;
    if (servo_.freq < -500e-6) servo_.freq = -500e-6;
    last_servo_mono_ = local_mono;
    // Lock bookkeeping: "good" while the smoothed error is below 250 us.
    if (jitter_ema_ < 250000.0) { if (!first_good_mono_) first_good_mono_ = local_mono; last_good_mono_ = local_mono; }
    else first_good_mono_ = 0;
}

// A (master time t1, local receive time t2) pair from Sync/Follow_Up.
void PtpClock::sync_measurement(int64_t t1, int64_t t2) {
    { std::lock_guard<std::mutex> g(mu_); last_t1_ = t1; last_t2_ = t2; }
    servo_update(t1 + path_delay_ns_.load(), t2);
    sync_count_++;
}

// ---------------------------------------------------------------- identity strings
std::string PtpClock::own_identity() const { return ptp_identity_string(own_id_.data()); }
std::string PtpClock::master_identity() const {
    std::lock_guard<std::mutex> g(mu_);
    if (have_master_ && !is_master_) return ptp_identity_string(cur_master_.gm_id.data());
    return ptp_identity_string(own_id_.data());
}
std::string PtpClock::refclk_string() const {
    return "ptp=IEEE1588-2008:" + master_identity() + ":" + std::to_string(p_.domain);
}

// ---------------------------------------------------------------- thread
void PtpClock::thread_main() {
    make_thread_realtime("aes67-ptp");
    uint8_t buf[512];
    while (running_) {
        fd_set rf; FD_ZERO(&rf);
        FD_SET(ev_.handle(), &rf); FD_SET(gen_.handle(), &rf);
        int maxfd = (int)std::max(ev_.handle(), gen_.handle()) + 1;
        timeval tv; tv.tv_sec = 0; tv.tv_usec = 10000;
        int r = select(maxfd, &rf, nullptr, nullptr, &tv);
        if (r > 0) {
            std::string from; uint16_t port; int64_t rx;
            if (FD_ISSET(ev_.handle(), &rf)) {
                int n = ev_.recv(buf, sizeof buf, &from, &port, &rx);
                if (n > 0) handle_event(buf, n, rx, from);
            }
            if (FD_ISSET(gen_.handle(), &rf)) {
                int n = gen_.recv(buf, sizeof buf, &from, &port, &rx);
                if (n > 0) handle_general(buf, n, rx, from);
            }
        }
        int64_t now = mono_ns();
        select_master(now);
        if (is_master_) master_tick(now); else slave_tick(now);
        // No PTP traffic at all for 10 s (and none from us as master): re-join the multicast group in case
        // the switch forgot the membership, and say what we see for the log.
        bool quiet; { std::lock_guard<std::mutex> g(mu_); quiet = (now - std::max(last_any_rx_mono_, start_mono_) > 10000000000LL); }
        if (!is_master_ && quiet && now - last_rejoin_mono_ > 10000000000LL) {
            last_rejoin_mono_ = now;
            for (UdpSocket* s : {&ev_, &gen_}) { s->leave_multicast(kPtpGroup, p_.iface_ip); s->join_multicast(kPtpGroup, p_.iface_ip); }
            LOG_W("PTP: no packets for 10 s on %s; re-joined %s", p_.iface_ip.c_str(), kPtpGroup);
        }
        if (!is_master_ && !have_master_ && now - last_diag_log_mono_ > 10000000000LL) {
            last_diag_log_mono_ = now;
            std::string seen = seen_summary();
            LOG_W("PTP: no master on domain %d yet; traffic seen: %s", p_.domain, seen.empty() ? "none" : seen.c_str());
        }
    }
}

void PtpClock::note_seen(const uint8_t* b, int n, int64_t rx_mono, const std::string& from) {
    if (n < 34) return;
    int ver = b[1] & 0x0F;
    if (ver != 1 && ver != 2) return;
    int dom = ver == 2 ? b[4] : 0;
    std::lock_guard<std::mutex> g(mu_);
    Seen& s = seen_[ver * 256 + dom];
    s.packets++; s.last_mono = rx_mono;
    if (s.sources.size() < 16) s.sources[from]++;
    last_any_rx_mono_ = rx_mono;
}
std::string PtpClock::seen_summary() const {
    std::lock_guard<std::mutex> g(mu_);
    std::string out; int64_t now = mono_ns();
    for (auto& kv : seen_) {
        if (now - kv.second.last_mono > 30000000000LL) continue;
        int ver = kv.first / 256, dom = kv.first % 256;
        if (!out.empty()) out += "; ";
        char b[160];
        if (ver == 1) snprintf(b, sizeof b, "PTPv1 (Dante native, not AES67) from %zu device(s)", kv.second.sources.size());
        else snprintf(b, sizeof b, "PTPv2 domain %d from %zu device(s)%s", dom, kv.second.sources.size(), dom == p_.domain ? "" : " (not our domain)");
        out += b;
    }
    return out;
}
bool PtpClock::anything_seen() const { std::lock_guard<std::mutex> g(mu_); return last_any_rx_mono_ && mono_ns() - last_any_rx_mono_ < 30000000000LL; }

static bool hdr_ok(const uint8_t* b, int n, int domain) {
    if (n < kHdr) return false;
    if ((b[1] & 0x0F) != 2) return false;       // versionPTP
    if (b[4] != domain) return false;
    return true;
}

void PtpClock::handle_event(const uint8_t* b, int n, int64_t rx_mono, const std::string& from) {
    note_seen(b, n, rx_mono, from);
    if (!hdr_ok(b, n, p_.domain)) return;
    if (memcmp(b + 20, own_port_id_.data(), 10) == 0) return;  // our own
    uint8_t type = b[0] & 0x0F;
    if (type == MT_SYNC && n >= kHdr + 10) {
        int64_t t1 = 0; bool one_step = false;
        {
            std::lock_guard<std::mutex> g(mu_);
            std::array<uint8_t, 10> pid; memcpy(pid.data(), b + 20, 10);
            sync_sources_[pid] = rx_mono;
            if (!have_master_ || is_master_ || memcmp(b + 20, cur_master_.port_id.data(), 10) != 0) return;
            uint16_t seq = rd16(b + 30);
            bool two_step = (b[6] & 0x02) != 0;
            last_sync_mono_ = rx_mono;
            if (two_step) {
                pending_sync_[seq] = rx_mono;
                if (pending_sync_.size() > 16) pending_sync_.erase(pending_sync_.begin());
            } else {
                t1 = ptp_read_timestamp(b + kHdr) + rd_corr(b + 8);
                one_step = true;
            }
        }
        if (one_step) sync_measurement(t1, rx_mono);
    } else if (type == MT_DELAY_REQ && n >= kHdr + 10) {
        if (is_master_) send_delay_resp(b, rx_mono);
    }
}

void PtpClock::handle_general(const uint8_t* b, int n, int64_t rx_mono, const std::string& from) {
    note_seen(b, n, rx_mono, from);
    if (!hdr_ok(b, n, p_.domain)) return;
    if (memcmp(b + 20, own_port_id_.data(), 10) == 0) return;
    uint8_t type = b[0] & 0x0F;
    if (type == MT_ANNOUNCE && n >= kHdr + 30) {
        handle_announce(b, n, rx_mono);
    } else if (type == MT_FOLLOW_UP && n >= kHdr + 10) {
        int64_t t1 = 0, t2 = 0; bool ok = false;
        {
            std::lock_guard<std::mutex> g(mu_);
            if (!have_master_ || is_master_ || memcmp(b + 20, cur_master_.port_id.data(), 10) != 0) return;
            uint16_t seq = rd16(b + 30);
            auto it = pending_sync_.find(seq);
            if (it == pending_sync_.end()) return;
            t2 = it->second; pending_sync_.erase(it);
            t1 = ptp_read_timestamp(b + kHdr) + rd_corr(b + 8);
            ok = true;
        }
        if (ok) sync_measurement(t1, t2);
    } else if (type == MT_DELAY_RESP && n >= kHdr + 20) {
        std::lock_guard<std::mutex> g(mu_);
        if (!have_master_ || is_master_) return;
        if (memcmp(b + kHdr + 10, own_port_id_.data(), 10) != 0) return;   // requestingPortIdentity
        if (rd16(b + 30) != dreq_seq_) return;
        int64_t t4 = ptp_read_timestamp(b + kHdr) + rd_corr(b + 8);
        int64_t t3 = dreq_sent_mono_;
        if (!last_t2_) return;
        // meanPathDelay = ((t2 - t3) + (t4 - t1)) / 2 : each difference is within one clock domain.
        int64_t rtt = (last_t2_ - t3) + (t4 - last_t1_);
        if (rtt < 0) rtt = 0;
        if (rtt > 100 * 1000000LL) return;  // nonsense
        int64_t mpd = rtt / 2;
        delay_win_[delay_win_i_] = mpd; delay_win_i_ = (delay_win_i_ + 1) % 8; if (delay_win_n_ < 8) delay_win_n_++;
        int64_t mn = delay_win_[0];
        for (int i = 1; i < delay_win_n_; i++) mn = std::min(mn, delay_win_[i]);
        path_delay_ns_ = mn;
    }
}

void PtpClock::handle_announce(const uint8_t* b, int, int64_t rx_mono) {
    ForeignMaster f;
    memcpy(f.port_id.data(), b + 20, 10);
    const uint8_t* body = b + kHdr;
    // body: originTimestamp(10) currentUtcOffset(2) reserved(1) gmPriority1(1) gmClockQuality(4) gmPriority2(1) gmIdentity(8) stepsRemoved(2) timeSource(1)
    f.priority1 = body[13];
    f.clock_class = body[14];
    f.accuracy = body[15];
    f.variance = rd16(body + 16);
    f.priority2 = body[18];
    memcpy(f.gm_id.data(), body + 19, 8);
    f.steps_removed = rd16(body + 27);
    f.log_announce = (int8_t)b[33];
    f.last_seen_mono = rx_mono;
    std::lock_guard<std::mutex> g(mu_);
    foreign_[f.port_id] = f;
}

static int cmp_dataset(const uint8_t p1a, uint8_t ca, uint8_t aa, uint16_t va, uint8_t p2a, const uint8_t* ida,
                       const uint8_t p1b, uint8_t cb, uint8_t ab, uint16_t vb, uint8_t p2b, const uint8_t* idb) {
    if (p1a != p1b) return p1a < p1b ? -1 : 1;
    if (ca != cb) return ca < cb ? -1 : 1;
    if (aa != ab) return aa < ab ? -1 : 1;
    if (va != vb) return va < vb ? -1 : 1;
    if (p2a != p2b) return p2a < p2b ? -1 : 1;
    return memcmp(ida, idb, 8);
}

bool PtpClock::foreign_better_than_us(const ForeignMaster& f) const {
    // Our dataset when acting as master: class 248 (free-running), accuracy unknown, variance max.
    return cmp_dataset(f.priority1, f.clock_class, f.accuracy, f.variance, f.priority2, f.gm_id.data(),
                       (uint8_t)p_.priority1, 248, 0xFE, 0xFFFF, (uint8_t)p_.priority2, own_id_.data()) < 0;
}

void PtpClock::select_master(int64_t now_mono) {
    std::lock_guard<std::mutex> g(mu_);
    // Expire foreign masters (3 announce intervals, min 3 s). Sync traffic also keeps a master alive.
    for (auto& kv : foreign_) { auto s = sync_sources_.find(kv.first); if (s != sync_sources_.end()) kv.second.last_seen_mono = std::max(kv.second.last_seen_mono, s->second); }
    for (auto it = foreign_.begin(); it != foreign_.end();) {
        double ivl = std::ldexp(1.0, it->second.log_announce);
        int64_t timeout = (int64_t)(std::max(3.0 * ivl, 3.0) * 1e9);
        if (now_mono - it->second.last_seen_mono > timeout) it = foreign_.erase(it); else ++it;
    }
    // A master that sends Sync but never Announce (seen on some devices / misconfigured domains):
    // after 3 s of Syncs with no Announce from anyone, treat it as a master with a default dataset
    // that beats our own free-running clock.
    for (auto it = sync_sources_.begin(); it != sync_sources_.end();) {
        if (now_mono - it->second > 5000000000LL) { it = sync_sources_.erase(it); continue; }
        if (foreign_.empty() && now_mono - start_mono_ > 3000000000LL && !is_master_) {
            ForeignMaster f; f.port_id = it->first; memcpy(f.gm_id.data(), it->first.data(), 8);
            f.priority1 = 128; f.clock_class = 248; f.accuracy = 0xFE; f.variance = 0xFFFF; f.priority2 = 128; f.log_announce = 1;
            f.last_seen_mono = it->second;
            foreign_[f.port_id] = f;
            LOG_I("PTP: %s sends Sync without Announce; following it anyway", ptp_identity_string(f.port_id.data()).c_str());
        }
        ++it;
    }
    const ForeignMaster* best = nullptr;
    for (auto& kv : foreign_) {
        const ForeignMaster& f = kv.second;
        if (f.clock_class == 255) continue; // slave-only
        if (!best || cmp_dataset(f.priority1, f.clock_class, f.accuracy, f.variance, f.priority2, f.gm_id.data(),
                                 best->priority1, best->clock_class, best->accuracy, best->variance, best->priority2, best->gm_id.data()) < 0)
            best = &f;
    }
    bool want_master = false;
    if (p_.mode == PtpMode::Master) {
        want_master = !best || !foreign_better_than_us(*best);
    } else if (p_.mode == PtpMode::Auto) {
        if (best) want_master = !foreign_better_than_us(*best);
        else want_master = (now_mono - start_mono_) > (int64_t)(p_.listen_seconds * 1e9);
    }
    if (want_master) {
        if (!is_master_) { mu_.unlock(); become_master(); mu_.lock(); }
        return;
    }
    if (best) {
        if (!have_master_ || is_master_ || memcmp(best->port_id.data(), cur_master_.port_id.data(), 10) != 0) {
            ForeignMaster f = *best;
            mu_.unlock(); become_slave(f); mu_.lock();
        } else {
            cur_master_ = *best; // refresh dataset
        }
    } else if (have_master_ && !is_master_) {
        // Lost our master.
        have_master_ = false;
        LOG_W("PTP master lost; free-running");
        set_state(p_.mode == PtpMode::Slave || (now_mono - start_mono_) > (int64_t)(p_.listen_seconds * 1e9) ? PtpState::FreeRunning : PtpState::Listening);
    } else if (!have_master_ && state_ == PtpState::Listening && p_.mode == PtpMode::Slave &&
               (now_mono - start_mono_) > (int64_t)(p_.listen_seconds * 1e9)) {
        set_state(PtpState::FreeRunning);
    }
}

void PtpClock::become_master() {
    {
        std::lock_guard<std::mutex> g(mu_);
        is_master_ = true; have_master_ = true;
        cur_master_ = ForeignMaster{};
        pending_sync_.clear();
        // Master time = free-running TAI wall clock.
        servo_.ref_mono = mono_ns(); servo_.ref_ptp = wall_tai_ns(); servo_.freq = 0; servo_.valid = true;
        last_offset_ns_ = 0; path_delay_ns_ = 0;
    }
    LOG_I("PTP: acting as master (%s), domain %d", own_identity().c_str(), p_.domain);
    set_state(PtpState::Master);
    last_sync_tx_mono_ = last_ann_tx_mono_ = 0;
}

void PtpClock::become_slave(const ForeignMaster& f) {
    {
        std::lock_guard<std::mutex> g(mu_);
        is_master_ = false; have_master_ = true; cur_master_ = f;
        pending_sync_.clear();
        delay_win_n_ = delay_win_i_ = 0; last_t1_ = last_t2_ = 0; jitter_ema_ = 0; outliers_in_row_ = 0; first_good_mono_ = last_good_mono_ = 0; ewin_n_ = ewin_i_ = 0; since_best_ = 0;
        servo_.valid = false;  // force a step on the first sync
        path_delay_ns_ = 0;
        last_dreq_mono_ = 0;
    }
    LOG_I("PTP: following master %s (gm %s, class %d, prio1 %d)", ptp_identity_string(f.port_id.data()).c_str(),
          ptp_identity_string(f.gm_id.data()).c_str(), f.clock_class, f.priority1);
    set_state(PtpState::Listening);
}

void PtpClock::slave_tick(int64_t now) {
    bool have; int64_t first_good, last_good;
    { std::lock_guard<std::mutex> g(mu_); have = have_master_; first_good = first_good_mono_; last_good = last_good_mono_; }
    if (!have) return;
    // Lock with hysteresis: locked after the error has stayed small for 1 s; unlocked only after it has
    // been large (or syncs missing) for 3 s. Brief timestamp noise therefore does not flap the state.
    if (state_ != PtpState::Locked && first_good && now - first_good > 1000000000LL) set_state(PtpState::Locked);
    if (state_ == PtpState::Locked && last_good && now - last_good > 3000000000LL) set_state(PtpState::Listening);
    if (last_sync_mono_ && now - last_sync_mono_ > 5000000000LL) {
        std::lock_guard<std::mutex> g(mu_);
        first_good_mono_ = 0;
    }
    // Delay request about once per second with jitter.
    if (now - last_dreq_mono_ > 900000000LL + (int64_t)(random_u32() % 200000000)) send_delay_req(now);
}

void PtpClock::fill_header(uint8_t* b, uint8_t type, uint16_t len, uint16_t seq, uint8_t control, int8_t log_interval, bool two_step) {
    memset(b, 0, len);
    b[0] = type & 0x0F;
    b[1] = 2;
    wr16(b + 2, len);
    b[4] = (uint8_t)p_.domain;
    b[6] = two_step ? 0x02 : 0x00;
    b[7] = 0x08; // PTP_TIMESCALE
    memcpy(b + 20, own_port_id_.data(), 10);
    wr16(b + 30, seq);
    b[32] = control;
    b[33] = (uint8_t)log_interval;
}

void PtpClock::send_delay_req(int64_t now) {
    uint8_t b[kHdr + 10];
    dreq_seq_++;
    fill_header(b, MT_DELAY_REQ, sizeof b, dreq_seq_, 1, 0x7F, false);
    int64_t t = mono_ns();
    ev_.send_to(b, sizeof b, kPtpGroup, p_.event_port);
    std::lock_guard<std::mutex> g(mu_);
    dreq_sent_mono_ = (t + mono_ns()) / 2;
    last_dreq_mono_ = now;
}

void PtpClock::master_tick(int64_t now) {
    double ann_ivl = std::ldexp(1.0, p_.announce_log_interval) * 1e9;
    double sync_ivl = std::ldexp(1.0, p_.sync_log_interval) * 1e9;
    if (now - last_ann_tx_mono_ >= (int64_t)ann_ivl) { send_announce(); last_ann_tx_mono_ = now; }
    if (now - last_sync_tx_mono_ >= (int64_t)sync_ivl) { send_sync(); last_sync_tx_mono_ = last_sync_tx_mono_ ? last_sync_tx_mono_ + (int64_t)sync_ivl : now; if (now - last_sync_tx_mono_ > (int64_t)sync_ivl) last_sync_tx_mono_ = now; }
}

void PtpClock::send_announce() {
    uint8_t b[kHdr + 30];
    ann_seq_++;
    fill_header(b, MT_ANNOUNCE, sizeof b, ann_seq_, 5, (int8_t)p_.announce_log_interval, false);
    uint8_t* body = b + kHdr;
    wr16(body + 10, 37);              // currentUtcOffset
    body[13] = (uint8_t)p_.priority1;
    body[14] = 248;                   // clockClass: default, not synchronized to a primary reference
    body[15] = 0xFE;                  // accuracy unknown
    wr16(body + 16, 0xFFFF);          // variance
    body[18] = (uint8_t)p_.priority2;
    memcpy(body + 19, own_id_.data(), 8);
    wr16(body + 27, 0);               // stepsRemoved
    body[29] = 0xA0;                  // timeSource: internal oscillator
    gen_.send_to(b, sizeof b, kPtpGroup, (uint16_t)(p_.event_port + 1));
}

void PtpClock::send_sync() {
    uint8_t s[kHdr + 10];
    sync_seq_++;
    fill_header(s, MT_SYNC, sizeof s, sync_seq_, 0, (int8_t)p_.sync_log_interval, true);
    int64_t t_before = mono_ns();
    ev_.send_to(s, sizeof s, kPtpGroup, p_.event_port);
    int64_t t_after = mono_ns();
    int64_t t1 = mono_to_ptp((t_before + t_after) / 2);
    uint8_t f[kHdr + 10];
    fill_header(f, MT_FOLLOW_UP, sizeof f, sync_seq_, 2, (int8_t)p_.sync_log_interval, false);
    ptp_write_timestamp(f + kHdr, t1);
    gen_.send_to(f, sizeof f, kPtpGroup, (uint16_t)(p_.event_port + 1));
}

void PtpClock::send_delay_resp(const uint8_t* req, int64_t rx_mono) {
    uint8_t b[kHdr + 20];
    fill_header(b, MT_DELAY_RESP, sizeof b, rd16(req + 30), 3, 0, false);
    ptp_write_timestamp(b + kHdr, mono_to_ptp(rx_mono));
    memcpy(b + kHdr + 10, req + 20, 10);
    gen_.send_to(b, sizeof b, kPtpGroup, (uint16_t)(p_.event_port + 1));
}

} // namespace aes67
