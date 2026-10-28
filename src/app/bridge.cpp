// aes67vsc-bridge: connects the AES67 streams to ordinary Windows (WASAPI) audio devices.
//   receive stream  -> any playback device (speakers, or "CABLE Input" of VB-CABLE so that
//                      normal apps can record the AES67 audio from "CABLE Output")
//   transmit stream <- any recording device (microphone, "CABLE Output") or the loopback of a
//                      playback device (everything the PC plays)
// The WASAPI devices run on their own clocks; small FIFOs with a rate-controlled linear
// resampler absorb the drift against the PTP-paced engine.
#include "../core/platform.h"   // winsock2 before windows.h
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include "../core/engine.h"
#include "../core/config.h"
#include "../core/log.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <algorithm>
#include <csignal>

using namespace aes67;

static std::atomic<bool> g_stop{false};
static BOOL WINAPI on_ctrl(DWORD) { g_stop = true; return TRUE; }

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
    size_t capacity() const { return cap_; }
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
    void clear() { rd_.store(wr_.load()); }
private:
    size_t cap_;
    std::vector<int32_t> left_, right_;
    std::atomic<size_t> wr_{0}, rd_{0};
};

// Linear-interpolation resampler pulling from a FIFO; 'step' = input frames per output frame.
struct Resampler {
    double pos = 1.0;            // fractional position between prev and cur
    int32_t prev[2] = {0, 0}, cur[2] = {0, 0};
    bool primed = false;
    // Produces one output frame. Returns false if the FIFO ran dry.
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

// ---------------------------------------------------------------- device enumeration
struct Dev { std::string id, name; bool render; };
static std::vector<Dev> list_devices(IMMDeviceEnumerator* en, EDataFlow flow) {
    std::vector<Dev> out;
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
static IMMDevice* find_device(IMMDeviceEnumerator* en, EDataFlow flow, const std::string& want, std::string& name) {
    IMMDevice* d = nullptr;
    if (want.empty() || lower(want) == "default") {
        if (FAILED(en->GetDefaultAudioEndpoint(flow, eConsole, &d))) return nullptr;
        for (auto& x : list_devices(en, flow)) { LPWSTR id; d->GetId(&id); if (narrow(id) == x.id) name = x.name; CoTaskMemFree(id); }
        return d;
    }
    for (auto& x : list_devices(en, flow)) {
        if (lower(x.name).find(lower(want)) != std::string::npos) {
            std::wstring wid(x.id.begin(), x.id.end());
            if (SUCCEEDED(en->GetDevice(wid.c_str(), &d))) { name = x.name; return d; }
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------- sample format helpers
static const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
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

// ---------------------------------------------------------------- the bridge
struct Bridge : AudioCallback {
    int engine_rate = 48000;
    Fifo rx{65536}, tx{65536};          // rx: AES67 -> playback device, tx: capture device -> AES67
    std::atomic<uint32_t> rx_drop{0}, rx_under{0}, tx_drop{0}, tx_under{0};
    std::atomic<bool> tx_ready{false};
    size_t tx_target = 0;

    void process(const int32_t* const* in, int32_t* const* out, int n, int64_t, int64_t, uint64_t) override {
        for (int i = 0; i < n; i++) if (!rx.push(in[0][i], in[1][i])) rx_drop++;
        if (!tx_ready && tx.size() >= tx_target) tx_ready = true;
        for (int i = 0; i < n; i++) {
            if (!tx_ready || !tx.pop(out[0][i], out[1][i])) { out[0][i] = out[1][i] = 0; if (tx_ready) tx_under++; }
        }
        if (tx_ready && tx.size() == 0) tx_ready = false;   // re-prime after an underrun
    }

    // Playback thread: pull from rx FIFO, resample to the device rate, render.
    void render_thread(IMMDevice* dev, std::string name) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        make_thread_realtime("bridge-render");
        IAudioClient* ac = nullptr; IAudioRenderClient* rc = nullptr; WAVEFORMATEX* wf = nullptr;
        HRESULT hr = dev->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr, (void**)&ac);
        if (FAILED(hr)) { LOG_E("playback: Activate failed 0x%08lx", (unsigned long)hr); return; }
        ac->GetMixFormat(&wf);
        Fmt f = parse_fmt(wf);
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 200000 /*20 ms*/, 0, wf, nullptr);
        if (FAILED(hr)) { LOG_E("playback: Initialize failed 0x%08lx", (unsigned long)hr); return; }
        ac->SetEventHandle(ev);
        UINT32 bufFrames = 0; ac->GetBufferSize(&bufFrames);
        ac->GetService(IID_IAudioRenderClient, (void**)&rc);
        LOG_I("playback -> \"%s\": %d Hz, %d ch, %s%d bit, buffer %u frames", name.c_str(), f.rate, f.ch, f.is_float ? "float" : "int", f.bits, bufFrames);
        Resampler rs; double nominal = (double)engine_rate / f.rate;
        const size_t target = (size_t)(engine_rate * 0.02) + (size_t)(bufFrames * nominal);   // ~20 ms + one device buffer
        bool primed = false;
        ac->Start();
        while (!g_stop) {
            if (WaitForSingleObject(ev, 1000) != WAIT_OBJECT_0) continue;
            UINT32 pad = 0; ac->GetCurrentPadding(&pad);
            UINT32 frames = bufFrames - pad;
            if (!frames) continue;
            BYTE* data = nullptr;
            if (FAILED(rc->GetBuffer(frames, &data))) continue;
            size_t level = rx.size();
            if (!primed && level >= target) primed = true;
            if (!primed) { rc->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT); continue; }
            // Rate control: consume a little faster when the FIFO is high, slower when low.
            double adj = 1.0 + ((double)level - (double)target) / (double)target * 0.02;
            adj = std::max(0.995, std::min(1.005, adj));
            double step = nominal * adj;
            for (UINT32 i = 0; i < frames; i++) {
                int32_t l, r;
                if (!rs.produce(rx, step, l, r)) { rx_under++; primed = false; }
                uint8_t* p = data + (size_t)i * f.block;
                int cb = f.block / f.ch;
                for (int c = 0; c < f.ch; c++) write_sample(p + c * cb, f, c == 0 ? l : (c == 1 ? r : 0));
            }
            rc->ReleaseBuffer(frames, 0);
        }
        ac->Stop();
        rc->Release(); ac->Release(); CoTaskMemFree(wf); CloseHandle(ev);
    }

    // Capture thread: read from a recording device (or playback loopback), resample to the engine rate, push to tx FIFO.
    void capture_thread(IMMDevice* dev, std::string name, bool loopback) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        make_thread_realtime("bridge-capture");
        IAudioClient* ac = nullptr; IAudioCaptureClient* cc = nullptr; WAVEFORMATEX* wf = nullptr;
        HRESULT hr = dev->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr, (void**)&ac);
        if (FAILED(hr)) { LOG_E("capture: Activate failed 0x%08lx", (unsigned long)hr); return; }
        ac->GetMixFormat(&wf);
        Fmt f = parse_fmt(wf);
        DWORD flags = loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
        hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 200000, 0, wf, nullptr);
        if (FAILED(hr)) { LOG_E("capture: Initialize failed 0x%08lx", (unsigned long)hr); return; }
        ac->GetService(IID_IAudioCaptureClient, (void**)&cc);
        LOG_I("capture <- \"%s\"%s: %d Hz, %d ch, %s%d bit", name.c_str(), loopback ? " (loopback)" : "", f.rate, f.ch, f.is_float ? "float" : "int", f.bits);
        // Resample device -> engine using a small staging FIFO.
        Fifo stage(32768); Resampler rs; double nominal = (double)f.rate / engine_rate;
        tx_target = (size_t)(engine_rate * 0.02);
        ac->Start();
        while (!g_stop) {
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
                    }
                    if (!stage.push(l, r)) tx_drop++;
                }
                cc->ReleaseBuffer(frames);
                // Drain the staging FIFO into the engine FIFO at the controlled rate.
                size_t level = tx.size();
                double adj = 1.0 + ((double)tx_target * 2 - (double)level) / (double)(tx_target * 2) * 0.02;   // low level -> produce more
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

static void usage() {
    printf("aes67vsc-bridge: route the AES67 streams to/from Windows audio devices\n\n"
           "  aes67vsc-bridge --list\n"
           "  aes67vsc-bridge [--play <device|default|none>] [--capture <device|default|none>] [--loopback <device|default>]\n\n"
           "  --play      playback device that receives the AES67 input stream (name substring)\n"
           "  --capture   recording device whose audio is sent as the AES67 output stream\n"
           "  --loopback  instead of --capture: send everything played on this playback device\n\n"
           "Defaults: --play default --capture default. With VB-CABLE installed:\n"
           "  aes67vsc-bridge --play \"CABLE Input\" --loopback \"Speakers\"\n"
           "then normal apps record the AES67 audio from \"CABLE Output\".\n"
           "Streams, interface and clock come from the same configuration as the ASIO driver (aes67vsc-panel.exe).\n");
}

int main(int argc, char** argv) {
    std::string play = "default", cap = "default"; bool loopback = false, list = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--list") list = true;
        else if (a == "--play") play = next();
        else if (a == "--capture") { cap = next(); loopback = false; }
        else if (a == "--loopback") { cap = next(); loopback = true; }
        else { usage(); return 1; }
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void**)&en))) { fprintf(stderr, "cannot create device enumerator\n"); return 2; }
    if (list) {
        printf("Playback devices:\n");  for (auto& d : list_devices(en, eRender))  printf("  %s\n", d.name.c_str());
        printf("Recording devices:\n"); for (auto& d : list_devices(en, eCapture)) printf("  %s\n", d.name.c_str());
        return 0;
    }
    Config cfg; cfg.load();
    log_set_file(config_dir() + "\\bridge.log");
    Bridge br; br.engine_rate = cfg.sample_rate;
    std::string pname, cname;
    IMMDevice* pdev = lower(play) == "none" ? nullptr : find_device(en, eRender, play, pname);
    IMMDevice* cdev = lower(cap) == "none" ? nullptr : find_device(en, loopback ? eRender : eCapture, cap, cname);
    if (lower(play) != "none" && !pdev) { fprintf(stderr, "playback device '%s' not found (use --list)\n", play.c_str()); return 2; }
    if (lower(cap) != "none" && !cdev) { fprintf(stderr, "%s device '%s' not found (use --list)\n", loopback ? "playback" : "recording", cap.c_str()); return 2; }

    Engine eng; std::string err;
    if (!eng.start(cfg, cfg.buffer_frames, &br, err)) { fprintf(stderr, "engine start failed: %s\n", err.c_str()); return 2; }
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    std::thread rt, ct;
    if (pdev) rt = std::thread([&] { br.render_thread(pdev, pname); });
    if (cdev) ct = std::thread([&] { br.capture_thread(cdev, cname, loopback); });
    printf("bridge running on %s. AES67 in -> %s ; AES67 out <- %s%s. Ctrl-C to stop.\n", eng.local_ip().c_str(),
           pdev ? pname.c_str() : "(none)", cdev ? cname.c_str() : "(none)", loopback ? " (loopback)" : "");
    int n = 0;
    while (!g_stop) {
        sleep_ms(500);
        if (++n % 10 == 0) {
            StatusBlock s = eng.snapshot();
            printf("ptp=%s off=%+.1fus rx=%u lost=%u tx=%u | play fifo %zu under=%u drop=%u | cap fifo %zu under=%u drop=%u\n",
                   ptp_state_name(s.ptp_state), s.ptp_offset_ns / 1000.0, s.rx_packets, s.rx_lost, s.tx_packets,
                   br.rx.size(), br.rx_under.load(), br.rx_drop.load(), br.tx.size(), br.tx_under.load(), br.tx_drop.load());
            fflush(stdout);
        }
    }
    g_stop = true;
    if (rt.joinable()) rt.join();
    if (ct.joinable()) ct.join();
    eng.stop();
    if (pdev) pdev->Release();
    if (cdev) cdev->Release();
    en->Release();
    return 0;
}
