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
#define TRANS_FRAMES 5

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

// ---------- 屏幕方向（横屏 + 单向竖屏） ----------
#define ORI_LAND 0   // 1280x720 横屏便携 MP4 双栏
#define ORI_PORT 1   // 720x1280 单向竖屏掌机 MP4
static int ori_locked = 0;
static int ori = ORI_LAND;
static int SCR_W = 1280, SCR_H = 720;
static int portrait(void) { return ori == ORI_PORT; }

// ---------- 现代 Hi-Res 便携 MP4 布局 ----------
static int L_X0, L_X1, L_TOP, L_BOT, L_ROW, L_CARD_H, L_TITLE, L_ART;
static void update_layout(void) {
    if (portrait()) {
        L_X0 = 20;
        L_X1 = SCR_W - 20;
        L_TOP = 132;
        L_BOT = SCR_H - 178; // 底部留 162px 给掌机触控播控底座
        L_ROW = 84;
        L_CARD_H = 76;
        L_TITLE = 20;
        L_ART = 14;
    } else {
        L_X0 = 486;          // 右侧曲目队列区（左侧为独立 MP4 播放机芯卡）
        L_X1 = SCR_W - 24;
        L_TOP = 110;
        L_BOT = SCR_H - 22;
        L_ROW = 68;
        L_CARD_H = 60;
        L_TITLE = 18;
        L_ART = 13;
    }
}

static int list_area_h(void) {
    int h = L_BOT - L_TOP;
    return h < L_ROW ? L_ROW : h;
}

static int rows_visible(void) {
    int r = list_area_h() / L_ROW;
    return r < 1 ? 1 : r;
}

// ---------- 现代 Hi-Res DAP 配色主题 ----------
#define THEME_GOLD  0
#define THEME_BLUE  1
#define THEME_LIGHT 2
#define THEME_COUNT 3

static uint32_t C_BG, C_PANEL, C_CARD, C_ELEV, C_ACCENT_BG, C_ACCENT, C_ACCENT_TXT;
static uint32_t C_ON_ACCENT, C_TXT, C_TXT2, C_TXT3, C_TRACK_BG, C_HEART;
static int g_is_light = 0;

typedef struct {
    uint32_t bg, panel, card, elev, accent_bg, accent, accent_txt, on_accent;
    uint32_t txt, txt2, txt3, track_bg;
    int is_light;
} Theme;

static const Theme themes[THEME_COUNT] = {
    // 黑金 Hi-Res（现代随身听 / DAP 深空琥珀金）
    { COL(0x0B,0x0D,0x12,0xFF), COL(0x13,0x16,0x1F,0xFF),
      COL(0x18,0x1C,0x27,0xFF), COL(0x22,0x28,0x36,0xFF),
      COL(0x2B,0x23,0x14,0xFF), COL(0xF3,0xC1,0x67,0xFF),
      COL(0xFB,0xDD,0x9B,0xFF), COL(0x1A,0x14,0x08,0xFF),
      COL(0xF4,0xF6,0xFA,0xFF), COL(0xB4,0xBD,0xCC,0xFF),
      COL(0x7C,0x86,0x99,0xFF), COL(0x2B,0x31,0x42,0xFF), 0 },
    // 极夜蓝（Arctic Blue 现代冷冽深蓝）
    { COL(0x0B,0x10,0x18,0xFF), COL(0x12,0x18,0x24,0xFF),
      COL(0x18,0x20,0x30,0xFF), COL(0x22,0x2D,0x42,0xFF),
      COL(0x16,0x2D,0x4C,0xFF), COL(0x67,0xB4,0xFF,0xFF),
      COL(0xC9,0xE4,0xFF,0xFF), COL(0x07,0x20,0x3D,0xFF),
      COL(0xF2,0xF6,0xFC,0xFF), COL(0xB2,0xC0,0xD4,0xFF),
      COL(0x78,0x89,0xA3,0xFF), COL(0x29,0x35,0x4B,0xFF), 0 },
    // 凝霜白（Modern Glacier Light · 无拟物边框）
    { COL(0xF2,0xF4,0xF8,0xFF), COL(0xFF,0xFF,0xFF,0xFF),
      COL(0xFF,0xFF,0xFF,0xFF), COL(0xE8,0xEC,0xF2,0xFF),
      COL(0xFF,0xE6,0xEA,0xFF), COL(0xFA,0x2D,0x55,0xFF),
      COL(0xC8,0x19,0x3E,0xFF), COL(0xFF,0xFF,0xFF,0xFF),
      COL(0x12,0x16,0x1F,0xFF), COL(0x4E,0x57,0x66,0xFF),
      COL(0x7C,0x85,0x94,0xFF), COL(0xE1,0xE5,0xED,0xFF), 1 }
};

static const char* theme_names[THEME_COUNT] = { "黑金", "极蓝", "凝霜白" };
static int cur_theme = THEME_GOLD;

static void apply_theme(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= THEME_COUNT) idx = THEME_COUNT - 1;
    cur_theme = idx;
    const Theme* t = &themes[idx];
    C_BG = t->bg; C_PANEL = t->panel; C_CARD = t->card; C_ELEV = t->elev;
    C_ACCENT_BG = t->accent_bg; C_ACCENT = t->accent; C_ACCENT_TXT = t->accent_txt;
    C_ON_ACCENT = t->on_accent;
    C_TXT = t->txt; C_TXT2 = t->txt2; C_TXT3 = t->txt3; C_TRACK_BG = t->track_bg;
    C_HEART = COL(0xFA, 0x45, 0x6C, 0xFF);
    g_is_light = t->is_light;
}

typedef struct {
    char path[600];
    TrackMeta meta;
    int meta_state;     // 0 = 未加载, 1 = 已加载
    uint32_t* thumb;    // 52x52 缩略图
    int fav;
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
static Canvas* bg = NULL;

// 播放页资源
static TrackMeta meta;
static Lyrics* lyr = NULL;
static uint32_t* cover = NULL;       // 340x340 全屏播放封面
static uint32_t* cover244 = NULL;    // 244x244 横屏左机芯卡片封面
static uint32_t* poster36 = NULL;
static uint32_t* poster52 = NULL;
static uint32_t* poster244 = NULL;
static int cover_ready = 0;
static char p_title[256];
static char p_artist[256];
static char p_fmt[12];
static float bars[28];

static int vol_overlay_timer = 0;
static int show_diag = 0;

// ---------- 提示气泡 ----------
static char toast_text[96];
static int toast_timer = 0;
static void show_toast(const char* s) {
    snprintf(toast_text, sizeof(toast_text), "%s", s);
    toast_timer = 90;
}

// ---------- 极简 5 帧淡化过渡 & 列表滚动 ----------
static double g_dt = 0.016;
static uint64_t g_last_tick = 0;
static float scroll_px = 0.0f;
static Canvas* trans_shot = NULL;
static int trans_frames = 0;

// 进度条与封面触摸区
static int ui_px0, ui_px1, ui_py;
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

// 可点击区域
typedef struct { int x, y, w, h; } Rect;
static Rect rc_tab_all, rc_tab_fav, rc_btn_search, rc_btn_rescan;
static Rect rc_btn_theme, rc_btn_ori, rc_btn_diag;
static Rect rc_empty_rescan, rc_p_back;
static Rect rc_tr_fav, rc_tr_prev, rc_tr_play, rc_tr_next, rc_tr_mode, rc_open_player;

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
    int t = THEME_GOLD, pm = MODE_LOOP_ALL;
    float vol = 0.85f;
    FILE* f = fopen("sdmc:/Siren/settings.cfg", "r");
    if (f) {
        if (fscanf(f, "%d %d %f", &t, &pm, &vol) >= 1) {
            if (pm >= 0 && pm < MODE_COUNT) play_mode = pm;
            if (vol >= 0.0f && vol <= 1.0f) audio_set_volume(vol);
        }
        fclose(f);
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

// ---------- 六轴加速度计（仅单向竖屏自动检测） ----------
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
    if (ax < -0.68f && fabsf(ay) < 0.42f) return ORI_PORT;
    if (fabsf(ay) > 0.68f && fabsf(ax) < 0.42f) return ORI_LAND;
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
        trans_frames = 0;
    }
    float ms = max_scroll_px();
    if (scroll_px > ms) scroll_px = ms;
    if (scroll_px < 0) scroll_px = 0;
    touch_drag = 0;
    touch_press_idx = -1;
    if (!bg && cover_ready && cover) build_bg_from(cover, 340, 340);
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

static void frame_begin(void) {
    uint64_t now = armGetSystemTick();
    uint64_t freq = armGetSystemTickFreq();
    if (g_last_tick) {
        double d = (double)(now - g_last_tick) / (double)freq;
        if (d < 0.001) d = 0.001;
        if (d > 0.05) d = 0.05;
        g_dt = d;
    }
    g_last_tick = now;
}

static void switch_page(int to) {
    if (to == ui_state) return;
    if (!trans_shot) trans_shot = canvas_new(SCR_W, SCR_H);
    if (trans_shot && screen)
        memcpy(trans_shot->px, screen->px, (size_t)SCR_W * SCR_H * 4);
    ui_state = to;
    trans_frames = TRANS_FRAMES;
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

// ---------- 默认封面与现代环境光背景 ----------
static void make_default_art(uint32_t* dst, int n, const char* seed) {
    uint32_t h = hash_str(seed);
    uint8_t r, g, b;
    hsv2rgb((float)((h % 360) / 360.0), 0.48f, 0.36f, &r, &g, &b);
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            float t = (x + y) / (float)(2 * n);
            dst[(size_t)y * n + x] = COL((uint8_t)(r * (1 - t) + 22 * t),
                                         (uint8_t)(g * (1 - t) + 24 * t),
                                         (uint8_t)(b * (1 - t) + 32 * t), 255);
        }
    }
    Canvas c = { n, n, dst };
    draw_note(&c, n / 6, n / 6, n * 2 / 3, COL(255, 255, 255, 220));
}

static void build_bg_from(const uint32_t* art, int aw, int ah) {
    if (!bg) bg = canvas_new(SCR_W, SCR_H);
    int iw = portrait() ? 36 : 64;
    int ih = portrait() ? 64 : 36;
    uint32_t small[64 * 64];
    resample_bilinear(art, aw, ah, small, iw, ih);
    box_blur(small, iw, ih, 3, 2);
    resample_bilinear(small, iw, ih, bg->px, bg->w, bg->h);
    scale_rgb(bg->px, (size_t)bg->w * bg->h, 0.35f);
    for (int y = 0; y < bg->h; y++) {
        for (int x = 0; x < bg->w; x++) {
            float dx = (x - bg->w / 2.0f) / (bg->w / 2.0f);
            float dy = (y - bg->h / 2.0f) / (bg->h / 2.0f);
            float d = sqrtf(dx * dx + dy * dy);
            float f = 1.0f - 0.45f * d * d;
            if (f < 0.16f) f = 0.16f;
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
    hsv2rgb((float)((h % 360) / 360.0), 0.45f, 0.18f, &r, &g, &b);
    for (int y = 0; y < bg->h; y++) {
        for (int x = 0; x < bg->w; x++) {
            float t = ((x * 0.6f + y) / (bg->w * 0.6f + bg->h));
            bg->px[(size_t)y * bg->w + x] = COL((uint8_t)(r * t + 14 * (1 - t)),
                                                (uint8_t)(g * t + 16 * (1 - t)),
                                                (uint8_t)(b * t + 22 * (1 - t)), 255);
        }
    }
}

static void build_poster_thumbs(void) {
    size_t poster_len = (size_t)(poster_end - poster_start);
    if (poster_len < 320 * 320 * 4) return;
    poster36  = (uint32_t*)calloc(36 * 36, 4);
    poster52  = (uint32_t*)calloc(52 * 52, 4);
    poster244 = (uint32_t*)calloc(244 * 244, 4);
    if (poster36)  resample_bilinear((const uint32_t*)poster_start, 320, 320, poster36, 36, 36);
    if (poster52)  resample_bilinear((const uint32_t*)poster_start, 320, 320, poster52, 52, 52);
    if (poster244) resample_bilinear((const uint32_t*)poster_start, 320, 320, poster244, 244, 244);
}

// ---------- SD 卡安全扫描 ----------
static int should_skip_dir(const char* name) {
    const char* skip[] = {
        "Nintendo", "atmosphere", "bootloader", "bootloader2",
        "config", "dev", "switch", "system", "SXOS", "HeiTag",
        "sept", "stratosphere", "firmware", "emuMMC", "JKSV",
        "backup", "tinfoil", "dbi", "RetroArch", "themes",
        "warmboot_mariko", "android", "lost+found",
        "System Volume Information", "$RECYCLE.BIN"
    };
    for (unsigned i = 0; i < sizeof(skip) / sizeof(skip[0]); i++)
        if (!strcasecmp(name, skip[i])) return 1;
    return 0;
}

static void join_clean_path(char* dst, size_t cap, const char* dir, const char* name) {
    size_t len = strlen(dir);
    while (len > 0 && dir[len - 1] == '/') len--;
    if (len == 0) snprintf(dst, cap, "/%s", name);
    else snprintf(dst, cap, "%.*s/%s", (int)len, dir, name);
}

static void scan_dir(const char* dir, int depth) {
    if (depth > 4 || ntracks >= MAX_TRACKS) return;
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char path[600];
        join_clean_path(path, sizeof(path), dir, e->d_name);

        int is_dir = 0, is_reg = 0;
#ifdef DT_DIR
        if (e->d_type == DT_DIR) is_dir = 1;
        else if (e->d_type == DT_REG) is_reg = 1;
        else
#endif
        {
            struct stat st;
            if (stat(path, &st) == 0) {
                is_dir = S_ISDIR(st.st_mode);
                is_reg = S_ISREG(st.st_mode);
            }
        }

        if (is_dir) {
            if (!should_skip_dir(e->d_name)) scan_dir(path, depth + 1);
        } else if (is_reg && dec_is_audio(path)) {
            if (ntracks < MAX_TRACKS) {
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

static void scan_library(int notify) {
    char playing_path[600] = {0};
    if (playing_idx >= 0 && playing_idx < ntracks)
        snprintf(playing_path, sizeof(playing_path), "%s", tracks[playing_idx].path);

    for (int i = 0; i < ntracks; i++) {
        free(tracks[i].thumb);
        meta_clear(&tracks[i].meta);
    }
    memset(tracks, 0, sizeof(Track) * MAX_TRACKS);
    ntracks = 0;
    loaded_thumbs = 0;
    fav_count = 0;
    playing_idx = -1;

    mkdir("sdmc:/music", 0777);
    const char* roots[] = {
        "sdmc:/music", "sdmc:/Music", "sdmc:/Musics",
        "sdmc:/Songs", "sdmc:/Audio", "sdmc:/Playlist",
        "sdmc:/Downloads", "sdmc:/MP3", "sdmc:/FLAC",
        "sdmc:/音乐", "sdmc:/歌曲", "sdmc:/CloudMusic", "sdmc:/QQMusic",
        "sdmc:/switch/Siren", "sdmc:/switch/music"
    };
    for (unsigned i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        scan_dir(roots[i], 0);
        if (ntracks >= MAX_TRACKS) break;
    }
    if (ntracks < MAX_TRACKS) scan_dir("sdmc:/", 0);

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

    if (playing_path[0]) {
        for (int i = 0; i < ntracks; i++) {
            if (!strcasecmp(tracks[i].path, playing_path)) {
                playing_idx = i;
                break;
            }
        }
    }

    load_favorites();
    scroll_px = 0;
    rebuild_visible();

    if (notify) {
        char msg[96];
        if (ntracks > 0) snprintf(msg, sizeof(msg), "扫描完成：共找到 %d 首歌曲", ntracks);
        else snprintf(msg, sizeof(msg), "未发现音频，请将歌曲放入 sdmc:/music");
        show_toast(msg);
    }
}

// ---------- 元数据 / 缩略图懒加载 ----------
static void ensure_track_meta(int idx, int full_scan) {
    if (idx < 0 || idx >= ntracks) return;
    Track* t = &tracks[idx];
    if (t->meta_state) return;
    t->meta_state = 1;
    loaded_thumbs++;

    if (!t->thumb) t->thumb = (uint32_t*)calloc(52 * 52, 4);

    if (full_scan) meta_load(t->path, &t->meta);
    else meta_load_quick(t->path, &t->meta);

    if (t->meta.art && t->meta.art_w > 8 && t->meta.art_h > 8) {
        if (t->thumb)
            resample_bilinear((const uint32_t*)t->meta.art, t->meta.art_w, t->meta.art_h, t->thumb, 52, 52);
        meta_release_art(&t->meta);
    } else {
        if (t->thumb && poster52) memcpy(t->thumb, poster52, (size_t)52 * 52 * 4);
        else if (t->thumb) make_default_art(t->thumb, 52, t->path);
    }
}

static void trim_thumbs(int first_vis_pos, int last_vis_pos) {
    if (loaded_thumbs <= 240) return;
    for (int i = 0; i < ntracks; i++) {
        Track* t = &tracks[i];
        if (!t->meta_state || i == playing_idx) continue;
        int keep = 0;
        for (int p = first_vis_pos - 12; p <= last_vis_pos + 12; p++) {
            if (vis_at(p) == i) { keep = 1; break; }
        }
        if (keep) continue;
        free(t->thumb); t->thumb = NULL;
        meta_clear(&t->meta);
        t->meta_state = 0;
        if (loaded_thumbs > 0) loaded_thumbs--;
    }
}

static void ensure_visible_thumbs(void) {
    int i0 = (int)floorf(scroll_px / (float)L_ROW);
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
            if (visible_idx[i] == cur) return visible_idx[(i + 1) % nvisible];
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
            if (visible_idx[i] == cur) return visible_idx[(i - 1 + nvisible) % nvisible];
        }
        return visible_idx[0];
    }
    if (ntracks <= 1) return 0;
    return (cur - 1 + ntracks) % ntracks;
}

// ---------- 曲目加载 ----------
static void sync_deck_cover(void) {
    if (!cover) return;
    if (!cover244) cover244 = (uint32_t*)calloc(244 * 244, 4);
    if (cover244) resample_bilinear(cover, 340, 340, cover244, 244, 244);
}

static void load_track(int idx) {
    if (idx < 0 || idx >= ntracks) return;
    playing_idx = idx;
    loading = 1;
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

    if (!cover) cover = (uint32_t*)calloc((size_t)340 * 340, 4);
    if (cover) {
        if (meta.art && meta.art_w > 8 && meta.art_h > 8) {
            resample_bilinear((const uint32_t*)meta.art, meta.art_w, meta.art_h, cover, 340, 340);
            build_bg_from((const uint32_t*)meta.art, meta.art_w, meta.art_h);
            cover_ready = 1;
        } else if ((size_t)(poster_end - poster_start) >= 320 * 320 * 4) {
            resample_bilinear((const uint32_t*)poster_start, 320, 320, cover, 340, 340);
            build_bg_from((const uint32_t*)poster_start, 320, 320);
            cover_ready = 1;
        } else {
            make_default_art(cover, 340, tracks[idx].path);
            build_default_bg(tracks[idx].path);
            cover_ready = 1;
        }
        sync_deck_cover();
    }

    lyr = lyrics_load_ex(tracks[idx].path, p_title, p_artist);
    for (int i = 0; i < 28; i++) bars[i] = 0;
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
    return r.w > 0 && r.h > 0 && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
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

static void toggle_orientation(void) {
    ori_locked = 1;
    apply_orientation(ori == ORI_LAND ? ORI_PORT : ORI_LAND);
    show_toast(ori == ORI_PORT ? "已切换：竖屏模式" : "已切换：横屏模式");
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

// ---------- 共享现代胶囊 & 播控绘制 ----------
static Rect draw_pill_chip(int x, int y, int h, float px, const char* txt, int active) {
    int tw = text_width(font, px, txt);
    int w = tw + 26;
    fill_round_rect_aa(screen, x, y, w, h, h / 2, active ? C_ACCENT_BG : C_ELEV);
    draw_text_centered(screen, font, px, x, x + w, y + (h - (int)px) / 2 - 1,
                       active ? C_ACCENT_TXT : C_TXT2, txt);
    Rect r = { x, y, w, h };
    return r;
}

static void draw_squircle_fab(int cx, int cy, int sz, int rad, int paused) {
    fill_round_rect_aa(screen, cx - sz / 2, cy - sz / 2, sz, sz, rad, C_ACCENT);
    int ic = sz * 36 / 100;
    if (paused) {
        fill_tri_aa(screen, cx - ic / 3 + 2, cy - ic / 2, cx - ic / 3 + 2, cy + ic / 2,
                    cx + ic / 2 + 2, cy, C_ON_ACCENT);
    } else {
        int bw = sz >= 56 ? 6 : 5;
        int bh = sz >= 56 ? 22 : 16;
        int gx = sz >= 56 ? 4 : 3;
        fill_round_rect_aa(screen, cx - gx - bw, cy - bh / 2, bw, bh, 2, C_ON_ACCENT);
        fill_round_rect_aa(screen, cx + gx, cy - bh / 2, bw, bh, 2, C_ON_ACCENT);
    }
}

static void draw_scrubber(int x0, int x1, int py, double pos, double dur, int h) {
    ui_px0 = x0; ui_px1 = x1; ui_py = py;
    int w = x1 - x0;
    fill_round_rect_aa(screen, x0, py, w, h, h / 2, C_TRACK_BG);
    int fillw = (int)((dur > 0 ? pos / dur : 0) * w);
    if (fillw < 0) fillw = 0;
    if (fillw > w) fillw = w;
    if (fillw > 0) fill_round_rect_aa(screen, x0, py, fillw, h, h / 2, C_ACCENT);
    int r = h >= 8 ? 9 : 7;
    fill_ellipse_aa(screen, x0 + fillw, py + h / 2, r, r, C_ACCENT);

    char t1[16], t2[16];
    fmt_time(pos, t1, sizeof(t1));
    fmt_time(dur, t2, sizeof(t2));
    draw_text(screen, font, 13, x0, py + h + 8, C_TXT3, t1);
    draw_text_right(screen, font, 13, x1, py + h + 8, C_TXT3, t2);
}

static void draw_transport_row(int cx, int cy, int gap, int gap2, int fab_sz) {
    int fav_on = (playing_idx >= 0 && tracks[playing_idx].fav);
    int btn_r = fab_sz >= 60 ? 25 : 21;
    int sm_r  = fab_sz >= 60 ? 22 : 19;

    fill_ellipse_aa(screen, cx - gap2, cy, sm_r, sm_r, fav_on ? C_ACCENT_BG : C_ELEV);
    icon_heart(screen, cx - gap2 - 9, cy - 9, 18, fav_on ? C_HEART : C_TXT2);
    rc_tr_fav = (Rect){ cx - gap2 - sm_r - 6, cy - sm_r - 6, (sm_r + 6) * 2, (sm_r + 6) * 2 };

    fill_ellipse_aa(screen, cx - gap, cy, btn_r, btn_r, C_ELEV);
    icon_prev(screen, cx - gap - 9, cy - 9, 18, C_TXT);
    rc_tr_prev = (Rect){ cx - gap - btn_r - 6, cy - btn_r - 6, (btn_r + 6) * 2, (btn_r + 6) * 2 };

    int paused = (playing_idx < 0) ? 1 : audio_paused();
    draw_squircle_fab(cx, cy, fab_sz, fab_sz >= 60 ? 20 : 16, paused);
    rc_tr_play = (Rect){ cx - fab_sz / 2 - 6, cy - fab_sz / 2 - 6, fab_sz + 12, fab_sz + 12 };

    fill_ellipse_aa(screen, cx + gap, cy, btn_r, btn_r, C_ELEV);
    icon_next(screen, cx + gap - 9, cy - 9, 18, C_TXT);
    rc_tr_next = (Rect){ cx + gap - btn_r - 6, cy - btn_r - 6, (btn_r + 6) * 2, (btn_r + 6) * 2 };

    const char* ml = mode_short[play_mode];
    int mw = text_width(font, 13, ml) + 24;
    int mh = sm_r * 2 - 2;
    int mx = cx + gap2 - mw / 2;
    int mbg = (play_mode == MODE_LOOP_ALL) ? C_ELEV : C_ACCENT_BG;
    uint32_t mfg = (play_mode == MODE_LOOP_ALL) ? C_TXT2 : C_ACCENT_TXT;
    fill_round_rect_aa(screen, mx, cy - mh / 2, mw, mh, mh / 2, mbg);
    draw_text_centered(screen, font, 13, mx, mx + mw, cy - 7, mfg, ml);
    rc_tr_mode = (Rect){ mx - 6, cy - mh / 2 - 6, mw + 12, mh + 12 };
}

// ---------- 触摸交互 ----------
static int handle_transport_touch(int x, int y) {
    if (in_rc(x, y, rc_tr_fav)) {
        if (playing_idx >= 0) toggle_fav(playing_idx);
        else if (sel >= 0) toggle_fav(sel);
        return 1;
    }
    if (in_rc(x, y, rc_tr_prev)) {
        if (ntracks > 0 && !loading) {
            int p = pick_prev(playing_idx >= 0 ? playing_idx : sel);
            load_track(p);
            sel = p;
            ensure_pos_visible(sel_pos());
        }
        return 1;
    }
    if (in_rc(x, y, rc_tr_play)) {
        if (playing_idx >= 0) {
            if (audio_paused()) audio_resume(); else audio_pause();
        } else if (nvisible > 0 && !loading) {
            int i = sel >= 0 ? sel : vis_at(0);
            if (i >= 0) { load_track(i); sel = i; }
        }
        return 1;
    }
    if (in_rc(x, y, rc_tr_next)) {
        if (ntracks > 0 && !loading) {
            int p = pick_next(playing_idx >= 0 ? playing_idx : sel, 1);
            load_track(p);
            sel = p;
            ensure_pos_visible(sel_pos());
        }
        return 1;
    }
    if (in_rc(x, y, rc_tr_mode)) {
        cycle_play_mode();
        return 1;
    }
    return 0;
}

static int handle_top_nav_touch(int x, int y) {
    if (in_rc(x, y, rc_tab_all)) {
        search_q[0] = 0;
        set_view(VIEW_HOME);
        return 1;
    }
    if (in_rc(x, y, rc_tab_fav)) {
        set_view(view == VIEW_FAVORITE ? VIEW_HOME : VIEW_FAVORITE);
        return 1;
    }
    if (in_rc(x, y, rc_btn_search)) {
        if (view == VIEW_SEARCH && search_q[0]) {
            search_q[0] = 0;
            set_view(VIEW_HOME);
            show_toast("已清除搜索");
        } else {
            open_search_keyboard();
        }
        return 1;
    }
    if (in_rc(x, y, rc_btn_rescan) || in_rc(x, y, rc_empty_rescan)) {
        scan_library(1);
        return 1;
    }
    if (in_rc(x, y, rc_btn_theme)) { cycle_theme(); return 1; }
    if (in_rc(x, y, rc_btn_ori))   { toggle_orientation(); return 1; }
    if (in_rc(x, y, rc_btn_diag))  { show_diag = 1; return 1; }
    return 0;
}

static void handle_touch_list(TouchInfo* t) {
    if (touch_scrub && t->held) {
        seek_from_touch(t->x, ui_px0, ui_px1);
        return;
    }
    if (touch_scrub && t->up) {
        touch_scrub = 0;
        return;
    }

    int cardw = L_X1 - L_X0;
    if (touch_drag && t->held && max_scroll_px() > 0.0f) {
        int dy = touch_last_y - t->y;
        if (abs(t->y - touch_drag_start_y) > 10) touch_moved = 1;
        if (dy != 0) {
            scroll_px += (float)dy;
            float ms = max_scroll_px();
            if (scroll_px < 0) scroll_px = 0;
            if (scroll_px > ms) scroll_px = ms;
        }
        touch_last_y = t->y;
    }

    if (!t->down) {
        if (t->up && touch_drag) {
            if (!touch_moved && touch_press_idx >= 0 && touch_press_idx < nvisible) {
                int idx = vis_at(touch_press_idx);
                if (idx >= 0) {
                    if (t->x >= L_X1 - 62 && t->x <= L_X1) {
                        toggle_fav(idx);
                    } else if (!loading) {
                        sel = idx;
                        load_track(idx);
                        if (portrait()) switch_page(UI_PLAYER);
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
    if (handle_top_nav_touch(x, y)) return;
    if (handle_transport_touch(x, y)) return;

    if (ui_px1 > ui_px0 && in_rect(x, y, ui_px0 - 10, ui_py - 18, ui_px1 - ui_px0 + 20, 44)) {
        if (playing_idx >= 0) {
            touch_scrub = 1;
            seek_from_touch(x, ui_px0, ui_px1);
        }
        return;
    }

    if (in_rc(x, y, rc_open_player) || in_rect(x, y, ui_cover_x, ui_cover_y, ui_cover_sz, ui_cover_sz)) {
        if (playing_idx >= 0) switch_page(UI_PLAYER);
        else if (nvisible > 0 && !loading) {
            int first = vis_at(0);
            if (first >= 0) { load_track(first); switch_page(UI_PLAYER); }
        }
        return;
    }

    if (nvisible > 0 && y >= L_TOP && y < L_BOT && x >= L_X0 && x < L_X0 + cardw) {
        int pos = (int)floorf(((float)(y - L_TOP) + scroll_px) / (float)L_ROW);
        if (pos >= 0 && pos < nvisible) {
            touch_press_idx = pos;
            sel = vis_at(pos);
            touch_drag = 1;
            touch_moved = 0;
            touch_drag_start_y = y;
            touch_last_y = y;
        }
    }
}

static void handle_touch_player(TouchInfo* t) {
    if (touch_scrub && t->held) {
        seek_from_touch(t->x, ui_px0, ui_px1);
        return;
    }
    if (touch_scrub && t->up) {
        touch_scrub = 0;
        return;
    }
    if (!t->down) return;

    int x = t->x, y = t->y;
    if (in_rc(x, y, rc_p_back)) {
        switch_page(UI_LIST);
        sel = playing_idx >= 0 ? playing_idx : sel;
        rebuild_visible();
        return;
    }
    if (handle_top_nav_touch(x, y)) return;
    if (handle_transport_touch(x, y)) return;

    if (in_rect(x, y, ui_px0 - 12, ui_py - 20, ui_px1 - ui_px0 + 24, 48)) {
        touch_scrub = 1;
        seek_from_touch(x, ui_px0, ui_px1);
        return;
    }
    if (in_rect(x, y, ui_cover_x, ui_cover_y, ui_cover_sz, ui_cover_sz)) {
        if (audio_paused()) audio_resume(); else audio_pause();
    }
}

// ---------- 主页绘制（现代掌机 MP4 双栏 / 竖屏全屏底座） ----------
static void draw_top_header(void) {
    if (!portrait()) {
        if (poster36) blit_rgba_round(screen, 24, 17, poster36, 32, 32, 8);
        draw_text(screen, font, 22, 64, 20, C_TXT, "Siren");

        char cnt[32];
        snprintf(cnt, sizeof(cnt), "%d 首", ntracks);
        draw_text(screen, font, 14, 64 + text_width(font, 22, "Siren") + 10, 26, C_TXT3, cnt);

        // 居中分段切换胶囊
        int sx = 240, sw = 408, sh = 38, sy = 14;
        fill_round_rect_aa(screen, sx, sy, sw, sh, 19, C_PANEL);
        int segw = (sw - 10) / 4;
        char fav_l[24];
        snprintf(fav_l, sizeof(fav_l), "喜欢 %d", fav_count);
        const char* sr_l = (view == VIEW_SEARCH && search_q[0]) ? "清除" : "搜索";
        const char* lbl[4] = { "全部", fav_l, sr_l, "扫描" };
        int act[4] = { view == VIEW_HOME, view == VIEW_FAVORITE, view == VIEW_SEARCH, 0 };
        Rect* rcs[4] = { &rc_tab_all, &rc_tab_fav, &rc_btn_search, &rc_btn_rescan };
        for (int i = 0; i < 4; i++) {
            int bx = sx + 5 + i * segw;
            if (act[i]) fill_round_rect_aa(screen, bx, sy + 4, segw, sh - 8, 15, C_ACCENT_BG);
            draw_text_centered(screen, font, 14, bx, bx + segw, sy + 11,
                               act[i] ? C_ACCENT_TXT : C_TXT2, lbl[i]);
            *rcs[i] = (Rect){ bx, sy + 4, segw, sh - 8 };
        }

        int rx = SCR_W - 24;
        int dw = text_width(font, 14, "自检") + 26;
        rx -= dw; rc_btn_diag = draw_pill_chip(rx, 15, 36, 14, "自检", 0);
        int ow = text_width(font, 14, "竖屏") + 26;
        rx -= ow + 8; rc_btn_ori = draw_pill_chip(rx, 15, 36, 14, "竖屏", 0);
        char th_l[32];
        snprintf(th_l, sizeof(th_l), "主题:%s", theme_names[cur_theme]);
        int tw = text_width(font, 14, th_l) + 26;
        rx -= tw + 8; rc_btn_theme = draw_pill_chip(rx, 15, 36, 14, th_l, 0);
    } else {
        if (poster36) blit_rgba_round(screen, 22, 18, poster36, 36, 36, 10);
        draw_text(screen, font, 24, 66, 22, C_TXT, "Siren");
        char cnt[32];
        snprintf(cnt, sizeof(cnt), "%d 首", nvisible);
        draw_text(screen, font, 15, 66 + text_width(font, 24, "Siren") + 10, 29, C_TXT3, cnt);

        int rx = SCR_W - 20;
        int dw = text_width(font, 14, "自检") + 24;
        rx -= dw; rc_btn_diag = draw_pill_chip(rx, 18, 36, 14, "自检", 0);
        int ow = text_width(font, 14, "横屏") + 24;
        rx -= ow + 8; rc_btn_ori = draw_pill_chip(rx, 18, 36, 14, "横屏", 0);
        int rw = text_width(font, 14, "扫描") + 24;
        rx -= rw + 8; rc_btn_rescan = draw_pill_chip(rx, 18, 36, 14, "扫描", 0);

        int sy = 70, sh = 46, sx = 20, sw = SCR_W - 40;
        fill_round_rect_aa(screen, sx, sy, sw, sh, 23, C_PANEL);
        int segw = (sw - 12) / 4;
        char fav_l[24], th_l[24];
        snprintf(fav_l, sizeof(fav_l), "喜欢 %d", fav_count);
        snprintf(th_l, sizeof(th_l), "主题:%s", theme_names[cur_theme]);
        const char* sr_l = (view == VIEW_SEARCH && search_q[0]) ? "清除" : "搜索";
        const char* lbl[4] = { "全部", fav_l, sr_l, th_l };
        int act[4] = { view == VIEW_HOME, view == VIEW_FAVORITE, view == VIEW_SEARCH, 0 };
        Rect* rcs[4] = { &rc_tab_all, &rc_tab_fav, &rc_btn_search, &rc_btn_theme };
        for (int i = 0; i < 4; i++) {
            int bx = sx + 6 + i * segw;
            if (act[i]) fill_round_rect_aa(screen, bx, sy + 4, segw, sh - 8, 19, C_ACCENT_BG);
            draw_text_centered(screen, font, 15, bx, bx + segw, sy + 14,
                               act[i] ? C_ACCENT_TXT : C_TXT2, lbl[i]);
            *rcs[i] = (Rect){ bx, sy + 4, segw, sh - 8 };
        }
    }
}

static void draw_land_mp4_deck(void) {
    int dx = 24, dy = 68, dw = 440, dh = SCR_H - 90;
    if (g_is_light) fill_round_rect_shadow(screen, dx, dy, dw, dh, 26, 22);
    fill_round_rect_aa(screen, dx, dy, dw, dh, 26, C_PANEL);

    int cx = dx + dw / 2;
    int csz = 244;
    int art_x = cx - csz / 2, art_y = dy + 26;
    ui_cover_x = art_x; ui_cover_y = art_y; ui_cover_sz = csz;

    fill_round_rect_shadow(screen, art_x, art_y, csz, csz, 22, 48);
    if (playing_idx >= 0 && cover244) {
        blit_rgba_round(screen, art_x, art_y, cover244, csz, csz, 22);
    } else if (poster244) {
        blit_rgba_round(screen, art_x, art_y, poster244, csz, csz, 22);
    }

    if (playing_idx >= 0) {
        // 封面右上角“全屏歌词”小胶囊
        int gx = art_x + csz - 84, gy = art_y + 10;
        fill_round_rect_aa_a(screen, gx, gy, 74, 26, 13, COL(0, 0, 0, 255), 145);
        draw_text_centered(screen, font, 12, gx, gx + 74, gy + 6, COL(255, 255, 255, 255), "全屏歌词 >");
        rc_open_player = (Rect){ dx + 20, art_y, dw - 40, csz + 76 };

        char ttl_line[256];
        snprintf(ttl_line, sizeof(ttl_line), "%s", p_title[0] ? p_title : "Siren");
        draw_text_centered(screen, font, 21, dx + 22, dx + dw - 22, art_y + csz + 18, C_TXT, ttl_line);

        char sub_line[300];
        snprintf(sub_line, sizeof(sub_line), "%s · %s", p_fmt[0] ? p_fmt : "AUDIO",
                 p_artist[0] ? p_artist : "未知艺术家");
        draw_text_centered(screen, font, 14, dx + 22, dx + dw - 22, art_y + csz + 48, C_TXT2, sub_line);

        double pos = audio_position();
        double dur = audio_duration();
        if (dur > 0 && pos > dur) pos = dur;
        draw_scrubber(dx + 30, dx + dw - 30, dy + 362, pos, dur, 7);

        draw_transport_row(cx, dy + 448, 72, 146, 62);

        // 底部单行实时歌词或操作提示条
        int lby = dy + dh - 56;
        fill_round_rect_aa(screen, dx + 20, lby, dw - 40, 38, 19, C_ELEV);
        if (lyr && lyr->n > 0) {
            int act = lyr->unsynced
                ? (dur > 0.0 && lyr->n > 1 ? (int)((pos / dur) * (double)(lyr->n - 1)) : 0)
                : lyrics_index(lyr, pos);
            if (act < 0) act = 0;
            if (act >= lyr->n) act = lyr->n - 1;
            draw_text_centered(screen, font, 14, dx + 34, dx + dw - 34, lby + 11, C_ACCENT_TXT, lyr->lines[act].text);
        } else {
            draw_text_centered(screen, font, 13, dx + 34, dx + dw - 34, lby + 12, C_TXT3,
                               "点封面进全屏 · 右摇杆调音量 · B 退出");
        }
    } else {
        rc_open_player = (Rect){ 0, 0, 0, 0 };
        draw_text_centered(screen, font, 21, dx + 24, dx + dw - 24, art_y + csz + 24, C_TXT, "Siren Portable");
        draw_text_centered(screen, font, 14, dx + 24, dx + dw - 24, art_y + csz + 56, C_TXT2,
                           "点击右侧曲目播放 · 支持 MP3/FLAC/OGG/WAV");
        draw_scrubber(dx + 30, dx + dw - 30, dy + 362, 0.0, 0.0, 7);
        draw_transport_row(cx, dy + 448, 72, 146, 62);

        int lby = dy + dh - 56;
        fill_round_rect_aa(screen, dx + 20, lby, dw - 40, 38, 19, C_ELEV);
        draw_text_centered(screen, font, 13, dx + 34, dx + dw - 34, lby + 12, C_TXT3,
                           "A 播放 · B 退出 · X 搜索 · ZL 收藏 · ZR 主题");
    }
}

static void draw_port_mini_deck(void) {
    int dx = 20, dw = SCR_W - 40, dh = 158, dy = SCR_H - dh - 14;
    if (g_is_light) fill_round_rect_shadow(screen, dx, dy, dw, dh, 26, 24);
    fill_round_rect_aa(screen, dx, dy, dw, dh, 26, C_PANEL);

    if (playing_idx >= 0) {
        int csz = 52;
        ui_cover_x = dx + 16; ui_cover_y = dy + 14; ui_cover_sz = csz;
        if (tracks[playing_idx].thumb)
            blit_rgba_round(screen, ui_cover_x, ui_cover_y, tracks[playing_idx].thumb, csz, csz, 12);

        draw_text_max(screen, font, 18, dx + 80, dy + 16, C_TXT, p_title[0] ? p_title : "Siren", dw - 210);
        draw_text_max(screen, font, 13, dx + 80, dy + 42, C_TXT2, p_artist[0] ? p_artist : p_fmt, dw - 210);

        int gx = dx + dw - 114, gy = dy + 22;
        fill_round_rect_aa(screen, gx, gy, 98, 32, 16, C_ACCENT_BG);
        draw_text_centered(screen, font, 13, gx, gx + 98, gy + 9, C_ACCENT_TXT, "歌词详情 >");
        rc_open_player = (Rect){ dx + 14, dy + 12, dw - 28, 56 };

        double pos = audio_position();
        double dur = audio_duration();
        if (dur > 0 && pos > dur) pos = dur;
        draw_scrubber(dx + 20, dx + dw - 20, dy + 78, pos, dur, 6);
        draw_transport_row(dx + dw / 2, dy + 126, 78, 160, 46);
    } else {
        ui_cover_x = ui_cover_y = ui_cover_sz = 0;
        rc_open_player = (Rect){ 0, 0, 0, 0 };
        ui_px0 = ui_px1 = ui_py = 0;
        const char* hint = ntracks == 0
            ? "未发现音频 · 点击“扫描”或按 A 键重扫 · B 退出"
            : "点击歌曲播放 · A 确认 · B 退出 · X 搜索";
        draw_text_centered(screen, font, 15, dx + 20, dx + dw - 20, dy + 32, C_TXT2, hint);
        draw_transport_row(dx + dw / 2, dy + 114, 78, 160, 46);
    }
}

static void draw_list(void) {
    canvas_clear(screen, C_BG);
    ensure_visible_thumbs();

    int cardw = L_X1 - L_X0;
    rc_empty_rescan = (Rect){ 0, 0, 0, 0 };

    if (nvisible == 0) {
        int bw = portrait() ? 580 : 620;
        int bh = 210;
        int bx = L_X0 + (cardw - bw) / 2;
        int by = L_TOP + (list_area_h() - bh) / 2;
        fill_round_rect_aa(screen, bx, by, bw, bh, 22, C_PANEL);

        const char* tm = "未在 SD 卡找到音频";
        const char* sm = "支持 MP3 / FLAC / OGG / WAV · 可放入 sdmc:/music";
        if (view == VIEW_FAVORITE && ntracks > 0) {
            tm = "暂无我喜欢的歌曲";
            sm = "点击右侧爱心或按 ZL 键即可收藏";
        } else if (view == VIEW_SEARCH && ntracks > 0) {
            tm = "未找到匹配歌曲";
            sm = "可点击“清除”返回全部曲库";
        }
        draw_text_centered(screen, font, 21, bx + 20, bx + bw - 20, by + 40, C_TXT, tm);
        draw_text_centered(screen, font, 14, bx + 20, bx + bw - 20, by + 78, C_TXT2, sm);

        int btn_w = 196, btn_h = 44;
        int btn_x = bx + (bw - btn_w) / 2, btn_y = by + 130;
        fill_round_rect_aa(screen, btn_x, btn_y, btn_w, btn_h, 22, C_ACCENT);
        draw_text_centered(screen, font, 15, btn_x, btn_x + btn_w, btn_y + 13, C_ON_ACCENT, "重新扫描 SD 卡");
        rc_empty_rescan = (Rect){ btn_x, btn_y, btn_w, btn_h };
    } else {
        int i0 = (int)floorf(scroll_px / (float)L_ROW) - 1;
        if (i0 < 0) i0 = 0;
        int i1 = (int)ceilf((scroll_px + (float)list_area_h()) / (float)L_ROW) + 1;
        if (i1 > nvisible) i1 = nvisible;

        int rad = portrait() ? 18 : 16;
        for (int i = i0; i < i1; i++) {
            int idx = vis_at(i);
            if (idx < 0) break;
            int y = L_TOP + i * L_ROW - (int)scroll_px;
            int pad_v = (L_ROW - L_CARD_H) / 2;
            int cy0 = y + pad_v;
            int ch = L_CARD_H;
            if (cy0 + ch <= L_TOP || cy0 >= L_BOT) continue;

            int is_playing = (idx == playing_idx);
            int is_sel = (idx == sel);
            int is_press = (touch_drag && touch_press_idx == i);

            uint32_t card_c = C_CARD;
            if (is_playing) card_c = C_ACCENT_BG;
            else if (is_press || is_sel) card_c = C_ELEV;
            fill_round_rect_aa(screen, L_X0, cy0, cardw - 10, ch, rad, card_c);

            if (is_playing) {
                fill_round_rect_aa(screen, L_X0 + 8, cy0 + ch / 2 - 12, 4, 24, 2, C_ACCENT);
            } else if (is_sel) {
                fill_round_rect_aa(screen, L_X0 + 8, cy0 + ch / 2 - 9, 4, 18, 2, C_TXT3);
            }

            int tx = L_X0 + 18;
            int ty = cy0 + (ch - 52) / 2;
            if (tracks[idx].thumb) blit_rgba_round(screen, tx, ty, tracks[idx].thumb, 52, 52, 11);

            char nm[256];
            basename_no_ext(tracks[idx].path, nm, sizeof(nm));
            const char* ttl = tracks[idx].meta.title[0] ? tracks[idx].meta.title : nm;
            const char* artist = tracks[idx].meta.artist[0] ? tracks[idx].meta.artist : "未知艺术家";
            uint32_t tc = is_playing ? C_ACCENT_TXT : C_TXT;
            uint32_t sc = is_playing ? C_ACCENT_TXT : C_TXT3;

            int txtx = tx + 66;
            int right_res = 66 + (is_playing ? 68 : 0);
            int maxw = (L_X0 + cardw - 10) - txtx - right_res;
            if (maxw < 80) maxw = 80;

            int block = L_TITLE + 6 + L_ART;
            int ty2 = cy0 + (ch - block) / 2 - 1;
            draw_text_max(screen, font, (float)L_TITLE, txtx, ty2, tc, ttl, maxw);

            char fmt[12];
            track_format_tag(tracks[idx].path, fmt, sizeof(fmt));
            char subline[300];
            snprintf(subline, sizeof(subline), "%s · %s", fmt, artist);
            draw_text_max(screen, font, (float)L_ART, txtx, ty2 + L_TITLE + 6, sc, subline, maxw);

            if (is_playing && audio_duration() > 0) {
                char dur[16];
                fmt_time(audio_duration(), dur, sizeof(dur));
                draw_text_right(screen, font, (float)L_ART, L_X1 - 62, cy0 + (ch - L_ART) / 2 - 1,
                                C_ACCENT_TXT, dur);
            }

            int hx = L_X1 - 44;
            int hy = cy0 + (ch - 18) / 2;
            icon_heart(screen, hx, hy, 18, tracks[idx].fav ? C_HEART : C_TRACK_BG);
        }

        float ms = max_scroll_px();
        if (ms > 0.0f) {
            int track_h = list_area_h() - 12;
            int thumb_h = (int)((float)list_area_h() / (float)(nvisible * L_ROW) * (float)track_h);
            if (thumb_h < 28) thumb_h = 28;
            if (thumb_h > track_h) thumb_h = track_h;
            int thumb_y = L_TOP + 6 + (int)((scroll_px / ms) * (float)(track_h - thumb_h));
            fill_round_rect_aa(screen, SCR_W - 14, thumb_y, 4, thumb_h, 2, C_TXT3);
        }
    }

    // 清理上下列表溢出区
    fill_rect(screen, L_X0, 0, SCR_W - L_X0, L_TOP, C_BG);
    fill_rect(screen, L_X0, L_BOT, SCR_W - L_X0, SCR_H - L_BOT, C_BG);

    if (!portrait()) {
        const char* qt = "播放队列";
        if (view == VIEW_FAVORITE) qt = "我喜欢的";
        else if (view == VIEW_SEARCH && search_q[0]) qt = "搜索结果";
        draw_text(screen, font, 20, L_X0 + 4, 74, C_TXT, qt);
        draw_text_right(screen, font, 13, L_X1 - 10, 79, C_TXT3, "A 播放 · B 退出 · X 搜索 · ZL 收藏");
        draw_land_mp4_deck();
    } else {
        draw_port_mini_deck();
    }

    draw_top_header();
}

static void ui_list_input(uint64_t down, uint64_t rep) {
    rebuild_visible();

    if (down & HidNpadButton_X) open_search_keyboard();
    if (down & HidNpadButton_ZR) cycle_theme();
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

    if (nvisible == 0) {
        if (down & HidNpadButton_A) scan_library(1);
        return;
    }

    int pos = sel_pos();
    if (rep & (HidNpadButton_Up | HidNpadButton_StickLUp)) {
        if (pos > 0) pos--;
        sel = vis_at(pos);
        ensure_pos_visible(pos);
    }
    if (rep & (HidNpadButton_Down | HidNpadButton_StickLDown)) {
        if (pos < nvisible - 1) pos++;
        sel = vis_at(pos);
        ensure_pos_visible(pos);
    }
    if (down & (HidNpadButton_L | HidNpadButton_Left)) {
        if (ntracks > 0 && !loading) {
            int p = pick_prev(playing_idx >= 0 ? playing_idx : sel);
            load_track(p); sel = p;
            ensure_pos_visible(sel_pos());
        }
    }
    if (down & (HidNpadButton_R | HidNpadButton_Right)) {
        if (ntracks > 0 && !loading) {
            int p = pick_next(playing_idx >= 0 ? playing_idx : sel, 1);
            load_track(p); sel = p;
            ensure_pos_visible(sel_pos());
        }
    }
    if ((down & HidNpadButton_A) && !loading) {
        int i = vis_at(pos);
        if (i >= 0) { load_track(i); switch_page(UI_PLAYER); }
    }
    if (down & HidNpadButton_ZL) {
        int i = vis_at(pos);
        if (i >= 0) toggle_fav(i);
    }
}

// ---------- 全屏播放页绘制 ----------
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
        int idxs2[8];
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
                    fill_round_rect_aa_a(screen, x0 + 24, y - 5, rw - 48, 42, 16, C_ACCENT_BG, 210);
                    draw_text_centered_max(screen, font, 25, x0 + 36, x1 - 36, y + 3, C_ACCENT_TXT, lyr->lines[i].text);
                } else {
                    fill_round_rect_aa_a(screen, x0, y - 5, rw, 42, 14, C_ACCENT_BG, 200);
                    fill_round_rect_aa(screen, x0 + 10, y + 4, 4, 24, 2, C_ACCENT);
                    draw_text_max(screen, font, 24, x0 + 24, y + 3, C_ACCENT_TXT, lyr->lines[i].text, rw - 36);
                }
                y += 52;
            } else {
                float px = centered
                    ? ((dist == -1 || dist == 1) ? 20.0f : 17.0f)
                    : ((dist == -1 || dist == 1) ? 19.0f : 16.0f);
                uint32_t cc = (dist == -1 || dist == 1) ? C_TXT : C_TXT2;
                if (centered) draw_text_centered_max(screen, font, px, x0, x1, y, cc, lyr->lines[i].text);
                else draw_text_max(screen, font, px, x0 + 14, y, cc, lyr->lines[i].text, rw - 24);
                y += (int)px + 21;
            }
        }
        if (lyr->unsynced) {
            const char* uh = "静态歌词（内嵌文本 · 按播放进度滚动）";
            if (centered) draw_text_centered(screen, font, 14, x0, x1, spec_hint_y, C_TXT2, uh);
            else draw_text(screen, font, 14, x0 + 14, spec_hint_y, C_TXT2, uh);
        }
    } else {
        float sp[28];
        audio_get_spectrum(sp);
        for (int i = 0; i < 28; i++) bars[i] = sp[i];

        int n = portrait() ? 18 : 22;
        int bw = 14;
        int span = rw - (centered ? 80 : 32);
        int gap = (span - n * bw) / (n - 1);
        if (gap < 4) gap = 4;
        int total_w = n * bw + (n - 1) * gap;
        int bx0 = x0 + (centered ? (rw - total_w) / 2 : 0);
        int max_h = portrait() ? 200 : 170;
        for (int i = 0; i < n; i++) {
            float v = clampf(bars[i], 0.04f, 1.0f);
            int hgt = (int)(v * max_h);
            int bx = bx0 + i * (bw + gap);
            fill_round_rect_aa_a(screen, bx, spec_base - hgt, bw, hgt, bw / 2, C_ACCENT, 215);
        }
        const char* sh = "未找到歌词 · 支持同名 .lrc 及 MP3/FLAC/OGG 内嵌歌词";
        if (centered) draw_text_centered(screen, font, 15, x0, x1, spec_hint_y, C_TXT2, sh);
        else draw_text(screen, font, 15, x0, spec_hint_y, C_TXT2, sh);
    }
}

static void draw_player(void) {
    if (bg) memcpy(screen->px, bg->px, (size_t)SCR_W * SCR_H * 4);
    else canvas_clear(screen, C_BG);
    fill_rect_a(screen, 0, 0, SCR_W, SCR_H, C_BG, g_is_light ? 215 : 165);

    fill_round_rect_aa(screen, 26, 20, 96, 36, 18, C_ELEV);
    icon_back_arrow(screen, 38, 30, 16, C_TXT);
    draw_text(screen, font, 15, 62, 29, C_TXT, "返回");
    rc_p_back = (Rect){ 26, 20, 96, 36 };
    rc_tab_all = rc_tab_fav = rc_btn_search = rc_btn_rescan = rc_empty_rescan = rc_open_player = (Rect){ 0, 0, 0, 0 };

    char top_info[64];
    snprintf(top_info, sizeof(top_info), "%s · %d/%d", p_fmt[0] ? p_fmt : "AUDIO",
             playing_idx >= 0 ? playing_idx + 1 : 0, ntracks);
    if (portrait()) draw_text_centered(screen, font, 15, 130, SCR_W - 292, 29, C_TXT2, top_info);
    else            draw_text_centered(screen, font, 15, 292, SCR_W - 292, 29, C_TXT2, top_info);

    int rx = SCR_W - 26;
    int dw = text_width(font, 14, "自检") + 26;
    rx -= dw; rc_btn_diag = draw_pill_chip(rx, 20, 36, 14, "自检", 0);
    int ow = text_width(font, 14, portrait() ? "横屏" : "竖屏") + 26;
    rx -= ow + 8; rc_btn_ori = draw_pill_chip(rx, 20, 36, 14, portrait() ? "横屏" : "竖屏", 0);
    char th_l[24];
    snprintf(th_l, sizeof(th_l), "主题:%s", theme_names[cur_theme]);
    int tw = text_width(font, 14, th_l) + 26;
    rx -= tw + 8; rc_btn_theme = draw_pill_chip(rx, 20, 36, 14, th_l, 0);

    double pos = audio_position();
    double dur = audio_duration();
    if (dur > 0 && pos > dur) pos = dur;

    if (!portrait()) {
        int csz = 340;
        int cx = 92, cy = 104;
        ui_cover_x = cx; ui_cover_y = cy; ui_cover_sz = csz;

        fill_round_rect_shadow(screen, cx, cy, csz, csz, 26, 56);
        if (cover) blit_rgba_round(screen, cx, cy, cover, csz, csz, 26);

        int col_x = 478, col_x1 = SCR_W - 64;
        int col_w = col_x1 - col_x;
        float tpx = text_width(font, 34, p_title) > col_w ? 28.0f : 34.0f;
        draw_text_max(screen, font, tpx, col_x, 108, C_TXT, p_title, col_w);
        draw_text_max(screen, font, 18, col_x, 156, C_TXT2, p_artist[0] ? p_artist : "未知艺术家", col_w);
        fill_round_rect_aa(screen, col_x, 192, 44, 4, 2, C_ACCENT);

        draw_lyric_or_spectrum(col_x, col_x1, 222, 480, 456, 480, pos, dur, 0);

        draw_scrubber(92, SCR_W - 92, 536, pos, dur, 8);
        draw_transport_row(SCR_W / 2, 634, 92, 184, 66);
    } else {
        int csz = 340;
        int cx = (SCR_W - csz) / 2, cy = 92;
        ui_cover_x = cx; ui_cover_y = cy; ui_cover_sz = csz;

        fill_round_rect_shadow(screen, cx, cy, csz, csz, 26, 56);
        if (cover) blit_rgba_round(screen, cx, cy, cover, csz, csz, 26);

        float tpx = text_width(font, 30, p_title) > (SCR_W - 80) ? 24.0f : 30.0f;
        draw_text_centered_max(screen, font, tpx, 40, SCR_W - 40, cy + csz + 24, C_TXT, p_title);
        draw_text_centered_max(screen, font, 18, 40, SCR_W - 40, cy + csz + 68, C_TXT2,
                               p_artist[0] ? p_artist : "未知艺术家");

        draw_lyric_or_spectrum(40, SCR_W - 40, cy + csz + 120, 956, 916, 950, pos, dur, 1);

        draw_scrubber(52, SCR_W - 52, SCR_H - 224, pos, dur, 8);
        draw_transport_row(SCR_W / 2, SCR_H - 122, 96, 192, 66);
    }

    if (audio_status() == 3) {
        draw_text_centered(screen, font, 22, 0, SCR_W, SCR_H / 2 - 40, C_HEART, "解码失败：文件不支持或损坏");
    }
    if (loading)
        draw_text_centered(screen, font, 22, 0, SCR_W, SCR_H / 2, C_TXT2, "加载中…");
}

static void draw_volume_overlay(void) {
    if (vol_overlay_timer <= 0) return;
    vol_overlay_timer--;
    char vs[64];
    snprintf(vs, sizeof(vs), "音量 %d%%", (int)(audio_volume() * 100 + 0.5f));
    int bw2 = 250, bh2 = 70;
    int bx = (SCR_W - bw2) / 2, by = 82;
    fill_round_rect_shadow(screen, bx, by, bw2, bh2, 20, 48);
    fill_round_rect_aa(screen, bx, by, bw2, bh2, 20, C_PANEL);
    icon_speaker(screen, bx + 24, by + 16, 20, C_ACCENT);
    draw_text_centered(screen, font, 18, bx + 20, bx + bw2, by + 14, C_TXT, vs);
    fill_round_rect_aa(screen, bx + 24, by + 48, bw2 - 48, 6, 3, C_TRACK_BG);
    fill_round_rect_aa(screen, bx + 24, by + 48, (int)((bw2 - 48) * audio_volume()), 6, 3, C_ACCENT);
}

static void draw_diag(void) {
    fill_rect_a(screen, 0, 0, SCR_W, SCR_H, COL(0, 0, 0, 255), 175);
    int cw = portrait() ? 640 : 860;
    int ch = 460;
    int cx = (SCR_W - cw) / 2;
    int cy = (SCR_H - ch) / 2;
    fill_round_rect_shadow(screen, cx, cy, cw, ch, 26, 64);
    fill_round_rect_aa(screen, cx, cy, cw, ch, 26, C_PANEL);

    draw_text(screen, font, 24, cx + 32, cy + 30, C_ACCENT_TXT, "Siren v1.2.1 系统与音频自检");
    draw_text_right(screen, font, 14, cx + cw - 32, cy + 38, C_TXT3, "点击或按 B/Y 关闭");

    char buf[1024];
    audio_get_diag(buf, sizeof(buf));
    int yy = cy + 86;
    char* line = strtok(buf, "\n");
    while (line && yy < cy + ch - 130) {
        draw_text(screen, font, 18, cx + 32, yy, C_TXT, line);
        yy += 34;
        line = strtok(NULL, "\n");
    }
    char extra[256];
    snprintf(extra, sizeof(extra), "曲库：%d 首   收藏：%d 首   模式：%s   主题：%s",
             ntracks, fav_count, mode_names[play_mode], theme_names[cur_theme]);
    draw_text_max(screen, font, 17, cx + 32, yy + 10, C_TXT2, extra, cw - 64);

    char lyr_info[256];
    if (lyr) snprintf(lyr_info, sizeof(lyr_info), "歌词：已加载 %d 行 (%s)", lyr->n, lyr->source);
    else snprintf(lyr_info, sizeof(lyr_info), "歌词：未匹配 (支持同名 .lrc 与 MP3/FLAC/OGG 内嵌标签)");
    draw_text_max(screen, font, 16, cx + 32, yy + 42, C_TXT2, lyr_info, cw - 64);

    draw_text_max(screen, font, 14, cx + 32, cy + ch - 44, C_TXT3,
                  "提示：主页按 B 键直接退出，按 − 一键切换单向竖屏，按 ZR 切换主题", cw - 64);
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
        cycle_theme();
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
    scan_library(0);

    build_poster_thumbs();
    memset(&meta, 0, sizeof(meta));

    int stride = 0;
    while (be_should_run()) {
        frame_begin();
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
        if (down & HidNpadButton_Minus) toggle_orientation();

        if (!ori_locked && g_six_ok) {
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

        if ((touch_now.held || touch_now.up) && ori == ORI_PORT) {
            int nx = 719 - touch_now.y;
            int ny = touch_now.x;
            touch_now.x = nx; touch_now.y = ny;
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

        uint32_t* fb = be_begin(&stride);
        if (ui_state == UI_LIST) draw_list();
        else draw_player();

        draw_volume_overlay();

        if (toast_timer > 0) {
            toast_timer--;
            int tw = text_width(font, 15, toast_text) + 46;
            if (tw > SCR_W - 80) tw = SCR_W - 80;
            int bx = (SCR_W - tw) / 2;
            fill_round_rect_shadow(screen, bx, 20, tw, 42, 21, 48);
            fill_round_rect_aa(screen, bx, 20, tw, 42, 21, C_ELEV);
            draw_text_centered(screen, font, 15, bx, bx + tw, 33, C_TXT, toast_text);
        }

        // 仅在切换页面时执行 5 帧轻量交叉淡化过渡
        if (trans_frames > 0 && trans_shot && trans_shot->w == SCR_W && trans_shot->h == SCR_H) {
            uint8_t a = (uint8_t)(trans_frames * 255 / (TRANS_FRAMES + 1));
            blit_rgba_a(screen, 0, 0, trans_shot->px, SCR_W, SCR_H, a);
            trans_frames--;
        }

        if (show_diag) draw_diag();

        if (ori == ORI_LAND) {
            for (int y = 0; y < SCR_H; y++) {
                uint32_t* dst = fb + (size_t)y * stride;
                uint32_t* src = screen->px + (size_t)y * SCR_W;
                memcpy(dst, src, (size_t)SCR_W * 4);
            }
        } else {
            for (int y = 0; y < 720; y++) {
                uint32_t* d = fb + (size_t)y * stride;
                int px = 719 - y;
                for (int x = 0; x < 1280; x++)
                    d[x] = screen->px[(size_t)x * 720 + px];
            }
        }
        be_end();
    }

    audio_exit();
    be_exit();
    if (font) font_free(font);
    if (screen) canvas_free(screen);
    if (bg) canvas_free(bg);
    if (cover) free(cover);
    if (cover244) free(cover244);
    if (trans_shot) canvas_free(trans_shot);
    if (poster36) free(poster36);
    if (poster52) free(poster52);
    if (poster244) free(poster244);
    meta_clear(&meta);
    if (lyr) lyrics_free(lyr);
    for (int i = 0; i < ntracks; i++) {
        free(tracks[i].thumb);
        meta_clear(&tracks[i].meta);
    }
    free(tracks);
    return 0;
}
