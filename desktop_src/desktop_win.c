#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend.h"
#include "plat.h"

static HWND      g_hwnd = NULL;
static uint32_t* g_backbuf = NULL;   // g_buf_w x g_buf_h RGBA
static uint32_t* g_bgra = NULL;      // g_buf_w x g_buf_h BGRA for GDI SetDIBitsToDevice
static BITMAPINFO g_bmi;
static int       g_running = 1;
static int       g_vp_x = 0, g_vp_y = 0, g_vp_w = 1360, g_vp_h = 840;
static int       g_buf_w = 0, g_buf_h = 0;
static float     g_dpi_scale = 1.0f;
static int       g_is_fullscreen = 0;
static WINDOWPLACEMENT g_prev_wp = {0};

// Mouse / Touch emulation state
static int g_m_held = 0, g_m_down = 0, g_m_up = 0, g_was_down = 0;
static int g_m_x = 0, g_m_y = 0, g_prev_x = 0, g_prev_y = 0;
static int g_hover_x = -1, g_hover_y = -1;
static int g_track_leave = 0, g_cursor_hand = 0, g_has_frame = 0;
static int g_touch_reset = 0;
static int g_wheel_accum = 0, g_wheel_frame = 0;

// Keyboard button state
static uint64_t g_key_held = 0, g_key_prev = 0, g_key_down = 0, g_key_down_pending = 0;
static int      g_hotkey = 0;
static int      g_text_enabled = 0;

typedef struct {
    int action;      // 0=utf8 char, 1=backspace, 2=enter, 3=esc
    char text[16];
} TextEv;
static TextEv g_tev[32];
static int g_tev_r = 0, g_tev_w = 0;

static char g_drop_path[600] = {0};
static int  g_has_drop = 0;
static char g_cli_path[600] = {0};
static char g_cfg_dir[512] = {0};

static void push_text_ev(int act, const char* u8) {
    int nxt = (g_tev_w + 1) & 31;
    if (nxt == g_tev_r) return;
    g_tev[g_tev_w].action = act;
    snprintf(g_tev[g_tev_w].text, sizeof(g_tev[g_tev_w].text), "%s", u8 ? u8 : "");
    g_tev_w = nxt;
}

static void refresh_dpi_scale(HWND hwnd) {
    typedef UINT (WINAPI *GetDpiFn)(HWND);
    UINT dpi = 96;
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32 && hwnd) {
        GetDpiFn fn = (GetDpiFn)(void*)GetProcAddress(u32, "GetDpiForWindow");
        if (fn) {
            UINT d = fn(hwnd);
            if (d >= 72 && d <= 384) dpi = d;
        }
    }
    g_dpi_scale = (float)dpi / 96.0f;
    if (g_dpi_scale < 1.0f) g_dpi_scale = 1.0f;
    if (g_dpi_scale > 2.5f) g_dpi_scale = 2.5f;
}

static void update_viewport(int cw, int ch) {
    if (cw < 320) cw = 320;
    if (ch < 240) ch = 240;
    g_vp_x = 0;
    g_vp_y = 0;
    g_vp_w = cw;
    g_vp_h = ch;
}

static void map_mouse_xy(int wx, int wy, int* ox, int* oy) {
    float s = g_dpi_scale > 0.0f ? g_dpi_scale : 1.0f;
    int lw = (int)((float)g_vp_w / s + 0.5f);
    int lh = (int)((float)g_vp_h / s + 0.5f);
    if (lw < 1) lw = 1;
    if (lh < 1) lh = 1;
    int lx = (int)((float)wx / s);
    int ly = (int)((float)wy / s);
    if (lx < 0) lx = 0;
    if (lx >= lw) lx = lw - 1;
    if (ly < 0) ly = 0;
    if (ly >= lh) ly = lh - 1;
    *ox = lx;
    *oy = ly;
}

static uint64_t vk_to_btn(WPARAM vk) {
    switch (vk) {
    case VK_UP:     return BTN_UP;
    case VK_DOWN:   return BTN_DOWN;
    case VK_LEFT:   return BTN_LEFT;
    case VK_RIGHT:  return BTN_RIGHT;
    case VK_RETURN: return BTN_A;
    case VK_ESCAPE: return BTN_B;
    case 'A':       return BTN_A;
    case 'B':       return BTN_B;
    case 'X':       return BTN_X;
    case 'Y':       return BTN_Y;
    case 'T':       return BTN_ZR;
    case 'H':       return BTN_ZL;
    case VK_SPACE:  return BTN_A;
    case VK_OEM_PLUS:
    case VK_ADD:    return BTN_STICK_R_UP;
    case VK_OEM_MINUS:
    case VK_SUBTRACT: return BTN_STICK_R_DOWN;
    default: return 0;
    }
}

static void handle_wchar(wchar_t ch) {
    if (!g_text_enabled) return;
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
    WideCharToMultiByte(CP_UTF8, 0, wbuf, wn, u8, sizeof(u8) - 1, NULL, NULL);
    if (u8[0]) push_text_ev(0, u8);
}

static void ensure_win_buffers(int w, int h) {
    if (w < 160) w = 160;
    if (h < 90) h = 90;
    if (w == g_buf_w && h == g_buf_h && g_backbuf && g_bgra) return;
    free(g_backbuf);
    free(g_bgra);
    g_backbuf = (uint32_t*)calloc((size_t)w * h, 4);
    g_bgra    = (uint32_t*)calloc((size_t)w * h, 4);
    g_buf_w = w;
    g_buf_h = h;
    g_has_frame = 0;
    memset(&g_bmi, 0, sizeof(g_bmi));
    g_bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    g_bmi.bmiHeader.biWidth = w;
    g_bmi.bmiHeader.biHeight = -h;
    g_bmi.bmiHeader.biPlanes = 1;
    g_bmi.bmiHeader.biBitCount = 32;
    g_bmi.bmiHeader.biCompression = BI_RGB;
}

static void present_to_hdc(HDC hdc, HWND hwnd) {
    if (!hdc || !hwnd || !g_has_frame || !g_bgra || g_buf_w <= 0 || g_buf_h <= 0) return;
    RECT cr;
    GetClientRect(hwnd, &cr);
    int cw = cr.right - cr.left, ch = cr.bottom - cr.top;
    if (g_vp_x > 0 || g_vp_y > 0) {
        HBRUSH br = CreateSolidBrush(RGB(22, 26, 34));
        if (g_vp_x > 0) {
            RECT r1 = { 0, 0, g_vp_x, ch }; FillRect(hdc, &r1, br);
            RECT r2 = { g_vp_x + g_vp_w, 0, cw, ch }; FillRect(hdc, &r2, br);
        }
        if (g_vp_y > 0) {
            RECT r3 = { 0, 0, cw, g_vp_y }; FillRect(hdc, &r3, br);
            RECT r4 = { 0, g_vp_y + g_vp_h, cw, ch }; FillRect(hdc, &r4, br);
        }
        DeleteObject(br);
    }
    if (g_vp_w == g_buf_w && g_vp_h == g_buf_h) {
        SetDIBitsToDevice(hdc, g_vp_x, g_vp_y, g_buf_w, g_buf_h, 0, 0, 0, g_buf_h,
                          g_bgra, &g_bmi, DIB_RGB_COLORS);
    } else {
        SetStretchBltMode(hdc, HALFTONE);
        SetBrushOrgEx(hdc, 0, 0, NULL);
        StretchDIBits(hdc, g_vp_x, g_vp_y, g_vp_w, g_vp_h,
                      0, 0, g_buf_w, g_buf_h, g_bgra, &g_bmi, DIB_RGB_COLORS, SRCCOPY);
    }
}

static LRESULT CALLBACK win_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE:
    case WM_DESTROY:
        g_running = 0;
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
            SetCursor(LoadCursorW(NULL, (LPCWSTR)(g_cursor_hand ? IDC_HAND : IDC_ARROW)));
            return TRUE;
        }
        break;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize.x = (LONG)(840 * g_dpi_scale);
        mm->ptMinTrackSize.y = (LONG)(520 * g_dpi_scale);
        return 0;
    }
    case WM_SIZE:
        refresh_dpi_scale(hwnd);
        update_viewport(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        map_mouse_xy(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &g_m_x, &g_m_y);
        g_hover_x = g_m_x; g_hover_y = g_m_y;
        g_m_held = 1;
        g_m_down = 1;
        return 0;
    case WM_MOUSEMOVE:
        if (!g_track_leave) {
            TRACKMOUSEEVENT tme;
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            tme.dwHoverTime = 0;
            if (TrackMouseEvent(&tme)) g_track_leave = 1;
        }
        map_mouse_xy(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &g_hover_x, &g_hover_y);
        if (g_m_held) {
            g_m_x = g_hover_x;
            g_m_y = g_hover_y;
        }
        return 0;
    case WM_MOUSELEAVE:
        g_track_leave = 0;
        g_hover_x = -1;
        g_hover_y = -1;
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == hwnd) ReleaseCapture();
        map_mouse_xy(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &g_m_x, &g_m_y);
        g_m_held = 0;
        g_m_up = 1;
        return 0;
    case WM_MOUSEWHEEL:
        g_wheel_accum += GET_WHEEL_DELTA_WPARAM(wp);
        return 0;
    case WM_CHAR:
        handle_wchar((wchar_t)wp);
        return 0;
    case WM_KEYDOWN: {
        int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (g_text_enabled) {
            if (wp == VK_BACK)   { push_text_ev(1, NULL); return 0; }
            if (wp == VK_RETURN) { push_text_ev(2, NULL); return 0; }
            if (wp == VK_ESCAPE) { push_text_ev(3, NULL); return 0; }
            if (wp == 'V' && ctrl) {
                if (OpenClipboard(hwnd)) {
                    HANDLE hd = GetClipboardData(CF_UNICODETEXT);
                    if (hd) {
                        const wchar_t* ws = (const wchar_t*)GlobalLock(hd);
                        if (ws) {
                            char u8[256];
                            plat_wide_to_utf8(ws, u8, sizeof(u8));
                            for (size_t i = 0; u8[i]; ) {
                                unsigned char c = (unsigned char)u8[i];
                                int len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
                                if (u8[i] != '\r' && u8[i] != '\n') {
                                    char ch[8] = {0};
                                    for (int k = 0; k < len && u8[i + k]; k++) ch[k] = u8[i + k];
                                    push_text_ev(0, ch);
                                }
                                i += (size_t)len;
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
        if (ctrl && wp == 'F') { g_hotkey = 's'; return 0; }
        if (ctrl && wp == 'O') { g_hotkey = 'f'; return 0; }
        if (wp == VK_OEM_2 || wp == 'S') { g_hotkey = 's'; return 0; }
        if (wp == 'F') { g_key_down_pending |= BTN_X; return 0; }
        if (wp == 'O') { g_hotkey = 'f'; return 0; }
        if (wp == 'M') { g_hotkey = 'm'; return 0; }
        if (wp == 'D') { g_hotkey = 'c'; return 0; }
        if (wp == 'V') { g_hotkey = 'v'; return 0; }
        if (wp == VK_PRIOR) { g_wheel_accum += 360; return 0; }
        if (wp == VK_NEXT)  { g_wheel_accum -= 360; return 0; }
        uint64_t b = vk_to_btn(wp);
        if (b) {
            if (!(g_key_held & b)) g_key_down_pending |= b;
            g_key_held |= b;
        }
        return 0;
    }
    case WM_KEYUP: {
        uint64_t b = vk_to_btn(wp);
        if (b) g_key_held &= ~b;
        return 0;
    }
    case WM_KILLFOCUS:
        g_key_held = 0;
        g_m_held = 0;
        return 0;
    case WM_DROPFILES: {
        HDROP hd = (HDROP)wp;
        wchar_t wpath[600];
        if (DragQueryFileW(hd, 0, wpath, 600) > 0) {
            plat_wide_to_utf8(wpath, g_drop_path, sizeof(g_drop_path));
            g_has_drop = 1;
        }
        DragFinish(hd);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int be_init(void) {
    typedef BOOL (WINAPI *SetDpiCtxFn)(HANDLE);
    typedef BOOL (WINAPI *SetDpiFn)(void);
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        SetDpiCtxFn fn2 = (SetDpiCtxFn)(void*)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (!fn2 || !fn2((HANDLE)(intptr_t)-4)) {
            SetDpiFn fn = (SetDpiFn)(void*)GetProcAddress(u32, "SetProcessDPIAware");
            if (fn) fn();
        }
    }
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    timeBeginPeriod(1);

    HINSTANCE hInst = GetModuleHandleW(NULL);
    HICON ico_big = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                      GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    HICON ico_sm  = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                      GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!ico_big) ico_big = LoadIconW(hInst, MAKEINTRESOURCEW(1));

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = win_proc;
    wc.hInstance = hInst;
    wc.hIcon = ico_big;
    wc.hIconSm = ico_sm ? ico_sm : ico_big;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"SirenePlayerWindow";
    RegisterClassExW(&wc);

    RECT wa = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    HMONITOR hm = MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (hm && GetMonitorInfoW(hm, &mi)) wa = mi.rcWork;
    int work_w = wa.right - wa.left;
    int work_h = wa.bottom - wa.top;

    int init_w = 1360, init_h = 840;
    if (work_w > 0 && work_h > 0) {
        int max_w = (int)(work_w * 0.86);
        int max_h = (int)(work_h * 0.88);
        if (init_w > max_w) init_w = max_w;
        if (init_h > max_h) init_h = max_h;
        if (init_w < 960 && work_w >= 960) init_w = 960;
        if (init_h < 600 && work_h >= 600) init_h = 600;
    }
    RECT rc = { 0, 0, init_w, init_h };
    DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    AdjustWindowRect(&rc, style, FALSE);
    int win_w = rc.right - rc.left;
    int win_h = rc.bottom - rc.top;
    int win_x = wa.left + (work_w - win_w) / 2;
    int win_y = wa.top + (work_h - win_h) / 2;
    if (win_x < wa.left) win_x = wa.left;
    if (win_y < wa.top) win_y = wa.top;

    g_hwnd = CreateWindowExW(
        WS_EX_ACCEPTFILES,
        L"SirenePlayerWindow",
        L"Sir\x00E8ne \x00B7 \x672C\x5730\x97F3\x4E50\x64AD\x653E\x5668",
        style,
        win_x, win_y,
        win_w, win_h,
        NULL, NULL, hInst, NULL);
    if (!g_hwnd) return -1;
    if (ico_big) SendMessageW(g_hwnd, WM_SETICON, ICON_BIG, (LPARAM)ico_big);
    if (ico_sm || ico_big) SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)(ico_sm ? ico_sm : ico_big));

    refresh_dpi_scale(g_hwnd);
    RECT cr0; GetClientRect(g_hwnd, &cr0);
    update_viewport(cr0.right - cr0.left, cr0.bottom - cr0.top);
    ensure_win_buffers(g_vp_w, g_vp_h);
    if (!g_backbuf || !g_bgra) return -1;
    return 0;
}

void be_exit(void) {
    free(g_backbuf); g_backbuf = NULL;
    free(g_bgra);    g_bgra = NULL;
    g_buf_w = g_buf_h = 0;
    if (g_hwnd) { DestroyWindow(g_hwnd); g_hwnd = NULL; }
    timeEndPeriod(1);
    CoUninitialize();
}

void be_viewport_size(int* out_w, int* out_h) {
    int w = g_vp_w < 320 ? 320 : g_vp_w;
    int h = g_vp_h < 240 ? 240 : g_vp_h;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

void be_screen_metrics(int* out_phys_w, int* out_phys_h, float* out_scale) {
    if (out_phys_w) *out_phys_w = g_vp_w < 320 ? 320 : g_vp_w;
    if (out_phys_h) *out_phys_h = g_vp_h < 240 ? 240 : g_vp_h;
    if (out_scale)  *out_scale  = g_dpi_scale;
}

void be_set_dark_titlebar(int dark) {
    if (!g_hwnd) return;
    typedef HRESULT (WINAPI *DwmSetAttrFn)(HWND, DWORD, LPCVOID, DWORD);
    HMODULE d = LoadLibraryW(L"dwmapi.dll");
    if (!d) return;
    DwmSetAttrFn fn = (DwmSetAttrFn)(void*)GetProcAddress(d, "DwmSetWindowAttribute");
    if (fn) {
        BOOL v = dark ? TRUE : FALSE;
        if (FAILED(fn(g_hwnd, 20, &v, sizeof(v)))) fn(g_hwnd, 19, &v, sizeof(v));
    }
}

void be_toggle_fullscreen(void) {
    if (!g_hwnd) return;
    DWORD st = (DWORD)GetWindowLongW(g_hwnd, GWL_STYLE);
    if (!g_is_fullscreen) {
        MONITORINFO mi;
        memset(&mi, 0, sizeof(mi));
        mi.cbSize = sizeof(mi);
        g_prev_wp.length = sizeof(g_prev_wp);
        if (GetWindowPlacement(g_hwnd, &g_prev_wp) &&
            GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLongW(g_hwnd, GWL_STYLE, (LONG)(st & ~WS_OVERLAPPEDWINDOW));
            SetWindowPos(g_hwnd, HWND_TOP,
                         mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            g_is_fullscreen = 1;
        }
    } else {
        SetWindowLongW(g_hwnd, GWL_STYLE, (LONG)(st | WS_OVERLAPPEDWINDOW));
        SetWindowPlacement(g_hwnd, &g_prev_wp);
        SetWindowPos(g_hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_is_fullscreen = 0;
    }
}

int be_is_fullscreen(void) { return g_is_fullscreen; }

uint32_t* be_begin(int* stride) {
    ensure_win_buffers(g_vp_w, g_vp_h);
    if (stride) *stride = g_buf_w;
    return g_backbuf;
}

void be_end(void) {
    if (!g_hwnd || !g_backbuf || !g_bgra || g_buf_w <= 0 || g_buf_h <= 0) return;
    size_t n = (size_t)g_buf_w * g_buf_h;
    for (size_t i = 0; i < n; i++) {
        uint32_t p = g_backbuf[i];
        g_bgra[i] = (p & 0xFF00FF00u) | ((p & 0xFFu) << 16) | ((p >> 16) & 0xFFu);
    }
    g_has_frame = 1;
    HDC hdc = GetDC(g_hwnd);
    if (hdc) {
        present_to_hdc(hdc, g_hwnd);
        ReleaseDC(g_hwnd, hdc);
    }

    // 60 FPS 精准帧率节拍
    static uint64_t next_frame_ns = 0;
    uint64_t now = plat_ticks_ns();
    if (next_frame_ns == 0 || now > next_frame_ns + 50000000ULL) {
        next_frame_ns = now + 16666667ULL;
    } else {
        while (now < next_frame_ns) {
            uint64_t rem_ms = (next_frame_ns - now) / 1000000ULL;
            if (rem_ms > 1) Sleep((DWORD)(rem_ms - 1));
            else Sleep(0);
            now = plat_ticks_ns();
        }
        next_frame_ns += 16666667ULL;
    }
}

void be_input_update(void) {
    MSG msg;
    g_wheel_frame = 0;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) g_running = 0;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_wheel_accum >= 120 || g_wheel_accum <= -120) {
        g_wheel_frame = g_wheel_accum / 120;
        g_wheel_accum %= 120;
    }
    g_key_down = g_key_down_pending | (g_key_held & ~g_key_prev);
    g_key_down_pending = 0;
    g_key_prev = g_key_held;
}

uint64_t be_btn_down(void) { return g_key_down; }
uint64_t be_btn_held(void) { return g_key_held; }
void be_sleep_ms(int ms) { Sleep((DWORD)ms); }
int be_should_run(void) { return g_running; }

void be_touch_update(TouchInfo* out) {
    memset(out, 0, sizeof(*out));
    if (g_touch_reset) {
        g_was_down = 0;
        g_m_down = 0;
        g_m_up = 0;
        g_touch_reset = 0;
        return;
    }
    if (g_m_held || g_m_down) {
        out->held = 1;
        out->x = g_m_x;
        out->y = g_m_y;
        if (!g_was_down || g_m_down) {
            out->down = 1;
            g_prev_x = g_m_x; g_prev_y = g_m_y;
        }
        out->dx = g_m_x - g_prev_x;
        out->dy = g_m_y - g_prev_y;
        g_prev_x = g_m_x; g_prev_y = g_m_y;
        g_was_down = 1;
        g_m_down = 0;
    } else if (g_was_down || g_m_up) {
        out->up = 1;
        out->x = g_m_x;
        out->y = g_m_y;
        g_was_down = 0;
        g_m_up = 0;
    }
}

void be_touch_reset(void) { g_touch_reset = 1; }
int be_is_desktop(void) { return 1; }
int be_wheel_delta(void) { return g_wheel_frame; }

const char* be_poll_dropped_path(void) {
    if (!g_has_drop) return NULL;
    g_has_drop = 0;
    return g_drop_path;
}

int be_pick_folder(char* out, size_t cap) {
    BROWSEINFOW bi;
    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = g_hwnd;
    bi.lpszTitle = L"\x9009\x62E9 Sir\x00E8ne \x672C\x5730\x97F3\x4E50\x6587\x4EF6\x5939";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return 0;
    wchar_t wdir[600];
    int ok = SHGetPathFromIDListW(pidl, wdir);
    CoTaskMemFree(pidl);
    if (!ok) return 0;
    plat_wide_to_utf8(wdir, out, (int)cap);
    for (char* p = out; *p; p++) if (*p == '\\') *p = '/';
    be_touch_reset();
    return out[0] ? 1 : 0;
}

void be_text_input(int enable) {
    g_text_enabled = enable;
    if (enable) { g_tev_r = g_tev_w = 0; g_key_held = 0; }
}

int be_poll_text_char(char* utf8_out, size_t cap, int* action) {
    if (g_tev_r == g_tev_w) return 0;
    TextEv e = g_tev[g_tev_r];
    g_tev_r = (g_tev_r + 1) & 31;
    if (action) *action = e.action;
    if (utf8_out && cap > 0) snprintf(utf8_out, cap, "%s", e.text);
    return 1;
}

int be_poll_hotkey(void) {
    int h = g_hotkey;
    g_hotkey = 0;
    return h;
}

const char* be_config_dir(void) {
    if (!g_cfg_dir[0]) {
        wchar_t wapp[480];
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, wapp))) {
            char u8[480];
            plat_wide_to_utf8(wapp, u8, sizeof(u8));
            for (char* p = u8; *p; p++) if (*p == '\\') *p = '/';
            snprintf(g_cfg_dir, sizeof(g_cfg_dir), "%s/Sirene", u8);
        } else {
            snprintf(g_cfg_dir, sizeof(g_cfg_dir), "./Sirene_config");
        }
    }
    return g_cfg_dir;
}

void be_set_cli_path(const char* p) {
    if (!p || !*p) return;
    snprintf(g_cli_path, sizeof(g_cli_path), "%s", p);
    for (char* q = g_cli_path; *q; q++) if (*q == '\\') *q = '/';
    snprintf(g_drop_path, sizeof(g_drop_path), "%s", g_cli_path);
    g_has_drop = 1;
}

int be_default_music_dirs(char dirs[][600], int max_dirs) {
    int n = 0;
    wchar_t wmus[512];
    if (n < max_dirs && SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_MYMUSIC, NULL, 0, wmus))) {
        plat_wide_to_utf8(wmus, dirs[n], 600);
        for (char* p = dirs[n]; *p; p++) if (*p == '\\') *p = '/';
        n++;
    }
    if (n < max_dirs) {
        snprintf(dirs[n++], 600, "./music");
    }
    return n;
}
void be_mouse_pos(int* x, int* y) {
    if (x) *x = g_hover_x;
    if (y) *y = g_hover_y;
}

void be_set_cursor(int hand) {
    int h = hand ? 1 : 0;
    if (h != g_cursor_hand) {
        g_cursor_hand = h;
        SetCursor(LoadCursorW(NULL, (LPCWSTR)(h ? IDC_HAND : IDC_ARROW)));
    }
}

#endif
