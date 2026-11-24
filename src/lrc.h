#pragma once

typedef struct {
    double t;          // seconds
    char text[256];    // utf-8
} LrcLine;

typedef struct {
    LrcLine* lines;
    int n;
    int unsynced;      // 1 = 纯文本静态歌词（无时间戳）
    char ti[256];      // title from [ti:...]
    char ar[256];      // artist from [ar:...]
    char source[64];   // 来源说明（用于自检诊断）
} Lyrics;

// Loads external .lrc or embedded lyrics (MP3 USLT/SYLT/TXXX/APEv2, FLAC/OGG Vorbis/ID3, WAV RIFF ID3, raw LRC fallback).
// Returns NULL if not found/empty.
Lyrics* lyrics_load(const char* media_path);
Lyrics* lyrics_load_ex(const char* media_path, const char* title, const char* artist);
Lyrics* lyrics_load_file(const char* lrc_path);
void lyrics_free(Lyrics* l);
// index of active line for time t, or -1
int lyrics_index(const Lyrics* l, double t);
// Diagnostic string from last lyrics_load_ex call
const char* lyrics_last_diag(void);

// 桌面端异步联网匹配歌词（网易云 163 key musicId / 网易云搜索 / LRCLIB + 本地缓存）
void lyrics_set_cache_dir(const char* dir);
int  lyrics_start_online(const char* media_path, const char* title, const char* artist, double duration_sec);
int  lyrics_poll_online(int req_id, Lyrics** out);
int  lyrics_online_busy(int req_id);
