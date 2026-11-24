#include "backend.h"
#include "audio.h"
#include "decode.h"
#include "meta.h"
#include "lrc.h"
#include "draw.h"
#include "util.h"
#include "plat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

extern const unsigned char font_ttf_start[];
extern const unsigned char font_ttf_end[];
extern const unsigned char poster_start[];
extern const unsigned char poster_end[];

#define MAX_TRACKS    4096
#define MAX_DIRS      12
#define MAX_THUMBS    240
#define THUMB_SZ      200
#define MINI_ART_SZ   54
#define COVER_SRC_SZ  360
#define ROW_H         54
#define RAIL_W        76
#define MAX_RIPPLES   8
#define MAX_GRID_HITS 96

typedef struct { int x, y, w, h; } Rect;

static inline int in_rect(int x, int y, int rx, int ry, int rw, int rh) {
    return rw > 0 && rh > 0 && x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}
static inline int in_rc(int x, int y, Rect r) { return in_rect(x, y, r.x, r.y, r.w, r.h); }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline uint8_t u8clamp(int v) { return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v); }

static uint32_t mix_col(uint32_t a, uint32_t b, float t) {
    t = clampf(t, 0.0f, 1.0f);
    uint32_t ar = a & 0xFF, ag = (a >> 8) & 0xFF, ab = (a >> 16) & 0xFF;
    uint32_t br = b & 0xFF, bg = (b >> 8) & 0xFF, bb = (b >> 16) & 0xFF;
    return COL(
        (uint8_t)(ar + (br - (float)ar) * t + 0.5f),
        (uint8_t)(ag + (bg - (float)ag) * t + 0.5f),
        (uint8_t)(ab + (bb - (float)ab) * t + 0.5f), 255);
}

// ---------------- Data Types ----------------
typedef struct {
    char path[600];
    char title[192];
    char artist[128];
    char album[128];
    char fmt[16];
    double dur;
    int dir_idx;
    int fav;
    int meta_state; // 0=none, 1=quick text/art, 2=duration probed
    uint32_t* thumb;
} Track;

typedef struct {
    int active;
    float x, y;
    float r, max_r;
    float alpha;
    uint32_t col;
} ClickRipple;

enum { PAGE_LIBRARY = 0, PAGE_LYRICS = 1 };
enum { TAB_ALL = 0, TAB_FAV = 1, TAB_DIR = 2 };
enum { FMT_FILT_ALL = 0, FMT_FILT_FAV = 1, FMT_FILT_LOSSLESS = 2, FMT_FILT_MP3 = 3, FMT_FILT_OTHER = 4 };
enum { MODE_LOOP_ALL = 0, MODE_LOOP_ONE = 1, MODE_SHUFFLE = 2 };
enum { PC_SORT_DEFAULT = 0, PC_SORT_TITLE = 1, PC_SORT_ARTIST = 2, PC_SORT_ALBUM = 3 };

// ---------------- App State ----------------
static Canvas* screen = NULL;
static Canvas* trans_shot = NULL;
static Canvas* bg_mesh = NULL;
static Font*   font = NULL;
static int W = 1360, H = 840;
static float g_ui_scale = 1.0f;

static Track* tracks = NULL;
static int    ntracks = 0;
static int*   vis_idx = NULL;
static int    nvisible = 0;

static char scan_dirs[MAX_DIRS][600];
static int  nscan_dirs = 0;
static int  active_dir = -1;

static int page = PAGE_LIBRARY;
static int lib_tab = TAB_ALL;
static int fmt_filter = FMT_FILT_ALL;
static int view_grid = 1; // 1 = M3 Giant Hero + Big Covers Grid (default), 0 = Compact Table
static int sort_mode = PC_SORT_DEFAULT;
static int show_diag = 0;
static int search_focused = 0;
static char search_q[160] = {0};

static int playing_idx = -1;
static int play_mode = MODE_LOOP_ALL;
static int g_is_light = 0;
static float g_saved_vol = 0.85f;

static TrackMeta meta;
static Lyrics* lyr = NULL;
static int g_lyr_req_id = 0;
static char p_title[256] = "Sirène";
static char p_artist[256] = "";
static char p_album[256] = "";
static char p_fmt[16] = "AUDIO";

static uint32_t* cover_src = NULL;
static uint32_t* cover_mini = NULL;
static uint32_t* poster_mini = NULL;
static uint32_t* poster_hero = NULL;
static uint32_t* poster_ico = NULL;
static int cover_ready = 0;

// Smooth color morphing + real-time water-ripple state
static float cur_qf[4][3] = {
    { 34, 88, 62 }, { 64, 128, 72 }, { 22, 48, 44 }, { 42, 92, 68 }
};
static float tgt_qf[4][3] = {
    { 34, 88, 62 }, { 64, 128, 72 }, { 22, 48, 44 }, { 42, 92, 68 }
};
static ClickRipple click_ripples[MAX_RIPPLES];
static uint32_t seed_rng = 0x9E3779B9u;

// Smooth animation / scrub / timing
static double g_dt = 1.0 / 60.0;
static uint64_t g_last_ns = 0;
static float fade_time = 0.0f;
static int   fade_armed = 0;
#define FADE_DUR 0.22f

static float scroll_px = 0.0f, scroll_target = 0.0f;
static float lyr_cam_y = 0.0f, lyr_cam_valid = 0;
static float lyr_manual_timer = 0.0f;
static float wave_phase = 0.0f, ripple_time = 0.0f, scallop_rot = 0.0f;
static float island_exp = 0.0f;
static float rail_pill_y = 92.0f;
static float bars[28] = {0};
static int   ui_cursor_hand = 0;
static int   probe_cursor = 0;

static int   hov_card_idx = -1;
static float hov_card_t = 0.0f;

static int scrub_drag = 0; // 1 = island prog, 2 = hero prog
static double scrub_sec = 0.0;
static int vol_drag = 0;
static int sb_drag = 0;
static int toast_timer = 0;
static char toast_text[160] = {0};

// Palette colors (dynamically tinted per playing song)
static uint32_t C_BASE, C_RAIL, C_SURFACE, C_GLASS, C_GLASS_HOV, C_GLASS_ACT;
static uint32_t C_ACCENT, C_ON_ACCENT, C_HERO_L, C_HERO_R;
static uint32_t C_TXT, C_TXT2, C_TXT3, C_DIV, C_TRACK_BG, C_HEART, C_PLAY_BTN, C_PLAY_ICO;

// Interactive hit rects updated each frame
static Rect rc_menu_btn, rc_nav_all, rc_nav_fav, rc_nav_lyr, rc_nav_add, rc_nav_diag;
static Rect rc_nav_theme, rc_nav_view, rc_nav_rescan;
static Rect rc_search_box, rc_search_clear, rc_sort_btn, rc_fs_btn, rc_view_pill, rc_pal_btn;
static Rect rc_filt_chips[5], rc_dir_chip;
static Rect rc_hero_card, rc_hero_cov, rc_hero_play, rc_hero_prog;
static Rect rc_sb_track, rc_fab_import;
static Rect rc_back_lib, rc_lyr_theme, rc_lyr_fs;
static Rect rc_island, rc_bar_cov, rc_bar_fav, rc_bar_prev, rc_bar_play, rc_bar_next;
static Rect rc_bar_mode, rc_bar_lyr, rc_bar_vol_ico, rc_bar_vol_trk, rc_bar_prog;
static int  ui_px0 = 0, ui_px1 = 100, ui_py = 0;
static int  ui_hpx0 = 0, ui_hpx1 = 100;
static int  ui_vx0 = 0, ui_vx1 = 100;
static int  ui_list_x0 = 0, ui_list_x1 = 0, ui_list_y0 = 0, ui_list_y1 = 0;

// Current layout metrics (grid geometry)
static int g_grid_cols = 4, g_grid_w = 1000, g_grid_gap = 18, g_card_w = 180, g_card_h = 240, g_hero_w = 378;

typedef struct { Rect card; Rect fav; int track_idx; } GridHit;
static GridHit grid_hits[MAX_GRID_HITS];
static int ngrid_hits = 0;

#define LYRIC_MAX_HITS 48
typedef struct { Rect rc; double t; } LyrHit;
static LyrHit lyr_hits[LYRIC_MAX_HITS];
static int nlyr_hits = 0;

static uint32_t next_rand(void) {
    seed_rng = seed_rng * 1664525u + 1013904223u;
    return seed_rng;
}

static void show_toast(const char* s) {
    snprintf(toast_text, sizeof(toast_text), "%s", s ? s : "");
    toast_timer = 130;
}

static void spawn_ripple(int x, int y, uint32_t col) {
    int slot = 0;
    float best_a = 999.0f;
    for (int i = 0; i < MAX_RIPPLES; i++) {
        if (!click_ripples[i].active) { slot = i; break; }
        if (click_ripples[i].alpha < best_a) { best_a = click_ripples[i].alpha; slot = i; }
    }
    click_ripples[slot].active = 1;
    click_ripples[slot].x = (float)x;
    click_ripples[slot].y = (float)y;
    click_ripples[slot].r = 12.0f;
    click_ripples[slot].max_r = (float)(W > H ? W : H) * 0.45f;
    click_ripples[slot].alpha = 140.0f;
    click_ripples[slot].col = col;
}

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
        fade_armed = 1;
    }
}

// ---------------- Path / Format / Utility Helpers ----------------
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

static int pc_ci_contains(const char* hay, const char* needle) {
    if (!needle || !*needle) return 1;
    if (!hay || !*hay) return 0;
    size_t nl = strlen(needle);
    for (const char* p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return 1;
    }
    return 0;
}

static void basename_str(const char* path, char* out, size_t cap) {
    const char* s = strrchr(path, '/');
    const char* bs = strrchr(path, '\\');
    if (bs && (!s || bs > s)) s = bs;
    const char* p = s ? (s + 1) : path;
    snprintf(out, cap, "%s", p[0] ? p : path);
}

static void ensure_dir_recursive(const char* path) {
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            plat_mkdir_p(tmp);
            *p = '/';
        }
    }
    plat_mkdir_p(tmp);
}

static void resample_cover_square(const uint32_t* src, int sw, int sh, uint32_t* dst, int dw, int crop) {
    if (!src || !dst || sw <= 0 || sh <= 0 || dw <= 0) return;
    int sx0 = 0, sy0 = 0, cw = sw, ch = sh;
    if (crop) {
        int m = sw < sh ? sw : sh;
        sx0 = (sw - m) / 2; sy0 = (sh - m) / 2;
        cw = ch = m;
    }
    uint32_t* tmp = (uint32_t*)malloc((size_t)cw * ch * 4);
    if (!tmp) return;
    for (int y = 0; y < ch; y++) {
        memcpy(tmp + (size_t)y * cw, src + (size_t)(sy0 + y) * sw + sx0, (size_t)cw * 4);
    }
    resample_bilinear(tmp, cw, ch, dst, dw, dw);
    free(tmp);
}

static void make_default_art(uint32_t* dst, int n, const char* key) {
    uint32_t h = 2166136261u;
    for (const unsigned char* p = (const unsigned char*)key; *p; p++) h = (h ^ *p) * 16777619u;
    float hue = (float)(h % 360) * (float)M_PI / 180.0f;
    uint8_t r0 = u8clamp((int)(88 + 52 * sinf(hue)));
    uint8_t g0 = u8clamp((int)(108 + 52 * sinf(hue + 2.1f)));
    uint8_t b0 = u8clamp((int)(134 + 52 * sinf(hue + 4.2f)));
    for (int y = 0; y < n; y++) {
        float fy = (float)y / (float)n;
        for (int x = 0; x < n; x++) {
            float fx = (float)x / (float)n;
            uint8_t r = u8clamp((int)(r0 * (1.0f - 0.30f * fy) + 28 * fx));
            uint8_t g = u8clamp((int)(g0 * (1.0f - 0.28f * fy) + 22 * fx));
            uint8_t b = u8clamp((int)(b0 * (1.0f - 0.20f * fy) + 36 * fx));
            dst[(size_t)y * n + x] = COL(r, g, b, 255);
        }
    }
}

// ---------------- Water-Ripple & Color-Morph Adaptive Background ----------------
static void update_target_cover_colors(void) {
    if (cover_ready && cover_src) {
        for (int q = 0; q < 4; q++) {
            int qx = (q & 1) ? COVER_SRC_SZ / 2 : 0;
            int qy = (q & 2) ? COVER_SRC_SZ / 2 : 0;
            uint64_t sr = 0, sg = 0, sb = 0, cnt = 0;
            for (int y = qy + 20; y < qy + COVER_SRC_SZ / 2 - 20; y += 8) {
                for (int x = qx + 20; x < qx + COVER_SRC_SZ / 2 - 20; x += 8) {
                    uint32_t c = cover_src[(size_t)y * COVER_SRC_SZ + x];
                    sr += c & 0xFF; sg += (c >> 8) & 0xFF; sb += (c >> 16) & 0xFF; cnt++;
                }
            }
            if (cnt > 0) {
                tgt_qf[q][0] = (float)(sr / cnt);
                tgt_qf[q][1] = (float)(sg / cnt);
                tgt_qf[q][2] = (float)(sb / cnt);
            }
        }
    } else {
        float def[4][3] = { { 42, 118, 86 }, { 76, 152, 92 }, { 24, 62, 58 }, { 48, 108, 82 } };
        memcpy(tgt_qf, def, sizeof(def));
    }
}

static inline uint8_t col_a(uint32_t col) { return (uint8_t)((col >> 24) & 0xFFu); }
static inline uint32_t with_alpha(uint32_t col, uint8_t a) { return (col & 0x00FFFFFFu) | ((uint32_t)a << 24); }

static uint32_t clamp_mesh_lum(uint32_t col, int is_light) {
    float r = (float)(col & 0xFF), g = (float)((col >> 8) & 0xFF), b = (float)((col >> 16) & 0xFF);
    float lum = 0.299f * r + 0.587f * g + 0.114f * b;
    if (!is_light && lum > 132.0f) {
        float sc = 132.0f / lum;
        return COL((uint8_t)(r * sc), (uint8_t)(g * sc), (uint8_t)(b * sc), 255);
    }
    if (is_light && lum < 175.0f && lum > 1.0f) {
        return mix_col(col, COL(236, 244, 255, 255), 0.55f);
    }
    return col;
}

static uint32_t boost_pastel_accent(float r, float g, float b, int is_light) {
    float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    if (mx - mn < 18.0f) {
        // Fallback pastel periwinkle-aqua for monochrome/neutral artwork
        return is_light ? COL(62, 110, 210, 255) : COL(132, 186, 255, 255);
    }
    float avg = (r + g + b) / 3.0f;
    r = clampf(avg + (r - avg) * 1.68f, 0, 255);
    g = clampf(avg + (g - avg) * 1.68f, 0, 255);
    b = clampf(avg + (b - avg) * 1.68f, 0, 255);
    float peak = r > g ? (r > b ? r : b) : (g > b ? g : b);
    if (!is_light) {
        if (peak > 1.0f && peak < 218.0f) {
            float sc = 224.0f / peak;
            if (sc > 2.2f) sc = 2.2f;
            r = clampf(r * sc, 0, 255);
            g = clampf(g * sc, 0, 255);
            b = clampf(b * sc, 0, 255);
        }
        uint32_t col = COL((uint8_t)r, (uint8_t)g, (uint8_t)b, 255);
        return mix_col(col, COL(245, 248, 255, 255), 0.16f);
    } else {
        if (peak > 165.0f) {
            float sc = 155.0f / peak;
            r *= sc; g *= sc; b *= sc;
        }
        return COL((uint8_t)r, (uint8_t)g, (uint8_t)b, 255);
    }
}

static void step_palette_and_water_ripples(void) {
    float k = clampf((float)g_dt * 3.8f, 0.02f, 1.0f);
    for (int q = 0; q < 4; q++) {
        for (int c = 0; c < 3; c++) cur_qf[q][c] += (tgt_qf[q][c] - cur_qf[q][c]) * k;
    }

    uint32_t qcol[4];
    int best_q = 0;
    float best_chroma = -1.0f;
    for (int q = 0; q < 4; q++) {
        float r = cur_qf[q][0], g = cur_qf[q][1], b = cur_qf[q][2];
        float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
        float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
        float lum = (r + g + b) / 3.0f;
        float score = (mx - mn) * (lum > 38.0f && lum < 228.0f ? 1.0f : 0.45f);
        if (score > best_chroma) { best_chroma = score; best_q = q; }
        qcol[q] = clamp_mesh_lum(COL((uint8_t)r, (uint8_t)g, (uint8_t)b, 255), g_is_light);
    }

    float acc_r = cur_qf[best_q][0], acc_g = cur_qf[best_q][1], acc_b = cur_qf[best_q][2];
    if (best_chroma < 14.0f) {
        acc_r = (cur_qf[0][0] + cur_qf[1][0] + cur_qf[2][0] + cur_qf[3][0]) * 0.25f;
        acc_g = (cur_qf[0][1] + cur_qf[1][1] + cur_qf[2][1] + cur_qf[3][1]) * 0.25f;
        acc_b = (cur_qf[0][2] + cur_qf[1][2] + cur_qf[2][2] + cur_qf[3][2]) * 0.25f;
    }

    C_ACCENT = boost_pastel_accent(acc_r, acc_g, acc_b, g_is_light);
    if (!g_is_light) {
        C_BASE       = COL(12, 14, 18, 255);
        C_RAIL       = with_alpha(mix_col(COL(14, 17, 23, 255), qcol[2], 0.18f), 168);
        C_SURFACE    = with_alpha(mix_col(COL(22, 26, 34, 255), qcol[1], 0.16f), 152);
        C_GLASS      = COL(255, 255, 255, 22);
        C_GLASS_HOV  = COL(255, 255, 255, 38);
        C_GLASS_ACT  = COL(255, 255, 255, 56);
        C_ON_ACCENT  = COL(14, 18, 24, 255);
        C_HERO_L     = mix_col(COL(18, 22, 28, 255), qcol[0], 0.32f);
        C_HERO_R     = mix_col(COL(22, 28, 36, 255), C_ACCENT, 0.36f);
        C_TXT        = COL(246, 248, 252, 255);
        C_TXT2       = COL(184, 190, 204, 255);
        C_TXT3       = COL(128, 136, 154, 255);
        C_DIV        = COL(255, 255, 255, 28);
        C_TRACK_BG   = COL(255, 255, 255, 42);
        C_HEART      = COL(248, 82, 104, 255);
        C_PLAY_BTN   = COL(248, 250, 252, 255);
        C_PLAY_ICO   = COL(14, 18, 22, 255);
    } else {
        C_BASE       = COL(242, 246, 250, 255);
        C_RAIL       = with_alpha(mix_col(COL(248, 250, 254, 255), qcol[2], 0.10f), 200);
        C_SURFACE    = with_alpha(mix_col(COL(255, 255, 255, 255), qcol[1], 0.08f), 184);
        C_GLASS      = COL(0, 0, 0, 14);
        C_GLASS_HOV  = COL(0, 0, 0, 26);
        C_GLASS_ACT  = COL(0, 0, 0, 38);
        C_ON_ACCENT  = COL(255, 255, 255, 255);
        C_HERO_L     = mix_col(COL(255, 255, 255, 255), C_ACCENT, 0.16f);
        C_HERO_R     = mix_col(COL(244, 248, 254, 255), C_ACCENT, 0.32f);
        C_TXT        = COL(18, 22, 32, 255);
        C_TXT2       = COL(88, 96, 114, 255);
        C_TXT3       = COL(138, 146, 164, 255);
        C_DIV        = COL(0, 0, 0, 24);
        C_TRACK_BG   = COL(0, 0, 0, 32);
        C_HEART      = COL(228, 60, 82, 255);
        C_PLAY_BTN   = C_ACCENT;
        C_PLAY_ICO   = COL(255, 255, 255, 255);
    }
    be_set_dark_titlebar(!g_is_light);

    for (int i = 0; i < MAX_RIPPLES; i++) {
        if (!click_ripples[i].active) continue;
        click_ripples[i].r += (float)g_dt * 340.0f;
        click_ripples[i].alpha -= (float)g_dt * 125.0f;
        if (click_ripples[i].alpha <= 1.0f || click_ripples[i].r >= click_ripples[i].max_r) {
            click_ripples[i].active = 0;
        }
    }
}

static void draw_water_ripple_bg(int is_immersive) {
    if (!screen) return;
    if (!bg_mesh) bg_mesh = canvas_new(144, 81);
    if (!bg_mesh) return;

    uint32_t q0 = clamp_mesh_lum(COL((uint8_t)cur_qf[0][0], (uint8_t)cur_qf[0][1], (uint8_t)cur_qf[0][2], 255), g_is_light);
    uint32_t q1 = clamp_mesh_lum(COL((uint8_t)cur_qf[1][0], (uint8_t)cur_qf[1][1], (uint8_t)cur_qf[1][2], 255), g_is_light);
    uint32_t q2 = clamp_mesh_lum(COL((uint8_t)cur_qf[2][0], (uint8_t)cur_qf[2][1], (uint8_t)cur_qf[2][2], 255), g_is_light);
    uint32_t q3 = clamp_mesh_lum(COL((uint8_t)cur_qf[3][0], (uint8_t)cur_qf[3][1], (uint8_t)cur_qf[3][2], 255), g_is_light);

    q1 = clamp_mesh_lum(mix_col(q1, C_ACCENT, 0.38f), g_is_light);
    q2 = mix_col(q2, C_HERO_L, 0.25f);

    float bass = (bars[0] + bars[1] + bars[2] + bars[3]) * 0.25f;
    float base_tint = is_immersive ? (g_is_light ? 0.36f : 0.62f) : (g_is_light ? 0.18f : 0.42f);
    uint32_t bg_floor = g_is_light ? COL(244, 246, 251, 255) : COL(11, 13, 18, 255);

    float e1x = 0.26f + 0.05f * cosf(ripple_time * 0.48f);
    float e1y = 0.34f + 0.05f * sinf(ripple_time * 0.41f);
    float e2x = 0.74f + 0.06f * sinf(ripple_time * 0.35f);
    float e2y = 0.66f + 0.05f * cosf(ripple_time * 0.43f);
    float e3x = 0.52f + 0.08f * sinf(ripple_time * 0.27f);
    float e3y = 0.22f + 0.04f * cosf(ripple_time * 0.33f);

    for (int y = 0; y < 81; y++) {
        float fy = (float)y / 80.0f;
        for (int x = 0; x < 144; x++) {
            float fx = (float)x / 143.0f;
            float d1 = sqrtf((fx - e1x) * (fx - e1x) * 1.45f + (fy - e1y) * (fy - e1y) * 0.55f);
            float d2 = sqrtf((fx - e2x) * (fx - e2x) * 1.45f + (fy - e2y) * (fy - e2y) * 0.55f);
            float d3 = sqrtf((fx - e3x) * (fx - e3x) * 1.25f + (fy - e3y) * (fy - e3y) * 0.65f);
            float w1 = sinf(d1 * 24.0f - ripple_time * 2.4f) * expf(-d1 * 1.30f);
            float w2 = sinf(d2 * 19.0f - ripple_time * 1.9f) * expf(-d2 * 1.20f);
            float w3 = sinf(d3 * 28.0f - ripple_time * 2.1f) * expf(-d3 * 1.55f);
            float wave = (w1 + w2 + w3 * 0.7f) * 0.44f;

            float sx = clampf(fx + wave * 0.14f, 0.0f, 1.0f);
            float sy = clampf(fy - wave * 0.14f, 0.0f, 1.0f);
            uint32_t top = mix_col(q0, q1, sx);
            uint32_t bot = mix_col(q2, q3, sx);
            uint32_t c_mesh = mix_col(top, bot, sy);

            float caustic = wave * (0.24f + bass * 0.16f);
            float t_mix = clampf(base_tint + caustic, 0.10f, 0.86f);
            uint32_t px = mix_col(bg_floor, c_mesh, t_mix);
            if (wave > 0.08f) {
                float shimmer = clampf((wave - 0.08f) * (is_immersive ? 0.30f : 0.24f), 0.0f, 0.38f);
                px = mix_col(px, C_ACCENT, shimmer);
            }
            bg_mesh->px[(size_t)y * 144 + x] = px;
        }
    }

    resample_bilinear(bg_mesh->px, 144, 81, screen->px, screen->w, screen->h);

    stroke_circle_aa(screen, (int)(e1x * W), (int)(e1y * H), 140 + (int)(20.0f * sinf(ripple_time)), 2, C_ACCENT, is_immersive ? 24 : 16);
    stroke_circle_aa(screen, (int)(e2x * W), (int)(e2y * H), 190 + (int)(26.0f * cosf(ripple_time * 0.8f)), 2, q1, is_immersive ? 24 : 18);

    for (int i = 0; i < MAX_RIPPLES; i++) {
        if (!click_ripples[i].active) continue;
        uint8_t a1 = (uint8_t)clampf(click_ripples[i].alpha, 0, 255);
        uint8_t a2 = (uint8_t)(a1 * 0.45f);
        stroke_circle_aa(screen, (int)click_ripples[i].x, (int)click_ripples[i].y,
                         (int)click_ripples[i].r, 3, click_ripples[i].col, a1);
        if (click_ripples[i].r > 32.0f) {
            stroke_circle_aa(screen, (int)click_ripples[i].x, (int)click_ripples[i].y,
                             (int)(click_ripples[i].r - 28.0f), 2, C_ACCENT, a2);
        }
    }
}

// ---------------- Persistence (Favorites, Dirs, Settings) ----------------
static void load_settings(void) {
    char p[600];
    snprintf(p, sizeof(p), "%s/pc_settings.cfg", be_config_dir());
    FILE* f = plat_fopen(p, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "vol=", 4) == 0) {
            float v = (float)atof(line + 4);
            if (v >= 0.0f && v <= 1.0f) { audio_set_volume(v); g_saved_vol = v > 0.01f ? v : 0.85f; }
        } else if (strncmp(line, "mode=", 5) == 0) {
            int m = atoi(line + 5);
            if (m >= 0 && m <= 2) play_mode = m;
        } else if (strncmp(line, "light=", 6) == 0) {
            g_is_light = atoi(line + 6) ? 1 : 0;
        } else if (strncmp(line, "view_grid=", 10) == 0) {
            view_grid = atoi(line + 10) ? 1 : 0;
        } else if (strncmp(line, "sort=", 5) == 0) {
            int sm = atoi(line + 5);
            if (sm >= 0 && sm <= 3) sort_mode = sm;
        }
    }
    fclose(f);
}

static void save_settings(void) {
    ensure_dir_recursive(be_config_dir());
    char p[600];
    snprintf(p, sizeof(p), "%s/pc_settings.cfg", be_config_dir());
    FILE* f = plat_fopen(p, "w");
    if (!f) return;
    fprintf(f, "vol=%.3f\nmode=%d\nlight=%d\nview_grid=%d\nsort=%d\n",
            audio_volume(), play_mode, g_is_light, view_grid, sort_mode);
    fclose(f);
}

static void load_dirs_cfg(void) {
    nscan_dirs = 0;
    char p[600];
    snprintf(p, sizeof(p), "%s/music_dirs.cfg", be_config_dir());
    FILE* f = plat_fopen(p, "r");
    if (f) {
        char buf[600];
        while (nscan_dirs < MAX_DIRS && fgets(buf, sizeof(buf), f)) {
            size_t n = strlen(buf);
            while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
            if (buf[0]) snprintf(scan_dirs[nscan_dirs++], 600, "%s", buf);
        }
        fclose(f);
    }
    if (nscan_dirs == 0) {
        snprintf(p, sizeof(p), "%s/music_dir.cfg", be_config_dir());
        FILE* f2 = plat_fopen(p, "r");
        if (f2) {
            char buf[600] = {0};
            if (fgets(buf, sizeof(buf), f2)) {
                size_t n = strlen(buf);
                while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
                if (buf[0]) snprintf(scan_dirs[nscan_dirs++], 600, "%s", buf);
            }
            fclose(f2);
        }
    }
    if (nscan_dirs == 0) {
        char defs[4][600];
        int nd = be_default_music_dirs(defs, 4);
        for (int i = 0; i < nd && nscan_dirs < MAX_DIRS; i++) {
            int is_d = 0, is_r = 0;
            if (plat_stat_type(defs[i], &is_d, &is_r) == 0 && is_d) {
                snprintf(scan_dirs[nscan_dirs++], 600, "%s", defs[i]);
                break;
            }
        }
    }
}

static void save_dirs_cfg(void) {
    ensure_dir_recursive(be_config_dir());
    char p[600];
    snprintf(p, sizeof(p), "%s/music_dirs.cfg", be_config_dir());
    FILE* f = plat_fopen(p, "w");
    if (!f) return;
    for (int i = 0; i < nscan_dirs; i++) fprintf(f, "%s\n", scan_dirs[i]);
    fclose(f);
}

static int add_scan_dir(const char* dir) {
    if (!dir || !*dir) return -1;
    char norm[600];
    snprintf(norm, sizeof(norm), "%s", dir);
    for (char* q = norm; *q; q++) if (*q == '\\') *q = '/';
    size_t len = strlen(norm);
    while (len > 1 && norm[len - 1] == '/') norm[--len] = 0;
    for (int i = 0; i < nscan_dirs; i++) if (strcasecmp(scan_dirs[i], norm) == 0) return i;
    if (nscan_dirs >= MAX_DIRS) return nscan_dirs - 1;
    snprintf(scan_dirs[nscan_dirs], 600, "%s", norm);
    save_dirs_cfg();
    return nscan_dirs++;
}

static void load_favorites(void) {
    char p[600];
    snprintf(p, sizeof(p), "%s/favorites.cfg", be_config_dir());
    FILE* f = plat_fopen(p, "r");
    if (!f) return;
    char buf[600];
    while (fgets(buf, sizeof(buf), f)) {
        size_t n = strlen(buf);
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
        if (!buf[0]) continue;
        for (int i = 0; i < ntracks; i++) if (strcmp(tracks[i].path, buf) == 0) { tracks[i].fav = 1; break; }
    }
    fclose(f);
}

static void save_favorites(void) {
    ensure_dir_recursive(be_config_dir());
    char p[600];
    snprintf(p, sizeof(p), "%s/favorites.cfg", be_config_dir());
    FILE* f = plat_fopen(p, "w");
    if (!f) return;
    for (int i = 0; i < ntracks; i++) if (tracks[i].fav) fprintf(f, "%s\n", tracks[i].path);
    fclose(f);
}

// ---------------- Library Scan & Metadata ----------------
static int add_track_path(const char* path, int dir_idx) {
    if (!path || !*path || ntracks >= MAX_TRACKS) return -1;
    for (int i = 0; i < ntracks; i++) if (strcmp(tracks[i].path, path) == 0) return i;
    Track* tr = &tracks[ntracks];
    memset(tr, 0, sizeof(*tr));
    snprintf(tr->path, sizeof(tr->path), "%s", path);
    basename_no_ext(path, tr->title, sizeof(tr->title));
    track_format_tag(path, tr->fmt, sizeof(tr->fmt));
    tr->dir_idx = dir_idx;
    return ntracks++;
}

static void scan_dir_rec(const char* dir, int dir_idx, int depth) {
    if (depth > 5 || ntracks >= MAX_TRACKS) return;
    PlatDir* d = plat_opendir(dir);
    if (!d) return;
    PlatDirent e;
    char sub[640];
    while (plat_readdir(d, &e)) {
        if (e.d_name[0] == '.') continue;
        size_t dl = strlen(dir);
        if (dl > 0 && dir[dl - 1] == '/') snprintf(sub, sizeof(sub), "%s%s", dir, e.d_name);
        else snprintf(sub, sizeof(sub), "%s/%s", dir, e.d_name);
        int is_d = e.is_dir, is_r = e.is_reg;
        if (!is_d && !is_r) plat_stat_type(sub, &is_d, &is_r);
        if (is_d) scan_dir_rec(sub, dir_idx, depth + 1);
        else if (is_r && dec_is_audio(e.d_name)) add_track_path(sub, dir_idx);
    }
    plat_closedir(d);
}

static int cmp_tracks_by_mode(const void* a, const void* b) {
    int ia = *(const int*)a, ib = *(const int*)b;
    const Track *ta = &tracks[ia], *tb = &tracks[ib];
    if (sort_mode == PC_SORT_TITLE) return strcasecmp(ta->title, tb->title);
    if (sort_mode == PC_SORT_ARTIST) {
        int r = strcasecmp(ta->artist, tb->artist);
        return r ? r : strcasecmp(ta->title, tb->title);
    }
    if (sort_mode == PC_SORT_ALBUM) {
        int r = strcasecmp(ta->album, tb->album);
        return r ? r : strcasecmp(ta->title, tb->title);
    }
    return strcasecmp(ta->path, tb->path);
}

static int match_fmt_filter(const Track* tr) {
    if (fmt_filter == FMT_FILT_ALL) return 1;
    if (fmt_filter == FMT_FILT_FAV) return tr->fav;
    if (fmt_filter == FMT_FILT_LOSSLESS) {
        return strcmp(tr->fmt, "FLAC") == 0 || strcmp(tr->fmt, "WAV") == 0 ||
               strcmp(tr->fmt, "AIFF") == 0 || strcmp(tr->fmt, "APE") == 0 || strcmp(tr->fmt, "WV") == 0;
    }
    if (fmt_filter == FMT_FILT_MP3) return strcmp(tr->fmt, "MP3") == 0;
    return strcmp(tr->fmt, "MP3") != 0 && strcmp(tr->fmt, "FLAC") != 0 &&
           strcmp(tr->fmt, "WAV") != 0 && strcmp(tr->fmt, "AIFF") != 0;
}

static void rebuild_visible(void) {
    nvisible = 0;
    for (int i = 0; i < ntracks; i++) {
        if (lib_tab == TAB_FAV && !tracks[i].fav) continue;
        if (lib_tab == TAB_DIR && active_dir >= 0 && tracks[i].dir_idx != active_dir) continue;
        if (!match_fmt_filter(&tracks[i])) continue;
        if (search_q[0]) {
            if (!pc_ci_contains(tracks[i].title, search_q) &&
                !pc_ci_contains(tracks[i].artist, search_q) &&
                !pc_ci_contains(tracks[i].album, search_q) &&
                !pc_ci_contains(tracks[i].path, search_q)) continue;
        }
        vis_idx[nvisible++] = i;
    }
    if (nvisible > 1) qsort(vis_idx, (size_t)nvisible, sizeof(int), cmp_tracks_by_mode);
}

static void free_all_thumbs(void) {
    for (int i = 0; i < ntracks; i++) {
        free(tracks[i].thumb); tracks[i].thumb = NULL; tracks[i].meta_state = 0;
    }
}

static void scan_library(int notify) {
    char prev_play[600] = {0};
    if (playing_idx >= 0 && playing_idx < ntracks) snprintf(prev_play, sizeof(prev_play), "%s", tracks[playing_idx].path);
    free_all_thumbs();
    ntracks = 0;
    for (int i = 0; i < nscan_dirs; i++) scan_dir_rec(scan_dirs[i], i, 0);
    load_favorites();
    playing_idx = -1;
    if (prev_play[0]) {
        for (int i = 0; i < ntracks; i++) if (strcmp(tracks[i].path, prev_play) == 0) { playing_idx = i; break; }
    }
    rebuild_visible();
    probe_cursor = 0;
    scroll_px = scroll_target = 0.0f;
    if (notify) {
        char m[96];
        snprintf(m, sizeof(m), "扫描完成：共 %d 首音频", ntracks);
        show_toast(m);
    }
}

static void ensure_track_meta(int idx, int probe_dur) {
    if (idx < 0 || idx >= ntracks) return;
    Track* tr = &tracks[idx];
    if (tr->meta_state == 0) {
        TrackMeta tm;
        if (meta_load_quick(tr->path, &tm)) {
            if (tm.title[0])  snprintf(tr->title, sizeof(tr->title), "%s", tm.title);
            if (tm.artist[0]) snprintf(tr->artist, sizeof(tr->artist), "%s", tm.artist);
            if (tm.album[0])  snprintf(tr->album, sizeof(tr->album), "%s", tm.album);
            if (tm.art && tm.art_w > 8 && tm.art_h > 8) {
                tr->thumb = (uint32_t*)malloc((size_t)THUMB_SZ * THUMB_SZ * 4);
                if (tr->thumb) resample_cover_square((const uint32_t*)tm.art, tm.art_w, tm.art_h, tr->thumb, THUMB_SZ, 1);
            }
            meta_clear(&tm);
        }
        tr->meta_state = 1;
    }
    if (probe_dur && tr->meta_state < 2) {
        double d = 0;
        if (dec_probe_info(tr->path, &d, NULL, NULL) == 0 && d > 0) tr->dur = d;
        tr->meta_state = 2;
    }
}

static void trim_thumbs(int center_pos) {
    int cnt = 0;
    for (int i = 0; i < ntracks; i++) if (tracks[i].thumb) cnt++;
    if (cnt <= MAX_THUMBS) return;
    int far_i = -1, max_dist = -1;
    for (int p = 0; p < nvisible; p++) {
        int idx = vis_idx[p];
        if (idx == playing_idx || !tracks[idx].thumb) continue;
        int d = abs(p - center_pos);
        if (d > max_dist) { max_dist = d; far_i = idx; }
    }
    if (far_i >= 0) { free(tracks[far_i].thumb); tracks[far_i].thumb = NULL; }
}

static void build_posters(void) {
    if ((size_t)(poster_end - poster_start) >= 320 * 320 * 4) {
        poster_mini = (uint32_t*)calloc(MINI_ART_SZ * MINI_ART_SZ, 4);
        poster_hero = (uint32_t*)calloc((size_t)COVER_SRC_SZ * COVER_SRC_SZ, 4);
        poster_ico  = (uint32_t*)calloc(40 * 40, 4);
        if (poster_mini) resample_cover_square((const uint32_t*)poster_start, 320, 320, poster_mini, MINI_ART_SZ, 1);
        if (poster_hero) resample_cover_square((const uint32_t*)poster_start, 320, 320, poster_hero, COVER_SRC_SZ, 1);
        if (poster_ico)  resample_cover_square((const uint32_t*)poster_start, 320, 320, poster_ico, 40, 1);
    }
}

// ---------------- Vector Icons ----------------
static void icon_heart_ex(Canvas* c, int x, int y, int sz, uint32_t col, int filled) {
    if (filled) {
        float r = sz * 0.25f;
        int r1 = (int)(r + 0.5f);
        fill_ellipse_aa(c, x + (int)(sz * 0.28f), y + (int)(sz * 0.34f), r1, r1, col);
        fill_ellipse_aa(c, x + (int)(sz * 0.72f), y + (int)(sz * 0.34f), r1, r1, col);
        fill_tri_aa(c, x + (int)(sz * 0.05f), y + (int)(sz * 0.42f),
                       x + (int)(sz * 0.95f), y + (int)(sz * 0.42f),
                       x + sz / 2, y + (int)(sz * 0.92f), col);
        fill_tri_aa(c, x + (int)(sz * 0.26f), y + (int)(sz * 0.32f),
                       x + (int)(sz * 0.74f), y + (int)(sz * 0.32f),
                       x + sz / 2, y + (int)(sz * 0.60f), col);
    } else {
        float hs = (float)sz * 0.44f;
        float cx = (float)x + (float)sz * 0.50f;
        float cy = (float)y + (float)sz * 0.54f;
        for (int j = y - 1; j <= y + sz + 1; j++) {
            float py = -(j + 0.5f - cy) / hs;
            for (int i = x - 1; i <= x + sz + 1; i++) {
                float px = (i + 0.5f - cx) / hs;
                float py_adj = py - 0.55f * sqrtf(fabsf(px));
                float d = (px * px + py_adj * py_adj - 1.0f);
                if (fabsf(d) < 0.30f) {
                    float a = 1.0f - fabsf(d) / 0.30f;
                    fill_rect_a(c, i, j, 1, 1, col, (uint8_t)(a * 215.0f));
                }
            }
        }
    }
}
static void icon_play(Canvas* c, int cx, int cy, int r, uint32_t col) {
    fill_tri_aa(c, cx - (int)(r * 0.38f), cy - (int)(r * 0.54f),
                   cx - (int)(r * 0.38f), cy + (int)(r * 0.54f),
                   cx + (int)(r * 0.62f), cy, col);
}
static void icon_pause(Canvas* c, int cx, int cy, int r, uint32_t col) {
    int bw = r / 3; if (bw < 3) bw = 3;
    int bh = (int)(r * 1.15f);
    fill_round_rect_aa(c, cx - bw - 2, cy - bh / 2, bw, bh, 2, col);
    fill_round_rect_aa(c, cx + 2,      cy - bh / 2, bw, bh, 2, col);
}
static void icon_prev(Canvas* c, int cx, int cy, int r, uint32_t col) {
    int bar = r / 4; if (bar < 2) bar = 2;
    fill_round_rect_aa(c, cx - (int)(r * 0.60f), cy - (int)(r * 0.50f), bar, r, 1, col);
    fill_tri_aa(c, cx + (int)(r * 0.52f), cy - (int)(r * 0.50f),
                   cx + (int)(r * 0.52f), cy + (int)(r * 0.50f),
                   cx - (int)(r * 0.28f), cy, col);
}
static void icon_next(Canvas* c, int cx, int cy, int r, uint32_t col) {
    int bar = r / 4; if (bar < 2) bar = 2;
    fill_tri_aa(c, cx - (int)(r * 0.52f), cy - (int)(r * 0.50f),
                   cx - (int)(r * 0.52f), cy + (int)(r * 0.50f),
                   cx + (int)(r * 0.28f), cy, col);
    fill_round_rect_aa(c, cx + (int)(r * 0.40f), cy - (int)(r * 0.50f), bar, r, 1, col);
}
static void icon_menu(Canvas* c, int x, int y, int sz, uint32_t col) {
    for (int i = 0; i < 3; i++) fill_round_rect_aa(c, x, y + i * (sz / 3), sz, 2, 1, col);
}
static void icon_search(Canvas* c, int cx, int cy, int r, uint32_t col) {
    stroke_circle_aa(c, cx - 1, cy - 1, r, 2, col, 210);
    for (int i = 0; i < 5; i++) fill_round_rect_aa(c, cx + r / 2 + i, cy + r / 2 + i, 2, 2, 1, col);
}
static void icon_vol(Canvas* c, int cx, int cy, int sz, uint32_t col, int muted) {
    fill_round_rect_aa(c, cx - sz / 2, cy - sz / 5, sz / 4 + 1, (sz * 2) / 5 + 1, 1, col);
    fill_tri_aa(c, cx - sz / 3, cy, cx + 1, cy - sz / 2 + 1, cx + 1, cy + sz / 2 - 1, col);
    if (!muted) stroke_circle_aa(c, cx + 2, cy, sz / 3, 2, col, 185);
    else fill_round_rect_aa(c, cx + 3, cy - 1, sz / 2 - 2, 2, 1, C_HEART);
}
static void icon_expand(Canvas* c, int x, int y, int sz, uint32_t col) {
    fill_round_rect_aa(c, x, y, sz / 2, 2, 1, col);
    fill_round_rect_aa(c, x, y, 2, sz / 2, 1, col);
    fill_round_rect_aa(c, x + sz - sz / 2, y + sz - 2, sz / 2, 2, 1, col);
    fill_round_rect_aa(c, x + sz - 2, y + sz - sz / 2, 2, sz / 2, 1, col);
}
static void icon_back(Canvas* c, int x, int y, int sz, uint32_t col) {
    int my = y + sz / 2;
    fill_round_rect_aa(c, x + 2, my - 1, sz - 3, 2, 1, col);
    fill_tri_aa(c, x, my, x + sz / 2, y + 2, x + sz / 2, y + sz - 2, col);
}

static void toggle_fav(int idx) {
    if (idx < 0 || idx >= ntracks) return;
    tracks[idx].fav = !tracks[idx].fav;
    save_favorites();
    if (lib_tab == TAB_FAV || fmt_filter == FMT_FILT_FAV) rebuild_visible();
}

static void load_track(int idx) {
    if (idx < 0 || idx >= ntracks) return;
    scrub_drag = 0; scrub_sec = 0.0;
    lyr_cam_valid = 0; lyr_manual_timer = 0.0f;
    playing_idx = idx;
    audio_play(tracks[idx].path);

    ensure_track_meta(idx, 1);
    meta_clear(&meta);
    if (lyr) { lyrics_free(lyr); lyr = NULL; }
    meta_load(tracks[idx].path, &meta);

    if (meta.title[0]) snprintf(p_title, sizeof(p_title), "%s", meta.title);
    else basename_no_ext(tracks[idx].path, p_title, sizeof(p_title));
    if (meta.artist[0]) snprintf(p_artist, sizeof(p_artist), "%s", meta.artist);
    else p_artist[0] = 0;
    if (meta.album[0]) snprintf(p_album, sizeof(p_album), "%s", meta.album);
    else p_album[0] = 0;
    track_format_tag(tracks[idx].path, p_fmt, sizeof(p_fmt));

    if (!cover_src) cover_src = (uint32_t*)calloc((size_t)COVER_SRC_SZ * COVER_SRC_SZ, 4);
    if (!cover_mini) cover_mini = (uint32_t*)calloc(MINI_ART_SZ * MINI_ART_SZ, 4);
    if (cover_src) {
        if (meta.art && meta.art_w > 8 && meta.art_h > 8) {
            resample_cover_square((const uint32_t*)meta.art, meta.art_w, meta.art_h, cover_src, COVER_SRC_SZ, 1);
        } else if (poster_hero) {
            memcpy(cover_src, poster_hero, (size_t)COVER_SRC_SZ * COVER_SRC_SZ * 4);
        } else {
            make_default_art(cover_src, COVER_SRC_SZ, tracks[idx].path);
        }
        if (cover_mini) resample_cover_square(cover_src, COVER_SRC_SZ, COVER_SRC_SZ, cover_mini, MINI_ART_SZ, 1);
        cover_ready = 1;
        update_target_cover_colors();
        spawn_ripple(W / 2, H / 2, C_ACCENT);
    }

    g_lyr_req_id = 0;
    lyr = lyrics_load_ex(tracks[idx].path, p_title, p_artist);
    if (!lyr) {
        g_lyr_req_id = lyrics_start_online(tracks[idx].path, p_title, p_artist,
                                           tracks[idx].dur > 1.0 ? tracks[idx].dur : audio_duration());
    }
    memset(bars, 0, sizeof(bars));
}

static int pick_next(int cur, int manual) {
    if (nvisible > 0) {
        if (!manual && play_mode == MODE_LOOP_ONE && cur >= 0) return cur;
        if (play_mode == MODE_SHUFFLE && nvisible > 1) {
            for (int tries = 0; tries < 8; tries++) {
                int idx = vis_idx[(int)(next_rand() % (uint32_t)nvisible)];
                if (idx != cur) return idx;
            }
        }
        for (int i = 0; i < nvisible; i++) if (vis_idx[i] == cur) return vis_idx[(i + 1) % nvisible];
        return vis_idx[0];
    }
    if (ntracks <= 1) return 0;
    return (cur + 1) % ntracks;
}

static int pick_prev(int cur) {
    if (nvisible > 0) {
        if (play_mode == MODE_SHUFFLE && nvisible > 1) {
            int idx = vis_idx[(int)(next_rand() % (uint32_t)nvisible)];
            if (idx != cur) return idx;
        }
        for (int i = 0; i < nvisible; i++) if (vis_idx[i] == cur) return vis_idx[(i - 1 + nvisible) % nvisible];
        return vis_idx[0];
    }
    if (ntracks <= 1) return 0;
    return (cur - 1 + ntracks) % ntracks;
}

static void open_folder_dialog(void) {
    char picked[600] = {0};
    if (!be_pick_folder(picked, sizeof(picked))) return;
    int idx = add_scan_dir(picked);
    if (idx >= 0) { active_dir = idx; lib_tab = TAB_DIR; }
    scan_library(1);
}

static void handle_dropped(const char* raw) {
    if (!raw || !*raw) return;
    char path[600];
    snprintf(path, sizeof(path), "%s", raw);
    for (char* q = path; *q; q++) if (*q == '\\') *q = '/';

    if (ci_ends_with(path, ".lrc") || ci_ends_with(path, ".txt")) {
        Lyrics* ext = lyrics_load_file(path);
        if (ext) {
            if (lyr) lyrics_free(lyr);
            lyr = ext;
            g_lyr_req_id = 0;
            show_toast("已加载外部 .lrc 歌词");
        } else {
            show_toast("外部 .lrc 解析失败");
        }
        return;
    }

    int is_d = 0, is_r = 0;
    if (plat_stat_type(path, &is_d, &is_r) == 0 && is_d) {
        int di = add_scan_dir(path);
        if (di >= 0) { active_dir = di; lib_tab = TAB_DIR; }
        scan_library(1);
        return;
    }

    if (dec_is_audio(path)) {
        int idx = add_track_path(path, active_dir >= 0 ? active_dir : 0);
        if (idx >= 0) {
            rebuild_visible();
            load_track(idx);
            show_toast("正在播放拖入音频");
        }
    } else {
        show_toast("不支持的文件格式");
    }
}


// ---------------- Text Wrapping & Layout Helpers ----------------
static int utf8_prev_cp(const char* s, int pos) {
    if (pos <= 0) return 0;
    pos--;
    while (pos > 0 && (((unsigned char)s[pos]) & 0xC0) == 0x80) pos--;
    return pos;
}

static int wrap_utf8_lines(const char* s, float px, int max_w, char out[][256], int max_lines) {
    if (!s || !*s || max_lines <= 0) return 0;
    int nlines = 0;
    const char* p = s;
    while (*p && nlines < max_lines) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char* cur = p;
        const char* last_space = NULL;
        const char* best_end = p;
        while (*cur) {
            unsigned char c = (unsigned char)*cur;
            int clen = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
            const char* nxt = cur;
            for (int k = 0; k < clen && *nxt; k++) nxt++;
            if (*cur == '\n') { best_end = cur; break; }
            if (*cur == ' ') last_space = cur;
            size_t seg_len = (size_t)(nxt - p);
            if (seg_len >= 255) break;
            char tmp[256];
            memcpy(tmp, p, seg_len);
            tmp[seg_len] = 0;
            if (text_width(font, px, tmp) > max_w && cur > p) {
                if (last_space && last_space > p && clen == 1 && *cur != ' ') best_end = last_space;
                else best_end = cur;
                break;
            }
            best_end = nxt;
            cur = nxt;
        }
        size_t cp = (size_t)(best_end - p);
        if (cp == 0) {
            unsigned char c = (unsigned char)*p;
            int clen = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
            cp = 0;
            while (cp < (size_t)clen && p[cp]) cp++;
        }
        if (cp > 255) cp = 255;
        memcpy(out[nlines], p, cp);
        out[nlines][cp] = 0;
        nlines++;
        p += cp;
        if (*p == '\n' || *p == ' ') p++;
    }
    return nlines;
}

static void calc_grid_metrics(int* out_cols, int* out_gap, int* out_cw, int* out_ch, int* out_herow) {
    int pad_x = 26;
    int sb_res = 22;
    int avail = W - RAIL_W - pad_x * 2 - sb_res;
    if (avail < 320) avail = 320;
    int gap = W >= 1440 ? 20 : 16;
    int cols = (avail + gap) / (218 + gap);
    if (cols < 2) cols = 2;
    if (cols > 6) cols = 6;
    int cw = (avail - gap * (cols - 1)) / cols;
    int ch = cw + 64;
    int herow = cols >= 3 ? (cw * 2 + gap) : avail;
    if (out_cols)  *out_cols = cols;
    if (out_gap)   *out_gap = gap;
    if (out_cw)    *out_cw = cw;
    if (out_ch)    *out_ch = ch;
    if (out_herow) *out_herow = herow;
    g_grid_cols = cols; g_grid_gap = gap; g_grid_w = avail;
    g_card_w = cw; g_card_h = ch; g_hero_w = herow;
}

static int list_vis_h(void) {
    int top_y = 114;
    int bot_pad = 16;
    int h = H - top_y - bot_pad;
    return h < 120 ? 120 : h;
}

static float max_scroll_px(void) {
    int dock_clear = 98;
    if (view_grid) {
        int cols, gap, cw, ch, herow;
        calc_grid_metrics(&cols, &gap, &cw, &ch, &herow);
        int rows = 1;
        if (cols >= 3) {
            int row0_rem = cols - 2;
            int rem = nvisible - row0_rem;
            if (rem > 0) rows += (rem + cols - 1) / cols;
        } else {
            if (nvisible > 0) rows += (nvisible + cols - 1) / cols;
        }
        int content_h = 8 + rows * ch + (rows > 0 ? (rows - 1) * gap : 0) + dock_clear;
        int vh = list_vis_h();
        return content_h > vh ? (float)(content_h - vh) : 0.0f;
    } else {
        int content_h = 34 + nvisible * ROW_H + dock_clear;
        int vh = list_vis_h();
        return content_h > vh ? (float)(content_h - vh) : 0.0f;
    }
}

static void pump_background_meta(void) {
    if (nvisible <= 0) return;
    int loaded = 0;
    int first_row = 0;
    if (view_grid) {
        int cols, gap, cw, ch, herow;
        calc_grid_metrics(&cols, &gap, &cw, &ch, &herow);
        int row = (int)(scroll_px / (float)(ch + gap));
        if (row <= 0) first_row = 0;
        else if (cols >= 3) first_row = (cols - 2) + (row - 1) * cols;
        else first_row = (row - 1) * cols;
        if (first_row < 0) first_row = 0;
    } else {
        first_row = (int)(scroll_px / (float)ROW_H);
        if (first_row < 0) first_row = 0;
    }
    int win = view_grid ? 18 : 16;
    for (int p = first_row; p < nvisible && p < first_row + win && loaded < 3; p++) {
        int idx = vis_idx[p];
        if (tracks[idx].meta_state == 0) {
            ensure_track_meta(idx, 1);
            loaded++;
        }
    }
    for (int step = 0; step < 3 && loaded < 2 && nvisible > 0; step++) {
        if (probe_cursor >= nvisible) probe_cursor = 0;
        int idx = vis_idx[probe_cursor++];
        if (tracks[idx].meta_state < 2) {
            ensure_track_meta(idx, 1);
            loaded++;
        }
    }
    trim_thumbs(first_row);
}

static void reset_hit_rects(void) {
    memset(&rc_menu_btn, 0, sizeof(rc_menu_btn));
    memset(&rc_nav_all, 0, sizeof(rc_nav_all));
    memset(&rc_nav_fav, 0, sizeof(rc_nav_fav));
    memset(&rc_nav_lyr, 0, sizeof(rc_nav_lyr));
    memset(&rc_nav_add, 0, sizeof(rc_nav_add));
    memset(&rc_nav_diag, 0, sizeof(rc_nav_diag));
    memset(&rc_nav_theme, 0, sizeof(rc_nav_theme));
    memset(&rc_nav_view, 0, sizeof(rc_nav_view));
    memset(&rc_nav_rescan, 0, sizeof(rc_nav_rescan));
    memset(&rc_search_box, 0, sizeof(rc_search_box));
    memset(&rc_search_clear, 0, sizeof(rc_search_clear));
    memset(&rc_sort_btn, 0, sizeof(rc_sort_btn));
    memset(&rc_fs_btn, 0, sizeof(rc_fs_btn));
    memset(&rc_view_pill, 0, sizeof(rc_view_pill));
    memset(&rc_pal_btn, 0, sizeof(rc_pal_btn));
    memset(rc_filt_chips, 0, sizeof(rc_filt_chips));
    memset(&rc_dir_chip, 0, sizeof(rc_dir_chip));
    memset(&rc_hero_card, 0, sizeof(rc_hero_card));
    memset(&rc_hero_cov, 0, sizeof(rc_hero_cov));
    memset(&rc_hero_play, 0, sizeof(rc_hero_play));
    memset(&rc_hero_prog, 0, sizeof(rc_hero_prog));
    memset(&rc_sb_track, 0, sizeof(rc_sb_track));
    memset(&rc_fab_import, 0, sizeof(rc_fab_import));
    memset(&rc_back_lib, 0, sizeof(rc_back_lib));
    memset(&rc_lyr_theme, 0, sizeof(rc_lyr_theme));
    memset(&rc_lyr_fs, 0, sizeof(rc_lyr_fs));
    memset(&rc_island, 0, sizeof(rc_island));
    memset(&rc_bar_cov, 0, sizeof(rc_bar_cov));
    memset(&rc_bar_fav, 0, sizeof(rc_bar_fav));
    memset(&rc_bar_prev, 0, sizeof(rc_bar_prev));
    memset(&rc_bar_play, 0, sizeof(rc_bar_play));
    memset(&rc_bar_next, 0, sizeof(rc_bar_next));
    memset(&rc_bar_mode, 0, sizeof(rc_bar_mode));
    memset(&rc_bar_lyr, 0, sizeof(rc_bar_lyr));
    memset(&rc_bar_vol_ico, 0, sizeof(rc_bar_vol_ico));
    memset(&rc_bar_vol_trk, 0, sizeof(rc_bar_vol_trk));
    memset(&rc_bar_prog, 0, sizeof(rc_bar_prog));
    ngrid_hits = 0;
    nlyr_hits = 0;
}

// ---------------- Extra Vector Icons ----------------
static void icon_library_grid(Canvas* c, int x, int y, int sz, uint32_t col) {
    int cell = (sz - 4) / 2;
    if (cell < 4) cell = 4;
    fill_round_rect_aa(c, x, y, cell, cell, 3, col);
    fill_round_rect_aa(c, x + cell + 4, y, cell, cell, 3, col);
    fill_round_rect_aa(c, x, y + cell + 4, cell, cell, 3, col);
    fill_round_rect_aa(c, x + cell + 4, y + cell + 4, cell, cell, 3, col);
}

static void icon_list_rows(Canvas* c, int x, int y, int sz, uint32_t col) {
    for (int i = 0; i < 3; i++) {
        int ry = y + 2 + i * ((sz - 4) / 2);
        fill_round_rect_aa(c, x, ry, 3, 3, 1, col);
        fill_round_rect_aa(c, x + 6, ry, sz - 6, 3, 1, col);
    }
}

static void icon_lyrics_waves(Canvas* c, int x, int y, int sz, uint32_t col) {
    int bw = 3;
    int gap = (sz - 4 * bw) / 3;
    if (gap < 2) gap = 2;
    int hs[4] = { (int)(sz * 0.48f), sz, (int)(sz * 0.72f), (int)(sz * 0.42f) };
    for (int i = 0; i < 4; i++) {
        int h = hs[i];
        int bx = x + i * (bw + gap);
        int by = y + (sz - h) / 2;
        fill_round_rect_aa(c, bx, by, bw, h, 2, col);
    }
}

static void icon_refresh(Canvas* c, int cx, int cy, int r, uint32_t col) {
    stroke_circle_aa(c, cx, cy, r, 2, col, 225);
    fill_tri_aa(c, cx + r - 3, cy - r + 2, cx + r + 4, cy - 1, cx + r - 5, cy, col);
}

static void icon_theme_orb(Canvas* c, int cx, int cy, int r, uint32_t col) {
    stroke_circle_aa(c, cx, cy, r, 2, col, 225);
    fill_ellipse_aa(c, cx - 1, cy, r - 3, r - 3, col);
}

static void icon_info_dot(Canvas* c, int cx, int cy, int r, uint32_t col) {
    stroke_circle_aa(c, cx, cy, r, 2, col, 215);
    fill_round_rect_aa(c, cx - 1, cy - r / 2, 2, 3, 1, col);
    fill_round_rect_aa(c, cx - 1, cy - 1, 2, r / 2 + 2, 1, col);
}

static void icon_plus(Canvas* c, int cx, int cy, int r, uint32_t col) {
    fill_round_rect_aa(c, cx - r, cy - 1, r * 2, 3, 1, col);
    fill_round_rect_aa(c, cx - 1, cy - r, 3, r * 2, 1, col);
}

// ---------------- Translucent Material You Navigation Rail ----------------
static void draw_nav_rail(int mx, int my) {
    // Translucent frosted rail allowing water ripples to bleed subtly through
    fill_rect_a(screen, 0, 0, RAIL_W, H, C_RAIL, col_a(C_RAIL));
    fill_rect_a(screen, RAIL_W - 1, 0, 1, H, C_DIV, col_a(C_DIV));

    // Top app avatar / poster icon
    int top_y = 18;
    rc_menu_btn = (Rect){ 16, top_y, 44, 44 };
    int hov_m = in_rc(mx, my, rc_menu_btn);
    if (hov_m) fill_round_rect_aa(screen, 14, top_y - 2, 48, 48, 16, C_GLASS_HOV);
    if (poster_ico) {
        fill_round_rect_shadow(screen, 19, top_y + 3, 38, 38, 14, 70);
        blit_rgb_round_fit(screen, 18, top_y + 2, 40, 40, poster_ico, 40, 40, 14);
        stroke_round_rect_aa(screen, 18, top_y + 2, 40, 40, 14, 1, C_ACCENT, 130);
    } else {
        fill_round_rect_aa(screen, 18, top_y + 2, 40, 40, 14, C_ACCENT);
        icon_menu(screen, 28, top_y + 16, 20, C_ON_ACCENT);
    }

    // Active gliding Material 3 pill indicator target
    int active_slot = 0;
    if (page == PAGE_LYRICS) active_slot = 2;
    else if (lib_tab == TAB_FAV) active_slot = 1;
    else active_slot = 0;

    int nav_y0 = 88;
    int nav_step = 68;
    float tgt_pill_y = (float)(nav_y0 + active_slot * nav_step);
    float kp = clampf((float)g_dt * 16.0f, 0.10f, 1.0f);
    rail_pill_y += (tgt_pill_y - rail_pill_y) * kp;

    // Animated gliding M3 pill backdrop
    fill_round_rect_aa_a(screen, 12, (int)(rail_pill_y + 0.5f), RAIL_W - 24, 34, 17,
                         mix_col(C_SURFACE, C_ACCENT, 0.34f), g_is_light ? 225 : 210);
    stroke_round_rect_aa(screen, 12, (int)(rail_pill_y + 0.5f), RAIL_W - 24, 34, 17, 1, C_ACCENT, 130);

    const char* labels[4] = { "曲库", "喜欢", "歌词", "刷新" };
    Rect* nav_rcs[4] = { &rc_nav_all, &rc_nav_fav, &rc_nav_lyr, &rc_nav_rescan };
    for (int i = 0; i < 4; i++) {
        int iy = nav_y0 + i * nav_step;
        *nav_rcs[i] = (Rect){ 8, iy - 4, RAIL_W - 16, 56 };
        int is_act = (i == active_slot);
        int is_hov = in_rc(mx, my, *nav_rcs[i]);
        if (is_hov && !is_act) {
            fill_round_rect_aa(screen, 12, iy, RAIL_W - 24, 34, 17, C_GLASS_HOV);
        }
        uint32_t icol = is_act ? C_ACCENT : (is_hov ? C_TXT : C_TXT2);
        int cx = RAIL_W / 2;
        int cy = iy + 17;
        if (i == 0) icon_library_grid(screen, cx - 9, cy - 9, 18, icol);
        else if (i == 1) icon_heart_ex(screen, cx - 9, cy - 8, 18, is_act ? C_HEART : icol, is_act);
        else if (i == 2) icon_lyrics_waves(screen, cx - 9, cy - 9, 18, icol);
        else icon_refresh(screen, cx, cy, 8, icol);
        draw_text_centered(screen, font, 11.5f, 4, RAIL_W - 4, iy + 38,
                           is_act ? C_ACCENT : C_TXT2, labels[i]);
    }

    // Bottom rail utility icons: View toggle, Dark/Light Theme, Audio Diagnostics
    int bot_y = H - 24;
    rc_nav_diag  = (Rect){ 14, bot_y - 44, RAIL_W - 28, 36 };
    rc_nav_theme = (Rect){ 14, bot_y - 88, RAIL_W - 28, 36 };
    rc_nav_view  = (Rect){ 14, bot_y - 132, RAIL_W - 28, 36 };

    if (in_rc(mx, my, rc_nav_view)) fill_round_rect_aa(screen, rc_nav_view.x, rc_nav_view.y, rc_nav_view.w, rc_nav_view.h, 14, C_GLASS_HOV);
    if (view_grid) icon_list_rows(screen, RAIL_W / 2 - 8, rc_nav_view.y + 10, 16, C_TXT2);
    else           icon_library_grid(screen, RAIL_W / 2 - 8, rc_nav_view.y + 10, 16, C_TXT2);

    if (in_rc(mx, my, rc_nav_theme)) fill_round_rect_aa(screen, rc_nav_theme.x, rc_nav_theme.y, rc_nav_theme.w, rc_nav_theme.h, 14, C_GLASS_HOV);
    icon_theme_orb(screen, RAIL_W / 2, rc_nav_theme.y + 18, 8, g_is_light ? C_ACCENT : C_TXT2);

    if (in_rc(mx, my, rc_nav_diag)) fill_round_rect_aa(screen, rc_nav_diag.x, rc_nav_diag.y, rc_nav_diag.w, rc_nav_diag.h, 14, C_GLASS_HOV);
    icon_info_dot(screen, RAIL_W / 2, rc_nav_diag.y + 18, 8, show_diag ? C_ACCENT : C_TXT2);
}

// ---------------- Top Apple Search Capsule & Material You Chips ----------------
static void draw_library_header(int mx, int my) {
    int lx = RAIL_W + 26;
    int rx = W - 26;

    const char* title = "音乐库";
    if (lib_tab == TAB_FAV) title = "我喜欢的";
    else if (lib_tab == TAB_DIR && active_dir >= 0 && active_dir < nscan_dirs) {
        static char dname[96];
        basename_str(scan_dirs[active_dir], dname, sizeof(dname));
        title = dname;
    }

    draw_text_max_bold(screen, font, 24.0f, lx, 18, C_TXT, title, 180);
    int tw = text_width(font, 24.0f, title);
    if (tw > 180) tw = 180;

    // Song count pastel badge pill next to title
    char badge[48];
    snprintf(badge, sizeof(badge), "%d 首歌曲", nvisible);
    int bw = text_width(font, 12.0f, badge) + 22;
    int bx = lx + tw + 12;
    fill_round_rect_aa_a(screen, bx, 21, bw, 26, 13, mix_col(C_SURFACE, C_ACCENT, 0.28f), 196);
    stroke_round_rect_aa(screen, bx, 21, bw, 26, 13, 1, C_ACCENT, 95);
    draw_text_centered(screen, font, 12.0f, bx, bx + bw, 27, C_ACCENT, badge);

    // Right controls (Sort pill, View toggle pill, Fullscreen button)
    int fs_sz = 38;
    rc_fs_btn = (Rect){ rx - fs_sz, 16, fs_sz, fs_sz };
    fill_round_rect_aa(screen, rc_fs_btn.x, rc_fs_btn.y, fs_sz, fs_sz, 14,
                       in_rc(mx, my, rc_fs_btn) ? C_GLASS_HOV : C_GLASS);
    stroke_round_rect_aa(screen, rc_fs_btn.x, rc_fs_btn.y, fs_sz, fs_sz, 14, 1, C_DIV, col_a(C_DIV) + 12);
    icon_expand(screen, rc_fs_btn.x + 11, rc_fs_btn.y + 11, 16, C_TXT2);

    const char* view_lbl = view_grid ? "网格视图" : "列表视图";
    int vw = text_width(font, 12.5f, view_lbl) + 28;
    rc_view_pill = (Rect){ rc_fs_btn.x - 10 - vw, 17, vw, 36 };
    fill_round_rect_aa(screen, rc_view_pill.x, rc_view_pill.y, vw, 36, 18,
                       in_rc(mx, my, rc_view_pill) ? C_GLASS_HOV : C_GLASS);
    stroke_round_rect_aa(screen, rc_view_pill.x, rc_view_pill.y, vw, 36, 18, 1, C_DIV, col_a(C_DIV) + 12);
    draw_text_centered(screen, font, 12.5f, rc_view_pill.x, rc_view_pill.x + vw, 28, C_TXT2, view_lbl);

    const char* sort_lbls[4] = { "排序: 默认", "排序: 歌名", "排序: 歌手", "排序: 专辑" };
    const char* slbl = sort_lbls[sort_mode & 3];
    int sw = text_width(font, 12.5f, slbl) + 26;
    rc_sort_btn = (Rect){ rc_view_pill.x - 10 - sw, 17, sw, 36 };
    fill_round_rect_aa(screen, rc_sort_btn.x, rc_sort_btn.y, sw, 36, 18,
                       in_rc(mx, my, rc_sort_btn) ? C_GLASS_HOV : C_GLASS);
    stroke_round_rect_aa(screen, rc_sort_btn.x, rc_sort_btn.y, sw, 36, 18, 1, C_DIV, col_a(C_DIV) + 12);
    draw_text_centered(screen, font, 12.5f, rc_sort_btn.x, rc_sort_btn.x + sw, 28, C_TXT2, slbl);

    // Centered Floating Apple Search Capsule
    int s_left = bx + bw + 16;
    int s_right = rc_sort_btn.x - 16;
    if (s_right - s_left >= 180) {
        int sbox_w = s_right - s_left;
        if (sbox_w > 430) sbox_w = 430;
        int sbox_x = s_left + (s_right - s_left - sbox_w) / 2;
        rc_search_box = (Rect){ sbox_x, 15, sbox_w, 40 };
        fill_round_rect_aa_a(screen, rc_search_box.x, rc_search_box.y, rc_search_box.w, rc_search_box.h, 20,
                             C_SURFACE, search_focused ? 215 : 155);
        stroke_round_rect_aa(screen, rc_search_box.x, rc_search_box.y, rc_search_box.w, rc_search_box.h, 20,
                             search_focused ? 2 : 1, search_focused ? C_ACCENT : C_DIV,
                             search_focused ? 210 : (col_a(C_DIV) + 18));
        icon_search(screen, rc_search_box.x + 20, rc_search_box.y + 19, 6,
                    search_focused ? C_ACCENT : C_TXT3);
        int text_max = rc_search_box.w - 70;
        if (search_q[0]) {
            draw_text_max(screen, font, 13.5f, rc_search_box.x + 38, rc_search_box.y + 12,
                          C_TXT, search_q, text_max);
            rc_search_clear = (Rect){ rc_search_box.x + rc_search_box.w - 34, rc_search_box.y + 6, 28, 28 };
            fill_round_rect_aa(screen, rc_search_clear.x, rc_search_clear.y, 28, 28, 14,
                               in_rc(mx, my, rc_search_clear) ? C_GLASS_ACT : C_GLASS);
            draw_text_centered(screen, font, 14.0f, rc_search_clear.x, rc_search_clear.x + 28,
                               rc_search_clear.y + 6, C_TXT2, "x");
        } else {
            draw_text_max(screen, font, 13.0f, rc_search_box.x + 38, rc_search_box.y + 12,
                          C_TXT3, "搜索歌曲、歌手、专辑...", text_max);
        }
    }

    // Second row: Material You filter chips + folder badge
    int cy = 68;
    int cx = lx;
    const char* chips[5] = { "全部", "已喜欢", "FLAC 无损", "MP3", "其他格式" };
    for (int i = 0; i < 5; i++) {
        int cw = text_width(font, 13.0f, chips[i]) + (fmt_filter == i ? 36 : 26);
        rc_filt_chips[i] = (Rect){ cx, cy, cw, 32 };
        int act = (fmt_filter == i);
        int hov = in_rc(mx, my, rc_filt_chips[i]);
        if (act) {
            fill_round_rect_aa_a(screen, cx, cy, cw, 32, 16, mix_col(C_SURFACE, C_ACCENT, 0.30f), 205);
            stroke_round_rect_aa(screen, cx, cy, cw, 32, 16, 1, C_ACCENT, 140);
            fill_ellipse_aa(screen, cx + 14, cy + 16, 4, 4, C_ACCENT);
            draw_text(screen, font, 13.0f, cx + 24, cy + 8, C_ACCENT, chips[i]);
        } else {
            fill_round_rect_aa(screen, cx, cy, cw, 32, 16, hov ? C_GLASS_HOV : C_GLASS);
            stroke_round_rect_aa(screen, cx, cy, cw, 32, 16, 1, C_DIV, col_a(C_DIV) + 10);
            draw_text_centered(screen, font, 13.0f, cx, cx + cw, cy + 8, hov ? C_TXT : C_TXT2, chips[i]);
        }
        cx += cw + 10;
    }

    if (nscan_dirs > 0) {
        char dlabel[96] = "全部目录";
        if (lib_tab == TAB_DIR && active_dir >= 0 && active_dir < nscan_dirs) {
            char bn[64];
            basename_str(scan_dirs[active_dir], bn, sizeof(bn));
            snprintf(dlabel, sizeof(dlabel), "目录: %s", bn);
        }
        int dw = text_width(font, 12.5f, dlabel) + 26;
        if (rx - dw > cx + 10) {
            rc_dir_chip = (Rect){ rx - dw, cy, dw, 32 };
            int act = (lib_tab == TAB_DIR);
            if (act) fill_round_rect_aa_a(screen, rc_dir_chip.x, rc_dir_chip.y, dw, 32, 16, mix_col(C_SURFACE, C_ACCENT, 0.26f), 205);
            else     fill_round_rect_aa(screen, rc_dir_chip.x, rc_dir_chip.y, dw, 32, 16, in_rc(mx, my, rc_dir_chip) ? C_GLASS_HOV : C_GLASS);
            stroke_round_rect_aa(screen, rc_dir_chip.x, rc_dir_chip.y, dw, 32, 16, 1, act ? C_ACCENT : C_DIV, act ? 140 : (col_a(C_DIV) + 12));
            draw_text_centered(screen, font, 12.5f, rc_dir_chip.x, rc_dir_chip.x + dw, cy + 8,
                               act ? C_ACCENT : C_TXT2, dlabel);
        }
    }
}

// ---------------- Material You Hero Card (Wavy Split + Rotating Starburst) ----------------
static void draw_hero_card(Rect hero, int mx, int my) {
    if (hero.w < 240 || hero.h < 150) return;
    rc_hero_card = hero;
    int r = 24;
    fill_round_rect_shadow(screen, hero.x, hero.y + 4, hero.w, hero.h, r, 78);
    fill_round_rect_aa_a(screen, hero.x, hero.y, hero.w, hero.h, r, C_HERO_L, g_is_light ? 210 : 186);

    // Material You animated wavy divider into right zone
    int split_x = hero.x + (int)(hero.w * 0.65f);
    float wave_amp = clampf((float)hero.h * 0.032f, 6.0f, 10.0f);
    fill_m3_wave_panel_right(screen, hero.x, hero.y, hero.w, hero.h, r,
                             split_x, wave_amp, 56.0f, wave_phase * 0.55f,
                             C_HERO_R, g_is_light ? 205 : 200);
    stroke_round_rect_aa(screen, hero.x, hero.y, hero.w, hero.h, r, 1, C_ACCENT, 88);

    int show_idx = playing_idx;
    if (show_idx < 0 && nvisible > 0) show_idx = vis_idx[0];

    int paused = audio_paused() || playing_idx < 0;
    double dur = playing_idx >= 0 ? audio_duration() : (show_idx >= 0 ? tracks[show_idx].dur : 0.0);
    double pos = playing_idx >= 0 ? (scrub_drag ? scrub_sec : audio_position()) : 0.0;
    float frac = (dur > 0.1) ? clampf((float)(pos / dur), 0.0f, 1.0f) : 0.0f;

    // Right wavy zone: Rotating Material 3 Scalloped Starburst Play/Pause Button
    int right_mid_x = split_x + (hero.x + hero.w - split_x) / 2;
    int star_cy = hero.y + hero.h / 2 - 12;
    int star_r = hero.h / 5;
    if (star_r < 28) star_r = 28;
    if (star_r > 46) star_r = 46;
    rc_hero_play = (Rect){ right_mid_x - star_r - 6, star_cy - star_r - 6, (star_r + 6) * 2, (star_r + 6) * 2 };
    int hov_play = in_rc(mx, my, rc_hero_play);
    int draw_r = hov_play ? star_r + 3 : star_r;
    fill_m3_scallop(screen, right_mid_x, star_cy, draw_r, 10, scallop_rot,
                    hov_play ? mix_col(C_ACCENT, COL(255, 255, 255, 255), 0.16f) : C_ACCENT);
    if (!paused && playing_idx >= 0) icon_pause(screen, right_mid_x, star_cy, (int)(draw_r * 0.34f), C_ON_ACCENT);
    else                             icon_play(screen, right_mid_x + 2, star_cy, (int)(draw_r * 0.36f), C_ON_ACCENT);

    const char* action_lbl = (playing_idx >= 0 && !paused) ? "点击暂停" : (playing_idx >= 0 ? "继续播放" : "开始聆听");
    draw_text_centered(screen, font, 12.5f, split_x + 6, hero.x + hero.w - 6,
                       star_cy + star_r + 12, C_ACCENT, action_lbl);

    // Left hero info zone
    int lx = hero.x + 20;
    int rx_lim = split_x - (int)wave_amp - 14;
    const char* pill_txt = (playing_idx >= 0 && !paused) ? "正在播放" : (playing_idx >= 0 ? "已暂停" : "精选曲目");
    int pill_w = text_width(font, 11.5f, pill_txt) + 26;
    fill_round_rect_aa_a(screen, lx, hero.y + 16, pill_w, 22, 11, mix_col(C_SURFACE, C_ACCENT, 0.26f), 196);
    fill_ellipse_aa(screen, lx + 11, hero.y + 27, 3, 3, C_ACCENT);
    draw_text(screen, font, 11.5f, lx + 19, hero.y + 20, C_ACCENT, pill_txt);

    // Hero cover square
    int cov_sz = (int)(hero.h * 0.36f);
    if (cov_sz < 72) cov_sz = 72;
    if (cov_sz > 106) cov_sz = 106;
    int cov_y = hero.y + 46;
    rc_hero_cov = (Rect){ lx, cov_y, cov_sz, cov_sz };
    const uint32_t* hc = (playing_idx >= 0 && cover_ready && cover_src) ? cover_src :
                         (show_idx >= 0 && tracks[show_idx].thumb) ? tracks[show_idx].thumb : poster_hero;
    int hsrc_sz = (playing_idx >= 0 && cover_ready && cover_src) ? COVER_SRC_SZ :
                  (show_idx >= 0 && tracks[show_idx].thumb) ? THUMB_SZ : COVER_SRC_SZ;
    fill_round_rect_shadow(screen, lx, cov_y + 3, cov_sz, cov_sz, 16, 75);
    if (hc) blit_rgb_round_fit(screen, lx, cov_y, cov_sz, cov_sz, hc, hsrc_sz, hsrc_sz, 16);
    else    fill_round_rect_aa(screen, lx, cov_y, cov_sz, cov_sz, 16, C_GLASS_ACT);
    stroke_round_rect_aa(screen, lx, cov_y, cov_sz, cov_sz, 16, 1, C_DIV, col_a(C_DIV) + 14);

    int tx = lx + cov_sz + 16;
    int tmax = rx_lim - tx;
    if (tmax < 60) tmax = 60;
    const char* ttl = playing_idx >= 0 ? p_title : (show_idx >= 0 ? tracks[show_idx].title : "拖入音乐或点击导入");
    const char* art = playing_idx >= 0 ? (p_artist[0] ? p_artist : "本地音乐") : (show_idx >= 0 && tracks[show_idx].artist[0] ? tracks[show_idx].artist : "Sirène");
    const char* fmt = playing_idx >= 0 ? p_fmt : (show_idx >= 0 ? tracks[show_idx].fmt : "HI-RES");

    draw_text_max_bold(screen, font, 19.0f, tx, cov_y + 4, C_TXT, ttl, tmax);
    int fw = text_width(font, 11.0f, fmt) + 16;
    fill_round_rect_aa(screen, tx, cov_y + 34, fw, 20, 6, C_GLASS_ACT);
    draw_text_centered(screen, font, 11.0f, tx, tx + fw, cov_y + 38, C_TXT2, fmt);
    if (tmax > fw + 16) draw_text_max(screen, font, 13.0f, tx + fw + 8, cov_y + 36, C_TXT2, art, tmax - fw - 8);

    // Large % indicator + time readout + bottom M3 wavy progress scrubber
    int bot_area_y = hero.y + hero.h - 68;
    char pct[16], tm[48], b_pos[20], b_dur[20];
    snprintf(pct, sizeof(pct), "%d%%", (int)(frac * 100.0f + 0.5f));
    fmt_time(pos, b_pos, sizeof(b_pos));
    fmt_time(dur, b_dur, sizeof(b_dur));
    snprintf(tm, sizeof(tm), "%s / %s", b_pos, b_dur);

    draw_text_bold(screen, font, 28.0f, lx, bot_area_y, C_TXT, pct);
    int pw = text_width(font, 28.0f, pct);
    draw_text(screen, font, 12.0f, lx + pw + 12, bot_area_y + 11, C_TXT2, tm);

    int hprog_y = hero.y + hero.h - 24;
    ui_hpx0 = lx + 4;
    ui_hpx1 = rx_lim - 4;
    if (ui_hpx1 > ui_hpx0 + 40) {
        rc_hero_prog = (Rect){ ui_hpx0 - 6, hprog_y - 12, (ui_hpx1 - ui_hpx0) + 12, 24 };
        draw_m3_wavy_progress(screen, ui_hpx0, ui_hpx1, hprog_y, frac,
                              wave_phase, (!paused && playing_idx >= 0) ? 3.8f : 1.5f, 18.0f,
                              4, 4, 1, C_ACCENT, C_TRACK_BG);
    }
}

// ---------------- Large Album Cover Bento Grid View ----------------
static void draw_grid_view(int mx, int my) {
    int lx = RAIL_W + 26;
    int top_y = 114;
    int vh = list_vis_h();
    int cols, gap, cw, ch, herow;
    calc_grid_metrics(&cols, &gap, &cw, &ch, &herow);

    ui_list_x0 = lx;
    ui_list_x1 = lx + g_grid_w;
    ui_list_y0 = top_y;
    ui_list_y1 = top_y + vh;

    canvas_set_clip(screen, lx - 8, top_y, lx + g_grid_w + 10, top_y + vh);

    int start_y = top_y + 8 - (int)scroll_px;

    // Hero Card in Row 0 (2 columns wide if cols >= 3, else full row width)
    if (start_y + ch > top_y - 16 && start_y < top_y + vh + 16) {
        Rect hero = { lx, start_y, herow, ch };
        draw_hero_card(hero, mx, my);
    }

    int next_hov = -1;
    int row0_rem = cols >= 3 ? (cols - 2) : 0;

    for (int p = 0; p < nvisible; p++) {
        int row, col;
        if (cols >= 3 && p < row0_rem) {
            row = 0;
            col = 2 + p;
        } else {
            int rem_idx = p - row0_rem;
            row = 1 + rem_idx / cols;
            col = rem_idx % cols;
        }
        int x = lx + col * (cw + gap);
        int y = start_y + row * (ch + gap);
        if (y + ch < top_y - 16 || y > top_y + vh + 16) continue;

        int idx = vis_idx[p];
        Track* tr = &tracks[idx];
        Rect card_rc = { x, y, cw, ch };
        int in_view = in_rect(mx, my, lx - 4, top_y, g_grid_w + 8, vh);
        int is_hov = in_view && in_rc(mx, my, card_rc);
        if (is_hov) next_hov = idx;

        int lift = (is_hov && hov_card_idx == idx) ? (int)(hov_card_t * 5.0f + 0.5f) : 0;
        int cy = y - lift;
        int is_play = (idx == playing_idx);

        fill_round_rect_shadow(screen, x, cy + 4, cw, ch, 18, is_play ? 110 : 62);
        fill_round_rect_aa_a(screen, x, cy, cw, ch, 18, C_SURFACE,
                             is_play ? 196 : (is_hov ? 184 : (g_is_light ? 184 : 148)));

        // Large Square Album Cover occupying full card width
        int cov_pad = 6;
        int cov_sz = cw - cov_pad * 2;
        int cx0 = x + cov_pad;
        int cy0 = cy + cov_pad;
        const uint32_t* art = tr->thumb ? tr->thumb : (poster_hero ? poster_hero : NULL);
        int art_sz = tr->thumb ? THUMB_SZ : COVER_SRC_SZ;
        if (art) blit_rgb_round_fit(screen, cx0, cy0, cov_sz, cov_sz, art, art_sz, art_sz, 14);
        else     fill_round_rect_aa(screen, cx0, cy0, cov_sz, cov_sz, 14, C_GLASS_ACT);

        // Top-left format pill on cover (translucent dark capsule)
        int fpw = text_width(font, 11.0f, tr->fmt) + 14;
        fill_round_rect_aa_a(screen, cx0 + 8, cy0 + 8, fpw, 21, 8, COL(12, 16, 22, 255), 182);
        draw_text_centered(screen, font, 11.0f, cx0 + 8, cx0 + 8 + fpw, cy0 + 12,
                           is_play ? C_ACCENT : COL(242, 246, 252, 255), tr->fmt);

        // Top-right Favorite Heart pill on cover
        Rect fav_rc = { cx0 + cov_sz - 34, cy0 + 6, 28, 28 };
        if (tr->fav || is_hov) {
            fill_round_rect_aa_a(screen, fav_rc.x, fav_rc.y, 28, 28, 14, COL(12, 16, 22, 255), 165);
            icon_heart_ex(screen, fav_rc.x + 6, fav_rc.y + 6, 16,
                          tr->fav ? C_HEART : COL(242, 246, 252, 220), tr->fav);
        }

        // Bottom-right quick Play / Active EQ indicator on cover
        if (is_play || is_hov) {
            int bx = cx0 + cov_sz - 40;
            int by = cy0 + cov_sz - 40;
            fill_round_rect_shadow(screen, bx, by + 2, 34, 34, 17, 90);
            fill_round_rect_aa(screen, bx, by, 34, 34, 17, C_ACCENT);
            if (is_play && !audio_paused()) {
                for (int b = 0; b < 3; b++) {
                    float lvl = clampf(bars[b * 2] * 0.8f + 0.25f + 0.20f * sinf(wave_phase * 2.1f + (float)b * 1.3f), 0.15f, 1.0f);
                    int bh = 5 + (int)(lvl * 13.0f);
                    fill_round_rect_aa(screen, bx + 10 + b * 6, by + 17 - bh / 2, 3, bh, 1, C_ON_ACCENT);
                }
            } else {
                icon_play(screen, bx + 18, by + 17, 11, C_ON_ACCENT);
            }
        }

        // Card outer border glow
        if (is_play) stroke_round_rect_aa(screen, x, cy, cw, ch, 18, 2, C_ACCENT, 210);
        else         stroke_round_rect_aa(screen, x, cy, cw, ch, 18, 1, C_DIV, is_hov ? 68 : col_a(C_DIV));

        // Clean 2-line footer text
        int info_y = cy0 + cov_sz + 8;
        draw_text_max_bold(screen, font, 14.0f, x + 10, info_y, is_play ? C_ACCENT : C_TXT, tr->title, cw - 20);
        char dur_s[16] = "--:--";
        if (tr->dur > 0.5) fmt_time(tr->dur, dur_s, sizeof(dur_s));
        int dw = text_width(font, 11.5f, dur_s);
        draw_text_right(screen, font, 11.5f, x + cw - 10, info_y + 24, is_play ? C_ACCENT : C_TXT3, dur_s);
        const char* sub = tr->artist[0] ? tr->artist : tr->album;
        draw_text_max(screen, font, 12.0f, x + 10, info_y + 23, C_TXT2, sub[0] ? sub : tr->fmt, cw - 26 - dw);

        if (ngrid_hits < MAX_GRID_HITS) {
            Rect hit_card = card_rc;
            if (hit_card.y < top_y) { hit_card.h -= (top_y - hit_card.y); hit_card.y = top_y; }
            if (hit_card.y + hit_card.h > top_y + vh) hit_card.h = top_y + vh - hit_card.y;
            if (hit_card.h > 0) {
                grid_hits[ngrid_hits++] = (GridHit){ hit_card, fav_rc, idx };
            }
        }
    }

    if (next_hov != hov_card_idx) { hov_card_idx = next_hov; hov_card_t = 0.0f; }

    canvas_reset_clip(screen);

    // Slim Material Scrollbar
    float mscr = max_scroll_px();
    if (mscr > 1.0f) {
        int sbx = W - 16;
        rc_sb_track = (Rect){ sbx - 4, top_y + 6, 14, vh - 100 };
        int th = rc_sb_track.h;
        int thumb_h = (int)((float)vh / (float)(vh + mscr) * (float)th);
        if (thumb_h < 34) thumb_h = 34;
        if (thumb_h > th) thumb_h = th;
        int ty = rc_sb_track.y + (int)((scroll_px / mscr) * (float)(th - thumb_h));
        fill_round_rect_aa(screen, sbx, rc_sb_track.y, 6, th, 3, C_TRACK_BG);
        fill_round_rect_aa(screen, sbx, ty, 6, thumb_h, 3, sb_drag ? C_ACCENT : C_TXT3);
    }
}

// ---------------- Compact Frosted Table View (Optional Toggle) ----------------
static void draw_table_view(int mx, int my) {
    int lx = RAIL_W + 26;
    int rx = W - 26;
    int top_y = 114;
    int vh = list_vis_h();
    int tw = rx - lx - 14;
    ui_list_x0 = lx; ui_list_x1 = lx + tw; ui_list_y0 = top_y; ui_list_y1 = top_y + vh;

    fill_round_rect_aa(screen, lx, top_y, tw, 30, 10, C_GLASS);
    draw_text(screen, font, 12.0f, lx + 56, top_y + 7, C_TXT3, "歌曲");
    draw_text(screen, font, 12.0f, lx + (int)(tw * 0.44f), top_y + 7, C_TXT3, "歌手");
    draw_text(screen, font, 12.0f, lx + (int)(tw * 0.68f), top_y + 7, C_TXT3, "格式");
    draw_text_right(screen, font, 12.0f, lx + tw - 24, top_y + 7, C_TXT3, "时长");

    int body_y = top_y + 34;
    int body_h = vh - 34;
    canvas_set_clip(screen, lx, body_y, lx + tw, body_y + body_h);

    for (int p = 0; p < nvisible; p++) {
        int y = body_y + p * ROW_H - (int)scroll_px;
        if (y + ROW_H < body_y || y > body_y + body_h) continue;
        int idx = vis_idx[p];
        Track* tr = &tracks[idx];
        Rect row_rc = { lx, y + 2, tw, ROW_H - 4 };
        int hov = in_rect(mx, my, lx, body_y, tw, body_h) && in_rc(mx, my, row_rc);
        int is_play = (idx == playing_idx);
        if (is_play) {
            fill_round_rect_aa_a(screen, row_rc.x, row_rc.y, row_rc.w, row_rc.h, 14, mix_col(C_SURFACE, C_ACCENT, 0.26f), 192);
        } else if (hov) {
            fill_round_rect_aa(screen, row_rc.x, row_rc.y, row_rc.w, row_rc.h, 14, C_GLASS_HOV);
        }
        const uint32_t* art = tr->thumb ? tr->thumb : poster_mini;
        int asz = tr->thumb ? THUMB_SZ : MINI_ART_SZ;
        if (art) blit_rgb_round_fit(screen, lx + 10, y + 7, 40, 40, art, asz, asz, 10);
        else     fill_round_rect_aa(screen, lx + 10, y + 7, 40, 40, 10, C_GLASS_ACT);

        Rect fav_rc = { lx + 56, y + 14, 24, 24 };
        icon_heart_ex(screen, fav_rc.x + 4, fav_rc.y + 4, 15, tr->fav ? C_HEART : C_TXT3, tr->fav);

        draw_text_max_bold(screen, font, 14.0f, lx + 88, y + 10, is_play ? C_ACCENT : C_TXT,
                           tr->title, (int)(tw * 0.44f) - 98);
        draw_text_max(screen, font, 12.0f, lx + 88, y + 30, C_TXT3, tr->album, (int)(tw * 0.44f) - 98);
        draw_text_max(screen, font, 13.0f, lx + (int)(tw * 0.44f), y + 17, C_TXT2,
                      tr->artist[0] ? tr->artist : "未知", (int)(tw * 0.22f));
        draw_text(screen, font, 12.0f, lx + (int)(tw * 0.68f), y + 18, is_play ? C_ACCENT : C_TXT3, tr->fmt);
        char ds[16] = "--:--";
        if (tr->dur > 0.5) fmt_time(tr->dur, ds, sizeof(ds));
        draw_text_right(screen, font, 12.5f, lx + tw - 24, y + 18, is_play ? C_ACCENT : C_TXT2, ds);

        if (ngrid_hits < MAX_GRID_HITS) {
            Rect hr = row_rc;
            if (hr.y < body_y) { hr.h -= (body_y - hr.y); hr.y = body_y; }
            if (hr.y + hr.h > body_y + body_h) hr.h = body_y + body_h - hr.y;
            if (hr.h > 0) grid_hits[ngrid_hits++] = (GridHit){ hr, fav_rc, idx };
        }
    }
    canvas_reset_clip(screen);

    float mscr = max_scroll_px();
    if (mscr > 1.0f) {
        int sbx = W - 16;
        rc_sb_track = (Rect){ sbx - 4, body_y, 14, body_h - 90 };
        int th = rc_sb_track.h;
        int thumb_h = (int)((float)body_h / (float)(body_h + mscr) * (float)th);
        if (thumb_h < 32) thumb_h = 32;
        int ty = rc_sb_track.y + (int)((scroll_px / mscr) * (float)(th - thumb_h));
        fill_round_rect_aa(screen, sbx, rc_sb_track.y, 6, th, 3, C_TRACK_BG);
        fill_round_rect_aa(screen, sbx, ty, 6, thumb_h, 3, sb_drag ? C_ACCENT : C_TXT3);
    }
}

// ---------------- Floating Apple Dynamic Island Dock + M3 Import FAB ----------------
static void draw_dynamic_island(int mx, int my) {
    // Bottom-Right Material 3 "导入" FAB
    int fab_w = 108, fab_h = 46;
    rc_fab_import = (Rect){ W - 26 - fab_w, H - 22 - fab_h, fab_w, fab_h };
    int hov_fab = in_rc(mx, my, rc_fab_import);
    fill_round_rect_shadow(screen, rc_fab_import.x, rc_fab_import.y + 3, fab_w, fab_h, 16, 110);
    fill_round_rect_aa(screen, rc_fab_import.x, rc_fab_import.y, fab_w, fab_h, 16,
                       hov_fab ? mix_col(C_ACCENT, COL(255, 255, 255, 255), 0.14f) : C_ACCENT);
    icon_plus(screen, rc_fab_import.x + 26, rc_fab_import.y + 23, 7, C_ON_ACCENT);
    draw_text_bold(screen, font, 15.0f, rc_fab_import.x + 42, rc_fab_import.y + 13, C_ON_ACCENT, "导入");

    // Centered Dynamic Island Capsule (never collides with Left Rail or Right FAB)
    int center_x = (RAIL_W + W) / 2;
    int max_half = rc_fab_import.x - 16 - center_x;
    if (center_x - RAIL_W - 14 < max_half) max_half = center_x - RAIL_W - 14;
    int max_w = max_half * 2;
    int base_w = max_w < 820 ? max_w : 820;
    if (base_w < 460) base_w = 460;

    int raw_hov = in_rect(mx, my, center_x - base_w / 2 - 20, H - 96, base_w + 40, 88) || scrub_drag == 1 || vol_drag;
    float tgt_exp = raw_hov ? 1.0f : 0.0f;
    island_exp += (tgt_exp - island_exp) * clampf((float)g_dt * 12.0f, 0.05f, 1.0f);

    int iw = base_w + (int)(26.0f * island_exp);
    if (iw > max_w) iw = max_w;
    int ih = 64 + (int)(6.0f * island_exp);
    int ix = center_x - iw / 2;
    int iy = H - 18 - ih;
    int ir = ih / 2;
    rc_island = (Rect){ ix, iy, iw, ih };

    fill_round_rect_shadow(screen, ix, iy + 5, iw, ih, ir, 130);
    uint32_t isl_bg = mix_col(g_is_light ? COL(252, 254, 255, 255) : COL(14, 18, 24, 255), C_HERO_L, 0.34f);
    fill_round_rect_aa_a(screen, ix, iy, iw, ih, ir, isl_bg, g_is_light ? 222 : 200);
    stroke_round_rect_aa(screen, ix, iy, iw, ih, ir, 1, C_ACCENT, 105);

    // Left mini cover inside capsule
    int cov_sz = ih - 18;
    rc_bar_cov = (Rect){ ix + 14, iy + (ih - cov_sz) / 2 - 2, cov_sz, cov_sz };
    const uint32_t* mc = (playing_idx >= 0 && cover_ready && cover_mini) ? cover_mini : poster_mini;
    if (mc) blit_rgb_round_fit(screen, rc_bar_cov.x, rc_bar_cov.y, cov_sz, cov_sz, mc, MINI_ART_SZ, MINI_ART_SZ, cov_sz / 2);
    else    fill_ellipse_aa(screen, rc_bar_cov.x + cov_sz / 2, rc_bar_cov.y + cov_sz / 2, cov_sz / 2, cov_sz / 2, C_ACCENT);

    // Center Transport Controls
    int mid_x = ix + iw / 2;
    int ctrl_y = iy + ih / 2 - 3;
    rc_bar_play = (Rect){ mid_x - 20, ctrl_y - 20, 40, 40 };
    rc_bar_prev = (Rect){ mid_x - 56, ctrl_y - 16, 32, 32 };
    rc_bar_next = (Rect){ mid_x + 24, ctrl_y - 16, 32, 32 };
    rc_bar_fav  = (Rect){ mid_x - 92, ctrl_y - 16, 30, 30 };

    int is_fav = (playing_idx >= 0 && tracks[playing_idx].fav);
    if (in_rc(mx, my, rc_bar_fav)) fill_round_rect_aa(screen, rc_bar_fav.x, rc_bar_fav.y, 30, 30, 15, C_GLASS_HOV);
    icon_heart_ex(screen, rc_bar_fav.x + 7, rc_bar_fav.y + 7, 16, is_fav ? C_HEART : C_TXT2, is_fav);

    if (in_rc(mx, my, rc_bar_prev)) fill_round_rect_aa(screen, rc_bar_prev.x, rc_bar_prev.y, 32, 32, 16, C_GLASS_HOV);
    icon_prev(screen, rc_bar_prev.x + 16, rc_bar_prev.y + 16, 11, C_TXT);

    int hov_p = in_rc(mx, my, rc_bar_play);
    fill_ellipse_aa(screen, mid_x, ctrl_y, hov_p ? 20 : 19, hov_p ? 20 : 19,
                    (playing_idx >= 0 && !audio_paused()) ? C_ACCENT : C_PLAY_BTN);
    if (playing_idx >= 0 && !audio_paused()) icon_pause(screen, mid_x, ctrl_y, 10, C_ON_ACCENT);
    else                                     icon_play(screen, mid_x + 1, ctrl_y, 11, C_PLAY_ICO);

    if (in_rc(mx, my, rc_bar_next)) fill_round_rect_aa(screen, rc_bar_next.x, rc_bar_next.y, 32, 32, 16, C_GLASS_HOV);
    icon_next(screen, rc_bar_next.x + 16, rc_bar_next.y + 16, 11, C_TXT);

    // Left track title + live EQ bars
    int info_x = rc_bar_cov.x + cov_sz + 12;
    int info_max = rc_bar_fav.x - info_x - 10;
    if (info_max > 40) {
        draw_text_max_bold(screen, font, 14.0f, info_x, iy + 11, C_TXT, p_title, info_max);
        int eq_x = info_x;
        int eq_y = iy + 39;
        for (int b = 0; b < 5; b++) {
            float lvl = (playing_idx >= 0 && !audio_paused())
                        ? clampf(bars[b * 2] * 0.75f + 0.25f + 0.22f * sinf(wave_phase * 2.4f + (float)b * 1.1f), 0.15f, 1.0f)
                        : 0.18f;
            int bh = 3 + (int)(lvl * 10.0f);
            fill_round_rect_aa(screen, eq_x + b * 5, eq_y - bh / 2, 3, bh, 1, C_ACCENT);
        }
        draw_text_max(screen, font, 11.5f, eq_x + 32, iy + 32, C_TXT2,
                      p_artist[0] ? p_artist : "点击封面进入歌词", info_max - 32);
    }

    // Right cluster inside Dynamic Island: Mode, Volume slider, Lyrics pill
    int rx = ix + iw - 16;
    rc_bar_lyr = (Rect){ rx - 34, ctrl_y - 14, 34, 28 };
    int act_lyr = (lyr && lyr->n > 0);
    if (act_lyr) fill_round_rect_aa_a(screen, rc_bar_lyr.x, rc_bar_lyr.y, 34, 28, 14, mix_col(C_SURFACE, C_ACCENT, 0.30f), 205);
    else         fill_round_rect_aa(screen, rc_bar_lyr.x, rc_bar_lyr.y, 34, 28, 14, in_rc(mx, my, rc_bar_lyr) ? C_GLASS_HOV : C_GLASS);
    draw_text_centered(screen, font, 12.5f, rc_bar_lyr.x, rc_bar_lyr.x + 34, rc_bar_lyr.y + 6,
                       act_lyr ? C_ACCENT : C_TXT2, "词");

    const char* mode_lbl = play_mode == MODE_LOOP_ONE ? "单曲" : (play_mode == MODE_SHUFFLE ? "随机" : "顺序");
    rc_bar_mode = (Rect){ rc_bar_lyr.x - 46, ctrl_y - 14, 40, 28 };
    fill_round_rect_aa(screen, rc_bar_mode.x, rc_bar_mode.y, 40, 28, 14,
                       in_rc(mx, my, rc_bar_mode) ? C_GLASS_HOV : C_GLASS);
    draw_text_centered(screen, font, 11.5f, rc_bar_mode.x, rc_bar_mode.x + 40, rc_bar_mode.y + 7, C_TXT2, mode_lbl);

    int vol_w = 56;
    if (rc_bar_mode.x - rc_bar_next.x - rc_bar_next.w > 110) {
        ui_vx1 = rc_bar_mode.x - 10;
        ui_vx0 = ui_vx1 - vol_w;
        rc_bar_vol_trk = (Rect){ ui_vx0 - 4, ctrl_y - 12, vol_w + 8, 24 };
        rc_bar_vol_ico = (Rect){ ui_vx0 - 26, ctrl_y - 12, 22, 24 };
        float v = audio_volume();
        icon_vol(screen, rc_bar_vol_ico.x + 11, ctrl_y, 14, C_TXT2, v <= 0.01f);
        fill_round_rect_aa(screen, ui_vx0, ctrl_y - 2, vol_w, 4, 2, C_TRACK_BG);
        int vw = (int)(v * (float)vol_w);
        if (vw > 0) fill_round_rect_aa(screen, ui_vx0, ctrl_y - 2, vw, 4, 2, C_ACCENT);
        fill_ellipse_aa(screen, ui_vx0 + vw, ctrl_y, 5, 5, C_ACCENT);
    }

    // Bottom edge integrated pill scrubber across the Dynamic Island
    double dur = playing_idx >= 0 ? audio_duration() : 0.0;
    double pos = playing_idx >= 0 ? (scrub_drag == 1 ? scrub_sec : audio_position()) : 0.0;
    float frac = (dur > 0.1) ? clampf((float)(pos / dur), 0.0f, 1.0f) : 0.0f;
    ui_px0 = ix + 34;
    ui_px1 = ix + iw - 34;
    ui_py = iy + ih - 7;
    rc_bar_prog = (Rect){ ui_px0, ui_py - 8, ui_px1 - ui_px0, 16 };
    fill_round_rect_aa(screen, ui_px0, ui_py - 1, ui_px1 - ui_px0, 3, 2, C_TRACK_BG);
    int aw = (int)((float)(ui_px1 - ui_px0) * frac);
    if (aw > 0) fill_round_rect_aa(screen, ui_px0, ui_py - 1, aw, 3, 2, C_ACCENT);
    if (island_exp > 0.15f || scrub_drag == 1) {
        fill_ellipse_aa(screen, ui_px0 + aw, ui_py, 4 + (int)(2.0f * island_exp), 4 + (int)(2.0f * island_exp), C_ACCENT);
    }
}

// ---------------- Immersive Fullscreen Water-Ripple Bilingual Lyrics View ----------------
static void draw_immersive_view(int mx, int my) {
    draw_water_ripple_bg(1);

    // Top Floating Pills: Back to Library, Theme, Fullscreen
    rc_back_lib = (Rect){ 26, 20, 128, 38 };
    fill_round_rect_aa_a(screen, rc_back_lib.x, rc_back_lib.y, rc_back_lib.w, rc_back_lib.h, 19,
                         C_SURFACE, in_rc(mx, my, rc_back_lib) ? 205 : 155);
    stroke_round_rect_aa(screen, rc_back_lib.x, rc_back_lib.y, rc_back_lib.w, rc_back_lib.h, 19, 1, C_DIV, col_a(C_DIV) + 12);
    icon_back(screen, rc_back_lib.x + 14, rc_back_lib.y + 12, 14, C_TXT);
    draw_text_bold(screen, font, 13.5f, rc_back_lib.x + 36, rc_back_lib.y + 10, C_TXT, "返回曲库");

    rc_lyr_fs = (Rect){ W - 64, 20, 38, 38 };
    fill_round_rect_aa_a(screen, rc_lyr_fs.x, rc_lyr_fs.y, 38, 38, 19,
                         C_SURFACE, in_rc(mx, my, rc_lyr_fs) ? 205 : 155);
    stroke_round_rect_aa(screen, rc_lyr_fs.x, rc_lyr_fs.y, 38, 38, 19, 1, C_DIV, col_a(C_DIV) + 12);
    icon_expand(screen, rc_lyr_fs.x + 11, rc_lyr_fs.y + 11, 16, C_TXT);

    rc_lyr_theme = (Rect){ W - 112, 20, 38, 38 };
    fill_round_rect_aa_a(screen, rc_lyr_theme.x, rc_lyr_theme.y, 38, 38, 19,
                         C_SURFACE, in_rc(mx, my, rc_lyr_theme) ? 205 : 155);
    stroke_round_rect_aa(screen, rc_lyr_theme.x, rc_lyr_theme.y, 38, 38, 19, 1, C_DIV, col_a(C_DIV) + 12);
    icon_theme_orb(screen, rc_lyr_theme.x + 19, rc_lyr_theme.y + 19, 8, g_is_light ? C_ACCENT : C_TXT);

    // Left Giant Album Art Column + Compact Transport Controls
    int left_col_w = (int)(W * 0.40f);
    if (left_col_w < 340) left_col_w = 340;
    if (left_col_w > 540) left_col_w = 540;
    int cov_sz = (int)(H * 0.44f);
    if (cov_sz > left_col_w - 72) cov_sz = left_col_w - 72;
    if (cov_sz < 200) cov_sz = 200;
    if (cov_sz > 430) cov_sz = 430;
    int cov_x = 36 + (left_col_w - cov_sz) / 2;
    int cov_y = (int)(H * 0.11f);
    if (cov_y < 72) cov_y = 72;

    fill_circle_a(screen, cov_x + cov_sz / 2, cov_y + cov_sz / 2, (int)(cov_sz * 0.56f), C_ACCENT, 34);
    fill_round_rect_shadow(screen, cov_x, cov_y + 10, cov_sz, cov_sz, 24, 130);
    const uint32_t* art = (playing_idx >= 0 && cover_ready && cover_src) ? cover_src : poster_hero;
    if (art) blit_rgb_round_fit(screen, cov_x, cov_y, cov_sz, cov_sz, art, COVER_SRC_SZ, COVER_SRC_SZ, 24);
    else     fill_round_rect_aa(screen, cov_x, cov_y, cov_sz, cov_sz, 24, C_GLASS_ACT);
    stroke_round_rect_aa(screen, cov_x, cov_y, cov_sz, cov_sz, 24, 1, C_DIV, col_a(C_DIV) + 12);

    int info_x = cov_x;
    int info_w = cov_sz;
    int ty = cov_y + cov_sz + 22;
    draw_text_max_bold(screen, font, 26.0f, info_x, ty, C_TXT, p_title, info_w);
    draw_text_max(screen, font, 16.0f, info_x, ty + 34, C_TXT2,
                  p_artist[0] ? p_artist : (p_album[0] ? p_album : "Sirène Hi-Res"), info_w);

    char spec[96];
    snprintf(spec, sizeof(spec), "%s  %dHz  %dch", p_fmt, audio_sample_rate(), audio_channels());
    int spw = text_width(font, 11.5f, spec) + 18;
    fill_round_rect_aa(screen, info_x, ty + 62, spw, 22, 11, C_GLASS);
    stroke_round_rect_aa(screen, info_x, ty + 62, spw, 22, 11, 1, C_DIV, col_a(C_DIV));
    draw_text_centered(screen, font, 11.5f, info_x, info_x + spw, ty + 66, C_ACCENT, spec);

    // Wavy progress bar + timestamps
    double dur = playing_idx >= 0 ? audio_duration() : 0.0;
    double pos = playing_idx >= 0 ? (scrub_drag == 1 ? scrub_sec : audio_position()) : 0.0;
    float frac = (dur > 0.1) ? clampf((float)(pos / dur), 0.0f, 1.0f) : 0.0f;
    int py = ty + 104;
    ui_px0 = info_x;
    ui_px1 = info_x + info_w;
    ui_py = py;
    rc_bar_prog = (Rect){ ui_px0, py - 12, info_w, 24 };
    draw_m3_wavy_progress(screen, ui_px0, ui_px1, py, frac, wave_phase,
                          (!audio_paused() && playing_idx >= 0) ? 4.2f : 1.5f, 22.0f, 4, 4, 1, C_ACCENT, C_TRACK_BG);

    char bpos[20], bdur[20];
    fmt_time(pos, bpos, sizeof(bpos));
    fmt_time(dur, bdur, sizeof(bdur));
    draw_text(screen, font, 12.0f, ui_px0, py + 12, C_TXT2, bpos);
    draw_text_right(screen, font, 12.0f, ui_px1, py + 12, C_TXT2, bdur);

    // Controls below wavy bar
    int cy = py + 52;
    int cx = info_x + info_w / 2;
    rc_bar_play = (Rect){ cx - 26, cy - 26, 52, 52 };
    rc_bar_prev = (Rect){ cx - 76, cy - 18, 36, 36 };
    rc_bar_next = (Rect){ cx + 40, cy - 18, 36, 36 };
    rc_bar_fav  = (Rect){ info_x, cy - 16, 32, 32 };
    rc_bar_mode = (Rect){ info_x + info_w - 48, cy - 15, 48, 30 };

    int is_fav = (playing_idx >= 0 && tracks[playing_idx].fav);
    icon_heart_ex(screen, rc_bar_fav.x + 6, rc_bar_fav.y + 6, 20, is_fav ? C_HEART : C_TXT2, is_fav);
    icon_prev(screen, rc_bar_prev.x + 18, rc_bar_prev.y + 18, 14, C_TXT);
    fill_m3_scallop(screen, cx, cy, in_rc(mx, my, rc_bar_play) ? 27 : 25, 10, scallop_rot, C_ACCENT);
    if (playing_idx >= 0 && !audio_paused()) icon_pause(screen, cx, cy, 13, C_ON_ACCENT);
    else                                     icon_play(screen, cx + 2, cy, 14, C_ON_ACCENT);
    icon_next(screen, rc_bar_next.x + 18, rc_bar_next.y + 18, 14, C_TXT);

    const char* ml = play_mode == MODE_LOOP_ONE ? "单曲" : (play_mode == MODE_SHUFFLE ? "随机" : "顺序");
    fill_round_rect_aa(screen, rc_bar_mode.x, rc_bar_mode.y, 48, 30, 15, C_GLASS);
    stroke_round_rect_aa(screen, rc_bar_mode.x, rc_bar_mode.y, 48, 30, 15, 1, C_DIV, col_a(C_DIV));
    draw_text_centered(screen, font, 12.0f, rc_bar_mode.x, rc_bar_mode.x + 48, rc_bar_mode.y + 7, C_TXT2, ml);

    // Right Column: Large Bilingual Wrapped Lyrics
    int lyr_x = cov_x + cov_sz + 56;
    int lyr_w = W - lyr_x - 48;
    int lyr_y0 = 64, lyr_y1 = H - 36;
    if (lyr_w < 240) { lyr_w = 240; }

    canvas_set_clip(screen, lyr_x - 16, lyr_y0, lyr_x + lyr_w + 16, lyr_y1);

    float orig_px = clampf((float)W * 0.029f, 34.0f, 48.0f);
    float sub_px  = clampf(orig_px * 0.62f, 20.0f, 30.0f);

    if (!lyr || lyr->n <= 0) {
        const char* msg = lyrics_online_busy(g_lyr_req_id) ? "正在从云端匹配双语歌词..." :
                          (playing_idx >= 0 ? "当前曲目暂无内嵌/匹配歌词，可将 .lrc 拖入窗口" : "请先选择一首音乐开始播放");
        draw_text_max_bold(screen, font, 26.0f, lyr_x, H / 2 - 20, C_TXT2, msg, lyr_w);
    } else {
        int cur_idx = lyrics_index(lyr, pos);
        if (cur_idx < 0) cur_idx = 0;

        // Measure line blocks first to center active line smoothly
        int line_ys[512];
        int line_hs[512];
        int max_n = lyr->n < 512 ? lyr->n : 512;
        int cur_acc_y = 0;
        for (int i = 0; i < max_n; i++) {
            char o_lines[4][256], s_lines[3][256];
            const char* orig_s = lyr->lines[i].orig[0] ? lyr->lines[i].orig : lyr->lines[i].text;
            int no = wrap_utf8_lines(orig_s, orig_px, lyr_w - 24, o_lines, 4);
            if (no == 0) no = 1;
            int ns = lyr->lines[i].sub[0] ? wrap_utf8_lines(lyr->lines[i].sub, sub_px, lyr_w - 24, s_lines, 3) : 0;
            int bh = no * (int)(orig_px * 1.28f) + (ns > 0 ? ns * (int)(sub_px * 1.32f) + 6 : 0) + 28;
            line_ys[i] = cur_acc_y;
            line_hs[i] = bh;
            cur_acc_y += bh;
        }

        if (lyr_manual_timer > 0.0f) {
            lyr_manual_timer -= (float)g_dt;
        } else {
            float target_y = (float)line_ys[cur_idx] + (float)line_hs[cur_idx] * 0.35f;
            if (!lyr_cam_valid) { lyr_cam_y = target_y; lyr_cam_valid = 1; }
            else lyr_cam_y += (target_y - lyr_cam_y) * clampf((float)g_dt * 6.5f, 0.04f, 1.0f);
        }

        int anchor_y = lyr_y0 + (int)((lyr_y1 - lyr_y0) * 0.42f);
        for (int i = 0; i < max_n; i++) {
            int y = anchor_y + line_ys[i] - (int)lyr_cam_y;
            if (y + line_hs[i] < lyr_y0 - 40 || y > lyr_y1 + 40) continue;

            int is_cur = (i == cur_idx);
            int dist = abs(i - cur_idx);
            float alpha = is_cur ? 1.0f : clampf(0.54f - (float)dist * 0.09f, 0.14f, 0.52f);
            // Smooth top/bottom edge fade
            if (y < lyr_y0 + 70) alpha *= clampf((float)(y - lyr_y0 + 20) / 90.0f, 0.0f, 1.0f);
            if (y + line_hs[i] > lyr_y1 - 60) alpha *= clampf((float)(lyr_y1 - y) / 90.0f, 0.0f, 1.0f);

            int hov_l = in_rect(mx, my, lyr_x - 10, y - 4, lyr_w, line_hs[i] - 12);
            if (hov_l) alpha = alpha < 0.78f ? 0.78f : alpha;

            if (is_cur) {
                fill_round_rect_aa_a(screen, lyr_x - 14, y - 8, lyr_w + 16, line_hs[i] - 14, 18,
                                     mix_col(C_SURFACE, C_ACCENT, 0.16f), g_is_light ? 160 : 88);
                fill_round_rect_aa(screen, lyr_x - 14, y + 6, 4, line_hs[i] - 42, 2, C_ACCENT);
            } else if (hov_l) {
                fill_round_rect_aa(screen, lyr_x - 14, y - 8, lyr_w + 16, line_hs[i] - 14, 18, C_GLASS);
            }

            uint32_t col_orig = mix_col(C_TXT3, C_TXT, alpha);
            uint32_t col_sub  = mix_col(C_TXT3, is_cur ? C_ACCENT : C_TXT2, alpha * 0.92f);

            char o_lines[4][256], s_lines[3][256];
            const char* orig_s = lyr->lines[i].orig[0] ? lyr->lines[i].orig : lyr->lines[i].text;
            int no = wrap_utf8_lines(orig_s, orig_px, lyr_w - 24, o_lines, 4);
            int cur_y = y;
            for (int k = 0; k < no; k++) {
                if (is_cur) draw_text_bold(screen, font, orig_px, lyr_x, cur_y, col_orig, o_lines[k]);
                else        draw_text(screen, font, orig_px, lyr_x, cur_y, col_orig, o_lines[k]);
                cur_y += (int)(orig_px * 1.28f);
            }
            if (lyr->lines[i].sub[0]) {
                int ns = wrap_utf8_lines(lyr->lines[i].sub, sub_px, lyr_w - 24, s_lines, 3);
                cur_y += 4;
                for (int k = 0; k < ns; k++) {
                    draw_text(screen, font, sub_px, lyr_x, cur_y, col_sub, s_lines[k]);
                    cur_y += (int)(sub_px * 1.32f);
                }
            }

            if (nlyr_hits < LYRIC_MAX_HITS) {
                lyr_hits[nlyr_hits++] = (LyrHit){ (Rect){ lyr_x - 14, y - 8, lyr_w + 16, line_hs[i] - 14 }, lyr->lines[i].t };
            }
        }
    }
    canvas_reset_clip(screen);
}

static void draw_diag_modal(void) {
    if (!show_diag) return;
    int mw = 520, mh = 250;
    int mx = (W - mw) / 2, my = (H - mh) / 2;
    fill_round_rect_shadow(screen, mx, my + 8, mw, mh, 22, 160);
    fill_round_rect_aa_a(screen, mx, my, mw, mh, 22, C_SURFACE, 245);
    stroke_round_rect_aa(screen, mx, my, mw, mh, 22, 1, C_ACCENT, 160);
    draw_text_bold(screen, font, 18.0f, mx + 24, my + 20, C_TXT, "Sirène 音频与歌词诊断");
    char adiag[256] = {0};
    audio_get_diag(adiag, sizeof(adiag));
    draw_text_max(screen, font, 13.0f, mx + 24, my + 60, C_TXT2, adiag, mw - 48);
    draw_text_max(screen, font, 13.0f, mx + 24, my + 96, C_TXT2, lyrics_last_diag(), mw - 48);
    char sc[128];
    snprintf(sc, sizeof(sc), "DPI 缩放: %.2fx  |  窗口逻辑分辨率: %dx%d  |  曲目数: %d",
             g_ui_scale, W, H, ntracks);
    draw_text_max(screen, font, 12.5f, mx + 24, my + 136, C_ACCENT, sc, mw - 48);
    draw_text(screen, font, 12.0f, mx + 24, my + 204, C_TXT3, "提示: 再次点击左下角 i 按钮或按 Esc 关闭");
}

// ---------------- Text Input & Mouse / Hotkey Input Handling ----------------
static void handle_text_input_events(void) {
    char ch[32];
    int act = 0;
    int changed = 0;
    while (be_poll_text_char(ch, sizeof(ch), &act)) {
        if (act == 1) {
            int len = (int)strlen(search_q);
            if (len > 0) {
                int np = utf8_prev_cp(search_q, len);
                search_q[np] = 0;
                changed = 1;
            }
        } else if (act == 2 || act == 3) {
            search_focused = 0;
            be_text_input(0);
        } else if (act == 0 && ch[0]) {
            size_t cur = strlen(search_q);
            size_t add = strlen(ch);
            if (cur + add + 1 < sizeof(search_q)) {
                memcpy(search_q + cur, ch, add + 1);
                changed = 1;
            }
        }
    }
    if (changed) {
        rebuild_visible();
        scroll_px = scroll_target = 0.0f;
    }
}

static void toggle_play_pause_action(void) {
    if (playing_idx >= 0) {
        if (audio_paused()) audio_resume();
        else audio_pause();
    } else if (nvisible > 0) {
        load_track(vis_idx[0]);
    } else if (ntracks > 0) {
        load_track(0);
    }
}

static void update_hover_cursor(int mx, int my) {
    int hand = 0;
    if (in_rc(mx, my, rc_bar_fav) || in_rc(mx, my, rc_bar_prev) || in_rc(mx, my, rc_bar_play) ||
        in_rc(mx, my, rc_bar_next) || in_rc(mx, my, rc_bar_mode) || in_rc(mx, my, rc_bar_lyr) ||
        in_rc(mx, my, rc_bar_cov) || in_rc(mx, my, rc_bar_vol_ico) || in_rc(mx, my, rc_bar_vol_trk) ||
        in_rc(mx, my, rc_bar_prog)) {
        hand = 1;
    } else if (page == PAGE_LYRICS) {
        if (in_rc(mx, my, rc_back_lib) || in_rc(mx, my, rc_lyr_theme) || in_rc(mx, my, rc_lyr_fs)) hand = 1;
        for (int i = 0; i < nlyr_hits && !hand; i++) if (in_rc(mx, my, lyr_hits[i].rc)) hand = 1;
    } else {
        if (in_rc(mx, my, rc_fab_import) || in_rc(mx, my, rc_menu_btn) ||
            in_rc(mx, my, rc_nav_all) || in_rc(mx, my, rc_nav_fav) || in_rc(mx, my, rc_nav_lyr) ||
            in_rc(mx, my, rc_nav_rescan) || in_rc(mx, my, rc_nav_view) || in_rc(mx, my, rc_nav_theme) ||
            in_rc(mx, my, rc_nav_diag) || in_rc(mx, my, rc_search_box) || in_rc(mx, my, rc_sort_btn) ||
            in_rc(mx, my, rc_view_pill) || in_rc(mx, my, rc_fs_btn) || in_rc(mx, my, rc_dir_chip) ||
            in_rc(mx, my, rc_sb_track) || in_rc(mx, my, rc_hero_card)) {
            hand = 1;
        }
        for (int i = 0; i < 5 && !hand; i++) if (in_rc(mx, my, rc_filt_chips[i])) hand = 1;
        for (int i = 0; i < ngrid_hits && !hand; i++) if (in_rc(mx, my, grid_hits[i].card)) hand = 1;
    }
    if (hand != ui_cursor_hand) {
        ui_cursor_hand = hand;
        be_set_cursor(hand);
    }
}

static void handle_input(void) {
    const char* dp = NULL;
    while ((dp = be_poll_dropped_path()) != NULL) handle_dropped(dp);

    handle_text_input_events();

    int wh = be_wheel_delta();
    if (wh != 0) {
        if (page == PAGE_LYRICS) {
            lyr_cam_y -= (float)wh * 48.0f;
            lyr_manual_timer = 4.5f;
        } else {
            scroll_target -= (float)wh * 82.0f;
            float m = max_scroll_px();
            scroll_target = clampf(scroll_target, 0.0f, m);
        }
    }

    int hk = be_poll_hotkey();
    if (hk == 's') {
        if (page != PAGE_LIBRARY) { capture_fade(); page = PAGE_LIBRARY; }
        search_focused = 1;
        be_text_input(1);
    } else if (hk == 'f') {
        open_folder_dialog();
    } else if (hk == 'm') {
        play_mode = (play_mode + 1) % 3;
        save_settings();
        show_toast(play_mode == MODE_LOOP_ONE ? "单曲循环" : (play_mode == MODE_SHUFFLE ? "随机播放" : "顺序循环"));
    } else if (hk == 'c') {
        capture_fade();
        g_is_light = !g_is_light;
        save_settings();
    } else if (hk == 'v') {
        if (page == PAGE_LIBRARY) {
            capture_fade();
            view_grid = !view_grid;
            scroll_px = scroll_target = 0.0f;
            save_settings();
        }
    }

    TouchInfo ti;
    be_touch_update(&ti);
    if (ti.down) {
        spawn_ripple(ti.x, ti.y, C_ACCENT);

        if (search_focused && !in_rc(ti.x, ti.y, rc_search_box)) {
            search_focused = 0;
            be_text_input(0);
        }
        if (show_diag) {
            int mw = 520, mh = 250;
            if (!in_rect(ti.x, ti.y, (W - mw) / 2, (H - mh) / 2, mw, mh)) show_diag = 0;
            else return;
        }

        if (page == PAGE_LIBRARY) {
            if (in_rc(ti.x, ti.y, rc_search_clear) && search_q[0]) {
                search_q[0] = 0;
                rebuild_visible();
                scroll_px = scroll_target = 0.0f;
                return;
            }
            if (in_rc(ti.x, ti.y, rc_search_box)) {
                search_focused = 1;
                be_text_input(1);
                return;
            }
            if (in_rc(ti.x, ti.y, rc_fab_import)) {
                open_folder_dialog();
                return;
            }
        }

        // Dynamic Island / Immersive Player Controls (checked before content beneath!)
        if (in_rc(ti.x, ti.y, rc_bar_vol_ico)) {
            float cur = audio_volume();
            if (cur > 0.01f) { g_saved_vol = cur; audio_set_volume(0.0f); }
            else { audio_set_volume(g_saved_vol > 0.05f ? g_saved_vol : 0.85f); }
            save_settings();
            return;
        }
        if (in_rc(ti.x, ti.y, rc_bar_vol_trk) && ui_vx1 > ui_vx0) {
            vol_drag = 1;
            float f = clampf((float)(ti.x - ui_vx0) / (float)(ui_vx1 - ui_vx0), 0.0f, 1.0f);
            audio_set_volume(f);
            if (f > 0.02f) g_saved_vol = f;
            return;
        }
        if (in_rc(ti.x, ti.y, rc_bar_prog) && playing_idx >= 0 && ui_px1 > ui_px0) {
            double dur = audio_duration();
            if (dur > 0.1) {
                scrub_drag = 1;
                scrub_sec = (double)clampf((float)(ti.x - ui_px0) / (float)(ui_px1 - ui_px0), 0.0f, 1.0f) * dur;
            }
            return;
        }
        if (in_rc(ti.x, ti.y, rc_bar_fav)) {
            if (playing_idx >= 0) toggle_fav(playing_idx);
            return;
        }
        if (in_rc(ti.x, ti.y, rc_bar_prev)) {
            if (ntracks > 0) load_track(pick_prev(playing_idx));
            return;
        }
        if (in_rc(ti.x, ti.y, rc_bar_play)) {
            toggle_play_pause_action();
            return;
        }
        if (in_rc(ti.x, ti.y, rc_bar_next)) {
            if (ntracks > 0) load_track(pick_next(playing_idx, 1));
            return;
        }
        if (in_rc(ti.x, ti.y, rc_bar_mode)) {
            play_mode = (play_mode + 1) % 3;
            save_settings();
            show_toast(play_mode == MODE_LOOP_ONE ? "单曲循环" : (play_mode == MODE_SHUFFLE ? "随机播放" : "顺序循环"));
            return;
        }
        if (in_rc(ti.x, ti.y, rc_bar_lyr) || in_rc(ti.x, ti.y, rc_bar_cov)) {
            capture_fade();
            page = (page == PAGE_LIBRARY) ? PAGE_LYRICS : PAGE_LIBRARY;
            return;
        }
        if (page == PAGE_LIBRARY && in_rc(ti.x, ti.y, rc_island)) {
            return;
        }

        if (page == PAGE_LYRICS) {
            if (in_rc(ti.x, ti.y, rc_back_lib)) { capture_fade(); page = PAGE_LIBRARY; return; }
            if (in_rc(ti.x, ti.y, rc_lyr_theme)) { capture_fade(); g_is_light = !g_is_light; save_settings(); return; }
            if (in_rc(ti.x, ti.y, rc_lyr_fs)) { be_toggle_fullscreen(); return; }
            for (int i = 0; i < nlyr_hits; i++) {
                if (in_rc(ti.x, ti.y, lyr_hits[i].rc)) {
                    audio_seek(lyr_hits[i].t);
                    if (audio_paused()) audio_resume();
                    lyr_manual_timer = 0.0f;
                    return;
                }
            }
            return;
        }

        // Navigation Rail actions
        if (in_rc(ti.x, ti.y, rc_menu_btn) || in_rc(ti.x, ti.y, rc_nav_all)) {
            lib_tab = TAB_ALL; active_dir = -1; fmt_filter = FMT_FILT_ALL;
            rebuild_visible(); scroll_px = scroll_target = 0.0f; return;
        }
        if (in_rc(ti.x, ti.y, rc_nav_fav)) {
            lib_tab = TAB_FAV; rebuild_visible(); scroll_px = scroll_target = 0.0f; return;
        }
        if (in_rc(ti.x, ti.y, rc_nav_lyr)) {
            capture_fade(); page = PAGE_LYRICS; return;
        }
        if (in_rc(ti.x, ti.y, rc_nav_rescan)) {
            scan_library(1); return;
        }
        if (in_rc(ti.x, ti.y, rc_nav_view) || in_rc(ti.x, ti.y, rc_view_pill)) {
            capture_fade(); view_grid = !view_grid; scroll_px = scroll_target = 0.0f; save_settings(); return;
        }
        if (in_rc(ti.x, ti.y, rc_nav_theme)) {
            capture_fade(); g_is_light = !g_is_light; save_settings(); return;
        }
        if (in_rc(ti.x, ti.y, rc_nav_diag)) {
            show_diag = !show_diag; return;
        }

        // Header Controls & Filter Chips
        if (in_rc(ti.x, ti.y, rc_sort_btn)) {
            sort_mode = (sort_mode + 1) % 4;
            rebuild_visible(); save_settings(); return;
        }
        if (in_rc(ti.x, ti.y, rc_fs_btn)) {
            be_toggle_fullscreen(); return;
        }
        if (in_rc(ti.x, ti.y, rc_dir_chip)) {
            if (lib_tab != TAB_DIR) { lib_tab = TAB_DIR; active_dir = 0; }
            else if (active_dir + 1 < nscan_dirs) active_dir++;
            else { lib_tab = TAB_ALL; active_dir = -1; }
            rebuild_visible(); scroll_px = scroll_target = 0.0f; return;
        }
        for (int i = 0; i < 5; i++) {
            if (in_rc(ti.x, ti.y, rc_filt_chips[i])) {
                fmt_filter = i;
                rebuild_visible(); scroll_px = scroll_target = 0.0f; return;
            }
        }
        if (in_rc(ti.x, ti.y, rc_sb_track)) {
            sb_drag = 1;
            float f = clampf((float)(ti.y - rc_sb_track.y) / (float)rc_sb_track.h, 0.0f, 1.0f);
            scroll_target = scroll_px = f * max_scroll_px();
            return;
        }

        // Scrollable Content Area Hits (strictly inside viewport bounds!)
        if (in_rect(ti.x, ti.y, ui_list_x0, ui_list_y0, ui_list_x1 - ui_list_x0, ui_list_y1 - ui_list_y0)) {
            if (in_rc(ti.x, ti.y, rc_hero_prog) && playing_idx >= 0 && ui_hpx1 > ui_hpx0) {
                double dur = audio_duration();
                if (dur > 0.1) {
                    scrub_drag = 2;
                    scrub_sec = (double)clampf((float)(ti.x - ui_hpx0) / (float)(ui_hpx1 - ui_hpx0), 0.0f, 1.0f) * dur;
                }
                return;
            }
            if (in_rc(ti.x, ti.y, rc_hero_play)) {
                toggle_play_pause_action();
                return;
            }
            if (in_rc(ti.x, ti.y, rc_hero_cov) || in_rc(ti.x, ti.y, rc_hero_card)) {
                if (playing_idx >= 0) { capture_fade(); page = PAGE_LYRICS; }
                else if (nvisible > 0) load_track(vis_idx[0]);
                return;
            }
            for (int i = 0; i < ngrid_hits; i++) {
                if (in_rc(ti.x, ti.y, grid_hits[i].fav)) {
                    toggle_fav(grid_hits[i].track_idx);
                    return;
                }
                if (in_rc(ti.x, ti.y, grid_hits[i].card)) {
                    int tidx = grid_hits[i].track_idx;
                    if (tidx == playing_idx) { capture_fade(); page = PAGE_LYRICS; }
                    else load_track(tidx);
                    return;
                }
            }
        }
    }

    if (ti.held) {
        if (scrub_drag == 1 && ui_px1 > ui_px0) {
            double dur = audio_duration();
            float f = clampf((float)(ti.x - ui_px0) / (float)(ui_px1 - ui_px0), 0.0f, 1.0f);
            scrub_sec = (double)f * dur;
        } else if (scrub_drag == 2 && ui_hpx1 > ui_hpx0) {
            double dur = audio_duration();
            float f = clampf((float)(ti.x - ui_hpx0) / (float)(ui_hpx1 - ui_hpx0), 0.0f, 1.0f);
            scrub_sec = (double)f * dur;
        } else if (vol_drag && ui_vx1 > ui_vx0) {
            float f = clampf((float)(ti.x - ui_vx0) / (float)(ui_vx1 - ui_vx0), 0.0f, 1.0f);
            audio_set_volume(f);
            if (f > 0.02f) g_saved_vol = f;
        } else if (sb_drag && rc_sb_track.h > 10) {
            float f = clampf((float)(ti.y - rc_sb_track.y) / (float)rc_sb_track.h, 0.0f, 1.0f);
            scroll_target = scroll_px = f * max_scroll_px();
        }
    }

    if (ti.up) {
        if (scrub_drag) { audio_seek(scrub_sec); scrub_drag = 0; }
        if (vol_drag) { vol_drag = 0; save_settings(); }
        sb_drag = 0;
    }

    uint64_t kd = be_btn_down();
    if (!search_focused && kd) {
        if (kd & BTN_B) {
            if (show_diag) show_diag = 0;
            else if (page == PAGE_LYRICS) { capture_fade(); page = PAGE_LIBRARY; }
        }
        if (kd & BTN_A) toggle_play_pause_action();
        if (kd & BTN_X) { if (playing_idx >= 0) toggle_fav(playing_idx); }
        if (kd & BTN_Y) { capture_fade(); page = (page == PAGE_LIBRARY) ? PAGE_LYRICS : PAGE_LIBRARY; }
        if (kd & BTN_ZL) { if (ntracks > 0) load_track(pick_prev(playing_idx)); }
        if (kd & BTN_ZR) { if (ntracks > 0) load_track(pick_next(playing_idx, 1)); }
        if (kd & BTN_LEFT) {
            if (playing_idx >= 0) {
                double p = audio_position() - 5.0;
                audio_seek(p < 0 ? 0 : p);
            }
        }
        if (kd & BTN_RIGHT) {
            if (playing_idx >= 0) {
                double d = audio_duration(), p = audio_position() + 5.0;
                audio_seek(p > d ? d : p);
            }
        }
        if (kd & BTN_STICK_R_UP)   { audio_set_volume(clampf(audio_volume() + 0.05f, 0, 1)); save_settings(); }
        if (kd & BTN_STICK_R_DOWN) { audio_set_volume(clampf(audio_volume() - 0.05f, 0, 1)); save_settings(); }
        if (kd & BTN_UP) {
            scroll_target = clampf(scroll_target - 140.0f, 0.0f, max_scroll_px());
        }
        if (kd & BTN_DOWN) {
            scroll_target = clampf(scroll_target + 140.0f, 0.0f, max_scroll_px());
        }
    }
}

static void sync_canvas_to_window(void) {
    int pw = 1360, ph = 840;
    float sc = 1.0f;
    be_screen_metrics(&pw, &ph, &sc);
    if (pw < 320) pw = 320;
    if (ph < 240) ph = 240;
    if (sc < 1.0f) sc = 1.0f;
    int lw = (int)((float)pw / sc + 0.5f);
    int lh = (int)((float)ph / sc + 0.5f);
    if (lw < 320) lw = 320;
    if (lh < 240) lh = 240;
    W = lw;
    H = lh;
    g_ui_scale = sc;

    if (!screen || screen->w != pw || screen->h != ph || screen->lw != lw || screen->lh != lh) {
        if (screen) canvas_free(screen);
        screen = canvas_new(pw, ph);
        if (screen) {
            screen->lw = lw;
            screen->lh = lh;
            screen->s  = sc;
        }
        if (trans_shot) { canvas_free(trans_shot); trans_shot = NULL; }
        fade_armed = 0;
    }
}

static void render_frame(int mx, int my) {
    if (!screen) return;
    reset_hit_rects();
    if (page == PAGE_LIBRARY) {
        draw_water_ripple_bg(0);
        draw_nav_rail(mx, my);
        draw_library_header(mx, my);
        if (view_grid) draw_grid_view(mx, my);
        else           draw_table_view(mx, my);
        draw_dynamic_island(mx, my);
    } else {
        draw_immersive_view(mx, my);
    }
    draw_diag_modal();

    if (toast_timer > 0 && toast_text[0]) {
        int tw = text_width(font, 13.0f, toast_text) + 32;
        int tx = (W - tw) / 2, ty = (page == PAGE_LYRICS) ? 20 : 66;
        uint8_t a = toast_timer < 20 ? (uint8_t)(toast_timer * 11) : 230;
        fill_round_rect_shadow(screen, tx, ty + 3, tw, 34, 17, (uint8_t)(a / 2));
        fill_round_rect_aa_a(screen, tx, ty, tw, 34, 17, C_SURFACE, a);
        stroke_round_rect_aa(screen, tx, ty, tw, 34, 17, 1, C_ACCENT, a);
        draw_text_centered(screen, font, 13.0f, tx, tx + tw, ty + 9, C_TXT, toast_text);
        toast_timer--;
    }

    if (fade_armed && trans_shot && fade_time > 0.0f &&
        trans_shot->w == screen->w && trans_shot->h == screen->h) {
        float t = clampf(fade_time / FADE_DUR, 0.0f, 1.0f);
        blend_over_opaque(screen->px, trans_shot->px, (size_t)screen->w * screen->h, (uint8_t)(t * 255.0f));
        fade_time -= (float)g_dt;
        if (fade_time <= 0.0f) fade_armed = 0;
    }
}

static void dump_rgba_shot(const char* dir, const char* name) {
    if (!dir || !*dir || !screen) return;
    ensure_dir_recursive(dir);
    char path[640];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE* f = plat_fopen(path, "wb");
    if (!f) return;
    uint32_t hdr[2] = { (uint32_t)screen->w, (uint32_t)screen->h };
    fwrite(hdr, sizeof(uint32_t), 2, f);
    fwrite(screen->px, 4, (size_t)screen->w * screen->h, f);
    fclose(f);
}

int main(int argc, char** argv) {
    if (argc > 1) be_set_cli_path(argv[1]);
    if (be_init() != 0) return 1;
    audio_init();

    tracks = (Track*)calloc(MAX_TRACKS, sizeof(Track));
    vis_idx = (int*)calloc(MAX_TRACKS, sizeof(int));
    font = font_load(font_ttf_start, (size_t)(font_ttf_end - font_ttf_start));
    build_posters();

    ensure_dir_recursive(be_config_dir());
    char lrc_cache[600];
    snprintf(lrc_cache, sizeof(lrc_cache), "%s/lyrics_cache", be_config_dir());
    ensure_dir_recursive(lrc_cache);
    lyrics_set_cache_dir(lrc_cache);

    load_settings();
    load_dirs_cfg();
    step_palette_and_water_ripples();
    scan_library(0);
    for (int i = 0; i < 16 && i < nvisible; i++) ensure_track_meta(vis_idx[i], 1);

    const char* dump_dir = getenv("SIRENE_DUMP_DIR");
    int dump_mode = (dump_dir && *dump_dir && dump_dir[0] != '0') ? 1 : 0;
    int dump_frame = 0;

    g_last_ns = plat_ticks_ns();
    while (be_should_run()) {
        uint64_t now = plat_ticks_ns();
        double dt = (double)(now - g_last_ns) / 1e9;
        g_last_ns = now;
        if (dt < 0.002) dt = 0.002;
        if (dt > 0.08) dt = 0.08;
        g_dt = dt;

        be_input_update();
        sync_canvas_to_window();
        audio_pump();

        if (audio_finished() && ntracks > 0) {
            load_track(pick_next(playing_idx, 0));
        }
        if (g_lyr_req_id > 0) {
            Lyrics* ol = NULL;
            if (lyrics_poll_online(g_lyr_req_id, &ol) != 0) {
                g_lyr_req_id = 0;
                if (ol) {
                    if (lyr) lyrics_free(lyr);
                    lyr = ol;
                    lyr_cam_valid = 0;
                }
            }
        }

        // Advance animations
        float live_bars[28];
        audio_get_spectrum(live_bars);
        float k_bar = clampf((float)g_dt * 14.0f, 0.08f, 1.0f);
        for (int i = 0; i < 28; i++) bars[i] += (live_bars[i] - bars[i]) * k_bar;

        int playing_active = (playing_idx >= 0 && !audio_paused());
        wave_phase += (float)g_dt * (playing_active ? 2.8f : 0.7f);
        ripple_time += (float)g_dt * (playing_active ? 1.15f : 0.45f);
        scallop_rot += (float)g_dt * (playing_active ? 0.55f : 0.12f);
        if (hov_card_idx >= 0) hov_card_t = clampf(hov_card_t + (float)g_dt * 9.0f, 0.0f, 1.0f);

        float mscr = max_scroll_px();
        scroll_target = clampf(scroll_target, 0.0f, mscr);
        scroll_px += (scroll_target - scroll_px) * clampf((float)g_dt * 16.0f, 0.1f, 1.0f);

        step_palette_and_water_ripples();
        pump_background_meta();

        if (dump_mode) {
            if (dump_frame == 2) {
                if (playing_idx < 0 && nvisible > 0) load_track(vis_idx[0]);
                audio_set_volume(0.0f);
                if (playing_idx >= 0) audio_seek(58.0);
            }
            if (dump_frame == 28) {
                int mx = 0, my = 0;
                render_frame(mx, my);
                dump_rgba_shot(dump_dir, "shot_lib.rgba");
                page = PAGE_LYRICS;
                fade_armed = 0;
            }
            if (dump_frame == 50) {
                int mx = 0, my = 0;
                render_frame(mx, my);
                dump_rgba_shot(dump_dir, "shot_lyr.rgba");
                break;
            }
            dump_frame++;
        }

        handle_input();
        int mx = 0, my = 0;
        be_mouse_pos(&mx, &my);
        update_hover_cursor(mx, my);
        render_frame(mx, my);

        int stride = 0;
        uint32_t* fb = be_begin(&stride);
        if (fb && screen && stride > 0) {
            int cw = screen->w < stride ? screen->w : stride;
            for (int y = 0; y < screen->h; y++) {
                memcpy(fb + (size_t)y * stride, screen->px + (size_t)y * screen->w, (size_t)cw * 4);
            }
        }
        be_end();
        be_sleep_ms(1);
    }

    free_all_thumbs();
    meta_clear(&meta);
    if (lyr) lyrics_free(lyr);
    free(cover_src); free(cover_mini);
    free(poster_mini); free(poster_hero); free(poster_ico);
    if (bg_mesh) canvas_free(bg_mesh);
    if (trans_shot) canvas_free(trans_shot);
    if (screen) canvas_free(screen);
    if (font) font_free(font);
    free(tracks); free(vis_idx);
    audio_exit();
    be_exit();
    return 0;
}
