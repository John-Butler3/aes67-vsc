// Streaming engine: PTP-paced period clock, RTP receive ring, RTP transmit, status publishing.
#pragma once
#include <cstdint>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <memory>
#include "config.h"
#include "ptp.h"
#include "sap.h"
#include "status.h"
#include "platform.h"

namespace aes67 {

class AudioCallback {
public:
    virtual ~AudioCallback() {}
    // Called from the engine's real-time thread once per period.
    // in[ch][frame] holds received audio, out[ch][frame] must be filled by the host.
    virtual void process(const int32_t* const* in, int32_t* const* out, int frames, int64_t ptp_ns, int64_t mono_ns, uint64_t sample_pos) = 0;
    // Called (from a housekeeping thread) when the configuration changed in a way that needs a restart.
    virtual void reset_request() {}
};

class Engine {
public:
    static const int kChannels = 2;
    Engine();
    ~Engine();

    bool start(const Config& cfg, int buffer_frames, AudioCallback* cb, std::string& err);
    void stop();
    bool running() const { return running_; }

    // Hot-apply a new configuration (stream/clock changes). Returns false if a restart is required.
    bool apply_config(const Config& cfg);
    const Config& config() const { return cfg_; }
    std::string local_ip() const { return iface_.ipv4; }

    StatusBlock snapshot() const;
    std::string tx_sdp() const;            // SDP describing our transmit stream
    PtpClock& ptp() { return ptp_; }
    SapAgent& sap() { return sap_; }
    int frames_per_packet_tx() const { return tx_fpp_; }
    int link_offset_frames() const { return link_offset_frames_.load(); }

    // Enable watching the config file for changes (default on).
    void set_watch_config(bool on) { watch_config_ = on; }
    // Shared-memory status block name (StatusShare::kDriverName by default); set before start().
    void set_status_name(const char* name) { status_name_ = name; }

private:
    struct RxSlot { int32_t s[kChannels]; };
    static const int kRingBits = 16;
    static const uint32_t kRingSize = 1u << kRingBits;
    static const uint32_t kRingMask = kRingSize - 1;

    void engine_thread();
    void rx_thread();
    void housekeeping_thread();
    bool start_rx(std::string& err);
    void stop_rx();
    bool open_tx(std::string& err);
    void close_tx();
    bool start_ptp(std::string& err);
    uint32_t samples_at(int64_t ptp_ns) const;
    void publish_status();
    void rx_note_arrival(uint32_t pkt_ts, int64_t rx_mono, int frames);

    Config cfg_;
    NetInterface iface_;
    AudioCallback* cb_ = nullptr;
    int buffer_frames_ = 256;
    int rate_ = 48000;
    int tx_fpp_ = 48;
    std::atomic<int> link_offset_frames_{192};
    std::atomic<bool> running_{false};
    std::atomic<bool> watch_config_{true};
    std::thread eng_thread_, rx_thread_, hk_thread_;

    // PTP / SAP / status
    PtpClock ptp_;
    SapAgent sap_;
    StatusShare share_;
    mutable std::mutex status_mu_;
    std::string message_;

    // RX
    std::mutex rx_mu_;                     // guards rx thread lifecycle & rx params
    std::atomic<bool> rx_running_{false};
    UdpSocket rx_sock_;
    StreamDesc rx_desc_;
    int rx_ch_off_ = 0;
    std::vector<RxSlot> ring_;
    std::vector<std::atomic<uint32_t>> stamps_;    // full timestamp of the frame in each slot
    std::atomic<uint32_t> rx_packets_{0}, rx_lost_frames_{0}, rx_resyncs_{0}, rx_bad_{0}, rx_slips_{0};
    std::atomic<int64_t> rx_last_mono_{0}, rx_first_mono_{0};
    std::atomic<uint32_t> rx_ssrc_{0};
    // arrival offset tracking (packet ts - local sample clock), written by rx thread
    std::mutex off_mu_;
    bool off_valid_ = false;
    int32_t off_est_ = 0;
    int32_t off_win_max_ = 0; int64_t off_win_start_ = 0; bool off_win_valid_ = false;
    std::atomic<int32_t> off_est_pub_{0};
    std::atomic<uint32_t> off_epoch_{0};        // bumps when offset was re-anchored

    // TX
    std::mutex tx_mu_;
    UdpSocket tx_sock_;
    StreamDesc tx_desc_;
    bool tx_enabled_ = false;
    uint32_t tx_ssrc_ = 0;
    uint16_t tx_seq_ = 0;
    std::atomic<uint32_t> tx_packets_{0}, tx_underruns_{0};
    std::vector<int32_t> tx_fifo_[kChannels];
    size_t tx_fifo_r_ = 0, tx_fifo_w_ = 0, tx_fifo_cap_ = 0;

    // engine stats
    std::atomic<uint32_t> periods_{0}, late_periods_{0};
    std::atomic<uint64_t> sample_pos_{0};
    int64_t cfg_mtime_ = 0;
    const char* status_name_ = nullptr;
};

} // namespace aes67
