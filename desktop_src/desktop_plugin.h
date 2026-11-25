#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace sirene {

struct PluginConfigField {
    std::string key;
    std::string label;
    std::string placeholder;
    std::string value;
};

struct PluginQrMode {
    std::string key;
    std::string label;
};

struct PluginInfo {
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    std::string path;
    bool enabled = true;
    bool valid = false;
    bool supports_login = false;
    bool supports_cookie_login = false;
    bool supports_save = false;
    std::string cookie_hint;
    std::vector<PluginQrMode> qr_modes;
    bool logged_in = false;
    std::string nickname;
    std::string last_error;
    std::vector<PluginConfigField> config_fields;
};

struct OnlinePlaylist {
    std::string plugin_id;
    std::string id;
    std::string name;
    std::string description;
    std::string cover_url;
    std::string category;
    int track_count = 0;
    std::vector<uint32_t> thumb;
    int thumb_state = 0; // 0=idle, 1=loading, 2=ready, 3=failed
};

struct OnlineTrack {
    std::string plugin_id;
    std::string id;
    std::string title;
    std::string artist;
    std::string album;
    std::string cover_url;
    char fmt[16] = "STREAM";
    double dur = 0.0;
    bool liked = false;
    std::vector<uint32_t> thumb;
    int thumb_state = 0; // 0=idle, 1=loading, 2=ready, 3=failed
    std::vector<uint32_t> cov_cache;
    int cov_cache_px = 0;
};

struct PluginQrState {
    bool active = false;
    std::string plugin_id;
    std::string mode;
    std::string qr_key;
    std::string qr_url;
    std::string status = "idle"; // idle, loading, waiting, scanned, ok, expired, error
    std::string message;
    int qr_size = 0;
    std::vector<uint8_t> modules; // qr_size * qr_size (0 or 1)
};

struct PluginReadyPlay {
    bool ready = false;
    bool ok = false;
    std::string plugin_id;
    OnlineTrack track;
    std::string audio_path;
    std::string cover_path;
    std::string lrc_orig;
    std::string lrc_trans;
    std::string error;
    bool is_trial = false;
    bool can_save = false;
};

void plugin_init(const char* config_dir);
void plugin_shutdown();

// Rescan plugins directory (<config_dir>/plugins and <exe_dir>/plugins)
void plugin_rescan();
bool plugin_import_file(const std::string& src_path, std::string* out_err = nullptr);
bool plugin_remove(const std::string& plugin_id);
void plugin_set_enabled(const std::string& plugin_id, bool enabled);
void plugin_set_config(const std::string& plugin_id, const std::string& key, const std::string& val);
std::string plugin_dir_path();
int plugin_clear_cache();

std::vector<PluginInfo> plugin_list();
int plugin_active_count();
std::string plugin_default_id();

// Async browse/search actions (results polled via plugin_poll_updates)
void plugin_async_search(const std::string& plugin_id, const std::string& query, int page = 1);
void plugin_async_home(const std::string& plugin_id); // loads playlists + favorites
void plugin_async_playlist_tracks(const std::string& plugin_id, const std::string& playlist_id);
void plugin_async_play_track(const std::string& plugin_id, const OnlineTrack& track, const std::string& quality = "high");
void plugin_async_thumb(const std::string& plugin_id, size_t track_list_idx, const std::string& url);
void plugin_async_playlist_thumb(const std::string& plugin_id, size_t playlist_idx, const std::string& url);

// Async login QR & credential flow
void plugin_qr_start(const std::string& plugin_id, const std::string& mode = "");
void plugin_qr_poll();
void plugin_qr_close();
void plugin_login_with_cookie(const std::string& plugin_id, const std::string& cookie);
void plugin_logout(const std::string& plugin_id);
PluginQrState plugin_get_qr_state();

// Poll finished background tasks into UI-owned containers
struct PluginUiSnapshot {
    bool playlists_updated = false;
    bool tracks_updated = false;
    bool plugins_updated = false;
    bool is_busy = false;
    std::string status_toast;
    std::string active_playlist_name;
    std::vector<OnlinePlaylist> playlists;
    std::vector<OnlineTrack> tracks;
    PluginReadyPlay ready_play;
};

void plugin_poll_updates(PluginUiSnapshot& dst, bool& out_got_playlists, bool& out_got_tracks, bool& out_got_play);

} // namespace sirene
