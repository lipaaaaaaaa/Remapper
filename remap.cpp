// Key remapper with classic Win95 style window, HID info button and RGB control for Dark Project ALU87A (342D:E40F).
// Build (MinGW): g++ -std=c++17 -O2 -municode -mwindows -static remap.cpp -o remap.exe -lgdi32 -lhid -lsetupapi -lcomdlg32
// Build (MSVC):  cl /EHsc /O2 remap.cpp user32.lib gdi32.lib hid.lib setupapi.lib comdlg32.lib /link /SUBSYSTEM:WINDOWS
// Do not add a manifest, that is what keeps the classic look.
// Rules are hex virtual key codes, saved to remap.ini as from=to.
//
// RGB:
//   Nothing painted: global effect. Multicolor fills the 8-slot palette (whole keyboard).
//   Any key or row painted: Custom mode (HID effect 14, slot byte 52 = 1) plus per-LED
//   colors on feature command 0x0A. Unpainted keys are sent black, so one row stays one row.
//   The 8-slot palette cannot target a row; that is why the whole board lit up before.
// LED indexes are the ALU87A Matrix_LEDCode (column-major, 6 keys per column).
#include <windows.h>
#include <commdlg.h>
#include <setupapi.h>
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <map>
#include <string>
#include <vector>

enum { ID_LIST = 1, ID_FROM, ID_TO, ID_ADD, ID_DEL, ID_SAVE, ID_HID, ID_FX, ID_MULTI, ID_COLOR, ID_BRIGHT, ID_SPEED, ID_RGB, ID_PICK };

static std::map<DWORD, WORD> g_map;
static HWND g_list, g_from, g_to, g_fx, g_multi, g_color;

static const struct { const wchar_t* name; BYTE id; } g_effects[] = {
    {L"Wave", 0}, {L"Spiral Wave", 6}, {L"Random", 2}, {L"Star", 7}, {L"Footprint", 4},
    {L"River", 8}, {L"Color Cycle", 1}, {L"Breathing", 3}, {L"Solid", 5}, {L"Ripples", 9},
    {L"Trigger", 10}, {L"Color Discharge", 11}, {L"Sine Wave", 12}, {L"Rain", 13},
    {L"Custom", 14},
};

struct Fx {
    BYTE id;
    DWORD rgb, bright, speed;
    bool multi;
    bool perkey;
    int painted;
    BYTE kr[96], kg[96], kb[96];
    COLORREF pal[6];
    int n;
};

static LRESULT CALLBACK hook(int n, WPARAM w, LPARAM l) {
    if (n == HC_ACTION) {
        auto* k = reinterpret_cast<KBDLLHOOKSTRUCT*>(l);
        if (!(k->flags & LLKHF_INJECTED)) {
            auto it = g_map.find(k->vkCode);
            if (it != g_map.end()) {
                INPUT in{};
                in.type = INPUT_KEYBOARD;
                in.ki.wVk = it->second;
                in.ki.dwFlags = (w == WM_KEYUP || w == WM_SYSKEYUP) ? KEYEVENTF_KEYUP : 0;
                SendInput(1, &in, sizeof in);
                return 1;
            }
        }
    }
    return CallNextHookEx(nullptr, n, w, l);
}

// Lists HID interfaces of VID 342D / PID E40F with usage page, usage and report sizes.
static std::wstring hidInfo() {
    std::wstring out;
    GUID g;
    HidD_GetHidGuid(&g);
    HDEVINFO set = SetupDiGetClassDevsW(&g, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    SP_DEVICE_INTERFACE_DATA d{};
    d.cbSize = sizeof d;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &g, i, &d); i++) {
        DWORD sz = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &d, nullptr, 0, &sz, nullptr);
        std::vector<char> buf(sz);
        auto* det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        det->cbSize = sizeof *det;
        if (!SetupDiGetDeviceInterfaceDetailW(set, &d, det, sz, nullptr, nullptr)) continue;
        HANDLE h = CreateFileW(det->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES a{};
        a.Size = sizeof a;
        PHIDP_PREPARSED_DATA pp;
        if (HidD_GetAttributes(h, &a) && a.VendorID == 0x342D && a.ProductID == 0xE40F &&
            HidD_GetPreparsedData(h, &pp)) {
            HIDP_CAPS c;
            HidP_GetCaps(pp, &c);
            wchar_t s[512];
            swprintf(s, 512, L"%ls\nUsagePage=%04X Usage=%04X In=%u Out=%u Feature=%u\n\n",
                     det->DevicePath, c.UsagePage, c.Usage, c.InputReportByteLength,
                     c.OutputReportByteLength, c.FeatureReportByteLength);
            out += s;
            HidD_FreePreparsedData(pp);
        }
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);
    return out.empty() ? L"No matching HID interfaces found." : out;
}

static std::wstring dump(const BYTE* p, int n) {
    std::wstring s;
    wchar_t t[8];
    for (int i = 0; i < n; i++) {
        if (i % 16 == 0) { swprintf(t, 8, L"\n%02X:", i); s += t; }
        swprintf(t, 8, L" %02X", p[i]);
        s += t;
    }
    return s + L"\n";
}

// Sends the effect to the keyboard and shows a log box (Ctrl+C in the box copies the text).
static DWORD WINAPI rgbApply(LPVOID p) {
    Fx f = *static_cast<Fx*>(p);
    delete static_cast<Fx*>(p);
    std::wstring log;
    bool found = false;
    auto step = [&](const wchar_t* what, BOOL ok) {
        DWORD err = GetLastError();
        wchar_t t[96];
        swprintf(t, 96, L"%ls: %ls", what, ok ? L"ok" : L"FAILED");
        log += t;
        if (!ok) { swprintf(t, 96, L" (error %lu)", err); log += t; }
        log += L"\n";
    };
    GUID g;
    HidD_GetHidGuid(&g);
    HDEVINFO set = SetupDiGetClassDevsW(&g, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    SP_DEVICE_INTERFACE_DATA d{};
    d.cbSize = sizeof d;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &g, i, &d); i++) {
        DWORD sz = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &d, nullptr, 0, &sz, nullptr);
        std::vector<char> buf(sz);
        auto* det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        det->cbSize = sizeof *det;
        if (!SetupDiGetDeviceInterfaceDetailW(set, &d, det, sz, nullptr, nullptr)) continue;
        HANDLE h = CreateFileW(det->DevicePath, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            if (wcsstr(det->DevicePath, L"pid_e40f") && wcsstr(det->DevicePath, L"mi_02")) {
                wchar_t t[64];
                swprintf(t, 64, L"Open mi_02 failed (error %lu)\n", GetLastError());
                log += t;
            }
            continue;
        }
        HIDD_ATTRIBUTES a{};
        a.Size = sizeof a;
        PHIDP_PREPARSED_DATA pp;
        if (HidD_GetAttributes(h, &a) && a.VendorID == 0x342D && a.ProductID == 0xE40F &&
            HidD_GetPreparsedData(h, &pp)) {
            HIDP_CAPS c;
            HidP_GetCaps(pp, &c);
            HidD_FreePreparsedData(pp);
            if (c.UsagePage == 0xFF01 && c.Usage == 1) {
                found = true;
                std::vector<BYTE> b(c.FeatureReportByteLength);
                b[0] = 7;
                b[1] = 129;
                step(L"Set 0x81", HidD_SetFeature(h, b.data(), (ULONG)b.size()));
                Sleep(100);
                step(L"Get 0x81", HidD_GetFeature(h, b.data(), (ULONG)b.size()));
                BYTE profile = b[3];
                wchar_t t[80];
                swprintf(t, 80, L"Profile byte b[3] = %u\n", profile);
                log += t;
                std::fill(b.begin(), b.end(), 0);
                b[0] = 7;
                b[1] = 130;
                b[2] = profile + 1;
                step(L"Set 0x82", HidD_SetFeature(h, b.data(), (ULONG)b.size()));
                Sleep(100);
                step(L"Get 0x82", HidD_GetFeature(h, b.data(), (ULONG)b.size()));
                log += L"Read back:" + dump(b.data(), 96);
                b[1] = 2;
                memmove(&b[2], &b[3], b.size() - 3);
                b.back() = 0;
                BYTE* u = b.data() + 1;
                std::vector<BYTE> q(c.FeatureReportByteLength);
                q[0] = 7;
                q[1] = 139;
                step(L"Set 0x8B", HidD_SetFeature(h, q.data(), (ULONG)q.size()));
                Sleep(100);
                step(L"Get 0x8B", HidD_GetFeature(h, q.data(), (ULONG)q.size()));
                swprintf(t, 80, L"Debounce = %u\n", q[4]);
                log += t;
                std::vector<BYTE> z(c.FeatureReportByteLength);
                z[0] = 7;
                z[1] = 11;
                z[2] = q[3];
                z[3] = std::max<BYTE>(q[4], 5);
                step(L"Set 0x0B", HidD_SetFeature(h, z.data(), (ULONG)z.size()));
                Sleep(100);
                BYTE id = f.perkey ? 14 : f.id;
                u[8] = id;
                u[9 + id] = (4 * f.bright + 99) / 100;
                u[23 + id] = (550 - 4 * f.speed) / 100;
                // 8 here is the web app's multicolor flag. It must stay 0 for Custom
                // or the firmware ignores the per-LED map.
                u[37 + id] = (!f.perkey && f.multi) ? 8 : 0;
                u[52] = f.perkey ? 1 : 0;
                u[85] = std::max<BYTE>(q[4], 5);
                u[82] = id == 3 ? 0 : 8;
                if (!f.perkey && f.multi && f.n > 0) {
                    for (int s = 0; s < 8; s++) {
                        COLORREF col = f.pal[s % f.n];
                        u[58 + s] = GetRValue(col);
                        u[66 + s] = GetGValue(col);
                        u[74 + s] = GetBValue(col);
                    }
                } else if (!f.perkey) {
                    u[58] = f.rgb >> 16;
                    u[66] = f.rgb >> 8;
                    u[74] = f.rgb;
                }
                if (f.perkey) {
                    swprintf(t, 80, L"Per-key custom, %d keys, effect 14 slot 1\n", f.painted);
                    log += t;
                }
                log += L"Writing:" + dump(b.data(), 96);
                BOOL wrote = HidD_SetFeature(h, b.data(), (ULONG)b.size());
                step(L"Set 0x02", wrote);
                Sleep(100);
                if (wrote && f.perkey && b.size() >= 200) {
                    for (int chunk = 0; chunk < 2; chunk++) {
                        std::vector<BYTE> w(b.size(), 0);
                        w[0] = 7;
                        w[1] = 10;
                        w[2] = profile + 1;
                        w[3] = 1;
                        w[4] = (BYTE)(chunk + 1);
                        for (int i = 0; i < 96; i++) {
                            if (chunk == 0) {
                                w[8 + i] = f.kr[i];
                                w[8 + 96 + i] = f.kg[i];
                            } else {
                                w[8 + i] = f.kb[i];
                            }
                        }
                        wchar_t what[40];
                        swprintf(what, 40, L"Set 0x0A chunk %d", chunk + 1);
                        step(what, HidD_SetFeature(h, w.data(), (ULONG)w.size()));
                        Sleep(40);
                    }
                    Sleep(100);
                    // The LED map is written after the effect report, so send the effect again
                    // to make the keyboard apply the map that was just written.
                    step(L"Set 0x02 again", HidD_SetFeature(h, b.data(), (ULONG)b.size()));
                    Sleep(100);
                    std::vector<BYTE> rb[2];
                    for (int chunk = 0; chunk < 2; chunk++) {
                        rb[chunk].assign(b.size(), 0);
                        rb[chunk][0] = 7;
                        rb[chunk][1] = 138;
                        rb[chunk][2] = profile + 1;
                        rb[chunk][3] = 1;
                        rb[chunk][4] = (BYTE)(chunk + 1);
                        HidD_SetFeature(h, rb[chunk].data(), (ULONG)rb[chunk].size());
                        Sleep(40);
                        HidD_GetFeature(h, rb[chunk].data(), (ULONG)rb[chunk].size());
                    }
                    int first = 0, last = 95;
                    while (first < 96 && !(f.kr[first] | f.kg[first] | f.kb[first])) first++;
                    while (last >= 0 && !(f.kr[last] | f.kg[last] | f.kb[last])) last--;
                    for (int k : {first, last}) {
                        swprintf(t, 80, L"LED %d sent %02X%02X%02X read8 %02X%02X%02X read9 %02X%02X%02X\n", k,
                                 f.kr[k], f.kg[k], f.kb[k], rb[0][8 + k], rb[0][104 + k], rb[1][8 + k],
                                 rb[0][9 + k], rb[0][105 + k], rb[1][9 + k]);
                        log += t;
                    }
                }
                CloseHandle(h);
                break;
            }
        }
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);
    if (!found) log += L"No FF01 interface of 342D:E40F could be opened.\n";
    MessageBoxW(nullptr, log.c_str(), L"Apply RGB", MB_OK);
    return 0;
}

struct Key { const wchar_t* n; int w; };
static const std::vector<std::vector<Key>> g_rows = {
    {{L"Esc",4},{L"F1",4},{L"F2",4},{L"F3",4},{L"F4",4},{L"F5",4},{L"F6",4},{L"F7",4},{L"F8",4},{L"F9",4},{L"F10",4},{L"F11",4},{L"F12",4}},
    {{L"`",4},{L"1",4},{L"2",4},{L"3",4},{L"4",4},{L"5",4},{L"6",4},{L"7",4},{L"8",4},{L"9",4},{L"0",4},{L"-",4},{L"=",4},{L"Bksp",8}},
    {{L"Tab",6},{L"Q",4},{L"W",4},{L"E",4},{L"R",4},{L"T",4},{L"Y",4},{L"U",4},{L"I",4},{L"O",4},{L"P",4},{L"[",4},{L"]",4},{L"\\",6}},
    {{L"Caps",7},{L"A",4},{L"S",4},{L"D",4},{L"F",4},{L"G",4},{L"H",4},{L"J",4},{L"K",4},{L"L",4},{L";",4},{L"'",4},{L"Enter",9}},
    {{L"Shift",9},{L"Z",4},{L"X",4},{L"C",4},{L"V",4},{L"B",4},{L"N",4},{L"M",4},{L",",4},{L".",4},{L"/",4},{L"Shift",11}},
    {{L"Ctrl",5},{L"Win",5},{L"Alt",5},{L"Space",25},{L"Alt",5},{L"Win",5},{L"Menu",5},{L"Ctrl",5}},
};
// Same order as g_rows. Index into the 96-LED custom buffer.
static const int g_led[6][16] = {
    {0, 6, 12, 18, 24, 30, 36, 42, 48, 54, 60, 66, 72, -1, -1, -1},
    {1, 7, 13, 19, 25, 31, 37, 43, 49, 55, 61, 67, 73, 79, -1, -1},
    {2, 8, 14, 20, 26, 32, 38, 44, 50, 56, 62, 68, 74, 80, -1, -1},
    {3, 9, 15, 21, 27, 33, 39, 45, 51, 57, 63, 69, 75, -1, -1, -1},
    {5, 10, 16, 22, 28, 34, 40, 46, 52, 58, 64, 70, -1, -1, -1, -1},
    {11, 17, 23, 29, 65, 71, 77, 83, -1, -1, -1, -1, -1, -1, -1, -1},
};
static const int Q = 7, KH = 28, KX = 310, KY = 34;
static COLORREF g_kc[6][16];   // preview color of every key
static COLORREF g_row[6];      // palette color of every row tab
static bool g_rowSet[6];       // true once the row tab was painted
static const COLORREF OFF = RGB(64, 64, 64);  // unpainted key look (dark gray)

static RECT rowBox(int r) { return RECT{KX - 18, KY + r * KH, KX - 4, KY + r * KH + KH}; }

template <class F> static void each(F f) {
    for (int r = 0; r < 6; r++) {
        int x = KX;
        for (int c = 0; c < (int)g_rows[r].size(); c++) {
            int w = g_rows[r][c].w * Q;
            f(r, c, RECT{x, KY + r * KH, x + w, KY + r * KH + KH});
            x += w;
        }
    }
}

static void refresh() {
    SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
    for (auto& [f, t] : g_map) {
        wchar_t s[32];
        swprintf(s, 32, L"%02lX  ->  %02X", f, t);
        int i = (int)SendMessageW(g_list, LB_ADDSTRING, 0, (LPARAM)s);
        SendMessageW(g_list, LB_SETITEMDATA, i, (LPARAM)f);
    }
}

static DWORD hex(HWND h) {
    wchar_t s[16];
    GetWindowTextW(h, s, 16);
    return wcstoul(s, nullptr, 16);
}

static HWND mk(HWND p, const wchar_t* cls, const wchar_t* txt, DWORD st, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(cls[0] == L'E' || cls[0] == L'L' ? WS_EX_CLIENTEDGE : 0, cls, txt,
                             WS_CHILD | WS_VISIBLE | st, x, y, w, h, p, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return c;
}

static void saveSettings(HWND h) {
    std::ofstream f("settings.ini");
    f << "effect=" << SendMessageW(g_fx, CB_GETCURSEL, 0, 0) << '\n';
    f << "multi=" << (SendMessageW(g_multi, BM_GETCHECK, 0, 0) == BST_CHECKED) << '\n';
    f << "bright=" << GetDlgItemInt(h, ID_BRIGHT, nullptr, FALSE) << '\n';
    f << "speed=" << GetDlgItemInt(h, ID_SPEED, nullptr, FALSE) << '\n';
    f << "color=" << std::hex << hex(g_color) << std::dec << '\n';
}

static LRESULT CALLBACK wnd(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        g_list = mk(h, L"LISTBOX", L"", WS_VSCROLL | LBS_NOTIFY, 10, 10, 270, 150, ID_LIST);
        mk(h, L"STATIC", L"From (hex VK):", 0, 10, 170, 90, 14, 0);
        mk(h, L"STATIC", L"To (hex VK):", 0, 110, 170, 90, 14, 0);
        g_from = mk(h, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 10, 186, 80, 22, ID_FROM);
        g_to = mk(h, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 110, 186, 80, 22, ID_TO);
        mk(h, L"BUTTON", L"Add", WS_TABSTOP | BS_PUSHBUTTON, 10, 218, 80, 24, ID_ADD);
        mk(h, L"BUTTON", L"Remove", WS_TABSTOP | BS_PUSHBUTTON, 100, 218, 80, 24, ID_DEL);
        mk(h, L"BUTTON", L"Save", WS_TABSTOP | BS_PUSHBUTTON, 190, 218, 80, 24, ID_SAVE);
        mk(h, L"BUTTON", L"HID Info", WS_TABSTOP | BS_PUSHBUTTON, 10, 250, 80, 24, ID_HID);
        mk(h, L"STATIC", L"Effect:", 0, 10, 284, 90, 14, 0);
        g_fx = mk(h, L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 10, 300, 180, 200, ID_FX);
        g_multi = mk(h, L"BUTTON", L"Multicolor", WS_TABSTOP | BS_AUTOCHECKBOX, 200, 300, 80, 22, ID_MULTI);
        mk(h, L"STATIC", L"Color (hex):", 0, 10, 332, 80, 14, 0);
        mk(h, L"STATIC", L"Bright 0-100:", 0, 100, 332, 85, 14, 0);
        mk(h, L"STATIC", L"Speed 0-100:", 0, 190, 332, 85, 14, 0);
        g_color = mk(h, L"EDIT", L"FF0000", WS_TABSTOP | ES_AUTOHSCROLL, 10, 348, 80, 22, ID_COLOR);
        mk(h, L"EDIT", L"100", WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER, 100, 348, 80, 22, ID_BRIGHT);
        mk(h, L"EDIT", L"50", WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER, 190, 348, 80, 22, ID_SPEED);
        mk(h, L"BUTTON", L"Apply RGB", WS_TABSTOP | BS_PUSHBUTTON, 10, 380, 80, 24, ID_RGB);
        for (auto& e : g_effects) SendMessageW(g_fx, CB_ADDSTRING, 0, (LPARAM)e.name);
        SendMessageW(g_fx, CB_SETCURSEL, 0, 0);
        std::fill(&g_kc[0][0], &g_kc[0][0] + 96, OFF);
        mk(h, L"STATIC", L"Row tab or key: that key only. Right click clears. Empty board = effect/palette.", 0, KX, 10, 430, 14, 0);
        mk(h, L"BUTTON", L"Pick...", WS_TABSTOP | BS_PUSHBUTTON, 100, 380, 80, 24, ID_PICK);
        {
            std::ifstream sf("settings.ini");
            std::string ln;
            while (std::getline(sf, ln)) {
                auto eq = ln.find('=');
                if (eq == std::string::npos) continue;
                std::string k = ln.substr(0, eq);
                bool dec = k == "effect" || k == "multi" || k == "bright" || k == "speed";
                DWORD v = std::stoul(ln.substr(eq + 1), nullptr, dec ? 10 : 16);
                if (k == "effect") SendMessageW(g_fx, CB_SETCURSEL, v, 0);
                else if (k == "multi") SendMessageW(g_multi, BM_SETCHECK, v ? BST_CHECKED : BST_UNCHECKED, 0);
                else if (k == "bright") SetDlgItemInt(h, ID_BRIGHT, v, FALSE);
                else if (k == "speed") SetDlgItemInt(h, ID_SPEED, v, FALSE);
                else if (k == "color") {
                    wchar_t s[8];
                    swprintf(s, 8, L"%06lX", v);
                    SetWindowTextW(g_color, s);
                }
            }
        }
        refresh();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
        SetBkMode(dc, TRANSPARENT);
        each([&](int r, int c, RECT k) {
            COLORREF v = g_kc[r][c];
            if (v != OFF) v = RGB(GetRValue(v) * 6 / 10, GetGValue(v) * 6 / 10, GetBValue(v) * 6 / 10);
            HBRUSH b = CreateSolidBrush(v);
            FillRect(dc, &k, b);
            DeleteObject(b);
            DrawEdge(dc, &k, EDGE_RAISED, BF_RECT);
            SetTextColor(dc, (GetRValue(v) * 299 + GetGValue(v) * 587 + GetBValue(v) * 114) / 1000 > 128 ? 0 : 0xFFFFFF);
            DrawTextW(dc, g_rows[r][c].n, -1, &k, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        });
        for (int r = 0; r < 6; r++) {
            RECT s = rowBox(r);
            HBRUSH b = CreateSolidBrush(g_rowSet[r] ? g_row[r] : OFF);
            FillRect(dc, &s, b);
            DeleteObject(b);
            DrawEdge(dc, &s, EDGE_RAISED, BF_RECT);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN: {
        POINT p{(short)LOWORD(l), (short)HIWORD(l)};
        bool clear = m == WM_RBUTTONDOWN;
        DWORD v = hex(g_color);
        COLORREF col = RGB(v >> 16, v >> 8, v);
        COLORREF off = OFF;
        each([&](int r, int c, RECT k) {
            if (PtInRect(&k, p)) g_kc[r][c] = (clear || g_kc[r][c] == col) ? off : col;
        });
        for (int r = 0; r < 6; r++) {
            RECT s = rowBox(r);
            if (PtInRect(&s, p)) {
                bool unpaint = clear || (g_rowSet[r] && g_row[r] == col);
                std::fill(g_kc[r], g_kc[r] + g_rows[r].size(), unpaint ? off : col);
                g_row[r] = col;
                g_rowSet[r] = !unpaint;
            }
        }
        InvalidateRect(h, nullptr, TRUE);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_ADD:
            if (hex(g_from) && hex(g_to)) {
                g_map[hex(g_from)] = (WORD)hex(g_to);
                refresh();
            }
            break;
        case ID_DEL: {
            int i = (int)SendMessageW(g_list, LB_GETCURSEL, 0, 0);
            if (i >= 0) {
                g_map.erase((DWORD)SendMessageW(g_list, LB_GETITEMDATA, i, 0));
                refresh();
            }
            break;
        }
        case ID_SAVE: {
            std::ofstream f("remap.ini");
            for (auto& [a, b] : g_map) f << std::hex << a << '=' << b << '\n';
            break;
        }
        case ID_HID:
            MessageBoxW(h, hidInfo().c_str(), L"HID Info", MB_OK);
            break;
        case ID_PICK: {
            static COLORREF cust[16];
            DWORD v = hex(g_color);
            CHOOSECOLORW cc{sizeof cc, h};
            cc.rgbResult = RGB(v >> 16, v >> 8, v);
            cc.lpCustColors = cust;
            cc.Flags = CC_RGBINIT | CC_FULLOPEN;
            if (ChooseColorW(&cc)) {
                wchar_t s[8];
                swprintf(s, 8, L"%02X%02X%02X", GetRValue(cc.rgbResult), GetGValue(cc.rgbResult), GetBValue(cc.rgbResult));
                SetWindowTextW(g_color, s);
            }
            break;
        }
        case ID_RGB: {
            saveSettings(h);
            Fx* f = new Fx{};
            f->id = g_effects[SendMessageW(g_fx, CB_GETCURSEL, 0, 0)].id;
            f->rgb = hex(g_color);
            f->bright = std::min(GetDlgItemInt(h, ID_BRIGHT, nullptr, FALSE), 100u);
            f->speed = std::min(GetDlgItemInt(h, ID_SPEED, nullptr, FALSE), 100u);
            f->multi = SendMessageW(g_multi, BM_GETCHECK, 0, 0) == BST_CHECKED;
            for (int r = 0; r < 6; r++)
                if (g_rowSet[r]) f->pal[f->n++] = g_row[r];
            COLORREF off = OFF;
            for (int r = 0; r < 6; r++) {
                for (int c = 0; c < (int)g_rows[r].size(); c++) {
                    int led = g_led[r][c];
                    if (led < 0 || g_kc[r][c] == off) continue;
                    f->kr[led] = GetRValue(g_kc[r][c]);
                    f->kg[led] = GetGValue(g_kc[r][c]);
                    f->kb[led] = GetBValue(g_kc[r][c]);
                    f->painted++;
                }
            }
            f->perkey = f->painted > 0;
            CloseHandle(CreateThread(nullptr, 0, rgbApply, f, 0, nullptr));
            break;
        }
        }
        return 0;
    case WM_DESTROY:
        saveSettings(h);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    std::ifstream f("remap.ini");
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        g_map[std::stoul(line.substr(0, eq), nullptr, 16)] =
            (WORD)std::stoul(line.substr(eq + 1), nullptr, 16);
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"Remap95";
    RegisterClassW(&wc);

    RECT r{0, 0, 750, 414};
    DWORD st = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, st, FALSE);
    HWND h = CreateWindowW(L"Remap95", L"Key Remapper", st, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    ShowWindow(h, show);

    HHOOK hk = SetWindowsHookExW(WH_KEYBOARD_LL, hook, inst, 0);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0)
        if (!IsDialogMessageW(h, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    UnhookWindowsHookEx(hk);
    return 0;
}