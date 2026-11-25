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
#include <ctime>
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
constexpr int ROW_H         = 56;
constexpr int HEADER_H      = 84;
constexpr int BANNER_H      = 188;
constexpr int SECTION_H     = 44;
constexpr int PLAYER_H      = 68;
constexpr int PLAYER_MARGIN = 14;
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
    int cov_cache_px = 0;
    std::vector<uint32_t> cov_cache;
    int fmt_w = -1;
    double cached_dur_val = -1.0;
    char cached_dur_s[16] = "--:--";
    int cached_dur_w = -1;
};

enum { DRAG_NONE = 0, DRAG_ISLAND_PROG = 1, DRAG_VOL = 2, DRAG_SB = 3, DRAG_LIST = 4, DRAG_LYRICS = 5 };

struct WrappedLyrLine {
    std::vector<std::string> o_lines;
    std::vector<std::string> s_lines;
    int y = 0;
    int h = 0;
};

struct GridHit { Rect card{}; Rect fav{}; int track_idx = 0; };
struct LyrHit  { Rect rc{}; double t = 0.0; };
struct DirHit  { Rect rc{}; int dir_idx = 0; };

enum {
    SET_LIGHT = 1, SET_ACCENT_FOLLOW = 2, SET_BG_PICK = 3, SET_BG_CLEAR = 4,
    SET_VIEW = 5, SET_MODE = 6, SET_ADD_DIR = 7, SET_RESCAN = 8,
    SET_RM_DIR = 9, SET_VOL_TRK = 10, SET_BACK = 11,
    SET_THEME_LIGHT = 12, SET_THEME_DARK = 13, SET_PLAY_MODE = 14, SET_DIR_REMOVE = 15
};
struct SetHit  { Rect rc{}; int id = 0; int arg = 0; };

enum { PAGE_LIBRARY = 0, PAGE_LYRICS = 1, PAGE_SETTINGS = 2 };
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
    int accent_follow = 1;
    float accent_sw_t = 1.0f;
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
    int poster_card_px = 0;
    std::vector<uint32_t> poster_card_cache;
    int cover_ready = 0;
    float scroll_rest_timer = 1.0f;

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
    float set_content_h = 820.0f;
    float lyr_cam_y = 0.0f;
    int   lyr_cam_valid = 0;
    float lyr_manual_timer = 0.0f;
    float wave_phase = 0.0f, ambient_t = 0.0f, scallop_rot = 0.0f;
    float rail_pill_y = 66.0f;
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

    Rect rc_menu_btn{}, rc_nav_all{}, rc_nav_fav{}, rc_nav_lyr{}, rc_nav_add{}, rc_nav_set{};
    Rect rc_nav_bg{}, rc_nav_theme{}, rc_nav_rescan{};
    Rect rc_search_box{}, rc_search_clear{}, rc_sort_btn{}, rc_fs_btn{}, rc_view_pill{}, rc_bg_btn{}, rc_bg_clear{};
    std::array<Rect, 5> rc_filt_chips{};
    Rect rc_dir_chip{};
    Rect rc_hero_card{}, rc_hero_cov{}, rc_hero_play{};
    std::array<Rect, 3> rc_banner_next{};
    std::array<int, 3> banner_next_idx{ -1, -1, -1 };
    Rect rc_sb_track{};
    Rect rc_back_lib{}, rc_lyr_bg{}, rc_lyr_bg_clear{}, rc_lyr_theme{}, rc_lyr_fs{};
    Rect rc_island{}, rc_bar_cov{}, rc_bar_fav{}, rc_bar_prev{}, rc_bar_play{}, rc_bar_next{};
    Rect rc_bar_mode{}, rc_bar_lyr{}, rc_bar_vol_ico{}, rc_bar_vol_trk{}, rc_bar_prog{};
    Rect rc_set_back{};
    int ui_px0 = 0, ui_px1 = 100, ui_py = 0;
    int ui_vx0 = 0, ui_vx1 = 100;
    int ui_list_x0 = 0, ui_list_x1 = 0, ui_list_y0 = 0, ui_list_y1 = 0;

    int grid_cols = 5, grid_w = 1000, grid_gap = 18, card_w = 168, card_h = 220;
    std::vector<GridHit> grid_hits;
    std::vector<LyrHit> lyr_hits;
    std::vector<DirHit> rc_dir_rows;
    std::vector<SetHit> set_hits;
};

AppState g;

inline int side_w() { return g.W >= 1000 ? 220 : 76; }

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

    const uint32_t d_accent = g.accent_follow
                              ? boost_pastel_accent(acc_r, acc_g, acc_b, 0)
                              : boost_pastel_accent(72.0f, 140.0f, 255.0f, 0);
    const uint32_t l_accent = g.accent_follow
                              ? boost_pastel_accent(acc_r, acc_g, acc_b, 1)
                              : boost_pastel_accent(72.0f, 140.0f, 255.0f, 1);
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
        } else if (std::strncmp(line, "accent_follow=", 14) == 0) {
            g.accent_follow = std::atoi(line + 14) ? 1 : 0;
            g.accent_sw_t = g.accent_follow ? 1.0f : 0.0f;
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
    fprintf(f, "vol=%.3f\nmode=%d\nlight=%d\nview_grid=%d\nsort=%d\nshortcut=%d\naccent_follow=%d\nbg_img=%s\n",
            audio_volume(), g.play_mode, g.is_light, g.view_grid, g.sort_mode, g.shortcut_done,
            g.accent_follow, g.bg_img_path.c_str());
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
    int cnt = 0, cov_cnt = 0;
    for (const auto& tr : g.tracks) {
        if (!tr.thumb.empty()) ++cnt;
        if (!tr.cov_cache.empty()) ++cov_cnt;
    }
    if (cnt > MAX_THUMBS) {
        int far_i = -1, max_dist = -1;
        for (int p = 0; p < static_cast<int>(g.vis_idx.size()); ++p) {
            const int idx = g.vis_idx[p];
            if (idx == g.playing_idx || g.tracks[idx].thumb.empty()) continue;
            const int d = std::abs(p - center_pos);
            if (d > max_dist) { max_dist = d; far_i = idx; }
        }
        if (far_i >= 0) {
            std::vector<uint32_t>().swap(g.tracks[far_i].thumb);
            std::vector<uint32_t>().swap(g.tracks[far_i].cov_cache);
            g.tracks[far_i].cov_cache_px = 0;
        }
    }
    while (cov_cnt > 72) {
        int far_i = -1, max_dist = -1;
        for (int p = 0; p < static_cast<int>(g.vis_idx.size()); ++p) {
            const int idx = g.vis_idx[p];
            if (idx == g.playing_idx || g.tracks[idx].cov_cache.empty()) continue;
            const int d = std::abs(p - center_pos);
            if (d > max_dist) { max_dist = d; far_i = idx; }
        }
        if (far_i < 0) break;
        std::vector<uint32_t>().swap(g.tracks[far_i].cov_cache);
        g.tracks[far_i].cov_cache_px = 0;
        --cov_cnt;
    }
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
void icon_folder(Canvas* c, int x, int y, int sz, uint32_t col) {
    fill_round_rect_aa(c, x, y + 1, std::max(5, sz * 4 / 10), 4, 2, col);
    stroke_round_rect_aa(c, x, y + 3, sz, sz - 4, 3, 2, col, 225);
}
void icon_gear(Canvas* c, int cx, int cy, int r, uint32_t col) {
    stroke_circle_aa(c, cx, cy, std::max(3, r - 2), 2, col, 230);
    fill_ellipse_aa(c, cx, cy, std::max(2, r / 3), std::max(2, r / 3), col);
    for (int k = 0; k < 4; ++k) {
        const float ang = static_cast<float>(k) * (kPi * 0.25f);
        const int dx = static_cast<int>(std::cos(ang) * static_cast<float>(r) + 0.5f);
        const int dy = static_cast<int>(std::sin(ang) * static_cast<float>(r) + 0.5f);
        fill_rect_a(c, cx + dx - 1, cy + dy - 1, 2, 2, col, 230);
        fill_rect_a(c, cx - dx - 1, cy - dy - 1, 2, 2, col, 230);
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

void calc_grid_metrics(int& out_cols, int& out_gap, int& out_cw, int& out_ch) {
    const int avail = std::max(320, g.W - side_w() - 26 * 2 - 20);
    constexpr int gap = 18;
    const int cols = std::clamp((avail + gap) / (168 + gap), 2, 8);
    const int cw = (avail - gap * (cols - 1)) / cols;
    const int ch = cw + 52;
    out_cols = cols; out_gap = gap; out_cw = cw; out_ch = ch;
    g.grid_cols = cols; g.grid_gap = gap; g.grid_w = avail;
    g.card_w = cw; g.card_h = ch;
}

int list_vis_h() { return std::max(120, g.H - HEADER_H - 8); }

constexpr int ITEMS_TOP_OFF = 4 + BANNER_H + 14 + SECTION_H + 6;
constexpr int DOCK_CLEAR    = PLAYER_H + PLAYER_MARGIN * 2 + 14;

float max_scroll_px() {
    const int vh = list_vis_h();
    if (g.page == PAGE_SETTINGS) {
        const float ch = g.set_content_h + static_cast<float>(DOCK_CLEAR);
        return ch > static_cast<float>(vh) ? (ch - static_cast<float>(vh)) : 0.0f;
    }
    const int nvis = static_cast<int>(g.vis_idx.size());
    if (g.view_grid) {
        int cols, gap, cw, ch;
        calc_grid_metrics(cols, gap, cw, ch);
        const int rows = (nvis > 0) ? ((nvis + cols - 1) / cols) : 0;
        const int content_h = ITEMS_TOP_OFF + rows * ch + (rows > 0 ? (rows - 1) * gap : 0) + DOCK_CLEAR;
        return content_h > vh ? static_cast<float>(content_h - vh) : 0.0f;
    }
    const int content_h = ITEMS_TOP_OFF + 32 + nvis * ROW_H + DOCK_CLEAR;
    return content_h > vh ? static_cast<float>(content_h - vh) : 0.0f;
}

void pump_background_meta() {
    const int nvis = static_cast<int>(g.vis_idx.size());
    if (nvis <= 0) return;
    int loaded = 0;
    int first_row = 0;
    const float list_scr = std::max(0.0f, g.scroll_px - static_cast<float>(ITEMS_TOP_OFF));
    if (g.view_grid) {
        int cols, gap, cw, ch;
        calc_grid_metrics(cols, gap, cw, ch);
        const int row = std::max(0, static_cast<int>(list_scr / static_cast<float>(ch + gap)));
        first_row = row * cols;
    } else {
        first_row = std::max(0, static_cast<int>(list_scr / static_cast<float>(ROW_H)));
    }
    const int win = g.view_grid ? 20 : 16;
    for (int p = first_row; p < nvis && p < first_row + win && loaded < 1; ++p) {
        const int idx = g.vis_idx[p];
        if (g.tracks[idx].meta_state == 0) { ensure_track_meta(idx, 1); ++loaded; }
    }
    for (int step = 0; step < 2 && loaded < 1; ++step) {
        if (g.probe_cursor >= nvis) g.probe_cursor = 0;
        const int idx = g.vis_idx[g.probe_cursor++];
        if (g.tracks[idx].meta_state < 2) { ensure_track_meta(idx, 1); ++loaded; }
    }
    trim_thumbs(first_row);
}

void reset_hit_rects() {
    g.rc_menu_btn = g.rc_nav_all = g.rc_nav_fav = g.rc_nav_lyr = g.rc_nav_add = g.rc_nav_set = {};
    g.rc_nav_bg = g.rc_nav_theme = g.rc_nav_rescan = {};
    g.rc_search_box = g.rc_search_clear = g.rc_sort_btn = g.rc_fs_btn = g.rc_view_pill = g.rc_bg_btn = g.rc_bg_clear = {};
    g.rc_filt_chips.fill(Rect{});
    g.rc_dir_chip = {};
    g.rc_hero_card = g.rc_hero_cov = g.rc_hero_play = {};
    g.rc_banner_next.fill(Rect{});
    g.banner_next_idx.fill(-1);
    g.rc_sb_track = g.rc_back_lib = g.rc_lyr_bg = g.rc_lyr_bg_clear = g.rc_lyr_theme = g.rc_lyr_fs = {};
    g.rc_island = g.rc_bar_cov = g.rc_bar_fav = g.rc_bar_prev = g.rc_bar_play = g.rc_bar_next = {};
    g.rc_bar_mode = g.rc_bar_lyr = g.rc_bar_vol_ico = g.rc_bar_vol_trk = g.rc_bar_prog = {};
    g.rc_set_back = {};
    g.grid_hits.clear();
    g.lyr_hits.clear();
    g.rc_dir_rows.clear();
    g.set_hits.clear();
}

inline uint8_t glass_rim(uint8_t dark_a = 48, uint8_t light_a = 205) {
    return static_cast<uint8_t>(static_cast<float>(dark_a) + (static_cast<float>(light_a) - static_cast<float>(dark_a)) * g.light_t);
}

void draw_nav_rail(int mx, int my) {
    Canvas* s = g.screen.get();
    Font* f = g.font.get();
    const bool lt = g.light_t > 0.5f;
    const int sw = side_w();
    const bool wide = (sw > 120);

    fill_frosted_round_rect(s, 0, 0, sw, g.H, 0, g.C_RAIL, lt ? 182 : 172, 3, 0);
    fill_rect_a(s, sw - 1, 0, 1, g.H, g.C_DIV, col_a(g.C_DIV) + 16);

    if (wide) {
        g.rc_search_box = Rect{ 14, 16, sw - 28, 36 };
        const bool s_hov = in_rc(mx, my, g.rc_search_box);
        fill_frosted_round_rect(s, g.rc_search_box.x, g.rc_search_box.y, g.rc_search_box.w, g.rc_search_box.h, 18,
                                g.C_SURFACE, g.search_focused ? 205 : (s_hov ? 185 : (lt ? 168 : 138)), 2,
                                g.search_focused ? glass_rim(96, 230) : glass_rim(52, 205));
        stroke_round_rect_aa(s, g.rc_search_box.x, g.rc_search_box.y, g.rc_search_box.w, g.rc_search_box.h, 18,
                             g.search_focused ? 2 : 1, g.search_focused ? g.C_ACCENT : g.C_DIV,
                             g.search_focused ? 210 : (col_a(g.C_DIV) + 14));
        icon_search(s, g.rc_search_box.x + 17, g.rc_search_box.y + 17, 5,
                    g.search_focused ? g.C_ACCENT : g.C_TXT3);
        const int tmax = g.rc_search_box.w - 64;
        if (!g.search_q.empty()) {
            draw_text_max_bold(s, f, 13.0f, g.rc_search_box.x + 31, g.rc_search_box.y + 10,
                               g.C_TXT, g.search_q.c_str(), tmax);
            g.rc_search_clear = Rect{ g.rc_search_box.x + g.rc_search_box.w - 30, g.rc_search_box.y + 3, 28, 30 };
            fill_round_rect_aa(s, g.rc_search_clear.x + 3, g.rc_search_clear.y + 3, 24, 24, 12,
                               in_rc(mx, my, g.rc_search_clear) ? g.C_GLASS_ACT : g.C_GLASS);
            draw_text_centered_bold(s, f, 12.5f, g.rc_search_clear.x, g.rc_search_clear.x + 28,
                                    g.rc_search_clear.y + 7, g.C_TXT2, "x");
        } else {
            draw_text_max_bold(s, f, 12.5f, g.rc_search_box.x + 31, g.rc_search_box.y + 10,
                               g.C_TXT3, "搜索歌曲、歌手...", tmax);
        }
    } else {
        g.rc_search_box = Rect{ 16, 16, sw - 32, 36 };
        const bool s_act = g.search_focused || !g.search_q.empty();
        fill_frosted_round_rect(s, g.rc_search_box.x, g.rc_search_box.y, g.rc_search_box.w, g.rc_search_box.h, 18,
                                s_act ? mix_col(g.C_SURFACE, g.C_ACCENT, 0.25f) : g.C_SURFACE,
                                in_rc(mx, my, g.rc_search_box) ? 185 : (lt ? 164 : 138), 2, glass_rim(54, 205));
        icon_search(s, sw / 2 - 1, 33, 6, s_act ? g.C_ACCENT : g.C_TXT2);
    }

    int active_slot = -1;
    if (g.page == PAGE_SETTINGS) active_slot = 4;
    else if (g.page == PAGE_LYRICS) active_slot = 2;
    else if (g.lib_tab == TAB_ALL) active_slot = 0;
    else if (g.lib_tab == TAB_FAV) active_slot = 1;

    constexpr int nav_y0 = 66;
    const int nav_step = wide ? 42 : 48;
    const int pill_h = wide ? 36 : 38;
    if (active_slot >= 0) {
        const float tgt_pill_y = static_cast<float>(nav_y0 + active_slot * nav_step);
        const float kp = 1.0f - std::exp(-16.0f * static_cast<float>(g.dt));
        g.rail_pill_y += (tgt_pill_y - g.rail_pill_y) * kp;
        const int py = static_cast<int>(g.rail_pill_y + 0.5f);
        fill_frosted_round_rect(s, 12, py, sw - 24, pill_h, 10,
                                mix_col(g.C_SURFACE, g.C_ACCENT, 0.30f), 192, 2, glass_rim(58, 205));
        stroke_round_rect_aa(s, 12, py, sw - 24, pill_h, 10, 1, g.C_ACCENT, 125);
    }

    const char* labels_wide[5]  = { "为你推荐", "我喜欢的", "歌词", "刷新曲库", "设置" };
    const char* labels_short[5] = { "推荐", "喜欢", "歌词", "刷新", "设置" };
    Rect* nav_rcs[5] = { &g.rc_nav_all, &g.rc_nav_fav, &g.rc_nav_lyr, &g.rc_nav_rescan, &g.rc_nav_set };
    for (int i = 0; i < 5; ++i) {
        const int iy = nav_y0 + i * nav_step;
        *nav_rcs[i] = Rect{ 12, iy, sw - 24, pill_h };
        const int is_act = (i == active_slot);
        const int is_hov = in_rc(mx, my, *nav_rcs[i]);
        if (is_hov && !is_act) {
            fill_round_rect_aa(s, 12, iy, sw - 24, pill_h, 10, g.C_GLASS_HOV);
        }
        const uint32_t icol = is_act ? g.C_ACCENT : (is_hov ? g.C_TXT : g.C_TXT2);
        const int cy = iy + (wide ? 18 : 14);
        const int ix = wide ? 24 : (sw / 2 - 8);
        if (i == 0) icon_library_grid(s, ix, cy - 8, 16, icol);
        else if (i == 1) icon_heart_ex(s, ix, cy - 7, 16, is_act ? g.C_HEART : icol, is_act);
        else if (i == 2) icon_lyrics_waves(s, ix, cy - 8, 16, icol);
        else if (i == 3) icon_refresh(s, ix + 8, cy, 7, icol);
        else icon_gear(s, ix + 8, cy, 7, icol);

        if (wide) {
            draw_text_max_bold(s, f, 14.0f, 50, iy + 9,
                               is_act ? g.C_ACCENT : (is_hov ? g.C_TXT : g.C_TXT2),
                               labels_wide[i], sw - 64);
        } else {
            draw_text_centered_bold(s, f, 11.5f, 8, sw - 8, iy + 24,
                                    is_act ? g.C_ACCENT : g.C_TXT2, labels_short[i]);
        }
    }

    const int div_y = nav_y0 + 5 * nav_step + 8;
    fill_rect_a(s, 14, div_y, sw - 28, 1, g.C_DIV, col_a(g.C_DIV) + 12);

    const int bot_top = wide ? (g.H - 60) : (g.H - 104);
    if (wide) {
        draw_text_bold(s, f, 12.0f, 18, div_y + 12, g.C_TXT3, "我的目录");
        g.rc_nav_add = Rect{ sw - 38, div_y + 6, 26, 26 };
        if (in_rc(mx, my, g.rc_nav_add)) {
            fill_round_rect_aa(s, g.rc_nav_add.x, g.rc_nav_add.y, 26, 26, 13, g.C_GLASS_HOV);
        }
        icon_plus(s, g.rc_nav_add.x + 13, g.rc_nav_add.y + 13, 5,
                  in_rc(mx, my, g.rc_nav_add) ? g.C_ACCENT : g.C_TXT2);

        int dy = div_y + 38;
        const int n_dirs = static_cast<int>(g.scan_dirs.size());
        if (n_dirs == 0 && dy + 28 < bot_top) {
            draw_text_bold(s, f, 12.5f, 18, dy + 6, g.C_TXT3, "点击 + 添加音乐目录");
        }
        for (int di = 0; di < n_dirs && dy + 32 <= bot_top - 6; ++di) {
            Rect r{ 12, dy, sw - 24, 32 };
            const bool is_act = (g.page == PAGE_LIBRARY && g.lib_tab == TAB_DIR && g.active_dir == di);
            const bool is_hov = in_rc(mx, my, r);
            if (is_act) {
                fill_frosted_round_rect(s, r.x, r.y, r.w, r.h, 10,
                                        mix_col(g.C_SURFACE, g.C_ACCENT, 0.28f), 186, 1, glass_rim(54, 200));
                stroke_round_rect_aa(s, r.x, r.y, r.w, r.h, 10, 1, g.C_ACCENT, 115);
            } else if (is_hov) {
                fill_round_rect_aa(s, r.x, r.y, r.w, r.h, 10, g.C_GLASS_HOV);
            }
            const uint32_t dc = is_act ? g.C_ACCENT : (is_hov ? g.C_TXT : g.C_TXT2);
            icon_folder(s, r.x + 10, r.y + 9, 14, dc);
            std::string dname = basename_str(g.scan_dirs[di]);
            if (dname.empty()) dname = g.scan_dirs[di];
            draw_text_max_bold(s, f, 13.0f, r.x + 30, r.y + 8, dc, dname.c_str(), r.w - 38);
            g.rc_dir_rows.push_back(DirHit{ r, di });
            dy += 34;
        }

        fill_rect_a(s, 14, bot_top, sw - 28, 1, g.C_DIV, col_a(g.C_DIV) + 12);
        const int half_w = (sw - 32) / 2;
        g.rc_nav_theme = Rect{ 12, g.H - 50, half_w, 36 };
        g.rc_nav_bg    = Rect{ 12 + half_w + 8, g.H - 50, half_w, 36 };

        fill_frosted_round_rect(s, g.rc_nav_theme.x, g.rc_nav_theme.y, half_w, 36, 10,
                                g.C_SURFACE, in_rc(mx, my, g.rc_nav_theme) ? 185 : (lt ? 156 : 132), 1, glass_rim(48, 195));
        icon_theme_orb(s, g.rc_nav_theme.x + 18, g.rc_nav_theme.y + 18, 7, mix_col(g.C_TXT2, g.C_ACCENT, g.light_t));
        draw_text_bold(s, f, 12.5f, g.rc_nav_theme.x + 32, g.rc_nav_theme.y + 10, g.C_TXT2, lt ? "浅色" : "深色");

        const bool has_bg = !g.bg_img_raw.empty();
        fill_frosted_round_rect(s, g.rc_nav_bg.x, g.rc_nav_bg.y, half_w, 36, 10,
                                has_bg ? mix_col(g.C_SURFACE, g.C_ACCENT, 0.22f) : g.C_SURFACE,
                                in_rc(mx, my, g.rc_nav_bg) ? 185 : (lt ? 156 : 132), 1, glass_rim(48, 195));
        icon_wallpaper(s, g.rc_nav_bg.x + 12, g.rc_nav_bg.y + 10, 15, has_bg ? g.C_ACCENT : g.C_TXT2);
        draw_text_bold(s, f, 12.5f, g.rc_nav_bg.x + 32, g.rc_nav_bg.y + 10,
                       has_bg ? g.C_ACCENT : g.C_TXT2, "壁纸");
    } else {
        if (div_y + 44 <= bot_top) {
            g.rc_nav_add = Rect{ 14, div_y + 10, sw - 28, 36 };
            if (in_rc(mx, my, g.rc_nav_add)) {
                fill_round_rect_aa(s, g.rc_nav_add.x, g.rc_nav_add.y, g.rc_nav_add.w, 36, 10, g.C_GLASS_HOV);
            }
            icon_plus(s, sw / 2, g.rc_nav_add.y + 18, 6,
                      in_rc(mx, my, g.rc_nav_add) ? g.C_ACCENT : g.C_TXT2);
        }
        fill_rect_a(s, 14, bot_top, sw - 28, 1, g.C_DIV, col_a(g.C_DIV) + 12);
        g.rc_nav_bg    = Rect{ 14, g.H - 96, sw - 28, 38 };
        g.rc_nav_theme = Rect{ 14, g.H - 52, sw - 28, 38 };
        if (in_rc(mx, my, g.rc_nav_bg)) fill_round_rect_aa(s, g.rc_nav_bg.x, g.rc_nav_bg.y, g.rc_nav_bg.w, 38, 10, g.C_GLASS_HOV);
        icon_wallpaper(s, sw / 2 - 8, g.rc_nav_bg.y + 11, 16, !g.bg_img_raw.empty() ? g.C_ACCENT : g.C_TXT2);
        if (in_rc(mx, my, g.rc_nav_theme)) fill_round_rect_aa(s, g.rc_nav_theme.x, g.rc_nav_theme.y, g.rc_nav_theme.w, 38, 10, g.C_GLASS_HOV);
        icon_theme_orb(s, sw / 2, g.rc_nav_theme.y + 19, 8, mix_col(g.C_TXT2, g.C_ACCENT, g.light_t));
    }
}

void draw_library_header(int mx, int my) {
    Canvas* s = g.screen.get();
    Font* f = g.font.get();
    const int lx = side_w() + 26;
    const int rx = g.W - 26;
    const bool lt = g.light_t > 0.5f;

    std::time_t now_t = std::time(nullptr);
    std::tm tm_now{};
#ifdef _WIN32
    localtime_s(&tm_now, &now_t);
#else
    localtime_r(&now_t, &tm_now);
#endif
    static const char* wdays[7] = { "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六" };
    const int wd = (tm_now.tm_wday >= 0 && tm_now.tm_wday < 7) ? tm_now.tm_wday : 0;
    char date_buf[64]{};
    snprintf(date_buf, sizeof(date_buf), "%d月%d日  %s", tm_now.tm_mon + 1, tm_now.tm_mday, wdays[wd]);
    draw_text_bold(s, f, 12.5f, lx, 15, g.C_TXT3, date_buf);

    const char* greet = "晚上好";
    if (tm_now.tm_hour >= 5 && tm_now.tm_hour < 12) greet = "早上好";
    else if (tm_now.tm_hour >= 12 && tm_now.tm_hour < 14) greet = "中午好";
    else if (tm_now.tm_hour >= 14 && tm_now.tm_hour < 19) greet = "下午好";

    std::string heading = greet;
    if (g.lib_tab == TAB_FAV) heading = "我喜欢的";
    else if (g.lib_tab == TAB_DIR && g.active_dir >= 0 && g.active_dir < static_cast<int>(g.scan_dirs.size())) {
        heading = basename_str(g.scan_dirs[g.active_dir]);
        if (heading.empty()) heading = "音乐目录";
    }
    const int title_max_w = std::max(120, rx - lx - 330);
    draw_text_max_bold(s, f, 25.0f, lx, 36, g.C_TXT, heading.c_str(), title_max_w);

    constexpr int ty = 24;
    constexpr int bh = 36;
    g.rc_fs_btn = Rect{ rx - bh, ty, bh, bh };
    {
        const bool hov = in_rc(mx, my, g.rc_fs_btn);
        fill_frosted_round_rect(s, g.rc_fs_btn.x, g.rc_fs_btn.y, bh, bh, 18,
                                g.C_SURFACE, hov ? 188 : (lt ? 156 : 132), 2, glass_rim(56, 210));
        stroke_round_rect_aa(s, g.rc_fs_btn.x, g.rc_fs_btn.y, bh, bh, 18, 1, g.C_DIV, col_a(g.C_DIV) + 12);
    }
    icon_expand(s, g.rc_fs_btn.x + 10, g.rc_fs_btn.y + 10, 16, g.C_TXT2);

    const bool has_bg = !g.bg_img_raw.empty();
    int right_anchor = g.rc_fs_btn.x;
    if (has_bg) {
        g.rc_bg_clear = Rect{ right_anchor - 8 - bh, ty, bh, bh };
        const bool hov_cl = in_rc(mx, my, g.rc_bg_clear);
        fill_frosted_round_rect(s, g.rc_bg_clear.x, g.rc_bg_clear.y, bh, bh, 18,
                                g.C_SURFACE, hov_cl ? 190 : (lt ? 156 : 132), 2, glass_rim(56, 210));
        stroke_round_rect_aa(s, g.rc_bg_clear.x, g.rc_bg_clear.y, bh, bh, 18, 1, g.C_DIV, col_a(g.C_DIV) + 12);
        icon_close_x(s, g.rc_bg_clear.x + bh / 2, g.rc_bg_clear.y + bh / 2, 4, hov_cl ? g.C_HEART : g.C_TXT2);
        right_anchor = g.rc_bg_clear.x;
    }

    const int bg_w = text_width_bold(f, 13.0f, "背景") + 38;
    g.rc_bg_btn = Rect{ right_anchor - (has_bg ? 6 : 8) - bg_w, ty, bg_w, bh };
    {
        const bool hov = in_rc(mx, my, g.rc_bg_btn);
        fill_frosted_round_rect(s, g.rc_bg_btn.x, g.rc_bg_btn.y, bg_w, bh, 18,
                                has_bg ? mix_col(g.C_SURFACE, g.C_ACCENT, 0.24f) : g.C_SURFACE,
                                has_bg ? 196 : (hov ? 188 : (lt ? 156 : 132)), 2, glass_rim(56, 210));
        stroke_round_rect_aa(s, g.rc_bg_btn.x, g.rc_bg_btn.y, bg_w, bh, 18, 1,
                             has_bg ? g.C_ACCENT : g.C_DIV, has_bg ? 135 : (col_a(g.C_DIV) + 12));
    }
    icon_wallpaper(s, g.rc_bg_btn.x + 11, g.rc_bg_btn.y + 10, 16, has_bg ? g.C_ACCENT : g.C_TXT2);
    draw_text_bold(s, f, 13.0f, g.rc_bg_btn.x + 31, ty + 9, has_bg ? g.C_ACCENT : g.C_TXT2, "背景");

    g.rc_view_pill = Rect{ g.rc_bg_btn.x - 8 - bh, ty, bh, bh };
    {
        const bool hov = in_rc(mx, my, g.rc_view_pill);
        fill_frosted_round_rect(s, g.rc_view_pill.x, g.rc_view_pill.y, bh, bh, 18,
                                g.C_SURFACE, hov ? 188 : (lt ? 156 : 132), 2, glass_rim(56, 210));
        stroke_round_rect_aa(s, g.rc_view_pill.x, g.rc_view_pill.y, bh, bh, 18, 1, g.C_DIV, col_a(g.C_DIV) + 12);
    }
    if (g.view_grid) icon_list_rows(s, g.rc_view_pill.x + 10, g.rc_view_pill.y + 10, 16, g.C_TXT2);
    else             icon_library_grid(s, g.rc_view_pill.x + 10, g.rc_view_pill.y + 10, 16, g.C_TXT2);

    const char* sort_lbls[4] = { "排序: 默认", "排序: 歌名", "排序: 歌手", "排序: 专辑" };
    const char* slbl = sort_lbls[g.sort_mode & 3];
    const int sw_btn = text_width_bold(f, 13.0f, slbl) + 24;
    g.rc_sort_btn = Rect{ g.rc_view_pill.x - 8 - sw_btn, ty, sw_btn, bh };
    {
        const bool hov = in_rc(mx, my, g.rc_sort_btn);
        fill_frosted_round_rect(s, g.rc_sort_btn.x, g.rc_sort_btn.y, sw_btn, bh, 18,
                                g.C_SURFACE, hov ? 188 : (lt ? 156 : 132), 2, glass_rim(56, 210));
        stroke_round_rect_aa(s, g.rc_sort_btn.x, g.rc_sort_btn.y, sw_btn, bh, 18, 1, g.C_DIV, col_a(g.C_DIV) + 12);
    }
    draw_text_centered_bold(s, f, 13.0f, g.rc_sort_btn.x, g.rc_sort_btn.x + sw_btn, ty + 9, g.C_TXT2, slbl);
}

void draw_banner(Rect ban, int mx, int my) {
    if (ban.w < 240 || ban.h < 120) return;
    Canvas* s = g.screen.get();
    Font* f = g.font.get();
    const bool lt = g.light_t > 0.5f;
    g.rc_hero_card = ban;
    constexpr int r = 18;

    fill_round_rect_shadow(s, ban.x, ban.y + 4, ban.w, ban.h, r, theme_shadow(76));
    fill_frosted_round_rect(s, ban.x, ban.y, ban.w, ban.h, r, g.C_HERO_L, lt ? 204 : 194, 2, glass_rim(58, 210));

    canvas_set_clip(s, ban.x + r, ban.y, ban.x + ban.w - r, ban.y + ban.h);
    const int x0 = ban.x + r;
    const int x1 = ban.x + ban.w - r;
    constexpr int strips = 48;
    for (int i = 0; i < strips; ++i) {
        const float u = static_cast<float>(i + 1) / static_cast<float>(strips);
        const float sm = u * u * (3.0f - 2.0f * u);
        const int sx0 = x0 + ((x1 - x0) * i) / strips;
        const int sx1 = x0 + ((x1 - x0) * (i + 1)) / strips;
        const uint8_t a = static_cast<uint8_t>(sm * (lt ? 78.0f : 86.0f));
        if (a > 0 && sx1 > sx0) fill_rect_a(s, sx0, ban.y, sx1 - sx0, ban.h, g.C_HERO_R, a);
    }
    canvas_reset_clip(s);
    stroke_round_rect_aa(s, ban.x, ban.y, ban.w, ban.h, r, 1, mix_col(g.C_DIV, g.C_ACCENT, 0.32f), lt ? 135 : 70);

    int show_idx = g.playing_idx;
    if (show_idx < 0 && !g.vis_idx.empty()) show_idx = g.vis_idx[0];
    const bool paused = audio_paused() || g.playing_idx < 0;

    const int lx = ban.x + 26;
    const int show_mid = (ban.w >= 680) ? 1 : 0;
    const int left_max_w = std::max(140, show_mid ? static_cast<int>(ban.w * 0.36f) : (ban.w - 240));

    const char* eyebrow = (g.playing_idx >= 0 && !paused) ? "正在播放" : (g.playing_idx >= 0 ? "已暂停" : "每日精选 · 随时聆听");
    draw_text_max_bold(s, f, 12.5f, lx, ban.y + 22, g.C_ACCENT, eyebrow, left_max_w);

    const char* ttl = g.playing_idx >= 0 ? g.p_title.c_str()
                                         : (show_idx >= 0 ? g.tracks[show_idx].title.c_str() : "添加音乐目录开始聆听");
    std::string sub_str;
    if (g.playing_idx >= 0) {
        sub_str = !g.p_artist.empty() ? g.p_artist : "本地音乐";
        if (!g.p_album.empty()) { sub_str += " · "; sub_str += g.p_album; }
    } else if (show_idx >= 0) {
        sub_str = !g.tracks[show_idx].artist.empty() ? g.tracks[show_idx].artist : "本地音乐";
        if (!g.tracks[show_idx].album.empty()) { sub_str += " · "; sub_str += g.tracks[show_idx].album; }
    } else {
        sub_str = "支持 FLAC / WAV / MP3 / OGG / OPUS / M4A";
    }
    draw_text_max_bold(s, f, 23.0f, lx, ban.y + 46, g.C_TXT, ttl, left_max_w);
    draw_text_max_bold(s, f, 13.0f, lx, ban.y + 80, g.C_TXT2, sub_str.c_str(), left_max_w);

    g.rc_hero_play = Rect{ lx, ban.y + 118, 108, 38 };
    const bool hov_play = in_rc(mx, my, g.rc_hero_play);
    fill_round_rect_shadow_fast(s, g.rc_hero_play.x, g.rc_hero_play.y + 2, g.rc_hero_play.w, g.rc_hero_play.h, 19, theme_shadow(78));
    fill_round_rect_aa(s, g.rc_hero_play.x, g.rc_hero_play.y, g.rc_hero_play.w, g.rc_hero_play.h, 19,
                       hov_play ? mix_col(g.C_ACCENT, COL(255, 255, 255, 255), 0.14f) : g.C_ACCENT);
    if (!paused && g.playing_idx >= 0) {
        icon_pause(s, g.rc_hero_play.x + 28, g.rc_hero_play.y + 19, 10, g.C_ON_ACCENT);
    } else {
        icon_play(s, g.rc_hero_play.x + 29, g.rc_hero_play.y + 19, 10, g.C_ON_ACCENT);
    }
    draw_text_bold(s, f, 14.0f, g.rc_hero_play.x + 48, g.rc_hero_play.y + 10, g.C_ON_ACCENT,
                   (!paused && g.playing_idx >= 0) ? "暂停" : "播放");

    const int nvis = static_cast<int>(g.vis_idx.size());
    if (show_mid && nvis > 0) {
        const int mid_x = ban.x + static_cast<int>(ban.w * 0.42f);
        const int cov_left = ban.x + ban.w - 208;
        const int mid_w = std::max(120, cov_left - mid_x - 20);
        int start_pos = 0;
        if (g.playing_idx >= 0) {
            for (int p = 0; p < nvis; ++p) {
                if (g.vis_idx[p] == g.playing_idx) { start_pos = (p + 1) % nvis; break; }
            }
        }
        const int cnt = std::min(3, nvis);
        for (int i = 0; i < cnt; ++i) {
            const int ti = g.vis_idx[(start_pos + i) % nvis];
            g.banner_next_idx[i] = ti;
            const int ry = ban.y + 30 + i * 44;
            g.rc_banner_next[i] = Rect{ mid_x - 6, ry, mid_w + 12, 38 };
            const bool rhov = in_rc(mx, my, g.rc_banner_next[i]);
            if (rhov) fill_round_rect_aa(s, g.rc_banner_next[i].x, ry, g.rc_banner_next[i].w, 38, 10, g.C_GLASS_HOV);
            char nb[8]{};
            std::snprintf(nb, sizeof(nb), "%02d", i + 1);
            draw_text_bold(s, f, 13.0f, mid_x, ry + 10, rhov ? g.C_ACCENT : g.C_TXT3, nb);
            draw_text_max_bold(s, f, 13.5f, mid_x + 26, ry + 2, rhov ? g.C_ACCENT : g.C_TXT,
                               g.tracks[ti].title.c_str(), mid_w - 28);
            const char* na = !g.tracks[ti].artist.empty() ? g.tracks[ti].artist.c_str() : g.tracks[ti].fmt;
            draw_text_max_bold(s, f, 11.5f, mid_x + 26, ry + 20, g.C_TXT2, na, mid_w - 28);
        }
    }

    constexpr int cov_sz = 136;
    const int cov_x = ban.x + ban.w - 24 - cov_sz - 34;
    const int cov_y = ban.y + (ban.h - cov_sz) / 2;
    const int disc_cx = cov_x + cov_sz - 14;
    const int disc_cy = ban.y + ban.h / 2;
    constexpr int disc_r = 56;
    fill_circle_a(s, disc_cx, disc_cy, disc_r, COL(18, 22, 28, 255), 235);
    stroke_circle_aa(s, disc_cx, disc_cy, disc_r - 12, 1, COL(255, 255, 255, 255), 26);
    stroke_circle_aa(s, disc_cx, disc_cy, disc_r - 24, 1, COL(255, 255, 255, 255), 20);
    fill_circle_a(s, disc_cx, disc_cy, 18, g.C_ACCENT, 210);
    fill_circle_a(s, disc_cx, disc_cy, 6, COL(18, 22, 28, 255), 240);

    g.rc_hero_cov = Rect{ cov_x, cov_y, cov_sz, cov_sz };
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
    fill_round_rect_shadow(s, cov_x, cov_y + 4, cov_sz, cov_sz, 14, theme_shadow(92));
    if (hc) blit_rgb_round_fit(s, cov_x, cov_y, cov_sz, cov_sz, hc, hsrc_sz, hsrc_sz, 14);
    else    fill_round_rect_aa(s, cov_x, cov_y, cov_sz, cov_sz, 14, g.C_GLASS_ACT);
    stroke_round_rect_aa(s, cov_x, cov_y, cov_sz, cov_sz, 14, 1, g.C_DIV, col_a(g.C_DIV) + 16);
}

void draw_section_bar(int x, int y, int w, int min_vis_y, int max_vis_y, int mx, int my) {
    if (y + SECTION_H < min_vis_y || y > max_vis_y) return;
    Canvas* s = g.screen.get();
    Font* f = g.font.get();
    const bool lt = g.light_t > 0.5f;

    std::string sec_prefix = "全部歌曲";
    if (g.lib_tab == TAB_FAV) sec_prefix = "我喜欢的";
    else if (g.lib_tab == TAB_DIR && g.active_dir >= 0 && g.active_dir < static_cast<int>(g.scan_dirs.size())) {
        sec_prefix = basename_str(g.scan_dirs[g.active_dir]);
        if (sec_prefix.empty()) sec_prefix = "音乐目录";
    }
    char sec_title[96]{};
    std::snprintf(sec_title, sizeof(sec_title), "%s · %d 首", sec_prefix.c_str(), static_cast<int>(g.vis_idx.size()));

    const char* chips[5] = { "全部", "喜欢", "FLAC", "MP3", "其他" };
    int chip_w[5]{};
    int total_cw = 0;
    for (int i = 0; i < 5; ++i) {
        chip_w[i] = text_width_bold(f, 12.5f, chips[i]) + 22;
        total_cw += chip_w[i] + (i > 0 ? 8 : 0);
    }
    const int title_w = text_width_bold(f, 18.0f, sec_title);
    int cx = x + w - total_cw;
    if (cx < x + title_w + 24) cx = x + std::min(w / 2, title_w + 24);
    const int title_max_w = std::max(120, cx - x - 14);
    draw_text_max_bold(s, f, 18.0f, x, y + 10, g.C_TXT, sec_title, title_max_w);
    const int cy = y + 7;
    const bool can_hit = (cy + 30 > min_vis_y && cy < max_vis_y);
    for (int i = 0; i < 5; ++i) {
        if (cx + chip_w[i] > x + w) break;
        const Rect rc{ cx, cy, chip_w[i], 30 };
        if (can_hit) g.rc_filt_chips[i] = rc;
        const bool act = (g.fmt_filter == i);
        const bool hov = can_hit && in_rc(mx, my, rc);
        if (act) {
            fill_frosted_round_rect(s, rc.x, rc.y, rc.w, rc.h, 15, mix_col(g.C_SURFACE, g.C_ACCENT, 0.30f),
                                    202, 1, glass_rim(62, 210));
            stroke_round_rect_aa(s, rc.x, rc.y, rc.w, rc.h, 15, 1, g.C_ACCENT, 145);
            draw_text_centered_bold(s, f, 12.5f, rc.x, rc.x + rc.w, rc.y + 7, g.C_ACCENT, chips[i]);
        } else {
            fill_frosted_round_rect(s, rc.x, rc.y, rc.w, rc.h, 15, g.C_SURFACE,
                                    hov ? 186 : (lt ? 156 : 130), 1, glass_rim(48, 195));
            stroke_round_rect_aa(s, rc.x, rc.y, rc.w, rc.h, 15, 1, g.C_DIV, col_a(g.C_DIV) + 12);
            draw_text_centered_bold(s, f, 12.5f, rc.x, rc.x + rc.w, rc.y + 7, hov ? g.C_TXT : g.C_TXT2, chips[i]);
        }
        cx += chip_w[i] + 8;
    }
}

void draw_grid_view(int mx, int my) {
    Canvas* s = g.screen.get();
    Font* f = g.font.get();
    const int lx = side_w() + 26;
    constexpr int top_y = HEADER_H;
    const int vh = list_vis_h();
    int cols, gap, cw, ch;
    calc_grid_metrics(cols, gap, cw, ch);

    g.ui_list_x0 = lx;
    g.ui_list_x1 = lx + g.grid_w;
    g.ui_list_y0 = top_y;
    g.ui_list_y1 = top_y + vh;

    canvas_set_clip(s, lx - 8, top_y, lx + g.grid_w + 10, top_y + vh);
    const int scroll_y = static_cast<int>(g.scroll_px);
    const int ban_y = top_y + 4 - scroll_y;
    if (ban_y + BANNER_H > top_y - 8 && ban_y < top_y + vh + 8) {
        draw_banner(Rect{ lx, ban_y, g.grid_w, BANNER_H }, mx, my);
    }
    const int sec_y = ban_y + BANNER_H + 14;
    draw_section_bar(lx, sec_y, g.grid_w, top_y, top_y + vh, mx, my);

    const int start_y = top_y + ITEMS_TOP_OFF - scroll_y;
    int next_hov = -1;
    const int nvis = static_cast<int>(g.vis_idx.size());
    const int cov_px = (std::fabs(s->s - 1.0f) < 0.001f)
                       ? cw
                       : std::max(1, static_cast<int>(std::floor(static_cast<float>(cw) * s->s + 0.5f)));
    if (!g.poster_hero.empty() &&
        (g.poster_card_px != cov_px || g.poster_card_cache.size() != static_cast<size_t>(cov_px) * cov_px)) {
        g.poster_card_cache.resize(static_cast<size_t>(cov_px) * cov_px);
        resample_bilinear(g.poster_hero.data(), COVER_SRC_SZ, COVER_SRC_SZ, g.poster_card_cache.data(), cov_px, cov_px);
        g.poster_card_px = cov_px;
    }
    int cov_builds = 0;

    if (nvis == 0) {
        draw_text_centered_bold(s, f, 15.0f, lx, lx + g.grid_w, start_y + 48, g.C_TXT3,
                                "暂无曲目，请从左侧“+”添加本地音乐目录");
    }

    for (int p = 0; p < nvis; ++p) {
        const int row = p / cols;
        const int col = p % cols;
        const int x = lx + col * (cw + gap);
        const int y = start_y + row * (ch + gap);
        if (y + ch < top_y - 16 || y > top_y + vh + 16) continue;

        const int idx = g.vis_idx[p];
        Track& tr = g.tracks[idx];
        const Rect card_rc{ x, y, cw, ch };
        const bool in_view = in_rect(mx, my, lx - 4, top_y, g.grid_w + 8, vh - DOCK_CLEAR + 16);
        const bool is_hov = in_view && in_rc(mx, my, card_rc);
        if (is_hov) next_hov = idx;

        const int lift = (is_hov && g.hov_card_idx == idx) ? static_cast<int>(g.hov_card_t * 4.0f + 0.5f) : 0;
        const int cy = y - lift;
        const bool is_play = (idx == g.playing_idx);

        fill_round_rect_shadow_fast(s, x, cy + 3, cw, cw, 12, theme_shadow(is_play ? 105 : 64));

        const uint32_t* art = nullptr;
        int art_sz = COVER_SRC_SZ;
        if (!tr.thumb.empty()) {
            if (tr.cov_cache_px == cov_px && tr.cov_cache.size() == static_cast<size_t>(cov_px) * cov_px) {
                art = tr.cov_cache.data();
                art_sz = cov_px;
            } else if (cov_builds < 4) {
                tr.cov_cache.resize(static_cast<size_t>(cov_px) * cov_px);
                resample_bilinear(tr.thumb.data(), THUMB_SZ, THUMB_SZ, tr.cov_cache.data(), cov_px, cov_px);
                tr.cov_cache_px = cov_px;
                art = tr.cov_cache.data();
                art_sz = cov_px;
                ++cov_builds;
            } else {
                art = tr.thumb.data();
                art_sz = THUMB_SZ;
            }
        } else if (!g.poster_card_cache.empty()) {
            art = g.poster_card_cache.data();
            art_sz = cov_px;
        } else if (!g.poster_hero.empty()) {
            art = g.poster_hero.data();
            art_sz = COVER_SRC_SZ;
        }
        if (art) blit_rgb_round_fit(s, x, cy, cw, cw, art, art_sz, art_sz, 12);
        else     fill_round_rect_aa(s, x, cy, cw, cw, 12, g.C_GLASS_ACT);

        if (is_play) stroke_round_rect_aa(s, x, cy, cw, cw, 12, 2, g.C_ACCENT, 215);
        else if (is_hov) stroke_round_rect_aa(s, x, cy, cw, cw, 12, 1, g.C_DIV, 90);

        if (tr.fmt_w < 0) tr.fmt_w = text_width_bold(f, 11.5f, tr.fmt);
        if (is_hov || is_play || tr.fmt_w > 0) {
            const int fpw = tr.fmt_w + 14;
            fill_round_rect_aa_a(s, x + 7, cy + 7, fpw, 20, 7, COL(12, 16, 22, 255), 178);
            draw_text_centered_bold(s, f, 11.5f, x + 7, x + 7 + fpw, cy + 9,
                                    is_play ? g.C_ACCENT : COL(244, 248, 255, 255), tr.fmt);
        }

        const Rect fav_draw_rc{ x + cw - 33, cy + 6, 27, 27 };
        const Rect fav_rc{ x + cw - 38, cy, 38, 38 };
        if (tr.fav || is_hov) {
            fill_round_rect_aa_a(s, fav_draw_rc.x, fav_draw_rc.y, 27, 27, 13, COL(12, 16, 22, 255), 162);
            icon_heart_ex(s, fav_draw_rc.x + 6, fav_draw_rc.y + 6, 15,
                          tr.fav ? g.C_HEART : COL(242, 246, 252, 220), tr.fav);
        }

        if (is_play || is_hov) {
            const int bx = x + cw - 39;
            const int by = cy + cw - 39;
            fill_round_rect_shadow_fast(s, bx, by + 2, 32, 32, 16, 86);
            fill_round_rect_aa(s, bx, by, 32, 32, 16, g.C_ACCENT);
            if (is_play && !audio_paused()) {
                for (int b = 0; b < 3; ++b) {
                    const float lvl = clampf(g.bars[b * 2] * 0.8f + 0.25f + 0.20f * std::sin(g.wave_phase * 2.1f + static_cast<float>(b) * 1.3f), 0.15f, 1.0f);
                    const int bh = 5 + static_cast<int>(lvl * 12.0f);
                    fill_round_rect_aa(s, bx + 9 + b * 6, by + 16 - bh / 2, 3, bh, 1, g.C_ON_ACCENT);
                }
            } else {
                icon_play(s, bx + 17, by + 16, 10, g.C_ON_ACCENT);
            }
        }

        const int info_y = cy + cw + 9;
        draw_text_max_bold(s, f, 14.5f, x + 2, info_y, is_play ? g.C_ACCENT : g.C_TXT, tr.title.c_str(), cw - 4);
        if (tr.cached_dur_w < 0 || std::fabs(tr.dur - tr.cached_dur_val) > 0.25) {
            if (tr.dur > 0.5) fmt_time(tr.dur, tr.cached_dur_s, sizeof(tr.cached_dur_s));
            else std::snprintf(tr.cached_dur_s, sizeof(tr.cached_dur_s), "--:--");
            tr.cached_dur_val = tr.dur;
            tr.cached_dur_w = text_width_bold(f, 12.0f, tr.cached_dur_s);
        }
        const int dw = tr.cached_dur_w;
        draw_text_right_bold(s, f, 12.0f, x + cw - 2, info_y + 24, is_play ? g.C_ACCENT : g.C_TXT3, tr.cached_dur_s);
        const char* sub = !tr.artist.empty() ? tr.artist.c_str() : (!tr.album.empty() ? tr.album.c_str() : tr.fmt);
        draw_text_max_bold(s, f, 12.5f, x + 2, info_y + 23, g.C_TXT2, sub, cw - 12 - dw);

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
        const int sbx = g.W - 14;
        const int sb_h = std::max(40, vh - DOCK_CLEAR);
        g.rc_sb_track = Rect{ g.W - 26, top_y + 6, 24, sb_h };
        const int th = g.rc_sb_track.h;
        const int thumb_h = std::min(th, std::max(38, static_cast<int>(static_cast<float>(vh) / static_cast<float>(vh + mscr) * static_cast<float>(th))));
        const int ty = g.rc_sb_track.y + static_cast<int>((g.scroll_px / mscr) * static_cast<float>(th - thumb_h));
        fill_round_rect_aa(s, sbx, g.rc_sb_track.y, 6, th, 3, g.C_TRACK_BG);
        fill_round_rect_aa(s, sbx, ty, 6, thumb_h, 3, g.sb_drag ? g.C_ACCENT : g.C_TXT3);
    }
}

void draw_table_view(int mx, int my) {
    Canvas* s = g.screen.get();
    Font* f = g.font.get();
    const int lx = side_w() + 26;
    const int rx = g.W - 26;
    constexpr int top_y = HEADER_H;
    const int vh = list_vis_h();
    const int tw = std::max(320, rx - lx - 14);
    g.ui_list_x0 = lx; g.ui_list_x1 = lx + tw; g.ui_list_y0 = top_y; g.ui_list_y1 = top_y + vh;

    canvas_set_clip(s, lx - 8, top_y, lx + tw + 10, top_y + vh);
    const int scroll_y = static_cast<int>(g.scroll_px);
    const int ban_y = top_y + 4 - scroll_y;
    if (ban_y + BANNER_H > top_y - 8 && ban_y < top_y + vh + 8) {
        draw_banner(Rect{ lx, ban_y, tw, BANNER_H }, mx, my);
    }
    const int sec_y = ban_y + BANNER_H + 14;
    draw_section_bar(lx, sec_y, tw, top_y, top_y + vh, mx, my);

    const int tbl_hdr_y = top_y + ITEMS_TOP_OFF - scroll_y;
    const int col_art = lx + static_cast<int>(tw * 0.48f);
    const int col_fmt = lx + static_cast<int>(tw * 0.72f);
    if (tbl_hdr_y + 32 > top_y && tbl_hdr_y < top_y + vh) {
        draw_text_bold(s, f, 12.0f, lx + 10, tbl_hdr_y + 7, g.C_TXT3, "#");
        draw_text_bold(s, f, 12.0f, lx + 122, tbl_hdr_y + 7, g.C_TXT3, "歌曲");
        draw_text_bold(s, f, 12.0f, col_art, tbl_hdr_y + 7, g.C_TXT3, "歌手");
        draw_text_bold(s, f, 12.0f, col_fmt, tbl_hdr_y + 7, g.C_TXT3, "格式");
        draw_text_right_bold(s, f, 12.0f, lx + tw - 16, tbl_hdr_y + 7, g.C_TXT3, "时长");
        fill_rect_a(s, lx, tbl_hdr_y + 30, tw, 1, g.C_DIV, col_a(g.C_DIV) + 14);
    }

    const int rows_y0 = tbl_hdr_y + 32;
    const int nvis = static_cast<int>(g.vis_idx.size());
    if (nvis == 0) {
        draw_text_centered_bold(s, f, 15.0f, lx, lx + tw, rows_y0 + 44, g.C_TXT3,
                                "暂无曲目，请从左侧“+”添加本地音乐目录");
    }

    for (int p = 0; p < nvis; ++p) {
        const int y = rows_y0 + p * ROW_H;
        if (y + ROW_H < top_y || y > top_y + vh) continue;
        const int idx = g.vis_idx[p];
        Track& tr = g.tracks[idx];
        const Rect row_rc{ lx, y + 2, tw, ROW_H - 4 };
        const bool in_v = in_rect(mx, my, lx, top_y, tw, vh - DOCK_CLEAR + 16);
        const bool hov = in_v && in_rc(mx, my, row_rc);
        const bool is_play = (idx == g.playing_idx);

        if (is_play) {
            fill_frosted_round_rect(s, row_rc.x, row_rc.y, row_rc.w, row_rc.h, 12,
                                    mix_col(g.C_SURFACE, g.C_ACCENT, 0.22f),
                                    static_cast<uint8_t>(172 + 24 * g.light_t), 2, glass_rim(86, 225));
        } else if (hov) {
            fill_round_rect_aa(s, row_rc.x, row_rc.y, row_rc.w, row_rc.h, 12, g.C_GLASS_HOV);
        }

        char idx_s[12]{};
        std::snprintf(idx_s, sizeof(idx_s), "%02d", p + 1);
        draw_text_bold(s, f, 12.5f, lx + 8, y + 20, is_play ? g.C_ACCENT : g.C_TXT3, idx_s);

        const uint32_t* art = !tr.thumb.empty() ? tr.thumb.data() : (!g.poster_mini.empty() ? g.poster_mini.data() : nullptr);
        const int asz = !tr.thumb.empty() ? THUMB_SZ : MINI_ART_SZ;
        if (art) blit_rgb_round_fit(s, lx + 40, y + 8, 40, 40, art, asz, asz, 10);
        else     fill_round_rect_aa(s, lx + 40, y + 8, 40, 40, 10, g.C_GLASS_ACT);

        const Rect fav_rc{ lx + 84, y + 8, 32, 40 };
        icon_heart_ex(s, lx + 92, y + 20, 15, tr.fav ? g.C_HEART : g.C_TXT3, tr.fav);

        const int title_w = std::max(60, col_art - (lx + 122) - 12);
        draw_text_max_bold(s, f, 14.5f, lx + 122, y + 9, is_play ? g.C_ACCENT : g.C_TXT,
                           tr.title.c_str(), title_w);
        draw_text_max_bold(s, f, 12.0f, lx + 122, y + 31, g.C_TXT3, tr.album.c_str(), title_w);
        draw_text_max_bold(s, f, 13.5f, col_art, y + 19, g.C_TXT2,
                           !tr.artist.empty() ? tr.artist.c_str() : "未知", std::max(40, col_fmt - col_art - 10));
        draw_text_bold(s, f, 12.5f, col_fmt, y + 20, is_play ? g.C_ACCENT : g.C_TXT3, tr.fmt);

        if (tr.cached_dur_w < 0 || std::fabs(tr.dur - tr.cached_dur_val) > 0.25) {
            if (tr.dur > 0.5) fmt_time(tr.dur, tr.cached_dur_s, sizeof(tr.cached_dur_s));
            else std::snprintf(tr.cached_dur_s, sizeof(tr.cached_dur_s), "--:--");
            tr.cached_dur_val = tr.dur;
            tr.cached_dur_w = text_width_bold(f, 13.0f, tr.cached_dur_s);
        }
        draw_text_right_bold(s, f, 13.0f, lx + tw - 16, y + 19, is_play ? g.C_ACCENT : g.C_TXT2, tr.cached_dur_s);

        if (static_cast<int>(g.grid_hits.size()) < MAX_GRID_HITS) {
            Rect hr = row_rc;
            if (hr.y < top_y) { hr.h -= (top_y - hr.y); hr.y = top_y; }
            if (hr.y + hr.h > top_y + vh) hr.h = top_y + vh - hr.y;
            if (hr.h > 0) g.grid_hits.push_back(GridHit{ hr, fav_rc, idx });
        }
    }
    canvas_reset_clip(s);

    const float mscr = max_scroll_px();
    if (mscr > 1.0f) {
        const int sbx = g.W - 14;
        const int sb_h = std::max(40, vh - DOCK_CLEAR);
        g.rc_sb_track = Rect{ g.W - 26, top_y + 6, 24, sb_h };
        const int th = g.rc_sb_track.h;
        const int thumb_h = std::min(th, std::max(38, static_cast<int>(static_cast<float>(vh) / static_cast<float>(vh + mscr) * static_cast<float>(th))));
        const int ty = g.rc_sb_track.y + static_cast<int>((g.scroll_px / mscr) * static_cast<float>(th - thumb_h));
        fill_round_rect_aa(s, sbx, g.rc_sb_track.y, 6, th, 3, g.C_TRACK_BG);
        fill_round_rect_aa(s, sbx, ty, 6, thumb_h, 3, g.sb_drag ? g.C_ACCENT : g.C_TXT3);
    }
}

void draw_player_bar(int mx, int my) {
    Canvas* s = g.screen.get();
    Font* f = g.font.get();
    const bool lt = g.light_t > 0.5f;

    const int bx = side_w() + PLAYER_MARGIN;
    const int bw = std::max(280, g.W - side_w() - PLAYER_MARGIN * 2);
    constexpr int bh = PLAYER_H;
    const int by = g.H - PLAYER_MARGIN - bh;
    constexpr int br = 16;
    g.rc_island = Rect{ bx, by, bw, bh };

    fill_round_rect_shadow(s, bx, by + 4, bw, bh, br, theme_shadow(110));
    const uint32_t bar_bg = mix_col(mix_col(COL(18, 22, 30, 255), COL(246, 249, 255, 255), g.light_t), g.C_HERO_L, 0.20f);
    fill_frosted_round_rect(s, bx, by, bw, bh, br, bar_bg, static_cast<uint8_t>(188 + 22 * g.light_t), 4, glass_rim(88, 235));
    stroke_round_rect_aa(s, bx, by, bw, bh, br, 1,
                         mix_col(g.C_DIV, g.C_ACCENT, g.light_t < 0.6f ? 0.22f : 0.08f), lt ? 130 : 72);

    constexpr int cov_sz = 48;
    g.rc_bar_cov = Rect{ bx + 12, by + (bh - cov_sz) / 2, cov_sz, cov_sz };
    const uint32_t* mc = (g.playing_idx >= 0 && g.cover_ready && !g.cover_mini.empty()) ? g.cover_mini.data()
                         : (!g.poster_mini.empty() ? g.poster_mini.data() : nullptr);
    if (mc) blit_rgb_round_fit(s, g.rc_bar_cov.x, g.rc_bar_cov.y, cov_sz, cov_sz, mc, MINI_ART_SZ, MINI_ART_SZ, 10);
    else    fill_round_rect_aa(s, g.rc_bar_cov.x, g.rc_bar_cov.y, cov_sz, cov_sz, 10, g.C_ACCENT);
    stroke_round_rect_aa(s, g.rc_bar_cov.x, g.rc_bar_cov.y, cov_sz, cov_sz, 10, 1, g.C_DIV, col_a(g.C_DIV) + 12);

    const int mid_x = bx + bw / 2;
    const int btn_y = by + 23;
    g.rc_bar_play = Rect{ mid_x - 19, btn_y - 19, 38, 38 };
    g.rc_bar_prev = Rect{ mid_x - 56, btn_y - 16, 32, 32 };
    g.rc_bar_next = Rect{ mid_x + 24, btn_y - 16, 32, 32 };

    if (in_rc(mx, my, g.rc_bar_prev)) fill_round_rect_aa(s, g.rc_bar_prev.x, g.rc_bar_prev.y, 32, 32, 16, g.C_GLASS_HOV);
    icon_prev(s, mid_x - 40, btn_y, 10, g.C_TXT);

    const bool hov_p = in_rc(mx, my, g.rc_bar_play);
    const bool playing_now = (g.playing_idx >= 0 && !audio_paused());
    fill_ellipse_aa(s, mid_x, btn_y, hov_p ? 18 : 17, hov_p ? 18 : 17, playing_now ? g.C_ACCENT : g.C_PLAY_BTN);
    if (playing_now) icon_pause(s, mid_x, btn_y, 9, g.C_ON_ACCENT);
    else             icon_play(s, mid_x + 1, btn_y, 10, g.C_PLAY_ICO);

    if (in_rc(mx, my, g.rc_bar_next)) fill_round_rect_aa(s, g.rc_bar_next.x, g.rc_bar_next.y, 32, 32, 16, g.C_GLASS_HOV);
    icon_next(s, mid_x + 40, btn_y, 10, g.C_TXT);

    const double dur = g.playing_idx >= 0 ? audio_duration() : 0.0;
    const double pos = g.playing_idx >= 0 ? (g.scrub_drag == 1 ? g.scrub_sec : audio_position()) : 0.0;
    const float frac = (dur > 0.1) ? clampf(static_cast<float>(pos / dur), 0.0f, 1.0f) : 0.0f;
    const int prog_span = std::clamp(bw - 480, 140, 360);
    g.ui_px0 = mid_x - prog_span / 2;
    g.ui_px1 = mid_x + prog_span / 2;
    g.ui_py  = by + bh - 14;
    g.rc_bar_prog = Rect{ g.ui_px0 - 8, g.ui_py - 12, (g.ui_px1 - g.ui_px0) + 16, 24 };

    fill_round_rect_aa(s, g.ui_px0, g.ui_py - 2, prog_span, 4, 2, g.C_TRACK_BG);
    const int fill_w = static_cast<int>(frac * static_cast<float>(prog_span));
    if (fill_w > 0) fill_round_rect_aa(s, g.ui_px0, g.ui_py - 2, fill_w, 4, 2, g.C_ACCENT);
    if (in_rc(mx, my, g.rc_bar_prog) || g.scrub_drag == 1) {
        fill_ellipse_aa(s, g.ui_px0 + fill_w, g.ui_py, 5, 5, g.C_ACCENT);
    }

    const bool show_times = (bw >= 740);
    if (show_times) {
        char pos_s[16]{}, dur_s[16]{};
        fmt_time(pos, pos_s, sizeof(pos_s));
        fmt_time(dur, dur_s, sizeof(dur_s));
        draw_text_right_bold(s, f, 11.0f, g.ui_px0 - 8, g.ui_py - 7, g.C_TXT3, pos_s);
        draw_text_bold(s, f, 11.0f, g.ui_px1 + 8, g.ui_py - 7, g.C_TXT3, dur_s);
    }

    const int left_end = std::min(bx + 290, g.ui_px0 - (show_times ? 48 : 16));
    const int info_x = g.rc_bar_cov.x + cov_sz + 10;
    const int fav_sz = 30;
    if (left_end - info_x >= 44) {
        g.rc_bar_fav = Rect{ left_end - fav_sz, by + (bh - fav_sz) / 2, fav_sz, fav_sz };
        const int is_fav = (g.playing_idx >= 0 && g.tracks[g.playing_idx].fav);
        if (in_rc(mx, my, g.rc_bar_fav)) fill_round_rect_aa(s, g.rc_bar_fav.x, g.rc_bar_fav.y, fav_sz, fav_sz, 15, g.C_GLASS_HOV);
        icon_heart_ex(s, g.rc_bar_fav.x + 7, g.rc_bar_fav.y + 7, 16, is_fav ? g.C_HEART : g.C_TXT2, is_fav);

        const int info_max = g.rc_bar_fav.x - info_x - 8;
        if (info_max > 28) {
            draw_text_max_bold(s, f, 14.0f, info_x, by + 13, g.C_TXT, g.p_title.c_str(), info_max);
            draw_text_max_bold(s, f, 12.0f, info_x, by + 35, g.C_TXT2,
                               !g.p_artist.empty() ? g.p_artist.c_str() : "点击封面进入歌词", info_max);
        }
    }

    const int rx = bx + bw - 14;
    const int cy = by + bh / 2;
    g.rc_bar_lyr = Rect{ rx - 34, cy - 15, 34, 30 };
    const bool act_lyr = (g.lyr && g.lyr->n > 0);
    fill_frosted_round_rect(s, g.rc_bar_lyr.x, g.rc_bar_lyr.y, 34, 30, 15,
                            act_lyr ? mix_col(g.C_SURFACE, g.C_ACCENT, 0.28f) : g.C_SURFACE,
                            act_lyr ? 192 : (in_rc(mx, my, g.rc_bar_lyr) ? 176 : 128), 1,
                            glass_rim(act_lyr ? 90 : 44, act_lyr ? 225 : 185));
    draw_text_centered_bold(s, f, 12.5f, g.rc_bar_lyr.x, g.rc_bar_lyr.x + 34, g.rc_bar_lyr.y + 7,
                            act_lyr ? g.C_ACCENT : g.C_TXT2, "词");

    const char* mode_lbl = g.play_mode == MODE_LOOP_ONE ? "单曲" : (g.play_mode == MODE_SHUFFLE ? "随机" : "顺序");
    g.rc_bar_mode = Rect{ g.rc_bar_lyr.x - 6 - 44, cy - 15, 44, 30 };
    fill_frosted_round_rect(s, g.rc_bar_mode.x, g.rc_bar_mode.y, 44, 30, 15,
                            g.C_SURFACE, in_rc(mx, my, g.rc_bar_mode) ? 176 : 128, 1, glass_rim(44, 185));
    draw_text_centered_bold(s, f, 12.0f, g.rc_bar_mode.x, g.rc_bar_mode.x + 44, g.rc_bar_mode.y + 7, g.C_TXT2, mode_lbl);

    const int center_right = std::max(g.rc_bar_next.x + g.rc_bar_next.w + 12,
                                      g.ui_px1 + (show_times ? 46 : 12));
    if (g.rc_bar_mode.x - center_right >= 92) {
        constexpr int vol_w = 64;
        g.ui_vx1 = g.rc_bar_mode.x - 10;
        g.ui_vx0 = g.ui_vx1 - vol_w;
        g.rc_bar_vol_trk = Rect{ g.ui_vx0 - 6, cy - 15, vol_w + 12, 30 };
        g.rc_bar_vol_ico = Rect{ g.ui_vx0 - 30, cy - 15, 24, 30 };
        const float v = audio_volume();
        icon_vol(s, g.ui_vx0 - 18, cy, 13, g.C_TXT2, v <= 0.01f);
        fill_round_rect_aa(s, g.ui_vx0, cy - 2, vol_w, 4, 2, g.C_TRACK_BG);
        const int vw = static_cast<int>(v * static_cast<float>(vol_w));
        if (vw > 0) fill_round_rect_aa(s, g.ui_vx0, cy - 2, vw, 4, 2, g.C_ACCENT);
        fill_ellipse_aa(s, g.ui_vx0 + vw, cy, 5, 5, g.C_ACCENT);
    }
}

void draw_immersive_view(int mx, int my) {
    draw_frosted_ambient_bg(1);
    Canvas* s = g.screen.get();
    Font* f = g.font.get();
    const bool lt = g.light_t > 0.5f;

    g.rc_back_lib = Rect{ 26, 18, 124, 40 };
    const bool hov_back = in_rc(mx, my, g.rc_back_lib);
    fill_frosted_round_rect(s, g.rc_back_lib.x, g.rc_back_lib.y, g.rc_back_lib.w, g.rc_back_lib.h, 20,
                            g.C_SURFACE, hov_back ? 186 : static_cast<uint8_t>(138 + 34 * g.light_t), 3,
                            glass_rim(hov_back ? 96 : 64, 235));
    icon_back(s, g.rc_back_lib.x + 14, g.rc_back_lib.y + 13, 14, g.C_TXT);
    draw_text_bold(s, f, 13.5f, g.rc_back_lib.x + 36, g.rc_back_lib.y + 10, g.C_TXT, "返回曲库");

    g.rc_lyr_fs = Rect{ g.W - 66, 18, 40, 40 };
    const bool hov_fs = in_rc(mx, my, g.rc_lyr_fs);
    fill_frosted_round_rect(s, g.rc_lyr_fs.x, g.rc_lyr_fs.y, 40, 40, 20,
                            g.C_SURFACE, hov_fs ? 186 : static_cast<uint8_t>(138 + 34 * g.light_t), 3,
                            glass_rim(hov_fs ? 96 : 64, 235));
    icon_expand(s, g.rc_lyr_fs.x + 12, g.rc_lyr_fs.y + 12, 16, g.C_TXT);

    g.rc_lyr_theme = Rect{ g.W - 114, 18, 40, 40 };
    const bool hov_thm = in_rc(mx, my, g.rc_lyr_theme);
    fill_frosted_round_rect(s, g.rc_lyr_theme.x, g.rc_lyr_theme.y, 40, 40, 20,
                            g.C_SURFACE, hov_thm ? 186 : static_cast<uint8_t>(138 + 34 * g.light_t), 3,
                            glass_rim(hov_thm ? 96 : 64, 235));
    icon_theme_orb(s, g.rc_lyr_theme.x + 20, g.rc_lyr_theme.y + 20, 8, mix_col(g.C_TXT, g.C_ACCENT, g.light_t));

    g.rc_lyr_bg = Rect{ g.W - 162, 18, 40, 40 };
    const bool has_bg = !g.bg_img_raw.empty();
    const bool hov_lbg = in_rc(mx, my, g.rc_lyr_bg);
    fill_frosted_round_rect(s, g.rc_lyr_bg.x, g.rc_lyr_bg.y, 40, 40, 20,
                            has_bg ? mix_col(g.C_SURFACE, g.C_ACCENT, 0.24f) : g.C_SURFACE,
                            has_bg ? 194 : (hov_lbg ? 186 : static_cast<uint8_t>(138 + 34 * g.light_t)), 3,
                            glass_rim((has_bg || hov_lbg) ? 96 : 64, 235));
    icon_wallpaper(s, g.rc_lyr_bg.x + 12, g.rc_lyr_bg.y + 12, 16, has_bg ? g.C_ACCENT : g.C_TXT);

    if (has_bg) {
        g.rc_lyr_bg_clear = Rect{ g.W - 210, 18, 40, 40 };
        const bool hov_lcl = in_rc(mx, my, g.rc_lyr_bg_clear);
        fill_frosted_round_rect(s, g.rc_lyr_bg_clear.x, g.rc_lyr_bg_clear.y, 40, 40, 20,
                                g.C_SURFACE, hov_lcl ? 190 : static_cast<uint8_t>(138 + 34 * g.light_t), 3,
                                glass_rim(hov_lcl ? 96 : 64, 235));
        icon_close_x(s, g.rc_lyr_bg_clear.x + 20, g.rc_lyr_bg_clear.y + 20, 4, hov_lcl ? g.C_HEART : g.C_TXT2);
    }

    const int left_col_w = std::clamp(static_cast<int>(g.W * 0.36f), 280, 440);
    constexpr int chrome_below = 194;
    const int max_cov_h = std::max(140, g.H - 72 - 26 - chrome_below);
    const int cov_sz = std::clamp(std::min(max_cov_h, std::min(static_cast<int>(g.H * 0.42f), left_col_w - 40)), 140, 340);
    const int block_h = cov_sz + chrome_below;
    const int cov_x = 40 + (left_col_w - cov_sz) / 2;
    const int cov_y = std::max(72, 72 + (g.H - 96 - block_h) / 2);

    fill_round_rect_shadow(s, cov_x, cov_y + 10, cov_sz, cov_sz, 14, theme_shadow(130));
    const uint32_t* art = (g.playing_idx >= 0 && g.cover_ready && !g.cover_src.empty()) ? g.cover_src.data()
                          : (!g.poster_hero.empty() ? g.poster_hero.data() : nullptr);
    if (art) blit_rgb_round_fit(s, cov_x, cov_y, cov_sz, cov_sz, art, COVER_SRC_SZ, COVER_SRC_SZ, 14);
    else     fill_round_rect_aa(s, cov_x, cov_y, cov_sz, cov_sz, 14, g.C_GLASS_ACT);
    stroke_round_rect_aa(s, cov_x, cov_y, cov_sz, cov_sz, 14, 1, g.C_DIV, col_a(g.C_DIV) + 14);

    const int info_x = cov_x;
    const int info_w = cov_sz;
    const int ty = cov_y + cov_sz + 18;
    g.rc_bar_fav = Rect{ info_x + info_w - 32, ty - 2, 32, 32 };
    const int is_fav = (g.playing_idx >= 0 && g.tracks[g.playing_idx].fav);
    if (in_rc(mx, my, g.rc_bar_fav)) fill_round_rect_aa(s, g.rc_bar_fav.x, g.rc_bar_fav.y, 32, 32, 16, g.C_GLASS_HOV);
    icon_heart_ex(s, g.rc_bar_fav.x + 7, g.rc_bar_fav.y + 7, 18, is_fav ? g.C_HEART : g.C_TXT2, is_fav);

    draw_text_max_bold(s, f, 22.0f, info_x, ty, g.C_TXT, g.p_title.c_str(), info_w - 38);
    draw_text_max_bold(s, f, 14.5f, info_x, ty + 30, g.C_TXT2,
                       !g.p_artist.empty() ? g.p_artist.c_str() : (!g.p_album.empty() ? g.p_album.c_str() : "Sir\xC3\xA8ne Hi-Res"), info_w);

    const double dur = g.playing_idx >= 0 ? audio_duration() : 0.0;
    const double pos = g.playing_idx >= 0 ? (g.scrub_drag == 1 ? g.scrub_sec : audio_position()) : 0.0;
    const float frac = (dur > 0.1) ? clampf(static_cast<float>(pos / dur), 0.0f, 1.0f) : 0.0f;
    const int py = ty + 68;
    g.ui_px0 = info_x;
    g.ui_px1 = info_x + info_w;
    g.ui_py = py;
    g.rc_bar_prog = Rect{ g.ui_px0 - 8, py - 14, info_w + 16, 28 };
    fill_round_rect_aa(s, g.ui_px0, py - 2, info_w, 5, 2, g.C_TRACK_BG);
    const int fpw = static_cast<int>(frac * static_cast<float>(info_w));
    if (fpw > 0) fill_round_rect_aa(s, g.ui_px0, py - 2, fpw, 5, 2, g.C_ACCENT);
    fill_ellipse_aa(s, g.ui_px0 + fpw, py, (in_rc(mx, my, g.rc_bar_prog) || g.scrub_drag == 1) ? 7 : 5,
                    (in_rc(mx, my, g.rc_bar_prog) || g.scrub_drag == 1) ? 7 : 5, g.C_ACCENT);

    char bpos[20]{}, bdur[20]{};
    fmt_time(pos, bpos, sizeof(bpos));
    fmt_time(dur, bdur, sizeof(bdur));
    draw_text_bold(s, f, 12.0f, g.ui_px0, py + 11, g.C_TXT3, bpos);
    draw_text_right_bold(s, f, 12.0f, g.ui_px1, py + 11, g.C_TXT3, bdur);

    const int cy = py + 48;
    const int cx = info_x + info_w / 2;
    g.rc_bar_play = Rect{ cx - 23, cy - 23, 46, 46 };
    g.rc_bar_prev = Rect{ cx - 74, cy - 18, 36, 36 };
    g.rc_bar_next = Rect{ cx + 38, cy - 18, 36, 36 };

    if (in_rc(mx, my, g.rc_bar_prev)) fill_round_rect_aa(s, g.rc_bar_prev.x, g.rc_bar_prev.y, 36, 36, 18, g.C_GLASS_HOV);
    icon_prev(s, cx - 56, cy, 13, g.C_TXT);

    const bool hov_p = in_rc(mx, my, g.rc_bar_play);
    fill_ellipse_aa(s, cx, cy, hov_p ? 23 : 22, hov_p ? 23 : 22, g.C_ACCENT);
    if (g.playing_idx >= 0 && !audio_paused()) icon_pause(s, cx, cy, 11, g.C_ON_ACCENT);
    else                                       icon_play(s, cx + 2, cy, 12, g.C_ON_ACCENT);

    if (in_rc(mx, my, g.rc_bar_next)) fill_round_rect_aa(s, g.rc_bar_next.x, g.rc_bar_next.y, 36, 36, 18, g.C_GLASS_HOV);
    icon_next(s, cx + 56, cy, 13, g.C_TXT);

    const int vy = cy + 40;
    char spec[64]{};
    snprintf(spec, sizeof(spec), "%s  %ukHz", g.p_fmt, audio_sample_rate() / 1000u);
    const int spw = text_width_bold(f, 11.5f, spec) + 18;
    fill_frosted_round_rect(s, info_x, vy - 13, spw, 26, 13,
                            g.C_SURFACE, lt ? 168 : 138, 1, glass_rim(44, 185));
    draw_text_centered_bold(s, f, 11.5f, info_x, info_x + spw, vy - 7, g.C_ACCENT, spec);

    const char* ml = g.play_mode == MODE_LOOP_ONE ? "单曲" : (g.play_mode == MODE_SHUFFLE ? "随机" : "顺序");
    g.rc_bar_mode = Rect{ info_x + info_w - 50, vy - 13, 50, 26 };
    const bool hov_m = in_rc(mx, my, g.rc_bar_mode);
    fill_frosted_round_rect(s, g.rc_bar_mode.x, g.rc_bar_mode.y, 50, 26, 13,
                            g.C_SURFACE, hov_m ? 182 : (lt ? 156 : 132), 1, glass_rim(48, 200));
    draw_text_centered_bold(s, f, 12.0f, g.rc_bar_mode.x, g.rc_bar_mode.x + 50, vy - 7, g.C_TXT2, ml);

    if (g.rc_bar_mode.x - (info_x + spw) >= 88) {
        const int vw_tot = std::min(72, g.rc_bar_mode.x - (info_x + spw) - 34);
        g.ui_vx1 = g.rc_bar_mode.x - 10;
        g.ui_vx0 = g.ui_vx1 - vw_tot;
        g.rc_bar_vol_trk = Rect{ g.ui_vx0 - 6, vy - 14, vw_tot + 12, 28 };
        g.rc_bar_vol_ico = Rect{ g.ui_vx0 - 28, vy - 14, 22, 28 };
        const float vol = audio_volume();
        icon_vol(s, g.ui_vx0 - 17, vy, 12, g.C_TXT2, vol <= 0.01f);
        fill_round_rect_aa(s, g.ui_vx0, vy - 2, vw_tot, 4, 2, g.C_TRACK_BG);
        const int vfill = static_cast<int>(vol * static_cast<float>(vw_tot));
        if (vfill > 0) fill_round_rect_aa(s, g.ui_vx0, vy - 2, vfill, 4, 2, g.C_ACCENT);
        fill_ellipse_aa(s, g.ui_vx0 + vfill, vy, 5, 5, g.C_ACCENT);
    }

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

void draw_switch(int x, int y, float t, uint32_t acc, int mx, int my) {
    Canvas* s = g.screen.get();
    constexpr int w = 44, h = 24;
    const Rect rc{ x, y, w, h };
    const uint32_t track = mix_col(g.C_TRACK_BG, acc, clampf(t, 0.0f, 1.0f));
    fill_round_rect_aa(s, x, y, w, h, 12, track);
    if (in_rc(mx, my, rc)) stroke_round_rect_aa(s, x, y, w, h, 12, 1, acc, 140);
    const int cx = x + 12 + static_cast<int>(clampf(t, 0.0f, 1.0f) * 20.0f + 0.5f);
    fill_ellipse_aa(s, cx, y + 12, 9, 9, COL(255, 255, 255, 255));
}

void draw_segmented(int x, int y, const char* const* opts, int n, int active_idx, int item_w,
                    int mx, int my, const int* hit_ids, bool can_hit) {
    Canvas* s = g.screen.get();
    Font* f = g.font.get();
    const bool lt = g.light_t > 0.5f;
    const int tot_w = n * item_w + 6;
    fill_frosted_round_rect(s, x, y, tot_w, 32, 16, g.C_SURFACE, lt ? 148 : 118, 1, glass_rim(42, 185));
    for (int i = 0; i < n; ++i) {
        const Rect ir{ x + 3 + i * item_w, y + 3, item_w, 26 };
        if (can_hit) g.set_hits.push_back(SetHit{ ir, hit_ids[i], i });
        const bool act = (i == active_idx);
        const bool hov = can_hit && in_rc(mx, my, ir);
        if (act) {
            fill_round_rect_aa(s, ir.x, ir.y, ir.w, ir.h, 13, mix_col(g.C_SURFACE, g.C_ACCENT, 0.30f));
            stroke_round_rect_aa(s, ir.x, ir.y, ir.w, ir.h, 13, 1, g.C_ACCENT, 145);
        } else if (hov) {
            fill_round_rect_aa(s, ir.x, ir.y, ir.w, ir.h, 13, g.C_GLASS_HOV);
        }
        draw_text_centered_bold(s, f, 12.5f, ir.x, ir.x + ir.w, ir.y + 5,
                                act ? g.C_ACCENT : (hov ? g.C_TXT : g.C_TXT2), opts[i]);
    }
}

void draw_settings_view(int mx, int my) {
    Canvas* s = g.screen.get();
    Font* f = g.font.get();
    const bool lt = g.light_t > 0.5f;
    const int lx = side_w() + 26;
    const int rx = g.W - 26;

    g.rc_set_back = Rect{ lx, 22, 88, 38 };
    const bool hov_back = in_rc(mx, my, g.rc_set_back);
    fill_frosted_round_rect(s, g.rc_set_back.x, g.rc_set_back.y, g.rc_set_back.w, g.rc_set_back.h, 19,
                            g.C_SURFACE, hov_back ? 188 : (lt ? 156 : 132), 2, glass_rim(56, 210));
    stroke_round_rect_aa(s, g.rc_set_back.x, g.rc_set_back.y, g.rc_set_back.w, g.rc_set_back.h, 19, 1,
                         g.C_DIV, col_a(g.C_DIV) + 12);
    icon_back(s, g.rc_set_back.x + 14, g.rc_set_back.y + 12, 13, g.C_TXT);
    draw_text_bold(s, f, 13.5f, g.rc_set_back.x + 34, g.rc_set_back.y + 10, g.C_TXT, "返回");
    draw_text_bold(s, f, 23.5f, lx + 104, 26, g.C_TXT, "设置");

    constexpr int top_y = HEADER_H;
    const int vh = list_vis_h();
    const int avail_w = std::max(320, rx - lx - 14);
    const int card_w = std::min(780, avail_w);
    const int cx0 = lx + (avail_w - card_w) / 2;

    g.ui_list_x0 = cx0; g.ui_list_x1 = cx0 + card_w; g.ui_list_y0 = top_y; g.ui_list_y1 = top_y + vh;
    canvas_set_clip(s, lx - 8, top_y, rx, top_y + vh);

    const int start_y = top_y + 8 - static_cast<int>(g.scroll_px);
    int y = start_y;

    auto draw_card_box = [&](int cy, int ch) {
        fill_round_rect_shadow(s, cx0, cy + 3, card_w, ch, 16, theme_shadow(58));
        fill_frosted_round_rect(s, cx0, cy, card_w, ch, 16, g.C_SURFACE,
                                lt ? 186 : 162, 3, glass_rim(54, 210));
        stroke_round_rect_aa(s, cx0, cy, card_w, ch, 16, 1, g.C_DIV, col_a(g.C_DIV) + 14);
    };
    auto vis_hit = [&](const Rect& r) { return r.y + r.h > top_y && r.y < top_y + vh; };

    draw_text_bold(s, f, 13.0f, cx0 + 8, y, g.C_TXT3, "外观");
    y += 24;
    {
        constexpr int row_h = 62;
        constexpr int card_h = row_h * 3;
        draw_card_box(y, card_h);

        int ry = y;
        draw_text_bold(s, f, 15.0f, cx0 + 20, ry + 12, g.C_TXT, "界面主题");
        draw_text_max(s, f, 12.5f, cx0 + 20, ry + 36, g.C_TXT2, "切换浅色或深色玻璃外观", card_w - 220);
        const char* th_opts[2] = { "浅色", "深色" };
        const int th_ids[2] = { SET_THEME_LIGHT, SET_THEME_DARK };
        draw_segmented(cx0 + card_w - 154, ry + 15, th_opts, 2, g.is_light ? 0 : 1, 64, mx, my, th_ids,
                       vis_hit(Rect{ cx0, ry, card_w, row_h }));
        fill_rect_a(s, cx0 + 18, ry + row_h - 1, card_w - 36, 1, g.C_DIV, col_a(g.C_DIV) + 12);

        ry += row_h;
        draw_text_bold(s, f, 15.0f, cx0 + 20, ry + 12, g.C_TXT, "主题色跟随封面");
        draw_text_max(s, f, 12.5f, cx0 + 20, ry + 36, g.C_TXT2, "按钮与高亮取当前播放封面配色，关闭后使用默认蓝", card_w - 110);
        const Rect sw_rc{ cx0 + card_w - 64, ry + 19, 44, 24 };
        draw_switch(sw_rc.x, sw_rc.y, g.accent_sw_t, g.C_ACCENT, mx, my);
        if (vis_hit(sw_rc)) g.set_hits.push_back(SetHit{ Rect{ sw_rc.x - 8, ry + 10, 60, 42 }, SET_ACCENT_FOLLOW, 0 });
        fill_rect_a(s, cx0 + 18, ry + row_h - 1, card_w - 36, 1, g.C_DIV, col_a(g.C_DIV) + 12);

        ry += row_h;
        draw_text_bold(s, f, 15.0f, cx0 + 20, ry + 12, g.C_TXT, "自定义背景图片");
        const bool has_bg = !g.bg_img_raw.empty();
        draw_text_max(s, f, 12.5f, cx0 + 20, ry + 36, g.C_TXT2,
                      has_bg && !g.bg_img_path.empty() ? g.bg_img_path.c_str() : "支持 PNG / JPG，作为窗口毛玻璃背景",
                      card_w - 220);
        int bx_r = cx0 + card_w - 20;
        if (has_bg) {
            const Rect cl_rc{ bx_r - 64, ry + 15, 64, 32 };
            const bool hov = vis_hit(cl_rc) && in_rc(mx, my, cl_rc);
            fill_frosted_round_rect(s, cl_rc.x, cl_rc.y, cl_rc.w, cl_rc.h, 16, g.C_SURFACE,
                                    hov ? 188 : (lt ? 156 : 132), 1, glass_rim(48, 200));
            draw_text_centered_bold(s, f, 12.5f, cl_rc.x, cl_rc.x + cl_rc.w, cl_rc.y + 8,
                                    hov ? g.C_HEART : g.C_TXT2, "清除");
            if (vis_hit(cl_rc)) g.set_hits.push_back(SetHit{ cl_rc, SET_BG_CLEAR, 0 });
            bx_r = cl_rc.x - 8;
        }
        {
            const Rect pk_rc{ bx_r - 92, ry + 15, 92, 32 };
            const bool hov = vis_hit(pk_rc) && in_rc(mx, my, pk_rc);
            fill_frosted_round_rect(s, pk_rc.x, pk_rc.y, pk_rc.w, pk_rc.h, 16,
                                    mix_col(g.C_SURFACE, g.C_ACCENT, 0.22f),
                                    hov ? 204 : 184, 1, glass_rim(56, 210));
            draw_text_centered_bold(s, f, 12.5f, pk_rc.x, pk_rc.x + pk_rc.w, pk_rc.y + 8, g.C_ACCENT, "选择图片");
            if (vis_hit(pk_rc)) g.set_hits.push_back(SetHit{ pk_rc, SET_BG_PICK, 0 });
        }
        y += card_h + 22;
    }

    draw_text_bold(s, f, 13.0f, cx0 + 8, y, g.C_TXT3, "播放");
    y += 24;
    {
        constexpr int row_h = 62;
        draw_card_box(y, row_h);
        draw_text_bold(s, f, 15.0f, cx0 + 20, y + 12, g.C_TXT, "播放模式");
        draw_text_max(s, f, 12.5f, cx0 + 20, y + 36, g.C_TXT2, "切换队列顺序、单曲循环或随机播放", card_w - 230);
        const char* pm_opts[3] = { "顺序", "单曲", "随机" };
        const int pm_ids[3] = { SET_PLAY_MODE, SET_PLAY_MODE, SET_PLAY_MODE };
        draw_segmented(cx0 + card_w - 194, y + 15, pm_opts, 3, g.play_mode % 3, 56, mx, my, pm_ids,
                       vis_hit(Rect{ cx0, y, card_w, row_h }));
        y += row_h + 22;
    }

    draw_text_bold(s, f, 13.0f, cx0 + 8, y, g.C_TXT3, "曲库");
    y += 24;
    {
        const int nd = static_cast<int>(g.scan_dirs.size());
        constexpr int hdr_h = 62;
        constexpr int dir_h = 46;
        const int list_h = std::max(1, nd) * dir_h + 8;
        const int card_h = hdr_h + list_h;
        draw_card_box(y, card_h);

        char lib_sub[64]{};
        std::snprintf(lib_sub, sizeof(lib_sub), "已添加 %d 个目录  ·  共 %d 首曲目", nd, static_cast<int>(g.tracks.size()));
        draw_text_bold(s, f, 15.0f, cx0 + 20, y + 12, g.C_TXT, "本地音乐目录");
        draw_text_max(s, f, 12.5f, cx0 + 20, y + 36, g.C_TXT2, lib_sub, card_w - 230);

        const Rect rs_rc{ cx0 + card_w - 20 - 82, y + 15, 82, 32 };
        const Rect ad_rc{ rs_rc.x - 8 - 88, y + 15, 88, 32 };
        {
            const bool hov = vis_hit(rs_rc) && in_rc(mx, my, rs_rc);
            fill_frosted_round_rect(s, rs_rc.x, rs_rc.y, rs_rc.w, rs_rc.h, 16, g.C_SURFACE,
                                    hov ? 188 : (lt ? 156 : 132), 1, glass_rim(48, 200));
            draw_text_centered_bold(s, f, 12.5f, rs_rc.x, rs_rc.x + rs_rc.w, rs_rc.y + 8, g.C_TXT2, "刷新扫描");
            if (vis_hit(rs_rc)) g.set_hits.push_back(SetHit{ rs_rc, SET_RESCAN, 0 });
        }
        {
            const bool hov = vis_hit(ad_rc) && in_rc(mx, my, ad_rc);
            fill_frosted_round_rect(s, ad_rc.x, ad_rc.y, ad_rc.w, ad_rc.h, 16,
                                    mix_col(g.C_SURFACE, g.C_ACCENT, 0.22f),
                                    hov ? 204 : 184, 1, glass_rim(56, 210));
            draw_text_centered_bold(s, f, 12.5f, ad_rc.x, ad_rc.x + ad_rc.w, ad_rc.y + 8, g.C_ACCENT, "添加目录");
            if (vis_hit(ad_rc)) g.set_hits.push_back(SetHit{ ad_rc, SET_ADD_DIR, 0 });
        }

        fill_rect_a(s, cx0 + 18, y + hdr_h - 1, card_w - 36, 1, g.C_DIV, col_a(g.C_DIV) + 12);

        int dy = y + hdr_h + 4;
        if (nd == 0) {
            draw_text_bold(s, f, 13.0f, cx0 + 20, dy + 14, g.C_TXT3, "尚未添加任何目录，点击右上角“添加目录”即可导入歌曲");
        } else {
            for (int di = 0; di < nd; ++di) {
                icon_folder(s, cx0 + 22, dy + 16, 15, g.C_TXT2);
                draw_text_max_bold(s, f, 13.0f, cx0 + 44, dy + 14, g.C_TXT, g.scan_dirs[di].c_str(), card_w - 134);
                const Rect rm_rc{ cx0 + card_w - 78, dy + 9, 58, 28 };
                const bool hov = vis_hit(rm_rc) && in_rc(mx, my, rm_rc);
                fill_frosted_round_rect(s, rm_rc.x, rm_rc.y, rm_rc.w, rm_rc.h, 14, g.C_SURFACE,
                                        hov ? 188 : (lt ? 150 : 126), 1, glass_rim(44, 190));
                draw_text_centered_bold(s, f, 12.0f, rm_rc.x, rm_rc.x + rm_rc.w, rm_rc.y + 6,
                                        hov ? g.C_HEART : g.C_TXT2, "移除");
                if (vis_hit(rm_rc)) g.set_hits.push_back(SetHit{ rm_rc, SET_DIR_REMOVE, di });
                dy += dir_h;
            }
        }
        y += card_h + 22;
    }

    draw_text_bold(s, f, 13.0f, cx0 + 8, y, g.C_TXT3, "关于与诊断");
    y += 24;
    {
        constexpr int card_h = 126;
        draw_card_box(y, card_h);
        draw_text_bold(s, f, 15.0f, cx0 + 20, y + 16, g.C_TXT, "Sir\xC3\xA8ne v1.2.6  ·  本地无损音频播放器");
        char adiag[256]{};
        audio_get_diag(adiag, sizeof(adiag));
        for (int i = 0; adiag[i]; ++i) {
            if (adiag[i] == '\n' || adiag[i] == '\r' || adiag[i] == '\t') adiag[i] = ' ';
        }
        draw_text_max(s, f, 12.5f, cx0 + 20, y + 42, g.C_TXT2, adiag, card_w - 40);
        draw_text_max(s, f, 12.5f, cx0 + 20, y + 66, g.C_TXT2, lyrics_last_diag(), card_w - 40);
        char sc[128]{};
        std::snprintf(sc, sizeof(sc), "DPI 缩放 %.2fx  |  窗口 %dx%d  |  仅限个人学习与音频界面技术研究",
                      g.ui_scale, g.W, g.H);
        draw_text_max(s, f, 12.0f, cx0 + 20, y + 92, g.C_TXT3, sc, card_w - 40);
        y += card_h + 12;
    }

    g.set_content_h = static_cast<float>(y - start_y);
    canvas_reset_clip(s);

    const float mscr = max_scroll_px();
    if (mscr > 1.0f) {
        const int sbx = g.W - 14;
        const int sb_h = std::max(40, vh - DOCK_CLEAR);
        g.rc_sb_track = Rect{ g.W - 26, top_y + 6, 24, sb_h };
        const int th = g.rc_sb_track.h;
        const int thumb_h = std::min(th, std::max(38, static_cast<int>(static_cast<float>(vh) / static_cast<float>(vh + mscr) * static_cast<float>(th))));
        const int ty = g.rc_sb_track.y + static_cast<int>((g.scroll_px / mscr) * static_cast<float>(th - thumb_h));
        fill_round_rect_aa(s, sbx, g.rc_sb_track.y, 6, th, 3, g.C_TRACK_BG);
        fill_round_rect_aa(s, sbx, ty, 6, thumb_h, 3, g.sb_drag ? g.C_ACCENT : g.C_TXT3);
    }
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
        if (in_rc(mx, my, g.rc_search_box) || in_rc(mx, my, g.rc_search_clear) ||
            in_rc(mx, my, g.rc_nav_all) || in_rc(mx, my, g.rc_nav_fav) || in_rc(mx, my, g.rc_nav_lyr) ||
            in_rc(mx, my, g.rc_nav_rescan) || in_rc(mx, my, g.rc_nav_set) || in_rc(mx, my, g.rc_nav_add) ||
            in_rc(mx, my, g.rc_nav_bg) || in_rc(mx, my, g.rc_nav_theme) || in_rc(mx, my, g.rc_sb_track)) {
            hand = true;
        }
        for (const auto& dr : g.rc_dir_rows) if (!hand && in_rc(mx, my, dr.rc)) hand = true;
        if (!hand && g.page == PAGE_SETTINGS) {
            if (in_rc(mx, my, g.rc_set_back)) hand = true;
            for (const auto& sh : g.set_hits) if (!hand && in_rc(mx, my, sh.rc)) hand = true;
        } else if (!hand) {
            if (in_rc(mx, my, g.rc_sort_btn) || in_rc(mx, my, g.rc_view_pill) || in_rc(mx, my, g.rc_bg_btn) ||
                in_rc(mx, my, g.rc_bg_clear) || in_rc(mx, my, g.rc_fs_btn) ||
                in_rc(mx, my, g.rc_hero_play) || in_rc(mx, my, g.rc_hero_cov)) {
                hand = true;
            }
            for (const auto& bn : g.rc_banner_next) if (!hand && in_rc(mx, my, bn)) hand = true;
            for (const auto& fc : g.rc_filt_chips) if (!hand && in_rc(mx, my, fc)) hand = true;
            for (const auto& gh : g.grid_hits) if (!hand && in_rc(mx, my, gh.card)) hand = true;
        }
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
    if (!g.search_q.empty() && g.search_focused && g.page != PAGE_LIBRARY) {
        g.page = PAGE_LIBRARY;
    }

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
    } else if (hk == 'd') {
        capture_fade();
        g.page = (g.page == PAGE_SETTINGS) ? PAGE_LIBRARY : PAGE_SETTINGS;
        g.scroll_px = g.scroll_target = g.scroll_vel = 0.0f;
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

        if (g.page != PAGE_LYRICS) {
            if (in_rc(ti.x, ti.y, g.rc_search_clear) && !g.search_q.empty()) {
                g.search_q.clear();
                rebuild_visible();
                g.scroll_px = g.scroll_target = 0.0f;
                return;
            }
            if (in_rc(ti.x, ti.y, g.rc_search_box)) {
                if (g.page != PAGE_LIBRARY) { capture_fade(); g.page = PAGE_LIBRARY; }
                g.search_focused = 1;
                be_text_input(1);
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
            g.page = (g.page == PAGE_LYRICS) ? PAGE_LIBRARY : PAGE_LYRICS;
            return;
        }
        if (g.page != PAGE_LYRICS && in_rc(ti.x, ti.y, g.rc_island)) return;

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

        if (in_rc(ti.x, ti.y, g.rc_nav_all)) {
            if (g.page != PAGE_LIBRARY) { capture_fade(); g.page = PAGE_LIBRARY; }
            g.lib_tab = TAB_ALL; g.active_dir = -1; g.fmt_filter = FMT_FILT_ALL;
            rebuild_visible(); g.scroll_px = g.scroll_target = g.scroll_vel = 0.0f; return;
        }
        if (in_rc(ti.x, ti.y, g.rc_nav_fav)) {
            if (g.page != PAGE_LIBRARY) { capture_fade(); g.page = PAGE_LIBRARY; }
            g.lib_tab = TAB_FAV; rebuild_visible(); g.scroll_px = g.scroll_target = g.scroll_vel = 0.0f; return;
        }
        if (in_rc(ti.x, ti.y, g.rc_nav_lyr)) { capture_fade(); g.page = PAGE_LYRICS; return; }
        if (in_rc(ti.x, ti.y, g.rc_nav_rescan)) { scan_library(1); return; }
        if (in_rc(ti.x, ti.y, g.rc_nav_set)) {
            capture_fade();
            g.page = (g.page == PAGE_SETTINGS) ? PAGE_LIBRARY : PAGE_SETTINGS;
            g.scroll_px = g.scroll_target = g.scroll_vel = 0.0f; return;
        }
        if (in_rc(ti.x, ti.y, g.rc_nav_add)) { open_folder_dialog(); return; }
        for (const auto& dr : g.rc_dir_rows) {
            if (in_rc(ti.x, ti.y, dr.rc)) {
                if (g.page != PAGE_LIBRARY) { capture_fade(); g.page = PAGE_LIBRARY; }
                g.lib_tab = TAB_DIR; g.active_dir = dr.dir_idx;
                rebuild_visible(); g.scroll_px = g.scroll_target = g.scroll_vel = 0.0f; return;
            }
        }
        if (in_rc(ti.x, ti.y, g.rc_nav_bg))    { pick_bg_image_action(); return; }
        if (in_rc(ti.x, ti.y, g.rc_nav_theme)) { toggle_theme_smooth(); return; }

        if (g.page == PAGE_SETTINGS) {
            if (in_rc(ti.x, ti.y, g.rc_set_back)) {
                capture_fade(); g.page = PAGE_LIBRARY; g.scroll_px = g.scroll_target = g.scroll_vel = 0.0f; return;
            }
            for (const auto& sh : g.set_hits) {
                if (!in_rc(ti.x, ti.y, sh.rc)) continue;
                switch (sh.id) {
                case SET_THEME_LIGHT:
                    if (!g.is_light) toggle_theme_smooth();
                    return;
                case SET_THEME_DARK:
                    if (g.is_light) toggle_theme_smooth();
                    return;
                case SET_ACCENT_FOLLOW:
                    g.accent_follow = !g.accent_follow;
                    save_settings();
                    step_palette_and_ambient();
                    show_toast(g.accent_follow ? "主题色已设为跟随封面" : "已切换为默认主题色");
                    return;
                case SET_BG_PICK:
                    pick_bg_image_action();
                    return;
                case SET_BG_CLEAR:
                    clear_bg_image();
                    return;
                case SET_PLAY_MODE:
                    g.play_mode = sh.arg % 3;
                    save_settings();
                    show_toast(g.play_mode == MODE_LOOP_ONE ? "单曲循环" : (g.play_mode == MODE_SHUFFLE ? "随机播放" : "顺序循环"));
                    return;
                case SET_ADD_DIR:
                    open_folder_dialog();
                    return;
                case SET_RESCAN:
                    scan_library(1);
                    return;
                case SET_DIR_REMOVE:
                    if (sh.arg >= 0 && sh.arg < static_cast<int>(g.scan_dirs.size())) {
                        g.scan_dirs.erase(g.scan_dirs.begin() + sh.arg);
                        if (g.lib_tab == TAB_DIR) { g.lib_tab = TAB_ALL; g.active_dir = -1; }
                        else if (g.active_dir > sh.arg) --g.active_dir;
                        else if (g.active_dir == sh.arg) g.active_dir = -1;
                        save_dirs_cfg();
                        scan_library(1);
                    }
                    return;
                default: break;
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
                g.drag_mode = DRAG_LIST;
                g.drag_start_x = ti.x;
                g.drag_start_y = ti.y;
                g.drag_moved = 0;
                g.drag_cand_track = -1;
                g.drag_cand_fav = 0;
                g.drag_cand_hero = 0;
                g.scroll_target = g.scroll_px;
            }
            return;
        }

        if (in_rc(ti.x, ti.y, g.rc_bg_clear))  { clear_bg_image(); return; }
        if (in_rc(ti.x, ti.y, g.rc_bg_btn))    { pick_bg_image_action(); return; }
        if (in_rc(ti.x, ti.y, g.rc_sort_btn))  { g.sort_mode = (g.sort_mode + 1) % 4; rebuild_visible(); save_settings(); return; }
        if (in_rc(ti.x, ti.y, g.rc_view_pill)) {
            capture_fade(); g.view_grid = !g.view_grid; g.scroll_px = g.scroll_target = g.scroll_vel = 0.0f; save_settings(); return;
        }
        if (in_rc(ti.x, ti.y, g.rc_fs_btn))    { be_toggle_fullscreen(); return; }
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
            if (in_rc(ti.x, ti.y, g.rc_hero_play)) { toggle_play_pause_action(); return; }

            g.drag_mode = DRAG_LIST;
            g.drag_start_x = ti.x;
            g.drag_start_y = ti.y;
            g.drag_moved = 0;
            g.drag_cand_track = -1;
            g.drag_cand_fav = 0;
            g.drag_cand_hero = 0;
            g.scroll_target = g.scroll_px;

            for (int i = 0; i < 3; ++i) {
                if (g.banner_next_idx[i] >= 0 && in_rc(ti.x, ti.y, g.rc_banner_next[i])) {
                    g.drag_cand_track = g.banner_next_idx[i];
                    return;
                }
            }
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
        } else if (g.vol_drag && g.ui_vx1 > g.ui_vx0) {
            const float f = clampf(static_cast<float>(ti.x - g.ui_vx0) / static_cast<float>(g.ui_vx1 - g.ui_vx0), 0.0f, 1.0f);
            audio_set_volume(f);
            if (f > 0.02f) g.saved_vol = f;
        } else if (g.sb_drag && g.rc_sb_track.h > 10) {
            const float f = clampf(static_cast<float>(ti.y - g.rc_sb_track.y) / static_cast<float>(g.rc_sb_track.h), 0.0f, 1.0f);
            g.scroll_target = g.scroll_px = f * max_scroll_px();
        } else if (g.drag_mode == DRAG_LIST) {
            const int thr = be_input_is_touch() ? 10 : 8;
            if (!g.drag_moved && (std::abs(ti.y - g.drag_start_y) > thr || std::abs(ti.x - g.drag_start_x) > thr)) {
                g.drag_moved = 1;
                g.scroll_rest_timer = 0.0f;
            }
            if (g.drag_moved && max_scroll_px() > 0.0f) {
                g.scroll_rest_timer = 0.0f;
                g.scroll_px = clampf(g.scroll_px - static_cast<float>(ti.dy), 0.0f, max_scroll_px());
                g.scroll_target = g.scroll_px;
                const float dt_s = std::clamp(static_cast<float>(g.dt), 0.004f, 0.050f);
                if (ti.dy != 0) {
                    const float inst_v = clampf(-static_cast<float>(ti.dy) / dt_s, -3400.0f, 3400.0f);
                    const float k_v = clampf(dt_s * 24.0f, 0.25f, 0.78f);
                    g.scroll_vel += (inst_v - g.scroll_vel) * k_v;
                } else {
                    g.scroll_vel *= std::exp(-12.0f * dt_s);
                }
            }
        } else if (g.drag_mode == DRAG_LYRICS) {
            const int thr = be_input_is_touch() ? 10 : 8;
            if (!g.drag_moved && (std::abs(ti.y - g.drag_start_y) > thr || std::abs(ti.x - g.drag_start_x) > thr)) {
                g.drag_moved = 1;
            }
            if (g.drag_moved) {
                g.lyr_cam_y = clampf(g.lyr_cam_y - static_cast<float>(ti.dy), 0.0f, std::max(0.0f, g.lyr_max_scroll));
                g.lyr_manual_timer = 4.5f;
                const float dt_s = std::clamp(static_cast<float>(g.dt), 0.004f, 0.050f);
                if (ti.dy != 0) {
                    const float inst_v = clampf(-static_cast<float>(ti.dy) / dt_s, -3000.0f, 3000.0f);
                    const float k_v = clampf(dt_s * 24.0f, 0.25f, 0.78f);
                    g.lyr_scroll_vel += (inst_v - g.lyr_scroll_vel) * k_v;
                } else {
                    g.lyr_scroll_vel *= std::exp(-12.0f * dt_s);
                }
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
            if (g.page == PAGE_SETTINGS || g.page == PAGE_LYRICS) {
                capture_fade();
                g.page = PAGE_LIBRARY;
                g.scroll_px = g.scroll_target = g.scroll_vel = 0.0f;
            }
        }
        if (kd & BTN_A) toggle_play_pause_action();
        if (kd & BTN_X) { if (g.playing_idx >= 0) toggle_fav(g.playing_idx); }
        if (kd & BTN_Y) { capture_fade(); g.page = (g.page == PAGE_LYRICS) ? PAGE_LIBRARY : PAGE_LYRICS; }
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
        draw_player_bar(mx, my);
    } else if (g.page == PAGE_SETTINGS) {
        draw_frosted_ambient_bg(0);
        draw_nav_rail(mx, my);
        draw_settings_view(mx, my);
        draw_player_bar(mx, my);
    } else {
        draw_immersive_view(mx, my);
    }

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
        const float tgt_sw = g.accent_follow ? 1.0f : 0.0f;
        g.accent_sw_t += (tgt_sw - g.accent_sw_t) * (1.0f - std::exp(-14.0f * static_cast<float>(g.dt)));

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
        if (scroll_moving) g.scroll_rest_timer = 0.0f;
        else g.scroll_rest_timer += static_cast<float>(g.dt);
        const bool theme_moving = (std::fabs(g.theme_pos - (g.is_light ? 1.0f : 0.0f)) > 0.001f) ||
                                  (std::fabs(g.accent_sw_t - tgt_sw) > 0.002f);
        if (!scroll_moving && !theme_moving && !g.fade_armed && g.scroll_rest_timer >= 0.22f) {
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
                g.page = PAGE_SETTINGS;
                g.scroll_px = g.scroll_target = 0.0f;
                g.fade_armed = 0;
            }
            if (dump_frame == 108) {
                render_frame(0, 0);
                dump_rgba_shot(dump_dir, "shot_set.rgba");
                g.page = PAGE_LYRICS;
                g.fade_armed = 0;
            }
            if (dump_frame == 124) {
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
            const bool suppress_hover = (g.drag_mode != DRAG_NONE) || g.drag_moved || be_input_is_touch();
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
