// config_win.hpp — native Win32 settings window for the TS plugin.
// Opened from TeamSpeak's Plugins dialog ("Settings" button). Runs on the
// thread TS spawns for ts3plugin_configure (PLUGIN_OFFERS_CONFIGURE_NEW_THREAD)
// so a blocking message loop is fine. Plain Win32 - no Qt dependency, which a
// real TS-integrated dialog would require.
//
// Rows are generated from rtr::settingsTable(), so new settings appear here
// automatically. Sliders apply LIVE (next audio frame) for tuning by ear;
// "Save" persists to the ini, "Reset" returns to defaults.
#pragma once
#ifdef _WIN32

#include <Windows.h>
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")

#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "settings.hpp"

namespace rtrcfg {

struct Host {
    std::function<rtr::Settings()> get;            // snapshot current settings
    std::function<void(const rtr::Settings&)> set; // live-apply
    std::function<bool()> save;                    // persist current to ini
    std::string iniPath;                           // shown for reference
};

// Wide-range positive knobs (Hz, smoothing) get a log-scaled slider so the
// low end isn't crushed into the first few pixels.
inline bool logScaled(const rtr::SettingDesc& d)
{
    return d.min > 0.0f && d.max / d.min >= 20.0f;
}

constexpr int kSteps = 400;

inline int toSlider(const rtr::SettingDesc& d, float v)
{
    v = std::clamp(v, d.min, d.max);
    const float t = logScaled(d)
        ? std::log(v / d.min) / std::log(d.max / d.min)
        : (v - d.min) / (d.max - d.min);
    return int(std::lround(t * kSteps));
}

inline float fromSlider(const rtr::SettingDesc& d, int pos)
{
    const float t = float(pos) / kSteps;
    return logScaled(d) ? d.min * std::pow(d.max / d.min, t)
                        : d.min + (d.max - d.min) * t;
}

struct DlgState {
    Host host;
    std::vector<HWND> sliders, values;
    HWND help = nullptr;
};

// Layout
constexpr int kMargin = 10, kRowH = 30, kLabelW = 130, kSliderW = 230,
              kValueW = 70, kGap = 8, kBtnW = 90, kBtnH = 26;
constexpr int kClientW = kMargin + kLabelW + kGap + kSliderW + kGap + kValueW + kMargin;

constexpr int IDC_SAVE = 1001, IDC_RESET = 1002, IDC_CLOSE = 1003;

inline std::atomic<HWND> g_hwnd{nullptr};

inline void setValueText(DlgState* st, size_t i, float v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", double(v));
    SetWindowTextA(st->values[i], buf);
}

inline void setSliders(DlgState* st, const rtr::Settings& s)
{
    const auto& table = rtr::settingsTable();
    for (size_t i = 0; i < table.size(); ++i) {
        SendMessage(st->sliders[i], TBM_SETPOS, TRUE, toSlider(table[i], s.*(table[i].field)));
        setValueText(st, i, s.*(table[i].field));
    }
}

inline LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* st = reinterpret_cast<DlgState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    const auto& table = rtr::settingsTable();

    switch (msg) {
    case WM_CREATE: {
        st = reinterpret_cast<DlgState*>(
            reinterpret_cast<CREATESTRUCT*>(lp)->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        const rtr::Settings cur = st->host.get();

        int y = kMargin;
        for (size_t i = 0; i < table.size(); ++i) {
            HWND label = CreateWindowA("STATIC", table[i].key,
                WS_CHILD | WS_VISIBLE | SS_RIGHT,
                kMargin, y + 6, kLabelW, 18, hwnd, nullptr, nullptr, nullptr);
            HWND slider = CreateWindowA(TRACKBAR_CLASSA, "",
                WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                kMargin + kLabelW + kGap, y + 2, kSliderW, 24, hwnd, nullptr, nullptr, nullptr);
            HWND value = CreateWindowA("STATIC", "",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                kMargin + kLabelW + kGap + kSliderW + kGap, y + 6, kValueW, 18,
                hwnd, nullptr, nullptr, nullptr);
            SendMessage(label, WM_SETFONT, (WPARAM)font, TRUE);
            SendMessage(value, WM_SETFONT, (WPARAM)font, TRUE);
            SendMessage(slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, kSteps));
            st->sliders.push_back(slider);
            st->values.push_back(value);
            y += kRowH;
        }
        setSliders(st, cur);

        y += 4;
        st->help = CreateWindowA("STATIC",
            ("changes apply instantly - Save writes " + st->host.iniPath).c_str(),
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            kMargin, y, kClientW - 2 * kMargin, 32, hwnd, nullptr, nullptr, nullptr);
        SendMessage(st->help, WM_SETFONT, (WPARAM)font, TRUE);
        y += 36;

        const struct { const char* text; int id; } btns[] = {
            {"Save", IDC_SAVE}, {"Reset", IDC_RESET}, {"Close", IDC_CLOSE}};
        int x = kClientW - kMargin - 3 * kBtnW - 2 * kGap;
        for (const auto& b : btns) {
            HWND btn = CreateWindowA("BUTTON", b.text,
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                x, y, kBtnW, kBtnH, hwnd, (HMENU)(INT_PTR)b.id, nullptr, nullptr);
            SendMessage(btn, WM_SETFONT, (WPARAM)font, TRUE);
            x += kBtnW + kGap;
        }
        return 0;
    }

    case WM_HSCROLL: {
        if (!st) break;
        const HWND slider = reinterpret_cast<HWND>(lp);
        for (size_t i = 0; i < st->sliders.size(); ++i) {
            if (st->sliders[i] != slider) continue;
            const int pos = (int)SendMessage(slider, TBM_GETPOS, 0, 0);
            const float v = fromSlider(table[i], pos);
            rtr::Settings s = st->host.get();
            s.*(table[i].field) = v;
            st->host.set(s); // live: audible on the next audio frame
            setValueText(st, i, v);
            char buf[192];
            std::snprintf(buf, sizeof(buf), "%s: %s (%g..%g)", table[i].key,
                          table[i].help, double(table[i].min), double(table[i].max));
            SetWindowTextA(st->help, buf);
            break;
        }
        return 0;
    }

    case WM_COMMAND:
        if (!st) break;
        switch (LOWORD(wp)) {
        case IDC_SAVE:
            SetWindowTextA(st->help, st->host.save()
                ? ("saved to " + st->host.iniPath).c_str()
                : ("save FAILED: " + st->host.iniPath).c_str());
            return 0;
        case IDC_RESET: {
            const rtr::Settings defaults{};
            st->host.set(defaults);
            setSliders(st, defaults);
            SetWindowTextA(st->help, "defaults restored (Save to persist)");
            return 0;
        }
        case IDC_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_hwnd = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// Blocking: runs the window's message loop on the calling (TS-spawned) thread.
inline void runDialog(Host host)
{
    if (HWND existing = g_hwnd.load()) { // already open: just focus it
        SetForegroundWindow(existing);
        return;
    }

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    static const char* kClass = "RtrSettingsWnd";
    WNDCLASSA wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassA(&wc); // idempotent: re-registration just fails

    const int rows = (int)rtr::settingsTable().size();
    const int clientH = kMargin + rows * kRowH + 4 + 36 + kBtnH + kMargin;
    RECT r{0, 0, kClientW, clientH};
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);

    DlgState st{};
    st.host = std::move(host);
    HWND hwnd = CreateWindowA(kClass, "RoN Tactical Radio settings",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
        nullptr, nullptr, wc.hInstance, &st);
    if (!hwnd) return;
    g_hwnd = hwnd;
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

} // namespace rtrcfg

#endif // _WIN32
