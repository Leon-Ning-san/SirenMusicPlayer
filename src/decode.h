#pragma once
#include <stdint.h>

#include "dr_wav.h"
#include "dr_mp3.h"
#include "dr_flac.h"
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

typedef enum {
    FMT_NONE = 0, FMT_WAV, FMT_MP3, FMT_FLAC, FMT_OGG
} DecFmt;

typedef struct Decoder {
    DecFmt fmt;
    drwav* wav;
    drmp3* mp3;
    drflac* flac;
    stb_vorbis* ogg;
    uint32_t channels;
    uint32_t rate;
    uint64_t total_frames; // at source rate
} Decoder;

int  dec_open(Decoder* d, const char* path);      // 0 on success
// read interleaved s16 frames (up to want). returns frames actually read
int  dec_read(Decoder* d, int16_t* out, int want);
// seek to source frame index. returns 0 on success
int  dec_seek(Decoder* d, uint64_t frame);
void dec_close(Decoder* d);
int  dec_is_audio(const char* path);              // supported extension?
