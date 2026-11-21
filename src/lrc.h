#pragma once

typedef struct {
    double t;          // seconds
    char text[256];    // utf-8
} LrcLine;

typedef struct {
    LrcLine* lines;
    int n;
    char ti[256];      // title from [ti:...]
    char ar[256];      // artist from [ar:...]
} Lyrics;

// Loads .lrc file matching the media file path (same basename).
// Returns NULL if not found/empty.
Lyrics* lyrics_load(const char* media_path);
void lyrics_free(Lyrics* l);
// index of active line for time t, or -1
int lyrics_index(const Lyrics* l, double t);

