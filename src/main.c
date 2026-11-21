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

extern const unsigned char font_ttf_start[];
extern const unsigned char font_ttf_end[];
extern const unsigned char poster_start[];
extern const unsigned char poster_end[];

#define UI_LIST   0
#define UI_PLAYER 1

#define MAX_TRACKS 8192

// ---------- 布局（极简单列列表） ----------
#define ROW_H        64
#define ROWS_VISIBLE 9
#define LIST_TOP     96
#define BAR_H        48   // 列表页底部播放条高度

// ---------- 主题配色（可在设置页切换） ----------
#define THEME_TEAL    0
#define THEME_BLUE    1
#define THEME_FOREST  2
#define THEME_COUNT   3

static uint32_t C_BG, C_SURFACE, C_SURF2, C_CARD, C_CARD2;
static uint32_t C_PRIMARY, C_PRIMDK, C_ONPRIM, C_PLAYBG;
static uint32_t C_RED, C_RED_SOFT, C_HL, C_HL_PLAY;
static uint32_t C_TXT, C_TXT2, C_TXT3, C_LINE;

typedef struct {
    uint32_t bg, surface, surf2, card, card2;
    uint32_t primary, primdk, onprim, playbg;
    uint32_t txt, txt2, txt3, line;
} Theme;

static const Theme themes[THEME_COUNT] = {
    // 青绿（Material You 原配色）
    { COL(0x11,0x13,0x16,0xFF), COL(0x1C,0x1F,0x23,0xFF), COL(0x25,0x29,0x2E,0xFF),
      COL(0x21,0x25,0x29,0xFF), COL(0x28,0x2D,0x32,0xFF),
      COL(0x52,0xD6,0xC0,0xFF), COL(0x2F,0x94,0x85,0xFF), COL(0x0A,0x26,0x22,0xFF),
      COL(0x1E,0x3A,0x36,0xFF),
      COL(0xE8,0xEC,0xEE,0xFF), COL(0xA8,0xB0,0xB6,0xFF), COL(0x74,0x7C,0x82,0xFF),
      COL(0x2C,0x31,0x36,0xFF) },
    // 蓝（冷色深空）
    { COL(0x0D,0x14,0x20,0xFF), COL(0x15,0x1F,0x2E,0xFF), COL(0x1D,0x2A,0x3C,0xFF),
      COL(0x18,0x23,0x34,0xFF), COL(0x20,0x2D,0x40,0xFF),
      COL(0x5A,0xA9,0xFF,0xFF), COL(0x2F,0x6F,0xB0,0xFF), COL(0x06,0x18,0x2B,0xFF),
      COL(0x18,0x2F,0x4A,0xFF),
      COL(0xE7,0xEE,0xF7,0xFF), COL(0xA7,0xB6,0xC9,0xFF), COL(0x71,0x83,0x9A,0xFF),
      COL(0x26,0x34,0x48,0xFF) },
    // 黑绿（近黑底 + 苔绿）
    { COL(0x07,0x11,0x0D,0xFF), COL(0x0E,0x1B,0x15,0xFF), COL(0x15,0x27,0x1F,0xFF),
      COL(0x11,0x20,0x19,0xFF), COL(0x17,0x2A,0x21,0xFF),
      COL(0x7D,0xDC,0x8C,0xFF), COL(0x3E,0x8A,0x50,0xFF), COL(0x07,0x1C,0x0E,0xFF),
      COL(0x12,0x33,0x24,0xFF),
      COL(0xE4,0xF2,0xE8,0xFF), COL(0xA3,0xBF,0xAE,0xFF), COL(0x6E,0x8C,0x78,0xFF),
      COL(0x1C,0x33,0x27,0xFF) }
};

static const char* theme_names[THEME_COUNT] = { "青绿", "蓝", "黑绿" };
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
    C_RED = C_PRIMARY; C_RED_SOFT = C_PRIMARY;
    C_HL = C_PLAYBG; C_HL_PLAY = C_PLAYBG;
    C_TXT = t->txt; C_TXT2 = t->txt2; C_TXT3 = t->txt3; C_LINE = t->line;
}

static void load_theme_setting(void) {
    mkdir("sdmc:/Siren", 0777);
    int t = THEME_TEAL;
    FILE* f = fopen("sdmc:/Siren/theme.cfg", "r");
    if (f) { if (fscanf(f, "%d", &t) != 1) t = THEME_TEAL; fclose(f); }
    apply_theme(t);
}

static void save_theme_setting(void) {
    FILE* f = fopen("sdmc:/Siren/theme.cfg", "w");
    if (f) { fprintf(f, "%d\n", cur_theme); fclose(f); }
}

typedef struct {
    char path[600];
    TrackMeta meta;
    int meta_state;     // 0 = 未尝试, 1 = 已尝试（含失败）
    uint32_t* thumb;    // 36x36 列表缩略图
    uint32_t* thumb48;  // 48x48 底部播放条缩略图
    int fav;            // 收藏标记
} Track;

// 列表视图
#define VIEW_HOME     0
#define VIEW_SEARCH   1
#define VIEW_LIBRARY  2
#define VIEW_FAVORITE 3
#define VIEW_SETTINGS 4

static Track* tracks = NULL;
static int ntracks = 0;
static int loaded_thumbs = 0;

static int ui_state = UI_LIST;
static int view = VIEW_HOME;
static int sel = 0;
static int scroll = 0;
static int playing_idx = -1;
static int loading = 0;

static Font* font = NULL;
static Canvas* screen = NULL;

// 播放页资源
static TrackMeta meta;
static Lyrics* lyr = NULL;
static uint32_t* cover = NULL;       // 380x380 当前曲目大图
static uint32_t* cover_small = NULL; // 140x140 歌单封面
static uint32_t* poster36 = NULL;    // 海报缩略图 36x36
static uint32_t* poster48 = NULL;    // 海报缩略图 48x48
static Canvas* bg = NULL;
static char p_title[256];
static char p_artist[256];
static float bars[28];
static float bar_target[28];

static int vol_overlay_timer = 0;
static int show_diag = 0;

// ---------- 动画 ----------
static double g_time = 0;          // 累计秒数（时间基准）
static double g_dt = 0.016;        // 上一帧耗时（秒）
static uint64_t g_last_tick = 0;
static float anim_scroll = 0;      // 平滑滚动位置（行，浮点）
static float anim_sel = 0;         // 平滑选中位置
static float anim_cover = 0;       // 切歌封面交叉淡化 0->1
static uint32_t* cover_prev = NULL;   // 切歌前的旧封面（380x380）
static Canvas* trans_shot = NULL;     // 页面切换截图（旧页面）
static float trans_t = 1.0f;          // 页面切换进度 0->1
static float touch_fx_alpha = 0;      // 触点反馈淡出
static float touch_fx_x = 0, touch_fx_y = 0;
static double show_pos = 0;           // 平滑显示的播放进度

// ---------- 触摸交互状态 ----------
static TouchInfo touch_now;
static int touch_drag = 0;
static int touch_scrub = 0;
static int touch_drag_start_y = 0;
static int touch_last_y = 0;
static int touch_drag_accum = 0;
static int touch_press_idx = -1;
static int setting_focus = 0;        // 设置页手柄焦点：0自检 1音量 2清空 3主题
static int search_active = 0;       // 搜索输入框是否激活
static char search_q[128] = "";     // 搜索关键词
static int fav_count = 0;           // 收藏数量

static void open_search_keyboard(void) {
    SwkbdConfig kbd;
    Result rc = swkbdCreate(&kbd, 0);
    if (R_FAILED(rc)) { search_active = 1; return; }
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetStringLenMax(&kbd, 64);
    swkbdConfigSetStringLenMin(&kbd, 1);
    swkbdConfigSetBlurBackground(&kbd, 1);
        char out[128] = "";
        rc = swkbdShow(&kbd, out, sizeof(out));
        swkbdClose(&kbd);
        be_touch_reset();   // 键盘期间触摸状态已过期，丢弃以防止幽灵点击
        if (R_SUCCEEDED(rc) && out[0]) {
        snprintf(search_q, sizeof(search_q), "%s", out);
        search_active = 1;
        scroll = 0;
        anim_scroll = 0;
    }
}

// ---------- 可见曲目筛选（搜索 / 收藏） ----------
static int visible_idx[8192];
static int nvisible = 0;

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
    int maxscroll = nvisible - ROWS_VISIBLE;
    if (maxscroll < 0) maxscroll = 0;
    if (scroll > maxscroll) scroll = maxscroll;
    if (scroll < 0) scroll = 0;
    if (nvisible == 0) { sel = 0; return; }
    int pos = 0, found = 0;
    for (int i = 0; i < nvisible; i++) {
        if (visible_idx[i] == sel) { pos = i; found = 1; break; }
    }
    if (!found) { pos = 0; sel = visible_idx[0]; }
    if (pos < scroll) scroll = pos;
    if (pos >= scroll + ROWS_VISIBLE) scroll = pos - ROWS_VISIBLE + 1;
}

static int vis_at(int pos) {
    return (pos >= 0 && pos < nvisible) ? visible_idx[pos] : -1;
}

// 当前 sel 在筛选结果中的位置，供按键导航使用
static int sel_pos(void) {
    for (int i = 0; i < nvisible; i++)
        if (visible_idx[i] == sel) return i;
    return 0;
}
static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
// 三次缓出：起步快、收尾平滑
static float ease_out(float t) {
    t = clampf(t, 0, 1);
    float u = 1 - t;
    return 1 - u * u * u;
}
// 指数趋近：与帧率无关的平滑跟随
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
        if (d > 0.05) d = 0.05;   //  limiting 大跳变
        g_dt = d;
    }
    g_last_tick = now;
    g_time += g_dt;
}
// 切换页面时截取旧画面，用于交叉淡化
static void switch_page(int to) {
    if (to == ui_state) return;
    if (!trans_shot) trans_shot = canvas_new(BE_W, BE_H);
    if (trans_shot && screen)
        memcpy(trans_shot->px, screen->px, (size_t)BE_W * BE_H * 4);
    ui_state = to;
    trans_t = 0;
}

// ---------- 基础工具 ----------
static uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return COL(r, g, b, a);
}

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

static void fill_round_rect(Canvas* c, int x, int y, int w, int h, int r, uint32_t col) {
    if (r < 1) { fill_rect(c, x, y, w, h, col); return; }
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    fill_rect(c, x + r, y, w - 2 * r, h, col);
    fill_rect(c, x, y + r, w, h - 2 * r, col);
    for (int j = 0; j < r; j++) {
        double u = (double)(r - j) / (double)r;
        int span = (int)(r * sqrt(1.0 - u * u));
        hline(c, x + r - span, y + j, span + 1, col);
        hline(c, x + w - r - 1, y + j, span + 2, col);
        hline(c, x + r - span, y + h - 1 - j, span + 1, col);
        hline(c, x + w - r - 1, y + h - 1 - j, span + 2, col);
    }
}

static void draw_ellipse(Canvas* c, int cx, int cy, int rx, int ry, uint32_t col) {
    if (rx < 1 || ry < 1) return;
    for (int dy = -ry; dy <= ry; dy++) {
        double t = (double)dy / (double)ry;
        int dx = (int)(rx * sqrt(1.0 - t * t));
        hline(c, cx - dx, cy + dy, dx * 2 + 1, col);
    }
}

// 带透明度的椭圆（用于触点反馈等叠加效果）
static void draw_ellipse_a(Canvas* c, int cx, int cy, int rx, int ry, uint32_t col, uint8_t a) {
    if (rx < 1 || ry < 1 || a == 0) return;
    for (int dy = -ry; dy <= ry; dy++) {
        double t = (double)dy / (double)ry;
        int dx = (int)(rx * sqrt(1.0 - t * t));
        fill_rect_a(c, cx - dx, cy + dy, dx * 2 + 1, 1, col, a);
    }
}

static void ring(Canvas* c, int cx, int cy, int r, int t, uint32_t col) {
    if (r < 2) return;
    for (int dy = -r; dy <= r; dy++) {
        int dx = (int)sqrt((double)(r * r - dy * dy));
        hline(c, cx - dx, cy + dy, t, col);
        hline(c, cx + dx - t + 1, cy + dy, t, col);
    }
}

static void draw_note(Canvas* c, int x, int y, int s, uint32_t col) {
    int r = s / 7;
    if (r < 1) return;
    draw_ellipse(c, x + s * 3 / 10, y + s * 4 / 5, r, (int)(r * 0.8), col);
    draw_ellipse(c, x + s * 7 / 10, y + s * 3 / 4, r, (int)(r * 0.8), col);
    fill_rect(c, x + s * 3 / 10 + r - 2, y + s / 6, s / 16 + 2, s * 4 / 5 - s / 6, col);
    fill_rect(c, x + s * 7 / 10 + r - 2, y + s / 12, s / 16 + 2, s * 3 / 4 - s / 12, col);
    fill_tri(c, x + s * 3 / 10 + r - 4, y + s / 6 - 2, x + s * 7 / 10 + r - 4, y + s / 12 - 2,
             x + s * 7 / 10 + r - 4, y + s / 12 + s / 9, col);
    fill_tri(c, x + s * 3 / 10 + r - 4, y + s / 6 - 2, x + s * 7 / 10 + r - 4, y + s / 12 + s / 9,
             x + s * 3 / 10 + r - 4, y + s / 6 + s / 9, col);
}

// ---------- 图标 ----------
static void icon_prev(Canvas* c, int x, int y, int s, uint32_t col) {
    fill_tri(c, x + s * 3 / 4, y, x + s * 3 / 4, y + s, x + s / 8, y + s / 2, col);
    fill_rect(c, x, y, s / 8, s, col);
}
static void icon_next(Canvas* c, int x, int y, int s, uint32_t col) {
    fill_tri(c, x + s / 4, y, x + s * 7 / 8, y + s / 2, x + s / 4, y + s, col);
    fill_rect(c, x + s * 7 / 8, y, s / 8, s, col);
}
static void icon_play(Canvas* c, int x, int y, int s, uint32_t col) {
    fill_tri(c, x, y, x, y + s, x + s, y + s / 2, col);
}
static void icon_pause(Canvas* c, int x, int y, int s, uint32_t col) {
    fill_rect(c, x, y, s * 3 / 10, s, col);
    fill_rect(c, x + s * 7 / 10, y, s * 3 / 10, s, col);
}
static void icon_heart(Canvas* c, int x, int y, int s, uint32_t col) {
    int rx = s / 4;
    draw_ellipse(c, x + s * 3 / 10, y + s * 3 / 10, rx, rx, col);
    draw_ellipse(c, x + s * 7 / 10, y + s * 3 / 10, rx, rx, col);
    fill_tri(c, x + s / 10, y + s * 2 / 5, x + s * 9 / 10, y + s * 2 / 5, x + s / 2, y + s, col);
}
static void icon_search(Canvas* c, int x, int y, int s, uint32_t col) {
    ring(c, x + s * 2 / 5, y + s * 2 / 5, s / 3, 2, col);
    hline(c, x + s * 3 / 5, y + s * 3 / 5, s / 3, col);
    hline(c, x + s * 3 / 5, y + s * 3 / 5 + 1, s / 3, col);
}
static void icon_disc(Canvas* c, int x, int y, int s, uint32_t col) {
    ring(c, x + s / 2, y + s / 2, s / 2, 2, col);
    draw_ellipse(c, x + s / 2, y + s / 2, s / 6, s / 6, col);
}
static void icon_lib(Canvas* c, int x, int y, int s, uint32_t col) {
    fill_rect(c, x + 1, y + 2, 3, s - 2, col);
    fill_rect(c, x + s / 2 - 1, y + 2, 3, s - 2, col);
    fill_rect(c, x + s - 3, y + 2, 3, s - 2, col);
}
static void icon_folder(Canvas* c, int x, int y, int s, uint32_t col) {
    fill_rect(c, x, y + s / 4, s, s * 3 / 4, col);
    fill_rect(c, x, y + s / 5, s / 2, s / 5, col);
}
static void icon_gear(Canvas* c, int x, int y, int s, uint32_t col) {
    ring(c, x + s / 2, y + s / 2, s / 3, 3, col);
    fill_rect(c, x + s / 2 - 1, y, 3, s, col);
    fill_rect(c, x, y + s / 2 - 1, s, 3, col);
}
static void icon_speaker(Canvas* c, int x, int y, int s, uint32_t col) {
    fill_rect(c, x, y + s / 3, s / 4, s / 3, col);
    fill_tri(c, x + s / 4, y + s / 3, x + s * 3 / 4, y, x + s * 3 / 4, y + s, col);
    hline(c, x + s * 3 / 4 + 2, y + s / 2, s / 5, col);
}
static void icon_queue(Canvas* c, int x, int y, int s, uint32_t col) {
    for (int i = 0; i < 3; i++) {
        hline(c, x, y + i * (s / 3), s * 3 / 4, col);
        hline(c, x, y + i * (s / 3) + 2, s / 2, col);
    }
}
static void icon_device(Canvas* c, int x, int y, int s, uint32_t col) {
    fill_rect(c, x, y + s / 5, s, s * 3 / 5, col);
    hline(c, x + s / 3, y + s - 2, s / 3, col);
}
static void icon_shuffle(Canvas* c, int x, int y, int s, uint32_t col) {
    fill_tri(c, x, y, x + s / 2, y + s / 2, x, y + s / 2, col);
    fill_tri(c, x, y + s / 2, x + s / 2, y + s / 2, x, y + s, col);
    fill_tri(c, x + s / 2, y, x + s, y + s / 2, x + s / 2, y + s / 2, col);
    fill_tri(c, x + s / 2, y + s / 2, x + s, y + s, x + s / 2, y + s, col);
}

// ---------- 默认封面 / 背景 ----------
static void make_default_art(uint32_t* dst, int n, const char* seed) {
    uint32_t h = hash_str(seed);
    uint8_t r, g, b;
    hsv2rgb((float)((h % 360) / 360.0), 0.58f, 0.34f, &r, &g, &b);
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            float t = (x + y) / (float)(2 * n);
            dst[(size_t)y * n + x] = COL((uint8_t)(r * (1 - t) + 42 * t),
                                         (uint8_t)(g * (1 - t) + 22 * t),
                                         (uint8_t)(b * (1 - t) + 52 * t), 255);
        }
    }
    Canvas c = { n, n, dst };
    draw_note(&c, n / 8, n / 8, n * 3 / 4, COL(255, 255, 255, 220));
}

static void build_bg_from(const uint32_t* art, int aw, int ah) {
    if (!bg) bg = canvas_new(BE_W, BE_H);
    uint32_t small[64 * 36];
    resample_bilinear(art, aw, ah, small, 64, 36);
    box_blur(small, 64, 36, 3, 2);
    resample_bilinear(small, 64, 36, bg->px, BE_W, BE_H);
    scale_rgb(bg->px, BE_W * BE_H, 0.38f);
    for (int y = 0; y < BE_H; y++) {
        for (int x = 0; x < BE_W; x++) {
            float dx = (x - BE_W / 2.0f) / (BE_W / 2.0f);
            float dy = (y - BE_H / 2.0f) / (BE_H / 2.0f);
            float d = sqrtf(dx * dx + dy * dy);
            float f = 1.0f - 0.42f * d * d;
            if (f < 0) f = 0;
            uint32_t* p = &bg->px[(size_t)y * BE_W + x];
            uint32_t c = *p;
            *p = 0xFF000000u |
                 ((uint32_t)(((c >> 16) & 0xFF) * f) << 16) |
                 ((uint32_t)(((c >> 8) & 0xFF) * f) << 8) |
                 (uint32_t)((c & 0xFF) * f);
        }
    }
}

static void build_default_bg(const char* seed) {
    if (!bg) bg = canvas_new(BE_W, BE_H);
    uint32_t h = hash_str(seed);
    uint8_t r, g, b;
    hsv2rgb((float)((h % 360) / 360.0), 0.5f, 0.16f, &r, &g, &b);
    for (int y = 0; y < BE_H; y++) {
        for (int x = 0; x < BE_W; x++) {
            float t = ((x * 0.6f + y) / (BE_W * 0.6f + BE_H));
            bg->px[(size_t)y * BE_W + x] = COL((uint8_t)(r * t + 10 * (1 - t)),
                                               (uint8_t)(g * t + 8 * (1 - t)),
                                               (uint8_t)(b * t + 14 * (1 - t)), 255);
        }
    }
}

static void build_playlist_cover(void) {
    if (!cover_small) cover_small = (uint32_t*)malloc((size_t)140 * 140 * 4);
    if (!cover_small) return;

    // 优先使用内嵌的 WUKONG 海报（320x320）
    size_t poster_len = (size_t)(poster_end - poster_start);
    if (poster_len >= 320 * 320 * 4) {
        resample_bilinear((const uint32_t*)poster_start, 320, 320, cover_small, 140, 140);
        return;
    }

    TrackMeta pm;
    memset(&pm, 0, sizeof(pm));
    if (ntracks > 0) {
        meta_load(tracks[0].path, &pm);
        if (pm.art && pm.art_w > 8 && pm.art_h > 8) {
            resample_bilinear((const uint32_t*)pm.art, pm.art_w, pm.art_h, cover_small, 140, 140);
            meta_clear(&pm);
            return;
        }
        meta_clear(&pm);
    }
    make_default_art(cover_small, 140, "lazer-playlist");
}

static void build_poster_thumbs(void) {
    size_t poster_len = (size_t)(poster_end - poster_start);
    if (poster_len < 320 * 320 * 4) return;
    poster36 = (uint32_t*)malloc((size_t)36 * 36 * 4);
    poster48 = (uint32_t*)malloc((size_t)48 * 48 * 4);
    if (poster36)
        resample_bilinear((const uint32_t*)poster_start, 320, 320, poster36, 36, 36);
    if (poster48)
        resample_bilinear((const uint32_t*)poster_start, 320, 320, poster48, 48, 48);
}

// ---------- 扫描 ----------
#include "decode.h"

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
            // FAT32 路径不区分大小写：/music 与 /Music 是同一目录，
            // 必须用 strcasecmp 去重，否则同一首歌会因大小写不同被加入两次
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
static void ensure_track_meta(int idx) {
    if (idx < 0 || idx >= ntracks) return;
    Track* t = &tracks[idx];
    if (t->meta_state) return;
    t->meta_state = 1;  // 先标记，失败也不重复读盘

    if (!t->thumb) t->thumb = (uint32_t*)malloc((size_t)36 * 36 * 4);
    if (!t->thumb48) t->thumb48 = (uint32_t*)malloc((size_t)48 * 48 * 4);

    meta_load(t->path, &t->meta);
    if (t->meta.art && t->meta.art_w > 8 && t->meta.art_h > 8) {
        if (t->thumb)
            resample_bilinear((const uint32_t*)t->meta.art, t->meta.art_w, t->meta.art_h, t->thumb, 36, 36);
        if (t->thumb48)
            resample_bilinear((const uint32_t*)t->meta.art, t->meta.art_w, t->meta.art_h, t->thumb48, 48, 48);
        meta_release_art(&t->meta);
    } else {
        // 没有内嵌封面时统一使用 WUKONG 海报作为占位图
        if (t->thumb && poster36) memcpy(t->thumb, poster36, (size_t)36 * 36 * 4);
        else if (t->thumb) make_default_art(t->thumb, 36, t->path);
        if (t->thumb48 && poster48) memcpy(t->thumb48, poster48, (size_t)48 * 48 * 4);
        else if (t->thumb48) make_default_art(t->thumb48, 48, t->path);
    }
}

static void trim_thumbs(int focus_idx) {
    if (loaded_thumbs <= 256) return;
    for (int i = 0; i < ntracks; i++) {
        Track* t = &tracks[i];
        if (!t->meta_state || !t->thumb) continue;
        if (abs(i - focus_idx) <= 40) continue;
        free(t->thumb); t->thumb = NULL;
        free(t->thumb48); t->thumb48 = NULL;
        meta_clear(&t->meta);
        t->meta_state = 0;
        loaded_thumbs--;
    }
}

static void ensure_visible_thumbs(void) {
    // 每帧最多解析 2 首，避免首次进入列表时卡顿
    int n = 0;
    for (int i = 0; i < ROWS_VISIBLE && n < 2; i++) {
        int idx = scroll + i;
        if (idx >= ntracks) break;
        if (!tracks[idx].meta_state) {
            ensure_track_meta(idx);
            loaded_thumbs++;
            n++;
        }
    }
    trim_thumbs(sel);
}

// ---------- 曲目加载 ----------
static void load_track(int idx) {
    if (idx < 0 || idx >= ntracks) return;
    playing_idx = idx;
    loading = 1;
    show_pos = 0;
    audio_play(tracks[idx].path);

    // 底部播放条缩略图
    if (!tracks[idx].meta_state) {
        ensure_track_meta(idx);
        loaded_thumbs++;
    }

    meta_clear(&meta);
    if (lyr) { lyrics_free(lyr); lyr = NULL; }
    meta_load(tracks[idx].path, &meta);

    if (meta.title[0]) snprintf(p_title, sizeof(p_title), "%s", meta.title);
    else basename_no_ext(tracks[idx].path, p_title, sizeof(p_title));
    if (meta.artist[0]) snprintf(p_artist, sizeof(p_artist), "%s", meta.artist);
    else p_artist[0] = 0;

    if (meta.art && meta.art_w > 8 && meta.art_h > 8) {
        if (!cover) cover = (uint32_t*)malloc((size_t)380 * 380 * 4);
        if (cover) {
            // 保存旧封面用于交叉淡化
            if (!cover_prev) cover_prev = (uint32_t*)malloc((size_t)380 * 380 * 4);
            if (cover_prev) memcpy(cover_prev, cover, (size_t)380 * 380 * 4);
            anim_cover = 0;

            resample_bilinear((const uint32_t*)meta.art, meta.art_w, meta.art_h, cover, 380, 380);
            build_bg_from((const uint32_t*)meta.art, meta.art_w, meta.art_h);
        }
    } else {
        // 没有内嵌封面时统一使用 WUKONG 海报
        if (!cover) cover = (uint32_t*)malloc((size_t)380 * 380 * 4);
        if (cover) {
            if (!cover_prev) cover_prev = (uint32_t*)malloc((size_t)380 * 380 * 4);
            if (cover_prev) memcpy(cover_prev, cover, (size_t)380 * 380 * 4);
            anim_cover = 0;
        }
        if (cover && (size_t)(poster_end - poster_start) >= 320 * 320 * 4) {
            resample_bilinear((const uint32_t*)poster_start, 320, 320, cover, 380, 380);
            build_bg_from((const uint32_t*)poster_start, 320, 320);
        } else if (cover) {
            make_default_art(cover, 380, tracks[idx].path);
            build_default_bg(tracks[idx].path);
        }
    }

    lyr = lyrics_load(tracks[idx].path);
    for (int i = 0; i < 28; i++) { bars[i] = 0; bar_target[i] = 0; }
    loading = 0;
}

// ---------- 输入 ----------
static uint64_t repeat_keys(uint64_t down, uint64_t held, uint64_t mask) {
    static int hold_frames = 0;
    uint64_t out = down & mask;
    if (held & mask) {
        hold_frames++;
        if (hold_frames > 28 && (hold_frames % 5) == 0) out |= held & mask;
    } else {
        hold_frames = 0;
    }
    return out & mask;
}

// ---------- 触摸交互 ----------
static int in_rect(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static int pick_next(int cur) {
    if (ntracks <= 1) return 0;
    return (cur + 1) % ntracks;
}

static void seek_from_touch(int x, int x0, int x1) {
    if (audio_duration() <= 0) return;
    if (x < x0) x = x0;
    if (x > x1) x = x1;
    double frac = (double)(x - x0) / (double)(x1 - x0);
    audio_seek(frac * audio_duration());
}

// 切换列表视图时立即重建可见曲目，避免按键/触摸读到上一页数据
static void set_view(int v) {
    if (view == v) return;
    view = v;
    scroll = 0;
    anim_scroll = 0;
    sel = 0;
    rebuild_visible();
}

// 手柄 L/R 循环切换五个视图
static void cycle_view(int dir) {
    int v = view + dir;
    if (v < VIEW_HOME) v = VIEW_SETTINGS;
    if (v > VIEW_SETTINGS) v = VIEW_HOME;
    set_view(v);
}

static void toggle_fav(int idx) {
    if (idx < 0 || idx >= ntracks) return;
    tracks[idx].fav = !tracks[idx].fav;
    fav_count += tracks[idx].fav ? 1 : -1;
    if (fav_count < 0) fav_count = 0;
    rebuild_visible();
}

static void handle_touch_list(TouchInfo* t) {
    // 拖动列表滚动
    if (touch_drag && t->held && nvisible > ROWS_VISIBLE) {
        touch_drag_accum += touch_last_y - t->y;
        int rows = touch_drag_accum / ROW_H;
        if (rows != 0) {
            touch_drag_accum -= rows * ROW_H;
            int maxscroll = nvisible - ROWS_VISIBLE;
            scroll += rows;
            if (scroll < 0) scroll = 0;
            if (scroll > maxscroll) scroll = maxscroll;
            if (sel < scroll) sel = scroll;
            if (sel >= scroll + ROWS_VISIBLE) sel = scroll + ROWS_VISIBLE - 1;
        }
        touch_last_y = t->y;
    }

    if (!t->down) {
        if (t->up) {
            if (touch_drag) {
                if (touch_press_idx >= 0 && touch_press_idx < nvisible &&
                    abs(t->y - touch_drag_start_y) < 12) {
                    sel = vis_at(touch_press_idx);
                    if (sel >= 0 && !loading) {
                        load_track(sel);
                        switch_page(UI_PLAYER);   // 点击歌曲直接显示大封面播放页
                    }
                }
                touch_drag = 0;
                touch_press_idx = -1;
            } else if (t->y >= BE_H - BAR_H) {
                // 底部播放条：点击进入沉浸播放页
                if (playing_idx < 0 && nvisible > 0) {
                    int first = vis_at(0);
                    if (first >= 0 && !loading) load_track(first);
                }
                if (playing_idx >= 0) switch_page(UI_PLAYER);
            }
        }
        return;
    }

    int x = t->x, y = t->y;
    touch_fx_x = x; touch_fx_y = y; touch_fx_alpha = 1;

    if (y >= BE_H - BAR_H) return;   // 底部播放条在松开时处理

    // 歌曲列表行：按下记录，松开且未拖动时播放
    if (nvisible > 0 && y >= LIST_TOP && x >= 20 && x <= 1260) {
        int pos = scroll + (y - LIST_TOP) / ROW_H;
        if (pos >= 0 && pos < nvisible) {
            touch_press_idx = pos;
            sel = vis_at(pos);
            touch_drag = 1;
            touch_drag_start_y = y;
            touch_last_y = y;
            touch_drag_accum = 0;
        }
    }
}

static void handle_touch_player(TouchInfo* t) {
    if (touch_scrub && t->held)
        seek_from_touch(t->x, 90, 1190);

    if (!t->down) {
        if (t->up) touch_scrub = 0;
        return;
    }

    int x = t->x, y = t->y;
    touch_fx_x = x; touch_fx_y = y; touch_fx_alpha = 1;

    if (y < 56) {                       // 顶部返回
        switch_page(UI_LIST);
        sel = playing_idx >= 0 ? playing_idx : sel;
        if (sel < scroll) scroll = sel;
        if (sel >= scroll + ROWS_VISIBLE) scroll = sel - ROWS_VISIBLE + 1;
        rebuild_visible();
        return;
    }
    if (in_rect(x, y, 90, 598, 1100, 36)) {   // 进度条
        touch_scrub = 1;
        seek_from_touch(x, 90, 1190);
        return;
    }
    if (y >= 640) {
        if (in_rect(x, y, 470, 640, 90, 70)) {
            if (playing_idx > 0) load_track(playing_idx - 1);
        } else if (in_rect(x, y, 590, 640, 100, 70)) {
            if (audio_paused()) audio_resume(); else audio_pause();
        } else if (in_rect(x, y, 720, 640, 90, 70)) {
            if (ntracks > 0) load_track(pick_next(playing_idx));
        }
        return;
    }
    // 画面分区：左切歌 / 中暂停 / 右切歌
    if (x < 400) {
        if (playing_idx > 0) load_track(playing_idx - 1);
    } else if (x > 880) {
        if (ntracks > 0) load_track(pick_next(playing_idx));
    } else {
        if (audio_paused()) audio_resume(); else audio_pause();
    }
}

// 触点反馈：按下时触发，随时间淡出（圆形，主题主色，柔和不突兀）
static void draw_touch_feedback(void) {
    if (touch_fx_alpha > 0) {
        int r = 18 + (int)(10 * (1.0f - touch_fx_alpha));   // 半径随淡出略微扩散
        draw_ellipse_a(screen, (int)touch_fx_x, (int)touch_fx_y, r, r,
                       C_PRIMARY, (uint8_t)(85 * touch_fx_alpha));
    }
}

// ---------- 列表页（平板分栏 + Material You） ----------
// ---------- 列表页（极简单列 + 底部播放条） ----------
static void draw_list(void) {
    canvas_clear(screen, C_BG);
    ensure_visible_thumbs();

    // 顶部标题栏
    draw_ellipse(screen, 40, 40, 15, 15, C_PRIMARY);
    draw_text(screen, font, 20, 66, 28, C_TXT, "Siren");
    char sub[64];
    snprintf(sub, sizeof(sub), "%d 首歌曲", ntracks);
    draw_text_right(screen, font, 15, 1256, 32, C_TXT3, sub);

    if (nvisible == 0) {
        draw_text_centered(screen, font, 22, 0, BE_W, 330, C_TXT2,
                           ntracks == 0 ? "未在 SD 卡找到音乐文件 · 可将歌曲放入 /music 目录" : "暂无歌曲");
        draw_touch_feedback();
        return;
    }

    // 歌曲列表（带小封面缩略图）
    int list_bottom = BE_H - BAR_H;
    int i0 = (int)floorf(anim_scroll) - 1;
    if (i0 < 0) i0 = 0;
    for (int i = i0; i < ROWS_VISIBLE + i0 + 2; i++) {
        int idx = vis_at(i);
        if (idx < 0) break;
        int y = LIST_TOP + (int)((i - anim_scroll) * ROW_H);
        if (y + 56 < LIST_TOP - 4 || y > list_bottom - 4) continue;

        uint32_t card = (idx % 2) ? C_CARD2 : C_CARD;
        if (idx == playing_idx) card = C_PLAYBG;
        else if (idx == sel) card = C_SURF2;
        fill_round_rect(screen, 20, y, 1240, 56, 14, card);

        // 小封面缩略图（无内嵌封面时用内嵌海报占位）
        if (tracks[idx].thumb48)
            blit_rgba_round(screen, 28, y + 4, tracks[idx].thumb48, 48, 48, 12);

        char nm[256];
        basename_no_ext(tracks[idx].path, nm, sizeof(nm));
        const char* ttl = tracks[idx].meta.title[0] ? tracks[idx].meta.title : nm;
        const char* art = tracks[idx].meta.artist[0] ? tracks[idx].meta.artist : "";
        uint32_t tc = (idx == playing_idx) ? C_PRIMARY : C_TXT;
        draw_text_max(screen, font, 20, 90, y + 6, tc, ttl, 960);
        if (art[0])
            draw_text_max(screen, font, 14, 90, y + 32, C_TXT3, art, 960);

        if (idx == playing_idx && audio_duration() > 0) {
            char dur[16];
            fmt_time(audio_duration(), dur, sizeof(dur));
            draw_text_right(screen, font, 15, 1240, y + 20, C_TXT3, dur);
        }
    }

    // 底部播放条：当前歌曲 + 播放状态，点击进沉浸页
    fill_rect(screen, 0, BE_H - BAR_H, BE_W, BAR_H, C_SURFACE);
    fill_rect(screen, 0, BE_H - BAR_H, BE_W, 1, C_LINE);
    if (playing_idx >= 0) {
        if (tracks[playing_idx].thumb)
            blit_rgba_round(screen, 14, BE_H - BAR_H + 6, tracks[playing_idx].thumb, 36, 36, 8);
        draw_text_max(screen, font, 16, 62, BE_H - BAR_H + 5,
                      C_TXT, p_title[0] ? p_title : "Siren", 900);
        if (p_artist[0])
            draw_text_max(screen, font, 13, 62, BE_H - BAR_H + 26, C_TXT3, p_artist, 900);
        if (audio_paused())
            fill_tri(screen, 1214, BE_H - BAR_H + 14, 1214, BE_H - BAR_H + 34, 1232, BE_H - BAR_H + 24, C_PRIMARY);
        else {
            fill_rect(screen, 1210, BE_H - BAR_H + 14, 5, 20, C_PRIMARY);
            fill_rect(screen, 1221, BE_H - BAR_H + 14, 5, 20, C_PRIMARY);
        }
    } else {
        draw_text(screen, font, 15, 16, BE_H - BAR_H + 16, C_TXT3,
                  "点击歌曲开始播放");
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
        if (pos < scroll) scroll = pos;
    }
    if (rep & (HidNpadButton_Down | HidNpadButton_StickLDown)) {
        if (pos < nvisible - 1) pos++;
        sel = vis_at(pos);
        if (pos >= scroll + ROWS_VISIBLE) scroll = pos - ROWS_VISIBLE + 1;
    }
    // L / ← / ZL：上一首；R / → / ZR：下一首
    if (down & (HidNpadButton_L | HidNpadButton_Left | HidNpadButton_ZL)) {
        int p = playing_idx > 0 ? playing_idx - 1 : (ntracks > 0 ? 0 : -1);
        if (p >= 0) { load_track(p); sel = p; }
    }
    if (down & (HidNpadButton_R | HidNpadButton_Right | HidNpadButton_ZR)) {
        if (ntracks > 0) { int p = pick_next(playing_idx); load_track(p); sel = p; }
    }
    if ((down & HidNpadButton_A) && !loading) {
        int i = vis_at(pos);
        if (i >= 0) { load_track(i); switch_page(UI_PLAYER); }
    }
    if (down & HidNpadButton_X) {
        if (playing_idx < 0 && nvisible > 0) {
            int i = vis_at(pos);
            if (i >= 0 && !loading) load_track(i);
        }
        switch_page(UI_PLAYER);
    }
}

// ---------- 沉浸播放页（Material You） ----------
static void draw_player(void) {
    if (bg) memcpy(screen->px, bg->px, (size_t)BE_W * BE_H * 4);
    else canvas_clear(screen, C_BG);

    // 暗化叠加（tonal 氛围）
    fill_rect_a(screen, 0, 0, BE_W, BE_H, COL(8, 10, 12, 255), 70);

    draw_text_centered(screen, font, 16, 900, 1240, 30, C_PRIMARY, "正在播放");

    // 大圆角封面 + 阴影
    int cx = 110, cy = 110, csz = 384;
    fill_round_rect(screen, cx + 6, cy + 8, csz, csz, 32, COL(0, 0, 0, 255));
    if (anim_cover < 1 && cover_prev) {
        float t = ease_out(anim_cover);
        blit_rgba_round_a(screen, cx, cy, cover_prev, csz, csz, 32, (uint8_t)((1 - t) * 255));
        if (cover) blit_rgba_round_a(screen, cx, cy, cover, csz, csz, 32, (uint8_t)(t * 255));
    } else if (cover) {
        blit_rgba_round(screen, cx, cy, cover, csz, csz, 32);
    }

    // 曲名 / 艺术家
    draw_text_max(screen, font, 52, 560, 150, C_TXT, p_title, 640);
    char artist_line[512];
    if (p_artist[0]) snprintf(artist_line, sizeof(artist_line), "%s", p_artist);
    else artist_line[0] = 0;
    draw_text_max(screen, font, 20, 562, 224, C_TXT2, artist_line, 640);

    double pos = show_pos;
    double dur = audio_duration();
    if (pos > dur) pos = dur;

    // 歌词 / 频谱
    int x0 = 620, x1 = 1200;
    if (lyr) {
        int act = lyrics_index(lyr, pos);
        int idxs2[9];
        int n = 0;
        for (int d = -3; d <= 4; d++) {
            int i = act + d;
            if (i >= 0 && i < lyr->n) idxs2[n++] = i;
        }
        int y = 330;
        for (int k = 0; k < n; k++) {
            int i = idxs2[k];
            int dist = (act >= 0) ? (i - act) : 4;
            if (dist == 0) {
                draw_text_centered(screen, font, 26, x0, x1, y, C_PRIMARY, lyr->lines[i].text);
                y += 66;
            } else {
                float px = (dist == -1 || dist == 1) ? 20 : ((dist == -2 || dist == 2) ? 18 : 16);
                uint32_t cc = (dist == -1 || dist == 1) ? C_TXT2 : C_TXT3;
                draw_text_centered(screen, font, px, x0, x1, y, cc, lyr->lines[i].text);
                y += (int)px + 24;
            }
        }
    } else {
        float sp[28];
        audio_get_spectrum(sp);
        for (int i = 0; i < 28; i++) {
            bar_target[i] = sp[i];
            bars[i] += (bar_target[i] - bars[i]) * 0.25f;
        }
        int n = 20, bw = 14;
        int gap = (x1 - x0 - 100 - n * bw) / (n - 1);
        int basey = 500;
        for (int i = 0; i < n; i++) {
            float v = bars[i];
            if (v < 0.02f) v = 0.02f;
            int hgt = (int)(v * 180);
            int bx = x0 + 50 + i * (bw + gap);
            fill_rect_a(screen, bx, basey - hgt, bw, hgt, C_PRIMARY, (uint8_t)(110 + v * 140));
        }
        draw_text_centered(screen, font, 16, x0, x1, 540, C_TXT3,
                           "无歌词 · 可放同名 .lrc 文件到歌曲目录");
    }

    // 进度条（Material pill）
    int py = 628, px0 = 110, px1 = 1170;
    fill_round_rect(screen, px0, py, px1 - px0, 6, 3, C_LINE);
    int w = px1 - px0;
    int fillw = (int)((dur > 0 ? pos / dur : 0) * w);
    if (fillw < 0) fillw = 0;
    if (fillw > w) fillw = w;
    if (fillw > 0) fill_round_rect(screen, px0, py, fillw, 6, 3, C_PRIMARY);
    draw_ellipse(screen, px0 + fillw, py + 3, 8, 8, C_PRIMARY);

    char t1[32], t2[32];
    fmt_time(pos, t1, sizeof(t1));
    fmt_time(dur, t2, sizeof(t2));
    draw_text(screen, font, 15, px0, py + 18, C_TXT2, t1);
    draw_text_right(screen, font, 15, px1, py + 18, C_TXT2, t2);

    // 控制按钮（圆形 tonal + 胶囊主按钮）
    int cy2 = 684;
    draw_ellipse(screen, 560, cy2, 22, 22, C_SURF2);
    icon_prev(screen, 550, cy2 - 10, 20, C_TXT);
    draw_ellipse(screen, 640, cy2, 30, 30, C_PRIMARY);
    if (audio_paused()) {
        fill_tri(screen, 632, 673, 632, 695, 650, 684, C_ONPRIM);
    } else {
        fill_rect(screen, 630, 673, 6, 22, C_ONPRIM);
        fill_rect(screen, 644, 673, 6, 22, C_ONPRIM);
    }
    draw_ellipse(screen, 720, cy2, 22, 22, C_SURF2);
    icon_next(screen, 710, cy2 - 10, 20, C_TXT);

    draw_text_right(screen, font, 15, 1240, 676, C_TXT3,
                    "A 播放/暂停 · B 返回 · L/R 切歌 · Y 自检");

    if (audio_status() == 3) {
        draw_text_centered(screen, font, 24, 0, 1280, 320, COL(255, 140, 130, 255), "解码失败：文件不支持或损坏");
        draw_text_centered(screen, font, 18, 0, 1280, 356, C_TXT2, p_title);
    }

    if (vol_overlay_timer > 0) {
        vol_overlay_timer--;
        char vs[64];
        snprintf(vs, sizeof(vs), "音量 %d%%", (int)(audio_volume() * 100 + 0.5));
        int bw2 = 240;
        fill_round_rect(screen, 520, 80, bw2, 64, 18, C_SURFACE);
        draw_text_centered(screen, font, 22, 520, 760, 90, C_TXT, vs);
        fill_round_rect(screen, 550, 144, 180, 6, 3, C_LINE);
        fill_round_rect(screen, 550, 144, (int)(180 * audio_volume()), 6, 3, C_PRIMARY);
    }

    if (loading)
        draw_text_centered(screen, font, 24, 0, 1280, 340, C_TXT2, "加载中…");

    draw_touch_feedback();
}

static void draw_diag(void) {
    fill_rect_a(screen, 0, 0, BE_W, BE_H, COL(0, 0, 0, 255), 210);
    draw_text(screen, font, 34, 60, 50, C_RED_SOFT, "音频自检 (按 Y 或点击任意处关闭)");
    char buf[1024];
    audio_get_diag(buf, sizeof(buf));
    int yy = 120;
    char* line = strtok(buf, "\n");
    while (line) {
        draw_text(screen, font, 26, 60, yy, C_TXT, line);
        yy += 44;
        line = strtok(NULL, "\n");
    }
    draw_text(screen, font, 20, 60, 660, C_TXT3, "若 状态=FAILED 或 设备=0 请截图此页反馈");
}

static void ui_player_input(uint64_t down, uint64_t rep) {
    if (down & HidNpadButton_A) {
        if (audio_paused()) audio_resume(); else audio_pause();
    }
    if (down & HidNpadButton_B) {
        switch_page(UI_LIST);
        sel = playing_idx >= 0 ? playing_idx : sel;
        if (sel < scroll) scroll = sel;
        if (sel >= scroll + ROWS_VISIBLE) scroll = sel - ROWS_VISIBLE + 1;
    }
    // X / ZL：收藏当前歌曲（对应播放页爱心）
    if (down & (HidNpadButton_X | HidNpadButton_ZL)) {
        if (playing_idx >= 0) toggle_fav(playing_idx);
    }
    if (down & HidNpadButton_ZR) {
        if (ntracks > 0) load_track(pick_next(playing_idx));
    }
    if (down & HidNpadButton_L) {
        int next = playing_idx > 0 ? playing_idx - 1 : ntracks - 1;
        load_track(next);
    }
    if (down & HidNpadButton_R) {
        int next = pick_next(playing_idx);
        load_track(next);
    }
    if (rep & (HidNpadButton_Left | HidNpadButton_StickLLeft)) {
        double p = audio_position() - 10;
        if (p < 0) p = 0;
        audio_seek(p);
    }
    if (rep & (HidNpadButton_Right | HidNpadButton_StickRRight | HidNpadButton_StickLRight)) {
        audio_seek(audio_position() + 10);
    }
    if (down & HidNpadButton_Up) {
        audio_set_volume(audio_volume() + 0.05f);
        vol_overlay_timer = 60;
    }
    if (down & HidNpadButton_Down) {
        audio_set_volume(audio_volume() - 0.05f);
        vol_overlay_timer = 60;
    }
}

// ---------- main ----------
int main(int argc, char** argv) {
    (void)argc; (void)argv;

    if (be_init() != 0) return 0;
    load_theme_setting();
    if (audio_init() != 0) {
        // 音频不可用时仍显示界面
    }

    screen = canvas_new(BE_W, BE_H);
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
        // 排序后兜底去重：路径大小写视为相同即为重复（FAT32 同一文件）
        int w = 0;
        for (int i = 0; i < ntracks; i++) {
            if (w > 0 && !strcasecmp(tracks[w - 1].path, tracks[i].path)) continue;
            if (w != i) tracks[w] = tracks[i];
            w++;
        }
        ntracks = w;
    }
    rebuild_visible();

    build_poster_thumbs();
    build_playlist_cover();
    memset(&meta, 0, sizeof(meta));

    int stride = 0;
    while (be_should_run()) {
        anim_frame_begin();
        be_input_update();
        be_touch_update(&touch_now);
        uint64_t down = be_btn_down();
        uint64_t held = be_btn_held();
        uint64_t rep = repeat_keys(down, held,
            HidNpadButton_Up | HidNpadButton_Down | HidNpadButton_Left | HidNpadButton_Right);

        if (down & HidNpadButton_Plus) break;
        if (down & HidNpadButton_Y) show_diag = !show_diag;

        if (show_diag) {
            if (touch_now.down) show_diag = 0;   // 自检页点任意处关闭
        } else if (ui_state == UI_LIST) {
            ui_list_input(down, rep);
            handle_touch_list(&touch_now);
        } else {
            ui_player_input(down, rep);
            handle_touch_player(&touch_now);
        }

        if (audio_finished() && playing_idx >= 0 && !loading) {
            load_track(pick_next(playing_idx));
        }

        audio_pump();

        // ---- 动画状态推进 ----
        anim_scroll = approach(anim_scroll, (float)scroll, 16.0f, g_dt);
        anim_sel = approach(anim_sel, (float)sel, 18.0f, g_dt);
        if (anim_cover < 1) anim_cover = (float)(anim_cover + g_dt / 0.32);
        if (anim_cover > 1) anim_cover = 1;
        if (trans_t < 1) trans_t = (float)(trans_t + g_dt / 0.24);
        if (trans_t > 1) trans_t = 1;
        if (touch_fx_alpha > 0) {
            touch_fx_alpha -= (float)(g_dt * 2.2);
            if (touch_fx_alpha < 0) touch_fx_alpha = 0;
        }
        // 播放进度平滑（暂停时保持，拖动时由 seek 同步）
        double ap = audio_position();
        show_pos = (double)approach((float)show_pos, (float)ap, 9.0f, g_dt);
        if (show_pos < 0) show_pos = 0;
        if (fabs(show_pos - ap) > 3.0) show_pos = ap;   // 跳变直接跟随

        uint32_t* fb = be_begin(&stride);
        if (ui_state == UI_LIST) draw_list();
        else draw_player();
        if (show_diag) draw_diag();
        // 页面切换：旧页面截图淡出，实现交叉淡化
        if (trans_t < 1 && trans_shot) {
            uint8_t a = (uint8_t)((1 - ease_out(trans_t)) * 255);
            blit_rgba_a(screen, 0, 0, trans_shot->px, BE_W, BE_H, a);
        }
        for (int y = 0; y < BE_H; y++) {
            uint32_t* dst = fb + (size_t)y * stride;
            uint32_t* src = screen->px + (size_t)y * BE_W;
            memcpy(dst, src, (size_t)BE_W * 4);
        }
        be_end();
    }

    audio_exit();
    be_exit();
    if (font) font_free(font);
    if (screen) canvas_free(screen);
    if (bg) canvas_free(bg);
    if (cover) free(cover);
    if (cover_prev) free(cover_prev);
    if (trans_shot) canvas_free(trans_shot);
    if (cover_small) free(cover_small);
    if (poster36) free(poster36);
    if (poster48) free(poster48);
    meta_clear(&meta);
    if (lyr) lyrics_free(lyr);
    for (int i = 0; i < ntracks; i++) {
        free(tracks[i].thumb);
        free(tracks[i].thumb48);
        meta_clear(&tracks[i].meta);
    }
    free(tracks);
    return 0;
}
