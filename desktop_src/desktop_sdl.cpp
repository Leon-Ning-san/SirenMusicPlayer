#if !defined(__SWITCH__) && !defined(_WIN32)
#include <SDL2/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include "backend.h"
#include "plat.h"

extern "C" {
extern const unsigned char poster_start[];
extern const unsigned char poster_end[];
}

namespace {

struct SdlWinDel { void operator()(SDL_Window* p) const { if (p) SDL_DestroyWindow(p); } };
struct SdlRenDel { void operator()(SDL_Renderer* p) const { if (p) SDL_DestroyRenderer(p); } };
struct SdlTexDel { void operator()(SDL_Texture* p) const { if (p) SDL_DestroyTexture(p); } };
struct SdlCurDel { void operator()(SDL_Cursor* p) const { if (p) SDL_FreeCursor(p); } };

struct TextEvent {
    int action = 0;
    std::string text;
};

struct SdlState {
    std::unique_ptr<SDL_Window, SdlWinDel> win;
    std::unique_ptr<SDL_Renderer, SdlRenDel> ren;
    std::unique_ptr<SDL_Texture, SdlTexDel> tex;
    std::unique_ptr<SDL_Cursor, SdlCurDel> cur_arrow;
    std::unique_ptr<SDL_Cursor, SdlCurDel> cur_hand;
    std::vector<uint32_t> backbuf;

    int running = 1;
    int vp_x = 0, vp_y = 0, vp_w = 1360, vp_h = 840;
    int win_w = 1360, win_h = 840;
    int buf_w = 0, buf_h = 0;
    float ui_scale = 1.0f;
    int is_fs = 0;

    int m_held = 0, m_down = 0, m_up = 0, was_down = 0;
    int m_x = 0, m_y = 0, prev_x = 0, prev_y = 0;
    int hover_x = -1, hover_y = -1;
    int cursor_hand = 0;
    int touch_reset = 0;
    int is_touch = 0;
    int idle_mode = 0;
    SDL_FingerID active_finger = 0;
    int finger_active = 0;
    int wheel_frame = 0;

    uint64_t key_held = 0, key_prev = 0, key_down = 0, key_down_pending = 0;
    int hotkey = 0;
    int text_enabled = 0;
    std::deque<TextEvent> text_q;

    std::string drop_path;
    int has_drop = 0;
    std::string cfg_dir;
    std::string last_title;
};

SdlState g_sdl;

void push_text_ev(int act, const char* u8) {
    if (g_sdl.text_q.size() >= 64) return;
    g_sdl.text_q.push_back(TextEvent{ act, u8 ? u8 : "" });
}

uint64_t sym_to_btn(SDL_Keycode k) {
    switch (k) {
    case SDLK_UP:        return BTN_UP;
    case SDLK_DOWN:      return BTN_DOWN;
    case SDLK_LEFT:      return BTN_LEFT;
    case SDLK_RIGHT:     return BTN_RIGHT;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
    case SDLK_SPACE:
    case SDLK_a:         return BTN_A;
    case SDLK_ESCAPE:
    case SDLK_b:         return BTN_B;
    case SDLK_x:
    case SDLK_f:         return BTN_X;
    case SDLK_y:         return BTN_Y;
    case SDLK_t:         return BTN_ZR;
    case SDLK_h:         return BTN_ZL;
    case SDLK_EQUALS:
    case SDLK_KP_PLUS:   return BTN_STICK_R_UP;
    case SDLK_MINUS:
    case SDLK_KP_MINUS:  return BTN_STICK_R_DOWN;
    default:             return 0;
    }
}

void update_sdl_viewport() {
    if (!g_sdl.win || !g_sdl.ren) return;
    if (SDL_GetWindowFlags(g_sdl.win.get()) & SDL_WINDOW_MINIMIZED) return;

    int ww = 1360, wh = 840, rw = 1360, rh = 840;
    SDL_GetWindowSize(g_sdl.win.get(), &ww, &wh);
    if (SDL_GetRendererOutputSize(g_sdl.ren.get(), &rw, &rh) != 0 || rw <= 0 || rh <= 0) {
        rw = ww;
        rh = wh;
    }
    ww = std::max(320, ww);
    wh = std::max(240, wh);
    rw = std::max(320, rw);
    rh = std::max(240, rh);
    g_sdl.win_w = ww;
    g_sdl.win_h = wh;
    g_sdl.vp_x = 0;
    g_sdl.vp_y = 0;
    g_sdl.vp_w = rw;
    g_sdl.vp_h = rh;
    g_sdl.ui_scale = std::clamp(static_cast<float>(rw) / static_cast<float>(ww), 1.0f, 3.0f);
}

bool ensure_sdl_buffers(int w, int h) {
    w = std::clamp(w, 320, 8192);
    h = std::clamp(h, 240, 8192);
    if (w == g_sdl.buf_w && h == g_sdl.buf_h && g_sdl.tex && !g_sdl.backbuf.empty()) return true;

    std::unique_ptr<SDL_Texture, SdlTexDel> ntex(
        SDL_CreateTexture(g_sdl.ren.get(), SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, w, h));
    if (!ntex) return false;
    try {
        std::vector<uint32_t> nbuf(static_cast<size_t>(w) * h, 0);
        g_sdl.backbuf.swap(nbuf);
    } catch (...) {
        return false;
    }
    g_sdl.tex = std::move(ntex);
    g_sdl.buf_w = w;
    g_sdl.buf_h = h;
    return true;
}

void map_mouse_xy(int wx, int wy, int* x, int* y) {
    *x = std::clamp(wx, 0, std::max(0, g_sdl.win_w - 1));
    *y = std::clamp(wy, 0, std::max(0, g_sdl.win_h - 1));
}

} // namespace

int be_init(void) {
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) return -1;

    int init_w = 1360, init_h = 840;
    SDL_Rect usable{};
    if (SDL_GetDisplayUsableBounds(0, &usable) == 0 && usable.w > 0 && usable.h > 0) {
        const int max_w = static_cast<int>(usable.w * 0.86);
        const int max_h = static_cast<int>(usable.h * 0.88);
        if (init_w > max_w) init_w = max_w;
        if (init_h > max_h) init_h = max_h;
        if (init_w < 860 && usable.w >= 860) init_w = 860;
        if (init_h < 540 && usable.h >= 540) init_h = 540;
    }

    g_sdl.win.reset(SDL_CreateWindow(
        "Sir\xC3\xA8ne \xC2\xB7 \xE6\x9C\xAC\xE5\x9C\xB0\xE9\x9F\xB3\xE4\xB9\x90\xE6\x92\xAD\xE6\x94\xBE\xE5\x99\xA8",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        init_w, init_h,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI));
    if (!g_sdl.win) return -1;
    SDL_SetWindowMinimumSize(g_sdl.win.get(), 840, 520);

    if (static_cast<size_t>(poster_end - poster_start) >= 320 * 320 * 4) {
        SDL_Surface* ico = SDL_CreateRGBSurfaceWithFormatFrom(
            const_cast<unsigned char*>(poster_start), 320, 320, 32, 320 * 4, SDL_PIXELFORMAT_RGBA32);
        if (ico) {
            SDL_SetWindowIcon(g_sdl.win.get(), ico);
            SDL_FreeSurface(ico);
        }
    }
    g_sdl.cur_arrow.reset(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW));
    g_sdl.cur_hand.reset(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND));

    g_sdl.ren.reset(SDL_CreateRenderer(g_sdl.win.get(), -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC));
    if (!g_sdl.ren) g_sdl.ren.reset(SDL_CreateRenderer(g_sdl.win.get(), -1, SDL_RENDERER_SOFTWARE));
    if (!g_sdl.ren) return -1;

    update_sdl_viewport();
    if (!ensure_sdl_buffers(g_sdl.vp_w, g_sdl.vp_h)) return -1;

    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    return 0;
}

void be_exit(void) {
    g_sdl.backbuf.clear();
    g_sdl.buf_w = g_sdl.buf_h = 0;
    g_sdl.cur_arrow.reset();
    g_sdl.cur_hand.reset();
    g_sdl.tex.reset();
    g_sdl.ren.reset();
    g_sdl.win.reset();
    SDL_Quit();
}

void be_viewport_size(int* out_w, int* out_h) {
    update_sdl_viewport();
    if (out_w) *out_w = g_sdl.vp_w;
    if (out_h) *out_h = g_sdl.vp_h;
}

void be_screen_metrics(int* out_phys_w, int* out_phys_h, float* out_scale) {
    update_sdl_viewport();
    if (out_phys_w) *out_phys_w = g_sdl.vp_w;
    if (out_phys_h) *out_phys_h = g_sdl.vp_h;
    if (out_scale)  *out_scale  = g_sdl.ui_scale;
}

void be_set_dark_titlebar(int dark) { (void)dark; }

void be_toggle_fullscreen(void) {
    if (!g_sdl.win) return;
    g_sdl.is_fs = !g_sdl.is_fs;
    SDL_SetWindowFullscreen(g_sdl.win.get(), g_sdl.is_fs ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

int be_is_fullscreen(void) { return g_sdl.is_fs; }

uint32_t* be_begin(int* stride) {
    update_sdl_viewport();
    ensure_sdl_buffers(g_sdl.vp_w, g_sdl.vp_h);
    if (stride) *stride = g_sdl.buf_w;
    return g_sdl.backbuf.empty() ? nullptr : g_sdl.backbuf.data();
}

void be_end(void) {
    if (!g_sdl.ren || !g_sdl.tex || g_sdl.backbuf.empty() || g_sdl.buf_w <= 0 || g_sdl.buf_h <= 0) return;
    SDL_UpdateTexture(g_sdl.tex.get(), nullptr, g_sdl.backbuf.data(), g_sdl.buf_w * 4);
    SDL_SetRenderDrawColor(g_sdl.ren.get(), 16, 18, 24, 255);
    SDL_RenderClear(g_sdl.ren.get());
    SDL_RenderCopy(g_sdl.ren.get(), g_sdl.tex.get(), nullptr, nullptr);
    SDL_RenderPresent(g_sdl.ren.get());
    if (g_sdl.idle_mode) SDL_Delay(20);
}

void be_input_update(void) {
    SDL_Event ev;
    g_sdl.wheel_frame = 0;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            g_sdl.running = 0;
            break;
        case SDL_WINDOWEVENT:
            if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                update_sdl_viewport();
            } else if (ev.window.event == SDL_WINDOWEVENT_LEAVE) {
                g_sdl.hover_x = -1;
                g_sdl.hover_y = -1;
            }
            break;
        case SDL_FINGERDOWN: {
            if (!g_sdl.finger_active) {
                g_sdl.finger_active = 1;
                g_sdl.active_finger = ev.tfinger.fingerId;
                g_sdl.is_touch = 1;
                g_sdl.idle_mode = 0;
                const int px = static_cast<int>(ev.tfinger.x * static_cast<float>(g_sdl.win_w));
                const int py = static_cast<int>(ev.tfinger.y * static_cast<float>(g_sdl.win_h));
                map_mouse_xy(px, py, &g_sdl.m_x, &g_sdl.m_y);
                g_sdl.hover_x = g_sdl.m_x;
                g_sdl.hover_y = g_sdl.m_y;
                g_sdl.m_held = 1;
                g_sdl.m_down = 1;
            }
            break;
        }
        case SDL_FINGERMOTION: {
            if (g_sdl.finger_active && ev.tfinger.fingerId == g_sdl.active_finger) {
                g_sdl.idle_mode = 0;
                const int px = static_cast<int>(ev.tfinger.x * static_cast<float>(g_sdl.win_w));
                const int py = static_cast<int>(ev.tfinger.y * static_cast<float>(g_sdl.win_h));
                map_mouse_xy(px, py, &g_sdl.m_x, &g_sdl.m_y);
                g_sdl.hover_x = g_sdl.m_x;
                g_sdl.hover_y = g_sdl.m_y;
            }
            break;
        }
        case SDL_FINGERUP: {
            if (g_sdl.finger_active && ev.tfinger.fingerId == g_sdl.active_finger) {
                g_sdl.idle_mode = 0;
                const int px = static_cast<int>(ev.tfinger.x * static_cast<float>(g_sdl.win_w));
                const int py = static_cast<int>(ev.tfinger.y * static_cast<float>(g_sdl.win_h));
                map_mouse_xy(px, py, &g_sdl.m_x, &g_sdl.m_y);
                g_sdl.m_held = 0;
                g_sdl.m_up = 1;
                g_sdl.finger_active = 0;
                g_sdl.hover_x = -1;
                g_sdl.hover_y = -1;
            }
            break;
        }
        case SDL_MOUSEBUTTONDOWN:
            if (ev.button.button == SDL_BUTTON_LEFT && ev.button.which != SDL_TOUCH_MOUSEID) {
                g_sdl.is_touch = 0;
                g_sdl.idle_mode = 0;
                map_mouse_xy(ev.button.x, ev.button.y, &g_sdl.m_x, &g_sdl.m_y);
                g_sdl.hover_x = g_sdl.m_x;
                g_sdl.hover_y = g_sdl.m_y;
                g_sdl.m_held = 1;
                g_sdl.m_down = 1;
            }
            break;
        case SDL_MOUSEMOTION:
            if (ev.motion.which != SDL_TOUCH_MOUSEID) {
                g_sdl.is_touch = 0;
                g_sdl.idle_mode = 0;
                map_mouse_xy(ev.motion.x, ev.motion.y, &g_sdl.hover_x, &g_sdl.hover_y);
                if (g_sdl.m_held) {
                    g_sdl.m_x = g_sdl.hover_x;
                    g_sdl.m_y = g_sdl.hover_y;
                }
            }
            break;
        case SDL_MOUSEBUTTONUP:
            if (ev.button.button == SDL_BUTTON_LEFT && ev.button.which != SDL_TOUCH_MOUSEID) {
                map_mouse_xy(ev.button.x, ev.button.y, &g_sdl.m_x, &g_sdl.m_y);
                g_sdl.m_held = 0;
                g_sdl.m_up = 1;
            }
            break;
        case SDL_MOUSEWHEEL:
            g_sdl.wheel_frame += ev.wheel.y;
            break;
        case SDL_TEXTINPUT:
            if (g_sdl.text_enabled) push_text_ev(0, ev.text.text);
            break;
        case SDL_KEYDOWN: {
            const SDL_Keycode k = ev.key.keysym.sym;
            const bool ctrl = (ev.key.keysym.mod & (KMOD_CTRL | KMOD_GUI)) != 0;
            if (g_sdl.text_enabled) {
                if (k == SDLK_BACKSPACE) { push_text_ev(1, nullptr); break; }
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER) { push_text_ev(2, nullptr); break; }
                if (k == SDLK_ESCAPE) { push_text_ev(3, nullptr); break; }
                break;
            }
            if (!ev.key.repeat) {
                if (k == SDLK_F11) { be_toggle_fullscreen(); break; }
                if ((ctrl && k == SDLK_f) || k == SDLK_SLASH) { g_sdl.hotkey = 's'; break; }
                if ((ctrl && k == SDLK_o) || k == SDLK_o) { g_sdl.hotkey = 'f'; break; }
                if (k == SDLK_m) { g_sdl.hotkey = 'm'; break; }
                if (k == SDLK_d) { g_sdl.hotkey = 'c'; break; }
                if (k == SDLK_v) { g_sdl.hotkey = 'v'; break; }
                if (k == SDLK_b) { g_sdl.hotkey = 'b'; break; }
            }
            if (k == SDLK_PAGEUP)   { g_sdl.wheel_frame += 3; break; }
            if (k == SDLK_PAGEDOWN) { g_sdl.wheel_frame -= 3; break; }
            const uint64_t b = sym_to_btn(k);
            if (b) {
                if (!(g_sdl.key_held & b)) g_sdl.key_down_pending |= b;
                g_sdl.key_held |= b;
            }
            break;
        }
        case SDL_KEYUP: {
            const uint64_t b = sym_to_btn(ev.key.keysym.sym);
            if (b) g_sdl.key_held &= ~b;
            break;
        }
        case SDL_DROPFILE:
            if (ev.drop.file) {
                g_sdl.drop_path = ev.drop.file;
                g_sdl.has_drop = 1;
                SDL_free(ev.drop.file);
            }
            break;
        default:
            break;
        }
    }
    g_sdl.key_down = g_sdl.key_down_pending | (g_sdl.key_held & ~g_sdl.key_prev);
    g_sdl.key_down_pending = 0;
    g_sdl.key_prev = g_sdl.key_held;
}

uint64_t be_btn_down(void) { return g_sdl.key_down; }
uint64_t be_btn_held(void) { return g_sdl.key_held; }
void be_sleep_ms(int ms) { SDL_Delay(static_cast<Uint32>(ms)); }
int be_should_run(void) { return g_sdl.running; }

void be_touch_update(TouchInfo* out) {
    std::memset(out, 0, sizeof(*out));
    if (g_sdl.touch_reset) {
        g_sdl.was_down = 0; g_sdl.m_down = 0; g_sdl.m_up = 0; g_sdl.touch_reset = 0;
        return;
    }
    if (g_sdl.m_held || g_sdl.m_down) {
        out->held = 1;
        out->x = g_sdl.m_x;
        out->y = g_sdl.m_y;
        if (!g_sdl.was_down || g_sdl.m_down) {
            out->down = 1;
            g_sdl.prev_x = g_sdl.m_x;
            g_sdl.prev_y = g_sdl.m_y;
        }
        out->dx = g_sdl.m_x - g_sdl.prev_x;
        out->dy = g_sdl.m_y - g_sdl.prev_y;
        g_sdl.prev_x = g_sdl.m_x;
        g_sdl.prev_y = g_sdl.m_y;
        g_sdl.was_down = 1;
        g_sdl.m_down = 0;
    } else if (g_sdl.was_down || g_sdl.m_up) {
        out->up = 1;
        out->x = g_sdl.m_x;
        out->y = g_sdl.m_y;
        g_sdl.was_down = 0;
        g_sdl.m_up = 0;
    }
}

void be_touch_reset(void) { g_sdl.touch_reset = 1; }
int be_is_desktop(void) { return 1; }
int be_wheel_delta(void) { return g_sdl.wheel_frame; }

const char* be_poll_dropped_path(void) {
    if (!g_sdl.has_drop) return nullptr;
    g_sdl.has_drop = 0;
    return g_sdl.drop_path.c_str();
}

int be_pick_folder(char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = 0;
#ifdef __APPLE__
    const char* cmd = "osascript -e 'POSIX path of (choose folder with prompt \"\xE9\x80\x89\xE6\x8B\xA9 Sir\xC3\xA8ne \xE6\x9C\xAC\xE5\x9C\xB0\xE9\x9F\xB3\xE4\xB9\x90\xE6\x96\x87\xE4\xBB\xB6\xE5\xA4\xB9\")' 2>/dev/null";
#else
    const char* cmd =
        "(zenity --file-selection --directory --title=\"\xE9\x80\x89\xE6\x8B\xA9 Sir\xC3\xA8ne \xE6\x9C\xAC\xE5\x9C\xB0\xE9\x9F\xB3\xE4\xB9\x90\xE6\x96\x87\xE4\xBB\xB6\xE5\xA4\xB9\" 2>/dev/null "
        "|| kdialog --getexistingdirectory \"$HOME\" --title \"\xE9\x80\x89\xE6\x8B\xA9 Sir\xC3\xA8ne \xE6\x9C\xAC\xE5\x9C\xB0\xE9\x9F\xB3\xE4\xB9\x90\xE6\x96\x87\xE4\xBB\xB6\xE5\xA4\xB9\" 2>/dev/null)";
#endif
    FILE* pp = popen(cmd, "r");
    if (!pp) return 0;
    if (fgets(out, static_cast<int>(cap), pp)) {
        size_t len = std::strlen(out);
        while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r' || out[len - 1] == '/'))
            out[--len] = 0;
        if (len == 0) out[0] = '/';
    }
    pclose(pp);
    be_touch_reset();
    return out[0] ? 1 : 0;
}

int be_pick_image(char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = 0;
#ifdef __APPLE__
    const char* cmd = "osascript -e 'POSIX path of (choose file with prompt \"\xE9\x80\x89\xE6\x8B\xA9 Sir\xC3\xA8ne \xE8\x83\x8C\xE6\x99\xAF\xE5\x9B\xBE\xE7\x89\x87\" of type {\"public.image\"})' 2>/dev/null";
#else
    const char* cmd =
        "(zenity --file-selection --title=\"\xE9\x80\x89\xE6\x8B\xA9 Sir\xC3\xA8ne \xE8\x83\x8C\xE6\x99\xAF\xE5\x9B\xBE\xE7\x89\x87\" --file-filter=\"Images | *.png *.jpg *.jpeg *.bmp *.tga *.gif *.PNG *.JPG *.JPEG *.BMP *.TGA *.GIF\" 2>/dev/null "
        "|| kdialog --getopenfilename \"$HOME\" \"*.png *.jpg *.jpeg *.bmp *.tga *.gif|Images\" --title \"\xE9\x80\x89\xE6\x8B\xA9 Sir\xC3\xA8ne \xE8\x83\x8C\xE6\x99\xAF\xE5\x9B\xBE\xE7\x89\x87\" 2>/dev/null)";
#endif
    FILE* pp = popen(cmd, "r");
    if (!pp) return 0;
    if (fgets(out, static_cast<int>(cap), pp)) {
        size_t len = std::strlen(out);
        while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r')) out[--len] = 0;
    }
    pclose(pp);
    be_touch_reset();
    return out[0] ? 1 : 0;
}

void be_text_input(int enable) {
    g_sdl.text_enabled = enable;
    if (enable) {
        g_sdl.text_q.clear();
        g_sdl.key_held = 0;
        SDL_StartTextInput();
    } else {
        SDL_StopTextInput();
    }
}

int be_poll_text_char(char* utf8_out, size_t cap, int* action) {
    if (g_sdl.text_q.empty()) return 0;
    TextEvent e = g_sdl.text_q.front();
    g_sdl.text_q.pop_front();
    if (action) *action = e.action;
    if (utf8_out && cap > 0) snprintf(utf8_out, cap, "%s", e.text.c_str());
    return 1;
}

int be_poll_hotkey(void) {
    const int h = g_sdl.hotkey;
    g_sdl.hotkey = 0;
    return h;
}

const char* be_config_dir(void) {
    if (g_sdl.cfg_dir.empty()) {
        const char* home = std::getenv("HOME");
#ifdef __APPLE__
        if (home && *home) g_sdl.cfg_dir = std::string(home) + "/Library/Application Support/Sirene";
        else               g_sdl.cfg_dir = "./Sirene_config";
#else
        const char* xdg = std::getenv("XDG_CONFIG_HOME");
        if (xdg && *xdg)       g_sdl.cfg_dir = std::string(xdg) + "/sirene";
        else if (home && *home) g_sdl.cfg_dir = std::string(home) + "/.config/sirene";
        else                    g_sdl.cfg_dir = "./.sirene_config";
#endif
    }
    return g_sdl.cfg_dir.c_str();
}

void be_set_cli_path(const char* p) {
    if (!p || !*p) return;
    g_sdl.drop_path = p;
    g_sdl.has_drop = 1;
}

int be_default_music_dirs(char dirs[][600], int max_dirs) {
    int n = 0;
    const char* home = std::getenv("HOME");
    if (home && *home) {
        if (n < max_dirs) snprintf(dirs[n++], 600, "%s/Music", home);
        if (n < max_dirs) snprintf(dirs[n++], 600, "%s/music", home);
    }
    if (n < max_dirs) snprintf(dirs[n++], 600, "./music");
    return n;
}

void be_mouse_pos(int* x, int* y) {
    if (x) *x = g_sdl.hover_x;
    if (y) *y = g_sdl.hover_y;
}

void be_set_cursor(int hand) {
    const int h = hand ? 1 : 0;
    if (h != g_sdl.cursor_hand) {
        g_sdl.cursor_hand = h;
        SDL_Cursor* c = h ? g_sdl.cur_hand.get() : g_sdl.cur_arrow.get();
        if (c) SDL_SetCursor(c);
    }
}

int be_ensure_desktop_shortcut(int only_repair) { (void)only_repair; return 0; }

void be_buffer_size(int* out_w, int* out_h) {
    if (out_w) *out_w = g_sdl.buf_w;
    if (out_h) *out_h = g_sdl.buf_h;
}

int be_is_minimized(void) {
    return g_sdl.win ? ((SDL_GetWindowFlags(g_sdl.win.get()) & SDL_WINDOW_MINIMIZED) != 0) : 0;
}

int be_poll_media_cmd(void) { return 0; }

void be_set_media_state(int has_track, int playing, float frac, const char* title, const char* artist) {
    (void)playing; (void)frac;
    if (!g_sdl.win) return;
    char cap[512]{};
    if (has_track && title && title[0]) {
        if (artist && artist[0]) snprintf(cap, sizeof(cap), "%.180s - %.100s \xC2\xB7 Sir\xC3\xA8ne", title, artist);
        else                     snprintf(cap, sizeof(cap), "%.180s \xC2\xB7 Sir\xC3\xA8ne", title);
    } else {
        snprintf(cap, sizeof(cap), "Sir\xC3\xA8ne \xC2\xB7 \xE6\x9C\xAC\xE5\x9C\xB0\xE9\x9F\xB3\xE4\xB9\x90\xE6\x92\xAD\xE6\x94\xBE\xE5\x99\xA8");
    }
    if (g_sdl.last_title != cap) {
        g_sdl.last_title = cap;
        SDL_SetWindowTitle(g_sdl.win.get(), cap);
    }
}

int  be_input_is_touch(void) { return g_sdl.is_touch; }
void be_set_idle_mode(int idle) { g_sdl.idle_mode = idle ? 1 : 0; }

#endif
