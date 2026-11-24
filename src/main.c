#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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

// ---------- 屏幕方向（仅单向竖屏 + 横屏） ----------
#define ORI_LAND 0   // 横屏 1280x720
#define ORI_PORT 1   // 单向竖屏 720x1280（顺时针90度手持，iPod竖持比例）
static int ori_locked = 0;   // 0=自动陀螺仪检测，1=手动锁定切换
static int ori = ORI_LAND;
static int SCR_W = 1280, SCR_H = 720;
static int portrait(void) { return ori == ORI_PORT; }

// ---------- LCD 与转盘几何布局 ----------
static int LCD_X, LCD_Y, LCD_W, LCD_H;
static int L_X0, L_X1, L_TOP, L_BAR, L_ROW, L_CARD_H, L_TITLE, L_ART;
static int W_CX, W_CY, W_RO, W_RI;

static void update_layout(void) {
    if (portrait()) {
        LCD_X = 28; LCD_Y = 26; LCD_W = 664; LCD_H = 612;
        L_X0 = LCD_X + 12;
        L_X1 = LCD_X + LCD_W - 12;
        L_TOP = LCD_Y + 90;
        L_BAR = SCR_H - (LCD_Y + LCD_H - 56);
        L_ROW = 66;
        L_CARD_H = 60;
        L_TITLE = 19;
        L_ART = 14;
        W_CX = SCR_W / 2;
        W_CY = 964;
        W_RO = 224;
        W_RI = 82;
    } else {
        LCD_X = 28; LCD_Y = 28; LCD_W = 818; LCD_H = 664;
        L_X0 = LCD_X + 12;
        L_X1 = LCD_X + LCD_W - 12;
        L_TOP = LCD_Y + 54;
        L_BAR = SCR_H - (LCD_Y + LCD_H - 58);
        L_ROW = 64;
        L_CARD_H = 58;
        L_TITLE = 19;
        L_ART = 13;
        W_CX = 1066;
        W_CY = 388;
        W_RO = 172;
        W_RI = 64;
    }
}

static int list_bottom_y(void) { return LCD_Y + LCD_H - 60; }

static int list_area_h(void) {
    int h = list_bottom_y() - L_TOP;
    return h < L_ROW ? L_ROW : h;
}

static int rows_visible(void) {
    int r = list_area_h() / L_ROW;
    return r < 1 ? 1 : r;
}

// ---------- iPod / MP4 硬件涂装主题 ----------
#define THEME_SILVER   0
#define THEME_OBSIDIAN 1
#define THEME_TEAL     2
#define THEME_COUNT    3

static uint32_t C_BODY, C_BEZEL, C_LCD, C_HDR_TOP, C_HDR_BOT, C_SUBBAR;
static uint32_t C_PRIM_TOP, C_PRIM_BOT, C_PRIM_TXT, C_ON_PRIM, C_PLAY_TINT;
static uint32_t C_WHEEL, C_WHEEL_RING, C_WHEEL_CENTER, C_WHEEL_TXT;
static uint32_t C_BTN, C_TXT, C_TXT2, C_TXT3, C_LINE, C_HEART;

typedef struct {
    uint32_t body, bezel, lcd, hdr_top, hdr_bot, subbar;
    uint32_t prim_top, prim_bot, prim_txt, on_prim, play_tint;
    uint32_t wheel, wheel_ring, wheel_center, wheel_txt;
    uint32_t btn, txt, txt2, txt3, line;
} Theme;

static const Theme themes[THEME_COUNT] = {
    // 经典银白（iPod Classic Silver & Aqua Blue）
    { COL(0xD8,0xDC,0xE4,0xFF), COL(0x1E,0x22,0x2A,0xFF), COL(0xF8,0xFA,0xFC,0xFF),
      COL(0xEC,0xF0,0xF5,0xFF), COL(0xD5,0xDC,0xE7,0xFF), COL(0xEC,0xF0,0xF6,0xFF),
      COL(0x2B,0x85,0xFA,0xFF), COL(0x10,0x55,0xC4,0xFF), COL(0x0E,0x52,0xBE,0xFF),
      COL(0xFF,0xFF,0xFF,0xFF), COL(0xE1,0xEC,0xFC,0xFF),
      COL(0xF5,0xF7,0xFA,0xFF), COL(0xB4,0xBC,0xCA,0xFF), COL(0xE0,0xE5,0xEE,0xFF),
      COL(0x2A,0x31,0x3E,0xFF),
      COL(0xE6,0xEA,0xF0,0xFF), COL(0x10,0x13,0x18,0xFF), COL(0x30,0x38,0x46,0xFF),
      COL(0x52,0x5B,0x6C,0xFF), COL(0xD8,0xDE,0xE8,0xFF) },
    // 暗夜黑胶（iPod Classic Obsidian）
    { COL(0x18,0x1B,0x22,0xFF), COL(0x2A,0x30,0x3C,0xFF), COL(0x0E,0x11,0x17,0xFF),
      COL(0x1E,0x23,0x2E,0xFF), COL(0x14,0x18,0x20,0xFF), COL(0x15,0x19,0x22,0xFF),
      COL(0x36,0x8B,0xFF,0xFF), COL(0x18,0x5C,0xCC,0xFF), COL(0x63,0xA8,0xFF,0xFF),
      COL(0xFF,0xFF,0xFF,0xFF), COL(0x18,0x2B,0x47,0xFF),
      COL(0x23,0x28,0x33,0xFF), COL(0x39,0x41,0x52,0xFF), COL(0x18,0x1C,0x24,0xFF),
      COL(0xD8,0xE0,0xEE,0xFF),
      COL(0x24,0x29,0x35,0xFF), COL(0xF4,0xF7,0xFC,0xFF), COL(0xBC,0xC6,0xD6,0xFF),
      COL(0x88,0x94,0xA8,0xFF), COL(0x25,0x2C,0x38,0xFF) },
    // 青碧 MP4（Walkman Teal）
    { COL(0x13,0x1D,0x1C,0xFF), COL(0x22,0x32,0x2F,0xFF), COL(0x0B,0x13,0x12,0xFF),
      COL(0x17,0x25,0x23,0xFF), COL(0x10,0x1A,0x18,0xFF), COL(0x12,0x1D,0x1B,0xFF),
      COL(0x38,0xC6,0xAD,0xFF), COL(0x1B,0x89,0x76,0xFF), COL(0x63,0xDF,0xC9,0xFF),
      COL(0xFF,0xFF,0xFF,0xFF), COL(0x16,0x39,0x33,0xFF),
      COL(0x1C,0x28,0x26,0xFF), COL(0x30,0x44,0x40,0xFF), COL(0x14,0x1D,0x1B,0xFF),
      COL(0xD0,0xEA,0xE4,0xFF),
      COL(0x1E,0x2B,0x29,0xFF), COL(0xF0,0xF8,0xF6,0xFF), COL(0xBA,0xCF,0xCA,0xFF),
      COL(0x86,0x9E,0x99,0xFF), COL(0x1F,0x2E,0x2B,0xFF) }
};

static const char* theme_names[THEME_COUNT] = { "经典银", "暗夜黑", "青碧" };
static int cur_theme = THEME_SILVER;

static void apply_theme(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= THEME_COUNT) idx = THEME_COUNT - 1;
    cur_theme = idx;
    const Theme* t = &themes[idx];
    C_BODY = t->body; C_BEZEL = t->bezel; C_LCD = t->lcd;
    C_HDR_TOP = t->hdr_top; C_HDR_BOT = t->hdr_bot; C_SUBBAR = t->subbar;
    C_PRIM_TOP = t->prim_top; C_PRIM_BOT = t->prim_bot; C_PRIM_TXT = t->prim_txt;
    C_ON_PRIM = t->on_prim; C_PLAY_TINT = t->play_tint;
    C_WHEEL = t->wheel; C_WHEEL_RING = t->wheel_ring;
    C_WHEEL_CENTER = t->wheel_center; C_WHEEL_TXT = t->wheel_txt;
    C_BTN = t->btn; C_TXT = t->txt; C_TXT2 = t->txt2; C_TXT3 = t->txt3; C_LINE = t->line;
    C_HEART = COL(0xE0, 0x3E, 0x62, 0xFF);
}

typedef struct {
    char path[600];
    TrackMeta meta;
    int meta_state;     // 0 = 未加载, 1 = 已加载
    uint32_t* thumb;    // 52x52 缩略图
    uint32_t* thumb84;  // 84x84 缩略图
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

// 播放页资源
static TrackMeta meta;
static Lyrics* lyr = NULL;
static uint32_t* cover = NULL;       // 360x360 当前曲目大图
static uint32_t* cover240 = NULL;    // 240x240 横屏 LCD 封面
static uint32_t* cover176 = NULL;    // 176x176 竖屏 LCD 封面
static uint32_t* poster24 = NULL;
static uint32_t* poster36 = NULL;
static uint32_t* poster52 = NULL;
static uint32_t* poster84 = NULL;
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

// 播放页进度条交互区
static int ui_px0, ui_px1, ui_py;
static int ui_cover_x, ui_cover_y, ui_cover_sz;

// 触摸状态（LCD + Click Wheel 转盘）
static TouchInfo touch_now;
static int touch_drag = 0;
static int touch_moved = 0;
static int touch_scrub = 0;
static int touch_drag_start_y = 0;
static int touch_last_y = 0;
static int touch_press_idx = -1;

// Click Wheel 转盘触控状态
static int wheel_active = 0;
static int wheel_moved = 0;
static int wheel_down_zone = -1; // -1=无, 0=中键, 1=上MENU, 2=右NEXT, 3=下PLAY, 4=左PREV
static float wheel_last_ang = 0.0f;
static float wheel_accum = 0.0f;

static char search_q[128] = "";
static int fav_count = 0;

// 可点击区域
typedef struct { int x, y, w, h; } Rect;
static Rect rc_tab_all, rc_tab_fav, rc_btn_search, rc_btn_rescan;
static Rect rc_btn_theme, rc_btn_ori, rc_btn_diag, rc_btn_mode, rc_btn_fav_p;
static Rect rc_empty_rescan, rc_mini_bar, rc_pod_card, rc_p_back;

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
    int t = THEME_SILVER, pm = MODE_LOOP_ALL;
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

// ---------- 六轴加速度计（仅单向竖屏自动旋转） ----------
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
    wheel_active = 0;
    wheel_down_zone = -1;
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
    if (bar_w < 2) bar_w = 2;
    fill_rect(c, x, y + 1, bar_w, s - 2, col);
    fill_tri_aa(c, x + s / 2 + 1, y + 1, x + s / 2 + 1, y + s - 1, x + bar_w + 1, y + s / 2, col);
    fill_tri_aa(c, x + s - 1, y + 1, x + s - 1, y + s - 1, x + s / 2 + 1, y + s / 2, col);
}

static void icon_next(Canvas* c, int x, int y, int s, uint32_t col) {
    int bar_w = s / 6;
    if (bar_w < 2) bar_w = 2;
    fill_tri_aa(c, x + 1, y + 1, x + s / 2 - 1, y + s / 2, x + 1, y + s - 1, col);
    fill_tri_aa(c, x + s / 2 - 1, y + 1, x + s - bar_w - 1, y + s / 2, x + s / 2 - 1, y + s - 1, col);
    fill_rect(c, x + s - bar_w, y + 1, bar_w, s - 2, col);
}

static void icon_play_pause(Canvas* c, int x, int y, int s, uint32_t col) {
    fill_tri_aa(c, x, y + 1, x + s * 7 / 15, y + s / 2, x, y + s - 1, col);
    int pw = s / 7; if (pw < 2) pw = 2;
    fill_rect(c, x + s * 9 / 15, y + 2, pw, s - 4, col);
    fill_rect(c, x + s * 12 / 15, y + 2, pw, s - 4, col);
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

static void icon_chevron_right(Canvas* c, int x, int y, int s, uint32_t col) {
    fill_tri_aa(c, x + 2, y + 1, x + s - 2, y + s / 2, x + 2, y + s - 1, col);
}

static void icon_back_arrow(Canvas* c, int x, int y, int s, uint32_t col) {
    fill_tri_aa(c, x + s * 2 / 3, y + 2, x + s * 2 / 3, y + s - 2, x + 2, y + s / 2, col);
}

static void fill_round_grad_v(Canvas* c, int x, int y, int w, int h, int rad, uint32_t top, uint32_t bot) {
    if (w <= 0 || h <= 0) return;
    fill_round_rect_aa(c, x, y, w, h, rad, bot);
    int tr = top & 0xFF, tg = (top >> 8) & 0xFF, tb = (top >> 16) & 0xFF;
    int br = bot & 0xFF, bg = (bot >> 8) & 0xFF, bb = (bot >> 16) & 0xFF;
    int denom = h > 1 ? h - 1 : 1;
    for (int j = 0; j < h; j++) {
        int ty = y + j;
        if (ty < 0 || ty >= c->h) continue;
        int inset = 0;
        if (j < rad) {
            float dy = (float)(rad - j);
            inset = rad - (int)sqrtf((float)rad * rad - dy * dy);
        } else if (j >= h - rad) {
            float dy = (float)(j - (h - rad - 1));
            inset = rad - (int)sqrtf((float)rad * rad - dy * dy);
        }
        int x0 = x + inset + 1;
        int len = w - 2 * (inset + 1);
        if (len <= 0) continue;
        uint8_t r = (uint8_t)(tr + (br - tr) * j / denom);
        uint8_t g = (uint8_t)(tg + (bg - tg) * j / denom);
        uint8_t b = (uint8_t)(tb + (bb - tb) * j / denom);
        hline(c, x0, ty, len, COL(r, g, b, 255));
    }
}

static void draw_cover_reflection(int cx, int cy, const uint32_t* art, int sz, int refl_h, uint32_t bg_c) {
    if (!art || refl_h <= 0) return;
    uint8_t br = bg_c & 0xFF, bg = (bg_c >> 8) & 0xFF, bb = (bg_c >> 16) & 0xFF;
    for (int j = 0; j < refl_h; j++) {
        int sy = sz - 1 - j;
        int dy = cy + sz + 3 + j;
        if (sy < 0 || dy >= screen->h) break;
        float alpha = 0.25f * (1.0f - (float)j / (float)refl_h) * (1.0f - (float)j / (float)refl_h);
        for (int i = 6; i < sz - 6; i++) {
            int dx = cx + i;
            if (dx < 0 || dx >= screen->w) continue;
            uint32_t s = art[(size_t)sy * sz + i];
            uint8_t sr = s & 0xFF, sg = (s >> 8) & 0xFF, sb = (s >> 16) & 0xFF;
            uint8_t r = (uint8_t)(br + (sr - br) * alpha);
            uint8_t g = (uint8_t)(bg + (sg - bg) * alpha);
            uint8_t b = (uint8_t)(bb + (sb - bb) * alpha);
            screen->px[(size_t)dy * screen->w + dx] = COL(r, g, b, 255);
        }
    }
}

// ---------- 默认封面与海报 ----------
static void make_default_art(uint32_t* dst, int n, const char* seed) {
    uint32_t h = hash_str(seed);
    uint8_t r, g, b;
    hsv2rgb((float)((h % 360) / 360.0), 0.45f, 0.34f, &r, &g, &b);
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            float t = (x + y) / (float)(2 * n);
            dst[(size_t)y * n + x] = COL((uint8_t)(r * (1 - t) + 24 * t),
                                         (uint8_t)(g * (1 - t) + 26 * t),
                                         (uint8_t)(b * (1 - t) + 32 * t), 255);
        }
    }
    Canvas c = { n, n, dst };
    draw_note(&c, n / 6, n / 6, n * 2 / 3, COL(255, 255, 255, 215));
}

static void build_poster_thumbs(void) {
    size_t poster_len = (size_t)(poster_end - poster_start);
    if (poster_len < 320 * 320 * 4) return;
    poster24 = (uint32_t*)calloc(26 * 26, 4);
    poster36 = (uint32_t*)calloc(36 * 36, 4);
    poster52 = (uint32_t*)calloc(52 * 52, 4);
    poster84 = (uint32_t*)calloc(84 * 84, 4);
    if (poster24) resample_bilinear((const uint32_t*)poster_start, 320, 320, poster24, 26, 26);
    if (poster36) resample_bilinear((const uint32_t*)poster_start, 320, 320, poster36, 36, 36);
    if (poster52) resample_bilinear((const uint32_t*)poster_start, 320, 320, poster52, 52, 52);
    if (poster84) resample_bilinear((const uint32_t*)poster_start, 320, 320, poster84, 84, 84);
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
        free(tracks[i].thumb84);
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
static void sync_player_covers(void) {
    if (!cover) return;
    if (!cover240) cover240 = (uint32_t*)calloc(240 * 240, 4);
    if (!cover176) cover176 = (uint32_t*)calloc(176 * 176, 4);
    if (cover240) resample_bilinear(cover, 360, 360, cover240, 240, 240);
    if (cover176) resample_bilinear(cover, 360, 360, cover176, 176, 176);
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

    if (!cover) cover = (uint32_t*)calloc((size_t)360 * 360, 4);
    if (cover) {
        if (meta.art && meta.art_w > 8 && meta.art_h > 8) {
            resample_bilinear((const uint32_t*)meta.art, meta.art_w, meta.art_h, cover, 360, 360);
        } else if ((size_t)(poster_end - poster_start) >= 320 * 320 * 4) {
            resample_bilinear((const uint32_t*)poster_start, 320, 320, cover, 360, 360);
        } else {
            make_default_art(cover, 360, tracks[idx].path);
        }
        sync_player_covers();
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
    snprintf(msg, sizeof(msg), "涂装：%s", theme_names[cur_theme]);
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
    show_toast(ori == ORI_PORT ? "已切换：竖屏 iPod 模式" : "已切换：横屏 MP4 模式");
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

// ---------- Click Wheel 转盘触发与滚动 ----------
static void trigger_wheel_zone(int zone) {
    if (zone == 0) {
        // 中心确认键
        if (ui_state == UI_LIST) {
            if (nvisible > 0 && !loading) {
                int i = sel >= 0 ? sel : vis_at(0);
                if (i >= 0) { load_track(i); switch_page(UI_PLAYER); }
            } else {
                scan_library(1);
            }
        } else {
            if (playing_idx >= 0) {
                if (audio_paused()) audio_resume(); else audio_pause();
            }
        }
    } else if (zone == 1) {
        // 顶部 MENU 键
        if (ui_state == UI_PLAYER) {
            switch_page(UI_LIST);
            sel = playing_idx >= 0 ? playing_idx : sel;
            rebuild_visible();
        } else if (view != VIEW_HOME) {
            search_q[0] = 0;
            set_view(VIEW_HOME);
        } else {
            set_view(VIEW_FAVORITE);
        }
    } else if (zone == 2) {
        // 右侧 下一曲
        if (ntracks > 0 && !loading) {
            int p = pick_next(playing_idx >= 0 ? playing_idx : sel, 1);
            load_track(p);
            sel = p;
            if (ui_state == UI_LIST) ensure_pos_visible(sel_pos());
        }
    } else if (zone == 3) {
        // 底部 播放/暂停
        if (playing_idx >= 0) {
            if (audio_paused()) audio_resume(); else audio_pause();
        } else if (nvisible > 0 && !loading) {
            int i = sel >= 0 ? sel : vis_at(0);
            if (i >= 0) { load_track(i); sel = i; }
        }
    } else if (zone == 4) {
        // 左侧 上一曲
        if (ntracks > 0 && !loading) {
            int p = pick_prev(playing_idx >= 0 ? playing_idx : sel);
            load_track(p);
            sel = p;
            if (ui_state == UI_LIST) ensure_pos_visible(sel_pos());
        }
    }
}

static int handle_wheel_touch(TouchInfo* t) {
    int dx = t->x - W_CX;
    int dy = t->y - W_CY;
    int d2 = dx * dx + dy * dy;

    if (wheel_active && t->held) {
        if (wheel_down_zone > 0 && d2 >= (W_RI - 8) * (W_RI - 8) && d2 <= (W_RO + 28) * (W_RO + 28)) {
            float ang = atan2f((float)dy, (float)dx);
            float da = ang - wheel_last_ang;
            while (da >  (float)M_PI) da -= 2.0f * (float)M_PI;
            while (da < -(float)M_PI) da += 2.0f * (float)M_PI;
            wheel_last_ang = ang;
            wheel_accum += da;
            if (fabsf(wheel_accum) > 0.22f) wheel_moved = 1;

            const float step = 0.28f;
            while (wheel_accum >= step) {
                wheel_accum -= step;
                if (ui_state == UI_LIST && nvisible > 0) {
                    int pos = sel_pos();
                    if (pos < nvisible - 1) pos++;
                    sel = vis_at(pos);
                    ensure_pos_visible(pos);
                } else if (ui_state == UI_PLAYER) {
                    audio_set_volume(audio_volume() + 0.04f);
                    save_settings();
                    vol_overlay_timer = 65;
                }
            }
            while (wheel_accum <= -step) {
                wheel_accum += step;
                if (ui_state == UI_LIST && nvisible > 0) {
                    int pos = sel_pos();
                    if (pos > 0) pos--;
                    sel = vis_at(pos);
                    ensure_pos_visible(pos);
                } else if (ui_state == UI_PLAYER) {
                    audio_set_volume(audio_volume() - 0.04f);
                    save_settings();
                    vol_overlay_timer = 65;
                }
            }
        }
        return 1;
    }

    if (wheel_active && t->up) {
        if (!wheel_moved && wheel_down_zone >= 0) trigger_wheel_zone(wheel_down_zone);
        wheel_active = 0;
        wheel_moved = 0;
        wheel_down_zone = -1;
        return 1;
    }

    if (t->down && d2 <= (W_RO + 6) * (W_RO + 6)) {
        wheel_active = 1;
        wheel_moved = 0;
        wheel_accum = 0.0f;
        if (d2 <= W_RI * W_RI) {
            wheel_down_zone = 0;
        } else {
            wheel_last_ang = atan2f((float)dy, (float)dx);
            if (abs(dy) >= abs(dx)) wheel_down_zone = (dy < 0) ? 1 : 3;
            else wheel_down_zone = (dx > 0) ? 2 : 4;
        }
        return 1;
    }

    return 0;
}

// ---------- 共享硬件按钮 & Click Wheel 绘制 ----------
static Rect draw_hw_btn(int x, int y, int w, int h, float px, const char* txt, int active) {
    fill_round_rect_aa(screen, x, y, w, h, h / 2, active ? C_PRIM_TOP : C_BTN);
    stroke_round_rect_aa(screen, x, y, w, h, h / 2, 1, active ? C_PRIM_BOT : C_WHEEL_RING, 180);
    draw_text_centered(screen, font, px, x, x + w, y + (h - (int)px) / 2 - 1,
                       active ? C_ON_PRIM : C_TXT2, txt);
    Rect r = { x, y, w, h };
    return r;
}

static void draw_lcd_tab(Rect* rc, int x, int y, int h, float px, const char* txt, int active) {
    int tw = text_width(font, px, txt);
    int w = tw + 20;
    if (active) fill_round_grad_v(screen, x, y, w, h, h / 2, C_PRIM_TOP, C_PRIM_BOT);
    draw_text_centered(screen, font, px, x, x + w, y + (h - (int)px) / 2 - 1,
                       active ? C_ON_PRIM : C_TXT2, txt);
    *rc = (Rect){ x, y, w, h };
}

static void draw_click_wheel(void) {
    int ro = W_RO, ri = W_RI;
    fill_circle_a(screen, W_CX, W_CY + 6, ro + 3, COL(0, 0, 0, 255), 34);
    fill_ellipse_aa(screen, W_CX, W_CY, ro, ro, C_WHEEL);
    stroke_circle_aa(screen, W_CX, W_CY, ro, 2, C_WHEEL_RING, 210);

    // 激活区轻微按压反馈
    int r_mid = (ro + ri) / 2;
    if (wheel_active && !wheel_moved && wheel_down_zone > 0) {
        int hx = W_CX, hy = W_CY;
        if (wheel_down_zone == 1) hy = W_CY - r_mid;
        else if (wheel_down_zone == 2) hx = W_CX + r_mid;
        else if (wheel_down_zone == 3) hy = W_CY + r_mid;
        else if (wheel_down_zone == 4) hx = W_CX - r_mid;
        fill_circle_a(screen, hx, hy, (ro - ri) / 2, C_PRIM_TOP, 48);
    }

    // 上 MENU
    float mpx = portrait() ? 19.0f : 16.0f;
    draw_text_centered(screen, font, mpx, W_CX - 60, W_CX + 60, W_CY - r_mid - (int)mpx / 2,
                       C_WHEEL_TXT, "MENU");

    // 左 |<<  右 >>|
    int ic = portrait() ? 22 : 18;
    icon_prev(screen, W_CX - r_mid - ic / 2, W_CY - ic / 2, ic, C_WHEEL_TXT);
    icon_next(screen, W_CX + r_mid - ic / 2, W_CY - ic / 2, ic, C_WHEEL_TXT);

    // 下 >||
    int pwc = portrait() ? 24 : 20;
    icon_play_pause(screen, W_CX - pwc / 2, W_CY + r_mid - pwc / 2 + 2, pwc, C_WHEEL_TXT);

    // 中心确认圆键
    uint32_t ccol = (wheel_active && wheel_down_zone == 0) ? C_PLAY_TINT : C_WHEEL_CENTER;
    fill_ellipse_aa(screen, W_CX, W_CY, ri, ri, ccol);
    stroke_circle_aa(screen, W_CX, W_CY, ri, 2, C_WHEEL_RING, 200);
    const char* clbl = ui_state == UI_LIST ? "确认" : (audio_paused() ? "播放" : "暂停");
    draw_text_centered(screen, font, portrait() ? 17.0f : 15.0f, W_CX - ri, W_CX + ri,
                       W_CY - (portrait() ? 10 : 8), C_WHEEL_TXT, clbl);
}

// ---------- 触摸交互 ----------
static void handle_common_touch(int x, int y) {
    if (in_rc(x, y, rc_btn_theme)) { cycle_theme(); return; }
    if (in_rc(x, y, rc_btn_ori))   { toggle_orientation(); return; }
    if (in_rc(x, y, rc_btn_diag))  { show_diag = 1; return; }
    if (in_rc(x, y, rc_btn_search)) {
        if (view == VIEW_SEARCH && search_q[0]) {
            search_q[0] = 0;
            set_view(VIEW_HOME);
            show_toast("已清除搜索");
        } else {
            open_search_keyboard();
        }
        return;
    }
}

static void handle_touch_list(TouchInfo* t) {
    if (handle_wheel_touch(t)) return;

    int list_bot = list_bottom_y();
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
                    if (t->x >= L_X1 - 80 && t->x <= L_X1 - 24) {
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

    if (in_rc(x, y, rc_tab_all))     { search_q[0] = 0; set_view(VIEW_HOME); return; }
    if (in_rc(x, y, rc_tab_fav))     { set_view(view == VIEW_FAVORITE ? VIEW_HOME : VIEW_FAVORITE); return; }
    if (in_rc(x, y, rc_btn_rescan) || in_rc(x, y, rc_empty_rescan)) { scan_library(1); return; }
    if (in_rc(x, y, rc_btn_mode))    { cycle_play_mode(); return; }
    if (in_rc(x, y, rc_pod_card) || in_rc(x, y, rc_mini_bar)) {
        if (playing_idx >= 0) switch_page(UI_PLAYER);
        else if (nvisible > 0 && !loading) {
            int first = vis_at(0);
            if (first >= 0) { load_track(first); switch_page(UI_PLAYER); }
        }
        return;
    }
    if (in_rc(x, y, rc_btn_theme) || in_rc(x, y, rc_btn_ori) || in_rc(x, y, rc_btn_diag) || in_rc(x, y, rc_btn_search)) {
        handle_common_touch(x, y);
        return;
    }

    if (nvisible > 0 && y >= L_TOP && y < list_bot && x >= L_X0 && x < L_X0 + cardw) {
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

    if (handle_wheel_touch(t)) return;
    if (!t->down) return;

    int x = t->x, y = t->y;

    if (in_rc(x, y, rc_p_back) || in_rc(x, y, rc_pod_card)) {
        switch_page(UI_LIST);
        sel = playing_idx >= 0 ? playing_idx : sel;
        rebuild_visible();
        return;
    }
    if (in_rc(x, y, rc_btn_fav_p)) {
        if (playing_idx >= 0) toggle_fav(playing_idx);
        return;
    }
    if (in_rc(x, y, rc_btn_mode)) {
        cycle_play_mode();
        return;
    }
    if (in_rc(x, y, rc_btn_theme) || in_rc(x, y, rc_btn_ori) || in_rc(x, y, rc_btn_diag) || in_rc(x, y, rc_btn_search)) {
        handle_common_touch(x, y);
        return;
    }

    if (in_rect(x, y, ui_px0 - 12, ui_py - 20, ui_px1 - ui_px0 + 24, 48)) {
        touch_scrub = 1;
        seek_from_touch(x, ui_px0, ui_px1);
        return;
    }

    if (in_rect(x, y, ui_cover_x, ui_cover_y, ui_cover_sz, ui_cover_sz)) {
        if (audio_paused()) audio_resume(); else audio_pause();
    }
}

// ---------- LCD 外框 + iPod 顶栏与硬件侧栏绘制 ----------
static void draw_lcd_shell(const char* title, const char* right_sub) {
    canvas_clear(screen, C_BODY);

    // 外壳微投影与深色玻璃边框
    fill_round_rect_shadow(screen, LCD_X - 8, LCD_Y - 8, LCD_W + 16, LCD_H + 16, 22, 42);
    fill_round_rect_aa(screen, LCD_X - 8, LCD_Y - 8, LCD_W + 16, LCD_H + 16, 22, C_BEZEL);
    fill_round_rect_aa(screen, LCD_X, LCD_Y, LCD_W, LCD_H, 16, C_LCD);

    // 经典 iPod 渐变顶栏
    fill_round_grad_v(screen, LCD_X, LCD_Y, LCD_W, 44, 14, C_HDR_TOP, C_HDR_BOT);
    fill_rect(screen, LCD_X, LCD_Y + 28, LCD_W, 16, C_HDR_BOT);
    fill_rect(screen, LCD_X, LCD_Y + 44, LCD_W, 1, C_LINE);

    if (poster24) blit_rgba_round(screen, LCD_X + 14, LCD_Y + 9, poster24, 26, 26, 6);
    draw_text(screen, font, 18, LCD_X + 48, LCD_Y + 12, C_TXT, title);

    if (right_sub && right_sub[0]) {
        draw_text_right(screen, font, 13, LCD_X + LCD_W - 16, LCD_Y + 15, C_TXT2, right_sub);
    }
}

static void draw_hardware_controls(void) {
    if (!portrait()) {
        // 横屏右侧 MP4 迷你状态窗 + 转盘 + 4 硬件键
        int px = 874, pw = 382;
        fill_round_rect_aa(screen, px, 22, pw, 148, 20, C_BEZEL);
        fill_round_rect_aa(screen, px + 6, 28, pw - 12, 136, 14, C_LCD);
        rc_pod_card = (Rect){ px, 22, pw, 148 };

        if (playing_idx >= 0) {
            if (tracks[playing_idx].thumb84)
                blit_rgba_round(screen, px + 18, 54, tracks[playing_idx].thumb84, 84, 84, 12);
            const char* st = audio_paused() ? "已暂停 · 点击进播放页" : "正在播放 · 点击进播放页";
            draw_text(screen, font, 13, px + 114, 42, C_PRIM_TXT, st);
            draw_text_max(screen, font, 19, px + 114, 68, C_TXT, p_title, pw - 134);
            draw_text_max(screen, font, 14, px + 114, 98, C_TXT2, p_artist[0] ? p_artist : p_fmt, pw - 134);
            char tm[64];
            char t1[16], t2[16];
            fmt_time(audio_position(), t1, sizeof(t1));
            fmt_time(audio_duration(), t2, sizeof(t2));
            snprintf(tm, sizeof(tm), "%s / %s · %s", t1, t2, mode_short[play_mode]);
            draw_text(screen, font, 13, px + 114, 126, C_TXT3, tm);
        } else {
            if (poster84) blit_rgba_round(screen, px + 18, 54, poster84, 84, 84, 12);
            draw_text(screen, font, 19, px + 116, 62, C_TXT, "Siren MP4");
            draw_text(screen, font, 14, px + 116, 94, C_TXT2, "滑动或点击下方转盘操控");
            draw_text(screen, font, 13, px + 116, 120, C_TXT3, "B 返回/退出 · 右摇杆音量");
        }

        draw_click_wheel();

        char th_lbl[32];
        snprintf(th_lbl, sizeof(th_lbl), "涂装：%s", theme_names[cur_theme]);
        rc_btn_theme  = draw_hw_btn(884,  588, 176, 40, 14, th_lbl, 0);
        rc_btn_ori    = draw_hw_btn(1072, 588, 176, 40, 14, "横屏 / 竖屏", 0);
        rc_btn_search = draw_hw_btn(884,  638, 176, 40, 14,
                                    (view == VIEW_SEARCH && search_q[0]) ? "清除搜索" : "搜索 [X]",
                                    view == VIEW_SEARCH);
        rc_btn_diag   = draw_hw_btn(1072, 638, 176, 40, 14, "自检 [Y]", 0);
    } else {
        // 竖屏下半部：经典乔布斯 iPod 转盘 + 四角实体按钮
        rc_pod_card = (Rect){ 0, 0, 0, 0 };
        draw_click_wheel();

        rc_btn_ori    = draw_hw_btn(24,  668, 136, 42, 14, "横 / 竖 [-]", 0);
        char th_lbl[32];
        snprintf(th_lbl, sizeof(th_lbl), "涂装:%s", theme_names[cur_theme]);
        rc_btn_theme  = draw_hw_btn(560, 668, 136, 42, 14, th_lbl, 0);
        rc_btn_search = draw_hw_btn(24,  1210, 136, 42, 14,
                                    (view == VIEW_SEARCH && search_q[0]) ? "清除搜索" : "搜索 [X]",
                                    view == VIEW_SEARCH);
        rc_btn_diag   = draw_hw_btn(560, 1210, 136, 42, 14, "自检 [Y]", 0);

        draw_text_centered(screen, font, 13, 166, 554, 1223, C_TXT2, "滑动转盘选曲 · B 返回/退出");
    }
}

// ---------- 列表页绘制（LCD 菜单） ----------
static void draw_list(void) {
    char hdr_sub[64];
    if (playing_idx >= 0)
        snprintf(hdr_sub, sizeof(hdr_sub), "%s · %s · %d首", audio_paused() ? "❚❚" : "▶", mode_short[play_mode], ntracks);
    else
        snprintf(hdr_sub, sizeof(hdr_sub), "%s · %d 首", mode_short[play_mode], ntracks);

    const char* hdr_t = "Siren · 音乐库";
    if (view == VIEW_FAVORITE) hdr_t = "我喜欢的";
    else if (view == VIEW_SEARCH && search_q[0]) hdr_t = "搜索结果";

    draw_lcd_shell(hdr_t, hdr_sub);

    int list_bot = list_bottom_y();
    int cardw = L_X1 - L_X0;
    rc_empty_rescan = (Rect){ 0, 0, 0, 0 };

    // LCD 内部标签栏
    if (!portrait()) {
        int tx = LCD_X + 190;
        char all_l[32], fav_l[32];
        snprintf(all_l, sizeof(all_l), "全部(%d)", ntracks);
        snprintf(fav_l, sizeof(fav_l), "喜欢(%d)", fav_count);
        draw_lcd_tab(&rc_tab_all, tx, LCD_Y + 8, 28, 13, all_l, view == VIEW_HOME);
        tx += rc_tab_all.w + 6;
        draw_lcd_tab(&rc_tab_fav, tx, LCD_Y + 8, 28, 13, fav_l, view == VIEW_FAVORITE);
        tx += rc_tab_fav.w + 6;
        draw_lcd_tab(&rc_btn_rescan, tx, LCD_Y + 8, 28, 13, "重新扫描", 0);
        tx += rc_btn_rescan.w + 6;
        draw_lcd_tab(&rc_btn_mode, tx, LCD_Y + 8, 28, 13, mode_short[play_mode], play_mode != MODE_LOOP_ALL);
    } else {
        int sy = LCD_Y + 48;
        fill_rect(screen, LCD_X, sy, LCD_W, 38, C_SUBBAR);
        fill_rect(screen, LCD_X, sy + 38, LCD_W, 1, C_LINE);
        int tx = LCD_X + 12;
        char all_l[32], fav_l[32];
        snprintf(all_l, sizeof(all_l), "全部(%d)", ntracks);
        snprintf(fav_l, sizeof(fav_l), "喜欢(%d)", fav_count);
        draw_lcd_tab(&rc_tab_all, tx, sy + 5, 28, 14, all_l, view == VIEW_HOME);
        tx += rc_tab_all.w + 8;
        draw_lcd_tab(&rc_tab_fav, tx, sy + 5, 28, 14, fav_l, view == VIEW_FAVORITE);
        tx += rc_tab_fav.w + 8;
        draw_lcd_tab(&rc_btn_rescan, tx, sy + 5, 28, 14, "扫描", 0);
        tx += rc_btn_rescan.w + 8;
        draw_lcd_tab(&rc_btn_mode, tx, sy + 5, 28, 14, mode_names[play_mode], play_mode != MODE_LOOP_ALL);
    }

    ensure_visible_thumbs();

    if (nvisible == 0) {
        int bw = portrait() ? 540 : 620;
        int bh = 186;
        int bx = L_X0 + (cardw - bw) / 2;
        int by = L_TOP + (list_area_h() - bh) / 2;
        fill_round_rect_aa(screen, bx, by, bw, bh, 14, C_SUBBAR);

        const char* tmsg = "未在 SD 卡找到音频";
        const char* smsg = "支持 MP3 / FLAC / OGG / WAV · 可放入 sdmc:/music";
        if (view == VIEW_FAVORITE && ntracks > 0) {
            tmsg = "暂无收藏歌曲";
            smsg = "点击歌曲右侧爱心或按 ZL 键添加收藏";
        } else if (view == VIEW_SEARCH && ntracks > 0) {
            tmsg = "未找到匹配歌曲";
            smsg = "可点击“清除搜索”返回全部歌曲";
        }
        draw_text_centered(screen, font, 20, bx + 16, bx + bw - 16, by + 32, C_TXT, tmsg);
        draw_text_centered(screen, font, 14, bx + 16, bx + bw - 16, by + 68, C_TXT2, smsg);

        int btn_w = 188, btn_h = 40;
        int btn_x = bx + (bw - btn_w) / 2;
        int btn_y = by + 114;
        fill_round_grad_v(screen, btn_x, btn_y, btn_w, btn_h, 20, C_PRIM_TOP, C_PRIM_BOT);
        draw_text_centered(screen, font, 15, btn_x, btn_x + btn_w, btn_y + 11, C_ON_PRIM, "重新扫描 SD 卡");
        rc_empty_rescan = (Rect){ btn_x, btn_y, btn_w, btn_h };
    } else {
        int i0 = (int)floorf(scroll_px / (float)L_ROW) - 1;
        if (i0 < 0) i0 = 0;
        int i1 = (int)ceilf((scroll_px + (float)list_area_h()) / (float)L_ROW) + 1;
        if (i1 > nvisible) i1 = nvisible;

        for (int i = i0; i < i1; i++) {
            int idx = vis_at(i);
            if (idx < 0) break;
            int y = L_TOP + i * L_ROW - (int)scroll_px;
            int pad_v = (L_ROW - L_CARD_H) / 2;
            int cy0 = y + pad_v;
            int ch = L_CARD_H;
            if (cy0 + ch <= L_TOP || cy0 >= list_bot) continue;

            int is_sel = (idx == sel);
            int is_playing = (idx == playing_idx);
            int is_press = (touch_drag && touch_press_idx == i);

            if (is_sel || is_press) {
                fill_round_grad_v(screen, L_X0, cy0, cardw - 10, ch, 10, C_PRIM_TOP, C_PRIM_BOT);
            } else if (is_playing) {
                fill_round_rect_aa(screen, L_X0, cy0, cardw - 10, ch, 10, C_PLAY_TINT);
            } else {
                fill_rect(screen, L_X0 + 8, cy0 + ch + 1, cardw - 26, 1, C_LINE);
            }

            int tx = L_X0 + 8;
            int ty = cy0 + (ch - 52) / 2;
            if (tracks[idx].thumb) blit_rgba_round(screen, tx, ty, tracks[idx].thumb, 52, 52, 8);

            char nm[256];
            basename_no_ext(tracks[idx].path, nm, sizeof(nm));
            const char* ttl = tracks[idx].meta.title[0] ? tracks[idx].meta.title : nm;
            const char* art = tracks[idx].meta.artist[0] ? tracks[idx].meta.artist : "未知艺术家";

            uint32_t tc = (is_sel || is_press) ? C_ON_PRIM : (is_playing ? C_PRIM_TXT : C_TXT);
            uint32_t sc = (is_sel || is_press) ? C_ON_PRIM : C_TXT3;

            int txtx = tx + 64;
            int right_res = 88;
            int maxw = (L_X0 + cardw - 10) - txtx - right_res;
            if (maxw < 80) maxw = 80;

            int block = L_TITLE + 5 + L_ART;
            int ty2 = cy0 + (ch - block) / 2 - 1;
            draw_text_max(screen, font, (float)L_TITLE, txtx, ty2, tc, ttl, maxw);

            char fmt[12];
            track_format_tag(tracks[idx].path, fmt, sizeof(fmt));
            char subline[300];
            snprintf(subline, sizeof(subline), "%s%s · %s", is_playing ? "▶ " : "", fmt, art);
            draw_text_max(screen, font, (float)L_ART, txtx, ty2 + L_TITLE + 5, sc, subline, maxw);

            // 爱心收藏 +经典 iPod '>' 箭头
            int hx = L_X1 - 62;
            int hy = cy0 + (ch - 18) / 2;
            uint32_t hcol = tracks[idx].fav ? C_HEART : ((is_sel || is_press) ? COL(255, 255, 255, 160) : C_TXT3);
            icon_heart(screen, hx, hy, 18, hcol);

            int ax = L_X1 - 30;
            int ay = cy0 + (ch - 12) / 2;
            icon_chevron_right(screen, ax, ay, 12, (is_sel || is_press) ? C_ON_PRIM : C_TXT3);
        }

        // LCD 内部滚动条
        float ms = max_scroll_px();
        if (ms > 0.0f) {
            int track_h = list_area_h() - 12;
            int thumb_h = (int)((float)list_area_h() / (float)(nvisible * L_ROW) * (float)track_h);
            if (thumb_h < 28) thumb_h = 28;
            if (thumb_h > track_h) thumb_h = track_h;
            int thumb_y = L_TOP + 6 + (int)((scroll_px / ms) * (float)(track_h - thumb_h));
            fill_round_rect_aa(screen, LCD_X + LCD_W - 8, thumb_y, 5, thumb_h, 2, C_TXT3);
        }
    }

    // 重刷 LCD 上下遮罩，保证滚动行不溢出顶栏与底栏
    fill_rect(screen, LCD_X, L_TOP - 4, LCD_W, 4, C_LCD);
    fill_round_rect_aa(screen, LCD_X, list_bot, LCD_W, 60, 14, C_SUBBAR);
    fill_rect(screen, LCD_X, list_bot, LCD_W, 14, C_SUBBAR);
    fill_rect(screen, LCD_X, list_bot, LCD_W, 1, C_LINE);
    rc_mini_bar = (Rect){ LCD_X, list_bot, LCD_W, 60 };

    if (playing_idx >= 0) {
        if (audio_duration() > 0) {
            float p = clampf((float)(audio_position() / audio_duration()), 0.0f, 1.0f);
            fill_rect(screen, LCD_X + 12, list_bot + 1, (int)((LCD_W - 24) * p), 3, C_PRIM_TOP);
        }
        char np[320];
        snprintf(np, sizeof(np), "%s %s", audio_paused() ? "❚❚" : "▶", p_title);
        draw_text_max(screen, font, 16, LCD_X + 18, list_bot + 14, C_TXT, np, LCD_W - 220);
        char t1[16], t2[16], tm[48];
        fmt_time(audio_position(), t1, sizeof(t1));
        fmt_time(audio_duration(), t2, sizeof(t2));
        snprintf(tm, sizeof(tm), "%s / %s  详情 >", t1, t2);
        draw_text_right(screen, font, 14, LCD_X + LCD_W - 18, list_bot + 16, C_PRIM_TXT, tm);
    } else {
        const char* hint = ntracks == 0
            ? "未发现音频 · 点击扫描或按 A 键重扫 · B 退出"
            : "点击歌曲或按 A 播放 · 滑转盘选曲 · B 退出";
        draw_text_centered(screen, font, 14, LCD_X + 12, LCD_X + LCD_W - 12, list_bot + 20, C_TXT2, hint);
    }

    draw_hardware_controls();
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
    if (down & HidNpadButton_ZL) {
        int i = vis_at(pos);
        if (i >= 0) toggle_fav(i);
    }
}

// ---------- 播放页绘制（LCD 内部：乔布斯 iPod 封面倒影 + 歌词 / 频谱） ----------
static void draw_lyric_or_spectrum(int x0, int x1, int ly_top, int ly_bot, int spec_base, int spec_hint_y,
                                   double pos, double dur) {
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
        for (int d = -2; d <= 2; d++) {
            int i = act + d;
            if (i >= 0 && i < lyr->n) idxs2[n++] = i;
        }
        int y = ly_top;
        for (int k = 0; k < n; k++) {
            int i = idxs2[k];
            int dist = (act >= 0) ? (i - act) : 2;
            if (y > ly_bot) break;
            if (dist == 0) {
                fill_round_rect_aa(screen, x0, y - 4, rw, 36, 10, C_PLAY_TINT);
                fill_round_rect_aa(screen, x0 + 6, y + 3, 4, 22, 2, C_PRIM_BOT);
                draw_text_max(screen, font, 21, x0 + 18, y + 3, C_PRIM_TXT, lyr->lines[i].text, rw - 24);
                y += 42;
            } else {
                float px = (dist == -1 || dist == 1) ? 17.0f : 15.0f;
                uint32_t cc = (dist == -1 || dist == 1) ? C_TXT : C_TXT2;
                draw_text_max(screen, font, px, x0 + 14, y, cc, lyr->lines[i].text, rw - 20);
                y += (int)px + 17;
            }
        }
        if (lyr->unsynced) {
            draw_text(screen, font, 13, x0 + 14, spec_hint_y, C_TXT3, "静态歌词（内嵌 · 按进度滚动）");
        }
    } else {
        float sp[28];
        audio_get_spectrum(sp);
        for (int i = 0; i < 28; i++) bars[i] = sp[i];

        int n = portrait() ? 20 : 18;
        int bw = portrait() ? 14 : 12;
        int span = rw - 24;
        int gap = (span - n * bw) / (n - 1);
        if (gap < 4) gap = 4;
        int total_w = n * bw + (n - 1) * gap;
        int bx0 = x0 + (rw - total_w) / 2;
        int max_h = portrait() ? 148 : 176;
        for (int i = 0; i < n; i++) {
            float v = clampf(bars[i], 0.05f, 1.0f);
            int hgt = (int)(v * max_h);
            int bx = bx0 + i * (bw + gap);
            fill_round_rect_aa(screen, bx, spec_base - hgt, bw, hgt, bw / 2, C_PRIM_TOP);
        }
        draw_text_centered(screen, font, 13, x0, x1, spec_hint_y, C_TXT3,
                           "未找到歌词 · 支持同名 .lrc 及 MP3/FLAC/OGG 内嵌歌词");
    }
}

static void draw_lcd_progress(int py, double pos, double dur) {
    int w = ui_px1 - ui_px0;
    fill_round_rect_aa(screen, ui_px0, py, w, 10, 5, C_LINE);
    int fillw = (int)((dur > 0 ? pos / dur : 0) * w);
    if (fillw < 0) fillw = 0;
    if (fillw > w) fillw = w;
    if (fillw > 4) fill_round_grad_v(screen, ui_px0, py, fillw, 10, 5, C_PRIM_TOP, C_PRIM_BOT);
    fill_ellipse_aa(screen, ui_px0 + fillw, py + 5, 9, 9, C_PRIM_BOT);
    fill_ellipse_aa(screen, ui_px0 + fillw, py + 5, 4, 4, C_ON_PRIM);

    char t1[32], t2[32];
    fmt_time(pos, t1, sizeof(t1));
    fmt_time(dur, t2, sizeof(t2));
    draw_text(screen, font, 14, ui_px0, py + 16, C_TXT2, t1);
    draw_text_right(screen, font, 14, ui_px1, py + 16, C_TXT2, t2);
}

static void draw_player(void) {
    char top_info[64];
    snprintf(top_info, sizeof(top_info), "%s · %d/%d · %s",
             p_fmt[0] ? p_fmt : "AUDIO",
             playing_idx >= 0 ? playing_idx + 1 : 0, ntracks,
             audio_paused() ? "已暂停" : "播放中");

    draw_lcd_shell("正在播放", top_info);
    rc_tab_all = rc_tab_fav = rc_btn_rescan = rc_empty_rescan = rc_mini_bar = (Rect){ 0, 0, 0, 0 };

    // LCD 顶栏左侧 < 返回 按钮（与右侧横/竖转盘 MENU 同步）
    int bx = LCD_X + 168, by = LCD_Y + 8;
    fill_round_rect_aa(screen, bx, by, 82, 28, 14, C_BTN);
    icon_back_arrow(screen, bx + 10, by + 6, 15, C_TXT);
    draw_text(screen, font, 14, bx + 30, by + 6, C_TXT, "返回");
    rc_p_back = (Rect){ bx, by, 82, 28 };

    int fav_on = (playing_idx >= 0 && tracks[playing_idx].fav);
    double pos = audio_position();
    double dur = audio_duration();
    if (dur > 0 && pos > dur) pos = dur;

    if (!portrait()) {
        // 横屏 LCD：左侧 240x240 封面 + 倒影 + 收藏/模式；右侧 标题/歌手/歌词或频谱；底部 iPod 进度条
        int csz = 240;
        int cx = LCD_X + 34, cy = LCD_Y + 78;
        ui_cover_x = cx; ui_cover_y = cy; ui_cover_sz = csz;

        fill_round_rect_shadow(screen, cx, cy, csz, csz, 16, 48);
        if (cover240) {
            blit_rgba_round(screen, cx, cy, cover240, csz, csz, 16);
            draw_cover_reflection(cx, cy, cover240, csz, 36, C_LCD);
        }

        // 封面下方：喜欢 & 播放模式开关
        fill_round_rect_aa(screen, cx, cy + csz + 48, 108, 36, 18, fav_on ? C_PLAY_TINT : C_BTN);
        icon_heart(screen, cx + 16, cy + csz + 57, 18, fav_on ? C_HEART : C_TXT2);
        draw_text(screen, font, 14, cx + 42, cy + csz + 57, fav_on ? C_HEART : C_TXT2, fav_on ? "已喜欢" : "喜欢");
        rc_btn_fav_p = (Rect){ cx, cy + csz + 48, 108, 36 };

        fill_round_rect_aa(screen, cx + 120, cy + csz + 48, 120, 36, 18,
                           play_mode == MODE_LOOP_ALL ? C_BTN : C_PLAY_TINT);
        draw_text_centered(screen, font, 14, cx + 120, cx + 240, cy + csz + 57,
                           play_mode == MODE_LOOP_ALL ? C_TXT2 : C_PRIM_TXT, mode_names[play_mode]);
        rc_btn_mode = (Rect){ cx + 120, cy + csz + 48, 120, 36 };

        int col_x = cx + csz + 32;
        int col_x1 = LCD_X + LCD_W - 26;
        int col_w = col_x1 - col_x;
        float tpx = text_width(font, 30, p_title) > col_w ? 24.0f : 30.0f;
        draw_text_max(screen, font, tpx, col_x, cy + 4, C_TXT, p_title, col_w);
        draw_text_max(screen, font, 17, col_x, cy + 44, C_TXT2, p_artist[0] ? p_artist : "未知艺术家", col_w);
        fill_round_rect_aa(screen, col_x, cy + 74, 48, 4, 2, C_PRIM_TOP);

        draw_lyric_or_spectrum(col_x, col_x1, cy + 96, cy + 340, cy + 314, cy + 332, pos, dur);

        int py = LCD_Y + LCD_H - 96;
        ui_px0 = LCD_X + 34; ui_px1 = LCD_X + LCD_W - 34; ui_py = py;
        draw_lcd_progress(py, pos, dur);

        draw_text_centered(screen, font, 13, LCD_X + 34, LCD_X + LCD_W - 34, py + 48, C_TXT3,
                           "拖动进度条定位 · 滑右侧转盘调音量 · MENU / B 返回列表");
    } else {
        // 竖屏 iPod 经典布局：上部左封面+倒影 / 右歌名歌手与收藏模式，中部歌词或频谱，底部 iPod 进度条
        int csz = 176;
        int cx = LCD_X + 26, cy = LCD_Y + 66;
        ui_cover_x = cx; ui_cover_y = cy; ui_cover_sz = csz;

        fill_round_rect_shadow(screen, cx, cy, csz, csz, 14, 44);
        if (cover176) {
            blit_rgba_round(screen, cx, cy, cover176, csz, csz, 14);
            draw_cover_reflection(cx, cy, cover176, csz, 28, C_LCD);
        }

        int rx = cx + csz + 22, rx1 = LCD_X + LCD_W - 22, rw = rx1 - rx;
        float tpx = text_width(font, 24, p_title) > rw ? 20.0f : 24.0f;
        draw_text_max(screen, font, tpx, rx, cy + 8, C_TXT, p_title, rw);
        draw_text_max(screen, font, 16, rx, cy + 44, C_TXT2, p_artist[0] ? p_artist : "未知艺术家", rw);

        char fmt_line[48];
        snprintf(fmt_line, sizeof(fmt_line), "%s · 第 %d / %d 首", p_fmt[0] ? p_fmt : "AUDIO",
                 playing_idx >= 0 ? playing_idx + 1 : 0, ntracks);
        draw_text(screen, font, 13, rx, cy + 78, C_TXT3, fmt_line);

        fill_round_rect_aa(screen, rx, cy + 114, 102, 34, 17, fav_on ? C_PLAY_TINT : C_BTN);
        icon_heart(screen, rx + 14, cy + 123, 16, fav_on ? C_HEART : C_TXT2);
        draw_text(screen, font, 14, rx + 38, cy + 123, fav_on ? C_HEART : C_TXT2, fav_on ? "已喜欢" : "喜欢");
        rc_btn_fav_p = (Rect){ rx, cy + 114, 102, 34 };

        fill_round_rect_aa(screen, rx + 112, cy + 114, 114, 34, 17,
                           play_mode == MODE_LOOP_ALL ? C_BTN : C_PLAY_TINT);
        draw_text_centered(screen, font, 14, rx + 112, rx + 226, cy + 123,
                           play_mode == MODE_LOOP_ALL ? C_TXT2 : C_PRIM_TXT, mode_names[play_mode]);
        rc_btn_mode = (Rect){ rx + 112, cy + 114, 114, 34 };

        fill_rect(screen, LCD_X + 26, cy + csz + 38, LCD_W - 52, 1, C_LINE);

        draw_lyric_or_spectrum(LCD_X + 24, LCD_X + LCD_W - 24,
                               cy + csz + 52, LCD_Y + LCD_H - 94,
                               LCD_Y + LCD_H - 122, LCD_Y + LCD_H - 102, pos, dur);

        int py = LCD_Y + LCD_H - 64;
        ui_px0 = LCD_X + 28; ui_px1 = LCD_X + LCD_W - 28; ui_py = py;
        draw_lcd_progress(py, pos, dur);
    }

    if (audio_status() == 3) {
        draw_text_centered(screen, font, 20, LCD_X, LCD_X + LCD_W, LCD_Y + LCD_H / 2 - 28,
                           C_HEART, "解码失败：文件不支持或损坏");
    }
    if (loading)
        draw_text_centered(screen, font, 20, LCD_X, LCD_X + LCD_W, LCD_Y + LCD_H / 2, C_TXT2, "加载中…");

    draw_hardware_controls();
}

static void draw_volume_overlay(void) {
    if (vol_overlay_timer <= 0) return;
    vol_overlay_timer--;
    char vs[64];
    snprintf(vs, sizeof(vs), "音量 %d%%", (int)(audio_volume() * 100 + 0.5f));
    int bw2 = 240, bh2 = 68;
    int bx = LCD_X + (LCD_W - bw2) / 2, by = LCD_Y + 60;
    fill_round_rect_shadow(screen, bx, by, bw2, bh2, 18, 56);
    fill_round_rect_aa(screen, bx, by, bw2, bh2, 18, C_LCD);
    stroke_round_rect_aa(screen, bx, by, bw2, bh2, 18, 1, C_WHEEL_RING, 200);
    icon_speaker(screen, bx + 22, by + 15, 20, C_PRIM_TOP);
    draw_text_centered(screen, font, 18, bx + 16, bx + bw2, by + 14, C_TXT, vs);
    fill_round_rect_aa(screen, bx + 22, by + 46, bw2 - 44, 6, 3, C_LINE);
    fill_round_rect_aa(screen, bx + 22, by + 46, (int)((bw2 - 44) * audio_volume()), 6, 3, C_PRIM_TOP);
}

static void draw_diag(void) {
    fill_rect_a(screen, 0, 0, SCR_W, SCR_H, COL(0, 0, 0, 255), 170);
    int cw = portrait() ? 640 : 860;
    int ch = 460;
    int cx = (SCR_W - cw) / 2;
    int cy = (SCR_H - ch) / 2;
    fill_round_rect_shadow(screen, cx, cy, cw, ch, 24, 72);
    fill_round_rect_aa(screen, cx, cy, cw, ch, 24, C_LCD);
    stroke_round_rect_aa(screen, cx, cy, cw, ch, 24, 2, C_WHEEL_RING, 200);

    draw_text(screen, font, 23, cx + 32, cy + 28, C_PRIM_TOP, "Siren v1.2.0 系统与音频自检");
    draw_text_right(screen, font, 14, cx + cw - 32, cy + 36, C_TXT3, "点击或按 B/Y 关闭");

    char buf[1024];
    audio_get_diag(buf, sizeof(buf));
    int yy = cy + 84;
    char* line = strtok(buf, "\n");
    while (line && yy < cy + ch - 130) {
        draw_text(screen, font, 18, cx + 32, yy, C_TXT, line);
        yy += 32;
        line = strtok(NULL, "\n");
    }
    char extra[256];
    snprintf(extra, sizeof(extra), "曲库：%d 首   收藏：%d 首   模式：%s   涂装：%s",
             ntracks, fav_count, mode_names[play_mode], theme_names[cur_theme]);
    draw_text_max(screen, font, 17, cx + 32, yy + 10, C_TXT2, extra, cw - 64);

    char lyr_info[256];
    if (lyr) snprintf(lyr_info, sizeof(lyr_info), "歌词：已加载 %d 行 (%s)", lyr->n, lyr->source);
    else snprintf(lyr_info, sizeof(lyr_info), "歌词：未匹配 (支持同名 .lrc 与 MP3/FLAC/OGG 内嵌标签)");
    draw_text_max(screen, font, 16, cx + 32, yy + 42, C_TXT2, lyr_info, cw - 64);

    draw_text_max(screen, font, 14, cx + 32, cy + ch - 44, C_TXT3,
                  "提示：可滑动或点击实体转盘，主页按 B 键退出软件，按 - 切换单向竖屏", cw - 64);
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
            int tw = text_width(font, 15, toast_text) + 44;
            if (tw > SCR_W - 80) tw = SCR_W - 80;
            int bx = LCD_X + (LCD_W - tw) / 2;
            fill_round_rect_shadow(screen, bx, LCD_Y + 52, tw, 40, 20, 56);
            fill_round_rect_aa(screen, bx, LCD_Y + 52, tw, 40, 20, C_BEZEL);
            draw_text_centered(screen, font, 15, bx, bx + tw, LCD_Y + 64, COL(255, 255, 255, 255), toast_text);
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
    if (cover) free(cover);
    if (cover240) free(cover240);
    if (cover176) free(cover176);
    if (trans_shot) canvas_free(trans_shot);
    if (poster24) free(poster24);
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
