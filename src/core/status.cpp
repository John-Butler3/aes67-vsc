#include "status.h"
#include "platform.h"
#include <cstring>
#include <chrono>
#ifndef _WIN32
#  include <sys/mman.h>
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace aes67 {

const char* StatusShare::kDriverName = "AES67VSC_Status_v1";
const char* StatusShare::kBridgeName = "AES67VSC_Bridge_v1";

int64_t wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
const char* ptp_state_name(uint32_t s) {
    switch (s) { case 0: return "off"; case 1: return "listening"; case 2: return "locked"; case 3: return "master"; case 4: return "free-running"; default: return "?"; }
}

StatusShare::StatusShare() {}
StatusShare::~StatusShare() { close(); }

#ifdef _WIN32
bool StatusShare::create(const char* name) {
    close();
    name_ = name ? name : kDriverName;
    std::string n = std::string("Local\\") + name_;
    handle_ = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(StatusBlock), n.c_str());
    if (!handle_) return false;
    blk_ = (StatusBlock*)MapViewOfFile(handle_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(StatusBlock));
    if (!blk_) { CloseHandle(handle_); handle_ = nullptr; return false; }
    *blk_ = StatusBlock();
    blk_->pid = GetCurrentProcessId();
    owner_ = true;
    return true;
}
bool StatusShare::open(const char* name) {
    close();
    name_ = name ? name : kDriverName;
    std::string n = std::string("Local\\") + name_;
    handle_ = OpenFileMappingA(FILE_MAP_READ, FALSE, n.c_str());
    if (!handle_) return false;
    blk_ = (StatusBlock*)MapViewOfFile(handle_, FILE_MAP_READ, 0, 0, sizeof(StatusBlock));
    if (!blk_) { CloseHandle(handle_); handle_ = nullptr; return false; }
    return true;
}
void StatusShare::close() {
    if (blk_) { UnmapViewOfFile(blk_); blk_ = nullptr; }
    if (handle_) { CloseHandle(handle_); handle_ = nullptr; }
    owner_ = false;
}
#else
bool StatusShare::create(const char* name) {
    close();
    name_ = name ? name : kDriverName;
    std::string n = std::string("/") + name_;
    fd_ = shm_open(n.c_str(), O_CREAT | O_RDWR, 0644);
    if (fd_ < 0) return false;
    if (ftruncate(fd_, sizeof(StatusBlock)) != 0) { ::close(fd_); fd_ = -1; return false; }
    blk_ = (StatusBlock*)mmap(nullptr, sizeof(StatusBlock), PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (blk_ == MAP_FAILED) { blk_ = nullptr; ::close(fd_); fd_ = -1; return false; }
    *blk_ = StatusBlock();
    blk_->pid = (uint32_t)getpid();
    owner_ = true;
    return true;
}
bool StatusShare::open(const char* name) {
    close();
    name_ = name ? name : kDriverName;
    std::string n = std::string("/") + name_;
    fd_ = shm_open(n.c_str(), O_RDONLY, 0);
    if (fd_ < 0) return false;
    blk_ = (StatusBlock*)mmap(nullptr, sizeof(StatusBlock), PROT_READ, MAP_SHARED, fd_, 0);
    if (blk_ == MAP_FAILED) { blk_ = nullptr; ::close(fd_); fd_ = -1; return false; }
    return true;
}
void StatusShare::close() {
    if (blk_) { munmap(blk_, sizeof(StatusBlock)); blk_ = nullptr; }
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    owner_ = false;
}
#endif

bool StatusShare::read(StatusBlock& out) {
    if (!blk_ && !open(name_.empty() ? nullptr : name_.c_str())) return false;
    memcpy(&out, blk_, sizeof out);
    if (out.magic != 0x41453637) return false;
    return true;
}

} // namespace aes67
