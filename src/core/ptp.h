// Minimal IEEE 1588-2008 (PTPv2) clock: end-to-end, multicast, two-step slave with a
// PI servo, plus a simple master so two PCs can lock to each other without a grandmaster.
#pragma once
#include <cstdint>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <map>
#include <array>
#include "config.h"
#include "platform.h"

namespace aes67 {

struct PtpParams {
    std::string iface_ip;
    int domain = 0;
    PtpMode mode = PtpMode::Auto;
    int priority1 = 200;             // deliberately worse than real devices (128) so they stay master
    int priority2 = 128;
    int sync_log_interval = -3;      // master
    int announce_log_interval = 1;   // master
    uint8_t mac[6] = {0};
    bool has_mac = false;
    uint16_t event_port = 319;       // general port = event_port + 1 (overridable for tests)
    int ttl = 1;
    double listen_seconds = 4.0;     // auto mode: how long to wait for a foreign master before becoming master
    bool stack_timestamps = false;   // Windows SIO_TIMESTAMPING (opt-in); Linux kernel timestamps are always used
};

enum class PtpState : uint32_t { Off = 0, Listening = 1, Locked = 2, Master = 3, FreeRunning = 4 };

class PtpClock {
public:
    PtpClock();
    ~PtpClock();
    bool start(const PtpParams& p, std::string& err);
    void stop();
    bool running() const { return running_; }

    // Time mapping. Always usable: when no master is available the clock free-runs on TAI wall time.
    int64_t now_ns() const { return mono_to_ptp(mono_ns()); }
    int64_t mono_to_ptp(int64_t mono) const;
    int64_t ptp_to_mono(int64_t ptp) const;

    PtpState state() const { return state_.load(); }
    int64_t offset_ns() const { return last_offset_ns_.load(); }
    int64_t path_delay_ns() const { return path_delay_ns_.load(); }
    int64_t jitter_ns() const { return jitter_ns_.load(); }       // smoothed |servo error|
    std::string master_identity() const;          // "00-1D-C1-FF-FE-12-34-56" of the GM we follow (or ours)
    std::string own_identity() const;
    std::string refclk_string() const;            // SDP a=ts-refclk value
    uint32_t sync_count() const { return sync_count_.load(); }
    uint32_t outlier_count() const { return outlier_count_.load(); }
    bool stack_timestamps() const { return ev_.has_timestamping(); }
    // What PTP traffic is on the wire (any version / domain), e.g. "PTPv2 domain 0: 3 sources; PTPv1 (Dante native): 1 source".
    std::string seen_summary() const;
    bool anything_seen() const;

    // Direct servo access for tests.
    void servo_reset();
    void servo_update(int64_t master_ns, int64_t local_mono_ns);

private:
    struct Servo {
        int64_t ref_mono = 0;     // anchor
        int64_t ref_ptp = 0;      // ptp time at anchor
        double  freq = 0.0;       // fractional frequency correction (ptp runs at (1+freq) * mono)
        bool    valid = false;
    };
    struct ForeignMaster {
        std::array<uint8_t, 10> port_id{};
        std::array<uint8_t, 8> gm_id{};
        uint8_t priority1 = 255, clock_class = 255, accuracy = 0xFE, priority2 = 255;
        uint16_t variance = 0xFFFF, steps_removed = 0;
        int8_t log_announce = 1;
        int64_t last_seen_mono = 0;
    };

    void thread_main();
    void handle_event(const uint8_t* b, int n, int64_t rx_mono, const std::string& from);
    void handle_general(const uint8_t* b, int n, int64_t rx_mono, const std::string& from);
    void handle_announce(const uint8_t* b, int n, int64_t rx_mono);
    void select_master(int64_t now_mono);
    bool foreign_better_than_us(const ForeignMaster& f) const;
    void become_master();
    void become_slave(const ForeignMaster& f);
    void master_tick(int64_t now_mono);
    void slave_tick(int64_t now_mono);
    void send_delay_req(int64_t now_mono);
    void sync_measurement(int64_t t1, int64_t t2_mono);
    void send_announce();
    void send_sync();
    void send_delay_resp(const uint8_t* req, int64_t rx_mono);
    void fill_header(uint8_t* b, uint8_t type, uint16_t len, uint16_t seq, uint8_t control, int8_t log_interval, bool two_step);
    void set_state(PtpState s);

    PtpParams p_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    UdpSocket ev_, gen_;
    std::array<uint8_t, 8> own_id_{};
    std::array<uint8_t, 10> own_port_id_{};

    mutable std::mutex mu_;
    Servo servo_;
    std::atomic<PtpState> state_{PtpState::Off};
    std::atomic<int64_t> last_offset_ns_{0};
    std::atomic<int64_t> path_delay_ns_{0};
    std::atomic<int64_t> jitter_ns_{0};
    double jitter_ema_ = 0;        // EMA of |e| in ns
    int outliers_in_row_ = 0;
    int64_t first_good_mono_ = 0;  // when the error first stayed small (for lock)
    int64_t last_good_mono_ = 0;
    std::atomic<uint32_t> sync_count_{0}, outlier_count_{0};
    // min-delay prefilter: of the last few measurements, the one with the largest error
    // (least delayed on its way to us) is the trustworthy one
    std::array<int64_t, 8> ewin_{}; int ewin_n_ = 0, ewin_i_ = 0, since_best_ = 0;
    std::map<std::array<uint8_t, 10>, int64_t> sync_sources_;   // port id -> last Sync seen (for masters that never Announce)
    struct Seen { uint32_t packets = 0; int64_t last_mono = 0; std::map<std::string, int> sources; };
    std::map<int, Seen> seen_;            // key = version * 256 + domain (domain 0 for v1)
    int64_t last_any_rx_mono_ = 0, last_rejoin_mono_ = 0, last_diag_log_mono_ = 0;
    void note_seen(const uint8_t* b, int n, int64_t rx_mono, const std::string& from);
    bool is_master_ = false;
    bool have_master_ = false;
    ForeignMaster cur_master_;
    std::map<std::array<uint8_t, 10>, ForeignMaster> foreign_;
    int64_t start_mono_ = 0;

    // slave state
    std::map<uint16_t, int64_t> pending_sync_;   // seq -> t2 (two-step)
    uint16_t dreq_seq_ = 0;
    int64_t dreq_sent_mono_ = 0;
    int64_t last_dreq_mono_ = 0;
    int64_t last_sync_mono_ = 0;
    int64_t last_servo_mono_ = 0;
    int64_t last_t1_ = 0, last_t2_ = 0;          // most recent sync pair
    std::array<int64_t, 8> delay_win_{}; int delay_win_n_ = 0, delay_win_i_ = 0;

    // master state
    uint16_t sync_seq_ = 0, ann_seq_ = 0;
    int64_t last_sync_tx_mono_ = 0, last_ann_tx_mono_ = 0;
};

// Helpers exposed for tests.
int64_t ptp_read_timestamp(const uint8_t* b);              // 10-byte PTP timestamp -> ns
void    ptp_write_timestamp(uint8_t* b, int64_t ns);
std::string ptp_identity_string(const uint8_t* id8);

} // namespace aes67
