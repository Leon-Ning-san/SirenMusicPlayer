#include "decode.h"
#include "util.h"
#include "plat.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#undef STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_IMPLEMENTATION
#include "stb_vorbis.c"

// ---------------- 纯 C AIFF / AIFC 解码器（全平台通用） ----------------
struct AiffDec {
    FILE* f;
    long data_off;
    uint32_t channels;
    uint32_t bits;
    int is_le;
    int is_float;
    uint64_t total_frames;
    uint64_t pos_frame;
};

static uint16_t aiff_be16(const unsigned char* p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static uint32_t aiff_be32(const unsigned char* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static double aiff_be80_to_double(const unsigned char b[10]) {
    int exp = ((b[0] & 0x7F) << 8) | b[1];
    uint64_t mant = 0;
    for (int i = 2; i < 10; i++) mant = (mant << 8) | b[i];
    if ((exp == 0 && mant == 0) || exp == 0x7FFF) return 0.0;
    double val = ldexp((double)mant, exp - 16383 - 63);
    return (b[0] & 0x80) ? -val : val;
}

static int aiff_open(Decoder* d, const char* path) {
    FILE* f = plat_fopen(path, "rb");
    if (!f) return -1;
    unsigned char hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "FORM", 4) != 0 ||
        (memcmp(hdr + 8, "AIFF", 4) != 0 && memcmp(hdr + 8, "AIFC", 4) != 0)) {
        fclose(f);
        return -1;
    }
    int is_aifc = (memcmp(hdr + 8, "AIFC", 4) == 0);
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    long pos = 12;

    uint32_t ch = 0, bits = 0, rate = 0;
    uint64_t frames = 0;
    long data_off = -1;
    int is_le = 0, is_float = 0, bad_codec = 0;

    while (pos + 8 <= fsz) {
        unsigned char ck[8];
        if (fseek(f, pos, SEEK_SET) != 0 || fread(ck, 1, 8, f) != 8) break;
        uint32_t cksz = aiff_be32(ck + 4);
        long cdata = pos + 8;
        if (cdata + (long)cksz > fsz) break;
        if (memcmp(ck, "COMM", 4) == 0 && cksz >= 18) {
            unsigned char cb[22];
            size_t want = cksz >= 22 ? 22 : 18;
            if (fread(cb, 1, want, f) != want) break;
            ch = aiff_be16(cb);
            frames = aiff_be32(cb + 2);
            bits = aiff_be16(cb + 6);
            rate = (uint32_t)(aiff_be80_to_double(cb + 8) + 0.5);
            if (is_aifc && want >= 22) {
                if (memcmp(cb + 18, "sowt", 4) == 0) is_le = 1;
                else if (memcmp(cb + 18, "fl32", 4) == 0 || memcmp(cb + 18, "FL32", 4) == 0) is_float = 1;
                else if (memcmp(cb + 18, "NONE", 4) != 0 && memcmp(cb + 18, "twos", 4) != 0) bad_codec = 1;
            }
        } else if (memcmp(ck, "SSND", 4) == 0 && cksz >= 8) {
            unsigned char sb[8];
            if (fread(sb, 1, 8, f) != 8) break;
            uint32_t ssnd_off = aiff_be32(sb);
            data_off = cdata + 8 + (long)ssnd_off;
        }
        pos = cdata + (long)((cksz + 1u) & ~1u);
    }

    if (bad_codec || data_off < 0 || ch < 1 || ch > 8 || rate == 0 ||
        (bits != 8 && bits != 16 && bits != 24 && bits != 32)) {
        fclose(f);
        return -1;
    }
    if (fseek(f, data_off, SEEK_SET) != 0) { fclose(f); return -1; }
    AiffDec* a = (AiffDec*)calloc(1, sizeof(AiffDec));
    if (!a) { fclose(f); return -1; }
    a->f = f;
    a->data_off = data_off;
    a->channels = ch;
    a->bits = bits;
    a->is_le = is_le;
    a->is_float = is_float;
    a->total_frames = frames;
    d->fmt = FMT_AIFF;
    d->aiff = a;
    d->channels = ch;
    d->rate = rate;
    d->total_frames = frames;
    return 0;
}

static int aiff_read(Decoder* d, int16_t* out, int want) {
    AiffDec* a = d->aiff;
    if (!a || !a->f || want <= 0) return 0;
    if (a->total_frames > 0 && a->pos_frame >= a->total_frames) return 0;
    int bps = (int)(a->bits / 8);
    int fb = (int)a->channels * bps;
    uint64_t rem = (a->total_frames > 0) ? (a->total_frames - a->pos_frame) : (uint64_t)want;
    if ((uint64_t)want > rem) want = (int)rem;
    unsigned char raw[4096 * 4];
    int total = 0;
    while (total < want) {
        int batch = want - total;
        int max_batch = (int)sizeof(raw) / fb;
        if (batch > max_batch) batch = max_batch;
        size_t got = fread(raw, (size_t)fb, (size_t)batch, a->f);
        if (got == 0) break;
        size_t nsamp = got * a->channels;
        for (size_t i = 0; i < nsamp; i++) {
            const unsigned char* p = raw + i * (size_t)bps;
            int16_t s = 0;
            if (a->bits == 8) {
                s = (int16_t)((int8_t)p[0] << 8);
            } else if (a->bits == 16) {
                s = a->is_le ? (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8))
                             : (int16_t)(((uint16_t)p[0] << 8) | p[1]);
            } else if (a->bits == 24) {
                uint32_t u = a->is_le
                    ? ((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16))
                    : (((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]);
                if (u & 0x800000u) u |= 0xFF000000u;
                s = (int16_t)(((int32_t)u) >> 8);
            } else if (a->bits == 32) {
                uint32_t u = a->is_le
                    ? ((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24))
                    : (((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]);
                if (a->is_float) {
                    float fv;
                    memcpy(&fv, &u, 4);
                    if (fv > 1.0f) fv = 1.0f;
                    if (fv < -1.0f) fv = -1.0f;
                    s = (int16_t)(fv * 32767.0f);
                } else {
                    s = (int16_t)(((int32_t)u) >> 16);
                }
            }
            out[(size_t)total * a->channels + i] = s;
        }
        total += (int)got;
        a->pos_frame += (uint64_t)got;
    }
    return total;
}

static int aiff_seek(Decoder* d, uint64_t frame) {
    AiffDec* a = d->aiff;
    if (!a || !a->f) return -1;
    if (a->total_frames > 0 && frame > a->total_frames) frame = a->total_frames;
    long off = a->data_off + (long)(frame * a->channels * (a->bits / 8));
    if (fseek(a->f, off, SEEK_SET) != 0) return -1;
    a->pos_frame = frame;
    return 0;
}

static void aiff_close(Decoder* d) {
    if (d->aiff) {
        if (d->aiff->f) fclose(d->aiff->f);
        free(d->aiff);
        d->aiff = NULL;
    }
}

// ---------------- 平台原生解码器（M4A / AAC / MP4 / ALAC / OPUS / WMA / CAF 等） ----------------
#if defined(_WIN32)
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

typedef HRESULT (WINAPI *PFN_MFStartup)(ULONG, DWORD);
typedef HRESULT (WINAPI *PFN_MFCreateMediaType)(IMFMediaType**);
typedef HRESULT (WINAPI *PFN_MFCreateSourceReaderFromURL)(LPCWSTR, IMFAttributes*, IMFSourceReader**);

typedef struct {
    IMFSourceReader* reader;
    int16_t* buf;
    int buf_frames;
    int buf_pos;
    int buf_cap;
    int eof;
} WinMFDec;

static const GUID g_GUID_NULL = {0,0,0,{0,0,0,0,0,0,0,0}};
static const GUID g_MFMediaType_Audio = {0x73647561, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID g_MFAudioFormat_PCM = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID g_MF_MT_MAJOR_TYPE = {0x48eba18e, 0xf8c9, 0x4687, {0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f}};
static const GUID g_MF_MT_SUBTYPE = {0xf7e34c9a, 0x42e8, 0x4714, {0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5}};
static const GUID g_MF_MT_AUDIO_NUM_CHANNELS = {0x37e48bf5, 0x645e, 0x4c5b, {0x89, 0xde, 0xad, 0xa9, 0xe2, 0x9b, 0x69, 0x6a}};
static const GUID g_MF_MT_AUDIO_SAMPLES_PER_SECOND = {0x5faeeae7, 0x0290, 0x4c31, {0x9e, 0x8a, 0xc5, 0x34, 0xf6, 0x8d, 0x9d, 0xba}};
static const GUID g_MF_MT_AUDIO_BITS_PER_SAMPLE = {0xf2deb57f, 0x40fa, 0x4764, {0xaa, 0x33, 0xed, 0x4f, 0x2d, 0x1f, 0xf6, 0x69}};
static const GUID g_MF_PD_DURATION = {0x6c990d33, 0xbb8e, 0x477a, {0x85, 0x98, 0x0d, 0x5d, 0x96, 0xfc, 0xd8, 0x8a}};

static PFN_MFStartup pMFStartup = NULL;
static PFN_MFCreateMediaType pMFCreateMediaType = NULL;
static PFN_MFCreateSourceReaderFromURL pMFCreateSourceReaderFromURL = NULL;

static int mf_ensure_init(void) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (pMFStartup && pMFCreateMediaType && pMFCreateSourceReaderFromURL) return 1;
    HMODULE mfp = LoadLibraryW(L"mfplat.dll");
    HMODULE mfr = LoadLibraryW(L"mfreadwrite.dll");
    if (!mfp || !mfr) return 0;
    pMFStartup = (PFN_MFStartup)(void*)GetProcAddress(mfp, "MFStartup");
    pMFCreateMediaType = (PFN_MFCreateMediaType)(void*)GetProcAddress(mfp, "MFCreateMediaType");
    pMFCreateSourceReaderFromURL = (PFN_MFCreateSourceReaderFromURL)(void*)GetProcAddress(mfr, "MFCreateSourceReaderFromURL");
    if (!pMFStartup || !pMFCreateMediaType || !pMFCreateSourceReaderFromURL) return 0;
    if (FAILED(pMFStartup(MF_VERSION, MFSTARTUP_LITE))) return 0;
    return 1;
}

static int native_open(Decoder* d, const char* path) {
    if (!mf_ensure_init()) return -1;
    wchar_t wpath[1024];
    if (!plat_utf8_to_wide(path, wpath, 1024)) return -1;

    IMFSourceReader* reader = NULL;
    if (FAILED(pMFCreateSourceReaderFromURL(wpath, NULL, &reader)) || !reader) return -1;

    IMFSourceReader_SetStreamSelection(reader, MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (FAILED(IMFSourceReader_SetStreamSelection(reader, MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE))) {
        IMFSourceReader_Release(reader);
        return -1;
    }

    IMFMediaType* pType = NULL;
    if (FAILED(pMFCreateMediaType(&pType)) || !pType) {
        IMFSourceReader_Release(reader);
        return -1;
    }
    IMFMediaType_SetGUID(pType, &g_MF_MT_MAJOR_TYPE, &g_MFMediaType_Audio);
    IMFMediaType_SetGUID(pType, &g_MF_MT_SUBTYPE, &g_MFAudioFormat_PCM);
    IMFMediaType_SetUINT32(pType, &g_MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    HRESULT hr = IMFSourceReader_SetCurrentMediaType(reader, MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, pType);
    IMFMediaType_Release(pType);
    if (FAILED(hr)) {
        IMFSourceReader_Release(reader);
        return -1;
    }

    IMFMediaType* curType = NULL;
    if (FAILED(IMFSourceReader_GetCurrentMediaType(reader, MF_SOURCE_READER_FIRST_AUDIO_STREAM, &curType)) || !curType) {
        IMFSourceReader_Release(reader);
        return -1;
    }
    UINT32 ch = 0, rate = 0, bits = 0;
    IMFMediaType_GetUINT32(curType, &g_MF_MT_AUDIO_NUM_CHANNELS, &ch);
    IMFMediaType_GetUINT32(curType, &g_MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    IMFMediaType_GetUINT32(curType, &g_MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
    IMFMediaType_Release(curType);
    if (ch < 1 || ch > 8) {
        IMFMediaType* pStereo = NULL;
        if (SUCCEEDED(pMFCreateMediaType(&pStereo)) && pStereo) {
            IMFMediaType_SetGUID(pStereo, &g_MF_MT_MAJOR_TYPE, &g_MFMediaType_Audio);
            IMFMediaType_SetGUID(pStereo, &g_MF_MT_SUBTYPE, &g_MFAudioFormat_PCM);
            IMFMediaType_SetUINT32(pStereo, &g_MF_MT_AUDIO_NUM_CHANNELS, 2);
            IMFMediaType_SetUINT32(pStereo, &g_MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            if (SUCCEEDED(IMFSourceReader_SetCurrentMediaType(reader, MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, pStereo))) {
                ch = 2; bits = 16;
            }
            IMFMediaType_Release(pStereo);
        }
    }
    if (ch < 1 || ch > 8 || rate == 0 || bits != 16) {
        IMFSourceReader_Release(reader);
        return -1;
    }

    PROPVARIANT var;
    memset(&var, 0, sizeof(var));
    uint64_t total_frames = 0;
    if (SUCCEEDED(IMFSourceReader_GetPresentationAttribute(reader, MF_SOURCE_READER_MEDIASOURCE, &g_MF_PD_DURATION, &var))) {
        if (var.vt == VT_UI8 || var.vt == VT_I8) {
            total_frames = (uint64_t)((double)var.uhVal.QuadPart * (double)rate / 10000000.0);
        }
    }

    WinMFDec* mf = (WinMFDec*)calloc(1, sizeof(WinMFDec));
    if (!mf) { IMFSourceReader_Release(reader); return -1; }
    mf->reader = reader;
    d->fmt = FMT_MF;
    d->nat = mf;
    d->channels = ch;
    d->rate = rate;
    d->total_frames = total_frames;
    return 0;
}

static int native_read(Decoder* d, int16_t* out, int want) {
    WinMFDec* mf = (WinMFDec*)d->nat;
    if (!mf || !mf->reader || want <= 0) return 0;
    int nc = (int)d->channels;
    int total = 0;
    while (total < want) {
        if (mf->buf_pos < mf->buf_frames) {
            int avail = mf->buf_frames - mf->buf_pos;
            int take = (want - total < avail) ? (want - total) : avail;
            memcpy(out + (size_t)total * nc, mf->buf + (size_t)mf->buf_pos * nc, (size_t)take * nc * sizeof(int16_t));
            mf->buf_pos += take;
            total += take;
            continue;
        }
        if (mf->eof) break;
        DWORD flags = 0;
        IMFSample* sample = NULL;
        HRESULT hr = IMFSourceReader_ReadSample(mf->reader, MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, NULL, &flags, NULL, &sample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
            if (sample) IMFSample_Release(sample);
            mf->eof = 1;
            break;
        }
        if (!sample) continue;
        IMFMediaBuffer* mbuf = NULL;
        if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(sample, &mbuf)) && mbuf) {
            BYTE* ptr = NULL;
            DWORD blen = 0;
            if (SUCCEEDED(IMFMediaBuffer_Lock(mbuf, &ptr, NULL, &blen)) && ptr && blen > 0) {
                int frames = (int)(blen / (DWORD)(nc * sizeof(int16_t)));
                if (frames > 0) {
                    if (frames > mf->buf_cap) {
                        int16_t* nb = (int16_t*)realloc(mf->buf, (size_t)frames * nc * sizeof(int16_t));
                        if (nb) { mf->buf = nb; mf->buf_cap = frames; }
                    }
                    if (mf->buf && frames <= mf->buf_cap) {
                        memcpy(mf->buf, ptr, (size_t)frames * nc * sizeof(int16_t));
                        mf->buf_frames = frames;
                        mf->buf_pos = 0;
                    }
                }
                IMFMediaBuffer_Unlock(mbuf);
            }
            IMFMediaBuffer_Release(mbuf);
        }
        IMFSample_Release(sample);
    }
    return total;
}

static int native_seek(Decoder* d, uint64_t frame) {
    WinMFDec* mf = (WinMFDec*)d->nat;
    if (!mf || !mf->reader || d->rate == 0) return -1;
    mf->buf_pos = mf->buf_frames = 0;
    mf->eof = 0;
    IMFSourceReader_Flush(mf->reader, MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    PROPVARIANT var;
    memset(&var, 0, sizeof(var));
    var.vt = VT_I8;
    var.hVal.QuadPart = (LONGLONG)(((double)frame / (double)d->rate) * 10000000.0);
    return SUCCEEDED(IMFSourceReader_SetCurrentPosition(mf->reader, &g_GUID_NULL, &var)) ? 0 : -1;
}

static void native_close(Decoder* d) {
    WinMFDec* mf = (WinMFDec*)d->nat;
    if (mf) {
        if (mf->reader) IMFSourceReader_Release(mf->reader);
        free(mf->buf);
        free(mf);
        d->nat = NULL;
    }
}

#elif defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

typedef struct {
    ExtAudioFileRef ref;
} MacATDec;

static int native_open(Decoder* d, const char* path) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8*)path, (CFIndex)strlen(path), false);
    if (!url) return -1;
    ExtAudioFileRef ref = NULL;
    OSStatus st = ExtAudioFileOpenURL(url, &ref);
    CFRelease(url);
    if (st != noErr || !ref) return -1;

    AudioStreamBasicDescription srcFmt;
    UInt32 propSz = sizeof(srcFmt);
    memset(&srcFmt, 0, sizeof(srcFmt));
    if (ExtAudioFileGetProperty(ref, kExtAudioFileProperty_FileDataFormat, &propSz, &srcFmt) != noErr ||
        srcFmt.mSampleRate <= 0 || srcFmt.mChannelsPerFrame < 1) {
        ExtAudioFileDispose(ref);
        return -1;
    }
    UInt32 ch = srcFmt.mChannelsPerFrame > 8 ? 2 : srcFmt.mChannelsPerFrame;

    AudioStreamBasicDescription dstFmt;
    memset(&dstFmt, 0, sizeof(dstFmt));
    dstFmt.mSampleRate = srcFmt.mSampleRate;
    dstFmt.mFormatID = kAudioFormatLinearPCM;
    dstFmt.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    dstFmt.mBitsPerChannel = 16;
    dstFmt.mChannelsPerFrame = ch;
    dstFmt.mBytesPerFrame = ch * 2;
    dstFmt.mFramesPerPacket = 1;
    dstFmt.mBytesPerPacket = ch * 2;
    if (ExtAudioFileSetProperty(ref, kExtAudioFileProperty_ClientDataFormat, sizeof(dstFmt), &dstFmt) != noErr) {
        ExtAudioFileDispose(ref);
        return -1;
    }

    SInt64 totalFrames = 0;
    propSz = sizeof(totalFrames);
    ExtAudioFileGetProperty(ref, kExtAudioFileProperty_FileLengthFrames, &propSz, &totalFrames);

    MacATDec* at = (MacATDec*)calloc(1, sizeof(MacATDec));
    if (!at) { ExtAudioFileDispose(ref); return -1; }
    at->ref = ref;
    d->fmt = FMT_AT;
    d->nat = at;
    d->channels = ch;
    d->rate = (uint32_t)(srcFmt.mSampleRate + 0.5);
    d->total_frames = totalFrames > 0 ? (uint64_t)totalFrames : 0;
    return 0;
}

static int native_read(Decoder* d, int16_t* out, int want) {
    MacATDec* at = (MacATDec*)d->nat;
    if (!at || !at->ref || want <= 0) return 0;
    UInt32 ioFrames = (UInt32)want;
    AudioBufferList abl;
    abl.mNumberBuffers = 1;
    abl.mBuffers[0].mNumberChannels = d->channels;
    abl.mBuffers[0].mDataByteSize = (UInt32)(want * (int)d->channels * 2);
    abl.mBuffers[0].mData = out;
    if (ExtAudioFileRead(at->ref, &ioFrames, &abl) != noErr) return 0;
    return (int)ioFrames;
}

static int native_seek(Decoder* d, uint64_t frame) {
    MacATDec* at = (MacATDec*)d->nat;
    if (!at || !at->ref) return -1;
    return ExtAudioFileSeek(at->ref, (SInt64)frame) == noErr ? 0 : -1;
}

static void native_close(Decoder* d) {
    MacATDec* at = (MacATDec*)d->nat;
    if (at) {
        if (at->ref) ExtAudioFileDispose(at->ref);
        free(at);
        d->nat = NULL;
    }
}

#elif defined(__linux__)
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

typedef struct {
    char path[600];
    int fd;
    pid_t pid;
} LinuxFFDec;

static int ff_spawn(LinuxFFDec* ff, uint32_t rate, uint32_t ch, double start_sec) {
    int pfd[2];
    if (pipe(pfd) != 0) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[1]);
        char ss_buf[32], ac_buf[16], ar_buf[16];
        snprintf(ss_buf, sizeof(ss_buf), "%.3f", start_sec);
        snprintf(ac_buf, sizeof(ac_buf), "%u", ch);
        snprintf(ar_buf, sizeof(ar_buf), "%u", rate);
        char* argv[] = {
            "ffmpeg", "-v", "error", "-nostdin",
            "-ss", ss_buf, "-i", ff->path,
            "-vn", "-sn", "-dn",
            "-f", "s16le", "-acodec", "pcm_s16le",
            "-ac", ac_buf, "-ar", ar_buf, "pipe:1", NULL
        };
        execvp("ffmpeg", argv);
        _exit(127);
    }
    close(pfd[1]);
    if (pid < 0) { close(pfd[0]); return -1; }
    ff->fd = pfd[0];
    ff->pid = pid;
    return 0;
}

static void ff_stop_proc(LinuxFFDec* ff) {
    if (ff->fd >= 0) { close(ff->fd); ff->fd = -1; }
    if (ff->pid > 0) {
        kill(ff->pid, SIGTERM);
        waitpid(ff->pid, NULL, 0);
        ff->pid = -1;
    }
}

static int native_open(Decoder* d, const char* path) {
    uint32_t ch = 2, rate = 44100;
    double dur = 0.0;
    int pfd[2];
    if (pipe(pfd) == 0) {
        pid_t pid = fork();
        if (pid == 0) {
            close(pfd[0]);
            dup2(pfd[1], STDOUT_FILENO);
            close(pfd[1]);
            char* argv[] = {
                "ffprobe", "-v", "error",
                "-select_streams", "a:0",
                "-show_entries", "stream=channels,sample_rate",
                "-show_entries", "format=duration",
                "-of", "default=noprint_wrappers=1",
                (char*)path, NULL
            };
            execvp("ffprobe", argv);
            _exit(127);
        }
        close(pfd[1]);
        if (pid > 0) {
            char buf[512];
            ssize_t n = read(pfd[0], buf, sizeof(buf) - 1);
            close(pfd[0]);
            int st = 0;
            waitpid(pid, &st, 0);
            if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) return -1;
            if (n > 0) {
                buf[n] = 0;
                const char* p = strstr(buf, "channels=");
                if (p) { int c = atoi(p + 9); if (c >= 1 && c <= 8) ch = (uint32_t)c; }
                p = strstr(buf, "sample_rate=");
                if (p) { int r = atoi(p + 12); if (r >= 8000 && r <= 384000) rate = (uint32_t)r; }
                p = strstr(buf, "duration=");
                if (p) { double dv = atof(p + 9); if (dv > 0.0) dur = dv; }
            }
        } else {
            close(pfd[0]);
            return -1;
        }
    }
    LinuxFFDec* ff = (LinuxFFDec*)calloc(1, sizeof(LinuxFFDec));
    if (!ff) return -1;
    ff->fd = -1; ff->pid = -1;
    snprintf(ff->path, sizeof(ff->path), "%s", path);
    if (ff_spawn(ff, rate, ch, 0.0) != 0) { free(ff); return -1; }
    d->fmt = FMT_FFPIPE;
    d->nat = ff;
    d->channels = ch;
    d->rate = rate;
    d->total_frames = dur > 0.0 ? (uint64_t)(dur * (double)rate) : 0;
    return 0;
}

static int native_read(Decoder* d, int16_t* out, int want) {
    LinuxFFDec* ff = (LinuxFFDec*)d->nat;
    if (!ff || ff->fd < 0 || want <= 0) return 0;
    size_t fb = (size_t)d->channels * sizeof(int16_t);
    size_t want_bytes = (size_t)want * fb;
    size_t got = 0;
    char* p = (char*)out;
    while (got < want_bytes) {
        ssize_t r = read(ff->fd, p + got, want_bytes - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    return (int)(got / fb);
}

static int native_seek(Decoder* d, uint64_t frame) {
    LinuxFFDec* ff = (LinuxFFDec*)d->nat;
    if (!ff || d->rate == 0) return -1;
    ff_stop_proc(ff);
    return ff_spawn(ff, d->rate, d->channels, (double)frame / (double)d->rate);
}

static void native_close(Decoder* d) {
    LinuxFFDec* ff = (LinuxFFDec*)d->nat;
    if (ff) {
        ff_stop_proc(ff);
        free(ff);
        d->nat = NULL;
    }
}

#else
static int native_open(Decoder* d, const char* path) { (void)d; (void)path; return -1; }
static int native_read(Decoder* d, int16_t* out, int want) { (void)d; (void)out; (void)want; return 0; }
static int native_seek(Decoder* d, uint64_t frame) { (void)d; (void)frame; return -1; }
static void native_close(Decoder* d) { (void)d; }
#endif

// ---------------- 公共接口 ----------------
int dec_is_audio(const char* path) {
    if (ci_ends_with(path, ".mp3") || ci_ends_with(path, ".flac") ||
        ci_ends_with(path, ".wav") || ci_ends_with(path, ".wave") ||
        ci_ends_with(path, ".ogg") || ci_ends_with(path, ".oga") ||
        ci_ends_with(path, ".aiff") || ci_ends_with(path, ".aif") || ci_ends_with(path, ".aifc")) {
        return 1;
    }
#ifndef __SWITCH__
    if (ci_ends_with(path, ".m4a") || ci_ends_with(path, ".aac") ||
        ci_ends_with(path, ".mp4") || ci_ends_with(path, ".m4b") ||
        ci_ends_with(path, ".alac") || ci_ends_with(path, ".opus") ||
        ci_ends_with(path, ".wma") || ci_ends_with(path, ".caf") ||
        ci_ends_with(path, ".ape") || ci_ends_with(path, ".wv")) {
        return 1;
    }
#endif
    return 0;
}

static int dec_open_ex(Decoder* d, const char* path, int build_seek_tbl) {
    memset(d, 0, sizeof(*d));
#ifdef _WIN32
    wchar_t wpath[1024];
    int has_w = plat_utf8_to_wide(path, wpath, 1024);
#endif
    if (ci_ends_with(path, ".wav") || ci_ends_with(path, ".wave")) {
        d->wav = (drwav*)malloc(sizeof(drwav));
#ifdef _WIN32
        int ok = d->wav && (has_w ? drwav_init_file_w(d->wav, wpath, NULL) : drwav_init_file(d->wav, path, NULL));
#else
        int ok = d->wav && drwav_init_file(d->wav, path, NULL);
#endif
        if (ok) {
            d->fmt = FMT_WAV;
            d->channels = d->wav->channels;
            d->rate = d->wav->sampleRate;
            d->total_frames = d->wav->totalPCMFrameCount;
            return 0;
        }
        free(d->wav); d->wav = NULL;
    }
    if (ci_ends_with(path, ".mp3")) {
        d->mp3 = (drmp3*)malloc(sizeof(drmp3));
#ifdef _WIN32
        int ok = d->mp3 && (has_w ? drmp3_init_file_w(d->mp3, wpath, NULL) : drmp3_init_file(d->mp3, path, NULL));
#else
        int ok = d->mp3 && drmp3_init_file(d->mp3, path, NULL);
#endif
        if (ok) {
            d->fmt = FMT_MP3;
            d->channels = d->mp3->channels;
            d->rate = d->mp3->sampleRate;
            d->total_frames = drmp3_get_pcm_frame_count(d->mp3);
            if (build_seek_tbl && d->total_frames > 0) {
                drmp3_uint32 cnt = 128;
                drmp3_seek_point* pts = (drmp3_seek_point*)malloc(sizeof(drmp3_seek_point) * cnt);
                if (pts) {
                    if (drmp3_calculate_seek_points(d->mp3, &cnt, pts) && cnt > 0) {
                        drmp3_bind_seek_table(d->mp3, cnt, pts);
                        d->mp3_seek_pts = pts;
                        d->mp3_seek_cnt = cnt;
                    } else {
                        free(pts);
                    }
                }
            }
            return 0;
        }
        free(d->mp3); d->mp3 = NULL;
    }
    if (ci_ends_with(path, ".flac")) {
#ifdef _WIN32
        d->flac = has_w ? drflac_open_file_w(wpath, NULL) : drflac_open_file(path, NULL);
#else
        d->flac = drflac_open_file(path, NULL);
#endif
        if (d->flac) {
            d->fmt = FMT_FLAC;
            d->channels = d->flac->channels;
            d->rate = d->flac->sampleRate;
            d->total_frames = d->flac->totalPCMFrameCount;
            return 0;
        }
    }
    if (ci_ends_with(path, ".ogg") || ci_ends_with(path, ".oga")) {
        int err = 0;
#ifdef _WIN32
        FILE* fp = plat_fopen(path, "rb");
        d->ogg = fp ? stb_vorbis_open_file(fp, 1, &err, NULL) : NULL;
        if (!d->ogg && fp) fclose(fp);
#else
        d->ogg = stb_vorbis_open_filename(path, &err, NULL);
#endif
        if (d->ogg) {
            d->fmt = FMT_OGG;
            stb_vorbis_info info = stb_vorbis_get_info(d->ogg);
            d->channels = info.channels;
            d->rate = info.sample_rate;
            d->total_frames = stb_vorbis_stream_length_in_samples(d->ogg);
            return 0;
        }
    }
    if (ci_ends_with(path, ".aiff") || ci_ends_with(path, ".aif") || ci_ends_with(path, ".aifc")) {
        if (aiff_open(d, path) == 0) return 0;
    }
    if (native_open(d, path) == 0) return 0;
    memset(d, 0, sizeof(*d));
    return -1;
}

int dec_open(Decoder* d, const char* path) {
    return dec_open_ex(d, path, 1);
}

int dec_probe_info(const char* path, double* dur_out, uint32_t* rate_out, uint32_t* ch_out) {
    Decoder d;
    if (dec_open_ex(&d, path, 0) != 0) return -1;
    if (dur_out)  *dur_out  = (d.rate > 0 && d.total_frames > 0) ? ((double)d.total_frames / (double)d.rate) : 0.0;
    if (rate_out) *rate_out = d.rate;
    if (ch_out)   *ch_out   = d.channels;
    dec_close(&d);
    return 0;
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
    case FMT_OGG:
        return stb_vorbis_get_samples_short_interleaved(d->ogg, (int)d->channels, (short*)out, want * (int)d->channels);
    case FMT_AIFF:
        return aiff_read(d, out, want);
    case FMT_MF:
    case FMT_AT:
    case FMT_FFPIPE:
        return native_read(d, out, want);
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
    case FMT_AIFF:
        return aiff_seek(d, frame);
    case FMT_MF:
    case FMT_AT:
    case FMT_FFPIPE:
        return native_seek(d, frame);
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
        free(d->mp3_seek_pts);
        break;
    case FMT_FLAC:
        if (d->flac) drflac_close(d->flac);
        break;
    case FMT_OGG:
        if (d->ogg) stb_vorbis_close(d->ogg);
        break;
    case FMT_AIFF:
        aiff_close(d);
        break;
    case FMT_MF:
    case FMT_AT:
    case FMT_FFPIPE:
        native_close(d);
        break;
    default: break;
    }
    memset(d, 0, sizeof(*d));
}
