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
#define MAX_THUMBS    640
#define THUMB_SZ      42
#define MINI_ART_SZ   54
#define COVER_SRC_SZ  360
#define ROW_H         54
#define BOT_H         86

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

enum { PAGE_LIBRARY = 0, PAGE_LYRICS = 1 };
enum { TAB_ALL = 0, TAB_FAV = 1, TAB_DIR = 2 };
enum { MODE_LOOP_ALL = 0, MODE_LOOP_ONE = 1, MODE_SHUFFLE = 2 };
enum { PC_SORT_DEFAULT = 0, PC_SORT_TITLE = 1, PC_SORT_ARTIST = 2, PC_SORT_ALBUM = 3 };

// ---------------- App State ----------------
static Canvas* screen = NULL;
static Canvas* trans_shot = NULL;
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
static int sort_mode = SORT_DEFAULT;
static int sidebar_collapsed = 0;
static int show_diag = 0;
static int search_focused = 0;
static char search_q[160] = {0};

static int playing_idx = -1;
static int sel_idx = -1;
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

static Canvas* mesh_lib = NULL;
static Canvas* mesh_lyr = NULL;
static uint32_t cov_q[4] = {0};
static uint32_t seed_rng = 0x9E3779B9u;

// Smooth animation / scrub / timing
static double g_dt = 1.0 / 60.0;
static uint64_t g_last_ns = 0;
static float fade_time = 0.0f;
static int   fade_armed = 0;
#define FADE_DUR 0.20f

static float scroll_px = 0.0f, scroll_target = 0.0f;
static float lyr_cam_y = 0.0f, lyr_cam_valid = 0;
static float lyr_manual_timer = 0.0f;
static float wave_phase = 0.0f, bars[28] = {0};
static int   ui_cursor_hand = 0;
static int   probe_cursor = 0;

static int scrub_drag = 0;
static double scrub_sec = 0.0;
static int vol_drag = 0;
static int sb_drag = 0;
static int toast_timer = 0;
static char toast_text[160] = {0};

// Palette colors
static uint32_t C_BASE, C_SIDE, C_SURFACE, C_CARD_HOV, C_CARD_ACT;
static uint32_t C_ACCENT, C_ON_ACCENT, C_ACCENT_SOFT;
static uint32_t C_TXT, C_TXT2, C_TXT3, C_DIV, C_TRACK_BG, C_HEART, C_PLAY_BTN, C_PLAY_ICO;

// Interactive rects updated each frame
static Rect rc_menu_btn, rc_nav_all, rc_nav_fav, rc_nav_lyr, rc_nav_add, rc_nav_rescan;
static Rect rc_nav_theme, rc_nav_diag;
static Rect rc_dir_items[MAX_DIRS];
static Rect rc_search_box, rc_search_clear, rc_sort_btn, rc_play_all, rc_fs_btn;
static Rect rc_sb_track, rc_back_lib, rc_lyr_theme, rc_lyr_fs;
static Rect rc_bar_cov, rc_bar_fav, rc_bar_rew, rc_bar_prev, rc_bar_play, rc_bar_next, rc_bar_fwd;
static Rect rc_bar_mode, rc_bar_lyr, rc_bar_vol_ico, rc_bar_vol_trk, rc_bar_prog;
static int  ui_px0 = 0, ui_px1 = 100, ui_py = 0;
static int  ui_vx0 = 0, ui_vx1 = 100;
static int  ui_list_x0 = 0, ui_list_x1 = 0, ui_list_y0 = 0, ui_list_y1 = 0;

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

// ---------------- Path / Format Helpers ----------------
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
    float hue = (float)(h % 360);
    float rad = hue * (float)M_PI / 180.0f;
    uint8_t r0 = u8clamp((int)(88 + 46 * sinf(rad)));
    uint8_t g0 = u8clamp((int)(96 + 44 * sinf(rad + 2.1f)));
    uint8_t b0 = u8clamp((int)(142 + 48 * sinf(rad + 4.2f)));
    for (int y = 0; y < n; y++) {
        float fy = (float)y / (float)n;
        for (int x = 0; x < n; x++) {
            float fx = (float)x / (float)n;
            uint8_t r = u8clamp((int)(r0 * (1.0f - 0.35f * fy) + 26 * fx));
            uint8_t g = u8clamp((int)(g0 * (1.0f - 0.30f * fy) + 18 * fx));
            uint8_t b = u8clamp((int)(b0 * (1.0f - 0.20f * fy) + 32 * fx));
            dst[(size_t)y * n + x] = COL(r, g, b, 255);
        }
    }
}

// ---------------- Theme & Ambient Gradient Engine ----------------
static void update_palette(void) {
    uint32_t base_cov = cover_ready ? mix_col(cov_q[0], cov_q[3], 0.5f) : COL(18, 198, 214, 255);
    if (!g_is_light) {
        C_BASE        = COL(13, 15, 21, 255);
        C_SIDE        = COL(18, 20, 28, 255);
        C_SURFACE     = COL(28, 31, 41, 255);
        C_CARD_HOV    = COL(255, 255, 255, 18);
        C_CARD_ACT    = COL(255, 255, 255, 32);
        C_ACCENT      = COL(0, 212, 224, 255);
        C_ON_ACCENT   = COL(8, 16, 20, 255);
        C_ACCENT_SOFT = mix_col(C_SIDE, C_ACCENT, 0.24f);
        C_TXT         = COL(245, 247, 251, 255);
        C_TXT2        = COL(166, 173, 188, 255);
        C_TXT3        = COL(110, 117, 134, 255);
        C_DIV         = COL(255, 255, 255, 22);
        C_TRACK_BG    = COL(255, 255, 255, 42);
        C_HEART       = COL(240, 74, 96, 255);
        C_PLAY_BTN    = COL(246, 248, 252, 255);
        C_PLAY_ICO    = COL(14, 16, 22, 255);
    } else {
        C_BASE        = COL(244, 246, 251, 255);
        C_SIDE        = COL(236, 240, 248, 255);
        C_SURFACE     = COL(255, 255, 255, 255);
        C_CARD_HOV    = COL(0, 0, 0, 12);
        C_CARD_ACT    = COL(27, 110, 243, 24);
        C_ACCENT      = cover_ready ? mix_col(base_cov, COL(24, 108, 238, 255), 0.45f) : COL(27, 110, 243, 255);
        uint8_t ar = C_ACCENT & 0xFF, ag = (C_ACCENT >> 8) & 0xFF, ab = (C_ACCENT >> 16) & 0xFF;
        float lum = (0.299f * ar + 0.587f * ag + 0.114f * ab) / 255.0f;
        if (lum > 0.56f) C_ACCENT = mix_col(C_ACCENT, COL(20, 72, 180, 255), 0.50f);
        C_ON_ACCENT   = COL(255, 255, 255, 255);
        C_ACCENT_SOFT = mix_col(COL(255, 255, 255, 255), C_ACCENT, 0.14f);
        C_TXT         = COL(20, 24, 34, 255);
        C_TXT2        = COL(92, 100, 118, 255);
        C_TXT3        = COL(140, 148, 166, 255);
        C_DIV         = COL(0, 0, 0, 20);
        C_TRACK_BG    = COL(0, 0, 0, 32);
        C_HEART       = COL(228, 60, 82, 255);
        C_PLAY_BTN    = C_ACCENT;
        C_PLAY_ICO    = COL(255, 255, 255, 255);
    }
    be_set_dark_titlebar(!g_is_light);
}

static void rebuild_ambient_meshes(void) {
    if (!mesh_lib) mesh_lib = canvas_new(64, 36);
    if (!mesh_lyr) mesh_lyr = canvas_new(64, 36);
    if (!mesh_lib || !mesh_lyr) return;

    if (cover_ready && cover_src) {
        // Sample 4 quadrants for rich multi-tone ambient mesh (like Apple Music / Salt Player)
        for (int q = 0; q < 4; q++) {
            int qx = (q & 1) ? COVER_SRC_SZ / 2 : 0;
            int qy = (q & 2) ? COVER_SRC_SZ / 2 : 0;
            uint64_t sr = 0, sg = 0, sb = 0, cnt = 0;
            for (int y = qy + 24; y < qy + COVER_SRC_SZ / 2 - 24; y += 8) {
                for (int x = qx + 24; x < qx + COVER_SRC_SZ / 2 - 24; x += 8) {
                    uint32_t c = cover_src[(size_t)y * COVER_SRC_SZ + x];
                    sr += c & 0xFF; sg += (c >> 8) & 0xFF; sb += (c >> 16) & 0xFF; cnt++;
                }
            }
            if (cnt > 0) cov_q[q] = COL((uint8_t)(sr / cnt), (uint8_t)(sg / cnt), (uint8_t)(sb / cnt), 255);
        }
    } else {
        cov_q[0] = COL(24, 52, 82, 255);
        cov_q[1] = COL(58, 32, 74, 255);
        cov_q[2] = COL(20, 36, 56, 255);
        cov_q[3] = COL(36, 56, 86, 255);
    }
    update_palette();

    uint32_t small_src[64 * 36];
    if (cover_ready && cover_src) {
        resample_bilinear(cover_src, COVER_SRC_SZ, COVER_SRC_SZ, small_src, 64, 36);
        box_blur(small_src, 64, 36, 18, 3);
    }

    for (int y = 0; y < 36; y++) {
        float fy = (float)y / 35.0f;
        for (int x = 0; x < 64; x++) {
            float fx = (float)x / 63.0f;
            uint32_t top = mix_col(cov_q[0], cov_q[1], fx);
            uint32_t bot = mix_col(cov_q[2], cov_q[3], fx);
            uint32_t quad = mix_col(top, bot, fy);
            uint32_t cov_col = (cover_ready && cover_src) ? mix_col(quad, small_src[(size_t)y * 64 + x], 0.28f) : quad;
            if (!g_is_light) {
                uint8_t r = cov_col & 0xFF, g = (cov_col >> 8) & 0xFF, b = (cov_col >> 16) & 0xFF;
                float lum = (0.299f * r + 0.587f * g + 0.114f * b) / 255.0f;
                float k_lyr = clampf(0.66f - lum * 0.34f, 0.30f, 0.64f);
                mesh_lyr->px[(size_t)y * 64 + x] = mix_col(COL(16, 14, 22, 255), cov_col, k_lyr);
                mesh_lib->px[(size_t)y * 64 + x] = mix_col(COL(14, 16, 22, 255), cov_col, 0.24f);
            } else {
                mesh_lyr->px[(size_t)y * 64 + x] = mix_col(COL(242, 245, 251, 255), cov_col, 0.26f);
                mesh_lib->px[(size_t)y * 64 + x] = mix_col(COL(245, 247, 252, 255), cov_col, 0.08f);
            }
        }
    }
}

static void draw_ambient_bg(Canvas* mesh) {
    if (!mesh || !screen) return;
    resample_bilinear(mesh->px, 64, 36, screen->px, screen->w, screen->h);
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
        } else if (strncmp(line, "side_col=", 9) == 0) {
            sidebar_collapsed = atoi(line + 9) ? 1 : 0;
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
    fprintf(f, "vol=%.3f\nmode=%d\nlight=%d\nside_col=%d\nsort=%d\n",
            audio_volume(), play_mode, g_is_light, sidebar_collapsed, sort_mode);
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
    for (int i = 0; i < nscan_dirs; i++) {
        if (strcasecmp(scan_dirs[i], norm) == 0) return i;
    }
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
        for (int i = 0; i < ntracks; i++) {
            if (strcmp(tracks[i].path, buf) == 0) { tracks[i].fav = 1; break; }
        }
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

// ---------------- Library Scan, Filtering, Metadata ----------------
static int add_track_path(const char* path, int dir_idx) {
    if (!path || !*path || ntracks >= MAX_TRACKS) return -1;
    for (int i = 0; i < ntracks; i++) {
        if (strcmp(tracks[i].path, path) == 0) return i;
    }
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
    int ia = *(const int*)a;
    int ib = *(const int*)b;
    const Track* ta = &tracks[ia];
    const Track* tb = &tracks[ib];
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

static void rebuild_visible(void) {
    nvisible = 0;
    for (int i = 0; i < ntracks; i++) {
        if (lib_tab == TAB_FAV && !tracks[i].fav) continue;
        if (lib_tab == TAB_DIR && active_dir >= 0 && tracks[i].dir_idx != active_dir) continue;
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
        free(tracks[i].thumb);
        tracks[i].thumb = NULL;
        tracks[i].meta_state = 0;
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
        for (int i = 0; i < ntracks; i++) {
            if (strcmp(tracks[i].path, prev_play) == 0) { playing_idx = i; break; }
        }
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
        poster_ico  = (uint32_t*)calloc(28 * 28, 4);
        if (poster_mini) resample_cover_square((const uint32_t*)poster_start, 320, 320, poster_mini, MINI_ART_SZ, 1);
        if (poster_hero) resample_cover_square((const uint32_t*)poster_start, 320, 320, poster_hero, COVER_SRC_SZ, 1);
        if (poster_ico)  resample_cover_square((const uint32_t*)poster_start, 320, 320, poster_ico, 28, 1);
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
        int i0 = x - 1, i1 = x + sz + 1, j0 = y - 1, j1 = y + sz + 1;
        for (int j = j0; j <= j1; j++) {
            float py = -(j + 0.5f - cy) / hs;
            for (int i = i0; i <= i1; i++) {
                float px = (i + 0.5f - cx) / hs;
                float py_adj = py - 0.55f * sqrtf(fabsf(px));
                float d = (px * px + py_adj * py_adj - 1.0f);
                if (fabsf(d) < 0.30f) {
                    float a = 1.0f - fabsf(d) / 0.30f;
                    fill_rect_a(c, i, j, 1, 1, col, (uint8_t)(a * 210.0f));
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
static void icon_vol(Canvas* c, int cx, int cy, int sz, uint32_t col, int muted) {
    fill_round_rect_aa(c, cx - sz / 2, cy - sz / 5, sz / 4 + 1, (sz * 2) / 5 + 1, 1, col);
    fill_tri_aa(c, cx - sz / 3, cy, cx + 1, cy - sz / 2 + 1, cx + 1, cy + sz / 2 - 1, col);
    if (!muted) {
        stroke_circle_aa(c, cx + 2, cy, sz / 3, 2, col, 185);
    } else {
        fill_round_rect_aa(c, cx + 3, cy - 1, sz / 2 - 2, 2, 1, C_HEART);
    }
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
    if (lib_tab == TAB_FAV) rebuild_visible();
}

static void load_track(int idx) {
    if (idx < 0 || idx >= ntracks) return;
    scrub_drag = 0; scrub_sec = 0.0;
    lyr_cam_valid = 0; lyr_manual_timer = 0.0f;
    capture_fade();
    playing_idx = idx;
    sel_idx = idx;
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
        rebuild_ambient_meshes();
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
// ---------------- Word Wrap Helper (UTF-8 aware for CJK + English) ----------------
typedef struct {
    char lines[3][192];
    int count;
} WrapLines;

static void wrap_utf8_lines(Font* f, float px, const char* txt, int max_w, WrapLines* out) {
    out->count = 0;
    if (!txt || !*txt || max_w < 60 || !f) return;
    const char* p = txt;
    while (*p && out->count < 3) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char* start = p;
        const char* last_fit = p;
        const char* last_space = NULL;
        char buf[192];
        while (*p) {
            const char* ch = p;
            unsigned char c = (unsigned char)*p;
            int clen = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
            const char* nxt = p;
            for (int k = 0; k < clen && *nxt; k++) nxt++;
            size_t len = (size_t)(nxt - start);
            if (len >= sizeof(buf) - 1) break;
            memcpy(buf, start, len);
            buf[len] = 0;
            if (text_width(f, px, buf) > max_w) {
                if (last_fit == start) last_fit = nxt;
                break;
            }
            if (*ch == ' ') last_space = ch;
            last_fit = nxt;
            p = nxt;
        }
        const char* end = last_fit;
        if (*p && last_space && last_space > start && (unsigned char)end[-1] < 0x80) {
            end = last_space;
        }
        size_t flen = (size_t)(end - start);
        if (flen >= sizeof(out->lines[0])) flen = sizeof(out->lines[0]) - 1;
        memcpy(out->lines[out->count], start, flen);
        out->lines[out->count][flen] = 0;
        out->count++;
        p = (*end == ' ') ? (end + 1) : end;
        if (out->count == 3 && *p) {
            size_t n = strlen(out->lines[2]);
            if (n + 4 < sizeof(out->lines[2])) snprintf(out->lines[2] + n, sizeof(out->lines[2]) - n, "…");
        }
    }
}

// ---------------- Scroll & Layout Helpers ----------------
static int list_vis_h(void) {
    int h = ui_list_y1 - ui_list_y0;
    return h > 40 ? h : 40;
}

static float max_scroll_px(void) {
    float tot = (float)nvisible * (float)ROW_H;
    float m = tot - (float)list_vis_h();
    return m > 0.0f ? m : 0.0f;
}

static void pump_background_meta(void) {
    int i0 = (int)(scroll_px / (float)ROW_H) - 2;
    if (i0 < 0) i0 = 0;
    int rows = list_vis_h() / ROW_H + 5;
    int i1 = i0 + rows;
    if (i1 > nvisible) i1 = nvisible;

    // Prioritize visible rows: 2 for quick text/art, 1 for duration probe
    for (int p = i0; p < i1; p++) {
        int idx = vis_idx[p];
        if (tracks[idx].meta_state == 0) {
            ensure_track_meta(idx, 0);
            break;
        }
    }
    if (nvisible > 0 && tracks[0].path[0]) {
        if (probe_cursor >= ntracks) probe_cursor = 0;
        for (int k = 0; k < 2 && probe_cursor < ntracks; k++, probe_cursor++) {
            if (tracks[probe_cursor].meta_state < 2) {
                ensure_track_meta(probe_cursor, 1);
            }
        }
    }
    trim_thumbs(i0 + rows / 2);
}

static void reset_hit_rects(void) {
    Rect z = {0, 0, 0, 0};
    rc_menu_btn = rc_nav_all = rc_nav_fav = rc_nav_lyr = rc_nav_add = rc_nav_rescan = z;
    rc_nav_theme = rc_nav_diag = rc_search_box = rc_search_clear = rc_sort_btn = z;
    rc_play_all = rc_fs_btn = rc_sb_track = rc_back_lib = rc_lyr_theme = rc_lyr_fs = z;
    rc_bar_cov = rc_bar_fav = rc_bar_rew = rc_bar_prev = rc_bar_play = rc_bar_next = rc_bar_fwd = z;
    rc_bar_mode = rc_bar_lyr = rc_bar_vol_ico = rc_bar_vol_trk = rc_bar_prog = z;
    for (int i = 0; i < MAX_DIRS; i++) rc_dir_items[i] = z;
    nlyr_hits = 0;
}

// ---------------- Bottom Transport Bar (Library View) ----------------
static void draw_bottom_bar(int mx, int my, double pos, double dur) {
    int by = H - BOT_H;
    fill_rect_a(screen, 0, by, W, BOT_H, g_is_light ? COL(252, 253, 255, 255) : COL(18, 20, 27, 255), 240);
    fill_rect(screen, 0, by, W, 1, C_DIV);

    // Top-edge full-width interactive progress bar (Image 2 style + M3 wavy highlight)
    char t1[24], t2[24];
    fmt_time(pos, t1, sizeof(t1));
    fmt_time(dur, t2, sizeof(t2));
    draw_text(screen, font, 11.5f, 16, by + 5, C_TXT2, t1);
    draw_text_right(screen, font, 11.5f, W - 16, by + 5, C_TXT2, t2);

    ui_px0 = 64;
    ui_px1 = W - 64;
    ui_py = by + 2;
    rc_bar_prog = (Rect){ ui_px0 - 10, by - 8, (ui_px1 - ui_px0) + 20, 22 };
    float frac = (dur > 0.0) ? clampf((float)(pos / dur), 0.0f, 1.0f) : 0.0f;
    float wamp = (playing_idx >= 0 && !audio_paused() && dur > 0.0) ? 2.2f : 0.0f;
    int prog_hot = scrub_drag || in_rc(mx, my, rc_bar_prog);
    draw_m3_wavy_progress(screen, ui_px0, ui_px1, ui_py + 2, frac, wave_phase,
                          wamp, 20.0f, prog_hot ? 5 : 4, 3, prog_hot ? 1 : 0, C_ACCENT, C_TRACK_BG);
    if (!prog_hot && dur > 0.0) {
        int kx = ui_px0 + (int)(frac * (float)(ui_px1 - ui_px0) + 0.5f);
        fill_ellipse_aa(screen, kx, ui_py + 2, 5, 5, C_TXT);
    }

    int cy = by + 22;
    // Left: Cover + Title + Artist/Spec + Heart
    int cov_x = 18, cov_y = cy, cov_sz = MINI_ART_SZ;
    rc_bar_cov = (Rect){ cov_x, cov_y, cov_sz, cov_sz };
    const uint32_t* mimg = (playing_idx >= 0 && cover_mini) ? cover_mini : poster_mini;
    if (mimg) blit_rgb_round(screen, cov_x, cov_y, mimg, MINI_ART_SZ, MINI_ART_SZ, 10);
    else fill_round_rect_aa(screen, cov_x, cov_y, cov_sz, cov_sz, 10, C_SURFACE);
    if (in_rc(mx, my, rc_bar_cov)) {
        fill_round_rect_aa_a(screen, cov_x, cov_y, cov_sz, cov_sz, 10, COL(0, 0, 0, 255), 110);
        icon_expand(screen, cov_x + 18, cov_y + 18, 18, COL(255, 255, 255, 255));
    }

    int info_x = cov_x + cov_sz + 14;
    int ctr_cx = W / 2;
    int max_info_w = ctr_cx - 170 - info_x - 36;
    if (max_info_w < 80) max_info_w = 80;
    if (playing_idx >= 0) {
        draw_text_max_bold(screen, font, 15.5f, info_x, cov_y + 5, C_TXT, p_title, max_info_w);
        char sub[320];
        uint32_t sr = audio_sample_rate();
        if (p_artist[0]) snprintf(sub, sizeof(sub), "%s · %s %uHz", p_artist, p_fmt, (unsigned)sr);
        else snprintf(sub, sizeof(sub), "%s %uHz", p_fmt, (unsigned)sr);
        draw_text_max(screen, font, 12.5f, info_x, cov_y + 30, C_TXT2, sub, max_info_w);

        int hx = info_x + text_width_max(font, 15.5f, p_title, max_info_w) + 12;
        if (hx > ctr_cx - 185) hx = ctr_cx - 185;
        rc_bar_fav = (Rect){ hx - 6, cov_y + 2, 28, 28 };
        int fav = tracks[playing_idx].fav;
        if (in_rc(mx, my, rc_bar_fav)) fill_circle_a(screen, hx + 8, cov_y + 15, 14, C_CARD_HOV, 255);
        icon_heart_ex(screen, hx, cov_y + 7, 16, fav ? C_HEART : C_TXT2, fav);
    } else {
        draw_text_bold(screen, font, 15.5f, info_x, cov_y + 6, C_TXT, "Sirène 桌面播放器");
        draw_text(screen, font, 12.5f, info_x, cov_y + 30, C_TXT3, "点击任意曲目开始播放 · 支持自动匹配在线歌词");
    }

    // Center: Transport Controls (-10, Prev, Play/Pause, Next, +10)
    int py_c = by + 51;
    rc_bar_rew  = (Rect){ ctr_cx - 118, py_c - 18, 36, 36 };
    rc_bar_prev = (Rect){ ctr_cx - 72,  py_c - 18, 36, 36 };
    rc_bar_play = (Rect){ ctr_cx - 22,  py_c - 22, 44, 44 };
    rc_bar_next = (Rect){ ctr_cx + 36,  py_c - 18, 36, 36 };
    rc_bar_fwd  = (Rect){ ctr_cx + 82,  py_c - 18, 36, 36 };

    if (in_rc(mx, my, rc_bar_rew))  fill_circle_a(screen, rc_bar_rew.x + 18, py_c, 17, C_CARD_HOV, 255);
    if (in_rc(mx, my, rc_bar_prev)) fill_circle_a(screen, rc_bar_prev.x + 18, py_c, 17, C_CARD_HOV, 255);
    if (in_rc(mx, my, rc_bar_next)) fill_circle_a(screen, rc_bar_next.x + 18, py_c, 17, C_CARD_HOV, 255);
    if (in_rc(mx, my, rc_bar_fwd))  fill_circle_a(screen, rc_bar_fwd.x + 18, py_c, 17, C_CARD_HOV, 255);

    draw_text_centered(screen, font, 12.0f, rc_bar_rew.x, rc_bar_rew.x + 36, py_c - 7, C_TXT2, "-10");
    icon_prev(screen, rc_bar_prev.x + 18, py_c, 14, C_TXT);
    fill_ellipse_aa(screen, ctr_cx, py_c, 22, 22, in_rc(mx, my, rc_bar_play) ? mix_col(C_PLAY_BTN, C_ACCENT, 0.25f) : C_PLAY_BTN);
    if (playing_idx >= 0 && !audio_paused()) icon_pause(screen, ctr_cx, py_c, 16, C_PLAY_ICO);
    else icon_play(screen, ctr_cx + 1, py_c, 16, C_PLAY_ICO);
    icon_next(screen, rc_bar_next.x + 18, py_c, 14, C_TXT);
    draw_text_centered(screen, font, 12.0f, rc_bar_fwd.x, rc_bar_fwd.x + 36, py_c - 7, C_TXT2, "+10");

    // Right: Volume, Mode, Lyrics Button ("词")
    int rx = W - 20;
    rc_bar_lyr = (Rect){ rx - 40, py_c - 15, 40, 30 };
    int lyr_has = (lyr && lyr->n > 0);
    fill_round_rect_aa(screen, rc_bar_lyr.x, rc_bar_lyr.y, rc_bar_lyr.w, rc_bar_lyr.h, 8,
                       in_rc(mx, my, rc_bar_lyr) ? C_ACCENT_SOFT : (lyr_has ? C_CARD_ACT : C_CARD_HOV));
    draw_text_centered(screen, font, 14.0f, rc_bar_lyr.x, rc_bar_lyr.x + rc_bar_lyr.w, py_c - 8,
                       lyr_has ? C_ACCENT : C_TXT2, "词");

    const char* mnames[3] = { "列表", "单曲", "随机" };
    rc_bar_mode = (Rect){ rc_bar_lyr.x - 58, py_c - 15, 50, 30 };
    if (in_rc(mx, my, rc_bar_mode)) fill_round_rect_aa(screen, rc_bar_mode.x, rc_bar_mode.y, rc_bar_mode.w, rc_bar_mode.h, 8, C_CARD_HOV);
    draw_text_centered(screen, font, 12.5f, rc_bar_mode.x, rc_bar_mode.x + 50, py_c - 7,
                       play_mode != MODE_LOOP_ALL ? C_ACCENT : C_TXT2, mnames[play_mode]);

    if (W >= 980) {
        ui_vx1 = rc_bar_mode.x - 14;
        ui_vx0 = ui_vx1 - 92;
        rc_bar_vol_trk = (Rect){ ui_vx0 - 6, py_c - 14, (ui_vx1 - ui_vx0) + 12, 28 };
        rc_bar_vol_ico = (Rect){ ui_vx0 - 34, py_c - 15, 28, 30 };
        float vol = audio_volume();
        if (in_rc(mx, my, rc_bar_vol_ico)) fill_circle_a(screen, rc_bar_vol_ico.x + 14, py_c, 14, C_CARD_HOV, 255);
        icon_vol(screen, rc_bar_vol_ico.x + 14, py_c, 14, C_TXT2, vol < 0.01f);
        fill_round_rect_aa(screen, ui_vx0, py_c - 2, ui_vx1 - ui_vx0, 4, 2, C_TRACK_BG);
        int vw = (int)(vol * (float)(ui_vx1 - ui_vx0) + 0.5f);
        if (vw > 0) fill_round_rect_aa(screen, ui_vx0, py_c - 2, vw, 4, 2, C_ACCENT);
        fill_ellipse_aa(screen, ui_vx0 + vw, py_c, (vol_drag || in_rc(mx, my, rc_bar_vol_trk)) ? 6 : 4,
                        (vol_drag || in_rc(mx, my, rc_bar_vol_trk)) ? 6 : 4, C_TXT);
    }
}

// ---------------- View 1: Desktop PC Library Table Workspace (Image 2 Style) ----------------
static void draw_nav_item(Rect r, const char* label, const char* badge, int active, int hovered, int collapsed) {
    if (active) {
        fill_round_rect_aa(screen, r.x, r.y, r.w, r.h, 8, C_CARD_ACT);
        fill_round_rect_aa(screen, r.x, r.y + 8, 3, r.h - 16, 2, C_ACCENT);
    } else if (hovered) {
        fill_round_rect_aa(screen, r.x, r.y, r.w, r.h, 8, C_CARD_HOV);
    }
    if (!collapsed) {
        draw_text(screen, font, 14.0f, r.x + 16, r.y + (r.h - 16) / 2, active ? C_TXT : C_TXT2, label);
        if (badge && badge[0]) {
            draw_text_right(screen, font, 12.0f, r.x + r.w - 12, r.y + (r.h - 13) / 2, active ? C_ACCENT : C_TXT3, badge);
        }
    } else {
        char short_t[8] = {0};
        const unsigned char* s = (const unsigned char*)label;
        int len = (s[0] >= 0xF0) ? 4 : (s[0] >= 0xE0) ? 3 : (s[0] >= 0xC0) ? 2 : 1;
        if (len < (int)sizeof(short_t)) memcpy(short_t, label, (size_t)len);
        draw_text_centered(screen, font, 14.0f, r.x, r.x + r.w, r.y + (r.h - 16) / 2, active ? C_ACCENT : C_TXT2, short_t);
    }
}

static void draw_library_view(int mx, int my, double pos, double dur) {
    draw_ambient_bg(mesh_lib);

    int side_w = sidebar_collapsed ? 64 : (W < 1040 ? 186 : 224);
    int content_x = side_w;
    int content_w = W - side_w;

    // 1. Left Sidebar
    fill_rect_a(screen, 0, 0, side_w, H - BOT_H, C_SIDE, 232);
    fill_rect(screen, side_w - 1, 0, 1, H - BOT_H, C_DIV);

    rc_menu_btn = (Rect){ 14, 14, 34, 34 };
    if (in_rc(mx, my, rc_menu_btn)) fill_round_rect_aa(screen, rc_menu_btn.x, rc_menu_btn.y, 34, 34, 8, C_CARD_HOV);
    icon_menu(screen, rc_menu_btn.x + 9, rc_menu_btn.y + 10, 16, C_TXT2);
    if (!sidebar_collapsed) {
        if (poster_ico) {
            blit_rgb_round(screen, 54, 17, poster_ico, 28, 28, 7);
        } else {
            fill_round_rect_aa(screen, 54, 17, 28, 28, 7, C_ACCENT);
            draw_text_centered(screen, font, 15.0f, 54, 82, 23, C_ON_ACCENT, "S");
        }
        draw_text_bold(screen, font, 17.0f, 90, 22, C_TXT, "Sirène");
    }

    int ny = 68;
    int nfavs = 0;
    for (int i = 0; i < ntracks; i++) if (tracks[i].fav) nfavs++;
    char c_all[16], c_fav[16];
    snprintf(c_all, sizeof(c_all), "%d", ntracks);
    snprintf(c_fav, sizeof(c_fav), "%d", nfavs);

    int nw = side_w - 20;
    rc_nav_all = (Rect){ 10, ny, nw, 38 };
    draw_nav_item(rc_nav_all, "全部歌曲", c_all, lib_tab == TAB_ALL, in_rc(mx, my, rc_nav_all), sidebar_collapsed);
    ny += 42;

    rc_nav_fav = (Rect){ 10, ny, nw, 38 };
    draw_nav_item(rc_nav_fav, "我喜欢的", c_fav, lib_tab == TAB_FAV, in_rc(mx, my, rc_nav_fav), sidebar_collapsed);
    ny += 42;

    rc_nav_lyr = (Rect){ 10, ny, nw, 38 };
    draw_nav_item(rc_nav_lyr, "歌词视界 [F]", NULL, 0, in_rc(mx, my, rc_nav_lyr), sidebar_collapsed);
    ny += 48;

    fill_rect(screen, 14, ny, side_w - 28, 1, C_DIV);
    ny += 12;
    if (!sidebar_collapsed) draw_text(screen, font, 12.0f, 18, ny, C_TXT3, "音乐文件夹");
    ny += sidebar_collapsed ? 4 : 24;

    rc_nav_add = (Rect){ 10, ny, nw, 34 };
    draw_nav_item(rc_nav_add, "+ 添加目录", NULL, 0, in_rc(mx, my, rc_nav_add), sidebar_collapsed);
    ny += 38;

    rc_nav_rescan = (Rect){ 10, ny, nw, 34 };
    draw_nav_item(rc_nav_rescan, "重新扫库", NULL, 0, in_rc(mx, my, rc_nav_rescan), sidebar_collapsed);
    ny += 40;

    if (!sidebar_collapsed) {
        int max_dir_y = H - BOT_H - 96;
        for (int i = 0; i < nscan_dirs && ny + 34 <= max_dir_y; i++) {
            char bname[128];
            basename_str(scan_dirs[i], bname, sizeof(bname));
            rc_dir_items[i] = (Rect){ 10, ny, nw, 34 };
            draw_nav_item(rc_dir_items[i], bname, NULL, (lib_tab == TAB_DIR && active_dir == i),
                          in_rc(mx, my, rc_dir_items[i]), 0);
            ny += 36;
        }
    }

    int bot_side = H - BOT_H - 86;
    rc_nav_theme = (Rect){ 10, bot_side, nw, 36 };
    draw_nav_item(rc_nav_theme, g_is_light ? "深邃暗色" : "清透浅色", NULL, 0, in_rc(mx, my, rc_nav_theme), sidebar_collapsed);
    rc_nav_diag = (Rect){ 10, bot_side + 38, nw, 36 };
    draw_nav_item(rc_nav_diag, "系统诊断", NULL, show_diag, in_rc(mx, my, rc_nav_diag), sidebar_collapsed);

    // 2. Main Header Area (Toolbar + Title + Search + Play All)
    int hx = content_x + 28, hw = content_w - 56;

    // Top utility row: Search Box + Sort + Fullscreen
    int fs_sz = 34;
    rc_fs_btn = (Rect){ hx + hw - fs_sz, 16, fs_sz, fs_sz };
    fill_round_rect_aa(screen, rc_fs_btn.x, rc_fs_btn.y, fs_sz, fs_sz, 8, in_rc(mx, my, rc_fs_btn) ? C_CARD_ACT : C_CARD_HOV);
    icon_expand(screen, rc_fs_btn.x + 9, rc_fs_btn.y + 9, 16, C_TXT2);

    const char* s_labels[4] = { "默认排序", "按歌名", "按歌手", "按专辑" };
    rc_sort_btn = (Rect){ rc_fs_btn.x - 96, 16, 86, 34 };
    fill_round_rect_aa(screen, rc_sort_btn.x, rc_sort_btn.y, 86, 34, 8, in_rc(mx, my, rc_sort_btn) ? C_CARD_ACT : C_CARD_HOV);
    draw_text_centered(screen, font, 12.5f, rc_sort_btn.x, rc_sort_btn.x + 86, 25, C_TXT2, s_labels[sort_mode]);

    int sr_w = (int)clampf((float)hw * 0.34f, 210.0f, 360.0f);
    rc_search_box = (Rect){ rc_sort_btn.x - sr_w - 12, 16, sr_w, 34 };
    fill_round_rect_aa(screen, rc_search_box.x, rc_search_box.y, sr_w, 34, 17,
                       search_focused ? C_CARD_ACT : C_CARD_HOV);
    if (search_focused || in_rc(mx, my, rc_search_box)) {
        stroke_round_rect_aa(screen, rc_search_box.x, rc_search_box.y, sr_w, 34, 17, 1, C_ACCENT, 190);
    }
    if (search_q[0]) {
        draw_text_max(screen, font, 13.5f, rc_search_box.x + 14, 24, C_TXT, search_q, sr_w - 46);
        rc_search_clear = (Rect){ rc_search_box.x + sr_w - 30, 18, 26, 30 };
        draw_text_centered(screen, font, 14.0f, rc_search_clear.x, rc_search_clear.x + 26, 24, C_TXT2, "×");
    } else {
        draw_text_max(screen, font, 13.0f, rc_search_box.x + 14, 24, C_TXT3,
                      search_focused ? "输入歌名 / 歌手 / 专辑…" : "搜索歌曲、歌手、专辑 (Ctrl+F)", sr_w - 24);
    }

    // Folder / View Headline + Subtitle + Track Count + CTA
    char htitle[256] = "全部歌曲";
    char hsub[600] = "本地音乐库 · 支持在线自动匹配网易云 / LRCLIB 歌词";
    if (lib_tab == TAB_FAV) {
        snprintf(htitle, sizeof(htitle), "我喜欢的歌曲");
        snprintf(hsub, sizeof(hsub), "已收藏的喜爱曲目");
    } else if (lib_tab == TAB_DIR && active_dir >= 0 && active_dir < nscan_dirs) {
        basename_str(scan_dirs[active_dir], htitle, sizeof(htitle));
        snprintf(hsub, sizeof(hsub), "%s", scan_dirs[active_dir]);
    } else if (nscan_dirs > 0) {
        snprintf(hsub, sizeof(hsub), "%s", scan_dirs[0]);
    }

    draw_text_max_bold(screen, font, 28.0f, hx, 24, C_TXT, htitle, hw - (rc_fs_btn.x + fs_sz - rc_search_box.x) - 24);
    draw_text_max(screen, font, 12.5f, hx, 66, C_TXT3, hsub, hw - 160);

    char cnt_str[32];
    snprintf(cnt_str, sizeof(cnt_str), "%d 首歌曲", nvisible);
    draw_text_right(screen, font, 13.0f, hx + hw, 62, C_TXT2, cnt_str);

    rc_play_all = (Rect){ hx + hw - 116, 84, 116, 34 };
    uint32_t cta_col = in_rc(mx, my, rc_play_all) ? mix_col(C_ACCENT, COL(255,255,255,255), 0.16f) : C_ACCENT;
    fill_round_rect_aa(screen, rc_play_all.x, rc_play_all.y, rc_play_all.w, rc_play_all.h, 8, cta_col);
    icon_play(screen, rc_play_all.x + 20, rc_play_all.y + 17, 10, C_ON_ACCENT);
    draw_text_bold(screen, font, 13.5f, rc_play_all.x + 34, rc_play_all.y + 8, C_ON_ACCENT, "播放全部");

    // 3. Song Table Columns
    int tbl_x = hx, tbl_w = hw - 12;
    int col_fav = tbl_x + 8;
    int col_cov = col_fav + 34;
    int show_album = (content_w >= 780);
    int show_fmt   = (content_w >= 640);
    int col_act = tbl_x + tbl_w - 36;
    int col_dur = col_act - 66;
    int col_fmt = show_fmt ? (col_dur - 66) : col_dur;
    int col_alb = show_album ? (col_cov + 52 + (col_fmt - (col_cov + 52)) * 62 / 100) : col_fmt;
    int col_art = col_cov + 52 + (col_alb - (col_cov + 52)) * 56 / 100;

    int hdr_y = 128;
    draw_text(screen, font, 12.0f, col_cov, hdr_y, C_TXT3, "歌曲");
    draw_text(screen, font, 12.0f, col_art, hdr_y, C_TXT3, "艺术家");
    if (show_album) draw_text(screen, font, 12.0f, col_alb, hdr_y, C_TXT3, "专辑");
    if (show_fmt)   draw_text(screen, font, 12.0f, col_fmt, hdr_y, C_TXT3, "格式");
    draw_text_right(screen, font, 12.0f, col_dur + 44, hdr_y, C_TXT3, "时长");
    fill_rect(screen, tbl_x, hdr_y + 22, tbl_w, 1, C_DIV);

    ui_list_x0 = tbl_x;
    ui_list_x1 = tbl_x + tbl_w;
    ui_list_y0 = hdr_y + 26;
    ui_list_y1 = H - BOT_H - 6;

    if (nvisible == 0) {
        draw_text_centered(screen, font, 18.0f, ui_list_x0, ui_list_x1, ui_list_y0 + 90, C_TXT2,
                           ntracks == 0 ? "尚未发现音乐文件" : "未找到匹配的曲目");
        draw_text_centered(screen, font, 13.5f, ui_list_x0, ui_list_x1, ui_list_y0 + 122, C_TXT3,
                           "点击左侧「+ 添加目录」或直接将音乐文件夹 / 歌曲 / .lrc 拖入窗口");
    } else {
        int i0 = (int)floorf(scroll_px / (float)ROW_H) - 1;
        if (i0 < 0) i0 = 0;
        int i1 = (int)ceilf((scroll_px + (float)list_vis_h()) / (float)ROW_H) + 1;
        if (i1 > nvisible) i1 = nvisible;

        int hov_row = -1;
        if (mx >= ui_list_x0 && mx < ui_list_x1 && my >= ui_list_y0 && my < ui_list_y1) {
            hov_row = (int)floorf(((float)(my - ui_list_y0) + scroll_px) / (float)ROW_H);
        }

        canvas_set_clip(screen, ui_list_x0, ui_list_y0, ui_list_x1, ui_list_y1);
        for (int p = i0; p < i1; p++) {
            int idx = vis_idx[p];
            Track* tr = &tracks[idx];
            int ry = ui_list_y0 + p * ROW_H - (int)scroll_px;
            if (ry + ROW_H <= ui_list_y0 || ry >= ui_list_y1) continue;

            int is_play = (idx == playing_idx);
            int is_hov  = (p == hov_row);
            if (is_play) {
                fill_round_rect_aa(screen, tbl_x, ry + 2, tbl_w, ROW_H - 4, 8, C_CARD_ACT);
                fill_round_rect_aa(screen, tbl_x, ry + 12, 3, ROW_H - 24, 2, C_ACCENT);
            } else if (is_hov) {
                fill_round_rect_aa(screen, tbl_x, ry + 2, tbl_w, ROW_H - 4, 8, C_CARD_HOV);
            }

            icon_heart_ex(screen, col_fav, ry + (ROW_H - 16) / 2, 16,
                          tr->fav ? C_HEART : (is_hov ? C_TXT2 : C_TXT3), tr->fav);

            int cy0 = ry + (ROW_H - THUMB_SZ) / 2;
            const uint32_t* timg = tr->thumb ? tr->thumb : poster_mini;
            if (timg) {
                if (tr->thumb) blit_rgb_round(screen, col_cov, cy0, tr->thumb, THUMB_SZ, THUMB_SZ, 7);
                else {
                    fill_round_rect_aa(screen, col_cov, cy0, THUMB_SZ, THUMB_SZ, 7, C_SURFACE);
                    draw_text_centered(screen, font, 14.0f, col_cov, col_cov + THUMB_SZ, cy0 + 12, C_TXT3, "♪");
                }
            }

            uint32_t tcol = is_play ? C_ACCENT : C_TXT;
            int tx0 = col_cov + THUMB_SZ + 14;
            if (is_play) draw_text_max_bold(screen, font, 14.0f, tx0, ry + 18, tcol, tr->title, col_art - tx0 - 12);
            else         draw_text_max(screen, font, 14.0f, tx0, ry + 18, tcol, tr->title, col_art - tx0 - 12);

            draw_text_max(screen, font, 13.0f, col_art, ry + 18, C_TXT2, tr->artist[0] ? tr->artist : "-", col_alb - col_art - 12);
            if (show_album) {
                draw_text_max(screen, font, 13.0f, col_alb, ry + 18, C_TXT3, tr->album[0] ? tr->album : "-", col_fmt - col_alb - 12);
            }
            if (show_fmt) {
                fill_round_rect_aa(screen, col_fmt, ry + 16, 48, 22, 6, C_CARD_HOV);
                draw_text_centered(screen, font, 11.0f, col_fmt, col_fmt + 48, ry + 20, C_TXT3, tr->fmt);
            }

            char dstr[16] = "--:--";
            if (is_play && dur > 0.0) fmt_time(dur, dstr, sizeof(dstr));
            else if (tr->dur > 0.0) fmt_time(tr->dur, dstr, sizeof(dstr));
            draw_text_right(screen, font, 13.0f, col_dur + 44, ry + 18, C_TXT3, dstr);

            if (is_play && !audio_paused()) icon_pause(screen, col_act + 14, ry + ROW_H / 2, 11, C_ACCENT);
            else icon_play(screen, col_act + 14, ry + ROW_H / 2, 10, is_hov ? C_TXT : C_TXT3);
        }
        canvas_reset_clip(screen);

        // Right-edge scrollbar
        float ms = max_scroll_px();
        if (ms > 0.0f) {
            int sbx = W - 12, tr_y0 = ui_list_y0 + 4, tr_h = list_vis_h() - 8;
            int th_h = (int)((float)list_vis_h() / ((float)nvisible * ROW_H) * (float)tr_h);
            if (th_h < 34) th_h = 34;
            if (th_h > tr_h) th_h = tr_h;
            int th_y = tr_y0 + (int)((scroll_px / ms) * (float)(tr_h - th_h));
            rc_sb_track = (Rect){ sbx - 6, tr_y0, 16, tr_h };
            int hot = sb_drag || in_rc(mx, my, rc_sb_track);
            int w_sb = hot ? 7 : 5;
            fill_round_rect_aa(screen, sbx - (hot ? 1 : 0), tr_y0, w_sb, tr_h, w_sb / 2, C_CARD_HOV);
            fill_round_rect_aa(screen, sbx - (hot ? 1 : 0), th_y, w_sb, th_h, w_sb / 2, hot ? C_ACCENT : C_TXT3);
        }
    }

    draw_bottom_bar(mx, my, pos, dur);
}

// ---------------- View 2: Immersive Full-Window Cover & Huge Lyrics (Image 1 Style) ----------------
static void draw_immersive_view(int mx, int my, double pos, double dur) {
    draw_ambient_bg(mesh_lyr);

    uint32_t WTXT  = g_is_light ? C_TXT  : COL(250, 252, 255, 255);
    uint32_t WTXT2 = g_is_light ? C_TXT2 : COL(236, 240, 250, 215);
    uint32_t WTXT3 = g_is_light ? C_TXT3 : COL(210, 216, 232, 165);
    uint32_t GLASS = g_is_light ? COL(0, 0, 0, 18) : COL(255, 255, 255, 26);
    uint32_t GLASS_H = g_is_light ? COL(0, 0, 0, 32) : COL(255, 255, 255, 48);

    // Floating top pills
    rc_back_lib = (Rect){ 28, 22, 134, 34 };
    fill_round_rect_aa(screen, rc_back_lib.x, rc_back_lib.y, 134, 34, 17, in_rc(mx, my, rc_back_lib) ? GLASS_H : GLASS);
    icon_back(screen, rc_back_lib.x + 14, rc_back_lib.y + 10, 14, WTXT);
    draw_text(screen, font, 13.5f, rc_back_lib.x + 34, rc_back_lib.y + 8, WTXT, "返回曲库 [Esc]");

    rc_lyr_fs = (Rect){ W - 62, 22, 34, 34 };
    fill_round_rect_aa(screen, rc_lyr_fs.x, 22, 34, 34, 17, in_rc(mx, my, rc_lyr_fs) ? GLASS_H : GLASS);
    icon_expand(screen, rc_lyr_fs.x + 9, 31, 16, WTXT);

    rc_lyr_theme = (Rect){ rc_lyr_fs.x - 86, 22, 76, 34 };
    fill_round_rect_aa(screen, rc_lyr_theme.x, 22, 76, 34, 17, in_rc(mx, my, rc_lyr_theme) ? GLASS_H : GLASS);
    draw_text_centered(screen, font, 13.0f, rc_lyr_theme.x, rc_lyr_theme.x + 76, 30, WTXT, g_is_light ? "暗色" : "浅色");

    // Left Hero Column (Cover + Left-Aligned Metadata + Compact Controls)
    int csz = (int)clampf(fminf((float)H * 0.45f, (float)W * 0.28f), 220.0f, 390.0f);
    int left_x = (int)clampf((float)W * 0.085f, 44.0f, 170.0f);
    int meta_block_h = 190;
    int tot_left_h = csz + 24 + meta_block_h;
    int cov_y = (H - tot_left_h) / 2;
    if (cov_y < 68) cov_y = 68;

    fill_round_rect_shadow(screen, left_x, cov_y, csz, csz, 20, g_is_light ? 34 : 76);
    if (playing_idx >= 0 && cover_src) {
        blit_rgb_round_fit(screen, left_x, cov_y, csz, csz, cover_src, COVER_SRC_SZ, COVER_SRC_SZ, 20);
    } else if (poster_hero) {
        blit_rgb_round_fit(screen, left_x, cov_y, csz, csz, poster_hero, COVER_SRC_SZ, COVER_SRC_SZ, 20);
    }

    int my0 = cov_y + csz + 24;
    float t_px = clampf((float)H * 0.034f, 22.0f, 32.0f);
    float s_px = clampf((float)H * 0.022f, 15.0f, 19.0f);
    draw_text_max_bold(screen, font, t_px, left_x, my0, WTXT, playing_idx >= 0 ? p_title : "Sirène", csz + 24);
    int cy_sub = my0 + (int)(t_px * 1.32f);
    if (playing_idx >= 0 && p_album[0] && strcmp(p_album, p_title) != 0) {
        draw_text_max(screen, font, s_px, left_x, cy_sub, WTXT2, p_album, csz + 24);
        cy_sub += (int)(s_px * 1.35f);
    }
    if (playing_idx >= 0 && p_artist[0]) {
        draw_text_max(screen, font, s_px, left_x, cy_sub, WTXT2, p_artist, csz + 24);
        cy_sub += (int)(s_px * 1.35f);
    }

    char spec[160] = "准备就绪";
    if (playing_idx >= 0) {
        snprintf(spec, sizeof(spec), "%s %uHz %uch · %s", p_fmt, (unsigned)audio_sample_rate(),
                 (unsigned)audio_channels(), (lyr && lyr->source[0]) ? lyr->source : (lyrics_online_busy(g_lyr_req_id) ? "在线匹配歌词中…" : "暂无歌词"));
    }
    draw_text_max(screen, font, 12.5f, left_x, cy_sub + 2, WTXT3, spec, csz + 24);

    // Compact progress scrubber + controls right beneath left metadata
    int ctrl_y = cy_sub + 32;
    ui_px0 = left_x;
    ui_px1 = left_x + csz;
    ui_py = ctrl_y;
    rc_bar_prog = (Rect){ ui_px0 - 8, ctrl_y - 12, csz + 16, 26 };
    float frac = (dur > 0.0) ? clampf((float)(pos / dur), 0.0f, 1.0f) : 0.0f;
    float wamp = (playing_idx >= 0 && !audio_paused() && dur > 0.0) ? 2.8f : 0.0f;
    draw_m3_wavy_progress(screen, ui_px0, ui_px1, ctrl_y + 2, frac, wave_phase, wamp, 20.0f,
                          5, 3, (scrub_drag || in_rc(mx, my, rc_bar_prog)) ? 1 : 0,
                          COL(255, 255, 255, 235), GLASS);
    char t1[24], t2[24];
    fmt_time(pos, t1, sizeof(t1)); fmt_time(dur, t2, sizeof(t2));
    draw_text(screen, font, 11.5f, ui_px0, ctrl_y + 13, WTXT3, t1);
    draw_text_right(screen, font, 11.5f, ui_px1, ctrl_y + 13, WTXT3, t2);

    int tr_y = ctrl_y + 50;
    rc_bar_fav  = (Rect){ left_x, tr_y - 16, 32, 32 };
    rc_bar_prev = (Rect){ left_x + 48, tr_y - 16, 34, 34 };
    rc_bar_play = (Rect){ left_x + 92, tr_y - 20, 42, 42 };
    rc_bar_next = (Rect){ left_x + 144, tr_y - 16, 34, 34 };
    rc_bar_mode = (Rect){ left_x + 188, tr_y - 15, 48, 30 };

    int fav = (playing_idx >= 0 && tracks[playing_idx].fav);
    if (in_rc(mx, my, rc_bar_fav)) fill_circle_a(screen, left_x + 16, tr_y, 16, GLASS_H, 255);
    icon_heart_ex(screen, left_x + 8, tr_y - 8, 16, fav ? C_HEART : WTXT2, fav);

    if (in_rc(mx, my, rc_bar_prev)) fill_circle_a(screen, rc_bar_prev.x + 17, tr_y, 16, GLASS_H, 255);
    icon_prev(screen, rc_bar_prev.x + 17, tr_y, 14, WTXT);

    uint32_t btn_bg = in_rc(mx, my, rc_bar_play) ? WTXT : mix_col(WTXT, GLASS_H, 0.12f);
    uint32_t btn_fg = g_is_light ? COL(255, 255, 255, 255) : COL(18, 20, 28, 255);
    fill_ellipse_aa(screen, rc_bar_play.x + 21, tr_y, 21, 21, btn_bg);
    if (playing_idx >= 0 && !audio_paused()) icon_pause(screen, rc_bar_play.x + 21, tr_y, 15, btn_fg);
    else icon_play(screen, rc_bar_play.x + 22, tr_y, 15, btn_fg);

    if (in_rc(mx, my, rc_bar_next)) fill_circle_a(screen, rc_bar_next.x + 17, tr_y, 16, GLASS_H, 255);
    icon_next(screen, rc_bar_next.x + 17, tr_y, 14, WTXT);

    const char* mnames[3] = { "顺序", "单曲", "随机" };
    if (in_rc(mx, my, rc_bar_mode)) fill_round_rect_aa(screen, rc_bar_mode.x, rc_bar_mode.y, 48, 30, 8, GLASS_H);
    draw_text_centered(screen, font, 12.5f, rc_bar_mode.x, rc_bar_mode.x + 48, tr_y - 7, WTXT2, mnames[play_mode]);

    // Volume mini slider next to mode
    ui_vx0 = rc_bar_mode.x + 60;
    ui_vx1 = left_x + csz;
    if (ui_vx1 - ui_vx0 >= 54) {
        rc_bar_vol_trk = (Rect){ ui_vx0 - 4, tr_y - 12, (ui_vx1 - ui_vx0) + 8, 24 };
        float vol = audio_volume();
        fill_round_rect_aa(screen, ui_vx0, tr_y - 2, ui_vx1 - ui_vx0, 4, 2, GLASS);
        int vw = (int)(vol * (float)(ui_vx1 - ui_vx0) + 0.5f);
        if (vw > 0) fill_round_rect_aa(screen, ui_vx0, tr_y - 2, vw, 4, 2, WTXT);
        fill_ellipse_aa(screen, ui_vx0 + vw, tr_y, 5, 5, WTXT);
    }

    // Right Hero Column: HUGE Apple Music / Salt Player Bilingual Lyrics
    int lx0 = left_x + csz + (int)clampf((float)W * 0.065f, 52.0f, 128.0f);
    int lx1 = W - (int)clampf((float)W * 0.060f, 44.0f, 108.0f);
    int ly0 = 64, ly1 = H - 36;
    int lw = lx1 - lx0;
    nlyr_hits = 0;

    if (lyr && lyr->n > 0 && lw >= 180) {
        float lpx = clampf((float)H * 0.052f, 34.0f, 52.0f);
        float spx = clampf(lpx * 0.56f, 20.0f, 29.0f);
        int orig_lh = (int)(lpx * 1.24f);
        int sub_lh  = (int)(spx * 1.32f);
        int gap_y   = (int)(lpx * 0.78f);

        int act = lyr->unsynced
            ? (dur > 0.0 && lyr->n > 1 ? (int)((pos / dur) * (double)(lyr->n - 1)) : 0)
            : lyrics_index(lyr, pos);
        if (act < 0) act = 0;
        if (act >= lyr->n) act = lyr->n - 1;

        // Precompute vertical positions of nearby window (all lines fast for N <= 500)
        float act_y = 0.0f;
        float cur_y = 0.0f;
        int w_lo = act - 18; if (w_lo < 0) w_lo = 0;
        int w_hi = act + 22; if (w_hi >= lyr->n) w_hi = lyr->n - 1;

        // Estimate act center relative to w_lo cleanly and damp camera
        typedef struct { float y; int h; WrapLines w1; WrapLines w2; } LyrLayout;
        static LyrLayout lay[64];
        int nlay = 0;
        for (int i = 0; i < lyr->n; i++) {
            const char* main_t = lyr->lines[i].orig[0] ? lyr->lines[i].orig : lyr->lines[i].text;
            const char* sub_t  = lyr->lines[i].sub;
            WrapLines w1, w2;
            wrap_utf8_lines(font, lpx, main_t, lw - 16, &w1);
            if (sub_t[0]) wrap_utf8_lines(font, spx, sub_t, lw - 16, &w2);
            else w2.count = 0;
            int bh = (w1.count > 0 ? w1.count : 1) * orig_lh + w2.count * sub_lh;
            if (i == act) act_y = cur_y + (float)bh * 0.45f;
            if (i >= w_lo && i <= w_hi && nlay < 64) {
                lay[nlay].y = cur_y;
                lay[nlay].h = bh;
                lay[nlay].w1 = w1;
                lay[nlay].w2 = w2;
                nlay++;
            }
            cur_y += (float)(bh + gap_y);
        }

        float target_cam = act_y - (float)(ly1 - ly0) * 0.40f;
        if (lyr_manual_timer <= 0.0f) {
            if (!lyr_cam_valid) { lyr_cam_y = target_cam; lyr_cam_valid = 1; }
            else {
                float k = (float)(g_dt * 11.0);
                if (k > 1.0f) k = 1.0f;
                lyr_cam_y += (target_cam - lyr_cam_y) * k;
            }
        }

        canvas_set_clip(screen, lx0 - 12, ly0, lx1 + 12, ly1);
        for (int li = 0; li < nlay; li++) {
            int i = w_lo + li;
            float sy = (float)ly0 + lay[li].y - lyr_cam_y;
            if (sy + lay[li].h < (float)ly0 - 20.0f || sy > (float)ly1 + 20.0f) continue;

            int is_act = (i == act);
            int dist = abs(i - act);
            Rect hrc = { lx0 - 10, (int)sy - 6, lw + 20, lay[li].h + 12 };
            int is_hov = in_rc(mx, my, hrc);
            if (nlyr_hits < LYRIC_MAX_HITS) {
                lyr_hits[nlyr_hits].rc = hrc;
                lyr_hits[nlyr_hits].t = lyr->lines[i].t;
                nlyr_hits++;
            }
            if (is_hov && !is_act) fill_round_rect_aa_a(screen, hrc.x, hrc.y, hrc.w, hrc.h, 12, WTXT, 16);

            uint8_t ma = is_act ? 255 : (is_hov ? 225 : (dist == 1 ? 160 : (dist == 2 ? 108 : 68)));
            uint8_t sa = is_act ? 220 : (is_hov ? 185 : (dist == 1 ? 124 : (dist == 2 ? 82  : 48)));
            uint32_t mcol = (WTXT & 0x00FFFFFFu) | ((uint32_t)ma << 24);
            uint32_t scol = (WTXT & 0x00FFFFFFu) | ((uint32_t)sa << 24);

            // Soft edge fade near top & bottom of lyric viewport
            int cy_line = (int)sy;
            for (int r = 0; r < lay[li].w1.count; r++) {
                if (is_act || is_hov) draw_text_bold(screen, font, lpx, lx0, cy_line, mcol, lay[li].w1.lines[r]);
                else                  draw_text(screen, font, lpx, lx0, cy_line, mcol, lay[li].w1.lines[r]);
                cy_line += orig_lh;
            }
            for (int r = 0; r < lay[li].w2.count; r++) {
                if (is_act) draw_text_bold(screen, font, spx, lx0, cy_line + 2, scol, lay[li].w2.lines[r]);
                else        draw_text(screen, font, spx, lx0, cy_line + 2, scol, lay[li].w2.lines[r]);
                cy_line += sub_lh;
            }
        }
        canvas_reset_clip(screen);
    } else {
        // Sleek translucent spectrum when no lyrics / fetching
        int nbars = 28, bw = (lw - (nbars - 1) * 6) / nbars;
        if (bw > 18) bw = 18;
        if (bw < 6) bw = 6;
        int tot_w = nbars * bw + (nbars - 1) * 6;
        int bx0 = lx0 + (lw - tot_w) / 2, base_y = H / 2 + 40;
        for (int i = 0; i < nbars; i++) {
            float v = clampf(bars[i], 0.08f, 1.0f);
            int bh = (int)(v * 140.0f);
            fill_round_rect_aa_a(screen, bx0 + i * (bw + 6), base_y - bh, bw, bh, bw / 2, WTXT, 140);
        }
        const char* msg = lyrics_online_busy(g_lyr_req_id) ? "正在为您从云端匹配同步歌词…" : lyrics_last_diag();
        draw_text_centered(screen, font, 17.0f, lx0, lx1, base_y + 32, WTXT2, msg);
    }
}

static void draw_diag_modal(int mx, int my) {
    (void)mx; (void)my;
    fill_rect_a(screen, 0, 0, W, H, COL(0, 0, 0, 255), 145);
    int mw = 600, mh = 320;
    if (mw > W - 60) mw = W - 60;
    int cx = (W - mw) / 2, cy = (H - mh) / 2;
    fill_round_rect_shadow(screen, cx, cy, mw, mh, 18, 80);
    fill_round_rect_aa(screen, cx, cy, mw, mh, 18, C_SURFACE);
    stroke_round_rect_aa(screen, cx, cy, mw, mh, 18, 1, C_DIV, 255);
    draw_text_bold(screen, font, 20.0f, cx + 26, cy + 24, C_ACCENT, "Sirène v1.2.6 系统与音频诊断");

    char d_aud[256], d_res[128], d_lyr[220];
    audio_get_diag(d_aud, sizeof(d_aud));
    snprintf(d_res, sizeof(d_res), "视口逻辑尺寸：%dx%d · 物理渲染：%dx%d (缩放 %.2fx)",
             W, H, screen->w, screen->h, g_ui_scale);
    if (lyr && lyr->source[0]) snprintf(d_lyr, sizeof(d_lyr), "歌词状态：%s (%d 行)", lyr->source, lyr->n);
    else snprintf(d_lyr, sizeof(d_lyr), "歌词状态：%s", lyrics_last_diag());

    draw_text_max(screen, font, 13.5f, cx + 26, cy + 74, C_TXT, d_res, mw - 52);
    draw_text_max(screen, font, 13.5f, cx + 26, cy + 106, C_TXT2, d_aud, mw - 52);
    draw_text_max(screen, font, 13.5f, cx + 26, cy + 138, C_TXT2, d_lyr, mw - 52);
    draw_text_max(screen, font, 13.0f, cx + 26, cy + 172, C_TXT3,
                  "支持格式：MP3 / FLAC / WAV / OGG / AIFF / M4A / AAC / MP4 / ALAC / OPUS / WMA / CAF", mw - 52);
    draw_text_centered(screen, font, 13.0f, cx + 26, cx + mw - 26, cy + mh - 42, C_TXT3, "点击任意处或按 Esc 关闭");
}

// ---------------- Input Handling ----------------
static double calc_scrub_sec(int x) {
    if (audio_duration() <= 0.0 || ui_px1 <= ui_px0) return 0.0;
    if (x < ui_px0) x = ui_px0;
    if (x > ui_px1) x = ui_px1;
    double s = ((double)(x - ui_px0) / (double)(ui_px1 - ui_px0)) * audio_duration();
    if (audio_duration() > 0.5 && s > audio_duration() - 0.25) s = audio_duration() - 0.25;
    return s;
}

static void set_vol_from_x(int x) {
    if (ui_vx1 <= ui_vx0) return;
    if (x < ui_vx0) x = ui_vx0;
    if (x > ui_vx1) x = ui_vx1;
    float v = (float)(x - ui_vx0) / (float)(ui_vx1 - ui_vx0);
    audio_set_volume(v);
    if (v > 0.01f) g_saved_vol = v;
    save_settings();
}

static void seek_sb_from_y(int y) {
    float ms = max_scroll_px();
    if (ms <= 0.0f || rc_sb_track.h <= 20) return;
    float f = (float)(y - rc_sb_track.y) / (float)rc_sb_track.h;
    f = clampf(f, 0.0f, 1.0f);
    scroll_px = scroll_target = f * ms;
}

static void handle_text_input_events(void) {
    char u8[32];
    int act = 0;
    while (be_poll_text_char(u8, sizeof(u8), &act)) {
        if (act == 3) { // ESC
            if (search_q[0]) { search_q[0] = 0; rebuild_visible(); scroll_px = scroll_target = 0; }
            search_focused = 0;
            be_text_input(0);
            break;
        } else if (act == 2) { // ENTER
            search_focused = 0;
            be_text_input(0);
            break;
        } else if (act == 1) { // BACKSPACE
            size_t n = strlen(search_q);
            while (n > 0 && ((unsigned char)search_q[n - 1] & 0xC0) == 0x80) n--;
            if (n > 0) n--;
            search_q[n] = 0;
            rebuild_visible();
            scroll_px = scroll_target = 0;
        } else if (act == 0 && u8[0]) {
            size_t n = strlen(search_q), al = strlen(u8);
            if (n + al < sizeof(search_q) - 1) {
                memcpy(search_q + n, u8, al + 1);
                rebuild_visible();
                scroll_px = scroll_target = 0;
            }
        }
    }
}

static void handle_input(TouchInfo* t, uint64_t down, int hk, int wh) {
    if (show_diag) {
        if (t->down || (down & (BTN_A | BTN_B | BTN_X))) show_diag = 0;
        return;
    }

    if (hk == 's') { search_focused = 1; page = PAGE_LIBRARY; be_text_input(1); return; }
    if (hk == 'f') { open_folder_dialog(); return; }
    if (hk == 'm') { play_mode = (play_mode + 1) % 3; save_settings(); return; }
    if (hk == 'c') { show_diag = !show_diag; return; }
    if (hk == 't') { g_is_light = !g_is_light; rebuild_ambient_meshes(); save_settings(); return; }

    if (search_focused) handle_text_input_events();

    if (!search_focused) {
        if (down & BTN_X) {
            if (playing_idx >= 0) { capture_fade(); page = (page == PAGE_LYRICS) ? PAGE_LIBRARY : PAGE_LYRICS; }
            else show_toast("请先点击播放一首歌曲");
        }
        if (down & BTN_B) {
            if (page == PAGE_LYRICS) { capture_fade(); page = PAGE_LIBRARY; }
            else if (search_q[0]) { search_q[0] = 0; rebuild_visible(); }
        }
        if (down & BTN_A) {
            if (playing_idx >= 0) { if (audio_paused()) audio_resume(); else audio_pause(); }
            else if (nvisible > 0) load_track(vis_idx[0]);
        }
        if (down & BTN_LEFT)  audio_seek(audio_position() - 5.0);
        if (down & BTN_RIGHT) audio_seek(audio_position() + 5.0);
        if (down & BTN_UP)   { audio_set_volume(audio_volume() + 0.05f); save_settings(); }
        if (down & BTN_DOWN) { audio_set_volume(audio_volume() - 0.05f); save_settings(); }
        if (down & BTN_ZR)   { g_is_light = !g_is_light; rebuild_ambient_meshes(); save_settings(); }
    }

    // Active drag continuations (commit seek ONLY on release!)
    if (scrub_drag) {
        if (t->held || t->down) { scrub_sec = calc_scrub_sec(t->x); return; }
        if (t->up) { scrub_sec = calc_scrub_sec(t->x); scrub_drag = 0; audio_seek(scrub_sec); return; }
        scrub_drag = 0;
    }
    if (vol_drag) {
        if (t->held || t->down) { set_vol_from_x(t->x); return; }
        vol_drag = 0;
        return;
    }
    if (sb_drag) {
        if (t->held || t->down) { seek_sb_from_y(t->y); return; }
        sb_drag = 0;
        return;
    }

    // Mouse wheel
    if (wh != 0) {
        int mx = -1, my = -1;
        be_mouse_pos(&mx, &my);
        if (in_rc(mx, my, rc_bar_prog) && audio_duration() > 0.0) {
            audio_seek(audio_position() + (double)wh * 5.0);
        } else if (in_rc(mx, my, rc_bar_vol_trk)) {
            audio_set_volume(audio_volume() + (float)wh * 0.05f);
            save_settings();
        } else if (page == PAGE_LYRICS && lyr && lyr->n > 0) {
            lyr_cam_y -= (float)wh * 58.0f;
            lyr_manual_timer = 3.0f;
        } else if (page == PAGE_LIBRARY) {
            scroll_target -= (float)wh * 135.0f;
            float ms = max_scroll_px();
            if (scroll_target < 0.0f) scroll_target = 0.0f;
            if (scroll_target > ms) scroll_target = ms;
        }
    }

    if (!t->down) return;
    int x = t->x, y = t->y;

    // Shared bar controls
    if (in_rc(x, y, rc_bar_prog) && audio_duration() > 0.0) {
        scrub_drag = 1; scrub_sec = calc_scrub_sec(x); return;
    }
    if (in_rc(x, y, rc_bar_vol_trk)) {
        vol_drag = 1; set_vol_from_x(x); return;
    }
    if (in_rc(x, y, rc_bar_vol_ico)) {
        if (audio_volume() > 0.01f) { g_saved_vol = audio_volume(); audio_set_volume(0.0f); }
        else audio_set_volume(g_saved_vol > 0.05f ? g_saved_vol : 0.75f);
        save_settings();
        return;
    }
    if (in_rc(x, y, rc_bar_fav) && playing_idx >= 0) { toggle_fav(playing_idx); return; }
    if (in_rc(x, y, rc_bar_rew) && audio_duration() > 0.0) { audio_seek(audio_position() - 10.0); return; }
    if (in_rc(x, y, rc_bar_fwd) && audio_duration() > 0.0) { audio_seek(audio_position() + 10.0); return; }
    if (in_rc(x, y, rc_bar_prev) && ntracks > 0) { load_track(pick_prev(playing_idx)); return; }
    if (in_rc(x, y, rc_bar_next) && ntracks > 0) { load_track(pick_next(playing_idx, 1)); return; }
    if (in_rc(x, y, rc_bar_play)) {
        if (playing_idx >= 0) { if (audio_paused()) audio_resume(); else audio_pause(); }
        else if (nvisible > 0) load_track(vis_idx[0]);
        return;
    }
    if (in_rc(x, y, rc_bar_mode)) { play_mode = (play_mode + 1) % 3; save_settings(); return; }

    // Unfocus search if clicked elsewhere
    if (search_focused && !in_rc(x, y, rc_search_box)) {
        search_focused = 0; be_text_input(0);
    }

    if (page == PAGE_LYRICS) {
        if (in_rc(x, y, rc_back_lib)) { capture_fade(); page = PAGE_LIBRARY; return; }
        if (in_rc(x, y, rc_lyr_theme)) { g_is_light = !g_is_light; rebuild_ambient_meshes(); save_settings(); return; }
        if (in_rc(x, y, rc_lyr_fs)) { be_toggle_fullscreen(); return; }
        for (int i = 0; i < nlyr_hits; i++) {
            if (in_rc(x, y, lyr_hits[i].rc) && audio_duration() > 0.0) {
                audio_seek(lyr_hits[i].t);
                lyr_manual_timer = 0.0f;
                return;
            }
        }
        return;
    }

    // PAGE_LIBRARY clicks
    if (in_rc(x, y, rc_bar_cov) || in_rc(x, y, rc_bar_lyr) || in_rc(x, y, rc_nav_lyr)) {
        if (playing_idx >= 0) { capture_fade(); page = PAGE_LYRICS; }
        else if (nvisible > 0) { load_track(vis_idx[0]); page = PAGE_LYRICS; }
        else show_toast("暂无曲目，请先添加目录");
        return;
    }
    if (in_rc(x, y, rc_menu_btn)) { sidebar_collapsed = !sidebar_collapsed; save_settings(); return; }
    if (in_rc(x, y, rc_nav_all))  { lib_tab = TAB_ALL; active_dir = -1; rebuild_visible(); scroll_px = scroll_target = 0; return; }
    if (in_rc(x, y, rc_nav_fav))  { lib_tab = TAB_FAV; rebuild_visible(); scroll_px = scroll_target = 0; return; }
    if (in_rc(x, y, rc_nav_add))  { open_folder_dialog(); return; }
    if (in_rc(x, y, rc_nav_rescan)) { scan_library(1); return; }
    if (in_rc(x, y, rc_nav_theme)) { g_is_light = !g_is_light; rebuild_ambient_meshes(); save_settings(); return; }
    if (in_rc(x, y, rc_nav_diag)) { show_diag = !show_diag; return; }
    for (int i = 0; i < nscan_dirs; i++) {
        if (in_rc(x, y, rc_dir_items[i])) {
            lib_tab = TAB_DIR; active_dir = i; rebuild_visible(); scroll_px = scroll_target = 0; return;
        }
    }
    if (in_rc(x, y, rc_search_clear)) { search_q[0] = 0; rebuild_visible(); scroll_px = scroll_target = 0; return; }
    if (in_rc(x, y, rc_search_box)) { search_focused = 1; be_text_input(1); return; }
    if (in_rc(x, y, rc_sort_btn)) { sort_mode = (sort_mode + 1) % 4; rebuild_visible(); save_settings(); return; }
    if (in_rc(x, y, rc_fs_btn)) { be_toggle_fullscreen(); return; }
    if (in_rc(x, y, rc_play_all) && nvisible > 0) { load_track(vis_idx[0]); return; }
    if (in_rc(x, y, rc_sb_track)) { sb_drag = 1; seek_sb_from_y(y); return; }

    if (in_rect(x, y, ui_list_x0, ui_list_y0, ui_list_x1 - ui_list_x0, ui_list_y1 - ui_list_y0)) {
        int pos_i = (int)floorf(((float)(y - ui_list_y0) + scroll_px) / (float)ROW_H);
        if (pos_i >= 0 && pos_i < nvisible) {
            int idx = vis_idx[pos_i];
            if (x < ui_list_x0 + 40) { toggle_fav(idx); return; }
            if (idx == playing_idx) { if (audio_paused()) audio_resume(); else audio_pause(); }
            else load_track(idx);
        }
    }
}

static void update_hover_cursor(void) {
    int mx = -1, my = -1;
    be_mouse_pos(&mx, &my);
    int h = 0;
    if (mx >= 0 && my >= 0 && !show_diag) {
        if (in_rc(mx, my, rc_bar_prog) || in_rc(mx, my, rc_bar_vol_trk) ||
            in_rc(mx, my, rc_bar_vol_ico) || in_rc(mx, my, rc_bar_fav) ||
            in_rc(mx, my, rc_bar_prev) || in_rc(mx, my, rc_bar_play) ||
            in_rc(mx, my, rc_bar_next) || in_rc(mx, my, rc_bar_mode)) h = 1;
        if (page == PAGE_LIBRARY) {
            if (in_rc(mx, my, rc_menu_btn) || in_rc(mx, my, rc_nav_all) || in_rc(mx, my, rc_nav_fav) ||
                in_rc(mx, my, rc_nav_lyr) || in_rc(mx, my, rc_nav_add) || in_rc(mx, my, rc_nav_rescan) ||
                in_rc(mx, my, rc_nav_theme) || in_rc(mx, my, rc_nav_diag) || in_rc(mx, my, rc_search_box) ||
                in_rc(mx, my, rc_sort_btn) || in_rc(mx, my, rc_play_all) || in_rc(mx, my, rc_fs_btn) ||
                in_rc(mx, my, rc_sb_track) || in_rc(mx, my, rc_bar_cov) || in_rc(mx, my, rc_bar_lyr) ||
                in_rc(mx, my, rc_bar_rew) || in_rc(mx, my, rc_bar_fwd)) h = 1;
            for (int i = 0; i < nscan_dirs; i++) if (in_rc(mx, my, rc_dir_items[i])) h = 1;
            if (!h && in_rect(mx, my, ui_list_x0, ui_list_y0, ui_list_x1 - ui_list_x0, ui_list_y1 - ui_list_y0)) {
                int p = (int)floorf(((float)(my - ui_list_y0) + scroll_px) / (float)ROW_H);
                if (p >= 0 && p < nvisible) h = 1;
            }
        } else {
            if (in_rc(mx, my, rc_back_lib) || in_rc(mx, my, rc_lyr_theme) || in_rc(mx, my, rc_lyr_fs)) h = 1;
            for (int i = 0; i < nlyr_hits; i++) if (in_rc(mx, my, lyr_hits[i].rc)) h = 1;
        }
    }
    if (h != ui_cursor_hand) { ui_cursor_hand = h; be_set_cursor(h); }
}

static void sync_canvas_to_window(void) {
    int pw = 1360, ph = 840;
    float s = 1.0f;
    be_screen_metrics(&pw, &ph, &s);
    if (!screen || screen->w != pw || screen->h != ph || fabsf(screen->s - s) > 0.01f) {
        if (screen) canvas_free(screen);
        screen = canvas_new(pw, ph);
        if (screen) {
            screen->s = s > 0.0f ? s : 1.0f;
            screen->lw = (int)((float)pw / screen->s + 0.5f);
            screen->lh = (int)((float)ph / screen->s + 0.5f);
            W = screen->lw;
            H = screen->lh;
        }
        fade_time = 0.0f;
    }
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (argv[i] && argv[i][0] && argv[i][0] != '-') {
            be_set_cli_path(argv[i]);
            break;
        }
    }
    if (be_init() != 0) return -1;
    audio_init();

    tracks = (Track*)calloc(MAX_TRACKS, sizeof(Track));
    vis_idx = (int*)calloc(MAX_TRACKS, sizeof(int));
    if (!tracks || !vis_idx) return -1;

    load_settings();
    {
        char lcache[580];
        ensure_dir_recursive(be_config_dir());
        snprintf(lcache, sizeof(lcache), "%s/lyrics_cache", be_config_dir());
        ensure_dir_recursive(lcache);
        lyrics_set_cache_dir(lcache);
    }
    load_dirs_cfg();

    font = font_load(font_ttf_start, (size_t)(font_ttf_end - font_ttf_start));
    build_posters();
    sync_canvas_to_window();
    rebuild_ambient_meshes();
    scan_library(0);

    const char* drop0 = be_poll_dropped_path();
    if (drop0) handle_dropped(drop0);

    const char* dump_dir = getenv("SIRENE_DUMP_DIR");
    int dump_frames = 0;

    g_last_ns = plat_ticks_ns();
    TouchInfo touch_now;
    int stride = 0;

    while (be_should_run()) {
        uint64_t now = plat_ticks_ns();
        double dt = (double)(now - g_last_ns) / 1000000000.0;
        g_last_ns = now;
        if (dt < 0.001) dt = 0.001;
        if (dt > 0.05)  dt = 0.05;
        g_dt = dt;

        be_input_update();
        be_touch_update(&touch_now);

        const char* drp = be_poll_dropped_path();
        if (drp) handle_dropped(drp);

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

        uint64_t down = be_btn_down();
        int hk = be_poll_hotkey();
        int wh = be_wheel_delta();

        handle_input(&touch_now, down, hk, wh);
        audio_pump();

        if (playing_idx >= 0 && audio_status() == 2 && audio_finished()) {
            load_track(pick_next(playing_idx, 0));
        }

        float sm = (float)dt * 20.0f;
        if (sm > 1.0f) sm = 1.0f;
        if (fabsf(scroll_target - scroll_px) > 0.5f) scroll_px += (scroll_target - scroll_px) * sm;
        else scroll_px = scroll_target;

        if (lyr_manual_timer > 0.0f) lyr_manual_timer -= (float)dt;
        if (playing_idx >= 0 && !audio_paused()) wave_phase += (float)dt * 3.8f;

        float spec_raw[28] = {0};
        audio_get_spectrum(spec_raw);
        for (int i = 0; i < 28; i++) bars[i] = bars[i] * 0.72f + spec_raw[i] * 0.28f;

        sync_canvas_to_window();
        pump_background_meta();

        uint32_t* fb = be_begin(&stride);
        int mx = -1, my = -1;
        be_mouse_pos(&mx, &my);
        double pos = scrub_drag ? scrub_sec : audio_position();
        double dur = audio_duration();
        if (dur > 0.0 && pos > dur) pos = dur;

        reset_hit_rects();
        if (page == PAGE_LYRICS) draw_immersive_view(mx, my, pos, dur);
        else draw_library_view(mx, my, pos, dur);
        if (show_diag) draw_diag_modal(mx, my);

        if (toast_timer > 0) {
            toast_timer--;
            int tw = text_width(font, 14.0f, toast_text) + 40;
            int tx = (W - tw) / 2;
            fill_round_rect_shadow(screen, tx, 20, tw, 38, 19, 48);
            fill_round_rect_aa(screen, tx, 20, tw, 38, 19, C_SURFACE);
            stroke_round_rect_aa(screen, tx, 20, tw, 38, 19, 1, C_ACCENT, 160);
            draw_text_centered(screen, font, 14.0f, tx, tx + tw, 29, C_TXT, toast_text);
        }

        if (fade_time > 0.0f && trans_shot && trans_shot->w == screen->w && trans_shot->h == screen->h) {
            float tn = fade_time / FADE_DUR;
            if (tn > 1.0f) tn = 1.0f;
            blend_over_opaque(screen->px, trans_shot->px, (size_t)screen->w * screen->h, (uint8_t)(tn * tn * 255.0f));
            if (fade_armed) fade_armed = 0;
            else { fade_time -= (float)dt; if (fade_time < 0.0f) fade_time = 0.0f; }
        }

        if (dump_dir && *dump_dir) {
            dump_frames++;
            if (dump_frames == 2 && playing_idx < 0 && nvisible > 0) {
                int target = 0;
                for (int k = 0; k < nvisible; k++) {
                    if (strstr(tracks[vis_idx[k]].title, "愛言葉") || strstr(tracks[vis_idx[k]].path, "DECO")) {
                        target = vis_idx[k]; break;
                    }
                }
                load_track(target);
            }
            if (dump_frames == 35) {
                char fp[600];
                snprintf(fp, sizeof(fp), "%s/shot_lib_%dx%d.rgba", dump_dir, screen->w, screen->h);
                FILE* sf = fopen(fp, "wb");
                if (sf) { fwrite(screen->px, 4, (size_t)screen->w * screen->h, sf); fclose(sf); }
                page = PAGE_LYRICS;
                lyr_cam_valid = 0;
                audio_seek(26.0);
            }
            if (dump_frames == 75) {
                char fp[600];
                snprintf(fp, sizeof(fp), "%s/shot_lyr_%dx%d.rgba", dump_dir, screen->w, screen->h);
                FILE* sf = fopen(fp, "wb");
                if (sf) { fwrite(screen->px, 4, (size_t)screen->w * screen->h, sf); fclose(sf); }
                be_end();
                break;
            }
        }

        update_hover_cursor();
        int cw = screen->w < stride ? screen->w : stride;
        for (int y = 0; y < screen->h; y++) {
            memcpy(fb + (size_t)y * stride, screen->px + (size_t)y * screen->w, (size_t)cw * 4);
        }
        be_end();
    }

    audio_exit();
    be_exit();
    if (lyr) lyrics_free(lyr);
    meta_clear(&meta);
    free_all_thumbs();
    free(tracks); free(vis_idx);
    free(cover_src); free(cover_mini); free(poster_mini); free(poster_hero); free(poster_ico);
    if (screen) canvas_free(screen);
    if (trans_shot) canvas_free(trans_shot);
    if (mesh_lib) canvas_free(mesh_lib);
    if (mesh_lyr) canvas_free(mesh_lyr);
    if (font) font_free(font);
    return 0;
}
