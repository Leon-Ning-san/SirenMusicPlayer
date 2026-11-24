#include "audio.h"
#include "decode.h"
#include <switch.h>
#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#define RING_FRAMES   (48000 * 12)   // 12 seconds @48k stereo
#define CHUNK_FRAMES  4096
#define OUT_RATE      48000
#define SPEC_N        2048
#define QUEUE_TARGET  (48000 * 2 * 2 * 15 / 100)  // keep ~150ms queued (bytes)

// ---------------- ring buffer (decoder thread -> main thread) ----------------
static int16_t* ring = NULL;
static volatile uint64_t ring_w = 0;
static volatile uint64_t ring_r = 0;
static Mutex ringmtx;

static uint64_t ring_count(void) { return ring_w - ring_r; }
static uint64_t ring_free(void) { return (uint64_t)RING_FRAMES - (ring_w - ring_r); }

// ---------------- decoder thread state ----------------
static Thread decThread;
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
static volatile uint64_t out_total = 0;

// command protocol
static volatile int cmd_seq = 0;
static volatile int cmd_done = 0;
static char cmd_path[600];
static volatile int cmd_is_play = 0;
static volatile uint64_t cmd_seek_frame = 0;

// ---------------- SDL audio device ----------------
static SDL_AudioDeviceID sdl_dev = 0;
static volatile uint64_t total_queued_bytes = 0;  // bytes ever queued since (re)start
static volatile double base_sec = 0;              // position offset (after seek)

// ---------------- diagnostics ----------------
static volatile int diag_sdl_init = 0;    // SDL_Init result (0=ok, <0 fail)
static volatile int diag_dev_open = 0;    // SDL_OpenAudioDevice: 1 ok, 0 fail
static volatile int diag_thread = 0;      // decoder thread created: 1 ok
static volatile uint64_t diag_decoded_chunks = 0; // chunks decoded by thread
static char diag_last_error[128] = {0};

// spectrum history (main thread)
static int16_t spec_hist[SPEC_N * 2];
static volatile uint64_t spec_w = 0;

// ---------------- resample & push into ring ----------------
static void push_chunk(const int16_t* chunk, int frames, int channels) {
    if (frames <= 0) return;
    double step = (double)a_rate / (double)OUT_RATE;
    long need = (long)((double)frames / step) + 4;
    long avail = (long)ring_free();
    if (avail < need) need = avail;
    if (need <= 0) return;
    int16_t* tmp = (int16_t*)malloc((size_t)need * 2 * sizeof(int16_t));
    if (!tmp) return;
    long outn = 0;
    while (outn < need) {
        int i0 = (int)rs_pos;
        double frac = rs_pos - (double)i0;
        int i1 = i0 + 1;
        if (i0 >= frames) break;
        for (int c = 0; c < 2; c++) {
            int sc = (channels == 1) ? 0 : c;
            int16_t s0 = (i0 < 0) ? rs_last[c] : chunk[(size_t)i0 * channels + sc];
            int16_t s1 = (i1 >= frames) ? chunk[(size_t)(frames - 1) * channels + sc]
                                        : chunk[(size_t)i1 * channels + sc];
            double v = (double)s0 + ((double)s1 - (double)s0) * frac;
            int iv = (int)v;
            if (iv > 32767) iv = 32767;
            if (iv < -32768) iv = -32768;
            tmp[(size_t)outn * 2 + c] = (int16_t)iv;
        }
        outn++;
        rs_pos += step;
    }
    for (int c = 0; c < 2; c++) {
        int sc = (channels == 1) ? 0 : c;
        rs_last[c] = chunk[(size_t)(frames - 1) * channels + sc];
    }
    rs_pos -= (double)frames;
    if (outn > 0) {
        mutexLock(&ringmtx);
        for (long i = 0; i < outn; i++) {
            uint64_t idx = ring_w % (uint64_t)RING_FRAMES;
            ring[(size_t)idx * 2 + 0] = tmp[(size_t)i * 2 + 0];
            ring[(size_t)idx * 2 + 1] = tmp[(size_t)i * 2 + 1];
            ring_w++;
        }
        out_total += (uint64_t)outn;
        mutexUnlock(&ringmtx);
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
                mutexLock(&ringmtx);
                ring_w = ring_r = 0;
                out_total = 0;
                mutexUnlock(&ringmtx);
                dec_eof = 0;
                rs_pos = 0;
                rs_last[0] = rs_last[1] = 0;
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
                if (dec.fmt != FMT_NONE && dec_seek(&dec, cmd_seek_frame) == 0) {
                    mutexLock(&ringmtx);
                    ring_w = ring_r = 0;
                    out_total = 0;
                    mutexUnlock(&ringmtx);
                    dec_eof = 0;
                    rs_pos = 0;
                }
            }
            __atomic_store_n(&cmd_done, seq, __ATOMIC_SEQ_CST);
        }

        if (a_status == 2 && !a_paused && !dec_eof && dec.fmt != FMT_NONE) {
            if (ring_free() >= (uint64_t)(CHUNK_FRAMES * 3)) {
                int got = dec_read(&dec, chunk, CHUNK_FRAMES);
                if (got <= 0) dec_eof = 1;
                else {
                    push_chunk(chunk, got, (int)dec.channels);
                    diag_decoded_chunks++;
                }
            } else {
                svcSleepThread(3 * 1000 * 1000);
            }
        } else {
            svcSleepThread(6 * 1000 * 1000);
        }
    }
}

// ---------------- public API ----------------
int audio_init(void) {
    diag_sdl_init = SDL_Init(SDL_INIT_AUDIO);
    if (diag_sdl_init != 0) { snprintf(diag_last_error, sizeof(diag_last_error), "SDL_Init失败: %s", SDL_GetError()); return -1; }

    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq = OUT_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = NULL;  // use SDL_QueueAudio
    sdl_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    diag_dev_open = sdl_dev ? 1 : 0;
    if (sdl_dev == 0) { snprintf(diag_last_error, sizeof(diag_last_error), "音频设备打开失败: %s", SDL_GetError()); return -1; }
    SDL_PauseAudioDevice(sdl_dev, 0);  // start

    ring = (int16_t*)malloc((size_t)RING_FRAMES * 2 * sizeof(int16_t));
    if (!ring) { snprintf(diag_last_error, sizeof(diag_last_error), "环形缓冲分配失败"); return -1; }
    mutexInit(&ringmtx);
    Result rc = threadCreate(&decThread, decoder_thread, NULL, NULL, 0x40000, 0x3B, -2);
    if (R_FAILED(rc)) { snprintf(diag_last_error, sizeof(diag_last_error), "解码线程创建失败: 0x%x", (unsigned)rc); return -1; }
    diag_thread = 1;
    threadStart(&decThread);
    return 0;
}

void audio_exit(void) {
    thrun = 0;
    threadWaitForExit(&decThread);
    threadClose(&decThread);
    if (sdl_dev) {
        SDL_PauseAudioDevice(sdl_dev, 1);
        SDL_CloseAudioDevice(sdl_dev);
        sdl_dev = 0;
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

static void wait_cmd_done(void) {
    for (int i = 0; i < 2000; i++) {
        if (__atomic_load_n(&cmd_done, __ATOMIC_SEQ_CST) >= __atomic_load_n(&cmd_seq, __ATOMIC_SEQ_CST)) return;
        svcSleepThread(2 * 1000 * 1000);
    }
}

static void reset_output(double base) {
    base_sec = base;
    total_queued_bytes = 0;
    if (sdl_dev) SDL_ClearQueuedAudio(sdl_dev);
}

int audio_play(const char* path) {
    snprintf(cmd_path, sizeof(cmd_path), "%s", path);
    cmd_is_play = 1;
    a_paused = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_add_fetch(&cmd_seq, 1, __ATOMIC_SEQ_CST);
    wait_cmd_done();
    reset_output(0);
    if (sdl_dev) SDL_PauseAudioDevice(sdl_dev, 0);
    return 0;
}

int audio_seek(double sec) {
    if (a_status != 2) return -1;
    if (sec < 0) sec = 0;
    double d = a_duration;
    if (d > 0.5 && sec > d - 0.25) sec = d - 0.25;
    cmd_seek_frame = (uint64_t)(sec * (double)a_rate);
    cmd_is_play = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_add_fetch(&cmd_seq, 1, __ATOMIC_SEQ_CST);
    wait_cmd_done();
    reset_output(sec);
    return 0;
}

void audio_pause(void) {
    a_paused = 1;
    if (sdl_dev) SDL_PauseAudioDevice(sdl_dev, 1);
}
void audio_resume(void) {
    a_paused = 0;
    if (sdl_dev) SDL_PauseAudioDevice(sdl_dev, 0);
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
    u32 queued = sdl_dev ? SDL_GetQueuedAudioSize(sdl_dev) : 0;
    u64 played_bytes = total_queued_bytes;
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
    u32 queued = sdl_dev ? SDL_GetQueuedAudioSize(sdl_dev) : 0;
    return (dec_eof && ring_count() == 0 && queued == 0) ? 1 : 0;
}

// pump decoded PCM from ring into SDL, applying software volume
void audio_pump(void) {
    if (a_status != 2 || !sdl_dev || a_paused) return;
    u32 queued = SDL_GetQueuedAudioSize(sdl_dev);
    if (queued >= QUEUE_TARGET) return;
    int want = (int)((QUEUE_TARGET - queued) / 4);
    if (want > 4096) want = 4096;

    int16_t buf[4096 * 2];
    mutexLock(&ringmtx);
    u64 avail = ring_count();
    if ((u64)want > avail) want = (int)avail;
    float vol = a_volume;
    for (int i = 0; i < want; i++) {
        uint64_t idx = ring_r % (uint64_t)RING_FRAMES;
        buf[i * 2 + 0] = (int16_t)(ring[(size_t)idx * 2 + 0] * vol);
        buf[i * 2 + 1] = (int16_t)(ring[(size_t)idx * 2 + 1] * vol);
        ring_r++;
    }
    mutexUnlock(&ringmtx);
    if (want <= 0) return;

    SDL_QueueAudio(sdl_dev, buf, (Uint32)want * 4);
    total_queued_bytes += (uint64_t)want * 4;

    // spectrum history
    for (int i = 0; i < want; i++) {
        uint64_t w = spec_w % (uint64_t)SPEC_N;
        spec_hist[(size_t)w * 2 + 0] = buf[i * 2 + 0];
        spec_hist[(size_t)w * 2 + 1] = buf[i * 2 + 1];
        spec_w++;
    }
}

// ---------------- spectrum ----------------
void audio_get_spectrum(float out[28]) {
    if (a_paused || a_status != 2) {
        memset(out, 0, sizeof(float) * 28);
        return;
    }
    static float freqs[28];
    static int inited = 0;
    if (!inited) {
        double f = 60.0;
        for (int i = 0; i < 28; i++) {
            freqs[i] = (float)f;
            f *= pow(12000.0 / 60.0, 1.0 / 27.0);
        }
        inited = 1;
    }
    float x[SPEC_N];
    for (int n = 0; n < SPEC_N; n++) {
        uint64_t ridx = (spec_w + (uint64_t)(SPEC_N - 1 - n)) % (uint64_t)SPEC_N;
        float l = (float)spec_hist[(size_t)ridx * 2 + 0];
        float r = (float)spec_hist[(size_t)ridx * 2 + 1];
        x[n] = (l + r) * (1.0f / 65536.0f);
        double w = 0.5 * (1.0 - cos(2.0 * M_PI * n / (double)(SPEC_N - 1)));
        x[n] *= (float)w;
    }
    for (int b = 0; b < 28; b++) {
        double k = 2.0 * M_PI * (double)freqs[b] / (double)OUT_RATE;
        double cc = 2.0 * cos(k);
        double s1 = 0, s2 = 0;
        for (int n = 0; n < SPEC_N; n++) {
            double s0 = (double)x[n] + cc * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        double power = s1 * s1 + s2 * s2 - cc * s1 * s2;
        float v = (float)(sqrt(power) * 400.0);
        if (v > 1) v = 1;
        out[b] = v;
    }
}

// ---------------- diagnostics ----------------
void audio_get_diag(char* buf, size_t cap) {
    u32 queued = sdl_dev ? SDL_GetQueuedAudioSize(sdl_dev) : 0;
    const char* st = a_status == 0 ? "IDLE" : a_status == 1 ? "OPENING" : a_status == 2 ? "READY" : "FAILED";
    snprintf(buf, cap,
        "SDL_Init=%d 设备=%d 线程=%d\n"
        "状态=%s  解码块=%llu\n"
        "队列=%u B  环形=%llu 帧\n"
        "时长=%.1fs 已播放=%.1fs\n"
        "重采样率=%u  声道=%u\n"
        "%s",
        diag_sdl_init, diag_dev_open, diag_thread,
        st, (unsigned long long)diag_decoded_chunks,
        (unsigned)queued, (unsigned long long)ring_count(),
        (double)a_duration, audio_position(),
        (unsigned)a_rate, (unsigned)dec.channels,
        diag_last_error[0] ? diag_last_error : "");
}
