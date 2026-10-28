// Windows audio (WASAPI) bridge: runs the AES67 engine and connects it to playback /
// recording devices. Used by aes67vsc-bridge.exe and hosted inside the tray application.
#pragma once
#ifdef _WIN32
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <memory>
#include "../core/config.h"
#include "../core/engine.h"
#include "../core/status.h"

struct IMMDevice;

namespace aes67 {

struct BridgeDevice { std::string id, name; bool render = false; };
// All active playback (render=true) and recording devices. Call CoInitialize first.
std::vector<BridgeDevice> bridge_list_devices();

struct BridgeStats {
    bool running = false;
    std::string play_name, source_name; bool loopback = false;
    uint32_t play_under = 0, play_drop = 0, cap_under = 0, cap_drop = 0;
    size_t play_fifo = 0, cap_fifo = 0;
    std::string error;
};

class BridgeImpl;
class BridgeRunner {
public:
    BridgeRunner();
    ~BridgeRunner();
    // play / source: "default", "none", or a case-insensitive substring of a device name.
    // loopback: source is a playback device captured in loopback mode.
    bool start(const Config& cfg, const std::string& play, const std::string& source, bool loopback, std::string& err);
    void stop();
    bool running() const;
    bool restart_needed();          // engine asked for a restart (configuration changed); call stop()+start()
    BridgeStats stats() const;
    StatusBlock engine_status() const;
private:
    std::unique_ptr<BridgeImpl> impl_;
};

} // namespace aes67
#endif
