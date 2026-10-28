#include "platform.h"
#include <chrono>
#include <cstring>
#include <cstdio>
#include <random>
#include <mutex>
#include <thread>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#  include <iphlpapi.h>
#  include <shlobj.h>
#  include <avrt.h>
#  include <mmsystem.h>
#else
#  include <time.h>
#  include <ifaddrs.h>
#  include <net/if.h>
#  include <sys/ioctl.h>
#  include <sys/stat.h>
#  include <fcntl.h>
#  include <errno.h>
#  include <pwd.h>
#  ifdef __linux__
#    include <linux/net_tstamp.h>
#    include <linux/errqueue.h>
#  endif
#endif

namespace aes67 {

// ---------------------------------------------------------------- time
#ifdef _WIN32
static int64_t qpc_freq() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    return f.QuadPart;
}
int64_t mono_ns() {
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    const int64_t f = qpc_freq();
    // split to avoid overflow: (c / f) * 1e9 + (c % f) * 1e9 / f
    return (c.QuadPart / f) * 1000000000LL + ((c.QuadPart % f) * 1000000000LL) / f;
}
int64_t wall_tai_ns() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    // FILETIME: 100ns since 1601-01-01. Unix epoch offset = 11644473600 s.
    int64_t unix_ns = (int64_t)(u.QuadPart - 116444736000000000ULL) * 100;
    return unix_ns + 37LL * 1000000000LL;
}
static HANDLE g_timer = nullptr;
static std::once_flag g_timer_once;
static void init_timer() {
    timeBeginPeriod(1);
    typedef HANDLE (WINAPI *CreateWaitableTimerExW_t)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    auto fn = k ? (CreateWaitableTimerExW_t)GetProcAddress(k, "CreateWaitableTimerExW") : nullptr;
    if (fn) g_timer = fn(nullptr, nullptr, 0x00000002 /*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
    if (!g_timer) g_timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
}
void sleep_until_ns(int64_t deadline, int64_t spin_ns) {
    std::call_once(g_timer_once, init_timer);
    // One waitable timer handle per thread would be cleaner; we keep it simple with thread-local.
    thread_local HANDLE t = nullptr;
    if (!t) {
        typedef HANDLE (WINAPI *CreateWaitableTimerExW_t)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
        HMODULE k = GetModuleHandleW(L"kernel32.dll");
        auto fn = k ? (CreateWaitableTimerExW_t)GetProcAddress(k, "CreateWaitableTimerExW") : nullptr;
        if (fn) t = fn(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
        if (!t) t = CreateWaitableTimerW(nullptr, TRUE, nullptr);
    }
    for (;;) {
        int64_t now = mono_ns();
        int64_t remain = deadline - now;
        if (remain <= spin_ns) break;
        int64_t wait = remain - spin_ns;
        LARGE_INTEGER due; due.QuadPart = -(wait / 100); // relative, 100ns units
        if (t && SetWaitableTimer(t, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(t, (DWORD)(wait / 1000000 + 2));
        } else {
            Sleep((DWORD)(wait / 1000000));
        }
    }
    while (mono_ns() < deadline) { YieldProcessor(); }
}
void sleep_ms(int ms) { Sleep(ms); }
void make_thread_realtime(const char* name) {
    (void)name;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    DWORD idx = 0;
    HMODULE av = LoadLibraryW(L"avrt.dll");
    if (av) {
        typedef HANDLE (WINAPI *AvSet_t)(LPCWSTR, LPDWORD);
        auto fn = (AvSet_t)GetProcAddress(av, "AvSetMmThreadCharacteristicsW");
        if (fn) fn(L"Pro Audio", &idx);
    }
}
#else
int64_t mono_ns() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
int64_t wall_tai_ns() {
    timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec + 37LL * 1000000000LL;
}
void sleep_until_ns(int64_t deadline, int64_t spin_ns) {
    for (;;) {
        int64_t remain = deadline - mono_ns();
        if (remain <= spin_ns) break;
        int64_t w = remain - spin_ns;
        timespec ts; ts.tv_sec = w / 1000000000LL; ts.tv_nsec = w % 1000000000LL;
        nanosleep(&ts, nullptr);
    }
    while (mono_ns() < deadline) { /* spin */ }
}
void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
void make_thread_realtime(const char* name) {
#ifdef __linux__
    if (name) pthread_setname_np(pthread_self(), name);
    sched_param sp{}; sp.sched_priority = 70;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp); // best effort; may fail without privileges
#else
    (void)name;
#endif
}
#endif

// ---------------------------------------------------------------- sockets
void net_init() {
#ifdef _WIN32
    static std::once_flag once;
    std::call_once(once, [] { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); });
#endif
}
int net_last_error() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}
std::string net_error_string(int err) {
#ifdef _WIN32
    char buf[256] = {0};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, (DWORD)err, 0, buf, sizeof buf, nullptr);
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s + " (" + std::to_string(err) + ")";
#else
    return std::string(strerror(err)) + " (" + std::to_string(err) + ")";
#endif
}

std::vector<NetInterface> list_interfaces() {
    net_init();
    std::vector<NetInterface> out;
#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf(size);
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG r = GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
    if (r == ERROR_BUFFER_OVERFLOW) { buf.resize(size); r = GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size); }
    if (r != NO_ERROR) return out;
    for (auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            NetInterface ni;
            char name[256] = {0};
            WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, name, sizeof name, nullptr, nullptr);
            ni.name = name;
            char ip[64] = {0};
            inet_ntop(AF_INET, &((sockaddr_in*)u->Address.lpSockaddr)->sin_addr, ip, sizeof ip);
            ni.ipv4 = ip;
            if (a->PhysicalAddressLength == 6) { memcpy(ni.mac, a->PhysicalAddress, 6); ni.has_mac = true; }
            out.push_back(ni);
        }
    }
#else
    ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) != 0) return out;
    for (auto* p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (!(p->ifa_flags & IFF_UP) || (p->ifa_flags & IFF_LOOPBACK)) continue;
        NetInterface ni;
        ni.name = p->ifa_name;
        char ip[64] = {0};
        inet_ntop(AF_INET, &((sockaddr_in*)p->ifa_addr)->sin_addr, ip, sizeof ip);
        ni.ipv4 = ip;
        std::ifstream f(std::string("/sys/class/net/") + p->ifa_name + "/address");
        std::string mac;
        if (f && std::getline(f, mac) && mac.size() >= 17) {
            unsigned m[6];
            if (sscanf(mac.c_str(), "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
                for (int i = 0; i < 6; i++) ni.mac[i] = (uint8_t)m[i];
                ni.has_mac = true;
            }
        }
        out.push_back(ni);
    }
    freeifaddrs(ifa);
#endif
    return out;
}

NetInterface pick_interface(const std::string& wanted) {
    auto all = list_interfaces();
    if (!wanted.empty() && wanted != "auto") {
        for (auto& i : all) if (i.ipv4 == wanted || i.name == wanted) return i;
        // Not found: use the literal IP anyway so the user sees the real socket error.
        NetInterface ni; ni.ipv4 = wanted; ni.name = wanted; return ni;
    }
    // Prefer a private LAN address that is not link-local.
    for (auto& i : all) if (i.ipv4.rfind("169.254.", 0) != 0) return i;
    if (!all.empty()) return all[0];
    NetInterface ni; ni.ipv4 = "0.0.0.0"; ni.name = "none"; return ni;
}

static void sock_close(socket_t s) {
#ifdef _WIN32
    closesocket(s);
#else
    ::close(s);
#endif
}

bool UdpSocket::open() {
    net_init();
    close();
    s_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    return valid();
}
void UdpSocket::close() {
    if (valid()) { sock_close(s_); s_ = INVALID_SOCKET_T; }
}
bool UdpSocket::set_reuse_addr(bool on) {
    int v = on ? 1 : 0;
    // Note: deliberately no SO_REUSEPORT; on Linux it load-balances datagrams between sockets
    // instead of delivering multicast to all of them.
    return setsockopt(s_, SOL_SOCKET, SO_REUSEADDR, (const char*)&v, sizeof v) == 0;
}
bool UdpSocket::bind_any(uint16_t port) { return bind_addr("0.0.0.0", port); }
bool UdpSocket::bind_addr(const std::string& ip, uint16_t port) {
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &a.sin_addr);
    return ::bind(s_, (sockaddr*)&a, sizeof a) == 0;
}
bool UdpSocket::join_multicast(const std::string& group, const std::string& iface_ip, const std::string& source_ip) {
    if (!source_ip.empty()) {
        ip_mreq_source m{};
        inet_pton(AF_INET, group.c_str(), &m.imr_multiaddr);
        inet_pton(AF_INET, iface_ip.c_str(), &m.imr_interface);
        inet_pton(AF_INET, source_ip.c_str(), &m.imr_sourceaddr);
        return setsockopt(s_, IPPROTO_IP, IP_ADD_SOURCE_MEMBERSHIP, (const char*)&m, sizeof m) == 0;
    }
    ip_mreq m{};
    inet_pton(AF_INET, group.c_str(), &m.imr_multiaddr);
    inet_pton(AF_INET, iface_ip.c_str(), &m.imr_interface);
    return setsockopt(s_, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&m, sizeof m) == 0;
}
bool UdpSocket::leave_multicast(const std::string& group, const std::string& iface_ip, const std::string& source_ip) {
    if (!source_ip.empty()) {
        ip_mreq_source m{};
        inet_pton(AF_INET, group.c_str(), &m.imr_multiaddr);
        inet_pton(AF_INET, iface_ip.c_str(), &m.imr_interface);
        inet_pton(AF_INET, source_ip.c_str(), &m.imr_sourceaddr);
        return setsockopt(s_, IPPROTO_IP, IP_DROP_SOURCE_MEMBERSHIP, (const char*)&m, sizeof m) == 0;
    }
    ip_mreq m{};
    inet_pton(AF_INET, group.c_str(), &m.imr_multiaddr);
    inet_pton(AF_INET, iface_ip.c_str(), &m.imr_interface);
    return setsockopt(s_, IPPROTO_IP, IP_DROP_MEMBERSHIP, (const char*)&m, sizeof m) == 0;
}
bool UdpSocket::set_multicast_if(const std::string& iface_ip) {
    in_addr a{}; inet_pton(AF_INET, iface_ip.c_str(), &a);
    return setsockopt(s_, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&a, sizeof a) == 0;
}
bool UdpSocket::set_multicast_ttl(int ttl) {
#ifdef _WIN32
    DWORD v = (DWORD)ttl;
#else
    unsigned char v = (unsigned char)ttl;
#endif
    return setsockopt(s_, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&v, sizeof v) == 0;
}
bool UdpSocket::set_multicast_loop(bool on) {
#ifdef _WIN32
    DWORD v = on ? 1 : 0;
#else
    unsigned char v = on ? 1 : 0;
#endif
    return setsockopt(s_, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&v, sizeof v) == 0;
}
bool UdpSocket::set_tos(int tos) {
    int v = tos;
    return setsockopt(s_, IPPROTO_IP, IP_TOS, (const char*)&v, sizeof v) == 0;
}
bool UdpSocket::set_recv_buffer(int bytes) { return setsockopt(s_, SOL_SOCKET, SO_RCVBUF, (const char*)&bytes, sizeof bytes) == 0; }
bool UdpSocket::set_send_buffer(int bytes) { return setsockopt(s_, SOL_SOCKET, SO_SNDBUF, (const char*)&bytes, sizeof bytes) == 0; }
bool UdpSocket::set_recv_timeout_ms(int ms) {
#ifdef _WIN32
    DWORD v = (DWORD)ms;
    return setsockopt(s_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&v, sizeof v) == 0;
#else
    timeval tv; tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000;
    return setsockopt(s_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv) == 0;
#endif
}
bool UdpSocket::set_nonblocking(bool on) {
#ifdef _WIN32
    u_long v = on ? 1 : 0;
    return ioctlsocket(s_, FIONBIO, &v) == 0;
#else
    int fl = fcntl(s_, F_GETFL, 0);
    return fcntl(s_, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK)) == 0;
#endif
}
#ifdef _WIN32
// Windows 10 2004+ can stamp received datagrams in the network stack (far below user mode),
// which removes most of the scheduling noise from PTP measurements. Definitions are spelled
// out here so that older SDK headers (MinGW) build as well.
#ifndef SIO_TIMESTAMPING
#  define SIO_TIMESTAMPING 0x980000EB
#endif
#ifndef SO_TIMESTAMP
#  define SO_TIMESTAMP 0x300A
#endif
struct TimestampingConfigW { ULONG Flags; USHORT TxTimestampsBuffered; };
typedef int (WSAAPI *WSARecvMsg_t)(SOCKET, LPWSAMSG, LPDWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
static WSARecvMsg_t get_wsarecvmsg(SOCKET s) {
    static WSARecvMsg_t fn = nullptr;
    if (fn) return fn;
    GUID guid = {0xf689d7c8, 0x6f1f, 0x436b, {0x8a, 0x53, 0xe5, 0x4f, 0xe3, 0x51, 0xc3, 0x22}};
    DWORD bytes = 0; WSARecvMsg_t f = nullptr;
    if (WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof guid, &f, sizeof f, &bytes, nullptr, nullptr) == 0) fn = f;
    return fn;
}
#endif

bool UdpSocket::set_timestamping(bool on) {
    want_timestamps_ = false;
#if defined(__linux__)
    int v = on ? 1 : 0;
    bool ok = setsockopt(s_, SOL_SOCKET, SO_TIMESTAMPNS, &v, sizeof v) == 0;
    want_timestamps_ = on && ok;
    return ok;
#elif defined(_WIN32)
    if (!on) return true;
    TimestampingConfigW cfg{}; cfg.Flags = 0x1 /*TIMESTAMPING_FLAG_RX*/; cfg.TxTimestampsBuffered = 0;
    DWORD bytes = 0;
    if (WSAIoctl(s_, SIO_TIMESTAMPING, &cfg, sizeof cfg, nullptr, 0, &bytes, nullptr, nullptr) != 0) return false;
    if (!get_wsarecvmsg(s_)) return false;
    want_timestamps_ = true;
    return true;
#else
    return false;
#endif
}

int UdpSocket::recv(void* buf, int len, std::string* from_ip, uint16_t* from_port, int64_t* rx_mono_ns) {
    sockaddr_in from{}; socklen_t fl = sizeof from;
    int n;
#if defined(__linux__)
    if (want_timestamps_) {
        iovec iov{buf, (size_t)len};
        char ctrl[256];
        msghdr msg{}; msg.msg_name = &from; msg.msg_namelen = fl; msg.msg_iov = &iov; msg.msg_iovlen = 1;
        msg.msg_control = ctrl; msg.msg_controllen = sizeof ctrl;
        n = (int)recvmsg(s_, &msg, 0);
        int64_t now = mono_ns();
        if (n > 0 && rx_mono_ns) {
            *rx_mono_ns = now;
            for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
                if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPNS) {
                    timespec ts; memcpy(&ts, CMSG_DATA(c), sizeof ts);
                    // Convert realtime stamp to the monotonic domain using the current offset.
                    timespec rt, mo; clock_gettime(CLOCK_REALTIME, &rt); clock_gettime(CLOCK_MONOTONIC, &mo);
                    int64_t rt_ns = (int64_t)rt.tv_sec * 1000000000LL + rt.tv_nsec;
                    int64_t mo_ns = (int64_t)mo.tv_sec * 1000000000LL + mo.tv_nsec;
                    int64_t st = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
                    *rx_mono_ns = st - rt_ns + mo_ns;
                }
            }
        }
        fl = msg.msg_namelen;
    } else
#endif
#if defined(_WIN32)
    if (want_timestamps_) {
        WSARecvMsg_t rm = get_wsarecvmsg(s_);
        WSABUF wb; wb.buf = (char*)buf; wb.len = (ULONG)len;
        char ctrl[128];
        WSAMSG msg{}; msg.name = (sockaddr*)&from; msg.namelen = fl; msg.lpBuffers = &wb; msg.dwBufferCount = 1;
        msg.Control.buf = ctrl; msg.Control.len = sizeof ctrl;
        DWORD got = 0;
        int r = rm ? rm(s_, &msg, &got, nullptr, nullptr) : SOCKET_ERROR;
        n = r == 0 ? (int)got : -1;
        int64_t now = mono_ns();
        if (n > 0 && rx_mono_ns) {
            *rx_mono_ns = now;
            // Walk the control messages by hand (same layout as WSACMSGHDR; avoids SDK header differences).
            struct CmsgW { SIZE_T len; INT level; INT type; };
            auto align = [](size_t l) { return (l + sizeof(SIZE_T) - 1) & ~(sizeof(SIZE_T) - 1); };
            const char* cb = msg.Control.buf; size_t cl = msg.Control.len, off = 0;
            while (off + sizeof(CmsgW) <= cl) {
                const CmsgW* c = (const CmsgW*)(cb + off);
                if (c->len < sizeof(CmsgW) || off + c->len > cl) break;
                if (c->level == SOL_SOCKET && c->type == SO_TIMESTAMP && c->len >= align(sizeof(CmsgW)) + sizeof(UINT64)) {
                    UINT64 qpc; memcpy(&qpc, cb + off + align(sizeof(CmsgW)), sizeof qpc);   // QueryPerformanceCounter units
                    const int64_t f = qpc_freq();
                    *rx_mono_ns = ((int64_t)qpc / f) * 1000000000LL + (((int64_t)qpc % f) * 1000000000LL) / f;
                }
                off += align(c->len);
            }
        }
        fl = msg.namelen;
    } else
#endif
    {
        n = (int)::recvfrom(s_, (char*)buf, len, 0, (sockaddr*)&from, &fl);
        if (rx_mono_ns) *rx_mono_ns = mono_ns();
    }
    if (n < 0) {
        int e = net_last_error();
#ifdef _WIN32
        if (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK) return 0;
#else
        if (e == EAGAIN || e == EWOULDBLOCK || e == EINTR) return 0;
#endif
        return -1;
    }
    if (from_ip) { char ip[64] = {0}; inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip); *from_ip = ip; }
    if (from_port) *from_port = ntohs(from.sin_port);
    return n;
}
int UdpSocket::send_to(const void* buf, int len, const std::string& ip, uint16_t port) {
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &a.sin_addr);
    return (int)::sendto(s_, (const char*)buf, len, 0, (sockaddr*)&a, sizeof a);
}
std::string UdpSocket::local_ip() const {
    sockaddr_in a{}; socklen_t l = sizeof a;
    if (getsockname(s_, (sockaddr*)&a, &l) != 0) return "";
    char ip[64] = {0}; inet_ntop(AF_INET, &a.sin_addr, ip, sizeof ip); return ip;
}
uint16_t UdpSocket::local_port() const {
    sockaddr_in a{}; socklen_t l = sizeof a;
    if (getsockname(s_, (sockaddr*)&a, &l) != 0) return 0;
    return ntohs(a.sin_port);
}

// ---------------------------------------------------------------- misc
std::string config_dir() {
    static std::string dir = [] {
        std::string d;
#ifdef _WIN32
        char path[MAX_PATH] = {0};
        if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, path))) d = std::string(path) + "\\AES67VSC";
        else d = "C:\\AES67VSC";
        CreateDirectoryA(d.c_str(), nullptr);
#else
        const char* x = getenv("XDG_CONFIG_HOME");
        if (x && *x) d = std::string(x) + "/aes67vsc";
        else {
            const char* h = getenv("HOME");
            if (!h) { passwd* pw = getpwuid(getuid()); h = pw ? pw->pw_dir : "/tmp"; }
            d = std::string(h) + "/.config/aes67vsc";
        }
        mkdir(d.substr(0, d.rfind('/')).c_str(), 0755);
        mkdir(d.c_str(), 0755);
#endif
        return d;
    }();
    return dir;
}
bool file_exists(const std::string& path) {
    std::ifstream f(path, std::ios::binary); return f.good();
}
int64_t file_mtime(const std::string& path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &d)) return 0;
    ULARGE_INTEGER u; u.LowPart = d.ftLastWriteTime.dwLowDateTime; u.HighPart = d.ftLastWriteTime.dwHighDateTime;
    return (int64_t)u.QuadPart + (int64_t)d.nFileSizeLow;
#else
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0;
    return (int64_t)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec + st.st_size;
#endif
}
bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf(); out = ss.str();
    return true;
}
bool write_file(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << data;
        if (!f) return false;
    }
#ifdef _WIN32
    return MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return rename(tmp.c_str(), path.c_str()) == 0;
#endif
}
std::string hostname() {
    char buf[256] = {0};
    net_init();
    if (gethostname(buf, sizeof buf - 1) != 0) return "host";
    return buf;
}
static std::mt19937_64& rng() {
    static std::mt19937_64 g([] {
        std::random_device rd;
        return ((uint64_t)rd() << 32) ^ rd() ^ (uint64_t)mono_ns();
    }());
    return g;
}
uint32_t random_u32() { static std::mutex m; std::lock_guard<std::mutex> l(m); return (uint32_t)rng()(); }
uint64_t random_u64() { static std::mutex m; std::lock_guard<std::mutex> l(m); return rng()(); }

} // namespace aes67
