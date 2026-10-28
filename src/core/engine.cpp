#include "engine.h"
#include "rtp.h"
#include "log.h"
#include <cstring>
#include <algorithm>
#include <cmath>

namespace aes67 {

Engine::Engine() : stamps_(kRingSize) {
    ring_.resize(kRingSize);
    for (auto& s : stamps_) s.store(0xFFFFFFFFu, std::memory_order_relaxed);
}
Engine::~Engine() { stop(); }

uint32_t Engine::samples_at(int64_t ptp_ns) const {
    int64_t sec = ptp_ns / 1000000000LL, ns = ptp_ns % 1000000000LL;
    if (ns < 0) { ns += 1000000000LL; sec -= 1; }
    uint64_t s = (uint64_t)sec * (uint64_t)rate_ + ((uint64_t)ns * (uint64_t)rate_) / 1000000000ULL;
    return (uint32_t)s;
}

// ---------------------------------------------------------------- start/stop
bool Engine::start(const Config& cfg, int buffer_frames, AudioCallback* cb, std::string& err) {
    stop();
    cfg_ = cfg; cb_ = cb; buffer_frames_ = buffer_frames; rate_ = cfg.sample_rate;
    log_set_level((LogLevel)std::clamp(cfg.log_level, 0, 3));
    net_init();
    iface_ = pick_interface(cfg.interface_ip);
    if (iface_.ipv4.empty() || iface_.ipv4 == "0.0.0.0") { err = "no usable network interface"; return false; }
    LOG_I("engine start: iface %s (%s) rate %d buffer %d", iface_.ipv4.c_str(), iface_.name.c_str(), rate_, buffer_frames_);

    StreamDesc txd = cfg.tx_stream(iface_.ipv4, "");
    tx_fpp_ = txd.frames_per_packet();
    link_offset_frames_ = (int)std::lround(cfg.link_offset_ms * rate_ / 1000.0);

    share_.create(status_name_);
    cfg_mtime_ = file_mtime(Config::default_path());

    if (!start_ptp(err)) { LOG_W("PTP unavailable: %s (free-running)", err.c_str()); message_ = "PTP: " + err; err.clear(); }
    if (!open_tx(err)) { LOG_W("TX unavailable: %s", err.c_str()); message_ = "TX: " + err; err.clear(); }
    if (cfg.rx_enabled && !start_rx(err)) { LOG_W("RX unavailable: %s", err.c_str()); message_ = "RX: " + err; err.clear(); }
    {
        std::string serr;
        if (!sap_.start(iface_.ipv4, false, serr)) LOG_W("SAP: %s", serr.c_str());
    }
    running_ = true;
    eng_thread_ = std::thread([this] { engine_thread(); });
    hk_thread_ = std::thread([this] { housekeeping_thread(); });
    return true;
}

void Engine::stop() {
    if (!running_) return;
    running_ = false;
    if (eng_thread_.joinable()) eng_thread_.join();
    if (hk_thread_.joinable()) hk_thread_.join();
    stop_rx();
    sap_.set_announcement("");
    sap_.stop();
    close_tx();
    ptp_.stop();
    if (share_.ptr()) { share_.ptr()->engine_running = 0; share_.ptr()->updated_ms = wall_ms(); }
    share_.close();
    LOG_I("engine stopped");
}

bool Engine::start_ptp(std::string& err) {
    PtpParams p;
    p.iface_ip = iface_.ipv4; p.domain = cfg_.ptp_domain; p.mode = cfg_.ptp_mode;
    p.priority1 = cfg_.ptp_priority1; p.sync_log_interval = cfg_.ptp_sync_log_interval; p.announce_log_interval = cfg_.ptp_announce_log_interval;
    memcpy(p.mac, iface_.mac, 6); p.has_mac = iface_.has_mac;
    // Test hooks: alternate port pair, and a random clock identity so two instances on one host differ.
    const char* e = getenv("AES67VSC_PTP_PORT"); if (e && atoi(e) > 0) p.event_port = (uint16_t)atoi(e);
    if (getenv("AES67VSC_PTP_RANDOM_ID")) p.has_mac = false;
    return ptp_.start(p, err);
}

// ---------------------------------------------------------------- TX
bool Engine::open_tx(std::string& err) {
    std::lock_guard<std::mutex> g(tx_mu_);
    tx_sock_.close();
    tx_enabled_ = false;
    tx_desc_ = cfg_.tx_stream(iface_.ipv4, ptp_.refclk_string());
    tx_fpp_ = tx_desc_.frames_per_packet();
    tx_fifo_cap_ = (size_t)(buffer_frames_ * 4 + tx_fpp_ * 8);
    for (int c = 0; c < kChannels; c++) tx_fifo_[c].assign(tx_fifo_cap_, 0);
    tx_fifo_r_ = tx_fifo_w_ = 0;
    if (!cfg_.tx_enabled) return true;
    if (!tx_sock_.open()) { err = "socket: " + net_error_string(net_last_error()); return false; }
    if (!tx_sock_.bind_addr(iface_.ipv4, 0)) { err = "bind " + iface_.ipv4 + ": " + net_error_string(net_last_error()); return false; }
    tx_sock_.set_multicast_if(iface_.ipv4);
    tx_sock_.set_multicast_ttl(cfg_.tx_ttl);
    tx_sock_.set_multicast_loop(true);
    tx_sock_.set_tos(cfg_.tx_dscp << 2);
    tx_sock_.set_send_buffer(256 * 1024);
    if (!tx_ssrc_) tx_ssrc_ = random_u32();
    tx_seq_ = (uint16_t)random_u32();
    tx_enabled_ = true;
    LOG_I("TX: %s", tx_desc_.summary().c_str());
    return true;
}
void Engine::close_tx() {
    std::lock_guard<std::mutex> g(tx_mu_);
    tx_sock_.close(); tx_enabled_ = false;
}
std::string Engine::tx_sdp() const {
    std::lock_guard<std::mutex> g(const_cast<std::mutex&>(tx_mu_));
    StreamDesc d = tx_desc_;
    d.refclk = ptp_.refclk_string();
    if (d.session_id.empty()) { d.session_id = std::to_string((uint32_t)(wall_ms() / 1000)); d.session_version = d.session_id; }
    return sdp_generate(d);
}

// ---------------------------------------------------------------- RX
bool Engine::start_rx(std::string& err) {
    std::lock_guard<std::mutex> g(rx_mu_);
    if (rx_running_) { rx_running_ = false; if (rx_thread_.joinable()) rx_thread_.join(); }
    rx_sock_.close();
    rx_desc_ = cfg_.rx; rx_ch_off_ = cfg_.rx_channel_offset;
    for (auto& s : stamps_) s.store(0xFFFFFFFFu, std::memory_order_relaxed);
    { std::lock_guard<std::mutex> o(off_mu_); off_valid_ = false; off_win_valid_ = false; }
    rx_ssrc_ = 0; rx_first_mono_ = 0; rx_last_mono_ = 0;
    if (rx_desc_.dest.empty()) { err = "no receive stream configured"; return false; }
    if (rx_desc_.sample_rate != rate_) { err = "stream rate " + std::to_string(rx_desc_.sample_rate) + " != device rate " + std::to_string(rate_); return false; }
    if (!rx_sock_.open()) { err = "socket: " + net_error_string(net_last_error()); return false; }
    rx_sock_.set_reuse_addr(true);
    rx_sock_.set_recv_buffer(1024 * 1024);
    rx_sock_.set_recv_timeout_ms(200);
    bool mc = rx_desc_.is_multicast();
    if (!rx_sock_.bind_any(rx_desc_.port)) { err = "bind udp " + std::to_string(rx_desc_.port) + ": " + net_error_string(net_last_error()); return false; }
    if (mc && !rx_sock_.join_multicast(rx_desc_.dest, iface_.ipv4, rx_desc_.source)) {
        if (rx_desc_.source.empty() || !rx_sock_.join_multicast(rx_desc_.dest, iface_.ipv4)) {
            err = "join " + rx_desc_.dest + ": " + net_error_string(net_last_error()); return false;
        }
    }
    rx_sock_.set_timestamping(true);
    rx_running_ = true;
    rx_thread_ = std::thread([this] { rx_thread(); });
    LOG_I("RX: %s (%s)", rx_desc_.summary().c_str(), rx_desc_.name.c_str());
    return true;
}
void Engine::stop_rx() {
    std::lock_guard<std::mutex> g(rx_mu_);
    if (rx_running_) { rx_running_ = false; if (rx_thread_.joinable()) rx_thread_.join(); }
    rx_sock_.close();
}

void Engine::rx_note_arrival(uint32_t pkt_ts, int64_t rx_mono, int frames) {
    // d = (packet first-frame timestamp) - (local sample clock at arrival). We track the maximum
    // (earliest arrivals) so that the playout pointer, offset by the link offset, stays behind the data.
    uint32_t local = samples_at(ptp_.mono_to_ptp(rx_mono));
    int32_t d = ts_diff(pkt_ts, local);
    std::lock_guard<std::mutex> g(off_mu_);
    const int32_t big = std::max(link_offset_frames_.load() / 2, frames * 2);
    if (!off_valid_ || d - off_est_ > big) {
        // First packet, or data arriving much earlier than before (sender restarted / clock stepped): re-anchor now.
        if (off_valid_) { rx_resyncs_++; rx_first_mono_ = rx_mono; LOG_I("RX resync: arrival offset %d -> %d frames", off_est_, d); }
        off_valid_ = true; off_est_ = d; off_win_max_ = d; off_win_start_ = rx_mono; off_win_valid_ = true;
        off_est_pub_ = d; off_epoch_++;
        return;
    }
    // Later-than-usual packets are never followed immediately (they are just late); the estimate only
    // moves down once per second to the maximum seen in that second, which tracks slow drift.
    if (d > off_est_) { off_est_ = d; off_est_pub_ = d; }
    if (!off_win_valid_ || d > off_win_max_) off_win_max_ = d;
    off_win_valid_ = true;
    if (rx_mono - off_win_start_ > 1000000000LL) {
        if (off_win_max_ < off_est_) {
            if (off_est_ - off_win_max_ > big) { rx_resyncs_++; off_epoch_++; LOG_I("RX resync: arrival offset %d -> %d frames", off_est_, off_win_max_); }
            off_est_ = off_win_max_; off_est_pub_ = off_est_;
        }
        off_win_start_ = rx_mono; off_win_valid_ = false;
    }
}

void Engine::rx_thread() {
    make_thread_realtime("aes67-rx");
    std::vector<uint8_t> buf(2048);
    const Encoding enc = rx_desc_.encoding;
    const int sch = rx_desc_.channels, bps = encoding_bytes(enc);
    const int frame_bytes = sch * bps;
    int32_t tmp[kChannels][512];
    int32_t* tmpp[kChannels] = {tmp[0], tmp[1]};
    while (rx_running_) {
        std::string from; uint16_t port; int64_t rx_mono;
        int n = rx_sock_.recv(buf.data(), (int)buf.size(), &from, &port, &rx_mono);
        if (n <= 0) { if (n < 0) sleep_ms(10); continue; }
        if (!rx_desc_.source.empty() && from != rx_desc_.source) continue;
        RtpHeader h; const uint8_t* pl; size_t pll;
        if (!rtp_parse(buf.data(), (size_t)n, h, &pl, &pll) || h.payload_type != rx_desc_.payload_type || frame_bytes == 0 || pll % frame_bytes != 0) { rx_bad_++; continue; }
        int frames = (int)(pll / frame_bytes);
        if (frames <= 0 || frames > 512) { rx_bad_++; continue; }
        if (rx_ssrc_.load() != h.ssrc) { rx_ssrc_ = h.ssrc; std::lock_guard<std::mutex> g(off_mu_); off_valid_ = false; }
        pcm_unpack(enc, pl, frames, sch, rx_ch_off_, tmpp, kChannels);
        for (int i = 0; i < frames; i++) {
            uint32_t ts = h.timestamp + (uint32_t)i;
            RxSlot& s = ring_[ts & kRingMask];
            for (int c = 0; c < kChannels; c++) s.s[c] = tmp[c][i];
            stamps_[ts & kRingMask].store(ts, std::memory_order_release);
        }
        rx_packets_++;
        if (!rx_first_mono_.load()) rx_first_mono_ = rx_mono;
        rx_last_mono_ = rx_mono;
        rx_note_arrival(h.timestamp, rx_mono, frames);
    }
}

// ---------------------------------------------------------------- engine thread
void Engine::engine_thread() {
    make_thread_realtime("aes67-engine");
    const int N = buffer_frames_;
    const bool tx_on = cfg_.tx_enabled;   // a change of this flag triggers a restart, so it is fixed for this run
    // The thread ticks once per transmitted packet (or once per period without TX). Host periods of
    // any size ride on top: a period fires on the first tick at or after its nominal time.
    const int tick_frames = tx_on ? std::min(tx_fpp_, N) : N;
    std::vector<int32_t> inbuf[kChannels], outbuf[kChannels];
    for (int c = 0; c < kChannels; c++) { inbuf[c].assign(N, 0); outbuf[c].assign(N, 0); }
    const int32_t* inp[kChannels] = {inbuf[0].data(), inbuf[1].data()};
    int32_t* outp[kChannels] = {outbuf[0].data(), outbuf[1].data()};
    std::vector<uint8_t> pkt(12 + 512 * kChannels * 3);
    int32_t txtmp[kChannels][512];
    const int32_t* txp[kChannels] = {txtmp[0], txtmp[1]};

    const int64_t tick_ns = (int64_t)tick_frames * 1000000000LL / rate_;
    const int64_t spin = (int64_t)cfg_.spin_us * 1000;
    int64_t base_ptp = 0; int64_t k = 0, period_index = 0;
    uint32_t base_samples = 0, tx_ts = 0, rx_read = 0; bool rx_read_valid = false; uint32_t seen_epoch = 0;
    int tx_acc = 0, frames_until_period = 0;
    int64_t slip_acc = 0; int slip_n = 0;
    auto anchor = [&]() {
        base_ptp = ptp_.now_ns() + 2 * tick_ns;
        base_ptp -= base_ptp % tick_ns;        // align to a whole tick of PTP time
        k = 0; period_index = 0; tx_acc = 0; frames_until_period = 0;
        base_samples = samples_at(base_ptp);
        tx_ts = base_samples;
        rx_read_valid = false;
        std::lock_guard<std::mutex> g(tx_mu_);
        // Pre-roll two packets of silence so the packet clock never runs ahead of the host blocks.
        tx_fifo_r_ = 0; tx_fifo_w_ = (size_t)(2 * tx_fpp_);
        for (int c = 0; c < kChannels; c++) std::fill(tx_fifo_[c].begin(), tx_fifo_[c].end(), 0);
    };
    anchor();
    LOG_I("engine thread: tick %d frames (%lld ns), period %d frames", tick_frames, (long long)tick_ns, N);

    while (running_) {
        int64_t frames_k = k * tick_frames;
        int64_t T = base_ptp + (frames_k / rate_) * 1000000000LL + ((frames_k % rate_) * 1000000000LL) / rate_;
        int64_t deadline = ptp_.ptp_to_mono(T);
        int64_t now = mono_ns();
        if (deadline - now > 100000000LL + 2 * tick_ns || now - deadline > 100000000LL) {
            LOG_W("engine clock jumped by %lld ms; re-anchoring", (long long)((deadline - now) / 1000000));
            anchor(); continue;
        }
        sleep_until_ns(deadline, spin);
        now = mono_ns();
        bool period_boundary = frames_until_period <= 0;
        if (period_boundary) frames_until_period += N;
        frames_until_period -= tick_frames;
        if (period_boundary && now - deadline > tick_ns) late_periods_++;

        // ---- transmit (from the FIFO filled by previous periods); sent before the host runs so
        //      packet departure times do not jitter with the host's processing time
        if (tx_on) {
            tx_acc += tick_frames;
            std::lock_guard<std::mutex> g(tx_mu_);
            while (tx_acc >= tx_fpp_) {
                tx_acc -= tx_fpp_;
                if (tx_enabled_) {
                    size_t avail = tx_fifo_w_ - tx_fifo_r_;
                    if (avail >= (size_t)tx_fpp_) {
                        for (int c = 0; c < kChannels; c++)
                            for (int i = 0; i < tx_fpp_; i++) txtmp[c][i] = tx_fifo_[c][(tx_fifo_r_ + i) % tx_fifo_cap_];
                        tx_fifo_r_ += tx_fpp_;
                    } else {
                        for (int c = 0; c < kChannels; c++) memset(txtmp[c], 0, sizeof(int32_t) * tx_fpp_);
                        if (periods_ > 0) tx_underruns_++;
                    }
                    RtpHeader h; h.payload_type = (uint8_t)tx_desc_.payload_type; h.sequence = tx_seq_++; h.timestamp = tx_ts; h.ssrc = tx_ssrc_;
                    rtp_write_header(pkt.data(), h);
                    pcm_pack(tx_desc_.encoding, txp, tx_fpp_, kChannels, pkt.data() + 12);
                    tx_sock_.send_to(pkt.data(), 12 + tx_fpp_ * kChannels * encoding_bytes(tx_desc_.encoding), tx_desc_.dest, tx_desc_.port);
                    tx_packets_++;
                }
                tx_ts += (uint32_t)tx_fpp_;
            }
        }

        // ---- period: deliver input, run host, queue output
        if (period_boundary) {
            // Playout pointer for this period's nominal time: the block ends 'link offset' before the
            // newest data (local clock plus the measured arrival offset), so it starts N frames earlier.
            uint32_t nominal = base_samples + (uint32_t)(period_index * N);
            int32_t off = off_est_pub_.load();
            uint32_t ideal = nominal + (uint32_t)off - (uint32_t)link_offset_frames_.load() - (uint32_t)N;
            uint32_t ep = off_epoch_.load();
            if (!rx_read_valid || ep != seen_epoch) { rx_read = ideal; rx_read_valid = true; seen_epoch = ep; slip_acc = 0; slip_n = 0; }
            else {
                int32_t diff = ts_diff(ideal, rx_read);
                int32_t big = std::max(link_offset_frames_.load() / 2, tx_fpp_ * 2);
                if (std::abs(diff) > big) { rx_read = ideal; rx_resyncs_++; slip_acc = 0; slip_n = 0; }
                else {
                    // Small differences are clock-phase noise; average them over ~1 s and only then slip,
                    // so a wobbling PTP phase does not click the audio.
                    slip_acc += diff; slip_n++;
                    if (slip_n * N >= rate_) {
                        int32_t mean = (int32_t)(slip_acc / slip_n);
                        if (std::abs(mean) >= 4) { rx_read += (uint32_t)mean; rx_slips_++; }
                        slip_acc = 0; slip_n = 0;
                    }
                }
            }
            bool rx_on = rx_running_.load();
            uint32_t lost = 0;
            for (int i = 0; i < N; i++) {
                uint32_t ts = rx_read + (uint32_t)i;
                if (rx_on && stamps_[ts & kRingMask].load(std::memory_order_acquire) == ts) {
                    const RxSlot& s = ring_[ts & kRingMask];
                    for (int c = 0; c < kChannels; c++) inbuf[c][i] = s.s[c];
                } else {
                    for (int c = 0; c < kChannels; c++) inbuf[c][i] = 0;
                    if (rx_on) lost++;
                }
            }
            rx_read += (uint32_t)N;
            // Count losses only while the stream is established (data flowing for > 1 s, newest packet < 1 s old).
            if (lost) {
                int64_t first = rx_first_mono_.load(), last = rx_last_mono_.load();
                if (first && now - first > 1000000000LL && now - last < 1000000000LL) rx_lost_frames_ += lost;
            }

            if (cb_) cb_->process(inp, outp, N, T, now, sample_pos_.load());
            else for (int c = 0; c < kChannels; c++) memset(outbuf[c].data(), 0, sizeof(int32_t) * N);
            sample_pos_ += (uint64_t)N;
            periods_++;
            period_index++;

            if (tx_on) {
                std::lock_guard<std::mutex> g(tx_mu_);
                if (tx_fifo_w_ - tx_fifo_r_ + (size_t)N > tx_fifo_cap_) tx_fifo_r_ = tx_fifo_w_;  // overflow: drop backlog
                for (int c = 0; c < kChannels; c++)
                    for (int i = 0; i < N; i++) tx_fifo_[c][(tx_fifo_w_ + i) % tx_fifo_cap_] = outbuf[c][i];
                tx_fifo_w_ += (size_t)N;
            }
        }
        k++;
    }
}

// ---------------------------------------------------------------- housekeeping
void Engine::housekeeping_thread() {
    int n = 0;
    while (running_) {
        for (int i = 0; i < 5 && running_; i++) sleep_ms(100);
        publish_status();
        if (++n % 2) continue;  // the rest once per second
        // SAP announcement follows the TX stream description (refclk may change with the PTP master).
        if (cfg_.tx_enabled && cfg_.sap_announce) sap_.set_announcement(tx_sdp());
        else sap_.set_announcement("");
        // Multicast stream vanished after having been active (typical IGMP snooping timeout without a
        // querier): leave and re-join the group so the switch forwards it again.
        {
            int64_t last = rx_last_mono_.load();
            if (rx_running_ && last && mono_ns() - last > 3000000000LL && mono_ns() - last_rejoin_mono_ > 10000000000LL) {
                last_rejoin_mono_ = mono_ns();
                std::lock_guard<std::mutex> g(rx_mu_);
                if (rx_running_ && rx_desc_.is_multicast()) {
                    LOG_W("RX: no packets for 3 s; re-joining %s", rx_desc_.dest.c_str());
                    rx_sock_.leave_multicast(rx_desc_.dest, iface_.ipv4, rx_desc_.source);
                    if (!rx_sock_.join_multicast(rx_desc_.dest, iface_.ipv4, rx_desc_.source) && !rx_desc_.source.empty())
                        rx_sock_.join_multicast(rx_desc_.dest, iface_.ipv4);
                }
            }
        }
        // Config file watch.
        if (watch_config_) {
            int64_t m = file_mtime(Config::default_path());
            if (m != cfg_mtime_) {
                cfg_mtime_ = m;
                Config c;
                if (c.load()) {
                    LOG_I("config changed on disk; applying");
                    if (!apply_config(c) && cb_) cb_->reset_request();
                }
            }
        }
    }
}

bool Engine::apply_config(const Config& c) {
    if (c.sample_rate != cfg_.sample_rate || c.interface_ip != cfg_.interface_ip || c.buffer_frames != cfg_.buffer_frames ||
        c.tx_ptime_ms != cfg_.tx_ptime_ms || c.spin_us != cfg_.spin_us) {
        cfg_ = c;
        return false;  // needs restart
    }
    bool rx_changed = c.rx_enabled != cfg_.rx_enabled || c.rx.dest != cfg_.rx.dest || c.rx.port != cfg_.rx.port || c.rx.source != cfg_.rx.source ||
        c.rx.payload_type != cfg_.rx.payload_type || c.rx.encoding != cfg_.rx.encoding || c.rx.channels != cfg_.rx.channels ||
        c.rx_channel_offset != cfg_.rx_channel_offset || c.rx.sample_rate != cfg_.rx.sample_rate;
    bool tx_changed = c.tx_enabled != cfg_.tx_enabled || c.tx_dest != cfg_.tx_dest || c.tx_port != cfg_.tx_port || c.tx_payload_type != cfg_.tx_payload_type ||
        c.tx_encoding != cfg_.tx_encoding || c.tx_ttl != cfg_.tx_ttl || c.tx_dscp != cfg_.tx_dscp || c.tx_name != cfg_.tx_name;
    bool ptp_changed = c.ptp_mode != cfg_.ptp_mode || c.ptp_domain != cfg_.ptp_domain || c.ptp_priority1 != cfg_.ptp_priority1 ||
        c.ptp_sync_log_interval != cfg_.ptp_sync_log_interval || c.ptp_announce_log_interval != cfg_.ptp_announce_log_interval;
    bool tx_enable_flip = c.tx_enabled != cfg_.tx_enabled;
    cfg_ = c;
    log_set_level((LogLevel)std::clamp(c.log_level, 0, 3));
    link_offset_frames_ = (int)std::lround(c.link_offset_ms * rate_ / 1000.0);
    std::string err;
    if (ptp_changed) { ptp_.stop(); if (!start_ptp(err)) LOG_W("PTP: %s", err.c_str()); }
    if (tx_enable_flip) return false;   // tick structure depends on it
    if (tx_changed) { if (!open_tx(err)) LOG_W("TX: %s", err.c_str()); }
    if (rx_changed) {
        if (c.rx_enabled) { if (!start_rx(err)) LOG_W("RX: %s", err.c_str()); }
        else stop_rx();
    }
    { std::lock_guard<std::mutex> g(status_mu_); message_ = err; }
    return true;
}

// ---------------------------------------------------------------- status
StatusBlock Engine::snapshot() const {
    StatusBlock s;
    s.updated_ms = wall_ms();
    s.engine_running = running_ ? 1 : 0;
    s.sample_rate = (uint32_t)rate_;
    s.buffer_frames = (uint32_t)buffer_frames_;
    s.ptp_state = (uint32_t)ptp_.state();
    s.ptp_offset_ns = ptp_.offset_ns();
    s.ptp_path_delay_ns = ptp_.path_delay_ns();
    s.ptp_jitter_ns = ptp_.jitter_ns();
    snprintf(s.ptp_master, sizeof s.ptp_master, "%s", ptp_.master_identity().c_str());
    int64_t last = rx_last_mono_.load();
    s.rx_active = (last && mono_ns() - last < 1000000000LL) ? 1 : 0;
    s.rx_packets = rx_packets_.load();
    s.rx_lost = rx_lost_frames_.load() / (uint32_t)std::max(1, cfg_.rx.frames_per_packet());
    s.rx_resyncs = rx_resyncs_.load();
    s.rx_bad = rx_bad_.load();
    s.rx_offset_frames = off_est_pub_.load();
    s.tx_packets = tx_packets_.load();
    s.tx_underruns = tx_underruns_.load();
    s.periods = periods_.load();
    s.late_periods = late_periods_.load();
    snprintf(s.local_ip, sizeof s.local_ip, "%s", iface_.ipv4.c_str());
    { std::lock_guard<std::mutex> g(const_cast<std::mutex&>(status_mu_)); snprintf(s.message, sizeof s.message, "%s", message_.c_str()); }
    return s;
}
void Engine::publish_status() {
    StatusBlock* p = share_.ptr();
    if (!p) return;
    StatusBlock s = snapshot();
    s.pid = p->pid;
    memcpy(p, &s, sizeof s);
}

} // namespace aes67
