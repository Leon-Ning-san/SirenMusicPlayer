#include "decode.h"
#include "util.h"
#include "plat.h"
#include <string.h>
#include <stdlib.h>

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#undef STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_IMPLEMENTATION
#include "stb_vorbis.c"

int dec_is_audio(const char* path) {
    return ci_ends_with(path, ".mp3") || ci_ends_with(path, ".flac") ||
           ci_ends_with(path, ".wav") || ci_ends_with(path, ".ogg") ||
           ci_ends_with(path, ".wave");
}

int dec_open(Decoder* d, const char* path) {
    memset(d, 0, sizeof(*d));
#ifdef _WIN32
    wchar_t wpath[1024];
    int has_w = plat_utf8_to_wide(path, wpath, 1024);
#endif
    if (ci_ends_with(path, ".wav") || ci_ends_with(path, ".wave")) {
        d->fmt = FMT_WAV;
        d->wav = (drwav*)malloc(sizeof(drwav));
#ifdef _WIN32
        int ok = has_w ? drwav_init_file_w(d->wav, wpath, NULL) : drwav_init_file(d->wav, path, NULL);
#else
        int ok = drwav_init_file(d->wav, path, NULL);
#endif
        if (!ok) { free(d->wav); d->wav = NULL; return -1; }
        d->channels = d->wav->channels;
        d->rate = d->wav->sampleRate;
        d->total_frames = d->wav->totalPCMFrameCount;
        return 0;
    }
    if (ci_ends_with(path, ".mp3")) {
        d->fmt = FMT_MP3;
        d->mp3 = (drmp3*)malloc(sizeof(drmp3));
#ifdef _WIN32
        int ok = has_w ? drmp3_init_file_w(d->mp3, wpath, NULL) : drmp3_init_file(d->mp3, path, NULL);
#else
        int ok = drmp3_init_file(d->mp3, path, NULL);
#endif
        if (!ok) { free(d->mp3); d->mp3 = NULL; return -1; }
        d->channels = d->mp3->channels;
        d->rate = d->mp3->sampleRate;
        d->total_frames = drmp3_get_pcm_frame_count(d->mp3);
        return 0;
    }
    if (ci_ends_with(path, ".flac")) {
        d->fmt = FMT_FLAC;
#ifdef _WIN32
        d->flac = has_w ? drflac_open_file_w(wpath, NULL) : drflac_open_file(path, NULL);
#else
        d->flac = drflac_open_file(path, NULL);
#endif
        if (!d->flac) return -1;
        d->channels = d->flac->channels;
        d->rate = d->flac->sampleRate;
        d->total_frames = d->flac->totalPCMFrameCount;
        return 0;
    }
    if (ci_ends_with(path, ".ogg")) {
        d->fmt = FMT_OGG;
        int err = 0;
#ifdef _WIN32
        FILE* fp = plat_fopen(path, "rb");
        d->ogg = fp ? stb_vorbis_open_file(fp, 1, &err, NULL) : NULL;
        if (!d->ogg && fp) fclose(fp);
#else
        d->ogg = stb_vorbis_open_filename(path, &err, NULL);
#endif
        if (!d->ogg) return -1;
        stb_vorbis_info info = stb_vorbis_get_info(d->ogg);
        d->channels = info.channels;
        d->rate = info.sample_rate;
        d->total_frames = stb_vorbis_stream_length_in_samples(d->ogg);
        return 0;
    }
    return -1;
}

int dec_read(Decoder* d, int16_t* out, int want) {
    if (want <= 0) return 0;
    switch (d->fmt) {
    case FMT_WAV:
        return (int)drwav_read_pcm_frames_s16(d->wav, (drwav_uint64)want, out);
    case FMT_MP3:
        return (int)drmp3_read_pcm_frames_s16(d->mp3, (drmp3_uint64)want, out);
    case FMT_FLAC:
        return (int)drflac_read_pcm_frames_s16(d->flac, (drflac_uint64)want, out);
    case FMT_OGG: {
        // stb_vorbis decodes one packet per call; returns samples per channel.
        int nc = (int)d->channels;
        int total = 0;
        while (total < want) {
            short* dst = (short*)(out + (size_t)total * nc);
            int r = stb_vorbis_get_frame_short_interleaved(d->ogg, nc, dst, (want - total) * nc);
            if (r <= 0) break;
            total += r;
        }
        return total;
    }
    default:
        return 0;
    }
}

int dec_seek(Decoder* d, uint64_t frame) {
    switch (d->fmt) {
    case FMT_WAV:
        return drwav_seek_to_pcm_frame(d->wav, (drwav_uint64)frame) ? 0 : -1;
    case FMT_MP3:
        return drmp3_seek_to_pcm_frame(d->mp3, (drmp3_uint64)frame) ? 0 : -1;
    case FMT_FLAC:
        return drflac_seek_to_pcm_frame(d->flac, (drflac_uint64)frame) ? 0 : -1;
    case FMT_OGG:
        return stb_vorbis_seek(d->ogg, (unsigned int)frame) ? 0 : -1;
    default:
        return -1;
    }
}

void dec_close(Decoder* d) {
    switch (d->fmt) {
    case FMT_WAV:
        if (d->wav) { drwav_uninit(d->wav); free(d->wav); }
        break;
    case FMT_MP3:
        if (d->mp3) { drmp3_uninit(d->mp3); free(d->mp3); }
        break;
    case FMT_FLAC:
        if (d->flac) drflac_close(d->flac);
        break;
    case FMT_OGG:
        if (d->ogg) stb_vorbis_close(d->ogg);
        break;
    default: break;
    }
    memset(d, 0, sizeof(*d));
}
