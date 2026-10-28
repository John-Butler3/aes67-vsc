#include "../core/platform.h"   // winsock2 before windows.h
#ifdef _WIN32
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include "bridge_core.h"
#include "../core/log.h"
#include <cstring>
#include <cmath>
#include <algorithm>
#include <mutex>

namespace aes67 {

// GUIDs by value so that neither toolchain needs the import libraries that define them.
static const CLSID kClsidEnumerator  = {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
static const IID   kIidEnumerator    = {0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
static const IID   kIidAudioClient   = {0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}};
static const IID   kIidRenderClient  = {0xf294acfc, 0x3146, 0x4483, {0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2}};
static const IID   kIidCaptureClient = {0xc8adbd64, 0xe71e, 0x48a0, {0xa4, 0xde, 0x18, 0x5c, 0x39, 0x5c, 0xd3, 0x17}};
static const GUID  kSubtypeFloat     = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

static std::string narrow(const wchar_t* w) {
    if (!w) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, 0);
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}
static std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

// ---------------------------------------------------------------- SPSC FIFO of stereo int32 frames
class Fifo {
public:
    explicit Fifo(size_t cap) : cap_(cap), left_(cap), right_(cap) {}
    size_t size() const { return wr_.load() - rd_.load(); }
    bool push(int32_t l, int32_t r) {
        size_t w = wr_.load(std::memory_order_relaxed);
        if (w - rd_.load(std::memory_order_acquire) >= cap_) return false;
        left_[w % cap_] = l; right_[w % cap_] = r;
        wr_.store(w + 1, std::memory_order_release);
        return true;
    }
    bool pop(int32_t& l, int32_t& r) {
        size_t rd = rd_.load(std::memory_order_relaxed);
        if (wr_.load(std::memory_order_acquire) == rd) return false;
        l = left_[rd % cap_]; r = right_[rd % cap_];
        rd_.store(rd + 1, std::memory_order_release);
        return true;
    }
private:
    size_t cap_;
    std::vector<int32_t> left_, right_;
    std::atomic<size_t> wr_{0}, rd_{0};
};

// Linear-interpolation resampler pulling from a FIFO; 'step' = input frames per output frame.
struct Resampler {
    double pos = 1.0;
    int32_t prev[2] = {0, 0}, cur[2] = {0, 0};
    bool produce(Fifo& f, double step, int32_t& l, int32_t& r) {
        pos += step;
        while (pos >= 1.0) {
            prev[0] = cur[0]; prev[1] = cur[1];
            if (!f.pop(cur[0], cur[1])) { l = r = 0; return false; }
            pos -= 1.0;
        }
        l = (int32_t)(prev[0] + (cur[0] - prev[0]) * pos);
        r = (int32_t)(prev[1] + (cur[1] - prev[1]) * pos);
        return true;
    }
};

// ---------------------------------------------------------------- devices
static IMMDeviceEnumerator* enumerator() {
    IMMDeviceEnumerator* en = nullptr;
    CoCreateInstance(kClsidEnumerator, nullptr, CLSCTX_ALL, kIidEnumerator, (void**)&en);
    return en;
}
static std::vector<BridgeDevice> list_flow(IMMDeviceEnumerator* en, EDataFlow flow) {
    std::vector<BridgeDevice> out;
    IMMDeviceCollection* col = nullptr;
    if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) return out;
    UINT n = 0; col->GetCount(&n);
    for (UINT i = 0; i < n; i++) {
        IMMDevice* d = nullptr;
        if (FAILED(col->Item(i, &d))) continue;
        LPWSTR id = nullptr; d->GetId(&id);
        IPropertyStore* ps = nullptr; std::string name;
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT v; PropVariantInit(&v);
            if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) name = narrow(v.pwszVal);
            PropVariantClear(&v); ps->Release();
        }
        out.push_back({narrow(id), name, flow == eRender});
        CoTaskMemFree(id); d->Release();
    }
    col->Release();
    return out;
}
std::vector<BridgeDevice> bridge_list_devices() {
    std::vector<BridgeDevice> out;
    IMMDeviceEnumerator* en = enumerator();
    if (!en) return out;
    out = list_flow(en, eRender);
    auto c = list_flow(en, eCapture);
    out.insert(out.end(), c.begin(), c.end());
    en->Release();
    return out;
}
static IMMDevice* find_device(IMMDeviceEnumerator* en, EDataFlow flow, const std::string& want, std::string& name) {
    IMMDevice* d = nullptr;
    if (want.empty() || lower(want) == "default") {
        if (FAILED(en->GetDefaultAudioEndpoint(flow, eConsole, &d))) return nullptr;
        LPWSTR id = nullptr; d->GetId(&id); std::string sid = narrow(id); CoTaskMemFree(id);
        for (auto& x : list_flow(en, flow)) if (x.id == sid) name = x.name;
        return d;
    }
    for (auto& x : list_flow(en, flow)) {
        if (lower(x.name).find(lower(want)) != std::string::npos) {
            std::wstring wid(x.id.begin(), x.id.end());
            if (SUCCEEDED(en->GetDevice(wid.c_str(), &d))) { name = x.name; return d; }
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------- sample formats
struct Fmt { int rate = 48000; int ch = 2; int bits = 32; bool is_float = false; int block = 8; };
static Fmt parse_fmt(const WAVEFORMATEX* w) {
    Fmt f; f.rate = (int)w->nSamplesPerSec; f.ch = w->nChannels; f.bits = w->wBitsPerSample; f.block = w->nBlockAlign;
    if (w->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) f.is_float = true;
    else if (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* e = (const WAVEFORMATEXTENSIBLE*)w;
        f.is_float = IsEqualGUID(e->SubFormat, kSubtypeFloat) != 0;
        if (e->Samples.wValidBitsPerSample) f.bits = e->Samples.wValidBitsPerSample;
    }
    return f;
}
static inline int32_t read_sample(const uint8_t* p, const Fmt& f) {
    int cb = f.block / f.ch;
    if (f.is_float) { float v; memcpy(&v, p, 4); v = std::max(-1.0f, std::min(1.0f, v)); return (int32_t)(v * 2147483647.0); }
    if (cb == 2) { int16_t v; memcpy(&v, p, 2); return (int32_t)v << 16; }
    if (cb == 3) { return (int32_t)(((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 24)); }
    int32_t v; memcpy(&v, p, 4); return f.bits == 32 ? v : v << (32 - f.bits);
}
static inline void write_sample(uint8_t* p, const Fmt& f, int32_t s) {
    int cb = f.block / f.ch;
    if (f.is_float) { float v = (float)(s / 2147483648.0); memcpy(p, &v, 4); return; }
    if (cb == 2) { int16_t v = (int16_t)(s >> 16); memcpy(p, &v, 2); return; }
    if (cb == 3) { uint32_t u = (uint32_t)s; p[0] = (uint8_t)(u >> 8); p[1] = (uint8_t)(u >> 16); p[2] = (uint8_t)(u >> 24); return; }
    int32_t v = f.bits == 32 ? s : s >> (32 - f.bits); memcpy(p, &v, 4);
}

// ---------------------------------------------------------------- implementation
class BridgeImpl : public AudioCallback {
public:
    Engine engine;
    int engine_rate = 48000;
    bool mono = false;
    Fifo rx{65536}, tx{65536};          // rx: AES67 -> playback device, tx: capture device -> AES67
    std::atomic<uint32_t> rx_drop{0}, rx_under{0}, tx_drop{0}, tx_under{0};
    std::atomic<bool> tx_ready{false}, stop_flag{false}, restart{false}, running{false};
    size_t tx_target = 960;
    std::thread rt, ct;
    IMMDevice* pdev = nullptr; IMMDevice* cdev = nullptr;
    std::string pname, cname; bool loopback = false;
    mutable std::mutex err_mu; std::string error;

    void set_error(const std::string& e) { std::lock_guard<std::mutex> g(err_mu); error = e; LOG_E("%s", e.c_str()); }

    void process(const int32_t* const* in, int32_t* const* out, int n, int64_t, int64_t, uint64_t) override {
        for (int i = 0; i < n; i++) {
            int32_t l = in[0][i], r = in[1][i];
            if (mono) l = r = (l >> 1) + (r >> 1);
            if (!rx.push(l, r)) rx_drop++;
        }
        if (!tx_ready && tx.size() >= tx_target) tx_ready = true;
        for (int i = 0; i < n; i++) {
            if (!tx_ready || !tx.pop(out[0][i], out[1][i])) { out[0][i] = out[1][i] = 0; if (tx_ready) tx_under++; }
        }
        if (tx_ready && tx.size() == 0) tx_ready = false;
    }
    void reset_request() override { restart = true; }

    void render_thread() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        make_thread_realtime("bridge-render");
        IAudioClient* ac = nullptr; IAudioRenderClient* rc = nullptr; WAVEFORMATEX* wf = nullptr;
        HRESULT hr = pdev->Activate(kIidAudioClient, CLSCTX_ALL, nullptr, (void**)&ac);
        if (FAILED(hr)) { set_error("playback device: Activate failed"); return; }
        ac->GetMixFormat(&wf);
        Fmt f = parse_fmt(wf);
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 200000, 0, wf, nullptr);
        if (FAILED(hr)) { set_error("playback device: Initialize failed (in use exclusively?)"); ac->Release(); CoTaskMemFree(wf); return; }
        ac->SetEventHandle(ev);
        UINT32 bufFrames = 0; ac->GetBufferSize(&bufFrames);
        ac->GetService(kIidRenderClient, (void**)&rc);
        LOG_I("bridge playback -> \"%s\": %d Hz, %d ch, %s%d bit, buffer %u", pname.c_str(), f.rate, f.ch, f.is_float ? "float" : "int", f.bits, bufFrames);
        Resampler rs; double nominal = (double)engine_rate / f.rate;
        const size_t target = (size_t)(engine_rate * 0.02) + (size_t)(bufFrames * nominal);
        bool primed = false;
        ac->Start();
        while (!stop_flag) {
            if (WaitForSingleObject(ev, 1000) != WAIT_OBJECT_0) continue;
            UINT32 pad = 0; ac->GetCurrentPadding(&pad);
            UINT32 frames = bufFrames - pad;
            if (!frames) continue;
            BYTE* data = nullptr;
            if (FAILED(rc->GetBuffer(frames, &data))) continue;
            size_t level = rx.size();
            if (!primed && level >= target) primed = true;
            if (!primed) { rc->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT); continue; }
            double adj = 1.0 + ((double)level - (double)target) / (double)target * 0.02;
            adj = std::max(0.995, std::min(1.005, adj));
            double step = nominal * adj;
            int cb = f.block / f.ch;
            for (UINT32 i = 0; i < frames; i++) {
                int32_t l, r;
                if (!rs.produce(rx, step, l, r)) { rx_under++; primed = false; }
                uint8_t* p = data + (size_t)i * f.block;
                for (int c = 0; c < f.ch; c++) write_sample(p + c * cb, f, c == 0 ? l : (c == 1 ? r : 0));
            }
            rc->ReleaseBuffer(frames, 0);
        }
        ac->Stop();
        rc->Release(); ac->Release(); CoTaskMemFree(wf); CloseHandle(ev);
    }

    void capture_thread() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        make_thread_realtime("bridge-capture");
        IAudioClient* ac = nullptr; IAudioCaptureClient* cc = nullptr; WAVEFORMATEX* wf = nullptr;
        HRESULT hr = cdev->Activate(kIidAudioClient, CLSCTX_ALL, nullptr, (void**)&ac);
        if (FAILED(hr)) { set_error("source device: Activate failed"); return; }
        ac->GetMixFormat(&wf);
        Fmt f = parse_fmt(wf);
        hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0, 200000, 0, wf, nullptr);
        if (FAILED(hr)) { set_error("source device: Initialize failed"); ac->Release(); CoTaskMemFree(wf); return; }
        ac->GetService(kIidCaptureClient, (void**)&cc);
        LOG_I("bridge capture <- \"%s\"%s: %d Hz, %d ch, %s%d bit", cname.c_str(), loopback ? " (loopback)" : "", f.rate, f.ch, f.is_float ? "float" : "int", f.bits);
        Fifo stage(32768); Resampler rs; double nominal = (double)f.rate / engine_rate;
        tx_target = (size_t)(engine_rate * 0.02);
        ac->Start();
        while (!stop_flag) {
            Sleep(5);
            UINT32 next = 0;
            while (SUCCEEDED(cc->GetNextPacketSize(&next)) && next > 0) {
                BYTE* data = nullptr; UINT32 frames = 0; DWORD fl = 0;
                if (FAILED(cc->GetBuffer(&data, &frames, &fl, nullptr, nullptr))) break;
                int cb = f.block / f.ch;
                for (UINT32 i = 0; i < frames; i++) {
                    int32_t l = 0, r = 0;
                    if (!(fl & AUDCLNT_BUFFERFLAGS_SILENT)) {
                        const uint8_t* p = data + (size_t)i * f.block;
                        l = read_sample(p, f); r = f.ch > 1 ? read_sample(p + cb, f) : l;
                        if (mono) l = r = (l >> 1) + (r >> 1);
                    }
                    if (!stage.push(l, r)) tx_drop++;
                }
                cc->ReleaseBuffer(frames);
                size_t level = tx.size();
                double adj = 1.0 + ((double)tx_target * 2 - (double)level) / (double)(tx_target * 2) * 0.02;
                adj = std::max(0.995, std::min(1.005, adj));
                double step = nominal / adj;
                int32_t ol, orr;
                while (stage.size() > 4 && rs.produce(stage, step, ol, orr)) {
                    if (!tx.push(ol, orr)) { tx_drop++; break; }
                }
            }
        }
        ac->Stop();
        cc->Release(); ac->Release(); CoTaskMemFree(wf);
    }
};

BridgeRunner::BridgeRunner() {}
BridgeRunner::~BridgeRunner() { stop(); }
bool BridgeRunner::running() const { return impl_ && impl_->running; }
bool BridgeRunner::restart_needed() { return impl_ && impl_->restart; }

bool BridgeRunner::start(const Config& cfg, const std::string& play, const std::string& source, bool loopback, std::string& err) {
    stop();
    impl_.reset(new BridgeImpl());
    BridgeImpl& b = *impl_;
    b.engine_rate = cfg.sample_rate; b.loopback = loopback; b.mono = cfg.channels == 1;
    IMMDeviceEnumerator* en = enumerator();
    if (!en) { err = "cannot access Windows audio devices"; impl_.reset(); return false; }
    if (lower(play) != "none") {
        b.pdev = find_device(en, eRender, play, b.pname);
        if (!b.pdev) { err = "playback device '" + play + "' not found"; en->Release(); impl_.reset(); return false; }
    }
    if (lower(source) != "none") {
        b.cdev = find_device(en, loopback ? eRender : eCapture, source, b.cname);
        if (!b.cdev) { err = std::string(loopback ? "playback" : "recording") + " device '" + source + "' not found"; en->Release(); impl_.reset(); return false; }
    }
    en->Release();
    b.engine.set_status_name(StatusShare::kBridgeName);
    if (!b.engine.start(cfg, cfg.buffer_frames, &b, err)) { err = "engine: " + err; impl_.reset(); return false; }
    b.running = true;
    if (b.pdev) b.rt = std::thread([&b] { b.render_thread(); });
    if (b.cdev) b.ct = std::thread([&b] { b.capture_thread(); });
    LOG_I("bridge started: play=%s source=%s%s", b.pname.c_str(), b.cname.c_str(), loopback ? " (loopback)" : "");
    return true;
}

void BridgeRunner::stop() {
    if (!impl_) return;
    BridgeImpl& b = *impl_;
    b.stop_flag = true;
    if (b.rt.joinable()) b.rt.join();
    if (b.ct.joinable()) b.ct.join();
    b.engine.stop();
    if (b.pdev) b.pdev->Release();
    if (b.cdev) b.cdev->Release();
    b.running = false;
    impl_.reset();
}

BridgeStats BridgeRunner::stats() const {
    BridgeStats s;
    if (!impl_) return s;
    const BridgeImpl& b = *impl_;
    s.running = b.running; s.play_name = b.pname; s.source_name = b.cname; s.loopback = b.loopback;
    s.play_under = b.rx_under; s.play_drop = b.rx_drop; s.cap_under = b.tx_under; s.cap_drop = b.tx_drop;
    s.play_fifo = b.rx.size(); s.cap_fifo = b.tx.size();
    { std::lock_guard<std::mutex> g(b.err_mu); s.error = b.error; }
    return s;
}
StatusBlock BridgeRunner::engine_status() const {
    if (!impl_) return StatusBlock();
    return impl_->engine.snapshot();
}

} // namespace aes67
#endif
