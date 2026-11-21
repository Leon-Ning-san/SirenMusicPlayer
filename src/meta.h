#pragma once
typedef struct TrackMeta {
    char title[256];
    char artist[256];
    char album[256];
    unsigned char* art; // RGBA, stb_image decoded
    int art_w, art_h;
} TrackMeta;

// Returns 1 if any metadata was found. art may be NULL.
// On return, fields not found remain empty strings, art NULL.
int meta_load(const char* path, TrackMeta* m);
// 只释放解码后的封面像素，保留 title/artist/album 文本
void meta_release_art(TrackMeta* m);
void meta_clear(TrackMeta* m);
