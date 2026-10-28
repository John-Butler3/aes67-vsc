// AES67 Virtual Sound Card - tray application and control panel.
// One window for everything: device/clock settings, receive & transmit streams (SDP in/out),
// the Windows audio bridge (hosted in-process), and a live status view of what is running.
// Closing or minimising hides the window into the notification area; the tray icon colour
// reflects the overall state.
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
#include <cstdio>
#include <string>
#include <vector>
#include <cmath>

using namespace aes67;

static const char* kTitle = "AES67 Virtual Sound Card";
static const UINT WM_TRAY = WM_APP + 1;
static UINT WM_SHOWME = 0;

static HINSTANCE g_inst;
static HWND g_dlg;
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
static int g_tray_state = -1;   // 0 grey 1 green 2 yellow 3 red
static bool g_balloon_shown = false;

// ---------------------------------------------------------------- small helpers
static std::string get_text(HWND dlg, int id) { char buf[1024] = {0}; GetDlgItemTextA(dlg, id, buf, sizeof buf); return buf; }
static void set_text(HWND dlg, int id, const std::string& s) { if (get_text(dlg, id) != s) SetDlgItemTextA(dlg, id, s.c_str()); }
static void combo_fill(HWND dlg, int id, const std::vector<std::string>& items, int sel) {
    HWND h = GetDlgItem(dlg, id);
    SendMessageA(h, CB_RESETCONTENT, 0, 0);
    for (auto& s : items) SendMessageA(h, CB_ADDSTRING, 0, (LPARAM)s.c_str());
    SendMessageA(h, CB_SETCURSEL, (WPARAM)sel, 0);
}
static int combo_sel(HWND dlg, int id) { return (int)SendMessageA(GetDlgItem(dlg, id), CB_GETCURSEL, 0, 0); }
static std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
static std::string exe_path() { char p[MAX_PATH] = {0}; GetModuleFileNameA(nullptr, p, MAX_PATH); return p; }
static bool driver_status(StatusBlock& s) { return g_share.read(s) && s.engine_running && wall_ms() - s.updated_ms < 3000; }

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

// ---------------------------------------------------------------- bridge device combos
// Entry 0 = default, 1 = none, then devices. Source list adds loopback entries for playback devices.
static void fill_bridge_combos(HWND dlg) {
    g_devs = bridge_list_devices();
    std::vector<std::string> play{"Default playback device", "(do not play)"}, src{"Default recording device (microphone)", "(send silence)"};
    int psel = lower(g_cfg.bridge_play) == "none" ? 1 : 0, ssel = lower(g_cfg.bridge_source) == "none" ? 1 : 0;
    for (auto& d : g_devs) if (d.render) { play.push_back(d.name); if (lower(g_cfg.bridge_play) != "default" && lower(g_cfg.bridge_play) != "none" && lower(d.name).find(lower(g_cfg.bridge_play)) != std::string::npos && psel < 2) psel = (int)play.size() - 1; }
    for (auto& d : g_devs) if (!d.render) { src.push_back(d.name); if (!g_cfg.bridge_loopback && lower(g_cfg.bridge_source) != "default" && lower(g_cfg.bridge_source) != "none" && lower(d.name).find(lower(g_cfg.bridge_source)) != std::string::npos && ssel < 2) ssel = (int)src.size() - 1; }
    src.push_back("Loopback: default playback device (everything the PC plays)");
    if (g_cfg.bridge_loopback && lower(g_cfg.bridge_source) == "default") ssel = (int)src.size() - 1;
    for (auto& d : g_devs) if (d.render) { src.push_back("Loopback: " + d.name); if (g_cfg.bridge_loopback && lower(g_cfg.bridge_source) != "default" && lower(d.name).find(lower(g_cfg.bridge_source)) != std::string::npos) ssel = (int)src.size() - 1; }
    combo_fill(dlg, IDC_BR_PLAY, play, psel);
    combo_fill(dlg, IDC_BR_SOURCE, src, ssel);
}
static void read_bridge_combos(HWND dlg) {
    std::string p = get_text(dlg, IDC_BR_PLAY), s = get_text(dlg, IDC_BR_SOURCE);
    int pi = combo_sel(dlg, IDC_BR_PLAY), si = combo_sel(dlg, IDC_BR_SOURCE);
    g_cfg.bridge_play = pi == 0 ? "default" : pi == 1 ? "none" : p;
    if (si == 0) { g_cfg.bridge_source = "default"; g_cfg.bridge_loopback = false; }
    else if (si == 1) { g_cfg.bridge_source = "none"; g_cfg.bridge_loopback = false; }
    else if (s.rfind("Loopback: default", 0) == 0) { g_cfg.bridge_source = "default"; g_cfg.bridge_loopback = true; }
    else if (s.rfind("Loopback: ", 0) == 0) { g_cfg.bridge_source = s.substr(10); g_cfg.bridge_loopback = true; }
    else { g_cfg.bridge_source = s; g_cfg.bridge_loopback = false; }
    g_cfg.bridge_autostart = IsDlgButtonChecked(dlg, IDC_BR_AUTO) == BST_CHECKED;
}

// ---------------------------------------------------------------- UI <-> config
static std::string describe_rx(const Config& c) {
    if (c.rx.dest.empty()) return "No receive stream configured.\nLoad an SDP file from the sending device, or pick a discovered stream.";
    const StreamDesc& d = c.rx;
    char buf[512];
    snprintf(buf, sizeof buf, "%s\n%s:%u%s   %s / %d Hz / %d ch   pt %d, %g ms%s%s\n%s",
             d.name.empty() ? "(unnamed stream)" : d.name.c_str(), d.dest.c_str(), d.port, d.source.empty() ? "" : (" from " + d.source).c_str(),
             encoding_name(d.encoding), d.sample_rate, d.channels, d.payload_type, d.ptime_ms,
             d.ptp_domain >= 0 ? "  PTP domain " : "", d.ptp_domain >= 0 ? std::to_string(d.ptp_domain).c_str() : "",
             d.refclk.empty() ? "" : ("clock: " + d.refclk).c_str());
    return buf;
}
static void fill_buffer_combo(HWND dlg, int rate, int value) {
    Config tmp = g_cfg; tmp.sample_rate = rate;
    int fpp = tmp.tx_stream("0.0.0.0", "").frames_per_packet();
    std::vector<std::string> b;
    for (int m : {1, 2, 3, 4, 5, 6, 8, 10, 16, 20, 32, 40}) b.push_back(std::to_string(fpp * m));
    combo_fill(dlg, IDC_BUFFER, b, -1);
    set_text(dlg, IDC_BUFFER, std::to_string(value));
}
static void load_to_ui(HWND dlg) {
    std::vector<std::string> names{"Automatic"}; int sel = 0;
    for (size_t i = 0; i < g_ifaces.size(); i++) {
        names.push_back(g_ifaces[i].ipv4 + "  (" + g_ifaces[i].name + ")");
        if (g_cfg.interface_ip != "auto" && g_ifaces[i].ipv4 == g_cfg.interface_ip) sel = (int)i + 1;
    }
    combo_fill(dlg, IDC_IFACE, names, sel);
    combo_fill(dlg, IDC_RATE, {"44100", "48000", "96000"}, g_cfg.sample_rate == 44100 ? 0 : g_cfg.sample_rate == 96000 ? 2 : 1);
    fill_buffer_combo(dlg, g_cfg.sample_rate, g_cfg.buffer_frames);
    combo_fill(dlg, IDC_PTPMODE, {"Automatic", "Follow only", "Master", "Off"}, (int)g_cfg.ptp_mode);
    set_text(dlg, IDC_PTPDOMAIN, std::to_string(g_cfg.ptp_domain));
    char lo[32]; snprintf(lo, sizeof lo, "%g", g_cfg.link_offset_ms); set_text(dlg, IDC_LINKOFF, lo);
    CheckDlgButton(dlg, IDC_RX_ENABLE, g_cfg.rx_enabled && !g_cfg.rx.dest.empty() ? BST_CHECKED : BST_UNCHECKED);
    set_text(dlg, IDC_RX_INFO, describe_rx(g_cfg));
    set_text(dlg, IDC_RX_CHOFF, std::to_string(g_cfg.rx_channel_offset + 1));
    CheckDlgButton(dlg, IDC_TX_ENABLE, g_cfg.tx_enabled ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_TX_SAP, g_cfg.sap_announce ? BST_CHECKED : BST_UNCHECKED);
    set_text(dlg, IDC_TX_NAME, g_cfg.tx_name);
    set_text(dlg, IDC_TX_DEST, g_cfg.tx_dest);
    set_text(dlg, IDC_TX_PORT, std::to_string(g_cfg.tx_port));
    combo_fill(dlg, IDC_TX_ENC, {"L24", "L16"}, g_cfg.tx_encoding == Encoding::L16 ? 1 : 0);
    fill_bridge_combos(dlg);
    CheckDlgButton(dlg, IDC_BR_AUTO, g_cfg.bridge_autostart ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_AUTORUN, autorun_enabled() ? BST_CHECKED : BST_UNCHECKED);
}
static bool ui_to_cfg(HWND dlg, std::string& err) {
    Config c = g_cfg;
    int is = combo_sel(dlg, IDC_IFACE);
    c.interface_ip = (is <= 0 || is > (int)g_ifaces.size()) ? "auto" : g_ifaces[is - 1].ipv4;
    int rs = combo_sel(dlg, IDC_RATE);
    c.sample_rate = rs == 0 ? 44100 : rs == 2 ? 96000 : 48000;
    c.buffer_frames = atoi(get_text(dlg, IDC_BUFFER).c_str());
    c.ptp_mode = (PtpMode)std::max(0, combo_sel(dlg, IDC_PTPMODE));
    c.ptp_domain = atoi(get_text(dlg, IDC_PTPDOMAIN).c_str());
    c.link_offset_ms = atof(get_text(dlg, IDC_LINKOFF).c_str());
    c.rx_enabled = IsDlgButtonChecked(dlg, IDC_RX_ENABLE) == BST_CHECKED;
    c.rx_channel_offset = std::max(0, atoi(get_text(dlg, IDC_RX_CHOFF).c_str()) - 1);
    c.tx_enabled = IsDlgButtonChecked(dlg, IDC_TX_ENABLE) == BST_CHECKED;
    c.sap_announce = IsDlgButtonChecked(dlg, IDC_TX_SAP) == BST_CHECKED;
    c.tx_name = get_text(dlg, IDC_TX_NAME);
    c.tx_dest = get_text(dlg, IDC_TX_DEST);
    c.tx_port = (uint16_t)atoi(get_text(dlg, IDC_TX_PORT).c_str());
    c.tx_encoding = combo_sel(dlg, IDC_TX_ENC) == 1 ? Encoding::L16 : Encoding::L24;
    in_addr a;
    if (c.tx_enabled && inet_pton(AF_INET, c.tx_dest.c_str(), &a) != 1) { err = "Transmit multicast address is not a valid IPv4 address."; return false; }
    if (c.tx_port < 1024) { err = "Transmit port must be 1024 or higher."; return false; }
    if (c.buffer_frames < 16 || c.buffer_frames > 4096) { err = "Buffer size must be between 16 and 4096 frames."; return false; }
    if (c.link_offset_ms < 0.5 || c.link_offset_ms > 500) { err = "Link offset must be between 0.5 and 500 ms."; return false; }
    if (c.rx_enabled && !c.rx.dest.empty() && c.rx.sample_rate != c.sample_rate) {
        err = "The receive stream is " + std::to_string(c.rx.sample_rate) + " Hz but the device sample rate is " + std::to_string(c.sample_rate) + " Hz. Set them equal.";
        return false;
    }
    g_cfg = c;
    read_bridge_combos(dlg);
    std::string e2; Config n; n.from_ini(g_cfg.to_ini(), e2);
    g_cfg = n;
    return true;
}
static bool apply(HWND dlg) {
    std::string err;
    if (!ui_to_cfg(dlg, err)) { MessageBoxA(dlg, err.c_str(), kTitle, MB_OK | MB_ICONWARNING); return false; }
    if (!g_cfg.save()) { MessageBoxA(dlg, ("Cannot write " + Config::default_path()).c_str(), kTitle, MB_OK | MB_ICONERROR); return false; }
    set_autorun(IsDlgButtonChecked(dlg, IDC_AUTORUN) == BST_CHECKED);
    load_to_ui(dlg);
    std::string e; g_sap.stop(); g_sap.start(pick_interface(g_cfg.interface_ip).ipv4, true, e);
    return true;
}

// ---------------------------------------------------------------- SDP import / export
static bool import_sdp_text(HWND dlg, const std::string& text, const std::string& source_desc) {
    StreamDesc d; std::string err;
    if (!sdp_parse(text, d, err)) { MessageBoxA(dlg, ("Could not use this SDP:\n" + err).c_str(), kTitle, MB_OK | MB_ICONERROR); return false; }
    std::string notes;
    if (d.sample_rate != g_cfg.sample_rate) { notes += "The stream runs at " + std::to_string(d.sample_rate) + " Hz; the device sample rate was changed to match.\n"; g_cfg.sample_rate = d.sample_rate; }
    if (d.channels > 2) notes += "The stream has " + std::to_string(d.channels) + " channels; inputs 1-2 take stream channels " + std::to_string(g_cfg.rx_channel_offset + 1) + "-" + std::to_string(g_cfg.rx_channel_offset + 2) + " (adjustable).\n";
    if (d.ptp_domain >= 0 && d.ptp_domain != g_cfg.ptp_domain) { notes += "The stream uses PTP domain " + std::to_string(d.ptp_domain) + "; the PTP domain was changed to match.\n"; g_cfg.ptp_domain = d.ptp_domain; }
    g_cfg.rx = d; g_cfg.rx_sdp_text = text; g_cfg.rx_enabled = true;
    load_to_ui(dlg);
    if (!notes.empty()) MessageBoxA(dlg, (source_desc + "\n\n" + notes + "\nPress Apply to save.").c_str(), kTitle, MB_OK | MB_ICONINFORMATION);
    return true;
}
static void do_load_sdp(HWND dlg) {
    char file[MAX_PATH] = {0};
    OPENFILENAMEA ofn{}; ofn.lStructSize = sizeof ofn; ofn.hwndOwner = dlg;
    ofn.lpstrFilter = "SDP files (*.sdp)\0*.sdp\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH; ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY; ofn.lpstrTitle = "Load receive stream SDP";
    if (!GetOpenFileNameA(&ofn)) return;
    std::string text;
    if (!read_file(file, text)) { MessageBoxA(dlg, "Cannot read the file.", kTitle, MB_OK | MB_ICONERROR); return; }
    import_sdp_text(dlg, text, std::string("Loaded ") + file);
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
static void do_export_sdp(HWND dlg) {
    if (!apply(dlg)) return;
    char file[MAX_PATH] = {0};
    std::string def = g_cfg.tx_name; for (auto& ch : def) if (strchr("\\/:*?\"<>|", ch)) ch = '_';
    snprintf(file, sizeof file, "%s.sdp", def.c_str());
    OPENFILENAMEA ofn{}; ofn.lStructSize = sizeof ofn; ofn.hwndOwner = dlg;
    ofn.lpstrFilter = "SDP files (*.sdp)\0*.sdp\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH; ofn.Flags = OFN_OVERWRITEPROMPT; ofn.lpstrDefExt = "sdp"; ofn.lpstrTitle = "Save transmit stream SDP";
    if (!GetSaveFileNameA(&ofn)) return;
    if (!write_file(file, current_tx_sdp())) MessageBoxA(dlg, "Cannot write the file.", kTitle, MB_OK | MB_ICONERROR);
}
static void do_copy_sdp(HWND dlg) {
    if (!apply(dlg)) return;
    std::string s = current_tx_sdp();
    if (!OpenClipboard(dlg)) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, s.size() + 1);
    if (h) { memcpy(GlobalLock(h), s.c_str(), s.size() + 1); GlobalUnlock(h); SetClipboardData(CF_TEXT, h); }
    CloseClipboard();
}

// ---------------------------------------------------------------- bridge control
static void bridge_stop() { g_bridge.stop(); g_bridge_error.clear(); }
static bool bridge_start(HWND dlg) {
    bridge_stop();
    std::string err;
    if (!g_bridge.start(g_cfg, g_cfg.bridge_play, g_cfg.bridge_source, g_cfg.bridge_loopback, err)) {
        g_bridge_error = err;
        if (dlg && IsWindowVisible(dlg)) MessageBoxA(dlg, ("The bridge could not start:\n" + err).c_str(), kTitle, MB_OK | MB_ICONERROR);
        return false;
    }
    return true;
}
static void bridge_toggle(HWND dlg) {
    if (g_bridge.running()) { bridge_stop(); return; }
    if (!apply(dlg)) return;
    bridge_start(dlg);
}
static void teams_preset(HWND dlg) {
    g_devs = bridge_list_devices();
    std::string cable, cable_a;
    for (auto& d : g_devs) if (d.render) { if (lower(d.name).find("cable input") != std::string::npos && lower(d.name).find("cable-a") == std::string::npos) cable = d.name; if (lower(d.name).find("cable-a input") != std::string::npos) cable_a = d.name; }
    if (cable.empty()) {
        if (MessageBoxA(dlg, "The free VB-CABLE virtual cable is not installed. Teams can only use Windows audio devices, so it is needed as the\n"
                             "\"microphone\" that carries the AES67 audio.\n\nOpen the VB-CABLE download page now? (install it, reboot once, then press Teams preset again)",
                        kTitle, MB_YESNO | MB_ICONQUESTION) == IDYES)
            ShellExecuteA(dlg, "open", "https://vb-audio.com/Cable/", nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (!ui_to_cfg(dlg, g_bridge_error)) { MessageBoxA(dlg, g_bridge_error.c_str(), kTitle, MB_OK | MB_ICONWARNING); return; }
    g_cfg.bridge_play = cable;
    g_cfg.bridge_source = cable_a.empty() ? "default" : cable_a;
    g_cfg.bridge_loopback = true;
    g_cfg.bridge_autostart = true;
    g_cfg.save();
    load_to_ui(dlg);
    bridge_start(dlg);
    std::string spk = cable_a.empty() ? "your normal speakers / headset (the default playback device)" : cable_a;
    std::string msg = "Bridge started for Teams.\n\nIn Teams > Settings > Devices choose:\n"
                      "   Microphone:  CABLE Output (VB-Audio Virtual Cable)\n      = the stream received from the AES67 network, so the far end hears it\n"
                      "   Speaker:     " + spk + "\n      = what Teams plays; it is transmitted to the AES67 network\n\n"
                      "Tips: turn Teams noise suppression off (or Low) and enable High fidelity music mode for programme audio.\n"
                      "Set both VB-CABLE endpoints to 2 channels / 48000 Hz in Windows Sound settings.\n"
                      "The same settings work for Zoom, Meet and other apps.";
    MessageBoxA(dlg, msg.c_str(), kTitle, MB_OK | MB_ICONINFORMATION);
}

// ---------------------------------------------------------------- status + tray
static void tray_set(int state, const std::string& tip) {
    static const int icons[4] = {IDI_TRAY_GREY, IDI_TRAY_GREEN, IDI_TRAY_YELLOW, IDI_TRAY_RED};
    if (state == g_tray_state && tip == g_nid.szTip) return;
    g_tray_state = state;
    g_nid.hIcon = (HICON)LoadImageA(g_inst, MAKEINTRESOURCEA(icons[state]), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    snprintf(g_nid.szTip, sizeof g_nid.szTip, "%s", tip.substr(0, 126).c_str());
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    Shell_NotifyIconA(NIM_MODIFY, &g_nid);
}
static void tray_add(HWND dlg) {
    g_nid.cbSize = sizeof g_nid; g_nid.hWnd = dlg; g_nid.uID = 1; g_nid.uCallbackMessage = WM_TRAY;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.hIcon = (HICON)LoadImageA(g_inst, MAKEINTRESOURCEA(IDI_TRAY_GREY), IMAGE_ICON, 16, 16, 0);
    snprintf(g_nid.szTip, sizeof g_nid.szTip, "%s", kTitle);
    Shell_NotifyIconA(NIM_ADD, &g_nid);
    g_nid.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconA(NIM_SETVERSION, &g_nid);
}
static void tray_remove() { Shell_NotifyIconA(NIM_DELETE, &g_nid); }
static void tray_balloon(const char* text) {
    g_nid.uFlags = NIF_INFO; g_nid.dwInfoFlags = NIIF_INFO;
    snprintf(g_nid.szInfoTitle, sizeof g_nid.szInfoTitle, "%s", kTitle);
    snprintf(g_nid.szInfo, sizeof g_nid.szInfo, "%s", text);
    Shell_NotifyIconA(NIM_MODIFY, &g_nid);
}

static void refresh_discovered(HWND dlg) {
    auto e = g_sap.entries();
    g_found.clear();
    std::vector<std::string> names;
    for (auto& x : e) {
        if (!x.parsed) continue;
        g_found.push_back(x);
        names.push_back(x.desc.name + "  [" + x.desc.dest + ":" + std::to_string(x.desc.port) + " " + encoding_name(x.desc.encoding) + "/" + std::to_string(x.desc.sample_rate) + "/" + std::to_string(x.desc.channels) + "]");
    }
    HWND h = GetDlgItem(dlg, IDC_RX_DISCOVERED);
    int cur = (int)SendMessageA(h, CB_GETCURSEL, 0, 0), count = (int)SendMessageA(h, CB_GETCOUNT, 0, 0);
    if (count == (int)names.size()) {
        bool same = true;
        for (int i = 0; i < count && same; i++) { char b[512] = {0}; SendMessageA(h, CB_GETLBTEXT, i, (LPARAM)b); same = names[i] == b; }
        if (same) return;
    }
    combo_fill(dlg, IDC_RX_DISCOVERED, names, names.empty() ? -1 : std::max(0, std::min(cur, (int)names.size() - 1)));
}

static std::string fmt_engine(const StatusBlock& s) {
    char buf[700];
    const char* clock = s.ptp_state == 2 ? "locked to" : s.ptp_state == 3 ? "this PC is master" : s.ptp_state == 4 ? "free-running (no master found)" : s.ptp_state == 1 ? "searching / unlocked" : "off";
    snprintf(buf, sizeof buf,
             "Clock:      PTP %s %s   offset %+.1f us\n"
             "Receive:    %s   %u packets, %u lost, %u resyncs%s%s\n"
             "Transmit:   %u packets, %u underruns   (%s, buffer %u, %u Hz, %u late periods)",
             clock, s.ptp_state == 2 ? s.ptp_master : "", s.ptp_offset_ns / 1000.0,
             s.rx_active ? "receiving" : "NO DATA", s.rx_packets, s.rx_lost, s.rx_resyncs, s.message[0] ? "\n            " : "", s.message,
             s.tx_packets, s.tx_underruns, s.local_ip, s.buffer_frames, s.sample_rate, s.late_periods);
    return buf;
}

static void refresh_status(HWND dlg) {
    StatusBlock drv; bool drv_on = driver_status(drv);
    bool br_on = g_bridge.running();
    BridgeStats bs = g_bridge.stats();
    std::string text, tip; int state = 0;
    // Driver line
    if (drv_on) text += "ASIO driver: running in an audio application (process " + std::to_string(drv.pid) + ")\n";
    else text += "ASIO driver: idle (no ASIO application has the device open)\n";
    // Bridge line
    if (br_on) {
        text += "Bridge:     running   incoming stream plays on: " + (bs.play_name.empty() ? std::string("(nothing)") : bs.play_name) + "   |   outgoing stream taken from: " + (bs.source_name.empty() ? std::string("(silence)") : bs.source_name) + (bs.loopback ? " (loopback)" : "") + "\n";
        if (!bs.error.empty()) text += "            ERROR: " + bs.error + "\n";
    } else text += std::string("Bridge:     stopped") + (g_bridge_error.empty() ? "" : "   last error: " + g_bridge_error) + "\n";
    if (drv_on && br_on) text += "WARNING: driver and bridge are both running and both transmit to the same stream.\n";
    // Engine details (bridge preferred, else driver)
    StatusBlock eng; bool have = false;
    if (br_on) { eng = g_bridge.engine_status(); have = true; } else if (drv_on) { eng = drv; have = true; }
    if (have) text += fmt_engine(eng);
    else text += "Nothing is streaming. Start the bridge, or open the ASIO device in an audio application.";
    set_text(dlg, IDC_STATUS, text);
    // Bridge status line
    if (br_on) {
        char b[160]; snprintf(b, sizeof b, "FIFO play %zu / source %zu   underruns %u / %u", bs.play_fifo, bs.cap_fifo, bs.play_under, bs.cap_under);
        set_text(dlg, IDC_BR_STATUS, bs.error.empty() ? b : "ERROR - see status");
    } else set_text(dlg, IDC_BR_STATUS, "");
    set_text(dlg, IDC_BR_START, br_on ? "Stop bridge" : "Start bridge");
    // Tray state
    bool bad = (br_on && !bs.error.empty()) || (!br_on && !g_bridge_error.empty());
    if (bad) { state = 3; tip = "AES67 VSC: ERROR - " + (bs.error.empty() ? g_bridge_error : bs.error); }
    else if (!have) { state = 0; tip = "AES67 VSC: idle"; }
    else {
        bool clock_ok = eng.ptp_state == 2 || eng.ptp_state == 3 || eng.ptp_state == 0;
        bool rx_ok = !g_cfg.rx_enabled || g_cfg.rx.dest.empty() || eng.rx_active;
        state = (clock_ok && rx_ok && !(drv_on && br_on)) ? 1 : 2;
        tip = std::string("AES67 VSC: ") + (br_on ? "bridge" : "driver") + " running, PTP " + ptp_state_name(eng.ptp_state) + ", RX " + (eng.rx_active ? "ok" : "no data") + ", TX " + std::to_string(eng.tx_packets) + " pkts";
    }
    tray_set(state, tip);
}

// ---------------------------------------------------------------- window
static void show_window(HWND dlg) { ShowWindow(dlg, SW_SHOW); ShowWindow(dlg, SW_RESTORE); SetForegroundWindow(dlg); }
static void hide_window(HWND dlg) {
    ShowWindow(dlg, SW_HIDE);
    if (!g_balloon_shown) { g_balloon_shown = true; tray_balloon("Still running here. Double-click the icon to open, right-click for the menu."); }
}
static void tray_menu(HWND dlg) {
    HMENU m = CreatePopupMenu();
    AppendMenuA(m, MF_STRING, IDM_SHOW, "Open AES67 Virtual Sound Card");
    AppendMenuA(m, MF_STRING, IDM_BRIDGE, g_bridge.running() ? "Stop bridge" : "Start bridge");
    AppendMenuA(m, MF_STRING, IDM_TEAMS, "Teams preset");
    AppendMenuA(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(m, MF_STRING, IDM_EXIT, "Exit (stops the bridge)");
    SetMenuDefaultItem(m, IDM_SHOW, FALSE);
    POINT p; GetCursorPos(&p);
    SetForegroundWindow(dlg);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, dlg, nullptr);
    PostMessageA(dlg, WM_NULL, 0, 0);
    DestroyMenu(m);
}
static void do_exit(HWND dlg) { bridge_stop(); tray_remove(); g_sap.stop(); DestroyWindow(dlg); PostQuitMessage(0); }

static INT_PTR CALLBACK DlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_SHOWME) { show_window(dlg); return TRUE; }
    switch (msg) {
    case WM_INITDIALOG: {
        g_dlg = dlg;
        HICON ic = LoadIconA(g_inst, MAKEINTRESOURCEA(IDI_APP));
        if (ic) { SendMessageA(dlg, WM_SETICON, ICON_BIG, (LPARAM)ic); SendMessageA(dlg, WM_SETICON, ICON_SMALL, (LPARAM)ic); }
        g_ifaces = list_interfaces();
        load_to_ui(dlg);
        { std::string err; g_sap.start(pick_interface(g_cfg.interface_ip).ipv4, true, err); }
        tray_add(dlg);
        refresh_status(dlg);
        SetTimer(dlg, 1, 500, nullptr);
        DragAcceptFiles(dlg, TRUE);
        if (!g_pending_sdp.empty()) { import_sdp_text(dlg, g_pending_sdp, "Loaded SDP from the command line"); g_pending_sdp.clear(); }
        return TRUE;
    }
    case WM_TIMER: {
        static int n = 0;
        if (g_bridge.restart_needed()) { g_cfg.load(); bridge_start(nullptr); }
        refresh_status(dlg);
        if (++n % 4 == 0 && IsWindowVisible(dlg)) refresh_discovered(dlg);
        return TRUE;
    }
    case WM_TRAY:
        switch (LOWORD(lp)) {
        case WM_LBUTTONDBLCLK: case NIN_KEYSELECT: show_window(dlg); break;
        case WM_LBUTTONUP: if (!IsWindowVisible(dlg)) show_window(dlg); break;
        case WM_RBUTTONUP: case WM_CONTEXTMENU: tray_menu(dlg); break;
        }
        return TRUE;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_MINIMIZE) { hide_window(dlg); return TRUE; }
        break;
    case WM_DROPFILES: {
        HDROP hd = (HDROP)wp; char file[MAX_PATH] = {0};
        if (DragQueryFileA(hd, 0, file, MAX_PATH)) { std::string text; if (read_file(file, text)) import_sdp_text(dlg, text, std::string("Loaded ") + file); }
        DragFinish(hd);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_RX_LOAD: do_load_sdp(dlg); return TRUE;
        case IDC_RX_USE: { int i = combo_sel(dlg, IDC_RX_DISCOVERED); if (i >= 0 && i < (int)g_found.size()) import_sdp_text(dlg, g_found[i].sdp, "Selected stream announced by " + g_found[i].origin_ip); return TRUE; }
        case IDC_RX_CLEAR: g_cfg.rx = StreamDesc(); g_cfg.rx.dest.clear(); g_cfg.rx_sdp_text.clear(); g_cfg.rx_enabled = false; load_to_ui(dlg); return TRUE;
        case IDC_TX_EXPORT: do_export_sdp(dlg); return TRUE;
        case IDC_TX_COPY: do_copy_sdp(dlg); return TRUE;
        case IDC_OPENCFG: ShellExecuteA(dlg, "open", config_dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); return TRUE;
        case IDC_RATE:
            if (HIWORD(wp) == CBN_SELCHANGE) { int rs = combo_sel(dlg, IDC_RATE); int rate = rs == 0 ? 44100 : rs == 2 ? 96000 : 48000; int v = atoi(get_text(dlg, IDC_BUFFER).c_str()); fill_buffer_combo(dlg, rate, v); }
            return TRUE;
        case IDC_BR_START: case IDM_BRIDGE: bridge_toggle(dlg); refresh_status(dlg); return TRUE;
        case IDC_BR_TEAMS: case IDM_TEAMS: show_window(dlg); teams_preset(dlg); refresh_status(dlg); return TRUE;
        case IDC_APPLY: apply(dlg); return TRUE;
        case IDM_SHOW: show_window(dlg); return TRUE;
        case IDCANCEL: hide_window(dlg); return TRUE;
        case IDC_EXIT: case IDM_EXIT:
            if (g_bridge.running() && MessageBoxA(dlg, "The bridge is running. Exit and stop it?", kTitle, MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return TRUE;
            do_exit(dlg); return TRUE;
        }
        break;
    case WM_CLOSE: hide_window(dlg); return TRUE;
    case WM_ENDSESSION: if (wp) do_exit(dlg); return TRUE;
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
    // Single instance: hand over to the running one.
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
    size_t sdp = args.find(".sdp");
    if (sdp != std::string::npos) {
        std::string f = args; size_t q1 = f.find('"'), q2 = f.rfind('"');
        if (q1 != std::string::npos && q2 > q1) f = f.substr(q1 + 1, q2 - q1 - 1);
        std::string text; if (read_file(f, text)) g_pending_sdp = text;
    }
    HWND dlg = CreateDialogParamA(inst, MAKEINTRESOURCEA(IDD_MAIN), nullptr, DlgProc, 0);
    if (!dlg) return 1;
    if (start_bridge) bridge_start(nullptr);
    if (!start_hidden) show_window(dlg); else g_balloon_shown = true;
    MSG m;
    while (GetMessageA(&m, nullptr, 0, 0) > 0) {
        if (!IsWindow(dlg) || !IsDialogMessageA(dlg, &m)) { TranslateMessage(&m); DispatchMessageA(&m); }
    }
    ReleaseMutex(mutex); CloseHandle(mutex);
    CoUninitialize();
    return 0;
}
