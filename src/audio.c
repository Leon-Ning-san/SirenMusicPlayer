#include "audio.h"
#include "decode.h"
#include "plat.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#if defined(_WIN32) && !defined(USE_SDL_AUDIO)
#define AUDIO_USE_WINMM 1
#include <mmsystem.h>
#else
#define AUDIO_USE_SDL 1
#include <SDL2/SDL.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define RING_FRAMES   (48000 * 12)   // 12 seconds @48k stereo
#define CHUNK_FRAMES  4096
#define OUT_RATE      48000
#define SPEC_N        2048
#define QUEUE_TARGET  (48000 * 2 * 2 * 15 / 100)  // keep ~150ms queued (bytes)

// ---------------- ring buffer (decoder thread -> main thread) ----------------
static int16_t* ring = NULL;
static volatile uint64_t ring_w = 0;
static volatile uint64_t ring_r = 0;
static PlatMutex ringmtx;

static uint64_t ring_count(void) { return ring_w - ring_r; }
static uint64_t ring_free(void) { return (uint64_t)RING_FRAMES - (ring_w - ring_r); }

// ---------------- decoder thread state ----------------
static PlatThread decThread;
static volatile int thrun = 1;
static volatile int a_paused = 0;
static volatile float a_volume = 0.85f;
static volatile int a_status = 0;        // 0 idle, 1 opening, 2 ready, 3 failed
static volatile double a_duration = 0;
static volatile uint32_t a_rate = OUT_RATE;
static volatile int dec_eof = 0;

static Decoder dec;

// resampler state (decoder thread only)
static double rs_pos = 0;
static int16_t rs_last[2] = {0, 0};
static int rs_has_last = 0;
static int fade_in_rem = 0;
static volatile uint64_t out_total = 0;

// command protocol
static volatile int cmd_seq = 0;
static volatile int cmd_done = 0;
static char cmd_path[600];
static volatile int cmd_is_play = 0;
static volatile uint64_t cmd_seek_frame = 0;
static volatile int cmd_seek_ok = 0;

// ---------------- audio output device ----------------
#if AUDIO_USE_SDL
static SDL_AudioDeviceID sdl_dev = 0;
#else
#define WIN_BUFS 12
#define WIN_BUF_FRAMES 2048
static HWAVEOUT hwo = NULL;
static WAVEHDR whdr[WIN_BUFS];
static int16_t* wbuf[WIN_BUFS];
#endif
static volatile uint64_t total_queued_bytes = 0;
static volatile double base_sec = 0;

// ---------------- diagnostics ----------------
static volatile int diag_sdl_init = 0;
static volatile int diag_dev_open = 0;
static volatile int diag_thread = 0;
static volatile uint64_t diag_decoded_chunks = 0;
static char diag_last_error[128] = {0};

// spectrum history (main thread)
static int16_t spec_hist[SPEC_N * 2];
static volatile uint64_t spec_w = 0;

// ---------------- resample & push into ring ----------------
static inline int16_t downmix_lr(const int16_t* frm, int channels, int lr) {
    if (channels <= 1) return frm[0];
    if (channels == 2) return frm[lr];
    int32_t v = frm[lr];
    if (channels >= 3) v += (frm[2] * 7) / 10;       // Center
    if (channels >= 6) v += (frm[4 + lr] * 7) / 10;  // Surround L/R
    if (channels >= 8) v += (frm[6 + lr] * 5) / 10;  // Back L/R
    v = (v * 3) / 4;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

static void push_ring_frame(int16_t l, int16_t r) {
    if (fade_in_rem > 0) {
        double g = (double)(240 - fade_in_rem) / 240.0;
        l = (int16_t)((double)l * g);
        r = (int16_t)((double)r * g);
        fade_in_rem--;
    }
    plat_mutex_lock(&ringmtx);
    uint64_t idx = ring_w % (uint64_t)RING_FRAMES;
    ring[(size_t)idx * 2 + 0] = l;
    ring[(size_t)idx * 2 + 1] = r;
    ring_w++;
    out_total++;
    plat_mutex_unlock(&ringmtx);
}

static void push_chunk(const int16_t* chunk, int frames, int channels) {
    if (frames <= 0) return;
    double step = (double)(a_rate > 0 ? a_rate : OUT_RATE) / (double)OUT_RATE;
    long need = (long)((double)frames / step) + 8;
    long avail = (long)ring_free();
    if (avail < need) need = avail;
    if (need <= 0) return;
    int16_t* tmp = (int16_t*)malloc((size_t)need * 2 * sizeof(int16_t));
    if (!tmp) return;
    long outn = 0;
    double limit = (double)(frames - 1);
    while (outn < need && rs_pos < limit) {
        int i0 = (int)floor(rs_pos);
        if (i0 < -1) { rs_pos += step; continue; }
        if (i0 < 0 && !rs_has_last) { i0 = 0; }
        double frac = rs_pos - floor(rs_pos);
        if (frac < 0.0) frac = 0.0;
        if (frac > 1.0) frac = 1.0;
        int i1 = i0 + 1;
        if (i1 >= frames) break;
        const int16_t* p0 = (i0 >= 0) ? (chunk + (size_t)i0 * channels) : NULL;
        const int16_t* p1 = chunk + (size_t)i1 * channels;
        for (int c = 0; c < 2; c++) {
            int16_t s0 = p0 ? downmix_lr(p0, channels, c) : rs_last[c];
            int16_t s1 = downmix_lr(p1, channels, c);
            double v = (double)s0 + ((double)s1 - (double)s0) * frac;
            int iv = (int)lrint(v);
            if (fade_in_rem > 0) {
                double g = (double)(240 - fade_in_rem) / 240.0;
                iv = (int)lrint((double)iv * g);
            }
            if (iv > 32767) iv = 32767;
            if (iv < -32768) iv = -32768;
            tmp[(size_t)outn * 2 + c] = (int16_t)iv;
        }
        if (fade_in_rem > 0) fade_in_rem--;
        outn++;
        rs_pos += step;
    }
    const int16_t* plast = chunk + (size_t)(frames - 1) * channels;
    rs_last[0] = downmix_lr(plast, channels, 0);
    rs_last[1] = downmix_lr(plast, channels, 1);
    rs_has_last = 1;
    rs_pos -= (double)frames;
    if (outn > 0) {
        plat_mutex_lock(&ringmtx);
        for (long i = 0; i < outn; i++) {
            uint64_t idx = ring_w % (uint64_t)RING_FRAMES;
            ring[(size_t)idx * 2 + 0] = tmp[(size_t)i * 2 + 0];
            ring[(size_t)idx * 2 + 1] = tmp[(size_t)i * 2 + 1];
            ring_w++;
        }
        out_total += (uint64_t)outn;
        plat_mutex_unlock(&ringmtx);
    }
    free(tmp);
}

static void decoder_thread(void* arg) {
    (void)arg;
    static int16_t chunk[CHUNK_FRAMES * 8];
    while (thrun) {
        int seq = __atomic_load_n(&cmd_seq, __ATOMIC_SEQ_CST);
        if (seq != cmd_done) {
            if (cmd_is_play) {
                dec_close(&dec);
                plat_mutex_lock(&ringmtx);
                ring_w = ring_r = 0;
                out_total = 0;
                plat_mutex_unlock(&ringmtx);
                dec_eof = 0;
                rs_pos = 0;
                rs_last[0] = rs_last[1] = 0;
                rs_has_last = 0;
                fade_in_rem = 240;
                a_status = 1;
                if (dec_open(&dec, cmd_path) == 0 && dec.channels >= 1 && dec.channels <= 8 && dec.rate > 0) {
                    a_rate = dec.rate;
                    a_duration = dec.total_frames > 0 ? (double)dec.total_frames / (double)dec.rate : 0;
                    a_status = 2;
                } else {
                    dec_close(&dec);
                    a_duration = 0;
                    a_status = 3;
                    snprintf(diag_last_error, sizeof(diag_last_error), "解码失败: %.110s", cmd_path);
                }
            } else {
                int ok = (dec.fmt != FMT_NONE && dec_seek(&dec, cmd_seek_frame) == 0) ? 1 : 0;
                if (ok) {
                    plat_mutex_lock(&ringmtx);
                    ring_w = ring_r = 0;
                    out_total = 0;
                    plat_mutex_unlock(&ringmtx);
                    dec_eof = 0;
                    rs_pos = 0;
                    rs_last[0] = rs_last[1] = 0;
                    rs_has_last = 0;
                    fade_in_rem = 240;
                }
                __atomic_store_n(&cmd_seek_ok, ok, __ATOMIC_SEQ_CST);
            }
            __atomic_store_n(&cmd_done, seq, __ATOMIC_SEQ_CST);
        }

        if (a_status == 2 && !a_paused && !dec_eof && dec.fmt != FMT_NONE) {
            uint64_t max_out = (uint64_t)((double)CHUNK_FRAMES * (double)OUT_RATE / (double)(a_rate > 0 ? a_rate : OUT_RATE)) + 64;
            if (max_out > (uint64_t)(RING_FRAMES / 2)) max_out = (uint64_t)(RING_FRAMES / 2);
            if (ring_free() >= max_out) {
                int got = dec_read(&dec, chunk, CHUNK_FRAMES);
                if (got <= 0) {
                    if (rs_has_last && ring_free() >= 1) {
                        push_ring_frame(rs_last[0], rs_last[1]);
                        rs_has_last = 0;
                    }
                    dec_eof = 1;
                } else {
                    push_chunk(chunk, got, (int)dec.channels);
                    diag_decoded_chunks++;
                }
            } else {
                plat_sleep_ms(3);
            }
        } else {
            plat_sleep_ms(6);
        }
    }
}

#if AUDIO_USE_WINMM
static uint32_t win_queued_bytes(void) {
    if (!hwo) return 0;
    uint32_t active_q = 0;
    for (int i = 0; i < WIN_BUFS; i++) {
        if ((whdr[i].dwFlags & WHDR_PREPARED) && !(whdr[i].dwFlags & WHDR_DONE))
            active_q += whdr[i].dwBufferLength;
    }
    if (active_q == 0) return 0;
    MMTIME mmt;
    mmt.wType = TIME_BYTES;
    if (waveOutGetPosition(hwo, &mmt, sizeof(mmt)) == MMSYSERR_NOERROR && mmt.wType == TIME_BYTES) {
        uint32_t q = 0;
        if ((uint64_t)mmt.u.cb < total_queued_bytes)
            q = (uint32_t)(total_queued_bytes - (uint64_t)mmt.u.cb);
        if (q > active_q) q = active_q;
        return q;
    }
    return active_q;
}
#endif

// ---------------- public API ----------------
int audio_init(void) {
#if AUDIO_USE_SDL
    diag_sdl_init = SDL_InitSubSystem(SDL_INIT_AUDIO);
    if (diag_sdl_init != 0) { snprintf(diag_last_error, sizeof(diag_last_error), "SDL_Init失败: %s", SDL_GetError()); return -1; }

    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq = OUT_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = NULL;
    sdl_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    diag_dev_open = sdl_dev ? 1 : 0;
    if (sdl_dev == 0) { snprintf(diag_last_error, sizeof(diag_last_error), "音频设备打开失败: %s", SDL_GetError()); return -1; }
    SDL_PauseAudioDevice(sdl_dev, 0);
#else
    WAVEFORMATEX wf;
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = OUT_RATE;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = OUT_RATE * 4;
    MMRESULT mr = waveOutOpen(&hwo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL);
    diag_sdl_init = (mr == MMSYSERR_NOERROR) ? 0 : -1;
    diag_dev_open = (hwo != NULL) ? 1 : 0;
    if (mr != MMSYSERR_NOERROR) {
        snprintf(diag_last_error, sizeof(diag_last_error), "waveOutOpen失败: %u", (unsigned)mr);
        return -1;
    }
    for (int i = 0; i < WIN_BUFS; i++) {
        wbuf[i] = (int16_t*)calloc(WIN_BUF_FRAMES * 2, sizeof(int16_t));
        memset(&whdr[i], 0, sizeof(WAVEHDR));
        whdr[i].dwFlags = WHDR_DONE;
    }
#endif

    ring = (int16_t*)malloc((size_t)RING_FRAMES * 2 * sizeof(int16_t));
    if (!ring) { snprintf(diag_last_error, sizeof(diag_last_error), "环形缓冲分配失败"); return -1; }
    plat_mutex_init(&ringmtx);
    if (plat_thread_create(&decThread, decoder_thread, NULL) != 0) {
        snprintf(diag_last_error, sizeof(diag_last_error), "解码线程创建失败");
        return -1;
    }
    diag_thread = 1;
    return 0;
}

void audio_exit(void) {
    thrun = 0;
    plat_thread_join(&decThread);
#if AUDIO_USE_SDL
    if (sdl_dev) {
        SDL_PauseAudioDevice(sdl_dev, 1);
        SDL_CloseAudioDevice(sdl_dev);
        sdl_dev = 0;
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
#else
    if (hwo) {
        waveOutReset(hwo);
        for (int i = 0; i < WIN_BUFS; i++) {
            if (whdr[i].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(hwo, &whdr[i], sizeof(WAVEHDR));
            free(wbuf[i]);
            wbuf[i] = NULL;
        }
        waveOutClose(hwo);
        hwo = NULL;
    }
#endif
    free(ring);
    ring = NULL;
}

static void wait_cmd_done(void) {
    for (int i = 0; i < 2000; i++) {
        if (__atomic_load_n(&cmd_done, __ATOMIC_SEQ_CST) >= __atomic_load_n(&cmd_seq, __ATOMIC_SEQ_CST)) return;
        plat_sleep_ms(2);
    }
}

static void reset_output(double base) {
    base_sec = base;
    total_queued_bytes = 0;
#if AUDIO_USE_SDL
    if (sdl_dev) SDL_ClearQueuedAudio(sdl_dev);
#else
    if (hwo) {
        waveOutReset(hwo);
        for (int i = 0; i < WIN_BUFS; i++) {
            if (whdr[i].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(hwo, &whdr[i], sizeof(WAVEHDR));
            memset(&whdr[i], 0, sizeof(WAVEHDR));
            whdr[i].dwFlags = WHDR_DONE;
        }
    }
#endif
}

int audio_play(const char* path) {
    snprintf(cmd_path, sizeof(cmd_path), "%s", path);
    cmd_is_play = 1;
    a_paused = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_add_fetch(&cmd_seq, 1, __ATOMIC_SEQ_CST);
    wait_cmd_done();
    reset_output(0);
#if AUDIO_USE_SDL
    if (sdl_dev) SDL_PauseAudioDevice(sdl_dev, 0);
#else
    if (hwo) waveOutRestart(hwo);
#endif
    return 0;
}

int audio_seek(double sec) {
    if (a_status != 2) return -1;
    if (sec < 0) sec = 0;
    double d = a_duration;
    if (d > 0.5 && sec > d - 0.25) sec = d - 0.25;
    cmd_seek_frame = (uint64_t)(sec * (double)a_rate);
    cmd_is_play = 0;
    __atomic_store_n(&cmd_seek_ok, 0, __ATOMIC_SEQ_CST);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_add_fetch(&cmd_seq, 1, __ATOMIC_SEQ_CST);
    wait_cmd_done();
    if (!__atomic_load_n(&cmd_seek_ok, __ATOMIC_SEQ_CST)) return -1;
    reset_output(sec);
    return 0;
}

void audio_pause(void) {
    a_paused = 1;
#if AUDIO_USE_SDL
    if (sdl_dev) SDL_PauseAudioDevice(sdl_dev, 1);
#else
    if (hwo) waveOutPause(hwo);
#endif
}
void audio_resume(void) {
    a_paused = 0;
#if AUDIO_USE_SDL
    if (sdl_dev) SDL_PauseAudioDevice(sdl_dev, 0);
#else
    if (hwo) waveOutRestart(hwo);
#endif
}
int audio_paused(void) { return a_paused; }

void audio_set_volume(float v) {
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    a_volume = v;
}
float audio_volume(void) { return a_volume; }

double audio_duration(void) { return a_duration; }

double audio_position(void) {
    if (a_status != 2) return 0;
#if AUDIO_USE_SDL
    uint32_t queued = sdl_dev ? SDL_GetQueuedAudioSize(sdl_dev) : 0;
#else
    uint32_t queued = win_queued_bytes();
#endif
    uint64_t played_bytes = total_queued_bytes;
    if (queued < played_bytes) played_bytes -= queued;
    else played_bytes = 0;
    double p = base_sec + (double)played_bytes / (4.0 * OUT_RATE);
    if (p < 0) p = 0;
    if (a_duration > 0 && p > a_duration) p = a_duration;
    return p;
}

int audio_status(void) { return a_status; }

int audio_finished(void) {
    if (a_status != 2) return 0;
#if AUDIO_USE_SDL
    uint32_t queued = sdl_dev ? SDL_GetQueuedAudioSize(sdl_dev) : 0;
#else
    uint32_t queued = win_queued_bytes();
#endif
    return (dec_eof && ring_count() == 0 && queued == 0) ? 1 : 0;
}

void audio_pump(void) {
    if (a_status != 2 || a_paused) return;
#if AUDIO_USE_SDL
    if (!sdl_dev) return;
    uint32_t queued = SDL_GetQueuedAudioSize(sdl_dev);
    if (queued >= QUEUE_TARGET) return;
    int want = (int)((QUEUE_TARGET - queued) / 4);
    if (want > 4096) want = 4096;

    int16_t buf[4096 * 2];
    plat_mutex_lock(&ringmtx);
    uint64_t avail = ring_count();
    if ((uint64_t)want > avail) want = (int)avail;
    float vol = a_volume;
    for (int i = 0; i < want; i++) {
        uint64_t idx = ring_r % (uint64_t)RING_FRAMES;
        buf[i * 2 + 0] = (int16_t)(ring[(size_t)idx * 2 + 0] * vol);
        buf[i * 2 + 1] = (int16_t)(ring[(size_t)idx * 2 + 1] * vol);
        ring_r++;
    }
    plat_mutex_unlock(&ringmtx);
    if (want <= 0) return;

    SDL_QueueAudio(sdl_dev, buf, (Uint32)want * 4);
    total_queued_bytes += (uint64_t)want * 4;

    for (int i = 0; i < want; i++) {
        uint64_t w = spec_w % (uint64_t)SPEC_N;
        spec_hist[(size_t)w * 2 + 0] = buf[i * 2 + 0];
        spec_hist[(size_t)w * 2 + 1] = buf[i * 2 + 1];
        spec_w++;
    }
#else
    if (!hwo) return;
    for (int b = 0; b < WIN_BUFS; b++) {
        if ((whdr[b].dwFlags & WHDR_PREPARED) && !(whdr[b].dwFlags & WHDR_DONE)) continue;
        if (win_queued_bytes() >= QUEUE_TARGET * 2) break;
        if (whdr[b].dwFlags & WHDR_PREPARED) {
            waveOutUnprepareHeader(hwo, &whdr[b], sizeof(WAVEHDR));
        }

        plat_mutex_lock(&ringmtx);
        uint64_t avail = ring_count();
        int want = WIN_BUF_FRAMES;
        if ((uint64_t)want > avail) want = (int)avail;
        float vol = a_volume;
        int16_t* dst = wbuf[b];
        for (int i = 0; i < want; i++) {
            uint64_t idx = ring_r % (uint64_t)RING_FRAMES;
            dst[i * 2 + 0] = (int16_t)(ring[(size_t)idx * 2 + 0] * vol);
            dst[i * 2 + 1] = (int16_t)(ring[(size_t)idx * 2 + 1] * vol);
            ring_r++;
        }
        plat_mutex_unlock(&ringmtx);
        if (want <= 0) { whdr[b].dwFlags = WHDR_DONE; break; }

        memset(&whdr[b], 0, sizeof(WAVEHDR));
        whdr[b].lpData = (LPSTR)dst;
        whdr[b].dwBufferLength = (DWORD)(want * 4);
        waveOutPrepareHeader(hwo, &whdr[b], sizeof(WAVEHDR));
        waveOutWrite(hwo, &whdr[b], sizeof(WAVEHDR));
        total_queued_bytes += (uint64_t)want * 4;

        for (int i = 0; i < want; i++) {
            uint64_t w = spec_w % (uint64_t)SPEC_N;
            spec_hist[(size_t)w * 2 + 0] = dst[i * 2 + 0];
            spec_hist[(size_t)w * 2 + 1] = dst[i * 2 + 1];
            spec_w++;
        }
    }
#endif
}

// ---------------- spectrum ----------------
void audio_get_spectrum(float out[28]) {
    if (a_paused || a_status != 2) {
        memset(out, 0, sizeof(float) * 28);
        return;
    }
    static float cc_tbl[28];
    static float hann[SPEC_N];
    static int inited = 0;
    if (!inited) {
        double f = 60.0;
        for (int i = 0; i < 28; i++) {
            double k = 2.0 * M_PI * f / (double)OUT_RATE;
            cc_tbl[i] = (float)(2.0 * cos(k));
            f *= pow(12000.0 / 60.0, 1.0 / 27.0);
        }
        for (int n = 0; n < SPEC_N; n++) {
            hann[n] = (float)(0.5 * (1.0 - cos(2.0 * M_PI * n / (double)(SPEC_N - 1)))) * (1.0f / 65536.0f);
        }
        inited = 1;
    }
    float x[SPEC_N];
    for (int n = 0; n < SPEC_N; n++) {
        uint64_t ridx = (spec_w + (uint64_t)(SPEC_N - 1 - n)) % (uint64_t)SPEC_N;
        float l = (float)spec_hist[(size_t)ridx * 2 + 0];
        float r = (float)spec_hist[(size_t)ridx * 2 + 1];
        x[n] = (l + r) * hann[n];
    }
    for (int b = 0; b < 28; b++) {
        float cc = cc_tbl[b];
        float s1 = 0.0f, s2 = 0.0f;
        for (int n = 0; n < SPEC_N; n++) {
            float s0 = x[n] + cc * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        float power = s1 * s1 + s2 * s2 - cc * s1 * s2;
        float v = power > 0.0f ? (sqrtf(power) * 400.0f) : 0.0f;
        if (v > 1.0f) v = 1.0f;
        out[b] = v;
    }
}

// ---------------- diagnostics ----------------
void audio_get_diag(char* buf, size_t cap) {
#if AUDIO_USE_SDL
    uint32_t queued = sdl_dev ? SDL_GetQueuedAudioSize(sdl_dev) : 0;
    const char* drv = "SDL2";
#else
    uint32_t queued = win_queued_bytes();
    const char* drv = "WinMM";
#endif
    const char* st = a_status == 0 ? "IDLE" : a_status == 1 ? "OPENING" : a_status == 2 ? "READY" : "FAILED";
    snprintf(buf, cap,
        "%s Init=%d 设备=%d 线程=%d\n"
        "状态=%s  解码块=%lu\n"
        "队列=%u B  环形=%lu 帧\n"
        "时长=%.1fs 已播放=%.1fs\n"
        "重采样率=%u  声道=%u\n"
        "%s",
        drv, diag_sdl_init, diag_dev_open, diag_thread,
        st, (unsigned long)diag_decoded_chunks,
        (unsigned)queued, (unsigned long)ring_count(),
        (double)a_duration, audio_position(),
        (unsigned)a_rate, (unsigned)dec.channels,
        diag_last_error[0] ? diag_last_error : "");
}
