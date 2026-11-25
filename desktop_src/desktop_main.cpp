#include "backend.h"
#include "audio.h"
#include "decode.h"
#include "meta.h"
#include "lrc.h"
#include "draw.h"
#include "util.h"
#include "plat.h"
#include "desktop_plugin.h"
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
    // Per-line spring physics & staggered quintic cascade state
    float spring_y = 0.0f;
    float spring_vy = 0.0f;
    float cur_scale = 1.0f;
    float scale_from = 1.0f;
    float scale_to = 1.0f;
    float scale_t0 = -10.0f;
    float cur_alpha = 0.35f;
    float alpha_from = 0.35f;
    float alpha_to = 0.35f;
    float alpha_t0 = -10.0f;
    float anim_dur = 0.42f;
    float cur_lift = 0.0f;
    float lift_from = 0.0f;
    float lift_to = 0.0f;
    int anim_init = 0;
};

struct GridHit { Rect card{}; Rect fav{}; int track_idx = 0; };
struct LyrHit  { Rect rc{}; double t = 0.0; };
struct DirHit  { Rect rc{}; int dir_idx = 0; };

enum {
    SET_LIGHT = 1, SET_ACCENT_FOLLOW = 2, SET_BG_PICK = 3, SET_BG_CLEAR = 4,
    SET_VIEW = 5, SET_MODE = 6, SET_ADD_DIR = 7, SET_RESCAN = 8,
    SET_RM_DIR = 9, SET_VOL_TRK = 10, SET_BACK = 11,
    SET_THEME_LIGHT = 12, SET_THEME_DARK = 13, SET_PLAY_MODE = 14, SET_DIR_REMOVE = 15,
    SET_OPEN_PLG_DIR = 16, SET_CLEAR_PLG_CACHE = 17,
    PLG_TAB_BROWSE = 100, PLG_TAB_MANAGE = 101, PLG_RESCAN = 102, PLG_CLEAR_CACHE = 103,
    PLG_SELECT = 104, PLG_TOGGLE = 105, PLG_LOGIN = 106, PLG_LOGOUT = 107,
    PLG_REMOVE = 108, PLG_PLAYLIST = 109, PLG_TRACK = 110, PLG_SEARCH_GO = 111,
    PLG_CFG_EDIT = 112, PLG_HOME = 113, PLG_QUALITY = 114,
    PLG_QR_TAB = 115, PLG_QR_MODE = 116, PLG_QR_REFRESH = 117, PLG_COOKIE_SUBMIT = 118
};
struct SetHit  { Rect rc{}; int id = 0; int arg = 0; };

enum { PAGE_LIBRARY = 0, PAGE_LYRICS = 1, PAGE_SETTINGS = 2, PAGE_PLUGINS = 3 };
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
    std::array<float, BG_ART_W * BG_ART_H * 3> prev_bg_art{};
    float bg_xfade_t = 1.0f;
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
    int   lyr_last_cur = -1;
    int   lyr_springs_busy = 0;
    float wave_phase = 0.0f, ambient_t = 0.0f, scallop_rot = 0.0f;
    float play_morph_t = 0.0f;
    float fav_bounce_t = 0.0f;
    float rail_pill_y = 66.0f;
    std::array<float, 28> bars{};
    std::array<float, 4> eq_bars{ 0.22f, 0.45f, 0.32f, 0.55f };
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

    Rect rc_menu_btn{}, rc_nav_all{}, rc_nav_fav{}, rc_nav_lyr{}, rc_nav_plg{}, rc_nav_add{}, rc_nav_set{};
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
    Rect rc_set_back{}, rc_plg_back{}, rc_plg_qr_close{};
    Rect rc_plg_search{}, rc_plg_search_clr{};
    Rect rc_plg_qr_tab[2]{}, rc_plg_qr_input{}, rc_plg_qr_submit{}, rc_plg_qr_refresh{};
    int ui_px0 = 0, ui_px1 = 100, ui_py = 0;
    int ui_vx0 = 0, ui_vx1 = 100;
    int ui_list_x0 = 0, ui_list_x1 = 0, ui_list_y0 = 0, ui_list_y1 = 0;

    int grid_cols = 5, grid_w = 1000, grid_gap = 18, card_w = 168, card_h = 220;
    std::vector<GridHit> grid_hits;
    std::vector<LyrHit> lyr_hits;
    std::vector<DirHit> rc_dir_rows;
    std::vector<SetHit> set_hits;

    int playing_online = 0;
    int online_play_pos = -1;
    double online_dur = 0.0;
    std::string online_play_id, online_play_pid;
    int plg_subtab = 0;
    float plg_content_h = 600.0f;
    float qr_poll_timer = 0.0f;
    int qr_tab = 0;
    int qr_mode_idx = 0;
    int qr_cookie_focused = 0;
    std::string qr_cookie_val;
    int plg_search_focused = 0;
    std::string plg_search_q;
    std::string plg_quality = "exhigh";
    std::string active_plugin_id;
    std::string cfg_edit_pid, cfg_edit_key, cfg_edit_val;
    int cfg_editing = 0;
    PluginUiSnapshot plg_snap;
    std::vector<PluginInfo> plg_list;
    std::vector<SetHit> plg_hits;
    std::vector<std::string> plg_hit_keys;
    std::string nav_tooltip;
    Rect nav_tooltip_rc{};
};

AppState g;

inline int side_w() { return g.W >= 1000 ? 210 : 76; }

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
        g.prev_bg_art = g.tgt_bg_art;
        g.bg_xfade_t = 1.0f;
        g.bg_art_init = 1;
    } else {
        g.prev_bg_art = g.cur_bg_art;
        g.bg_xfade_t = 0.0f;
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
        const float scrim_dark  = imm_flag ? (0.46f + 0.26f * u_bright) : (0.20f + 0.38f * u_bright);
        const float scrim_light = imm_flag ? scrim_dark                 : (0.46f + 0.20f * u_dim);
        const float scrim_f = scrim_dark + (scrim_light - scrim_dark) * (imm_flag ? 0.0f : t);
        const uint32_t ua = static_cast<uint32_t>(std::clamp(static_cast<int>(scrim_f * 256.0f + 0.5f), 0, 256));
        const uint32_t na = 256u - ua;
        const uint32_t scrim_col = imm_flag ? COL(8, 10, 16, 255)
                                            : mix_col(COL(10, 12, 18, 255), COL(238, 242, 248, 255), t);
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
    const float bx1 = (is_immersive ? 0.28f : 0.25f) + 0.08f * std::cos(g.ambient_t * 0.22f);
    const float by1 = (is_immersive ? 0.38f : 0.30f) + 0.07f * std::sin(g.ambient_t * 0.19f);
    const float bx2 = 0.72f + 0.09f * std::sin(g.ambient_t * 0.16f);
    const float by2 = 0.66f + 0.08f * std::cos(g.ambient_t * 0.21f);

    const float ar = static_cast<float>(g.C_ACCENT & 0xFFu);
    const float ag = static_cast<float>((g.C_ACCENT >> 8) & 0xFFu);
    const float ab = static_cast<float>((g.C_ACCENT >> 16) & 0xFFu);

    constexpr int MN = BG_ART_W * BG_ART_H;
    float mr[MN]{}, mg[MN]{}, mb[MN]{};
    float tr[MN]{}, tg[MN]{}, tb[MN]{};

    const float xf = smooth_u(g.bg_xfade_t);
    const float inv_xf = 1.0f - xf;
    const float low_energy = clampf((g.bars[0] + g.bars[1] + g.bars[2]) * 0.34f, 0.0f, 1.0f);
    auto sample_art = [&](float u, float v, float& or_, float& og_, float& ob_) {
        // Wrap gently inside cover bounds using mirror reflection for seamless swirling blobs
        auto mirror_u = [](float t) {
            float m = std::fmod(std::fabs(t), 2.0f);
            return (m > 1.0f) ? (2.0f - m) : m;
        };
        const float fx = mirror_u(u) * static_cast<float>(BG_ART_W - 1);
        const float fy = mirror_u(v) * static_cast<float>(BG_ART_H - 1);
        const int x0 = std::clamp(static_cast<int>(fx), 0, BG_ART_W - 2);
        const int y0 = std::clamp(static_cast<int>(fy), 0, BG_ART_H - 2);
        const float tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0);
        const float w00 = (1.0f - tx) * (1.0f - ty), w10 = tx * (1.0f - ty);
        const float w01 = (1.0f - tx) * ty, w11 = tx * ty;
        const size_t i00 = (static_cast<size_t>(y0) * BG_ART_W + x0) * 3;
        const size_t i10 = i00 + 3, i01 = i00 + BG_ART_W * 3, i11 = i01 + 3;
        auto mixp = [&](int c) {
            const float cur_v = g.cur_bg_art[i00 + c] * w00 + g.cur_bg_art[i10 + c] * w10 +
                                g.cur_bg_art[i01 + c] * w01 + g.cur_bg_art[i11 + c] * w11;
            if (xf >= 0.999f) return cur_v;
            const float prv_v = g.prev_bg_art[i00 + c] * w00 + g.prev_bg_art[i10 + c] * w10 +
                                g.prev_bg_art[i01 + c] * w01 + g.prev_bg_art[i11 + c] * w11;
            return prv_v * inv_xf + cur_v * xf;
        };
        or_ = mixp(0); og_ = mixp(1); ob_ = mixp(2);
    };

    const float cs1 = std::cos(g.ambient_t * 0.17f), sn1 = std::sin(g.ambient_t * 0.17f);
    const float cs2 = std::cos(-g.ambient_t * 0.13f + 1.1f), sn2 = std::sin(-g.ambient_t * 0.13f + 1.1f);
    const float pulse = 1.0f + low_energy * 0.10f;

    for (int y = 0; y < BG_ART_H; ++y) {
        const float fy = static_cast<float>(y) / static_cast<float>(BG_ART_H - 1);
        for (int x = 0; x < BG_ART_W; ++x) {
            const float fx = static_cast<float>(x) / static_cast<float>(BG_ART_W - 1);
            const size_t p = static_cast<size_t>(y) * BG_ART_W + x;
            const size_t idx = p * 3;
            const float dx = fx - bx1, dy = fy - by1;
            const float glow = clampf(0.22f / (1.0f + (dx * dx + dy * dy) * 3.6f) + breath * (1.0f - fy) + 0.14f, 0.08f, 0.44f);

            if (is_immersive) {
                // Klang FluidBackground: 2-octave organic domain warp + multi-layer rotating/orbiting cover blooms
                const float wox = 0.065f * std::sin(fy * 4.2f + g.ambient_t * 0.34f) + 0.032f * std::cos(fx * 6.8f - g.ambient_t * 0.48f);
                const float woy = 0.065f * std::cos(fx * 4.0f - g.ambient_t * 0.31f) + 0.032f * std::sin(fy * 6.4f + g.ambient_t * 0.44f);
                const float px_c = (fx - 0.5f) * 1.15f + wox;
                const float py_c = (fy - 0.5f) * 1.15f + woy;

                // Layer 1: wide rotated cover base
                float r1, g1c, b1;
                sample_art(0.5f + (px_c * cs1 - py_c * sn1) * 0.65f, 0.5f + (px_c * sn1 + py_c * cs1) * 0.65f, r1, g1c, b1);
                // Layer 2: counter-orbiting mid-scale cover patch (high-energy pulse)
                float r2, g2c, b2;
                const float o2x = px_c - 0.16f * std::cos(g.ambient_t * 0.23f);
                const float o2y = py_c - 0.14f * std::sin(g.ambient_t * 0.19f);
                sample_art(0.5f + (o2x * cs2 - o2y * sn2) * (0.85f * pulse), 0.5f + (o2x * sn2 + o2y * cs2) * (0.85f * pulse), r2, g2c, b2);
                // Layer 3: warm accent quadrant bloom
                const float dx2 = fx - bx2, dy2 = fy - by2;
                const float g1w = clampf(0.30f / (1.0f + (dx * dx + dy * dy) * 2.5f), 0.0f, 0.35f);
                const float g2w = clampf(0.28f / (1.0f + (dx2 * dx2 + dy2 * dy2) * 2.4f), 0.0f, 0.32f);

                float cr = r1 * 0.54f + r2 * 0.46f;
                float cg = g1c * 0.54f + g2c * 0.46f;
                float cb = b1 * 0.54f + b2 * 0.46f;
                cr = cr * (1.0f - g1w - g2w) + g.cur_qf[0][0] * g1w + g.cur_qf[3][0] * g2w;
                cg = cg * (1.0f - g1w - g2w) + g.cur_qf[0][1] * g1w + g.cur_qf[3][1] * g2w;
                cb = cb * (1.0f - g1w - g2w) + g.cur_qf[0][2] * g1w + g.cur_qf[3][2] * g2w;

                // Vibrancy boost + ceiling + deep atmospheric dark scrim for crisp white lyrics
                const float cavg = (cr + cg + cb) / 3.0f;
                cr = clampf(cavg + (cr - cavg) * 1.34f, 0.0f, 255.0f);
                cg = clampf(cavg + (cg - cavg) * 1.34f, 0.0f, 255.0f);
                cb = clampf(cavg + (cb - cavg) * 1.34f, 0.0f, 255.0f);
                const float lum = 0.299f * cr + 0.587f * cg + 0.114f * cb;
                if (lum > 184.0f) {
                    const float sc = 184.0f / lum;
                    cr *= sc; cg *= sc; cb *= sc;
                }

                const float vx = fx - 0.5f, vy = fy - 0.5f;
                const float dark_scrim = clampf(0.49f + 0.24f * (vx * vx + vy * vy) + 0.02f * std::sin(g.ambient_t * 0.35f) - low_energy * 0.03f, 0.44f, 0.66f);
                const float keep = 1.0f - dark_scrim;
                mr[p] = cr * keep + 8.0f  * dark_scrim;
                mg[p] = cg * keep + 10.0f * dark_scrim;
                mb[p] = cb * keep + 16.0f * dark_scrim;
                continue;
            }

            const float base_r = (xf >= 0.999f) ? g.cur_bg_art[idx + 0] : (g.prev_bg_art[idx + 0] * inv_xf + g.cur_bg_art[idx + 0] * xf);
            const float base_g = (xf >= 0.999f) ? g.cur_bg_art[idx + 1] : (g.prev_bg_art[idx + 1] * inv_xf + g.cur_bg_art[idx + 1] * xf);
            const float base_b = (xf >= 0.999f) ? g.cur_bg_art[idx + 2] : (g.prev_bg_art[idx + 2] * inv_xf + g.cur_bg_art[idx + 2] * xf);
            const float cr = base_r + (ar - base_r) * glow;
            const float cg = base_g + (ag - base_g) * glow;
            const float cb = base_b + (ab - base_b) * glow;

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

    // Separable [1 2 1]/4 smoothing passes keep cover gradients ultra-smooth
    const int blur_passes = is_immersive ? 3 : 2;
    for (int pass = 0; pass < blur_passes; ++pass) {
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

inline float quintic_eval(float t) {
    t = clampf(t, 0.0f, 1.0f);
    const float t3 = t * t * t;
    return t3 * (10.0f + t * (-15.0f + 6.0f * t));
}

inline void spring_step(float& pos, float& vel, float target, float dt,
                        float omega = 11.0f, float zeta = 0.82f) {
    const float rel = pos - target;
    if (std::fabs(rel) < 0.08f && std::fabs(vel) < 0.4f) {
        pos = target; vel = 0.0f; return;
    }
    const float wd = omega * std::sqrt(std::max(0.01f, 1.0f - zeta * zeta));
    const float e  = std::exp(-zeta * omega * dt);
    const float cs = std::cos(wd * dt);
    const float sn = std::sin(wd * dt);
    const float a  = (zeta * omega) / wd;
    const float c1 = rel, c2 = (vel + zeta * omega * rel) / wd;
    pos = target + e * (c1 * cs + c2 * sn);
    vel = e * ((c2 * wd - zeta * omega * c1) * cs - (c1 * wd + zeta * omega * c2) * sn);
    (void)a;
}

inline float bounce_scale(float bounce_rem) {
    if (bounce_rem <= 0.0f) return 1.0f;
    const float u = clampf(1.0f - bounce_rem / 0.42f, 0.0f, 1.0f);
    if (u < 0.26f) return 0.80f + 0.52f * quintic_eval(u / 0.26f);
    if (u < 0.56f) return 1.32f - 0.38f * quintic_eval((u - 0.26f) / 0.30f);
    return 0.94f + 0.06f * quintic_eval((u - 0.56f) / 0.44f);
}

void fill_quad_aa(Canvas* c, const float lpts[4][2], uint32_t col) {
    if (!c || !c->px) return;
    const uint8_t sa = col_a(col);
    if (sa == 0) return;
    const float sc = c->s > 0.1f ? c->s : 1.0f;
    float pts[4][2];
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    for (int i = 0; i < 4; ++i) {
        pts[i][0] = lpts[i][0] * sc;
        pts[i][1] = lpts[i][1] * sc;
        minx = std::min(minx, pts[i][0]); maxx = std::max(maxx, pts[i][0]);
        miny = std::min(miny, pts[i][1]); maxy = std::max(maxy, pts[i][1]);
    }
    int cx0 = c->clip_x0, cy0 = c->clip_y0, cx1 = c->clip_x1, cy1 = c->clip_y1;
    if (cx0 == 0 && cy0 == 0 && cx1 == 0 && cy1 == 0) { cx1 = c->w; cy1 = c->h; }
    cx0 = std::max(0, cx0); cy0 = std::max(0, cy0);
    cx1 = std::min(c->w, cx1); cy1 = std::min(c->h, cy1);
    const int x0 = std::max(cx0, static_cast<int>(std::floor(minx)));
    const int y0 = std::max(cy0, static_cast<int>(std::floor(miny)));
    const int x1 = std::min(cx1 - 1, static_cast<int>(std::ceil(maxx)));
    const int y1 = std::min(cy1 - 1, static_cast<int>(std::ceil(maxy)));
    if (x0 > x1 || y0 > y1) return;

    float area2 = 0.0f;
    for (int i = 0; i < 4; ++i) {
        const int j = (i + 1) & 3;
        area2 += pts[i][0] * pts[j][1] - pts[j][0] * pts[i][1];
    }
    const float orient = (area2 >= 0.0f) ? 1.0f : -1.0f;
    const uint32_t sr = col & 0xFFu, sg = (col >> 8) & 0xFFu, sb = (col >> 16) & 0xFFu;

    for (int py = y0; py <= y1; ++py) {
        uint32_t* drow = c->px + static_cast<size_t>(py) * c->w;
        for (int px = x0; px <= x1; ++px) {
            int cov = 0;
            for (int sy = 0; sy < 4; ++sy) {
                const float fy = static_cast<float>(py) + (static_cast<float>(sy) + 0.5f) * 0.25f;
                for (int sx = 0; sx < 4; ++sx) {
                    const float fx = static_cast<float>(px) + (static_cast<float>(sx) + 0.5f) * 0.25f;
                    bool in_poly = true;
                    for (int e = 0; e < 4; ++e) {
                        const int n = (e + 1) & 3;
                        const float dx = pts[n][0] - pts[e][0], dy = pts[n][1] - pts[e][1];
                        if (dx * dx + dy * dy < 1e-6f) continue;
                        const float cross = (dx * (fy - pts[e][1]) - dy * (fx - pts[e][0])) * orient;
                        if (cross < -1e-4f) { in_poly = false; break; }
                    }
                    if (in_poly) ++cov;
                }
            }
            if (cov == 0) continue;
            const uint32_t a = (static_cast<uint32_t>(sa) * static_cast<uint32_t>(cov)) / 16u;
            if (a == 0) continue;
            if (a >= 255u) { drow[px] = col | 0xFF000000u; continue; }
            const uint32_t dst = drow[px];
            const uint32_t na = 255u - a;
            const uint32_t dr = ((sr * a + (dst & 0xFFu) * na + 127u) >> 8) & 0xFFu;
            const uint32_t dg = ((sg * a + ((dst >> 8) & 0xFFu) * na + 127u) >> 8) & 0xFFu;
            const uint32_t db = ((sb * a + ((dst >> 16) & 0xFFu) * na + 127u) >> 8) & 0xFFu;
            drow[px] = COL(dr, dg, db, 255);
        }
    }
}

void draw_morph_play_pause(Canvas* c, float cx, float cy, float size, float morph_t, uint32_t col) {
    // Klang PlayPauseGlyph: split left + right 4-vertex morphing (16x16 canonical coords)
    static constexpr float kPlayL[4][2]  = { {3.6f, 1.5f}, {9.1f, 4.75f}, {9.1f, 11.25f}, {3.6f, 14.5f} };
    static constexpr float kPauseL[4][2] = { {3.0f, 2.0f}, {6.6f, 2.0f},  {6.6f, 14.0f},  {3.0f, 14.0f} };
    static constexpr float kPlayR[4][2]  = { {8.7f, 4.5f}, {14.2f, 8.0f}, {14.2f, 8.0f},  {8.7f, 11.5f} };
    static constexpr float kPauseR[4][2] = { {9.4f, 2.0f}, {13.0f, 2.0f}, {13.0f, 14.0f}, {9.4f, 14.0f} };

    const float t = smooth_u(morph_t);
    const float sc = size / 16.0f;
    const float ox = cx - size * 0.5f;
    const float oy = cy - size * 0.5f;
    float qL[4][2], qR[4][2];
    for (int i = 0; i < 4; ++i) {
        qL[i][0] = ox + (kPlayL[i][0] + (kPauseL[i][0] - kPlayL[i][0]) * t) * sc;
        qL[i][1] = oy + (kPlayL[i][1] + (kPauseL[i][1] - kPlayL[i][1]) * t) * sc;
        qR[i][0] = ox + (kPlayR[i][0] + (kPauseR[i][0] - kPlayR[i][0]) * t) * sc;
        qR[i][1] = oy + (kPlayR[i][1] + (kPauseR[i][1] - kPlayR[i][1]) * t) * sc;
    }
    fill_quad_aa(c, qL, col);
    fill_quad_aa(c, qR, col);
}

void draw_now_playing_bars(Canvas* c, int x, int y, int sz, int playing_now, uint32_t col) {
    // Klang NowPlayingBars: 4 asymmetric-damped vertical rounded bars
    const int bw = std::max(2, sz * 15 / 100);
    const int gap = std::max(1, (sz - 4 * bw) / 3);
    const int tot_w = 4 * bw + 3 * gap;
    const int x0 = x + (sz - tot_w) / 2;
    for (int i = 0; i < 4; ++i) {
        const float lvl = playing_now ? clampf(g.eq_bars[i], 0.16f, 1.0f) : 0.18f;
        const int bh = std::max(3, static_cast<int>(static_cast<float>(sz) * lvl + 0.5f));
        fill_round_rect_aa(c, x0 + i * (bw + gap), y + sz - bh, bw, bh, std::max(1, bw / 2), col);
    }
}

void draw_skeleton_shimmer(Canvas* c, int x, int y, int w, int h, int r, float phase, bool lt) {
    if (w <= 2 || h <= 2) return;
    const uint32_t base = lt ? COL(0, 0, 0, 18) : COL(255, 255, 255, 20);
    fill_round_rect_aa(c, x, y, w, h, r, base);
    const float u = std::fmod(phase * 0.65f, 1.5f) - 0.25f;
    const int sw = std::max(28, w / 3);
    const int sx = x + static_cast<int>(u * static_cast<float>(w)) - sw / 2;
    const int cx0 = std::max(x, sx), cx1 = std::min(x + w, sx + sw);
    if (cx1 > cx0) {
        const float edge = 1.0f - std::fabs(static_cast<float>(cx0 + cx1) * 0.5f - static_cast<float>(x + w / 2)) / static_cast<float>(std::max(1, w));
        const uint8_t sa = static_cast<uint8_t>(clampf(edge, 0.2f, 1.0f) * (lt ? 42.0f : 32.0f));
        fill_round_rect_aa_a(c, cx0, y, cx1 - cx0, h, std::min(r, h / 2), COL(255, 255, 255, 255), sa);
    }
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
void icon_plug(Canvas* c, int x, int y, int sz, uint32_t col) {
    stroke_round_rect_aa(c, x + 2, y + 4, sz - 4, sz - 7, 4, 2, col, 230);
    fill_round_rect_aa(c, x + sz / 3 - 1, y + 1, 2, 4, 1, col);
    fill_round_rect_aa(c, x + (sz * 2) / 3 - 1, y + 1, 2, 4, 1, col);
    fill_round_rect_aa(c, x + sz / 2 - 1, y + sz - 4, 2, 4, 1, col);
}

void toggle_fav(int idx) {
    if (idx < 0 || idx >= static_cast<int>(g.tracks.size())) return;
    g.tracks[idx].fav = !g.tracks[idx].fav;
    g.fav_bounce_t = 0.42f;
    save_favorites();
    if (g.lib_tab == TAB_FAV || g.fmt_filter == FMT_FILT_FAV) rebuild_visible();
}

inline bool has_active_track() {
    return g.playing_idx >= 0 || g.playing_online;
}

inline double active_track_dur() {
    if (!has_active_track()) return 0.0;
    const double d = audio_duration();
    if (d > 0.1) return d;
    return g.playing_online ? g.online_dur : 0.0;
}

inline double active_track_pos() {
    if (!has_active_track()) return 0.0;
    return (g.scrub_drag == 1) ? g.scrub_sec : audio_position();
}

void play_online_step(int dir, bool manual) {
    if (g.plg_snap.tracks.empty()) return;
    const int n = static_cast<int>(g.plg_snap.tracks.size());
    int np = g.online_play_pos;
    if (np < 0 || np >= n) {
        for (int i = 0; i < n; ++i) {
            if (g.plg_snap.tracks[i].id == g.online_play_id &&
                g.plg_snap.tracks[i].plugin_id == g.online_play_pid) {
                np = i; break;
            }
        }
    }
    if (!manual && g.play_mode == MODE_LOOP_ONE && np >= 0) {
        // Loop current online track
    } else if (g.play_mode == MODE_SHUFFLE && n > 1) {
        for (int tries = 0; tries < 8; ++tries) {
            const int cand = static_cast<int>(next_rand() % static_cast<uint32_t>(n));
            if (cand != np) { np = cand; break; }
        }
    } else {
        if (dir >= 0) np = (np + 1 + n) % n;
        else          np = (np > 0 ? np - 1 : n - 1);
    }
    np = std::clamp(np, 0, n - 1);
    g.online_play_pos = np;
    plugin_async_play_track(g.plg_snap.tracks[np].plugin_id, g.plg_snap.tracks[np], g.plg_quality);
    if (manual) show_toast(dir >= 0 ? "正在缓冲下一首在线曲目..." : "正在缓冲上一首在线曲目...");
}

void play_online_ready(const PluginReadyPlay& rp) {
    if (!rp.ok || rp.audio_path.empty()) {
        show_toast(!rp.error.empty() ? rp.error.c_str() : "在线音轨缓冲失败");
        return;
    }
    g.scrub_drag = 0; g.scrub_sec = 0.0;
    g.lyr_cam_valid = 0; g.lyr_manual_timer = 0.0f;
    g.lyr_scroll_vel = 0.0f;
    g.lyr_last_cur = -1;
    g.lyr_wrap_ptr = nullptr;
    g.lyr_wrapped.clear();
    g.playing_idx = -1;
    g.playing_online = 1;
    g.online_dur = rp.track.dur;
    g.online_play_id = rp.track.id;
    g.online_play_pid = rp.track.plugin_id;
    audio_play(rp.audio_path.c_str());

    g.meta.clear();
    g.lyr.reset();
    g.p_title  = rp.track.title;
    g.p_artist = rp.track.artist;
    g.p_album  = rp.track.album;
    std::snprintf(g.p_fmt, sizeof(g.p_fmt), "%s", rp.track.fmt[0] ? rp.track.fmt : "STREAM");

    g.cover_src.resize(static_cast<size_t>(COVER_SRC_SZ) * COVER_SRC_SZ);
    g.cover_mini.resize(static_cast<size_t>(MINI_ART_SZ) * MINI_ART_SZ);
    bool got_cov = false;
    if (!rp.cover_path.empty()) {
        FILE* cf = plat_fopen(rp.cover_path.c_str(), "rb");
        if (cf) {
            std::fseek(cf, 0, SEEK_END);
            long sz = std::ftell(cf);
            std::fseek(cf, 0, SEEK_SET);
            if (sz > 16 && sz < 16 * 1024 * 1024) {
                std::vector<uint8_t> raw(static_cast<size_t>(sz));
                size_t rd = std::fread(raw.data(), 1, static_cast<size_t>(sz), cf);
                int cw = 0, ch = 0, cc = 0;
                unsigned char* img = stbi_load_from_memory(raw.data(), static_cast<int>(rd), &cw, &ch, &cc, 4);
                if (img && cw > 0 && ch > 0) {
                    std::vector<uint32_t> tmp(static_cast<size_t>(cw) * ch);
                    for (size_t i = 0; i < tmp.size(); ++i) {
                        const unsigned char* sp = img + i * 4;
                        tmp[i] = COL(sp[0], sp[1], sp[2], 255);
                    }
                    resample_cover_square(tmp.data(), cw, ch, g.cover_src.data(), COVER_SRC_SZ, 1);
                    got_cov = true;
                }
                if (img) stbi_image_free(img);
            }
            std::fclose(cf);
        }
    }
    if (!got_cov) make_default_art(g.cover_src.data(), COVER_SRC_SZ, rp.track.title.c_str());
    resample_cover_square(g.cover_src.data(), COVER_SRC_SZ, COVER_SRC_SZ, g.cover_mini.data(), MINI_ART_SZ, 1);
    g.cover_ready = 1;
    update_target_cover_colors();

    g.lyr_req_id = 0;
    if (!rp.lrc_orig.empty()) {
        std::string lrc_tmp = std::string(be_config_dir()) + "/stream_cache/active_online.lrc";
        std::string combined = rp.lrc_orig;
        if (!rp.lrc_trans.empty()) combined += "\n" + rp.lrc_trans;
        FILE* lf = plat_fopen(lrc_tmp.c_str(), "wb");
        if (lf) {
            std::fwrite(combined.data(), 1, combined.size(), lf);
            std::fclose(lf);
            g.lyr.reset(lyrics_load_file(lrc_tmp.c_str()));
        }
    }
    if (!g.lyr) {
        g.lyr_req_id = lyrics_start_online(rp.audio_path.c_str(), g.p_title.c_str(), g.p_artist.c_str(),
                                           rp.track.dur > 1.0 ? rp.track.dur : audio_duration());
    }
    g.bars.fill(0.0f);
}

void load_track(int idx) {
    if (idx < 0 || idx >= static_cast<int>(g.tracks.size())) return;
    g.playing_online = 0;
    g.online_dur = 0.0;
    g.online_play_id.clear();
    g.online_play_pid.clear();
    g.scrub_drag = 0; g.scrub_sec = 0.0;
    g.lyr_cam_valid = 0; g.lyr_manual_timer = 0.0f;
    g.lyr_scroll_vel = 0.0f;
    g.lyr_last_cur = -1;
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

    if (ci_ends_with(path.c_str(), ".lua")) {
        std::string perr;
        if (plugin_import_file(path, &perr)) {
            g.plg_list = plugin_list();
            if (g.active_plugin_id.empty()) g.active_plugin_id = plugin_default_id();
            capture_fade();
            g.page = PAGE_PLUGINS;
            g.plg_subtab = 1;
            g.scroll_px = g.scroll_target = g.scroll_vel = 0.0f;
            show_toast("已导入并加载扩展插件");
        } else {
            show_toast(!perr.empty() ? perr.c_str() : "扩展插件导入失败");
        }
        return;
    }

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
    constexpr int gap = 24;
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
    if (g.page == PAGE_PLUGINS) {
        const float ch = g.plg_content_h + static_cast<float>(DOCK_CLEAR);
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
    g.rc_menu_btn = g.rc_nav_all = g.rc_nav_fav = g.rc_nav_lyr = g.rc_nav_plg = g.rc_nav_add = g.rc_nav_set = {};
    g.rc_nav_bg = g.rc_nav_theme = g.rc_nav_rescan = {};
    g.nav_tooltip.clear();
    g.nav_tooltip_rc = {};
    g.rc_search_box = g.rc_search_clear = g.rc_sort_btn = g.rc_fs_btn = g.rc_view_pill = g.rc_bg_btn = g.rc_bg_clear = {};
    g.rc_filt_chips.fill(Rect{});
    g.rc_dir_chip = {};
    g.rc_hero_card = g.rc_hero_cov = g.rc_hero_play = {};
    g.rc_banner_next.fill(Rect{});
    g.banner_next_idx.fill(-1);
    g.rc_sb_track = g.rc_back_lib = g.rc_lyr_bg = g.rc_lyr_bg_clear = g.rc_lyr_theme = g.rc_lyr_fs = {};
    g.rc_island = g.rc_bar_cov = g.rc_bar_fav = g.rc_bar_prev = g.rc_bar_play = g.rc_bar_next = {};
    g.rc_bar_mode = g.rc_bar_lyr = g.rc_bar_vol_ico = g.rc_bar_vol_trk = g.rc_bar_prog = {};
    g.rc_set_back = g.rc_plg_back = g.rc_plg_qr_close = {};
    g.rc_plg_search = g.rc_plg_search_clr = {};
    g.rc_plg_qr_tab[0] = g.rc_plg_qr_tab[1] = {};
    g.rc_plg_qr_input = g.rc_plg_qr_submit = g.rc_plg_qr_refresh = {};
    g.grid_hits.clear();
    g.lyr_hits.clear();
    g.rc_dir_rows.clear();
    g.set_hits.clear();
    g.plg_hits.clear();
    g.plg_hit_keys.clear();
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
    fill_rect_a(s, sw - 1, 0, 1, g.H, g.C_DIV, col_a(g.C_DIV) + 12);

    // Lightweight translucent surface tokens (bg-white/5, bg-white/8 in dark mode)
    const uint32_t pill_sel_bg = mix_col_a(COL(255, 255, 255, 21), COL(18, 24, 38, 16), g.light_t);
    const uint32_t pill_hov_bg = mix_col_a(COL(255, 255, 255, 13), COL(18, 24, 38, 10), g.light_t);
    const uint32_t ind_blue    = mix_col(COL(74, 162, 255, 255), COL(44, 118, 245, 255), g.light_t);

    if (wide) {
        g.rc_search_box = Rect{ 14, 16, sw - 28, 34 };
        const bool s_hov = in_rc(mx, my, g.rc_search_box);
        const uint32_t s_bg = g.search_focused
                              ? mix_col_a(COL(255, 255, 255, 24), COL(18, 24, 38, 22), g.light_t)
                              : (s_hov ? mix_col_a(COL(255, 255, 255, 18), COL(18, 24, 38, 16), g.light_t)
                                       : mix_col_a(COL(255, 255, 255, 13), COL(18, 24, 38, 11), g.light_t));
        fill_round_rect_aa(s, g.rc_search_box.x, g.rc_search_box.y, g.rc_search_box.w, g.rc_search_box.h, 17, s_bg);
        icon_search(s, g.rc_search_box.x + 16, g.rc_search_box.y + 16, 5,
                    g.search_focused ? ind_blue : g.C_TXT3);
        const int tmax = g.rc_search_box.w - 60;
        if (!g.search_q.empty()) {
            draw_text_max_bold(s, f, 13.0f, g.rc_search_box.x + 30, g.rc_search_box.y + 9,
                               g.C_TXT, g.search_q.c_str(), tmax);
            g.rc_search_clear = Rect{ g.rc_search_box.x + g.rc_search_box.w - 28, g.rc_search_box.y + 3, 26, 28 };
            fill_round_rect_aa(s, g.rc_search_clear.x + 2, g.rc_search_clear.y + 2, 24, 24, 12,
                               in_rc(mx, my, g.rc_search_clear) ? g.C_GLASS_ACT : g.C_GLASS);
            draw_text_centered_bold(s, f, 12.0f, g.rc_search_clear.x, g.rc_search_clear.x + 26,
                                    g.rc_search_clear.y + 7, g.C_TXT2, "x");
        } else {
            draw_text_max_bold(s, f, 12.5f, g.rc_search_box.x + 30, g.rc_search_box.y + 9,
                               g.C_TXT3, "搜索歌曲、歌手...", tmax);
        }
    } else {
        g.rc_search_box = Rect{ 16, 16, sw - 32, 34 };
        const bool s_act = g.search_focused || !g.search_q.empty();
        const uint32_t s_bg = s_act ? pill_sel_bg : (in_rc(mx, my, g.rc_search_box) ? pill_hov_bg : mix_col_a(COL(255, 255, 255, 13), COL(18, 24, 38, 11), g.light_t));
        fill_round_rect_aa(s, g.rc_search_box.x, g.rc_search_box.y, g.rc_search_box.w, g.rc_search_box.h, 17, s_bg);
        icon_search(s, sw / 2 - 1, 32, 6, s_act ? ind_blue : g.C_TXT2);
        if (in_rc(mx, my, g.rc_search_box)) { g.nav_tooltip = "搜索歌曲"; g.nav_tooltip_rc = g.rc_search_box; }
    }

    int active_slot = -1;
    if (g.page == PAGE_PLUGINS) active_slot = 3;
    else if (g.page == PAGE_LYRICS) active_slot = 2;
    else if (g.page == PAGE_LIBRARY && g.lib_tab == TAB_ALL) active_slot = 0;
    else if (g.page == PAGE_LIBRARY && g.lib_tab == TAB_FAV) active_slot = 1;

    constexpr int nav_y0 = 64;
    const int nav_step = wide ? 36 : 42;
    const int pill_h = wide ? 32 : 36;
    if (active_slot >= 0) {
        const float tgt_pill_y = static_cast<float>(nav_y0 + active_slot * nav_step);
        const float kp = 1.0f - std::exp(-16.0f * static_cast<float>(g.dt));
        g.rail_pill_y += (tgt_pill_y - g.rail_pill_y) * kp;
        const int py = static_cast<int>(g.rail_pill_y + 0.5f);
        fill_round_rect_aa(s, 12, py, sw - 24, pill_h, 9, pill_sel_bg);
        fill_round_rect_aa(s, 12, py + 6, 3, pill_h - 12, 1, ind_blue);
    }

    const char* labels_wide[4]  = { "为你推荐", "我喜欢的", "歌词", "扩展插件" };
    const char* labels_short[4] = { "推荐", "喜欢", "歌词", "插件" };
    Rect* nav_rcs[4] = { &g.rc_nav_all, &g.rc_nav_fav, &g.rc_nav_lyr, &g.rc_nav_plg };
    for (int i = 0; i < 4; ++i) {
        const int iy = nav_y0 + i * nav_step;
        *nav_rcs[i] = Rect{ 12, iy, sw - 24, pill_h };
        const int is_act = (i == active_slot);
        const int is_hov = in_rc(mx, my, *nav_rcs[i]);
        if (is_hov && !is_act) {
            fill_round_rect_aa(s, 12, iy, sw - 24, pill_h, 9, pill_hov_bg);
        }
        if (!wide && is_hov) { g.nav_tooltip = labels_wide[i]; g.nav_tooltip_rc = *nav_rcs[i]; }

        const uint32_t icol = is_act ? (i == 1 ? g.C_HEART : ind_blue) : (is_hov ? g.C_TXT : g.C_TXT2);
        const int cy = iy + (wide ? 16 : 13);
        const int ix = wide ? 24 : (sw / 2 - 8);
        if (i == 0) icon_library_grid(s, ix, cy - 8, 16, icol);
        else if (i == 1) icon_heart_ex(s, ix, cy - 7, 16, is_act ? g.C_HEART : icol, is_act);
        else if (i == 2) icon_lyrics_waves(s, ix, cy - 8, 16, icol);
        else icon_plug(s, ix, cy - 8, 16, icol);

        if (wide) {
            draw_text_max_bold(s, f, 13.0f, 48, iy + 8,
                               is_act ? g.C_TXT : (is_hov ? g.C_TXT : g.C_TXT2),
                               labels_wide[i], sw - 60);
        } else {
            draw_text_centered_bold(s, f, 10.5f, 8, sw - 8, iy + 22,
                                    is_act ? g.C_TXT : g.C_TXT2, labels_short[i]);
        }
    }

    // Group with generous whitespace instead of horizontal divider lines
    const int dir_y = nav_y0 + 4 * nav_step + 18;
    const int bot_top = wide ? (g.H - 50) : (g.H - 134);
    if (wide) {
        draw_text_bold(s, f, 12.0f, 16, dir_y + 4, g.C_TXT3, "我的目录");

        g.rc_nav_rescan = Rect{ sw - 62, dir_y - 1, 24, 24 };
        const bool hov_rs = in_rc(mx, my, g.rc_nav_rescan);
        if (hov_rs) {
            fill_round_rect_aa(s, g.rc_nav_rescan.x, g.rc_nav_rescan.y, 24, 24, 12, pill_hov_bg);
            g.nav_tooltip = "刷新曲库";
            g.nav_tooltip_rc = g.rc_nav_rescan;
        }
        icon_refresh(s, g.rc_nav_rescan.x + 12, g.rc_nav_rescan.y + 12, 6, hov_rs ? ind_blue : g.C_TXT3);

        g.rc_nav_add = Rect{ sw - 34, dir_y - 1, 24, 24 };
        const bool hov_add = in_rc(mx, my, g.rc_nav_add);
        if (hov_add) {
            fill_round_rect_aa(s, g.rc_nav_add.x, g.rc_nav_add.y, 24, 24, 12, pill_hov_bg);
            g.nav_tooltip = "添加目录";
            g.nav_tooltip_rc = g.rc_nav_add;
        }
        icon_plus(s, g.rc_nav_add.x + 12, g.rc_nav_add.y + 12, 5, hov_add ? ind_blue : g.C_TXT3);

        int dy = dir_y + 30;
        const int n_dirs = static_cast<int>(g.scan_dirs.size());
        if (n_dirs == 0 && dy + 28 < bot_top) {
            draw_text_bold(s, f, 12.0f, 16, dy + 6, g.C_TXT3, "点击 + 添加音乐目录");
        }
        for (int di = 0; di < n_dirs && dy + 30 <= bot_top - 6; ++di) {
            Rect r{ 12, dy, sw - 24, 30 };
            const bool is_act = (g.page == PAGE_LIBRARY && g.lib_tab == TAB_DIR && g.active_dir == di);
            const bool is_hov = in_rc(mx, my, r);
            if (is_act) {
                fill_round_rect_aa(s, r.x, r.y, r.w, r.h, 8, pill_sel_bg);
                fill_round_rect_aa(s, r.x, r.y + 6, 3, r.h - 12, 1, ind_blue);
            } else if (is_hov) {
                fill_round_rect_aa(s, r.x, r.y, r.w, r.h, 8, pill_hov_bg);
            }
            const uint32_t dc = is_act ? ind_blue : (is_hov ? g.C_TXT : g.C_TXT2);
            icon_folder(s, r.x + 12, r.y + 7, 16, dc);
            std::string dname = basename_str(g.scan_dirs[di]);
            if (dname.empty()) dname = g.scan_dirs[di];
            draw_text_max_bold(s, f, 13.0f, r.x + 36, r.y + 7, is_act ? g.C_TXT : dc, dname.c_str(), r.w - 42);
            g.rc_dir_rows.push_back(DirHit{ r, di });
            dy += 32;
        }

        // Single borderless icon-only footer row: Theme + Wallpaper + Settings
        constexpr int b_w = 40, b_h = 34, b_gap = 14;
        const int bx0 = (sw - (b_w * 3 + b_gap * 2)) / 2;
        const int by = g.H - 44;
        g.rc_nav_theme = Rect{ bx0, by, b_w, b_h };
        g.rc_nav_bg    = Rect{ bx0 + b_w + b_gap, by, b_w, b_h };
        g.rc_nav_set   = Rect{ bx0 + (b_w + b_gap) * 2, by, b_w, b_h };

        const bool hov_th = in_rc(mx, my, g.rc_nav_theme);
        if (hov_th) {
            fill_round_rect_aa(s, g.rc_nav_theme.x, g.rc_nav_theme.y, b_w, b_h, 9, pill_hov_bg);
            g.nav_tooltip = lt ? "切换深色模式" : "切换浅色模式";
            g.nav_tooltip_rc = g.rc_nav_theme;
        }
        icon_theme_orb(s, g.rc_nav_theme.x + b_w / 2, by + b_h / 2, 8,
                       hov_th ? g.C_TXT : mix_col(g.C_TXT2, ind_blue, g.light_t));

        const bool has_bg = !g.bg_img_raw.empty();
        const bool hov_bg = in_rc(mx, my, g.rc_nav_bg);
        if (has_bg || hov_bg) {
            fill_round_rect_aa(s, g.rc_nav_bg.x, g.rc_nav_bg.y, b_w, b_h, 9, has_bg ? pill_sel_bg : pill_hov_bg);
        }
        if (hov_bg) {
            g.nav_tooltip = "自定义壁纸";
            g.nav_tooltip_rc = g.rc_nav_bg;
        }
        icon_wallpaper(s, g.rc_nav_bg.x + b_w / 2 - 8, by + b_h / 2 - 8, 16,
                       has_bg ? ind_blue : (hov_bg ? g.C_TXT : g.C_TXT2));

        const bool act_set = (g.page == PAGE_SETTINGS);
        const bool hov_set = in_rc(mx, my, g.rc_nav_set);
        if (act_set) {
            fill_round_rect_aa(s, g.rc_nav_set.x, g.rc_nav_set.y, b_w, b_h, 9, pill_sel_bg);
        } else if (hov_set) {
            fill_round_rect_aa(s, g.rc_nav_set.x, g.rc_nav_set.y, b_w, b_h, 9, pill_hov_bg);
        }
        if (hov_set) {
            g.nav_tooltip = "设置";
            g.nav_tooltip_rc = g.rc_nav_set;
        }
        icon_gear(s, g.rc_nav_set.x + b_w / 2, by + b_h / 2, 7,
                  act_set ? ind_blue : (hov_set ? g.C_TXT : g.C_TXT2));
    } else {
        if (dir_y + 72 <= bot_top) {
            g.rc_nav_rescan = Rect{ 14, dir_y, sw - 28, 32 };
            const bool hov_rs = in_rc(mx, my, g.rc_nav_rescan);
            if (hov_rs) {
                fill_round_rect_aa(s, g.rc_nav_rescan.x, g.rc_nav_rescan.y, g.rc_nav_rescan.w, 32, 9, pill_hov_bg);
                g.nav_tooltip = "刷新曲库"; g.nav_tooltip_rc = g.rc_nav_rescan;
            }
            icon_refresh(s, sw / 2, g.rc_nav_rescan.y + 16, 6, hov_rs ? ind_blue : g.C_TXT2);

            g.rc_nav_add = Rect{ 14, dir_y + 38, sw - 28, 32 };
            const bool hov_add = in_rc(mx, my, g.rc_nav_add);
            if (hov_add) {
                fill_round_rect_aa(s, g.rc_nav_add.x, g.rc_nav_add.y, g.rc_nav_add.w, 32, 9, pill_hov_bg);
                g.nav_tooltip = "添加目录"; g.nav_tooltip_rc = g.rc_nav_add;
            }
            icon_plus(s, sw / 2, g.rc_nav_add.y + 16, 5, hov_add ? ind_blue : g.C_TXT2);
        }
        g.rc_nav_theme = Rect{ 14, g.H - 126, sw - 28, 34 };
        g.rc_nav_bg    = Rect{ 14, g.H - 86, sw - 28, 34 };
        g.rc_nav_set   = Rect{ 14, g.H - 46, sw - 28, 34 };
        if (in_rc(mx, my, g.rc_nav_theme)) {
            fill_round_rect_aa(s, g.rc_nav_theme.x, g.rc_nav_theme.y, g.rc_nav_theme.w, 34, 9, pill_hov_bg);
            g.nav_tooltip = lt ? "切换深色模式" : "切换浅色模式"; g.nav_tooltip_rc = g.rc_nav_theme;
        }
        icon_theme_orb(s, sw / 2, g.rc_nav_theme.y + 17, 8, mix_col(g.C_TXT2, ind_blue, g.light_t));

        const bool has_bg = !g.bg_img_raw.empty();
        if (has_bg || in_rc(mx, my, g.rc_nav_bg)) {
            fill_round_rect_aa(s, g.rc_nav_bg.x, g.rc_nav_bg.y, g.rc_nav_bg.w, 34, 9, has_bg ? pill_sel_bg : pill_hov_bg);
        }
        if (in_rc(mx, my, g.rc_nav_bg)) { g.nav_tooltip = "自定义壁纸"; g.nav_tooltip_rc = g.rc_nav_bg; }
        icon_wallpaper(s, sw / 2 - 8, g.rc_nav_bg.y + 9, 16, has_bg ? ind_blue : g.C_TXT2);

        const bool act_set = (g.page == PAGE_SETTINGS);
        if (act_set || in_rc(mx, my, g.rc_nav_set)) {
            fill_round_rect_aa(s, g.rc_nav_set.x, g.rc_nav_set.y, g.rc_nav_set.w, 34, 9, act_set ? pill_sel_bg : pill_hov_bg);
        }
        if (in_rc(mx, my, g.rc_nav_set)) { g.nav_tooltip = "设置"; g.nav_tooltip_rc = g.rc_nav_set; }
        icon_gear(s, sw / 2, g.rc_nav_set.y + 17, 7, act_set ? ind_blue : g.C_TXT2);
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
    const bool has_track = has_active_track();
    const bool paused = audio_paused() || !has_track;

    const int lx = ban.x + 26;
    const int show_mid = (ban.w >= 680) ? 1 : 0;
    const int left_max_w = std::max(140, show_mid ? static_cast<int>(ban.w * 0.36f) : (ban.w - 240));

    const char* eyebrow = (has_track && !paused) ? (g.playing_online ? "在线流媒体 · 正在播放" : "正在播放")
                                                   : (has_track ? "已暂停" : "每日精选 · 随时聆听");
    draw_text_max_bold(s, f, 12.5f, lx, ban.y + 22, g.C_ACCENT, eyebrow, left_max_w);

    const char* ttl = has_track ? g.p_title.c_str()
                                : (show_idx >= 0 ? g.tracks[show_idx].title.c_str() : "添加音乐目录开始聆听");
    std::string sub_str;
    if (has_track) {
        sub_str = !g.p_artist.empty() ? g.p_artist : (g.playing_online ? "云端扩展源" : "本地音乐");
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
    draw_morph_play_pause(s, static_cast<float>(g.rc_hero_play.x + 28), static_cast<float>(g.rc_hero_play.y + 19),
                          15.0f, has_track ? g.play_morph_t : 0.0f, g.C_ON_ACCENT);
    draw_text_bold(s, f, 14.0f, g.rc_hero_play.x + 48, g.rc_hero_play.y + 10, g.C_ON_ACCENT,
                   (!paused && has_track) ? "暂停" : "播放");

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
    if (has_track && g.cover_ready && !g.cover_src.empty()) {
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
            draw_text_centered_bold(s, f, 12.5f, rc.x, rc.x + rc.w, rc.y + 5, g.C_ACCENT, chips[i]);
        } else {
            fill_frosted_round_rect(s, rc.x, rc.y, rc.w, rc.h, 15, g.C_SURFACE,
                                    hov ? 186 : (lt ? 156 : 130), 1, glass_rim(48, 195));
            stroke_round_rect_aa(s, rc.x, rc.y, rc.w, rc.h, 15, 1, g.C_DIV, col_a(g.C_DIV) + 12);
            draw_text_centered_bold(s, f, 12.5f, rc.x, rc.x + rc.w, rc.y + 5, hov ? g.C_TXT : g.C_TXT2, chips[i]);
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
            fill_round_rect_aa(s, bx, by, 32, 32, 16, is_play ? g.C_ACCENT : g.C_ACCENT);
            if (is_play) {
                draw_now_playing_bars(s, bx + 7, by + 8, 17, !audio_paused(), g.C_ON_ACCENT);
            } else {
                icon_play(s, bx + 17, by + 16, 10, g.C_ON_ACCENT);
            }
        }

        const int info_y = cy + cw + 9;
        draw_text_max_bold(s, f, 14.5f, x + 8, info_y, is_play ? g.C_ACCENT : g.C_TXT, tr.title.c_str(), cw - 16);
        if (tr.cached_dur_w < 0 || std::fabs(tr.dur - tr.cached_dur_val) > 0.25) {
            if (tr.dur > 0.5) fmt_time(tr.dur, tr.cached_dur_s, sizeof(tr.cached_dur_s));
            else std::snprintf(tr.cached_dur_s, sizeof(tr.cached_dur_s), "--:--");
            tr.cached_dur_val = tr.dur;
            tr.cached_dur_w = text_width_bold(f, 12.0f, tr.cached_dur_s);
        }
        const int dw = tr.cached_dur_w;
        draw_text_right_bold(s, f, 12.0f, x + cw - 8, info_y + 24, is_play ? g.C_ACCENT : g.C_TXT3, tr.cached_dur_s);
        const char* sub = !tr.artist.empty() ? tr.artist.c_str() : (!tr.album.empty() ? tr.album.c_str() : tr.fmt);
        draw_text_max_bold(s, f, 12.5f, x + 8, info_y + 23, g.C_TXT2, sub, cw - 26 - dw);

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
        if (is_play) {
            fill_round_rect_aa_a(s, lx + 40, y + 8, 40, 40, 10, COL(10, 14, 22, 255), 145);
            draw_now_playing_bars(s, lx + 51, y + 19, 18, !audio_paused(), COL(255, 255, 255, 245));
        }

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

    fill_round_rect_shadow(s, bx, by + 4, bw, bh, br, theme_shadow(96));
    const uint32_t bar_bg = mix_col(mix_col(COL(20, 25, 30, 255), COL(248, 250, 255, 255), g.light_t), g.C_HERO_L, 0.14f);
    fill_frosted_round_rect(s, bx, by, bw, bh, br, bar_bg, static_cast<uint8_t>(104 + 48 * g.light_t), 12, glass_rim(64, 215));
    stroke_round_rect_aa(s, bx, by, bw, bh, br, 1, COL(255, 255, 255, 255), lt ? 165 : 54);

    const bool has_track = has_active_track();
    constexpr int cov_sz = 48;
    g.rc_bar_cov = Rect{ bx + 12, by + (bh - cov_sz) / 2, cov_sz, cov_sz };
    const uint32_t* mc = (has_track && g.cover_ready && !g.cover_mini.empty()) ? g.cover_mini.data()
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
    const bool playing_now = (has_track && !audio_paused());
    fill_ellipse_aa(s, mid_x, btn_y, hov_p ? 18 : 17, hov_p ? 18 : 17, playing_now ? g.C_ACCENT : g.C_PLAY_BTN);
    draw_morph_play_pause(s, static_cast<float>(mid_x), static_cast<float>(btn_y), 14.5f,
                          has_track ? g.play_morph_t : 0.0f, playing_now ? g.C_ON_ACCENT : g.C_PLAY_ICO);

    if (in_rc(mx, my, g.rc_bar_next)) fill_round_rect_aa(s, g.rc_bar_next.x, g.rc_bar_next.y, 32, 32, 16, g.C_GLASS_HOV);
    icon_next(s, mid_x + 40, btn_y, 10, g.C_TXT);

    const double dur = active_track_dur();
    const double pos = active_track_pos();
    const float frac = (dur > 0.1) ? clampf(static_cast<float>(pos / dur), 0.0f, 1.0f) : 0.0f;
    const int prog_span = std::clamp(bw - 480, 140, 360);
    g.ui_px0 = mid_x - prog_span / 2;
    g.ui_px1 = mid_x + prog_span / 2;
    g.ui_py  = by + bh - 14;
    g.rc_bar_prog = Rect{ g.ui_px0 - 8, g.ui_py - 12, (g.ui_px1 - g.ui_px0) + 16, 24 };

    const bool hov_prog = in_rc(mx, my, g.rc_bar_prog) || g.scrub_drag == 1;
    const int bar_h = hov_prog ? 6 : 4;
    const int bar_y = g.ui_py - bar_h / 2;
    fill_round_rect_aa(s, g.ui_px0, bar_y, prog_span, bar_h, bar_h / 2, g.C_TRACK_BG);
    const int fill_w = static_cast<int>(frac * static_cast<float>(prog_span));
    if (fill_w > 0) fill_round_rect_aa(s, g.ui_px0, bar_y, fill_w, bar_h, bar_h / 2, g.C_ACCENT);
    if (hov_prog) {
        fill_ellipse_aa(s, g.ui_px0 + fill_w, g.ui_py, 6, 6, g.C_ACCENT);
        fill_ellipse_aa(s, g.ui_px0 + fill_w, g.ui_py, 2, 2, COL(255, 255, 255, 235));
        if (dur > 0.5 && mx >= g.ui_px0 - 4 && mx <= g.ui_px1 + 4) {
            const float hf = clampf(static_cast<float>(mx - g.ui_px0) / static_cast<float>(std::max(1, prog_span)), 0.0f, 1.0f);
            char hbuf[16]{};
            fmt_time(static_cast<double>(hf) * dur, hbuf, sizeof(hbuf));
            const int tw = text_width_bold(f, 11.0f, hbuf) + 14;
            const int tx = std::clamp(mx - tw / 2, g.ui_px0 - 10, g.ui_px1 - tw + 10);
            const int ty = g.ui_py - 28;
            fill_round_rect_shadow_fast(s, tx, ty + 1, tw, 20, 6, theme_shadow(80));
            fill_round_rect_aa(s, tx, ty, tw, 20, 6, lt ? COL(250, 252, 255, 245) : COL(22, 26, 36, 242));
            stroke_round_rect_aa(s, tx, ty, tw, 20, 6, 1, g.C_DIV, 90);
            draw_text_centered_bold(s, f, 11.0f, tx, tx + tw, ty + 3, g.C_ACCENT, hbuf);
        }
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
        const float bsc = (g.playing_idx >= 0) ? bounce_scale(g.fav_bounce_t) : 1.0f;
        const int hsz = std::clamp(static_cast<int>(16.0f * bsc + 0.5f), 12, 22);
        icon_heart_ex(s, g.rc_bar_fav.x + (fav_sz - hsz) / 2, g.rc_bar_fav.y + (fav_sz - hsz) / 2,
                      hsz, is_fav ? g.C_HEART : g.C_TXT2, is_fav);

        const int info_max = g.rc_bar_fav.x - info_x - 8;
        if (info_max > 28) {
            draw_text_max_bold(s, f, 14.0f, info_x, by + 13, g.C_TXT, g.p_title.c_str(), info_max);
            draw_text_max_bold(s, f, 12.0f, info_x, by + 35, g.C_TXT2,
                               !g.p_artist.empty() ? g.p_artist.c_str()
                                                   : (g.playing_online ? "云端在线曲目" : "点击封面进入歌词"), info_max);
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

    // Dark-scrimmed high-blur album backdrop always pairs with dark translucent glass & light ink
    const uint32_t imm_surface   = mix_col_a(g.C_SURFACE, COL(18, 22, 32, 168), g.light_t);
    const uint32_t imm_txt       = COL(250, 252, 255, 255);
    const uint32_t imm_txt2      = COL(214, 222, 236, 225);
    const uint32_t imm_txt3      = COL(188, 198, 216, 185);
    const uint32_t imm_hov       = COL(255, 255, 255, 34);
    const uint32_t imm_trk_bg    = COL(255, 255, 255, 48);
    const uint32_t imm_div       = COL(255, 255, 255, 36);
    const uint8_t  imm_rim_norm  = 64;
    const uint8_t  imm_rim_hov   = 96;
    const uint32_t imm_accent    = lt ? mix_col(g.C_ACCENT, COL(255, 255, 255, 255), 0.30f) : g.C_ACCENT;
    const uint32_t imm_on_accent = COL(14, 18, 24, 255);

    g.rc_back_lib = Rect{ 26, 18, 124, 40 };
    const bool hov_back = in_rc(mx, my, g.rc_back_lib);
    fill_frosted_round_rect(s, g.rc_back_lib.x, g.rc_back_lib.y, g.rc_back_lib.w, g.rc_back_lib.h, 20,
                            imm_surface, hov_back ? 186 : 142, 3,
                            hov_back ? imm_rim_hov : imm_rim_norm);
    icon_back(s, g.rc_back_lib.x + 14, g.rc_back_lib.y + 13, 14, imm_txt);
    draw_text_bold(s, f, 13.5f, g.rc_back_lib.x + 36, g.rc_back_lib.y + 10, imm_txt, "返回曲库");

    g.rc_lyr_fs = Rect{ g.W - 66, 18, 40, 40 };
    const bool hov_fs = in_rc(mx, my, g.rc_lyr_fs);
    fill_frosted_round_rect(s, g.rc_lyr_fs.x, g.rc_lyr_fs.y, 40, 40, 20,
                            imm_surface, hov_fs ? 186 : 142, 3,
                            hov_fs ? imm_rim_hov : imm_rim_norm);
    icon_expand(s, g.rc_lyr_fs.x + 12, g.rc_lyr_fs.y + 12, 16, imm_txt);

    g.rc_lyr_theme = Rect{ g.W - 114, 18, 40, 40 };
    const bool hov_thm = in_rc(mx, my, g.rc_lyr_theme);
    fill_frosted_round_rect(s, g.rc_lyr_theme.x, g.rc_lyr_theme.y, 40, 40, 20,
                            imm_surface, hov_thm ? 186 : 142, 3,
                            hov_thm ? imm_rim_hov : imm_rim_norm);
    icon_theme_orb(s, g.rc_lyr_theme.x + 20, g.rc_lyr_theme.y + 20, 8, mix_col(imm_txt, imm_accent, g.light_t));

    g.rc_lyr_bg = Rect{ g.W - 162, 18, 40, 40 };
    const bool has_bg = !g.bg_img_raw.empty();
    const bool hov_lbg = in_rc(mx, my, g.rc_lyr_bg);
    fill_frosted_round_rect(s, g.rc_lyr_bg.x, g.rc_lyr_bg.y, 40, 40, 20,
                            has_bg ? mix_col(imm_surface, imm_accent, 0.24f) : imm_surface,
                            has_bg ? 194 : (hov_lbg ? 186 : 142), 3,
                            (has_bg || hov_lbg) ? imm_rim_hov : imm_rim_norm);
    icon_wallpaper(s, g.rc_lyr_bg.x + 12, g.rc_lyr_bg.y + 12, 16, has_bg ? imm_accent : imm_txt);

    if (has_bg) {
        g.rc_lyr_bg_clear = Rect{ g.W - 210, 18, 40, 40 };
        const bool hov_lcl = in_rc(mx, my, g.rc_lyr_bg_clear);
        fill_frosted_round_rect(s, g.rc_lyr_bg_clear.x, g.rc_lyr_bg_clear.y, 40, 40, 20,
                                imm_surface, hov_lcl ? 190 : 142, 3,
                                hov_lcl ? imm_rim_hov : imm_rim_norm);
        icon_close_x(s, g.rc_lyr_bg_clear.x + 20, g.rc_lyr_bg_clear.y + 20, 4, hov_lcl ? g.C_HEART : imm_txt2);
    }

    // Centered, tightly capped two-column layout eliminating excess right-side whitespace
    const int content_w = std::clamp(g.W - 88, 640, 1020);
    const int content_x0 = (g.W - content_w) / 2;
    constexpr int chrome_below = 194;
    const int max_cov_h = std::max(140, g.H - 74 - 24 - chrome_below);
    const int left_target = static_cast<int>(static_cast<float>(content_w) * 0.40f);
    const int cov_sz = std::clamp(std::min(max_cov_h, std::min(static_cast<int>(g.H * 0.50f), left_target)), 150, 396);
    const int block_h = cov_sz + chrome_below;
    const int cov_x = content_x0;
    const int cov_y = std::max(74, 74 + (g.H - 98 - block_h) / 2);

    const bool has_track = has_active_track();
    fill_round_rect_shadow(s, cov_x, cov_y + 10, cov_sz, cov_sz, 16, 140);
    const uint32_t* art = (has_track && g.cover_ready && !g.cover_src.empty()) ? g.cover_src.data()
                          : (!g.poster_hero.empty() ? g.poster_hero.data() : nullptr);
    if (art) blit_rgb_round_fit(s, cov_x, cov_y, cov_sz, cov_sz, art, COVER_SRC_SZ, COVER_SRC_SZ, 16);
    else     fill_round_rect_aa(s, cov_x, cov_y, cov_sz, cov_sz, 16, COL(255, 255, 255, 24));
    stroke_round_rect_aa(s, cov_x, cov_y, cov_sz, cov_sz, 16, 1, imm_div, col_a(imm_div) + 18);

    const int info_x = cov_x;
    const int info_w = cov_sz;
    const int ty = cov_y + cov_sz + 18;
    g.rc_bar_fav = Rect{ info_x + info_w - 32, ty - 2, 32, 32 };
    const int is_fav = (g.playing_idx >= 0 && g.tracks[g.playing_idx].fav);
    if (in_rc(mx, my, g.rc_bar_fav)) fill_round_rect_aa(s, g.rc_bar_fav.x, g.rc_bar_fav.y, 32, 32, 16, imm_hov);
    const float bsc = (g.playing_idx >= 0) ? bounce_scale(g.fav_bounce_t) : 1.0f;
    const int ihsz = std::clamp(static_cast<int>(18.0f * bsc + 0.5f), 14, 24);
    icon_heart_ex(s, g.rc_bar_fav.x + (32 - ihsz) / 2, g.rc_bar_fav.y + (32 - ihsz) / 2,
                  ihsz, is_fav ? g.C_HEART : imm_txt2, is_fav);

    draw_text_max_bold(s, f, 22.0f, info_x, ty, imm_txt, g.p_title.c_str(), info_w - 38);
    draw_text_max_bold(s, f, 14.5f, info_x, ty + 30, imm_txt2,
                       !g.p_artist.empty() ? g.p_artist.c_str()
                                           : (!g.p_album.empty() ? g.p_album.c_str()
                                                                 : (g.playing_online ? "云端在线曲目" : "Sir\xC3\xA8ne Hi-Res")), info_w);

    const double dur = active_track_dur();
    const double pos = active_track_pos();
    const float frac = (dur > 0.1) ? clampf(static_cast<float>(pos / dur), 0.0f, 1.0f) : 0.0f;
    const int py = ty + 68;
    g.ui_px0 = info_x;
    g.ui_px1 = info_x + info_w;
    g.ui_py = py;
    g.rc_bar_prog = Rect{ g.ui_px0 - 8, py - 14, info_w + 16, 28 };
    const bool hov_prog = in_rc(mx, my, g.rc_bar_prog) || g.scrub_drag == 1;
    const int bh = hov_prog ? 7 : 5;
    fill_round_rect_aa(s, g.ui_px0, py - bh / 2, info_w, bh, bh / 2, imm_trk_bg);
    const int fpw = static_cast<int>(frac * static_cast<float>(info_w));
    if (fpw > 0) fill_round_rect_aa(s, g.ui_px0, py - bh / 2, fpw, bh, bh / 2, imm_accent);
    fill_ellipse_aa(s, g.ui_px0 + fpw, py, hov_prog ? 7 : 5, hov_prog ? 7 : 5, imm_accent);
    if (hov_prog && dur > 0.5 && mx >= g.ui_px0 - 4 && mx <= g.ui_px1 + 4) {
        const float hf = clampf(static_cast<float>(mx - g.ui_px0) / static_cast<float>(std::max(1, info_w)), 0.0f, 1.0f);
        char hbuf[16]{};
        fmt_time(static_cast<double>(hf) * dur, hbuf, sizeof(hbuf));
        const int htw = text_width_bold(f, 11.5f, hbuf) + 14;
        const int htx = std::clamp(mx - htw / 2, g.ui_px0, g.ui_px1 - htw);
        fill_round_rect_aa(s, htx, py - 28, htw, 20, 6, COL(12, 16, 24, 228));
        draw_text_centered_bold(s, f, 11.5f, htx, htx + htw, py - 25, imm_txt, hbuf);
    }

    char bpos[20]{}, bdur[20]{};
    fmt_time(pos, bpos, sizeof(bpos));
    fmt_time(dur, bdur, sizeof(bdur));
    draw_text_bold(s, f, 12.0f, g.ui_px0, py + 11, imm_txt3, bpos);
    draw_text_right_bold(s, f, 12.0f, g.ui_px1, py + 11, imm_txt3, bdur);

    const int cy = py + 48;
    const int cx = info_x + info_w / 2;
    g.rc_bar_play = Rect{ cx - 23, cy - 23, 46, 46 };
    g.rc_bar_prev = Rect{ cx - 74, cy - 18, 36, 36 };
    g.rc_bar_next = Rect{ cx + 38, cy - 18, 36, 36 };

    if (in_rc(mx, my, g.rc_bar_prev)) fill_round_rect_aa(s, g.rc_bar_prev.x, g.rc_bar_prev.y, 36, 36, 18, imm_hov);
    icon_prev(s, cx - 56, cy, 13, imm_txt);

    const bool hov_p = in_rc(mx, my, g.rc_bar_play);
    fill_ellipse_aa(s, cx, cy, hov_p ? 23 : 22, hov_p ? 23 : 22, imm_accent);
    draw_morph_play_pause(s, static_cast<float>(cx), static_cast<float>(cy), 18.0f,
                          has_track ? g.play_morph_t : 0.0f, imm_on_accent);

    if (in_rc(mx, my, g.rc_bar_next)) fill_round_rect_aa(s, g.rc_bar_next.x, g.rc_bar_next.y, 36, 36, 18, imm_hov);
    icon_next(s, cx + 56, cy, 13, imm_txt);

    const int vy = cy + 40;
    char spec[64]{};
    if (g.playing_online) snprintf(spec, sizeof(spec), "CLOUD · %s  %ukHz", g.p_fmt, audio_sample_rate() / 1000u);
    else                  snprintf(spec, sizeof(spec), "%s  %ukHz", g.p_fmt, audio_sample_rate() / 1000u);
    const int spw = text_width_bold(f, 11.5f, spec) + 18;
    fill_frosted_round_rect(s, info_x, vy - 13, spw, 26, 13,
                            imm_surface, 146, 1, 48);
    draw_text_centered_bold(s, f, 11.5f, info_x, info_x + spw, vy - 7, imm_accent, spec);

    const char* ml = g.play_mode == MODE_LOOP_ONE ? "单曲" : (g.play_mode == MODE_SHUFFLE ? "随机" : "顺序");
    g.rc_bar_mode = Rect{ info_x + info_w - 50, vy - 13, 50, 26 };
    const bool hov_m = in_rc(mx, my, g.rc_bar_mode);
    fill_frosted_round_rect(s, g.rc_bar_mode.x, g.rc_bar_mode.y, 50, 26, 13,
                            imm_surface, hov_m ? 184 : 142, 1, 52);
    draw_text_centered_bold(s, f, 12.0f, g.rc_bar_mode.x, g.rc_bar_mode.x + 50, vy - 7, imm_txt2, ml);

    if (g.rc_bar_mode.x - (info_x + spw) >= 88) {
        const int vw_tot = std::min(72, g.rc_bar_mode.x - (info_x + spw) - 34);
        g.ui_vx1 = g.rc_bar_mode.x - 10;
        g.ui_vx0 = g.ui_vx1 - vw_tot;
        g.rc_bar_vol_trk = Rect{ g.ui_vx0 - 6, vy - 14, vw_tot + 12, 28 };
        g.rc_bar_vol_ico = Rect{ g.ui_vx0 - 28, vy - 14, 22, 28 };
        const float vol = audio_volume();
        icon_vol(s, g.ui_vx0 - 17, vy, 12, imm_txt2, vol <= 0.01f);
        fill_round_rect_aa(s, g.ui_vx0, vy - 2, vw_tot, 4, 2, imm_trk_bg);
        const int vfill = static_cast<int>(vol * static_cast<float>(vw_tot));
        if (vfill > 0) fill_round_rect_aa(s, g.ui_vx0, vy - 2, vfill, 4, 2, imm_accent);
        fill_ellipse_aa(s, g.ui_vx0 + vfill, vy, 5, 5, imm_accent);
    }

    const int col_gap = std::clamp(content_w / 18, 40, 56);
    const int lyr_x = cov_x + cov_sz + col_gap;
    const int lyr_w = std::max(240, (content_x0 + content_w) - lyr_x);
    constexpr int lyr_y0 = 70;
    const int lyr_y1 = g.H - 42;
    g.ui_list_x0 = std::max(0, lyr_x - 16);
    g.ui_list_x1 = std::min(s->w, lyr_x + lyr_w + 16);
    g.ui_list_y0 = lyr_y0;
    g.ui_list_y1 = lyr_y1;

    // Snapshot top & bottom backdrop strips so pixel-smooth edge mask matches CSS mask-image
    const int mask_w = std::max(0, g.ui_list_x1 - g.ui_list_x0);
    const int fade_h = (mask_w > 0 && lyr_y1 - lyr_y0 > 80) ? std::min(92, (lyr_y1 - lyr_y0) / 4) : 0;
    std::vector<uint32_t> mask_top_bg, mask_bot_bg;
    if (fade_h > 0 && s->px) {
        mask_top_bg.resize(static_cast<size_t>(mask_w) * fade_h);
        mask_bot_bg.resize(static_cast<size_t>(mask_w) * fade_h);
        for (int ry = 0; ry < fade_h; ++ry) {
            std::memcpy(mask_top_bg.data() + static_cast<size_t>(ry) * mask_w,
                        s->px + static_cast<size_t>(lyr_y0 + ry) * s->w + g.ui_list_x0,
                        static_cast<size_t>(mask_w) * 4);
            std::memcpy(mask_bot_bg.data() + static_cast<size_t>(ry) * mask_w,
                        s->px + static_cast<size_t>(lyr_y1 - fade_h + ry) * s->w + g.ui_list_x0,
                        static_cast<size_t>(mask_w) * 4);
        }
    }

    canvas_set_clip(s, g.ui_list_x0, lyr_y0, g.ui_list_x1, lyr_y1);

    const float orig_px = std::round(clampf(std::min(static_cast<float>(g.W) * 0.025f, static_cast<float>(lyr_w) * 0.062f), 26.0f, 36.0f));
    const float cur_orig_px = std::round(orig_px * 1.13f);
    const float sub_px  = std::round(clampf(orig_px * 0.62f, 17.0f, 23.0f));
    const float cur_sub_px  = std::round(sub_px * 1.08f);
    g.lyr_springs_busy = 0;

    if (!g.lyr || g.lyr->n <= 0) {
        g.lyr_max_scroll = 0.0f;
        const char* msg = lyrics_online_busy(g.lyr_req_id) ? "正在从云端匹配双语歌词..." :
                          (has_track ? "当前曲目暂无内嵌/匹配歌词，可将 .lrc 拖入窗口" : "请先选择一首音乐开始播放");
        draw_text_max_bold(s, f, 26.0f, lyr_x, g.H / 2 - 20, imm_txt2, msg, lyr_w);
    } else {
        const int max_n = g.lyr->n;
        const int cur_idx = std::clamp(lyrics_index(g.lyr.get(), pos), 0, max_n - 1);

        if (g.lyr_wrap_ptr != g.lyr.get() || g.lyr_wrap_w != (lyr_w - 16) ||
            std::fabs(g.lyr_wrap_opx - cur_orig_px) > 0.25f || std::fabs(g.lyr_wrap_spx - cur_sub_px) > 0.25f ||
            static_cast<int>(g.lyr_wrapped.size()) != max_n) {
            g.lyr_wrapped.resize(static_cast<size_t>(max_n));
            int cur_acc_y = 0;
            for (int i = 0; i < max_n; ++i) {
                auto& wl = g.lyr_wrapped[i];
                const char* orig_s = g.lyr->lines[i].orig[0] ? g.lyr->lines[i].orig : g.lyr->lines[i].text;
                wl.o_lines = wrap_utf8_lines(orig_s, cur_orig_px, lyr_w - 16, 4, 1);
                if (wl.o_lines.empty()) wl.o_lines.emplace_back(orig_s);
                if (g.lyr->lines[i].sub[0]) wl.s_lines = wrap_utf8_lines(g.lyr->lines[i].sub, cur_sub_px, lyr_w - 16, 3, 1);
                else wl.s_lines.clear();
                const int no = static_cast<int>(wl.o_lines.size());
                const int ns = static_cast<int>(wl.s_lines.size());
                wl.h = no * static_cast<int>(orig_px * 1.32f) + (ns > 0 ? ns * static_cast<int>(sub_px * 1.34f) + 6 : 0) + 32;
                wl.y = cur_acc_y;
                wl.anim_init = 0;
                cur_acc_y += wl.h;
            }
            g.lyr_wrap_ptr = g.lyr.get();
            g.lyr_wrap_w   = lyr_w - 16;
            g.lyr_wrap_opx = cur_orig_px;
            g.lyr_wrap_spx = cur_sub_px;
            g.lyr_last_cur = -1;
            const auto& last_wl = g.lyr_wrapped[max_n - 1];
            g.lyr_max_scroll = static_cast<float>(last_wl.y + last_wl.h);
        }

        if (g.lyr_manual_timer > 0.0f) {
            g.lyr_manual_timer -= static_cast<float>(g.dt);
        } else {
            const float target_y = static_cast<float>(g.lyr_wrapped[cur_idx].y) + static_cast<float>(g.lyr_wrapped[cur_idx].h) * 0.35f;
            if (!g.lyr_cam_valid) { g.lyr_cam_y = target_y; g.lyr_cam_valid = 1; }
            else g.lyr_cam_y += (target_y - g.lyr_cam_y) * (1.0f - std::exp(-7.5f * static_cast<float>(g.dt)));
        }
        g.lyr_cam_y = clampf(g.lyr_cam_y, 0.0f, std::max(0.0f, g.lyr_max_scroll));

        const int anchor_y = lyr_y0 + static_cast<int>((lyr_y1 - lyr_y0) * 0.40f);
        const bool line_switched = (cur_idx != g.lyr_last_cur);
        const bool manual_snap = (g.drag_mode == DRAG_LYRICS) || (std::fabs(g.lyr_scroll_vel) > 5.0f) || (g.lyr_manual_timer > 3.8f);
        const float dt_f = clampf(static_cast<float>(g.dt), 0.002f, 0.05f);
        if (line_switched) g.lyr_last_cur = cur_idx;

        for (int i = 0; i < max_n; ++i) {
            auto& wl = g.lyr_wrapped[i];
            const float tgt_sy = static_cast<float>(anchor_y + wl.y) - g.lyr_cam_y;
            const bool is_cur = (i == cur_idx);
            const int dist = std::abs(i - cur_idx);
            const float tgt_scale = is_cur ? 1.13f : 1.00f;
            const float base_alpha = is_cur ? 1.0f : clampf(0.35f - static_cast<float>(std::max(0, dist - 1)) * 0.045f, 0.16f, 0.35f);

            if (!wl.anim_init) {
                wl.spring_y = tgt_sy; wl.spring_vy = 0.0f;
                wl.cur_scale = wl.scale_from = wl.scale_to = tgt_scale;
                wl.cur_alpha = wl.alpha_from = wl.alpha_to = base_alpha;
                wl.scale_t0 = wl.anim_dur = 0.42f;
                wl.cur_lift = wl.lift_from = wl.lift_to = is_cur ? -3.0f : 0.0f;
                wl.anim_init = 1;
            } else if (line_switched && dist <= 10) {
                // Klang LyricsStageControl staggered quintic delay cascade
                const int k = std::min(dist, 6);
                const float kf = static_cast<float>(k);
                const float delay = (i >= cur_idx)
                    ? (0.028f * (kf + 0.6f * kf * (1.0f - kf / 7.0f)))
                    : (0.014f * kf);
                wl.scale_from = wl.cur_scale;
                wl.scale_to = tgt_scale;
                wl.alpha_from = wl.cur_alpha;
                wl.alpha_to = base_alpha;
                wl.lift_from = wl.cur_lift;
                wl.lift_to = is_cur ? -3.0f : 0.0f;
                wl.scale_t0 = -delay;
                wl.anim_dur = is_cur ? 0.44f : 0.36f;
            }

            // Advance quintic stagger timer
            if (wl.scale_t0 < wl.anim_dur) {
                wl.scale_t0 += dt_f;
                g.lyr_springs_busy = 1;
                if (wl.scale_t0 >= 0.0f) {
                    const float e = quintic_eval(wl.scale_t0 / std::max(0.05f, wl.anim_dur));
                    wl.cur_scale = wl.scale_from + (wl.scale_to - wl.scale_from) * e;
                    wl.cur_alpha = wl.alpha_from + (wl.alpha_to - wl.alpha_from) * e;
                    wl.cur_lift  = wl.lift_from  + (wl.lift_to  - wl.lift_from)  * e;
                }
            } else {
                wl.cur_scale = tgt_scale;
                wl.cur_alpha = base_alpha;
                wl.cur_lift  = is_cur ? -3.0f : 0.0f;
            }

            // Advance damped harmonic Y spring after stagger gate opens
            if (manual_snap || dist > 12) {
                wl.spring_y = tgt_sy; wl.spring_vy = 0.0f;
            } else if (wl.scale_t0 >= 0.0f) {
                spring_step(wl.spring_y, wl.spring_vy, tgt_sy, dt_f, 11.2f, 0.80f);
                if (std::fabs(wl.spring_y - tgt_sy) > 0.25f || std::fabs(wl.spring_vy) > 0.5f) {
                    g.lyr_springs_busy = 1;
                }
            } else {
                g.lyr_springs_busy = 1;
            }

            const int y = static_cast<int>(std::round(wl.spring_y));
            if (y + wl.h < lyr_y0 - 40 || y > lyr_y1 + 40) continue;

            // Frameless fluid typography: pure white + quintic overshoot scale on current, <= 35% opacity on others
            float alpha = wl.cur_alpha;
            const bool hov_l = in_rect(mx, my, lyr_x - 8, y - 4, lyr_w, wl.h - 10);
            if (!is_cur && hov_l) alpha = 0.72f;

            const uint8_t oa = static_cast<uint8_t>(clampf(alpha * 255.0f, 4.0f, 255.0f));
            const uint8_t sa = static_cast<uint8_t>(clampf(alpha * 222.0f, 4.0f, 255.0f));
            const uint32_t col_orig = with_alpha(COL(255, 255, 255, 255), oa);
            const uint32_t col_sub  = with_alpha(COL(240, 244, 252, 255), sa);
            const float draw_opx = clampf(std::round(orig_px * wl.cur_scale * 2.0f) * 0.5f, orig_px, cur_orig_px + 2.0f);
            const float draw_spx = clampf(std::round(sub_px * (1.0f + (wl.cur_scale - 1.0f) * 0.62f) * 2.0f) * 0.5f, sub_px, cur_sub_px + 1.0f);
            const int y_nudge = static_cast<int>(std::round(wl.cur_lift));

            int cur_y = y + y_nudge;
            for (const auto& line : wl.o_lines) {
                draw_text_bold(s, f, draw_opx, lyr_x, cur_y, col_orig, line.c_str());
                cur_y += static_cast<int>(draw_opx * 1.24f);
            }
            if (!wl.s_lines.empty()) {
                cur_y += 4;
                for (const auto& line : wl.s_lines) {
                    if (is_cur) draw_text_bold(s, f, draw_spx, lyr_x, cur_y, col_sub, line.c_str());
                    else        draw_text(s, f, draw_spx, lyr_x, cur_y, col_sub, line.c_str());
                    cur_y += static_cast<int>(draw_spx * 1.28f);
                }
            }

            // Klang Interlude Breathing Dots when waiting across a long musical gap (>= 4.5s)
            if (is_cur && i + 1 < max_n) {
                const double gap_t = g.lyr->lines[i + 1].t - g.lyr->lines[i].t;
                if (gap_t >= 4.5 && (pos - g.lyr->lines[i].t) > 1.8 && (g.lyr->lines[i + 1].t - pos) > 0.8) {
                    const float gp = clampf(static_cast<float>((pos - g.lyr->lines[i].t) / gap_t), 0.0f, 1.0f);
                    for (int d = 0; d < 3; ++d) {
                        const float pulse_d = 0.52f + 0.48f * std::sin(g.wave_phase * 2.2f - static_cast<float>(d) * 0.9f);
                        const float fill_d  = clampf((gp - static_cast<float>(d) * 0.28f) * 3.2f, 0.25f, 1.0f);
                        const uint8_t da = static_cast<uint8_t>(clampf((0.40f + 0.60f * pulse_d * fill_d) * 230.0f, 40.0f, 245.0f));
                        const int r = (pulse_d > 0.72f) ? 5 : 4;
                        fill_ellipse_aa(s, lyr_x + 8 + d * 18, cur_y + 8, r, r, with_alpha(COL(255, 255, 255, 255), da));
                    }
                    g.lyr_springs_busy = 1;
                }
            }

            if (static_cast<int>(g.lyr_hits.size()) < LYRIC_MAX_HITS) {
                g.lyr_hits.push_back(LyrHit{ Rect{ lyr_x - 8, y - 4, lyr_w, wl.h - 10 }, g.lyr->lines[i].t });
            }
        }
    }
    canvas_reset_clip(s);

    // Apply smooth pixel-level top/bottom gradient fade mask over the lyrics container
    if (fade_h > 0 && s->px && !mask_top_bg.empty()) {
        for (int ry = 0; ry < fade_h; ++ry) {
            const float kt = smooth_u(static_cast<float>(ry) / static_cast<float>(fade_h));
            const uint32_t kfg = static_cast<uint32_t>(std::clamp(static_cast<int>(kt * 256.0f + 0.5f), 0, 256));
            const uint32_t kbg = 256u - kfg;
            uint32_t* drow = s->px + static_cast<size_t>(lyr_y0 + ry) * s->w + g.ui_list_x0;
            const uint32_t* brow = mask_top_bg.data() + static_cast<size_t>(ry) * mask_w;
            for (int rx = 0; rx < mask_w; ++rx) {
                const uint32_t fg = drow[rx], bg = brow[rx];
                const uint32_t rb = ((((fg & 0x00FF00FFu) * kfg + (bg & 0x00FF00FFu) * kbg + 0x00800080u) >> 8) & 0x00FF00FFu);
                const uint32_t gv = (((((fg >> 8) & 0xFFu) * kfg + ((bg >> 8) & 0xFFu) * kbg + 128u) & 0xFF00u));
                drow[rx] = 0xFF000000u | rb | gv;
            }

            const float kb_f = smooth_u(static_cast<float>(fade_h - 1 - ry) / static_cast<float>(fade_h));
            const uint32_t bfg = static_cast<uint32_t>(std::clamp(static_cast<int>(kb_f * 256.0f + 0.5f), 0, 256));
            const uint32_t bbg = 256u - bfg;
            uint32_t* bdrow = s->px + static_cast<size_t>(lyr_y1 - fade_h + ry) * s->w + g.ui_list_x0;
            const uint32_t* bbrow = mask_bot_bg.data() + static_cast<size_t>(ry) * mask_w;
            for (int rx = 0; rx < mask_w; ++rx) {
                const uint32_t fg = bdrow[rx], bg = bbrow[rx];
                const uint32_t rb = ((((fg & 0x00FF00FFu) * bfg + (bg & 0x00FF00FFu) * bbg + 0x00800080u) >> 8) & 0x00FF00FFu);
                const uint32_t gv = (((((fg >> 8) & 0xFFu) * bfg + ((bg >> 8) & 0xFFu) * bbg + 128u) & 0xFF00u));
                bdrow[rx] = 0xFF000000u | rb | gv;
            }
        }
    }
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
        draw_text_bold(s, f, 15.0f, cx0 + 20, y + 16, g.C_TXT, "Sir\xC3\xA8ne v1.2.8  ·  本地无损音频播放器");
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

void draw_plugins_view(int mx, int my) {
    Canvas* s = g.screen.get();
    Font* f = g.font.get();
    const bool lt = g.light_t > 0.5f;
    const int lx = side_w() + 26;
    const int rx = g.W - 26;

    g.rc_plg_back = Rect{ lx, 22, 88, 38 };
    const bool hov_back = in_rc(mx, my, g.rc_plg_back);
    fill_frosted_round_rect(s, g.rc_plg_back.x, g.rc_plg_back.y, g.rc_plg_back.w, g.rc_plg_back.h, 19,
                            g.C_SURFACE, hov_back ? 188 : (lt ? 156 : 132), 2, glass_rim(56, 210));
    stroke_round_rect_aa(s, g.rc_plg_back.x, g.rc_plg_back.y, g.rc_plg_back.w, g.rc_plg_back.h, 19, 1,
                         g.C_DIV, col_a(g.C_DIV) + 12);
    icon_back(s, g.rc_plg_back.x + 14, g.rc_plg_back.y + 12, 13, g.C_TXT);
    draw_text_bold(s, f, 13.5f, g.rc_plg_back.x + 34, g.rc_plg_back.y + 10, g.C_TXT, "返回");
    draw_text_bold(s, f, 23.5f, lx + 104, 26, g.C_TXT, g.plg_subtab == 0 ? "云端音乐" : "扩展管理");

    auto reg_hit = [&](const Rect& r, int id, int arg, const std::string& key = "") {
        g.plg_hits.push_back(SetHit{ r, id, arg });
        g.plg_hit_keys.push_back(key);
    };

    const int seg_w = 176;
    const int seg_x = rx - seg_w;
    {
        fill_frosted_round_rect(s, seg_x, 25, seg_w, 32, 16, g.C_SURFACE, lt ? 148 : 118, 1, glass_rim(42, 185));
        const char* tabs[2] = { "浏览音源", "管理扩展" };
        for (int i = 0; i < 2; ++i) {
            Rect trc{ seg_x + 3 + i * 85, 28, 85, 26 };
            reg_hit(trc, i == 0 ? PLG_TAB_BROWSE : PLG_TAB_MANAGE, i);
            const bool act = (g.plg_subtab == i);
            const bool hov = in_rc(mx, my, trc);
            if (act) {
                fill_round_rect_aa(s, trc.x, trc.y, trc.w, trc.h, 13, mix_col(g.C_SURFACE, g.C_ACCENT, 0.30f));
                stroke_round_rect_aa(s, trc.x, trc.y, trc.w, trc.h, 13, 1, g.C_ACCENT, 145);
            } else if (hov) {
                fill_round_rect_aa(s, trc.x, trc.y, trc.w, trc.h, 13, g.C_GLASS_HOV);
            }
            draw_text_centered_bold(s, f, 12.5f, trc.x, trc.x + trc.w, trc.y + 5,
                                    act ? g.C_ACCENT : (hov ? g.C_TXT : g.C_TXT2), tabs[i]);
        }
    }

    constexpr int top_y = HEADER_H;
    const int vh = list_vis_h();
    const int avail_w = std::max(320, rx - lx - 14);
    g.ui_list_x0 = lx; g.ui_list_x1 = lx + avail_w; g.ui_list_y0 = top_y; g.ui_list_y1 = top_y + vh;
    canvas_set_clip(s, lx - 8, top_y, rx, top_y + vh);

    const int start_y = top_y + 4 - static_cast<int>(g.scroll_px);
    int y = start_y;
    auto vis_hit = [&](const Rect& r) { return r.y + r.h > top_y && r.y < top_y + vh; };

    if (g.plg_subtab == 1 || g.plg_list.empty()) {
        const int card_w = std::min(840, avail_w);
        const int cx0 = lx + (avail_w - card_w) / 2;

        fill_round_rect_shadow(s, cx0, y + 3, card_w, 86, 16, theme_shadow(54));
        fill_frosted_round_rect(s, cx0, y, card_w, 86, 16, g.C_SURFACE, lt ? 186 : 162, 3, glass_rim(54, 210));
        stroke_round_rect_aa(s, cx0, y, card_w, 86, 16, 1, g.C_DIV, col_a(g.C_DIV) + 14);
        draw_text_bold(s, f, 15.0f, cx0 + 20, y + 14, g.C_TXT, "外部扩展插件目录（直接将 .lua 文件拖入窗口即可安装）");
        std::string pdir = plugin_dir_path();
        draw_text_max(s, f, 12.0f, cx0 + 20, y + 40, g.C_TXT2, pdir.c_str(), card_w - 240);

        Rect rs_rc{ cx0 + card_w - 20 - 82, y + 27, 82, 32 };
        Rect cl_rc{ rs_rc.x - 8 - 88, y + 27, 88, 32 };
        {
            const bool hov = vis_hit(rs_rc) && in_rc(mx, my, rs_rc);
            fill_frosted_round_rect(s, rs_rc.x, rs_rc.y, rs_rc.w, rs_rc.h, 16,
                                    mix_col(g.C_SURFACE, g.C_ACCENT, 0.22f), hov ? 204 : 184, 1, glass_rim(56, 210));
            draw_text_centered_bold(s, f, 12.5f, rs_rc.x, rs_rc.x + rs_rc.w, rs_rc.y + 7, g.C_ACCENT, "扫描插件");
            if (vis_hit(rs_rc)) reg_hit(rs_rc, PLG_RESCAN, 0);
        }
        {
            const bool hov = vis_hit(cl_rc) && in_rc(mx, my, cl_rc);
            fill_frosted_round_rect(s, cl_rc.x, cl_rc.y, cl_rc.w, cl_rc.h, 16, g.C_SURFACE,
                                    hov ? 188 : (lt ? 156 : 132), 1, glass_rim(48, 200));
            draw_text_centered_bold(s, f, 12.5f, cl_rc.x, cl_rc.x + cl_rc.w, cl_rc.y + 7, g.C_TXT2, "清理缓存");
            if (vis_hit(cl_rc)) reg_hit(cl_rc, PLG_CLEAR_CACHE, 0);
        }
        y += 102;

        if (g.plg_list.empty()) {
            fill_round_rect_shadow(s, cx0, y + 3, card_w, 124, 16, theme_shadow(48));
            fill_frosted_round_rect(s, cx0, y, card_w, 124, 16, g.C_SURFACE, lt ? 176 : 150, 3, glass_rim(48, 195));
            draw_text_centered_bold(s, f, 16.0f, cx0, cx0 + card_w, y + 34, g.C_TXT, "尚未安装外部音源扩展");
            draw_text_centered_bold(s, f, 12.5f, cx0, cx0 + card_w, y + 66, g.C_TXT2,
                                    "可从外部导入兼容 Sirène Lua SDK 的 .lua 插件（支持搜索 / 歌单 / 登录 / 流媒体播放）");
            y += 138;
        } else {
            for (int pi = 0; pi < static_cast<int>(g.plg_list.size()); ++pi) {
                const auto& plg = g.plg_list[pi];
                const int nf = static_cast<int>(plg.config_fields.size());
                const int ch = 96 + nf * 48;
                fill_round_rect_shadow(s, cx0, y + 3, card_w, ch, 16, theme_shadow(54));
                fill_frosted_round_rect(s, cx0, y, card_w, ch, 16, g.C_SURFACE, lt ? 186 : 162, 3, glass_rim(54, 210));
                stroke_round_rect_aa(s, cx0, y, card_w, ch, 16, 1, g.C_DIV, col_a(g.C_DIV) + 14);

                char hdr[192]{};
                std::snprintf(hdr, sizeof(hdr), "%s  v%s%s%s",
                              plg.name.c_str(), plg.version.c_str(),
                              plg.author.empty() ? "" : " · ", plg.author.c_str());
                draw_text_max_bold(s, f, 15.5f, cx0 + 20, y + 14, g.C_TXT, hdr, card_w - 280);
                const char* desc = !plg.last_error.empty() ? plg.last_error.c_str()
                                 : (!plg.description.empty() ? plg.description.c_str() : plg.id.c_str());
                draw_text_max(s, f, 12.5f, cx0 + 20, y + 40, !plg.last_error.empty() ? g.C_HEART : g.C_TXT2, desc, card_w - 280);
                if (plg.logged_in) {
                    std::string lg = "已登录: " + plg.nickname;
                    draw_text_bold(s, f, 12.0f, cx0 + 20, y + 64, g.C_ACCENT, lg.c_str());
                } else {
                    draw_text_bold(s, f, 12.0f, cx0 + 20, y + 64, plg.enabled ? g.C_TXT3 : g.C_HEART,
                                   plg.enabled ? "已启用" : "已停用");
                }

                int bx = cx0 + card_w - 20;
                Rect rm_rc{ bx - 56, y + 18, 56, 30 };
                {
                    const bool hov = vis_hit(rm_rc) && in_rc(mx, my, rm_rc);
                    fill_frosted_round_rect(s, rm_rc.x, rm_rc.y, rm_rc.w, rm_rc.h, 15, g.C_SURFACE,
                                            hov ? 188 : (lt ? 150 : 126), 1, glass_rim(44, 190));
                    draw_text_centered_bold(s, f, 12.0f, rm_rc.x, rm_rc.x + rm_rc.w, rm_rc.y + 6,
                                            hov ? g.C_HEART : g.C_TXT2, "移除");
                    if (vis_hit(rm_rc)) reg_hit(rm_rc, PLG_REMOVE, pi, plg.id);
                    bx = rm_rc.x - 8;
                }
                Rect sw_rc{ bx - 44, y + 21, 44, 24 };
                draw_switch(sw_rc.x, sw_rc.y, plg.enabled ? 1.0f : 0.0f, g.C_ACCENT, mx, my);
                if (vis_hit(sw_rc)) reg_hit(Rect{ sw_rc.x - 4, sw_rc.y - 4, 52, 32 }, PLG_TOGGLE, pi, plg.id);
                bx = sw_rc.x - 10;

                if (plg.supports_login || plg.supports_cookie_login) {
                    Rect lg_rc{ bx - 84, y + 18, 84, 30 };
                    const bool hov = vis_hit(lg_rc) && in_rc(mx, my, lg_rc);
                    fill_frosted_round_rect(s, lg_rc.x, lg_rc.y, lg_rc.w, lg_rc.h, 15,
                                            mix_col(g.C_SURFACE, g.C_ACCENT, 0.22f), hov ? 204 : 184, 1, glass_rim(54, 210));
                    draw_text_centered_bold(s, f, 12.0f, lg_rc.x, lg_rc.x + lg_rc.w, lg_rc.y + 6, g.C_ACCENT,
                                            plg.logged_in ? "退出登录" : "登录绑定");
                    if (vis_hit(lg_rc)) reg_hit(lg_rc, plg.logged_in ? PLG_LOGOUT : PLG_LOGIN, pi, plg.id);
                }

                int fy = y + 92;
                for (int fi = 0; fi < nf; ++fi) {
                    const auto& cf = plg.config_fields[fi];
                    fill_rect_a(s, cx0 + 18, fy - 4, card_w - 36, 1, g.C_DIV, col_a(g.C_DIV) + 10);
                    draw_text_bold(s, f, 13.0f, cx0 + 20, fy + 10, g.C_TXT2, cf.label.c_str());
                    Rect in_rc_box{ cx0 + 160, fy + 4, card_w - 180, 32 };
                    const bool is_ed = (g.cfg_editing && g.cfg_edit_pid == plg.id && g.cfg_edit_key == cf.key);
                    const std::string& disp = is_ed ? g.cfg_edit_val : (!cf.value.empty() ? cf.value : cf.placeholder);
                    fill_frosted_round_rect(s, in_rc_box.x, in_rc_box.y, in_rc_box.w, in_rc_box.h, 10,
                                            g.C_SURFACE, is_ed ? 210 : (lt ? 160 : 134), 1,
                                            is_ed ? glass_rim(96, 235) : glass_rim(44, 185));
                    if (is_ed) stroke_round_rect_aa(s, in_rc_box.x, in_rc_box.y, in_rc_box.w, in_rc_box.h, 10, 1, g.C_ACCENT, 200);
                    draw_text_max(s, f, 12.5f, in_rc_box.x + 12, in_rc_box.y + 8,
                                  (is_ed || !cf.value.empty()) ? g.C_TXT : g.C_TXT3, disp.c_str(), in_rc_box.w - 24);
                    if (vis_hit(in_rc_box)) reg_hit(in_rc_box, PLG_CFG_EDIT, pi, plg.id + "\n" + cf.key);
                    fy += 48;
                }
                y += ch + 16;
            }
        }
    } else {
        // Apple-Music-inspired Cloud Hub: source pills + sound quality + account + inline search + covered shelves + modern rows
        int sx = lx;
        const PluginInfo* cur_p = nullptr;
        for (const auto& plg : g.plg_list) {
            if (!plg.enabled || !plg.valid) continue;
            const int pw = text_width_bold(f, 12.5f, plg.name.c_str()) + 26;
            Rect pr{ sx, y, pw, 32 };
            const bool act = (g.active_plugin_id == plg.id);
            const bool hov = vis_hit(pr) && in_rc(mx, my, pr);
            fill_frosted_round_rect(s, pr.x, pr.y, pr.w, pr.h, 16,
                                    act ? mix_col(g.C_SURFACE, g.C_ACCENT, 0.28f) : g.C_SURFACE,
                                    act ? 204 : (hov ? 186 : (lt ? 156 : 130)), 1, glass_rim(48, 200));
            if (act) stroke_round_rect_aa(s, pr.x, pr.y, pr.w, pr.h, 16, 1, g.C_ACCENT, 145);
            draw_text_centered_bold(s, f, 12.5f, pr.x, pr.x + pr.w, pr.y + 7, act ? g.C_ACCENT : g.C_TXT2, plg.name.c_str());
            if (vis_hit(pr)) reg_hit(pr, PLG_SELECT, 0, plg.id);
            sx += pw + 8;
            if (act) cur_p = &plg;
        }
        if (cur_p) {
            int rbx = lx + avail_w;
            if (cur_p->supports_login || cur_p->supports_cookie_login) {
                std::string ltxt = cur_p->logged_in ? ("账号: " + cur_p->nickname) : "连接账号";
                int lw = std::min(156, text_width_bold(f, 12.0f, ltxt.c_str()) + 26);
                Rect lr{ rbx - lw, y, lw, 32 };
                const bool hov = vis_hit(lr) && in_rc(mx, my, lr);
                fill_frosted_round_rect(s, lr.x, lr.y, lr.w, lr.h, 16,
                                        mix_col(g.C_SURFACE, g.C_ACCENT, cur_p->logged_in ? 0.16f : 0.24f),
                                        hov ? 204 : 182, 1, glass_rim(54, 210));
                if (cur_p->logged_in) stroke_round_rect_aa(s, lr.x, lr.y, lr.w, lr.h, 16, 1, g.C_ACCENT, 110);
                draw_text_centered_bold(s, f, 12.0f, lr.x, lr.x + lr.w, lr.y + 7, g.C_ACCENT, ltxt.c_str());
                if (vis_hit(lr)) reg_hit(lr, cur_p->logged_in ? PLG_LOGOUT : PLG_LOGIN, 0, cur_p->id);
                rbx = lr.x - 8;
            }
            // Sound quality segmented pill (标准 / 极高 / 无损)
            if (rbx - sx >= 164) {
                static constexpr const char* q_ids[3] = { "standard", "exhigh", "lossless" };
                static constexpr const char* q_lbls[3] = { "标准", "极高", "无损" };
                constexpr int q_tot = 144;
                Rect q_bg{ rbx - q_tot, y, q_tot, 32 };
                fill_frosted_round_rect(s, q_bg.x, q_bg.y, q_bg.w, q_bg.h, 16, g.C_SURFACE, lt ? 148 : 118, 1, glass_rim(42, 185));
                for (int qi = 0; qi < 3; ++qi) {
                    Rect qr_c{ q_bg.x + 3 + qi * 46, y + 3, 46, 26 };
                    const bool qact = (g.plg_quality == q_ids[qi]);
                    const bool qhov = vis_hit(qr_c) && in_rc(mx, my, qr_c);
                    if (qact) {
                        fill_round_rect_aa(s, qr_c.x, qr_c.y, qr_c.w, qr_c.h, 13, mix_col(g.C_SURFACE, g.C_ACCENT, 0.30f));
                        stroke_round_rect_aa(s, qr_c.x, qr_c.y, qr_c.w, qr_c.h, 13, 1, g.C_ACCENT, 135);
                    } else if (qhov) {
                        fill_round_rect_aa(s, qr_c.x, qr_c.y, qr_c.w, qr_c.h, 13, g.C_GLASS_HOV);
                    }
                    draw_text_centered_bold(s, f, 11.5f, qr_c.x, qr_c.x + qr_c.w, qr_c.y + 5,
                                            qact ? g.C_ACCENT : (qhov ? g.C_TXT : g.C_TXT2), q_lbls[qi]);
                    if (vis_hit(qr_c)) reg_hit(qr_c, PLG_QUALITY, qi, q_ids[qi]);
                }
                rbx = q_bg.x - 8;
            }
            if (rbx - sx >= 90) {
                Rect hr{ rbx - 82, y, 82, 32 };
                const bool hhov = vis_hit(hr) && in_rc(mx, my, hr);
                fill_frosted_round_rect(s, hr.x, hr.y, hr.w, hr.h, 16, g.C_SURFACE, hhov ? 186 : (lt ? 156 : 130), 1, glass_rim(48, 200));
                draw_text_centered_bold(s, f, 12.0f, hr.x, hr.x + hr.w, hr.y + 7, g.C_TXT2, "推荐歌单");
                if (vis_hit(hr)) reg_hit(hr, PLG_HOME, 0, cur_p->id);
            }
        }
        y += 44;

        // Inline Cloud Search capsule right inside Cloud Hub
        if (cur_p) {
            const int go_w = 78;
            const int sbox_w = std::clamp(avail_w - go_w - 10, 220, 560);
            Rect sb{ lx, y, sbox_w, 38 };
            g.rc_plg_search = sb;
            const bool shov = vis_hit(sb) && in_rc(mx, my, sb);
            fill_frosted_round_rect(s, sb.x, sb.y, sb.w, sb.h, 19, g.C_SURFACE,
                                    (g.plg_search_focused || shov) ? (lt ? 214 : 184) : (lt ? 168 : 136), 2,
                                    g.plg_search_focused ? glass_rim(96, 235) : glass_rim(44, 185));
            if (g.plg_search_focused) stroke_round_rect_aa(s, sb.x, sb.y, sb.w, sb.h, 19, 1, g.C_ACCENT, 190);
            icon_search(s, sb.x + 19, sb.y + 18, 6, g.plg_search_focused ? g.C_ACCENT : g.C_TXT2);
            const std::string& disp_q = !g.plg_search_q.empty() ? g.plg_search_q : g.search_q;
            if (!disp_q.empty()) {
                draw_text_max_bold(s, f, 13.0f, sb.x + 36, sb.y + 10, g.C_TXT, disp_q.c_str(), sb.w - 68);
                Rect clr{ sb.x + sb.w - 30, sb.y + 7, 24, 24 };
                g.rc_plg_search_clr = clr;
                const bool chov = vis_hit(clr) && in_rc(mx, my, clr);
                if (chov) fill_round_rect_aa(s, clr.x, clr.y, 24, 24, 12, g.C_GLASS_HOV);
                icon_close_x(s, clr.x + 12, clr.y + 12, 3, chov ? g.C_HEART : g.C_TXT3);
            } else {
                draw_text_max(s, f, 13.0f, sb.x + 36, sb.y + 10, g.C_TXT3,
                              "搜索云端歌曲、歌手或专辑（回车直达）", sb.w - 48);
            }
            Rect go_rc{ sb.x + sb.w + 10, y + 1, go_w, 36 };
            const bool ghov = vis_hit(go_rc) && in_rc(mx, my, go_rc);
            fill_round_rect_aa(s, go_rc.x, go_rc.y, go_rc.w, go_rc.h, 18,
                               ghov ? mix_col(g.C_ACCENT, COL(255, 255, 255, 255), 0.14f) : g.C_ACCENT);
            draw_text_centered_bold(s, f, 13.0f, go_rc.x, go_rc.x + go_rc.w, go_rc.y + 9, g.C_ON_ACCENT, "搜在线");
            if (vis_hit(go_rc)) reg_hit(go_rc, PLG_SEARCH_GO, 0, cur_p->id);
            y += 52;
        }

        // Featured Playlists Shelf: Apple Music square covered cards
        if (!g.plg_snap.playlists.empty()) {
            draw_text_bold(s, f, 16.0f, lx, y, g.C_TXT, "推荐与收藏歌单");
            y += 30;
            constexpr int pcard_w = 132;
            constexpr int pcard_h = 178;
            constexpr int pgap = 14;
            const int pcols = std::max(2, (avail_w + pgap) / (pcard_w + pgap));
            int p_thumb_q = 0;
            const int np = static_cast<int>(g.plg_snap.playlists.size());
            const int show_np = std::min(np, pcols * 2);
            const int prows = (show_np + pcols - 1) / pcols;
            for (int i = 0; i < show_np; ++i) {
                auto& pl = g.plg_snap.playlists[i];
                if (pl.thumb_state == 0 && !pl.cover_url.empty() && p_thumb_q < 6) {
                    pl.thumb_state = 1;
                    plugin_async_playlist_thumb(pl.plugin_id, static_cast<size_t>(i), pl.cover_url);
                    ++p_thumb_q;
                }
                const int pr_idx = i / pcols, pc_idx = i % pcols;
                const int px = lx + pc_idx * (pcard_w + pgap);
                const int py = y + pr_idx * (pcard_h + pgap);
                Rect card{ px, py, pcard_w, pcard_h };
                if (card.y + card.h > top_y - 16 && card.y < top_y + vh + 16) {
                    const bool hov = vis_hit(card) && in_rc(mx, my, card);
                    const int cy = hov ? (py - 3) : py;
                    fill_round_rect_shadow_fast(s, px, cy + 3, pcard_w, pcard_w, 14, theme_shadow(64));
                    if (!pl.thumb.empty()) {
                        blit_rgb_round_fit(s, px, cy, pcard_w, pcard_w, pl.thumb.data(), 160, 160, 14);
                    } else if (!g.poster_hero.empty()) {
                        blit_rgb_round_fit(s, px, cy, pcard_w, pcard_w, g.poster_hero.data(), COVER_SRC_SZ, COVER_SRC_SZ, 14);
                        fill_round_rect_aa_a(s, px, cy, pcard_w, pcard_w, 14, COL(12, 16, 24, 255), 88);
                    } else {
                        fill_round_rect_aa(s, px, cy, pcard_w, pcard_w, 14, g.C_GLASS_ACT);
                    }
                    if (!pl.category.empty()) {
                        const char* cat = (pl.category == "mine") ? "我的" : (pl.category == "daily" ? "每日" : "推荐");
                        const int cw_b = text_width_bold(f, 10.5f, cat) + 12;
                        fill_round_rect_aa_a(s, px + 8, cy + 8, cw_b, 18, 9, COL(12, 16, 24, 255), 178);
                        draw_text_centered_bold(s, f, 10.5f, px + 8, px + 8 + cw_b, cy + 10, COL(244, 248, 255, 255), cat);
                    }
                    if (hov) stroke_round_rect_aa(s, px, cy, pcard_w, pcard_w, 14, 2, g.C_ACCENT, 195);
                    draw_text_max_bold(s, f, 13.0f, px + 2, cy + pcard_w + 8, hov ? g.C_ACCENT : g.C_TXT,
                                       pl.name.c_str(), pcard_w - 4);
                    if (pl.track_count > 0) {
                        char tcb[32]{};
                        std::snprintf(tcb, sizeof(tcb), "%d 首", pl.track_count);
                        draw_text_max(s, f, 11.0f, px + 2, cy + pcard_w + 27, g.C_TXT3, tcb, pcard_w - 4);
                    }
                    if (vis_hit(card)) reg_hit(card, PLG_PLAYLIST, i, pl.id);
                }
            }
            y += prows * pcard_h + std::max(0, prows - 1) * pgap + 14;
        }

        char sec[128]{};
        std::snprintf(sec, sizeof(sec), "%s · %d 首",
                      g.plg_snap.active_playlist_name.empty() ? "在线曲目" : g.plg_snap.active_playlist_name.c_str(),
                      static_cast<int>(g.plg_snap.tracks.size()));
        draw_text_bold(s, f, 16.5f, lx, y, g.C_TXT, sec);
        if (g.plg_snap.is_busy) draw_text_right_bold(s, f, 12.5f, lx + avail_w, y + 3, g.C_ACCENT, "正在连接数据源...");
        y += 34;

        if (g.plg_snap.is_busy && g.plg_snap.tracks.empty()) {
            // Klang SkeletonList shimmer loader rows
            for (int si = 0; si < 6; ++si) {
                const int sy = y + si * 58;
                if (sy + 54 < top_y || sy > top_y + vh) continue;
                const float ph = g.wave_phase + static_cast<float>(si) * 0.25f;
                draw_skeleton_shimmer(s, lx + 40, sy + 8, 42, 42, 10, ph, lt);
                draw_skeleton_shimmer(s, lx + 96, sy + 12, std::min(220, avail_w / 3), 14, 7, ph, lt);
                draw_skeleton_shimmer(s, lx + 96, sy + 32, std::min(140, avail_w / 4), 11, 5, ph, lt);
                draw_skeleton_shimmer(s, lx + avail_w - 64, sy + 21, 44, 12, 6, ph, lt);
            }
            y += 6 * 58 + 12;
        } else if (g.plg_snap.tracks.empty()) {
            fill_frosted_round_rect(s, lx, y, avail_w, 92, 16, g.C_SURFACE, lt ? 160 : 130, 2, glass_rim(44, 190));
            draw_text_centered_bold(s, f, 14.5f, lx, lx + avail_w, y + 34, g.C_TXT3,
                                    "在上方搜索框输入歌名 / 歌手按回车，或点击「推荐歌单 / 连接账号」");
            y += 106;
        } else {
            // Apple Music clean horizontal track rows with active highlight + NowPlayingBars
            const int col_art = lx + static_cast<int>(avail_w * 0.50f);
            const int col_fmt = lx + static_cast<int>(avail_w * 0.76f);
            if (y + 30 > top_y && y < top_y + vh) {
                draw_text_bold(s, f, 12.0f, lx + 10, y + 6, g.C_TXT3, "#");
                draw_text_bold(s, f, 12.0f, lx + 96, y + 6, g.C_TXT3, "歌曲");
                draw_text_bold(s, f, 12.0f, col_art, y + 6, g.C_TXT3, "歌手");
                draw_text_bold(s, f, 12.0f, col_fmt, y + 6, g.C_TXT3, "音质");
                draw_text_right_bold(s, f, 12.0f, lx + avail_w - 16, y + 6, g.C_TXT3, "时长");
                fill_rect_a(s, lx, y + 28, avail_w, 1, g.C_DIV, col_a(g.C_DIV) + 14);
            }
            y += 32;

            int thumb_queued = 0;
            const int n = static_cast<int>(g.plg_snap.tracks.size());
            const int playing_online_active = !audio_paused();
            for (int p = 0; p < n; ++p) {
                const int ry = y + p * ROW_H;
                if (ry + ROW_H < top_y || ry > top_y + vh) continue;

                auto& tr = g.plg_snap.tracks[p];
                if (tr.thumb_state == 0 && !tr.cover_url.empty() && thumb_queued < 6) {
                    tr.thumb_state = 1;
                    plugin_async_thumb(tr.plugin_id, static_cast<size_t>(p), tr.cover_url);
                    ++thumb_queued;
                }
                const Rect row_rc{ lx, ry + 2, avail_w, ROW_H - 4 };
                const bool in_v = in_rect(mx, my, lx - 4, top_y, avail_w + 8, vh - DOCK_CLEAR + 16);
                const bool hov = in_v && in_rc(mx, my, row_rc);
                const bool is_play = (g.playing_online && tr.id == g.online_play_id && tr.plugin_id == g.online_play_pid);

                if (is_play) {
                    fill_frosted_round_rect(s, row_rc.x, row_rc.y, row_rc.w, row_rc.h, 12,
                                            mix_col(g.C_SURFACE, g.C_ACCENT, 0.22f),
                                            static_cast<uint8_t>(172 + 24 * g.light_t), 2, glass_rim(86, 225));
                    fill_round_rect_aa(s, row_rc.x + 2, row_rc.y + 10, 3, row_rc.h - 20, 1, g.C_ACCENT);
                } else if (hov) {
                    fill_round_rect_aa(s, row_rc.x, row_rc.y, row_rc.w, row_rc.h, 12, g.C_GLASS_HOV);
                }

                char idx_s[12]{};
                std::snprintf(idx_s, sizeof(idx_s), "%02d", p + 1);
                draw_text_bold(s, f, 12.5f, lx + 10, ry + 20, is_play ? g.C_ACCENT : g.C_TXT3, idx_s);

                if (!tr.thumb.empty()) {
                    blit_rgb_round_fit(s, lx + 42, ry + 8, 40, 40, tr.thumb.data(), 160, 160, 10);
                } else if (!g.poster_mini.empty()) {
                    blit_rgb_round_fit(s, lx + 42, ry + 8, 40, 40, g.poster_mini.data(), MINI_ART_SZ, MINI_ART_SZ, 10);
                } else {
                    fill_round_rect_aa(s, lx + 42, ry + 8, 40, 40, 10, g.C_GLASS_ACT);
                }
                if (is_play) {
                    fill_round_rect_aa_a(s, lx + 42, ry + 8, 40, 40, 10, COL(10, 14, 22, 255), 145);
                    draw_now_playing_bars(s, lx + 53, ry + 19, 18, playing_online_active, COL(255, 255, 255, 245));
                }

                const int title_w = std::max(60, col_art - (lx + 96) - 12);
                draw_text_max_bold(s, f, 14.5f, lx + 96, ry + 9, is_play ? g.C_ACCENT : (hov ? g.C_ACCENT : g.C_TXT),
                                   tr.title.c_str(), title_w);
                draw_text_max(s, f, 12.0f, lx + 96, ry + 31, g.C_TXT3, tr.album.c_str(), title_w);
                draw_text_max_bold(s, f, 13.5f, col_art, ry + 19, g.C_TXT2,
                                   !tr.artist.empty() ? tr.artist.c_str() : "在线艺人", std::max(40, col_fmt - col_art - 10));
                const char* ftag = tr.fmt[0] ? tr.fmt : "CLOUD";
                const int fw = text_width_bold(f, 11.0f, ftag) + 14;
                fill_round_rect_aa(s, col_fmt, ry + 18, fw, 20, 10,
                                   is_play ? mix_col(g.C_SURFACE, g.C_ACCENT, 0.28f) : g.C_GLASS_ACT);
                draw_text_centered_bold(s, f, 11.0f, col_fmt, col_fmt + fw, ry + 20,
                                        is_play ? g.C_ACCENT : g.C_TXT3, ftag);

                char ds[16] = "--:--";
                if (tr.dur > 0.5) fmt_time(tr.dur, ds, sizeof(ds));
                draw_text_right_bold(s, f, 13.0f, lx + avail_w - 16, ry + 19, is_play ? g.C_ACCENT : g.C_TXT2, ds);

                if (vis_hit(row_rc)) reg_hit(row_rc, PLG_TRACK, p, tr.id);
            }
            y += n * ROW_H + 14;
        }
    }

    g.plg_content_h = static_cast<float>(y - start_y);
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

void draw_qr_modal(int mx, int my) {
    PluginQrState qr = plugin_get_qr_state();
    if (!qr.active) return;
    Canvas* s = g.screen.get();
    Font* f = g.font.get();
    const bool lt = g.light_t > 0.5f;
    const PluginInfo* qr_plg = nullptr;
    for (const auto& p : g.plg_list) {
        if (p.id == qr.plugin_id) { qr_plg = &p; break; }
    }
    const bool has_cookie_tab = (qr_plg && qr_plg->supports_cookie_login);
    if (!has_cookie_tab && g.qr_tab != 0) g.qr_tab = 0;

    fill_rect_a(s, 0, 0, g.W, g.H, COL(8, 10, 14, 255), 148);
    constexpr int mw = 408, mh = 466;
    const int x = (g.W - mw) / 2, y = (g.H - mh) / 2;
    fill_round_rect_shadow(s, x, y + 8, mw, mh, 22, 146);
    fill_frosted_round_rect(s, x, y, mw, mh, 22, g.C_SURFACE, 236, 4, glass_rim(96, 235));
    stroke_round_rect_aa(s, x, y, mw, mh, 22, 1, g.C_ACCENT, 130);

    std::string modal_ttl = qr_plg ? (qr_plg->name + " · 账号连接") : std::string("扩展账号连接");
    draw_text_centered_bold(s, f, 16.5f, x + 16, x + mw - 16, y + 18, g.C_TXT, modal_ttl.c_str());

    int content_y = y + 52;
    if (has_cookie_tab) {
        constexpr int tab_w = 264;
        const int tx0 = x + (mw - tab_w) / 2;
        fill_frosted_round_rect(s, tx0, content_y, tab_w, 32, 16, g.C_SURFACE, lt ? 148 : 118, 1, glass_rim(42, 185));
        const char* t_names[2] = { "扫码登录", "Cookie 直连" };
        for (int ti = 0; ti < 2; ++ti) {
            Rect tr{ tx0 + 3 + ti * 129, content_y + 3, 129, 26 };
            g.rc_plg_qr_tab[ti] = tr;
            const bool act = (g.qr_tab == ti);
            const bool hov = in_rc(mx, my, tr);
            if (act) {
                fill_round_rect_aa(s, tr.x, tr.y, tr.w, tr.h, 13, mix_col(g.C_SURFACE, g.C_ACCENT, 0.30f));
                stroke_round_rect_aa(s, tr.x, tr.y, tr.w, tr.h, 13, 1, g.C_ACCENT, 145);
            } else if (hov) {
                fill_round_rect_aa(s, tr.x, tr.y, tr.w, tr.h, 13, g.C_GLASS_HOV);
            }
            draw_text_centered_bold(s, f, 12.0f, tr.x, tr.x + tr.w, tr.y + 5,
                                    act ? g.C_ACCENT : (hov ? g.C_TXT : g.C_TXT2), t_names[ti]);
        }
        content_y += 44;
    }

    if (g.qr_tab == 1 && has_cookie_tab) {
        // Cookie direct auth view
        const char* hint = (!qr_plg->cookie_hint.empty()) ? qr_plg->cookie_hint.c_str()
                         : "从浏览器开发者工具复制 Cookie（支持 Ctrl+V 粘贴）";
        draw_text_centered_bold(s, f, 12.5f, x + 24, x + mw - 24, content_y + 12, g.C_TXT2, hint);

        g.rc_plg_qr_input = Rect{ x + 28, content_y + 52, mw - 56, 86 };
        const bool ihov = in_rc(mx, my, g.rc_plg_qr_input);
        fill_frosted_round_rect(s, g.rc_plg_qr_input.x, g.rc_plg_qr_input.y, g.rc_plg_qr_input.w, g.rc_plg_qr_input.h, 14,
                                g.C_SURFACE, g.qr_cookie_focused ? 216 : (ihov ? 186 : 148), 2, glass_rim(64, 215));
        stroke_round_rect_aa(s, g.rc_plg_qr_input.x, g.rc_plg_qr_input.y, g.rc_plg_qr_input.w, g.rc_plg_qr_input.h, 14, 1,
                             g.qr_cookie_focused ? g.C_ACCENT : g.C_DIV, g.qr_cookie_focused ? 220 : 120);
        if (g.qr_cookie_val.empty()) {
            draw_text_centered_bold(s, f, 12.5f, g.rc_plg_qr_input.x + 12, g.rc_plg_qr_input.x + g.rc_plg_qr_input.w - 12,
                                    g.rc_plg_qr_input.y + 34, g.C_TXT3, "点击此处并按 Ctrl+V 粘贴 Cookie 凭证");
        } else {
            const size_t n = g.qr_cookie_val.size();
            std::string preview = (n > 76) ? (std::string("...") + g.qr_cookie_val.substr(n - 73)) : g.qr_cookie_val;
            draw_text_max(s, f, 12.5f, g.rc_plg_qr_input.x + 14, g.rc_plg_qr_input.y + 18, g.C_TXT,
                          preview.c_str(), g.rc_plg_qr_input.w - 28);
            char len_s[48]{};
            std::snprintf(len_s, sizeof(len_s), "已输入 %u 字符（点击验证即可保存本地）", static_cast<unsigned>(n));
            draw_text(s, f, 11.5f, g.rc_plg_qr_input.x + 14, g.rc_plg_qr_input.y + 56, g.C_ACCENT, len_s);
        }

        if (!qr.message.empty()) {
            draw_text_centered_bold(s, f, 12.5f, x + 24, x + mw - 24, content_y + 162,
                                    qr.status == "error" ? g.C_HEART : g.C_TXT2, qr.message.c_str());
        }

        g.rc_plg_qr_submit = Rect{ x + mw / 2 - 118, y + mh - 56, 124, 38 };
        g.rc_plg_qr_close  = Rect{ x + mw / 2 + 16,  y + mh - 56, 102, 38 };
        const bool shov = in_rc(mx, my, g.rc_plg_qr_submit);
        fill_round_rect_aa(s, g.rc_plg_qr_submit.x, g.rc_plg_qr_submit.y, g.rc_plg_qr_submit.w, g.rc_plg_qr_submit.h, 19,
                           shov ? mix_col(g.C_ACCENT, COL(255, 255, 255, 255), 0.14f) : g.C_ACCENT);
        draw_text_centered_bold(s, f, 13.0f, g.rc_plg_qr_submit.x, g.rc_plg_qr_submit.x + g.rc_plg_qr_submit.w,
                                g.rc_plg_qr_submit.y + 9, g.C_ON_ACCENT, "连接并验证");
    } else {
        // QR Code tab (with optional multi-channel qr_modes pills!)
        if (qr_plg && !qr_plg->qr_modes.empty()) {
            const int nm = std::min(4, static_cast<int>(qr_plg->qr_modes.size()));
            const int mode_w = nm * 92 + 6;
            const int mx0 = x + (mw - mode_w) / 2;
            fill_frosted_round_rect(s, mx0, content_y, mode_w, 30, 15, g.C_SURFACE, lt ? 148 : 118, 1, glass_rim(42, 185));
            for (int mi = 0; mi < nm; ++mi) {
                Rect mr{ mx0 + 3 + mi * 92, content_y + 3, 92, 24 };
                const bool act = (qr.mode.empty() ? (mi == g.qr_mode_idx) : (qr.mode == qr_plg->qr_modes[mi].key));
                const bool hov = in_rc(mx, my, mr);
                if (act) fill_round_rect_aa(s, mr.x, mr.y, mr.w, mr.h, 12, mix_col(g.C_SURFACE, g.C_ACCENT, 0.30f));
                else if (hov) fill_round_rect_aa(s, mr.x, mr.y, mr.w, mr.h, 12, g.C_GLASS_HOV);
                draw_text_centered_bold(s, f, 11.5f, mr.x, mr.x + mr.w, mr.y + 4,
                                        act ? g.C_ACCENT : g.C_TXT2, qr_plg->qr_modes[mi].label.c_str());
                g.plg_hits.push_back(SetHit{ mr, PLG_QR_MODE, mi });
                g.plg_hit_keys.push_back(qr_plg->qr_modes[mi].key);
            }
            content_y += 38;
        }

        constexpr int box_sz = 204;
        const int bx = x + (mw - box_sz) / 2;
        const int by = content_y + 6;
        fill_round_rect_aa(s, bx, by, box_sz, box_sz, 14, COL(255, 255, 255, 255));
        if (qr.qr_size > 0 && static_cast<int>(qr.modules.size()) == qr.qr_size * qr.qr_size) {
            const int pad = 12;
            const int inner = box_sz - pad * 2;
            const int cell = std::max(2, inner / qr.qr_size);
            const int tot = cell * qr.qr_size;
            const int qx0 = bx + (box_sz - tot) / 2;
            const int qy0 = by + (box_sz - tot) / 2;
            for (int r = 0; r < qr.qr_size; ++r) {
                for (int c = 0; c < qr.qr_size; ++c) {
                    if (qr.modules[static_cast<size_t>(r) * qr.qr_size + c]) {
                        fill_rect(s, qx0 + c * cell, qy0 + r * cell, cell, cell, COL(18, 22, 28, 255));
                    }
                }
            }
        } else {
            draw_text_centered_bold(s, f, 13.0f, bx, bx + box_sz, by + 92, COL(60, 68, 80, 255), "正在生成二维码...");
        }

        draw_text_centered_bold(s, f, 13.0f, x + 20, x + mw - 20, by + box_sz + 14,
                                qr.status == "error" ? g.C_HEART : (qr.status == "scanned" ? g.C_ACCENT : g.C_TXT2),
                                qr.message.c_str());

        g.rc_plg_qr_refresh = Rect{ x + mw / 2 - 114, y + mh - 56, 108, 38 };
        g.rc_plg_qr_close   = Rect{ x + mw / 2 + 8,   y + mh - 56, 106, 38 };
        const bool rhov = in_rc(mx, my, g.rc_plg_qr_refresh);
        fill_frosted_round_rect(s, g.rc_plg_qr_refresh.x, g.rc_plg_qr_refresh.y, g.rc_plg_qr_refresh.w, g.rc_plg_qr_refresh.h, 19,
                                g.C_SURFACE, rhov ? 190 : 148, 2, glass_rim(56, 210));
        draw_text_centered_bold(s, f, 12.5f, g.rc_plg_qr_refresh.x, g.rc_plg_qr_refresh.x + g.rc_plg_qr_refresh.w,
                                g.rc_plg_qr_refresh.y + 9, rhov ? g.C_ACCENT : g.C_TXT, "刷新二维码");
    }

    const bool hov = in_rc(mx, my, g.rc_plg_qr_close);
    fill_frosted_round_rect(s, g.rc_plg_qr_close.x, g.rc_plg_qr_close.y, g.rc_plg_qr_close.w, g.rc_plg_qr_close.h, 19,
                            g.C_SURFACE, hov ? 196 : 154, 2, glass_rim(64, 210));
    draw_text_centered_bold(s, f, 13.0f, g.rc_plg_qr_close.x, g.rc_plg_qr_close.x + g.rc_plg_qr_close.w,
                            g.rc_plg_qr_close.y + 9, g.C_TXT, "关闭");
}

void handle_text_input_events() {
    char ch[32]{};
    int act = 0;
    bool changed = false;
    bool submitted = false;
    bool plg_submitted = false;
    while (be_poll_text_char(ch, sizeof(ch), &act)) {
        if (g.qr_cookie_focused) {
            if (act == 1) {
                if (!g.qr_cookie_val.empty()) pop_utf8_cp(g.qr_cookie_val);
            } else if (act == 2) {
                g.qr_cookie_focused = 0;
                be_text_input(0);
                PluginQrState qr = plugin_get_qr_state();
                if (qr.active && !g.qr_cookie_val.empty()) {
                    plugin_login_with_cookie(qr.plugin_id, g.qr_cookie_val);
                }
            } else if (act == 3) {
                g.qr_cookie_focused = 0;
                be_text_input(0);
            } else if (act == 0 && ch[0]) {
                if (g.qr_cookie_val.size() + std::strlen(ch) < 4096) g.qr_cookie_val += ch;
            }
            continue;
        }
        if (g.cfg_editing) {
            if (act == 1) {
                if (!g.cfg_edit_val.empty()) pop_utf8_cp(g.cfg_edit_val);
            } else if (act == 2) {
                plugin_set_config(g.cfg_edit_pid, g.cfg_edit_key, g.cfg_edit_val);
                g.cfg_editing = 0;
                g.plg_list = plugin_list();
                be_text_input(0);
                show_toast("已保存扩展配置");
            } else if (act == 3) {
                g.cfg_editing = 0;
                be_text_input(0);
            } else if (act == 0 && ch[0]) {
                if (g.cfg_edit_val.size() + std::strlen(ch) < 256) g.cfg_edit_val += ch;
            }
            continue;
        }
        if (g.plg_search_focused) {
            if (act == 1) {
                if (!g.plg_search_q.empty()) pop_utf8_cp(g.plg_search_q);
            } else if (act == 2) {
                plg_submitted = true;
                g.plg_search_focused = 0;
                be_text_input(0);
            } else if (act == 3) {
                g.plg_search_focused = 0;
                be_text_input(0);
            } else if (act == 0 && ch[0]) {
                if (g.plg_search_q.size() + std::strlen(ch) < 156) g.plg_search_q += ch;
            }
            continue;
        }
        if (act == 1) {
            if (!g.search_q.empty()) { pop_utf8_cp(g.search_q); changed = true; }
        } else if (act == 2) {
            submitted = true;
            g.search_focused = 0;
            be_text_input(0);
        } else if (act == 3) {
            g.search_focused = 0;
            be_text_input(0);
        } else if (act == 0 && ch[0]) {
            if (g.search_q.size() + std::strlen(ch) < 156) {
                g.search_q += ch;
                changed = true;
            }
        }
    }
    if (changed && g.page == PAGE_LIBRARY) {
        rebuild_visible();
        g.scroll_px = g.scroll_target = 0.0f;
    }
    if (plg_submitted && !g.active_plugin_id.empty()) {
        plugin_async_search(g.active_plugin_id, g.plg_search_q, 1);
    } else if (submitted && g.page == PAGE_PLUGINS && !g.active_plugin_id.empty()) {
        plugin_async_search(g.active_plugin_id, g.search_q, 1);
    }
}

void toggle_play_pause_action() {
    if (g.playing_idx >= 0 || g.playing_online) {
        if (audio_paused()) audio_resume();
        else audio_pause();
    } else if (!g.vis_idx.empty()) {
        load_track(g.vis_idx[0]);
    } else if (!g.tracks.empty()) {
        load_track(0);
    }
}

void update_hover_cursor(int mx, int my) {
    if (plugin_get_qr_state().active) {
        bool mhand = in_rc(mx, my, g.rc_plg_qr_close) || in_rc(mx, my, g.rc_plg_qr_tab[0]) ||
                     in_rc(mx, my, g.rc_plg_qr_tab[1]) || in_rc(mx, my, g.rc_plg_qr_input) ||
                     in_rc(mx, my, g.rc_plg_qr_submit) || in_rc(mx, my, g.rc_plg_qr_refresh);
        for (const auto& ph : g.plg_hits) {
            if (!mhand && ph.id == PLG_QR_MODE && in_rc(mx, my, ph.rc)) mhand = true;
        }
        const int ih = mhand ? 1 : 0;
        if (ih != g.ui_cursor_hand) { g.ui_cursor_hand = ih; be_set_cursor(ih); }
        return;
    }
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
            in_rc(mx, my, g.rc_nav_plg) || in_rc(mx, my, g.rc_nav_rescan) || in_rc(mx, my, g.rc_nav_set) ||
            in_rc(mx, my, g.rc_nav_add) || in_rc(mx, my, g.rc_nav_bg) || in_rc(mx, my, g.rc_nav_theme) ||
            in_rc(mx, my, g.rc_sb_track)) {
            hand = true;
        }
        for (const auto& dr : g.rc_dir_rows) if (!hand && in_rc(mx, my, dr.rc)) hand = true;
        if (!hand && g.page == PAGE_SETTINGS) {
            if (in_rc(mx, my, g.rc_set_back)) hand = true;
            for (const auto& sh : g.set_hits) if (!hand && in_rc(mx, my, sh.rc)) hand = true;
        } else if (!hand && g.page == PAGE_PLUGINS) {
            if (in_rc(mx, my, g.rc_plg_back) || in_rc(mx, my, g.rc_plg_search) || in_rc(mx, my, g.rc_plg_search_clr)) hand = true;
            for (const auto& ph : g.plg_hits) if (!hand && in_rc(mx, my, ph.rc)) hand = true;
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
        else if (mcmd == 2) {
            if (g.playing_online && !g.plg_snap.tracks.empty()) play_online_step(-1, true);
            else if (!g.tracks.empty()) load_track(pick_prev(g.playing_idx));
        } else if (mcmd == 3) {
            if (g.playing_online && !g.plg_snap.tracks.empty()) play_online_step(1, true);
            else if (!g.tracks.empty()) load_track(pick_next(g.playing_idx, 1));
        } else if (mcmd == 4) {
            if (has_active_track() && !audio_paused()) audio_pause();
        }
    }

    const char* dp = nullptr;
    while ((dp = be_poll_dropped_path()) != nullptr) handle_dropped(dp);

    handle_text_input_events();
    if (!g.search_q.empty() && g.search_focused && g.page != PAGE_LIBRARY && g.page != PAGE_PLUGINS) {
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
    if (g.search_focused || g.plg_search_focused || g.qr_cookie_focused || g.cfg_editing) {
        // Ignore single-key / search hotkeys while typing in any input box
    } else if (hk == 's') {
        if (g.page == PAGE_PLUGINS) {
            g.plg_search_focused = 1;
            be_text_input(1);
        } else {
            if (g.page != PAGE_LIBRARY) { capture_fade(); g.page = PAGE_LIBRARY; }
            g.search_focused = 1;
            be_text_input(1);
        }
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

        if (plugin_get_qr_state().active) {
            PluginQrState qr = plugin_get_qr_state();
            if (in_rc(ti.x, ti.y, g.rc_plg_qr_close)) {
                plugin_qr_close();
                g.qr_cookie_focused = 0;
                be_text_input(0);
                return;
            }
            if (in_rc(ti.x, ti.y, g.rc_plg_qr_tab[0])) {
                g.qr_tab = 0;
                g.qr_cookie_focused = 0;
                be_text_input(0);
                return;
            }
            if (in_rc(ti.x, ti.y, g.rc_plg_qr_tab[1])) {
                g.qr_tab = 1;
                g.qr_cookie_focused = 1;
                be_text_input(1);
                return;
            }
            if (g.qr_tab == 1) {
                if (in_rc(ti.x, ti.y, g.rc_plg_qr_input)) {
                    g.qr_cookie_focused = 1;
                    be_text_input(1);
                    return;
                }
                if (in_rc(ti.x, ti.y, g.rc_plg_qr_submit)) {
                    g.qr_cookie_focused = 0;
                    be_text_input(0);
                    if (!g.qr_cookie_val.empty()) {
                        plugin_login_with_cookie(qr.plugin_id, g.qr_cookie_val);
                    } else {
                        show_toast("请先粘贴 Cookie 凭证");
                    }
                    return;
                }
            } else {
                if (in_rc(ti.x, ti.y, g.rc_plg_qr_refresh)) {
                    plugin_qr_start(qr.plugin_id, qr.mode);
                    g.qr_poll_timer = 1.0f;
                    return;
                }
                for (size_t hi = 0; hi < g.plg_hits.size(); ++hi) {
                    if (g.plg_hits[hi].id == PLG_QR_MODE && in_rc(ti.x, ti.y, g.plg_hits[hi].rc)) {
                        g.qr_mode_idx = g.plg_hits[hi].arg;
                        const std::string& m = (hi < g.plg_hit_keys.size()) ? g.plg_hit_keys[hi] : std::string();
                        plugin_qr_start(qr.plugin_id, m);
                        g.qr_poll_timer = 1.0f;
                        return;
                    }
                }
            }
            return;
        }
        if (g.cfg_editing) {
            plugin_set_config(g.cfg_edit_pid, g.cfg_edit_key, g.cfg_edit_val);
            g.cfg_editing = 0;
            g.plg_list = plugin_list();
            be_text_input(0);
        }

        if (g.plg_search_focused && !in_rc(ti.x, ti.y, g.rc_plg_search)) {
            g.plg_search_focused = 0;
            be_text_input(0);
        }

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
                if (g.page != PAGE_LIBRARY && g.page != PAGE_PLUGINS) { capture_fade(); g.page = PAGE_LIBRARY; }
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
        if (in_rc(ti.x, ti.y, g.rc_bar_prog) && has_active_track() && g.ui_px1 > g.ui_px0) {
            const double dur = active_track_dur();
            if (dur > 0.1) {
                g.scrub_drag = 1;
                g.drag_mode = DRAG_ISLAND_PROG;
                g.scrub_sec = static_cast<double>(clampf(static_cast<float>(ti.x - g.ui_px0) / static_cast<float>(g.ui_px1 - g.ui_px0), 0.0f, 1.0f)) * dur;
            }
            return;
        }
        if (in_rc(ti.x, ti.y, g.rc_bar_fav))  { if (g.playing_idx >= 0) toggle_fav(g.playing_idx); return; }
        if (in_rc(ti.x, ti.y, g.rc_bar_prev)) {
            if (g.playing_online && !g.plg_snap.tracks.empty()) play_online_step(-1, true);
            else if (!g.tracks.empty()) load_track(pick_prev(g.playing_idx));
            return;
        }
        if (in_rc(ti.x, ti.y, g.rc_bar_play)) { toggle_play_pause_action(); return; }
        if (in_rc(ti.x, ti.y, g.rc_bar_next)) {
            if (g.playing_online && !g.plg_snap.tracks.empty()) play_online_step(1, true);
            else if (!g.tracks.empty()) load_track(pick_next(g.playing_idx, 1));
            return;
        }
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
        if (in_rc(ti.x, ti.y, g.rc_nav_plg)) {
            capture_fade();
            g.plg_list = plugin_list();
            if (g.active_plugin_id.empty()) g.active_plugin_id = plugin_default_id();
            g.page = PAGE_PLUGINS;
            if (g.plg_list.empty()) g.plg_subtab = 1;
            else if (g.plg_snap.playlists.empty() && g.plg_snap.tracks.empty() && !g.active_plugin_id.empty()) {
                plugin_async_home(g.active_plugin_id);
            }
            g.scroll_px = g.scroll_target = g.scroll_vel = 0.0f;
            return;
        }
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

        if (g.page == PAGE_PLUGINS) {
            if (in_rc(ti.x, ti.y, g.rc_plg_back)) {
                capture_fade(); g.page = PAGE_LIBRARY; g.scroll_px = g.scroll_target = g.scroll_vel = 0.0f; return;
            }
            if (in_rc(ti.x, ti.y, g.rc_plg_search_clr) && !g.plg_search_q.empty()) {
                g.plg_search_q.clear();
                g.plg_search_focused = 1;
                g.search_focused = 0;
                be_text_input(1);
                return;
            }
            if (in_rc(ti.x, ti.y, g.rc_plg_search)) {
                g.plg_search_focused = 1;
                g.search_focused = 0;
                be_text_input(1);
                return;
            }
            for (size_t hi = 0; hi < g.plg_hits.size(); ++hi) {
                const auto& ph = g.plg_hits[hi];
                if (!in_rc(ti.x, ti.y, ph.rc)) continue;
                const std::string& hkey = (hi < g.plg_hit_keys.size()) ? g.plg_hit_keys[hi] : std::string();
                switch (ph.id) {
                case PLG_TAB_BROWSE:
                    g.plg_subtab = 0; g.scroll_px = g.scroll_target = g.scroll_vel = 0.0f; return;
                case PLG_TAB_MANAGE:
                    g.plg_list = plugin_list();
                    g.plg_subtab = 1; g.scroll_px = g.scroll_target = g.scroll_vel = 0.0f; return;
                case PLG_RESCAN:
                    plugin_rescan();
                    g.plg_list = plugin_list();
                    if (g.active_plugin_id.empty()) g.active_plugin_id = plugin_default_id();
                    show_toast("已重新扫描扩展目录");
                    return;
                case PLG_CLEAR_CACHE: {
                    int n = plugin_clear_cache();
                    char buf[64]{};
                    std::snprintf(buf, sizeof(buf), "已清理 %d 个在线缓存文件", n);
                    show_toast(buf);
                    return;
                }
                case PLG_SELECT:
                    g.active_plugin_id = hkey;
                    g.scroll_px = g.scroll_target = g.scroll_vel = 0.0f;
                    plugin_async_home(hkey);
                    return;
                case PLG_TOGGLE:
                    if (ph.arg >= 0 && ph.arg < static_cast<int>(g.plg_list.size())) {
                        plugin_set_enabled(hkey, !g.plg_list[ph.arg].enabled);
                        g.plg_list = plugin_list();
                        if (g.active_plugin_id.empty()) g.active_plugin_id = plugin_default_id();
                    }
                    return;
                case PLG_LOGIN: {
                    g.qr_tab = 0;
                    g.qr_mode_idx = 0;
                    g.qr_cookie_focused = 0;
                    g.qr_cookie_val.clear();
                    std::string first_mode;
                    for (const auto& p : g.plg_list) {
                        if (p.id == hkey && !p.qr_modes.empty()) {
                            first_mode = p.qr_modes[0].key;
                            break;
                        }
                    }
                    plugin_qr_start(hkey, first_mode);
                    g.qr_poll_timer = 1.0f;
                    return;
                }
                case PLG_LOGOUT:
                    plugin_logout(hkey);
                    g.plg_list = plugin_list();
                    show_toast("已退出扩展账号");
                    return;
                case PLG_REMOVE:
                    if (plugin_remove(hkey)) {
                        g.plg_list = plugin_list();
                        if (g.active_plugin_id == hkey) g.active_plugin_id = plugin_default_id();
                        show_toast("已移除扩展插件");
                    }
                    return;
                case PLG_HOME:
                    g.scroll_px = g.scroll_target = g.scroll_vel = 0.0f;
                    plugin_async_home(hkey);
                    return;
                case PLG_QUALITY:
                    g.plg_quality = hkey;
                    show_toast(hkey == "lossless" ? "优先音质: 无损品质"
                                                  : (hkey == "exhigh" ? "优先音质: 极高品质" : "优先音质: 标准品质"));
                    return;
                case PLG_SEARCH_GO: {
                    const std::string& q = !g.plg_search_q.empty() ? g.plg_search_q : g.search_q;
                    if (!q.empty()) {
                        plugin_async_search(hkey, q, 1);
                    } else {
                        g.plg_search_focused = 1;
                        be_text_input(1);
                        show_toast("请输入搜索关键词");
                    }
                    return;
                }
                case PLG_PLAYLIST:
                    plugin_async_playlist_tracks(g.active_plugin_id, hkey);
                    return;
                case PLG_TRACK:
                    if (ph.arg >= 0 && ph.arg < static_cast<int>(g.plg_snap.tracks.size())) {
                        g.online_play_pos = ph.arg;
                        plugin_async_play_track(g.plg_snap.tracks[ph.arg].plugin_id, g.plg_snap.tracks[ph.arg], g.plg_quality);
                        show_toast("正在缓冲在线音轨...");
                    }
                    return;
                case PLG_CFG_EDIT: {
                    auto nl = hkey.find('\n');
                    if (nl != std::string::npos && ph.arg >= 0 && ph.arg < static_cast<int>(g.plg_list.size())) {
                        g.cfg_edit_pid = hkey.substr(0, nl);
                        g.cfg_edit_key = hkey.substr(nl + 1);
                        g.cfg_edit_val.clear();
                        for (const auto& cf : g.plg_list[ph.arg].config_fields) {
                            if (cf.key == g.cfg_edit_key) { g.cfg_edit_val = cf.value; break; }
                        }
                        g.cfg_editing = 1;
                        be_text_input(1);
                    }
                    return;
                }
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
                g.drag_start_x = ti.x; g.drag_start_y = ti.y; g.drag_moved = 0;
                g.drag_cand_track = -1; g.drag_cand_fav = 0; g.drag_cand_hero = 0;
                g.scroll_target = g.scroll_px;
            }
            return;
        }

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
                    if (has_active_track()) { capture_fade(); g.page = PAGE_LYRICS; }
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
    if (!g.search_focused && !g.plg_search_focused && !g.qr_cookie_focused && !g.cfg_editing && kd) {
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
        if (kd & BTN_ZL) {
            if (g.playing_online && !g.plg_snap.tracks.empty()) play_online_step(-1, true);
            else if (!g.tracks.empty()) load_track(pick_prev(g.playing_idx));
        }
        if (kd & BTN_ZR) {
            if (g.playing_online && !g.plg_snap.tracks.empty()) play_online_step(1, true);
            else if (!g.tracks.empty()) load_track(pick_next(g.playing_idx, 1));
        }
        if (kd & BTN_LEFT)  { if (has_active_track()) audio_seek(std::max(0.0, audio_position() - 5.0)); }
        if (kd & BTN_RIGHT) { if (has_active_track()) audio_seek(std::min(active_track_dur(), audio_position() + 5.0)); }
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
    } else if (g.page == PAGE_PLUGINS) {
        draw_frosted_ambient_bg(0);
        draw_nav_rail(mx, my);
        draw_plugins_view(mx, my);
        draw_player_bar(mx, my);
    } else {
        draw_immersive_view(mx, my);
    }
    draw_qr_modal(mx, my);

    if (!g.nav_tooltip.empty() && g.nav_tooltip_rc.w > 0 && !plugin_get_qr_state().active) {
        Canvas* s = g.screen.get();
        Font* f = g.font.get();
        const bool lt = g.light_t > 0.5f;
        const int tw = text_width_bold(f, 11.5f, g.nav_tooltip.c_str()) + 16;
        constexpr int th = 24;
        int tx = g.nav_tooltip_rc.x + (g.nav_tooltip_rc.w - tw) / 2;
        tx = std::clamp(tx, 8, std::max(8, g.W - tw - 8));
        int ty = g.nav_tooltip_rc.y - th - 6;
        if (ty < 8) ty = g.nav_tooltip_rc.y + g.nav_tooltip_rc.h + 6;
        fill_round_rect_shadow_fast(s, tx, ty + 2, tw, th, 6, theme_shadow(84));
        fill_round_rect_aa(s, tx, ty, tw, th, 6, lt ? COL(248, 250, 254, 244) : COL(24, 28, 38, 240));
        stroke_round_rect_aa(s, tx, ty, tw, th, 6, 1, g.C_DIV, col_a(g.C_DIV) + 28);
        draw_text_centered_bold(s, f, 11.5f, tx, tx + tw, ty + 5, g.C_TXT, g.nav_tooltip.c_str());
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
        // Klang UiAnimations.FadeThrough: smooth opacity crossfade + 10px subtle vertical rise
        const float u_left = clampf(g.fade_time / FADE_DUR, 0.0f, 1.0f);
        const float u_in   = 1.0f - u_left;
        const float ease   = 1.0f - std::pow(1.0f - u_in, 3.0f);
        const int rise_px  = static_cast<int>(std::round((1.0f - ease) * 10.0f * (g.ui_scale > 0.1f ? g.ui_scale : 1.0f)));
        const int sw = g.screen->w, sh = g.screen->h;
        if (rise_px == 0) {
            blend_over_opaque(g.screen->px, g.trans_shot->px,
                              static_cast<size_t>(sw) * sh, static_cast<uint8_t>((1.0f - ease) * 255.0f));
        } else {
            const uint32_t k_old = static_cast<uint32_t>(clampf((1.0f - ease) * 256.0f + 0.5f, 0.0f, 256.0f));
            const uint32_t k_new = 256u - k_old;
            std::vector<uint32_t> tmp_copy(g.screen->px, g.screen->px + static_cast<size_t>(sw) * sh);
            for (int y = 0; y < sh; ++y) {
                const int sy = std::clamp(y - rise_px, 0, sh - 1);
                const uint32_t* snew = tmp_copy.data() + static_cast<size_t>(sy) * sw;
                const uint32_t* sold = g.trans_shot->px + static_cast<size_t>(y) * sw;
                uint32_t* dst = g.screen->px + static_cast<size_t>(y) * sw;
                for (int x = 0; x < sw; ++x) {
                    const uint32_t fg = snew[x], bg = sold[x];
                    const uint32_t rb = ((((fg & 0x00FF00FFu) * k_new + (bg & 0x00FF00FFu) * k_old + 0x00800080u) >> 8) & 0x00FF00FFu);
                    const uint32_t gv = (((((fg >> 8) & 0xFFu) * k_new + ((bg >> 8) & 0xFFu) * k_old + 128u) & 0xFF00u));
                    dst[x] = 0xFF000000u | rb | gv;
                }
            }
        }
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
    plugin_init(be_config_dir());
    g.plg_list = plugin_list();
    g.active_plugin_id = plugin_default_id();
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

        if (audio_finished()) {
            if (g.playing_online && !g.plg_snap.tracks.empty()) {
                play_online_step(1, false);
            } else if (!g.tracks.empty()) {
                load_track(pick_next(g.playing_idx, 0));
            }
        }

        {
            bool gp = false, gt = false, go = false;
            plugin_poll_updates(g.plg_snap, gp, gt, go);
            (void)gp; (void)gt;
            if (g.plg_snap.plugins_updated) {
                g.plg_list = plugin_list();
                if (g.active_plugin_id.empty()) g.active_plugin_id = plugin_default_id();
            }
            if (!g.plg_snap.status_toast.empty()) {
                show_toast(g.plg_snap.status_toast.c_str());
                g.plg_snap.status_toast.clear();
            }
            if (go) play_online_ready(g.plg_snap.ready_play);
            PluginQrState qrs = plugin_get_qr_state();
            if (qrs.active && qrs.status == "ok") {
                std::string authed_pid = qrs.plugin_id;
                plugin_qr_close();
                g.qr_cookie_focused = 0;
                be_text_input(0);
                g.plg_list = plugin_list();
                if (!authed_pid.empty() && authed_pid == g.active_plugin_id) {
                    plugin_async_home(authed_pid);
                }
            } else if (qrs.active && (qrs.status == "waiting" || qrs.status == "scanned")) {
                g.qr_poll_timer -= static_cast<float>(g.dt);
                if (g.qr_poll_timer <= 0.0f) {
                    g.qr_poll_timer = 1.5f;
                    plugin_qr_poll();
                }
            }
        }

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

        const bool playing_active = (has_active_track() && !audio_paused());
        g.wave_phase  += static_cast<float>(g.dt) * (playing_active ? 2.6f : 0.45f);
        g.ambient_t   += static_cast<float>(g.dt) * (playing_active ? 0.48f : 0.14f);
        g.scallop_rot += static_cast<float>(g.dt) * (playing_active ? 0.50f : 0.08f);

        // Klang PlayPauseGlyph morph (220ms transition) + Bouncy heart timer + 1.2s backdrop crossfade
        const float morph_tgt = playing_active ? 1.0f : 0.0f;
        if (g.play_morph_t < morph_tgt) g.play_morph_t = std::min(morph_tgt, g.play_morph_t + static_cast<float>(g.dt) / 0.22f);
        else if (g.play_morph_t > morph_tgt) g.play_morph_t = std::max(morph_tgt, g.play_morph_t - static_cast<float>(g.dt) / 0.22f);
        if (g.fav_bounce_t > 0.0f) g.fav_bounce_t = std::max(0.0f, g.fav_bounce_t - static_cast<float>(g.dt));
        if (g.bg_xfade_t < 1.0f) g.bg_xfade_t = clampf(g.bg_xfade_t + static_cast<float>(g.dt) / 1.20f, 0.0f, 1.0f);

        // Klang NowPlayingBars asymmetric attack (0.55) / decay (0.22) at ~60fps reference
        const float fps_dt = clampf(static_cast<float>(g.dt) * 60.0f, 0.1f, 3.0f);
        const float atk = 1.0f - std::pow(1.0f - 0.55f, fps_dt);
        const float dec = 1.0f - std::pow(1.0f - 0.22f, fps_dt);
        static constexpr float eq_freq[4] = { 3.1f, 4.3f, 2.7f, 3.8f };
        for (int b = 0; b < 4; ++b) {
            float tgt = 0.18f;
            if (playing_active) {
                const float spec = clampf(g.bars[b * 4 + 1] * 0.72f + g.bars[b * 4 + 2] * 0.45f, 0.0f, 1.0f);
                const float osc  = 0.36f + 0.30f * std::sin(g.wave_phase * eq_freq[b] + static_cast<float>(b) * 1.45f)
                                         + 0.14f * std::cos(g.wave_phase * (eq_freq[b] * 1.6f) + static_cast<float>(b));
                tgt = clampf(std::max(spec, osc * 0.85f), 0.20f, 1.0f);
            }
            const float cur = g.eq_bars[b];
            g.eq_bars[b] = cur + (tgt - cur) * (tgt > cur ? atk : dec);
        }

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
        const bool anim_moving = (std::fabs(g.play_morph_t - morph_tgt) > 0.002f) ||
                                 (g.fav_bounce_t > 0.0f) || (g.bg_xfade_t < 0.999f) ||
                                 (g.lyr_springs_busy != 0) || (g.plg_snap.is_busy);
        be_set_idle_mode((!dump_mode && !playing_active && !scroll_moving && !theme_moving &&
                          !anim_moving && !g.fade_armed && g.toast_timer <= 0) ? 1 : 0);

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

        const double cur_dur = active_track_dur();
        const double cur_pos = active_track_pos();
        const float cur_frac = (cur_dur > 0.1) ? clampf(static_cast<float>(cur_pos / cur_dur), 0.0f, 1.0f) : 0.0f;
        be_set_media_state(has_active_track(), has_active_track() && !audio_paused(), cur_frac,
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

    plugin_shutdown();
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
