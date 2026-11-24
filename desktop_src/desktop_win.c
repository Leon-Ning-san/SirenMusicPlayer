#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "backend.h"
#include "plat.h"

#define ID_THB_PREV 1001
#define ID_THB_PLAY 1002
#define ID_THB_NEXT 1003

enum {
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

static HWND      g_hwnd = NULL;
static uint32_t* g_backbuf = NULL;   // g_buf_w x g_buf_h RGBA
static uint32_t* g_bgra = NULL;      // g_buf_w x g_buf_h BGRA for GDI SetDIBitsToDevice
static BITMAPINFO g_bmi;
static int       g_running = 1;
static int       g_minimized = 0;
static int       g_vp_x = 0, g_vp_y = 0, g_vp_w = 1360, g_vp_h = 840;
static int       g_buf_w = 0, g_buf_h = 0;
static float     g_dpi_scale = 1.0f;
static int       g_is_fullscreen = 0;
static WINDOWPLACEMENT g_prev_wp = {0};

// Taskbar Thumbnail Toolbar & media command queue
static UINT          g_msg_taskbar_created = 0;
static ITaskbarList3* g_taskbar3 = NULL;
static int           g_thumbbar_added = 0;
static int           g_tb_has_track = 0;
static int           g_tb_playing = 0;
static int           g_tb_pct = -1;
static HICON         g_ico_prev = NULL;
static HICON         g_ico_play = NULL;
static HICON         g_ico_pause = NULL;
static HICON         g_ico_next = NULL;
static char          g_last_title_utf8[512] = {0};
static int           g_mcmd_q[16];
static int           g_mcmd_r = 0, g_mcmd_w = 0;

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

static void push_media_cmd(int cmd) {
    if (cmd == MEDIA_CMD_NONE) return;
    int nxt = (g_mcmd_w + 1) & 15;
    if (nxt == g_mcmd_r) return;
    g_mcmd_q[g_mcmd_w] = cmd;
    g_mcmd_w = nxt;
}

static int win_system_uses_light_theme(void) {
    DWORD val = 0, sz = sizeof(val);
    HKEY hk = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &hk) == ERROR_SUCCESS) {
        if (RegQueryValueExW(hk, L"SystemUsesLightTheme", NULL, NULL, (LPBYTE)&val, &sz) != ERROR_SUCCESS) {
            val = 0;
        }
        RegCloseKey(hk);
    }
    return val ? 1 : 0;
}

static float seg_dist(float px, float py, float ax, float ay, float bx, float by) {
    float vx = bx - ax, vy = by - ay;
    float wx = px - ax, wy = py - ay;
    float c1 = vx * wx + vy * wy;
    if (c1 <= 0.0f) return sqrtf(wx * wx + wy * wy);
    float c2 = vx * vx + vy * vy;
    if (c2 <= c1) {
        float dx = px - bx, dy = py - by;
        return sqrtf(dx * dx + dy * dy);
    }
    float t = c1 / c2;
    float qx = ax + t * vx, qy = ay + t * vy;
    float dx = px - qx, dy = py - qy;
    return sqrtf(dx * dx + dy * dy);
}

static HICON create_media_thumb_icon(int kind, int sz, int light_bar) {
    if (sz < 16) sz = 16;
    if (sz > 96) sz = 96;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = sz;
    bi.bmiHeader.biHeight = -sz;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    uint32_t* bits = NULL;
    HDC sdc = GetDC(NULL);
    HBITMAP hcol = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void**)&bits, NULL, 0);
    if (sdc) ReleaseDC(NULL, sdc);
    if (!hcol || !bits) {
        if (hcol) DeleteObject(hcol);
        return NULL;
    }

    uint8_t fg = light_bar ? 26u : 246u;
    float hw = (sz >= 44 ? 2.15f : 1.50f) / (float)sz;
    float aa = 1.15f / (float)sz;

    for (int y = 0; y < sz; y++) {
        float py = ((float)y + 0.5f) / (float)sz;
        for (int x = 0; x < sz; x++) {
            float px = ((float)x + 0.5f) / (float)sz;
            float d = 1.0f;
            if (kind == 0) {
                // Prev: |<
                float d0 = seg_dist(px, py, 0.25f, 0.24f, 0.25f, 0.76f);
                float d1 = seg_dist(px, py, 0.40f, 0.50f, 0.74f, 0.25f);
                float d2 = seg_dist(px, py, 0.74f, 0.25f, 0.74f, 0.75f);
                float d3 = seg_dist(px, py, 0.74f, 0.75f, 0.40f, 0.50f);
                d = d0; if (d1 < d) d = d1; if (d2 < d) d = d2; if (d3 < d) d = d3;
            } else if (kind == 1) {
                // Play: >
                float d1 = seg_dist(px, py, 0.32f, 0.22f, 0.76f, 0.50f);
                float d2 = seg_dist(px, py, 0.76f, 0.50f, 0.32f, 0.78f);
                float d3 = seg_dist(px, py, 0.32f, 0.78f, 0.32f, 0.22f);
                d = d1; if (d2 < d) d = d2; if (d3 < d) d = d3;
            } else if (kind == 2) {
                // Next: >|
                float d1 = seg_dist(px, py, 0.26f, 0.25f, 0.60f, 0.50f);
                float d2 = seg_dist(px, py, 0.60f, 0.50f, 0.26f, 0.75f);
                float d3 = seg_dist(px, py, 0.26f, 0.75f, 0.26f, 0.25f);
                float d0 = seg_dist(px, py, 0.75f, 0.24f, 0.75f, 0.76f);
                d = d0; if (d1 < d) d = d1; if (d2 < d) d = d2; if (d3 < d) d = d3;
            } else {
                // Pause: ||
                float d0 = seg_dist(px, py, 0.36f, 0.24f, 0.36f, 0.76f);
                float d1 = seg_dist(px, py, 0.64f, 0.24f, 0.64f, 0.76f);
                d = d0 < d1 ? d0 : d1;
            }
            float cov = (hw + aa * 0.5f - d) / aa;
            if (cov <= 0.0f) {
                bits[(size_t)y * sz + x] = 0;
            } else {
                if (cov > 1.0f) cov = 1.0f;
                uint8_t a = (uint8_t)(cov * 255.0f + 0.5f);
                uint8_t p = (uint8_t)((uint32_t)fg * a / 255u);
                bits[(size_t)y * sz + x] = ((uint32_t)a << 24) | ((uint32_t)p << 16) | ((uint32_t)p << 8) | (uint32_t)p;
            }
        }
    }

    size_t mask_stride = (size_t)(((sz + 15) / 16) * 2);
    void* mbits = calloc(mask_stride * (size_t)sz, 1);
    HBITMAP hmask = mbits ? CreateBitmap(sz, sz, 1, 1, mbits) : NULL;
    free(mbits);

    HICON out = NULL;
    if (hmask) {
        ICONINFO ii;
        memset(&ii, 0, sizeof(ii));
        ii.fIcon = TRUE;
        ii.hbmColor = hcol;
        ii.hbmMask = hmask;
        out = CreateIconIndirect(&ii);
        DeleteObject(hmask);
    }
    DeleteObject(hcol);
    return out;
}

static void taskbar_destroy_icons(void) {
    if (g_ico_prev)  { DestroyIcon(g_ico_prev);  g_ico_prev = NULL; }
    if (g_ico_play)  { DestroyIcon(g_ico_play);  g_ico_play = NULL; }
    if (g_ico_pause) { DestroyIcon(g_ico_pause); g_ico_pause = NULL; }
    if (g_ico_next)  { DestroyIcon(g_ico_next);  g_ico_next = NULL; }
}

static void taskbar_rebuild_icons(HWND hwnd) {
    typedef UINT (WINAPI *GetMetDpiFn)(int, UINT);
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
    int sz = GetSystemMetrics(SM_CXICON);
    if (u32) {
        GetMetDpiFn mfn = (GetMetDpiFn)(void*)GetProcAddress(u32, "GetSystemMetricsForDpi");
        if (mfn) {
            int dsz = mfn(SM_CXICON, dpi);
            if (dsz >= 16 && dsz <= 96) sz = dsz;
        }
    }
    if (sz < 24) sz = 24;
    int light = win_system_uses_light_theme();
    taskbar_destroy_icons();
    g_ico_prev  = create_media_thumb_icon(0, sz, light);
    g_ico_play  = create_media_thumb_icon(1, sz, light);
    g_ico_next  = create_media_thumb_icon(2, sz, light);
    g_ico_pause = create_media_thumb_icon(3, sz, light);
}

static void taskbar_fill_buttons(THUMBBUTTON btns[3]) {
    memset(btns, 0, sizeof(THUMBBUTTON) * 3);
    btns[0].dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
    btns[0].iId = ID_THB_PREV;
    btns[0].hIcon = g_ico_prev;
    wcscpy(btns[0].szTip, L"\x4E0A\x4E00\x9996"); // 上一首
    btns[0].dwFlags = THBF_ENABLED;

    btns[1].dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
    btns[1].iId = ID_THB_PLAY;
    btns[1].hIcon = (g_tb_has_track && g_tb_playing) ? g_ico_pause : g_ico_play;
    wcscpy(btns[1].szTip, (g_tb_has_track && g_tb_playing) ? L"\x6682\x505C" : L"\x64AD\x653E"); // 暂停 / 播放
    btns[1].dwFlags = THBF_ENABLED;

    btns[2].dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
    btns[2].iId = ID_THB_NEXT;
    btns[2].hIcon = g_ico_next;
    wcscpy(btns[2].szTip, L"\x4E0B\x4E00\x9996"); // 下一首
    btns[2].dwFlags = THBF_ENABLED;
}

static void taskbar_update_buttons(void) {
    if (!g_hwnd || !g_taskbar3 || !g_thumbbar_added) return;
    THUMBBUTTON btns[3];
    taskbar_fill_buttons(btns);
    g_taskbar3->lpVtbl->ThumbBarUpdateButtons(g_taskbar3, g_hwnd, 3, btns);
}

static void taskbar_on_created(HWND hwnd, int from_msg) {
    if (!hwnd) return;
    if (g_taskbar3) {
        g_taskbar3->lpVtbl->Release(g_taskbar3);
        g_taskbar3 = NULL;
    }

    ITaskbarList3* tb = NULL;
    if (FAILED(CoCreateInstance(&CLSID_TaskbarList, NULL, CLSCTX_INPROC_SERVER,
                                &IID_ITaskbarList3, (void**)&tb)) || !tb) {
        if (from_msg) g_thumbbar_added = 0;
        return;
    }
    if (FAILED(tb->lpVtbl->HrInit(tb))) {
        tb->lpVtbl->Release(tb);
        if (from_msg) g_thumbbar_added = 0;
        return;
    }
    g_taskbar3 = tb;
    taskbar_rebuild_icons(hwnd);

    THUMBBUTTON btns[3];
    taskbar_fill_buttons(btns);
    HRESULT hr = g_taskbar3->lpVtbl->ThumbBarAddButtons(g_taskbar3, hwnd, 3, btns);
    if (SUCCEEDED(hr)) {
        g_thumbbar_added = 1;
    } else if (from_msg && g_thumbbar_added) {
        g_taskbar3->lpVtbl->ThumbBarUpdateButtons(g_taskbar3, hwnd, 3, btns);
    } else {
        g_thumbbar_added = 0;
    }
}

static void taskbar_free(void) {
    if (g_taskbar3) {
        g_taskbar3->lpVtbl->Release(g_taskbar3);
        g_taskbar3 = NULL;
    }
    g_thumbbar_added = 0;
    taskbar_destroy_icons();
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
    if (cw <= 0 || ch <= 0) return;
    if (cw < 320) cw = 320;
    if (ch < 240) ch = 240;
    if (cw > 8192) cw = 8192;
    if (ch > 8192) ch = 8192;
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
    if (w > 8192) w = 8192;
    if (h > 8192) h = 8192;
    if (w == g_buf_w && h == g_buf_h && g_backbuf && g_bgra) return;
    uint32_t* nb = (uint32_t*)calloc((size_t)w * h, 4);
    uint32_t* ng = (uint32_t*)calloc((size_t)w * h, 4);
    if (!nb || !ng) {
        free(nb);
        free(ng);
        return;
    }
    free(g_backbuf);
    free(g_bgra);
    g_backbuf = nb;
    g_bgra    = ng;
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
    if (g_msg_taskbar_created != 0 && msg == g_msg_taskbar_created) {
        taskbar_on_created(hwnd, 1);
        return 0;
    }
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
        if (hwnd) refresh_dpi_scale(hwnd);
        LONG mw = (LONG)(840 * g_dpi_scale);
        LONG mh = (LONG)(520 * g_dpi_scale);
        HMONITOR hm = MonitorFromWindow(hwnd ? hwnd : GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        if (hm && GetMonitorInfoW(hm, &mi)) {
            LONG ww = mi.rcWork.right - mi.rcWork.left;
            LONG wh = mi.rcWork.bottom - mi.rcWork.top;
            if (ww > 400 && mw > ww * 85 / 100) mw = ww * 85 / 100;
            if (wh > 300 && mh > wh * 85 / 100) mh = wh * 85 / 100;
        }
        mm->ptMinTrackSize.x = mw;
        mm->ptMinTrackSize.y = mh;
        return 0;
    }
    case WM_DPICHANGED: {
        UINT dpi = HIWORD(wp);
        if (dpi >= 72 && dpi <= 384) {
            g_dpi_scale = (float)dpi / 96.0f;
            if (g_dpi_scale < 1.0f) g_dpi_scale = 1.0f;
            if (g_dpi_scale > 2.5f) g_dpi_scale = 2.5f;
        }
        const RECT* prc = (const RECT*)lp;
        if (prc && !g_is_fullscreen) {
            SetWindowPos(hwnd, NULL, prc->left, prc->top,
                         prc->right - prc->left, prc->bottom - prc->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (g_thumbbar_added) {
            taskbar_rebuild_icons(hwnd);
            taskbar_update_buttons();
        }
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (g_thumbbar_added && lp && wcscmp((const wchar_t*)lp, L"ImmersiveColorSet") == 0) {
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
        short cmd = GET_APPCOMMAND_LPARAM(lp);
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
            g_minimized = 1;
            return 0;
        }
        g_minimized = 0;
        refresh_dpi_scale(hwnd);
        update_viewport(LOWORD(lp), HIWORD(lp));
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_CAPTURECHANGED:
        g_m_held = 0;
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
        if (wp == VK_MEDIA_PLAY_PAUSE) { push_media_cmd(MEDIA_CMD_TOGGLE); return 0; }
        if (wp == VK_MEDIA_PREV_TRACK) { push_media_cmd(MEDIA_CMD_PREV);   return 0; }
        if (wp == VK_MEDIA_NEXT_TRACK) { push_media_cmd(MEDIA_CMD_NEXT);   return 0; }
        if (wp == VK_MEDIA_STOP)       { push_media_cmd(MEDIA_CMD_PAUSE);  return 0; }
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

    g_msg_taskbar_created = RegisterWindowMessageW(L"TaskbarButtonCreated");

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

    if (u32) {
        typedef BOOL (WINAPI *ChgMsgFltExFn)(HWND, UINT, DWORD, PVOID);
        ChgMsgFltExFn flt = (ChgMsgFltExFn)(void*)GetProcAddress(u32, "ChangeWindowMessageFilterEx");
        if (flt) {
            if (g_msg_taskbar_created) flt(g_hwnd, g_msg_taskbar_created, 1 /* MSGFLT_ALLOW */, NULL);
            flt(g_hwnd, WM_COMMAND, 1, NULL);
            flt(g_hwnd, WM_APPCOMMAND, 1, NULL);
        }
    }

    refresh_dpi_scale(g_hwnd);
    RECT cr0; GetClientRect(g_hwnd, &cr0);
    update_viewport(cr0.right - cr0.left, cr0.bottom - cr0.top);
    ensure_win_buffers(g_vp_w, g_vp_h);
    if (!g_backbuf || !g_bgra) return -1;
    return 0;
}

void be_exit(void) {
    taskbar_free();
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
    if (g_minimized || (g_hwnd && IsIconic(g_hwnd))) {
        if (stride) *stride = g_buf_w;
        return g_backbuf;
    }
    ensure_win_buffers(g_vp_w, g_vp_h);
    if (stride) *stride = g_buf_w;
    return g_backbuf;
}

void be_end(void) {
    if (g_hwnd && !g_minimized && !IsIconic(g_hwnd) && g_backbuf && g_bgra && g_buf_w > 0 && g_buf_h > 0) {
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
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv) {
        if (wargc > 1 && wargv[1] && wargv[1][0]) {
            plat_wide_to_utf8(wargv[1], g_cli_path, sizeof(g_cli_path));
            for (char* q = g_cli_path; *q; q++) if (*q == '\\') *q = '/';
            snprintf(g_drop_path, sizeof(g_drop_path), "%s", g_cli_path);
            g_has_drop = 1;
            LocalFree(wargv);
            return;
        }
        LocalFree(wargv);
    }
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

int be_ensure_desktop_shortcut(int only_repair) {
    wchar_t wdesk[600] = {0};
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, wdesk)) || !wdesk[0]) {
        if (FAILED(SHGetFolderPathW(NULL, CSIDL_DESKTOP, NULL, 0, wdesk)) || !wdesk[0]) return 0;
    }
    wchar_t wexe[600] = {0};
    DWORD n = GetModuleFileNameW(NULL, wexe, 600);
    if (n == 0 || n >= 600) return 0;

    size_t dlen = wcslen(wdesk);
    if (_wcsnicmp(wexe, wdesk, dlen) == 0 && (wexe[dlen] == L'\\' || wexe[dlen] == L'/')) {
        return 0; // Running directly from Desktop
    }

    wchar_t wdir[600] = {0};
    wcsncpy(wdir, wexe, 599);
    for (int i = (int)wcslen(wdir) - 1; i >= 0; i--) {
        if (wdir[i] == L'\\' || wdir[i] == L'/') { wdir[i] = 0; break; }
    }

    wchar_t wlnk[640] = {0};
    _snwprintf(wlnk, 639, L"%ls\\Sir\x00E8ne.lnk", wdesk);
    DWORD attr = GetFileAttributesW(wlnk);
    if (attr == INVALID_FILE_ATTRIBUTES && only_repair) return 0;

    IShellLinkW* psl = NULL;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void**)&psl)) || !psl) {
        return 0;
    }
    IPersistFile* ppf = NULL;
    int did = 0;
    if (SUCCEEDED(psl->lpVtbl->QueryInterface(psl, &IID_IPersistFile, (void**)&ppf)) && ppf) {
        int need_save = 1;
        if (attr != INVALID_FILE_ATTRIBUTES) {
            need_save = 0;
            if (SUCCEEDED(ppf->lpVtbl->Load(ppf, wlnk, STGM_READ))) {
                wchar_t old_tgt[600] = {0};
                if (SUCCEEDED(psl->lpVtbl->GetPath(psl, old_tgt, 600, NULL, 0)) && old_tgt[0]) {
                    if (GetFileAttributesW(old_tgt) == INVALID_FILE_ATTRIBUTES) need_save = 1;
                }
            }
        }
        if (need_save) {
            psl->lpVtbl->SetPath(psl, wexe);
            psl->lpVtbl->SetWorkingDirectory(psl, wdir);
            psl->lpVtbl->SetIconLocation(psl, wexe, 0);
            psl->lpVtbl->SetDescription(psl, L"Sir\x00E8ne \x00B7 \x9AD8\x6E05\x672C\x5730\x97F3\x4E50\x64AD\x653E\x5668");
            if (SUCCEEDED(ppf->lpVtbl->Save(ppf, wlnk, TRUE))) did = 1;
        }
        ppf->lpVtbl->Release(ppf);
    }
    psl->lpVtbl->Release(psl);
    return did;
}

void be_buffer_size(int* out_w, int* out_h) {
    if (out_w) *out_w = g_backbuf ? g_buf_w : 0;
    if (out_h) *out_h = g_backbuf ? g_buf_h : 0;
}

int be_is_minimized(void) {
    return g_minimized || (g_hwnd && IsIconic(g_hwnd));
}

int be_poll_media_cmd(void) {
    if (g_mcmd_r == g_mcmd_w) return MEDIA_CMD_NONE;
    int cmd = g_mcmd_q[g_mcmd_r];
    g_mcmd_r = (g_mcmd_r + 1) & 15;
    return cmd;
}

void be_set_media_state(int has_track, int playing, float frac, const char* title, const char* artist) {
    if (!g_hwnd) return;
    if (!g_thumbbar_added) {
        static int retry_ticks = 0;
        if (retry_ticks == 45) {
            taskbar_on_created(g_hwnd, 0);
        }
        if (retry_ticks < 60) retry_ticks++;
    }

    int ht = has_track ? 1 : 0;
    int pl = (ht && playing) ? 1 : 0;
    int state_changed = (ht != g_tb_has_track || pl != g_tb_playing);
    g_tb_has_track = ht;
    g_tb_playing = pl;

    char cap[512];
    if (ht && title && title[0]) {
        if (artist && artist[0]) snprintf(cap, sizeof(cap), "%.180s - %.100s \xC2\xB7 Sir\xC3\xA8ne", title, artist);
        else                     snprintf(cap, sizeof(cap), "%.180s \xC2\xB7 Sir\xC3\xA8ne", title);
    } else {
        snprintf(cap, sizeof(cap), "Sir\xC3\xA8ne \xC2\xB7 \xE6\x9C\xAC\xE5\x9C\xB0\xE9\x9F\xB3\xE4\xB9\x90\xE6\x92\xAD\xE6\x94\xBE\xE5\x99\xA8");
    }
    int title_changed = (strcmp(cap, g_last_title_utf8) != 0);
    if (title_changed) {
        snprintf(g_last_title_utf8, sizeof(g_last_title_utf8), "%s", cap);
        wchar_t wcap[512] = {0};
        MultiByteToWideChar(CP_UTF8, 0, cap, -1, wcap, 511);
        if (wcap[0]) SetWindowTextW(g_hwnd, wcap);
    }

    if (g_taskbar3) {
        if (state_changed && g_thumbbar_added) {
            taskbar_update_buttons();
        }
        if (title_changed || state_changed) {
            char tip[512];
            if (ht && title && title[0]) {
                const char* icon = pl ? "\xE2\x96\xB6 " : "\xE2\x8F\xB8 ";
                if (artist && artist[0]) snprintf(tip, sizeof(tip), "%s%.180s - %.100s", icon, title, artist);
                else                     snprintf(tip, sizeof(tip), "%s%.180s", icon, title);
            } else {
                snprintf(tip, sizeof(tip), "Sir\xC3\xA8ne \xC2\xB7 \xE6\x9C\xAC\xE5\x9C\xB0\xE9\x9F\xB3\xE4\xB9\x90\xE6\x92\xAD\xE6\x94\xBE\xE5\x99\xA8");
            }
            wchar_t wtip[512] = {0};
            MultiByteToWideChar(CP_UTF8, 0, tip, -1, wtip, 511);
            if (wtip[0]) g_taskbar3->lpVtbl->SetThumbnailTooltip(g_taskbar3, g_hwnd, wtip);
        }
        int pct = ht ? (int)((frac < 0.0f ? 0.0f : (frac > 1.0f ? 1.0f : frac)) * 100.0f + 0.5f) : -1;
        if (state_changed || pct != g_tb_pct) {
            g_tb_pct = pct;
            if (!ht) {
                g_taskbar3->lpVtbl->SetProgressState(g_taskbar3, g_hwnd, TBPF_NOPROGRESS);
            } else {
                g_taskbar3->lpVtbl->SetProgressState(g_taskbar3, g_hwnd, pl ? TBPF_NORMAL : TBPF_PAUSED);
                g_taskbar3->lpVtbl->SetProgressValue(g_taskbar3, g_hwnd, (ULONGLONG)pct, 100ULL);
            }
        }
    }
}

#endif
