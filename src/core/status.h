// Runtime status shared between the driver (inside the audio host) and the control panel.
#pragma once
#include <cstdint>
#include <string>

namespace aes67 {

#pragma pack(push, 1)
struct StatusBlock {
    uint32_t magic = 0x41453637;   // 'AE67'
    uint32_t version = 1;
    uint32_t pid = 0;
    int64_t  updated_ms = 0;        // wall clock ms when written
    uint32_t engine_running = 0;
    uint32_t sample_rate = 0;
    uint32_t buffer_frames = 0;
    uint32_t ptp_state = 0;         // 0 off, 1 listening, 2 locked (slave), 3 master, 4 free-running
    int64_t  ptp_offset_ns = 0;     // last measured offset from master
    int64_t  ptp_path_delay_ns = 0;
    char     ptp_master[32] = {0};
    uint32_t rx_active = 0;
    uint32_t rx_packets = 0;
    uint32_t rx_lost = 0;           // missing frames / fpp
    uint32_t rx_resyncs = 0;
    uint32_t rx_bad = 0;            // malformed / wrong PT
    int32_t  rx_offset_frames = 0;  // measured arrival offset relative to local clock
    uint32_t tx_packets = 0;
    uint32_t tx_underruns = 0;
    uint32_t periods = 0;
    uint32_t late_periods = 0;      // periods delivered > 1 ptime late
    char     local_ip[16] = {0};
    char     message[96] = {0};     // last error / info
};
#pragma pack(pop)

// Shared memory publisher (driver side) and reader (panel side).
class StatusShare {
public:
    StatusShare();
    ~StatusShare();
    static const char* kDriverName;   // "AES67VSC_Status_v1"  (ASIO driver inside the audio host)
    static const char* kBridgeName;   // "AES67VSC_Bridge_v1"  (bridge engine)
    bool create(const char* name = nullptr);   // publisher (default: driver block)
    bool open(const char* name = nullptr);     // reader
    StatusBlock* ptr() { return blk_; }
    bool read(StatusBlock& out);   // copy; false if not available
    void close();
private:
    StatusBlock* blk_ = nullptr;
    void* handle_ = nullptr;
    int fd_ = -1;
    bool owner_ = false;
    std::string name_;
};

int64_t wall_ms();
const char* ptp_state_name(uint32_t s);

} // namespace aes67
