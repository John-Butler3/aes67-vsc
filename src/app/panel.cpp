// AES67 Virtual Sound Card - control panel (Win32 dialog).
// Edits the shared configuration, imports/exports SDP, shows live driver status,
// and lists streams announced via SAP on the network.
#include "../core/config.h"
#include "../core/sdp.h"
#include "../core/sap.h"
#include "../core/status.h"
#include "../core/platform.h"
#include "../core/ptp.h"
#include "panel_res.h"
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <cstdio>
#include <string>
#include <vector>
#include <cmath>

using namespace aes67;

static Config g_cfg;
static std::vector<NetInterface> g_ifaces;
static SapAgent g_sap;
static std::vector<SapEntry> g_found;
static StatusShare g_share;
static std::string g_pending_sdp;   // SDP passed on the command line / dropped

static std::string get_text(HWND dlg, int id) {
    char buf[1024] = {0};
    GetDlgItemTextA(dlg, id, buf, sizeof buf);
    return buf;
}
static void set_text(HWND dlg, int id, const std::string& s) { SetDlgItemTextA(dlg, id, s.c_str()); }
static void combo_fill(HWND dlg, int id, const std::vector<std::string>& items, int sel) {
    HWND h = GetDlgItem(dlg, id);
    SendMessageA(h, CB_RESETCONTENT, 0, 0);
    for (auto& s : items) SendMessageA(h, CB_ADDSTRING, 0, (LPARAM)s.c_str());
    SendMessageA(h, CB_SETCURSEL, (WPARAM)sel, 0);
}
static int combo_sel(HWND dlg, int id) { return (int)SendMessageA(GetDlgItem(dlg, id), CB_GETCURSEL, 0, 0); }

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

static void load_to_ui(HWND dlg) {
    std::vector<std::string> names; int sel = 0;
    names.push_back("Automatic");
    NetInterface picked = pick_interface(g_cfg.interface_ip);
    for (size_t i = 0; i < g_ifaces.size(); i++) {
        names.push_back(g_ifaces[i].ipv4 + "  (" + g_ifaces[i].name + ")");
        if (g_cfg.interface_ip != "auto" && g_ifaces[i].ipv4 == g_cfg.interface_ip) sel = (int)i + 1;
    }
    (void)picked;
    combo_fill(dlg, IDC_IFACE, names, sel);
    combo_fill(dlg, IDC_RATE, {"44100", "48000", "96000"}, g_cfg.sample_rate == 44100 ? 0 : g_cfg.sample_rate == 96000 ? 2 : 1);
    {
        int fpp = g_cfg.tx_stream("0.0.0.0", "").frames_per_packet();
        std::vector<std::string> b;
        for (int m : {1, 2, 3, 4, 5, 6, 8, 10, 16, 20, 32, 40}) b.push_back(std::to_string(fpp * m));
        combo_fill(dlg, IDC_BUFFER, b, -1);
        set_text(dlg, IDC_BUFFER, std::to_string(g_cfg.buffer_frames));
    }
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
    std::string e2; Config n; n.from_ini(c.to_ini(), e2);  // normalise
    g_cfg = n;
    return true;
}

static bool import_sdp_text(HWND dlg, const std::string& text, const std::string& source_desc) {
    StreamDesc d; std::string err;
    if (!sdp_parse(text, d, err)) {
        MessageBoxA(dlg, ("Could not use this SDP:\n" + err).c_str(), "AES67 VSC", MB_OK | MB_ICONERROR);
        return false;
    }
    std::string notes;
    if (d.sample_rate != g_cfg.sample_rate) {
        notes += "The stream runs at " + std::to_string(d.sample_rate) + " Hz; the device sample rate was changed to match.\n";
        g_cfg.sample_rate = d.sample_rate;
    }
    if (d.channels > 2) notes += "The stream has " + std::to_string(d.channels) + " channels; inputs 1-2 take stream channels " + std::to_string(g_cfg.rx_channel_offset + 1) + "-" + std::to_string(g_cfg.rx_channel_offset + 2) + " (adjustable below).\n";
    if (d.ptp_domain >= 0 && d.ptp_domain != g_cfg.ptp_domain) {
        notes += "The stream uses PTP domain " + std::to_string(d.ptp_domain) + "; the PTP domain was changed to match.\n";
        g_cfg.ptp_domain = d.ptp_domain;
    }
    g_cfg.rx = d; g_cfg.rx_sdp_text = text; g_cfg.rx_enabled = true;
    load_to_ui(dlg);
    if (!notes.empty()) MessageBoxA(dlg, (source_desc + "\n\n" + notes + "\nPress Apply to save.").c_str(), "AES67 VSC", MB_OK | MB_ICONINFORMATION);
    return true;
}

static void do_load_sdp(HWND dlg) {
    char file[MAX_PATH] = {0};
    OPENFILENAMEA ofn{}; ofn.lStructSize = sizeof ofn; ofn.hwndOwner = dlg;
    ofn.lpstrFilter = "SDP files (*.sdp)\0*.sdp\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH; ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrTitle = "Load receive stream SDP";
    if (!GetOpenFileNameA(&ofn)) return;
    std::string text;
    if (!read_file(file, text)) { MessageBoxA(dlg, "Cannot read the file.", "AES67 VSC", MB_OK | MB_ICONERROR); return; }
    import_sdp_text(dlg, text, std::string("Loaded ") + file);
}

static std::string current_tx_sdp() {
    NetInterface ni = pick_interface(g_cfg.interface_ip);
    StatusBlock s; std::string gm;
    if (g_share.read(s) && s.engine_running && wall_ms() - s.updated_ms < 3000 && s.ptp_master[0]) gm = s.ptp_master;
    else {
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
    std::string err;
    if (!ui_to_cfg(dlg, err)) { MessageBoxA(dlg, err.c_str(), "AES67 VSC", MB_OK | MB_ICONWARNING); return; }
    g_cfg.save();
    char file[MAX_PATH] = {0};
    std::string def = g_cfg.tx_name; for (auto& ch : def) if (strchr("\\/:*?\"<>|", ch)) ch = '_';
    snprintf(file, sizeof file, "%s.sdp", def.c_str());
    OPENFILENAMEA ofn{}; ofn.lStructSize = sizeof ofn; ofn.hwndOwner = dlg;
    ofn.lpstrFilter = "SDP files (*.sdp)\0*.sdp\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH; ofn.Flags = OFN_OVERWRITEPROMPT; ofn.lpstrDefExt = "sdp";
    ofn.lpstrTitle = "Save transmit stream SDP";
    if (!GetSaveFileNameA(&ofn)) return;
    if (!write_file(file, current_tx_sdp())) MessageBoxA(dlg, "Cannot write the file.", "AES67 VSC", MB_OK | MB_ICONERROR);
}

static void do_copy_sdp(HWND dlg) {
    std::string err;
    if (!ui_to_cfg(dlg, err)) { MessageBoxA(dlg, err.c_str(), "AES67 VSC", MB_OK | MB_ICONWARNING); return; }
    std::string s = current_tx_sdp();
    if (!OpenClipboard(dlg)) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, s.size() + 1);
    if (h) { memcpy(GlobalLock(h), s.c_str(), s.size() + 1); GlobalUnlock(h); SetClipboardData(CF_TEXT, h); }
    CloseClipboard();
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
    int cur = (int)SendMessageA(h, CB_GETCURSEL, 0, 0);
    int count = (int)SendMessageA(h, CB_GETCOUNT, 0, 0);
    if (count == (int)names.size()) {
        bool same = true;
        for (int i = 0; i < count && same; i++) { char b[512] = {0}; SendMessageA(h, CB_GETLBTEXT, i, (LPARAM)b); same = names[i] == b; }
        if (same) return;
    }
    combo_fill(dlg, IDC_RX_DISCOVERED, names, names.empty() ? -1 : std::max(0, std::min(cur, (int)names.size() - 1)));
}

static void refresh_status(HWND dlg) {
    StatusBlock s;
    std::string t;
    if (!g_share.read(s) || !s.engine_running || wall_ms() - s.updated_ms > 3000) {
        t = "Driver idle. Open the \"AES67 Virtual Sound Card\" ASIO device in your audio application to start streaming.\n"
            "Changes made here are saved with Apply and picked up by the driver automatically.";
    } else {
        char buf[1024];
        snprintf(buf, sizeof buf,
                 "Driver running in process %u   %u Hz, %u frames per buffer, local IP %s\n"
                 "Clock:     PTP %s   master %s   offset %+.1f us   path delay %.1f us\n"
                 "Receive:   %s   %u packets   %u lost   %u resyncs   %u rejected   arrival offset %d frames\n"
                 "Transmit:  %u packets   %u underruns\n"
                 "Engine:    %u periods   %u late\n%s",
                 s.pid, s.sample_rate, s.buffer_frames, s.local_ip,
                 ptp_state_name(s.ptp_state), s.ptp_master, s.ptp_offset_ns / 1000.0, s.ptp_path_delay_ns / 1000.0,
                 s.rx_active ? "receiving" : "no data", s.rx_packets, s.rx_lost, s.rx_resyncs, s.rx_bad, s.rx_offset_frames,
                 s.tx_packets, s.tx_underruns, s.periods, s.late_periods, s.message);
        t = buf;
    }
    if (get_text(dlg, IDC_STATUS) != t) set_text(dlg, IDC_STATUS, t);
}

static INT_PTR CALLBACK DlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        HICON ic = LoadIconA(GetModuleHandleA(nullptr), MAKEINTRESOURCEA(IDI_APP));
        if (ic) SendMessageA(dlg, WM_SETICON, ICON_BIG, (LPARAM)ic);
        g_ifaces = list_interfaces();
        load_to_ui(dlg);
        {
            std::string err;
            g_sap.start(pick_interface(g_cfg.interface_ip).ipv4, true, err);
        }
        refresh_status(dlg);
        SetTimer(dlg, 1, 500, nullptr);
        DragAcceptFiles(dlg, TRUE);
        if (!g_pending_sdp.empty()) { import_sdp_text(dlg, g_pending_sdp, "Loaded SDP from the command line"); g_pending_sdp.clear(); }
        return TRUE;
    }
    case WM_TIMER:
        refresh_status(dlg);
        static int n = 0; if (++n % 4 == 0) refresh_discovered(dlg);
        return TRUE;
    case WM_DROPFILES: {
        HDROP hd = (HDROP)wp; char file[MAX_PATH] = {0};
        if (DragQueryFileA(hd, 0, file, MAX_PATH)) { std::string text; if (read_file(file, text)) import_sdp_text(dlg, text, std::string("Loaded ") + file); }
        DragFinish(hd);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_RX_LOAD: do_load_sdp(dlg); return TRUE;
        case IDC_RX_USE: {
            int i = combo_sel(dlg, IDC_RX_DISCOVERED);
            if (i >= 0 && i < (int)g_found.size()) import_sdp_text(dlg, g_found[i].sdp, "Selected stream announced by " + g_found[i].origin_ip);
            return TRUE;
        }
        case IDC_RX_CLEAR: g_cfg.rx = StreamDesc(); g_cfg.rx.dest.clear(); g_cfg.rx_sdp_text.clear(); g_cfg.rx_enabled = false; load_to_ui(dlg); return TRUE;
        case IDC_TX_EXPORT: do_export_sdp(dlg); return TRUE;
        case IDC_TX_COPY: do_copy_sdp(dlg); return TRUE;
        case IDC_OPENCFG: ShellExecuteA(dlg, "open", config_dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); return TRUE;
        case IDC_RATE:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                // Refill the buffer-size suggestions for the new packet size, keeping the current value.
                int rs = combo_sel(dlg, IDC_RATE);
                Config tmp = g_cfg; tmp.sample_rate = rs == 0 ? 44100 : rs == 2 ? 96000 : 48000;
                int fpp = tmp.tx_stream("0.0.0.0", "").frames_per_packet();
                std::string cur = get_text(dlg, IDC_BUFFER);
                std::vector<std::string> b;
                for (int m : {1, 2, 3, 4, 5, 6, 8, 10, 16, 20, 32, 40}) b.push_back(std::to_string(fpp * m));
                combo_fill(dlg, IDC_BUFFER, b, -1);
                int v = atoi(cur.c_str()); v = std::max(fpp, ((v + fpp - 1) / fpp) * fpp);
                set_text(dlg, IDC_BUFFER, std::to_string(v));
            }
            return TRUE;
        case IDC_APPLY: {
            std::string err;
            if (!ui_to_cfg(dlg, err)) { MessageBoxA(dlg, err.c_str(), "AES67 VSC", MB_OK | MB_ICONWARNING); return TRUE; }
            if (!g_cfg.save()) { MessageBoxA(dlg, ("Cannot write " + Config::default_path()).c_str(), "AES67 VSC", MB_OK | MB_ICONERROR); return TRUE; }
            load_to_ui(dlg);
            {
                std::string e; g_sap.stop(); g_sap.start(pick_interface(g_cfg.interface_ip).ipv4, true, e);
            }
            return TRUE;
        }
        case IDCANCEL: EndDialog(dlg, 0); return TRUE;
        }
        break;
    case WM_CLOSE: EndDialog(dlg, 0); return TRUE;
    }
    return FALSE;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR cmdline, int) {
    net_init();
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
    InitCommonControlsEx(&icc);
    g_cfg.load();
    if (cmdline && *cmdline) {
        std::string f = cmdline;
        if (f.size() >= 2 && f.front() == '"' && f.back() == '"') f = f.substr(1, f.size() - 2);
        std::string text;
        if (read_file(f, text)) g_pending_sdp = text;
    }
    DialogBoxParamA(inst, MAKEINTRESOURCEA(IDD_MAIN), nullptr, DlgProc, 0);
    g_sap.stop();
    return 0;
}
