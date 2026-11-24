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

// Loads external .lrc or embedded lyrics (MP3 USLT/SYLT, FLAC/OGG LYRICS).
// Returns NULL if not found/empty.
Lyrics* lyrics_load(const char* media_path);
Lyrics* lyrics_load_ex(const char* media_path, const char* title, const char* artist);
void lyrics_free(Lyrics* l);
// index of active line for time t, or -1
int lyrics_index(const Lyrics* l, double t);
