// AES67 Virtual Sound Card - ASIO driver (in-process COM server).
// 2 inputs / 2 outputs, 32-bit integer samples, clocked by PTP (or free-running).
#include "../core/platform.h"   // winsock2.h must precede windows.h
#include "iasio.h"
#include "../core/engine.h"
#include "../core/config.h"
#include "../core/log.h"
#include <shellapi.h>
#include <olectl.h>
#include <cstring>
#include <cstdio>
#include <string>
#include <mutex>
#include <atomic>
#include <vector>
#include <cmath>

using namespace aes67;

// {7C3A1F52-9B6E-4D1A-8E2F-5A6B7C8D9E01}
static const CLSID CLSID_Aes67Asio = {0x7c3a1f52, 0x9b6e, 0x4d1a, {0x8e, 0x2f, 0x5a, 0x6b, 0x7c, 0x8d, 0x9e, 0x01}};
static const char* kDriverName = "AES67 Virtual Sound Card";
static const char* kClsidString = "{7C3A1F52-9B6E-4D1A-8E2F-5A6B7C8D9E01}";
static HMODULE g_module = nullptr;
static std::atomic<long> g_objects{0};

static std::string module_dir() {
    char path[MAX_PATH] = {0};
    GetModuleFileNameA(g_module, path, MAX_PATH);
    std::string p(path);
    size_t s = p.find_last_of("\\/");
    return s == std::string::npos ? "." : p.substr(0, s);
}

class Aes67Asio : public IASIO, public AudioCallback {
public:
    Aes67Asio() {
        g_objects++;
        log_set_file(config_dir() + "\\driver.log");
        cfg_.load();
        LOG_I("driver instance created (host pid %lu)", (unsigned long)GetCurrentProcessId());
    }
    ~Aes67Asio() override {
        stop();
        disposeBuffers();
        g_objects--;
        LOG_I("driver instance destroyed");
    }

    // ---- IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, CLSID_Aes67Asio)) { *ppv = static_cast<IASIO*>(this); AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)++ref_; }
    ULONG STDMETHODCALLTYPE Release() override { long r = --ref_; if (r == 0) delete this; return (ULONG)r; }

    // ---- IASIO
    ASIOBool init(void* sysHandle) override {
        host_wnd_ = (HWND)sysHandle;
        cfg_.load();
        NetInterface ni = pick_interface(cfg_.interface_ip);
        if (ni.ipv4.empty() || ni.ipv4 == "0.0.0.0") { set_error("No usable IPv4 network interface found"); return ASIOFalse; }
        local_ip_ = ni.ipv4;
        set_error("");
        LOG_I("init: interface %s, rate %d, preferred buffer %d", local_ip_.c_str(), cfg_.sample_rate, cfg_.buffer_frames);
        return ASIOTrue;
    }
    void getDriverName(char* name) override { strcpy(name, kDriverName); }
    long getDriverVersion() override { return 1; }
    void getErrorMessage(char* s) override { strncpy(s, error_.c_str(), 123); s[123] = 0; }

    ASIOError start() override {
        std::lock_guard<std::mutex> g(mu_);
        if (!buffers_ready_) return ASE_NotPresent;
        if (running_) return ASE_OK;
        sample_pos_ = 0; sys_time_ns_ = 0; buffer_index_ = 0;
        std::string err;
        cfg_.load();
        if (!engine_.start(cfg_, (int)buffer_size_, this, err)) { set_error("Engine start failed: " + err); LOG_E("%s", error_.c_str()); return ASE_HWMalfunction; }
        running_ = true;
        return ASE_OK;
    }
    ASIOError stop() override {
        std::lock_guard<std::mutex> g(mu_);
        if (!running_) return ASE_OK;
        running_ = false;
        engine_.stop();
        return ASE_OK;
    }
    long nch() const { return cfg_.channels == 1 ? 1 : 2; }
    ASIOError getChannels(long* in, long* out) override { if (in) *in = nch(); if (out) *out = nch(); return ASE_OK; }
    ASIOError getLatencies(long* in, long* out) override {
        long fpp = frames_per_packet();
        long link = (long)std::lround(cfg_.link_offset_ms * cfg_.sample_rate / 1000.0);
        if (in) *in = buffer_size_ + link;
        if (out) *out = buffer_size_ + fpp;
        return ASE_OK;
    }
    ASIOError getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) override {
        // Any size from 16 frames up works; the engine ticks per network packet underneath.
        long pref = cfg_.buffer_frames;
        if (pref < 16) pref = 16;
        if (pref > 4096) pref = 4096;
        if (minSize) *minSize = 16;
        if (maxSize) *maxSize = 4096;
        if (preferredSize) *preferredSize = pref;
        if (granularity) *granularity = 1;
        return ASE_OK;
    }
    ASIOError canSampleRate(ASIOSampleRate r) override {
        return (r == 44100.0 || r == 48000.0 || r == 96000.0) ? ASE_OK : ASE_NoClock;
    }
    ASIOError getSampleRate(ASIOSampleRate* r) override { if (!r) return ASE_InvalidParameter; *r = (double)cfg_.sample_rate; return ASE_OK; }
    ASIOError setSampleRate(ASIOSampleRate r) override {
        if (canSampleRate(r) != ASE_OK) return ASE_NoClock;
        int nr = (int)r;
        if (nr == cfg_.sample_rate) return ASE_OK;
        if (running_) return ASE_InvalidMode;
        LOG_I("sample rate %d -> %d", cfg_.sample_rate, nr);
        cfg_.sample_rate = nr;
        cfg_.save();
        if (callbacks_ && callbacks_->sampleRateDidChange) callbacks_->sampleRateDidChange(r);
        return ASE_OK;
    }
    ASIOError getClockSources(ASIOClockSource* clocks, long* num) override {
        if (!clocks || !num || *num < 1) return ASE_InvalidParameter;
        clocks[0].index = 0; clocks[0].associatedChannel = -1; clocks[0].associatedGroup = -1; clocks[0].isCurrentSource = ASIOTrue;
        strcpy(clocks[0].name, "PTP / Network");
        *num = 1;
        return ASE_OK;
    }
    ASIOError setClockSource(long ref) override { return ref == 0 ? ASE_OK : ASE_InvalidParameter; }
    ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) override {
        uint64_t p = sample_pos_.load(); int64_t t = sys_time_ns_.load();
        if (sPos) { sPos->hi = (unsigned long)(p >> 32); sPos->lo = (unsigned long)(p & 0xFFFFFFFFu); }
        if (tStamp) { tStamp->hi = (unsigned long)((uint64_t)t >> 32); tStamp->lo = (unsigned long)((uint64_t)t & 0xFFFFFFFFu); }
        return ASE_OK;
    }
    ASIOError getChannelInfo(ASIOChannelInfo* info) override {
        if (!info || info->channel < 0 || info->channel >= nch()) return ASE_InvalidParameter;
        info->isActive = active_[info->isInput ? 0 : 1][info->channel] ? ASIOTrue : ASIOFalse;
        info->channelGroup = 0;
        info->type = ASIOSTInt32LSB;
        if (nch() == 1) snprintf(info->name, sizeof info->name, "AES67 %s (mono)", info->isInput ? "In" : "Out");
        else snprintf(info->name, sizeof info->name, "AES67 %s %ld", info->isInput ? "In" : "Out", info->channel + 1);
        return ASE_OK;
    }
    ASIOError createBuffers(ASIOBufferInfo* infos, long num, long size, ASIOCallbacks* cb) override {
        std::lock_guard<std::mutex> g(mu_);
        if (running_) return ASE_InvalidMode;
        long mn, mx, pref, gran; getBufferSize(&mn, &mx, &pref, &gran);
        (void)pref; (void)gran;
        if (!infos || !cb || num < 1 || num > 4 || size < mn || size > mx) {
            set_error("Invalid buffer size (" + std::to_string(mn) + " to " + std::to_string(mx) + " frames)");
            return ASE_InvalidMode;
        }
        release_buffers();
        for (long i = 0; i < num; i++) {
            ASIOBufferInfo& b = infos[i];
            if (b.channelNum < 0 || b.channelNum >= nch()) { release_buffers(); return ASE_InvalidParameter; }
            int dir = b.isInput ? 0 : 1;
            for (int h = 0; h < 2; h++) {
                int32_t* mem = (int32_t*)calloc((size_t)size, sizeof(int32_t));
                if (!mem) { release_buffers(); return ASE_NoMemory; }
                buf_[dir][b.channelNum][h] = mem;
                b.buffers[h] = mem;
            }
            active_[dir][b.channelNum] = true;
        }
        buffer_size_ = size;
        callbacks_ = cb;
        use_time_info_ = false;
        if (cb->asioMessage && cb->asioMessage(kAsioSelectorSupported, kAsioSupportsTimeInfo, nullptr, nullptr) == 1)
            use_time_info_ = cb->asioMessage(kAsioSupportsTimeInfo, 0, nullptr, nullptr) == 1;
        buffers_ready_ = true;
        cfg_.buffer_frames = (int)size;
        LOG_I("createBuffers: %ld channels, %ld frames, timeInfo=%d", num, size, use_time_info_ ? 1 : 0);
        return ASE_OK;
    }
    ASIOError disposeBuffers() override {
        std::lock_guard<std::mutex> g(mu_);
        if (running_) { running_ = false; engine_.stop(); }
        release_buffers();
        callbacks_ = nullptr;
        buffers_ready_ = false;
        return ASE_OK;
    }
    ASIOError controlPanel() override {
        std::string exe = module_dir() + "\\aes67vsc-panel.exe";
        HINSTANCE r = ShellExecuteA(host_wnd_, "open", exe.c_str(), nullptr, module_dir().c_str(), SW_SHOWNORMAL);
        if ((INT_PTR)r <= 32) {
            std::string msg = "Control panel not found:\n" + exe + "\n\nEdit the configuration file instead:\n" + Config::default_path();
            MessageBoxA(host_wnd_, msg.c_str(), kDriverName, MB_OK | MB_ICONINFORMATION);
        }
        return ASE_OK;
    }
    ASIOError future(long, void*) override { return ASE_NotPresent; }
    ASIOError outputReady() override { return ASE_NotPresent; }

    // ---- AudioCallback (engine real-time thread)
    void process(const int32_t* const* in, int32_t* const* out, int frames, int64_t ptp_ns, int64_t mono, uint64_t pos) override {
        (void)ptp_ns;
        if (!buffers_ready_ || !callbacks_) { for (int c = 0; c < 2; c++) memset(out[c], 0, sizeof(int32_t) * frames); return; }
        int idx = buffer_index_;
        const bool is_mono = nch() == 1;
        if (is_mono) { if (buf_[0][0][idx]) for (int i = 0; i < frames; i++) buf_[0][0][idx][i] = (in[0][i] >> 1) + (in[1][i] >> 1); }
        else for (int c = 0; c < 2; c++) if (buf_[0][c][idx]) memcpy(buf_[0][c][idx], in[c], sizeof(int32_t) * frames);
        sample_pos_ = pos + (uint64_t)frames;
        sys_time_ns_ = mono;
        if (use_time_info_ && callbacks_->bufferSwitchTimeInfo) {
            ASIOTime t; memset(&t, 0, sizeof t);
            t.timeInfo.speed = 1.0;
            t.timeInfo.sampleRate = (double)cfg_.sample_rate;
            uint64_t p = sample_pos_.load();
            t.timeInfo.samplePosition.hi = (unsigned long)(p >> 32); t.timeInfo.samplePosition.lo = (unsigned long)(p & 0xFFFFFFFFu);
            t.timeInfo.systemTime.hi = (unsigned long)((uint64_t)mono >> 32); t.timeInfo.systemTime.lo = (unsigned long)((uint64_t)mono & 0xFFFFFFFFu);
            t.timeInfo.flags = kSystemTimeValid | kSamplePositionValid | kSampleRateValid | kSpeedValid;
            callbacks_->bufferSwitchTimeInfo(&t, idx, ASIOTrue);
        } else if (callbacks_->bufferSwitch) {
            callbacks_->bufferSwitch(idx, ASIOTrue);
        }
        if (is_mono) {
            if (buf_[1][0][idx]) { memcpy(out[0], buf_[1][0][idx], sizeof(int32_t) * frames); memcpy(out[1], buf_[1][0][idx], sizeof(int32_t) * frames); }
            else { memset(out[0], 0, sizeof(int32_t) * frames); memset(out[1], 0, sizeof(int32_t) * frames); }
        } else for (int c = 0; c < 2; c++) {
            if (buf_[1][c][idx]) memcpy(out[c], buf_[1][c][idx], sizeof(int32_t) * frames);
            else memset(out[c], 0, sizeof(int32_t) * frames);
        }
        buffer_index_ = idx ^ 1;
    }
    void reset_request() override {
        LOG_I("configuration requires restart; asking host to reset");
        if (callbacks_ && callbacks_->asioMessage) callbacks_->asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
    }

private:
    long frames_per_packet() const {
        StreamDesc d = cfg_.tx_stream("0.0.0.0", "");
        return d.frames_per_packet();
    }
    void set_error(const std::string& e) { error_ = e; }
    void release_buffers() {
        for (int d = 0; d < 2; d++) for (int c = 0; c < 2; c++) { for (int h = 0; h < 2; h++) { free(buf_[d][c][h]); buf_[d][c][h] = nullptr; } active_[d][c] = false; }
    }

    std::atomic<long> ref_{1};
    std::mutex mu_;
    Config cfg_;
    Engine engine_;
    HWND host_wnd_ = nullptr;
    std::string error_, local_ip_;
    bool running_ = false, buffers_ready_ = false, use_time_info_ = false;
    long buffer_size_ = 240;
    int buffer_index_ = 0;
    int32_t* buf_[2][2][2] = {};   // [in/out][channel][half]
    bool active_[2][2] = {};
    ASIOCallbacks* callbacks_ = nullptr;
    std::atomic<uint64_t> sample_pos_{0};
    std::atomic<int64_t> sys_time_ns_{0};
};

// ---------------------------------------------------------------- class factory
class Factory : public IClassFactory {
public:
    virtual ~Factory() {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)++ref_; }
    ULONG STDMETHODCALLTYPE Release() override { long r = --ref_; if (r == 0) delete this; return (ULONG)r; }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        Aes67Asio* o = new (std::nothrow) Aes67Asio();
        if (!o) return E_OUTOFMEMORY;
        HRESULT hr = o->QueryInterface(riid, ppv);
        o->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override { if (lock) g_objects++; else g_objects--; return S_OK; }
private:
    std::atomic<long> ref_{1};
};

extern "C" {

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { g_module = inst; DisableThreadLibraryCalls(inst); }
    return TRUE;
}

HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    if (!IsEqualCLSID(rclsid, CLSID_Aes67Asio)) return CLASS_E_CLASSNOTAVAILABLE;
    Factory* f = new (std::nothrow) Factory();
    if (!f) return E_OUTOFMEMORY;
    HRESULT hr = f->QueryInterface(riid, ppv);
    f->Release();
    return hr;
}

HRESULT STDAPICALLTYPE DllCanUnloadNow() { return g_objects == 0 ? S_OK : S_FALSE; }

static LONG set_value(HKEY root, const char* path, const char* name, const char* value) {
    HKEY k; DWORD disp;
    LONG r = RegCreateKeyExA(root, path, 0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY, nullptr, &k, &disp);
    if (r != ERROR_SUCCESS) return r;
    r = RegSetValueExA(k, name, 0, REG_SZ, (const BYTE*)value, (DWORD)strlen(value) + 1);
    RegCloseKey(k);
    return r;
}

HRESULT STDAPICALLTYPE DllRegisterServer() {
    char path[MAX_PATH] = {0};
    GetModuleFileNameA(g_module, path, MAX_PATH);
    std::string clsid = std::string("CLSID\\") + kClsidString;
    std::string inproc = clsid + "\\InprocServer32";
    std::string asio = std::string("SOFTWARE\\ASIO\\") + kDriverName;
    if (set_value(HKEY_CLASSES_ROOT, clsid.c_str(), nullptr, kDriverName) != ERROR_SUCCESS) return SELFREG_E_CLASS;
    if (set_value(HKEY_CLASSES_ROOT, inproc.c_str(), nullptr, path) != ERROR_SUCCESS) return SELFREG_E_CLASS;
    if (set_value(HKEY_CLASSES_ROOT, inproc.c_str(), "ThreadingModel", "Apartment") != ERROR_SUCCESS) return SELFREG_E_CLASS;
    if (set_value(HKEY_LOCAL_MACHINE, asio.c_str(), "CLSID", kClsidString) != ERROR_SUCCESS) return SELFREG_E_CLASS;
    if (set_value(HKEY_LOCAL_MACHINE, asio.c_str(), "Description", kDriverName) != ERROR_SUCCESS) return SELFREG_E_CLASS;
    return S_OK;
}

HRESULT STDAPICALLTYPE DllUnregisterServer() {
    std::string clsid = std::string("CLSID\\") + kClsidString;
    std::string asio = std::string("SOFTWARE\\ASIO\\") + kDriverName;
    RegDeleteTreeA(HKEY_CLASSES_ROOT, clsid.c_str());
    RegDeleteTreeA(HKEY_LOCAL_MACHINE, asio.c_str());
    return S_OK;
}

} // extern "C"
