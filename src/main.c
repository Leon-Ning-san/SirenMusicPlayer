#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#include "backend.h"
#include "draw.h"
#include "audio.h"
#include "meta.h"
#include "lrc.h"
#include "util.h"
#include "decode.h"

extern const unsigned char font_ttf_start[];
extern const unsigned char font_ttf_end[];
extern const unsigned char poster_start[];
extern const unsigned char poster_end[];

#define UI_LIST   0
#define UI_PLAYER 1

#define MAX_TRACKS 8192

// ---------- 播放模式 ----------
#define MODE_LOOP_ALL 0
#define MODE_LOOP_ONE 1
#define MODE_SHUFFLE  2
#define MODE_COUNT    3

static const char* mode_names[MODE_COUNT] = { "列表循环", "单曲循环", "随机播放" };
static const char* mode_short[MODE_COUNT] = { "循环", "单曲", "随机" };
static int play_mode = MODE_LOOP_ALL;
static uint32_t rng_state = 0x1234567u;

static uint32_t next_rand(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    if (rng_state == 0) rng_state = 0x9E3779B9u;
    return rng_state;
}

// ---------- 屏幕方向（横屏 / 竖屏自动旋转） ----------
#define ORI_LAND   0   // 正常横持
#define ORI_PORT_A 1   // 顺时针竖持
#define ORI_PORT_B 2   // 逆时针竖持
static int ori_mode = 0;       // 0=自动 1=横屏 2=竖A 3=竖B
static int ori = ORI_LAND;
static int SCR_W = 1280, SCR_H = 720;
static int portrait(void) { return ori != ORI_LAND; }

// ---------- 布局（Switch Glass & Material 大卡片） ----------
static int L_TOP, L_BAR, L_ROW, L_THUMB, L_M, L_TITLE, L_ART, L_ARTIST2;
static void update_layout(void) {
    L_M = portrait() ? 22 : 28;
    L_THUMB = 84;
    if (portrait()) {
        L_TOP = 138;
        L_BAR = 96;
        L_ROW = 134;
        L_TITLE = 27;
        L_ART = 18;
        L_ARTIST2 = 16;
    } else {
        L_TOP = 82;
        L_BAR = 76;
        L_ROW = 112;
        L_TITLE = 22;
        L_ART = 16;
        L_ARTIST2 = 14;
    }
}

static int list_area_h(void) {
    int h = SCR_H - L_TOP - L_BAR;
    return h < L_ROW ? L_ROW : h;
}

static int rows_visible(void) {
    int r = list_area_h() / L_ROW;
    return r < 1 ? 1 : r;
}

// ---------- 主题配色 ----------
#define THEME_TEAL    0
#define THEME_BLUE    1
#define THEME_FOREST  2
#define THEME_COUNT   3

static uint32_t C_BG, C_SURFACE, C_SURF2, C_CARD, C_CARD2;
static uint32_t C_PRIMARY, C_PRIMDK, C_ONPRIM, C_PLAYBG;
static uint32_t C_TXT, C_TXT2, C_TXT3, C_LINE, C_HEART;

typedef struct {
    uint32_t bg, surface, surf2, card, card2;
    uint32_t primary, primdk, onprim, playbg;
    uint32_t txt, txt2, txt3, line;
} Theme;

static const Theme themes[THEME_COUNT] = {
    // 青绿（Material You 原配色）
    { COL(0x0E,0x11,0x15,0xFF), COL(0x18,0x1C,0x22,0xFF), COL(0x23,0x29,0x31,0xFF),
      COL(0x1C,0x21,0x27,0xFF), COL(0x25,0x2C,0x34,0xFF),
      COL(0x52,0xD6,0xC0,0xFF), COL(0x2F,0x94,0x85,0xFF), COL(0x0A,0x26,0x22,0xFF),
      COL(0x19,0x36,0x33,0xFF),
      COL(0xF0,0xF4,0xF7,0xFF), COL(0xC0,0xC8,0xD2,0xFF), COL(0x90,0x9A,0xA6,0xFF),
      COL(0x2E,0x36,0x3F,0xFF) },
    // 深空蓝
    { COL(0x0B,0x12,0x1E,0xFF), COL(0x14,0x1E,0x2D,0xFF), COL(0x1E,0x2B,0x3F,0xFF),
      COL(0x17,0x22,0x33,0xFF), COL(0x20,0x2E,0x43,0xFF),
      COL(0x5A,0xA9,0xFF,0xFF), COL(0x2F,0x6F,0xB0,0xFF), COL(0x06,0x18,0x2B,0xFF),
      COL(0x17,0x2E,0x4B,0xFF),
      COL(0xEC,0xF2,0xFA,0xFF), COL(0xBC,0xC9,0xDA,0xFF), COL(0x8B,0x9C,0xB2,0xFF),
      COL(0x28,0x37,0x4C,0xFF) },
    // 黑绿（苔绿）
    { COL(0x07,0x10,0x0C,0xFF), COL(0x0E,0x1A,0x14,0xFF), COL(0x17,0x28,0x20,0xFF),
      COL(0x11,0x1F,0x18,0xFF), COL(0x19,0x2C,0x22,0xFF),
      COL(0x7D,0xDC,0x8C,0xFF), COL(0x3E,0x8A,0x50,0xFF), COL(0x07,0x1C,0x0E,0xFF),
      COL(0x13,0x34,0x25,0xFF),
      COL(0xEA,0xF5,0xED,0xFF), COL(0xB8,0xD0,0xC0,0xFF), COL(0x88,0xA4,0x92,0xFF),
      COL(0x1F,0x36,0x2A,0xFF) }
};

static const char* theme_names[THEME_COUNT] = { "青绿", "深空蓝", "苔绿" };
static int cur_theme = THEME_TEAL;

static void apply_theme(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= THEME_COUNT) idx = THEME_COUNT - 1;
    cur_theme = idx;
    const Theme* t = &themes[idx];
    C_BG = t->bg; C_SURFACE = t->surface; C_SURF2 = t->surf2;
    C_CARD = t->card; C_CARD2 = t->card2;
    C_PRIMARY = t->primary; C_PRIMDK = t->primdk;
    C_ONPRIM = t->onprim; C_PLAYBG = t->playbg;
    C_TXT = t->txt; C_TXT2 = t->txt2; C_TXT3 = t->txt3; C_LINE = t->line;
    C_HEART = COL(0xFF, 0x5C, 0x7A, 0xFF);
}

typedef struct {
    char path[600];
    TrackMeta meta;
    int meta_state;     // 0 = 未尝试, 1 = 已尝试
    uint32_t* thumb;    // 52x52 底部播放条缩略图
    uint32_t* thumb84;  // 84x84 列表大卡片缩略图
    int fav;            // 收藏标记
} Track;

#define VIEW_HOME     0
#define VIEW_SEARCH   1
#define VIEW_FAVORITE 2

static Track* tracks = NULL;
static int ntracks = 0;
static int loaded_thumbs = 0;

static int ui_state = UI_LIST;
static int view = VIEW_HOME;
static int sel = 0;
static int playing_idx = -1;
static int loading = 0;

static Font* font = NULL;
static Canvas* screen = NULL;
static Canvas* list_bg = NULL;

// 播放页资源
static TrackMeta meta;
static Lyrics* lyr = NULL;
static uint32_t* cover = NULL;       // 360x360 当前曲目大图
static uint32_t* cover_prev = NULL;
static uint32_t* poster36 = NULL;
static uint32_t* poster52 = NULL;
static uint32_t* poster84 = NULL;
static Canvas* bg = NULL;
static char p_title[256];
static char p_artist[256];
static char p_fmt[12];
static float bars[28];
static float bar_peaks[28];

static int vol_overlay_timer = 0;
static int show_diag = 0;

// ---------- 提示气泡 ----------
static char toast_text[96];
static int toast_timer = 0;
static void show_toast(const char* s) {
    snprintf(toast_text, sizeof(toast_text), "%s", s);
    toast_timer = 90;
}

// ---------- 动画 & 平滑像素滚动 ----------
static double g_time = 0;
static double g_dt = 0.016;
static uint64_t g_last_tick = 0;
static float scroll_px = 0.0f;
static float anim_scroll_px = 0.0f;
static float scroll_vel = 0.0f;
static float anim_cover = 1.0f;
static int cover_ready = 0;
static Canvas* trans_shot = NULL;
static float trans_t = 1.0f;
static float touch_fx_alpha = 0;
static float touch_fx_x = 0, touch_fx_y = 0;
static double show_pos = 0;

// Material 水波纹
static float ripple_t = 1.0f;
static int ripple_x, ripple_y;
static int ripple_row = -1;
static int ripple_cw, ripple_ch;

// 播放页交互布局
static int ui_px0, ui_px1, ui_py;
static int ui_cx, ui_cy, ui_gap, ui_gap2;
static int ui_cover_x, ui_cover_y, ui_cover_sz;

// 触摸交互状态
static TouchInfo touch_now;
static int touch_drag = 0;
static int touch_moved = 0;
static int touch_scrub = 0;
static int touch_drag_start_y = 0;
static int touch_last_y = 0;
static int touch_press_idx = -1;
static char search_q[128] = "";
static int fav_count = 0;

// 可点击区域结构体
typedef struct { int x, y, w, h; } Rect;
static Rect rc_tab_all, rc_tab_fav, rc_btn_search, rc_btn_theme, rc_btn_ori, rc_btn_diag;
static Rect rc_mini_prev, rc_mini_play, rc_mini_next;
static Rect rc_p_back, rc_p_ori, rc_p_diag;

// ---------- 设置 & 收藏持久化 ----------
static void save_settings(void) {
    mkdir("sdmc:/Siren", 0777);
    FILE* f = fopen("sdmc:/Siren/settings.cfg", "w");
    if (f) {
        fprintf(f, "%d %d %.2f\n", cur_theme, play_mode, audio_volume());
        fclose(f);
    }
}

static void load_settings(void) {
    mkdir("sdmc:/Siren", 0777);
    int t = THEME_TEAL, pm = MODE_LOOP_ALL;
    float vol = 0.85f;
    FILE* f = fopen("sdmc:/Siren/settings.cfg", "r");
    if (f) {
        if (fscanf(f, "%d %d %f", &t, &pm, &vol) >= 1) {
            if (pm >= 0 && pm < MODE_COUNT) play_mode = pm;
            if (vol >= 0.0f && vol <= 1.0f) audio_set_volume(vol);
        }
        fclose(f);
    } else {
        FILE* tf = fopen("sdmc:/Siren/theme.cfg", "r");
        if (tf) { if (fscanf(tf, "%d", &t) != 1) t = THEME_TEAL; fclose(tf); }
    }
    apply_theme(t);
}

static void save_favorites(void) {
    mkdir("sdmc:/Siren", 0777);
    FILE* f = fopen("sdmc:/Siren/favorites.cfg", "w");
    if (!f) return;
    for (int i = 0; i < ntracks; i++) {
        if (tracks[i].fav) fprintf(f, "%s\n", tracks[i].path);
    }
    fclose(f);
}

static int cmp_track_key(const void* key, const void* elem) {
    return strcasecmp((const char*)key, ((const Track*)elem)->path);
}

static void load_favorites(void) {
    FILE* f = fopen("sdmc:/Siren/favorites.cfg", "r");
    if (!f) return;
    char line[640];
    fav_count = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        if (!len || ntracks == 0) continue;
        Track* hit = (Track*)bsearch(line, tracks, (size_t)ntracks, sizeof(Track), cmp_track_key);
        if (hit && !hit->fav) {
            hit->fav = 1;
            fav_count++;
        }
    }
    fclose(f);
}

// ---------- 六轴加速度计（竖屏自动检测） ----------
static HidSixAxisSensorHandle g_six_handle;
static int g_six_ok = 0;
static int ori_cand = ORI_LAND;
static int ori_hold = 0;

static void sixaxis_init(void) {
    g_six_ok = R_SUCCEEDED(hidGetSixAxisSensorHandles(&g_six_handle, 1,
                    HidNpadIdType_No1, HidNpadStyleTag_NpadHandheld));
}

static int ori_read_candidate(void) {
    if (!g_six_ok) return ori;
    HidSixAxisSensorState st[1];
    size_t n = hidGetSixAxisSensorStates(g_six_handle, st, 1);
    if (n < 1) return ori;
    if (!(st[0].attributes & HidSixAxisSensorAttribute_IsConnected)) return ori;
    float ax = st[0].acceleration.x, ay = st[0].acceleration.y;
    if (ax < -0.65f && fabsf(ay) < 0.45f) return ORI_PORT_A;
    if (ax >  0.65f && fabsf(ay) < 0.45f) return ORI_PORT_B;
    if (ay >  0.65f && fabsf(ax) < 0.45f) return ORI_LAND;
    if (ay < -0.65f && fabsf(ax) < 0.45f) return ORI_LAND;
    return ori;
}

static void build_bg_from(const uint32_t* art, int aw, int ah);
static float max_scroll_px(void);

static void apply_orientation(int o) {
    ori = o;
    if (ori == ORI_LAND) { SCR_W = 1280; SCR_H = 720; }
    else                 { SCR_W = 720;  SCR_H = 1280; }
    update_layout();
    if (!screen || screen->w != SCR_W || screen->h != SCR_H) {
        if (screen) canvas_free(screen);
        screen = canvas_new(SCR_W, SCR_H);
    }
    if (bg && (bg->w != SCR_W || bg->h != SCR_H)) { canvas_free(bg); bg = NULL; }
    if (trans_shot && (trans_shot->w != SCR_W || trans_shot->h != SCR_H)) {
        canvas_free(trans_shot);
        trans_shot = NULL;
        trans_t = 1;
    }
    float ms = max_scroll_px();
    if (scroll_px > ms) scroll_px = ms;
    if (scroll_px < 0) scroll_px = 0;
    anim_scroll_px = scroll_px;
    scroll_vel = 0;
    ripple_t = 1;
    touch_fx_alpha = 0;
    touch_drag = 0;
    touch_press_idx = -1;
    if (list_bg) { canvas_free(list_bg); list_bg = NULL; }
    if (!bg && cover_ready && cover) build_bg_from(cover, 360, 360);
}

// ---------- 可见曲目筛选 ----------
static int visible_idx[MAX_TRACKS];
static int nvisible = 0;

static float max_scroll_px(void) {
    float total = (float)nvisible * (float)L_ROW;
    float view_h = (float)list_area_h();
    float m = total - view_h;
    return m > 0.0f ? m : 0.0f;
}

static void ensure_pos_visible(int pos) {
    if (pos < 0 || pos >= nvisible) return;
    float top_px = (float)pos * (float)L_ROW;
    float bot_px = top_px + (float)L_ROW;
    float view_h = (float)list_area_h();
    if (top_px < scroll_px) scroll_px = top_px;
    else if (bot_px > scroll_px + view_h) scroll_px = bot_px - view_h;
    float ms = max_scroll_px();
    if (scroll_px < 0) scroll_px = 0;
    if (scroll_px > ms) scroll_px = ms;
}

static int contains_nocase(const char* hay, const char* needle) {
    if (!hay || !needle || !*needle) return 1;
    size_t nl = strlen(needle);
    for (const char* p = hay; *p; p++) {
        size_t k = 0;
        while (k < nl &&
               tolower((unsigned char)p[k]) == tolower((unsigned char)needle[k])) k++;
        if (k == nl) return 1;
    }
    return 0;
}

static void rebuild_visible(void) {
    nvisible = 0;
    for (int i = 0; i < ntracks; i++) {
        if (view == VIEW_FAVORITE && !tracks[i].fav) continue;
        if (view == VIEW_SEARCH && search_q[0]) {
            char nm[256];
            basename_no_ext(tracks[i].path, nm, sizeof(nm));
            const char* a = tracks[i].meta.title[0] ? tracks[i].meta.title : nm;
            const char* b = tracks[i].meta.artist;
            int hit = contains_nocase(a, search_q) ||
                      contains_nocase(b, search_q) ||
                      contains_nocase(tracks[i].path, search_q);
            if (!hit) continue;
        }
        visible_idx[nvisible++] = i;
    }
    float ms = max_scroll_px();
    if (scroll_px > ms) scroll_px = ms;
    if (scroll_px < 0) scroll_px = 0;
    if (anim_scroll_px > ms) anim_scroll_px = ms;
    if (nvisible == 0) { sel = -1; return; }
    int pos = 0, found = 0;
    for (int i = 0; i < nvisible; i++) {
        if (visible_idx[i] == sel) { pos = i; found = 1; break; }
    }
    if (!found) { pos = 0; sel = visible_idx[0]; }
    ensure_pos_visible(pos);
}

static int vis_at(int pos) {
    return (pos >= 0 && pos < nvisible) ? visible_idx[pos] : -1;
}

static int sel_pos(void) {
    for (int i = 0; i < nvisible; i++)
        if (visible_idx[i] == sel) return i;
    return 0;
}

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static float ease_out(float t) {
    t = clampf(t, 0, 1);
    float u = 1 - t;
    return 1 - u * u * u;
}
static float approach(float cur, float target, float speed, float dt) {
    float k = 1.0f - expf(-speed * dt);
    return cur + (target - cur) * k;
}

static void anim_frame_begin(void) {
    uint64_t now = armGetSystemTick();
    uint64_t freq = armGetSystemTickFreq();
    if (g_last_tick) {
        double d = (double)(now - g_last_tick) / (double)freq;
        if (d < 0.001) d = 0.001;
        if (d > 0.05) d = 0.05;
        g_dt = d;
    }
    g_last_tick = now;
    g_time += g_dt;
}

static void switch_page(int to) {
    if (to == ui_state) return;
    if (!trans_shot) trans_shot = canvas_new(SCR_W, SCR_H);
    if (trans_shot && screen)
        memcpy(trans_shot->px, screen->px, (size_t)SCR_W * SCR_H * 4);
    ui_state = to;
    trans_t = 0;
}

// ---------- 基础工具与图标 ----------
static uint32_t hash_str(const char* s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

static void hsv2rgb(float h, float s, float v, uint8_t* r, uint8_t* g, uint8_t* b) {
    int i = (int)(h * 6);
    float f = h * 6 - i;
    float p = v * (1 - s);
    float q = v * (1 - f * s);
    float t = v * (1 - (1 - f) * s);
    float rr, gg, bb;
    switch (i % 6) {
    case 0: rr = v; gg = t; bb = p; break;
    case 1: rr = q; gg = v; bb = p; break;
    case 2: rr = p; gg = v; bb = t; break;
    case 3: rr = p; gg = q; bb = v; break;
    case 4: rr = t; gg = p; bb = v; break;
    default: rr = v; gg = p; bb = q; break;
    }
    *r = (uint8_t)(rr * 255); *g = (uint8_t)(gg * 255); *b = (uint8_t)(bb * 255);
}

static void track_format_tag(const char* path, char* out, size_t cap) {
    if (ci_ends_with(path, ".flac")) snprintf(out, cap, "FLAC");
    else if (ci_ends_with(path, ".mp3")) snprintf(out, cap, "MP3");
    else if (ci_ends_with(path, ".ogg")) snprintf(out, cap, "OGG");
    else if (ci_ends_with(path, ".wav") || ci_ends_with(path, ".wave")) snprintf(out, cap, "WAV");
    else snprintf(out, cap, "AUDIO");
}

static void draw_note(Canvas* c, int x, int y, int s, uint32_t col) {
    int r = s / 7;
    if (r < 1) return;
    fill_ellipse_aa(c, x + s * 3 / 10, y + s * 4 / 5, r, (int)(r * 0.8), col);
    fill_ellipse_aa(c, x + s * 7 / 10, y + s * 3 / 4, r, (int)(r * 0.8), col);
    fill_rect(c, x + s * 3 / 10 + r - 2, y + s / 6, s / 16 + 2, s * 4 / 5 - s / 6, col);
    fill_rect(c, x + s * 7 / 10 + r - 2, y + s / 12, s / 16 + 2, s * 3 / 4 - s / 12, col);
    fill_tri_aa(c, x + s * 3 / 10 + r - 4, y + s / 6 - 2, x + s * 7 / 10 + r - 4, y + s / 12 - 2,
                x + s * 7 / 10 + r - 4, y + s / 12 + s / 9, col);
    fill_tri_aa(c, x + s * 3 / 10 + r - 4, y + s / 6 - 2, x + s * 7 / 10 + r - 4, y + s / 12 + s / 9,
                x + s * 3 / 10 + r - 4, y + s / 6 + s / 9, col);
}

static void icon_prev(Canvas* c, int x, int y, int s, uint32_t col) {
    int bar_w = s / 6;
    if (bar_w < 3) bar_w = 3;
    fill_rect(c, x, y + 1, bar_w, s - 2, col);
    fill_tri_aa(c, x + s - 1, y + 1, x + s - 1, y + s - 1, x + bar_w + 1, y + s / 2, col);
}

static void icon_next(Canvas* c, int x, int y, int s, uint32_t col) {
    int bar_w = s / 6;
    if (bar_w < 3) bar_w = 3;
    fill_tri_aa(c, x + 1, y + 1, x + s - bar_w - 1, y + s / 2, x + 1, y + s - 1, col);
    fill_rect(c, x + s - bar_w, y + 1, bar_w, s - 2, col);
}

static void icon_heart(Canvas* c, int x, int y, int s, uint32_t col) {
    int rx = s / 4;
    if (rx < 2) rx = 2;
    fill_ellipse_aa(c, x + s * 3 / 10, y + s * 3 / 10, rx, rx, col);
    fill_ellipse_aa(c, x + s * 7 / 10, y + s * 3 / 10, rx, rx, col);
    fill_tri_aa(c, x + s / 12, y + s * 2 / 5, x + s * 11 / 12, y + s * 2 / 5, x + s / 2, y + s * 19 / 20, col);
}

static void icon_speaker(Canvas* c, int x, int y, int s, uint32_t col) {
    fill_rect(c, x, y + s / 3, s / 4, s / 3, col);
    fill_tri_aa(c, x + s / 4, y + s / 3, x + s * 3 / 4, y, x + s * 3 / 4, y + s, col);
    fill_rect(c, x + s * 4 / 5, y + s / 4, 2, s / 2, col);
}

static void icon_back_arrow(Canvas* c, int x, int y, int s, uint32_t col) {
    fill_tri_aa(c, x + s * 2 / 3, y + 2, x + s * 2 / 3, y + s - 2, x + 2, y + s / 2, col);
}

// ---------- 默认封面 / 背景 ----------
static void make_default_art(uint32_t* dst, int n, const char* seed) {
    uint32_t h = hash_str(seed);
    uint8_t r, g, b;
    hsv2rgb((float)((h % 360) / 360.0), 0.52f, 0.32f, &r, &g, &b);
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            float t = (x + y) / (float)(2 * n);
            dst[(size_t)y * n + x] = COL((uint8_t)(r * (1 - t) + 28 * t),
                                         (uint8_t)(g * (1 - t) + 22 * t),
                                         (uint8_t)(b * (1 - t) + 40 * t), 255);
        }
    }
    Canvas c = { n, n, dst };
    draw_note(&c, n / 6, n / 6, n * 2 / 3, COL(255, 255, 255, 210));
}

static void build_bg_from(const uint32_t* art, int aw, int ah) {
    if (!bg) bg = canvas_new(SCR_W, SCR_H);
    int iw = portrait() ? 36 : 64;
    int ih = portrait() ? 64 : 36;
    uint32_t small[64 * 64];
    resample_bilinear(art, aw, ah, small, iw, ih);
    box_blur(small, iw, ih, 3, 2);
    resample_bilinear(small, iw, ih, bg->px, bg->w, bg->h);
    scale_rgb(bg->px, (size_t)bg->w * bg->h, 0.34f);
    for (int y = 0; y < bg->h; y++) {
        for (int x = 0; x < bg->w; x++) {
            float dx = (x - bg->w / 2.0f) / (bg->w / 2.0f);
            float dy = (y - bg->h / 2.0f) / (bg->h / 2.0f);
            float d = sqrtf(dx * dx + dy * dy);
            float f = 1.0f - 0.45f * d * d;
            if (f < 0.15f) f = 0.15f;
            uint32_t* p = &bg->px[(size_t)y * bg->w + x];
            uint32_t c = *p;
            *p = 0xFF000000u |
                 ((uint32_t)(((c >> 16) & 0xFF) * f) << 16) |
                 ((uint32_t)(((c >> 8) & 0xFF) * f) << 8) |
                 (uint32_t)((c & 0xFF) * f);
        }
    }
}

static void build_default_bg(const char* seed) {
    if (!bg) bg = canvas_new(SCR_W, SCR_H);
    uint32_t h = hash_str(seed);
    uint8_t r, g, b;
    hsv2rgb((float)((h % 360) / 360.0), 0.5f, 0.16f, &r, &g, &b);
    for (int y = 0; y < bg->h; y++) {
        for (int x = 0; x < bg->w; x++) {
            float t = ((x * 0.6f + y) / (bg->w * 0.6f + bg->h));
            bg->px[(size_t)y * bg->w + x] = COL((uint8_t)(r * t + 10 * (1 - t)),
                                                (uint8_t)(g * t + 8 * (1 - t)),
                                                (uint8_t)(b * t + 14 * (1 - t)), 255);
        }
    }
}

static void build_poster_thumbs(void) {
    size_t poster_len = (size_t)(poster_end - poster_start);
    if (poster_len < 320 * 320 * 4) return;
    poster36 = (uint32_t*)calloc(36 * 36, 4);
    poster52 = (uint32_t*)calloc(52 * 52, 4);
    poster84 = (uint32_t*)calloc(84 * 84, 4);
    if (poster36)
        resample_bilinear((const uint32_t*)poster_start, 320, 320, poster36, 36, 36);
    if (poster52)
        resample_bilinear((const uint32_t*)poster_start, 320, 320, poster52, 52, 52);
    if (poster84)
        resample_bilinear((const uint32_t*)poster_start, 320, 320, poster84, 84, 84);
}

// ---------- 扫描 ----------
static int should_skip_dir(const char* name) {
    const char* skip[] = {
        "Nintendo", "atmosphere", "Atmosphere", "bootloader", "bootloader2",
        "config", "Config", "dev", "switch", "Switch", "system", "System",
        "SXOS", "sxos", "HeiTag", "sept", "stratosphere", "firmware"
    };
    for (unsigned i = 0; i < sizeof(skip) / sizeof(skip[0]); i++)
        if (!strcmp(name, skip[i])) return 1;
    return 0;
}

static void scan_dir(const char* dir, int depth) {
    if (depth > 5 || ntracks >= MAX_TRACKS) return;
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char path[600];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (!should_skip_dir(e->d_name)) scan_dir(path, depth + 1);
        } else if (S_ISREG(st.st_mode) && dec_is_audio(path)) {
            int dup = 0;
            for (int i = 0; i < ntracks; i++)
                if (!strcasecmp(tracks[i].path, path)) { dup = 1; break; }
            if (!dup && ntracks < MAX_TRACKS) {
                snprintf(tracks[ntracks].path, sizeof(tracks[ntracks].path), "%s", path);
                ntracks++;
            }
        }
    }
    closedir(d);
}

static int cmp_track(const void* a, const void* b) {
    return strcasecmp(((const Track*)a)->path, ((const Track*)b)->path);
}

// ---------- 元数据 / 缩略图懒加载 ----------
static void ensure_track_meta(int idx, int full_scan) {
    if (idx < 0 || idx >= ntracks) return;
    Track* t = &tracks[idx];
    if (t->meta_state) return;
    t->meta_state = 1;
    loaded_thumbs++;

    if (!t->thumb) t->thumb = (uint32_t*)calloc(52 * 52, 4);
    if (!t->thumb84) t->thumb84 = (uint32_t*)calloc(84 * 84, 4);

    if (full_scan) meta_load(t->path, &t->meta);
    else meta_load_quick(t->path, &t->meta);

    if (t->meta.art && t->meta.art_w > 8 && t->meta.art_h > 8) {
        if (t->thumb)
            resample_bilinear((const uint32_t*)t->meta.art, t->meta.art_w, t->meta.art_h, t->thumb, 52, 52);
        if (t->thumb84)
            resample_bilinear((const uint32_t*)t->meta.art, t->meta.art_w, t->meta.art_h, t->thumb84, 84, 84);
        meta_release_art(&t->meta);
    } else {
        if (t->thumb && poster52) memcpy(t->thumb, poster52, (size_t)52 * 52 * 4);
        else if (t->thumb) make_default_art(t->thumb, 52, t->path);
        if (t->thumb84 && poster84) memcpy(t->thumb84, poster84, (size_t)84 * 84 * 4);
        else if (t->thumb84) make_default_art(t->thumb84, 84, t->path);
    }
}

static void trim_thumbs(int first_vis_pos, int last_vis_pos) {
    if (loaded_thumbs <= 200) return;
    for (int i = 0; i < ntracks; i++) {
        Track* t = &tracks[i];
        if (!t->meta_state || i == playing_idx) continue;
        int keep = 0;
        for (int p = first_vis_pos - 12; p <= last_vis_pos + 12; p++) {
            if (vis_at(p) == i) { keep = 1; break; }
        }
        if (keep) continue;
        free(t->thumb); t->thumb = NULL;
        free(t->thumb84); t->thumb84 = NULL;
        meta_clear(&t->meta);
        t->meta_state = 0;
        if (loaded_thumbs > 0) loaded_thumbs--;
    }
}

static void ensure_visible_thumbs(void) {
    int i0 = (int)floorf(anim_scroll_px / (float)L_ROW);
    if (i0 < 0) i0 = 0;
    int rows = rows_visible();
    for (int i = 0; i <= rows + 1; i++) {
        int idx = vis_at(i0 + i);
        if (idx < 0) break;
        if (!tracks[idx].meta_state) {
            ensure_track_meta(idx, 0);
            break;
        }
    }
    trim_thumbs(i0, i0 + rows);
}

// ---------- 切歌与随机 ----------
static int pick_next(int cur, int manual) {
    if (nvisible > 0) {
        if (!manual && play_mode == MODE_LOOP_ONE && cur >= 0) return cur;
        if (play_mode == MODE_SHUFFLE && nvisible > 1) {
            for (int tries = 0; tries < 8; tries++) {
                int r = (int)(next_rand() % (uint32_t)nvisible);
                int idx = vis_at(r);
                if (idx != cur) return idx;
            }
        }
        for (int i = 0; i < nvisible; i++) {
            if (visible_idx[i] == cur) {
                return visible_idx[(i + 1) % nvisible];
            }
        }
        return visible_idx[0];
    }
    if (ntracks <= 1) return 0;
    return (cur + 1) % ntracks;
}

static int pick_prev(int cur) {
    if (nvisible > 0) {
        if (play_mode == MODE_SHUFFLE && nvisible > 1) {
            int r = (int)(next_rand() % (uint32_t)nvisible);
            int idx = vis_at(r);
            if (idx != cur) return idx;
        }
        for (int i = 0; i < nvisible; i++) {
            if (visible_idx[i] == cur) {
                return visible_idx[(i - 1 + nvisible) % nvisible];
            }
        }
        return visible_idx[0];
    }
    if (ntracks <= 1) return 0;
    return (cur - 1 + ntracks) % ntracks;
}

// ---------- 曲目加载 ----------
static void load_track(int idx) {
    if (idx < 0 || idx >= ntracks) return;
    playing_idx = idx;
    loading = 1;
    show_pos = 0;
    audio_play(tracks[idx].path);

    if (!tracks[idx].meta_state) ensure_track_meta(idx, 1);

    meta_clear(&meta);
    if (lyr) { lyrics_free(lyr); lyr = NULL; }
    meta_load(tracks[idx].path, &meta);

    if (meta.title[0]) snprintf(p_title, sizeof(p_title), "%s", meta.title);
    else basename_no_ext(tracks[idx].path, p_title, sizeof(p_title));
    if (meta.artist[0]) snprintf(p_artist, sizeof(p_artist), "%s", meta.artist);
    else p_artist[0] = 0;
    track_format_tag(tracks[idx].path, p_fmt, sizeof(p_fmt));

    if (!cover) cover = (uint32_t*)calloc((size_t)360 * 360, 4);
    if (cover) {
        if (!cover_prev) cover_prev = (uint32_t*)calloc((size_t)360 * 360, 4);
        if (cover_prev) {
            if (cover_ready) {
                memcpy(cover_prev, cover, (size_t)360 * 360 * 4);
                anim_cover = 0;
            } else {
                memset(cover_prev, 0, (size_t)360 * 360 * 4);
                anim_cover = 1;
            }
        }

        if (meta.art && meta.art_w > 8 && meta.art_h > 8) {
            resample_bilinear((const uint32_t*)meta.art, meta.art_w, meta.art_h, cover, 360, 360);
            build_bg_from((const uint32_t*)meta.art, meta.art_w, meta.art_h);
            cover_ready = 1;
        } else if ((size_t)(poster_end - poster_start) >= 320 * 320 * 4) {
            resample_bilinear((const uint32_t*)poster_start, 320, 320, cover, 360, 360);
            build_bg_from((const uint32_t*)poster_start, 320, 320);
            cover_ready = 1;
        } else {
            make_default_art(cover, 360, tracks[idx].path);
            build_default_bg(tracks[idx].path);
            cover_ready = 1;
        }
    }

    lyr = lyrics_load_ex(tracks[idx].path, p_title, p_artist);
    for (int i = 0; i < 28; i++) { bars[i] = 0; bar_peaks[i] = 0; }
    loading = 0;
}

// ---------- 按键与软键盘搜索 ----------
static uint64_t repeat_keys(uint64_t down, uint64_t held, uint64_t mask) {
    static int hold_frames = 0;
    uint64_t out = down & mask;
    if (held & mask) {
        hold_frames++;
        if (hold_frames > 22 && (hold_frames % 4) == 0) out |= held & mask;
    } else {
        hold_frames = 0;
    }
    return out & mask;
}

static int in_rect(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static int in_rc(int x, int y, Rect r) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static void seek_from_touch(int x, int x0, int x1) {
    if (audio_duration() <= 0 || x1 <= x0) return;
    if (x < x0) x = x0;
    if (x > x1) x = x1;
    double frac = (double)(x - x0) / (double)(x1 - x0);
    audio_seek(frac * audio_duration());
}

static void set_view(int v) {
    if (view == v) return;
    view = v;
    scroll_px = 0;
    anim_scroll_px = 0;
    scroll_vel = 0;
    rebuild_visible();
}

static void toggle_fav(int idx) {
    if (idx < 0 || idx >= ntracks) return;
    tracks[idx].fav = !tracks[idx].fav;
    fav_count += tracks[idx].fav ? 1 : -1;
    if (fav_count < 0) fav_count = 0;
    save_favorites();
    rebuild_visible();
    show_toast(tracks[idx].fav ? "已添加到我喜欢" : "已取消喜欢");
}

static void cycle_theme(void) {
    apply_theme((cur_theme + 1) % THEME_COUNT);
    if (list_bg) { canvas_free(list_bg); list_bg = NULL; }
    save_settings();
    char msg[64];
    snprintf(msg, sizeof(msg), "主题：%s", theme_names[cur_theme]);
    show_toast(msg);
}

static void cycle_play_mode(void) {
    play_mode = (play_mode + 1) % MODE_COUNT;
    save_settings();
    show_toast(mode_names[play_mode]);
}

static void cycle_orientation(void) {
    ori_mode = (ori_mode + 1) % 4;
    if (ori_mode == 0) show_toast("方向：自动（陀螺仪）");
    else if (ori_mode == 1) { apply_orientation(ORI_LAND); show_toast("方向：横屏"); }
    else if (ori_mode == 2) { apply_orientation(ORI_PORT_A); show_toast("方向：竖屏 A"); }
    else { apply_orientation(ORI_PORT_B); show_toast("方向：竖屏 B"); }
}

static void open_search_keyboard(void) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) return;
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, "搜索歌曲、艺术家或路径");
    swkbdConfigSetGuideText(&kbd, "输入关键词过滤，留空按确认显示全部");
    swkbdConfigSetOkButtonText(&kbd, "搜索");
    swkbdConfigSetInitialText(&kbd, search_q);
    swkbdConfigSetStringLenMax(&kbd, 64);

    char out[128] = {0};
    Result rc = swkbdShow(&kbd, out, sizeof(out));
    swkbdClose(&kbd);
    be_touch_reset();
    g_last_tick = armGetSystemTick();

    if (R_SUCCEEDED(rc)) {
        snprintf(search_q, sizeof(search_q), "%s", out);
        view = search_q[0] ? VIEW_SEARCH : VIEW_HOME;
        scroll_px = 0;
        anim_scroll_px = 0;
        scroll_vel = 0;
        rebuild_visible();
        if (search_q[0]) {
            char msg[96];
            snprintf(msg, sizeof(msg), "找到 %d 首匹配歌曲", nvisible);
            show_toast(msg);
        } else {
            show_toast("已显示全部歌曲");
        }
    }
}

// ---------- 胶囊按钮绘制辅助 ----------
static Rect draw_pill_btn(int x, int y, int h, float px, const char* txt, int active, uint32_t active_col) {
    int tw = text_width(font, px, txt);
    int w = tw + 26;
    uint32_t fill = active ? C_PLAYBG : C_SURF2;
    uint8_t alpha = active ? 235 : 210;
    fill_round_rect_aa_a(screen, x, y, w, h, h / 2, fill, alpha);
    if (active) stroke_round_rect_aa(screen, x, y, w, h, h / 2, 2, active_col, 220);
    else stroke_round_rect_aa(screen, x, y, w, h, h / 2, 1, C_LINE, 160);
    draw_text_centered(screen, font, px, x, x + w, y + (h - (int)px) / 2 - 1,
                       active ? active_col : C_TXT2, txt);
    Rect r = { x, y, w, h };
    return r;
}

// ---------- 触摸交互：列表页 ----------
static void handle_touch_list(TouchInfo* t) {
    int list_bottom = SCR_H - L_BAR;
    int cardw = SCR_W - 2 * L_M;

    if (touch_drag && t->held && max_scroll_px() > 0.0f) {
        int dy = touch_last_y - t->y;
        if (abs(t->y - touch_drag_start_y) > 12) touch_moved = 1;
        if (dy != 0) {
            scroll_px += (float)dy;
            float ms = max_scroll_px();
            if (scroll_px < 0) scroll_px = 0;
            if (scroll_px > ms) scroll_px = ms;
            anim_scroll_px = scroll_px;
            scroll_vel = (float)dy / (float)g_dt;
            if (scroll_vel > 2400.0f) scroll_vel = 2400.0f;
            if (scroll_vel < -2400.0f) scroll_vel = -2400.0f;
        }
        touch_last_y = t->y;
    }

    if (!t->down) {
        if (t->up && touch_drag) {
            if (!touch_moved && touch_press_idx >= 0 && touch_press_idx < nvisible) {
                int idx = vis_at(touch_press_idx);
                if (idx >= 0) {
                    if (t->x >= SCR_W - L_M - 66 && t->x <= SCR_W - L_M - 6) {
                        toggle_fav(idx);
                    } else if (!loading) {
                        sel = idx;
                        load_track(idx);
                        switch_page(UI_PLAYER);
                    }
                }
            }
            touch_drag = 0;
            touch_moved = 0;
            touch_press_idx = -1;
        }
        return;
    }

    int x = t->x, y = t->y;
    touch_fx_x = (float)x; touch_fx_y = (float)y; touch_fx_alpha = 1;
    scroll_vel = 0;

    if (y < L_TOP) {
        if (in_rc(x, y, rc_tab_all)) {
            search_q[0] = 0;
            set_view(VIEW_HOME);
        } else if (in_rc(x, y, rc_tab_fav)) {
            set_view(view == VIEW_FAVORITE ? VIEW_HOME : VIEW_FAVORITE);
        } else if (in_rc(x, y, rc_btn_search)) {
            if (view == VIEW_SEARCH && search_q[0]) {
                search_q[0] = 0;
                set_view(VIEW_HOME);
                show_toast("已清除搜索");
            } else {
                open_search_keyboard();
            }
        } else if (in_rc(x, y, rc_btn_theme)) {
            cycle_theme();
        } else if (in_rc(x, y, rc_btn_ori)) {
            cycle_orientation();
        } else if (in_rc(x, y, rc_btn_diag)) {
            show_diag = 1;
        }
        return;
    }

    if (y >= list_bottom) {
        if (playing_idx >= 0) {
            if (in_rc(x, y, rc_mini_prev)) {
                int p = pick_prev(playing_idx);
                if (p >= 0 && !loading) { load_track(p); sel = p; }
                return;
            }
            if (in_rc(x, y, rc_mini_play)) {
                if (audio_paused()) audio_resume(); else audio_pause();
                return;
            }
            if (in_rc(x, y, rc_mini_next)) {
                int p = pick_next(playing_idx, 1);
                if (p >= 0 && !loading) { load_track(p); sel = p; }
                return;
            }
            switch_page(UI_PLAYER);
        } else if (nvisible > 0 && !loading) {
            int first = vis_at(0);
            if (first >= 0) { load_track(first); switch_page(UI_PLAYER); }
        }
        return;
    }

    if (nvisible > 0 && y >= L_TOP && x >= L_M && x <= SCR_W - L_M) {
        int pos = (int)floorf(((float)(y - L_TOP) + anim_scroll_px) / (float)L_ROW);
        if (pos >= 0 && pos < nvisible) {
            touch_press_idx = pos;
            sel = vis_at(pos);
            touch_drag = 1;
            touch_moved = 0;
            touch_drag_start_y = y;
            touch_last_y = y;
            ripple_x = x;
            ripple_y = y;
            ripple_row = pos;
            ripple_cw = cardw;
            ripple_ch = L_ROW - 12;
            ripple_t = 0;
        }
    }
}

// ---------- 触摸交互：播放页 ----------
static void handle_touch_player(TouchInfo* t) {
    if (touch_scrub && t->held)
        seek_from_touch(t->x, ui_px0, ui_px1);

    if (!t->down) {
        if (t->up) touch_scrub = 0;
        return;
    }

    int x = t->x, y = t->y;
    touch_fx_x = (float)x; touch_fx_y = (float)y; touch_fx_alpha = 1;

    if (in_rc(x, y, rc_p_back)) {
        switch_page(UI_LIST);
        sel = playing_idx >= 0 ? playing_idx : sel;
        rebuild_visible();
        return;
    }
    if (in_rc(x, y, rc_p_ori)) {
        cycle_orientation();
        return;
    }
    if (in_rc(x, y, rc_p_diag)) {
        show_diag = 1;
        return;
    }

    if (in_rect(x, y, ui_px0 - 14, ui_py - 26, ui_px1 - ui_px0 + 28, 56)) {
        touch_scrub = 1;
        seek_from_touch(x, ui_px0, ui_px1);
        return;
    }

    if (abs(y - ui_cy) <= 44) {
        if (abs(x - (ui_cx - ui_gap2)) <= 36) {
            if (playing_idx >= 0) toggle_fav(playing_idx);
            return;
        }
        if (abs(x - (ui_cx - ui_gap)) <= 36) {
            if (ntracks > 0 && !loading) load_track(pick_prev(playing_idx));
            return;
        }
        if (abs(x - ui_cx) <= 42) {
            if (audio_paused()) audio_resume(); else audio_pause();
            return;
        }
        if (abs(x - (ui_cx + ui_gap)) <= 36) {
            if (ntracks > 0 && !loading) load_track(pick_next(playing_idx, 1));
            return;
        }
        if (abs(x - (ui_cx + ui_gap2)) <= 42) {
            cycle_play_mode();
            return;
        }
    }

    if (in_rect(x, y, ui_cover_x, ui_cover_y, ui_cover_sz, ui_cover_sz)) {
        if (audio_paused()) audio_resume(); else audio_pause();
    }
}

// ---------- 触点反馈 & 水波纹 ----------
static void draw_touch_feedback(void) {
    if (touch_fx_alpha > 0) {
        int r = 16 + (int)(10 * (1.0f - touch_fx_alpha));
        fill_circle_a(screen, (int)touch_fx_x, (int)touch_fx_y, r,
                      C_PRIMARY, (uint8_t)(80 * touch_fx_alpha));
    }
}

static void draw_ripples(void) {
    if (ripple_t >= 1 || ripple_row < 0) return;
    float t = ease_out(ripple_t);
    int rmax = (ripple_cw > ripple_ch ? ripple_cw : ripple_ch) * 6 / 5 + 40;
    int r = (int)(rmax * t) + 10;
    uint8_t a = (uint8_t)(85 * (1.0f - ripple_t));
    if (a > 0) {
        int cy0 = L_TOP + ripple_row * L_ROW - (int)anim_scroll_px + 6;
        if (cy0 + ripple_ch <= L_TOP || cy0 >= SCR_H - L_BAR) return;
        int clip_y = cy0 < L_TOP ? L_TOP : cy0;
        int clip_h = (cy0 + ripple_ch > SCR_H - L_BAR ? SCR_H - L_BAR : cy0 + ripple_ch) - clip_y;
        if (clip_h > 0)
            fill_circle_clip_a(screen, ripple_x, ripple_y, r, C_PRIMARY, a,
                               L_M, clip_y, ripple_cw, clip_h);
    }
}

// ---------- 背景渐变壁纸 ----------
static void build_list_bg(void) {
    if (list_bg && list_bg->w == SCR_W && list_bg->h == SCR_H) return;
    if (list_bg) { canvas_free(list_bg); list_bg = NULL; }
    list_bg = canvas_new(SCR_W, SCR_H);
    uint32_t bgr = C_BG & 0xFF, bgg = (C_BG >> 8) & 0xFF, bgb = (C_BG >> 16) & 0xFF;
    uint32_t sfr = C_SURFACE & 0xFF, sfg = (C_SURFACE >> 8) & 0xFF, sfb = (C_SURFACE >> 16) & 0xFF;
    uint32_t prr = C_PRIMARY & 0xFF, prg = (C_PRIMARY >> 8) & 0xFF, prb = (C_PRIMARY >> 16) & 0xFF;
    for (int y = 0; y < SCR_H; y++) {
        float t = (float)y / (SCR_H - 1);
        uint32_t r = (uint32_t)(bgr + (sfr - bgr) * t);
        uint32_t g = (uint32_t)(bgg + (sfg - bgg) * t);
        uint32_t b = (uint32_t)(bgb + (sfb - bgb) * t);
        hline(list_bg, 0, y, SCR_W, COL((uint8_t)r, (uint8_t)g, (uint8_t)b, 255));
    }
    fill_circle_a(list_bg, SCR_W * 4 / 5, L_TOP + 50, 320, COL((uint8_t)prr, (uint8_t)prg, (uint8_t)prb, 255), 14);
    fill_circle_a(list_bg, SCR_W / 6, SCR_H - L_BAR - 120, 260, COL((uint8_t)prr, (uint8_t)prg, (uint8_t)prb, 255), 10);
}

// ---------- 列表页绘制 ----------
static void draw_list(void) {
    if (bg) {
        memcpy(screen->px, bg->px, (size_t)SCR_W * SCR_H * 4);
        fill_rect_a(screen, 0, 0, SCR_W, SCR_H, C_BG, 180);
    } else {
        build_list_bg();
        memcpy(screen->px, list_bg->px, (size_t)SCR_W * SCR_H * 4);
    }
    ensure_visible_thumbs();

    int cardw = SCR_W - 2 * L_M;
    int list_bottom = SCR_H - L_BAR;

    // 先绘制列表卡片区域，再在其上覆盖顶部栏和底部播放条，确保滚动绝不溢出遮挡栏位
    if (nvisible == 0) {
        const char* empty_msg = "暂无歌曲";
        if (ntracks == 0) empty_msg = "未在 SD 卡找到音频 · 可放入 sdmc:/music 目录";
        else if (view == VIEW_FAVORITE) empty_msg = "暂无收藏歌曲 · 点击右侧爱心或按 ZL 收藏";
        else if (view == VIEW_SEARCH) empty_msg = "未找到匹配歌曲 · 点击“清除搜索”返回";
        draw_text_centered(screen, font, 20, 40, SCR_W - 40, SCR_H / 2 - 20, C_TXT2, empty_msg);
    } else {
        int i0 = (int)floorf(anim_scroll_px / (float)L_ROW) - 1;
        if (i0 < 0) i0 = 0;
        int i1 = (int)ceilf((anim_scroll_px + (float)list_area_h()) / (float)L_ROW) + 1;
        if (i1 > nvisible) i1 = nvisible;

        for (int i = i0; i < i1; i++) {
            int idx = vis_at(i);
            if (idx < 0) break;
            int y = L_TOP + i * L_ROW - (int)anim_scroll_px;
            int cy0 = y + 6;
            int ch = L_ROW - 12;
            if (cy0 + ch <= L_TOP || cy0 >= list_bottom) continue;

            int is_playing = (idx == playing_idx);
            int is_sel = (idx == sel);
            int is_press = (touch_drag && touch_press_idx == i);

            uint32_t card = C_CARD;
            uint8_t card_a = 212;
            if (is_playing) { card = C_PLAYBG; card_a = 236; }
            else if (is_press) { card = C_SURF2; card_a = 242; }
            else if (is_sel) { card = C_CARD2; card_a = 230; }
            fill_round_rect_aa_a(screen, L_M, cy0, cardw, ch, 18, card, card_a);

            int tx = L_M + 12;
            int ty = cy0 + (ch - 84) / 2;
            if (tracks[idx].thumb84)
                blit_rgba_round(screen, tx, ty, tracks[idx].thumb84, 84, 84, 15);

            if (is_playing) {
                fill_round_rect_aa_a(screen, tx, ty, 84, 84, 15, COL(0, 0, 0, 255), 110);
                for (int b = 0; b < 3; b++) {
                    float ph = audio_paused() ? 0.35f : (0.35f + 0.65f * fabsf(sinf((float)g_time * 6.5f + b * 1.4f)));
                    int bh = (int)(26 * ph);
                    if (bh < 6) bh = 6;
                    fill_round_rect_aa(screen, tx + 24 + b * 14, ty + 58 - bh, 8, bh, 4, C_PRIMARY);
                }
            }

            char nm[256];
            basename_no_ext(tracks[idx].path, nm, sizeof(nm));
            const char* ttl = tracks[idx].meta.title[0] ? tracks[idx].meta.title : nm;
            const char* art = tracks[idx].meta.artist[0] ? tracks[idx].meta.artist : "未知艺术家";
            uint32_t tc = is_playing ? C_PRIMARY : C_TXT;

            int txtx = tx + 84 + 18;
            int right_reserve = 96 + (is_playing ? 74 : 0);
            int maxw = cardw - (txtx - L_M) - right_reserve;
            if (maxw < 80) maxw = 80;

            int block = L_TITLE + 8 + L_ART;
            int ty2 = cy0 + (ch - block) / 2 - 1;
            draw_text_max(screen, font, L_TITLE, txtx, ty2, tc, ttl, maxw);

            char fmt[12];
            track_format_tag(tracks[idx].path, fmt, sizeof(fmt));
            char subline[300];
            snprintf(subline, sizeof(subline), "%s · %s", fmt, art);
            draw_text_max(screen, font, L_ART, txtx, ty2 + L_TITLE + 8, C_TXT3, subline, maxw);

            if (is_playing && audio_duration() > 0) {
                char dur[16];
                fmt_time(audio_duration(), dur, sizeof(dur));
                draw_text_right(screen, font, L_ART, SCR_W - L_M - 68, cy0 + (ch - L_ART) / 2 - 1, C_PRIMARY, dur);
            }

            int hx = SCR_W - L_M - 46;
            int hy = cy0 + (ch - 22) / 2;
            icon_heart(screen, hx, hy, 22, tracks[idx].fav ? C_HEART : C_LINE);

            // 空心抗锯齿描边，不会遮挡卡片内部
            if (is_playing) {
                stroke_round_rect_aa(screen, L_M, cy0, cardw, ch, 18, 2, C_PRIMARY, 240);
            } else if (is_sel) {
                stroke_round_rect_aa(screen, L_M, cy0, cardw, ch, 18, 2, C_TXT2, 140);
            }
        }

        draw_ripples();

        float ms = max_scroll_px();
        if (ms > 0.0f) {
            int track_h = list_area_h() - 16;
            int thumb_h = (int)((float)list_area_h() / (float)(nvisible * L_ROW) * (float)track_h);
            if (thumb_h < 32) thumb_h = 32;
            if (thumb_h > track_h) thumb_h = track_h;
            int thumb_y = L_TOP + 8 + (int)((anim_scroll_px / ms) * (float)(track_h - thumb_h));
            fill_round_rect_aa_a(screen, SCR_W - 10, thumb_y, 4, thumb_h, 2, C_TXT3, 140);
        }
    }

    // 顶部栏覆盖绘制（完全遮住滚动到顶部之上的部分卡片）
    fill_rect_a(screen, 0, 0, SCR_W, L_TOP, C_BG, 244);
    fill_rect(screen, 0, L_TOP - 1, SCR_W, 1, C_LINE);

    if (!portrait()) {
        if (poster36) {
            blit_rgba_round(screen, 26, 23, poster36, 36, 36, 10);
            stroke_round_rect_aa(screen, 26, 23, 36, 36, 10, 1, C_LINE, 180);
        } else {
            fill_ellipse_aa(screen, 42, 41, 16, 16, C_PRIMARY);
            draw_note(screen, 33, 32, 18, C_ONPRIM);
        }
        draw_text(screen, font, 26, 70, 26, C_TXT, "Siren");

        char sub[64];
        if (view == VIEW_SEARCH && search_q[0]) snprintf(sub, sizeof(sub), "%d 首匹配", nvisible);
        else if (view == VIEW_FAVORITE) snprintf(sub, sizeof(sub), "%d 首收藏", nvisible);
        else snprintf(sub, sizeof(sub), "%d 首", ntracks);
        int cnt_x = 70 + text_width(font, 26, "Siren") + 14;
        draw_text(screen, font, 16, cnt_x, 33, C_TXT3, sub);

        int px = SCR_W - L_M;
        int ph = 36, py = 23;
        int dw = text_width(font, 15, "自检") + 26;
        px -= dw; rc_btn_diag = draw_pill_btn(px, py, ph, 15, "自检", 0, C_PRIMARY);

        const char* ori_lbl = "横/竖";
        int ow = text_width(font, 15, ori_lbl) + 26;
        px -= ow + 10; rc_btn_ori = draw_pill_btn(px, py, ph, 15, ori_lbl, ori_mode != 0, C_PRIMARY);

        char th_lbl[32];
        snprintf(th_lbl, sizeof(th_lbl), "主题:%s", theme_names[cur_theme]);
        int thw = text_width(font, 15, th_lbl) + 26;
        px -= thw + 10; rc_btn_theme = draw_pill_btn(px, py, ph, 15, th_lbl, 0, C_PRIMARY);

        const char* sr_lbl = (view == VIEW_SEARCH && search_q[0]) ? "清除搜索" : "搜索";
        int srw = text_width(font, 15, sr_lbl) + 26;
        px -= srw + 10; rc_btn_search = draw_pill_btn(px, py, ph, 15, sr_lbl, view == VIEW_SEARCH, C_PRIMARY);

        char fav_lbl[32];
        snprintf(fav_lbl, sizeof(fav_lbl), "喜欢 %d", fav_count);
        int fw = text_width(font, 15, fav_lbl) + 26;
        px -= fw + 10; rc_tab_fav = draw_pill_btn(px, py, ph, 15, fav_lbl, view == VIEW_FAVORITE, C_HEART);

        int aw = text_width(font, 15, "全部") + 26;
        px -= aw + 10; rc_tab_all = draw_pill_btn(px, py, ph, 15, "全部", view == VIEW_HOME, C_PRIMARY);
    } else {
        if (poster36) {
            blit_rgba_round(screen, 22, 19, poster36, 36, 36, 10);
            stroke_round_rect_aa(screen, 22, 19, 36, 36, 10, 1, C_LINE, 180);
        } else {
            fill_ellipse_aa(screen, 38, 37, 16, 16, C_PRIMARY);
            draw_note(screen, 29, 28, 18, C_ONPRIM);
        }
        draw_text(screen, font, 28, 66, 21, C_TXT, "Siren");

        char sub[64];
        if (view == VIEW_SEARCH && search_q[0]) snprintf(sub, sizeof(sub), "%d 首匹配", nvisible);
        else if (view == VIEW_FAVORITE) snprintf(sub, sizeof(sub), "%d 首收藏", nvisible);
        else snprintf(sub, sizeof(sub), "%d 首", nvisible);
        int cnt_x = 66 + text_width(font, 28, "Siren") + 14;
        draw_text(screen, font, 17, cnt_x, 30, C_TXT3, sub);

        int px = SCR_W - L_M;
        int dw = text_width(font, 16, "自检") + 26;
        px -= dw; rc_btn_diag = draw_pill_btn(px, 18, 38, 16, "自检", 0, C_PRIMARY);
        int ow = text_width(font, 16, "横/竖") + 26;
        px -= ow + 10; rc_btn_ori = draw_pill_btn(px, 18, 38, 16, "横/竖", ori_mode != 0, C_PRIMARY);

        int x2 = L_M;
        rc_tab_all = draw_pill_btn(x2, 74, 40, 16, "全部", view == VIEW_HOME, C_PRIMARY);
        x2 += rc_tab_all.w + 10;

        char fav_lbl[32];
        snprintf(fav_lbl, sizeof(fav_lbl), "喜欢 %d", fav_count);
        rc_tab_fav = draw_pill_btn(x2, 74, 40, 16, fav_lbl, view == VIEW_FAVORITE, C_HEART);
        x2 += rc_tab_fav.w + 10;

        const char* sr_lbl = (view == VIEW_SEARCH && search_q[0]) ? "清除搜索" : "搜索";
        rc_btn_search = draw_pill_btn(x2, 74, 40, 16, sr_lbl, view == VIEW_SEARCH, C_PRIMARY);
        x2 += rc_btn_search.w + 10;

        char th_lbl[32];
        snprintf(th_lbl, sizeof(th_lbl), "主题:%s", theme_names[cur_theme]);
        rc_btn_theme = draw_pill_btn(x2, 74, 40, 16, th_lbl, 0, C_PRIMARY);
    }

    // 底部迷你播放条
    int by = SCR_H - L_BAR;
    fill_rect_a(screen, 0, by, SCR_W, L_BAR, C_SURFACE, 246);
    fill_rect(screen, 0, by, SCR_W, 1, C_LINE);
    if (playing_idx >= 0 && audio_duration() > 0) {
        float p = (float)(audio_position() / audio_duration());
        if (p < 0) p = 0;
        if (p > 1) p = 1;
        fill_rect(screen, 0, by, (int)(SCR_W * p), 3, C_PRIMARY);
    }

    int midy = by + L_BAR / 2;
    if (playing_idx >= 0) {
        int csz = 52;
        if (tracks[playing_idx].thumb)
            blit_rgba_round(screen, 18, midy - csz / 2, tracks[playing_idx].thumb, csz, csz, 10);

        int ctrl_w = 196;
        int txt_x = 18 + csz + 14;
        int txt_maxw = SCR_W - txt_x - ctrl_w - 16;
        if (txt_maxw < 80) txt_maxw = 80;

        draw_text_max(screen, font, portrait() ? 22 : 18, txt_x, midy - 23,
                      C_TXT, p_title[0] ? p_title : "Siren", txt_maxw);
        draw_text_max(screen, font, L_ARTIST2, txt_x, midy + 4,
                      C_TXT3, p_artist[0] ? p_artist : p_fmt, txt_maxw);

        int bx3 = SCR_W - 44;
        int bx2 = SCR_W - 102;
        int bx1 = SCR_W - 160;

        fill_ellipse_aa(screen, bx1, midy, 20, 20, C_SURF2);
        icon_prev(screen, bx1 - 9, midy - 9, 18, C_TXT);

        fill_ellipse_aa(screen, bx2, midy, 24, 24, C_PRIMARY);
        if (audio_paused()) {
            fill_tri_aa(screen, bx2 - 6, midy - 9, bx2 - 6, midy + 9, bx2 + 9, midy, C_ONPRIM);
        } else {
            fill_rect(screen, bx2 - 7, midy - 9, 5, 18, C_ONPRIM);
            fill_rect(screen, bx2 + 2, midy - 9, 5, 18, C_ONPRIM);
        }

        fill_ellipse_aa(screen, bx3, midy, 20, 20, C_SURF2);
        icon_next(screen, bx3 - 9, midy - 9, 18, C_TXT);

        rc_mini_prev = (Rect){ bx1 - 26, by, 52, L_BAR };
        rc_mini_play = (Rect){ bx2 - 28, by, 56, L_BAR };
        rc_mini_next = (Rect){ bx3 - 26, by, 52, L_BAR };
    } else {
        draw_text(screen, font, 17, 24, midy - 10, C_TXT2, "点击任意歌曲播放 · A 确认 · X 搜索 · ZL 收藏 · ZR 主题");
        rc_mini_prev = rc_mini_play = rc_mini_next = (Rect){ 0, 0, 0, 0 };
    }

    draw_touch_feedback();
}

static void ui_list_input(uint64_t down, uint64_t rep) {
    rebuild_visible();
    if (nvisible == 0) return;
    int pos = sel_pos();

    if (rep & (HidNpadButton_Up | HidNpadButton_StickLUp)) {
        if (pos > 0) pos--;
        sel = vis_at(pos);
        scroll_vel = 0;
        ensure_pos_visible(pos);
    }
    if (rep & (HidNpadButton_Down | HidNpadButton_StickLDown)) {
        if (pos < nvisible - 1) pos++;
        sel = vis_at(pos);
        scroll_vel = 0;
        ensure_pos_visible(pos);
    }
    if (rep & HidNpadButton_StickRUp) {
        audio_set_volume(audio_volume() + 0.05f);
        save_settings();
        vol_overlay_timer = 70;
    }
    if (rep & HidNpadButton_StickRDown) {
        audio_set_volume(audio_volume() - 0.05f);
        save_settings();
        vol_overlay_timer = 70;
    }
    if (down & (HidNpadButton_L | HidNpadButton_Left)) {
        if (ntracks > 0 && !loading) {
            int p = pick_prev(playing_idx >= 0 ? playing_idx : sel);
            load_track(p); sel = p;
        }
    }
    if (down & (HidNpadButton_R | HidNpadButton_Right)) {
        if (ntracks > 0 && !loading) {
            int p = pick_next(playing_idx >= 0 ? playing_idx : sel, 1);
            load_track(p); sel = p;
        }
    }
    if ((down & HidNpadButton_A) && !loading) {
        int i = vis_at(pos);
        if (i >= 0) { load_track(i); switch_page(UI_PLAYER); }
    }
    if (down & HidNpadButton_X) {
        open_search_keyboard();
    }
    if (down & HidNpadButton_ZL) {
        int i = vis_at(pos);
        if (i >= 0) toggle_fav(i);
    }
    if (down & HidNpadButton_ZR) {
        cycle_theme();
    }
}

// ---------- 播放页绘制辅助 ----------
static void draw_text_centered_max(Canvas* c, Font* f, float px, int x0, int x1, int y, uint32_t col, const char* s) {
    if (!s || !*s) return;
    int maxw = x1 - x0;
    int w = text_width(f, px, s);
    if (w <= maxw) {
        draw_text_centered(c, f, px, x0, x1, y, col, s);
        return;
    }
    int tw = text_width_max(f, px, s, maxw);
    int x = x0 + (maxw - tw) / 2;
    draw_text_max(c, f, px, x, y, col, s, maxw);
}

static void draw_lyric_or_spectrum(int x0, int x1, int ly_top, int ly_bot, int spec_base, int spec_hint_y,
                                   double pos, double dur, int centered) {
    int rw = x1 - x0;
    if (lyr) {
        int act = lyr->unsynced
            ? (dur > 0.0 && lyr->n > 1 ? (int)((pos / dur) * (double)(lyr->n - 1)) : 0)
            : lyrics_index(lyr, pos);
        if (lyr->unsynced) {
            if (act < 0) act = 0;
            if (act >= lyr->n) act = lyr->n - 1;
        }
        int idxs2[9];
        int n = 0;
        for (int d = -2; d <= 3; d++) {
            int i = act + d;
            if (i >= 0 && i < lyr->n) idxs2[n++] = i;
        }
        int y = ly_top;
        for (int k = 0; k < n; k++) {
            int i = idxs2[k];
            int dist = (act >= 0) ? (i - act) : 3;
            if (y > ly_bot) break;
            if (dist == 0) {
                if (centered) {
                    draw_text_centered_max(screen, font, 28, x0, x1, y, C_PRIMARY, lyr->lines[i].text);
                } else {
                    fill_round_rect_aa(screen, x0, y + 3, 4, 26, 2, C_PRIMARY);
                    draw_text_max(screen, font, 25, x0 + 16, y, C_PRIMARY, lyr->lines[i].text, rw - 16);
                }
                y += centered ? 52 : 48;
            } else {
                float px = centered
                    ? ((dist == -1 || dist == 1) ? 22.0f : 18.0f)
                    : ((dist == -1 || dist == 1) ? 20.0f : 17.0f);
                uint32_t cc = (dist == -1 || dist == 1) ? C_TXT2 : C_TXT3;
                if (centered) {
                    draw_text_centered_max(screen, font, px, x0, x1, y, cc, lyr->lines[i].text);
                } else {
                    draw_text_max(screen, font, px, x0 + 16, y, cc, lyr->lines[i].text, rw - 16);
                }
                y += (int)px + (centered ? 22 : 20);
            }
        }
        if (lyr->unsynced) {
            if (centered) {
                draw_text_centered(screen, font, 14, x0, x1, spec_hint_y, C_TXT3, "静态歌词（内嵌文本 · 按播放进度自动滚动）");
            } else {
                draw_text(screen, font, 14, x0 + 16, spec_hint_y, C_TXT3, "静态歌词（内嵌文本 · 按播放进度自动滚动）");
            }
        }
    } else {
        float sp[28];
        audio_get_spectrum(sp);
        for (int i = 0; i < 28; i++) {
            if (sp[i] > bars[i]) bars[i] += (sp[i] - bars[i]) * 0.45f;
            else bars[i] += (sp[i] - bars[i]) * 0.22f;
            if (bars[i] > bar_peaks[i]) bar_peaks[i] = bars[i];
            else bar_peaks[i] = clampf(bar_peaks[i] - (float)(g_dt * 0.35), bars[i], 1.0f);
        }
        int n = portrait() ? 18 : 22;
        int bw = 16;
        int span = rw - (centered ? 80 : 32);
        int gap = (span - n * bw) / (n - 1);
        if (gap < 4) gap = 4;
        int total_w = n * bw + (n - 1) * gap;
        int bx0 = x0 + (centered ? (rw - total_w) / 2 : 0);
        int max_h = portrait() ? 220 : 190;
        for (int i = 0; i < n; i++) {
            float v = clampf(bars[i], 0.03f, 1.0f);
            int hgt = (int)(v * max_h);
            int bx = bx0 + i * (bw + gap);
            fill_round_rect_aa_a(screen, bx, spec_base - hgt, bw, hgt, bw / 2, C_PRIMARY, 215);
            int py_dot = spec_base - (int)(clampf(bar_peaks[i], 0.03f, 1.0f) * max_h) - 8;
            fill_round_rect_aa_a(screen, bx, py_dot, bw, 4, 2, C_TXT2, 180);
        }
        if (centered) {
            draw_text_centered(screen, font, 16, x0, x1, spec_hint_y, C_TXT3,
                               "未找到歌词 · 支持同名 .lrc 及 MP3/FLAC 内嵌歌词");
        } else {
            draw_text(screen, font, 15, x0, spec_hint_y, C_TXT3,
                      "未找到歌词 · 支持同名 .lrc 及 MP3/FLAC 内嵌歌词");
        }
    }
}

static void draw_progress_bar(int py, double pos, double dur) {
    int w = ui_px1 - ui_px0;
    fill_round_rect_aa(screen, ui_px0, py, w, 6, 3, C_LINE);
    int fillw = (int)((dur > 0 ? pos / dur : 0) * w);
    if (fillw < 0) fillw = 0;
    if (fillw > w) fillw = w;
    if (fillw > 0) fill_round_rect_aa(screen, ui_px0, py, fillw, 6, 3, C_PRIMARY);
    fill_ellipse_aa(screen, ui_px0 + fillw, py + 3, 9, 9, C_PRIMARY);

    char t1[32], t2[32];
    fmt_time(pos, t1, sizeof(t1));
    fmt_time(dur, t2, sizeof(t2));
    float tsz = portrait() ? 16.0f : 15.0f;
    draw_text(screen, font, tsz, ui_px0, py + 18, C_TXT2, t1);
    draw_text_right(screen, font, tsz, ui_px1, py + 18, C_TXT2, t2);
}

static void draw_controls(int cy) {
    int fav_on = (playing_idx >= 0 && tracks[playing_idx].fav);

    fill_ellipse_aa(screen, ui_cx - ui_gap2, cy, 22, 22, fav_on ? C_PLAYBG : C_SURF2);
    if (fav_on) stroke_circle_aa(screen, ui_cx - ui_gap2, cy, 22, 2, C_HEART, 220);
    icon_heart(screen, ui_cx - ui_gap2 - 11, cy - 10, 22, fav_on ? C_HEART : C_TXT2);

    fill_ellipse_aa(screen, ui_cx - ui_gap, cy, 25, 25, C_SURF2);
    icon_prev(screen, ui_cx - ui_gap - 11, cy - 11, 22, C_TXT);

    fill_ellipse_aa(screen, ui_cx, cy, 32, 32, C_PRIMARY);
    if (audio_paused()) {
        fill_tri_aa(screen, ui_cx - 8, cy - 12, ui_cx - 8, cy + 12, ui_cx + 12, cy, C_ONPRIM);
    } else {
        fill_rect(screen, ui_cx - 10, cy - 12, 6, 24, C_ONPRIM);
        fill_rect(screen, ui_cx + 4, cy - 12, 6, 24, C_ONPRIM);
    }

    fill_ellipse_aa(screen, ui_cx + ui_gap, cy, 25, 25, C_SURF2);
    icon_next(screen, ui_cx + ui_gap - 11, cy - 11, 22, C_TXT);

    const char* ml = mode_short[play_mode];
    int mw = text_width(font, 16, ml) + 32;
    int mx = ui_cx + ui_gap2 - mw / 2;
    fill_round_rect_aa_a(screen, mx, cy - 20, mw, 40, 20, C_SURF2, 235);
    stroke_round_rect_aa(screen, mx, cy - 20, mw, 40, 20, 1, play_mode == MODE_LOOP_ALL ? C_LINE : C_PRIMARY, 200);
    draw_text_centered(screen, font, 16, mx, mx + mw, cy - 9,
                       play_mode == MODE_LOOP_ALL ? C_TXT2 : C_PRIMARY, ml);
}

// ---------- 沉浸播放页 ----------
static void draw_player(void) {
    if (bg) memcpy(screen->px, bg->px, (size_t)SCR_W * SCR_H * 4);
    else canvas_clear(screen, C_BG);
    fill_rect_a(screen, 0, 0, SCR_W, SCR_H, COL(8, 10, 14, 255), 88);

    fill_round_rect_aa_a(screen, 26, 18, 96, 36, 18, C_SURF2, 210);
    stroke_round_rect_aa(screen, 26, 18, 96, 36, 18, 1, C_LINE, 160);
    icon_back_arrow(screen, 38, 28, 16, C_TXT);
    draw_text(screen, font, 15, 62, 27, C_TXT, "返回");
    rc_p_back = (Rect){ 26, 18, 96, 36 };

    char top_info[64];
    snprintf(top_info, sizeof(top_info), "%s · %d/%d", p_fmt[0] ? p_fmt : "AUDIO",
             playing_idx >= 0 ? playing_idx + 1 : 0, ntracks);
    if (portrait()) {
        draw_text_centered(screen, font, 16, 128, SCR_W - 196, 27, C_TXT2, top_info);
    } else {
        char land_info[80];
        snprintf(land_info, sizeof(land_info), "正在播放 · %s", top_info);
        draw_text_centered(screen, font, 16, 130, SCR_W - 220, 27, C_TXT2, land_info);
    }

    int rx = SCR_W - 26;
    int dw = text_width(font, 15, "自检") + 24;
    rx -= dw; rc_p_diag = draw_pill_btn(rx, 18, 36, 15, "自检", 0, C_PRIMARY);
    int ow = text_width(font, 15, "横/竖") + 24;
    rx -= ow + 8; rc_p_ori = draw_pill_btn(rx, 18, 36, 15, "横/竖", ori_mode != 0, C_PRIMARY);

    double pos = show_pos;
    double dur = audio_duration();
    if (dur > 0 && pos > dur) pos = dur;

    if (!portrait()) {
        int csz = 360;
        int cx = 84, cy = 104;
        ui_cover_x = cx; ui_cover_y = cy; ui_cover_sz = csz;

        fill_round_rect_shadow(screen, cx, cy, csz, csz, 28, 72);
        if (anim_cover < 1 && cover_prev) {
            float t = ease_out(anim_cover);
            blit_rgba_round_a(screen, cx, cy, cover_prev, csz, csz, 28, (uint8_t)((1 - t) * 255));
            if (cover) blit_rgba_round_a(screen, cx, cy, cover, csz, csz, 28, (uint8_t)(t * 255));
        } else if (cover) {
            blit_rgba_round(screen, cx, cy, cover, csz, csz, 28);
        }
        stroke_round_rect_aa(screen, cx, cy, csz, csz, 28, 1, COL(255, 255, 255, 255), 36);

        int col_x = 492, col_x1 = SCR_W - 64;
        int col_w = col_x1 - col_x;
        float tpx = text_width(font, 38, p_title) > col_w ? 30.0f : 38.0f;
        draw_text_max(screen, font, tpx, col_x, 110, C_TXT, p_title, col_w);
        draw_text_max(screen, font, 19, col_x, 162, C_TXT2, p_artist[0] ? p_artist : "未知艺术家", col_w);
        fill_rect_a(screen, col_x, 200, 64, 3, C_PRIMARY, 190);

        draw_lyric_or_spectrum(col_x, col_x1, 232, 496, 466, 492, pos, dur, 0);

        int py = 562;
        ui_px0 = 84; ui_px1 = SCR_W - 84; ui_py = py;
        draw_progress_bar(py, pos, dur);

        int cy2 = 646;
        ui_cx = SCR_W / 2; ui_cy = cy2; ui_gap = 88; ui_gap2 = 176;
        draw_controls(cy2);
    } else {
        int csz = 360;
        int cx = (SCR_W - csz) / 2, cy = 94;
        ui_cover_x = cx; ui_cover_y = cy; ui_cover_sz = csz;

        fill_round_rect_shadow(screen, cx, cy, csz, csz, 28, 72);
        if (anim_cover < 1 && cover_prev) {
            float t = ease_out(anim_cover);
            blit_rgba_round_a(screen, cx, cy, cover_prev, csz, csz, 28, (uint8_t)((1 - t) * 255));
            if (cover) blit_rgba_round_a(screen, cx, cy, cover, csz, csz, 28, (uint8_t)(t * 255));
        } else if (cover) {
            blit_rgba_round(screen, cx, cy, cover, csz, csz, 28);
        }
        stroke_round_rect_aa(screen, cx, cy, csz, csz, 28, 1, COL(255, 255, 255, 255), 36);

        float tpx = text_width(font, 36, p_title) > (SCR_W - 80) ? 28.0f : 36.0f;
        draw_text_centered_max(screen, font, tpx, 40, SCR_W - 40, cy + csz + 24, C_TXT, p_title);
        draw_text_centered_max(screen, font, 20, 40, SCR_W - 40, cy + csz + 76, C_TXT2,
                               p_artist[0] ? p_artist : "未知艺术家");

        draw_lyric_or_spectrum(40, SCR_W - 40, cy + csz + 132, 956, 920, 954, pos, dur, 1);

        int py = SCR_H - 224;
        ui_px0 = 56; ui_px1 = SCR_W - 56; ui_py = py;
        draw_progress_bar(py, pos, dur);

        int cy2 = SCR_H - 122;
        ui_cx = SCR_W / 2; ui_cy = cy2; ui_gap = 96; ui_gap2 = 192;
        draw_controls(cy2);
    }

    if (audio_status() == 3) {
        draw_text_centered(screen, font, 24, 0, SCR_W, SCR_H / 2 - 40, COL(255, 140, 130, 255), "解码失败：文件不支持或损坏");
        draw_text_centered(screen, font, 18, 0, SCR_W, SCR_H / 2 - 4, C_TXT2, p_title);
    }

    if (loading)
        draw_text_centered(screen, font, 24, 0, SCR_W, SCR_H / 2, C_TXT2, "加载中…");

    draw_touch_feedback();
}

static void draw_volume_overlay(void) {
    if (vol_overlay_timer <= 0) return;
    vol_overlay_timer--;
    char vs[64];
    snprintf(vs, sizeof(vs), "音量 %d%%", (int)(audio_volume() * 100 + 0.5f));
    int bw2 = 260, bh2 = 72;
    int bx = (SCR_W - bw2) / 2, by = 84;
    fill_round_rect_aa_a(screen, bx, by, bw2, bh2, 18, C_SURFACE, 242);
    stroke_round_rect_aa(screen, bx, by, bw2, bh2, 18, 1, C_LINE, 200);
    icon_speaker(screen, bx + 26, by + 16, 20, C_PRIMARY);
    draw_text_centered(screen, font, 20, bx + 20, bx + bw2, by + 14, C_TXT, vs);
    fill_round_rect_aa(screen, bx + 26, by + 48, bw2 - 52, 6, 3, C_LINE);
    fill_round_rect_aa(screen, bx + 26, by + 48, (int)((bw2 - 52) * audio_volume()), 6, 3, C_PRIMARY);
}

static void draw_diag(void) {
    fill_rect_a(screen, 0, 0, SCR_W, SCR_H, COL(0, 0, 0, 255), 220);
    draw_text(screen, font, 30, 48, 44, C_PRIMARY, "Siren 音频与系统自检 (按 Y 或点击任意处关闭)");
    char buf[1024];
    audio_get_diag(buf, sizeof(buf));
    int yy = 104;
    char* line = strtok(buf, "\n");
    while (line) {
        draw_text(screen, font, 22, 48, yy, C_TXT, line);
        yy += 38;
        line = strtok(NULL, "\n");
    }
    char extra[256];
    snprintf(extra, sizeof(extra), "曲库=%d 收藏=%d 模式=%s 主题=%s",
             ntracks, fav_count, mode_names[play_mode], theme_names[cur_theme]);
    draw_text(screen, font, 22, 48, yy + 10, C_TXT2, extra);
    char lyr_info[256];
    if (lyr) snprintf(lyr_info, sizeof(lyr_info), "歌词=已加载 %d 行 (%s)", lyr->n, lyr->source);
    else snprintf(lyr_info, sizeof(lyr_info), "歌词=未找到 (支持外置 .lrc 与 MP3/FLAC/OGG 内嵌标签)");
    draw_text(screen, font, 20, 48, yy + 48, C_TXT2, lyr_info);
    draw_text(screen, font, 18, 48, SCR_H - 54, C_TXT3, "若 状态=FAILED 或 设备=0 请检查 sd 卡音频与主机声音输出");
}

static void ui_player_input(uint64_t down, uint64_t rep) {
    if (down & HidNpadButton_A) {
        if (audio_paused()) audio_resume(); else audio_pause();
    }
    if (down & HidNpadButton_B) {
        switch_page(UI_LIST);
        sel = playing_idx >= 0 ? playing_idx : sel;
        rebuild_visible();
    }
    if (down & HidNpadButton_X) {
        if (playing_idx >= 0) toggle_fav(playing_idx);
    }
    if (down & HidNpadButton_ZL) {
        cycle_play_mode();
    }
    if (down & HidNpadButton_ZR) {
        if (ntracks > 0 && !loading) load_track(pick_next(playing_idx, 1));
    }
    if (down & HidNpadButton_L) {
        if (ntracks > 0 && !loading) load_track(pick_prev(playing_idx));
    }
    if (down & HidNpadButton_R) {
        if (ntracks > 0 && !loading) load_track(pick_next(playing_idx, 1));
    }
    if (rep & (HidNpadButton_Left | HidNpadButton_StickLLeft)) {
        double p = audio_position() - 10;
        if (p < 0) p = 0;
        audio_seek(p);
    }
    if (rep & (HidNpadButton_Right | HidNpadButton_StickRRight | HidNpadButton_StickLRight)) {
        audio_seek(audio_position() + 10);
    }
    if (rep & (HidNpadButton_Up | HidNpadButton_StickLUp | HidNpadButton_StickRUp)) {
        audio_set_volume(audio_volume() + 0.05f);
        save_settings();
        vol_overlay_timer = 70;
    }
    if (rep & (HidNpadButton_Down | HidNpadButton_StickLDown | HidNpadButton_StickRDown)) {
        audio_set_volume(audio_volume() - 0.05f);
        save_settings();
        vol_overlay_timer = 70;
    }
}

// ---------- main ----------
int main(int argc, char** argv) {
    (void)argc; (void)argv;

    if (be_init() != 0) return 0;
    rng_state = (uint32_t)(armGetSystemTick() ^ 0xA5A55A5Au);
    if (audio_init() != 0) {
        // 音频不可用时仍显示界面
    }
    load_settings();
    sixaxis_init();

    update_layout();
    apply_orientation(ORI_LAND);
    size_t font_len = (size_t)(font_ttf_end - font_ttf_start);
    font = font_load(font_ttf_start, font_len);

    tracks = (Track*)calloc(MAX_TRACKS, sizeof(Track));

    mkdir("sdmc:/music", 0777);

    const char* roots[] = {
        "sdmc:/music", "sdmc:/Music", "sdmc:/Musics",
        "sdmc:/Songs", "sdmc:/Audio", "sdmc:/Playlist", "sdmc:/Downloads"
    };
    for (unsigned i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        scan_dir(roots[i], 0);
        if (ntracks >= MAX_TRACKS) break;
    }
    if (ntracks == 0) scan_dir("sdmc:/", 0);
    if (ntracks > 1) {
        qsort(tracks, (size_t)ntracks, sizeof(Track), cmp_track);
        int w = 0;
        for (int i = 0; i < ntracks; i++) {
            if (w > 0 && !strcasecmp(tracks[w - 1].path, tracks[i].path)) continue;
            if (w != i) tracks[w] = tracks[i];
            w++;
        }
        ntracks = w;
    }
    load_favorites();
    rebuild_visible();

    build_poster_thumbs();
    memset(&meta, 0, sizeof(meta));

    int stride = 0;
    while (be_should_run()) {
        anim_frame_begin();
        be_input_update();
        be_touch_update(&touch_now);
        uint64_t down = be_btn_down();
        uint64_t held = be_btn_held();
        uint64_t rep = repeat_keys(down, held,
            HidNpadButton_Up | HidNpadButton_Down | HidNpadButton_Left | HidNpadButton_Right |
            HidNpadButton_StickLUp | HidNpadButton_StickLDown | HidNpadButton_StickLLeft | HidNpadButton_StickLRight |
            HidNpadButton_StickRUp | HidNpadButton_StickRDown | HidNpadButton_StickRLeft | HidNpadButton_StickRRight);

        if (down & HidNpadButton_Plus) break;
        if (down & HidNpadButton_Y) show_diag = !show_diag;
        if (down & HidNpadButton_Minus) cycle_orientation();

        if (ori_mode == 0 && g_six_ok) {
            int want = ori_read_candidate();
            if (want == ori) {
                ori_hold = 0;
            } else if (want == ori_cand) {
                if (++ori_hold >= 20) { apply_orientation(want); ori_hold = 0; }
            } else {
                ori_cand = want;
                ori_hold = 0;
            }
        }

        if (touch_now.held || touch_now.up) {
            if (ori == ORI_PORT_A) {
                // 顺时针竖持：硬件 (x, y) -> 竖屏画布 (719 - y, x)
                int nx = 719 - touch_now.y;
                int ny = touch_now.x;
                touch_now.x = nx; touch_now.y = ny;
            } else if (ori == ORI_PORT_B) {
                // 逆时针竖持：硬件 (x, y) -> 竖屏画布 (y, 1279 - x)
                int nx = touch_now.y;
                int ny = 1279 - touch_now.x;
                touch_now.x = nx; touch_now.y = ny;
            }
        }

        if (show_diag) {
            if (touch_now.down || (down & HidNpadButton_B)) show_diag = 0;
        } else if (ui_state == UI_LIST) {
            if (down & HidNpadButton_B) break;
            ui_list_input(down, rep);
            handle_touch_list(&touch_now);
        } else {
            ui_player_input(down, rep);
            handle_touch_player(&touch_now);
        }

        if (audio_finished() && playing_idx >= 0 && !loading) {
            load_track(pick_next(playing_idx, 0));
        }

        audio_pump();

        if (!touch_drag && fabsf(scroll_vel) > 10.0f) {
            scroll_px += scroll_vel * (float)g_dt;
            scroll_vel *= expf(-6.5f * (float)g_dt);
            float ms = max_scroll_px();
            if (scroll_px <= 0.0f) { scroll_px = 0.0f; scroll_vel = 0.0f; }
            if (scroll_px >= ms)   { scroll_px = ms;   scroll_vel = 0.0f; }
        } else if (!touch_drag) {
            scroll_vel = 0.0f;
        }
        anim_scroll_px = approach(anim_scroll_px, scroll_px, 20.0f, (float)g_dt);
        if (fabsf(anim_scroll_px - scroll_px) < 0.4f) anim_scroll_px = scroll_px;

        if (anim_cover < 1) anim_cover = (float)(anim_cover + g_dt / 0.30);
        if (anim_cover > 1) anim_cover = 1;
        if (trans_t < 1) trans_t = (float)(trans_t + g_dt / 0.24);
        if (trans_t > 1) trans_t = 1;
        if (ripple_t < 1) ripple_t = (float)(ripple_t + g_dt / 0.42);
        if (touch_fx_alpha > 0) {
            touch_fx_alpha -= (float)(g_dt * 2.4);
            if (touch_fx_alpha < 0) touch_fx_alpha = 0;
        }

        double ap = audio_position();
        show_pos = (double)approach((float)show_pos, (float)ap, 10.0f, (float)g_dt);
        if (show_pos < 0) show_pos = 0;
        if (fabs(show_pos - ap) > 3.0) show_pos = ap;

        uint32_t* fb = be_begin(&stride);
        if (ui_state == UI_LIST) draw_list();
        else draw_player();

        draw_volume_overlay();

        if (toast_timer > 0) {
            toast_timer--;
            int tw = text_width(font, 16, toast_text) + 52;
            if (tw > SCR_W - 80) tw = SCR_W - 80;
            int bx = (SCR_W - tw) / 2;
            fill_round_rect_aa_a(screen, bx, 20, tw, 44, 22, C_SURFACE, 240);
            stroke_round_rect_aa(screen, bx, 20, tw, 44, 22, 1, C_PRIMARY, 180);
            draw_text_centered(screen, font, 16, bx, bx + tw, 31, C_TXT, toast_text);
        }

        if (trans_t < 1 && trans_shot && trans_shot->w == SCR_W && trans_shot->h == SCR_H) {
            uint8_t a = (uint8_t)((1 - ease_out(trans_t)) * 255);
            int dy = -(int)(ease_out(trans_t) * 44);
            blit_rgba_a(screen, 0, dy, trans_shot->px, SCR_W, SCR_H, a);
        }

        if (show_diag) draw_diag();

        if (ori == ORI_LAND) {
            for (int y = 0; y < SCR_H; y++) {
                uint32_t* dst = fb + (size_t)y * stride;
                uint32_t* src = screen->px + (size_t)y * SCR_W;
                memcpy(dst, src, (size_t)SCR_W * 4);
            }
        } else if (ori == ORI_PORT_A) {
            // 顺时针竖持 90° 纯旋转 (det=+1)：fb[y][x] = screen(px=719-y, py=x)
            for (int y = 0; y < 720; y++) {
                uint32_t* d = fb + (size_t)y * stride;
                int px = 719 - y;
                for (int x = 0; x < 1280; x++)
                    d[x] = screen->px[(size_t)x * 720 + px];
            }
        } else {
            // 逆时针竖持 90° 纯旋转 (det=+1)：fb[y][x] = screen(px=y, py=1279-x)
            for (int y = 0; y < 720; y++) {
                uint32_t* d = fb + (size_t)y * stride;
                for (int x = 0; x < 1280; x++)
                    d[x] = screen->px[(size_t)(1279 - x) * 720 + y];
            }
        }
        be_end();
    }

    audio_exit();
    be_exit();
    if (font) font_free(font);
    if (screen) canvas_free(screen);
    if (bg) canvas_free(bg);
    if (list_bg) canvas_free(list_bg);
    if (cover) free(cover);
    if (cover_prev) free(cover_prev);
    if (trans_shot) canvas_free(trans_shot);
    if (poster36) free(poster36);
    if (poster52) free(poster52);
    if (poster84) free(poster84);
    meta_clear(&meta);
    if (lyr) lyrics_free(lyr);
    for (int i = 0; i < ntracks; i++) {
        free(tracks[i].thumb);
        free(tracks[i].thumb84);
        meta_clear(&tracks[i].meta);
    }
    free(tracks);
    return 0;
}
