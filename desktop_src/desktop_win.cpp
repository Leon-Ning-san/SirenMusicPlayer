#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(_WIN32_WINNT) || (_WIN32_WINNT < 0x0603)
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0603
#endif
#if !defined(WINVER) || (WINVER < 0x0603)
#undef WINVER
#define WINVER 0x0603
#endif

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commdlg.h>
#include <mmsystem.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "backend.h"
#include "plat.h"

namespace {

constexpr UINT ID_THB_PREV = 1001;
constexpr UINT ID_THB_PLAY = 1002;
constexpr UINT ID_THB_NEXT = 1003;

enum MediaCmd {
    MEDIA_CMD_NONE   = 0,
    MEDIA_CMD_TOGGLE = 1,
    MEDIA_CMD_PREV   = 2,
    MEDIA_CMD_NEXT   = 3,
    MEDIA_CMD_PAUSE  = 4
};

#ifndef VK_MEDIA_NEXT_TRACK
#define VK_MEDIA_NEXT_TRACK 0xB0
#define VK_MEDIA_PREV_TRACK 0xB1
#define VK_MEDIA_STOP       0xB2
#define VK_MEDIA_PLAY_PAUSE 0xB3
#endif
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
#ifndef APPCOMMAND_MEDIA_NEXTTRACK
#define APPCOMMAND_MEDIA_NEXTTRACK      11
#define APPCOMMAND_MEDIA_PREVIOUSTRACK  12
#define APPCOMMAND_MEDIA_STOP           13
#define APPCOMMAND_MEDIA_PLAY_PAUSE     14
#endif
#ifndef APPCOMMAND_MEDIA_PLAY
#define APPCOMMAND_MEDIA_PLAY  46
#define APPCOMMAND_MEDIA_PAUSE 47
#endif
#ifndef GET_APPCOMMAND_LPARAM
#define GET_APPCOMMAND_LPARAM(lParam) ((short)(HIWORD(lParam) & ~0xF000))
#endif

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& o) noexcept : ptr_(o.ptr_) { o.ptr_ = nullptr; }
    ComPtr& operator=(ComPtr&& o) noexcept {
        if (this != &o) {
            reset();
            ptr_ = o.ptr_;
            o.ptr_ = nullptr;
        }
        return *this;
    }

    void reset(T* p = nullptr) {
        if (ptr_) ptr_->Release();
        ptr_ = p;
    }
    T* get() const { return ptr_; }
    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }
    void** put_void() {
        reset();
        return reinterpret_cast<void**>(&ptr_);
    }

private:
    T* ptr_ = nullptr;
};

struct UniqueIcon {
    HICON h = nullptr;
    UniqueIcon() = default;
    explicit UniqueIcon(HICON v) : h(v) {}
    ~UniqueIcon() { reset(); }
    UniqueIcon(const UniqueIcon&) = delete;
    UniqueIcon& operator=(const UniqueIcon&) = delete;
    UniqueIcon(UniqueIcon&& o) noexcept : h(o.h) { o.h = nullptr; }
    UniqueIcon& operator=(UniqueIcon&& o) noexcept {
        if (this != &o) { reset(o.h); o.h = nullptr; }
        return *this;
    }
    void reset(HICON v = nullptr) {
        if (h) DestroyIcon(h);
        h = v;
    }
    HICON get() const { return h; }
};

struct TextEvent {
    int action = 0;
    std::string text;
};

struct WinState {
    HWND hwnd = nullptr;
    std::vector<uint32_t> backbuf;
    std::vector<uint32_t> bgra;
    BITMAPINFO bmi{};
    int running = 1;
    int minimized = 0;
    int vp_x = 0, vp_y = 0, vp_w = 1360, vp_h = 840;
    int buf_w = 0, buf_h = 0;
    float dpi_scale = 1.0f;
    int is_fullscreen = 0;
    WINDOWPLACEMENT prev_wp{};

    UINT msg_taskbar_created = 0;
    ComPtr<ITaskbarList3> taskbar3;
    int thumbbar_added = 0;
    int tb_has_track = 0;
    int tb_playing = 0;
    int tb_pct = -1;
    UniqueIcon ico_prev;
    UniqueIcon ico_play;
    UniqueIcon ico_pause;
    UniqueIcon ico_next;
    std::string last_title;
    std::deque<int> media_cmds;

    int m_held = 0, m_down = 0, m_up = 0, was_down = 0;
    int m_x = 0, m_y = 0, prev_x = 0, prev_y = 0;
    int hover_x = -1, hover_y = -1;
    int track_leave = 0, cursor_hand = 0, has_frame = 0;
    int touch_reset = 0;
    int is_touch = 0;
    int idle_mode = 0;
    UINT32 active_ptr_id = 0;
    using GetPtrTypeFn = BOOL(WINAPI*)(UINT32, POINTER_INPUT_TYPE*);
    GetPtrTypeFn fn_get_ptr_type = nullptr;
    int wheel_accum = 0, wheel_frame = 0;

    uint64_t key_held = 0, key_prev = 0, key_down = 0, key_down_pending = 0;
    int hotkey = 0;
    int text_enabled = 0;
    std::deque<TextEvent> text_q;

    std::string drop_path;
    int has_drop = 0;
    std::string cfg_dir;
};

WinState g_win;

void push_text_ev(int act, const char* u8) {
    if (g_win.text_q.size() >= 64) return;
    g_win.text_q.push_back(TextEvent{ act, u8 ? u8 : "" });
}

void push_media_cmd(int cmd) {
    if (cmd == MEDIA_CMD_NONE || g_win.media_cmds.size() >= 32) return;
    g_win.media_cmds.push_back(cmd);
}

int win_system_uses_light_theme() {
    DWORD val = 0, sz = sizeof(val);
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &hk) == ERROR_SUCCESS) {
        if (RegQueryValueExW(hk, L"SystemUsesLightTheme", nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(&val), &sz) != ERROR_SUCCESS) {
            val = 0;
        }
        RegCloseKey(hk);
    }
    return val ? 1 : 0;
}

float seg_dist(float px, float py, float ax, float ay, float bx, float by) {
    const float vx = bx - ax, vy = by - ay;
    const float wx = px - ax, wy = py - ay;
    const float c1 = vx * wx + vy * wy;
    if (c1 <= 0.0f) return std::sqrt(wx * wx + wy * wy);
    const float c2 = vx * vx + vy * vy;
    if (c2 <= c1) {
        const float dx = px - bx, dy = py - by;
        return std::sqrt(dx * dx + dy * dy);
    }
    const float t = c1 / c2;
    const float qx = ax + t * vx, qy = ay + t * vy;
    const float dx = px - qx, dy = py - qy;
    return std::sqrt(dx * dx + dy * dy);
}

UniqueIcon create_media_thumb_icon(int kind, int sz, int light_bar) {
    sz = std::clamp(sz, 16, 96);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = sz;
    bi.bmiHeader.biHeight = -sz;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    uint32_t* bits = nullptr;
    HDC sdc = GetDC(nullptr);
    HBITMAP hcol = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS,
                                    reinterpret_cast<void**>(&bits), nullptr, 0);
    if (sdc) ReleaseDC(nullptr, sdc);
    if (!hcol || !bits) {
        if (hcol) DeleteObject(hcol);
        return {};
    }

    const uint8_t fg = light_bar ? 26u : 246u;
    const float hw = (sz >= 44 ? 2.15f : 1.50f) / static_cast<float>(sz);
    const float aa = 1.15f / static_cast<float>(sz);

    for (int y = 0; y < sz; ++y) {
        const float py = (static_cast<float>(y) + 0.5f) / static_cast<float>(sz);
        for (int x = 0; x < sz; ++x) {
            const float px = (static_cast<float>(x) + 0.5f) / static_cast<float>(sz);
            float d = 1.0f;
            if (kind == 0) {
                d = std::min({ seg_dist(px, py, 0.25f, 0.24f, 0.25f, 0.76f),
                               seg_dist(px, py, 0.40f, 0.50f, 0.74f, 0.25f),
                               seg_dist(px, py, 0.74f, 0.25f, 0.74f, 0.75f),
                               seg_dist(px, py, 0.74f, 0.75f, 0.40f, 0.50f) });
            } else if (kind == 1) {
                d = std::min({ seg_dist(px, py, 0.32f, 0.22f, 0.76f, 0.50f),
                               seg_dist(px, py, 0.76f, 0.50f, 0.32f, 0.78f),
                               seg_dist(px, py, 0.32f, 0.78f, 0.32f, 0.22f) });
            } else if (kind == 2) {
                d = std::min({ seg_dist(px, py, 0.26f, 0.25f, 0.60f, 0.50f),
                               seg_dist(px, py, 0.60f, 0.50f, 0.26f, 0.75f),
                               seg_dist(px, py, 0.26f, 0.75f, 0.26f, 0.25f),
                               seg_dist(px, py, 0.75f, 0.24f, 0.75f, 0.76f) });
            } else {
                d = std::min(seg_dist(px, py, 0.36f, 0.24f, 0.36f, 0.76f),
                             seg_dist(px, py, 0.64f, 0.24f, 0.64f, 0.76f));
            }
            const float cov = (hw + aa * 0.5f - d) / aa;
            if (cov <= 0.0f) {
                bits[static_cast<size_t>(y) * sz + x] = 0;
            } else {
                const float c = std::min(1.0f, cov);
                const uint8_t a = static_cast<uint8_t>(c * 255.0f + 0.5f);
                const uint8_t p = static_cast<uint8_t>(static_cast<uint32_t>(fg) * a / 255u);
                bits[static_cast<size_t>(y) * sz + x] =
                    (static_cast<uint32_t>(a) << 24) |
                    (static_cast<uint32_t>(p) << 16) |
                    (static_cast<uint32_t>(p) << 8) |
                    static_cast<uint32_t>(p);
            }
        }
    }

    const size_t mask_stride = static_cast<size_t>(((sz + 15) / 16) * 2);
    std::vector<uint8_t> mbits(mask_stride * static_cast<size_t>(sz), 0);
    HBITMAP hmask = CreateBitmap(sz, sz, 1, 1, mbits.data());

    UniqueIcon out;
    if (hmask) {
        ICONINFO ii{};
        ii.fIcon = TRUE;
        ii.hbmColor = hcol;
        ii.hbmMask = hmask;
        out.reset(CreateIconIndirect(&ii));
        DeleteObject(hmask);
    }
    DeleteObject(hcol);
    return out;
}

void taskbar_rebuild_icons(HWND hwnd) {
    using GetMetDpiFn = UINT(WINAPI*)(int, UINT);
    using GetDpiFn = UINT(WINAPI*)(HWND);

    UINT dpi = 96;
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32 && hwnd) {
        auto fn = reinterpret_cast<GetDpiFn>(reinterpret_cast<void*>(GetProcAddress(u32, "GetDpiForWindow")));
        if (fn) {
            const UINT d = fn(hwnd);
            if (d >= 72 && d <= 384) dpi = d;
        }
    }
    int sz = GetSystemMetrics(SM_CXICON);
    if (u32) {
        auto mfn = reinterpret_cast<GetMetDpiFn>(reinterpret_cast<void*>(GetProcAddress(u32, "GetSystemMetricsForDpi")));
        if (mfn) {
            const int dsz = static_cast<int>(mfn(SM_CXICON, dpi));
            if (dsz >= 16 && dsz <= 96) sz = dsz;
        }
    }
    if (sz < 24) sz = 24;
    const int light = win_system_uses_light_theme();
    g_win.ico_prev  = create_media_thumb_icon(0, sz, light);
    g_win.ico_play  = create_media_thumb_icon(1, sz, light);
    g_win.ico_next  = create_media_thumb_icon(2, sz, light);
    g_win.ico_pause = create_media_thumb_icon(3, sz, light);
}

void taskbar_fill_buttons(THUMBBUTTON btns[3]) {
    std::memset(btns, 0, sizeof(THUMBBUTTON) * 3);
    btns[0].dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
    btns[0].iId = ID_THB_PREV;
    btns[0].hIcon = g_win.ico_prev.get();
    wcscpy(btns[0].szTip, L"\x4E0A\x4E00\x9996");
    btns[0].dwFlags = THBF_ENABLED;

    const bool active_play = (g_win.tb_has_track && g_win.tb_playing);
    btns[1].dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
    btns[1].iId = ID_THB_PLAY;
    btns[1].hIcon = active_play ? g_win.ico_pause.get() : g_win.ico_play.get();
    wcscpy(btns[1].szTip, active_play ? L"\x6682\x505C" : L"\x64AD\x653E");
    btns[1].dwFlags = THBF_ENABLED;

    btns[2].dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
    btns[2].iId = ID_THB_NEXT;
    btns[2].hIcon = g_win.ico_next.get();
    wcscpy(btns[2].szTip, L"\x4E0B\x4E00\x9996");
    btns[2].dwFlags = THBF_ENABLED;
}

void taskbar_update_buttons() {
    if (!g_win.hwnd || !g_win.taskbar3 || !g_win.thumbbar_added) return;
    THUMBBUTTON btns[3];
    taskbar_fill_buttons(btns);
    g_win.taskbar3->ThumbBarUpdateButtons(g_win.hwnd, 3, btns);
}

void taskbar_on_created(HWND hwnd, int from_msg) {
    if (!hwnd) return;
    g_win.taskbar3.reset();

    ComPtr<ITaskbarList3> tb;
    if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                IID_ITaskbarList3, tb.put_void())) || !tb) {
        if (from_msg) g_win.thumbbar_added = 0;
        return;
    }
    if (FAILED(tb->HrInit())) {
        if (from_msg) g_win.thumbbar_added = 0;
        return;
    }
    g_win.taskbar3 = std::move(tb);
    taskbar_rebuild_icons(hwnd);

    THUMBBUTTON btns[3];
    taskbar_fill_buttons(btns);
    const HRESULT hr = g_win.taskbar3->ThumbBarAddButtons(hwnd, 3, btns);
    if (SUCCEEDED(hr)) {
        g_win.thumbbar_added = 1;
    } else if (from_msg && g_win.thumbbar_added) {
        g_win.taskbar3->ThumbBarUpdateButtons(hwnd, 3, btns);
    } else {
        g_win.thumbbar_added = 0;
    }
}

void taskbar_free() {
    g_win.taskbar3.reset();
    g_win.thumbbar_added = 0;
    g_win.ico_prev.reset();
    g_win.ico_play.reset();
    g_win.ico_pause.reset();
    g_win.ico_next.reset();
}

void refresh_dpi_scale(HWND hwnd) {
    using GetDpiFn = UINT(WINAPI*)(HWND);
    UINT dpi = 96;
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32 && hwnd) {
        auto fn = reinterpret_cast<GetDpiFn>(reinterpret_cast<void*>(GetProcAddress(u32, "GetDpiForWindow")));
        if (fn) {
            const UINT d = fn(hwnd);
            if (d >= 72 && d <= 384) dpi = d;
        }
    }
    g_win.dpi_scale = std::clamp(static_cast<float>(dpi) / 96.0f, 1.0f, 2.5f);
}

void update_viewport(int cw, int ch) {
    if (cw <= 0 || ch <= 0) return;
    g_win.vp_x = 0;
    g_win.vp_y = 0;
    g_win.vp_w = std::clamp(cw, 320, 8192);
    g_win.vp_h = std::clamp(ch, 240, 8192);
}

void map_mouse_xy(int wx, int wy, int* ox, int* oy) {
    const float s = g_win.dpi_scale > 0.0f ? g_win.dpi_scale : 1.0f;
    const int lw = std::max(1, static_cast<int>(static_cast<float>(g_win.vp_w) / s + 0.5f));
    const int lh = std::max(1, static_cast<int>(static_cast<float>(g_win.vp_h) / s + 0.5f));
    *ox = std::clamp(static_cast<int>(static_cast<float>(wx) / s), 0, lw - 1);
    *oy = std::clamp(static_cast<int>(static_cast<float>(wy) / s), 0, lh - 1);
}

uint64_t vk_to_btn(WPARAM vk) {
    switch (vk) {
    case VK_UP:       return BTN_UP;
    case VK_DOWN:     return BTN_DOWN;
    case VK_LEFT:     return BTN_LEFT;
    case VK_RIGHT:    return BTN_RIGHT;
    case VK_RETURN:   return BTN_A;
    case VK_ESCAPE:   return BTN_B;
    case 'A':         return BTN_A;
    case 'B':         return BTN_B;
    case 'X':         return BTN_X;
    case 'Y':         return BTN_Y;
    case 'T':         return BTN_ZR;
    case 'H':         return BTN_ZL;
    case VK_SPACE:    return BTN_A;
    case VK_OEM_PLUS:
    case VK_ADD:      return BTN_STICK_R_UP;
    case VK_OEM_MINUS:
    case VK_SUBTRACT: return BTN_STICK_R_DOWN;
    default:          return 0;
    }
}

void handle_wchar(wchar_t ch) {
    if (!g_win.text_enabled) return;
    static wchar_t hi_surr = 0;
    if (ch >= 0xD800 && ch <= 0xDBFF) { hi_surr = ch; return; }
    wchar_t wbuf[3] = {0};
    int wn = 0;
    if (ch >= 0xDC00 && ch <= 0xDFFF && hi_surr) {
        wbuf[0] = hi_surr; wbuf[1] = ch; wn = 2;
        hi_surr = 0;
    } else {
        hi_surr = 0;
        if (ch < 0x20 || ch == 0x7F) return;
        wbuf[0] = ch; wn = 1;
    }
    char u8[16] = {0};
    WideCharToMultiByte(CP_UTF8, 0, wbuf, wn, u8, sizeof(u8) - 1, nullptr, nullptr);
    if (u8[0]) push_text_ev(0, u8);
}

bool ensure_win_buffers(int w, int h) {
    w = std::clamp(w, 160, 8192);
    h = std::clamp(h, 90, 8192);
    if (w == g_win.buf_w && h == g_win.buf_h && !g_win.backbuf.empty() && !g_win.bgra.empty()) return true;
    try {
        std::vector<uint32_t> nb(static_cast<size_t>(w) * h, 0);
        std::vector<uint32_t> ng(static_cast<size_t>(w) * h, 0);
        g_win.backbuf.swap(nb);
        g_win.bgra.swap(ng);
    } catch (...) {
        return false;
    }
    g_win.buf_w = w;
    g_win.buf_h = h;
    g_win.has_frame = 0;
    std::memset(&g_win.bmi, 0, sizeof(g_win.bmi));
    g_win.bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    g_win.bmi.bmiHeader.biWidth = w;
    g_win.bmi.bmiHeader.biHeight = -h;
    g_win.bmi.bmiHeader.biPlanes = 1;
    g_win.bmi.bmiHeader.biBitCount = 32;
    g_win.bmi.bmiHeader.biCompression = BI_RGB;
    return true;
}

void present_to_hdc(HDC hdc, HWND hwnd) {
    if (!hdc || !hwnd || !g_win.has_frame || g_win.bgra.empty() ||
        g_win.buf_w <= 0 || g_win.buf_h <= 0) return;

    RECT cr{};
    GetClientRect(hwnd, &cr);
    const int cw = cr.right - cr.left, ch = cr.bottom - cr.top;
    if (g_win.vp_x > 0 || g_win.vp_y > 0) {
        HBRUSH br = CreateSolidBrush(RGB(22, 26, 34));
        if (g_win.vp_x > 0) {
            RECT r1 = { 0, 0, g_win.vp_x, ch }; FillRect(hdc, &r1, br);
            RECT r2 = { g_win.vp_x + g_win.vp_w, 0, cw, ch }; FillRect(hdc, &r2, br);
        }
        if (g_win.vp_y > 0) {
            RECT r3 = { 0, 0, cw, g_win.vp_y }; FillRect(hdc, &r3, br);
            RECT r4 = { 0, g_win.vp_y + g_win.vp_h, cw, ch }; FillRect(hdc, &r4, br);
        }
        DeleteObject(br);
    }
    if (g_win.vp_w == g_win.buf_w && g_win.vp_h == g_win.buf_h) {
        SetDIBitsToDevice(hdc, g_win.vp_x, g_win.vp_y, g_win.buf_w, g_win.buf_h,
                          0, 0, 0, g_win.buf_h, g_win.bgra.data(), &g_win.bmi, DIB_RGB_COLORS);
    } else {
        SetStretchBltMode(hdc, HALFTONE);
        SetBrushOrgEx(hdc, 0, 0, nullptr);
        StretchDIBits(hdc, g_win.vp_x, g_win.vp_y, g_win.vp_w, g_win.vp_h,
                      0, 0, g_win.buf_w, g_win.buf_h, g_win.bgra.data(), &g_win.bmi, DIB_RGB_COLORS, SRCCOPY);
    }
}

LRESULT CALLBACK win_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_win.msg_taskbar_created != 0 && msg == g_win.msg_taskbar_created) {
        taskbar_on_created(hwnd, 1);
        return 0;
    }
    switch (msg) {
    case WM_CLOSE:
    case WM_DESTROY:
        g_win.running = 0;
        PostQuitMessage(0);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        present_to_hdc(hdc, hwnd);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(g_win.cursor_hand ? IDC_HAND : IDC_ARROW)));
            return TRUE;
        }
        break;
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        if (hwnd) refresh_dpi_scale(hwnd);
        LONG mw = static_cast<LONG>(840 * g_win.dpi_scale);
        LONG mh = static_cast<LONG>(520 * g_win.dpi_scale);
        HMONITOR hm = MonitorFromWindow(hwnd ? hwnd : GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (hm && GetMonitorInfoW(hm, &mi)) {
            const LONG ww = mi.rcWork.right - mi.rcWork.left;
            const LONG wh = mi.rcWork.bottom - mi.rcWork.top;
            if (ww > 400 && mw > ww * 85 / 100) mw = ww * 85 / 100;
            if (wh > 300 && mh > wh * 85 / 100) mh = wh * 85 / 100;
        }
        mm->ptMinTrackSize.x = mw;
        mm->ptMinTrackSize.y = mh;
        return 0;
    }
    case WM_DPICHANGED: {
        const UINT dpi = HIWORD(wp);
        if (dpi >= 72 && dpi <= 384) {
            g_win.dpi_scale = std::clamp(static_cast<float>(dpi) / 96.0f, 1.0f, 2.5f);
        }
        const auto* prc = reinterpret_cast<const RECT*>(lp);
        if (prc && !g_win.is_fullscreen) {
            SetWindowPos(hwnd, nullptr, prc->left, prc->top,
                         prc->right - prc->left, prc->bottom - prc->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (g_win.thumbbar_added) {
            taskbar_rebuild_icons(hwnd);
            taskbar_update_buttons();
        }
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (g_win.thumbbar_added && lp &&
            std::wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) {
            taskbar_rebuild_icons(hwnd);
            taskbar_update_buttons();
        }
        return 0;
    case WM_COMMAND:
        if (HIWORD(wp) == THBN_CLICKED) {
            switch (LOWORD(wp)) {
            case ID_THB_PREV: push_media_cmd(MEDIA_CMD_PREV);   return 0;
            case ID_THB_PLAY: push_media_cmd(MEDIA_CMD_TOGGLE); return 0;
            case ID_THB_NEXT: push_media_cmd(MEDIA_CMD_NEXT);   return 0;
            default: break;
            }
        }
        break;
    case WM_APPCOMMAND: {
        const short cmd = GET_APPCOMMAND_LPARAM(lp);
        if (cmd == APPCOMMAND_MEDIA_PLAY_PAUSE || cmd == APPCOMMAND_MEDIA_PLAY || cmd == APPCOMMAND_MEDIA_PAUSE) {
            push_media_cmd(MEDIA_CMD_TOGGLE);
            return TRUE;
        }
        if (cmd == APPCOMMAND_MEDIA_PREVIOUSTRACK) { push_media_cmd(MEDIA_CMD_PREV); return TRUE; }
        if (cmd == APPCOMMAND_MEDIA_NEXTTRACK)     { push_media_cmd(MEDIA_CMD_NEXT); return TRUE; }
        if (cmd == APPCOMMAND_MEDIA_STOP)          { push_media_cmd(MEDIA_CMD_PAUSE); return TRUE; }
        break;
    }
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) {
            g_win.minimized = 1;
            return 0;
        }
        g_win.minimized = 0;
        refresh_dpi_scale(hwnd);
        update_viewport(LOWORD(lp), HIWORD(lp));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_CAPTURECHANGED:
        g_win.m_held = 0;
        g_win.active_ptr_id = 0;
        return 0;
    case WM_POINTERDOWN:
    case WM_POINTERUPDATE:
    case WM_POINTERUP:
    case WM_POINTERCAPTURECHANGED: {
        if (!g_win.fn_get_ptr_type) break;
        const UINT32 pid = GET_POINTERID_WPARAM(wp);
        POINTER_INPUT_TYPE ptype = PT_MOUSE;
        if (!g_win.fn_get_ptr_type(pid, &ptype)) break;
        if (ptype != PT_TOUCH && ptype != PT_PEN) break;

        if (msg == WM_POINTERDOWN) {
            if (g_win.active_ptr_id != 0 && g_win.active_ptr_id != pid && !IS_POINTER_PRIMARY_WPARAM(wp)) {
                return 0;
            }
            g_win.active_ptr_id = pid;
            g_win.is_touch = (ptype == PT_TOUCH) ? 1 : 0;
            g_win.idle_mode = 0;
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(hwnd, &pt);
            map_mouse_xy(pt.x, pt.y, &g_win.m_x, &g_win.m_y);
            g_win.hover_x = g_win.m_x;
            g_win.hover_y = g_win.m_y;
            g_win.m_held = 1;
            g_win.m_down = 1;
            SetFocus(hwnd);
            return 0;
        }
        if (pid != g_win.active_ptr_id) return 0;
        g_win.idle_mode = 0;
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        map_mouse_xy(pt.x, pt.y, &g_win.m_x, &g_win.m_y);
        if (msg == WM_POINTERUPDATE) {
            if (g_win.m_held) {
                g_win.hover_x = g_win.m_x;
                g_win.hover_y = g_win.m_y;
            }
            return 0;
        }
        g_win.m_held = 0;
        g_win.m_up = 1;
        g_win.active_ptr_id = 0;
        if (g_win.is_touch) {
            g_win.hover_x = -1;
            g_win.hover_y = -1;
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (g_win.fn_get_ptr_type && (static_cast<ULONG_PTR>(GetMessageExtraInfo()) & 0xFFFFFF00UL) == 0xFF515700UL) {
            return 0;
        }
        g_win.is_touch = 0;
        g_win.idle_mode = 0;
        SetCapture(hwnd);
        map_mouse_xy(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &g_win.m_x, &g_win.m_y);
        g_win.hover_x = g_win.m_x;
        g_win.hover_y = g_win.m_y;
        g_win.m_held = 1;
        g_win.m_down = 1;
        return 0;
    case WM_MOUSEMOVE:
        if (g_win.fn_get_ptr_type && (static_cast<ULONG_PTR>(GetMessageExtraInfo()) & 0xFFFFFF00UL) == 0xFF515700UL) {
            return 0;
        }
        g_win.is_touch = 0;
        g_win.idle_mode = 0;
        if (!g_win.track_leave) {
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            tme.dwHoverTime = 0;
            if (TrackMouseEvent(&tme)) g_win.track_leave = 1;
        }
        map_mouse_xy(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &g_win.hover_x, &g_win.hover_y);
        if (g_win.m_held) {
            g_win.m_x = g_win.hover_x;
            g_win.m_y = g_win.hover_y;
        }
        return 0;
    case WM_MOUSELEAVE:
        g_win.track_leave = 0;
        g_win.hover_x = -1;
        g_win.hover_y = -1;
        return 0;
    case WM_LBUTTONUP:
        if (g_win.fn_get_ptr_type && (static_cast<ULONG_PTR>(GetMessageExtraInfo()) & 0xFFFFFF00UL) == 0xFF515700UL) {
            return 0;
        }
        if (GetCapture() == hwnd) ReleaseCapture();
        map_mouse_xy(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &g_win.m_x, &g_win.m_y);
        g_win.m_held = 0;
        g_win.m_up = 1;
        return 0;
    case WM_MOUSEWHEEL:
        g_win.idle_mode = 0;
        g_win.wheel_accum += GET_WHEEL_DELTA_WPARAM(wp);
        return 0;
    case WM_CHAR:
        handle_wchar(static_cast<wchar_t>(wp));
        return 0;
    case WM_KEYDOWN: {
        if (wp == VK_MEDIA_PLAY_PAUSE) { push_media_cmd(MEDIA_CMD_TOGGLE); return 0; }
        if (wp == VK_MEDIA_PREV_TRACK) { push_media_cmd(MEDIA_CMD_PREV);   return 0; }
        if (wp == VK_MEDIA_NEXT_TRACK) { push_media_cmd(MEDIA_CMD_NEXT);   return 0; }
        if (wp == VK_MEDIA_STOP)       { push_media_cmd(MEDIA_CMD_PAUSE);  return 0; }
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (g_win.text_enabled) {
            if (wp == VK_BACK)   { push_text_ev(1, nullptr); return 0; }
            if (wp == VK_RETURN) { push_text_ev(2, nullptr); return 0; }
            if (wp == VK_ESCAPE) { push_text_ev(3, nullptr); return 0; }
            if (wp == 'V' && ctrl) {
                if (OpenClipboard(hwnd)) {
                    HANDLE hd = GetClipboardData(CF_UNICODETEXT);
                    if (hd) {
                        const auto* ws = static_cast<const wchar_t*>(GlobalLock(hd));
                        if (ws) {
                            char u8[256]{};
                            plat_wide_to_utf8(ws, u8, sizeof(u8));
                            for (size_t i = 0; u8[i]; ) {
                                const unsigned char c = static_cast<unsigned char>(u8[i]);
                                const int len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
                                if (u8[i] != '\r' && u8[i] != '\n') {
                                    char ch[8] = {0};
                                    for (int k = 0; k < len && u8[i + k]; k++) ch[k] = u8[i + k];
                                    push_text_ev(0, ch);
                                }
                                i += static_cast<size_t>(len);
                            }
                            GlobalUnlock(hd);
                        }
                    }
                    CloseClipboard();
                }
                return 0;
            }
            return 0;
        }
        if (wp == VK_F11) { be_toggle_fullscreen(); return 0; }
        if (ctrl && wp == 'F') { g_win.hotkey = 's'; return 0; }
        if (ctrl && wp == 'O') { g_win.hotkey = 'f'; return 0; }
        if (wp == VK_OEM_2 || wp == 'S') { g_win.hotkey = 's'; return 0; }
        if (wp == 'F') { g_win.key_down_pending |= BTN_X; return 0; }
        if (wp == 'O') { g_win.hotkey = 'f'; return 0; }
        if (wp == 'M') { g_win.hotkey = 'm'; return 0; }
        if (wp == 'D') { g_win.hotkey = 'c'; return 0; }
        if (wp == 'V') { g_win.hotkey = 'v'; return 0; }
        if (wp == VK_PRIOR) { g_win.wheel_accum += 360; return 0; }
        if (wp == VK_NEXT)  { g_win.wheel_accum -= 360; return 0; }
        const uint64_t b = vk_to_btn(wp);
        if (b) {
            if (!(g_win.key_held & b)) g_win.key_down_pending |= b;
            g_win.key_held |= b;
        }
        return 0;
    }
    case WM_KEYUP: {
        const uint64_t b = vk_to_btn(wp);
        if (b) g_win.key_held &= ~b;
        return 0;
    }
    case WM_KILLFOCUS:
        g_win.key_held = 0;
        g_win.m_held = 0;
        g_win.active_ptr_id = 0;
        return 0;
    case WM_DROPFILES: {
        auto hd = reinterpret_cast<HDROP>(wp);
        wchar_t wpath[600]{};
        if (DragQueryFileW(hd, 0, wpath, 600) > 0) {
            char u8[600]{};
            plat_wide_to_utf8(wpath, u8, sizeof(u8));
            g_win.drop_path = u8;
            g_win.has_drop = 1;
        }
        DragFinish(hd);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int be_init(void) {
    using SetDpiCtxFn = BOOL(WINAPI*)(HANDLE);
    using SetDpiFn = BOOL(WINAPI*)();
    using GetSysDpiFn = UINT(WINAPI*)(void);
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    float init_scale = 1.0f;
    if (u32) {
        auto fn2 = reinterpret_cast<SetDpiCtxFn>(reinterpret_cast<void*>(GetProcAddress(u32, "SetProcessDpiAwarenessContext")));
        if (!fn2 || !fn2(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-4)))) {
            auto fn = reinterpret_cast<SetDpiFn>(reinterpret_cast<void*>(GetProcAddress(u32, "SetProcessDPIAware")));
            if (fn) fn();
        }
        g_win.fn_get_ptr_type = reinterpret_cast<WinState::GetPtrTypeFn>(
            reinterpret_cast<void*>(GetProcAddress(u32, "GetPointerType")));
        auto gdpi = reinterpret_cast<GetSysDpiFn>(reinterpret_cast<void*>(GetProcAddress(u32, "GetDpiForSystem")));
        if (gdpi) {
            const UINT d = gdpi();
            if (d >= 72 && d <= 384) init_scale = std::clamp(static_cast<float>(d) / 96.0f, 1.0f, 2.5f);
        }
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    timeBeginPeriod(1);

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    HICON ico_big = static_cast<HICON>(LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                  GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    HICON ico_sm = static_cast<HICON>(LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                 GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    if (!ico_big) ico_big = LoadIconW(hInst, MAKEINTRESOURCEW(1));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = win_proc;
    wc.hInstance = hInst;
    wc.hIcon = ico_big;
    wc.hIconSm = ico_sm ? ico_sm : ico_big;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.lpszClassName = L"SirenePlayerWindow";
    RegisterClassExW(&wc);

    RECT wa = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    HMONITOR hm = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (hm && GetMonitorInfoW(hm, &mi)) wa = mi.rcWork;
    const int work_w = wa.right - wa.left;
    const int work_h = wa.bottom - wa.top;

    int init_w = static_cast<int>(1280.0f * init_scale);
    int init_h = static_cast<int>(800.0f * init_scale);
    if (work_w > 0 && work_h > 0) {
        const int max_w = static_cast<int>(work_w * 0.88);
        const int max_h = static_cast<int>(work_h * 0.88);
        if (init_w > max_w) init_w = max_w;
        if (init_h > max_h) init_h = max_h;
        if (init_w < 960 && work_w >= 960) init_w = 960;
        if (init_h < 600 && work_h >= 600) init_h = 600;
    }
    RECT rc = { 0, 0, init_w, init_h };
    const DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    AdjustWindowRect(&rc, style, FALSE);
    const int win_w = rc.right - rc.left;
    const int win_h = rc.bottom - rc.top;
    const int win_x = std::max(wa.left, wa.left + (work_w - win_w) / 2);
    const int win_y = std::max(wa.top, wa.top + (work_h - win_h) / 2);

    g_win.msg_taskbar_created = RegisterWindowMessageW(L"TaskbarButtonCreated");
    g_win.hwnd = CreateWindowExW(
        WS_EX_ACCEPTFILES,
        L"SirenePlayerWindow",
        L"Sir\x00E8ne \x00B7 \x672C\x5730\x97F3\x4E50\x64AD\x653E\x5668",
        style,
        win_x, win_y,
        win_w, win_h,
        nullptr, nullptr, hInst, nullptr);
    if (!g_win.hwnd) return -1;

    // Disable Windows press-and-hold right-click delay ring and pen flicks for Surface touch responsiveness
    SetPropW(g_win.hwnd, L"MicrosoftTabletPenServiceProperty",
             reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x00010019)));

    if (ico_big) SendMessageW(g_win.hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(ico_big));
    if (ico_sm || ico_big) SendMessageW(g_win.hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(ico_sm ? ico_sm : ico_big));

    if (u32) {
        using ChgMsgFltExFn = BOOL(WINAPI*)(HWND, UINT, DWORD, PVOID);
        auto flt = reinterpret_cast<ChgMsgFltExFn>(reinterpret_cast<void*>(GetProcAddress(u32, "ChangeWindowMessageFilterEx")));
        if (flt) {
            if (g_win.msg_taskbar_created) flt(g_win.hwnd, g_win.msg_taskbar_created, 1, nullptr);
            flt(g_win.hwnd, WM_COMMAND, 1, nullptr);
            flt(g_win.hwnd, WM_APPCOMMAND, 1, nullptr);
        }
    }

    refresh_dpi_scale(g_win.hwnd);
    RECT cr0{};
    GetClientRect(g_win.hwnd, &cr0);
    update_viewport(cr0.right - cr0.left, cr0.bottom - cr0.top);
    if (!ensure_win_buffers(g_win.vp_w, g_win.vp_h)) return -1;
    return 0;
}

void be_exit(void) {
    taskbar_free();
    g_win.backbuf.clear();
    g_win.bgra.clear();
    g_win.buf_w = g_win.buf_h = 0;
    if (g_win.hwnd) { DestroyWindow(g_win.hwnd); g_win.hwnd = nullptr; }
    timeEndPeriod(1);
    CoUninitialize();
}

void be_viewport_size(int* out_w, int* out_h) {
    if (out_w) *out_w = std::max(320, g_win.vp_w);
    if (out_h) *out_h = std::max(240, g_win.vp_h);
}

void be_screen_metrics(int* out_phys_w, int* out_phys_h, float* out_scale) {
    if (out_phys_w) *out_phys_w = std::max(320, g_win.vp_w);
    if (out_phys_h) *out_phys_h = std::max(240, g_win.vp_h);
    if (out_scale)  *out_scale  = g_win.dpi_scale;
}

void be_set_dark_titlebar(int dark) {
    if (!g_win.hwnd) return;
    using DwmSetAttrFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    static HMODULE d = LoadLibraryW(L"dwmapi.dll");
    if (!d) return;
    auto fn = reinterpret_cast<DwmSetAttrFn>(reinterpret_cast<void*>(GetProcAddress(d, "DwmSetWindowAttribute")));
    if (fn) {
        const BOOL v = dark ? TRUE : FALSE;
        if (FAILED(fn(g_win.hwnd, 20, &v, sizeof(v)))) fn(g_win.hwnd, 19, &v, sizeof(v));
    }
}

void be_toggle_fullscreen(void) {
    if (!g_win.hwnd) return;
    const DWORD st = static_cast<DWORD>(GetWindowLongW(g_win.hwnd, GWL_STYLE));
    if (!g_win.is_fullscreen) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        g_win.prev_wp.length = sizeof(g_win.prev_wp);
        if (GetWindowPlacement(g_win.hwnd, &g_win.prev_wp) &&
            GetMonitorInfoW(MonitorFromWindow(g_win.hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLongW(g_win.hwnd, GWL_STYLE, static_cast<LONG>(st & ~WS_OVERLAPPEDWINDOW));
            SetWindowPos(g_win.hwnd, HWND_TOP,
                         mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            g_win.is_fullscreen = 1;
        }
    } else {
        SetWindowLongW(g_win.hwnd, GWL_STYLE, static_cast<LONG>(st | WS_OVERLAPPEDWINDOW));
        SetWindowPlacement(g_win.hwnd, &g_win.prev_wp);
        SetWindowPos(g_win.hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_win.is_fullscreen = 0;
    }
}

int be_is_fullscreen(void) { return g_win.is_fullscreen; }

uint32_t* be_begin(int* stride) {
    if (g_win.minimized || (g_win.hwnd && IsIconic(g_win.hwnd))) {
        if (stride) *stride = g_win.buf_w;
        return g_win.backbuf.empty() ? nullptr : g_win.backbuf.data();
    }
    ensure_win_buffers(g_win.vp_w, g_win.vp_h);
    if (stride) *stride = g_win.buf_w;
    return g_win.backbuf.empty() ? nullptr : g_win.backbuf.data();
}

void be_end(void) {
    if (g_win.hwnd && !g_win.minimized && !IsIconic(g_win.hwnd) &&
        !g_win.backbuf.empty() && !g_win.bgra.empty() && g_win.buf_w > 0 && g_win.buf_h > 0) {
        const size_t n = static_cast<size_t>(g_win.buf_w) * g_win.buf_h;
        for (size_t i = 0; i < n; ++i) {
            const uint32_t p = g_win.backbuf[i];
            g_win.bgra[i] = (p & 0xFF00FF00u) | ((p & 0xFFu) << 16) | ((p >> 16) & 0xFFu);
        }
        g_win.has_frame = 1;
        HDC hdc = GetDC(g_win.hwnd);
        if (hdc) {
            present_to_hdc(hdc, g_win.hwnd);
            ReleaseDC(g_win.hwnd, hdc);
        }
    }

    const uint64_t frame_ns = g_win.idle_mode ? 40000000ULL : 16666667ULL;
    static uint64_t next_frame_ns = 0;
    uint64_t now = plat_ticks_ns();
    if (next_frame_ns == 0 || now > next_frame_ns + frame_ns * 2ULL) {
        next_frame_ns = now + frame_ns;
    } else {
        while (now < next_frame_ns) {
            const uint64_t rem_ns = next_frame_ns - now;
            const DWORD rem_ms = static_cast<DWORD>(rem_ns / 1000000ULL);
            if (g_win.idle_mode && rem_ms > 2) {
                const DWORD res = MsgWaitForMultipleObjects(0, nullptr, FALSE, rem_ms - 1, QS_INPUT);
                if (res == WAIT_OBJECT_0) {
                    next_frame_ns = plat_ticks_ns();
                    break;
                }
            } else if (rem_ms > 1) {
                Sleep(rem_ms - 1);
            } else {
                Sleep(1);
            }
            now = plat_ticks_ns();
        }
        next_frame_ns += frame_ns;
    }
}

void be_input_update(void) {
    MSG msg;
    g_win.wheel_frame = 0;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) g_win.running = 0;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_win.wheel_accum >= 120 || g_win.wheel_accum <= -120) {
        g_win.wheel_frame = g_win.wheel_accum / 120;
        g_win.wheel_accum %= 120;
    }
    g_win.key_down = g_win.key_down_pending | (g_win.key_held & ~g_win.key_prev);
    g_win.key_down_pending = 0;
    g_win.key_prev = g_win.key_held;
}

uint64_t be_btn_down(void) { return g_win.key_down; }
uint64_t be_btn_held(void) { return g_win.key_held; }
void be_sleep_ms(int ms) { Sleep(static_cast<DWORD>(ms)); }
int be_should_run(void) { return g_win.running; }

void be_touch_update(TouchInfo* out) {
    std::memset(out, 0, sizeof(*out));
    if (g_win.touch_reset) {
        g_win.was_down = 0;
        g_win.m_down = 0;
        g_win.m_up = 0;
        g_win.touch_reset = 0;
        return;
    }
    if (g_win.m_held || g_win.m_down) {
        out->held = 1;
        out->x = g_win.m_x;
        out->y = g_win.m_y;
        if (!g_win.was_down || g_win.m_down) {
            out->down = 1;
            g_win.prev_x = g_win.m_x;
            g_win.prev_y = g_win.m_y;
        }
        out->dx = g_win.m_x - g_win.prev_x;
        out->dy = g_win.m_y - g_win.prev_y;
        g_win.prev_x = g_win.m_x;
        g_win.prev_y = g_win.m_y;
        g_win.was_down = 1;
        g_win.m_down = 0;
    } else if (g_win.was_down || g_win.m_up) {
        out->up = 1;
        out->x = g_win.m_x;
        out->y = g_win.m_y;
        g_win.was_down = 0;
        g_win.m_up = 0;
    }
}

void be_touch_reset(void) { g_win.touch_reset = 1; }
int be_is_desktop(void) { return 1; }
int be_wheel_delta(void) { return g_win.wheel_frame; }

const char* be_poll_dropped_path(void) {
    if (!g_win.has_drop) return nullptr;
    g_win.has_drop = 0;
    return g_win.drop_path.c_str();
}

int be_pick_folder(char* out, size_t cap) {
    BROWSEINFOW bi{};
    bi.hwndOwner = g_win.hwnd;
    bi.lpszTitle = L"\x9009\x62E9 Sir\x00E8ne \x672C\x5730\x97F3\x4E50\x6587\x4EF6\x5939";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return 0;
    wchar_t wdir[600]{};
    const BOOL ok = SHGetPathFromIDListW(pidl, wdir);
    CoTaskMemFree(pidl);
    if (!ok) return 0;
    plat_wide_to_utf8(wdir, out, static_cast<int>(cap));
    for (char* p = out; *p; ++p) if (*p == '\\') *p = '/';
    be_touch_reset();
    return out[0] ? 1 : 0;
}

void be_text_input(int enable) {
    g_win.text_enabled = enable;
    if (enable) {
        g_win.text_q.clear();
        g_win.key_held = 0;
    }
}

int be_poll_text_char(char* utf8_out, size_t cap, int* action) {
    if (g_win.text_q.empty()) return 0;
    TextEvent e = g_win.text_q.front();
    g_win.text_q.pop_front();
    if (action) *action = e.action;
    if (utf8_out && cap > 0) snprintf(utf8_out, cap, "%s", e.text.c_str());
    return 1;
}

int be_poll_hotkey(void) {
    const int h = g_win.hotkey;
    g_win.hotkey = 0;
    return h;
}

const char* be_config_dir(void) {
    if (g_win.cfg_dir.empty()) {
        wchar_t wapp[480]{};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, wapp))) {
            char u8[480]{};
            plat_wide_to_utf8(wapp, u8, sizeof(u8));
            for (char* p = u8; *p; ++p) if (*p == '\\') *p = '/';
            g_win.cfg_dir = std::string(u8) + "/Sirene";
        } else {
            g_win.cfg_dir = "./Sirene_config";
        }
    }
    return g_win.cfg_dir.c_str();
}

void be_set_cli_path(const char* p) {
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv) {
        if (wargc > 1 && wargv[1] && wargv[1][0]) {
            char u8[600]{};
            plat_wide_to_utf8(wargv[1], u8, sizeof(u8));
            for (char* q = u8; *q; ++q) if (*q == '\\') *q = '/';
            g_win.drop_path = u8;
            g_win.has_drop = 1;
            LocalFree(wargv);
            return;
        }
        LocalFree(wargv);
    }
    if (!p || !*p) return;
    g_win.drop_path = p;
    std::replace(g_win.drop_path.begin(), g_win.drop_path.end(), '\\', '/');
    g_win.has_drop = 1;
}

int be_default_music_dirs(char dirs[][600], int max_dirs) {
    int n = 0;
    wchar_t wmus[512]{};
    if (n < max_dirs && SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_MYMUSIC, nullptr, 0, wmus))) {
        plat_wide_to_utf8(wmus, dirs[n], 600);
        for (char* p = dirs[n]; *p; ++p) if (*p == '\\') *p = '/';
        n++;
    }
    if (n < max_dirs) snprintf(dirs[n++], 600, "./music");
    return n;
}

void be_mouse_pos(int* x, int* y) {
    if (x) *x = g_win.hover_x;
    if (y) *y = g_win.hover_y;
}

void be_set_cursor(int hand) {
    const int h = hand ? 1 : 0;
    if (h != g_win.cursor_hand) {
        g_win.cursor_hand = h;
        SetCursor(LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(h ? IDC_HAND : IDC_ARROW)));
    }
}

int be_ensure_desktop_shortcut(int only_repair) {
    wchar_t wdesk[600]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, wdesk)) || !wdesk[0]) {
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_DESKTOP, nullptr, 0, wdesk)) || !wdesk[0]) return 0;
    }
    wchar_t wexe[600]{};
    const DWORD n = GetModuleFileNameW(nullptr, wexe, 600);
    if (n == 0 || n >= 600) return 0;

    const size_t dlen = std::wcslen(wdesk);
    if (_wcsnicmp(wexe, wdesk, dlen) == 0 && (wexe[dlen] == L'\\' || wexe[dlen] == L'/')) {
        return 0;
    }

    std::wstring wdir(wexe);
    const size_t pos = wdir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) wdir.resize(pos);

    std::wstring wlnk = std::wstring(wdesk) + L"\\Sir\x00E8ne.lnk";
    const DWORD attr = GetFileAttributesW(wlnk.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES && only_repair) return 0;

    ComPtr<IShellLinkW> psl;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, psl.put_void())) || !psl) {
        return 0;
    }
    ComPtr<IPersistFile> ppf;
    if (FAILED(psl->QueryInterface(IID_IPersistFile, ppf.put_void())) || !ppf) return 0;

    bool need_save = true;
    if (attr != INVALID_FILE_ATTRIBUTES) {
        need_save = false;
        if (SUCCEEDED(ppf->Load(wlnk.c_str(), STGM_READ))) {
            wchar_t old_tgt[600]{};
            if (SUCCEEDED(psl->GetPath(old_tgt, 600, nullptr, 0)) && old_tgt[0]) {
                if (GetFileAttributesW(old_tgt) == INVALID_FILE_ATTRIBUTES) need_save = true;
            }
        }
    }
    if (!need_save) return 0;

    psl->SetPath(wexe);
    psl->SetWorkingDirectory(wdir.c_str());
    psl->SetIconLocation(wexe, 0);
    psl->SetDescription(L"Sir\x00E8ne \x00B7 \x9AD8\x6E05\x672C\x5730\x97F3\x4E50\x64AD\x653E\x5668");
    return SUCCEEDED(ppf->Save(wlnk.c_str(), TRUE)) ? 1 : 0;
}

void be_buffer_size(int* out_w, int* out_h) {
    const int ok = !g_win.backbuf.empty();
    if (out_w) *out_w = ok ? g_win.buf_w : 0;
    if (out_h) *out_h = ok ? g_win.buf_h : 0;
}

int be_is_minimized(void) {
    return g_win.minimized || (g_win.hwnd && IsIconic(g_win.hwnd));
}

int be_poll_media_cmd(void) {
    if (g_win.media_cmds.empty()) return MEDIA_CMD_NONE;
    const int cmd = g_win.media_cmds.front();
    g_win.media_cmds.pop_front();
    return cmd;
}

void be_set_media_state(int has_track, int playing, float frac, const char* title, const char* artist) {
    if (!g_win.hwnd) return;
    if (!g_win.thumbbar_added) {
        static int retry_ticks = 0;
        if (retry_ticks == 45) taskbar_on_created(g_win.hwnd, 0);
        if (retry_ticks < 60) ++retry_ticks;
    }

    const int ht = has_track ? 1 : 0;
    const int pl = (ht && playing) ? 1 : 0;
    const int state_changed = (ht != g_win.tb_has_track || pl != g_win.tb_playing);
    g_win.tb_has_track = ht;
    g_win.tb_playing = pl;

    char cap[512]{};
    if (ht && title && title[0]) {
        if (artist && artist[0]) snprintf(cap, sizeof(cap), "%.180s - %.100s \xC2\xB7 Sir\xC3\xA8ne", title, artist);
        else                     snprintf(cap, sizeof(cap), "%.180s \xC2\xB7 Sir\xC3\xA8ne", title);
    } else {
        snprintf(cap, sizeof(cap), "Sir\xC3\xA8ne \xC2\xB7 \xE6\x9C\xAC\xE5\x9C\xB0\xE9\x9F\xB3\xE4\xB9\x90\xE6\x92\xAD\xE6\x94\xBE\xE5\x99\xA8");
    }

    const bool title_changed = (g_win.last_title != cap);
    if (title_changed) {
        g_win.last_title = cap;
        wchar_t wcap[512]{};
        MultiByteToWideChar(CP_UTF8, 0, cap, -1, wcap, 511);
        if (wcap[0]) SetWindowTextW(g_win.hwnd, wcap);
    }

    if (g_win.taskbar3) {
        if (state_changed && g_win.thumbbar_added) taskbar_update_buttons();
        if (title_changed || state_changed) {
            char tip[512]{};
            if (ht && title && title[0]) {
                const char* icon = pl ? "\xE2\x96\xB6 " : "\xE2\x8F\xB8 ";
                if (artist && artist[0]) snprintf(tip, sizeof(tip), "%s%.180s - %.100s", icon, title, artist);
                else                     snprintf(tip, sizeof(tip), "%s%.180s", icon, title);
            } else {
                snprintf(tip, sizeof(tip), "Sir\xC3\xA8ne \xC2\xB7 \xE6\x9C\xAC\xE5\x9C\xB0\xE9\x9F\xB3\xE4\xB9\x90\xE6\x92\xAD\xE6\x94\xBE\xE5\x99\xA8");
            }
            wchar_t wtip[512]{};
            MultiByteToWideChar(CP_UTF8, 0, tip, -1, wtip, 511);
            if (wtip[0]) g_win.taskbar3->SetThumbnailTooltip(g_win.hwnd, wtip);
        }
        const int pct = ht ? static_cast<int>(std::clamp(frac, 0.0f, 1.0f) * 100.0f + 0.5f) : -1;
        if (state_changed || pct != g_win.tb_pct) {
            g_win.tb_pct = pct;
            if (!ht) {
                g_win.taskbar3->SetProgressState(g_win.hwnd, TBPF_NOPROGRESS);
            } else {
                g_win.taskbar3->SetProgressState(g_win.hwnd, pl ? TBPF_NORMAL : TBPF_PAUSED);
                g_win.taskbar3->SetProgressValue(g_win.hwnd, static_cast<ULONGLONG>(pct), 100ULL);
            }
        }
    }
}

int  be_input_is_touch(void) { return g_win.is_touch; }
void be_set_idle_mode(int idle) { g_win.idle_mode = idle ? 1 : 0; }

#endif
