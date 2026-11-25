#include "backend.h"
#include "audio.h"
#include "decode.h"
#include "meta.h"
#include "lrc.h"
#include "draw.h"
#include "util.h"
#include "plat.h"
#include "stb_image.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

extern "C" {
extern const unsigned char font_ttf_start[];
extern const unsigned char font_ttf_end[];
extern const unsigned char poster_start[];
extern const unsigned char poster_end[];
}

namespace sirene {

constexpr int MAX_TRACKS    = 8192;
constexpr int MAX_DIRS      = 12;
constexpr int MAX_THUMBS    = 240;
constexpr int THUMB_SZ      = 200;
constexpr int MINI_ART_SZ   = 54;
constexpr int COVER_SRC_SZ  = 360;
constexpr int ROW_H         = 54;
constexpr int RAIL_W        = 76;
constexpr int BG_ART_W      = 48;
constexpr int BG_ART_H      = 27;
constexpr int MAX_GRID_HITS = 96;
constexpr int LYRIC_MAX_HITS = 48;
constexpr float kPi         = 3.14159265358979323846f;
constexpr float FADE_DUR    = 0.28f;

struct CanvasDel { void operator()(Canvas* c) const { if (c) canvas_free(c); } };
struct FontDel   { void operator()(Font* f) const { if (f) font_free(f); } };
struct LyricsDel { void operator()(Lyrics* l) const { if (l) lyrics_free(l); } };
using CanvasPtr = std::unique_ptr<Canvas, CanvasDel>;
using FontPtr   = std::unique_ptr<Font, FontDel>;
using LyricsPtr = std::unique_ptr<Lyrics, LyricsDel>;

struct TrackMetaGuard {
    TrackMeta m{};
    ~TrackMetaGuard() { meta_clear(&m); }
    void clear() { meta_clear(&m); }
};

struct Rect { int x = 0, y = 0, w = 0, h = 0; };

inline bool in_rect(int x, int y, int rx, int ry, int rw, int rh) {
    return rw > 0 && rh > 0 && x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}
inline bool in_rc(int x, int y, Rect r) { return in_rect(x, y, r.x, r.y, r.w, r.h); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline uint8_t u8clamp(int v) { return v < 0 ? 0 : (v > 255 ? 255 : static_cast<uint8_t>(v)); }
inline uint8_t col_a(uint32_t col) { return static_cast<uint8_t>((col >> 24) & 0xFFu); }
inline uint32_t with_alpha(uint32_t col, uint8_t a) { return (col & 0x00FFFFFFu) | (static_cast<uint32_t>(a) << 24); }

inline float smooth_u(float t) {
    t = clampf(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

uint32_t mix_col(uint32_t a, uint32_t b, float t) {
    t = clampf(t, 0.0f, 1.0f);
    const uint32_t ar = a & 0xFFu, ag = (a >> 8) & 0xFFu, ab = (a >> 16) & 0xFFu;
    const uint32_t br = b & 0xFFu, bg = (b >> 8) & 0xFFu, bb = (b >> 16) & 0xFFu;
    return COL(
        static_cast<uint8_t>(ar + (static_cast<float>(br) - static_cast<float>(ar)) * t + 0.5f),
        static_cast<uint8_t>(ag + (static_cast<float>(bg) - static_cast<float>(ag)) * t + 0.5f),
        static_cast<uint8_t>(ab + (static_cast<float>(bb) - static_cast<float>(ab)) * t + 0.5f), 255);
}

uint32_t mix_col_a(uint32_t a, uint32_t b, float t) {
    t = clampf(t, 0.0f, 1.0f);
    const uint32_t ar = a & 0xFFu, ag = (a >> 8) & 0xFFu, ab = (a >> 16) & 0xFFu, aa = (a >> 24) & 0xFFu;
    const uint32_t br = b & 0xFFu, bg = (b >> 8) & 0xFFu, bb = (b >> 16) & 0xFFu, ba = (b >> 24) & 0xFFu;
    return COL(
        static_cast<uint8_t>(ar + (static_cast<float>(br) - static_cast<float>(ar)) * t + 0.5f),
        static_cast<uint8_t>(ag + (static_cast<float>(bg) - static_cast<float>(ag)) * t + 0.5f),
        static_cast<uint8_t>(ab + (static_cast<float>(bb) - static_cast<float>(ab)) * t + 0.5f),
        static_cast<uint8_t>(aa + (static_cast<float>(ba) - static_cast<float>(aa)) * t + 0.5f));
}

int ascii_icmp(const std::string& a, const std::string& b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const unsigned char ca = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(a[i])));
        const unsigned char cb = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(b[i])));
        if (ca != cb) return static_cast<int>(ca) - static_cast<int>(cb);
    }
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    return 0;
}

bool ascii_icase_contains(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    if (hay.size() < needle.size()) return false;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](char c1, char c2) {
                              return std::tolower(static_cast<unsigned char>(c1)) ==
                                     std::tolower(static_cast<unsigned char>(c2));
                          });
    return it != hay.end();
}

struct Track {
    std::string path;
    std::string title;
    std::string artist;
    std::string album;
    char fmt[16] = "AUDIO";
    double dur = 0.0;
    int dir_idx = 0;
    int fav = 0;
    int meta_state = 0; // 0=none, 1=quick text/art, 2=duration probed
    std::vector<uint32_t> thumb;
};

enum { DRAG_NONE = 0, DRAG_ISLAND_PROG = 1, DRAG_HERO_PROG = 2, DRAG_VOL = 3, DRAG_SB = 4, DRAG_LIST = 5, DRAG_LYRICS = 6 };

struct WrappedLyrLine {
    std::vector<std::string> o_lines;
    std::vector<std::string> s_lines;
    int y = 0;
    int h = 0;
};

struct GridHit { Rect card{}; Rect fav{}; int track_idx = 0; };
struct LyrHit  { Rect rc{}; double t = 0.0; };

enum { PAGE_LIBRARY = 0, PAGE_LYRICS = 1 };
enum { TAB_ALL = 0, TAB_FAV = 1, TAB_DIR = 2 };
enum { FMT_FILT_ALL = 0, FMT_FILT_FAV = 1, FMT_FILT_LOSSLESS = 2, FMT_FILT_MP3 = 3, FMT_FILT_OTHER = 4 };
enum { MODE_LOOP_ALL = 0, MODE_LOOP_ONE = 1, MODE_SHUFFLE = 2 };
enum { PC_SORT_DEFAULT = 0, PC_SORT_TITLE = 1, PC_SORT_ARTIST = 2, PC_SORT_ALBUM = 3 };

struct AppState {
    CanvasPtr screen;
    CanvasPtr trans_shot;
    CanvasPtr bg_mesh;
    FontPtr   font;
    int W = 1360, H = 840;
    float ui_scale = 1.0f;

    std::vector<Track> tracks;
    std::vector<int>   vis_idx;
    std::vector<std::string> scan_dirs;
    int active_dir = -1;

    int page = PAGE_LIBRARY;
    int lib_tab = TAB_ALL;
    int fmt_filter = FMT_FILT_ALL;
    int view_grid = 1;
    int sort_mode = PC_SORT_DEFAULT;
    int show_diag = 0;
    int search_focused = 0;
    std::string search_q;

    int playing_idx = -1;
    int play_mode = MODE_LOOP_ALL;
    int is_light = 0;
    float theme_pos = 0.0f;
    float light_t = 0.0f;
    float saved_vol = 0.85f;
    int shortcut_done = 0;

    TrackMetaGuard meta;
    LyricsPtr lyr;
    int lyr_req_id = 0;
    std::string p_title = "Sir\xC3\xA8ne";
    std::string p_artist;
    std::string p_album;
    char p_fmt[16] = "AUDIO";

    std::vector<uint32_t> cover_src;
    std::vector<uint32_t> cover_mini;
    std::vector<uint32_t> poster_mini;
    std::vector<uint32_t> poster_hero;
    std::vector<uint32_t> poster_ico;
    int cover_ready = 0;

    float cur_qf[4][3] = { { 34, 88, 62 }, { 64, 128, 72 }, { 22, 48, 44 }, { 42, 92, 68 } };
    float tgt_qf[4][3] = { { 34, 88, 62 }, { 64, 128, 72 }, { 22, 48, 44 }, { 42, 92, 68 } };
    std::array<float, BG_ART_W * BG_ART_H * 3> cur_bg_art{};
    std::array<float, BG_ART_W * BG_ART_H * 3> tgt_bg_art{};
    int bg_art_init = 0;

    std::string bg_img_path;
    int bg_img_raw_w = 0, bg_img_raw_h = 0;
    float bg_img_avg_lum = 128.0f;
    std::vector<uint32_t> bg_img_raw;
    CanvasPtr bg_fit_lib, bg_fit_lyr, bg_out_cache;
    int bg_out_w = 0, bg_out_h = 0, bg_out_imm = -1, bg_out_lt = -1;

    uint32_t seed_rng = 0x9E3779B9u;

    double dt = 1.0 / 60.0;
    uint64_t last_ns = 0;
    float fade_time = 0.0f;
    int   fade_armed = 0;

    float scroll_px = 0.0f, scroll_target = 0.0f;
    float lyr_cam_y = 0.0f;
    int   lyr_cam_valid = 0;
    float lyr_manual_timer = 0.0f;
    float wave_phase = 0.0f, ambient_t = 0.0f, scallop_rot = 0.0f;
    float island_exp = 0.0f;
    float rail_pill_y = 92.0f;
    std::array<float, 28> bars{};
    int ui_cursor_hand = 0;
    int probe_cursor = 0;

    int   hov_card_idx = -1;
    float hov_card_t = 0.0f;

    int scrub_drag = 0;
    double scrub_sec = 0.0;
    int vol_drag = 0;
    int sb_drag = 0;
    int drag_mode = DRAG_NONE;
    int drag_start_x = 0, drag_start_y = 0, drag_moved = 0;
    int drag_cand_track = -1, drag_cand_fav = 0, drag_cand_hero = 0;
    double drag_cand_lyr_t = -1.0;
    float scroll_vel = 0.0f;
    float lyr_scroll_vel = 0.0f;
    float lyr_max_scroll = 0.0f;

    const Lyrics* lyr_wrap_ptr = nullptr;
    int lyr_wrap_w = 0;
    float lyr_wrap_opx = 0.0f, lyr_wrap_spx = 0.0f;
    std::vector<WrappedLyrLine> lyr_wrapped;

    int toast_timer = 0;
    std::string toast_text;

    uint32_t C_BASE = 0, C_RAIL = 0, C_SURFACE = 0, C_GLASS = 0, C_GLASS_HOV = 0, C_GLASS_ACT = 0;
    uint32_t C_ACCENT = 0, C_ON_ACCENT = 0, C_HERO_L = 0, C_HERO_R = 0;
    uint32_t C_TXT = 0, C_TXT2 = 0, C_TXT3 = 0, C_DIV = 0, C_TRACK_BG = 0, C_HEART = 0, C_PLAY_BTN = 0, C_PLAY_ICO = 0;

    Rect rc_menu_btn{}, rc_nav_all{}, rc_nav_fav{}, rc_nav_lyr{}, rc_nav_add{}, rc_nav_diag{};
    Rect rc_nav_bg{}, rc_nav_theme{}, rc_nav_view{}, rc_nav_rescan{};
    Rect rc_search_box{}, rc_search_clear{}, rc_sort_btn{}, rc_fs_btn{}, rc_view_pill{}, rc_bg_btn{}, rc_bg_clear{};
    std::array<Rect, 5> rc_filt_chips{};
    Rect rc_dir_chip{};
    Rect rc_hero_card{}, rc_hero_cov{}, rc_hero_play{}, rc_hero_prog{};
    Rect rc_sb_track{}, rc_fab_import{};
    Rect rc_back_lib{}, rc_lyr_bg{}, rc_lyr_bg_clear{}, rc_lyr_theme{}, rc_lyr_fs{};
    Rect rc_island{}, rc_bar_cov{}, rc_bar_fav{}, rc_bar_prev{}, rc_bar_play{}, rc_bar_next{};
    Rect rc_bar_mode{}, rc_bar_lyr{}, rc_bar_vol_ico{}, rc_bar_vol_trk{}, rc_bar_prog{};
    int ui_px0 = 0, ui_px1 = 100, ui_py = 0;
    int ui_hpx0 = 0, ui_hpx1 = 100;
    int ui_vx0 = 0, ui_vx1 = 100;
    int ui_list_x0 = 0, ui_list_x1 = 0, ui_list_y0 = 0, ui_list_y1 = 0;

    int grid_cols = 4, grid_w = 1000, grid_gap = 18, card_w = 180, card_h = 240, hero_w = 378;
    std::vector<GridHit> grid_hits;
    std::vector<LyrHit> lyr_hits;
};

AppState g;

inline uint8_t theme_shadow(uint8_t dark_a) {
    return static_cast<uint8_t>(static_cast<float>(dark_a) * (1.0f - 0.58f * g.light_t));
}

uint32_t next_rand() {
    g.seed_rng = g.seed_rng * 1664525u + 1013904223u;
    return g.seed_rng;
}

void show_toast(const char* s) {
    g.toast_text = s ? s : "";
    g.toast_timer = 130;
}

void capture_fade() {
    if (!g.screen) return;
    if (g.trans_shot && (g.trans_shot->w != g.screen->w || g.trans_shot->h != g.screen->h)) {
        g.trans_shot.reset();
    }
    if (!g.trans_shot) g.trans_shot.reset(canvas_new(g.screen->w, g.screen->h));
    if (g.trans_shot) {
        std::memcpy(g.trans_shot->px, g.screen->px, static_cast<size_t>(g.screen->w) * g.screen->h * 4);
        g.fade_time = FADE_DUR;
        g.fade_armed = 1;
    }
}

void track_format_tag(const char* path, char* out, size_t cap) {
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

std::string basename_str(const std::string& path) {
    const size_t pos = path.find_last_of("/\\");
    return (pos != std::string::npos && pos + 1 < path.size()) ? path.substr(pos + 1) : path;
}

void ensure_dir_recursive(const char* path) {
    std::string tmp(path ? path : "");
    for (size_t i = 1; i < tmp.size(); ++i) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            plat_mkdir_p(tmp.c_str());
            tmp[i] = '/';
        }
    }
    if (!tmp.empty()) plat_mkdir_p(tmp.c_str());
}

void resample_cover_square(const uint32_t* src, int sw, int sh, uint32_t* dst, int dw, int crop) {
    if (!src || !dst || sw <= 0 || sh <= 0 || dw <= 0) return;
    int sx0 = 0, sy0 = 0, cw = sw, ch = sh;
    if (crop) {
        const int m = std::min(sw, sh);
        sx0 = (sw - m) / 2;
        sy0 = (sh - m) / 2;
        cw = ch = m;
    }
    std::vector<uint32_t> tmp(static_cast<size_t>(cw) * ch);
    for (int y = 0; y < ch; ++y) {
        std::memcpy(tmp.data() + static_cast<size_t>(y) * cw,
                    src + static_cast<size_t>(sy0 + y) * sw + sx0,
                    static_cast<size_t>(cw) * 4);
    }
    resample_bilinear(tmp.data(), cw, ch, dst, dw, dw);
}

void make_default_art(uint32_t* dst, int n, const char* key) {
    uint32_t h = 2166136261u;
    for (const auto* p = reinterpret_cast<const unsigned char*>(key); *p; ++p) h = (h ^ *p) * 16777619u;
    const float hue = static_cast<float>(h % 360) * kPi / 180.0f;
    const uint8_t r0 = u8clamp(static_cast<int>(88 + 52 * std::sin(hue)));
    const uint8_t g0 = u8clamp(static_cast<int>(108 + 52 * std::sin(hue + 2.1f)));
    const uint8_t b0 = u8clamp(static_cast<int>(134 + 52 * std::sin(hue + 4.2f)));
    for (int y = 0; y < n; ++y) {
        const float fy = static_cast<float>(y) / static_cast<float>(n);
        for (int x = 0; x < n; ++x) {
            const float fx = static_cast<float>(x) / static_cast<float>(n);
            const uint8_t r = u8clamp(static_cast<int>(r0 * (1.0f - 0.30f * fy) + 28 * fx));
            const uint8_t g = u8clamp(static_cast<int>(g0 * (1.0f - 0.28f * fy) + 22 * fx));
            const uint8_t b = u8clamp(static_cast<int>(b0 * (1.0f - 0.20f * fy) + 36 * fx));
            dst[static_cast<size_t>(y) * n + x] = COL(r, g, b, 255);
        }
    }
}

void update_target_cover_colors() {
    std::array<uint32_t, BG_ART_W * BG_ART_H> tmp_bg{};
    const uint32_t* art_src = (g.cover_ready && !g.cover_src.empty()) ? g.cover_src.data()
                            : (!g.poster_hero.empty() ? g.poster_hero.data() : nullptr);
    if (g.cover_ready && !g.cover_src.empty()) {
        for (int q = 0; q < 4; ++q) {
            const int qx = (q & 1) ? COVER_SRC_SZ / 2 : 0;
            const int qy = (q & 2) ? COVER_SRC_SZ / 2 : 0;
            uint64_t sr = 0, sg = 0, sb = 0, cnt = 0;
            for (int y = qy + 20; y < qy + COVER_SRC_SZ / 2 - 20; y += 8) {
                for (int x = qx + 20; x < qx + COVER_SRC_SZ / 2 - 20; x += 8) {
                    const uint32_t c = g.cover_src[static_cast<size_t>(y) * COVER_SRC_SZ + x];
                    sr += c & 0xFFu; sg += (c >> 8) & 0xFFu; sb += (c >> 16) & 0xFFu; ++cnt;
                }
            }
            if (cnt > 0) {
                g.tgt_qf[q][0] = static_cast<float>(sr / cnt);
                g.tgt_qf[q][1] = static_cast<float>(sg / cnt);
                g.tgt_qf[q][2] = static_cast<float>(sb / cnt);
            }
        }
    } else {
        constexpr float def[4][3] = { { 42, 118, 86 }, { 76, 152, 92 }, { 24, 62, 58 }, { 48, 108, 82 } };
        std::memcpy(g.tgt_qf, def, sizeof(def));
    }

    if (art_src) {
        const int crop_h = std::max(16, (COVER_SRC_SZ * BG_ART_H) / BG_ART_W);
        const int y0 = std::max(0, (COVER_SRC_SZ - crop_h) / 2);
        resample_bilinear(art_src + static_cast<size_t>(y0) * COVER_SRC_SZ, COVER_SRC_SZ, crop_h,
                          tmp_bg.data(), BG_ART_W, BG_ART_H);
        box_blur(tmp_bg.data(), BG_ART_W, BG_ART_H, 5, 2);
    } else {
        for (int y = 0; y < BG_ART_H; ++y) {
            const float fy = static_cast<float>(y) / static_cast<float>(BG_ART_H - 1);
            for (int x = 0; x < BG_ART_W; ++x) {
                const float fx = static_cast<float>(x) / static_cast<float>(BG_ART_W - 1);
                const uint32_t t = mix_col(COL(42, 118, 86, 255), COL(76, 152, 92, 255), fx);
                const uint32_t b = mix_col(COL(24, 62, 58, 255), COL(48, 108, 82, 255), fx);
                tmp_bg[static_cast<size_t>(y) * BG_ART_W + x] = mix_col(t, b, fy);
            }
        }
    }
    for (int i = 0; i < BG_ART_W * BG_ART_H; ++i) {
        const uint32_t c = tmp_bg[i];
        g.tgt_bg_art[static_cast<size_t>(i) * 3 + 0] = static_cast<float>(c & 0xFFu);
        g.tgt_bg_art[static_cast<size_t>(i) * 3 + 1] = static_cast<float>((c >> 8) & 0xFFu);
        g.tgt_bg_art[static_cast<size_t>(i) * 3 + 2] = static_cast<float>((c >> 16) & 0xFFu);
    }
    if (!g.bg_art_init) {
        g.cur_bg_art = g.tgt_bg_art;
        g.bg_art_init = 1;
    }
}

uint32_t clamp_mesh_lum(uint32_t col, int is_light) {
    const float r = static_cast<float>(col & 0xFFu);
    const float gr = static_cast<float>((col >> 8) & 0xFFu);
    const float b = static_cast<float>((col >> 16) & 0xFFu);
    const float lum = 0.299f * r + 0.587f * gr + 0.114f * b;
    if (!is_light) {
        if (lum > 112.0f) {
            const float sc = 112.0f / lum;
            col = COL(static_cast<uint8_t>(r * sc), static_cast<uint8_t>(gr * sc), static_cast<uint8_t>(b * sc), 255);
        }
        return mix_col(col, COL(18, 22, 30, 255), 0.34f);
    }
    return mix_col(col, COL(236, 240, 246, 255), (lum < 185.0f) ? 0.66f : 0.48f);
}

uint32_t boost_pastel_accent(float r, float gr, float b, int is_light) {
    const float mx = std::max({ r, gr, b });
    const float mn = std::min({ r, gr, b });
    if (mx - mn < 18.0f) {
        return is_light ? COL(56, 102, 198, 255) : COL(132, 186, 255, 255);
    }
    const float avg = (r + gr + b) / 3.0f;
    r  = clampf(avg + (r  - avg) * 1.68f, 0.0f, 255.0f);
    gr = clampf(avg + (gr - avg) * 1.68f, 0.0f, 255.0f);
    b  = clampf(avg + (b  - avg) * 1.68f, 0.0f, 255.0f);
    const float peak = std::max({ r, gr, b });
    if (!is_light) {
        if (peak > 1.0f && peak < 218.0f) {
            const float sc = std::min(2.2f, 224.0f / peak);
            r = clampf(r * sc, 0.0f, 255.0f);
            gr = clampf(gr * sc, 0.0f, 255.0f);
            b = clampf(b * sc, 0.0f, 255.0f);
        }
        const uint32_t col = COL(static_cast<uint8_t>(r), static_cast<uint8_t>(gr), static_cast<uint8_t>(b), 255);
        return mix_col(col, COL(245, 248, 255, 255), 0.16f);
    }
    if (peak > 160.0f) {
        const float sc = 152.0f / peak;
        r *= sc; gr *= sc; b *= sc;
    }
    return COL(static_cast<uint8_t>(r), static_cast<uint8_t>(gr), static_cast<uint8_t>(b), 255);
}

void step_palette_and_ambient() {
    const float k = 1.0f - std::exp(-4.0f * static_cast<float>(g.dt));
    for (int q = 0; q < 4; ++q) {
        for (int c = 0; c < 3; ++c) g.cur_qf[q][c] += (g.tgt_qf[q][c] - g.cur_qf[q][c]) * k;
    }
    if (g.bg_art_init) {
        constexpr size_t N = static_cast<size_t>(BG_ART_W) * BG_ART_H * 3;
        for (size_t i = 0; i < N; ++i) g.cur_bg_art[i] += (g.tgt_bg_art[i] - g.cur_bg_art[i]) * k;
    }

    const float tgt_theme = g.is_light ? 1.0f : 0.0f;
    const float theme_step = static_cast<float>(g.dt) / 0.85f;
    if (g.theme_pos < tgt_theme) g.theme_pos = std::min(tgt_theme, g.theme_pos + theme_step);
    else if (g.theme_pos > tgt_theme) g.theme_pos = std::max(tgt_theme, g.theme_pos - theme_step);

    const float t_ui = smooth_u(g.theme_pos);
    g.light_t = t_ui;

    uint32_t q_dark[4]{}, q_light[4]{};
    int best_q = 0;
    float best_chroma = -1.0f;
    for (int q = 0; q < 4; ++q) {
        const float r = g.cur_qf[q][0], gr = g.cur_qf[q][1], b = g.cur_qf[q][2];
        const float mx = std::max({ r, gr, b });
        const float mn = std::min({ r, gr, b });
        const float lum = (r + gr + b) / 3.0f;
        const float score = (mx - mn) * (lum > 38.0f && lum < 228.0f ? 1.0f : 0.45f);
        if (score > best_chroma) { best_chroma = score; best_q = q; }
        const uint32_t raw = COL(static_cast<uint8_t>(r), static_cast<uint8_t>(gr), static_cast<uint8_t>(b), 255);
        q_dark[q]  = clamp_mesh_lum(raw, 0);
        q_light[q] = clamp_mesh_lum(raw, 1);
    }

    float acc_r = g.cur_qf[best_q][0], acc_g = g.cur_qf[best_q][1], acc_b = g.cur_qf[best_q][2];
    if (best_chroma < 14.0f) {
        acc_r = (g.cur_qf[0][0] + g.cur_qf[1][0] + g.cur_qf[2][0] + g.cur_qf[3][0]) * 0.25f;
        acc_g = (g.cur_qf[0][1] + g.cur_qf[1][1] + g.cur_qf[2][1] + g.cur_qf[3][1]) * 0.25f;
        acc_b = (g.cur_qf[0][2] + g.cur_qf[1][2] + g.cur_qf[2][2] + g.cur_qf[3][2]) * 0.25f;
    }

    const uint32_t d_accent = boost_pastel_accent(acc_r, acc_g, acc_b, 0);
    const uint32_t l_accent = boost_pastel_accent(acc_r, acc_g, acc_b, 1);
    g.C_ACCENT = mix_col(d_accent, l_accent, t_ui);

    const uint32_t d_base   = COL(12, 14, 18, 255);
    const uint32_t d_rail   = with_alpha(mix_col(COL(14, 17, 23, 255), q_dark[2], 0.18f), 168);
    const uint32_t d_surf   = with_alpha(mix_col(COL(22, 26, 34, 255), q_dark[1], 0.16f), 152);
    const uint32_t d_herol  = mix_col(COL(18, 22, 28, 255), q_dark[0], 0.32f);
    const uint32_t d_heror  = mix_col(COL(22, 28, 36, 255), d_accent, 0.36f);

    const uint32_t l_base   = mix_col(COL(234, 238, 244, 255), q_light[1], 0.12f);
    const uint32_t l_rail   = with_alpha(mix_col(COL(238, 242, 248, 255), q_light[2], 0.12f), 192);
    const uint32_t l_surf   = with_alpha(mix_col(COL(244, 247, 251, 255), q_light[1], 0.08f), 184);
    const uint32_t l_herol  = mix_col(COL(242, 246, 250, 255), l_accent, 0.16f);
    const uint32_t l_heror  = mix_col(COL(232, 238, 246, 255), l_accent, 0.30f);

    g.C_BASE      = mix_col(d_base, l_base, t_ui);
    g.C_RAIL      = mix_col_a(d_rail, l_rail, t_ui);
    g.C_SURFACE   = mix_col_a(d_surf, l_surf, t_ui);
    g.C_GLASS     = mix_col_a(COL(255, 255, 255, 22), COL(18, 24, 38, 16), t_ui);
    g.C_GLASS_HOV = mix_col_a(COL(255, 255, 255, 38), COL(18, 24, 38, 28), t_ui);
    g.C_GLASS_ACT = mix_col_a(COL(255, 255, 255, 56), COL(18, 24, 38, 42), t_ui);
    g.C_ON_ACCENT = mix_col(COL(14, 18, 24, 255), COL(252, 253, 255, 255), t_ui);
    g.C_HERO_L    = mix_col(d_herol, l_herol, t_ui);
    g.C_HERO_R    = mix_col(d_heror, l_heror, t_ui);
    g.C_DIV       = mix_col_a(COL(255, 255, 255, 28), COL(18, 24, 38, 24), t_ui);
    g.C_TRACK_BG  = mix_col_a(COL(255, 255, 255, 42), COL(18, 24, 38, 32), t_ui);
    g.C_HEART     = mix_col(COL(248, 82, 104, 255), COL(228, 60, 82, 255), t_ui);
    g.C_PLAY_BTN  = mix_col(COL(248, 250, 252, 255), l_accent, t_ui);
    g.C_PLAY_ICO  = mix_col(COL(14, 18, 22, 255), COL(252, 253, 255, 255), t_ui);

    const float t_txt = smooth_u((t_ui - 0.44f) / 0.12f);
    g.C_TXT  = mix_col(COL(250, 252, 255, 255), COL(18, 22, 32, 255), t_txt);
    g.C_TXT2 = mix_col(COL(204, 210, 224, 255), COL(64, 72, 92, 255), t_txt);
    g.C_TXT3 = mix_col(COL(148, 156, 174, 255), COL(110, 120, 140, 255), t_txt);

    static int prev_dark_bar = -1;
    const int want_dark_bar = (t_ui < 0.5f) ? 1 : 0;
    if (want_dark_bar != prev_dark_bar) {
        be_set_dark_titlebar(want_dark_bar);
        prev_dark_bar = want_dark_bar;
    }

}

void invalidate_bg_cache() {
    g.bg_fit_lib.reset();
    g.bg_fit_lyr.reset();
    g.bg_out_cache.reset();
    g.bg_out_w = g.bg_out_h = 0;
    g.bg_out_imm = g.bg_out_lt = -1;
}

bool is_image_file(const char* path) {
    if (!path || !*path) return false;
    return ci_ends_with(path, ".png") || ci_ends_with(path, ".jpg") || ci_ends_with(path, ".jpeg") ||
           ci_ends_with(path, ".bmp") || ci_ends_with(path, ".tga") || ci_ends_with(path, ".gif");
}

void render_wallpaper_backdrop(int is_immersive) {
    if (!g.screen || !g.screen->px || g.bg_img_raw.empty()) return;
    const int dw = g.screen->w, dh = g.screen->h;
    if (dw <= 0 || dh <= 0) return;

    if (!g.bg_fit_lib || g.bg_fit_lib->w != dw || g.bg_fit_lib->h != dh) {
        const int sw = g.bg_img_raw_w, sh = g.bg_img_raw_h;
        if (sw <= 0 || sh <= 0) return;
        const float src_ar = static_cast<float>(sw) / static_cast<float>(sh);
        const float dst_ar = static_cast<float>(dw) / static_cast<float>(dh);
        int cx = 0, cy = 0, cw = sw, ch = sh;
        if (src_ar > dst_ar) {
            cw = std::clamp(static_cast<int>(static_cast<float>(sh) * dst_ar + 0.5f), 1, sw);
            cx = (sw - cw) / 2;
        } else {
            ch = std::clamp(static_cast<int>(static_cast<float>(sw) / dst_ar + 0.5f), 1, sh);
            cy = (sh - ch) / 2;
        }
        g.bg_fit_lib.reset(canvas_new(dw, dh));
        if (!g.bg_fit_lib) return;
        if (cx == 0 && cy == 0 && cw == sw && ch == sh) {
            resample_bilinear(g.bg_img_raw.data(), sw, sh, g.bg_fit_lib->px, dw, dh);
        } else {
            std::vector<uint32_t> crop(static_cast<size_t>(cw) * ch);
            for (int y = 0; y < ch; ++y) {
                std::memcpy(crop.data() + static_cast<size_t>(y) * cw,
                            g.bg_img_raw.data() + static_cast<size_t>(cy + y) * sw + cx,
                            static_cast<size_t>(cw) * 4);
            }
            resample_bilinear(crop.data(), cw, ch, g.bg_fit_lib->px, dw, dh);
        }
        g.bg_fit_lyr.reset();
        g.bg_out_cache.reset();
        g.bg_out_imm = g.bg_out_lt = -1;
    }

    if (is_immersive && (!g.bg_fit_lyr || g.bg_fit_lyr->w != dw || g.bg_fit_lyr->h != dh)) {
        g.bg_fit_lyr.reset(canvas_new(dw, dh));
        if (g.bg_fit_lyr) {
            std::memcpy(g.bg_fit_lyr->px, g.bg_fit_lib->px, static_cast<size_t>(dw) * dh * 4);
            box_blur(g.bg_fit_lyr->px, dw, dh, std::clamp(dw / 140, 5, 12), 2);
        }
    }

    const int imm_flag = is_immersive ? 1 : 0;
    const int lt_q = std::clamp(static_cast<int>(g.light_t * 255.0f + 0.5f), 0, 255);
    if (!g.bg_out_cache || g.bg_out_w != dw || g.bg_out_h != dh || g.bg_out_imm != imm_flag || g.bg_out_lt != lt_q) {
        if (!g.bg_out_cache || g.bg_out_w != dw || g.bg_out_h != dh) {
            g.bg_out_cache.reset(canvas_new(dw, dh));
        }
        const Canvas* src_c = (imm_flag && g.bg_fit_lyr) ? g.bg_fit_lyr.get() : g.bg_fit_lib.get();
        if (!g.bg_out_cache || !g.bg_out_cache->px || !src_c || !src_c->px) return;

        const float t = static_cast<float>(lt_q) / 255.0f;
        const float u_bright = clampf((g.bg_img_avg_lum - 60.0f) / 160.0f, 0.0f, 1.0f);
        const float u_dim    = clampf((180.0f - g.bg_img_avg_lum) / 130.0f, 0.0f, 1.0f);
        const float scrim_dark  = imm_flag ? (0.34f + 0.34f * u_bright) : (0.20f + 0.38f * u_bright);
        const float scrim_light = imm_flag ? (0.54f + 0.18f * u_dim)    : (0.46f + 0.20f * u_dim);
        const float scrim_f = scrim_dark + (scrim_light - scrim_dark) * t;
        const uint32_t ua = static_cast<uint32_t>(std::clamp(static_cast<int>(scrim_f * 256.0f + 0.5f), 0, 256));
        const uint32_t na = 256u - ua;
        const uint32_t scrim_col = mix_col(COL(10, 12, 18, 255), COL(238, 242, 248, 255), t);
        const uint32_t sc_rb = (scrim_col & 0x00FF00FFu) * ua;
        const uint32_t sc_g  = ((scrim_col >> 8) & 0xFFu) * ua;

        const uint32_t* sp = src_c->px;
        uint32_t* dp = g.bg_out_cache->px;
        const size_t N = static_cast<size_t>(dw) * dh;
        for (size_t i = 0; i < N; ++i) {
            const uint32_t p = sp[i];
            const uint32_t rb = (((p & 0x00FF00FFu) * na + sc_rb + 0x00800080u) >> 8) & 0x00FF00FFu;
            const uint32_t gv = ((((p >> 8) & 0xFFu) * na + sc_g + 128u) & 0xFF00u);
            dp[i] = 0xFF000000u | rb | gv;
        }
        g.bg_out_w = dw; g.bg_out_h = dh; g.bg_out_imm = imm_flag; g.bg_out_lt = lt_q;
    }

    std::memcpy(g.screen->px, g.bg_out_cache->px, static_cast<size_t>(dw) * dh * 4);
}

void draw_frosted_ambient_bg(int is_immersive) {
    if (!g.screen || !g.screen->px) return;
    if (!g.bg_img_raw.empty()) {
        render_wallpaper_backdrop(is_immersive);
        return;
    }
    if (!g.bg_art_init) update_target_cover_colors();

    const int dw = g.screen->w, dh = g.screen->h;
    if (dw <= 0 || dh <= 0) return;

    const float lt = g.light_t;
    const float art_w = (is_immersive ? 0.62f : 0.46f) * (1.0f - lt) + (is_immersive ? 0.54f : 0.40f) * lt;
    const float breath = 0.04f * std::sin(g.ambient_t * 0.45f);
    const float bx1 = (is_immersive ? 0.22f : 0.25f) + 0.05f * std::cos(g.ambient_t * 0.22f);
    const float by1 = (is_immersive ? 0.42f : 0.30f) + 0.05f * std::sin(g.ambient_t * 0.19f);

    const float ar = static_cast<float>(g.C_ACCENT & 0xFFu);
    const float ag = static_cast<float>((g.C_ACCENT >> 8) & 0xFFu);
    const float ab = static_cast<float>((g.C_ACCENT >> 16) & 0xFFu);

    constexpr int MN = BG_ART_W * BG_ART_H;
    float mr[MN]{}, mg[MN]{}, mb[MN]{};
    float tr[MN]{}, tg[MN]{}, tb[MN]{};

    for (int y = 0; y < BG_ART_H; ++y) {
        const float fy = static_cast<float>(y) / static_cast<float>(BG_ART_H - 1);
        for (int x = 0; x < BG_ART_W; ++x) {
            const float fx = static_cast<float>(x) / static_cast<float>(BG_ART_W - 1);
            const size_t p = static_cast<size_t>(y) * BG_ART_W + x;
            const size_t idx = p * 3;
            const float dx = fx - bx1, dy = fy - by1;
            const float glow = clampf(0.22f / (1.0f + (dx * dx + dy * dy) * 3.6f) + breath * (1.0f - fy) + 0.14f, 0.08f, 0.44f);

            const float cr = g.cur_bg_art[idx + 0] + (ar - g.cur_bg_art[idx + 0]) * glow;
            const float cg = g.cur_bg_art[idx + 1] + (ag - g.cur_bg_art[idx + 1]) * glow;
            const float cb = g.cur_bg_art[idx + 2] + (ab - g.cur_bg_art[idx + 2]) * glow;

            // Dark branch: clamp luminance <= 112 and blend gently with deep base
            float dr = cr, dg = cg, db = cb;
            const float dlum = 0.299f * dr + 0.587f * dg + 0.114f * db;
            if (dlum > 112.0f) { const float sc = 112.0f / dlum; dr *= sc; dg *= sc; db *= sc; }
            dr = dr * 0.66f + 18.0f * 0.34f;
            dg = dg * 0.66f + 22.0f * 0.34f;
            db = db * 0.66f + 30.0f * 0.34f;
            dr = 11.0f + (dr - 11.0f) * art_w;
            dg = 13.0f + (dg - 13.0f) * art_w;
            db = 19.0f + (db - 19.0f) * art_w;

            // Light branch: soft milky tint
            const float llum = 0.299f * cr + 0.587f * cg + 0.114f * cb;
            const float lmix = (llum < 185.0f) ? 0.66f : 0.48f;
            float lr = cr * (1.0f - lmix) + 236.0f * lmix;
            float lg = cg * (1.0f - lmix) + 240.0f * lmix;
            float lb = cb * (1.0f - lmix) + 246.0f * lmix;
            lr = 228.0f + (lr - 228.0f) * art_w;
            lg = 234.0f + (lg - 234.0f) * art_w;
            lb = 244.0f + (lb - 244.0f) * art_w;

            mr[p] = dr + (lr - dr) * lt;
            mg[p] = dg + (lg - dg) * lt;
            mb[p] = db + (lb - db) * lt;
        }
    }

    // Two tiny float [1 2 1]/4 separable passes keep gradients ultra-smooth
    for (int pass = 0; pass < 2; ++pass) {
        for (int y = 0; y < BG_ART_H; ++y) {
            const int row = y * BG_ART_W;
            for (int x = 0; x < BG_ART_W; ++x) {
                const int xl = x > 0 ? x - 1 : 0;
                const int xr = (x + 1 < BG_ART_W) ? x + 1 : BG_ART_W - 1;
                tr[row + x] = (mr[row + xl] + mr[row + x] * 2.0f + mr[row + xr]) * 0.25f;
                tg[row + x] = (mg[row + xl] + mg[row + x] * 2.0f + mg[row + xr]) * 0.25f;
                tb[row + x] = (mb[row + xl] + mb[row + x] * 2.0f + mb[row + xr]) * 0.25f;
            }
        }
        for (int y = 0; y < BG_ART_H; ++y) {
            const int yu = y > 0 ? y - 1 : 0;
            const int yd = (y + 1 < BG_ART_H) ? y + 1 : BG_ART_H - 1;
            const int ru = yu * BG_ART_W, rc = y * BG_ART_W, rd = yd * BG_ART_W;
            for (int x = 0; x < BG_ART_W; ++x) {
                mr[rc + x] = (tr[ru + x] + tr[rc + x] * 2.0f + tr[rd + x]) * 0.25f;
                mg[rc + x] = (tg[ru + x] + tg[rc + x] * 2.0f + tg[rd + x]) * 0.25f;
                mb[rc + x] = (tb[ru + x] + tb[rc + x] * 2.0f + tb[rd + x]) * 0.25f;
            }
        }
    }

    uint32_t rb_q[MN]{}, g_q[MN]{};
    for (int i = 0; i < MN; ++i) {
        const int r12 = std::clamp(static_cast<int>(mr[i] * 16.0f + 0.5f), 0, 4080);
        const int g12 = std::clamp(static_cast<int>(mg[i] * 16.0f + 0.5f), 0, 4080);
        const int b12 = std::clamp(static_cast<int>(mb[i] * 16.0f + 0.5f), 0, 4080);
        rb_q[i] = (static_cast<uint32_t>(b12) << 16) | static_cast<uint32_t>(r12);
        g_q[i]  = static_cast<uint32_t>(g12);
    }

    int stk_x0[2560], stk_x1[2560], stk_wx[2560];
    std::vector<int> dyn_x0, dyn_x1, dyn_wx;
    int* ix0 = stk_x0;
    int* ix1 = stk_x1;
    int* wx  = stk_wx;
    if (dw > 2560) {
        dyn_x0.resize(dw); dyn_x1.resize(dw); dyn_wx.resize(dw);
        ix0 = dyn_x0.data(); ix1 = dyn_x1.data(); wx = dyn_wx.data();
    }

    const float sx = static_cast<float>(BG_ART_W) / static_cast<float>(dw);
    const float sy = static_cast<float>(BG_ART_H) / static_cast<float>(dh);
    for (int x = 0; x < dw; ++x) {
        const float fx = (static_cast<float>(x) + 0.5f) * sx - 0.5f;
        int x0 = static_cast<int>(std::floor(fx));
        int wxi = (x0 < 0) ? (x0 = 0, 0) : static_cast<int>((fx - static_cast<float>(x0)) * 16.0f + 0.5f);
        if (x0 >= BG_ART_W) x0 = BG_ART_W - 1;
        int x1 = x0 + 1;
        if (x1 >= BG_ART_W) { x1 = BG_ART_W - 1; wxi = 0; }
        ix0[x] = x0; ix1[x] = x1; wx[x] = std::clamp(wxi, 0, 16);
    }

    static constexpr uint8_t bayer4[4][4] = {
        {  0,  8,  2, 10 },
        { 12,  4, 14,  6 },
        {  3, 11,  1,  9 },
        { 15,  7, 13,  5 }
    };

    for (int y = 0; y < dh; ++y) {
        const float fy = (static_cast<float>(y) + 0.5f) * sy - 0.5f;
        int y0 = static_cast<int>(std::floor(fy));
        int wyi = (y0 < 0) ? (y0 = 0, 0) : static_cast<int>((fy - static_cast<float>(y0)) * 16.0f + 0.5f);
        if (y0 >= BG_ART_H) y0 = BG_ART_H - 1;
        int y1 = y0 + 1;
        if (y1 >= BG_ART_H) { y1 = BG_ART_H - 1; wyi = 0; }
        wyi = std::clamp(wyi, 0, 16);
        const uint32_t uwy = static_cast<uint32_t>(wyi), unwy = 16u - uwy;

        const uint32_t* sr0_rb = rb_q + static_cast<size_t>(y0) * BG_ART_W;
        const uint32_t* sr1_rb = rb_q + static_cast<size_t>(y1) * BG_ART_W;
        const uint32_t* sr0_g  = g_q  + static_cast<size_t>(y0) * BG_ART_W;
        const uint32_t* sr1_g  = g_q  + static_cast<size_t>(y1) * BG_ART_W;
        uint32_t* drow = g.screen->px + static_cast<size_t>(y) * dw;
        const uint8_t* brow = bayer4[y & 3];

        for (int x = 0; x < dw; ++x) {
            const int p0 = ix0[x], p1 = ix1[x];
            const uint32_t uwx = static_cast<uint32_t>(wx[x]), unwx = 16u - uwx;
            const uint32_t d = static_cast<uint32_t>(brow[x & 3]) << 4;

            const uint32_t trb = ((sr0_rb[p0] * unwx + sr0_rb[p1] * uwx + 0x00080008u) >> 4) & 0x0FFF0FFFu;
            const uint32_t brb = ((sr1_rb[p0] * unwx + sr1_rb[p1] * uwx + 0x00080008u) >> 4) & 0x0FFF0FFFu;
            const uint32_t orb = ((trb * unwy + brb * uwy + d * 0x00010001u) >> 8) & 0x00FF00FFu;

            const uint32_t tg_i = (sr0_g[p0] * unwx + sr0_g[p1] * uwx + 8u) >> 4;
            const uint32_t bg_i = (sr1_g[p0] * unwx + sr1_g[p1] * uwx + 8u) >> 4;
            const uint32_t og_i = (tg_i * unwy + bg_i * uwy + d) & 0x0000FF00u;

            drow[x] = 0xFF000000u | orb | og_i;
        }
    }
}

bool load_bg_image(const char* raw_path) {
    if (!raw_path || !*raw_path) return false;
    std::string path(raw_path);
    std::replace(path.begin(), path.end(), '\\', '/');

    FILE* f = plat_fopen(path.c_str(), "rb");
    if (!f) return false;
    if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); return false; }
    const long sz = std::ftell(f);
    if (sz <= 0 || sz > 64L * 1024L * 1024L || std::fseek(f, 0, SEEK_SET) != 0) {
        std::fclose(f);
        return false;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(sz));
    const size_t nread = std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (nread != bytes.size()) return false;

    int w = 0, h = 0, comp = 0;
    unsigned char* img = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &comp, 4);
    if (!img || w <= 0 || h <= 0 || w > 16384 || h > 16384) {
        if (img) stbi_image_free(img);
        return false;
    }

    std::vector<uint32_t> dec(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < dec.size(); ++i) {
        const uint8_t r  = img[i * 4 + 0];
        const uint8_t gr = img[i * 4 + 1];
        const uint8_t b  = img[i * 4 + 2];
        const uint8_t a  = img[i * 4 + 3];
        if (a == 255) {
            dec[i] = COL(r, gr, b, 255);
        } else {
            const uint32_t inv = 255u - a;
            dec[i] = COL(static_cast<uint8_t>((r * a + 18u * inv) / 255u),
                         static_cast<uint8_t>((gr * a + 22u * inv) / 255u),
                         static_cast<uint8_t>((b * a + 30u * inv) / 255u), 255);
        }
    }
    stbi_image_free(img);

    if (w > 1600 || h > 1600) {
        const float sc = 1600.0f / static_cast<float>(std::max(w, h));
        const int nw = std::max(1, static_cast<int>(static_cast<float>(w) * sc + 0.5f));
        const int nh = std::max(1, static_cast<int>(static_cast<float>(h) * sc + 0.5f));
        std::vector<uint32_t> small(static_cast<size_t>(nw) * nh);
        resample_bilinear(dec.data(), w, h, small.data(), nw, nh);
        dec.swap(small);
        w = nw; h = nh;
    }

    uint64_t lum_sum = 0;
    for (size_t i = 0; i < dec.size(); ++i) {
        const uint32_t c = dec[i];
        lum_sum += (77u * (c & 0xFFu) + 150u * ((c >> 8) & 0xFFu) + 29u * ((c >> 16) & 0xFFu)) >> 8;
    }
    g.bg_img_avg_lum = dec.empty() ? 128.0f : static_cast<float>(lum_sum) / static_cast<float>(dec.size());
    g.bg_img_path = std::move(path);
    g.bg_img_raw_w = w;
    g.bg_img_raw_h = h;
    g.bg_img_raw = std::move(dec);
    invalidate_bg_cache();
    return true;
}

void load_settings() {
    char p[600]{};
    snprintf(p, sizeof(p), "%s/pc_settings.cfg", be_config_dir());
    FILE* f = plat_fopen(p, "r");
    if (!f) return;
    char line[768]{};
    std::string saved_bg;
    while (fgets(line, sizeof(line), f)) {
        size_t len = std::strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        if (std::strncmp(line, "vol=", 4) == 0) {
            const float v = static_cast<float>(std::atof(line + 4));
            if (v >= 0.0f && v <= 1.0f) { audio_set_volume(v); g.saved_vol = v > 0.01f ? v : 0.85f; }
        } else if (std::strncmp(line, "mode=", 5) == 0) {
            const int m = std::atoi(line + 5);
            if (m >= 0 && m <= 2) g.play_mode = m;
        } else if (std::strncmp(line, "light=", 6) == 0) {
            g.is_light = std::atoi(line + 6) ? 1 : 0;
            g.theme_pos = g.is_light ? 1.0f : 0.0f;
            g.light_t = g.theme_pos;
        } else if (std::strncmp(line, "view_grid=", 10) == 0) {
            g.view_grid = std::atoi(line + 10) ? 1 : 0;
        } else if (std::strncmp(line, "sort=", 5) == 0) {
            const int sm = std::atoi(line + 5);
            if (sm >= 0 && sm <= 3) g.sort_mode = sm;
        } else if (std::strncmp(line, "shortcut=", 9) == 0) {
            g.shortcut_done = std::atoi(line + 9) ? 1 : 0;
        } else if (std::strncmp(line, "bg_img=", 7) == 0) {
            saved_bg = line + 7;
        }
    }
    fclose(f);
    if (!saved_bg.empty()) load_bg_image(saved_bg.c_str());
}

void save_settings() {
    ensure_dir_recursive(be_config_dir());
    char p[600]{};
    snprintf(p, sizeof(p), "%s/pc_settings.cfg", be_config_dir());
    FILE* f = plat_fopen(p, "w");
    if (!f) return;
    fprintf(f, "vol=%.3f\nmode=%d\nlight=%d\nview_grid=%d\nsort=%d\nshortcut=%d\nbg_img=%s\n",
            audio_volume(), g.play_mode, g.is_light, g.view_grid, g.sort_mode, g.shortcut_done,
            g.bg_img_path.c_str());
    fclose(f);
}

void clear_bg_image() {
    g.bg_img_path.clear();
    g.bg_img_raw.clear();
    g.bg_img_raw_w = g.bg_img_raw_h = 0;
    invalidate_bg_cache();
    save_settings();
    show_toast("已恢复默认封面流光背景");
}

void pick_bg_image_action() {
    char picked[600]{};
    if (!be_pick_image(picked, sizeof(picked))) return;
    if (load_bg_image(picked)) {
        save_settings();
        show_toast("已更换自定义背景图片");
    } else {
        show_toast("图片解码失败，请更换 PNG / JPG 图片");
    }
}

void load_dirs_cfg() {
    g.scan_dirs.clear();
    auto read_cfg_lines = [&](const char* fname, bool single) {
        char p[600]{};
        snprintf(p, sizeof(p), "%s/%s", be_config_dir(), fname);
        FILE* f = plat_fopen(p, "r");
        if (!f) return;
        char buf[600]{};
        while (static_cast<int>(g.scan_dirs.size()) < MAX_DIRS && fgets(buf, sizeof(buf), f)) {
            size_t n = std::strlen(buf);
            while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
            if (buf[0]) g.scan_dirs.emplace_back(buf);
            if (single) break;
        }
        fclose(f);
    };
    read_cfg_lines("music_dirs.cfg", false);
    if (g.scan_dirs.empty()) read_cfg_lines("music_dir.cfg", true);
    if (g.scan_dirs.empty()) {
        char defs[4][600]{};
        const int nd = be_default_music_dirs(defs, 4);
        for (int i = 0; i < nd && static_cast<int>(g.scan_dirs.size()) < MAX_DIRS; ++i) {
            int is_d = 0, is_r = 0;
            if (plat_stat_type(defs[i], &is_d, &is_r) == 0 && is_d) {
                g.scan_dirs.emplace_back(defs[i]);
                break;
            }
        }
    }
}

void save_dirs_cfg() {
    ensure_dir_recursive(be_config_dir());
    char p[600]{};
    snprintf(p, sizeof(p), "%s/music_dirs.cfg", be_config_dir());
    FILE* f = plat_fopen(p, "w");
    if (!f) return;
    for (const auto& d : g.scan_dirs) fprintf(f, "%s\n", d.c_str());
    fclose(f);
}

int add_scan_dir(const char* dir) {
    if (!dir || !*dir) return -1;
    std::string norm(dir);
    std::replace(norm.begin(), norm.end(), '\\', '/');
    while (norm.size() > 1 && norm.back() == '/') norm.pop_back();
    for (size_t i = 0; i < g.scan_dirs.size(); ++i) {
        if (ascii_icmp(g.scan_dirs[i], norm) == 0) return static_cast<int>(i);
    }
    if (static_cast<int>(g.scan_dirs.size()) >= MAX_DIRS) return static_cast<int>(g.scan_dirs.size()) - 1;
    g.scan_dirs.push_back(std::move(norm));
    save_dirs_cfg();
    return static_cast<int>(g.scan_dirs.size()) - 1;
}

void load_favorites() {
    char p[600]{};
    snprintf(p, sizeof(p), "%s/favorites.cfg", be_config_dir());
    FILE* f = plat_fopen(p, "r");
    if (!f) return;
    char buf[600]{};
    while (fgets(buf, sizeof(buf), f)) {
        size_t n = std::strlen(buf);
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
        if (!buf[0]) continue;
        for (auto& tr : g.tracks) {
            if (tr.path == buf) { tr.fav = 1; break; }
        }
    }
    fclose(f);
}

void save_favorites() {
    ensure_dir_recursive(be_config_dir());
    char p[600]{};
    snprintf(p, sizeof(p), "%s/favorites.cfg", be_config_dir());
    FILE* f = plat_fopen(p, "w");
    if (!f) return;
    for (const auto& tr : g.tracks) if (tr.fav) fprintf(f, "%s\n", tr.path.c_str());
    fclose(f);
}

int add_track_path(const char* path, int dir_idx) {
    if (!path || !*path || static_cast<int>(g.tracks.size()) >= MAX_TRACKS) return -1;
    for (size_t i = 0; i < g.tracks.size(); ++i) {
        if (g.tracks[i].path == path) return static_cast<int>(i);
    }
    Track tr;
    tr.path = path;
    char title_buf[192]{};
    basename_no_ext(path, title_buf, sizeof(title_buf));
    tr.title = title_buf;
    track_format_tag(path, tr.fmt, sizeof(tr.fmt));
    tr.dir_idx = dir_idx;
    g.tracks.push_back(std::move(tr));
    return static_cast<int>(g.tracks.size()) - 1;
}

void scan_dir_rec(const char* dir, int dir_idx, int depth) {
    if (depth > 5 || static_cast<int>(g.tracks.size()) >= MAX_TRACKS) return;
    PlatDir* d = plat_opendir(dir);
    if (!d) return;
    PlatDirent e{};
    char sub[640]{};
    while (plat_readdir(d, &e)) {
        if (e.d_name[0] == '.') continue;
        const size_t dl = std::strlen(dir);
        if (dl > 0 && dir[dl - 1] == '/') snprintf(sub, sizeof(sub), "%s%s", dir, e.d_name);
        else                               snprintf(sub, sizeof(sub), "%s/%s", dir, e.d_name);
        int is_d = e.is_dir, is_r = e.is_reg;
        if (!is_d && !is_r) plat_stat_type(sub, &is_d, &is_r);
        if (is_d) scan_dir_rec(sub, dir_idx, depth + 1);
        else if (is_r && dec_is_audio(e.d_name)) add_track_path(sub, dir_idx);
    }
    plat_closedir(d);
}

bool match_fmt_filter(const Track& tr) {
    if (g.fmt_filter == FMT_FILT_ALL) return true;
    if (g.fmt_filter == FMT_FILT_FAV) return tr.fav != 0;
    if (g.fmt_filter == FMT_FILT_LOSSLESS) {
        return std::strcmp(tr.fmt, "FLAC") == 0 || std::strcmp(tr.fmt, "WAV") == 0 ||
               std::strcmp(tr.fmt, "AIFF") == 0 || std::strcmp(tr.fmt, "APE") == 0 || std::strcmp(tr.fmt, "WV") == 0;
    }
    if (g.fmt_filter == FMT_FILT_MP3) return std::strcmp(tr.fmt, "MP3") == 0;
    return std::strcmp(tr.fmt, "MP3") != 0 && std::strcmp(tr.fmt, "FLAC") != 0 &&
           std::strcmp(tr.fmt, "WAV") != 0 && std::strcmp(tr.fmt, "AIFF") != 0;
}

void rebuild_visible() {
    g.vis_idx.clear();
    for (int i = 0; i < static_cast<int>(g.tracks.size()); ++i) {
        const auto& tr = g.tracks[i];
        if (g.lib_tab == TAB_FAV && !tr.fav) continue;
        if (g.lib_tab == TAB_DIR && g.active_dir >= 0 && tr.dir_idx != g.active_dir) continue;
        if (!match_fmt_filter(tr)) continue;
        if (!g.search_q.empty()) {
            if (!ascii_icase_contains(tr.title, g.search_q) &&
                !ascii_icase_contains(tr.artist, g.search_q) &&
                !ascii_icase_contains(tr.album, g.search_q) &&
                !ascii_icase_contains(tr.path, g.search_q)) continue;
        }
        g.vis_idx.push_back(i);
    }
    std::sort(g.vis_idx.begin(), g.vis_idx.end(), [](int ia, int ib) {
        const Track& ta = g.tracks[ia];
        const Track& tb = g.tracks[ib];
        if (g.sort_mode == PC_SORT_TITLE) return ascii_icmp(ta.title, tb.title) < 0;
        if (g.sort_mode == PC_SORT_ARTIST) {
            const int r = ascii_icmp(ta.artist, tb.artist);
            return r ? (r < 0) : (ascii_icmp(ta.title, tb.title) < 0);
        }
        if (g.sort_mode == PC_SORT_ALBUM) {
            const int r = ascii_icmp(ta.album, tb.album);
            return r ? (r < 0) : (ascii_icmp(ta.title, tb.title) < 0);
        }
        return ascii_icmp(ta.path, tb.path) < 0;
    });
}

void scan_library(int notify) {
    std::string prev_play;
    if (g.playing_idx >= 0 && g.playing_idx < static_cast<int>(g.tracks.size())) {
        prev_play = g.tracks[g.playing_idx].path;
    }
    g.tracks.clear();
    for (size_t i = 0; i < g.scan_dirs.size(); ++i) scan_dir_rec(g.scan_dirs[i].c_str(), static_cast<int>(i), 0);
    load_favorites();
    g.playing_idx = -1;
    if (!prev_play.empty()) {
        for (size_t i = 0; i < g.tracks.size(); ++i) {
            if (g.tracks[i].path == prev_play) { g.playing_idx = static_cast<int>(i); break; }
        }
    }
    rebuild_visible();
    g.probe_cursor = 0;
    g.scroll_px = g.scroll_target = 0.0f;
    if (notify) {
        char m[96]{};
        snprintf(m, sizeof(m), "\xE6\x89\xAB\xE6\x8F\x8F\xE5\xAE\x8C\xE6\x88\x90\xEF\xBC\x9A\xE5\x85\xB1 %d \xE9\xA6\x96\xE9\x9F\xB3\xE9\xA2\x91", static_cast<int>(g.tracks.size()));
        show_toast(m);
    }
}

void ensure_track_meta(int idx, int probe_dur) {
    if (idx < 0 || idx >= static_cast<int>(g.tracks.size())) return;
    Track& tr = g.tracks[idx];
    if (tr.meta_state == 0) {
        TrackMetaGuard tm;
        if (meta_load_quick(tr.path.c_str(), &tm.m)) {
            if (tm.m.title[0])  tr.title  = tm.m.title;
            if (tm.m.artist[0]) tr.artist = tm.m.artist;
            if (tm.m.album[0])  tr.album  = tm.m.album;
            if (tm.m.art && tm.m.art_w > 8 && tm.m.art_h > 8) {
                tr.thumb.resize(static_cast<size_t>(THUMB_SZ) * THUMB_SZ);
                resample_cover_square(reinterpret_cast<const uint32_t*>(tm.m.art), tm.m.art_w, tm.m.art_h, tr.thumb.data(), THUMB_SZ, 1);
            }
        }
        tr.meta_state = 1;
    }
    if (probe_dur && tr.meta_state < 2) {
        double d = 0.0;
        if (dec_probe_info(tr.path.c_str(), &d, nullptr, nullptr) == 0 && d > 0.0) tr.dur = d;
        tr.meta_state = 2;
    }
}

void trim_thumbs(int center_pos) {
    int cnt = 0;
    for (const auto& tr : g.tracks) if (!tr.thumb.empty()) ++cnt;
    if (cnt <= MAX_THUMBS) return;
    int far_i = -1, max_dist = -1;
    for (int p = 0; p < static_cast<int>(g.vis_idx.size()); ++p) {
        const int idx = g.vis_idx[p];
        if (idx == g.playing_idx || g.tracks[idx].thumb.empty()) continue;
        const int d = std::abs(p - center_pos);
        if (d > max_dist) { max_dist = d; far_i = idx; }
    }
    if (far_i >= 0) std::vector<uint32_t>().swap(g.tracks[far_i].thumb);
}

void build_posters() {
    if (static_cast<size_t>(poster_end - poster_start) >= 320 * 320 * 4) {
        g.poster_mini.resize(static_cast<size_t>(MINI_ART_SZ) * MINI_ART_SZ);
        g.poster_hero.resize(static_cast<size_t>(COVER_SRC_SZ) * COVER_SRC_SZ);
        g.poster_ico.resize(40 * 40);
        const auto* src = reinterpret_cast<const uint32_t*>(poster_start);
        resample_cover_square(src, 320, 320, g.poster_mini.data(), MINI_ART_SZ, 1);
        resample_cover_square(src, 320, 320, g.poster_hero.data(), COVER_SRC_SZ, 1);
        resample_cover_square(src, 320, 320, g.poster_ico.data(), 40, 1);
    }
}

void icon_heart_ex(Canvas* c, int x, int y, int sz, uint32_t col, int filled) {
    if (filled) {
        const int r1 = static_cast<int>(sz * 0.25f + 0.5f);
        fill_ellipse_aa(c, x + static_cast<int>(sz * 0.28f), y + static_cast<int>(sz * 0.34f), r1, r1, col);
        fill_ellipse_aa(c, x + static_cast<int>(sz * 0.72f), y + static_cast<int>(sz * 0.34f), r1, r1, col);
        fill_tri_aa(c, x + static_cast<int>(sz * 0.05f), y + static_cast<int>(sz * 0.42f),
                       x + static_cast<int>(sz * 0.95f), y + static_cast<int>(sz * 0.42f),
                       x + sz / 2, y + static_cast<int>(sz * 0.92f), col);
        fill_tri_aa(c, x + static_cast<int>(sz * 0.26f), y + static_cast<int>(sz * 0.32f),
                       x + static_cast<int>(sz * 0.74f), y + static_cast<int>(sz * 0.32f),
                       x + sz / 2, y + static_cast<int>(sz * 0.60f), col);
    } else {
        const float hs = static_cast<float>(sz) * 0.44f;
        const float cx = static_cast<float>(x) + static_cast<float>(sz) * 0.50f;
        const float cy = static_cast<float>(y) + static_cast<float>(sz) * 0.54f;
        for (int j = y - 1; j <= y + sz + 1; ++j) {
            const float py = -(j + 0.5f - cy) / hs;
            for (int i = x - 1; i <= x + sz + 1; ++i) {
                const float px = (i + 0.5f - cx) / hs;
                const float py_adj = py - 0.55f * std::sqrt(std::fabs(px));
                const float d = (px * px + py_adj * py_adj - 1.0f);
                if (std::fabs(d) < 0.30f) {
                    const float a = 1.0f - std::fabs(d) / 0.30f;
                    fill_rect_a(c, i, j, 1, 1, col, static_cast<uint8_t>(a * 215.0f));
                }
            }
        }
    }
}
void icon_play(Canvas* c, int cx, int cy, int r, uint32_t col) {
    fill_tri_aa(c, cx - static_cast<int>(r * 0.38f), cy - static_cast<int>(r * 0.54f),
                   cx - static_cast<int>(r * 0.38f), cy + static_cast<int>(r * 0.54f),
                   cx + static_cast<int>(r * 0.62f), cy, col);
}
void icon_pause(Canvas* c, int cx, int cy, int r, uint32_t col) {
    const int bw = std::max(3, r / 3);
    const int bh = static_cast<int>(r * 1.15f);
    fill_round_rect_aa(c, cx - bw - 2, cy - bh / 2, bw, bh, 2, col);
    fill_round_rect_aa(c, cx + 2,      cy - bh / 2, bw, bh, 2, col);
}
void icon_prev(Canvas* c, int cx, int cy, int r, uint32_t col) {
    const int bar = std::max(2, r / 4);
    fill_round_rect_aa(c, cx - static_cast<int>(r * 0.60f), cy - static_cast<int>(r * 0.50f), bar, r, 1, col);
    fill_tri_aa(c, cx + static_cast<int>(r * 0.52f), cy - static_cast<int>(r * 0.50f),
                   cx + static_cast<int>(r * 0.52f), cy + static_cast<int>(r * 0.50f),
                   cx - static_cast<int>(r * 0.28f), cy, col);
}
void icon_next(Canvas* c, int cx, int cy, int r, uint32_t col) {
    const int bar = std::max(2, r / 4);
    fill_tri_aa(c, cx - static_cast<int>(r * 0.52f), cy - static_cast<int>(r * 0.50f),
                   cx - static_cast<int>(r * 0.52f), cy + static_cast<int>(r * 0.50f),
                   cx + static_cast<int>(r * 0.28f), cy, col);
    fill_round_rect_aa(c, cx + static_cast<int>(r * 0.40f), cy - static_cast<int>(r * 0.50f), bar, r, 1, col);
}
void icon_menu(Canvas* c, int x, int y, int sz, uint32_t col) {
    for (int i = 0; i < 3; ++i) fill_round_rect_aa(c, x, y + i * (sz / 3), sz, 2, 1, col);
}
void icon_search(Canvas* c, int cx, int cy, int r, uint32_t col) {
    stroke_circle_aa(c, cx - 1, cy - 1, r, 2, col, 210);
    for (int i = 0; i < 5; ++i) fill_round_rect_aa(c, cx + r / 2 + i, cy + r / 2 + i, 2, 2, 1, col);
}
void icon_vol(Canvas* c, int cx, int cy, int sz, uint32_t col, int muted) {
    fill_round_rect_aa(c, cx - sz / 2, cy - sz / 5, sz / 4 + 1, (sz * 2) / 5 + 1, 1, col);
    fill_tri_aa(c, cx - sz / 3, cy, cx + 1, cy - sz / 2 + 1, cx + 1, cy + sz / 2 - 1, col);
    if (!muted) stroke_circle_aa(c, cx + 2, cy, sz / 3, 2, col, 185);
    else fill_round_rect_aa(c, cx + 3, cy - 1, sz / 2 - 2, 2, 1, g.C_HEART);
}
void icon_expand(Canvas* c, int x, int y, int sz, uint32_t col) {
    fill_round_rect_aa(c, x, y, sz / 2, 2, 1, col);
    fill_round_rect_aa(c, x, y, 2, sz / 2, 1, col);
    fill_round_rect_aa(c, x + sz - sz / 2, y + sz - 2, sz / 2, 2, 1, col);
    fill_round_rect_aa(c, x + sz - 2, y + sz - sz / 2, 2, sz / 2, 1, col);
}
void icon_back(Canvas* c, int x, int y, int sz, uint32_t col) {
    const int my = y + sz / 2;
    fill_round_rect_aa(c, x + 2, my - 1, sz - 3, 2, 1, col);
    fill_tri_aa(c, x, my, x + sz / 2, y + 2, x + sz / 2, y + sz - 2, col);
}
void icon_library_grid(Canvas* c, int x, int y, int sz, uint32_t col) {
    const int cell = std::max(4, (sz - 4) / 2);
    fill_round_rect_aa(c, x, y, cell, cell, 3, col);
    fill_round_rect_aa(c, x + cell + 4, y, cell, cell, 3, col);
    fill_round_rect_aa(c, x, y + cell + 4, cell, cell, 3, col);
    fill_round_rect_aa(c, x + cell + 4, y + cell + 4, cell, cell, 3, col);
}
void icon_list_rows(Canvas* c, int x, int y, int sz, uint32_t col) {
    for (int i = 0; i < 3; ++i) {
        const int ry = y + 2 + i * ((sz - 4) / 2);
        fill_round_rect_aa(c, x, ry, 3, 3, 1, col);
        fill_round_rect_aa(c, x + 6, ry, sz - 6, 3, 1, col);
    }
}
void icon_lyrics_waves(Canvas* c, int x, int y, int sz, uint32_t col) {
    constexpr int bw = 3;
    const int gap = std::max(2, (sz - 4 * bw) / 3);
    const int hs[4] = { static_cast<int>(sz * 0.48f), sz, static_cast<int>(sz * 0.72f), static_cast<int>(sz * 0.42f) };
    for (int i = 0; i < 4; ++i) {
        const int h = hs[i];
        fill_round_rect_aa(c, x + i * (bw + gap), y + (sz - h) / 2, bw, h, 2, col);
    }
}
void icon_refresh(Canvas* c, int cx, int cy, int r, uint32_t col) {
    stroke_circle_aa(c, cx, cy, r, 2, col, 225);
    fill_tri_aa(c, cx + r - 3, cy - r + 2, cx + r + 4, cy - 1, cx + r - 5, cy, col);
}
void icon_theme_orb(Canvas* c, int cx, int cy, int r, uint32_t col) {
    stroke_circle_aa(c, cx, cy, r, 2, col, 225);
    fill_ellipse_aa(c, cx - 1, cy, r - 3, r - 3, col);
}
void icon_info_dot(Canvas* c, int cx, int cy, int r, uint32_t col) {
    stroke_circle_aa(c, cx, cy, r, 2, col, 215);
    fill_round_rect_aa(c, cx - 1, cy - r / 2, 2, 3, 1, col);
    fill_round_rect_aa(c, cx - 1, cy - 1, 2, r / 2 + 2, 1, col);
}
void icon_plus(Canvas* c, int cx, int cy, int r, uint32_t col) {
    fill_round_rect_aa(c, cx - r, cy - 1, r * 2, 3, 1, col);
    fill_round_rect_aa(c, cx - 1, cy - r, 3, r * 2, 1, col);
}
void icon_wallpaper(Canvas* c, int x, int y, int sz, uint32_t col) {
    stroke_round_rect_aa(c, x, y + 1, sz, sz - 2, 4, 2, col, 225);
    fill_ellipse_aa(c, x + sz - 5, y + 5, 2, 2, col);
    fill_tri_aa(c, x + 2, y + sz - 3, x + (sz * 4) / 10, y + sz / 2, x + (sz * 7) / 10, y + sz - 3, col);
    fill_tri_aa(c, x + (sz * 5) / 10, y + sz - 3, x + (sz * 7) / 10, y + (sz * 6) / 10, x + sz - 2, y + sz - 3, col);
}
void icon_close_x(Canvas* c, int cx, int cy, int r, uint32_t col) {
    for (int i = -r; i <= r; ++i) {
        fill_rect_a(c, cx + i, cy + i, 2, 2, col, 235);
        fill_rect_a(c, cx + i, cy - i, 2, 2, col, 235);
    }
}

void toggle_fav(int idx) {
    if (idx < 0 || idx >= static_cast<int>(g.tracks.size())) return;
    g.tracks[idx].fav = !g.tracks[idx].fav;
    save_favorites();
    if (g.lib_tab == TAB_FAV || g.fmt_filter == FMT_FILT_FAV) rebuild_visible();
}

void load_track(int idx) {
    if (idx < 0 || idx >= static_cast<int>(g.tracks.size())) return;
    g.scrub_drag = 0; g.scrub_sec = 0.0;
    g.lyr_cam_valid = 0; g.lyr_manual_timer = 0.0f;
    g.lyr_scroll_vel = 0.0f;
    g.lyr_wrap_ptr = nullptr;
    g.lyr_wrapped.clear();
    g.playing_idx = idx;
    audio_play(g.tracks[idx].path.c_str());

    ensure_track_meta(idx, 1);
    g.meta.clear();
    g.lyr.reset();
    meta_load(g.tracks[idx].path.c_str(), &g.meta.m);

    if (g.meta.m.title[0]) g.p_title = g.meta.m.title;
    else {
        char tb[256]{};
        basename_no_ext(g.tracks[idx].path.c_str(), tb, sizeof(tb));
        g.p_title = tb;
    }
    g.p_artist = g.meta.m.artist[0] ? g.meta.m.artist : "";
    g.p_album  = g.meta.m.album[0]  ? g.meta.m.album  : "";
    track_format_tag(g.tracks[idx].path.c_str(), g.p_fmt, sizeof(g.p_fmt));

    g.cover_src.resize(static_cast<size_t>(COVER_SRC_SZ) * COVER_SRC_SZ);
    g.cover_mini.resize(static_cast<size_t>(MINI_ART_SZ) * MINI_ART_SZ);
    if (g.meta.m.art && g.meta.m.art_w > 8 && g.meta.m.art_h > 8) {
        resample_cover_square(reinterpret_cast<const uint32_t*>(g.meta.m.art), g.meta.m.art_w, g.meta.m.art_h, g.cover_src.data(), COVER_SRC_SZ, 1);
    } else if (!g.poster_hero.empty()) {
        std::copy(g.poster_hero.begin(), g.poster_hero.end(), g.cover_src.begin());
    } else {
        make_default_art(g.cover_src.data(), COVER_SRC_SZ, g.tracks[idx].path.c_str());
    }
    resample_cover_square(g.cover_src.data(), COVER_SRC_SZ, COVER_SRC_SZ, g.cover_mini.data(), MINI_ART_SZ, 1);
    g.cover_ready = 1;
    update_target_cover_colors();

    g.lyr_req_id = 0;
    g.lyr.reset(lyrics_load_ex(g.tracks[idx].path.c_str(), g.p_title.c_str(), g.p_artist.c_str()));
    if (!g.lyr) {
        g.lyr_req_id = lyrics_start_online(g.tracks[idx].path.c_str(), g.p_title.c_str(), g.p_artist.c_str(),
                                           g.tracks[idx].dur > 1.0 ? g.tracks[idx].dur : audio_duration());
    }
    g.bars.fill(0.0f);
}

int pick_next(int cur, int manual) {
    const int nvis = static_cast<int>(g.vis_idx.size());
    if (nvis > 0) {
        if (!manual && g.play_mode == MODE_LOOP_ONE && cur >= 0) return cur;
        if (g.play_mode == MODE_SHUFFLE && nvis > 1) {
            for (int tries = 0; tries < 8; ++tries) {
                const int idx = g.vis_idx[static_cast<int>(next_rand() % static_cast<uint32_t>(nvis))];
                if (idx != cur) return idx;
            }
        }
        for (int i = 0; i < nvis; ++i) if (g.vis_idx[i] == cur) return g.vis_idx[(i + 1) % nvis];
        return g.vis_idx[0];
    }
    if (g.tracks.size() <= 1) return 0;
    return (cur + 1) % static_cast<int>(g.tracks.size());
}

int pick_prev(int cur) {
    const int nvis = static_cast<int>(g.vis_idx.size());
    if (nvis > 0) {
        if (g.play_mode == MODE_SHUFFLE && nvis > 1) {
            const int idx = g.vis_idx[static_cast<int>(next_rand() % static_cast<uint32_t>(nvis))];
            if (idx != cur) return idx;
        }
        for (int i = 0; i < nvis; ++i) if (g.vis_idx[i] == cur) return g.vis_idx[(i - 1 + nvis) % nvis];
        return g.vis_idx[0];
    }
    if (g.tracks.size() <= 1) return 0;
    return (cur - 1 + static_cast<int>(g.tracks.size())) % static_cast<int>(g.tracks.size());
}

void open_folder_dialog() {
    char picked[600]{};
    if (!be_pick_folder(picked, sizeof(picked))) return;
    const int idx = add_scan_dir(picked);
    if (idx >= 0) { g.active_dir = idx; g.lib_tab = TAB_DIR; }
    scan_library(1);
}

void handle_dropped(const char* raw) {
    if (!raw || !*raw) return;
    std::string path(raw);
    std::replace(path.begin(), path.end(), '\\', '/');

    if (ci_ends_with(path.c_str(), ".lrc") || ci_ends_with(path.c_str(), ".txt")) {
        LyricsPtr ext(lyrics_load_file(path.c_str()));
        if (ext) {
            g.lyr = std::move(ext);
            g.lyr_req_id = 0;
            show_toast("\xE5\xB7\xB2\xE5\x8A\xA0\xE8\xBD\xBD\xE5\xA4\x96\xE9\x83\xA8 .lrc \xE6\xAD\x8C\xE8\xAF\x8D");
        } else {
            show_toast("\xE5\xA4\x96\xE9\x83\xA8 .lrc \xE8\xA7\xA3\xE6\x9E\x90\xE5\xA4\xB1\xE8\xB4\xA5");
        }
        return;
    }

    if (is_image_file(path.c_str())) {
        if (load_bg_image(path.c_str())) {
            save_settings();
            show_toast("已更换自定义背景图片");
        } else {
            show_toast("图片解码失败，请更换 PNG / JPG 图片");
        }
        return;
    }

    int is_d = 0, is_r = 0;
    if (plat_stat_type(path.c_str(), &is_d, &is_r) == 0 && is_d) {
        const int di = add_scan_dir(path.c_str());
        if (di >= 0) { g.active_dir = di; g.lib_tab = TAB_DIR; }
        scan_library(1);
        return;
    }

    if (dec_is_audio(path.c_str())) {
        const int idx = add_track_path(path.c_str(), g.active_dir >= 0 ? g.active_dir : 0);
        if (idx >= 0) {
            rebuild_visible();
            load_track(idx);
            show_toast("\xE6\xAD\xA3\xE5\x9C\xA8\xE6\x92\xAD\xE6\x94\xBE\xE6\x8B\x96\xE5\x85\xA5\xE9\x9F\xB3\xE9\xA2\x91");
        }
    } else {
        show_toast("\xE4\xB8\x8D\xE6\x94\xAF\xE6\x8C\x81\xE7\x9A\x84\xE6\x96\x87\xE4\xBB\xB6\xE6\xA0\xBC\xE5\xBC\x8F");
    }
}

void pop_utf8_cp(std::string& s) {
    while (!s.empty() && ((static_cast<unsigned char>(s.back()) & 0xC0u) == 0x80u)) {
        s.pop_back();
    }
    if (!s.empty()) s.pop_back();
}

std::vector<std::string> wrap_utf8_lines(const char* s, float px, int max_w, int max_lines, int bold = 0) {
    std::vector<std::string> out;
    if (!s || !*s || max_lines <= 0) return out;
    const char* p = s;
    while (*p && static_cast<int>(out.size()) < max_lines) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        const char* cur = p;
        const char* last_space = nullptr;
        const char* best_end = p;
        while (*cur) {
            const unsigned char c = static_cast<unsigned char>(*cur);
            const int clen = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
            const char* nxt = cur;
            for (int k = 0; k < clen && *nxt; ++k) ++nxt;
            if (*cur == '\n') { best_end = cur; break; }
            if (*cur == ' ') last_space = cur;
            const size_t seg_len = static_cast<size_t>(nxt - p);
            if (seg_len >= 255) break;
            std::string tmp(p, seg_len);
            const int w = bold ? text_width_bold(g.font.get(), px, tmp.c_str())
                               : text_width(g.font.get(), px, tmp.c_str());
            if (w > max_w && cur > p) {
                best_end = (last_space && last_space > p && clen == 1 && *cur != ' ') ? last_space : cur;
                break;
            }
            best_end = nxt;
            cur = nxt;
        }
        size_t cp = static_cast<size_t>(best_end - p);
        if (cp == 0) {
            const unsigned char c = static_cast<unsigned char>(*p);
            const int clen = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
            while (cp < static_cast<size_t>(clen) && p[cp]) ++cp;
        }
        out.emplace_back(p, std::min<size_t>(cp, 255));
        p += cp;
        if (*p == '\n' || *p == ' ') ++p;
    }
    return out;
}

void calc_grid_metrics(int& out_cols, int& out_gap, int& out_cw, int& out_ch, int& out_herow) {
    const int avail = std::max(320, g.W - RAIL_W - 26 * 2 - 22);
    const int gap = g.W >= 1440 ? 20 : 16;
    const int cols = std::clamp((avail + gap) / (218 + gap), 2, 6);
    const int cw = (avail - gap * (cols - 1)) / cols;
    const int ch = cw + 64;
    const int herow = cols >= 3 ? (cw * 2 + gap) : avail;
    out_cols = cols; out_gap = gap; out_cw = cw; out_ch = ch; out_herow = herow;
    g.grid_cols = cols; g.grid_gap = gap; g.grid_w = avail;
    g.card_w = cw; g.card_h = ch; g.hero_w = herow;
}

int list_vis_h() { return std::max(120, g.H - 114 - 16); }

float max_scroll_px() {
    constexpr int dock_clear = 98;
    const int nvis = static_cast<int>(g.vis_idx.size());
    const int vh = list_vis_h();
    if (g.view_grid) {
        int cols, gap, cw, ch, herow;
        calc_grid_metrics(cols, gap, cw, ch, herow);
        int rows = 1;
        if (cols >= 3) {
            const int rem = nvis - (cols - 2);
            if (rem > 0) rows += (rem + cols - 1) / cols;
        } else if (nvis > 0) {
            rows += (nvis + cols - 1) / cols;
        }
        const int content_h = 8 + rows * ch + (rows > 0 ? (rows - 1) * gap : 0) + dock_clear;
        return content_h > vh ? static_cast<float>(content_h - vh) : 0.0f;
    }
    const int content_h = 34 + nvis * ROW_H + dock_clear;
    return content_h > vh ? static_cast<float>(content_h - vh) : 0.0f;
}

void pump_background_meta() {
    const int nvis = static_cast<int>(g.vis_idx.size());
    if (nvis <= 0) return;
    int loaded = 0;
    int first_row = 0;
    if (g.view_grid) {
        int cols, gap, cw, ch, herow;
        calc_grid_metrics(cols, gap, cw, ch, herow);
        const int row = static_cast<int>(g.scroll_px / static_cast<float>(ch + gap));
        if (row <= 0) first_row = 0;
        else if (cols >= 3) first_row = (cols - 2) + (row - 1) * cols;
        else first_row = (row - 1) * cols;
        first_row = std::max(0, first_row);
    } else {
        first_row = std::max(0, static_cast<int>(g.scroll_px / static_cast<float>(ROW_H)));
    }
    const int win = g.view_grid ? 18 : 16;
    for (int p = first_row; p < nvis && p < first_row + win && loaded < 3; ++p) {
        const int idx = g.vis_idx[p];
        if (g.tracks[idx].meta_state == 0) { ensure_track_meta(idx, 1); ++loaded; }
    }
    for (int step = 0; step < 3 && loaded < 2; ++step) {
        if (g.probe_cursor >= nvis) g.probe_cursor = 0;
        const int idx = g.vis_idx[g.probe_cursor++];
        if (g.tracks[idx].meta_state < 2) { ensure_track_meta(idx, 1); ++loaded; }
    }
    trim_thumbs(first_row);
}

void reset_hit_rects() {
    g.rc_menu_btn = g.rc_nav_all = g.rc_nav_fav = g.rc_nav_lyr = g.rc_nav_add = g.rc_nav_diag = {};
    g.rc_nav_bg = g.rc_nav_theme = g.rc_nav_view = g.rc_nav_rescan = {};
    g.rc_search_box = g.rc_search_clear = g.rc_sort_btn = g.rc_fs_btn = g.rc_view_pill = g.rc_bg_btn = g.rc_bg_clear = {};
    g.rc_filt_chips.fill(Rect{});
    g.rc_dir_chip = {};
    g.rc_hero_card = g.rc_hero_cov = g.rc_hero_play = g.rc_hero_prog = {};
    g.rc_sb_track = g.rc_fab_import = g.rc_back_lib = g.rc_lyr_bg = g.rc_lyr_bg_clear = g.rc_lyr_theme = g.rc_lyr_fs = {};
    g.rc_island = g.rc_bar_cov = g.rc_bar_fav = g.rc_bar_prev = g.rc_bar_play = g.rc_bar_next = {};
    g.rc_bar_mode = g.rc_bar_lyr = g.rc_bar_vol_ico = g.rc_bar_vol_trk = g.rc_bar_prog = {};
    g.grid_hits.clear();
    g.lyr_hits.clear();
}

inline uint8_t glass_rim(uint8_t dark_a = 48, uint8_t light_a = 205) {
    return static_cast<uint8_t>(static_cast<float>(dark_a) + (static_cast<float>(light_a) - static_cast<float>(dark_a)) * g.light_t);
}

void draw_nav_rail(int mx, int my) {
    Canvas* s = g.screen.get();
    Font* f = g.font.get();
    const bool lt = g.light_t > 0.5f;
    fill_frosted_round_rect(s, 0, 0, RAIL_W, g.H, 0, g.C_RAIL, lt ? 172 : 160, 3, 0);
    fill_rect_a(s, RAIL_W - 1, 0, 1, g.H, g.C_DIV, col_a(g.C_DIV) + 14);

    const int top_y = 18;
    g.rc_menu_btn = Rect{ 16, top_y, 44, 44 };
    if (in_rc(mx, my, g.rc_menu_btn)) fill_round_rect_aa(s, 14, top_y - 2, 48, 48, 16, g.C_GLASS_HOV);
    if (!g.poster_ico.empty()) {
        fill_round_rect_shadow(s, 19, top_y + 3, 38, 38, 14, theme_shadow(70));
        blit_rgb_round_fit(s, 18, top_y + 2, 40, 40, g.poster_ico.data(), 40, 40, 14);
        stroke_round_rect_aa(s, 18, top_y + 2, 40, 40, 14, 1, g.C_ACCENT, 130);
    } else {
        fill_round_rect_aa(s, 18, top_y + 2, 40, 40, 14, g.C_ACCENT);
        icon_menu(s, 28, top_y + 16, 20, g.C_ON_ACCENT);
    }

    int active_slot = 0;
    if (g.page == PAGE_LYRICS) active_slot = 2;
    else if (g.lib_tab == TAB_FAV) active_slot = 1;

    constexpr int nav_y0 = 88;
    constexpr int nav_step = 68;
    const float tgt_pill_y = static_cast<float>(nav_y0 + active_slot * nav_step);
    const float kp = 1.0f - std::exp(-16.0f * static_cast<float>(g.dt));
    g.rail_pill_y += (tgt_pill_y - g.rail_pill_y) * kp;

    fill_frosted_round_rect(s, 12, static_cast<int>(g.rail_pill_y + 0.5f), RAIL_W - 24, 34, 17,
                            mix_col(g.C_SURFACE, g.C_ACCENT, 0.34f), 192, 2, glass_rim(62, 210));
    stroke_round_rect_aa(s, 12, static_cast<int>(g.rail_pill_y + 0.5f), RAIL_W - 24, 34, 17, 1, g.C_ACCENT, 130);

    const char* labels[4] = { "曲库", "喜欢", "歌词", "刷新" };
    Rect* nav_rcs[4] = { &g.rc_nav_all, &g.rc_nav_fav, &g.rc_nav_lyr, &g.rc_nav_rescan };
    for (int i = 0; i < 4; ++i) {
        const int iy = nav_y0 + i * nav_step;
        *nav_rcs[i] = Rect{ 6, iy - 6, RAIL_W - 12, 60 };
        const int is_act = (i == active_slot);
        const int is_hov = in_rc(mx, my, *nav_rcs[i]);
        if (is_hov && !is_act) {
            fill_round_rect_aa(s, 12, iy, RAIL_W - 24, 34, 17, g.C_GLASS_HOV);
        }
        const uint32_t icol = is_act ? g.C_ACCENT : (is_hov ? g.C_TXT : g.C_TXT2);
        const int cx = RAIL_W / 2;
        const int cy = iy + 17;
        if (i == 0) icon_library_grid(s, cx - 9, cy - 9, 18, icol);
        else if (i == 1) icon_heart_ex(s, cx - 9, cy - 8, 18, is_act ? g.C_HEART : icol, is_act);
        else if (i == 2) icon_lyrics_waves(s, cx - 9, cy - 9, 18, icol);
        else icon_refresh(s, cx, cy, 8, icol);
        draw_text_centered_bold(s, f, 13.0f, 4, RAIL_W - 4, iy + 37,
                                is_act ? g.C_ACCENT : g.C_TXT2, labels[i]);
    }

    const int bot_y = g.H - 20;
    g.rc_nav_diag  = Rect{ 12, bot_y - 46, RAIL_W - 24, 42 };
    g.rc_nav_theme = Rect{ 12, bot_y - 92, RAIL_W - 24, 42 };
    g.rc_nav_view  = Rect{ 12, bot_y - 138, RAIL_W - 24, 42 };
    g.rc_nav_bg    = Rect{ 12, bot_y - 184, RAIL_W - 24, 42 };

    if (in_rc(mx, my, g.rc_nav_bg)) fill_round_rect_aa(s, g.rc_nav_bg.x, g.rc_nav_bg.y, g.rc_nav_bg.w, g.rc_nav_bg.h, 14, g.C_GLASS_HOV);
    icon_wallpaper(s, RAIL_W / 2 - 8, g.rc_nav_bg.y + 13, 16, !g.bg_img_raw.empty() ? g.C_ACCENT : g.C_TXT2);

    if (in_rc(mx, my, g.rc_nav_view)) fill_round_rect_aa(s, g.rc_nav_view.x, g.rc_nav_view.y, g.rc_nav_view.w, g.rc_nav_view.h, 14, g.C_GLASS_HOV);
    if (g.view_grid) icon_list_rows(s, RAIL_W / 2 - 8, g.rc_nav_view.y + 13, 16, g.C_TXT2);
    else             icon_library_grid(s, RAIL_W / 2 - 8, g.rc_nav_view.y + 13, 16, g.C_TXT2);

    if (in_rc(mx, my, g.rc_nav_theme)) fill_round_rect_aa(s, g.rc_nav_theme.x, g.rc_nav_theme.y, g.rc_nav_theme.w, g.rc_nav_theme.h, 14, g.C_GLASS_HOV);
    icon_theme_orb(s, RAIL_W / 2, g.rc_nav_theme.y + 21, 8, mix_col(g.C_TXT2, g.C_ACCENT, g.light_t));

    if (in_rc(mx, my, g.rc_nav_diag)) fill_round_rect_aa(s, g.rc_nav_diag.x, g.rc_nav_diag.y, g.rc_nav_diag.w, g.rc_nav_diag.h, 14, g.C_GLASS_HOV);
    icon_info_dot(s, RAIL_W / 2, g.rc_nav_diag.y + 21, 8, g.show_diag ? g.C_ACCENT : g.C_TXT2);
}

void draw_library_header(int mx, int my) {
    Canvas* s = g.screen.get();
    Font* f = g.font.get();
    const int lx = RAIL_W + 26;
    const int rx = g.W - 26;
    const bool lt = g.light_t > 0.5f;

    std::string title = "音乐库";
    if (g.lib_tab == TAB_FAV) title = "我喜欢的";
    else if (g.lib_tab == TAB_DIR && g.active_dir >= 0 && g.active_dir < static_cast<int>(g.scan_dirs.size())) {
        title = basename_str(g.scan_dirs[g.active_dir]);
    }

    draw_text_max_bold(s, f, 25.5f, lx, 16, g.C_TXT, title.c_str(), 184);
    const int tw = std::min(184, text_width_bold(f, 25.5f, title.c_str()));

    char badge[48]{};
    snprintf(badge, sizeof(badge), "%d 首歌曲", static_cast<int>(g.vis_idx.size()));
    const int bw = text_width_bold(f, 13.0f, badge) + 24;
    const int bx = lx + tw + 12;
    fill_frosted_round_rect(s, bx, 21, bw, 28, 14, mix_col(g.C_SURFACE, g.C_ACCENT, 0.28f), 196, 2, glass_rim(54, 195));
    stroke_round_rect_aa(s, bx, 21, bw, 28, 14, 1, g.C_ACCENT, 105);
    draw_text_centered_bold(s, f, 13.0f, bx, bx + bw, 27, g.C_ACCENT, badge);

    constexpr int fs_sz = 40;
    g.rc_fs_btn = Rect{ rx - fs_sz, 15, fs_sz, fs_sz };
    {
        const bool hov = in_rc(mx, my, g.rc_fs_btn);
        fill_frosted_round_rect(s, g.rc_fs_btn.x, g.rc_fs_btn.y, fs_sz, fs_sz, 14,
                                g.C_SURFACE, hov ? 185 : (lt ? 156 : 132), 2, glass_rim(56, 210));
        stroke_round_rect_aa(s, g.rc_fs_btn.x, g.rc_fs_btn.y, fs_sz, fs_sz, 14, 1, g.C_DIV, col_a(g.C_DIV) + 12);
    }
    icon_expand(s, g.rc_fs_btn.x + 12, g.rc_fs_btn.y + 12, 16, g.C_TXT2);

    const bool has_bg = !g.bg_img_raw.empty();
    int right_anchor = g.rc_fs_btn.x;
    if (has_bg) {
        constexpr int cl_sz = 38;
        g.rc_bg_clear = Rect{ right_anchor - 8 - cl_sz, 16, cl_sz, cl_sz };
        const bool hov_cl = in_rc(mx, my, g.rc_bg_clear);
        fill_frosted_round_rect(s, g.rc_bg_clear.x, g.rc_bg_clear.y, cl_sz, cl_sz, 19,
                                g.C_SURFACE, hov_cl ? 190 : (lt ? 156 : 132), 2, glass_rim(56, 210));
        stroke_round_rect_aa(s, g.rc_bg_clear.x, g.rc_bg_clear.y, cl_sz, cl_sz, 19, 1, g.C_DIV, col_a(g.C_DIV) + 12);
        icon_close_x(s, g.rc_bg_clear.x + cl_sz / 2, g.rc_bg_clear.y + cl_sz / 2, 4, hov_cl ? g.C_HEART : g.C_TXT2);
        right_anchor = g.rc_bg_clear.x;
    }

    const int bg_w = text_width_bold(f, 13.5f, "背景") + 42;
    g.rc_bg_btn = Rect{ right_anchor - (has_bg ? 6 : 10) - bg_w, 16, bg_w, 38 };
    {
        const bool hov = in_rc(mx, my, g.rc_bg_btn);
        fill_frosted_round_rect(s, g.rc_bg_btn.x, g.rc_bg_btn.y, bg_w, 38, 19,
                                has_bg ? mix_col(g.C_SURFACE, g.C_ACCENT, 0.24f) : g.C_SURFACE,
                                has_bg ? 196 : (hov ? 185 : (lt ? 156 : 132)), 2, glass_rim(56, 210));
        stroke_round_rect_aa(s, g.rc_bg_btn.x, g.rc_bg_btn.y, bg_w, 38, 19, 1,
                             has_bg ? g.C_ACCENT : g.C_DIV, has_bg ? 135 : (col_a(g.C_DIV) + 12));
    }
    icon_wallpaper(s, g.rc_bg_btn.x + 12, g.rc_bg_btn.y + 11, 16, has_bg ? g.C_ACCENT : g.C_TXT2);
    draw_text_bold(s, f, 13.5f, g.rc_bg_btn.x + 33, 26, has_bg ? g.C_ACCENT : g.C_TXT2, "背景");

    const char* view_lbl = g.view_grid ? "网格视图" : "列表视图";
    const int vw = text_width_bold(f, 13.5f, view_lbl) + 28;
    g.rc_view_pill = Rect{ g.rc_bg_btn.x - 10 - vw, 16, vw, 38 };
    {
        const bool hov = in_rc(mx, my, g.rc_view_pill);
        fill_frosted_round_rect(s, g.rc_view_pill.x, g.rc_view_pill.y, vw, 38, 19,
                                g.C_SURFACE, hov ? 185 : (lt ? 156 : 132), 2, glass_rim(56, 210));
        stroke_round_rect_aa(s, g.rc_view_pill.x, g.rc_view_pill.y, vw, 38, 19, 1, g.C_DIV, col_a(g.C_DIV) + 12);
    }
    draw_text_centered_bold(s, f, 13.5f, g.rc_view_pill.x, g.rc_view_pill.x + vw, 26, g.C_TXT2, view_lbl);

    const char* sort_lbls[4] = { "排序: 默认", "排序: 歌名", "排序: 歌手", "排序: 专辑" };
    const char* slbl = sort_lbls[g.sort_mode & 3];
    const int sw = text_width_bold(f, 13.5f, slbl) + 28;
    g.rc_sort_btn = Rect{ g.rc_view_pill.x - 10 - sw, 16, sw, 38 };
    {
        const bool hov = in_rc(mx, my, g.rc_sort_btn);
        fill_frosted_round_rect(s, g.rc_sort_btn.x, g.rc_sort_btn.y, sw, 38, 19,
                                g.C_SURFACE, hov ? 185 : (lt ? 156 : 132), 2, glass_rim(56, 210));
        stroke_round_rect_aa(s, g.rc_sort_btn.x, g.rc_sort_btn.y, sw, 38, 19, 1, g.C_DIV, col_a(g.C_DIV) + 12);
    }
    draw_text_centered_bold(s, f, 13.5f, g.rc_sort_btn.x, g.rc_sort_btn.x + sw, 26, g.C_TXT2, slbl);

    const int s_left = bx + bw + 16;
    const int s_right = g.rc_sort_btn.x - 16;
    if (s_right - s_left >= 140) {
        const int sbox_w = std::min(430, s_right - s_left);
        const int sbox_x = s_left + (s_right - s_left - sbox_w) / 2;
        g.rc_search_box = Rect{ sbox_x, 14, sbox_w, 42 };
        fill_frosted_round_rect(s, g.rc_search_box.x, g.rc_search_box.y, g.rc_search_box.w, g.rc_search_box.h, 21,
                                g.C_SURFACE, g.search_focused ? 205 : (lt ? 168 : 142), 2,
                                g.search_focused ? glass_rim(100, 235) : glass_rim(58, 210));
        stroke_round_rect_aa(s, g.rc_search_box.x, g.rc_search_box.y, g.rc_search_box.w, g.rc_search_box.h, 21,
                             g.search_focused ? 2 : 1, g.search_focused ? g.C_ACCENT : g.C_DIV,
                             g.search_focused ? 215 : (col_a(g.C_DIV) + 18));
        icon_search(s, g.rc_search_box.x + 20, g.rc_search_box.y + 20, 6,
                    g.search_focused ? g.C_ACCENT : g.C_TXT3);
        const int text_max = g.rc_search_box.w - 76;
        if (!g.search_q.empty()) {
            draw_text_max_bold(s, f, 14.5f, g.rc_search_box.x + 38, g.rc_search_box.y + 12,
                               g.C_TXT, g.search_q.c_str(), text_max);
            g.rc_search_clear = Rect{ g.rc_search_box.x + g.rc_search_box.w - 38, g.rc_search_box.y + 3, 36, 36 };
            fill_round_rect_aa(s, g.rc_search_clear.x + 4, g.rc_search_clear.y + 4, 28, 28, 14,
                               in_rc(mx, my, g.rc_search_clear) ? g.C_GLASS_ACT : g.C_GLASS);
            draw_text_centered_bold(s, f, 14.5f, g.rc_search_clear.x, g.rc_search_clear.x + 36,
                                    g.rc_search_clear.y + 9, g.C_TXT2, "x");
        } else {
            draw_text_max_bold(s, f, 13.5f, g.rc_search_box.x + 38, g.rc_search_box.y + 12,
                               g.C_TXT3, "搜索歌曲、歌手、专辑...", text_max);
        }
    }

    constexpr int cy = 66;
    int cx = lx;
    const char* chips[5] = { "全部", "已喜欢", "FLAC 无损", "MP3", "其他格式" };
    for (int i = 0; i < 5; ++i) {
        const int cw = text_width_bold(f, 13.5f, chips[i]) + (g.fmt_filter == i ? 38 : 28);
        g.rc_filt_chips[i] = Rect{ cx, cy, cw, 36 };
        const bool act = (g.fmt_filter == i);
        const bool hov = in_rc(mx, my, g.rc_filt_chips[i]);
        if (act) {
            fill_frosted_round_rect(s, cx, cy, cw, 36, 18, mix_col(g.C_SURFACE, g.C_ACCENT, 0.32f), 202, 2, glass_rim(66, 215));
            stroke_round_rect_aa(s, cx, cy, cw, 36, 18, 1, g.C_ACCENT, 145);
            fill_ellipse_aa(s, cx + 14, cy + 18, 4, 4, g.C_ACCENT);
            draw_text_bold(s, f, 13.5f, cx + 24, cy + 9, g.C_ACCENT, chips[i]);
        } else {
            fill_frosted_round_rect(s, cx, cy, cw, 36, 18, g.C_SURFACE, hov ? 185 : (lt ? 156 : 130), 2, glass_rim(50, 200));
            stroke_round_rect_aa(s, cx, cy, cw, 36, 18, 1, g.C_DIV, col_a(g.C_DIV) + 12);
            draw_text_centered_bold(s, f, 13.5f, cx, cx + cw, cy + 9, hov ? g.C_TXT : g.C_TXT2, chips[i]);
        }
        cx += cw + 10;
    }

    if (!g.scan_dirs.empty()) {
        std::string dlabel = "全部目录";
        if (g.lib_tab == TAB_DIR && g.active_dir >= 0 && g.active_dir < static_cast<int>(g.scan_dirs.size())) {
            dlabel = "目录: " + basename_str(g.scan_dirs[g.active_dir]);
        }
        const int dw = text_width_bold(f, 13.5f, dlabel.c_str()) + 28;
        if (rx - dw > cx + 10) {
            g.rc_dir_chip = Rect{ rx - dw, cy, dw, 36 };
            const bool act = (g.lib_tab == TAB_DIR);
            const bool hov = in_rc(mx, my, g.rc_dir_chip);
            fill_frosted_round_rect(s, g.rc_dir_chip.x, g.rc_dir_chip.y, dw, 36, 18,
                                    act ? mix_col(g.C_SURFACE, g.C_ACCENT, 0.28f) : g.C_SURFACE,
                                    act ? 202 : (hov ? 185 : (lt ? 156 : 130)), 2, glass_rim(52, 205));
            stroke_round_rect_aa(s, g.rc_dir_chip.x, g.rc_dir_chip.y, dw, 36, 18, 1, act ? g.C_ACCENT : g.C_DIV, act ? 145 : (col_a(g.C_DIV) + 12));
            draw_text_centered_bold(s, f, 13.5f, g.rc_dir_chip.x, g.rc_dir_chip.x + dw, cy + 9,
                                    act ? g.C_ACCENT : g.C_TXT2, dlabel.c_str());
        }
    }
}

void draw_hero_card(Rect hero, int mx, int my) {
    if (hero.w < 240 || hero.h < 150) return;
    Canvas* s = g.screen.get();
    Font* f = g.font.get();
    const bool lt = g.light_t > 0.5f;
    g.rc_hero_card = hero;
    constexpr int r = 24;
    fill_round_rect_shadow(s, hero.x, hero.y + 4, hero.w, hero.h, r, theme_shadow(84));
    fill_frosted_round_rect(s, hero.x, hero.y, hero.w, hero.h, r, g.C_HERO_L, lt ? 168 : 150, 3, glass_rim(62, 215));

    const int split_x = hero.x + static_cast<int>(hero.w * 0.65f);
    const float wave_amp = clampf(static_cast<float>(hero.h) * 0.032f, 6.0f, 10.0f);
    fill_m3_wave_panel_right(s, hero.x, hero.y, hero.w, hero.h, r,
                             split_x, wave_amp, 56.0f, g.wave_phase * 0.55f,
                             g.C_HERO_R, static_cast<uint8_t>(182 + 12 * g.light_t));
    stroke_round_rect_aa(s, hero.x, hero.y, hero.w, hero.h, r, 1, g.C_ACCENT, 88);

    int show_idx = g.playing_idx;
    if (show_idx < 0 && !g.vis_idx.empty()) show_idx = g.vis_idx[0];

    const bool paused = audio_paused() || g.playing_idx < 0;
    const double dur = g.playing_idx >= 0 ? audio_duration() : (show_idx >= 0 ? g.tracks[show_idx].dur : 0.0);
    const double pos = g.playing_idx >= 0 ? (g.scrub_drag ? g.scrub_sec : audio_position()) : 0.0;
    const float frac = (dur > 0.1) ? clampf(static_cast<float>(pos / dur), 0.0f, 1.0f) : 0.0f;

    const int right_mid_x = split_x + (hero.x + hero.w - split_x) / 2;
    const int star_cy = hero.y + hero.h / 2 - 12;
    const int star_r = std::clamp(hero.h / 5, 28, 46);
    g.rc_hero_play = Rect{ right_mid_x - star_r - 8, star_cy - star_r - 8, (star_r + 8) * 2, (star_r + 8) * 2 };
    const bool hov_play = in_rc(mx, my, g.rc_hero_play);
    const int draw_r = hov_play ? star_r + 3 : star_r;
    fill_m3_scallop(s, right_mid_x, star_cy, draw_r, 10, g.scallop_rot,
                    hov_play ? mix_col(g.C_ACCENT, COL(255, 255, 255, 255), 0.16f) : g.C_ACCENT);
    if (!paused && g.playing_idx >= 0) icon_pause(s, right_mid_x, star_cy, static_cast<int>(draw_r * 0.34f), g.C_ON_ACCENT);
    else                               icon_play(s, right_mid_x + 2, star_cy, static_cast<int>(draw_r * 0.36f), g.C_ON_ACCENT);

    const char* action_lbl = (g.playing_idx >= 0 && !paused) ? "点击暂停" : (g.playing_idx >= 0 ? "继续播放" : "开始聆听");
    draw_text_centered_bold(s, f, 14.0f, split_x + 6, hero.x + hero.w - 6, star_cy + star_r + 12, g.C_ACCENT, action_lbl);

    const int lx = hero.x + 20;
    const int rx_lim = split_x - static_cast<int>(wave_amp) - 14;
    const char* pill_txt = (g.playing_idx >= 0 && !paused) ? "正在播放" : (g.playing_idx >= 0 ? "已暂停" : "精选曲目");
    const int pill_w = text_width_bold(f, 13.0f, pill_txt) + 28;
    fill_round_rect_aa_a(s, lx, hero.y + 15, pill_w, 25, 12, mix_col(g.C_SURFACE, g.C_ACCENT, 0.26f), 205);
    fill_ellipse_aa(s, lx + 12, hero.y + 27, 3, 3, g.C_ACCENT);
    draw_text_bold(s, f, 13.0f, lx + 20, hero.y + 19, g.C_ACCENT, pill_txt);

    const int cov_sz = std::clamp(static_cast<int>(hero.h * 0.36f), 72, 106);
    const int cov_y = hero.y + 46;
    g.rc_hero_cov = Rect{ lx, cov_y, cov_sz, cov_sz };
    const uint32_t* hc = nullptr;
    int hsrc_sz = COVER_SRC_SZ;
    if (g.playing_idx >= 0 && g.cover_ready && !g.cover_src.empty()) {
        hc = g.cover_src.data();
    } else if (show_idx >= 0 && !g.tracks[show_idx].thumb.empty()) {
        hc = g.tracks[show_idx].thumb.data();
        hsrc_sz = THUMB_SZ;
    } else if (!g.poster_hero.empty()) {
        hc = g.poster_hero.data();
    }

    fill_round_rect_shadow(s, lx, cov_y + 3, cov_sz, cov_sz, 16, 75);
    if (hc) blit_rgb_round_fit(s, lx, cov_y, cov_sz, cov_sz, hc, hsrc_sz, hsrc_sz, 16);
    else    fill_round_rect_aa(s, lx, cov_y, cov_sz, cov_sz, 16, g.C_GLASS_ACT);
    stroke_round_rect_aa(s, lx, cov_y, cov_sz, cov_sz, 16, 1, g.C_DIV, col_a(g.C_DIV) + 14);

    const int tx = lx + cov_sz + 16;
    const int tmax = std::max(60, rx_lim - tx);
    const char* ttl = g.playing_idx >= 0 ? g.p_title.c_str() : (show_idx >= 0 ? g.tracks[show_idx].title.c_str() : "拖入音乐或点击导入");
    const char* art = g.playing_idx >= 0 ? (!g.p_artist.empty() ? g.p_artist.c_str() : "本地音乐")
                                         : (show_idx >= 0 && !g.tracks[show_idx].artist.empty() ? g.tracks[show_idx].artist.c_str() : "Sir\xC3\xA8ne");
    const char* fmt = g.playing_idx >= 0 ? g.p_fmt : (show_idx >= 0 ? g.tracks[show_idx].fmt : "HI-RES");

    draw_text_max_bold(s, f, 20.5f, tx, cov_y + 3, g.C_TXT, ttl, tmax);
    const int fw = text_width_bold(f, 12.5f, fmt) + 18;
    fill_round_rect_aa(s, tx, cov_y + 35, fw, 22, 7, g.C_GLASS_ACT);
    draw_text_centered_bold(s, f, 12.5f, tx, tx + fw, cov_y + 38, g.C_TXT2, fmt);
    if (tmax > fw + 16) draw_text_max_bold(s, f, 14.0f, tx + fw + 8, cov_y + 37, g.C_TXT2, art, tmax - fw - 8);

    const int bot_area_y = hero.y + hero.h - 70;
    char pct[16]{}, tm[48]{}, b_pos[20]{}, b_dur[20]{};
    snprintf(pct, sizeof(pct), "%d%%", static_cast<int>(frac * 100.0f + 0.5f));
    fmt_time(pos, b_pos, sizeof(b_pos));
    fmt_time(dur, b_dur, sizeof(b_dur));
    snprintf(tm, sizeof(tm), "%s / %s", b_pos, b_dur);

    draw_text_bold(s, f, 30.0f, lx, bot_area_y - 2, g.C_TXT, pct);
    const int pw = text_width_bold(f, 30.0f, pct);
    draw_text_bold(s, f, 13.5f, lx + pw + 14, bot_area_y + 11, g.C_TXT2, tm);

    const int hprog_y = hero.y + hero.h - 22;
    g.ui_hpx0 = lx + 4;
    g.ui_hpx1 = rx_lim - 4;
    if (g.ui_hpx1 > g.ui_hpx0 + 40) {
        g.rc_hero_prog = Rect{ g.ui_hpx0 - 10, hprog_y - 16, (g.ui_hpx1 - g.ui_hpx0) + 20, 32 };
        draw_m3_water_progress(s, g.ui_hpx0, g.ui_hpx1, hprog_y, frac,
                               g.wave_phase, 9, 1, g.C_ACCENT, g.C_TRACK_BG);
    }
}

void draw_grid_view(int mx, int my) {
    Canvas* s = g.screen.get();
    Font* f = g.font.get();
    constexpr int lx = RAIL_W + 26;
    constexpr int top_y = 114;
    const int vh = list_vis_h();
    int cols, gap, cw, ch, herow;
    calc_grid_metrics(cols, gap, cw, ch, herow);

    g.ui_list_x0 = lx;
    g.ui_list_x1 = lx + g.grid_w;
    g.ui_list_y0 = top_y;
    g.ui_list_y1 = top_y + vh;

    canvas_set_clip(s, lx - 8, top_y, lx + g.grid_w + 10, top_y + vh);
    const int start_y = top_y + 8 - static_cast<int>(g.scroll_px);

    if (start_y + ch > top_y - 16 && start_y < top_y + vh + 16) {
        draw_hero_card(Rect{ lx, start_y, herow, ch }, mx, my);
    }

    int next_hov = -1;
    const int row0_rem = cols >= 3 ? (cols - 2) : 0;
    const int nvis = static_cast<int>(g.vis_idx.size());

    for (int p = 0; p < nvis; ++p) {
        int row, col;
        if (cols >= 3 && p < row0_rem) {
            row = 0;
            col = 2 + p;
        } else {
            const int rem_idx = p - row0_rem;
            row = 1 + rem_idx / cols;
            col = rem_idx % cols;
        }
        const int x = lx + col * (cw + gap);
        const int y = start_y + row * (ch + gap);
        if (y + ch < top_y - 16 || y > top_y + vh + 16) continue;

        const int idx = g.vis_idx[p];
        const Track& tr = g.tracks[idx];
        const Rect card_rc{ x, y, cw, ch };
        const bool in_view = in_rect(mx, my, lx - 4, top_y, g.grid_w + 8, vh);
        const bool is_hov = in_view && in_rc(mx, my, card_rc);
        if (is_hov) next_hov = idx;

        const int lift = (is_hov && g.hov_card_idx == idx) ? static_cast<int>(g.hov_card_t * 5.0f + 0.5f) : 0;
        const int cy = y - lift;
        const bool is_play = (idx == g.playing_idx);

        fill_round_rect_shadow(s, x, cy + 4, cw, ch, 18, theme_shadow(is_play ? 110 : 64));
        const uint32_t card_tint = is_play ? mix_col(g.C_SURFACE, g.C_ACCENT, 0.16f)
                                           : (is_hov ? mix_col(g.C_SURFACE, 0xFFFFFFFFu, 0.05f * (1.0f - g.light_t)) : g.C_SURFACE);
        const uint8_t card_alpha = is_play ? static_cast<uint8_t>(180 + 18 * g.light_t)
                                           : (is_hov ? static_cast<uint8_t>(168 + 20 * g.light_t)
                                                     : static_cast<uint8_t>(142 + 34 * g.light_t));
        fill_frosted_round_rect(s, x, cy, cw, ch, 18, card_tint, card_alpha, 2,
                                is_play ? glass_rim(96, 230) : glass_rim(44, 195));

        constexpr int cov_pad = 6;
        const int cov_sz = cw - cov_pad * 2;
        const int cx0 = x + cov_pad;
        const int cy0 = cy + cov_pad;
        const uint32_t* art = !tr.thumb.empty() ? tr.thumb.data() : (!g.poster_hero.empty() ? g.poster_hero.data() : nullptr);
        const int art_sz = !tr.thumb.empty() ? THUMB_SZ : COVER_SRC_SZ;
        if (art) blit_rgb_round_fit(s, cx0, cy0, cov_sz, cov_sz, art, art_sz, art_sz, 14);
        else     fill_round_rect_aa(s, cx0, cy0, cov_sz, cov_sz, 14, g.C_GLASS_ACT);

        const int fpw = text_width_bold(f, 12.0f, tr.fmt) + 16;
        fill_round_rect_aa_a(s, cx0 + 8, cy0 + 8, fpw, 22, 8, COL(12, 16, 22, 255), 186);
        draw_text_centered_bold(s, f, 12.0f, cx0 + 8, cx0 + 8 + fpw, cy0 + 11,
                                is_play ? g.C_ACCENT : COL(244, 248, 255, 255), tr.fmt);

        const Rect fav_draw_rc{ cx0 + cov_sz - 34, cy0 + 6, 28, 28 };
        const Rect fav_rc{ cx0 + cov_sz - 40, cy0, 38, 38 };
        if (tr.fav || is_hov) {
            fill_round_rect_aa_a(s, fav_draw_rc.x, fav_draw_rc.y, 28, 28, 14, COL(12, 16, 22, 255), 165);
            icon_heart_ex(s, fav_draw_rc.x + 6, fav_draw_rc.y + 6, 16,
                          tr.fav ? g.C_HEART : COL(242, 246, 252, 220), tr.fav);
        }

        if (is_play || is_hov) {
            const int bx = cx0 + cov_sz - 40;
            const int by = cy0 + cov_sz - 40;
            fill_round_rect_shadow(s, bx, by + 2, 34, 34, 17, 90);
            fill_round_rect_aa(s, bx, by, 34, 34, 17, g.C_ACCENT);
            if (is_play && !audio_paused()) {
                for (int b = 0; b < 3; ++b) {
                    const float lvl = clampf(g.bars[b * 2] * 0.8f + 0.25f + 0.20f * std::sin(g.wave_phase * 2.1f + static_cast<float>(b) * 1.3f), 0.15f, 1.0f);
                    const int bh = 5 + static_cast<int>(lvl * 13.0f);
                    fill_round_rect_aa(s, bx + 10 + b * 6, by + 17 - bh / 2, 3, bh, 1, g.C_ON_ACCENT);
                }
            } else {
                icon_play(s, bx + 18, by + 17, 11, g.C_ON_ACCENT);
            }
        }

        if (is_play) stroke_round_rect_aa(s, x, cy, cw, ch, 18, 2, g.C_ACCENT, 210);
        else         stroke_round_rect_aa(s, x, cy, cw, ch, 18, 1, g.C_DIV, is_hov ? 68 : col_a(g.C_DIV));

        const int info_y = cy0 + cov_sz + 7;
        draw_text_max_bold(s, f, 15.5f, x + 10, info_y, is_play ? g.C_ACCENT : g.C_TXT, tr.title.c_str(), cw - 20);
        char dur_s[16] = "--:--";
        if (tr.dur > 0.5) fmt_time(tr.dur, dur_s, sizeof(dur_s));
        const int dw = text_width_bold(f, 12.5f, dur_s);
        draw_text_right_bold(s, f, 12.5f, x + cw - 10, info_y + 26, is_play ? g.C_ACCENT : g.C_TXT3, dur_s);
        const char* sub = !tr.artist.empty() ? tr.artist.c_str() : (!tr.album.empty() ? tr.album.c_str() : tr.fmt);
        draw_text_max_bold(s, f, 13.0f, x + 10, info_y + 25, g.C_TXT2, sub, cw - 26 - dw);

        if (static_cast<int>(g.grid_hits.size()) < MAX_GRID_HITS) {
            Rect hit_card = card_rc;
            if (hit_card.y < top_y) { hit_card.h -= (top_y - hit_card.y); hit_card.y = top_y; }
            if (hit_card.y + hit_card.h > top_y + vh) hit_card.h = top_y + vh - hit_card.y;
            if (hit_card.h > 0) g.grid_hits.push_back(GridHit{ hit_card, fav_rc, idx });
        }
    }

    if (next_hov != g.hov_card_idx) { g.hov_card_idx = next_hov; g.hov_card_t = 0.0f; }
    canvas_reset_clip(s);

    const float mscr = max_scroll_px();
    if (mscr > 1.0f) {
        const int sbx = g.W - 16;
        g.rc_sb_track = Rect{ g.W - 28, top_y + 6, 26, vh - 100 };
        const int th = g.rc_sb_track.h;
        const int thumb_h = std::min(th, std::max(38, static_cast<int>(static_cast<float>(vh) / static_cast<float>(vh + mscr) * static_cast<float>(th))));
        const int ty = g.rc_sb_track.y + static_cast<int>((g.scroll_px / mscr) * static_cast<float>(th - thumb_h));
        fill_round_rect_aa(s, sbx, g.rc_sb_track.y, 7, th, 3, g.C_TRACK_BG);
        fill_round_rect_aa(s, sbx, ty, 7, thumb_h, 3, g.sb_drag ? g.C_ACCENT : g.C_TXT3);
    }
}

void draw_table_view(int mx, int my) {
    Canvas* s = g.screen.get();
    Font* f = g.font.get();
    constexpr int lx = RAIL_W + 26;
    const int rx = g.W - 26;
    constexpr int top_y = 114;
    const int vh = list_vis_h();
    const int tw = rx - lx - 14;
    g.ui_list_x0 = lx; g.ui_list_x1 = lx + tw; g.ui_list_y0 = top_y; g.ui_list_y1 = top_y + vh;

    fill_frosted_round_rect(s, lx, top_y, tw, 30, 10, g.C_SURFACE,
                            static_cast<uint8_t>(118 + 38 * g.light_t), 2, glass_rim(42, 180));
    draw_text_bold(s, f, 13.0f, lx + 56, top_y + 7, g.C_TXT3, "歌曲");
    draw_text_bold(s, f, 13.0f, lx + static_cast<int>(tw * 0.44f), top_y + 7, g.C_TXT3, "歌手");
    draw_text_bold(s, f, 13.0f, lx + static_cast<int>(tw * 0.68f), top_y + 7, g.C_TXT3, "格式");
    draw_text_right_bold(s, f, 13.0f, lx + tw - 24, top_y + 7, g.C_TXT3, "时长");

    constexpr int body_y = top_y + 34;
    const int body_h = vh - 34;
    canvas_set_clip(s, lx, body_y, lx + tw, body_y + body_h);

    const int nvis = static_cast<int>(g.vis_idx.size());
    for (int p = 0; p < nvis; ++p) {
        const int y = body_y + p * ROW_H - static_cast<int>(g.scroll_px);
        if (y + ROW_H < body_y || y > body_y + body_h) continue;
        const int idx = g.vis_idx[p];
        const Track& tr = g.tracks[idx];
        const Rect row_rc{ lx, y + 2, tw, ROW_H - 4 };
        const bool hov = in_rect(mx, my, lx, body_y, tw, body_h) && in_rc(mx, my, row_rc);
        const bool is_play = (idx == g.playing_idx);
        if (is_play)
            fill_frosted_round_rect(s, row_rc.x, row_rc.y, row_rc.w, row_rc.h, 14,
                                    mix_col(g.C_SURFACE, g.C_ACCENT, 0.22f),
                                    static_cast<uint8_t>(172 + 24 * g.light_t), 2, glass_rim(86, 225));
        else if (hov)
            fill_frosted_round_rect(s, row_rc.x, row_rc.y, row_rc.w, row_rc.h, 14,
                                    g.C_SURFACE, static_cast<uint8_t>(128 + 38 * g.light_t), 1, glass_rim(44, 190));

        const uint32_t* art = !tr.thumb.empty() ? tr.thumb.data() : (!g.poster_mini.empty() ? g.poster_mini.data() : nullptr);
        const int asz = !tr.thumb.empty() ? THUMB_SZ : MINI_ART_SZ;
        if (art) blit_rgb_round_fit(s, lx + 10, y + 7, 40, 40, art, asz, asz, 10);
        else     fill_round_rect_aa(s, lx + 10, y + 7, 40, 40, 10, g.C_GLASS_ACT);

        const Rect fav_rc{ lx + 50, y + 7, 36, 40 };
        icon_heart_ex(s, lx + 60, y + 18, 15, tr.fav ? g.C_HEART : g.C_TXT3, tr.fav);

        draw_text_max_bold(s, f, 15.5f, lx + 90, y + 8, is_play ? g.C_ACCENT : g.C_TXT,
                           tr.title.c_str(), static_cast<int>(tw * 0.44f) - 100);
        draw_text_max_bold(s, f, 12.5f, lx + 90, y + 30, g.C_TXT3, tr.album.c_str(), static_cast<int>(tw * 0.44f) - 100);
        draw_text_max_bold(s, f, 14.0f, lx + static_cast<int>(tw * 0.44f), y + 17, g.C_TXT2,
                           !tr.artist.empty() ? tr.artist.c_str() : "未知", static_cast<int>(tw * 0.22f));
        draw_text_bold(s, f, 13.0f, lx + static_cast<int>(tw * 0.68f), y + 18, is_play ? g.C_ACCENT : g.C_TXT3, tr.fmt);
        char ds[16] = "--:--";
        if (tr.dur > 0.5) fmt_time(tr.dur, ds, sizeof(ds));
        draw_text_right_bold(s, f, 13.5f, lx + tw - 24, y + 18, is_play ? g.C_ACCENT : g.C_TXT2, ds);

        if (static_cast<int>(g.grid_hits.size()) < MAX_GRID_HITS) {
            Rect hr = row_rc;
            if (hr.y < body_y) { hr.h -= (body_y - hr.y); hr.y = body_y; }
            if (hr.y + hr.h > body_y + body_h) hr.h = body_y + body_h - hr.y;
            if (hr.h > 0) g.grid_hits.push_back(GridHit{ hr, fav_rc, idx });
        }
    }
    canvas_reset_clip(s);

    const float mscr = max_scroll_px();
    if (mscr > 1.0f) {
        const int sbx = g.W - 16;
        g.rc_sb_track = Rect{ g.W - 28, body_y, 26, body_h - 90 };
        const int th = g.rc_sb_track.h;
        const int thumb_h = std::max(38, static_cast<int>(static_cast<float>(body_h) / static_cast<float>(body_h + mscr) * static_cast<float>(th)));
        const int ty = g.rc_sb_track.y + static_cast<int>((g.scroll_px / mscr) * static_cast<float>(th - thumb_h));
        fill_round_rect_aa(s, sbx, g.rc_sb_track.y, 7, th, 3, g.C_TRACK_BG);
        fill_round_rect_aa(s, sbx, ty, 7, thumb_h, 3, g.sb_drag ? g.C_ACCENT : g.C_TXT3);
    }
}

void draw_dynamic_island(int mx, int my) {
    Canvas* s = g.screen.get();
    Font* f = g.font.get();
    constexpr int fab_w = 108, fab_h = 46;
    g.rc_fab_import = Rect{ g.W - 26 - fab_w, g.H - 22 - fab_h, fab_w, fab_h };
    const bool hov_fab = in_rc(mx, my, g.rc_fab_import);
    fill_round_rect_shadow(s, g.rc_fab_import.x, g.rc_fab_import.y + 3, fab_w, fab_h, 16, theme_shadow(110));
    fill_round_rect_aa(s, g.rc_fab_import.x, g.rc_fab_import.y, fab_w, fab_h, 16,
                       hov_fab ? mix_col(g.C_ACCENT, COL(255, 255, 255, 255), 0.14f) : g.C_ACCENT);
    icon_plus(s, g.rc_fab_import.x + 26, g.rc_fab_import.y + 23, 7, g.C_ON_ACCENT);
    draw_text_bold(s, f, 15.0f, g.rc_fab_import.x + 42, g.rc_fab_import.y + 13, g.C_ON_ACCENT, "导入");

    const int center_x = (RAIL_W + g.W) / 2;
    const int max_half = std::min(g.rc_fab_import.x - 16 - center_x, center_x - RAIL_W - 14);
    const int max_w = std::max(240, max_half * 2);
    int base_w = std::min(820, max_w);
    if (base_w < 460 && max_w >= 460) base_w = 460;

    const bool raw_hov = in_rect(mx, my, center_x - base_w / 2 - 20, g.H - 96, base_w + 40, 88) || g.scrub_drag == 1 || g.vol_drag;
    g.island_exp += ((raw_hov ? 1.0f : 0.0f) - g.island_exp) * (1.0f - std::exp(-12.0f * static_cast<float>(g.dt)));

    const int iw = std::min(max_w, base_w + static_cast<int>(26.0f * g.island_exp));
    const int ih = 66 + static_cast<int>(6.0f * g.island_exp);
    const int ix = center_x - iw / 2;
    const int iy = g.H - 18 - ih;
    const int ir = ih / 2;
    g.rc_island = Rect{ ix, iy, iw, ih };

    fill_round_rect_shadow(s, ix, iy + 5, iw, ih, ir, theme_shadow(115));
    const uint32_t isl_bg = mix_col(mix_col(COL(16, 20, 28, 255), COL(246, 249, 255, 255), g.light_t), g.C_HERO_L, 0.24f);
    fill_frosted_round_rect(s, ix, iy, iw, ih, ir, isl_bg, static_cast<uint8_t>(168 + 28 * g.light_t), 4, glass_rim(92, 240));
    if (g.light_t < 0.7f) {
        stroke_round_rect_aa(s, ix, iy, iw, ih, ir, 1, g.C_ACCENT, static_cast<uint8_t>(38.0f * (1.0f - g.light_t)));
    }

    const int cov_sz = ih - 18;
    g.rc_bar_cov = Rect{ ix + 14, iy + (ih - cov_sz) / 2 - 2, cov_sz, cov_sz };
    const uint32_t* mc = (g.playing_idx >= 0 && g.cover_ready && !g.cover_mini.empty()) ? g.cover_mini.data()
                         : (!g.poster_mini.empty() ? g.poster_mini.data() : nullptr);
    if (mc) blit_rgb_round_fit(s, g.rc_bar_cov.x, g.rc_bar_cov.y, cov_sz, cov_sz, mc, MINI_ART_SZ, MINI_ART_SZ, cov_sz / 2);
    else    fill_ellipse_aa(s, g.rc_bar_cov.x + cov_sz / 2, g.rc_bar_cov.y + cov_sz / 2, cov_sz / 2, cov_sz / 2, g.C_ACCENT);

    const int mid_x = ix + iw / 2;
    const int ctrl_y = iy + ih / 2 - 3;
    g.rc_bar_play = Rect{ mid_x - 22, ctrl_y - 22, 44, 44 };
    g.rc_bar_prev = Rect{ mid_x - 62, ctrl_y - 19, 38, 38 };
    g.rc_bar_next = Rect{ mid_x + 24, ctrl_y - 19, 38, 38 };
    g.rc_bar_fav  = Rect{ mid_x - 102, ctrl_y - 19, 36, 38 };

    const int is_fav = (g.playing_idx >= 0 && g.tracks[g.playing_idx].fav);
    if (in_rc(mx, my, g.rc_bar_fav)) fill_round_rect_aa(s, mid_x - 98, ctrl_y - 15, 30, 30, 15, g.C_GLASS_HOV);
    icon_heart_ex(s, mid_x - 91, ctrl_y - 8, 16, is_fav ? g.C_HEART : g.C_TXT2, is_fav);

    if (in_rc(mx, my, g.rc_bar_prev)) fill_round_rect_aa(s, mid_x - 59, ctrl_y - 16, 32, 32, 16, g.C_GLASS_HOV);
    icon_prev(s, mid_x - 43, ctrl_y, 11, g.C_TXT);

    const bool hov_p = in_rc(mx, my, g.rc_bar_play);
    const bool playing_now = (g.playing_idx >= 0 && !audio_paused());
    fill_ellipse_aa(s, mid_x, ctrl_y, hov_p ? 20 : 19, hov_p ? 20 : 19, playing_now ? g.C_ACCENT : g.C_PLAY_BTN);
    if (playing_now) icon_pause(s, mid_x, ctrl_y, 10, g.C_ON_ACCENT);
    else             icon_play(s, mid_x + 1, ctrl_y, 11, g.C_PLAY_ICO);

    if (in_rc(mx, my, g.rc_bar_next)) fill_round_rect_aa(s, mid_x + 27, ctrl_y - 16, 32, 32, 16, g.C_GLASS_HOV);
    icon_next(s, mid_x + 43, ctrl_y, 11, g.C_TXT);

    const int info_x = g.rc_bar_cov.x + cov_sz + 12;
    const int info_max = g.rc_bar_fav.x - info_x - 8;
    if (info_max > 40) {
        draw_text_max_bold(s, f, 15.5f, info_x, iy + 9, g.C_TXT, g.p_title.c_str(), info_max);
        const int eq_y = iy + 40;
        for (int b = 0; b < 5; ++b) {
            const float lvl = playing_now
                              ? clampf(g.bars[b * 2] * 0.75f + 0.25f + 0.22f * std::sin(g.wave_phase * 2.4f + static_cast<float>(b) * 1.1f), 0.15f, 1.0f)
                              : 0.18f;
            const int bh = 3 + static_cast<int>(lvl * 10.0f);
            fill_round_rect_aa(s, info_x + b * 5, eq_y - bh / 2, 3, bh, 1, g.C_ACCENT);
        }
        draw_text_max_bold(s, f, 12.5f, info_x + 32, iy + 32, g.C_TXT2,
                           !g.p_artist.empty() ? g.p_artist.c_str() : "点击封面进入歌词", info_max - 32);
    }

    const int rx = ix + iw - 16;
    g.rc_bar_lyr = Rect{ rx - 38, ctrl_y - 17, 40, 34 };
    const bool act_lyr = (g.lyr && g.lyr->n > 0);
    if (act_lyr) {
        fill_frosted_round_rect(s, rx - 36, ctrl_y - 14, 36, 28, 14,
                                mix_col(g.C_SURFACE, g.C_ACCENT, 0.28f), 192, 1, glass_rim(92, 225));
    } else {
        fill_frosted_round_rect(s, rx - 36, ctrl_y - 14, 36, 28, 14,
                                g.C_SURFACE, in_rc(mx, my, g.rc_bar_lyr) ? 168 : 122, 1, glass_rim(44, 180));
    }
    draw_text_centered_bold(s, f, 13.0f, rx - 36, rx, ctrl_y - 9, act_lyr ? g.C_ACCENT : g.C_TXT2, "词");

    const char* mode_lbl = g.play_mode == MODE_LOOP_ONE ? "单曲" : (g.play_mode == MODE_SHUFFLE ? "随机" : "顺序");
    g.rc_bar_mode = Rect{ rx - 86, ctrl_y - 17, 46, 34 };
    fill_frosted_round_rect(s, rx - 85, ctrl_y - 14, 44, 28, 14,
                            g.C_SURFACE, in_rc(mx, my, g.rc_bar_mode) ? 168 : 122, 1, glass_rim(44, 180));
    draw_text_centered_bold(s, f, 12.5f, rx - 85, rx - 41, ctrl_y - 8, g.C_TXT2, mode_lbl);

    const int vol_w = (g.rc_bar_mode.x - g.rc_bar_next.x - g.rc_bar_next.w > 130) ? 68 : 54;
    if (g.rc_bar_mode.x - g.rc_bar_next.x - g.rc_bar_next.w > 106) {
        g.ui_vx1 = g.rc_bar_mode.x - 10;
        g.ui_vx0 = g.ui_vx1 - vol_w;
        g.rc_bar_vol_trk = Rect{ g.ui_vx0 - 8, ctrl_y - 17, vol_w + 16, 34 };
        g.rc_bar_vol_ico = Rect{ g.ui_vx0 - 34, ctrl_y - 17, 26, 34 };
        const float v = audio_volume();
        icon_vol(s, g.ui_vx0 - 20, ctrl_y, 14, g.C_TXT2, v <= 0.01f);
        fill_round_rect_aa(s, g.ui_vx0, ctrl_y - 3, vol_w, 6, 3, g.C_TRACK_BG);
        const int vw = static_cast<int>(v * static_cast<float>(vol_w));
        if (vw > 0) fill_round_rect_aa(s, g.ui_vx0, ctrl_y - 3, vw, 6, 3, g.C_ACCENT);
        fill_ellipse_aa(s, g.ui_vx0 + vw, ctrl_y, 6, 6, g.C_ACCENT);
    }

    const double dur = g.playing_idx >= 0 ? audio_duration() : 0.0;
    const double pos = g.playing_idx >= 0 ? (g.scrub_drag == 1 ? g.scrub_sec : audio_position()) : 0.0;
    const float frac = (dur > 0.1) ? clampf(static_cast<float>(pos / dur), 0.0f, 1.0f) : 0.0f;
    g.ui_px0 = ix + 34;
    g.ui_px1 = ix + iw - 34;
    g.ui_py = iy + ih - 8;
    g.rc_bar_prog = Rect{ g.ui_px0 - 8, g.ui_py - 16, (g.ui_px1 - g.ui_px0) + 16, 28 };
    draw_m3_water_progress(s, g.ui_px0, g.ui_px1, g.ui_py, frac, g.wave_phase, 6,
                           (g.island_exp > 0.15f || g.scrub_drag == 1) ? 1 : 0, g.C_ACCENT, g.C_TRACK_BG);
}

void draw_immersive_view(int mx, int my) {
    draw_frosted_ambient_bg(1);
    Canvas* s = g.screen.get();
    Font* f = g.font.get();

    g.rc_back_lib = Rect{ 26, 18, 132, 42 };
    const bool hov_back = in_rc(mx, my, g.rc_back_lib);
    fill_frosted_round_rect(s, g.rc_back_lib.x, g.rc_back_lib.y, g.rc_back_lib.w, g.rc_back_lib.h, 21,
                            g.C_SURFACE, hov_back ? 186 : static_cast<uint8_t>(138 + 34 * g.light_t), 3,
                            glass_rim(hov_back ? 96 : 64, 235));
    icon_back(s, g.rc_back_lib.x + 15, g.rc_back_lib.y + 14, 14, g.C_TXT);
    draw_text_bold(s, f, 14.0f, g.rc_back_lib.x + 38, g.rc_back_lib.y + 11, g.C_TXT, "返回曲库");

    g.rc_lyr_fs = Rect{ g.W - 68, 18, 42, 42 };
    const bool hov_fs = in_rc(mx, my, g.rc_lyr_fs);
    fill_frosted_round_rect(s, g.rc_lyr_fs.x, g.rc_lyr_fs.y, 42, 42, 21,
                            g.C_SURFACE, hov_fs ? 186 : static_cast<uint8_t>(138 + 34 * g.light_t), 3,
                            glass_rim(hov_fs ? 96 : 64, 235));
    icon_expand(s, g.rc_lyr_fs.x + 13, g.rc_lyr_fs.y + 13, 16, g.C_TXT);

    g.rc_lyr_theme = Rect{ g.W - 118, 18, 42, 42 };
    const bool hov_thm = in_rc(mx, my, g.rc_lyr_theme);
    fill_frosted_round_rect(s, g.rc_lyr_theme.x, g.rc_lyr_theme.y, 42, 42, 21,
                            g.C_SURFACE, hov_thm ? 186 : static_cast<uint8_t>(138 + 34 * g.light_t), 3,
                            glass_rim(hov_thm ? 96 : 64, 235));
    icon_theme_orb(s, g.rc_lyr_theme.x + 21, g.rc_lyr_theme.y + 21, 8, mix_col(g.C_TXT, g.C_ACCENT, g.light_t));

    g.rc_lyr_bg = Rect{ g.W - 168, 18, 42, 42 };
    const bool has_bg = !g.bg_img_raw.empty();
    const bool hov_lbg = in_rc(mx, my, g.rc_lyr_bg);
    fill_frosted_round_rect(s, g.rc_lyr_bg.x, g.rc_lyr_bg.y, 42, 42, 21,
                            has_bg ? mix_col(g.C_SURFACE, g.C_ACCENT, 0.24f) : g.C_SURFACE,
                            has_bg ? 194 : (hov_lbg ? 186 : static_cast<uint8_t>(138 + 34 * g.light_t)), 3,
                            glass_rim((has_bg || hov_lbg) ? 96 : 64, 235));
    icon_wallpaper(s, g.rc_lyr_bg.x + 13, g.rc_lyr_bg.y + 13, 16, has_bg ? g.C_ACCENT : g.C_TXT);

    if (has_bg) {
        g.rc_lyr_bg_clear = Rect{ g.W - 218, 18, 42, 42 };
        const bool hov_lcl = in_rc(mx, my, g.rc_lyr_bg_clear);
        fill_frosted_round_rect(s, g.rc_lyr_bg_clear.x, g.rc_lyr_bg_clear.y, 42, 42, 21,
                                g.C_SURFACE, hov_lcl ? 190 : static_cast<uint8_t>(138 + 34 * g.light_t), 3,
                                glass_rim(hov_lcl ? 96 : 64, 235));
        icon_close_x(s, g.rc_lyr_bg_clear.x + 21, g.rc_lyr_bg_clear.y + 21, 4, hov_lcl ? g.C_HEART : g.C_TXT2);
    }

    const int left_col_w = std::clamp(static_cast<int>(g.W * 0.40f), 340, 540);
    const int cov_sz = std::clamp(std::min(static_cast<int>(g.H * 0.44f), left_col_w - 72), 200, 430);
    const int cov_x = 36 + (left_col_w - cov_sz) / 2;
    const int cov_y = std::max(72, static_cast<int>(g.H * 0.11f));

    fill_round_rect_shadow(s, cov_x, cov_y + 12, cov_sz, cov_sz, 24, 138);
    const uint32_t* art = (g.playing_idx >= 0 && g.cover_ready && !g.cover_src.empty()) ? g.cover_src.data()
                          : (!g.poster_hero.empty() ? g.poster_hero.data() : nullptr);
    if (art) blit_rgb_round_fit(s, cov_x, cov_y, cov_sz, cov_sz, art, COVER_SRC_SZ, COVER_SRC_SZ, 24);
    else     fill_round_rect_aa(s, cov_x, cov_y, cov_sz, cov_sz, 24, g.C_GLASS_ACT);
    stroke_round_rect_aa(s, cov_x, cov_y, cov_sz, cov_sz, 24, 1, g.C_DIV, col_a(g.C_DIV) + 12);

    const int info_x = cov_x;
    const int info_w = cov_sz;
    const int ty = cov_y + cov_sz + 22;
    draw_text_max_bold(s, f, 26.5f, info_x, ty, g.C_TXT, g.p_title.c_str(), info_w);
    draw_text_max_bold(s, f, 16.5f, info_x, ty + 35, g.C_TXT2,
                       !g.p_artist.empty() ? g.p_artist.c_str() : (!g.p_album.empty() ? g.p_album.c_str() : "Sir\xC3\xA8ne Hi-Res"), info_w);

    char spec[96]{};
    snprintf(spec, sizeof(spec), "%s  %uHz  %uch", g.p_fmt, audio_sample_rate(), audio_channels());
    const int spw = text_width_bold(f, 12.5f, spec) + 22;
    fill_frosted_round_rect(s, info_x, ty + 64, spw, 25, 12,
                            g.C_SURFACE, static_cast<uint8_t>(118 + 38 * g.light_t), 2, glass_rim(44, 185));
    draw_text_centered_bold(s, f, 12.5f, info_x, info_x + spw, ty + 68, g.C_ACCENT, spec);

    const double dur = g.playing_idx >= 0 ? audio_duration() : 0.0;
    const double pos = g.playing_idx >= 0 ? (g.scrub_drag == 1 ? g.scrub_sec : audio_position()) : 0.0;
    const float frac = (dur > 0.1) ? clampf(static_cast<float>(pos / dur), 0.0f, 1.0f) : 0.0f;
    const int py = ty + 108;
    g.ui_px0 = info_x;
    g.ui_px1 = info_x + info_w;
    g.ui_py = py;
    g.rc_bar_prog = Rect{ g.ui_px0 - 10, py - 16, info_w + 20, 32 };
    draw_m3_water_progress(s, g.ui_px0, g.ui_px1, py, frac, g.wave_phase, 10, 1, g.C_ACCENT, g.C_TRACK_BG);

    char bpos[20]{}, bdur[20]{};
    fmt_time(pos, bpos, sizeof(bpos));
    fmt_time(dur, bdur, sizeof(bdur));
    draw_text_bold(s, f, 13.5f, g.ui_px0, py + 15, g.C_TXT2, bpos);
    draw_text_right_bold(s, f, 13.5f, g.ui_px1, py + 15, g.C_TXT2, bdur);

    const int cy = py + 56;
    const int cx = info_x + info_w / 2;
    g.rc_bar_play = Rect{ cx - 28, cy - 28, 56, 56 };
    g.rc_bar_prev = Rect{ cx - 80, cy - 21, 44, 42 };
    g.rc_bar_next = Rect{ cx + 36, cy - 21, 44, 42 };
    g.rc_bar_fav  = Rect{ info_x - 4, cy - 20, 40, 40 };
    g.rc_bar_mode = Rect{ info_x + info_w - 58, cy - 19, 58, 38 };

    const int is_fav = (g.playing_idx >= 0 && g.tracks[g.playing_idx].fav);
    icon_heart_ex(s, info_x + 6, cy - 10, 20, is_fav ? g.C_HEART : g.C_TXT2, is_fav);
    icon_prev(s, cx - 58, cy, 14, g.C_TXT);
    fill_m3_scallop(s, cx, cy, in_rc(mx, my, g.rc_bar_play) ? 27 : 25, 10, g.scallop_rot, g.C_ACCENT);
    if (g.playing_idx >= 0 && !audio_paused()) icon_pause(s, cx, cy, 13, g.C_ON_ACCENT);
    else                                       icon_play(s, cx + 2, cy, 14, g.C_ON_ACCENT);
    icon_next(s, cx + 58, cy, 14, g.C_TXT);

    const char* ml = g.play_mode == MODE_LOOP_ONE ? "单曲" : (g.play_mode == MODE_SHUFFLE ? "随机" : "顺序");
    const bool hov_m = in_rc(mx, my, g.rc_bar_mode);
    fill_frosted_round_rect(s, g.rc_bar_mode.x, g.rc_bar_mode.y, 58, 38, 19,
                            g.C_SURFACE, hov_m ? 180 : static_cast<uint8_t>(128 + 38 * g.light_t), 2, glass_rim(48, 205));
    draw_text_centered_bold(s, f, 13.5f, g.rc_bar_mode.x, g.rc_bar_mode.x + 58, g.rc_bar_mode.y + 10, g.C_TXT2, ml);

    const int lyr_x = cov_x + cov_sz + 56;
    const int lyr_w = std::max(240, g.W - lyr_x - 48);
    constexpr int lyr_y0 = 64;
    const int lyr_y1 = g.H - 36;
    g.ui_list_x0 = lyr_x - 16; g.ui_list_x1 = lyr_x + lyr_w + 16;
    g.ui_list_y0 = lyr_y0;     g.ui_list_y1 = lyr_y1;
    canvas_set_clip(s, g.ui_list_x0, lyr_y0, g.ui_list_x1, lyr_y1);

    const float orig_px = clampf(static_cast<float>(g.W) * 0.029f, 34.0f, 48.0f);
    const float sub_px  = clampf(orig_px * 0.62f, 20.0f, 30.0f);

    if (!g.lyr || g.lyr->n <= 0) {
        g.lyr_max_scroll = 0.0f;
        const char* msg = lyrics_online_busy(g.lyr_req_id) ? "正在从云端匹配双语歌词..." :
                          (g.playing_idx >= 0 ? "当前曲目暂无内嵌/匹配歌词，可将 .lrc 拖入窗口" : "请先选择一首音乐开始播放");
        draw_text_max_bold(s, f, 26.0f, lyr_x, g.H / 2 - 20, g.C_TXT2, msg, lyr_w);
    } else {
        const int max_n = g.lyr->n;
        const int cur_idx = std::clamp(lyrics_index(g.lyr.get(), pos), 0, max_n - 1);

        if (g.lyr_wrap_ptr != g.lyr.get() || g.lyr_wrap_w != (lyr_w - 24) ||
            std::fabs(g.lyr_wrap_opx - orig_px) > 0.25f || std::fabs(g.lyr_wrap_spx - sub_px) > 0.25f ||
            static_cast<int>(g.lyr_wrapped.size()) != max_n) {
            g.lyr_wrapped.resize(static_cast<size_t>(max_n));
            int cur_acc_y = 0;
            for (int i = 0; i < max_n; ++i) {
                auto& wl = g.lyr_wrapped[i];
                const char* orig_s = g.lyr->lines[i].orig[0] ? g.lyr->lines[i].orig : g.lyr->lines[i].text;
                wl.o_lines = wrap_utf8_lines(orig_s, orig_px, lyr_w - 24, 4, 1);
                if (wl.o_lines.empty()) wl.o_lines.emplace_back(orig_s);
                if (g.lyr->lines[i].sub[0]) wl.s_lines = wrap_utf8_lines(g.lyr->lines[i].sub, sub_px, lyr_w - 24, 3, 1);
                else wl.s_lines.clear();
                const int no = static_cast<int>(wl.o_lines.size());
                const int ns = static_cast<int>(wl.s_lines.size());
                wl.h = no * static_cast<int>(orig_px * 1.28f) + (ns > 0 ? ns * static_cast<int>(sub_px * 1.32f) + 6 : 0) + 28;
                wl.y = cur_acc_y;
                cur_acc_y += wl.h;
            }
            g.lyr_wrap_ptr = g.lyr.get();
            g.lyr_wrap_w   = lyr_w - 24;
            g.lyr_wrap_opx = orig_px;
            g.lyr_wrap_spx = sub_px;
            const auto& last_wl = g.lyr_wrapped[max_n - 1];
            g.lyr_max_scroll = static_cast<float>(last_wl.y + last_wl.h);
        }

        if (g.lyr_manual_timer > 0.0f) {
            g.lyr_manual_timer -= static_cast<float>(g.dt);
        } else {
            const float target_y = static_cast<float>(g.lyr_wrapped[cur_idx].y) + static_cast<float>(g.lyr_wrapped[cur_idx].h) * 0.35f;
            if (!g.lyr_cam_valid) { g.lyr_cam_y = target_y; g.lyr_cam_valid = 1; }
            else g.lyr_cam_y += (target_y - g.lyr_cam_y) * (1.0f - std::exp(-7.0f * static_cast<float>(g.dt)));
        }
        g.lyr_cam_y = clampf(g.lyr_cam_y, 0.0f, std::max(0.0f, g.lyr_max_scroll));

        const int anchor_y = lyr_y0 + static_cast<int>((lyr_y1 - lyr_y0) * 0.42f);
        for (int i = 0; i < max_n; ++i) {
            const auto& wl = g.lyr_wrapped[i];
            const int y = anchor_y + wl.y - static_cast<int>(g.lyr_cam_y);
            if (y + wl.h < lyr_y0 - 40 || y > lyr_y1 + 40) continue;

            const bool is_cur = (i == cur_idx);
            const int dist = std::abs(i - cur_idx);
            float alpha = is_cur ? 1.0f : clampf(0.72f - static_cast<float>(dist) * 0.10f, 0.28f, 0.72f);
            float edge_fade = 1.0f;
            if (y < lyr_y0 + 68) edge_fade = clampf(static_cast<float>(y - lyr_y0 + 6) / 74.0f, 0.0f, 1.0f);
            if (y + wl.h > lyr_y1 - 56) {
                edge_fade = std::min(edge_fade, clampf(static_cast<float>(lyr_y1 - y - wl.h / 2) / 64.0f, 0.0f, 1.0f));
            }
            if (!is_cur && edge_fade <= 0.03f) continue;

            const bool hov_l = in_rect(mx, my, lyr_x - 10, y - 4, lyr_w, wl.h - 12);
            if (hov_l) alpha = std::max(alpha, 0.88f);

            if (is_cur) {
                fill_frosted_round_rect(s, lyr_x - 20, y - 8, lyr_w + 22, wl.h - 14, 18,
                                        mix_col(g.C_SURFACE, g.C_ACCENT, 0.16f),
                                        static_cast<uint8_t>(106 + 68 * g.light_t), 3, glass_rim(34, 155));
                fill_round_rect_aa(s, lyr_x - 11, y + 10, 4, std::max(14, wl.h - 50), 2, g.C_ACCENT);
            } else if (hov_l) {
                fill_frosted_round_rect(s, lyr_x - 20, y - 8, lyr_w + 22, wl.h - 14, 18,
                                        g.C_SURFACE, static_cast<uint8_t>(86 + 46 * g.light_t), 2, glass_rim(28, 140));
            }

            const uint8_t oa = static_cast<uint8_t>(clampf(alpha * edge_fade * 255.0f, 4.0f, 255.0f));
            const uint8_t sa = static_cast<uint8_t>(clampf(alpha * edge_fade * 235.0f, 4.0f, 255.0f));
            const uint32_t col_orig = with_alpha(is_cur ? g.C_TXT : g.C_TXT2, oa);
            const uint32_t col_sub  = with_alpha(is_cur ? g.C_ACCENT : g.C_TXT2, sa);

            int cur_y = y;
            for (const auto& line : wl.o_lines) {
                draw_text_bold(s, f, orig_px, lyr_x, cur_y, col_orig, line.c_str());
                cur_y += static_cast<int>(orig_px * 1.28f);
            }
            if (!wl.s_lines.empty()) {
                cur_y += 4;
                for (const auto& line : wl.s_lines) {
                    if (is_cur || dist <= 1) draw_text_bold(s, f, sub_px, lyr_x, cur_y, col_sub, line.c_str());
                    else                     draw_text(s, f, sub_px, lyr_x, cur_y, col_sub, line.c_str());
                    cur_y += static_cast<int>(sub_px * 1.32f);
                }
            }

            if (static_cast<int>(g.lyr_hits.size()) < LYRIC_MAX_HITS) {
                g.lyr_hits.push_back(LyrHit{ Rect{ lyr_x - 14, y - 8, lyr_w + 16, wl.h - 14 }, g.lyr->lines[i].t });
            }
        }
    }
    canvas_reset_clip(s);
}

void draw_diag_modal() {
    if (!g.show_diag) return;
    Canvas* s = g.screen.get();
    Font* f = g.font.get();
    constexpr int mw = 520, mh = 250;
    const int mx = (g.W - mw) / 2, my = (g.H - mh) / 2;
    fill_round_rect_shadow(s, mx, my + 8, mw, mh, 22, theme_shadow(150));
    fill_frosted_round_rect(s, mx, my, mw, mh, 22, g.C_SURFACE,
                            static_cast<uint8_t>(210 + 26 * g.light_t), 4, glass_rim(96, 235));
    stroke_round_rect_aa(s, mx, my, mw, mh, 22, 1, g.C_ACCENT, 130);
    draw_text_bold(s, f, 18.0f, mx + 24, my + 20, g.C_TXT, "Sir\xC3\xA8ne 音频与歌词诊断（学习研究版）");
    char adiag[256]{};
    audio_get_diag(adiag, sizeof(adiag));
    draw_text_max(s, f, 13.0f, mx + 24, my + 60, g.C_TXT2, adiag, mw - 48);
    draw_text_max(s, f, 13.0f, mx + 24, my + 96, g.C_TXT2, lyrics_last_diag(), mw - 48);
    char sc[128]{};
    snprintf(sc, sizeof(sc), "DPI 缩放: %.2fx  |  窗口逻辑分辨率: %dx%d  |  曲目数: %d",
             g.ui_scale, g.W, g.H, static_cast<int>(g.tracks.size()));
    draw_text_max(s, f, 12.5f, mx + 24, my + 136, g.C_ACCENT, sc, mw - 48);
    draw_text(s, f, 12.0f, mx + 24, my + 204, g.C_TXT3, "提示: 再次点击左下角 i 按钮或按 Esc 关闭");
}

void handle_text_input_events() {
    char ch[32]{};
    int act = 0;
    bool changed = false;
    while (be_poll_text_char(ch, sizeof(ch), &act)) {
        if (act == 1) {
            if (!g.search_q.empty()) { pop_utf8_cp(g.search_q); changed = true; }
        } else if (act == 2 || act == 3) {
            g.search_focused = 0;
            be_text_input(0);
        } else if (act == 0 && ch[0]) {
            if (g.search_q.size() + std::strlen(ch) < 156) {
                g.search_q += ch;
                changed = true;
            }
        }
    }
    if (changed) {
        rebuild_visible();
        g.scroll_px = g.scroll_target = 0.0f;
    }
}

void toggle_play_pause_action() {
    if (g.playing_idx >= 0) {
        if (audio_paused()) audio_resume();
        else audio_pause();
    } else if (!g.vis_idx.empty()) {
        load_track(g.vis_idx[0]);
    } else if (!g.tracks.empty()) {
        load_track(0);
    }
}

void update_hover_cursor(int mx, int my) {
    bool hand = in_rc(mx, my, g.rc_bar_fav) || in_rc(mx, my, g.rc_bar_prev) || in_rc(mx, my, g.rc_bar_play) ||
                in_rc(mx, my, g.rc_bar_next) || in_rc(mx, my, g.rc_bar_mode) || in_rc(mx, my, g.rc_bar_lyr) ||
                in_rc(mx, my, g.rc_bar_cov) || in_rc(mx, my, g.rc_bar_vol_ico) || in_rc(mx, my, g.rc_bar_vol_trk) ||
                in_rc(mx, my, g.rc_bar_prog);
    if (!hand && g.page == PAGE_LYRICS) {
        if (in_rc(mx, my, g.rc_back_lib) || in_rc(mx, my, g.rc_lyr_bg) || in_rc(mx, my, g.rc_lyr_bg_clear) ||
            in_rc(mx, my, g.rc_lyr_theme) || in_rc(mx, my, g.rc_lyr_fs)) hand = true;
        for (const auto& lh : g.lyr_hits) if (!hand && in_rc(mx, my, lh.rc)) hand = true;
    } else if (!hand) {
        if (in_rc(mx, my, g.rc_fab_import) || in_rc(mx, my, g.rc_menu_btn) ||
            in_rc(mx, my, g.rc_nav_all) || in_rc(mx, my, g.rc_nav_fav) || in_rc(mx, my, g.rc_nav_lyr) ||
            in_rc(mx, my, g.rc_nav_rescan) || in_rc(mx, my, g.rc_nav_bg) || in_rc(mx, my, g.rc_nav_view) ||
            in_rc(mx, my, g.rc_nav_theme) || in_rc(mx, my, g.rc_nav_diag) || in_rc(mx, my, g.rc_search_box) ||
            in_rc(mx, my, g.rc_sort_btn) || in_rc(mx, my, g.rc_view_pill) || in_rc(mx, my, g.rc_bg_btn) ||
            in_rc(mx, my, g.rc_bg_clear) || in_rc(mx, my, g.rc_fs_btn) || in_rc(mx, my, g.rc_dir_chip) ||
            in_rc(mx, my, g.rc_sb_track) || in_rc(mx, my, g.rc_hero_card)) {
            hand = true;
        }
        for (const auto& fc : g.rc_filt_chips) if (!hand && in_rc(mx, my, fc)) hand = true;
        for (const auto& gh : g.grid_hits) if (!hand && in_rc(mx, my, gh.card)) hand = true;
    }
    const int ih = hand ? 1 : 0;
    if (ih != g.ui_cursor_hand) { g.ui_cursor_hand = ih; be_set_cursor(ih); }
}

void toggle_theme_smooth() {
    g.is_light = !g.is_light;
    save_settings();
    show_toast(g.is_light ? "已切换 · 柔和日间主题" : "已切换 · 深邃夜间主题");
}

void handle_input() {
    int mcmd = 0;
    while ((mcmd = be_poll_media_cmd()) != 0) {
        if (mcmd == 1) toggle_play_pause_action();
        else if (mcmd == 2) { if (!g.tracks.empty()) load_track(pick_prev(g.playing_idx)); }
        else if (mcmd == 3) { if (!g.tracks.empty()) load_track(pick_next(g.playing_idx, 1)); }
        else if (mcmd == 4) { if (g.playing_idx >= 0 && !audio_paused()) audio_pause(); }
    }

    const char* dp = nullptr;
    while ((dp = be_poll_dropped_path()) != nullptr) handle_dropped(dp);

    handle_text_input_events();

    const int wh = be_wheel_delta();
    if (wh != 0) {
        if (g.page == PAGE_LYRICS) {
            g.lyr_scroll_vel = 0.0f;
            g.lyr_cam_y = clampf(g.lyr_cam_y - static_cast<float>(wh) * 56.0f, 0.0f, std::max(0.0f, g.lyr_max_scroll));
            g.lyr_manual_timer = 4.5f;
        } else {
            g.scroll_vel = 0.0f;
            g.scroll_target = clampf(g.scroll_target - static_cast<float>(wh) * 86.0f, 0.0f, max_scroll_px());
        }
    }

    const int hk = be_poll_hotkey();
    if (hk == 's') {
        if (g.page != PAGE_LIBRARY) { capture_fade(); g.page = PAGE_LIBRARY; }
        g.search_focused = 1;
        be_text_input(1);
    } else if (hk == 'f') {
        open_folder_dialog();
    } else if (hk == 'm') {
        g.play_mode = (g.play_mode + 1) % 3;
        save_settings();
        show_toast(g.play_mode == MODE_LOOP_ONE ? "单曲循环" : (g.play_mode == MODE_SHUFFLE ? "随机播放" : "顺序循环"));
    } else if (hk == 'c') {
        toggle_theme_smooth();
    } else if (hk == 'v') {
        if (g.page == PAGE_LIBRARY) {
            capture_fade();
            g.view_grid = !g.view_grid;
            g.scroll_px = g.scroll_target = 0.0f;
            g.scroll_vel = 0.0f;
            save_settings();
        }
    } else if (hk == 'b') {
        pick_bg_image_action();
    }

    TouchInfo ti{};
    be_touch_update(&ti);
    if (ti.down) {
        g.scroll_vel = 0.0f;
        g.lyr_scroll_vel = 0.0f;

        if (g.search_focused && !in_rc(ti.x, ti.y, g.rc_search_box)) {
            g.search_focused = 0;
            be_text_input(0);
        }
        if (g.show_diag) {
            constexpr int mw = 520, mh = 250;
            if (!in_rect(ti.x, ti.y, (g.W - mw) / 2, (g.H - mh) / 2, mw, mh)) g.show_diag = 0;
            else return;
        }

        if (g.page == PAGE_LIBRARY) {
            if (in_rc(ti.x, ti.y, g.rc_search_clear) && !g.search_q.empty()) {
                g.search_q.clear();
                rebuild_visible();
                g.scroll_px = g.scroll_target = 0.0f;
                return;
            }
            if (in_rc(ti.x, ti.y, g.rc_search_box)) {
                g.search_focused = 1;
                be_text_input(1);
                return;
            }
            if (in_rc(ti.x, ti.y, g.rc_fab_import)) {
                open_folder_dialog();
                return;
            }
        }

        if (in_rc(ti.x, ti.y, g.rc_bar_vol_ico)) {
            const float cur = audio_volume();
            if (cur > 0.01f) { g.saved_vol = cur; audio_set_volume(0.0f); }
            else             { audio_set_volume(g.saved_vol > 0.05f ? g.saved_vol : 0.85f); }
            save_settings();
            return;
        }
        if (in_rc(ti.x, ti.y, g.rc_bar_vol_trk) && g.ui_vx1 > g.ui_vx0) {
            g.vol_drag = 1;
            g.drag_mode = DRAG_VOL;
            const float f = clampf(static_cast<float>(ti.x - g.ui_vx0) / static_cast<float>(g.ui_vx1 - g.ui_vx0), 0.0f, 1.0f);
            audio_set_volume(f);
            if (f > 0.02f) g.saved_vol = f;
            return;
        }
        if (in_rc(ti.x, ti.y, g.rc_bar_prog) && g.playing_idx >= 0 && g.ui_px1 > g.ui_px0) {
            const double dur = audio_duration();
            if (dur > 0.1) {
                g.scrub_drag = 1;
                g.drag_mode = DRAG_ISLAND_PROG;
                g.scrub_sec = static_cast<double>(clampf(static_cast<float>(ti.x - g.ui_px0) / static_cast<float>(g.ui_px1 - g.ui_px0), 0.0f, 1.0f)) * dur;
            }
            return;
        }
        if (in_rc(ti.x, ti.y, g.rc_bar_fav))  { if (g.playing_idx >= 0) toggle_fav(g.playing_idx); return; }
        if (in_rc(ti.x, ti.y, g.rc_bar_prev)) { if (!g.tracks.empty()) load_track(pick_prev(g.playing_idx)); return; }
        if (in_rc(ti.x, ti.y, g.rc_bar_play)) { toggle_play_pause_action(); return; }
        if (in_rc(ti.x, ti.y, g.rc_bar_next)) { if (!g.tracks.empty()) load_track(pick_next(g.playing_idx, 1)); return; }
        if (in_rc(ti.x, ti.y, g.rc_bar_mode)) {
            g.play_mode = (g.play_mode + 1) % 3;
            save_settings();
            show_toast(g.play_mode == MODE_LOOP_ONE ? "单曲循环" : (g.play_mode == MODE_SHUFFLE ? "随机播放" : "顺序循环"));
            return;
        }
        if (in_rc(ti.x, ti.y, g.rc_bar_lyr) || in_rc(ti.x, ti.y, g.rc_bar_cov)) {
            capture_fade();
            g.page = (g.page == PAGE_LIBRARY) ? PAGE_LYRICS : PAGE_LIBRARY;
            return;
        }
        if (g.page == PAGE_LIBRARY && in_rc(ti.x, ti.y, g.rc_island)) return;

        if (g.page == PAGE_LYRICS) {
            if (in_rc(ti.x, ti.y, g.rc_back_lib))     { capture_fade(); g.page = PAGE_LIBRARY; return; }
            if (in_rc(ti.x, ti.y, g.rc_lyr_bg_clear)) { clear_bg_image(); return; }
            if (in_rc(ti.x, ti.y, g.rc_lyr_bg))       { pick_bg_image_action(); return; }
            if (in_rc(ti.x, ti.y, g.rc_lyr_theme))    { toggle_theme_smooth(); return; }
            if (in_rc(ti.x, ti.y, g.rc_lyr_fs))       { be_toggle_fullscreen(); return; }
            if (in_rect(ti.x, ti.y, g.ui_list_x0, g.ui_list_y0, g.ui_list_x1 - g.ui_list_x0, g.ui_list_y1 - g.ui_list_y0)) {
                g.drag_mode = DRAG_LYRICS;
                g.drag_start_x = ti.x;
                g.drag_start_y = ti.y;
                g.drag_moved = 0;
                g.drag_cand_lyr_t = -1.0;
                for (const auto& lh : g.lyr_hits) {
                    if (in_rc(ti.x, ti.y, lh.rc)) { g.drag_cand_lyr_t = lh.t; break; }
                }
            }
            return;
        }

        if (in_rc(ti.x, ti.y, g.rc_menu_btn) || in_rc(ti.x, ti.y, g.rc_nav_all)) {
            g.lib_tab = TAB_ALL; g.active_dir = -1; g.fmt_filter = FMT_FILT_ALL;
            rebuild_visible(); g.scroll_px = g.scroll_target = 0.0f; return;
        }
        if (in_rc(ti.x, ti.y, g.rc_nav_fav)) { g.lib_tab = TAB_FAV; rebuild_visible(); g.scroll_px = g.scroll_target = 0.0f; return; }
        if (in_rc(ti.x, ti.y, g.rc_nav_lyr)) { capture_fade(); g.page = PAGE_LYRICS; return; }
        if (in_rc(ti.x, ti.y, g.rc_nav_rescan)) { scan_library(1); return; }
        if (in_rc(ti.x, ti.y, g.rc_nav_bg)) { pick_bg_image_action(); return; }
        if (in_rc(ti.x, ti.y, g.rc_nav_view) || in_rc(ti.x, ti.y, g.rc_view_pill)) {
            capture_fade(); g.view_grid = !g.view_grid; g.scroll_px = g.scroll_target = 0.0f; save_settings(); return;
        }
        if (in_rc(ti.x, ti.y, g.rc_nav_theme)) { toggle_theme_smooth(); return; }
        if (in_rc(ti.x, ti.y, g.rc_nav_diag))  { g.show_diag = !g.show_diag; return; }

        if (in_rc(ti.x, ti.y, g.rc_bg_clear))  { clear_bg_image(); return; }
        if (in_rc(ti.x, ti.y, g.rc_bg_btn))    { pick_bg_image_action(); return; }
        if (in_rc(ti.x, ti.y, g.rc_sort_btn))  { g.sort_mode = (g.sort_mode + 1) % 4; rebuild_visible(); save_settings(); return; }
        if (in_rc(ti.x, ti.y, g.rc_fs_btn))    { be_toggle_fullscreen(); return; }
        if (in_rc(ti.x, ti.y, g.rc_dir_chip)) {
            if (g.lib_tab != TAB_DIR) { g.lib_tab = TAB_DIR; g.active_dir = 0; }
            else if (g.active_dir + 1 < static_cast<int>(g.scan_dirs.size())) ++g.active_dir;
            else { g.lib_tab = TAB_ALL; g.active_dir = -1; }
            rebuild_visible(); g.scroll_px = g.scroll_target = 0.0f; return;
        }
        for (int i = 0; i < 5; ++i) {
            if (in_rc(ti.x, ti.y, g.rc_filt_chips[i])) {
                g.fmt_filter = i; rebuild_visible(); g.scroll_px = g.scroll_target = 0.0f; return;
            }
        }
        if (in_rc(ti.x, ti.y, g.rc_sb_track)) {
            g.sb_drag = 1;
            g.drag_mode = DRAG_SB;
            const float f = clampf(static_cast<float>(ti.y - g.rc_sb_track.y) / static_cast<float>(g.rc_sb_track.h), 0.0f, 1.0f);
            g.scroll_target = g.scroll_px = f * max_scroll_px();
            return;
        }

        if (in_rect(ti.x, ti.y, g.ui_list_x0, g.ui_list_y0, g.ui_list_x1 - g.ui_list_x0, g.ui_list_y1 - g.ui_list_y0)) {
            if (in_rc(ti.x, ti.y, g.rc_hero_prog) && g.playing_idx >= 0 && g.ui_hpx1 > g.ui_hpx0) {
                const double dur = audio_duration();
                if (dur > 0.1) {
                    g.scrub_drag = 2;
                    g.drag_mode = DRAG_HERO_PROG;
                    g.scrub_sec = static_cast<double>(clampf(static_cast<float>(ti.x - g.ui_hpx0) / static_cast<float>(g.ui_hpx1 - g.ui_hpx0), 0.0f, 1.0f)) * dur;
                }
                return;
            }
            if (in_rc(ti.x, ti.y, g.rc_hero_play)) { toggle_play_pause_action(); return; }

            g.drag_mode = DRAG_LIST;
            g.drag_start_x = ti.x;
            g.drag_start_y = ti.y;
            g.drag_moved = 0;
            g.drag_cand_track = -1;
            g.drag_cand_fav = 0;
            g.drag_cand_hero = 0;
            g.scroll_target = g.scroll_px;

            if (in_rc(ti.x, ti.y, g.rc_hero_cov) || in_rc(ti.x, ti.y, g.rc_hero_card)) {
                g.drag_cand_hero = 1;
                return;
            }
            for (const auto& gh : g.grid_hits) {
                if (in_rc(ti.x, ti.y, gh.fav))  { g.drag_cand_track = gh.track_idx; g.drag_cand_fav = 1; return; }
                if (in_rc(ti.x, ti.y, gh.card)) { g.drag_cand_track = gh.track_idx; g.drag_cand_fav = 0; return; }
            }
        }
    }

    if (ti.held) {
        if (g.scrub_drag == 1 && g.ui_px1 > g.ui_px0) {
            g.scrub_sec = static_cast<double>(clampf(static_cast<float>(ti.x - g.ui_px0) / static_cast<float>(g.ui_px1 - g.ui_px0), 0.0f, 1.0f)) * audio_duration();
        } else if (g.scrub_drag == 2 && g.ui_hpx1 > g.ui_hpx0) {
            g.scrub_sec = static_cast<double>(clampf(static_cast<float>(ti.x - g.ui_hpx0) / static_cast<float>(g.ui_hpx1 - g.ui_hpx0), 0.0f, 1.0f)) * audio_duration();
        } else if (g.vol_drag && g.ui_vx1 > g.ui_vx0) {
            const float f = clampf(static_cast<float>(ti.x - g.ui_vx0) / static_cast<float>(g.ui_vx1 - g.ui_vx0), 0.0f, 1.0f);
            audio_set_volume(f);
            if (f > 0.02f) g.saved_vol = f;
        } else if (g.sb_drag && g.rc_sb_track.h > 10) {
            const float f = clampf(static_cast<float>(ti.y - g.rc_sb_track.y) / static_cast<float>(g.rc_sb_track.h), 0.0f, 1.0f);
            g.scroll_target = g.scroll_px = f * max_scroll_px();
        } else if (g.drag_mode == DRAG_LIST) {
            const int thr = be_input_is_touch() ? 12 : 8;
            if (!g.drag_moved && (std::abs(ti.y - g.drag_start_y) > thr || std::abs(ti.x - g.drag_start_x) > thr)) {
                g.drag_moved = 1;
            }
            if (g.drag_moved && max_scroll_px() > 0.0f) {
                g.scroll_px = clampf(g.scroll_px - static_cast<float>(ti.dy), 0.0f, max_scroll_px());
                g.scroll_target = g.scroll_px;
                const float dt_s = std::max(0.004f, static_cast<float>(g.dt));
                const float inst_v = -static_cast<float>(ti.dy) / dt_s;
                g.scroll_vel = (ti.dy == 0) ? (g.scroll_vel * 0.65f) : (g.scroll_vel * 0.50f + inst_v * 0.50f);
            }
        } else if (g.drag_mode == DRAG_LYRICS) {
            const int thr = be_input_is_touch() ? 12 : 8;
            if (!g.drag_moved && (std::abs(ti.y - g.drag_start_y) > thr || std::abs(ti.x - g.drag_start_x) > thr)) {
                g.drag_moved = 1;
            }
            if (g.drag_moved) {
                g.lyr_cam_y = clampf(g.lyr_cam_y - static_cast<float>(ti.dy), 0.0f, std::max(0.0f, g.lyr_max_scroll));
                g.lyr_manual_timer = 4.5f;
                const float dt_s = std::max(0.004f, static_cast<float>(g.dt));
                const float inst_v = -static_cast<float>(ti.dy) / dt_s;
                g.lyr_scroll_vel = (ti.dy == 0) ? (g.lyr_scroll_vel * 0.65f) : (g.lyr_scroll_vel * 0.50f + inst_v * 0.50f);
            }
        }
    }

    if (ti.up) {
        if (g.scrub_drag) { audio_seek(g.scrub_sec); g.scrub_drag = 0; }
        if (g.vol_drag)   { g.vol_drag = 0; save_settings(); }
        g.sb_drag = 0;
        if (g.drag_mode == DRAG_LIST) {
            if (!g.drag_moved) {
                if (g.drag_cand_hero) {
                    if (g.playing_idx >= 0) { capture_fade(); g.page = PAGE_LYRICS; }
                    else if (!g.vis_idx.empty()) load_track(g.vis_idx[0]);
                } else if (g.drag_cand_track >= 0) {
                    if (g.drag_cand_fav) toggle_fav(g.drag_cand_track);
                    else if (g.drag_cand_track == g.playing_idx) { capture_fade(); g.page = PAGE_LYRICS; }
                    else load_track(g.drag_cand_track);
                }
            } else {
                if (std::fabs(g.scroll_vel) < 65.0f) g.scroll_vel = 0.0f;
                else g.scroll_vel = clampf(g.scroll_vel, -3400.0f, 3400.0f);
            }
        } else if (g.drag_mode == DRAG_LYRICS) {
            if (!g.drag_moved && g.drag_cand_lyr_t >= 0.0) {
                audio_seek(g.drag_cand_lyr_t);
                if (audio_paused()) audio_resume();
                g.lyr_manual_timer = 0.0f;
            } else if (g.drag_moved) {
                if (std::fabs(g.lyr_scroll_vel) < 65.0f) g.lyr_scroll_vel = 0.0f;
                else g.lyr_scroll_vel = clampf(g.lyr_scroll_vel, -3000.0f, 3000.0f);
            }
        }
        g.drag_mode = DRAG_NONE;
        g.drag_moved = 0;
        g.drag_cand_track = -1;
        g.drag_cand_fav = 0;
        g.drag_cand_hero = 0;
        g.drag_cand_lyr_t = -1.0;
    }

    const uint64_t kd = be_btn_down();
    if (!g.search_focused && kd) {
        if (kd & BTN_B) {
            if (g.show_diag) g.show_diag = 0;
            else if (g.page == PAGE_LYRICS) { capture_fade(); g.page = PAGE_LIBRARY; }
        }
        if (kd & BTN_A) toggle_play_pause_action();
        if (kd & BTN_X) { if (g.playing_idx >= 0) toggle_fav(g.playing_idx); }
        if (kd & BTN_Y) { capture_fade(); g.page = (g.page == PAGE_LIBRARY) ? PAGE_LYRICS : PAGE_LIBRARY; }
        if (kd & BTN_ZL) { if (!g.tracks.empty()) load_track(pick_prev(g.playing_idx)); }
        if (kd & BTN_ZR) { if (!g.tracks.empty()) load_track(pick_next(g.playing_idx, 1)); }
        if (kd & BTN_LEFT)  { if (g.playing_idx >= 0) audio_seek(std::max(0.0, audio_position() - 5.0)); }
        if (kd & BTN_RIGHT) { if (g.playing_idx >= 0) audio_seek(std::min(audio_duration(), audio_position() + 5.0)); }
        if (kd & BTN_STICK_R_UP)   { audio_set_volume(clampf(audio_volume() + 0.05f, 0.0f, 1.0f)); save_settings(); }
        if (kd & BTN_STICK_R_DOWN) { audio_set_volume(clampf(audio_volume() - 0.05f, 0.0f, 1.0f)); save_settings(); }
        if (kd & BTN_UP)   { g.scroll_target = clampf(g.scroll_target - 140.0f, 0.0f, max_scroll_px()); }
        if (kd & BTN_DOWN) { g.scroll_target = clampf(g.scroll_target + 140.0f, 0.0f, max_scroll_px()); }
    }
}

int sync_canvas_to_window() {
    int pw = 1360, ph = 840;
    float sc = 1.0f;
    be_screen_metrics(&pw, &ph, &sc);
    pw = std::max(320, pw);
    ph = std::max(240, ph);
    sc = std::max(1.0f, sc);
    const int lw = std::max(200, static_cast<int>(static_cast<float>(pw) / sc + 0.5f));
    const int lh = std::max(150, static_cast<int>(static_cast<float>(ph) / sc + 0.5f));
    g.W = lw;
    g.H = lh;
    g.ui_scale = sc;

    if (!g.screen || g.screen->w != pw || g.screen->h != ph || g.screen->lw != lw || g.screen->lh != lh) {
        g.screen.reset(canvas_new(pw, ph));
        if (g.screen) {
            g.screen->lw = lw;
            g.screen->lh = lh;
            g.screen->s  = sc;
        }
        g.trans_shot.reset();
        g.fade_armed = 0;
        return 1;
    }
    return 0;
}

void render_frame(int mx, int my) {
    if (!g.screen) return;
    reset_hit_rects();
    if (g.page == PAGE_LIBRARY) {
        draw_frosted_ambient_bg(0);
        draw_nav_rail(mx, my);
        draw_library_header(mx, my);
        if (g.view_grid) draw_grid_view(mx, my);
        else             draw_table_view(mx, my);
        draw_dynamic_island(mx, my);
    } else {
        draw_immersive_view(mx, my);
    }
    draw_diag_modal();

    if (g.toast_timer > 0 && !g.toast_text.empty()) {
        Canvas* s = g.screen.get();
        const int tw = text_width_bold(g.font.get(), 13.5f, g.toast_text.c_str()) + 34;
        const int tx = (g.W - tw) / 2, ty = (g.page == PAGE_LYRICS) ? 20 : 66;
        const uint8_t a = g.toast_timer < 18 ? static_cast<uint8_t>(g.toast_timer * 12) : 220;
        const uint8_t sha = static_cast<uint8_t>(static_cast<float>(a) * (0.42f - 0.24f * g.light_t));
        fill_round_rect_shadow(s, tx, ty + 3, tw, 36, 18, sha);
        if (g.toast_timer >= 18) {
            fill_frosted_round_rect(s, tx, ty, tw, 36, 18, g.C_SURFACE,
                                    static_cast<uint8_t>(178 + 30 * g.light_t), 3, glass_rim(96, 235));
            stroke_round_rect_aa(s, tx, ty, tw, 36, 18, 1, g.C_ACCENT, 95);
        } else {
            fill_round_rect_aa_a(s, tx, ty, tw, 36, 18, g.C_SURFACE, a);
            stroke_round_rect_aa(s, tx, ty, tw, 36, 18, 1, g.C_ACCENT, static_cast<uint8_t>(a / 2));
        }
        const uint8_t ta = g.toast_timer < 18 ? static_cast<uint8_t>(std::min(255, g.toast_timer * 14)) : 255;
        draw_text_centered_bold(s, g.font.get(), 13.5f, tx, tx + tw, ty + 9, with_alpha(g.C_TXT, ta), g.toast_text.c_str());
        --g.toast_timer;
    }

    if (g.fade_armed && g.trans_shot && g.fade_time > 0.0f &&
        g.trans_shot->w == g.screen->w && g.trans_shot->h == g.screen->h) {
        const float t = smooth_u(g.fade_time / FADE_DUR);
        blend_over_opaque(g.screen->px, g.trans_shot->px,
                          static_cast<size_t>(g.screen->w) * g.screen->h, static_cast<uint8_t>(t * 255.0f));
        g.fade_time -= static_cast<float>(g.dt);
        if (g.fade_time <= 0.0f) g.fade_armed = 0;
    }
}

void dump_rgba_shot(const char* dir, const char* name) {
    if (!dir || !*dir || !g.screen) return;
    ensure_dir_recursive(dir);
    char path[640]{};
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE* f = plat_fopen(path, "wb");
    if (!f) return;
    const uint32_t hdr[2] = { static_cast<uint32_t>(g.screen->w), static_cast<uint32_t>(g.screen->h) };
    fwrite(hdr, sizeof(uint32_t), 2, f);
    fwrite(g.screen->px, 4, static_cast<size_t>(g.screen->w) * g.screen->h, f);
    fclose(f);
}

} // namespace sirene

int main(int argc, char** argv) {
    using namespace sirene;

    if (argc > 1) be_set_cli_path(argv[1]);
    if (be_init() != 0) return 1;
    audio_init();

    g.tracks.reserve(1024);
    g.font.reset(font_load(font_ttf_start, static_cast<size_t>(font_ttf_end - font_ttf_start)));
    build_posters();

    ensure_dir_recursive(be_config_dir());
    char lrc_cache[600]{};
    snprintf(lrc_cache, sizeof(lrc_cache), "%s/lyrics_cache", be_config_dir());
    ensure_dir_recursive(lrc_cache);
    lyrics_set_cache_dir(lrc_cache);

    load_settings();
    load_dirs_cfg();
    step_palette_and_ambient();
    scan_library(0);
    for (int i = 0; i < 16 && i < static_cast<int>(g.vis_idx.size()); ++i) {
        ensure_track_meta(g.vis_idx[i], 1);
    }

    const char* dump_dir = std::getenv("SIRENE_DUMP_DIR");
    const bool dump_mode = (dump_dir && *dump_dir && dump_dir[0] != '0');
    int dump_frame = 0;

    if (!dump_mode) {
        const int created = be_ensure_desktop_shortcut(g.shortcut_done);
        if (!g.shortcut_done) {
            g.shortcut_done = 1;
            save_settings();
        }
        if (created) show_toast("已自动在桌面生成 Sirène 图标");
    }

    g.last_ns = plat_ticks_ns();
    while (be_should_run()) {
        const uint64_t now = plat_ticks_ns();
        double dt = static_cast<double>(now - g.last_ns) / 1e9;
        g.last_ns = now;
        g.dt = std::clamp(dt, 0.002, 0.08);

        be_input_update();
        sync_canvas_to_window();
        audio_pump();

        if (audio_finished() && !g.tracks.empty()) load_track(pick_next(g.playing_idx, 0));

        if (g.lyr_req_id > 0) {
            Lyrics* ol = nullptr;
            if (lyrics_poll_online(g.lyr_req_id, &ol) != 0) {
                g.lyr_req_id = 0;
                if (ol) {
                    g.lyr.reset(ol);
                    g.lyr_cam_valid = 0;
                    g.lyr_wrap_ptr = nullptr;
                    g.lyr_wrapped.clear();
                }
            }
        }

        float live_bars[28]{};
        audio_get_spectrum(live_bars);
        const float k_bar = 1.0f - std::exp(-14.0f * static_cast<float>(g.dt));
        for (int i = 0; i < 28; ++i) g.bars[i] += (live_bars[i] - g.bars[i]) * k_bar;

        const bool playing_active = (g.playing_idx >= 0 && !audio_paused());
        g.wave_phase  += static_cast<float>(g.dt) * (playing_active ? 2.6f : 0.45f);
        g.ambient_t   += static_cast<float>(g.dt) * (playing_active ? 0.48f : 0.14f);
        g.scallop_rot += static_cast<float>(g.dt) * (playing_active ? 0.50f : 0.08f);
        if (g.hov_card_idx >= 0) {
            g.hov_card_t = clampf(g.hov_card_t + static_cast<float>(g.dt) * 9.0f, 0.0f, 1.0f);
        }

        const float mscr = max_scroll_px();
        if (g.drag_mode != DRAG_LIST && std::fabs(g.scroll_vel) > 4.0f) {
            g.scroll_px += g.scroll_vel * static_cast<float>(g.dt);
            if (g.scroll_px <= 0.0f)      { g.scroll_px = 0.0f; g.scroll_vel = 0.0f; }
            else if (g.scroll_px >= mscr) { g.scroll_px = mscr; g.scroll_vel = 0.0f; }
            g.scroll_vel *= std::exp(-5.2f * static_cast<float>(g.dt));
            g.scroll_target = g.scroll_px;
        } else if (g.drag_mode != DRAG_LIST) {
            g.scroll_target = clampf(g.scroll_target, 0.0f, mscr);
            const float k_scr = 1.0f - std::exp(-16.0f * static_cast<float>(g.dt));
            g.scroll_px += (g.scroll_target - g.scroll_px) * k_scr;
        }
        if (g.page == PAGE_LYRICS && g.drag_mode != DRAG_LYRICS && std::fabs(g.lyr_scroll_vel) > 4.0f) {
            g.lyr_cam_y = clampf(g.lyr_cam_y + g.lyr_scroll_vel * static_cast<float>(g.dt),
                                 0.0f, std::max(0.0f, g.lyr_max_scroll));
            if (g.lyr_cam_y <= 0.0f || g.lyr_cam_y >= g.lyr_max_scroll) g.lyr_scroll_vel = 0.0f;
            g.lyr_scroll_vel *= std::exp(-5.5f * static_cast<float>(g.dt));
            g.lyr_manual_timer = std::max(g.lyr_manual_timer, 3.0f);
        }

        step_palette_and_ambient();

        const bool scroll_moving = (g.drag_mode != DRAG_NONE) || g.scrub_drag || g.vol_drag || g.sb_drag ||
                                   (std::fabs(g.scroll_vel) > 3.0f) || (std::fabs(g.lyr_scroll_vel) > 3.0f) ||
                                   (std::fabs(g.scroll_target - g.scroll_px) > 1.0f);
        const bool theme_moving  = std::fabs(g.theme_pos - (g.is_light ? 1.0f : 0.0f)) > 0.001f;
        if (!scroll_moving && !theme_moving && !g.fade_armed) {
            pump_background_meta();
        }
        be_set_idle_mode((!dump_mode && !playing_active && !scroll_moving && !theme_moving &&
                          !g.fade_armed && g.toast_timer <= 0) ? 1 : 0);

        if (dump_mode) {
            if (dump_frame == 2) {
                if (const char* dump_bg = std::getenv("SIRENE_DUMP_BG")) {
                    if (std::strcmp(dump_bg, "none") == 0) {
                        g.bg_img_path.clear(); g.bg_img_raw.clear(); invalidate_bg_cache();
                    } else if (*dump_bg) {
                        load_bg_image(dump_bg);
                    }
                }
                g.is_light = 0; g.theme_pos = 0.0f; g.light_t = 0.0f;
                g.view_grid = 1;
                step_palette_and_ambient();
                if (g.playing_idx < 0 && !g.vis_idx.empty()) load_track(g.vis_idx[0]);
                audio_set_volume(0.0f);
                if (g.playing_idx >= 0) audio_seek(58.0);
            }
            if (dump_frame == 28) {
                render_frame(0, 0);
                dump_rgba_shot(dump_dir, "shot_lib.rgba");
                g.is_light = 1;
            }
            if (dump_frame == 48) {
                render_frame(0, 0);
                dump_rgba_shot(dump_dir, "shot_mid.rgba");
                g.view_grid = 0;
            }
            if (dump_frame == 94) {
                render_frame(0, 0);
                dump_rgba_shot(dump_dir, "shot_day.rgba");
                g.is_light = 0; g.theme_pos = 0.0f; g.light_t = 0.0f;
                step_palette_and_ambient();
                g.page = PAGE_LYRICS;
                g.fade_armed = 0;
            }
            if (dump_frame == 112) {
                render_frame(0, 0);
                dump_rgba_shot(dump_dir, "shot_lyr.rgba");
                break;
            }
            ++dump_frame;
        }

        handle_input();
        sync_canvas_to_window();

        const double cur_dur = (g.playing_idx >= 0) ? audio_duration() : 0.0;
        const double cur_pos = (g.playing_idx >= 0) ? (g.scrub_drag ? g.scrub_sec : audio_position()) : 0.0;
        const float cur_frac = (cur_dur > 0.1) ? clampf(static_cast<float>(cur_pos / cur_dur), 0.0f, 1.0f) : 0.0f;
        be_set_media_state(g.playing_idx >= 0, g.playing_idx >= 0 && !audio_paused(), cur_frac,
                           g.p_title.c_str(), g.p_artist.c_str());

        if (!be_is_minimized()) {
            int mx = 0, my = 0;
            be_mouse_pos(&mx, &my);
            const bool suppress_hover = g.drag_moved || (be_input_is_touch() && mx < 0);
            const int eff_mx = suppress_hover ? -1 : mx;
            const int eff_my = suppress_hover ? -1 : my;
            update_hover_cursor(eff_mx, eff_my);
            render_frame(eff_mx, eff_my);

            int stride = 0;
            uint32_t* fb = be_begin(&stride);
            int bw = 0, bh = 0;
            be_buffer_size(&bw, &bh);
            if (fb && g.screen && stride > 0 && bw > 0 && bh > 0) {
                const int cw = std::min({ g.screen->w, stride, bw });
                const int ch = std::min(g.screen->h, bh);
                for (int y = 0; y < ch; ++y) {
                    std::memcpy(fb + static_cast<size_t>(y) * stride,
                                g.screen->px + static_cast<size_t>(y) * g.screen->w,
                                static_cast<size_t>(cw) * 4);
                }
            }
            be_end();
        } else {
            be_sleep_ms(16);
        }
    }

    g.tracks.clear();
    g.meta.clear();
    g.lyr.reset();
    g.bg_mesh.reset();
    g.trans_shot.reset();
    g.screen.reset();
    g.font.reset();
    audio_exit();
    be_exit();
    return 0;
}
