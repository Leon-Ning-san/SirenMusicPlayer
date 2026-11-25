#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "plat.h"
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

#define MAX_TRACKS  8192
#define FADE_DUR    0.16f

// 严格固定各尺寸封面缓冲区边长，彻底杜绝步长错位导致左上角图标/封面撕裂
#define LOGO_SZ       30
#define MINI_ART_SZ   42
#define THUMB_SZ      50
#define POD_ART_SZ    86
#define COVER_PORT_SZ 184
#define COVER_LAND_SZ 236
#define COVER_SRC_SZ  320

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

// ---------- 屏幕方向（横屏 + 单向顺时针竖屏 iPod 比例） ----------
#define ORI_LAND 0   // 1280x720
#define ORI_PORT 1   // 720x1280（单向竖屏）
static int ori_locked = 0;
static int ori = ORI_LAND;
static int SCR_W = 1280, SCR_H = 720;
static int portrait(void) { return ori == ORI_PORT; }

// ---------- 乔布斯 iPod 显示窗 + Click Wheel 现代几何布局 ----------
static int LCD_X, LCD_Y, LCD_W, LCD_H;
static int L_X0, L_X1, L_TOP, L_ROW, L_CARD_H, L_TITLE, L_ART;
static int W_CX, W_CY, W_RO, W_RI;

static void update_layout(void) {
    if (be_is_desktop()) {
        LCD_X = 20; LCD_Y = 66; LCD_W = 742; LCD_H = 550;
        L_X0 = LCD_X + 14;
        L_X1 = LCD_X + LCD_W - 22;
        L_TOP = LCD_Y + 48;
        L_ROW = 62;
        L_CARD_H = 54;
        L_TITLE = 16;
        L_ART = 12;
        W_CX = 0; W_CY = 0; W_RO = 0; W_RI = 0;
    } else if (portrait()) {
        LCD_X = 24; LCD_Y = 22; LCD_W = 672; LCD_H = 636;
        L_X0 = LCD_X + 14;
        L_X1 = LCD_X + LCD_W - 14;
        L_TOP = LCD_Y + 96;
        L_ROW = 64;
        L_CARD_H = 56;
        L_TITLE = 18;
        L_ART = 13;
        W_CX = SCR_W / 2;
        W_CY = 948;
        W_RO = 198;
        W_RI = 74;
    } else {
        LCD_X = 24; LCD_Y = 22; LCD_W = 820; LCD_H = 676;
        L_X0 = LCD_X + 16;
        L_X1 = LCD_X + LCD_W - 16;
        L_TOP = LCD_Y + 64;
        L_ROW = 64;
        L_CARD_H = 56;
        L_TITLE = 18;
        L_ART = 13;
        W_CX = 1061;
        W_CY = 342;
        W_RO = 152;
        W_RI = 58;
    }
}

static int list_bottom_y(void) {
    if (be_is_desktop()) return LCD_Y + LCD_H - 12;
    return LCD_Y + LCD_H - ((portrait() && be_is_desktop()) ? 116 : 68);
}

static int list_area_h(void) {
    int h = list_bottom_y() - L_TOP;
    return h < L_ROW ? L_ROW : h;
}

static int rows_visible(void) {
    int r = list_area_h() / L_ROW;
    return r < 1 ? 1 : r;
}

// ---------- 现代 Apple × Google Material You 融合主题 ----------
#define THEME_FROST    0
#define THEME_TITANIUM 1
#define THEME_SAGE     2
#define THEME_COUNT    3

static uint32_t C_BODY, C_SURFACE, C_SURF_SUB, C_CARD, C_CARD_SEL;
static uint32_t C_TONAL_BG, C_TONAL_TXT, C_ACCENT, C_ON_ACCENT;
static uint32_t C_WHEEL, C_WHEEL_RING, C_WHEEL_CENTER, C_WHEEL_TXT;
static uint32_t C_CHIP, C_TXT, C_TXT2, C_TXT3, C_TRACK_BG, C_HEART;
static int g_is_light = 1;

typedef struct {
    uint32_t body, surface, surf_sub, card, card_sel;
    uint32_t tonal_bg, tonal_txt, accent, on_accent;
    uint32_t wheel, wheel_ring, wheel_center, wheel_txt;
    uint32_t chip, txt, txt2, txt3, track_bg;
    int is_light;
} Theme;

static const Theme themes[THEME_COUNT] = {
    // 0: 冰霜银白（Apple Frost Silver × Material You Indigo Blue）
    { COL(0xE7,0xEC,0xF4,0xFF), COL(0xFB,0xFC,0xFF,0xFF), COL(0xF1,0xF4,0xFA,0xFF),
      COL(0xF4,0xF6,0xFB,0xFF), COL(0xE8,0xEE,0xF9,0xFF),
      COL(0xDC,0xE7,0xFF,0xFF), COL(0x1C,0x4E,0xC9,0xFF),
      COL(0x2E,0x67,0xF6,0xFF), COL(0xFF,0xFF,0xFF,0xFF),
      COL(0xF9,0xFA,0xFD,0xFF), COL(0xD3,0xDB,0xE8,0xFF), COL(0xE8,0xEE,0xF7,0xFF),
      COL(0x36,0x41,0x54,0xFF),
      COL(0xDC,0xE3,0xEF,0xFF), COL(0x12,0x17,0x22,0xFF), COL(0x49,0x54,0x68,0xFF),
      COL(0x7A,0x86,0x9B,0xFF), COL(0xD9,0xE0,0xED,0xFF), 1 },
    // 1: 钛金暗夜（Apple Titanium Dark × Material You Periwinkle）
    { COL(0x0E,0x11,0x17,0xFF), COL(0x16,0x1A,0x24,0xFF), COL(0x1D,0x23,0x30,0xFF),
      COL(0x1B,0x20,0x2C,0xFF), COL(0x25,0x2C,0x3D,0xFF),
      COL(0x1E,0x30,0x52,0xFF), COL(0xA8,0xCB,0xFF,0xFF),
      COL(0x64,0xA2,0xFF,0xFF), COL(0x08,0x1B,0x38,0xFF),
      COL(0x1B,0x20,0x2C,0xFF), COL(0x2E,0x36,0x48,0xFF), COL(0x12,0x16,0x1F,0xFF),
      COL(0xD6,0xDF,0xEF,0xFF),
      COL(0x21,0x27,0x36,0xFF), COL(0xF3,0xF6,0xFC,0xFF), COL(0xB0,0xBC,0xD0,0xFF),
      COL(0x76,0x83,0x9A,0xFF), COL(0x2A,0x32,0x44,0xFF), 0 },
    // 2: 青瓷雾绿（Material You Dynamic Sage × Apple Mint）
    { COL(0xE3,0xED,0xE9,0xFF), COL(0xF9,0xFC,0xFA,0xFF), COL(0xEE,0xF5,0xF2,0xFF),
      COL(0xF1,0xF7,0xF4,0xFF), COL(0xE3,0xF0,0xEB,0xFF),
      COL(0xCE,0xEC,0xE2,0xFF), COL(0x0F,0x5E,0x4C,0xFF),
      COL(0x1A,0x85,0x6D,0xFF), COL(0xFF,0xFF,0xFF,0xFF),
      COL(0xF7,0xFB,0xF9,0xFF), COL(0xC9,0xDB,0xD4,0xFF), COL(0xE2,0xEE,0xE9,0xFF),
      COL(0x2B,0x45,0x3E,0xFF),
      COL(0xD4,0xE4,0xDE,0xFF), COL(0x11,0x1D,0x1A,0xFF), COL(0x44,0x59,0x53,0xFF),
      COL(0x71,0x88,0x81,0xFF), COL(0xCF,0xDF,0xD9,0xFF), 1 }
};

static const char* theme_names[THEME_COUNT] = { "冰霜白", "钛空黑", "青瓷绿" };
static int cur_theme = THEME_FROST;

static void apply_theme(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= THEME_COUNT) idx = THEME_COUNT - 1;
    cur_theme = idx;
    const Theme* t = &themes[idx];
    C_BODY = t->body; C_SURFACE = t->surface; C_SURF_SUB = t->surf_sub;
    C_CARD = t->card; C_CARD_SEL = t->card_sel;
    C_TONAL_BG = t->tonal_bg; C_TONAL_TXT = t->tonal_txt;
    C_ACCENT = t->accent; C_ON_ACCENT = t->on_accent;
    C_WHEEL = t->wheel; C_WHEEL_RING = t->wheel_ring;
    C_WHEEL_CENTER = t->wheel_center; C_WHEEL_TXT = t->wheel_txt;
    C_CHIP = t->chip; C_TXT = t->txt; C_TXT2 = t->txt2; C_TXT3 = t->txt3;
    C_TRACK_BG = t->track_bg;
    C_HEART = COL(0xF2, 0x3D, 0x64, 0xFF);
    g_is_light = t->is_light;
}

typedef struct {
    char path[600];
    TrackMeta meta;
    int meta_state;
    uint32_t* thumb;    // THUMB_SZ x THUMB_SZ
    uint32_t* thumb_pod;// POD_ART_SZ x POD_ART_SZ
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
static Canvas* lcd_ambient = NULL;

// 播放与固定尺寸图像缓存
static TrackMeta meta;
static Lyrics* lyr = NULL;
static uint32_t* cover_src  = NULL; // COVER_SRC_SZ  (320x320)
static uint32_t* cover_land = NULL; // COVER_LAND_SZ (236x236)
static uint32_t* cover_port = NULL; // COVER_PORT_SZ (184x184)
static uint32_t* cover_mini = NULL; // MINI_ART_SZ   (42x42)
static uint32_t* logo_img   = NULL; // LOGO_SZ       (30x30)
static uint32_t* poster_mini= NULL; // MINI_ART_SZ   (42x42)
static uint32_t* poster_thm = NULL; // THUMB_SZ      (50x50)
static uint32_t* poster_pod = NULL; // POD_ART_SZ    (86x86)
static int cover_ready = 0;
static char p_title[256];
static char p_artist[256];
static char p_fmt[12];
static float bars[28];

static int vol_overlay_timer = 0;
static int show_diag = 0;
static int show_vol_panel = 0;
static int show_exit_confirm = 0;
static int show_search_box = 0;
static int g_want_exit = 0;
static float saved_mute_vol = 0.85f;
static char custom_music_dir[600] = "";
static char search_edit[128] = "";

// ---------- 60 FPS 时间驱动淡入动画 & Click Wheel 缓存 ----------
static double g_dt = 0.016;
static uint64_t g_last_tick = 0;
static float scroll_px = 0.0f;
static float scroll_target = 0.0f;
static Canvas* trans_shot = NULL;
static float fade_time = 0.0f;
static int fade_mode = 0;    // 0 = 无, 1 = 交叉淡入 (trans_shot), 2 = 底色淡入 (boot/ori)
static int fade_armed = 0;   // 1 = 首帧跳过阻塞加载耗时，从下帧开始平滑计时
static float wave_phase = 0.0f;
static float wave_amp_cur = 4.4f;

static Canvas* wheel_cache = NULL;
static int wc_ori = -1, wc_theme = -1, wc_zone = -1, wc_list = -1, wc_paused = -1;
static int wc_sel_play = -1, wc_ro = 0, wc_ri = 0;

// ---------- 提示气泡 ----------
static char toast_text[96];
static int toast_timer = 0;
static void show_toast(const char* s) {
    if (!s) { toast_text[0] = 0; toast_timer = 0; return; }
    size_t n = strlen(s);
    if (n >= sizeof(toast_text)) {
        n = sizeof(toast_text) - 1;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    }
    memcpy(toast_text, s, n);
    toast_text[n] = 0;
    toast_timer = 90;
}

// 进度条与封面触摸区
static int ui_px0, ui_px1, ui_py;
static int ui_cover_x, ui_cover_y, ui_cover_sz;

// 触摸与 Click Wheel 状态
static TouchInfo touch_now;
static int touch_drag = 0;
static int touch_moved = 0;
static int touch_scrub = 0;
static double scrub_preview_sec = 0.0;
static int g_lyr_req_id = 0;
static int touch_drag_start_y = 0;
static int touch_last_y = 0;
static int touch_press_idx = -1;
static int touch_vol_scrub = 0;

static int wheel_active = 0;
static int wheel_moved = 0;
static int wheel_down_zone = -1;
static float wheel_last_ang = 0.0f;
static float wheel_accum = 0.0f;

static char search_q[128] = "";
static int fav_count = 0;

typedef struct { int x, y, w, h; } Rect;
static Rect rc_tab_all, rc_tab_fav, rc_btn_rescan, rc_btn_mode, rc_btn_full;
static Rect rc_pod_card, rc_mini_bar, rc_empty_rescan, rc_p_back, rc_btn_fav_p;
static Rect rc_btn_vol, rc_btn_ori, rc_btn_search, rc_btn_theme, rc_btn_diag, rc_btn_exit;
static Rect rc_vol_box, rc_vol_minus, rc_vol_plus, rc_vol_mute, rc_vol_close;
static int  vol_sx0, vol_sx1, vol_sy;
static Rect rc_exit_box, rc_exit_ok, rc_exit_cancel;
static Rect rc_diag_close, rc_diag_box;
static Rect rc_sr_box, rc_sr_ok, rc_sr_clear, rc_sr_cancel;

// Desktop PC Material You 界面交互状态
static int desktop_full = 0;
static int touch_sb_drag = 0;
static int touch_vol_inline = 0;
static Rect rc_desk_sb, rc_desk_prev, rc_desk_play, rc_desk_next, rc_desk_mute, rc_desk_sr_clear;
static Rect rc_desk_vol_track, rc_desk_full_mode;
#define MAX_DESK_LYR_HITS 12
typedef struct { Rect rc; int idx; } DeskLyrHit;
static DeskLyrHit desk_lyr_hits[MAX_DESK_LYR_HITS];
static int ndesk_lyr_hits = 0;
static int desk_lyr_y0 = 0, desk_lyr_y1 = 0, desk_lyr_x0 = 0, desk_lyr_x1 = 0;

// ---------- 淡入过渡辅助 ----------
static void capture_fade(void) {
    if (!screen) return;
    if (trans_shot && (trans_shot->w != screen->w || trans_shot->h != screen->h)) {
        canvas_free(trans_shot);
        trans_shot = NULL;
    }
    if (!trans_shot) trans_shot = canvas_new(screen->w, screen->h);
    if (trans_shot) {
        memcpy(trans_shot->px, screen->px, (size_t)screen->w * screen->h * 4);
        fade_time = FADE_DUR;
        fade_mode = 1;
        fade_armed = 1;
    }
}

static void trigger_modal_fade(void) {
    capture_fade();
}

// ---------- 设置 & 收藏持久化 ----------
static void ensure_dir_recursive(const char* path) {
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
#ifdef _WIN32
            if (p == tmp + 2 && tmp[1] == ':') continue;
#endif
            char sav = *p;
            *p = 0;
            plat_mkdir_p(tmp);
            *p = sav;
        }
    }
    plat_mkdir_p(tmp);
}

static void cfg_join(char* out, size_t cap, const char* file) {
    const char* dir = be_config_dir();
    size_t n = strlen(dir);
    while (n > 0 && (dir[n - 1] == '/' || dir[n - 1] == '\\')) n--;
    snprintf(out, cap, "%.*s/%s", (int)n, dir, file);
}

static void save_custom_dir(void) {
    if (!be_is_desktop()) return;
    ensure_dir_recursive(be_config_dir());
    char p[640];
    cfg_join(p, sizeof(p), "music_dir.cfg");
    FILE* f = plat_fopen(p, "w");
    if (f) {
        if (custom_music_dir[0]) fprintf(f, "%s\n", custom_music_dir);
        fclose(f);
    }
}

static void load_custom_dir(void) {
    if (!be_is_desktop()) return;
    char p[640];
    cfg_join(p, sizeof(p), "music_dir.cfg");
    FILE* f = plat_fopen(p, "r");
    if (f) {
        if (fgets(custom_music_dir, sizeof(custom_music_dir), f)) {
            size_t len = strlen(custom_music_dir);
            while (len > 0 && (custom_music_dir[len - 1] == '\n' || custom_music_dir[len - 1] == '\r'))
                custom_music_dir[--len] = 0;
        }
        fclose(f);
    }
}

static void save_settings(void) {
    ensure_dir_recursive(be_config_dir());
    char p[640];
    cfg_join(p, sizeof(p), "settings.cfg");
    FILE* f = plat_fopen(p, "w");
    if (f) {
        fprintf(f, "%d %d %.2f\n", cur_theme, play_mode, audio_volume());
        fclose(f);
    }
}

static void load_settings(void) {
    ensure_dir_recursive(be_config_dir());
    int t = THEME_FROST, pm = MODE_LOOP_ALL;
    float vol = 0.85f;
    char p[640];
    cfg_join(p, sizeof(p), "settings.cfg");
    FILE* f = plat_fopen(p, "r");
#ifdef __SWITCH__
    if (!f) f = plat_fopen("sdmc:/Siren/settings.cfg", "r");
#endif
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
    ensure_dir_recursive(be_config_dir());
    char p[640];
    cfg_join(p, sizeof(p), "favorites.cfg");
    FILE* f = plat_fopen(p, "w");
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
    char p[640];
    cfg_join(p, sizeof(p), "favorites.cfg");
    FILE* f = plat_fopen(p, "r");
#ifdef __SWITCH__
    if (!f) f = plat_fopen("sdmc:/Siren/favorites.cfg", "r");
#endif
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

#ifdef __SWITCH__
// ---------- 六轴加速度计（单向顺时针竖屏自动检测） ----------
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
#else
static void sixaxis_init(void) {}
#endif

static void rebuild_lcd_ambient(void);
static float max_scroll_px(void);

static void apply_orientation(int o) {
    ori = o;
    if (ori == ORI_LAND) { SCR_W = 1280; SCR_H = 720; }
    else                 { SCR_W = 720;  SCR_H = 1280; }
    update_layout();
    int pw = SCR_W, ph = SCR_H;
    if (be_is_desktop()) be_viewport_size(&pw, &ph);
    if (!screen || screen->w != pw || screen->h != ph) {
        if (screen) canvas_free(screen);
        screen = be_is_desktop() ? canvas_new_scaled(SCR_W, SCR_H, (float)pw / (float)SCR_W)
                                 : canvas_new(SCR_W, SCR_H);
    }
    if (lcd_ambient && (lcd_ambient->w != LCD_W || lcd_ambient->h != LCD_H)) {
        canvas_free(lcd_ambient);
        lcd_ambient = NULL;
    }
    if (trans_shot) {
        canvas_free(trans_shot);
        trans_shot = NULL;
    }
    fade_time = FADE_DUR;
    fade_mode = 2;
    fade_armed = 1;
    wc_ori = -1;
    float ms = max_scroll_px();
    if (scroll_px > ms) scroll_px = ms;
    if (scroll_px < 0) scroll_px = 0;
    scroll_target = scroll_px;
    touch_drag = 0;
    wheel_active = 0;
    touch_press_idx = -1;
    if (cover_ready && cover_src) rebuild_lcd_ambient();
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
    if (top_px < scroll_target) scroll_target = top_px;
    else if (bot_px > scroll_target + view_h) scroll_target = bot_px - view_h;
    float ms = max_scroll_px();
    if (scroll_target < 0) scroll_target = 0;
    if (scroll_target > ms) scroll_target = ms;
    if (fabsf(scroll_target - scroll_px) < 1.0f) scroll_px = scroll_target;
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
    scroll_target = scroll_px;
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
    uint64_t now = plat_ticks_ns();
    if (g_last_tick) {
        double d = (double)(now - g_last_tick) / 1000000000.0;
        if (d < 0.001) d = 0.001;
        if (d > 0.05) d = 0.05;
        g_dt = d;
    }
    g_last_tick = now;
}

static void switch_page(int to) {
    if (to == ui_state) return;
    capture_fade();
    ui_state = to;
}

// ---------- 等比居中裁切缩放（修复非正方形封面拉伸与边缘异常） ----------
static void resample_cover_square(const uint32_t* src, int sw, int sh, uint32_t* dst, int dsz, int force_opaque) {
    if (!src || !dst || sw <= 0 || sh <= 0 || dsz <= 0) return;
    int side = sw < sh ? sw : sh;
    float ox = (sw - side) * 0.5f;
    float oy = (sh - side) * 0.5f;
    float scale = (float)side / (float)dsz;

    for (int y = 0; y < dsz; y++) {
        float fy = oy + (y + 0.5f) * scale - 0.5f;
        int y0 = (int)floorf(fy);
        float ty = fy - y0;
        if (y0 < 0) { y0 = 0; ty = 0.0f; }
        int y1 = y0 + 1;
        if (y1 >= sh) y1 = sh - 1;

        for (int x = 0; x < dsz; x++) {
            float fx = ox + (x + 0.5f) * scale - 0.5f;
            int x0 = (int)floorf(fx);
            float tx = fx - x0;
            if (x0 < 0) { x0 = 0; tx = 0.0f; }
            int x1 = x0 + 1;
            if (x1 >= sw) x1 = sw - 1;

            uint32_t p00 = src[(size_t)y0 * sw + x0];
            uint32_t p10 = src[(size_t)y0 * sw + x1];
            uint32_t p01 = src[(size_t)y1 * sw + x0];
            uint32_t p11 = src[(size_t)y1 * sw + x1];

            uint32_t out = 0;
            for (int ch = 0; ch < 4; ch++) {
                int shf = ch * 8;
                float a = (float)((p00 >> shf) & 0xFF);
                float b = (float)((p10 >> shf) & 0xFF);
                float c = (float)((p01 >> shf) & 0xFF);
                float d = (float)((p11 >> shf) & 0xFF);
                float v = (a + (b - a) * tx) * (1.0f - ty) + (c + (d - c) * tx) * ty;
                int iv = (int)(v + 0.5f);
                if (iv < 0) iv = 0;
                if (iv > 255) iv = 255;
                out |= ((uint32_t)iv) << shf;
            }
            if (force_opaque) out |= 0xFF000000u;
            dst[(size_t)y * dsz + x] = out;
        }
    }
}

// ---------- 基础图标绘制 ----------
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
    else if (ci_ends_with(path, ".m4a") || ci_ends_with(path, ".alac")) snprintf(out, cap, "M4A");
    else if (ci_ends_with(path, ".aac")) snprintf(out, cap, "AAC");
    else if (ci_ends_with(path, ".mp4") || ci_ends_with(path, ".m4b")) snprintf(out, cap, "MP4");
    else if (ci_ends_with(path, ".opus")) snprintf(out, cap, "OPUS");
    else if (ci_ends_with(path, ".ogg") || ci_ends_with(path, ".oga")) snprintf(out, cap, "OGG");
    else if (ci_ends_with(path, ".wav") || ci_ends_with(path, ".wave")) snprintf(out, cap, "WAV");
    else if (ci_ends_with(path, ".aiff") || ci_ends_with(path, ".aif") || ci_ends_with(path, ".aifc")) snprintf(out, cap, "AIFF");
    else if (ci_ends_with(path, ".wma")) snprintf(out, cap, "WMA");
    else if (ci_ends_with(path, ".caf")) snprintf(out, cap, "CAF");
    else if (ci_ends_with(path, ".ape")) snprintf(out, cap, "APE");
    else if (ci_ends_with(path, ".wv")) snprintf(out, cap, "WV");
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
    int bw = s / 6;
    if (bw < 3) bw = 3;
    fill_rect(c, x, y + 1, bw, s - 2, col);
    fill_tri_aa(c, x + s / 2 + 1, y + 1, x + s / 2 + 1, y + s - 1, x + bw, y + s / 2, col);
    fill_tri_aa(c, x + s - 1, y + 1, x + s - 1, y + s - 1, x + s / 2, y + s / 2, col);
}

static void icon_next(Canvas* c, int x, int y, int s, uint32_t col) {
    int bw = s / 6;
    if (bw < 3) bw = 3;
    fill_tri_aa(c, x + 1, y + 1, x + s / 2, y + s / 2, x + 1, y + s - 1, col);
    fill_tri_aa(c, x + s / 2 - 1, y + 1, x + s - bw, y + s / 2, x + s / 2 - 1, y + s - 1, col);
    fill_rect(c, x + s - bw, y + 1, bw, s - 2, col);
}

static void icon_play_pause(Canvas* c, int x, int y, int s, uint32_t col) {
    int h = s - 4;
    fill_tri_aa(c, x, y + 2, x + s * 9 / 20, y + s / 2, x, y + 2 + h, col);
    int bw = s / 7;
    if (bw < 3) bw = 3;
    fill_round_rect_aa(c, x + s * 11 / 20, y + 2, bw, h, 1, col);
    fill_round_rect_aa(c, x + s * 11 / 20 + bw + 3, y + 2, bw, h, 1, col);
}

static void icon_play_only(Canvas* c, int x, int y, int s, uint32_t col) {
    fill_tri_aa(c, x + 2, y + 1, x + s - 1, y + s / 2, x + 2, y + s - 1, col);
}

static void icon_pause_only(Canvas* c, int x, int y, int s, uint32_t col) {
    int bw = s / 4;
    if (bw < 3) bw = 3;
    int gap = s / 5;
    if (gap < 3) gap = 3;
    int x0 = x + (s - bw * 2 - gap) / 2;
    fill_round_rect_aa(c, x0, y + 2, bw, s - 4, 2, col);
    fill_round_rect_aa(c, x0 + bw + gap, y + 2, bw, s - 4, 2, col);
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

static void make_default_art(uint32_t* dst, int n, const char* seed) {
    uint32_t h = hash_str(seed);
    uint8_t r, g, b;
    hsv2rgb((float)((h % 360) / 360.0), 0.45f, 0.46f, &r, &g, &b);
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            float t = (x + y) / (float)(2 * n);
            dst[(size_t)y * n + x] = COL((uint8_t)(r * (1 - t) + 28 * t),
                                         (uint8_t)(g * (1 - t) + 32 * t),
                                         (uint8_t)(b * (1 - t) + 44 * t), 255);
        }
    }
    Canvas c = { .w = n, .h = n, .lw = n, .lh = n, .s = 1.0f, .px = dst, .clip_x0 = 0, .clip_y0 = 0, .clip_x1 = n, .clip_y1 = n };
    draw_note(&c, n / 6, n / 6, n * 2 / 3, COL(255, 255, 255, 225));
}

// Apple Music 风格柔和封面流体光晕（渲染到全屏播放 / 桌面英雄卡片内部）
static void rebuild_lcd_ambient(void) {
    if (!cover_src) return;
    int aw = be_is_desktop() ? (desktop_full ? 1240 : 484) : LCD_W;
    int ah = be_is_desktop() ? 550 : LCD_H;
    if (lcd_ambient && (lcd_ambient->w != aw || lcd_ambient->h != ah)) {
        canvas_free(lcd_ambient);
        lcd_ambient = NULL;
    }
    if (!lcd_ambient) lcd_ambient = canvas_new(aw, ah);
    if (!lcd_ambient) return;

    uint32_t small[48 * 48];
    resample_bilinear(cover_src, COVER_SRC_SZ, COVER_SRC_SZ, small, 48, 48);
    box_blur(small, 48, 48, 4, 2);
    resample_bilinear(small, 48, 48, lcd_ambient->px, aw, ah);

    // 与当前主题 Surface 柔和融合，保证浅色与深色下文字都极度清晰
    uint8_t sr = C_SURFACE & 0xFF, sg = (C_SURFACE >> 8) & 0xFF, sb = (C_SURFACE >> 16) & 0xFF;
    float mix = g_is_light ? 0.80f : 0.72f;
    size_t total = (size_t)aw * ah;
    for (size_t i = 0; i < total; i++) {
        uint32_t c = lcd_ambient->px[i];
        uint8_t r = (uint8_t)((c & 0xFF) * (1.0f - mix) + sr * mix);
        uint8_t g = (uint8_t)(((c >> 8) & 0xFF) * (1.0f - mix) + sg * mix);
        uint8_t b = (uint8_t)(((c >> 16) & 0xFF) * (1.0f - mix) + sb * mix);
        lcd_ambient->px[i] = COL(r, g, b, 255);
    }
}

static void build_poster_thumbs(void) {
    size_t poster_len = (size_t)(poster_end - poster_start);
    logo_img    = (uint32_t*)calloc(LOGO_SZ * LOGO_SZ, 4);
    poster_mini = (uint32_t*)calloc(MINI_ART_SZ * MINI_ART_SZ, 4);
    poster_thm  = (uint32_t*)calloc(THUMB_SZ * THUMB_SZ, 4);
    poster_pod  = (uint32_t*)calloc(POD_ART_SZ * POD_ART_SZ, 4);

    if (poster_len >= 320 * 320 * 4) {
        const uint32_t* src = (const uint32_t*)poster_start;
        if (logo_img)    resample_cover_square(src, 320, 320, logo_img,    LOGO_SZ,     0);
        if (poster_mini) resample_cover_square(src, 320, 320, poster_mini, MINI_ART_SZ, 1);
        if (poster_thm)  resample_cover_square(src, 320, 320, poster_thm,  THUMB_SZ,    1);
        if (poster_pod)  resample_cover_square(src, 320, 320, poster_pod,  POD_ART_SZ,  1);
    } else {
        if (logo_img)    make_default_art(logo_img,    LOGO_SZ,     "SirenLogo");
        if (poster_mini) make_default_art(poster_mini, MINI_ART_SZ, "SirenMini");
        if (poster_thm)  make_default_art(poster_thm,  THUMB_SZ,    "SirenThumb");
        if (poster_pod)  make_default_art(poster_pod,  POD_ART_SZ,  "SirenPod");
    }
}

// ---------- 目录安全扫描 ----------
static int should_skip_dir(const char* name) {
    if (!name || !*name) return 1;
    uint32_t h = 0x811c9dc5u;
    for (const unsigned char* p = (const unsigned char*)name; *p; p++) {
        unsigned char c = *p;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + ('a' - 'A'));
        h ^= c;
        h *= 0x01000193u;
    }
    static const uint32_t skip_hashes[] = {
        0x5595F010u, 0xD7FE1AD5u, 0xC54A59D2u, 0x740B7BA0u,
        0x23FF66C7u, 0xD55997BCu, 0x93E05F71u, 0x491E0A9Cu,
        0x74F012C0u, 0x35B3A5ABu, 0x7679D3E1u, 0x267C9E9Bu,
        0x7AA6946Cu, 0x97F6E475u, 0x856B932Fu, 0x32AFA6DFu,
        0x2DD29BE2u, 0xCC4CBD9Eu, 0xFB7B53DBu, 0x76B22013u,
        0x1527CD10u, 0xFA1B0B2Cu, 0xA3603574u, 0x377AC524u,
        0xF06DF42Au
    };
    for (unsigned i = 0; i < sizeof(skip_hashes) / sizeof(skip_hashes[0]); i++)
        if (h == skip_hashes[i]) return 1;
    return 0;
}

static void join_clean_path(char* dst, size_t cap, const char* dir, const char* name) {
    size_t len = strlen(dir);
    while (len > 0 && (dir[len - 1] == '/' || dir[len - 1] == '\\')) len--;
    if (len == 0) snprintf(dst, cap, "/%s", name);
    else snprintf(dst, cap, "%.*s/%s", (int)len, dir, name);
}

static void scan_dir(const char* dir, int depth) {
    if (!dir || !*dir || depth > 5 || ntracks >= MAX_TRACKS) return;
    PlatDir* d = plat_opendir(dir);
    if (!d) return;
    PlatDirent e;
    while (plat_readdir(d, &e)) {
        if (e.d_name[0] == '.') continue;
        char path[600];
        join_clean_path(path, sizeof(path), dir, e.d_name);

        int is_dir = e.is_dir, is_reg = e.is_reg;
        if (!is_dir && !is_reg) {
            if (plat_stat_type(path, &is_dir, &is_reg) != 0) continue;
        }

        if (is_dir) {
            if (!should_skip_dir(e.d_name)) scan_dir(path, depth + 1);
        } else if (is_reg && dec_is_audio(path)) {
            if (ntracks < MAX_TRACKS) {
                snprintf(tracks[ntracks].path, sizeof(tracks[ntracks].path), "%s", path);
                ntracks++;
            }
        }
    }
    plat_closedir(d);
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
        free(tracks[i].thumb_pod);
        meta_clear(&tracks[i].meta);
    }
    memset(tracks, 0, sizeof(Track) * MAX_TRACKS);
    ntracks = 0;
    loaded_thumbs = 0;
    fav_count = 0;
    playing_idx = -1;

    if (be_is_desktop()) {
        if (custom_music_dir[0]) scan_dir(custom_music_dir, 0);
        char defs[4][600];
        int nd = be_default_music_dirs(defs, 4);
        for (int i = 0; i < nd && ntracks < MAX_TRACKS; i++) {
            if (custom_music_dir[0] && !strcasecmp(defs[i], custom_music_dir)) continue;
            scan_dir(defs[i], 0);
        }
    } else {
        plat_mkdir_p("sdmc:/music");
        const char* roots[] = {
            "sdmc:/music", "sdmc:/Music", "sdmc:/Musics",
            "sdmc:/Songs", "sdmc:/Audio", "sdmc:/Playlist",
            "sdmc:/Downloads", "sdmc:/MP3", "sdmc:/FLAC",
            "sdmc:/音乐", "sdmc:/歌曲", "sdmc:/CloudMusic", "sdmc:/QQMusic",
            "sdmc:/switch/Sirene", "sdmc:/switch/Siren", "sdmc:/switch/music"
        };
        for (unsigned i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
            scan_dir(roots[i], 0);
            if (ntracks >= MAX_TRACKS) break;
        }
        if (ntracks < MAX_TRACKS) scan_dir("sdmc:/", 0);
    }

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
    scroll_target = 0;
    rebuild_visible();

    if (notify) {
        capture_fade();
        char msg[96];
        if (ntracks > 0) snprintf(msg, sizeof(msg), "扫描完成：共找到 %d 首歌曲", ntracks);
        else snprintf(msg, sizeof(msg), be_is_desktop() ? "未发现音频，点“选目录”或拖入歌曲" : "未发现音频，请放入 sdmc:/music");
        show_toast(msg);
    }
}

// ---------- 元数据 / 封面懒加载 ----------
static void ensure_track_meta(int idx, int full_scan) {
    if (idx < 0 || idx >= ntracks) return;
    Track* t = &tracks[idx];
    if (t->meta_state) return;
    t->meta_state = 1;
    loaded_thumbs++;

    if (!t->thumb)     t->thumb     = (uint32_t*)calloc(THUMB_SZ * THUMB_SZ, 4);
    if (!t->thumb_pod) t->thumb_pod = (uint32_t*)calloc(POD_ART_SZ * POD_ART_SZ, 4);

    if (full_scan) meta_load(t->path, &t->meta);
    else meta_load_quick(t->path, &t->meta);

    if (t->meta.art && t->meta.art_w > 8 && t->meta.art_h > 8) {
        if (t->thumb)
            resample_cover_square((const uint32_t*)t->meta.art, t->meta.art_w, t->meta.art_h, t->thumb, THUMB_SZ, 1);
        if (t->thumb_pod)
            resample_cover_square((const uint32_t*)t->meta.art, t->meta.art_w, t->meta.art_h, t->thumb_pod, POD_ART_SZ, 1);
        meta_release_art(&t->meta);
    } else {
        if (t->thumb && poster_thm) memcpy(t->thumb, poster_thm, (size_t)THUMB_SZ * THUMB_SZ * 4);
        else if (t->thumb) make_default_art(t->thumb, THUMB_SZ, t->path);
        if (t->thumb_pod && poster_pod) memcpy(t->thumb_pod, poster_pod, (size_t)POD_ART_SZ * POD_ART_SZ * 4);
        else if (t->thumb_pod) make_default_art(t->thumb_pod, POD_ART_SZ, t->path);
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
        free(t->thumb_pod); t->thumb_pod = NULL;
        meta_clear(&t->meta);
        t->meta_state = 0;
        if (loaded_thumbs > 0) loaded_thumbs--;
    }
}

static void ensure_visible_thumbs(void) {
    if (fade_time > 0.0f) return;
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

// ---------- 切歌与曲目加载 ----------
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

static void sync_player_covers(void) {
    if (!cover_src) return;
    if (!cover_land) cover_land = (uint32_t*)calloc(COVER_LAND_SZ * COVER_LAND_SZ, 4);
    if (!cover_port) cover_port = (uint32_t*)calloc(COVER_PORT_SZ * COVER_PORT_SZ, 4);
    if (!cover_mini) cover_mini = (uint32_t*)calloc(MINI_ART_SZ * MINI_ART_SZ, 4);
    if (cover_land) resample_cover_square(cover_src, COVER_SRC_SZ, COVER_SRC_SZ, cover_land, COVER_LAND_SZ, 1);
    if (cover_port) resample_cover_square(cover_src, COVER_SRC_SZ, COVER_SRC_SZ, cover_port, COVER_PORT_SZ, 1);
    if (cover_mini) resample_cover_square(cover_src, COVER_SRC_SZ, COVER_SRC_SZ, cover_mini, MINI_ART_SZ, 1);
    cover_ready = 1;
    rebuild_lcd_ambient();
}

static void load_track(int idx) {
    if (idx < 0 || idx >= ntracks) return;
    touch_scrub = 0;
    scrub_preview_sec = 0.0;
    capture_fade();
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

    if (!cover_src) cover_src = (uint32_t*)calloc((size_t)COVER_SRC_SZ * COVER_SRC_SZ, 4);
    if (cover_src) {
        if (meta.art && meta.art_w > 8 && meta.art_h > 8) {
            resample_cover_square((const uint32_t*)meta.art, meta.art_w, meta.art_h, cover_src, COVER_SRC_SZ, 1);
        } else if ((size_t)(poster_end - poster_start) >= 320 * 320 * 4) {
            resample_cover_square((const uint32_t*)poster_start, 320, 320, cover_src, COVER_SRC_SZ, 1);
        } else {
            make_default_art(cover_src, COVER_SRC_SZ, tracks[idx].path);
        }
        sync_player_covers();
    }

    g_lyr_req_id = 0;
    lyr = lyrics_load_ex(tracks[idx].path, p_title, p_artist);
    if (!lyr && be_is_desktop()) {
        g_lyr_req_id = lyrics_start_online(tracks[idx].path, p_title, p_artist, audio_duration());
    }
    for (int i = 0; i < 28; i++) bars[i] = 0;
    loading = 0;
    g_last_tick = plat_ticks_ns();
}

static void toggle_fullscreen_player(void) {
    if (ui_state == UI_PLAYER) {
        switch_page(UI_LIST);
        sel = playing_idx >= 0 ? playing_idx : sel;
        rebuild_visible();
    } else {
        if (playing_idx >= 0) {
            switch_page(UI_PLAYER);
        } else if (nvisible > 0 && !loading) {
            int idx = (sel >= 0) ? sel : vis_at(0);
            if (idx >= 0) {
                load_track(idx);
                switch_page(UI_PLAYER);
            }
        } else {
            show_toast("暂无曲目，请先扫描 SD 卡");
        }
    }
}

// 列表就地播放（A 键 / 点按列表曲目 / 转盘中心确认：不跳进全屏！）
static void play_or_toggle_in_list(int idx) {
    if (idx < 0 || idx >= ntracks || loading) return;
    sel = idx;
    if (playing_idx == idx) {
        if (audio_paused()) audio_resume();
        else audio_pause();
    } else {
        load_track(idx);
    }
}

static void step_track_dir(int dir) {
    if (ntracks <= 0 || loading) return;
    int base = playing_idx >= 0 ? playing_idx : (sel >= 0 ? sel : 0);
    int nxt = (dir > 0) ? pick_next(base, 1) : pick_prev(base);
    load_track(nxt);
    sel = nxt;
    ensure_pos_visible(sel_pos());
}

// ---------- 按键重复与搜索软键盘 ----------
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

static double calc_scrub_sec(int x, int x0, int x1) {
    double d = audio_duration();
    if (d <= 0 || x1 <= x0) return 0.0;
    if (x < x0) x = x0;
    if (x > x1) x = x1;
    double frac = (double)(x - x0) / (double)(x1 - x0);
    double s = frac * d;
    if (d > 0.5 && s > d - 0.25) s = d - 0.25;
    return s;
}

static void set_view(int v) {
    if (view == v) return;
    capture_fade();
    view = v;
    scroll_px = 0;
    scroll_target = 0;
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
    capture_fade();
    apply_theme((cur_theme + 1) % THEME_COUNT);
    if (cover_ready && cover_src) rebuild_lcd_ambient();
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
    if (be_is_desktop()) return;
    ori_locked = 1;
    apply_orientation(ori == ORI_LAND ? ORI_PORT : ORI_LAND);
    show_toast(ori == ORI_PORT ? "竖屏 iPod 模式" : "横屏双栏模式");
}

static void apply_search_query(const char* q) {
    capture_fade();
    snprintf(search_q, sizeof(search_q), "%s", q ? q : "");
    view = search_q[0] ? VIEW_SEARCH : VIEW_HOME;
    scroll_px = 0;
    scroll_target = 0;
    rebuild_visible();
    if (search_q[0]) {
        char msg[96];
        snprintf(msg, sizeof(msg), "找到 %d 首匹配歌曲", nvisible);
        show_toast(msg);
    } else {
        show_toast("已显示全部歌曲");
    }
}

static void open_desktop_folder(void) {
    char picked[600] = {0};
    if (!be_pick_folder(picked, sizeof(picked)) || !picked[0]) return;
    snprintf(custom_music_dir, sizeof(custom_music_dir), "%s", picked);
    save_custom_dir();
    scan_library(1);
}

static void handle_dropped_path(const char* raw_p) {
    if (!raw_p || !*raw_p) return;
    char clean[600];
    snprintf(clean, sizeof(clean), "%s", raw_p);
    for (char* q = clean; *q; q++) if (*q == '\\') *q = '/';

    int is_dir = 0, is_reg = 0;
    if (plat_stat_type(clean, &is_dir, &is_reg) != 0) return;

    if (is_dir) {
        snprintf(custom_music_dir, sizeof(custom_music_dir), "%s", clean);
        save_custom_dir();
        scan_library(1);
    } else if (is_reg && dec_is_audio(clean)) {
        const char* sep = path_last_sep(clean);
        if (sep && sep > clean) {
            size_t dlen = (size_t)(sep - clean);
            if (dlen >= sizeof(custom_music_dir)) dlen = sizeof(custom_music_dir) - 1;
            memcpy(custom_music_dir, clean, dlen);
            custom_music_dir[dlen] = 0;
            save_custom_dir();
            scan_library(0);
        }
        int hit = -1;
        for (int i = 0; i < ntracks; i++) {
            if (!strcasecmp(tracks[i].path, clean)) { hit = i; break; }
        }
        if (hit < 0 && ntracks < MAX_TRACKS) {
            hit = ntracks++;
            snprintf(tracks[hit].path, sizeof(tracks[hit].path), "%s", clean);
            rebuild_visible();
        }
        if (hit >= 0) {
            sel = hit;
            ensure_pos_visible(sel_pos());
            load_track(hit);
            show_toast(p_title[0] ? p_title : "开始播放拖入曲目");
        }
    } else if (is_reg && ci_ends_with(clean, ".lrc")) {
        Lyrics* nl = lyrics_load_file(clean);
        if (nl) {
            if (lyr) lyrics_free(lyr);
            lyr = nl;
            show_toast("已加载拖入 .lrc 歌词");
        } else {
            show_toast("拖入 .lrc 歌词解析失败");
        }
    }
}

static void open_search_keyboard(void) {
#ifdef __SWITCH__
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
    g_last_tick = plat_ticks_ns();

    if (R_SUCCEEDED(rc)) apply_search_query(out);
#else
    capture_fade();
    snprintf(search_edit, sizeof(search_edit), "%s", search_q);
    show_search_box = 1;
    show_vol_panel = 0;
    show_exit_confirm = 0;
    show_diag = 0;
    be_text_input(1);
#endif
}

// ---------- Click Wheel 乔布斯转盘交互 ----------
static void trigger_wheel_zone(int zone) {
    if (zone == 0) {
        // 中心键：列表页就地播放（不进全屏），全屏页暂停/恢复
        if (ui_state == UI_LIST) {
            if (nvisible > 0) {
                int i = sel >= 0 ? sel : vis_at(0);
                play_or_toggle_in_list(i);
            } else {
                scan_library(1);
            }
        } else {
            if (playing_idx >= 0) {
                if (audio_paused()) audio_resume(); else audio_pause();
            }
        }
    } else if (zone == 1) {
        // MENU 键：在全屏页返回列表，在列表页切换 全部/喜欢
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
        // 右键 >>| 下一首
        step_track_dir(+1);
    } else if (zone == 3) {
        // 下键 >|| 播放/暂停
        if (playing_idx >= 0) {
            if (audio_paused()) audio_resume(); else audio_pause();
        } else if (nvisible > 0) {
            play_or_toggle_in_list(sel >= 0 ? sel : vis_at(0));
        }
    } else if (zone == 4) {
        // 左键 |<< 上一首
        step_track_dir(-1);
    }
}

static int handle_wheel_touch(TouchInfo* t) {
    int dx = t->x - W_CX, dy = t->y - W_CY;
    int d2 = dx * dx + dy * dy;

    if (wheel_active && t->held) {
        if (wheel_down_zone > 0 && d2 >= (W_RI - 18) * (W_RI - 18)) {
            float ang = atan2f((float)dy, (float)dx);
            float da = ang - wheel_last_ang;
            while (da >  (float)M_PI) da -= 2.0f * (float)M_PI;
            while (da < -(float)M_PI) da += 2.0f * (float)M_PI;
            wheel_last_ang = ang;
            wheel_accum += da;
            const float step = 0.28f;
            if (fabsf(wheel_accum) >= step) wheel_moved = 1;
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

// ---------- 弹窗与通用触摸交互 ----------
static void set_vol_from_touch(int x) {
    if (vol_sx1 <= vol_sx0) return;
    float frac = (float)(x - vol_sx0) / (float)(vol_sx1 - vol_sx0);
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    audio_set_volume(frac);
    if (frac > 0.01f) saved_mute_vol = frac;
    save_settings();
}

static void handle_touch_vol_panel(TouchInfo* t) {
    if (touch_vol_scrub && t->held) {
        set_vol_from_touch(t->x);
        return;
    }
    if (touch_vol_scrub && t->up) {
        touch_vol_scrub = 0;
        return;
    }
    if (!t->down) return;
    int x = t->x, y = t->y;
    if (in_rc(x, y, rc_vol_close) || !in_rc(x, y, rc_vol_box)) {
        capture_fade();
        show_vol_panel = 0;
        touch_vol_scrub = 0;
        return;
    }
    if (in_rc(x, y, rc_vol_minus)) {
        float v = audio_volume() - 0.05f;
        if (v < 0.0f) v = 0.0f;
        audio_set_volume(v);
        if (v > 0.01f) saved_mute_vol = v;
        save_settings();
        return;
    }
    if (in_rc(x, y, rc_vol_plus)) {
        float v = audio_volume() + 0.05f;
        if (v > 1.0f) v = 1.0f;
        audio_set_volume(v);
        saved_mute_vol = v;
        save_settings();
        return;
    }
    if (in_rc(x, y, rc_vol_mute)) {
        if (audio_volume() > 0.01f) {
            saved_mute_vol = audio_volume();
            audio_set_volume(0.0f);
        } else {
            audio_set_volume(saved_mute_vol > 0.05f ? saved_mute_vol : 0.85f);
        }
        save_settings();
        return;
    }
    if (in_rect(x, y, vol_sx0 - 14, vol_sy - 22, vol_sx1 - vol_sx0 + 28, 54)) {
        touch_vol_scrub = 1;
        set_vol_from_touch(x);
    }
}

static void handle_touch_exit_confirm(TouchInfo* t) {
    if (!t->down) return;
    int x = t->x, y = t->y;
    if (in_rc(x, y, rc_exit_ok)) {
        g_want_exit = 1;
        return;
    }
    if (in_rc(x, y, rc_exit_cancel) || !in_rc(x, y, rc_exit_box)) {
        capture_fade();
        show_exit_confirm = 0;
    }
}

static int handle_common_touch(int x, int y) {
    if (in_rc(x, y, rc_btn_vol)) {
        show_vol_panel = !show_vol_panel;
        show_exit_confirm = 0;
        show_diag = 0;
        if (show_vol_panel) trigger_modal_fade();
        return 1;
    }
    if (in_rc(x, y, rc_btn_ori)) {
        if (be_is_desktop()) open_desktop_folder();
        else toggle_orientation();
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
    if (in_rc(x, y, rc_btn_theme)) {
        cycle_theme();
        return 1;
    }
    if (in_rc(x, y, rc_btn_diag)) {
        show_diag = 1;
        show_vol_panel = 0;
        show_exit_confirm = 0;
        trigger_modal_fade();
        return 1;
    }
    if (in_rc(x, y, rc_btn_exit)) {
        show_exit_confirm = 1;
        show_vol_panel = 0;
        show_diag = 0;
        trigger_modal_fade();
        return 1;
    }
    return 0;
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
            scroll_target = scroll_px;
        }
        touch_last_y = t->y;
    }

    if (!t->down) {
        if (t->up && touch_drag) {
            if (!touch_moved && touch_press_idx >= 0 && touch_press_idx < nvisible) {
                int idx = vis_at(touch_press_idx);
                if (idx >= 0) {
                    if (t->x >= L_X1 - 66 && t->x <= L_X1) {
                        toggle_fav(idx);
                    } else {
                        // 点选歌曲：就地播放，不强行跳进全屏
                        play_or_toggle_in_list(idx);
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
    if (handle_common_touch(x, y)) return;

    if (in_rc(x, y, rc_tab_all))  { search_q[0] = 0; set_view(VIEW_HOME); return; }
    if (in_rc(x, y, rc_tab_fav))  { set_view(view == VIEW_FAVORITE ? VIEW_HOME : VIEW_FAVORITE); return; }
    if (in_rc(x, y, rc_btn_rescan)) { scan_library(1); return; }
    if (in_rc(x, y, rc_empty_rescan)) {
        if (be_is_desktop()) open_desktop_folder();
        else scan_library(1);
        return;
    }
    if (in_rc(x, y, rc_btn_mode)) { cycle_play_mode(); return; }
    if (in_rc(x, y, rc_btn_full) || in_rc(x, y, rc_pod_card) || in_rc(x, y, rc_mini_bar)) {
        toggle_fullscreen_player();
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
    if (touch_scrub) {
        if (t->held || t->down) {
            scrub_preview_sec = calc_scrub_sec(t->x, ui_px0, ui_px1);
            return;
        }
        if (t->up) {
            scrub_preview_sec = calc_scrub_sec(t->x, ui_px0, ui_px1);
            touch_scrub = 0;
            audio_seek(scrub_preview_sec);
            return;
        }
        touch_scrub = 0;
    }

    if (handle_wheel_touch(t)) return;
    if (!t->down) return;

    int x = t->x, y = t->y;
    if (handle_common_touch(x, y)) return;

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
    if (in_rect(x, y, ui_px0 - 12, ui_py - 20, ui_px1 - ui_px0 + 24, 48)) {
        if (audio_duration() > 0) {
            touch_scrub = 1;
            scrub_preview_sec = calc_scrub_sec(x, ui_px0, ui_px1);
        }
        return;
    }
    if (in_rect(x, y, ui_cover_x, ui_cover_y, ui_cover_sz, ui_cover_sz)) {
        if (audio_paused()) audio_resume(); else audio_pause();
    }
}

// ---------- Apple × Material You 共享组件与 Click Wheel 绘制 ----------
static Rect draw_m3_pill_fixed(int x, int y, int w, int h, float px, const char* txt, int active, int is_danger) {
    uint32_t bgc = C_CHIP;
    uint32_t fgc = C_TXT2;
    if (is_danger) {
        bgc = g_is_light ? COL(0xFD, 0xE2, 0xE8, 0xFF) : COL(0x3B, 0x1A, 0x24, 0xFF);
        fgc = C_HEART;
    } else if (active) {
        bgc = C_TONAL_BG;
        fgc = C_TONAL_TXT;
    }
    fill_round_rect_aa(screen, x, y, w, h, h / 2, bgc);
    draw_text_centered(screen, font, px, x + 4, x + w - 4, y + (h - (int)px) / 2 - 1, fgc, txt);
    Rect r = { x, y, w, h };
    return r;
}

static void draw_m3_tab(Rect* rc, int x, int y, int h, float px, const char* txt, int active, int primary) {
    int tw = text_width(font, px, txt);
    int w = tw + 22;
    uint32_t bgc = primary ? C_ACCENT : (active ? C_TONAL_BG : C_SURF_SUB);
    uint32_t fgc = primary ? C_ON_ACCENT : (active ? C_TONAL_TXT : C_TXT2);
    fill_round_rect_aa(screen, x, y, w, h, h / 2, bgc);
    draw_text_centered(screen, font, px, x, x + w, y + (h - (int)px) / 2 - 1, fgc, txt);
    *rc = (Rect){ x, y, w, h };
}

static void draw_lcd_shell(const char* title, const char* right_sub, int use_ambient) {
    canvas_clear(screen, C_BODY);

    // 现代 Apple 无黑框悬浮圆角视窗（柔和投影 + 1px 玻璃微描边）
    fill_round_rect_shadow(screen, LCD_X, LCD_Y, LCD_W, LCD_H, 28, g_is_light ? 26 : 52);
    if (use_ambient && lcd_ambient && lcd_ambient->w == LCD_W && lcd_ambient->h == LCD_H) {
        blit_rgb_round(screen, LCD_X, LCD_Y, lcd_ambient->px, LCD_W, LCD_H, 28);
    } else {
        fill_round_rect_aa(screen, LCD_X, LCD_Y, LCD_W, LCD_H, 28, C_SURFACE);
    }
    stroke_round_rect_aa(screen, LCD_X, LCD_Y, LCD_W, LCD_H, 28, 1,
                         g_is_light ? COL(255, 255, 255, 255) : C_WHEEL_RING, g_is_light ? 190 : 120);

    // 左上角严格 30x30 匹配步长绘制图标，绝不错位撕裂
    if (logo_img) {
        blit_rgba_round(screen, LCD_X + 18, LCD_Y + 13, logo_img, LOGO_SZ, LOGO_SZ, 8);
    }
    draw_text(screen, font, 20, LCD_X + 56, LCD_Y + 17, C_TXT, title);

    if (right_sub && right_sub[0]) {
        draw_text_right(screen, font, 13, LCD_X + LCD_W - 20, LCD_Y + 21, C_TXT3, right_sub);
    }
}

static void draw_click_wheel(void) {
    int ro = W_RO, ri = W_RI;
    int wsz = (ro + 10) * 2;
    int wx0 = W_CX - (ro + 10), wy0 = W_CY - (ro + 10);
    int hl = (wheel_active && !wheel_moved) ? wheel_down_zone : -1;
    int is_list = (ui_state == UI_LIST);
    int sel_play = (playing_idx >= 0 && playing_idx == sel) ? 1 : 0;
    int paused = (playing_idx >= 0) ? audio_paused() : 1;

    if (!wheel_cache || wheel_cache->w != wsz || wheel_cache->h != wsz) {
        if (wheel_cache) canvas_free(wheel_cache);
        wheel_cache = canvas_new(wsz, wsz);
        wc_ori = -1;
    }

    if (wheel_cache && (wc_ori != ori || wc_theme != cur_theme || wc_ro != ro || wc_ri != ri ||
                        wc_zone != hl || wc_list != is_list || wc_paused != paused || wc_sel_play != sel_play)) {
        int lcx = wsz / 2, lcy = wsz / 2;
        canvas_clear(wheel_cache, C_BODY);
        fill_circle_a(wheel_cache, lcx, lcy + 5, ro + 4, COL(0, 0, 0, 255), g_is_light ? 20 : 48);
        fill_ellipse_aa(wheel_cache, lcx, lcy, ro, ro, C_WHEEL);
        stroke_circle_aa(wheel_cache, lcx, lcy, ro, 2, C_WHEEL_RING, 185);

        int r_mid = (ro + ri) / 2;
        if (hl > 0) {
            int hx = lcx, hy = lcy;
            if (hl == 1) hy = lcy - r_mid;
            else if (hl == 2) hx = lcx + r_mid;
            else if (hl == 3) hy = lcy + r_mid;
            else if (hl == 4) hx = lcx - r_mid;
            fill_circle_a(wheel_cache, hx, hy, (ro - ri) / 2, C_ACCENT, 42);
        }

        float mpx = portrait() ? 17.0f : 14.0f;
        draw_text_centered(wheel_cache, font, mpx, lcx - 60, lcx + 60, lcy - r_mid - (int)mpx / 2,
                           C_WHEEL_TXT, "MENU");

        int ic = portrait() ? 20 : 16;
        icon_prev(wheel_cache, lcx - r_mid - ic / 2, lcy - ic / 2, ic, C_WHEEL_TXT);
        icon_next(wheel_cache, lcx + r_mid - ic / 2, lcy - ic / 2, ic, C_WHEEL_TXT);

        int pwc = portrait() ? 22 : 18;
        icon_play_pause(wheel_cache, lcx - pwc / 2, lcy + r_mid - pwc / 2 + 1, pwc, C_WHEEL_TXT);

        uint32_t ccol = (hl == 0) ? C_TONAL_BG : C_WHEEL_CENTER;
        fill_ellipse_aa(wheel_cache, lcx, lcy, ri, ri, ccol);
        stroke_circle_aa(wheel_cache, lcx, lcy, ri, 1, C_WHEEL_RING, 190);
        const char* clbl = is_list
            ? (sel_play ? (paused ? "播放" : "暂停") : "播放")
            : (paused ? "播放" : "暂停");
        draw_text_centered(wheel_cache, font, portrait() ? 16.0f : 14.0f, lcx - ri, lcx + ri,
                           lcy - (portrait() ? 9 : 8),
                           (hl == 0) ? C_TONAL_TXT : C_WHEEL_TXT, clbl);

        wc_ori = ori; wc_theme = cur_theme; wc_ro = ro; wc_ri = ri;
        wc_zone = hl; wc_list = is_list; wc_paused = paused; wc_sel_play = sel_play;
    }

    if (wheel_cache) blit_canvas_opaque(screen, wx0, wy0, wheel_cache);
}

static void draw_hardware_controls(void) {
    char vol_lbl[24], th_lbl[32];
    snprintf(vol_lbl, sizeof(vol_lbl), "音量 %d%%", (int)(audio_volume() * 100 + 0.5f));
    snprintf(th_lbl, sizeof(th_lbl), "主题:%s", theme_names[cur_theme]);
    const char* sr_lbl = (view == VIEW_SEARCH && search_q[0]) ? "清除搜索" : "搜索";

    if (!portrait()) {
        // 横屏右上：现代 Apple 迷你播放灵动卡片（86x86 严格匹配步长）
        int px = 866, py = 22, pw = 390, ph = 134;
        fill_round_rect_shadow(screen, px, py, pw, ph, 24, g_is_light ? 20 : 44);
        fill_round_rect_aa(screen, px, py, pw, ph, 24, C_SURFACE);
        stroke_round_rect_aa(screen, px, py, pw, ph, 24, 1, C_WHEEL_RING, 110);
        rc_pod_card = (Rect){ px, py, pw, ph };

        if (playing_idx >= 0) {
            const uint32_t* pimg = tracks[playing_idx].thumb_pod ? tracks[playing_idx].thumb_pod : poster_pod;
            if (pimg) blit_rgb_round(screen, px + 16, py + 24, pimg, POD_ART_SZ, POD_ART_SZ, 18);

            const char* badge = (ui_state == UI_PLAYER)
                ? "点击返回曲库 [X]"
                : (audio_paused() ? "已暂停 · 点此全屏 [X]" : "播放中 · 点此全屏 [X]");
            draw_text(screen, font, 12, px + 116, py + 22, C_TONAL_TXT, badge);
            draw_text_max(screen, font, 18, px + 116, py + 44, C_TXT, p_title, pw - 132);
            draw_text_max(screen, font, 13, px + 116, py + 72, C_TXT2, p_artist[0] ? p_artist : p_fmt, pw - 132);

            // 迷你实时单行歌词或进度时间
            double pos = audio_position(), dur = audio_duration();
            if (lyr && lyr->n > 0) {
                int act = lyr->unsynced
                    ? (dur > 0.0 && lyr->n > 1 ? (int)((pos / dur) * (double)(lyr->n - 1)) : 0)
                    : lyrics_index(lyr, pos);
                if (act < 0) act = 0;
                if (act >= lyr->n) act = lyr->n - 1;
                draw_text_max(screen, font, 13, px + 116, py + 96, C_ACCENT, lyr->lines[act].text, pw - 132);
            } else {
                char t1[16], t2[16], tm[48];
                fmt_time(pos, t1, sizeof(t1));
                fmt_time(dur, t2, sizeof(t2));
                snprintf(tm, sizeof(tm), "%s / %s · %s", t1, t2, mode_short[play_mode]);
                draw_text(screen, font, 13, px + 116, py + 96, C_TXT3, tm);
            }
        } else {
            if (poster_pod) blit_rgb_round(screen, px + 16, py + 24, poster_pod, POD_ART_SZ, POD_ART_SZ, 18);
            draw_text(screen, font, 18, px + 116, py + 34, C_TXT, "Sirène");
            draw_text(screen, font, 13, px + 116, py + 62, C_TXT2, "A 就地播放 · X 全屏歌词");
            draw_text(screen, font, 12, px + 116, py + 88, C_TXT3, "Y 切换竖屏 · 左方向键切歌");
        }

        draw_click_wheel();

        // 右下 3x2 全触控胶囊矩阵（无手柄也可完成音量/旋转/搜索/主题/自检/退出）
        int gx = 866, bw = 124, gap = 9, bh = 42;
        int r1y = 528, r2y = 580;
        rc_btn_vol    = draw_m3_pill_fixed(gx,                r1y, bw, bh, 13.5f, vol_lbl, show_vol_panel, 0);
        rc_btn_ori    = draw_m3_pill_fixed(gx + bw + gap,     r1y, bw, bh, 13.5f, "竖屏 [Y]", 0, 0);
        rc_btn_search = draw_m3_pill_fixed(gx + 2*(bw + gap), r1y, bw, bh, 13.5f, sr_lbl, view == VIEW_SEARCH, 0);

        rc_btn_theme  = draw_m3_pill_fixed(gx,                r2y, bw, bh, 13.5f, th_lbl, 0, 0);
        rc_btn_diag   = draw_m3_pill_fixed(gx + bw + gap,     r2y, bw, bh, 13.5f, "自检", show_diag, 0);
        rc_btn_exit   = draw_m3_pill_fixed(gx + 2*(bw + gap), r2y, bw, bh, 13.5f, "退出", 0, 1);

        draw_text_centered(screen, font, 12, gx, gx + 390, 642, C_TXT3,
                           "滑转盘选曲/调音量 · A播放 · X全屏 · 左方向键切歌");
    } else {
        // 竖屏下半部：顶部 3 枚触控胶囊 + 居中乔布斯 Click Wheel + 底部 3 枚触控胶囊
        rc_pod_card = (Rect){ 0, 0, 0, 0 };
        int gx = 28, bw = 212, gap = 14, bh = 42;
        int r1y = 678, r2y = 1180;

        rc_btn_vol    = draw_m3_pill_fixed(gx,                r1y, bw, bh, 14.0f, vol_lbl, show_vol_panel, 0);
        rc_btn_ori    = draw_m3_pill_fixed(gx + bw + gap,     r1y, bw, bh, 14.0f,
                                           be_is_desktop() ? "选目录" : "横屏 [Y]", 0, 0);
        rc_btn_search = draw_m3_pill_fixed(gx + 2*(bw + gap), r1y, bw, bh, 14.0f, sr_lbl, view == VIEW_SEARCH, 0);

        draw_click_wheel();

        rc_btn_theme  = draw_m3_pill_fixed(gx,                r2y, bw, bh, 14.0f, th_lbl, 0, 0);
        rc_btn_diag   = draw_m3_pill_fixed(gx + bw + gap,     r2y, bw, bh, 14.0f, "自检", show_diag, 0);
        rc_btn_exit   = draw_m3_pill_fixed(gx + 2*(bw + gap), r2y, bw, bh, 14.0f, "退出", 0, 1);

        draw_text_centered(screen, font, 12.5f, 40, SCR_W - 40, 1238, C_TXT3,
                           be_is_desktop()
                               ? "滚轮/拖放选曲 · Space播放 · F全屏 · ←/→切歌 · Ctrl+O选目录"
                               : "纯触屏或手柄均可操控 · A播放 · X全屏 · 左方向键切歌");
    }
}

// ---------- 列表页绘制（现代 Apple 悬浮视窗 × Material You 色调卡片） ----------
static void draw_list(void) {
    char hdr_sub[64];
    snprintf(hdr_sub, sizeof(hdr_sub), "%d 首", ntracks);

    const char* hdr_t = "Sirène";
    if (view == VIEW_FAVORITE) hdr_t = "Sirène · 喜欢";
    else if (view == VIEW_SEARCH && search_q[0]) hdr_t = "Sirène · 搜索";

    draw_lcd_shell(hdr_t, portrait() ? hdr_sub : "", 0);

    int list_bot = list_bottom_y();
    int cardw = L_X1 - L_X0;
    rc_empty_rescan = (Rect){ 0, 0, 0, 0 };

    // 视窗顶部 Material You 色调胶囊栏
    char all_l[32], fav_l[32];
    snprintf(all_l, sizeof(all_l), "全部 %d", ntracks);
    snprintf(fav_l, sizeof(fav_l), "喜欢 %d", fav_count);

    if (!portrait()) {
        int tx = LCD_X + 148, ty = LCD_Y + 12;
        draw_m3_tab(&rc_tab_all, tx, ty, 30, 13.5f, all_l, view == VIEW_HOME, 0);
        tx += rc_tab_all.w + 8;
        draw_m3_tab(&rc_tab_fav, tx, ty, 30, 13.5f, fav_l, view == VIEW_FAVORITE, 0);
        tx += rc_tab_fav.w + 8;
        draw_m3_tab(&rc_btn_rescan, tx, ty, 30, 13.5f, "扫描", 0, 0);
        tx += rc_btn_rescan.w + 8;
        draw_m3_tab(&rc_btn_mode, tx, ty, 30, 13.5f, mode_names[play_mode], play_mode != MODE_LOOP_ALL, 0);
        draw_m3_tab(&rc_btn_full, LCD_X + LCD_W - 110, ty, 30, 13.5f, "全屏 [X]", 0, 1);
    } else {
        draw_m3_tab(&rc_btn_full, LCD_X + LCD_W - 170, LCD_Y + 13, 28, 13.0f, "全屏 [X]", 0, 1);
        int sy = LCD_Y + 52, tx = LCD_X + 16;
        draw_m3_tab(&rc_tab_all, tx, sy, 32, 13.5f, all_l, view == VIEW_HOME, 0);
        tx += rc_tab_all.w + 8;
        draw_m3_tab(&rc_tab_fav, tx, sy, 32, 13.5f, fav_l, view == VIEW_FAVORITE, 0);
        tx += rc_tab_fav.w + 8;
        draw_m3_tab(&rc_btn_rescan, tx, sy, 32, 13.5f, "重新扫描", 0, 0);
        tx += rc_btn_rescan.w + 8;
        draw_m3_tab(&rc_btn_mode, tx, sy, 32, 13.5f, mode_names[play_mode], play_mode != MODE_LOOP_ALL, 0);
    }

    ensure_visible_thumbs();

    if (nvisible == 0) {
        int bw = portrait() ? 560 : 620;
        int bh = 186;
        int bx = L_X0 + (cardw - bw) / 2;
        int by = L_TOP + (list_area_h() - bh) / 2;
        fill_round_rect_aa(screen, bx, by, bw, bh, 22, C_SURF_SUB);

        const char* tmsg = be_is_desktop() ? "未在音乐目录找到音频" : "未在 SD 卡找到音频";
        const char* smsg = be_is_desktop()
            ? "支持 MP3 / FLAC / OGG / WAV · 点上方「选目录」或直接拖入"
            : "支持 MP3 / FLAC / OGG / WAV · 可放入 sdmc:/music";
        if (view == VIEW_FAVORITE && ntracks > 0) {
            tmsg = "暂无收藏歌曲";
            smsg = "点击歌曲右侧爱心或按 ZL 键即可收藏";
        } else if (view == VIEW_SEARCH && ntracks > 0) {
            tmsg = "未找到匹配歌曲";
            smsg = "点击右侧或下方“清除搜索”恢复全部歌曲";
        }
        draw_text_centered(screen, font, 20, bx + 16, bx + bw - 16, by + 34, C_TXT, tmsg);
        draw_text_centered(screen, font, 14, bx + 16, bx + bw - 16, by + 70, C_TXT2, smsg);

        int btn_w = 188, btn_h = 42;
        int btn_x = bx + (bw - btn_w) / 2, btn_y = by + 116;
        fill_round_rect_aa(screen, btn_x, btn_y, btn_w, btn_h, 21, C_ACCENT);
        draw_text_centered(screen, font, 15, btn_x, btn_x + btn_w, btn_y + 12, C_ON_ACCENT,
                           be_is_desktop() ? "选择音乐目录" : "重新扫描 SD 卡");
        rc_empty_rescan = (Rect){ btn_x, btn_y, btn_w, btn_h };
    } else {
        int i0 = (int)floorf(scroll_px / (float)L_ROW) - 1;
        if (i0 < 0) i0 = 0;
        int i1 = (int)ceilf((scroll_px + (float)list_area_h()) / (float)L_ROW) + 1;
        if (i1 > nvisible) i1 = nvisible;

        canvas_set_clip(screen, LCD_X + 4, L_TOP, LCD_X + LCD_W - 4, list_bot);
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

            uint32_t row_bg = C_CARD;
            if (is_playing) row_bg = C_TONAL_BG;
            else if (is_sel || is_press) row_bg = C_CARD_SEL;

            fill_round_rect_aa(screen, L_X0, cy0, cardw - 10, ch, 16, row_bg);
            if (is_sel && !is_playing) {
                stroke_round_rect_aa(screen, L_X0, cy0, cardw - 10, ch, 16, 1, C_ACCENT, 145);
            }
            if (is_playing) {
                fill_round_rect_aa(screen, L_X0 + 7, cy0 + ch / 2 - 11, 4, 22, 2, C_ACCENT);
            } else if (is_sel) {
                fill_round_rect_aa(screen, L_X0 + 7, cy0 + ch / 2 - 8, 4, 16, 2, C_TXT3);
            }

            int tx = L_X0 + 16;
            int ty = cy0 + (ch - THUMB_SZ) / 2;
            if (tracks[idx].thumb)
                blit_rgb_round(screen, tx, ty, tracks[idx].thumb, THUMB_SZ, THUMB_SZ, 12);

            char nm[256];
            basename_no_ext(tracks[idx].path, nm, sizeof(nm));
            const char* ttl = tracks[idx].meta.title[0] ? tracks[idx].meta.title : nm;
            const char* art = tracks[idx].meta.artist[0] ? tracks[idx].meta.artist : "未知艺术家";

            uint32_t tc = is_playing ? C_TONAL_TXT : C_TXT;
            uint32_t sc = is_playing ? C_TONAL_TXT : C_TXT3;

            int txtx = tx + THUMB_SZ + 14;
            int right_res = 64 + (is_playing ? 64 : 0);
            int maxw = (L_X0 + cardw - 10) - txtx - right_res;
            if (maxw < 80) maxw = 80;

            int block = L_TITLE + 4 + L_ART;
            int ty2 = cy0 + (ch - block) / 2 - 1;
            draw_text_max(screen, font, (float)L_TITLE, txtx, ty2, tc, ttl, maxw);

            char fmt[12];
            track_format_tag(tracks[idx].path, fmt, sizeof(fmt));
            char subline[300];
            snprintf(subline, sizeof(subline), "%s · %s", fmt, art);
            draw_text_max(screen, font, (float)L_ART, txtx, ty2 + L_TITLE + 4, sc, subline, maxw);

            if (is_playing) {
                const char* st_tag = audio_paused() ? "已暂停" : "播放中";
                draw_text_right(screen, font, 12.5f, L_X1 - 58, cy0 + (ch - 13) / 2, C_ACCENT, st_tag);
            }

            int hx = L_X1 - 44;
            int hy = cy0 + (ch - 18) / 2;
            icon_heart(screen, hx, hy, 18, tracks[idx].fav ? C_HEART : C_TRACK_BG);
        }

        // 视窗内部极简胶囊滚动条
        float ms = max_scroll_px();
        if (ms > 0.0f) {
            int track_h = list_area_h() - 12;
            int thumb_h = (int)((float)list_area_h() / (float)(nvisible * L_ROW) * (float)track_h);
            if (thumb_h < 28) thumb_h = 28;
            if (thumb_h > track_h) thumb_h = track_h;
            int thumb_y = L_TOP + 6 + (int)((scroll_px / ms) * (float)(track_h - thumb_h));
            fill_round_rect_aa(screen, LCD_X + LCD_W - 10, thumb_y, 4, thumb_h, 2, C_TXT3);
        }
        canvas_reset_clip(screen);
    }

    // 底部 Apple × M3 悬浮迷你灵动岛播控条（桌面竖屏集成实时同步歌词栏 + Material You 波浪条）
    int desk_port = (portrait() && be_is_desktop());
    int dock_h = desk_port ? 106 : 54;
    fill_round_rect_aa(screen, LCD_X + 4, list_bot - 2, LCD_W - 8, (LCD_Y + LCD_H) - (list_bot - 2) - 4, 24, C_SURFACE);
    fill_round_rect_aa(screen, LCD_X + 12, list_bot + 4, LCD_W - 24, dock_h, 18, C_SURF_SUB);
    rc_mini_bar = (Rect){ LCD_X + 12, list_bot + 4, LCD_W - 24, dock_h };

    if (playing_idx >= 0) {
        const uint32_t* mimg = cover_mini ? cover_mini : poster_mini;
        if (mimg) blit_rgb_round(screen, LCD_X + 20, list_bot + 10, mimg, MINI_ART_SZ, MINI_ART_SZ, 10);

        double pos = audio_position(), dur = audio_duration();
        float p = (dur > 0.0) ? clampf((float)(pos / dur), 0.0f, 1.0f) : 0.0f;
        float mamp = (!audio_paused()) ? (wave_amp_cur * 0.65f) : 0.0f;
        if (!audio_paused() && mamp < 0.4f) mamp = 2.8f;

        if (desk_port) {
            draw_text_max(screen, font, 14.5f, LCD_X + 72, list_bot + 11, C_TXT, p_title, LCD_W - 210);
            char sub[320];
            snprintf(sub, sizeof(sub), "%s · %s", audio_paused() ? "已暂停" : p_fmt,
                     p_artist[0] ? p_artist : "未知艺术家");
            draw_text_max(screen, font, 12.0f, LCD_X + 72, list_bot + 30, C_TXT2, sub, LCD_W - 210);

            fill_round_rect_aa(screen, LCD_X + LCD_W - 114, list_bot + 16, 90, 30, 15, C_TONAL_BG);
            draw_text_centered(screen, font, 12.5f, LCD_X + LCD_W - 114, LCD_X + LCD_W - 24, list_bot + 24,
                               C_TONAL_TXT, "歌词全屏");

            if (dur > 0.0) {
                draw_m3_wavy_progress(screen, LCD_X + 22, LCD_X + LCD_W - 22, list_bot + 59, p,
                                      wave_phase, mamp, 22.0f, 5, 4, 0, C_ACCENT, C_TRACK_BG);
            } else {
                fill_round_rect_aa(screen, LCD_X + 22, list_bot + 57, LCD_W - 44, 4, 2, C_TRACK_BG);
            }

            fill_round_rect_aa(screen, LCD_X + 20, list_bot + 69, LCD_W - 40, 34, 11,
                               (lyr && lyr->n > 0) ? C_TONAL_BG : C_CARD);
            if (lyr && lyr->n > 0) {
                int act = lyr->unsynced
                    ? (dur > 0.0 && lyr->n > 1 ? (int)((pos / dur) * (double)(lyr->n - 1)) : 0)
                    : lyrics_index(lyr, pos);
                if (act < 0) act = 0;
                if (act >= lyr->n) act = lyr->n - 1;
                fill_round_rect_aa(screen, LCD_X + 27, list_bot + 76, 4, 20, 2, C_ACCENT);
                draw_text_max(screen, font, 14.0f, LCD_X + 38, list_bot + 78, C_TONAL_TXT,
                              lyr->lines[act].text, LCD_W - 72);
            } else {
                draw_text_max(screen, font, 12.0f, LCD_X + 30, list_bot + 79, C_TXT3,
                              lyrics_last_diag(), LCD_W - 60);
            }
        } else {
            if (dur > 0.0) {
                float sm_amp = (!audio_paused()) ? 1.8f : 0.0f;
                draw_m3_wavy_progress(screen, LCD_X + 70, LCD_X + LCD_W - 110, list_bot + 53, p,
                                      wave_phase, sm_amp, 20.0f, 4, 3, 0, C_ACCENT, C_TRACK_BG);
            }

            draw_text_max(screen, font, 15, LCD_X + 72, list_bot + 11, C_TXT, p_title, LCD_W - 210);
            if (lyr && lyr->n > 0) {
                int act = lyr->unsynced
                    ? (dur > 0.0 && lyr->n > 1 ? (int)((pos / dur) * (double)(lyr->n - 1)) : 0)
                    : lyrics_index(lyr, pos);
                if (act < 0) act = 0;
                if (act >= lyr->n) act = lyr->n - 1;
                draw_text_max(screen, font, 12.5f, LCD_X + 72, list_bot + 31, C_TONAL_TXT, lyr->lines[act].text, LCD_W - 210);
            } else {
                char sub[320];
                snprintf(sub, sizeof(sub), "%s · %s", audio_paused() ? "已暂停" : "正在播放",
                         p_artist[0] ? p_artist : p_fmt);
                draw_text_max(screen, font, 12.5f, LCD_X + 72, list_bot + 31, C_TXT2, sub, LCD_W - 210);
            }

            fill_round_rect_aa(screen, LCD_X + LCD_W - 114, list_bot + 16, 90, 30, 15, C_TONAL_BG);
            draw_text_centered(screen, font, 12.5f, LCD_X + LCD_W - 114, LCD_X + LCD_W - 24, list_bot + 24,
                               C_TONAL_TXT, "歌词全屏");
        }
    } else {
        if (desk_port) {
            const char* hint = ntracks == 0
                ? "未发现音频 · 点上方「选目录」或拖入文件夹/曲目"
                : "点歌曲就地播放 · 下栏实时显示歌词（也可直接拖入 .lrc）";
            draw_text_centered(screen, font, 14.0f, LCD_X + 20, LCD_X + LCD_W - 20, list_bot + 30, C_TXT2, hint);
            fill_round_rect_aa(screen, LCD_X + 20, list_bot + 69, LCD_W - 40, 34, 11, C_CARD);
            draw_text_centered(screen, font, 12.5f, LCD_X + 28, LCD_X + LCD_W - 28, list_bot + 79,
                               C_TXT3, "提示：按 F / X 或点此卡片可随时进入全屏歌词视图");
        } else {
            const char* hint = ntracks == 0
                ? "未发现音频 · 点击扫描按钮检索 SD 卡"
                : "点歌曲或按 A 就地播放 · 点此或按 X 进入全屏歌词";
            draw_text_centered(screen, font, 13.5f, LCD_X + 20, LCD_X + LCD_W - 20, list_bot + 23, C_TXT2, hint);
        }
    }

    draw_hardware_controls();
}

// ---------- 全屏播放页绘制（Apple 环境流体光晕 + 歌词滚动高亮 + M3 进度条） ----------
static void draw_lyric_or_spectrum(int x0, int x1, int ly_top, int ly_bot, int spec_base, int spec_hint_y,
                                   double pos, double dur) {
    int rw = x1 - x0;
    if (lyr && lyr->n > 0) {
        int act = lyr->unsynced
            ? (dur > 0.0 && lyr->n > 1 ? (int)((pos / dur) * (double)(lyr->n - 1)) : 0)
            : lyrics_index(lyr, pos);
        if (act < 0) act = 0;
        if (act >= lyr->n) act = lyr->n - 1;

        int idxs2[8];
        int n = 0;
        for (int d = -2; d <= 3; d++) {
            int i = act + d;
            if (i >= 0 && i < lyr->n) idxs2[n++] = i;
        }
        int y = ly_top;
        for (int k = 0; k < n; k++) {
            int i = idxs2[k];
            int dist = i - act;
            if (y + 24 > ly_bot) break;
            if (dist == 0) {
                fill_round_rect_aa_a(screen, x0, y - 4, rw, 38, 14, C_TONAL_BG, 225);
                fill_round_rect_aa(screen, x0 + 8, y + 4, 4, 22, 2, C_ACCENT);
                draw_text_max(screen, font, 19.5f, x0 + 20, y + 4, C_TONAL_TXT, lyr->lines[i].text, rw - 28);
                y += 44;
            } else {
                float px = (dist == -1 || dist == 1) ? 16.0f : 14.5f;
                uint32_t cc = (dist == -1 || dist == 1) ? C_TXT : C_TXT3;
                draw_text_max(screen, font, px, x0 + 14, y, cc, lyr->lines[i].text, rw - 22);
                y += (int)px + 15;
            }
        }
        if (lyr->unsynced) {
            draw_text(screen, font, 12.5f, x0 + 14, spec_hint_y, C_TXT3, "静态歌词（按播放进度自动定位）");
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
        int max_h = portrait() ? 140 : 168;
        for (int i = 0; i < n; i++) {
            float v = clampf(bars[i], 0.05f, 1.0f);
            int hgt = (int)(v * max_h);
            int bx = bx0 + i * (bw + gap);
            fill_round_rect_aa_a(screen, bx, spec_base - hgt, bw, hgt, bw / 2, C_ACCENT, 215);
        }
        draw_text_centered(screen, font, 13, x0, x1, spec_hint_y, C_TXT3, lyrics_last_diag());
    }
}

static void draw_lcd_progress(int py, double pos, double dur) {
    float frac = (dur > 0.0) ? clampf((float)(pos / dur), 0.0f, 1.0f) : 0.0f;
    float amp = (playing_idx >= 0 && !audio_paused()) ? wave_amp_cur : (wave_amp_cur > 0.05f ? wave_amp_cur : 0.0f);
    if (playing_idx >= 0 && !audio_paused() && amp < 0.5f) amp = 4.4f;
    draw_m3_wavy_progress(screen, ui_px0, ui_px1, py + 4, frac,
                          wave_phase, amp, 28.0f, 7, 6, 1, C_ACCENT, C_TRACK_BG);

    char t1[32], t2[32];
    fmt_time(pos, t1, sizeof(t1));
    fmt_time(dur, t2, sizeof(t2));
    draw_text(screen, font, 13, ui_px0, py + 20, C_TXT2, t1);
    draw_text_right(screen, font, 13, ui_px1, py + 20, C_TXT2, t2);
}

static void draw_player(void) {
    char top_info[64];
    snprintf(top_info, sizeof(top_info), "%s · %d/%d · %s",
             p_fmt[0] ? p_fmt : "AUDIO",
             playing_idx >= 0 ? playing_idx + 1 : 0, ntracks,
             audio_paused() ? "已暂停" : "播放中");

    draw_lcd_shell("正在播放", top_info, 1);
    rc_tab_all = rc_tab_fav = rc_btn_rescan = rc_empty_rescan = rc_mini_bar = rc_btn_full = (Rect){ 0, 0, 0, 0 };

    // 顶栏返回列表按钮（触控返回，也可按 X / B / MENU）
    int bx = LCD_X + 168, by = LCD_Y + 12;
    fill_round_rect_aa(screen, bx, by, 98, 30, 15, C_TONAL_BG);
    icon_back_arrow(screen, bx + 12, by + 8, 14, C_TONAL_TXT);
    draw_text(screen, font, 13.5f, bx + 32, by + 7, C_TONAL_TXT, "列表 [X]");
    rc_p_back = (Rect){ bx, by, 98, 30 };

    int fav_on = (playing_idx >= 0 && tracks[playing_idx].fav);
    double pos = touch_scrub ? scrub_preview_sec : audio_position();
    double dur = audio_duration();
    if (dur > 0 && pos > dur) pos = dur;

    if (!portrait()) {
        int csz = COVER_LAND_SZ;
        int cx = LCD_X + 34, cy = LCD_Y + 80;
        ui_cover_x = cx; ui_cover_y = cy; ui_cover_sz = csz;

        fill_round_rect_shadow(screen, cx, cy, csz, csz, 24, g_is_light ? 34 : 58);
        if (cover_land) blit_rgb_round(screen, cx, cy, cover_land, COVER_LAND_SZ, COVER_LAND_SZ, 24);

        // 封面下方 M3 喜欢与模式切换胶囊
        int pill_y = cy + csz + 24;
        fill_round_rect_aa(screen, cx, pill_y, 110, 36, 18, fav_on ? C_TONAL_BG : C_SURF_SUB);
        icon_heart(screen, cx + 16, pill_y + 9, 18, fav_on ? C_HEART : C_TXT2);
        draw_text(screen, font, 13.5f, cx + 42, pill_y + 10, fav_on ? C_HEART : C_TXT2, fav_on ? "已喜欢" : "喜欢");
        rc_btn_fav_p = (Rect){ cx, pill_y, 110, 36 };

        fill_round_rect_aa(screen, cx + 120, pill_y, 116, 36, 18,
                           play_mode == MODE_LOOP_ALL ? C_SURF_SUB : C_TONAL_BG);
        draw_text_centered(screen, font, 13.5f, cx + 120, cx + 236, pill_y + 10,
                           play_mode == MODE_LOOP_ALL ? C_TXT2 : C_TONAL_TXT, mode_names[play_mode]);
        rc_btn_mode = (Rect){ cx + 120, pill_y, 116, 36 };

        int col_x = cx + csz + 32;
        int col_x1 = LCD_X + LCD_W - 26;
        int col_w = col_x1 - col_x;
        float tpx = text_width(font, 28, p_title) > col_w ? 23.0f : 28.0f;
        draw_text_max(screen, font, tpx, col_x, cy + 4, C_TXT, p_title, col_w);
        draw_text_max(screen, font, 16, col_x, cy + 42, C_TXT2, p_artist[0] ? p_artist : "未知艺术家", col_w);
        fill_round_rect_aa(screen, col_x, cy + 72, 44, 4, 2, C_ACCENT);

        draw_lyric_or_spectrum(col_x, col_x1, cy + 94, cy + 362, cy + 326, cy + 344, pos, dur);

        int py = LCD_Y + LCD_H - 92;
        ui_px0 = LCD_X + 34; ui_px1 = LCD_X + LCD_W - 34; ui_py = py;
        draw_lcd_progress(py, pos, dur);

        draw_text_centered(screen, font, 12.5f, LCD_X + 34, LCD_X + LCD_W - 34, py + 46, C_TXT3,
                           "拖动进度条快进 · 滑右侧转盘调音量 · 左方向键切歌 · X / B 返回列表");
    } else {
        int csz = COVER_PORT_SZ;
        int cx = LCD_X + 26, cy = LCD_Y + 68;
        ui_cover_x = cx; ui_cover_y = cy; ui_cover_sz = csz;

        fill_round_rect_shadow(screen, cx, cy, csz, csz, 22, g_is_light ? 34 : 58);
        if (cover_port) blit_rgb_round(screen, cx, cy, cover_port, COVER_PORT_SZ, COVER_PORT_SZ, 22);

        int rx = cx + csz + 22, rx1 = LCD_X + LCD_W - 22, rw = rx1 - rx;
        float tpx = text_width(font, 24, p_title) > rw ? 20.0f : 24.0f;
        draw_text_max(screen, font, tpx, rx, cy + 8, C_TXT, p_title, rw);
        draw_text_max(screen, font, 15.5f, rx, cy + 44, C_TXT2, p_artist[0] ? p_artist : "未知艺术家", rw);

        if (lyr && lyr->source[0]) {
            char src_tag[96];
            snprintf(src_tag, sizeof(src_tag), "歌词：%s (%d行)", lyr->source, lyr->n);
            draw_text_max(screen, font, 12.5f, rx, cy + 78, C_TONAL_TXT, src_tag, rw);
        } else {
            draw_text_max(screen, font, 12.5f, rx, cy + 78, C_TXT3, "频谱模式 · 点击封面暂停/播放", rw);
        }

        fill_round_rect_aa(screen, rx, cy + 118, 104, 34, 17, fav_on ? C_TONAL_BG : C_SURF_SUB);
        icon_heart(screen, rx + 14, cy + 127, 16, fav_on ? C_HEART : C_TXT2);
        draw_text(screen, font, 13.5f, rx + 38, cy + 127, fav_on ? C_HEART : C_TXT2, fav_on ? "已喜欢" : "喜欢");
        rc_btn_fav_p = (Rect){ rx, cy + 118, 104, 34 };

        fill_round_rect_aa(screen, rx + 114, cy + 118, 114, 34, 17,
                           play_mode == MODE_LOOP_ALL ? C_SURF_SUB : C_TONAL_BG);
        draw_text_centered(screen, font, 13.5f, rx + 114, rx + 228, cy + 127,
                           play_mode == MODE_LOOP_ALL ? C_TXT2 : C_TONAL_TXT, mode_names[play_mode]);
        rc_btn_mode = (Rect){ rx + 114, cy + 118, 114, 34 };

        draw_lyric_or_spectrum(LCD_X + 24, LCD_X + LCD_W - 24,
                               cy + csz + 24, LCD_Y + LCD_H - 90,
                               LCD_Y + LCD_H - 118, LCD_Y + LCD_H - 98, pos, dur);

        int py = LCD_Y + LCD_H - 62;
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

// ---------- PC 桌面端 Material You 现代横屏双栏 UI + 鼠标交互 ----------
static Rect draw_m3_sq_btn(int x, int y, int w, int h, int rad, float px, const char* txt,
                           int active, int hovered) {
    uint32_t bgc = active ? C_TONAL_BG : (hovered ? C_CARD_SEL : C_CHIP);
    uint32_t fgc = active ? C_TONAL_TXT : (hovered ? C_TXT : C_TXT2);
    fill_round_rect_aa(screen, x, y, w, h, rad, bgc);
    if (hovered && !active) {
        stroke_round_rect_aa(screen, x, y, w, h, rad, 1, C_ACCENT, 120);
    }
    draw_text_centered(screen, font, px, x + 6, x + w - 6, y + (h - (int)px) / 2 - 1, fgc, txt);
    return (Rect){ x, y, w, h };
}

static void toggle_desktop_fullscreen(void) {
    capture_fade();
    desktop_full = !desktop_full;
    if (cover_ready && cover_src) rebuild_lcd_ambient();
}

static void draw_desktop_lyrics_block(int x0, int x1, int y0, int y1, double pos, double dur,
                                      int mx, int my, int big_font) {
    ndesk_lyr_hits = 0;
    desk_lyr_x0 = x0; desk_lyr_x1 = x1; desk_lyr_y0 = y0; desk_lyr_y1 = y1;
    int rw = x1 - x0;
    if (rw < 60 || y1 <= y0 + 40) return;

    canvas_set_clip(screen, x0, y0, x1, y1);
    if (lyr && lyr->n > 0) {
        int act = lyr->unsynced
            ? (dur > 0.0 && lyr->n > 1 ? (int)((pos / dur) * (double)(lyr->n - 1)) : 0)
            : lyrics_index(lyr, pos);
        if (act < 0) act = 0;
        if (act >= lyr->n) act = lyr->n - 1;

        int idxs[10];
        int n = 0;
        int lo_d = big_font ? -2 : -2;
        int hi_d = big_font ? 5 : 4;
        for (int d = lo_d; d <= hi_d; d++) {
            int i = act + d;
            if (i >= 0 && i < lyr->n && n < 10) idxs[n++] = i;
        }
        float act_px = big_font ? 21.0f : 17.0f;
        float near_px = big_font ? 17.0f : 14.5f;
        float far_px = big_font ? 15.0f : 13.0f;
        int act_h = big_font ? 44 : 38;

        int y = y0 + 6;
        for (int k = 0; k < n; k++) {
            int i = idxs[k];
            int dist = i - act;
            if (y + 24 > y1) break;
            if (dist == 0) {
                Rect hr = { x0, y, rw, act_h };
                if (ndesk_lyr_hits < MAX_DESK_LYR_HITS) {
                    desk_lyr_hits[ndesk_lyr_hits++] = (DeskLyrHit){ hr, i };
                }
                fill_round_rect_aa_a(screen, hr.x, hr.y, hr.w, hr.h, 14, C_TONAL_BG, 232);
                fill_round_rect_aa(screen, hr.x + 8, hr.y + 8, 4, act_h - 16, 2, C_ACCENT);
                draw_text_max(screen, font, act_px, hr.x + 20, hr.y + (act_h - (int)act_px) / 2 - 1,
                              C_TONAL_TXT, lyr->lines[i].text, rw - 30);
                y += act_h + 8;
            } else {
                float px = (dist == -1 || dist == 1) ? near_px : far_px;
                int row_h = (int)px + 14;
                Rect hr = { x0, y - 3, rw, row_h };
                int hov = !show_diag && !show_search_box && in_rc(mx, my, hr);
                if (hov) {
                    fill_round_rect_aa_a(screen, hr.x, hr.y, hr.w, hr.h, 10, C_CARD_SEL, 195);
                }
                if (ndesk_lyr_hits < MAX_DESK_LYR_HITS) {
                    desk_lyr_hits[ndesk_lyr_hits++] = (DeskLyrHit){ hr, i };
                }
                uint32_t cc = hov ? C_TXT : ((dist == -1 || dist == 1) ? C_TXT : C_TXT3);
                draw_text_max(screen, font, px, x0 + 16, y, cc, lyr->lines[i].text, rw - 24);
                y += row_h + 2;
            }
        }
    } else {
        float sp[28];
        audio_get_spectrum(sp);
        for (int i = 0; i < 28; i++) bars[i] = sp[i];

        int n = big_font ? 24 : 18;
        int bw = big_font ? 16 : 13;
        int span = rw - 24;
        int gap = (span - n * bw) / (n - 1);
        if (gap < 4) gap = 4;
        int total_w = n * bw + (n - 1) * gap;
        int bx0 = x0 + (rw - total_w) / 2;
        int spec_base = y1 - 46;
        int max_h = y1 - y0 - 84;
        if (max_h > 190) max_h = 190;
        if (max_h < 60) max_h = 60;
        for (int i = 0; i < n; i++) {
            float v = clampf(bars[i], 0.06f, 1.0f);
            int hgt = (int)(v * max_h);
            int bx = bx0 + i * (bw + gap);
            fill_round_rect_aa_a(screen, bx, spec_base - hgt, bw, hgt, bw / 2, C_ACCENT, 215);
        }
        const char* lmsg = lyrics_online_busy(g_lyr_req_id) ? "正在在线匹配歌词…" : lyrics_last_diag();
        draw_text_centered(screen, font, 13.0f, x0 + 8, x1 - 8, y1 - 30, C_TXT3, lmsg);
    }
    canvas_reset_clip(screen);
}

static void draw_desktop_ui(void) {
    int mx = -1, my = -1;
    be_mouse_pos(&mx, &my);
    if (show_search_box || show_diag) { mx = -1; my = -1; }

    canvas_clear(screen, C_BODY);
    rc_vol_box = rc_exit_box = rc_pod_card = rc_empty_rescan = (Rect){ 0, 0, 0, 0 };
    rc_btn_vol = rc_btn_exit = (Rect){ 0, 0, 0, 0 };

    // 1. 顶部 PC 应用栏（App 图标 + Sirène + 视图/目录/搜索/主题）
    int icon_x = 24, icon_y = 17;
    if (logo_img) blit_rgb_round(screen, icon_x, icon_y, logo_img, LOGO_SZ, LOGO_SZ, 9);
    draw_text(screen, font, 22, 62, 20, C_TXT, "Sirène");

    char cnt_lbl[32], all_l[32], fav_l[32], th_l[36];
    snprintf(cnt_lbl, sizeof(cnt_lbl), "%d 首", ntracks);
    snprintf(all_l, sizeof(all_l), "全部 %d", ntracks);
    snprintf(fav_l, sizeof(fav_l), "喜欢 %d", fav_count);
    snprintf(th_l, sizeof(th_l), "主题:%s", theme_names[cur_theme]);

    int cnt_w = text_width(font, 12.5f, cnt_lbl) + 18;
    fill_round_rect_aa(screen, 134, 21, cnt_w, 24, 10, C_SURF_SUB);
    draw_text_centered(screen, font, 12.5f, 134, 134 + cnt_w, 26, C_TXT2, cnt_lbl);

    int tx = 144 + cnt_w + 14;
    int ty = 16, bh = 34;
    int aw = text_width(font, 13.5f, all_l) + 26;
    rc_tab_all = draw_m3_sq_btn(tx, ty, aw, bh, 12, 13.5f, all_l, view == VIEW_HOME, in_rc(mx, my, (Rect){tx,ty,aw,bh}));
    tx += aw + 8;

    int fw = text_width(font, 13.5f, fav_l) + 26;
    rc_tab_fav = draw_m3_sq_btn(tx, ty, fw, bh, 12, 13.5f, fav_l, view == VIEW_FAVORITE, in_rc(mx, my, (Rect){tx,ty,fw,bh}));
    tx += fw + 8;

    rc_btn_ori = draw_m3_sq_btn(tx, ty, 74, bh, 12, 13.5f, "选目录", 0, in_rc(mx, my, (Rect){tx,ty,74,bh}));
    tx += 82;

    rc_btn_rescan = draw_m3_sq_btn(tx, ty, 64, bh, 12, 13.5f, "扫库", 0, in_rc(mx, my, (Rect){tx,ty,64,bh}));
    tx += 72;

    int mw = text_width(font, 13.5f, mode_names[play_mode]) + 24;
    rc_btn_mode = draw_m3_sq_btn(tx, ty, mw, bh, 12, 13.5f, mode_names[play_mode],
                                 play_mode != MODE_LOOP_ALL, in_rc(mx, my, (Rect){tx,ty,mw,bh}));

    // 右侧搜索框 + 主题 + 诊断
    int rx = 1260;
    int dw = 62;
    rx -= dw;
    rc_btn_diag = draw_m3_sq_btn(rx, ty, dw, bh, 12, 13.5f, "诊断", show_diag, in_rc(mx, my, (Rect){rx,ty,dw,bh}));
    rx -= 8;

    int thw = text_width(font, 13.5f, th_l) + 24;
    rx -= thw;
    rc_btn_theme = draw_m3_sq_btn(rx, ty, thw, bh, 12, 13.5f, th_l, 0, in_rc(mx, my, (Rect){rx,ty,thw,bh}));
    rx -= 10;

    int sr_w = rx - (tx + mw + 16);
    if (sr_w > 260) sr_w = 260;
    if (sr_w < 160) sr_w = 160;
    int sr_x = rx - sr_w;
    Rect sr_rc = { sr_x, ty, sr_w, bh };
    int sr_hov = in_rc(mx, my, sr_rc);
    fill_round_rect_aa(screen, sr_x, ty, sr_w, bh, 12, (view == VIEW_SEARCH && search_q[0]) ? C_TONAL_BG : C_SURFACE);
    stroke_round_rect_aa(screen, sr_x, ty, sr_w, bh, 12, 1, sr_hov ? C_ACCENT : C_WHEEL_RING, sr_hov ? 200 : 140);
    if (search_q[0]) {
        char sq[140];
        snprintf(sq, sizeof(sq), "搜索: %s", search_q);
        draw_text_max(screen, font, 13.5f, sr_x + 12, ty + 9, C_TONAL_TXT, sq, sr_w - 54);
        rc_desk_sr_clear = (Rect){ sr_x + sr_w - 38, ty + 4, 34, bh - 8 };
        fill_round_rect_aa(screen, rc_desk_sr_clear.x, rc_desk_sr_clear.y, rc_desk_sr_clear.w, rc_desk_sr_clear.h,
                           8, C_SURFACE);
        draw_text_centered(screen, font, 12.0f, rc_desk_sr_clear.x, rc_desk_sr_clear.x + rc_desk_sr_clear.w,
                           ty + 10, C_TXT2, "清除");
    } else {
        rc_desk_sr_clear = (Rect){ 0, 0, 0, 0 };
        draw_text_max(screen, font, 13.5f, sr_x + 14, ty + 9, C_TXT3, "搜索歌曲 / 歌手 (Ctrl+F)", sr_w - 24);
    }
    rc_btn_search = sr_rc;

    double pos = touch_scrub ? scrub_preview_sec : audio_position();
    double dur = audio_duration();
    if (dur > 0 && pos > dur) pos = dur;
    int fav_on = (playing_idx >= 0 && tracks[playing_idx].fav);

    // 2. 主内容区 (y = 66..616)
    if (!desktop_full) {
        rc_desk_full_mode = (Rect){ 0, 0, 0, 0 };
        // ---- 左栏：Material You 曲库大圆角卡片 (742 x 550) ----
        fill_round_rect_shadow(screen, LCD_X, LCD_Y, LCD_W, LCD_H, 24, g_is_light ? 22 : 46);
        fill_round_rect_aa(screen, LCD_X, LCD_Y, LCD_W, LCD_H, 24, C_SURFACE);
        stroke_round_rect_aa(screen, LCD_X, LCD_Y, LCD_W, LCD_H, 24, 1, C_WHEEL_RING, 110);

        const char* list_title = "全部曲库";
        if (view == VIEW_FAVORITE) list_title = "我喜欢的歌曲";
        else if (view == VIEW_SEARCH && search_q[0]) list_title = "搜索匹配结果";
        draw_text(screen, font, 16.5f, LCD_X + 20, LCD_Y + 14, C_TXT, list_title);
        draw_text_right(screen, font, 12.0f, LCD_X + LCD_W - 22, LCD_Y + 17, C_TXT3,
                        "单击播放/暂停 · 滚轮/拖滚动条翻页 · 拖入文件夹或 .lrc");
        fill_rect_a(screen, LCD_X + 16, LCD_Y + 42, LCD_W - 32, 1, C_WHEEL_RING, 95);

        int list_bot = list_bottom_y();
        int cardw = L_X1 - L_X0;
        rc_desk_sb = (Rect){ 0, 0, 0, 0 };
        rc_p_back = (Rect){ 0, 0, 0, 0 };

        ensure_visible_thumbs();
        if (nvisible == 0) {
            int bw = 540, bh2 = 190;
            int bx = L_X0 + (cardw - bw) / 2;
            int by = L_TOP + (list_area_h() - bh2) / 2;
            fill_round_rect_aa(screen, bx, by, bw, bh2, 22, C_SURF_SUB);
            const char* tmsg = "未找到本地音频文件";
            const char* smsg = "支持 MP3 / FLAC / OGG / WAV · 点击「选择目录」或直接将文件夹拖进窗口";
            if (view == VIEW_FAVORITE && ntracks > 0) {
                tmsg = "暂无收藏歌曲";
                smsg = "点击曲目右侧爱心即可收藏";
            } else if (view == VIEW_SEARCH && ntracks > 0) {
                tmsg = "没有匹配的搜索结果";
                smsg = "点击右上角搜索栏中的「清除」恢复完整曲库";
            }
            draw_text_centered(screen, font, 19, bx + 16, bx + bw - 16, by + 36, C_TXT, tmsg);
            draw_text_centered(screen, font, 13.5f, bx + 16, bx + bw - 16, by + 74, C_TXT2, smsg);
            int bt_w = 180, bt_h = 40;
            int bt_x = bx + (bw - bt_w) / 2, bt_y = by + 118;
            fill_round_rect_aa(screen, bt_x, bt_y, bt_w, bt_h, 14, C_ACCENT);
            draw_text_centered(screen, font, 14.5f, bt_x, bt_x + bt_w, bt_y + 11, C_ON_ACCENT, "选择音乐目录");
            rc_empty_rescan = (Rect){ bt_x, bt_y, bt_w, bt_h };
        } else {
            int i0 = (int)floorf(scroll_px / (float)L_ROW) - 1;
            if (i0 < 0) i0 = 0;
            int i1 = (int)ceilf((scroll_px + (float)list_area_h()) / (float)L_ROW) + 1;
            if (i1 > nvisible) i1 = nvisible;

            int hover_pos = -1;
            if (!touch_drag && mx >= L_X0 && mx < L_X1 && my >= L_TOP && my < list_bot) {
                hover_pos = (int)floorf(((float)(my - L_TOP) + scroll_px) / (float)L_ROW);
            }

            canvas_set_clip(screen, LCD_X + 6, L_TOP, LCD_X + LCD_W - 6, list_bot);
            for (int i = i0; i < i1; i++) {
                int idx = vis_at(i);
                if (idx < 0) break;
                int y = L_TOP + i * L_ROW - (int)scroll_px;
                int pad_v = (L_ROW - L_CARD_H) / 2;
                int cy0 = y + pad_v;
                int ch = L_CARD_H;
                if (cy0 + ch <= L_TOP || cy0 >= list_bot) continue;

                int is_playing = (idx == playing_idx);
                int is_hov = (i == hover_pos) || (touch_drag && touch_press_idx == i);
                int is_sel = (idx == sel);

                uint32_t row_bg = C_CARD;
                if (is_playing) row_bg = C_TONAL_BG;
                else if (is_hov || (playing_idx < 0 && is_sel)) row_bg = C_CARD_SEL;

                fill_round_rect_aa(screen, L_X0, cy0, cardw - 10, ch, 15, row_bg);
                if (is_hov && !is_playing) {
                    stroke_round_rect_aa(screen, L_X0, cy0, cardw - 10, ch, 15, 1, C_ACCENT, 130);
                }
                if (is_playing) {
                    fill_round_rect_aa(screen, L_X0 + 7, cy0 + ch / 2 - 11, 4, 22, 2, C_ACCENT);
                }

                int th_x = L_X0 + 16;
                int th_y = cy0 + (ch - THUMB_SZ) / 2;
                if (tracks[idx].thumb)
                    blit_rgb_round(screen, th_x, th_y, tracks[idx].thumb, THUMB_SZ, THUMB_SZ, 12);

                char nm[256];
                basename_no_ext(tracks[idx].path, nm, sizeof(nm));
                const char* ttl = tracks[idx].meta.title[0] ? tracks[idx].meta.title : nm;
                const char* art = tracks[idx].meta.artist[0] ? tracks[idx].meta.artist : "未知艺术家";

                uint32_t tc = is_playing ? C_TONAL_TXT : C_TXT;
                uint32_t sc = is_playing ? C_TONAL_TXT : C_TXT3;
                int txtx = th_x + THUMB_SZ + 14;
                int right_res = 62 + (is_playing ? 64 : 0);
                int maxw = (L_X0 + cardw - 10) - txtx - right_res;
                if (maxw < 80) maxw = 80;

                int block = L_TITLE + 4 + L_ART;
                int ty2 = cy0 + (ch - block) / 2 - 1;
                draw_text_max(screen, font, (float)L_TITLE, txtx, ty2, tc, ttl, maxw);

                char fmt[12];
                track_format_tag(tracks[idx].path, fmt, sizeof(fmt));
                char subline[300];
                snprintf(subline, sizeof(subline), "%s · %s", fmt, art);
                draw_text_max(screen, font, (float)L_ART, txtx, ty2 + L_TITLE + 4, sc, subline, maxw);

                if (is_playing) {
                    const char* st_tag = audio_paused() ? "已暂停" : "播放中";
                    draw_text_right(screen, font, 12.5f, L_X1 - 54, cy0 + (ch - 13) / 2, C_ACCENT, st_tag);
                }

                int hx = L_X1 - 40;
                int hy = cy0 + (ch - 18) / 2;
                int h_hov = (mx >= hx - 8 && mx <= hx + 26 && my >= cy0 && my < cy0 + ch);
                icon_heart(screen, hx, hy, 18, tracks[idx].fav ? C_HEART : (h_hov ? C_TXT2 : C_TRACK_BG));
            }
            canvas_reset_clip(screen);

            float ms = max_scroll_px();
            if (ms > 0.0f) {
                int sbx = LCD_X + LCD_W - 14;
                int track_h = list_area_h() - 12;
                int thumb_h = (int)((float)list_area_h() / (float)(nvisible * L_ROW) * (float)track_h);
                if (thumb_h < 32) thumb_h = 32;
                if (thumb_h > track_h) thumb_h = track_h;
                int thumb_y = L_TOP + 6 + (int)((scroll_px / ms) * (float)(track_h - thumb_h));
                rc_desk_sb = (Rect){ sbx - 6, L_TOP + 2, 18, list_area_h() - 4 };
                int sb_hot = touch_sb_drag || in_rc(mx, my, rc_desk_sb);
                fill_round_rect_aa(screen, sbx, L_TOP + 6, 6, track_h, 3, C_TRACK_BG);
                fill_round_rect_aa(screen, sbx, thumb_y, 6, thumb_h, 3, sb_hot ? C_ACCENT : C_TXT3);
            }
        }

        // ---- 右栏：正在播放封面与常驻实时歌词卡片 (484 x 550) ----
        int rpx = 776, rpy = 66, rpw = 484, rph = 550;
        fill_round_rect_shadow(screen, rpx, rpy, rpw, rph, 24, g_is_light ? 22 : 46);
        if (playing_idx >= 0 && cover_ready && cover_src) {
            if (!lcd_ambient || lcd_ambient->w != rpw || lcd_ambient->h != rph) rebuild_lcd_ambient();
            if (lcd_ambient) blit_rgb_round(screen, rpx, rpy, lcd_ambient->px, lcd_ambient->w, lcd_ambient->h, 24);
            else fill_round_rect_aa(screen, rpx, rpy, rpw, rph, 24, C_SURFACE);
        } else {
            fill_round_rect_aa(screen, rpx, rpy, rpw, rph, 24, C_SURFACE);
        }
        stroke_round_rect_aa(screen, rpx, rpy, rpw, rph, 24, 1, C_WHEEL_RING, 120);

        int cx = rpx + 20, cy = rpy + 20, csz = COVER_PORT_SZ;
        ui_cover_x = cx; ui_cover_y = cy; ui_cover_sz = csz;
        fill_round_rect_shadow(screen, cx, cy, csz, csz, 22, g_is_light ? 30 : 54);
        const uint32_t* hero_cov = (playing_idx >= 0 && cover_port) ? cover_port : poster_pod;
        if (playing_idx >= 0 && cover_port) {
            blit_rgb_round(screen, cx, cy, cover_port, COVER_PORT_SZ, COVER_PORT_SZ, 22);
        } else if ((size_t)(poster_end - poster_start) >= 320 * 320 * 4) {
            uint32_t tmp_cov[COVER_PORT_SZ * COVER_PORT_SZ];
            resample_cover_square((const uint32_t*)poster_start, 320, 320, tmp_cov, COVER_PORT_SZ, 1);
            blit_rgb_round(screen, cx, cy, tmp_cov, COVER_PORT_SZ, COVER_PORT_SZ, 22);
        } else if (hero_cov) {
            blit_rgb_round(screen, cx, cy, hero_cov, POD_ART_SZ, POD_ART_SZ, 22);
        }

        int mx_col = cx + csz + 18;
        int mw_col = (rpx + rpw - 18) - mx_col;
        if (playing_idx >= 0) {
            char fmt_b[64];
            snprintf(fmt_b, sizeof(fmt_b), "%s · %d/%d", p_fmt[0] ? p_fmt : "AUDIO", playing_idx + 1, ntracks);
            int bdg_w = text_width(font, 11.5f, fmt_b) + 18;
            fill_round_rect_aa(screen, mx_col, cy + 4, bdg_w, 24, 10, C_TONAL_BG);
            draw_text_centered(screen, font, 11.5f, mx_col, mx_col + bdg_w, cy + 9, C_TONAL_TXT, fmt_b);

            draw_text_max(screen, font, 20, mx_col, cy + 38, C_TXT, p_title, mw_col);
            draw_text_max(screen, font, 14, mx_col, cy + 70, C_TXT2, p_artist[0] ? p_artist : "未知艺术家", mw_col);

            if (lyr && lyr->source[0]) {
                char lsrc[80];
                snprintf(lsrc, sizeof(lsrc), "歌词: %s (%d行)", lyr->source, lyr->n);
                draw_text_max(screen, font, 12, mx_col, cy + 100, C_TONAL_TXT, lsrc, mw_col);
            } else {
                const char* tip = lyrics_online_busy(g_lyr_req_id) ? "正在在线匹配歌词…" : "支持 LRCLIB 在线匹配与拖入 .lrc";
                draw_text_max(screen, font, 12, mx_col, cy + 100, C_TXT3, tip, mw_col);
            }

            rc_btn_fav_p = draw_m3_sq_btn(mx_col, cy + 144, 108, 36, 12, 13.5f,
                                          fav_on ? "♥ 已喜欢" : "♡ 喜欢", fav_on,
                                          in_rc(mx, my, (Rect){mx_col, cy + 144, 108, 36}));
            rc_btn_full  = draw_m3_sq_btn(mx_col + 116, cy + 144, 118, 36, 12, 13.5f,
                                          "沉浸全屏 [F]", 0,
                                          in_rc(mx, my, (Rect){mx_col + 116, cy + 144, 118, 36}));
        } else {
            rc_btn_fav_p = (Rect){ 0, 0, 0, 0 };
            rc_btn_full  = (Rect){ 0, 0, 0, 0 };
            draw_text(screen, font, 21, mx_col, cy + 28, C_TXT, "Sirène");
            draw_text_max(screen, font, 13.5f, mx_col, cy + 62, C_TXT2, "点击左侧曲目即刻开始播放", mw_col);
            draw_text_max(screen, font, 12.5f, mx_col, cy + 92, C_TXT3, "右侧区域将自动滚动显示歌词", mw_col);
        }

        fill_rect_a(screen, rpx + 18, cy + csz + 14, rpw - 36, 1, C_WHEEL_RING, 90);
        draw_desktop_lyrics_block(rpx + 16, rpx + rpw - 16, cy + csz + 24, rpy + rph - 16, pos, dur, mx, my, 0);
    } else {
        // ---- 桌面沉浸式全屏歌词视图 (1240 x 550 大圆角英雄卡) ----
        int hx = 20, hy = 66, hw = 1240, hh = 550;
        fill_round_rect_shadow(screen, hx, hy, hw, hh, 24, g_is_light ? 24 : 50);
        if (playing_idx >= 0 && cover_ready && cover_src) {
            if (!lcd_ambient || lcd_ambient->w != hw || lcd_ambient->h != hh) rebuild_lcd_ambient();
            if (lcd_ambient) blit_rgb_round(screen, hx, hy, lcd_ambient->px, lcd_ambient->w, lcd_ambient->h, 24);
            else fill_round_rect_aa(screen, hx, hy, hw, hh, 24, C_SURFACE);
        } else {
            fill_round_rect_aa(screen, hx, hy, hw, hh, 24, C_SURFACE);
        }
        stroke_round_rect_aa(screen, hx, hy, hw, hh, 24, 1, C_WHEEL_RING, 120);

        rc_btn_full = (Rect){ 0, 0, 0, 0 };
        rc_desk_sb = (Rect){ 0, 0, 0, 0 };
        rc_p_back = draw_m3_sq_btn(hx + 22, hy + 18, 132, 34, 12, 13.5f, "← 返回双栏 [F]", 1,
                                   in_rc(mx, my, (Rect){hx + 22, hy + 18, 132, 34}));

        int csz = COVER_SRC_SZ;
        int cx = hx + 58, cy = hy + 96;
        ui_cover_x = cx; ui_cover_y = cy; ui_cover_sz = csz;
        fill_round_rect_shadow(screen, cx, cy, csz, csz, 26, g_is_light ? 36 : 60);
        if (cover_src) blit_rgb_round(screen, cx, cy, cover_src, COVER_SRC_SZ, COVER_SRC_SZ, 26);

        rc_btn_fav_p = draw_m3_sq_btn(cx + 30, cy + csz + 24, 124, 38, 14, 14.0f,
                                      fav_on ? "♥ 已喜欢" : "♡ 喜欢", fav_on,
                                      in_rc(mx, my, (Rect){cx + 30, cy + csz + 24, 124, 38}));
        rc_desk_full_mode = draw_m3_sq_btn(cx + 166, cy + csz + 24, 124, 38, 14, 14.0f,
                                           mode_names[play_mode], play_mode != MODE_LOOP_ALL,
                                           in_rc(mx, my, (Rect){cx + 166, cy + csz + 24, 124, 38}));

        int lx0 = cx + csz + 56, lx1 = hx + hw - 44;
        draw_text_max(screen, font, 28, lx0, hy + 34, C_TXT, playing_idx >= 0 ? p_title : "Sirène", lx1 - lx0);
        draw_text_max(screen, font, 16, lx0, hy + 72, C_TXT2,
                      (playing_idx >= 0 && p_artist[0]) ? p_artist : "本地音乐播放器", lx1 - lx0);
        fill_round_rect_aa(screen, lx0, hy + 104, 52, 4, 2, C_ACCENT);
        draw_desktop_lyrics_block(lx0, lx1, hy + 124, hy + hh - 20, pos, dur, mx, my, 1);
    }

    // 3. 底部 Material You 播控与粗波浪进度条栏 (x=20..1260, y=628..702, 大圆角方块按钮)
    int dx = 20, dy = 628, dw_bar = 1240, dh = 74;
    fill_round_rect_shadow(screen, dx, dy, dw_bar, dh, 22, g_is_light ? 20 : 44);
    fill_round_rect_aa(screen, dx, dy, dw_bar, dh, 22, C_SURFACE);
    stroke_round_rect_aa(screen, dx, dy, dw_bar, dh, 22, 1, C_WHEEL_RING, 115);

    rc_mini_bar = (Rect){ dx + 12, dy + 10, 276, 54 };
    const uint32_t* mimg = (playing_idx >= 0 && cover_mini) ? cover_mini : poster_mini;
    if (mimg) blit_rgb_round(screen, dx + 16, dy + 16, mimg, MINI_ART_SZ, MINI_ART_SZ, 12);
    if (playing_idx >= 0) {
        draw_text_max(screen, font, 15, dx + 68, dy + 17, C_TXT, p_title, 214);
        draw_text_max(screen, font, 12.5f, dx + 68, dy + 39, C_TXT2,
                      p_artist[0] ? p_artist : p_fmt, 214);
    } else {
        draw_text(screen, font, 15, dx + 68, dy + 18, C_TXT, "Sirène Desktop");
        draw_text(screen, font, 12, dx + 68, dy + 39, C_TXT3, "HiDPI 清晰横屏版");
    }

    // 中左：Material You 大方块圆角播控键（取代巨大 iPod 圆形转盘）
    rc_desk_prev = (Rect){ 310, dy + 15, 44, 44 };
    rc_desk_play = (Rect){ 364, dy + 12, 62, 50 };
    rc_desk_next = (Rect){ 436, dy + 15, 44, 44 };

    fill_round_rect_aa(screen, rc_desk_prev.x, rc_desk_prev.y, rc_desk_prev.w, rc_desk_prev.h, 14,
                       in_rc(mx, my, rc_desk_prev) ? C_CARD_SEL : C_SURF_SUB);
    icon_prev(screen, rc_desk_prev.x + 13, rc_desk_prev.y + 13, 18, C_TXT);

    uint32_t play_bg = C_ACCENT;
    fill_round_rect_aa(screen, rc_desk_play.x, rc_desk_play.y, rc_desk_play.w, rc_desk_play.h, 16, play_bg);
    if (in_rc(mx, my, rc_desk_play)) {
        stroke_round_rect_aa(screen, rc_desk_play.x - 1, rc_desk_play.y - 1,
                             rc_desk_play.w + 2, rc_desk_play.h + 2, 17, 2, C_TONAL_BG, 210);
    }
    if (playing_idx >= 0 && !audio_paused()) {
        icon_pause_only(screen, rc_desk_play.x + 20, rc_desk_play.y + 14, 22, C_ON_ACCENT);
    } else {
        icon_play_only(screen, rc_desk_play.x + 21, rc_desk_play.y + 14, 22, C_ON_ACCENT);
    }

    fill_round_rect_aa(screen, rc_desk_next.x, rc_desk_next.y, rc_desk_next.w, rc_desk_next.h, 14,
                       in_rc(mx, my, rc_desk_next) ? C_CARD_SEL : C_SURF_SUB);
    icon_next(screen, rc_desk_next.x + 13, rc_desk_next.y + 13, 18, C_TXT);

    // 中右：Material You 粗波浪动态进度条 + 起止时间
    ui_px0 = 552; ui_px1 = 964; ui_py = dy + 33;
    char t1[32], t2[32];
    fmt_time(pos, t1, sizeof(t1));
    fmt_time(dur, t2, sizeof(t2));
    draw_text_right(screen, font, 12.5f, ui_px0 - 12, dy + 30, C_TXT2, t1);
    draw_text(screen, font, 12.5f, ui_px1 + 12, dy + 30, C_TXT2, t2);

    float frac = (dur > 0.0) ? clampf((float)(pos / dur), 0.0f, 1.0f) : 0.0f;
    float amp = (playing_idx >= 0 && !audio_paused()) ? wave_amp_cur : (wave_amp_cur > 0.05f ? wave_amp_cur : 0.0f);
    if (playing_idx >= 0 && !audio_paused() && amp < 0.5f) amp = 4.4f;
    draw_m3_wavy_progress(screen, ui_px0, ui_px1, dy + 37, frac,
                          wave_phase, amp, 28.0f, 7, 6, 1, C_ACCENT, C_TRACK_BG);

    // 最右：鼠标直拖音量滑块 + 静音按键
    rc_desk_mute = (Rect){ 1036, dy + 19, 36, 36 };
    fill_round_rect_aa(screen, rc_desk_mute.x, rc_desk_mute.y, rc_desk_mute.w, rc_desk_mute.h, 11,
                       in_rc(mx, my, rc_desk_mute) ? C_CARD_SEL : C_SURF_SUB);
    icon_speaker(screen, rc_desk_mute.x + 9, rc_desk_mute.y + 9, 18,
                 audio_volume() <= 0.01f ? C_HEART : C_ACCENT);

    float vol = audio_volume();
    vol_sx0 = 1082; vol_sx1 = 1192; vol_sy = dy + 37;
    rc_desk_vol_track = (Rect){ vol_sx0 - 8, dy + 16, (vol_sx1 - vol_sx0) + 16, 42 };
    int vsw = vol_sx1 - vol_sx0;
    fill_round_rect_aa(screen, vol_sx0, vol_sy - 3, vsw, 6, 3, C_TRACK_BG);
    int vfw = (int)(vsw * vol + 0.5f);
    if (vfw > 0) fill_round_rect_aa(screen, vol_sx0, vol_sy - 3, vfw, 6, 3, C_ACCENT);
    fill_ellipse_aa(screen, vol_sx0 + vfw, vol_sy, in_rc(mx, my, rc_desk_vol_track) || touch_vol_inline ? 9 : 7,
                    in_rc(mx, my, rc_desk_vol_track) || touch_vol_inline ? 9 : 7, C_ACCENT);

    char vpct[16];
    snprintf(vpct, sizeof(vpct), "%d%%", (int)(vol * 100 + 0.5f));
    draw_text_right(screen, font, 12.5f, 1244, dy + 30, C_TXT2, vpct);

    // 指针反馈：悬停在任意可交互区域时切换手型指针
    int hand = 0;
    if (in_rc(mx, my, rc_tab_all) || in_rc(mx, my, rc_tab_fav) || in_rc(mx, my, rc_btn_ori) ||
        in_rc(mx, my, rc_btn_rescan) || in_rc(mx, my, rc_btn_mode) || in_rc(mx, my, rc_btn_search) ||
        in_rc(mx, my, rc_btn_theme) || in_rc(mx, my, rc_btn_diag) || in_rc(mx, my, rc_desk_prev) ||
        in_rc(mx, my, rc_desk_play) || in_rc(mx, my, rc_desk_next) || in_rc(mx, my, rc_desk_mute) ||
        in_rc(mx, my, rc_desk_vol_track) || in_rc(mx, my, rc_btn_fav_p) || in_rc(mx, my, rc_btn_full) ||
        in_rc(mx, my, rc_p_back) || in_rc(mx, my, rc_desk_sb) || in_rc(mx, my, rc_mini_bar) ||
        in_rc(mx, my, rc_desk_full_mode) || in_rc(mx, my, rc_empty_rescan) ||
        in_rect(mx, my, ui_px0 - 10, dy + 12, ui_px1 - ui_px0 + 20, 50) ||
        (ui_cover_sz > 0 && in_rect(mx, my, ui_cover_x, ui_cover_y, ui_cover_sz, ui_cover_sz)) ||
        (!desktop_full && my >= L_TOP && my < list_bottom_y() && mx >= L_X0 && mx < L_X1)) {
        hand = 1;
    } else {
        for (int k = 0; k < ndesk_lyr_hits; k++) {
            if (in_rc(mx, my, desk_lyr_hits[k].rc)) { hand = 1; break; }
        }
    }
    be_set_cursor(hand);
}

static void desktop_seek_scrollbar(int y) {
    float ms = max_scroll_px();
    if (ms <= 0.0f) return;
    int track_h = list_area_h() - 12;
    if (track_h < 20) return;
    int thumb_h = (int)((float)list_area_h() / (float)(nvisible * L_ROW) * (float)track_h);
    if (thumb_h < 32) thumb_h = 32;
    if (thumb_h > track_h) thumb_h = track_h;
    int usable = track_h - thumb_h;
    if (usable <= 0) return;
    float frac = (float)(y - (L_TOP + 6 + thumb_h / 2)) / (float)usable;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    scroll_px = frac * ms;
    scroll_target = scroll_px;
}

static void desktop_set_vol_from_x(int x) {
    if (vol_sx1 <= vol_sx0) return;
    float v = clampf((float)(x - vol_sx0) / (float)(vol_sx1 - vol_sx0), 0.0f, 1.0f);
    audio_set_volume(v);
    if (v > 0.02f) saved_mute_vol = v;
    save_settings();
}

static void handle_desktop_input(TouchInfo* t, uint64_t down, uint64_t rep, int wh) {
    int mx = -1, my = -1;
    be_mouse_pos(&mx, &my);

    // 滚轮分区控制：音量栏调音量 / 进度条快进退 / 曲库平滑翻页
    if (wh != 0) {
        if (in_rc(mx, my, rc_desk_vol_track) || in_rc(mx, my, rc_desk_mute)) {
            audio_set_volume(audio_volume() + (float)wh * 0.05f);
            save_settings();
        } else if (in_rect(mx, my, ui_px0 - 12, 628, ui_px1 - ui_px0 + 24, 74) && audio_duration() > 0.0) {
            double p = audio_position() + (double)wh * 5.0;
            if (p < 0.0) p = 0.0;
            if (p > audio_duration()) p = audio_duration();
            audio_seek(p);
        } else if (!desktop_full && max_scroll_px() > 0.0f) {
            scroll_target -= (float)wh * (float)L_ROW * 1.35f;
            float ms = max_scroll_px();
            if (scroll_target < 0.0f) scroll_target = 0.0f;
            if (scroll_target > ms) scroll_target = ms;
        }
    }

    // 键盘快捷键
    if (down & BTN_X) toggle_desktop_fullscreen();
    if ((down & BTN_B) && desktop_full) toggle_desktop_fullscreen();
    if (down & BTN_ZR) cycle_theme();
    if (down & BTN_MINUS) open_search_keyboard();
    if (down & (BTN_LEFT | BTN_L)) step_track_dir(-1);
    if (down & (BTN_RIGHT | BTN_R)) step_track_dir(+1);
    if (down & BTN_ZL) {
        int tgt = (playing_idx >= 0) ? playing_idx : sel;
        if (tgt >= 0) toggle_fav(tgt);
    }
    if (down & BTN_A) {
        if (playing_idx >= 0) {
            if (audio_paused()) audio_resume();
            else audio_pause();
        } else if (nvisible > 0 && !loading) {
            int idx = (sel >= 0) ? sel : vis_at(0);
            if (idx >= 0) load_track(idx);
        }
    }
    if (rep & BTN_UP) {
        if (desktop_full) {
            audio_set_volume(audio_volume() + 0.05f);
            save_settings();
        } else if (nvisible > 0) {
            int p = sel_pos();
            p = (p <= 0) ? 0 : p - 1;
            sel = vis_at(p);
            ensure_pos_visible(p);
        }
    }
    if (rep & BTN_DOWN) {
        if (desktop_full) {
            audio_set_volume(audio_volume() - 0.05f);
            save_settings();
        } else if (nvisible > 0) {
            int p = sel_pos();
            p = (p < 0) ? 0 : (p + 1 < nvisible ? p + 1 : nvisible - 1);
            sel = vis_at(p);
            ensure_pos_visible(p);
        }
    }
    if (rep & BTN_STICK_R_UP)   { audio_set_volume(audio_volume() + 0.05f); save_settings(); }
    if (rep & BTN_STICK_R_DOWN) { audio_set_volume(audio_volume() - 0.05f); save_settings(); }

    // 连续鼠标拖拽（进度条 / 音量条 / 滚动条 / 列表滑动）
    if (touch_scrub) {
        if (t->held || t->down) { scrub_preview_sec = calc_scrub_sec(t->x, ui_px0, ui_px1); return; }
        if (t->up) {
            scrub_preview_sec = calc_scrub_sec(t->x, ui_px0, ui_px1);
            touch_scrub = 0;
            audio_seek(scrub_preview_sec);
            return;
        }
        touch_scrub = 0;
    }
    if (touch_vol_inline) {
        if (t->held) { desktop_set_vol_from_x(t->x); return; }
        if (t->up) { touch_vol_inline = 0; return; }
    }
    if (touch_sb_drag) {
        if (t->held) { desktop_seek_scrollbar(t->y); return; }
        if (t->up) { touch_sb_drag = 0; return; }
    }
    if (touch_drag && t->held && max_scroll_px() > 0.0f) {
        int dy = touch_last_y - t->y;
        if (abs(t->y - touch_drag_start_y) > 8) touch_moved = 1;
        if (dy != 0 && touch_moved) {
            scroll_px += (float)dy;
            float ms = max_scroll_px();
            if (scroll_px < 0.0f) scroll_px = 0.0f;
            if (scroll_px > ms) scroll_px = ms;
            scroll_target = scroll_px;
        }
        touch_last_y = t->y;
    }

    if (!t->down) {
        if (t->up && touch_drag) {
            if (!touch_moved && touch_press_idx >= 0 && touch_press_idx < nvisible) {
                int idx = vis_at(touch_press_idx);
                if (idx >= 0) {
                    if (t->x >= L_X1 - 60 && t->x <= L_X1) toggle_fav(idx);
                    else play_or_toggle_in_list(idx);
                }
            }
            touch_drag = 0;
            touch_moved = 0;
            touch_press_idx = -1;
        }
        return;
    }

    int x = t->x, y = t->y;
    if (in_rc(x, y, rc_desk_sr_clear)) { search_q[0] = 0; set_view(VIEW_HOME); return; }
    if (in_rc(x, y, rc_tab_all)) { search_q[0] = 0; set_view(VIEW_HOME); return; }
    if (in_rc(x, y, rc_tab_fav)) { set_view(view == VIEW_FAVORITE ? VIEW_HOME : VIEW_FAVORITE); return; }
    if (in_rc(x, y, rc_btn_ori)) { open_desktop_folder(); return; }
    if (in_rc(x, y, rc_btn_rescan) || in_rc(x, y, rc_empty_rescan)) {
        if (in_rc(x, y, rc_empty_rescan)) open_desktop_folder();
        else scan_library(1);
        return;
    }
    if (in_rc(x, y, rc_btn_mode) || in_rc(x, y, rc_desk_full_mode)) { cycle_play_mode(); return; }
    if (in_rc(x, y, rc_btn_search)) { open_search_keyboard(); return; }
    if (in_rc(x, y, rc_btn_theme)) { cycle_theme(); return; }
    if (in_rc(x, y, rc_btn_diag)) { show_diag = 1; trigger_modal_fade(); return; }

    if (in_rc(x, y, rc_desk_prev)) { step_track_dir(-1); return; }
    if (in_rc(x, y, rc_desk_next)) { step_track_dir(+1); return; }
    if (in_rc(x, y, rc_desk_play)) {
        if (playing_idx >= 0) {
            if (audio_paused()) audio_resume();
            else audio_pause();
        } else if (nvisible > 0 && !loading) {
            int idx = (sel >= 0) ? sel : vis_at(0);
            if (idx >= 0) load_track(idx);
        }
        return;
    }
    if (in_rc(x, y, rc_desk_mute)) {
        if (audio_volume() > 0.01f) {
            saved_mute_vol = audio_volume();
            audio_set_volume(0.0f);
        } else {
            audio_set_volume(saved_mute_vol > 0.05f ? saved_mute_vol : 0.85f);
        }
        save_settings();
        return;
    }
    if (in_rc(x, y, rc_desk_vol_track)) {
        touch_vol_inline = 1;
        desktop_set_vol_from_x(x);
        return;
    }
    if (in_rect(x, y, ui_px0 - 12, 628, ui_px1 - ui_px0 + 24, 74)) {
        if (audio_duration() > 0) {
            touch_scrub = 1;
            scrub_preview_sec = calc_scrub_sec(x, ui_px0, ui_px1);
        }
        return;
    }
    if (in_rc(x, y, rc_btn_fav_p)) {
        if (playing_idx >= 0) toggle_fav(playing_idx);
        return;
    }
    if (in_rc(x, y, rc_btn_full) || in_rc(x, y, rc_p_back) || in_rc(x, y, rc_mini_bar)) {
        toggle_desktop_fullscreen();
        return;
    }
    if (ui_cover_sz > 0 && in_rect(x, y, ui_cover_x, ui_cover_y, ui_cover_sz, ui_cover_sz)) {
        if (playing_idx >= 0) {
            if (audio_paused()) audio_resume();
            else audio_pause();
        }
        return;
    }
    for (int k = 0; k < ndesk_lyr_hits; k++) {
        if (in_rc(x, y, desk_lyr_hits[k].rc) && lyr && !lyr->unsynced) {
            int li = desk_lyr_hits[k].idx;
            if (li >= 0 && li < lyr->n) audio_seek(lyr->lines[li].t);
            return;
        }
    }
    if (!desktop_full && in_rc(x, y, rc_desk_sb)) {
        touch_sb_drag = 1;
        desktop_seek_scrollbar(y);
        return;
    }
    int list_bot = list_bottom_y();
    if (!desktop_full && nvisible > 0 && y >= L_TOP && y < list_bot && x >= L_X0 && x < L_X1) {
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

// ---------- 弹窗绘制（由全局 capture_fade 统一驱动平滑 60FPS 淡入/淡出） ----------
static void draw_volume_overlay(void) {
    if (vol_overlay_timer <= 0) return;
    vol_overlay_timer--;
    char vs[64];
    snprintf(vs, sizeof(vs), "音量 %d%%", (int)(audio_volume() * 100 + 0.5f));
    int bw2 = 240, bh2 = 66;
    int bx = LCD_X + (LCD_W - bw2) / 2, by = LCD_Y + 58;
    fill_round_rect_shadow(screen, bx, by, bw2, bh2, 20, 44);
    fill_round_rect_aa(screen, bx, by, bw2, bh2, 20, C_SURFACE);
    stroke_round_rect_aa(screen, bx, by, bw2, bh2, 20, 1, C_WHEEL_RING, 160);
    icon_speaker(screen, bx + 22, by + 14, 18, C_ACCENT);
    draw_text_centered(screen, font, 17, bx + 18, bx + bw2, by + 13, C_TXT, vs);
    fill_round_rect_aa(screen, bx + 22, by + 44, bw2 - 44, 6, 3, C_TRACK_BG);
    fill_round_rect_aa(screen, bx + 22, by + 44, (int)((bw2 - 44) * audio_volume()), 6, 3, C_ACCENT);
}

static void draw_vol_panel(void) {
    fill_rect_a(screen, 0, 0, SCR_W, SCR_H, COL(0, 0, 0, 255), 135);
    int cw = portrait() ? 560 : 480;
    int ch = 180;
    int cx = (SCR_W - cw) / 2;
    int cy = (SCR_H - ch) / 2;
    rc_vol_box = (Rect){ cx, cy, cw, ch };
    fill_round_rect_shadow(screen, cx, cy, cw, ch, 26, 56);
    fill_round_rect_aa(screen, cx, cy, cw, ch, 26, C_SURFACE);
    stroke_round_rect_aa(screen, cx, cy, cw, ch, 26, 1, C_WHEEL_RING, 160);

    float v = audio_volume();
    char ttl[48];
    snprintf(ttl, sizeof(ttl), "音量调节 · %d%%", (int)(v * 100 + 0.5f));
    icon_speaker(screen, cx + 26, cy + 26, 20, C_ACCENT);
    draw_text(screen, font, 19, cx + 54, cy + 24, C_TXT, ttl);

    rc_vol_close = draw_m3_pill_fixed(cx + cw - 92, cy + 18, 68, 34, 13.5f, "完成", 1, 0);
    rc_vol_mute  = draw_m3_pill_fixed(rc_vol_close.x - 76, cy + 18, 68, 34, 13.5f,
                                      v > 0.01f ? "静音" : "恢复", v <= 0.01f, 0);

    int row_y = cy + 96;
    fill_ellipse_aa(screen, cx + 44, row_y, 22, 22, C_SURF_SUB);
    draw_text_centered(screen, font, 24, cx + 22, cx + 66, row_y - 13, C_TXT, "−");
    rc_vol_minus = (Rect){ cx + 18, row_y - 26, 52, 52 };

    fill_ellipse_aa(screen, cx + cw - 44, row_y, 22, 22, C_SURF_SUB);
    draw_text_centered(screen, font, 24, cx + cw - 66, cx + cw - 22, row_y - 13, C_TXT, "+");
    rc_vol_plus = (Rect){ cx + cw - 70, row_y - 26, 52, 52 };

    vol_sx0 = cx + 82;
    vol_sx1 = cx + cw - 82;
    vol_sy  = row_y - 5;
    int sw = vol_sx1 - vol_sx0;
    fill_round_rect_aa(screen, vol_sx0, vol_sy, sw, 10, 5, C_TRACK_BG);
    int fw = (int)(sw * v);
    if (fw > 0) fill_round_rect_aa(screen, vol_sx0, vol_sy, fw, 10, 5, C_ACCENT);
    fill_ellipse_aa(screen, vol_sx0 + fw, vol_sy + 5, 11, 11, C_ACCENT);

    draw_text_centered(screen, font, 13, cx + 20, cx + cw - 20, cy + ch - 34, C_TXT3,
                       "拖动滑块或点击 − / + 调节 · 无需手柄即可纯触控操作");
}

static void draw_exit_confirm(void) {
    fill_rect_a(screen, 0, 0, SCR_W, SCR_H, COL(0, 0, 0, 255), 150);
    int cw = portrait() ? 460 : 420;
    int ch = 190;
    int cx = (SCR_W - cw) / 2;
    int cy = (SCR_H - ch) / 2;
    rc_exit_box = (Rect){ cx, cy, cw, ch };
    fill_round_rect_shadow(screen, cx, cy, cw, ch, 26, 58);
    fill_round_rect_aa(screen, cx, cy, cw, ch, 26, C_SURFACE);
    stroke_round_rect_aa(screen, cx, cy, cw, ch, 26, 1, C_WHEEL_RING, 160);

    draw_text_centered(screen, font, 21, cx + 20, cx + cw - 20, cy + 32, C_TXT, "退出 Sirène 播放器？");
    draw_text_centered(screen, font, 14, cx + 20, cx + cw - 20, cy + 68, C_TXT2,
                       "触屏点击“确认退出”或按 A 键即可返回桌面");

    int bw = (cw - 68) / 2, bh = 44, by = cy + ch - 68;
    fill_round_rect_aa(screen, cx + 24, by, bw, bh, 22, C_SURF_SUB);
    draw_text_centered(screen, font, 15.5f, cx + 24, cx + 24 + bw, by + 13, C_TXT2, "取消 [B]");
    rc_exit_cancel = (Rect){ cx + 24, by, bw, bh };

    int ox = cx + 24 + bw + 20;
    fill_round_rect_aa(screen, ox, by, bw, bh, 22, C_HEART);
    draw_text_centered(screen, font, 15.5f, ox, ox + bw, by + 13, COL(255, 255, 255, 255), "确认退出 [A]");
    rc_exit_ok = (Rect){ ox, by, bw, bh };
}

static void draw_diag(void) {
    fill_rect_a(screen, 0, 0, SCR_W, SCR_H, COL(0, 0, 0, 255), 155);
    int cw = portrait() ? 648 : 860;
    int ch = 468;
    int cx = (SCR_W - cw) / 2;
    int cy = (SCR_H - ch) / 2;
    rc_diag_box = (Rect){ cx, cy, cw, ch };
    fill_round_rect_shadow(screen, cx, cy, cw, ch, 28, 60);
    fill_round_rect_aa(screen, cx, cy, cw, ch, 28, C_SURFACE);
    stroke_round_rect_aa(screen, cx, cy, cw, ch, 28, 1, C_WHEEL_RING, 160);

    draw_text(screen, font, 22, cx + 32, cy + 28, C_TONAL_TXT, "Sirène v1.2.6 系统与歌词诊断");
    rc_diag_close = draw_m3_pill_fixed(cx + cw - 104, cy + 22, 76, 36, 14.0f, "关闭", 1, 0);

    char buf[1024];
    audio_get_diag(buf, sizeof(buf));
    int yy = cy + 82;
    char* line = strtok(buf, "\n");
    while (line && yy < cy + ch - 140) {
        draw_text_max(screen, font, 17, cx + 32, yy, C_TXT, line, cw - 64);
        yy += 32;
        line = strtok(NULL, "\n");
    }
    char extra[256];
    snprintf(extra, sizeof(extra), "曲库：%d 首   收藏：%d 首   模式：%s   主题：%s",
             ntracks, fav_count, mode_names[play_mode], theme_names[cur_theme]);
    draw_text_max(screen, font, 16, cx + 32, yy + 8, C_TXT2, extra, cw - 64);

    char lyr_info[280];
    if (lyr) snprintf(lyr_info, sizeof(lyr_info), "歌词状态：已解析 %d 行 (%s)", lyr->n, lyr->source);
    else snprintf(lyr_info, sizeof(lyr_info), "歌词状态：%s", lyrics_last_diag());
    draw_text_max(screen, font, 15.5f, cx + 32, yy + 40, C_TONAL_TXT, lyr_info, cw - 64);

    draw_text_max(screen, font, 13.5f, cx + 32, cy + ch - 44, C_TXT3,
                  be_is_desktop()
                      ? "按键：Space/A就地播放 · F/X全屏 · ←/→切歌 · Ctrl+F搜索 · Ctrl+O选目录"
                      : "按键：A就地播放 · X全屏切换 · Y竖屏切换 · 左方向键切歌 · B返回/退出", cw - 64);
}

static void search_edit_backspace(void) {
    size_t len = strlen(search_edit);
    while (len > 0 && ((unsigned char)search_edit[len - 1] & 0xC0) == 0x80) len--;
    if (len > 0) len--;
    search_edit[len] = 0;
}

static void handle_search_box_events(TouchInfo* t) {
    char u8[32];
    int act = 0;
    while (be_poll_text_char(u8, sizeof(u8), &act)) {
        if (act == 1) search_edit_backspace();
        else if (act == 2) {
            be_text_input(0);
            show_search_box = 0;
            apply_search_query(search_edit);
            return;
        } else if (act == 3) {
            capture_fade();
            be_text_input(0);
            show_search_box = 0;
            return;
        } else if (act == 0 && u8[0]) {
            size_t cl = strlen(search_edit), add = strlen(u8);
            if (cl + add + 1 < sizeof(search_edit)) memcpy(search_edit + cl, u8, add + 1);
        }
    }
    if (!t->down) return;
    int x = t->x, y = t->y;
    if (in_rc(x, y, rc_sr_ok)) {
        be_text_input(0);
        show_search_box = 0;
        apply_search_query(search_edit);
        return;
    }
    if (in_rc(x, y, rc_sr_clear)) {
        search_edit[0] = 0;
        return;
    }
    if (in_rc(x, y, rc_sr_cancel) || !in_rc(x, y, rc_sr_box)) {
        capture_fade();
        be_text_input(0);
        show_search_box = 0;
    }
}

static void draw_search_box(void) {
    fill_rect_a(screen, 0, 0, SCR_W, SCR_H, COL(0, 0, 0, 255), 145);
    int cw = 580, ch = 228;
    int cx = (SCR_W - cw) / 2, cy = 220;
    rc_sr_box = (Rect){ cx, cy, cw, ch };
    fill_round_rect_shadow(screen, cx, cy, cw, ch, 26, 56);
    fill_round_rect_aa(screen, cx, cy, cw, ch, 26, C_SURFACE);
    stroke_round_rect_aa(screen, cx, cy, cw, ch, 26, 1, C_WHEEL_RING, 160);

    draw_text(screen, font, 20, cx + 28, cy + 24, C_TXT, "搜索本地曲库");
    draw_text_right(screen, font, 13, cx + cw - 28, cy + 28, C_TXT3, "输入歌名、歌手或路径");

    int ibx = cx + 26, iby = cy + 68, ibw = cw - 52, ibh = 52;
    fill_round_rect_aa(screen, ibx, iby, ibw, ibh, 16, C_SURF_SUB);
    stroke_round_rect_aa(screen, ibx, iby, ibw, ibh, 16, 1, C_ACCENT, 180);
    if (search_edit[0]) {
        draw_text_max(screen, font, 18, ibx + 18, iby + 16, C_TXT, search_edit, ibw - 40);
    } else {
        draw_text(screen, font, 16, ibx + 18, iby + 17, C_TXT3, "直接键盘输入关键词，按 Enter 搜索…");
    }

    int by = cy + ch - 70, bw = 152, bh = 42;
    rc_sr_cancel = draw_m3_pill_fixed(cx + 28,               by, bw, bh, 14.5f, "取消 [Esc]", 0, 0);
    rc_sr_clear  = draw_m3_pill_fixed(cx + (cw - bw) / 2,    by, bw, bh, 14.5f, "清空", 0, 0);
    fill_round_rect_aa(screen, cx + cw - 28 - bw, by, bw, bh, 21, C_ACCENT);
    draw_text_centered(screen, font, 14.5f, cx + cw - 28 - bw, cx + cw - 28, by + 13, C_ON_ACCENT, "搜索 [Enter]");
    rc_sr_ok = (Rect){ cx + cw - 28 - bw, by, bw, bh };
}

// ---------- 按键逻辑（严格区分左方向键切歌与摇杆，A不进全屏，X全屏，Y竖屏） ----------
static void ui_list_input(uint64_t down, uint64_t rep) {
    rebuild_visible();

    if (rep & BTN_STICK_R_UP) {
        audio_set_volume(audio_volume() + 0.05f);
        save_settings();
        vol_overlay_timer = 70;
    }
    if (rep & BTN_STICK_R_DOWN) {
        audio_set_volume(audio_volume() - 0.05f);
        save_settings();
        vol_overlay_timer = 70;
    }

    if (nvisible == 0) {
        if (down & BTN_A) scan_library(1);
        return;
    }

    int pos = sel_pos();
    if (rep & (BTN_UP | BTN_STICK_L_UP)) {
        if (pos > 0) pos--;
        sel = vis_at(pos);
        ensure_pos_visible(pos);
    }
    if (rep & (BTN_DOWN | BTN_STICK_L_DOWN)) {
        if (pos < nvisible - 1) pos++;
        sel = vis_at(pos);
        ensure_pos_visible(pos);
    }
    if (down & BTN_A) {
        int i = vis_at(pos);
        if (i >= 0) play_or_toggle_in_list(i);
    }
    if (down & BTN_ZL) {
        int i = vis_at(pos);
        if (i >= 0) toggle_fav(i);
    }
}

static void ui_player_input(uint64_t down, uint64_t rep) {
    if (down & BTN_A) {
        if (audio_paused()) audio_resume(); else audio_pause();
    }
    if (down & BTN_B) {
        switch_page(UI_LIST);
        sel = playing_idx >= 0 ? playing_idx : sel;
        rebuild_visible();
    }
    if (down & BTN_ZL) {
        if (playing_idx >= 0) toggle_fav(playing_idx);
    }
    if (rep & BTN_STICK_L_LEFT) {
        double p = audio_position() - 10.0;
        if (p < 0) p = 0;
        audio_seek(p);
    }
    if (rep & (BTN_STICK_L_RIGHT | BTN_STICK_R_RIGHT)) {
        audio_seek(audio_position() + 10.0);
    }
    if (rep & (BTN_UP | BTN_STICK_R_UP)) {
        audio_set_volume(audio_volume() + 0.05f);
        save_settings();
        vol_overlay_timer = 70;
    }
    if (rep & (BTN_DOWN | BTN_STICK_R_DOWN)) {
        audio_set_volume(audio_volume() - 0.05f);
        save_settings();
        vol_overlay_timer = 70;
    }
}

// ---------- main ----------
int main(int argc, char** argv) {
    if (argc > 1 && argv[1] && argv[1][0]) be_set_cli_path(argv[1]);

    if (be_init() != 0) return 0;
    rng_state = (uint32_t)(plat_ticks_ns() ^ 0xA5A55A5Au);
    if (audio_init() != 0) {
        // 音频设备异常时仍允许进入 UI 自检
    }
    load_settings();
    if (be_is_desktop()) {
        char lcache[580];
        ensure_dir_recursive(be_config_dir());
        snprintf(lcache, sizeof(lcache), "%s/lyrics_cache", be_config_dir());
        ensure_dir_recursive(lcache);
        lyrics_set_cache_dir(lcache);
    }
    load_custom_dir();
    sixaxis_init();

    update_layout();
    apply_orientation(ORI_LAND);
    if (be_is_desktop()) ori_locked = 1;
    size_t font_len = (size_t)(font_ttf_end - font_ttf_start);
    font = font_load(font_ttf_start, font_len);

    build_poster_thumbs();

    tracks = (Track*)calloc(MAX_TRACKS, sizeof(Track));
    scan_library(0);
    memset(&meta, 0, sizeof(meta));

    const char* init_drop = be_poll_dropped_path();
    if (init_drop) handle_dropped_path(init_drop);

    int stride = 0;
    while (be_should_run()) {
        frame_begin();
        be_input_update();
        be_touch_update(&touch_now);

        if (g_lyr_req_id > 0) {
            Lyrics* onl = NULL;
            if (lyrics_poll_online(g_lyr_req_id, &onl)) {
                g_lyr_req_id = 0;
                if (onl) {
                    if (lyr) lyrics_free(lyr);
                    lyr = onl;
                }
            }
        }

        const char* dropped = be_poll_dropped_path();
        if (dropped) handle_dropped_path(dropped);

        uint64_t down = be_btn_down();
        uint64_t held = be_btn_held();
        uint64_t rep = repeat_keys(down, held,
            BTN_UP | BTN_DOWN |
            BTN_STICK_L_UP | BTN_STICK_L_DOWN | BTN_STICK_L_LEFT | BTN_STICK_L_RIGHT |
            BTN_STICK_R_UP | BTN_STICK_R_DOWN | BTN_STICK_R_LEFT | BTN_STICK_R_RIGHT);

        if (down & BTN_PLUS) break;

#ifdef __SWITCH__
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
#endif

        if (show_search_box) {
            if (be_is_desktop()) {
                int mx = -1, my = -1;
                be_mouse_pos(&mx, &my);
                be_set_cursor(in_rc(mx, my, rc_sr_ok) || in_rc(mx, my, rc_sr_clear) ||
                              in_rc(mx, my, rc_sr_cancel) ? 1 : 0);
            }
            handle_search_box_events(&touch_now);
        } else if (show_diag) {
            if (be_is_desktop()) {
                int mx = -1, my = -1;
                be_mouse_pos(&mx, &my);
                be_set_cursor(in_rc(mx, my, rc_diag_close) ? 1 : 0);
            }
            if ((touch_now.down && (!be_is_desktop() || in_rc(touch_now.x, touch_now.y, rc_diag_close) || !in_rc(touch_now.x, touch_now.y, rc_diag_box))) ||
                (down & (BTN_B | BTN_A))) {
                capture_fade(); show_diag = 0;
            }
        } else if (show_exit_confirm) {
            if (down & BTN_A) g_want_exit = 1;
            if (down & BTN_B) { capture_fade(); show_exit_confirm = 0; }
            handle_touch_exit_confirm(&touch_now);
        } else if (show_vol_panel) {
            if (down & BTN_B) { capture_fade(); show_vol_panel = 0; }
            handle_touch_vol_panel(&touch_now);
        } else {
            int hk = be_poll_hotkey();
            if (hk == 's') open_search_keyboard();
            else if (hk == 'f' && be_is_desktop()) open_desktop_folder();
            else if (hk == 'm') cycle_play_mode();
            else if (hk == 'c') { show_diag = 1; trigger_modal_fade(); }
            else if (hk == 'v' && !be_is_desktop()) { show_vol_panel = 1; trigger_modal_fade(); }

            int wh = be_wheel_delta();
            if (be_is_desktop()) {
                handle_desktop_input(&touch_now, down, rep, wh);
            } else {
                if (wh != 0) {
                    if (ui_state == UI_LIST && max_scroll_px() > 0.0f) {
                        scroll_target -= (float)wh * (float)L_ROW * 1.4f;
                        float ms = max_scroll_px();
                        if (scroll_target < 0.0f) scroll_target = 0.0f;
                        if (scroll_target > ms) scroll_target = ms;
                    } else if (ui_state == UI_PLAYER) {
                        audio_set_volume(audio_volume() + (float)wh * 0.05f);
                        save_settings();
                        vol_overlay_timer = 70;
                    }
                }

                if (down & BTN_Y) toggle_orientation();
                if (down & BTN_X) toggle_fullscreen_player();
                if (down & BTN_ZR) cycle_theme();
                if (down & BTN_MINUS) open_search_keyboard();

                if (down & (BTN_LEFT | BTN_L))  step_track_dir(-1);
                if (down & (BTN_RIGHT | BTN_R)) step_track_dir(+1);

                if (ui_state == UI_LIST) {
                    if (down & BTN_B) {
                        show_exit_confirm = 1;
                        trigger_modal_fade();
                    } else {
                        ui_list_input(down, rep);
                        handle_touch_list(&touch_now);
                    }
                } else {
                    ui_player_input(down, rep);
                    handle_touch_player(&touch_now);
                }
            }
        }
        if (g_want_exit) break;

        if (audio_finished() && playing_idx >= 0 && !loading) {
            load_track(pick_next(playing_idx, 0));
        }

        audio_pump();

        {
            int wave_on = (playing_idx >= 0 && !audio_paused());
            float target_amp = wave_on ? 4.4f : 0.0f;
            float wk = (float)(g_dt * 8.0);
            if (wk > 1.0f) wk = 1.0f;
            wave_amp_cur += (target_amp - wave_amp_cur) * wk;
            if (wave_on) {
                wave_phase += (float)(g_dt * 4.5);
                if (wave_phase > 628.3185f) wave_phase -= 628.3185f;
            }
        }

        if (fabsf(scroll_target - scroll_px) > 0.5f) {
            float k = (float)(g_dt * 22.0);
            if (k > 1.0f) k = 1.0f;
            scroll_px += (scroll_target - scroll_px) * k;
        } else {
            scroll_px = scroll_target;
        }

        if (be_is_desktop()) {
            int vw = SCR_W, vh = SCR_H;
            be_viewport_size(&vw, &vh);
            if (!screen || screen->w != vw || screen->h != vh) {
                if (screen) canvas_free(screen);
                screen = canvas_new_scaled(SCR_W, SCR_H, (float)vw / (float)SCR_W);
                fade_time = 0.0f; fade_mode = 0;
            }
        }
        uint32_t* fb = be_begin(&stride);
        if (be_is_desktop()) draw_desktop_ui();
        else if (ui_state == UI_LIST) draw_list();
        else draw_player();

        draw_volume_overlay();

        if (toast_timer > 0) {
            toast_timer--;
            int tw = text_width(font, 14.5f, toast_text) + 44;
            if (tw > SCR_W - 80) tw = SCR_W - 80;
            int bx = (SCR_W - tw) / 2;
            fill_round_rect_shadow(screen, bx, 18, tw, 40, 20, 38);
            fill_round_rect_aa(screen, bx, 18, tw, 40, 20, C_SURFACE);
            stroke_round_rect_aa(screen, bx, 18, tw, 40, 20, 1, C_WHEEL_RING, 160);
            draw_text_centered(screen, font, 14.5f, bx, bx + tw, 30, C_TXT, toast_text);
        }

        if (show_search_box) draw_search_box();
        else if (show_diag) draw_diag();
        else if (show_vol_panel) draw_vol_panel();
        else if (show_exit_confirm) draw_exit_confirm();

        if (fade_time > 0.0f) {
            float t_norm = fade_time / FADE_DUR;
            if (t_norm > 1.0f) t_norm = 1.0f;
            uint8_t a = (uint8_t)(t_norm * t_norm * t_norm * 255.0f);
        if (fade_mode == 1 && trans_shot && trans_shot->w == screen->w && trans_shot->h == screen->h) {
                blend_over_opaque(screen->px, trans_shot->px, (size_t)screen->w * screen->h, a);
            } else if (fade_mode == 2) {
                fill_rect_a(screen, 0, 0, SCR_W, SCR_H, C_BODY, (uint8_t)((a * 220u) >> 8));
            }
            if (fade_armed) {
                fade_armed = 0;
            } else {
                fade_time -= (float)g_dt;
                if (fade_time <= 0.0f) { fade_time = 0.0f; fade_mode = 0; }
            }
        }

        if (be_is_desktop() || ori == ORI_LAND) {
            int cw = screen->w < stride ? screen->w : stride;
            for (int y = 0; y < screen->h; y++) {
                uint32_t* dst = fb + (size_t)y * stride;
                uint32_t* src = screen->px + (size_t)y * screen->w;
                memcpy(dst, src, (size_t)cw * 4);
            }
        } else {
            uint32_t tile[16][32];
            for (int y0 = 0; y0 < 720; y0 += 16) {
                for (int x0 = 0; x0 < 1280; x0 += 32) {
                    for (int lx = 0; lx < 32; lx++) {
                        const uint32_t* srow = screen->px + (size_t)(x0 + lx) * 720 + (719 - y0 - 15);
                        for (int ly = 0; ly < 16; ly++) {
                            tile[15 - ly][lx] = srow[ly];
                        }
                    }
                    for (int ly = 0; ly < 16; ly++) {
                        memcpy(fb + (size_t)(y0 + ly) * stride + x0, tile[ly], 32 * 4);
                    }
                }
            }
        }
        be_end();
    }

    audio_exit();
    be_exit();
    if (font) font_free(font);
    if (screen) canvas_free(screen);
    if (lcd_ambient) canvas_free(lcd_ambient);
    if (wheel_cache) canvas_free(wheel_cache);
    if (cover_src) free(cover_src);
    if (cover_land) free(cover_land);
    if (cover_port) free(cover_port);
    if (cover_mini) free(cover_mini);
    if (logo_img) free(logo_img);
    if (poster_mini) free(poster_mini);
    if (poster_thm) free(poster_thm);
    if (poster_pod) free(poster_pod);
    if (trans_shot) canvas_free(trans_shot);
    meta_clear(&meta);
    if (lyr) lyrics_free(lyr);
    for (int i = 0; i < ntracks; i++) {
        free(tracks[i].thumb);
        free(tracks[i].thumb_pod);
        meta_clear(&tracks[i].meta);
    }
    free(tracks);
    return 0;
}
