// Platform abstraction: sockets, clocks, threads.  C++17, no external deps.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
typedef SOCKET socket_t;
#  define INVALID_SOCKET_T INVALID_SOCKET
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
typedef int socket_t;
#  define INVALID_SOCKET_T (-1)
#endif

namespace aes67 {

// ---- time -------------------------------------------------------------
// Monotonic clock in nanoseconds (QPC on Windows, CLOCK_MONOTONIC elsewhere).
int64_t mono_ns();
// Wall clock as TAI nanoseconds since the PTP epoch (UTC + 37 s). Used when no PTP master exists.
int64_t wall_tai_ns();
// High resolution sleep until a monotonic deadline. Sleeps coarsely, then spins the last bit.
void sleep_until_ns(int64_t deadline_mono_ns, int64_t spin_ns = 150000);
void sleep_ms(int ms);
// Mark the calling thread as time critical (pro audio class on Windows).
void make_thread_realtime(const char* name);

// ---- sockets -----------------------------------------------------------
void net_init();   // WSAStartup on Windows, no-op elsewhere. Safe to call repeatedly.
int  net_last_error();
std::string net_error_string(int err);

struct NetInterface {
    std::string name;     // friendly name
    std::string ipv4;     // dotted quad
    uint8_t     mac[6] = {0};
    bool        has_mac = false;
};
std::vector<NetInterface> list_interfaces();
// Resolve the interface config string ("", "auto", "192.168.1.10" or an interface name) to an IPv4 string.
NetInterface pick_interface(const std::string& wanted);

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket() { close(); }
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    bool open();
    void close();
    bool valid() const { return s_ != INVALID_SOCKET_T; }
    socket_t handle() const { return s_; }

    bool set_reuse_addr(bool on);
    bool bind_any(uint16_t port);
    bool bind_addr(const std::string& ip, uint16_t port);
    bool join_multicast(const std::string& group, const std::string& iface_ip, const std::string& source_ip = "");
    bool leave_multicast(const std::string& group, const std::string& iface_ip, const std::string& source_ip = "");
    bool set_multicast_if(const std::string& iface_ip);
    bool set_multicast_ttl(int ttl);
    bool set_multicast_loop(bool on);
    bool set_tos(int tos);                  // DSCP << 2
    bool set_recv_buffer(int bytes);
    bool set_send_buffer(int bytes);
    bool set_recv_timeout_ms(int ms);
    bool set_nonblocking(bool on);
    bool set_timestamping(bool on);         // kernel/stack receive timestamps where available (Linux, Windows 10 2004+)
    bool has_timestamping() const { return want_timestamps_; }

    // Returns bytes received, 0 on timeout, -1 on error. rx_mono_ns gets the receive timestamp.
    int recv(void* buf, int len, std::string* from_ip, uint16_t* from_port, int64_t* rx_mono_ns);
    int send_to(const void* buf, int len, const std::string& ip, uint16_t port);
    std::string local_ip() const;
    uint16_t local_port() const;
private:
    socket_t s_ = INVALID_SOCKET_T;
    bool want_timestamps_ = false;
};

// ---- misc --------------------------------------------------------------
std::string config_dir();            // per-user config directory (created on demand)
bool file_exists(const std::string& path);
int64_t file_mtime(const std::string& path);
bool read_file(const std::string& path, std::string& out);
bool write_file(const std::string& path, const std::string& data);
std::string hostname();
uint32_t random_u32();
uint64_t random_u64();

} // namespace aes67
