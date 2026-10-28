// AES67 Virtual Sound Card - tray application and control panel.
// Tabs: Status (dashboard), Streams (SDP in/out), Bridge (Windows audio devices), Settings.
// Closing or minimising hides the window into the notification area; the tray icon colour
// mirrors the main indicator on the Status tab.
#include "../core/platform.h"
#include "../core/config.h"
#include "../core/sdp.h"
#include "../core/sap.h"
#include "../core/status.h"
#include "../core/ptp.h"
#include "../core/log.h"
#include "bridge_core.h"
#include "panel_res.h"
#include <windows.h>
#include <objbase.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>
#include <cstdio>
#include <string>
#include <vector>
#include <map>
#include <cmath>

using namespace aes67;

#ifndef AES67VSC_VERSION
#define AES67VSC_VERSION "dev"
#endif

static const char* kTitle = "AES67 Virtual Sound Card";
static const UINT WM_TRAY = WM_APP + 1;
static UINT WM_SHOWME = 0;
enum { PAGE_STATUS = 0, PAGE_STREAMS, PAGE_BRIDGE, PAGE_SETTINGS, PAGE_COUNT };

static HINSTANCE g_inst;
static HWND g_main, g_pages[PAGE_COUNT];
static int g_page = PAGE_STATUS;
static HFONT g_font_head, g_font_section, g_font_title, g_font_nav_bold;
static Config g_cfg;
static std::vector<NetInterface> g_ifaces;
static SapAgent g_sap;
static std::vector<SapEntry> g_found;
static StatusShare g_share;
static std::string g_pending_sdp;
static BridgeRunner g_bridge;
static std::string g_bridge_error;
static std::vector<BridgeDevice> g_devs;
static NOTIFYICONDATAA g_nid{};
static int g_tray_state = -1;
static bool g_balloon_shown = false, g_dirty = false, g_loading = false;
static std::map<int, COLORREF> g_dot_color;
static std::map<int, bool> g_checks;      // owner-drawn check boxes
static int g_last_notified_state = -1;
static int g_sidebar_px = 0;

// ---------------------------------------------------------------- theme
struct Theme {
    COLORREF side, panel, text, subtext, line, input_bg, input_text, accent, accent_text, btn, btn_border, nav_active;
    bool dark;
};
static Theme g_t;
static HBRUSH g_br_side, g_br_panel, g_br_input, g_br_btn;
static const COLORREF C_GREY = RGB(160, 160, 160), C_GREEN = RGB(52, 168, 83), C_YELLOW = RGB(241, 180, 24), C_RED = RGB(217, 48, 37);

static bool system_dark() {
    HKEY k; DWORD v = 1, len = sizeof v;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    RegQueryValueExA(k, "AppsUseLightTheme", nullptr, nullptr, (BYTE*)&v, &len);
    RegCloseKey(k);
    return v == 0;
}
static void set_window_theme(HWND h, const wchar_t* name) {
    static HMODULE ux = LoadLibraryW(L"uxtheme.dll");
    typedef HRESULT (WINAPI *Fn)(HWND, LPCWSTR, LPCWSTR);
    static Fn fn = ux ? (Fn)GetProcAddress(ux, "SetWindowTheme") : nullptr;
    if (fn) fn(h, name, nullptr);
}
static void set_dark_titlebar(HWND h, bool dark) {
    static HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    typedef HRESULT (WINAPI *Fn)(HWND, DWORD, LPCVOID, DWORD);
    static Fn fn = dwm ? (Fn)GetProcAddress(dwm, "DwmSetWindowAttribute") : nullptr;
    if (!fn) return;
    BOOL v = dark ? TRUE : FALSE;
    if (FAILED(fn(h, 20, &v, sizeof v))) fn(h, 19, &v, sizeof v);
}
static BOOL CALLBACK theme_child(HWND h, LPARAM) {
    char cls[32] = {0}; GetClassNameA(h, cls, sizeof cls);
    std::string c = cls;
    if (c == "ComboBox") set_window_theme(h, g_t.dark ? L"DarkMode_CFD" : L"Explorer");
    else if (c == "Edit") set_window_theme(h, g_t.dark ? L"DarkMode_Explorer" : L"Explorer");
    else if (c == "ScrollBar") set_window_theme(h, g_t.dark ? L"DarkMode_Explorer" : L"Explorer");
    return TRUE;
}
static void apply_theme() {
    bool dark = g_cfg.theme == "dark" || (g_cfg.theme == "auto" && system_dark());
    if (dark) g_t = {RGB(27, 27, 31), RGB(38, 38, 43), RGB(236, 236, 236), RGB(163, 163, 168), RGB(58, 58, 64), RGB(31, 31, 36), RGB(236, 236, 236), RGB(76, 141, 255), RGB(255, 255, 255), RGB(51, 51, 58), RGB(74, 74, 82), RGB(47, 58, 82), true};
    else      g_t = {RGB(237, 238, 240), RGB(255, 255, 255), RGB(31, 31, 31), RGB(107, 107, 107), RGB(227, 227, 227), RGB(255, 255, 255), RGB(31, 31, 31), RGB(47, 111, 237), RGB(255, 255, 255), RGB(243, 244, 246), RGB(208, 211, 216), RGB(221, 230, 247), false};
    for (HBRUSH* b : {&g_br_side, &g_br_panel, &g_br_input, &g_br_btn}) if (*b) DeleteObject(*b);
    g_br_side = CreateSolidBrush(g_t.side); g_br_panel = CreateSolidBrush(g_t.panel); g_br_input = CreateSolidBrush(g_t.input_bg); g_br_btn = CreateSolidBrush(g_t.btn);
    if (!g_main) return;
    set_dark_titlebar(g_main, dark);
    EnumChildWindows(g_main, theme_child, 0);
    for (HWND p : g_pages) if (p) EnumChildWindows(p, theme_child, 0);
    RedrawWindow(g_main, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME);
}

// ---------------------------------------------------------------- control helpers (controls live on pages)
static HWND ctl(int id) {
    for (HWND p : g_pages) { if (p) { HWND h = GetDlgItem(p, id); if (h) return h; } }
    return GetDlgItem(g_main, id);
}
static std::string get_text(int id) { char buf[2048] = {0}; GetWindowTextA(ctl(id), buf, sizeof buf); return buf; }
static void set_text(int id, const std::string& s) { if (get_text(id) != s) SetWindowTextA(ctl(id), s.c_str()); }
static bool is_checkbox(int id) { return id == IDC_RX_ENABLE || id == IDC_TX_ENABLE || id == IDC_TX_SAP || id == IDC_BR_AUTO || id == IDC_AUTORUN; }
static bool get_check(int id) { return g_checks[id]; }
static void set_check(int id, bool on) { if (g_checks[id] != on) { g_checks[id] = on; InvalidateRect(ctl(id), nullptr, TRUE); } }
static void combo_fill(int id, const std::vector<std::string>& items, int sel) {
    HWND h = ctl(id);
    SendMessageA(h, CB_RESETCONTENT, 0, 0);
    for (auto& s : items) SendMessageA(h, CB_ADDSTRING, 0, (LPARAM)s.c_str());
    SendMessageA(h, CB_SETCURSEL, (WPARAM)sel, 0);
}
static int combo_sel(int id) { return (int)SendMessageA(ctl(id), CB_GETCURSEL, 0, 0); }
static void set_dot(int id, COLORREF c) { if (g_dot_color[id] != c) { g_dot_color[id] = c; InvalidateRect(ctl(id), nullptr, TRUE); } }
static std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
static std::string exe_path() { char p[MAX_PATH] = {0}; GetModuleFileNameA(nullptr, p, MAX_PATH); return p; }
static std::string exe_dir() { std::string p = exe_path(); size_t s = p.find_last_of("\\/"); return s == std::string::npos ? "." : p.substr(0, s); }
static bool driver_status(StatusBlock& s) { return g_share.read(s) && s.engine_running && wall_ms() - s.updated_ms < 3000; }
static void set_dirty(bool d) {
    if (g_loading) return;
    g_dirty = d;
    EnableWindow(ctl(IDC_APPLY), d);
    InvalidateRect(ctl(IDC_APPLY), nullptr, TRUE);
    set_text(IDC_FOOT, d ? "Unsaved changes - press Apply to save and apply them." : "");
}
static std::string human(uint32_t n) {
    char b[32];
    if (n >= 1000000) snprintf(b, sizeof b, "%.1fM", n / 1e6); else if (n >= 10000) snprintf(b, sizeof b, "%uk", n / 1000); else snprintf(b, sizeof b, "%u", n);
    return b;
}

// ---------------------------------------------------------------- autorun (HKCU Run key)
static bool autorun_enabled() {
    HKEY k; char buf[1024] = {0}; DWORD len = sizeof buf;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    LONG r = RegQueryValueExA(k, "AES67VSC", nullptr, nullptr, (BYTE*)buf, &len);
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}
static void set_autorun(bool on) {
    HKEY k;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    if (on) { std::string v = "\"" + exe_path() + "\" --tray"; RegSetValueExA(k, "AES67VSC", 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)v.size() + 1); }
    else RegDeleteValueA(k, "AES67VSC");
    RegCloseKey(k);
}

// ---------------------------------------------------------------- virtual cables
static std::string virtual_mic_for(const std::string& play_name) {
    std::string l = lower(play_name);
    if (l.find("cable-a input") != std::string::npos) return "CABLE-A Output";
    if (l.find("cable-b input") != std::string::npos) return "CABLE-B Output";
    if (l.find("cable input") != std::string::npos) return "CABLE Output";
    if (l.find("voicemeeter aux input") != std::string::npos) return "Voicemeeter Aux Output";
    if (l.find("voicemeeter vaio3 input") != std::string::npos) return "Voicemeeter VAIO3 Output";
    if (l.find("voicemeeter input") != std::string::npos) return "Voicemeeter Output";
    if (l.find("virtual") != std::string::npos && l.find("cable") != std::string::npos) return "(its recording side)";
    return "";
}
static std::string annotate(const std::string& name) {
    std::string mic = virtual_mic_for(name);
    return mic.empty() ? name : name + "   [virtual cable -> mic \"" + mic + "\"]";
}
static std::string strip_annotation(const std::string& s) { size_t p = s.find("   ["); return p == std::string::npos ? s : s.substr(0, p); }

static void fill_bridge_combos() {
    g_devs = bridge_list_devices();
    std::vector<std::string> play{"Default playback device", "(do not play)"}, src{"Default recording device (microphone)", "(send silence)"};
    int psel = lower(g_cfg.bridge_play) == "none" ? 1 : 0, ssel = lower(g_cfg.bridge_source) == "none" ? 1 : 0;
    bool pnamed = lower(g_cfg.bridge_play) != "default" && lower(g_cfg.bridge_play) != "none";
    bool snamed = lower(g_cfg.bridge_source) != "default" && lower(g_cfg.bridge_source) != "none";
    for (auto& d : g_devs) if (d.render) { play.push_back(annotate(d.name)); if (pnamed && lower(d.name).find(lower(g_cfg.bridge_play)) != std::string::npos && psel < 2) psel = (int)play.size() - 1; }
    for (auto& d : g_devs) if (!d.render) { src.push_back(d.name); if (!g_cfg.bridge_loopback && snamed && lower(d.name).find(lower(g_cfg.bridge_source)) != std::string::npos && ssel < 2) ssel = (int)src.size() - 1; }
    src.push_back("Loopback: default playback device (everything the PC plays)");
    if (g_cfg.bridge_loopback && lower(g_cfg.bridge_source) == "default") ssel = (int)src.size() - 1;
    for (auto& d : g_devs) if (d.render) { std::string mic = virtual_mic_for(d.name); src.push_back("Loopback: " + d.name + (mic.empty() ? "" : "   [virtual cable: set it as the app's speaker]")); if (g_cfg.bridge_loopback && snamed && lower(d.name).find(lower(g_cfg.bridge_source)) != std::string::npos) ssel = (int)src.size() - 1; }
    combo_fill(IDC_BR_PLAY, play, psel);
    combo_fill(IDC_BR_SOURCE, src, ssel);
}
static void read_bridge_combos() {
    std::string p = strip_annotation(get_text(IDC_BR_PLAY)), s = strip_annotation(get_text(IDC_BR_SOURCE));
    int pi = combo_sel(IDC_BR_PLAY), si = combo_sel(IDC_BR_SOURCE);
    g_cfg.bridge_play = pi == 0 ? "default" : pi == 1 ? "none" : p;
    if (si == 0) { g_cfg.bridge_source = "default"; g_cfg.bridge_loopback = false; }
    else if (si == 1) { g_cfg.bridge_source = "none"; g_cfg.bridge_loopback = false; }
    else if (s.rfind("Loopback: default", 0) == 0) { g_cfg.bridge_source = "default"; g_cfg.bridge_loopback = true; }
    else if (s.rfind("Loopback: ", 0) == 0) { g_cfg.bridge_source = s.substr(10); g_cfg.bridge_loopback = true; }
    else { g_cfg.bridge_source = s; g_cfg.bridge_loopback = false; }
    g_cfg.bridge_autostart = get_check(IDC_BR_AUTO);
}

// ---------------------------------------------------------------- UI <-> config
static std::string describe_rx(const Config& c) {
    if (c.rx.dest.empty()) return "No incoming stream configured.\nLoad the SDP file exported by the sending device, or pick a discovered stream below.";
    const StreamDesc& d = c.rx;
    char buf[512];
    snprintf(buf, sizeof buf, "%s\n%s:%u%s   %s / %d Hz / %d ch   payload %d, %g ms packets%s%s\n%s",
             d.name.empty() ? "(unnamed stream)" : d.name.c_str(), d.dest.c_str(), d.port, d.source.empty() ? "" : (" from " + d.source).c_str(),
             encoding_name(d.encoding), d.sample_rate, d.channels, d.payload_type, d.ptime_ms,
             d.ptp_domain >= 0 ? "   PTP domain " : "", d.ptp_domain >= 0 ? std::to_string(d.ptp_domain).c_str() : "",
             d.refclk.empty() ? "" : ("clock: " + d.refclk).c_str());
    return buf;
}
static void fill_buffer_combo(int rate, int value) {
    Config tmp = g_cfg; tmp.sample_rate = rate;
    int fpp = tmp.tx_stream("0.0.0.0", "").frames_per_packet();
    std::vector<std::string> b;
    for (int m : {1, 2, 3, 4, 5, 6, 8, 10, 16, 20, 32, 40}) b.push_back(std::to_string(fpp * m));
    combo_fill(IDC_BUFFER, b, -1);
    set_text(IDC_BUFFER, std::to_string(value));
}
static void load_to_ui() {
    g_loading = true;
    std::vector<std::string> names{"Automatic"}; int sel = 0;
    for (size_t i = 0; i < g_ifaces.size(); i++) {
        names.push_back(g_ifaces[i].ipv4 + "  (" + g_ifaces[i].name + ")");
        if (g_cfg.interface_ip != "auto" && g_ifaces[i].ipv4 == g_cfg.interface_ip) sel = (int)i + 1;
    }
    combo_fill(IDC_IFACE, names, sel);
    combo_fill(IDC_RATE, {"44100 Hz", "48000 Hz", "96000 Hz"}, g_cfg.sample_rate == 44100 ? 0 : g_cfg.sample_rate == 96000 ? 2 : 1);
    combo_fill(IDC_CHANNELS, {"2 x 2 stereo", "1 x 1 mono (sum)"}, g_cfg.channels == 1 ? 1 : 0);
    fill_buffer_combo(g_cfg.sample_rate, g_cfg.buffer_frames);
    combo_fill(IDC_PTPMODE, {"Automatic", "Follow only", "Master", "Off"}, (int)g_cfg.ptp_mode);
    combo_fill(IDC_THEME, {"Follow Windows", "Light", "Dark"}, g_cfg.theme == "light" ? 1 : g_cfg.theme == "dark" ? 2 : 0);
    set_text(IDC_PTPDOMAIN, std::to_string(g_cfg.ptp_domain));
    char lo[32]; snprintf(lo, sizeof lo, "%g", g_cfg.link_offset_ms); set_text(IDC_LINKOFF, lo);
    set_check(IDC_RX_ENABLE, g_cfg.rx_enabled && !g_cfg.rx.dest.empty());
    set_text(IDC_RX_INFO, describe_rx(g_cfg));
    set_text(IDC_RX_CHOFF, std::to_string(g_cfg.rx_channel_offset + 1));
    set_check(IDC_TX_ENABLE, g_cfg.tx_enabled);
    set_check(IDC_TX_SAP, g_cfg.sap_announce);
    set_text(IDC_TX_NAME, g_cfg.tx_name);
    set_text(IDC_TX_DEST, g_cfg.tx_dest);
    set_text(IDC_TX_PORT, std::to_string(g_cfg.tx_port));
    combo_fill(IDC_TX_ENC, {"L24", "L16"}, g_cfg.tx_encoding == Encoding::L16 ? 1 : 0);
    fill_bridge_combos();
    set_check(IDC_BR_AUTO, g_cfg.bridge_autostart);
    set_check(IDC_AUTORUN, autorun_enabled());
    g_loading = false;
    set_dirty(false);
}
static bool ui_to_cfg(std::string& err) {
    Config c = g_cfg;
    int is = combo_sel(IDC_IFACE);
    c.interface_ip = (is <= 0 || is > (int)g_ifaces.size()) ? "auto" : g_ifaces[is - 1].ipv4;
    int rs = combo_sel(IDC_RATE);
    c.sample_rate = rs == 0 ? 44100 : rs == 2 ? 96000 : 48000;
    c.channels = combo_sel(IDC_CHANNELS) == 1 ? 1 : 2;
    c.buffer_frames = atoi(get_text(IDC_BUFFER).c_str());
    c.ptp_mode = (PtpMode)std::max(0, combo_sel(IDC_PTPMODE));
    c.ptp_domain = atoi(get_text(IDC_PTPDOMAIN).c_str());
    { int t = combo_sel(IDC_THEME); c.theme = t == 1 ? "light" : t == 2 ? "dark" : "auto"; }
    c.link_offset_ms = atof(get_text(IDC_LINKOFF).c_str());
    c.rx_enabled = get_check(IDC_RX_ENABLE);
    c.rx_channel_offset = std::max(0, atoi(get_text(IDC_RX_CHOFF).c_str()) - 1);
    c.tx_enabled = get_check(IDC_TX_ENABLE);
    c.sap_announce = get_check(IDC_TX_SAP);
    c.tx_name = get_text(IDC_TX_NAME);
    c.tx_dest = get_text(IDC_TX_DEST);
    c.tx_port = (uint16_t)atoi(get_text(IDC_TX_PORT).c_str());
    c.tx_encoding = combo_sel(IDC_TX_ENC) == 1 ? Encoding::L16 : Encoding::L24;
    in_addr a;
    if (c.tx_enabled && inet_pton(AF_INET, c.tx_dest.c_str(), &a) != 1) { err = "The outgoing multicast address is not a valid IPv4 address."; return false; }
    if (c.tx_port < 1024) { err = "The outgoing port must be 1024 or higher."; return false; }
    if (c.buffer_frames < 16 || c.buffer_frames > 4096) { err = "Buffer size must be between 16 and 4096 frames."; return false; }
    if (c.link_offset_ms < 0.5 || c.link_offset_ms > 500) { err = "Playout delay must be between 0.5 and 500 ms."; return false; }
    if (c.rx_enabled && !c.rx.dest.empty() && c.rx.sample_rate != c.sample_rate) {
        err = "The incoming stream is " + std::to_string(c.rx.sample_rate) + " Hz but the device sample rate is " + std::to_string(c.sample_rate) + " Hz. Set them equal.";
        return false;
    }
    g_cfg = c;
    read_bridge_combos();
    std::string e2; Config n; n.from_ini(g_cfg.to_ini(), e2);
    g_cfg = n;
    return true;
}
static bool apply() {
    std::string err;
    if (!ui_to_cfg(err)) { MessageBoxA(g_main, err.c_str(), kTitle, MB_OK | MB_ICONWARNING); return false; }
    if (!g_cfg.save()) { MessageBoxA(g_main, ("Cannot write " + Config::default_path()).c_str(), kTitle, MB_OK | MB_ICONERROR); return false; }
    set_autorun(get_check(IDC_AUTORUN));
    load_to_ui();
    apply_theme();
    std::string e; g_sap.stop(); g_sap.start(pick_interface(g_cfg.interface_ip).ipv4, true, e);
    return true;
}

static void select_page(int i);
// ---------------------------------------------------------------- SDP import / export
static bool import_sdp_text(const std::string& text, const std::string& source_desc) {
    StreamDesc d; std::string err;
    if (!sdp_parse(text, d, err)) { MessageBoxA(g_main, ("Could not use this SDP:\n" + err).c_str(), kTitle, MB_OK | MB_ICONERROR); return false; }
    std::string notes;
    if (d.sample_rate != g_cfg.sample_rate) { notes += "The stream runs at " + std::to_string(d.sample_rate) + " Hz; the device sample rate was changed to match.\n"; g_cfg.sample_rate = d.sample_rate; }
    if (d.channels > 2) notes += "The stream has " + std::to_string(d.channels) + " channels; inputs 1-2 take stream channels " + std::to_string(g_cfg.rx_channel_offset + 1) + "-" + std::to_string(g_cfg.rx_channel_offset + 2) + " (adjustable on the Streams tab).\n";
    if (d.ptp_domain >= 0 && d.ptp_domain != g_cfg.ptp_domain) { notes += "The stream uses PTP domain " + std::to_string(d.ptp_domain) + "; the PTP domain was changed to match.\n"; g_cfg.ptp_domain = d.ptp_domain; }
    g_cfg.rx = d; g_cfg.rx_sdp_text = text; g_cfg.rx_enabled = true;
    load_to_ui();
    set_dirty(true);
    select_page(PAGE_STREAMS);
    if (!notes.empty()) MessageBoxA(g_main, (source_desc + "\n\n" + notes + "\nPress Apply to save.").c_str(), kTitle, MB_OK | MB_ICONINFORMATION);
    return true;
}
static void do_load_sdp() {
    char file[MAX_PATH] = {0};
    OPENFILENAMEA ofn{}; ofn.lStructSize = sizeof ofn; ofn.hwndOwner = g_main;
    ofn.lpstrFilter = "SDP files (*.sdp)\0*.sdp\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH; ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY; ofn.lpstrTitle = "Load incoming stream SDP";
    if (!GetOpenFileNameA(&ofn)) return;
    std::string text;
    if (!read_file(file, text)) { MessageBoxA(g_main, "Cannot read the file.", kTitle, MB_OK | MB_ICONERROR); return; }
    import_sdp_text(text, std::string("Loaded ") + file);
}
static std::string current_tx_sdp() {
    NetInterface ni = pick_interface(g_cfg.interface_ip);
    std::string gm; StatusBlock s;
    if (g_bridge.running()) { StatusBlock b = g_bridge.engine_status(); if (b.ptp_master[0]) gm = b.ptp_master; }
    if (gm.empty() && driver_status(s) && s.ptp_master[0]) gm = s.ptp_master;
    if (gm.empty()) {
        uint8_t id[8] = {0, 0, 0, 0xFF, 0xFE, 0, 0, 0};
        if (ni.has_mac) { id[0] = ni.mac[0]; id[1] = ni.mac[1]; id[2] = ni.mac[2]; id[5] = ni.mac[3]; id[6] = ni.mac[4]; id[7] = ni.mac[5]; }
        gm = ptp_identity_string(id);
    }
    std::string refclk = g_cfg.ptp_mode == PtpMode::Off ? "" : ("ptp=IEEE1588-2008:" + gm + ":" + std::to_string(g_cfg.ptp_domain));
    StreamDesc d = g_cfg.tx_stream(ni.ipv4, refclk);
    d.session_id = std::to_string((uint32_t)(wall_ms() / 1000)); d.session_version = d.session_id;
    return sdp_generate(d);
}
static void do_export_sdp() {
    if (!apply()) return;
    char file[MAX_PATH] = {0};
    std::string def = g_cfg.tx_name; for (auto& ch : def) if (strchr("\\/:*?\"<>|", ch)) ch = '_';
    snprintf(file, sizeof file, "%s.sdp", def.c_str());
    OPENFILENAMEA ofn{}; ofn.lStructSize = sizeof ofn; ofn.hwndOwner = g_main;
    ofn.lpstrFilter = "SDP files (*.sdp)\0*.sdp\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH; ofn.Flags = OFN_OVERWRITEPROMPT; ofn.lpstrDefExt = "sdp"; ofn.lpstrTitle = "Save outgoing stream SDP";
    if (!GetSaveFileNameA(&ofn)) return;
    if (!write_file(file, current_tx_sdp())) MessageBoxA(g_main, "Cannot write the file.", kTitle, MB_OK | MB_ICONERROR);
}
static void do_copy_sdp() {
    if (!apply()) return;
    std::string s = current_tx_sdp();
    if (!OpenClipboard(g_main)) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, s.size() + 1);
    if (h) { memcpy(GlobalLock(h), s.c_str(), s.size() + 1); GlobalUnlock(h); SetClipboardData(CF_TEXT, h); }
    CloseClipboard();
    set_text(IDC_FOOT, "SDP copied to the clipboard.");
}

// ---------------------------------------------------------------- bridge control
static void bridge_stop() { g_bridge.stop(); g_bridge_error.clear(); }
static bool bridge_start(bool interactive) {
    bridge_stop();
    std::string err;
    if (!g_bridge.start(g_cfg, g_cfg.bridge_play, g_cfg.bridge_source, g_cfg.bridge_loopback, err)) {
        g_bridge_error = err;
        if (interactive) MessageBoxA(g_main, ("The bridge could not start:\n" + err).c_str(), kTitle, MB_OK | MB_ICONERROR);
        return false;
    }
    return true;
}
static void bridge_toggle() {
    if (g_bridge.running()) { bridge_stop(); return; }
    if (g_dirty && !apply()) return;
    if (!g_dirty) { std::string e; if (!ui_to_cfg(e)) { MessageBoxA(g_main, e.c_str(), kTitle, MB_OK | MB_ICONWARNING); return; } }
    bridge_start(true);
}
static void teams_preset() {
    g_devs = bridge_list_devices();
    std::string cable, cable_a, vm_in, vm_aux;
    for (auto& d : g_devs) if (d.render) {
        std::string l = lower(d.name);
        if (l.find("cable input") != std::string::npos && l.find("cable-") == std::string::npos) cable = d.name;
        if (l.find("cable-a input") != std::string::npos) cable_a = d.name;
        if (l.find("voicemeeter input") != std::string::npos) vm_in = d.name;
        if (l.find("voicemeeter aux input") != std::string::npos) vm_aux = d.name;
    }
    if (cable.empty() && vm_in.empty()) {
        if (MessageBoxA(g_main, "No virtual audio cable is installed. Teams can only use Windows audio devices, so a virtual cable is needed as the\n"
                                "\"microphone\" that carries the incoming AES67 stream (Windows cannot create one without a driver).\n\n"
                                "Free: VB-CABLE (one cable: isolated incoming, outgoing = loopback of your speakers).\n"
                                "Isolated in both directions: VB-CABLE plus VB-CABLE A+B, or Voicemeeter Banana (two virtual cables).\n\n"
                                "Open the VB-Audio download page now? (install, reboot once, then press Teams preset again)",
                        kTitle, MB_YESNO | MB_ICONQUESTION) == IDYES)
            ShellExecuteA(g_main, "open", "https://vb-audio.com/Cable/", nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    std::string e;
    if (!ui_to_cfg(e)) { MessageBoxA(g_main, e.c_str(), kTitle, MB_OK | MB_ICONWARNING); return; }
    std::string mic, spk, extra; bool isolated = true;
    if (!cable.empty()) {
        g_cfg.bridge_play = cable; mic = "CABLE Output (VB-Audio Virtual Cable)";
        if (!cable_a.empty()) { g_cfg.bridge_source = cable_a; spk = cable_a; }
        else if (!vm_aux.empty()) { g_cfg.bridge_source = vm_aux; spk = vm_aux; extra = "Voicemeeter must be running for its virtual devices to work.\n"; }
        else { g_cfg.bridge_source = "default"; spk = "your normal speakers / headset (the default playback device)"; isolated = false; }
    } else {
        g_cfg.bridge_play = vm_in; mic = "Voicemeeter Output (VB-Audio Voicemeeter VAIO)";
        extra = "Voicemeeter must be running. On its \"Voicemeeter VAIO\" strip enable B1 so the incoming audio reaches \"Voicemeeter Output\".\n";
        if (!vm_aux.empty()) { g_cfg.bridge_source = vm_aux; spk = vm_aux; }
        else { g_cfg.bridge_source = "default"; spk = "your normal speakers / headset (the default playback device)"; isolated = false; }
    }
    g_cfg.bridge_loopback = true;
    g_cfg.bridge_autostart = true;
    g_cfg.save();
    load_to_ui();
    bridge_start(true);
    std::string msg = "Bridge started for Teams.\n\nIn Teams > Settings > Devices choose:\n"
                      "   Microphone:  " + mic + "\n      = the stream received from the AES67 network, so the far end hears it\n"
                      "   Speaker:     " + spk + "\n      = what Teams plays; it is transmitted to the AES67 network\n\n" + extra;
    if (isolated) msg += "Both directions are isolated: only the AES67 stream reaches Teams' microphone and only Teams' audio is transmitted.\n";
    else msg += "Note: with a single cable the outgoing stream is the loopback of your speakers, so everything the PC plays is sent.\n"
                "For an isolated outgoing stream add a second virtual cable (VB-CABLE A+B or Voicemeeter Banana) and press Teams preset again.\n";
    msg += "Tips: turn Teams noise suppression off (or Low) and enable High fidelity music mode for programme audio.\n"
           "Set the virtual cable endpoints to 2 channels / 48000 Hz in Windows Sound settings. The same settings work for Zoom and Meet.";
    MessageBoxA(g_main, msg.c_str(), kTitle, MB_OK | MB_ICONINFORMATION);
}

// ---------------------------------------------------------------- tray
static void tray_set(int state, const std::string& tip) {
    static const int icons[4] = {IDI_TRAY_GREY, IDI_TRAY_GREEN, IDI_TRAY_YELLOW, IDI_TRAY_RED};
    if (state == g_tray_state && tip == g_nid.szTip) return;
    g_tray_state = state;
    g_nid.hIcon = (HICON)LoadImageA(g_inst, MAKEINTRESOURCEA(icons[state]), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    snprintf(g_nid.szTip, sizeof g_nid.szTip, "%s", tip.substr(0, 126).c_str());
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    Shell_NotifyIconA(NIM_MODIFY, &g_nid);
}
static void tray_add(HWND w) {
    g_nid.cbSize = sizeof g_nid; g_nid.hWnd = w; g_nid.uID = 1; g_nid.uCallbackMessage = WM_TRAY;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.hIcon = (HICON)LoadImageA(g_inst, MAKEINTRESOURCEA(IDI_TRAY_GREY), IMAGE_ICON, 16, 16, 0);
    snprintf(g_nid.szTip, sizeof g_nid.szTip, "%s", kTitle);
    Shell_NotifyIconA(NIM_ADD, &g_nid);
    g_nid.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconA(NIM_SETVERSION, &g_nid);
}
static void tray_remove() { Shell_NotifyIconA(NIM_DELETE, &g_nid); }
static void tray_balloon(const char* text, bool warning) {
    g_nid.uFlags = NIF_INFO; g_nid.dwInfoFlags = warning ? NIIF_WARNING : NIIF_INFO;
    snprintf(g_nid.szInfoTitle, sizeof g_nid.szInfoTitle, "%s", kTitle);
    snprintf(g_nid.szInfo, sizeof g_nid.szInfo, "%s", text);
    Shell_NotifyIconA(NIM_MODIFY, &g_nid);
}

// ---------------------------------------------------------------- status refresh
static void refresh_discovered() {
    auto e = g_sap.entries();
    g_found.clear();
    std::vector<std::string> names;
    for (auto& x : e) {
        if (!x.parsed) continue;
        g_found.push_back(x);
        names.push_back(x.desc.name + "  [" + x.desc.dest + ":" + std::to_string(x.desc.port) + " " + encoding_name(x.desc.encoding) + "/" + std::to_string(x.desc.sample_rate) + "/" + std::to_string(x.desc.channels) + "]");
    }
    HWND h = ctl(IDC_RX_DISCOVERED);
    int cur = (int)SendMessageA(h, CB_GETCURSEL, 0, 0), count = (int)SendMessageA(h, CB_GETCOUNT, 0, 0);
    if (count == (int)names.size()) {
        bool same = true;
        for (int i = 0; i < count && same; i++) { char b[512] = {0}; SendMessageA(h, CB_GETLBTEXT, i, (LPARAM)b); same = names[i] == b; }
        if (same) return;
    }
    combo_fill(IDC_RX_DISCOVERED, names, names.empty() ? -1 : std::max(0, std::min(cur, (int)names.size() - 1)));
    EnableWindow(ctl(IDC_RX_USE), !names.empty());
}

static void refresh_status() {
    StatusBlock drv; bool drv_on = driver_status(drv);
    bool br_on = g_bridge.running();
    BridgeStats bs = g_bridge.stats();
    StatusBlock eng; bool have = false;
    if (br_on) { eng = g_bridge.engine_status(); have = true; } else if (drv_on) { eng = drv; have = true; }
    char b[512];

    // Driver
    if (drv_on) { snprintf(b, sizeof b, "running inside an ASIO application (process %u)", drv.pid); set_text(IDC_VAL_DRV, b); set_dot(IDC_DOT_DRV, C_GREEN); }
    else { set_text(IDC_VAL_DRV, "idle - becomes active when an application opens the \"AES67 Virtual Sound Card\" ASIO device"); set_dot(IDC_DOT_DRV, C_GREY); }
    // Bridge
    if (br_on) {
        std::string t = "running - incoming plays on " + (bs.play_name.empty() ? std::string("(nothing)") : "\"" + bs.play_name + "\"") + ", outgoing from " + (bs.source_name.empty() ? std::string("(silence)") : "\"" + bs.source_name + "\"") + (bs.loopback ? " (loopback)" : "");
        if (!bs.error.empty()) t += "\nERROR: " + bs.error;
        set_text(IDC_VAL_BR, t); set_dot(IDC_DOT_BR, bs.error.empty() ? C_GREEN : C_RED);
    } else {
        set_text(IDC_VAL_BR, g_bridge_error.empty() ? "stopped - start it to use the streams with Teams, browsers and other Windows applications" : "stopped - last error: " + g_bridge_error);
        set_dot(IDC_DOT_BR, g_bridge_error.empty() ? C_GREY : C_RED);
    }
    // Clock / RX / TX
    bool clock_ok = false, rx_ok = false, rx_wanted = g_cfg.rx_enabled && !g_cfg.rx.dest.empty();
    if (have) {
        clock_ok = eng.ptp_state == 2 || eng.ptp_state == 3 || eng.ptp_state == 0;
        switch (eng.ptp_state) {
        case 2: snprintf(b, sizeof b, "locked to %s   offset %+.1f us, jitter %.0f us", eng.ptp_master, eng.ptp_offset_ns / 1000.0, eng.ptp_jitter_ns / 1000.0); set_dot(IDC_DOT_CLK, C_GREEN); break;
        case 3: snprintf(b, sizeof b, "this PC is the PTP master (%s) - other devices follow it", eng.ptp_master); set_dot(IDC_DOT_CLK, C_GREEN); break;
        case 4: snprintf(b, sizeof b, "free-running - no PTP master found on the network"); set_dot(IDC_DOT_CLK, C_YELLOW); break;
        case 1: snprintf(b, sizeof b, "searching / not yet locked   (jitter %.0f us)", eng.ptp_jitter_ns / 1000.0); set_dot(IDC_DOT_CLK, C_YELLOW); break;
        default: snprintf(b, sizeof b, "PTP off - timestamps from the PC clock"); set_dot(IDC_DOT_CLK, C_GREY); break;
        }
        set_text(IDC_VAL_CLK, b);
        if (!rx_wanted) { set_text(IDC_VAL_RX, "no incoming stream configured (Streams tab)"); set_dot(IDC_DOT_RX, C_GREY); rx_ok = true; }
        else if (eng.rx_active) { snprintf(b, sizeof b, "receiving %s:%u   %s packets, %u lost, %u resyncs", g_cfg.rx.dest.c_str(), g_cfg.rx.port, human(eng.rx_packets).c_str(), eng.rx_lost, eng.rx_resyncs); set_text(IDC_VAL_RX, b); set_dot(IDC_DOT_RX, C_GREEN); rx_ok = true; }
        else { snprintf(b, sizeof b, "NO DATA from %s:%u - check the sender, the network interface and the IGMP/multicast setup", g_cfg.rx.dest.c_str(), g_cfg.rx.port); set_text(IDC_VAL_RX, b); set_dot(IDC_DOT_RX, C_YELLOW); }
        if (!g_cfg.tx_enabled) { set_text(IDC_VAL_TX, "transmit disabled (Streams tab)"); set_dot(IDC_DOT_TX, C_GREY); }
        else { snprintf(b, sizeof b, "sending to %s:%u   %s packets, %u underruns", g_cfg.tx_dest.c_str(), g_cfg.tx_port, human(eng.tx_packets).c_str(), eng.tx_underruns); set_text(IDC_VAL_TX, b); set_dot(IDC_DOT_TX, C_GREEN); }
        snprintf(b, sizeof b, "Engine: %s, %u Hz, %u-frame buffer, %u periods (%u late)%s%s", eng.local_ip, eng.sample_rate, eng.buffer_frames, eng.periods, eng.late_periods, eng.message[0] ? "\nMessage: " : "", eng.message);
        set_text(IDC_COUNTERS, b);
    } else {
        set_text(IDC_VAL_CLK, "-"); set_dot(IDC_DOT_CLK, C_GREY);
        set_text(IDC_VAL_RX, rx_wanted ? ("configured: " + g_cfg.rx.dest + ":" + std::to_string(g_cfg.rx.port) + " (not running)") : "no incoming stream configured (Streams tab)"); set_dot(IDC_DOT_RX, C_GREY);
        set_text(IDC_VAL_TX, g_cfg.tx_enabled ? ("configured: " + g_cfg.tx_dest + ":" + std::to_string(g_cfg.tx_port) + " (not running)") : "transmit disabled (Streams tab)"); set_dot(IDC_DOT_TX, C_GREY);
        set_text(IDC_COUNTERS, "");
    }
    // Headline + tray
    int state; std::string head, sub, tip;
    bool bad = (br_on && !bs.error.empty()) || (!br_on && !g_bridge_error.empty());
    if (bad) { state = 3; head = "Error"; sub = bs.error.empty() ? g_bridge_error : bs.error; }
    else if (!have) { state = 0; head = "Idle"; sub = "Nothing is streaming. Start the bridge, or open the ASIO device in an audio application."; }
    else if (drv_on && br_on) { state = 2; head = "Attention: driver and bridge both running"; sub = "Both transmit to the same stream. Stop the bridge or close the ASIO application."; }
    else if (clock_ok && rx_ok) { state = 1; head = br_on ? "Streaming via the bridge" : "Streaming via the ASIO driver"; sub = "Clock and streams are healthy."; }
    else { state = 2; head = br_on ? "Streaming via the bridge, with warnings" : "Streaming via the ASIO driver, with warnings"; sub = !clock_ok ? "The PTP clock is not locked; audio continues but may drift against other devices." : "The incoming stream is not arriving."; }
    set_text(IDC_HEAD, head); set_text(IDC_SUB, sub);
    set_dot(IDC_DOT_MAIN, state == 0 ? C_GREY : state == 1 ? C_GREEN : state == 2 ? C_YELLOW : C_RED);
    tip = "AES67 VSC - " + head;
    tray_set(state, tip);
    if (state != g_last_notified_state) {
        if (state == 3) tray_balloon(sub.c_str(), true);
        else if (state == 2 && g_last_notified_state == 1 && !IsWindowVisible(g_main)) tray_balloon(sub.c_str(), true);
        g_last_notified_state = state;
    }
    // Buttons and bridge page
    set_text(IDC_ST_BRIDGE, br_on ? "Stop bridge" : "Start bridge");
    set_text(IDC_BR_START, br_on ? "Stop bridge" : "Start bridge");
    if (br_on) { snprintf(b, sizeof b, "buffers: play %zu / source %zu   underruns %u / %u", bs.play_fifo, bs.cap_fifo, bs.play_under, bs.cap_under); set_text(IDC_BR_STATUS, bs.error.empty() ? b : "ERROR - see Status tab"); }
    else set_text(IDC_BR_STATUS, "");
    // Bridge hint line
    {
        bool any_cable = false;
        for (auto& d : g_devs) if (d.render && !virtual_mic_for(d.name).empty()) any_cable = true;
        std::string hint;
        if (!any_cable) hint = "No virtual cable found: install the free VB-CABLE (button below), reboot, and \"CABLE Input\" appears in the lists.";
        else {
            std::string play = strip_annotation(get_text(IDC_BR_PLAY)), src = strip_annotation(get_text(IDC_BR_SOURCE));
            std::string mic = virtual_mic_for(play);
            hint = "In the application choose microphone = " + (mic.empty() ? (play.rfind("Default", 0) == 0 ? std::string("(incoming goes to your speakers, not to a microphone)") : "\"" + play + "\"") : "\"" + mic + "\"");
            if (src.rfind("Loopback: ", 0) == 0) { std::string dev = src.substr(10); hint += ", speaker = " + (dev.rfind("default", 0) == 0 ? std::string("your normal speakers (everything the PC plays is sent)") : "\"" + dev + "\""); }
            else hint += "; outgoing comes from the recording device \"" + src + "\"";
        }
        set_text(IDC_BR_HINT, hint);
    }
}

// ---------------------------------------------------------------- window plumbing
static void show_window() { ShowWindow(g_main, SW_SHOW); ShowWindow(g_main, SW_RESTORE); SetForegroundWindow(g_main); }
static void hide_window() {
    ShowWindow(g_main, SW_HIDE);
    if (!g_balloon_shown) { g_balloon_shown = true; tray_balloon("Still running here. Double-click the icon to open, right-click for the menu.", false); }
}
static void select_page(int i) {
    g_page = i;
    for (int k = 0; k < PAGE_COUNT; k++) ShowWindow(g_pages[k], k == i ? SW_SHOW : SW_HIDE);
    for (int k = 0; k < PAGE_COUNT; k++) InvalidateRect(ctl(IDC_NAV0 + k), nullptr, TRUE);
    if (i == PAGE_STREAMS) refresh_discovered();
}
static void tray_menu() {
    HMENU m = CreatePopupMenu();
    AppendMenuA(m, MF_STRING | MF_GRAYED, IDM_STATE, get_text(IDC_HEAD).c_str());
    AppendMenuA(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(m, MF_STRING, IDM_SHOW, "Open");
    AppendMenuA(m, MF_STRING, IDM_BRIDGE, g_bridge.running() ? "Stop bridge" : "Start bridge");
    AppendMenuA(m, MF_STRING, IDM_TEAMS, "Teams preset...");
    AppendMenuA(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(m, MF_STRING, IDM_EXIT, "Exit (stops the bridge)");
    SetMenuDefaultItem(m, IDM_SHOW, FALSE);
    POINT p; GetCursorPos(&p);
    SetForegroundWindow(g_main);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, g_main, nullptr);
    PostMessageA(g_main, WM_NULL, 0, 0);
    DestroyMenu(m);
}
static void do_exit() { bridge_stop(); tray_remove(); g_sap.stop(); DestroyWindow(g_main); PostQuitMessage(0); }

// ---------------------------------------------------------------- owner drawing
static COLORREF shade(COLORREF c, double f) {
    auto ch = [&](int v) { int r = (int)(v * f); return (BYTE)std::max(0, std::min(255, r)); };
    return RGB(ch(GetRValue(c)), ch(GetGValue(c)), ch(GetBValue(c)));
}
static void fill_round(HDC dc, RECT r, COLORREF fill, COLORREF border, int radius) {
    HBRUSH b = CreateSolidBrush(fill); HPEN p = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b); DeleteObject(p);
}
static void draw_dot(const DRAWITEMSTRUCT* d) {
    COLORREF c = g_dot_color.count((int)d->CtlID) ? g_dot_color[(int)d->CtlID] : C_GREY;
    RECT r = d->rcItem;
    FillRect(d->hDC, &r, g_br_panel);
    int w = r.right - r.left, h = r.bottom - r.top, s = std::min(w, h);
    int x = r.left + (w - s) / 2, y = r.top + (h - s) / 2;
    HBRUSH br = CreateSolidBrush(c); HPEN pen = CreatePen(PS_SOLID, 1, shade(c, 0.75));
    HGDIOBJ ob = SelectObject(d->hDC, br), op = SelectObject(d->hDC, pen);
    Ellipse(d->hDC, x, y, x + s, y + s);
    SelectObject(d->hDC, ob); SelectObject(d->hDC, op); DeleteObject(br); DeleteObject(pen);
}
static void draw_line(const DRAWITEMSTRUCT* d) {
    HBRUSH b = CreateSolidBrush(g_t.line); FillRect(d->hDC, &d->rcItem, b); DeleteObject(b);
}
static void draw_button(const DRAWITEMSTRUCT* d) {
    int id = (int)d->CtlID;
    char text[256] = {0}; GetWindowTextA(d->hwndItem, text, sizeof text);
    RECT r = d->rcItem;
    bool nav = id >= IDC_NAV0 && id <= IDC_NAV3;
    bool pressed = (d->itemState & ODS_SELECTED) != 0, disabled = (d->itemState & ODS_DISABLED) != 0, focus = (d->itemState & ODS_FOCUS) != 0;
    HBRUSH bg = nav ? g_br_side : (GetParent(d->hwndItem) == g_main ? g_br_panel : g_br_panel);
    FillRect(d->hDC, &r, bg);
    SetBkMode(d->hDC, TRANSPARENT);
    if (nav) {
        bool active = (id - IDC_NAV0) == g_page;
        if (active) {
            fill_round(d->hDC, r, g_t.nav_active, g_t.nav_active, 6);
            RECT bar = {r.left + 2, r.top + 4, r.left + 5, r.bottom - 4};
            HBRUSH ab = CreateSolidBrush(g_t.accent); FillRect(d->hDC, &bar, ab); DeleteObject(ab);
        } else if (pressed) fill_round(d->hDC, r, shade(g_t.side, g_t.dark ? 1.3 : 0.94), g_t.side, 6);
        SetTextColor(d->hDC, disabled ? g_t.subtext : g_t.text);
        HGDIOBJ of = SelectObject(d->hDC, active ? g_font_nav_bold : (HFONT)SendMessageA(d->hwndItem, WM_GETFONT, 0, 0));
        RECT tr = r; tr.left += 14;
        DrawTextA(d->hDC, text, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        SelectObject(d->hDC, of);
        return;
    }
    if (is_checkbox(id)) {
        int h = r.bottom - r.top, box = std::min(h, 15);
        RECT b = {r.left, r.top + (h - box) / 2, r.left + box, r.top + (h - box) / 2 + box};
        bool on = g_checks[id];
        fill_round(d->hDC, b, on ? g_t.accent : g_t.input_bg, on ? g_t.accent : g_t.btn_border, 3);
        if (on) {
            HPEN p = CreatePen(PS_SOLID, 2, g_t.accent_text); HGDIOBJ op = SelectObject(d->hDC, p);
            int x = b.left, y = b.top, s = box;
            MoveToEx(d->hDC, x + s * 22 / 100, y + s * 52 / 100, nullptr); LineTo(d->hDC, x + s * 42 / 100, y + s * 72 / 100); LineTo(d->hDC, x + s * 78 / 100, y + s * 30 / 100);
            SelectObject(d->hDC, op); DeleteObject(p);
        }
        SetTextColor(d->hDC, disabled ? g_t.subtext : g_t.text);
        RECT tr = r; tr.left = b.right + 6;
        DrawTextA(d->hDC, text, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        if (focus) { RECT fr = tr; fr.right = fr.left + 4 + (int)strlen(text) * 6; }
        return;
    }
    bool primary = id == IDC_APPLY || id == IDC_ST_BRIDGE || id == IDC_BR_START || id == IDC_RX_LOAD;
    COLORREF fill = primary ? g_t.accent : g_t.btn, border = primary ? g_t.accent : g_t.btn_border, txt = primary ? g_t.accent_text : g_t.text;
    if (disabled) { fill = g_t.dark ? RGB(45, 45, 50) : RGB(235, 236, 238); border = g_t.btn_border; txt = g_t.subtext; }
    else if (pressed) { fill = shade(fill, 0.85); }
    fill_round(d->hDC, r, fill, border, 6);
    SetTextColor(d->hDC, txt);
    DrawTextA(d->hDC, text, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    if (focus && !disabled) { RECT fr = r; InflateRect(&fr, -3, -3); DrawFocusRect(d->hDC, &fr); }
}
static void draw_item(const DRAWITEMSTRUCT* d) {
    int id = (int)d->CtlID;
    if (d->CtlType == ODT_BUTTON) draw_button(d);
    else if (id >= IDC_LINE_BASE && id < IDC_LINE_BASE + 20) draw_line(d);
    else draw_dot(d);
}
// Colour handling shared by the main window and the pages.
static INT_PTR ctl_color(HWND owner, UINT msg, HDC dc, HWND h) {
    bool side = owner == g_main;
    HBRUSH bg = side ? g_br_side : g_br_panel;
    int id = GetDlgCtrlID(h);
    switch (msg) {
    case WM_CTLCOLORDLG: return (INT_PTR)bg;
    case WM_CTLCOLORSTATIC: {
        bool sub = id == IDC_SUB || id == IDC_COUNTERS || id == IDC_ST_HINT || id == IDC_BR_HINT || id == IDC_BR_INTRO || id == IDC_BR_HOWTO || id == IDC_APPSUB || id == IDC_VERSION || id == IDC_FOOT || id == IDC_BR_STATUS || id == IDC_RX_INFO;
        char cls[16] = {0}; GetClassNameA(h, cls, sizeof cls);
        if (std::string(cls) == "Edit") { SetTextColor(dc, g_t.subtext); SetBkColor(dc, g_t.input_bg); return (INT_PTR)g_br_input; }  // read-only edits
        SetTextColor(dc, sub ? g_t.subtext : g_t.text);
        if (id == IDC_APPTITLE) SetTextColor(dc, g_t.accent);
        if (id == IDC_FOOT && g_dirty) SetTextColor(dc, g_t.dark ? RGB(255, 200, 90) : RGB(176, 110, 0));
        SetBkColor(dc, side ? g_t.side : g_t.panel);
        return (INT_PTR)bg;
    }
    case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: SetTextColor(dc, g_t.input_text); SetBkColor(dc, g_t.input_bg); return (INT_PTR)g_br_input;
    case WM_CTLCOLORBTN: return (INT_PTR)bg;
    }
    return 0;
}

// Shared command handling for all pages and the main window.
static bool on_command(int id, int code) {
    if (is_checkbox(id) && code == BN_CLICKED) { g_checks[id] = !g_checks[id]; InvalidateRect(ctl(id), nullptr, TRUE); set_dirty(true); if (id == IDC_RX_ENABLE || id == IDC_TX_ENABLE) refresh_status(); return true; }
    if (id >= IDC_NAV0 && id <= IDC_NAV3 && code == BN_CLICKED) { select_page(id - IDC_NAV0); return true; }
    switch (id) {
    case IDC_RX_LOAD: do_load_sdp(); return true;
    case IDC_RX_USE: { int i = combo_sel(IDC_RX_DISCOVERED); if (i >= 0 && i < (int)g_found.size()) import_sdp_text(g_found[i].sdp, "Selected stream announced by " + g_found[i].origin_ip); return true; }
    case IDC_RX_CLEAR: g_cfg.rx = StreamDesc(); g_cfg.rx.dest.clear(); g_cfg.rx_sdp_text.clear(); g_cfg.rx_enabled = false; load_to_ui(); set_dirty(true); return true;
    case IDC_TX_EXPORT: do_export_sdp(); return true;
    case IDC_TX_COPY: do_copy_sdp(); return true;
    case IDC_OPENCFG: ShellExecuteA(g_main, "open", config_dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); return true;
    case IDC_CHECKDRV: {
        std::string cmd = "/k \"\"" + exe_dir() + "\\aes67vsc.exe\" check\"";
        ShellExecuteA(g_main, "open", "cmd.exe", cmd.c_str(), exe_dir().c_str(), SW_SHOWNORMAL);
        return true;
    }
    case IDC_ABOUT: {
        std::string m = std::string("AES67 Virtual Sound Card ") + AES67VSC_VERSION + "\n\nFree, MIT-licensed 2x2 (or 1x1) AES67 virtual sound card for Windows:\n"
                        "ASIO driver, PTP clock, SAP discovery, SDP import/export, and a bridge to Windows audio devices.\n\n"
                        "Project page and documentation: https://github.com/John-Butler3/aes67-vsc\n\nOpen the project page now?";
        if (MessageBoxA(g_main, m.c_str(), kTitle, MB_YESNO | MB_ICONINFORMATION) == IDYES) ShellExecuteA(g_main, "open", "https://github.com/John-Butler3/aes67-vsc", nullptr, nullptr, SW_SHOWNORMAL);
        return true;
    }
    case IDC_BR_GETCABLE:
        MessageBoxA(g_main, "VB-CABLE is a free, signed virtual audio cable by VB-Audio.\n\n1. Download and unzip it, right-click VBCABLE_Setup_x64.exe > Run as administrator, press Install Driver.\n"
                            "2. Reboot.\n3. Back here, choose \"CABLE Input\" for the incoming stream (or press Teams preset).\n\n"
                            "For a second, independent cable (so only the application's audio goes out) add VB-CABLE A+B or Voicemeeter from the same site.", kTitle, MB_OK | MB_ICONINFORMATION);
        ShellExecuteA(g_main, "open", "https://vb-audio.com/Cable/", nullptr, nullptr, SW_SHOWNORMAL);
        return true;
    case IDC_RATE:
        if (code == CBN_SELCHANGE) { int rs = combo_sel(IDC_RATE); int rate = rs == 0 ? 44100 : rs == 2 ? 96000 : 48000; fill_buffer_combo(rate, atoi(get_text(IDC_BUFFER).c_str())); set_dirty(true); }
        return true;
    case IDC_BR_START: case IDC_ST_BRIDGE: case IDM_BRIDGE: bridge_toggle(); refresh_status(); return true;
    case IDC_BR_TEAMS: case IDC_ST_TEAMS: case IDM_TEAMS: show_window(); teams_preset(); refresh_status(); return true;
    case IDC_APPLY: apply(); refresh_status(); return true;
    case IDM_SHOW: show_window(); return true;
    case IDCANCEL: hide_window(); return true;
    case IDC_EXIT: case IDM_EXIT:
        if (g_bridge.running() && MessageBoxA(g_main, "The bridge is running. Exit and stop it?", kTitle, MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return true;
        if (g_dirty && MessageBoxA(g_main, "There are unsaved changes. Exit without applying them?", kTitle, MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return true;
        do_exit(); return true;
    }
    // Anything else that edits a setting marks the configuration dirty.
    if (code == EN_CHANGE || code == CBN_SELCHANGE || code == CBN_EDITCHANGE || code == BN_CLICKED) {
        switch (id) {
        case IDC_IFACE: case IDC_BUFFER: case IDC_LINKOFF: case IDC_PTPMODE: case IDC_PTPDOMAIN: case IDC_CHANNELS: case IDC_AUTORUN: case IDC_THEME:
        case IDC_RX_ENABLE: case IDC_RX_CHOFF: case IDC_TX_ENABLE: case IDC_TX_NAME: case IDC_TX_DEST: case IDC_TX_PORT: case IDC_TX_ENC: case IDC_TX_SAP:
        case IDC_BR_PLAY: case IDC_BR_SOURCE: case IDC_BR_AUTO:
            set_dirty(true);
            if (id == IDC_BR_PLAY || id == IDC_BR_SOURCE) refresh_status();
            if (id == IDC_THEME) { int t = combo_sel(IDC_THEME); g_cfg.theme = t == 1 ? "light" : t == 2 ? "dark" : "auto"; apply_theme(); }
            return true;
        }
    }
    return false;
}

static INT_PTR CALLBACK PageProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: return TRUE;
    case WM_COMMAND: return on_command(LOWORD(wp), HIWORD(wp)) ? TRUE : FALSE;
    case WM_DRAWITEM: draw_item((const DRAWITEMSTRUCT*)lp); return TRUE;
    case WM_CTLCOLORDLG: case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN:
        return ctl_color(dlg, msg, (HDC)wp, (HWND)lp);
    case WM_ERASEBKGND: { RECT r; GetClientRect(dlg, &r); FillRect((HDC)wp, &r, g_br_panel); return TRUE; }
    }
    return FALSE;
}

static INT_PTR CALLBACK MainProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_SHOWME) { show_window(); return TRUE; }
    switch (msg) {
    case WM_INITDIALOG: {
        g_main = dlg;
        HICON ic = LoadIconA(g_inst, MAKEINTRESOURCEA(IDI_APP));
        if (ic) { SendMessageA(dlg, WM_SETICON, ICON_BIG, (LPARAM)ic); SendMessageA(dlg, WM_SETICON, ICON_SMALL, (LPARAM)ic); }
        // Geometry: sidebar 100 dlu wide, pages at (104, 8) sized 448 x 340 dlu.
        RECT side = {0, 0, 100, 8}; MapDialogRect(dlg, &side); g_sidebar_px = side.right;
        RECT pr = {104, 8, 104 + 448, 8 + 340}; MapDialogRect(dlg, &pr);
        const int ids[PAGE_COUNT] = {IDD_PAGE_STATUS, IDD_PAGE_STREAMS, IDD_PAGE_BRIDGE, IDD_PAGE_SETTINGS};
        for (int i = 0; i < PAGE_COUNT; i++) {
            g_pages[i] = CreateDialogParamA(g_inst, MAKEINTRESOURCEA(ids[i]), dlg, PageProc, 0);
            SetWindowPos(g_pages[i], HWND_TOP, pr.left, pr.top, pr.right - pr.left, pr.bottom - pr.top, SWP_NOACTIVATE);
        }
        // Fonts
        LOGFONTA lf{}; GetObjectA((HFONT)SendMessageA(dlg, WM_GETFONT, 0, 0), sizeof lf, &lf);
        LOGFONTA f = lf; f.lfWeight = FW_SEMIBOLD; f.lfHeight = (LONG)(lf.lfHeight * 1.5); g_font_head = CreateFontIndirectA(&f);
        f = lf; f.lfWeight = FW_SEMIBOLD; f.lfHeight = (LONG)(lf.lfHeight * 1.15); g_font_section = CreateFontIndirectA(&f);
        f = lf; f.lfWeight = FW_BOLD; f.lfHeight = (LONG)(lf.lfHeight * 1.35); g_font_title = CreateFontIndirectA(&f);
        f = lf; f.lfWeight = FW_SEMIBOLD; g_font_nav_bold = CreateFontIndirectA(&f);
        SendMessageA(ctl(IDC_HEAD), WM_SETFONT, (WPARAM)g_font_head, TRUE);
        SendMessageA(ctl(IDC_APPTITLE), WM_SETFONT, (WPARAM)g_font_title, TRUE);
        for (int i = 0; i < 20; i++) { HWND h = ctl(IDC_SEC_BASE + i); if (h) SendMessageA(h, WM_SETFONT, (WPARAM)g_font_section, TRUE); }
        set_text(IDC_VERSION, std::string("v") + AES67VSC_VERSION);
        set_text(IDC_BR_INTRO, "Connects the AES67 streams to ordinary Windows audio devices, so applications that cannot use ASIO (Teams, Zoom, browsers, media players) "
                               "can use them. Only devices that exist in Windows can be chosen; a virtual cable such as the free VB-CABLE provides the \"microphone\" and \"speaker\" those applications see.");
        set_text(IDC_BR_HOWTO, "Teams preset configures the common case: incoming stream -> CABLE Input (applications record it as microphone \"CABLE Output\"); what the application plays -> transmitted.\r\n\r\n"
                               "With only the free VB-CABLE the outgoing side is the loopback of your speakers, so everything the PC plays is sent. For an application-only outgoing stream add a second cable "
                               "(VB-CABLE A+B or Voicemeeter Banana); the preset detects it.");
        g_ifaces = list_interfaces();
        load_to_ui();
        apply_theme();
        { std::string err; g_sap.start(pick_interface(g_cfg.interface_ip).ipv4, true, err); }
        tray_add(dlg);
        refresh_status();
        select_page(PAGE_STATUS);
        SetTimer(dlg, 1, 500, nullptr);
        DragAcceptFiles(dlg, TRUE);
        if (!g_pending_sdp.empty()) { import_sdp_text(g_pending_sdp, "Loaded SDP from the command line"); g_pending_sdp.clear(); }
        return TRUE;
    }
    case WM_ERASEBKGND: {
        RECT r; GetClientRect(dlg, &r);
        RECT s = r; s.right = g_sidebar_px; FillRect((HDC)wp, &s, g_br_side);
        RECT c = r; c.left = g_sidebar_px; FillRect((HDC)wp, &c, g_br_panel);
        return TRUE;
    }
    case WM_DRAWITEM: draw_item((const DRAWITEMSTRUCT*)lp); return TRUE;
    case WM_CTLCOLORDLG: case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN: {
        HWND h = (HWND)lp;
        // Footer and buttons sit on the content (panel) area, the rest of the main window is sidebar.
        int id = GetDlgCtrlID(h);
        bool content = id == IDC_FOOT || id == IDC_APPLY || id == IDCANCEL || id == IDC_EXIT;
        INT_PTR r = ctl_color(content ? g_pages[0] : dlg, msg, (HDC)wp, h);
        return r;
    }
    case WM_SETTINGCHANGE:
        if (lp && lstrcmpiA((const char*)lp, "ImmersiveColorSet") == 0 && g_cfg.theme == "auto") apply_theme();
        return TRUE;
    case WM_TIMER: {
        static int n = 0;
        if (g_bridge.restart_needed()) { g_cfg.load(); bridge_start(false); }
        refresh_status();
        if (++n % 4 == 0 && IsWindowVisible(dlg) && g_page == PAGE_STREAMS) refresh_discovered();
        return TRUE;
    }
    case WM_TRAY:
        switch (LOWORD(lp)) {
        case WM_LBUTTONDBLCLK: case NIN_KEYSELECT: show_window(); break;
        case WM_LBUTTONUP: if (!IsWindowVisible(dlg)) show_window(); break;
        case WM_RBUTTONUP: case WM_CONTEXTMENU: tray_menu(); break;
        }
        return TRUE;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_MINIMIZE) { hide_window(); return TRUE; }
        break;
    case WM_DROPFILES: {
        HDROP hd = (HDROP)wp; char file[MAX_PATH] = {0};
        if (DragQueryFileA(hd, 0, file, MAX_PATH)) { std::string text; if (read_file(file, text)) import_sdp_text(text, std::string("Loaded ") + file); }
        DragFinish(hd);
        return TRUE;
    }
    case WM_COMMAND: return on_command(LOWORD(wp), HIWORD(wp)) ? TRUE : FALSE;
    case WM_CLOSE: hide_window(); return TRUE;
    case WM_ENDSESSION: if (wp) do_exit(); return TRUE;
    }
    return FALSE;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR cmdline, int) {
    g_inst = inst;
    net_init();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
    InitCommonControlsEx(&icc);
    WM_SHOWME = RegisterWindowMessageA("AES67VSC_ShowPanel");
    HANDLE mutex = CreateMutexA(nullptr, TRUE, "Local\\AES67VSC_Panel");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowA(nullptr, kTitle);
        if (other) PostMessageA(other, WM_SHOWME, 0, 0);
        return 0;
    }
    g_cfg.load();
    log_set_file(config_dir() + "\\panel.log");
    bool start_hidden = false, start_bridge = g_cfg.bridge_autostart;
    std::string args = cmdline ? cmdline : "";
    if (args.find("--tray") != std::string::npos) start_hidden = true;
    if (args.find("--bridge") != std::string::npos) start_bridge = true;
    if (args.find(".sdp") != std::string::npos) {
        std::string f = args; size_t q1 = f.find('"'), q2 = f.rfind('"');
        if (q1 != std::string::npos && q2 > q1) f = f.substr(q1 + 1, q2 - q1 - 1);
        std::string text; if (read_file(f, text)) g_pending_sdp = text;
    }
    HWND dlg = CreateDialogParamA(inst, MAKEINTRESOURCEA(IDD_MAIN), nullptr, MainProc, 0);
    if (!dlg) return 1;
    if (start_bridge) { bridge_start(false); refresh_status(); }
    if (!start_hidden) show_window(); else g_balloon_shown = true;
    MSG m;
    while (GetMessageA(&m, nullptr, 0, 0) > 0) {
        bool handled = false;
        for (HWND p : g_pages) if (p && IsWindowVisible(p) && IsDialogMessageA(p, &m)) { handled = true; break; }
        if (!handled && IsWindow(dlg) && IsDialogMessageA(dlg, &m)) handled = true;
        if (!handled) { TranslateMessage(&m); DispatchMessageA(&m); }
    }
    ReleaseMutex(mutex); CloseHandle(mutex);
    CoUninitialize();
    return 0;
}
