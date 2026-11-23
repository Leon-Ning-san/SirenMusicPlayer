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

// ---------- 屏幕方向（横屏 / 竖屏自动旋转） ----------
#define ORI_LAND   0   // 正常横持
#define ORI_PORT_A 1   // 顺时针竖持
#define ORI_PORT_B 2   // 逆时针竖持
static int ori_mode = 0;       // 0=自动 1=横屏 2=竖A 3=竖B
static int ori = ORI_LAND;
static int SCR_W = 1280, SCR_H = 720;
static int portrait(void) { return ori != ORI_LAND; }

// ---------- 布局（Material 大卡片单列） ----------
static int L_TOP, L_BAR, L_ROW, L_THUMB, L_M, L_TITLE, L_ART, L_ARTIST2;
static void update_layout(void) {
    L_M = 24;
    L_TOP = 96;
    L_THUMB = 96;
    if (portrait()) { L_BAR = 84;  L_ROW = 168; L_TITLE = 30; L_ART = 19; L_ARTIST2 = 15; }
    else            { L_BAR = 60;  L_ROW = 130; L_TITLE = 26; L_ART = 17; L_ARTIST2 = 13; }
}
// 当前可视行数（横屏 4 行大卡 / 竖屏 6 行大卡，比旧版少而大）
static int rows_visible(void) {
    int area = SCR_H - L_TOP - L_BAR;
    int r = area / L_ROW;
    return r < 1 ? 1 : r;
}

// ---------- 主题配色 ----------
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
    uint32_t* thumb;    // 36x36 底部播放条缩略图
    uint32_t* thumb96;  // 96x96 列表大卡片缩略图
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
static Canvas* list_bg = NULL;   // 列表页渐变背景缓存（半透明卡片透出）

// 播放页资源
static TrackMeta meta;
static Lyrics* lyr = NULL;
static uint32_t* cover = NULL;       // 380x380 当前曲目大图
static uint32_t* cover_small = NULL; // 140x140 歌单封面
static uint32_t* poster36 = NULL;    // 海报缩略图 36x36
static uint32_t* poster96 = NULL;    // 海报缩略图 96x96
static Canvas* bg = NULL;
static char p_title[256];
static char p_artist[256];
static float bars[28];
static float bar_target[28];

static int vol_overlay_timer = 0;
static int show_diag = 0;

// ---------- 提示气泡（方向切换等） ----------
static char toast_text[96];
static int toast_timer = 0;
static void show_toast(const char* s) {
    snprintf(toast_text, sizeof(toast_text), "%s", s);
    toast_timer = 90;
}

// ---------- 动画 ----------
static double g_time = 0;
static double g_dt = 0.016;
static uint64_t g_last_tick = 0;
static float anim_scroll = 0;      // 平滑滚动位置（行，浮点）
static float anim_cover = 0;       // 切歌封面交叉淡化 0->1
static int cover_ready = 0;
static uint32_t* cover_prev = NULL;
static Canvas* trans_shot = NULL;
static float trans_t = 1.0f;
static float touch_fx_alpha = 0;
static float touch_fx_x = 0, touch_fx_y = 0;
static double show_pos = 0;

// Material 水波纹
static float ripple_t = 1.0f;      // >=1 表示无水波纹
static int ripple_x, ripple_y;
static int ripple_row = -1;        // 触发行（绘制时按滚动动画位置实时换算，滚动中不漂移）
static int ripple_cw, ripple_ch;

// 播放页交互布局（draw_player 填充，handle_touch_player 使用）
static int ui_px0, ui_px1, ui_py;      // 进度条
static int ui_cx, ui_cy, ui_gap;       // 控制按钮中心 / 间距

// ---------- 触摸交互状态 ----------
static TouchInfo touch_now;
static int touch_drag = 0;
static int touch_scrub = 0;
static int touch_drag_start_y = 0;
static int touch_last_y = 0;
static int touch_drag_accum = 0;
static int touch_press_idx = -1;
static char search_q[128] = "";
static int fav_count = 0;

// ---------- 六轴加速度计（竖屏自动检测） ----------
static HidSixAxisSensorHandle g_six_handle;
static int g_six_ok = 0;
static int ori_cand = ORI_LAND;
static int ori_hold = 0;

static void sixaxis_init(void) {
    g_six_ok = R_SUCCEEDED(hidGetSixAxisSensorHandles(&g_six_handle, 1,
                    HidNpadIdType_No1, HidNpadStyleTag_NpadHandheld));
}

// 依据加速度判断候选方向（带阈值；持平时返回当前方向）
static int ori_read_candidate(void) {
    if (!g_six_ok) return ori;
    HidSixAxisSensorState st[1];
    size_t n = hidGetSixAxisSensorStates(g_six_handle, st, 1);
    if (n < 1) return ori;
    if (!(st[0].attributes & HidSixAxisSensorAttribute_IsConnected)) return ori;
    float ax = st[0].acceleration.x, ay = st[0].acceleration.y;
    // 手持横放时重力沿 y 轴；竖放时沿 x 轴；平放时 z 轴（不触发旋转）
    if (ax < -0.65f && ay < 0.5f)  return ORI_PORT_A;   // 顺时针竖持
    if (ax >  0.65f && ay < 0.5f)  return ORI_PORT_B;   // 逆时针竖持
    if (ay >  0.65f && ax < 0.5f)  return ORI_LAND;
    if (ay < -0.65f && ax < 0.5f)  return ORI_LAND;     // 倒置横持仍按横屏
    return ori;
}

// 屏幕尺寸/画布随方向变化
static void build_bg_from(const uint32_t* art, int aw, int ah);
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
    scroll = 0;
    anim_scroll = 0;
    ripple_t = 1;
    touch_fx_alpha = 0;
    touch_drag = 0;
    touch_press_idx = -1;
    // 背景（模糊封面 / 渐变）随尺寸变化重建
    if (list_bg) { canvas_free(list_bg); list_bg = NULL; }
    // 竖/横切换后立即重建模糊背景，避免黑屏
    if (!bg && cover_ready && cover) build_bg_from(cover, 380, 380);
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
    int maxscroll = nvisible - rows_visible();
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
    if (pos >= scroll + rows_visible()) scroll = pos - rows_visible() + 1;
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
        if (d > 0.05) d = 0.05;
        g_dt = d;
    }
    g_last_tick = now;
    g_time += g_dt;
}
// 切换页面时截取旧画面，用于 Material 淡出上滑过渡
static void switch_page(int to) {
    if (to == ui_state) return;
    if (!trans_shot) trans_shot = canvas_new(SCR_W, SCR_H);
    if (trans_shot && screen)
        memcpy(trans_shot->px, screen->px, (size_t)SCR_W * SCR_H * 4);
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

// ---------- 图标 ----------
static void icon_prev(Canvas* c, int x, int y, int s, uint32_t col) {
    fill_tri_aa(c, x + s * 3 / 4, y, x + s * 3 / 4, y + s, x + s / 8, y + s / 2, col);
    fill_rect(c, x, y, s / 8, s, col);
}
static void icon_next(Canvas* c, int x, int y, int s, uint32_t col) {
    fill_tri_aa(c, x + s / 4, y, x + s * 7 / 8, y + s / 2, x + s / 4, y + s, col);
    fill_rect(c, x + s * 7 / 8, y, s / 8, s, col);
}
static void icon_play(Canvas* c, int x, int y, int s, uint32_t col) {
    fill_tri_aa(c, x, y, x, y + s, x + s, y + s / 2, col);
}
static void icon_pause(Canvas* c, int x, int y, int s, uint32_t col) {
    fill_rect(c, x, y, s * 3 / 10, s, col);
    fill_rect(c, x + s * 7 / 10, y, s * 3 / 10, s, col);
}
static void icon_heart(Canvas* c, int x, int y, int s, uint32_t col) {
    int rx = s / 4;
    fill_ellipse_aa(c, x + s * 3 / 10, y + s * 3 / 10, rx, rx, col);
    fill_ellipse_aa(c, x + s * 7 / 10, y + s * 3 / 10, rx, rx, col);
    fill_tri_aa(c, x + s / 10, y + s * 2 / 5, x + s * 9 / 10, y + s * 2 / 5, x + s / 2, y + s, col);
}
static void icon_speaker(Canvas* c, int x, int y, int s, uint32_t col) {
    fill_rect(c, x, y + s / 3, s / 4, s / 3, col);
    fill_tri(c, x + s / 4, y + s / 3, x + s * 3 / 4, y, x + s * 3 / 4, y + s, col);
    hline(c, x + s * 3 / 4 + 2, y + s / 2, s / 5, col);
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
    if (!bg) bg = canvas_new(SCR_W, SCR_H);
    // 中间缩略按屏幕纵横比取尺寸，避免竖屏时背景被拉伸
    int iw = portrait() ? 36 : 64;
    int ih = portrait() ? 64 : 36;
    uint32_t small[64 * 64];
    resample_bilinear(art, aw, ah, small, iw, ih);
    box_blur(small, iw, ih, 3, 2);
    resample_bilinear(small, iw, ih, bg->px, bg->w, bg->h);
    scale_rgb(bg->px, (size_t)bg->w * bg->h, 0.38f);
    for (int y = 0; y < bg->h; y++) {
        for (int x = 0; x < bg->w; x++) {
            float dx = (x - bg->w / 2.0f) / (bg->w / 2.0f);
            float dy = (y - bg->h / 2.0f) / (bg->h / 2.0f);
            float d = sqrtf(dx * dx + dy * dy);
            float f = 1.0f - 0.42f * d * d;
            if (f < 0) f = 0;
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

static void build_playlist_cover(void) {
    if (!cover_small) cover_small = (uint32_t*)malloc((size_t)140 * 140 * 4);
    if (!cover_small) return;

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
    make_default_art(cover_small, 140, "siren-playlist");
}

static void build_poster_thumbs(void) {
    size_t poster_len = (size_t)(poster_end - poster_start);
    if (poster_len < 320 * 320 * 4) return;
    poster36 = (uint32_t*)calloc(36 * 36, 4);
    poster96 = (uint32_t*)calloc(96 * 96, 4);
    if (poster36)
        resample_bilinear((const uint32_t*)poster_start, 320, 320, poster36, 36, 36);
    if (poster96)
        resample_bilinear((const uint32_t*)poster_start, 320, 320, poster96, 96, 96);
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
    t->meta_state = 1;

    if (!t->thumb) t->thumb = (uint32_t*)calloc(36 * 36, 4);
    if (!t->thumb96) t->thumb96 = (uint32_t*)calloc(96 * 96, 4);

    meta_load(t->path, &t->meta);
    if (t->meta.art && t->meta.art_w > 8 && t->meta.art_h > 8) {
        if (t->thumb)
            resample_bilinear((const uint32_t*)t->meta.art, t->meta.art_w, t->meta.art_h, t->thumb, 36, 36);
        if (t->thumb96)
            resample_bilinear((const uint32_t*)t->meta.art, t->meta.art_w, t->meta.art_h, t->thumb96, 96, 96);
        meta_release_art(&t->meta);
    } else {
        // 没有内嵌封面时统一使用内嵌海报作为占位图
        if (t->thumb && poster36) memcpy(t->thumb, poster36, (size_t)36 * 36 * 4);
        else if (t->thumb) make_default_art(t->thumb, 36, t->path);
        if (t->thumb96 && poster96) memcpy(t->thumb96, poster96, (size_t)96 * 96 * 4);
        else if (t->thumb96) make_default_art(t->thumb96, 96, t->path);
    }
}

static void trim_thumbs(int focus_idx) {
    if (loaded_thumbs <= 256) return;
    for (int i = 0; i < ntracks; i++) {
        Track* t = &tracks[i];
        if (!t->meta_state || !t->thumb) continue;
        if (abs(i - focus_idx) <= 40) continue;
        free(t->thumb); t->thumb = NULL;
        free(t->thumb96); t->thumb96 = NULL;
        meta_clear(&t->meta);
        t->meta_state = 0;
        loaded_thumbs--;
    }
}

static void ensure_visible_thumbs(void) {
    // 每帧最多解析 3 首，避免首次进入列表时卡顿
    int n = 0;
    for (int i = 0; i < rows_visible() && n < 3; i++) {
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
        if (!cover) cover = (uint32_t*)calloc((size_t)380 * 380, 4);
        if (cover) {
            // 保存旧封面用于交叉淡化；首播时 cover 还没有有效内容，跳过淡化
            if (!cover_prev) cover_prev = (uint32_t*)calloc((size_t)380 * 380, 4);
            if (cover_prev) {
                if (cover_ready) {
                    memcpy(cover_prev, cover, (size_t)380 * 380 * 4);
                    anim_cover = 0;
                } else {
                    memset(cover_prev, 0, (size_t)380 * 380 * 4);
                    anim_cover = 1;
                }
            }

            resample_bilinear((const uint32_t*)meta.art, meta.art_w, meta.art_h, cover, 380, 380);
            build_bg_from((const uint32_t*)meta.art, meta.art_w, meta.art_h);
            cover_ready = 1;
        }
    } else {
        // 没有内嵌封面时统一使用内嵌海报
        if (!cover) cover = (uint32_t*)calloc((size_t)380 * 380, 4);
        if (cover) {
            if (!cover_prev) cover_prev = (uint32_t*)calloc((size_t)380 * 380, 4);
            if (cover_prev) {
                if (cover_ready) {
                    memcpy(cover_prev, cover, (size_t)380 * 380 * 4);
                    anim_cover = 0;
                } else {
                    memset(cover_prev, 0, (size_t)380 * 380 * 4);
                    anim_cover = 1;
                }
            }
        }
        if (cover && (size_t)(poster_end - poster_start) >= 320 * 320 * 4) {
            resample_bilinear((const uint32_t*)poster_start, 320, 320, cover, 380, 380);
            build_bg_from((const uint32_t*)poster_start, 320, 320);
            cover_ready = 1;
        } else if (cover) {
            make_default_art(cover, 380, tracks[idx].path);
            build_default_bg(tracks[idx].path);
            cover_ready = 1;
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

static void set_view(int v) {
    if (view == v) return;
    view = v;
    scroll = 0;
    anim_scroll = 0;
    sel = 0;
    rebuild_visible();
}

static void toggle_fav(int idx) {
    if (idx < 0 || idx >= ntracks) return;
    tracks[idx].fav = !tracks[idx].fav;
    fav_count += tracks[idx].fav ? 1 : -1;
    if (fav_count < 0) fav_count = 0;
    rebuild_visible();
}

static void handle_touch_list(TouchInfo* t) {
    int rows = rows_visible();
    int cardw = SCR_W - 2 * L_M;
    int list_bottom = SCR_H - L_BAR;

    // 拖动列表滚动
    if (touch_drag && t->held && nvisible > rows) {
        touch_drag_accum += touch_last_y - t->y;
        int drags = touch_drag_accum / L_ROW;
        if (drags != 0) {
            touch_drag_accum -= drags * L_ROW;
            int maxscroll = nvisible - rows;
            scroll += drags;
            if (scroll < 0) scroll = 0;
            if (scroll > maxscroll) scroll = maxscroll;
            if (sel < scroll) sel = scroll;
            if (sel >= scroll + rows) sel = scroll + rows - 1;
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
                        switch_page(UI_PLAYER);
                    }
                }
                touch_drag = 0;
                touch_press_idx = -1;
            } else if (t->y >= list_bottom) {
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

    if (y >= list_bottom) return;

    // 歌曲列表行：按下记录 + 触发水波纹，松开且未拖动时播放
    if (nvisible > 0 && y >= L_TOP && x >= L_M && x <= SCR_W - L_M) {
        int pos = scroll + (y - L_TOP) / L_ROW;
        if (pos >= 0 && pos < nvisible) {
            touch_press_idx = pos;
            sel = vis_at(pos);
            touch_drag = 1;
            touch_drag_start_y = y;
            touch_last_y = y;
            touch_drag_accum = 0;
            // Material 水波纹：从触点在被按的卡片内扩散
            ripple_x = x;
            ripple_y = y;
            ripple_row = pos;
            ripple_cw = cardw;
            ripple_ch = L_ROW - 12;
            ripple_t = 0;
        }
    }
}

static void handle_touch_player(TouchInfo* t) {
    int back_h = 64;
    if (touch_scrub && t->held)
        seek_from_touch(t->x, ui_px0, ui_px1);

    if (!t->down) {
        if (t->up) touch_scrub = 0;
        return;
    }

    int x = t->x, y = t->y;
    touch_fx_x = x; touch_fx_y = y; touch_fx_alpha = 1;

    if (y < back_h) {                       // 顶部返回
        switch_page(UI_LIST);
        sel = playing_idx >= 0 ? playing_idx : sel;
        rebuild_visible();
        return;
    }
    // 进度条（区域略放大，便于触摸）
    if (in_rect(x, y, ui_px0 - 12, ui_py - 26, ui_px1 - ui_px0 + 24, 52)) {
        touch_scrub = 1;
        seek_from_touch(x, ui_px0, ui_px1);
        return;
    }
    // 控制按钮区（上一首 / 播放暂停 / 下一首）
    if (in_rect(x, y, ui_cx - ui_gap - 52, ui_cy - 48, ui_gap * 2 + 104, 96)) {
        if (x < ui_cx - ui_gap / 2) {
            if (playing_idx > 0) load_track(playing_idx - 1);
        } else if (x > ui_cx + ui_gap / 2) {
            if (ntracks > 0) load_track(pick_next(playing_idx));
        } else {
            if (audio_paused()) audio_resume(); else audio_pause();
        }
        return;
    }
    // 画面分区：左切歌 / 中暂停 / 右切歌
    if (x < SCR_W / 3) {
        if (playing_idx > 0) load_track(playing_idx - 1);
    } else if (x > SCR_W * 2 / 3) {
        if (ntracks > 0) load_track(pick_next(playing_idx));
    } else {
        if (audio_paused()) audio_resume(); else audio_pause();
    }
}

// 触点反馈：按下时触发，随时间淡出（抗锯齿圆形，主色）
static void draw_touch_feedback(void) {
    if (touch_fx_alpha > 0) {
        int r = 18 + (int)(10 * (1.0f - touch_fx_alpha));
        fill_circle_a(screen, (int)touch_fx_x, (int)touch_fx_y, r,
                      C_PRIMARY, (uint8_t)(85 * touch_fx_alpha));
    }
}

// Material 水波纹：从触点在被按卡片内扩散并淡出
static void draw_ripples(void) {
    if (ripple_t >= 1 || ripple_row < 0) return;
    float t = ease_out(ripple_t);
    int rmax = (ripple_cw > ripple_ch ? ripple_cw : ripple_ch) * 6 / 5 + 40;
    int r = (int)(rmax * t) + 10;
    uint8_t a = (uint8_t)(95 * (1.0f - ripple_t));
    if (a > 0) {
        // 裁剪框按当前滚动动画位置实时换算，滚动时水波纹跟随卡片
        int cy0 = L_TOP + (int)((ripple_row - anim_scroll) * L_ROW) + 6;
        fill_circle_clip_a(screen, ripple_x, ripple_y, r, C_PRIMARY, a,
                           L_M, cy0, ripple_cw, ripple_ch);
    }
}

// Material 渐变壁纸（缓存一次）：半透明卡片透出背景产生层次感
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
    // 大面积低透明度主色光斑，Material 壁纸质感
    fill_circle_a(list_bg, SCR_W * 4 / 5, L_TOP + 60, 320, COL((uint8_t)prr, (uint8_t)prg, (uint8_t)prb, 255), 13);
    fill_circle_a(list_bg, SCR_W / 6, SCR_H - L_BAR - 140, 260, COL((uint8_t)prr, (uint8_t)prg, (uint8_t)prb, 255), 10);
}

// ---------- 列表页（Material 大卡片单列 + 底部播放条） ----------
static void draw_list(void) {
    // 背景：正在播放时用模糊封面加深色叠加，否则用缓存的 Material 渐变壁纸；
    // 卡片半透明后背景会透出，形成层次感
    if (bg) {
        memcpy(screen->px, bg->px, (size_t)SCR_W * SCR_H * 4);
        fill_rect_a(screen, 0, 0, SCR_W, SCR_H, C_BG, 165);
    } else {
        build_list_bg();
        memcpy(screen->px, list_bg->px, (size_t)SCR_W * SCR_H * 4);
    }
    ensure_visible_thumbs();

    int rows = rows_visible();
    int cardw = SCR_W - 2 * L_M;
    int list_bottom = SCR_H - L_BAR;

    // 顶部标题栏
    fill_ellipse_aa(screen, 42, 34, 15, 15, C_PRIMARY);
    draw_text(screen, font, 28, 20, 56, C_TXT, "Siren");
    char sub[64];
    snprintf(sub, sizeof(sub), "%d 首歌曲", ntracks);
    draw_text_right(screen, font, 15, SCR_W - L_M, 62, C_TXT3, sub);

    if (nvisible == 0) {
        draw_text_centered(screen, font, 22, 0, SCR_W, SCR_H / 2 - 20, C_TXT2,
                           ntracks == 0 ? "未在 SD 卡找到音乐文件 · 可将歌曲放入 /music 目录" : "暂无歌曲");
        draw_touch_feedback();
        return;
    }

    // 第一遍：卡片与内容（半透明，透出背景）
    int i0 = (int)floorf(anim_scroll) - 1;
    if (i0 < 0) i0 = 0;
    for (int i = i0; i < rows + i0 + 2; i++) {
        int idx = vis_at(i);
        if (idx < 0) break;
        int y = L_TOP + (int)((i - anim_scroll) * L_ROW);
        int cy0 = y + 6;
        int ch = L_ROW - 12;
        if (y + L_ROW < L_TOP - 4 || y > list_bottom - 4) continue;

        int is_playing = (idx == playing_idx);
        int is_sel = (idx == sel);
        int is_press = (touch_drag && touch_press_idx == i);

        // Google Material：半透明卡片底色，按压/选中/播放分别提亮或着色
        uint32_t card = C_CARD;
        uint8_t card_a = 205;
        if (is_playing) { card = C_PLAYBG; card_a = 232; }
        else if (is_press) { card = C_SURF2; card_a = 240; }
        else if (is_sel) { card = C_CARD2; card_a = 226; }
        fill_round_rect_aa_a(screen, L_M, cy0, cardw, ch, 20, card, card_a);

        // 大封面缩略图（抗锯齿圆角）
        int tsize = ch - 20;
        if (tsize > 96) tsize = 96;
        int tx = L_M + 14;
        int ty = cy0 + (ch - tsize) / 2;
        if (tsize == 96 && tracks[idx].thumb96)
            blit_rgba_round(screen, tx, ty, tracks[idx].thumb96, 96, 96, 18);
        else if (tsize == 36 && tracks[idx].thumb)
            blit_rgba_round(screen, tx, ty, tracks[idx].thumb, 36, 36, 8);

        // 歌名 / 艺术家（垂直居中）
        char nm[256];
        basename_no_ext(tracks[idx].path, nm, sizeof(nm));
        const char* ttl = tracks[idx].meta.title[0] ? tracks[idx].meta.title : nm;
        const char* art = tracks[idx].meta.artist[0] ? tracks[idx].meta.artist : "";
        uint32_t tc = is_playing ? C_PRIMARY : C_TXT;
        int txtx = tx + 96 + 18;
        int block = L_TITLE + 10 + (art[0] ? L_ART + 8 : 0);
        int ty2 = cy0 + (ch - block) / 2;
        int maxw = cardw - (96 + 14 + 18) - 24 - (is_playing ? 100 : 0);
        draw_text_max(screen, font, L_TITLE, txtx, ty2, tc, ttl, maxw);
        if (art[0])
            draw_text_max(screen, font, L_ART, txtx, ty2 + L_TITLE + 10, C_TXT3, art, maxw);

        if (is_playing && audio_duration() > 0) {
            char dur[16];
            fmt_time(audio_duration(), dur, sizeof(dur));
            draw_text_right(screen, font, L_ART, SCR_W - L_M - 20, cy0 + ch / 2 - 12, C_TXT3, dur);
        }
    }

    // 第二遍：选中 / 播放描边（最后绘制，避免下滑时被相邻卡片覆盖——图层修正）
    for (int i = i0; i < rows + i0 + 2; i++) {
        int idx = vis_at(i);
        if (idx < 0) break;
        if (idx != sel && idx != playing_idx) continue;
        int y = L_TOP + (int)((i - anim_scroll) * L_ROW);
        if (y + L_ROW < L_TOP - 4 || y > list_bottom - 4) continue;
        fill_round_rect_aa(screen, L_M - 3, y + 3, cardw + 6, L_ROW - 6, 22, C_PRIMARY);
    }

    draw_ripples();

    // 底部播放条：当前歌曲 + 顶部细进度线，点击进沉浸页（半透明）
    int by = SCR_H - L_BAR;
    fill_rect_a(screen, 0, by, SCR_W, L_BAR, C_SURFACE, 232);
    fill_rect(screen, 0, by, SCR_W, 1, C_LINE);
    if (playing_idx >= 0 && audio_duration() > 0) {
        float p = (float)(audio_position() / audio_duration());
        if (p < 0) p = 0; if (p > 1) p = 1;
        fill_rect(screen, 0, by, (int)(SCR_W * p), 2, C_PRIMARY);
    }
    int midy = by + L_BAR / 2;
    if (playing_idx >= 0) {
        if (tracks[playing_idx].thumb)
            blit_rgba_round(screen, 16, midy - 18, tracks[playing_idx].thumb, 36, 36, 8);
        draw_text_max(screen, font, portrait() ? 19 : 16, 64, midy - (portrait() ? 24 : 20),
                      C_TXT, p_title[0] ? p_title : "Siren", SCR_W - 140);
        if (p_artist[0])
            draw_text_max(screen, font, L_ARTIST2, 64, midy + (portrait() ? 2 : 0),
                          C_TXT3, p_artist, SCR_W - 140);
        // 右侧播放 / 暂停状态图标
        if (audio_paused())
            fill_tri_aa(screen, SCR_W - 58, midy - 11, SCR_W - 58, midy + 11, SCR_W - 40, midy, C_PRIMARY);
        else {
            fill_rect(screen, SCR_W - 62, midy - 11, 5, 22, C_PRIMARY);
            fill_rect(screen, SCR_W - 51, midy - 11, 5, 22, C_PRIMARY);
        }
    } else {
        draw_text(screen, font, 15, 20, midy - 10, C_TXT3, "点击歌曲开始播放");
    }

    draw_touch_feedback();
}

static void ui_list_input(uint64_t down, uint64_t rep) {
    rebuild_visible();
    if (nvisible == 0) return;
    int pos = sel_pos();
    int rows = rows_visible();

    if (rep & (HidNpadButton_Up | HidNpadButton_StickLUp)) {
        if (pos > 0) pos--;
        sel = vis_at(pos);
        if (pos < scroll) scroll = pos;
    }
    if (rep & (HidNpadButton_Down | HidNpadButton_StickLDown)) {
        if (pos < nvisible - 1) pos++;
        sel = vis_at(pos);
        if (pos >= scroll + rows) scroll = pos - rows + 1;
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

// ---------- 播放页共用绘制辅助 ----------
// 居中显示、超宽省略
static void draw_text_centered_max(Canvas* c, Font* f, float px, int x0, int x1, int y, uint32_t col, const char* s) {
    int w = text_width(f, px, s);
    char buf[256];
    const char* use = s;
    if (w > x1 - x0) {
        snprintf(buf, sizeof(buf), "%s", s);
        int ellw = text_width(f, px, "…");
        int limit = x1 - x0 - ellw;
        if (limit < 0) limit = 0;
        int len = (int)strlen(buf);
        int acc = 0, cut = len;
        for (int i = 0; i < len; ) {
            int cl = 1;
            unsigned char ch = (unsigned char)buf[i];
            if (ch >= 0xF0) cl = 4; else if (ch >= 0xE0) cl = 3; else if (ch >= 0xC0) cl = 2;
            if (i + cl > len) cl = len - i;
            char tmp[8];
            memcpy(tmp, buf + i, cl); tmp[cl] = 0;
            acc += text_width(f, px, tmp);
            if (acc > limit) { cut = i; break; }
            i += cl;
        }
        buf[cut] = 0;
        strcat(buf, "…");
        use = buf;
    }
    int w2 = text_width(f, px, use);
    int x = x0 + (x1 - x0 - w2) / 2;
    draw_text(c, f, px, x, y, col, use);
}

// 歌词 / 频谱区域（横竖屏共用）
static void draw_lyric_or_spectrum(int x0, int x1, int ly_top, int spec_base, int spec_hint_y, double pos) {
    if (lyr) {
        int act = lyrics_index(lyr, pos);
        int idxs2[9];
        int n = 0;
        for (int d = -3; d <= 4; d++) {
            int i = act + d;
            if (i >= 0 && i < lyr->n) idxs2[n++] = i;
        }
        int y = ly_top;
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
            if (y > spec_base + 40) break;
        }
    } else {
        float sp[28];
        audio_get_spectrum(sp);
        for (int i = 0; i < 28; i++) {
            bar_target[i] = sp[i];
            bars[i] += (bar_target[i] - bars[i]) * 0.25f;
        }
        int n = portrait() ? 14 : 20;
        int bw = portrait() ? 16 : 14;
        int avail = (x1 - x0 - 100 - n * bw);
        if (avail < n) avail = n;
        int gap = avail / (n - 1);
        for (int i = 0; i < n; i++) {
            float v = bars[i];
            if (v < 0.02f) v = 0.02f;
            int hgt = (int)(v * 180);
            int bx = x0 + 50 + i * (bw + gap);
            fill_round_rect_aa(screen, bx, spec_base - hgt, bw, hgt, bw / 2,
                               C_PRIMARY);
        }
        draw_text_centered(screen, font, 16, x0, x1, spec_hint_y, C_TXT3,
                           "无歌词 · 可放同名 .lrc 文件到歌曲目录");
    }
}

// 进度条（Material pill + 抗锯齿滑块）
static void draw_progress_bar(int py, double pos, double dur) {
    int w = ui_px1 - ui_px0;
    fill_round_rect_aa(screen, ui_px0, py, w, 6, 3, C_LINE);
    int fillw = (int)((dur > 0 ? pos / dur : 0) * w);
    if (fillw < 0) fillw = 0;
    if (fillw > w) fillw = w;
    if (fillw > 0) fill_round_rect_aa(screen, ui_px0, py, fillw, 6, 3, C_PRIMARY);
    fill_ellipse_aa(screen, ui_px0 + fillw, py + 3, 8, 8, C_PRIMARY);

    char t1[32], t2[32];
    fmt_time(pos, t1, sizeof(t1));
    fmt_time(dur, t2, sizeof(t2));
    draw_text(screen, font, 15, ui_px0, py + 18, C_TXT2, t1);
    draw_text_right(screen, font, 15, ui_px1, py + 18, C_TXT2, t2);
}

// 控制按钮：上一首 / 播放暂停（主色大圆）/ 下一首
static void draw_controls(int cy) {
    fill_ellipse_aa(screen, ui_cx - ui_gap, cy, 24, 24, C_SURF2);
    icon_prev(screen, ui_cx - ui_gap - 11, cy - 11, 22, C_TXT);
    fill_ellipse_aa(screen, ui_cx, cy, 32, 32, C_PRIMARY);
    if (audio_paused()) {
        fill_tri_aa(screen, ui_cx - 9, cy - 12, ui_cx - 9, cy + 12, ui_cx + 10, cy, C_ONPRIM);
    } else {
        fill_rect(screen, ui_cx - 11, cy - 12, 6, 24, C_ONPRIM);
        fill_rect(screen, ui_cx + 4, cy - 12, 6, 24, C_ONPRIM);
    }
    fill_ellipse_aa(screen, ui_cx + ui_gap, cy, 24, 24, C_SURF2);
    icon_next(screen, ui_cx + ui_gap - 11, cy - 11, 22, C_TXT);
}

// ---------- 沉浸播放页（Material You，横竖屏自适应） ----------
static void draw_player(void) {
    if (bg) memcpy(screen->px, bg->px, (size_t)SCR_W * SCR_H * 4);
    else canvas_clear(screen, C_BG);

    // 暗化叠加（tonal 氛围）
    fill_rect_a(screen, 0, 0, SCR_W, SCR_H, COL(8, 10, 12, 255), 70);

    int csz = 380;

    if (!portrait()) {
        // ---- 横屏布局 ----
        draw_text_centered(screen, font, 16, 900, SCR_W - 40, 30, C_PRIMARY, "正在播放");

        int cx = 110, cy = 110;
        fill_round_rect_aa(screen, cx + 6, cy + 8, csz, csz, 32, COL(0, 0, 0, 255));
        if (anim_cover < 1 && cover_prev) {
            float t = ease_out(anim_cover);
            blit_rgba_round_a(screen, cx, cy, cover_prev, csz, csz, 32, (uint8_t)((1 - t) * 255));
            if (cover) blit_rgba_round_a(screen, cx, cy, cover, csz, csz, 32, (uint8_t)(t * 255));
        } else if (cover) {
            blit_rgba_round(screen, cx, cy, cover, csz, csz, 32);
        }

        draw_text_max(screen, font, 52, 560, 150, C_TXT, p_title, SCR_W - 560 - 40);
        draw_text_max(screen, font, 20, 562, 224, C_TXT2, p_artist, SCR_W - 560 - 40);

        double pos = show_pos;
        double dur = audio_duration();
        if (pos > dur) pos = dur;

        // 歌词 / 频谱
        int x0 = 620, x1 = SCR_W - 80;
        draw_lyric_or_spectrum(x0, x1, 330, 500, 540, pos);

        // 进度条（Material pill）
        int py = 628;
        ui_px0 = 110; ui_px1 = SCR_W - 110; ui_py = py;
        draw_progress_bar(py, pos, dur);

        // 控制按钮（圆形 tonal + 胶囊主按钮）
        int cy2 = 684;
        ui_cx = SCR_W / 2; ui_cy = cy2; ui_gap = 80;
        draw_controls(cy2);

        draw_text_right(screen, font, 15, SCR_W - 40, 676, C_TXT3,
                        "A 播放/暂停 · B 返回 · L/R 切歌 · − 切换横竖屏 · Y 自检");
    } else {
        // ---- 竖屏布局 ----
        draw_text_centered(screen, font, 16, 0, SCR_W, 30, C_PRIMARY, "正在播放");

        int cx = (SCR_W - csz) / 2, cy = 100;
        fill_round_rect_aa(screen, cx + 6, cy + 8, csz, csz, 32, COL(0, 0, 0, 255));
        if (anim_cover < 1 && cover_prev) {
            float t = ease_out(anim_cover);
            blit_rgba_round_a(screen, cx, cy, cover_prev, csz, csz, 32, (uint8_t)((1 - t) * 255));
            if (cover) blit_rgba_round_a(screen, cx, cy, cover, csz, csz, 32, (uint8_t)(t * 255));
        } else if (cover) {
            blit_rgba_round(screen, cx, cy, cover, csz, csz, 32);
        }

        draw_text_centered_max(screen, font, 34, 40, SCR_W - 40, cy + csz + 34, C_TXT, p_title);
        if (p_artist[0])
            draw_text_centered_max(screen, font, 20, 40, SCR_W - 40, cy + csz + 96, C_TXT2, p_artist);

        double pos = show_pos;
        double dur = audio_duration();
        if (pos > dur) pos = dur;

        draw_lyric_or_spectrum(40, SCR_W - 40, cy + csz + 150, cy + csz + 330, cy + csz + 380, pos);

        // 进度条
        int py = SCR_H - 210;
        ui_px0 = 70; ui_px1 = SCR_W - 70; ui_py = py;
        draw_progress_bar(py, pos, dur);

        // 控制按钮
        int cy2 = SCR_H - 130;
        ui_cx = SCR_W / 2; ui_cy = cy2; ui_gap = 120;
        draw_controls(cy2);
    }

    if (audio_status() == 3) {
        draw_text_centered(screen, font, 24, 0, SCR_W, SCR_H / 2 - 40, COL(255, 140, 130, 255), "解码失败：文件不支持或损坏");
        draw_text_centered(screen, font, 18, 0, SCR_W, SCR_H / 2 - 4, C_TXT2, p_title);
    }

    if (vol_overlay_timer > 0) {
        vol_overlay_timer--;
        char vs[64];
        snprintf(vs, sizeof(vs), "音量 %d%%", (int)(audio_volume() * 100 + 0.5));
        int bw2 = 240;
        int bx = (SCR_W - bw2) / 2;
        fill_round_rect_aa(screen, bx, 80, bw2, 64, 18, C_SURFACE);
        draw_text_centered(screen, font, 22, bx, bx + bw2, 90, C_TXT, vs);
        fill_round_rect_aa(screen, bx + 30, 144, bw2 - 60, 6, 3, C_LINE);
        fill_round_rect_aa(screen, bx + 30, 144, (int)((bw2 - 60) * audio_volume()), 6, 3, C_PRIMARY);
    }

    if (loading)
        draw_text_centered(screen, font, 24, 0, SCR_W, SCR_H / 2, C_TXT2, "加载中…");

    draw_touch_feedback();
}

static void draw_diag(void) {
    fill_rect_a(screen, 0, 0, SCR_W, SCR_H, COL(0, 0, 0, 255), 210);
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
    draw_text(screen, font, 20, 60, SCR_H - 60, C_TXT3, "若 状态=FAILED 或 设备=0 请截图此页反馈");
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
    // X / ZL：收藏当前歌曲
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
    sixaxis_init();
    if (audio_init() != 0) {
        // 音频不可用时仍显示界面
    }

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

        // ---- 手动方向切换（− 键循环：自动 / 横屏 / 竖A / 竖B） ----
        if (down & HidNpadButton_Minus) {
            ori_mode = (ori_mode + 1) % 4;
            if (ori_mode == 0) show_toast("方向：自动（陀螺仪）");
            else if (ori_mode == 1) { apply_orientation(ORI_LAND); show_toast("方向：横屏"); }
            else if (ori_mode == 2) { apply_orientation(ORI_PORT_A); show_toast("方向：竖屏 A"); }
            else { apply_orientation(ORI_PORT_B); show_toast("方向：竖屏 B"); }
        }

        // ---- 自动方向检测（竖持 0.35 秒后切换，防抖） ----
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

        // ---- 触摸坐标映射到当前画面方向 ----
        if (touch_now.held || touch_now.up) {
            if (ori == ORI_PORT_A) {
                int nx = touch_now.y, ny = touch_now.x;
                touch_now.x = nx; touch_now.y = ny;
            } else if (ori == ORI_PORT_B) {
                int nx = 719 - touch_now.y;
                int ny = 1279 - touch_now.x;
                touch_now.x = nx; touch_now.y = ny;
            }
        }

        if (show_diag) {
            if (touch_now.down) show_diag = 0;
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
        anim_scroll = approach(anim_scroll, (float)scroll, 18.0f, g_dt);
        if (anim_cover < 1) anim_cover = (float)(anim_cover + g_dt / 0.32);
        if (anim_cover > 1) anim_cover = 1;
        if (trans_t < 1) trans_t = (float)(trans_t + g_dt / 0.26);
        if (trans_t > 1) trans_t = 1;
        if (ripple_t < 1) ripple_t = (float)(ripple_t + g_dt / 0.45);
        if (touch_fx_alpha > 0) {
            touch_fx_alpha -= (float)(g_dt * 2.2);
            if (touch_fx_alpha < 0) touch_fx_alpha = 0;
        }
        // 播放进度平滑（暂停时保持，拖动时由 seek 同步）
        double ap = audio_position();
        show_pos = (double)approach((float)show_pos, (float)ap, 9.0f, g_dt);
        if (show_pos < 0) show_pos = 0;
        if (fabs(show_pos - ap) > 3.0) show_pos = ap;

        uint32_t* fb = be_begin(&stride);
        if (ui_state == UI_LIST) draw_list();
        else draw_player();

        // 提示气泡（方向切换等）
        if (toast_timer > 0) {
            toast_timer--;
            int tw = text_width(font, 17, toast_text) + 56;
            if (tw > SCR_W - 80) tw = SCR_W - 80;
            int bx = (SCR_W - tw) / 2;
            fill_round_rect_aa(screen, bx, 20, tw, 46, 23, C_SURFACE);
            draw_text_centered(screen, font, 17, bx, bx + tw, 32, C_TXT, toast_text);
        }

        // 页面切换：旧页面上滑淡出（Material shared axis Y）
        if (trans_t < 1 && trans_shot && trans_shot->w == SCR_W && trans_shot->h == SCR_H) {
            uint8_t a = (uint8_t)((1 - ease_out(trans_t)) * 255);
            int dy = -(int)(ease_out(trans_t) * 48);
            blit_rgba_a(screen, 0, dy, trans_shot->px, SCR_W, SCR_H, a);
        }

        if (show_diag) draw_diag();

        // ---- 写入 framebuffer（按当前方向旋转） ----
        if (ori == ORI_LAND) {
            for (int y = 0; y < SCR_H; y++) {
                uint32_t* dst = fb + (size_t)y * stride;
                uint32_t* src = screen->px + (size_t)y * SCR_W;
                memcpy(dst, src, (size_t)SCR_W * 4);
            }
        } else if (ori == ORI_PORT_A) {
            // 转置：fb[y][x] = screen(px=y, py=x)
            for (int y = 0; y < 720; y++) {
                uint32_t* d = fb + (size_t)y * stride;
                for (int x = 0; x < 1280; x++)
                    d[x] = screen->px[(size_t)x * 720 + y];
            }
        } else {
            // 转置 + 180°：fb[y][x] = screen(719-y, 1279-x)
            for (int y = 0; y < 720; y++) {
                uint32_t* d = fb + (size_t)y * stride;
                for (int x = 0; x < 1280; x++)
                    d[x] = screen->px[(size_t)(1279 - x) * 720 + (719 - y)];
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
    if (cover_small) free(cover_small);
    if (poster36) free(poster36);
    if (poster96) free(poster96);
    meta_clear(&meta);
    if (lyr) lyrics_free(lyr);
    for (int i = 0; i < ntracks; i++) {
        free(tracks[i].thumb);
        free(tracks[i].thumb96);
        meta_clear(&tracks[i].meta);
    }
    free(tracks);
    return 0;
}
