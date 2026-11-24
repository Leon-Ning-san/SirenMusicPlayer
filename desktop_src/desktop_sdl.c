#if !defined(__SWITCH__) && !defined(_WIN32)
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "backend.h"
#include "plat.h"

static SDL_Window*   g_win = NULL;
static SDL_Renderer* g_ren = NULL;
static SDL_Texture*  g_tex = NULL;
static uint32_t*     g_backbuf = NULL;
static int           g_running = 1;
static int           g_vp_x = 0, g_vp_y = 0, g_vp_w = BE_W, g_vp_h = BE_H;
static int           g_win_vp_x = 0, g_win_vp_y = 0, g_win_vp_w = BE_W, g_win_vp_h = BE_H;
static int           g_buf_w = 0, g_buf_h = 0;

static int g_m_held = 0, g_m_down = 0, g_m_up = 0, g_was_down = 0;
static int g_m_x = 0, g_m_y = 0, g_prev_x = 0, g_prev_y = 0;
static int g_hover_x = -1, g_hover_y = -1;
static int g_cursor_hand = 0;
static SDL_Cursor* g_cur_arrow = NULL;
static SDL_Cursor* g_cur_hand = NULL;
static int g_touch_reset = 0;
static int g_wheel_frame = 0;
extern const unsigned char poster_start[];
extern const unsigned char poster_end[];

static uint64_t g_key_held = 0, g_key_prev = 0, g_key_down = 0, g_key_down_pending = 0;
static int      g_hotkey = 0;
static int      g_text_enabled = 0;

typedef struct {
    int action;
    char text[32];
} TextEv;
static TextEv g_tev[32];
static int g_tev_r = 0, g_tev_w = 0;

static char g_drop_path[600] = {0};
static int  g_has_drop = 0;
static char g_cfg_dir[512] = {0};

static void push_text_ev(int act, const char* u8) {
    int nxt = (g_tev_w + 1) & 31;
    if (nxt == g_tev_r) return;
    g_tev[g_tev_w].action = act;
    snprintf(g_tev[g_tev_w].text, sizeof(g_tev[g_tev_w].text), "%s", u8 ? u8 : "");
    g_tev_w = nxt;
}

static uint64_t sym_to_btn(SDL_Keycode k) {
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
    default: return 0;
    }
}

static void calc_fit_vp(int cw, int ch, int* vx, int* vy, int* vw, int* vh) {
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    double target_r = (double)BE_W / (double)BE_H;
    double win_r = (double)cw / (double)ch;
    if (win_r > target_r) {
        *vh = ch;
        *vw = (int)(ch * target_r + 0.5);
        *vx = (cw - *vw) / 2;
        *vy = 0;
    } else {
        *vw = cw;
        *vh = (int)(cw / target_r + 0.5);
        *vx = 0;
        *vy = (ch - *vh) / 2;
    }
    if (*vw < 160) *vw = 160;
    if (*vh < 90)  *vh = 90;
}

static void update_sdl_viewport(void) {
    if (!g_win || !g_ren) return;
    int ww = BE_W, wh = BE_H, rw = BE_W, rh = BE_H;
    SDL_GetWindowSize(g_win, &ww, &wh);
    if (SDL_GetRendererOutputSize(g_ren, &rw, &rh) != 0 || rw <= 0 || rh <= 0) {
        rw = ww; rh = wh;
    }
    calc_fit_vp(ww, wh, &g_win_vp_x, &g_win_vp_y, &g_win_vp_w, &g_win_vp_h);
    calc_fit_vp(rw, rh, &g_vp_x, &g_vp_y, &g_vp_w, &g_vp_h);
}

static void ensure_sdl_buffers(int w, int h) {
    if (w < 160) w = 160;
    if (h < 90) h = 90;
    if (w == g_buf_w && h == g_buf_h && g_tex && g_backbuf) return;
    free(g_backbuf);
    if (g_tex) SDL_DestroyTexture(g_tex);
    g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    g_backbuf = (uint32_t*)calloc((size_t)w * h, 4);
    g_buf_w = w;
    g_buf_h = h;
}

int be_init(void) {
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) return -1;

    int init_w = BE_W, init_h = BE_H;
    SDL_Rect usable;
    if (SDL_GetDisplayUsableBounds(0, &usable) == 0 && usable.w > 0 && usable.h > 0) {
        if (usable.w < BE_W + 40 || usable.h < BE_H + 60) {
            int max_w = (int)(usable.w * 0.92);
            int max_h = (int)(usable.h * 0.88);
            if (max_w * BE_H <= max_h * BE_W) {
                init_w = max_w;
                init_h = init_w * BE_H / BE_W;
            } else {
                init_h = max_h;
                init_w = init_h * BE_W / BE_H;
            }
        }
    }

    g_win = SDL_CreateWindow(
        "Sirène · 本地音乐播放器",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        init_w, init_h,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!g_win) return -1;

    if ((size_t)(poster_end - poster_start) >= 320 * 320 * 4) {
        SDL_Surface* ico = SDL_CreateRGBSurfaceWithFormatFrom(
            (void*)poster_start, 320, 320, 32, 320 * 4, SDL_PIXELFORMAT_RGBA32);
        if (ico) {
            SDL_SetWindowIcon(g_win, ico);
            SDL_FreeSurface(ico);
        }
    }
    g_cur_arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
    g_cur_hand  = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);

    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_ren) g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
    if (!g_ren) return -1;

    update_sdl_viewport();
    ensure_sdl_buffers(g_vp_w, g_vp_h);
    if (!g_tex || !g_backbuf) return -1;

    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    return 0;
}

void be_exit(void) {
    free(g_backbuf); g_backbuf = NULL;
    g_buf_w = g_buf_h = 0;
    if (g_cur_arrow) { SDL_FreeCursor(g_cur_arrow); g_cur_arrow = NULL; }
    if (g_cur_hand)  { SDL_FreeCursor(g_cur_hand);  g_cur_hand = NULL; }
    if (g_tex) { SDL_DestroyTexture(g_tex); g_tex = NULL; }
    if (g_ren) { SDL_DestroyRenderer(g_ren); g_ren = NULL; }
    if (g_win) { SDL_DestroyWindow(g_win); g_win = NULL; }
    SDL_Quit();
}

void be_viewport_size(int* out_w, int* out_h) {
    update_sdl_viewport();
    if (out_w) *out_w = g_vp_w;
    if (out_h) *out_h = g_vp_h;
}

uint32_t* be_begin(int* stride) {
    update_sdl_viewport();
    ensure_sdl_buffers(g_vp_w, g_vp_h);
    if (stride) *stride = g_buf_w;
    return g_backbuf;
}

void be_end(void) {
    if (!g_ren || !g_tex || !g_backbuf || g_buf_w <= 0 || g_buf_h <= 0) return;
    SDL_UpdateTexture(g_tex, NULL, g_backbuf, g_buf_w * 4);
    SDL_SetRenderDrawColor(g_ren, 22, 26, 34, 255);
    SDL_RenderClear(g_ren);
    SDL_Rect dst = { g_vp_x, g_vp_y, g_vp_w, g_vp_h };
    SDL_RenderCopy(g_ren, g_tex, NULL, &dst);
    SDL_RenderPresent(g_ren);
}

static void map_mouse_xy(int wx, int wy, int* x, int* y) {
    int lx = (int)((double)(wx - g_win_vp_x) * (double)BE_W / (double)(g_win_vp_w > 0 ? g_win_vp_w : BE_W));
    int ly = (int)((double)(wy - g_win_vp_y) * (double)BE_H / (double)(g_win_vp_h > 0 ? g_win_vp_h : BE_H));
    if (lx < 0) lx = 0;
    if (lx >= BE_W) lx = BE_W - 1;
    if (ly < 0) ly = 0;
    if (ly >= BE_H) ly = BE_H - 1;
    *x = lx;
    *y = ly;
}

void be_input_update(void) {
    SDL_Event ev;
    g_wheel_frame = 0;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            g_running = 0;
            break;
        case SDL_WINDOWEVENT:
            if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                update_sdl_viewport();
            } else if (ev.window.event == SDL_WINDOWEVENT_LEAVE) {
                g_hover_x = -1;
                g_hover_y = -1;
            }
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (ev.button.button == SDL_BUTTON_LEFT) {
                map_mouse_xy(ev.button.x, ev.button.y, &g_m_x, &g_m_y);
                g_hover_x = g_m_x; g_hover_y = g_m_y;
                g_m_held = 1; g_m_down = 1;
            }
            break;
        case SDL_MOUSEMOTION:
            map_mouse_xy(ev.motion.x, ev.motion.y, &g_hover_x, &g_hover_y);
            if (g_m_held) {
                g_m_x = g_hover_x; g_m_y = g_hover_y;
            }
            break;
        case SDL_MOUSEBUTTONUP:
            if (ev.button.button == SDL_BUTTON_LEFT) {
                map_mouse_xy(ev.button.x, ev.button.y, &g_m_x, &g_m_y);
                g_m_held = 0; g_m_up = 1;
            }
            break;
        case SDL_MOUSEWHEEL:
            g_wheel_frame += ev.wheel.y;
            break;
        case SDL_TEXTINPUT:
            if (g_text_enabled) push_text_ev(0, ev.text.text);
            break;
        case SDL_KEYDOWN: {
            SDL_Keycode k = ev.key.keysym.sym;
            int ctrl = (ev.key.keysym.mod & (KMOD_CTRL | KMOD_GUI)) != 0;
            if (g_text_enabled) {
                if (k == SDLK_BACKSPACE) { push_text_ev(1, NULL); break; }
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER) { push_text_ev(2, NULL); break; }
                if (k == SDLK_ESCAPE) { push_text_ev(3, NULL); break; }
                break;
            }
            if (!ev.key.repeat) {
                if ((ctrl && k == SDLK_f) || k == SDLK_SLASH || k == SDLK_s) { g_hotkey = 's'; break; }
                if ((ctrl && k == SDLK_o) || k == SDLK_o) { g_hotkey = 'f'; break; }
                if (k == SDLK_m) { g_hotkey = 'm'; break; }
                if (k == SDLK_d) { g_hotkey = 'c'; break; }
                if (k == SDLK_v) { g_hotkey = 'v'; break; }
            }
            if (k == SDLK_PAGEUP)   { g_wheel_frame += 3; break; }
            if (k == SDLK_PAGEDOWN) { g_wheel_frame -= 3; break; }
            uint64_t b = sym_to_btn(k);
            if (b) {
                if (!(g_key_held & b)) g_key_down_pending |= b;
                g_key_held |= b;
            }
            break;
        }
        case SDL_KEYUP: {
            uint64_t b = sym_to_btn(ev.key.keysym.sym);
            if (b) g_key_held &= ~b;
            break;
        }
        case SDL_DROPFILE:
            if (ev.drop.file) {
                snprintf(g_drop_path, sizeof(g_drop_path), "%s", ev.drop.file);
                g_has_drop = 1;
                SDL_free(ev.drop.file);
            }
            break;
        }
    }
    g_key_down = g_key_down_pending | (g_key_held & ~g_key_prev);
    g_key_down_pending = 0;
    g_key_prev = g_key_held;
}

uint64_t be_btn_down(void) { return g_key_down; }
uint64_t be_btn_held(void) { return g_key_held; }
void be_sleep_ms(int ms) { SDL_Delay((Uint32)ms); }
int be_should_run(void) { return g_running; }

void be_touch_update(TouchInfo* out) {
    memset(out, 0, sizeof(*out));
    if (g_touch_reset) {
        g_was_down = 0; g_m_down = 0; g_m_up = 0; g_touch_reset = 0;
        return;
    }
    if (g_m_held || g_m_down) {
        out->held = 1;
        out->x = g_m_x; out->y = g_m_y;
        if (!g_was_down || g_m_down) {
            out->down = 1;
            g_prev_x = g_m_x; g_prev_y = g_m_y;
        }
        out->dx = g_m_x - g_prev_x; out->dy = g_m_y - g_prev_y;
        g_prev_x = g_m_x; g_prev_y = g_m_y;
        g_was_down = 1; g_m_down = 0;
    } else if (g_was_down || g_m_up) {
        out->up = 1;
        out->x = g_m_x; out->y = g_m_y;
        g_was_down = 0; g_m_up = 0;
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
    if (!out || cap == 0) return 0;
    out[0] = 0;
#ifdef __APPLE__
    const char* cmd = "osascript -e 'POSIX path of (choose folder with prompt \"选择 Sirène 本地音乐文件夹\")' 2>/dev/null";
#else
    const char* cmd =
        "(zenity --file-selection --directory --title=\"选择 Sirène 本地音乐文件夹\" 2>/dev/null "
        "|| kdialog --getexistingdirectory \"$HOME\" --title \"选择 Sirène 本地音乐文件夹\" 2>/dev/null)";
#endif
    FILE* pp = popen(cmd, "r");
    if (!pp) return 0;
    if (fgets(out, (int)cap, pp)) {
        size_t len = strlen(out);
        while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r' || out[len - 1] == '/'))
            out[--len] = 0;
        if (len == 0) out[0] = '/';
    }
    pclose(pp);
    be_touch_reset();
    return out[0] ? 1 : 0;
}

void be_text_input(int enable) {
    g_text_enabled = enable;
    if (enable) {
        g_tev_r = g_tev_w = 0;
        g_key_held = 0;
        SDL_StartTextInput();
    } else {
        SDL_StopTextInput();
    }
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
        const char* home = getenv("HOME");
#ifdef __APPLE__
        if (home && *home) snprintf(g_cfg_dir, sizeof(g_cfg_dir), "%s/Library/Application Support/Sirene", home);
        else snprintf(g_cfg_dir, sizeof(g_cfg_dir), "./Sirene_config");
#else
        const char* xdg = getenv("XDG_CONFIG_HOME");
        if (xdg && *xdg) snprintf(g_cfg_dir, sizeof(g_cfg_dir), "%s/sirene", xdg);
        else if (home && *home) snprintf(g_cfg_dir, sizeof(g_cfg_dir), "%s/.config/sirene", home);
        else snprintf(g_cfg_dir, sizeof(g_cfg_dir), "./.sirene_config");
#endif
    }
    return g_cfg_dir;
}

void be_set_cli_path(const char* p) {
    if (!p || !*p) return;
    snprintf(g_drop_path, sizeof(g_drop_path), "%s", p);
    g_has_drop = 1;
}

int be_default_music_dirs(char dirs[][600], int max_dirs) {
    int n = 0;
    const char* home = getenv("HOME");
    if (home && *home) {
        if (n < max_dirs) snprintf(dirs[n++], 600, "%s/Music", home);
        if (n < max_dirs) snprintf(dirs[n++], 600, "%s/music", home);
    }
    if (n < max_dirs) snprintf(dirs[n++], 600, "./music");
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
        SDL_Cursor* c = h ? g_cur_hand : g_cur_arrow;
        if (c) SDL_SetCursor(c);
    }
}
#endif
