#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdalign.h>

#include <switch.h>

#include "sirene_ipc.h"
#include "decode.h"
#include "util.h"

/* 极轻量 2 MiB 静态堆：适配 Atmosphère sysmodule 紧凑内存池 */
#define INNER_HEAP_SIZE 0x200000

#define OUT_RATE        48000
#define AUDOUT_FRAMES   4096
#define AUDOUT_BYTES    (AUDOUT_FRAMES * 2 * (int)sizeof(int16_t)) /* 16384 = 4 * 0x1000 */
#define DEC_CHUNK       2048
#define MAX_SESSIONS    4

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

void __libnx_initheap(void) {
    static u8 inner_heap[INNER_HEAP_SIZE];
    extern void* fake_heap_start;
    extern void* fake_heap_end;
    fake_heap_start = inner_heap;
    fake_heap_end   = inner_heap + sizeof(inner_heap);
}

void __appInit(void) {
    Result rc = smInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));

    rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw)))
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }

    rc = fsInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));

    fsdevMountSdmc();
}

void __appExit(void) {
    audoutExit();
    fsdevUnmountAll();
    fsExit();
    smExit();
}

/* ==================== 后台队列与播放状态 ==================== */

typedef struct BgQueueEntry {
    char path[SIRENE_BG_MAX_PATH];
    char title[SIRENE_BG_MAX_TITLE];
} BgQueueEntry;

static BgQueueEntry g_queue[SIRENE_BG_MAX_QUEUE];
static uint32_t g_queue_count = 0;
static int32_t  g_queue_index = -1;

static Mutex    g_state_mtx;
static volatile int g_running = 1;

static uint32_t g_state       = SIR_STATE_IDLE;
static uint32_t g_play_mode   = SIR_MODE_LOOP_ALL;
static float    g_volume      = 0.85f;
static double   g_pos_sec     = 0.0;
static double   g_dur_sec     = 0.0;
static uint32_t g_src_rate    = OUT_RATE;
static uint32_t g_src_ch      = 2;
static char     g_cur_path[SIRENE_BG_MAX_PATH];
static char     g_cur_title[SIRENE_BG_MAX_TITLE];
static char     g_cur_artist[SIRENE_BG_MAX_ARTIST];

/* 音频线程命令槽 */
enum {
    BG_ACT_NONE = 0,
    BG_ACT_PLAY = 1,
    BG_ACT_STOP = 2,
    BG_ACT_SEEK = 3
};

static uint32_t g_act_seq     = 0;
static uint32_t g_act_done    = 0;
static int      g_act_kind    = BG_ACT_NONE;
static char     g_act_path[SIRENE_BG_MAX_PATH];
static double   g_act_sec     = 0.0;
static volatile int g_paused  = 0;
static uint32_t g_rng         = 0x2468ACE1u;

/* audout 页对齐双缓冲（静态分配，零堆碎片） */
static alignas(0x1000) int16_t g_pcm_buf[2][AUDOUT_FRAMES * 2];
static AudioOutBuffer g_aud_buf[2];
static int g_aud_inited  = 0;
static int g_aud_started = 0;

static uint32_t bg_rand(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    if (g_rng == 0) g_rng = 0x9E3779B9u;
    return g_rng;
}

static void extract_title(const char* path, char* out, size_t cap) {
    basename_no_ext(path, out, cap);
    if (!out[0]) {
        snprintf(out, cap, "%s", path_basename(path));
    }
}

static void queue_add_locked(const char* path) {
    if (!path || !path[0] || g_queue_count >= SIRENE_BG_MAX_QUEUE) return;
    BgQueueEntry* e = &g_queue[g_queue_count];
    memset(e, 0, sizeof(*e));
    snprintf(e->path, sizeof(e->path), "%s", path);
    extract_title(path, e->title, sizeof(e->title));
    g_queue_count++;
}

static void scan_dir_recursive_locked(const char* dir_path, int depth) {
    if (depth > 4 || g_queue_count >= SIRENE_BG_MAX_QUEUE) return;
    DIR* d = opendir(dir_path);
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL && g_queue_count < SIRENE_BG_MAX_QUEUE) {
        if (ent->d_name[0] == '.') continue;
        char full[SIRENE_BG_MAX_PATH];
        int n = snprintf(full, sizeof(full), "%s/%s", dir_path, ent->d_name);
        if (n <= 0 || n >= (int)sizeof(full)) continue;
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            scan_dir_recursive_locked(full, depth + 1);
        } else if (S_ISREG(st.st_mode) && dec_is_audio(full)) {
            queue_add_locked(full);
        }
    }
    closedir(d);
}

static uint32_t scan_library_locked(void) {
    g_queue_count = 0;
    g_queue_index = -1;
    static const char* roots[] = {
        "sdmc:/music",
        "sdmc:/Music",
        "sdmc:/MUSIC",
        "sdmc:/switch/music"
    };
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        scan_dir_recursive_locked(roots[i], 0);
        if (g_queue_count >= SIRENE_BG_MAX_QUEUE) break;
    }
    return g_queue_count;
}

static int32_t pick_next_index_locked(int manual) {
    if (g_queue_count == 0) return -1;
    if (!manual && g_play_mode == SIR_MODE_LOOP_ONE && g_queue_index >= 0) {
        return g_queue_index;
    }
    if (g_play_mode == SIR_MODE_SHUFFLE && g_queue_count > 1) {
        int32_t nxt = (int32_t)(bg_rand() % g_queue_count);
        if (nxt == g_queue_index) nxt = (nxt + 1) % (int32_t)g_queue_count;
        return nxt;
    }
    if (g_queue_index < 0) return 0;
    return (g_queue_index + 1) % (int32_t)g_queue_count;
}

static int32_t pick_prev_index_locked(void) {
    if (g_queue_count == 0) return -1;
    if (g_queue_index <= 0) return (int32_t)g_queue_count - 1;
    return g_queue_index - 1;
}

static void request_play_locked(const char* path, double start_sec, int32_t qidx) {
    if (!path || !path[0]) return;
    g_queue_index = qidx;
    snprintf(g_act_path, sizeof(g_act_path), "%s", path);
    g_act_sec = start_sec > 0.0 ? start_sec : 0.0;
    g_act_kind = BG_ACT_PLAY;
    g_paused = 0;
    g_state = SIR_STATE_OPENING;
    snprintf(g_cur_path, sizeof(g_cur_path), "%s", path);
    extract_title(path, g_cur_title, sizeof(g_cur_title));
    g_cur_artist[0] = '\0';
    g_pos_sec = g_act_sec;
    g_dur_sec = 0.0;
    g_act_seq++;
}

static void request_stop_locked(void) {
    g_act_kind = BG_ACT_STOP;
    g_paused = 0;
    g_state = SIR_STATE_IDLE;
    g_pos_sec = 0.0;
    g_dur_sec = 0.0;
    g_cur_path[0] = '\0';
    g_cur_title[0] = '\0';
    g_cur_artist[0] = '\0';
    g_act_seq++;
}

static void request_seek_locked(double sec) {
    if (g_state != SIR_STATE_PLAYING && g_state != SIR_STATE_PAUSED) return;
    if (sec < 0.0) sec = 0.0;
    if (g_dur_sec > 0.5 && sec > g_dur_sec - 0.25) sec = g_dur_sec - 0.25;
    g_act_sec = sec;
    g_act_kind = BG_ACT_SEEK;
    g_pos_sec = sec;
    g_act_seq++;
}

/* ==================== audout 音频解码与混音线程 ==================== */

static int ensure_audout_started(void) {
    if (!g_aud_inited) {
        Result rc = audoutInitialize();
        if (R_FAILED(rc)) return -1;
        g_aud_inited = 1;
        for (int i = 0; i < 2; i++) {
            memset(&g_aud_buf[i], 0, sizeof(AudioOutBuffer));
            g_aud_buf[i].buffer      = g_pcm_buf[i];
            g_aud_buf[i].buffer_size = AUDOUT_BYTES;
            g_aud_buf[i].data_size   = AUDOUT_BYTES;
            g_aud_buf[i].data_offset = 0;
        }
    }
    if (!g_aud_started) {
        Result rc = audoutStartAudioOut();
        if (R_FAILED(rc)) return -1;
        g_aud_started = 1;
    }
    return 0;
}

static void stop_audout_if_running(void) {
    if (g_aud_inited && g_aud_started) {
        audoutStopAudioOut();
        g_aud_started = 0;
    }
}

static inline int16_t downmix_sample(const int16_t* frm, int ch, int lr) {
    if (ch <= 1) return frm[0];
    if (ch == 2) return frm[lr];
    int32_t v = frm[lr];
    if (ch >= 3) v += (frm[2] * 7) / 10;
    if (ch >= 6) v += (frm[4 + lr] * 7) / 10;
    v = (v * 3) / 4;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

static void audio_thread_fn(void* arg) {
    (void)arg;
    static Decoder dec;
    static int16_t raw_chunk[DEC_CHUNK * 8];
    int raw_frames = 0;
    int raw_ch     = 2;
    double rs_pos  = 0.0;
    int16_t rs_last[2] = { 0, 0 };
    int rs_has_last = 0;
    int fade_in = 0;
    int dec_eof = 0;
    int active  = 0;
    int buf_idx = 0;
    int queued_cnt = 0;
    uint64_t played_out_frames = 0;
    double base_sec = 0.0;

    memset(&dec, 0, sizeof(dec));

    while (g_running) {
        /* 1. 处理控制命令 */
        int act_kind = BG_ACT_NONE;
        char path_copy[SIRENE_BG_MAX_PATH];
        double sec_copy = 0.0;
        uint32_t seq_now = 0;

        mutexLock(&g_state_mtx);
        if (g_act_seq != g_act_done) {
            seq_now   = g_act_seq;
            act_kind  = g_act_kind;
            sec_copy  = g_act_sec;
            snprintf(path_copy, sizeof(path_copy), "%s", g_act_path);
        }
        int paused_now = g_paused;
        float vol_now  = g_volume;
        mutexUnlock(&g_state_mtx);

        if (act_kind != BG_ACT_NONE) {
            if (act_kind == BG_ACT_STOP) {
                dec_close(&dec);
                active = 0;
                dec_eof = 0;
                raw_frames = 0;
                queued_cnt = 0;
                stop_audout_if_running();
                mutexLock(&g_state_mtx);
                g_act_done = seq_now;
                mutexUnlock(&g_state_mtx);
            } else if (act_kind == BG_ACT_PLAY) {
                dec_close(&dec);
                stop_audout_if_running();
                queued_cnt = 0;
                raw_frames = 0;
                rs_pos = 0.0;
                rs_last[0] = rs_last[1] = 0;
                rs_has_last = 0;
                fade_in = 240;
                dec_eof = 0;
                played_out_frames = 0;
                base_sec = 0.0;

                if (dec_open(&dec, path_copy) == 0 && dec.channels >= 1 && dec.channels <= 8 && dec.rate > 0) {
                    raw_ch = (int)dec.channels;
                    double dur = dec.total_frames > 0 ? (double)dec.total_frames / (double)dec.rate : 0.0;
                    if (sec_copy > 0.2) {
                        uint64_t target_f = (uint64_t)(sec_copy * (double)dec.rate);
                        if (dec_seek(&dec, target_f) == 0) {
                            base_sec = sec_copy;
                        }
                    }
                    if (ensure_audout_started() == 0) {
                        active = 1;
                        mutexLock(&g_state_mtx);
                        g_src_rate = dec.rate;
                        g_src_ch   = dec.channels;
                        g_dur_sec  = dur;
                        g_pos_sec  = base_sec;
                        g_state    = g_paused ? SIR_STATE_PAUSED : SIR_STATE_PLAYING;
                        g_act_done = seq_now;
                        mutexUnlock(&g_state_mtx);
                    } else {
                        dec_close(&dec);
                        active = 0;
                        mutexLock(&g_state_mtx);
                        g_state    = SIR_STATE_ERROR;
                        g_act_done = seq_now;
                        mutexUnlock(&g_state_mtx);
                    }
                } else {
                    dec_close(&dec);
                    active = 0;
                    mutexLock(&g_state_mtx);
                    g_state    = SIR_STATE_ERROR;
                    g_dur_sec  = 0.0;
                    g_act_done = seq_now;
                    mutexUnlock(&g_state_mtx);
                }
            } else if (act_kind == BG_ACT_SEEK) {
                if (active && dec.fmt != FMT_NONE && dec.rate > 0) {
                    uint64_t target_f = (uint64_t)(sec_copy * (double)dec.rate);
                    if (dec_seek(&dec, target_f) == 0) {
                        stop_audout_if_running();
                        queued_cnt = 0;
                        raw_frames = 0;
                        rs_pos = 0.0;
                        rs_last[0] = rs_last[1] = 0;
                        rs_has_last = 0;
                        fade_in = 240;
                        dec_eof = 0;
                        base_sec = sec_copy;
                        played_out_frames = 0;
                    }
                }
                mutexLock(&g_state_mtx);
                g_act_done = seq_now;
                mutexUnlock(&g_state_mtx);
            }
            continue;
        }

        /* 2. 空闲或暂停时不占用 audout 与 CPU */
        if (!active || paused_now) {
            if (active && paused_now && g_aud_started) {
                stop_audout_if_running();
                queued_cnt = 0;
            }
            svcSleepThread(20000000ULL); /* 20 ms */
            continue;
        }

        if (!g_aud_started) {
            if (ensure_audout_started() != 0) {
                svcSleepThread(50000000ULL);
                continue;
            }
            queued_cnt = 0;
        }

        /* 3. 解码并重采样填充一个 AUDOUT_FRAMES 缓冲 */
        int16_t* dst = g_pcm_buf[buf_idx];
        int filled = 0;
        double step = (double)(dec.rate > 0 ? dec.rate : OUT_RATE) / (double)OUT_RATE;

        while (filled < AUDOUT_FRAMES && !dec_eof) {
            if (raw_frames <= 0 || rs_pos >= (double)(raw_frames - 1)) {
                if (raw_frames > 0) {
                    const int16_t* plast = raw_chunk + (size_t)(raw_frames - 1) * raw_ch;
                    rs_last[0] = downmix_sample(plast, raw_ch, 0);
                    rs_last[1] = downmix_sample(plast, raw_ch, 1);
                    rs_has_last = 1;
                    rs_pos -= (double)raw_frames;
                }
                int got = dec_read(&dec, raw_chunk, DEC_CHUNK);
                if (got <= 0) {
                    dec_eof = 1;
                    raw_frames = 0;
                    break;
                }
                raw_frames = got;
            }

            double limit = (double)(raw_frames - 1);
            while (filled < AUDOUT_FRAMES && rs_pos < limit) {
                int i0 = (int)floor(rs_pos);
                if (i0 < -1) { rs_pos += step; continue; }
                if (i0 < 0 && !rs_has_last) i0 = 0;
                double frac = rs_pos - floor(rs_pos);
                if (frac < 0.0) frac = 0.0;
                if (frac > 1.0) frac = 1.0;
                int i1 = i0 + 1;
                if (i1 >= raw_frames) break;

                const int16_t* p0 = (i0 >= 0) ? (raw_chunk + (size_t)i0 * raw_ch) : NULL;
                const int16_t* p1 = raw_chunk + (size_t)i1 * raw_ch;
                float gain = vol_now;
                if (fade_in > 0) {
                    gain *= (float)(240 - fade_in) / 240.0f;
                    fade_in--;
                }
                for (int c = 0; c < 2; c++) {
                    int16_t s0 = p0 ? downmix_sample(p0, raw_ch, c) : rs_last[c];
                    int16_t s1 = downmix_sample(p1, raw_ch, c);
                    double v = ((double)s0 + ((double)s1 - (double)s0) * frac) * (double)gain;
                    int iv = (int)lrint(v);
                    if (iv > 32767) iv = 32767;
                    if (iv < -32768) iv = -32768;
                    dst[filled * 2 + c] = (int16_t)iv;
                }
                filled++;
                rs_pos += step;
            }
        }

        if (filled > 0) {
            if (filled < AUDOUT_FRAMES) {
                memset(dst + filled * 2, 0, (size_t)(AUDOUT_FRAMES - filled) * 2 * sizeof(int16_t));
            }
            g_aud_buf[buf_idx].buffer      = dst;
            g_aud_buf[buf_idx].buffer_size = AUDOUT_BYTES;
            g_aud_buf[buf_idx].data_size   = AUDOUT_BYTES;
            g_aud_buf[buf_idx].data_offset = 0;

            if (queued_cnt >= 2) {
                AudioOutBuffer* rel = NULL;
                u32 rel_cnt = 0;
                audoutWaitPlayFinish(&rel, &rel_cnt, AUDOUT_MAX_DELAY);
                if (rel_cnt > 0 && queued_cnt >= (int)rel_cnt) queued_cnt -= (int)rel_cnt;
                else if (queued_cnt > 0) queued_cnt--;
            }

            if (R_SUCCEEDED(audoutAppendAudioOutBuffer(&g_aud_buf[buf_idx]))) {
                queued_cnt++;
                buf_idx ^= 1;
                played_out_frames += (uint64_t)filled;

                double cur_p = base_sec + (double)played_out_frames / (double)OUT_RATE;
                mutexLock(&g_state_mtx);
                if (g_dur_sec > 0.0 && cur_p > g_dur_sec) cur_p = g_dur_sec;
                g_pos_sec = cur_p;
                mutexUnlock(&g_state_mtx);
            } else {
                svcSleepThread(10000000ULL);
            }
        }

        /* 4. 曲目播完自动切下一首 */
        if (dec_eof) {
            while (queued_cnt > 0) {
                AudioOutBuffer* rel = NULL;
                u32 rel_cnt = 0;
                if (R_FAILED(audoutWaitPlayFinish(&rel, &rel_cnt, AUDOUT_MAX_DELAY))) break;
                if (rel_cnt > 0 && queued_cnt >= (int)rel_cnt) queued_cnt -= (int)rel_cnt;
                else break;
            }
            dec_close(&dec);
            active = 0;

            mutexLock(&g_state_mtx);
            if (g_act_seq == g_act_done) {
                int32_t nxt = pick_next_index_locked(0);
                if (nxt >= 0 && nxt < (int32_t)g_queue_count) {
                    request_play_locked(g_queue[nxt].path, 0.0, nxt);
                } else {
                    g_state = SIR_STATE_IDLE;
                    stop_audout_if_running();
                }
            }
            mutexUnlock(&g_state_mtx);
        }
    }

    dec_close(&dec);
    stop_audout_if_running();
}

/* ==================== 纯 libnx CMIF IPC 服务端 ==================== */

static void write_cmif_reply(Result rc, const void* out_raw, u32 out_raw_size) {
    void* base = armGetTls();
    u32 data_words = (16u + (u32)sizeof(CmifOutHeader) + out_raw_size + 3u) / 4u;
    HipcRequest resp = hipcMakeRequestInline(base,
        .type           = 0,
        .num_data_words = data_words,
    );
    CmifOutHeader* hdr = (CmifOutHeader*)cmifGetAlignedDataStart(resp.data_words, base);
    hdr->magic   = CMIF_OUT_HEADER_MAGIC;
    hdr->version = 0;
    hdr->result  = rc;
    hdr->token   = 0;
    if (out_raw && out_raw_size > 0) {
        memcpy(hdr + 1, out_raw, out_raw_size);
    }
}

/* 返回 1 表示客户端请求关闭会话，返回 0 表示已写入回复 */
static int handle_ipc_message(void) {
    void* base = armGetTls();
    HipcParsedRequest r = hipcParseRequest(base);

    if (r.meta.type == CmifCommandType_Close) {
        return 1;
    }

    void* data_start = cmifGetAlignedDataStart(r.data.data_words, base);
    CmifInHeader* in_hdr = (CmifInHeader*)data_start;

    if (r.meta.type == CmifCommandType_Control || r.meta.type == CmifCommandType_LegacyControl) {
        if (in_hdr->magic != CMIF_IN_HEADER_MAGIC) {
            write_cmif_reply(MAKERESULT(Module_Libnx, LibnxError_InvalidCmifOutHeader), NULL, 0);
            return 0;
        }
        if (in_hdr->command_id == 3) { /* QueryPointerBufferSize */
            u16 ptr_sz = 0;
            write_cmif_reply(0, &ptr_sz, sizeof(ptr_sz));
            return 0;
        }
        write_cmif_reply(MAKERESULT(Module_Libnx, LibnxError_BadInput), NULL, 0);
        return 0;
    }

    if ((r.meta.type != CmifCommandType_Request && r.meta.type != CmifCommandType_LegacyRequest) ||
        in_hdr->magic != CMIF_IN_HEADER_MAGIC) {
        write_cmif_reply(MAKERESULT(Module_Libnx, LibnxError_BadInput), NULL, 0);
        return 0;
    }

    const u8* in_raw = (const u8*)(in_hdr + 1);
    const void* in_buf = (r.meta.num_send_buffers > 0) ? hipcGetBufferAddress(&r.data.send_buffers[0]) : NULL;
    size_t in_buf_sz   = (r.meta.num_send_buffers > 0) ? hipcGetBufferSize(&r.data.send_buffers[0]) : 0;
    void* out_buf      = (r.meta.num_recv_buffers > 0) ? hipcGetBufferAddress(&r.data.recv_buffers[0]) : NULL;
    size_t out_buf_sz  = (r.meta.num_recv_buffers > 0) ? hipcGetBufferSize(&r.data.recv_buffers[0]) : 0;

    switch (in_hdr->command_id) {
        case SIR_CMD_GET_API_VERSION: {
            u32 ver = SIRENE_IPC_API_VERSION;
            write_cmif_reply(0, &ver, sizeof(ver));
            break;
        }
        case SIR_CMD_GET_STATUS: {
            if (!out_buf || out_buf_sz < sizeof(SireneBgStatus)) {
                write_cmif_reply(MAKERESULT(Module_Libnx, LibnxError_BadInput), NULL, 0);
                break;
            }
            SireneBgStatus st;
            memset(&st, 0, sizeof(st));
            mutexLock(&g_state_mtx);
            st.magic        = SIRENE_BG_STATUS_MAGIC;
            st.state        = g_state;
            st.play_mode    = g_play_mode;
            st.queue_index  = g_queue_index;
            st.queue_count  = g_queue_count;
            st.volume       = g_volume;
            st.position_sec = g_pos_sec;
            st.duration_sec = g_dur_sec;
            st.sample_rate  = g_src_rate;
            st.channels     = g_src_ch;
            snprintf(st.path, sizeof(st.path), "%s", g_cur_path);
            snprintf(st.title, sizeof(st.title), "%s", g_cur_title);
            snprintf(st.artist, sizeof(st.artist), "%s", g_cur_artist);
            mutexUnlock(&g_state_mtx);
            memcpy(out_buf, &st, sizeof(st));
            write_cmif_reply(0, NULL, 0);
            break;
        }
        case SIR_CMD_PLAY_PATH: {
            if (!in_buf || in_buf_sz == 0) {
                write_cmif_reply(MAKERESULT(Module_Libnx, LibnxError_BadInput), NULL, 0);
                break;
            }
            double start_sec = 0.0;
            memcpy(&start_sec, in_raw, sizeof(double));
            char path[SIRENE_BG_MAX_PATH];
            size_t copy_n = in_buf_sz < sizeof(path) - 1 ? in_buf_sz : sizeof(path) - 1;
            memcpy(path, in_buf, copy_n);
            path[copy_n] = '\0';
            mutexLock(&g_state_mtx);
            int32_t match = -1;
            for (uint32_t i = 0; i < g_queue_count; i++) {
                if (strcmp(g_queue[i].path, path) == 0) { match = (int32_t)i; break; }
            }
            request_play_locked(path, start_sec, match);
            mutexUnlock(&g_state_mtx);
            write_cmif_reply(0, NULL, 0);
            break;
        }
        case SIR_CMD_SET_PAUSED: {
            u32 mode = 0;
            memcpy(&mode, in_raw, sizeof(u32));
            mutexLock(&g_state_mtx);
            if (g_state == SIR_STATE_PLAYING || g_state == SIR_STATE_PAUSED) {
                if (mode == 2) g_paused = !g_paused;
                else g_paused = (mode != 0) ? 1 : 0;
                g_state = g_paused ? SIR_STATE_PAUSED : SIR_STATE_PLAYING;
            } else if (mode != 1 && g_queue_count > 0) {
                int32_t idx = (g_queue_index >= 0 && g_queue_index < (int32_t)g_queue_count) ? g_queue_index : 0;
                request_play_locked(g_queue[idx].path, 0.0, idx);
            }
            mutexUnlock(&g_state_mtx);
            write_cmif_reply(0, NULL, 0);
            break;
        }
        case SIR_CMD_STOP: {
            mutexLock(&g_state_mtx);
            request_stop_locked();
            mutexUnlock(&g_state_mtx);
            write_cmif_reply(0, NULL, 0);
            break;
        }
        case SIR_CMD_NEXT: {
            mutexLock(&g_state_mtx);
            if (g_queue_count == 0) scan_library_locked();
            int32_t nxt = pick_next_index_locked(1);
            if (nxt >= 0 && nxt < (int32_t)g_queue_count) {
                request_play_locked(g_queue[nxt].path, 0.0, nxt);
            }
            mutexUnlock(&g_state_mtx);
            write_cmif_reply(0, NULL, 0);
            break;
        }
        case SIR_CMD_PREV: {
            mutexLock(&g_state_mtx);
            if (g_queue_count == 0) scan_library_locked();
            int32_t prv = pick_prev_index_locked();
            if (prv >= 0 && prv < (int32_t)g_queue_count) {
                request_play_locked(g_queue[prv].path, 0.0, prv);
            }
            mutexUnlock(&g_state_mtx);
            write_cmif_reply(0, NULL, 0);
            break;
        }
        case SIR_CMD_SEEK: {
            double sec = 0.0;
            memcpy(&sec, in_raw, sizeof(double));
            mutexLock(&g_state_mtx);
            request_seek_locked(sec);
            mutexUnlock(&g_state_mtx);
            write_cmif_reply(0, NULL, 0);
            break;
        }
        case SIR_CMD_SET_VOLUME: {
            float vol = 0.85f;
            memcpy(&vol, in_raw, sizeof(float));
            if (vol < 0.0f) vol = 0.0f;
            if (vol > 1.0f) vol = 1.0f;
            mutexLock(&g_state_mtx);
            g_volume = vol;
            mutexUnlock(&g_state_mtx);
            write_cmif_reply(0, NULL, 0);
            break;
        }
        case SIR_CMD_SET_MODE: {
            u32 mode = 0;
            memcpy(&mode, in_raw, sizeof(u32));
            if (mode >= SIR_MODE_COUNT) mode = SIR_MODE_LOOP_ALL;
            mutexLock(&g_state_mtx);
            g_play_mode = mode;
            mutexUnlock(&g_state_mtx);
            write_cmif_reply(0, NULL, 0);
            break;
        }
        case SIR_CMD_QUEUE_CLEAR: {
            mutexLock(&g_state_mtx);
            g_queue_count = 0;
            g_queue_index = -1;
            mutexUnlock(&g_state_mtx);
            write_cmif_reply(0, NULL, 0);
            break;
        }
        case SIR_CMD_QUEUE_ADD: {
            if (!in_buf || in_buf_sz == 0) {
                write_cmif_reply(MAKERESULT(Module_Libnx, LibnxError_BadInput), NULL, 0);
                break;
            }
            char path[SIRENE_BG_MAX_PATH];
            size_t copy_n = in_buf_sz < sizeof(path) - 1 ? in_buf_sz : sizeof(path) - 1;
            memcpy(path, in_buf, copy_n);
            path[copy_n] = '\0';
            mutexLock(&g_state_mtx);
            queue_add_locked(path);
            mutexUnlock(&g_state_mtx);
            write_cmif_reply(0, NULL, 0);
            break;
        }
        case SIR_CMD_QUEUE_PLAY_INDEX: {
            SireneBgPlayIndexArgs args;
            memcpy(&args, in_raw, sizeof(args));
            mutexLock(&g_state_mtx);
            if (args.index >= 0 && args.index < (int32_t)g_queue_count) {
                request_play_locked(g_queue[args.index].path, args.start_sec, args.index);
            }
            mutexUnlock(&g_state_mtx);
            write_cmif_reply(0, NULL, 0);
            break;
        }
        case SIR_CMD_SCAN_LIBRARY: {
            mutexLock(&g_state_mtx);
            u32 cnt = scan_library_locked();
            mutexUnlock(&g_state_mtx);
            write_cmif_reply(0, &cnt, sizeof(cnt));
            break;
        }
        case SIR_CMD_GET_QUEUE_ITEM: {
            u32 idx = 0;
            memcpy(&idx, in_raw, sizeof(u32));
            if (!out_buf || out_buf_sz < sizeof(SireneBgQueueItem)) {
                write_cmif_reply(MAKERESULT(Module_Libnx, LibnxError_BadInput), NULL, 0);
                break;
            }
            SireneBgQueueItem item;
            memset(&item, 0, sizeof(item));
            mutexLock(&g_state_mtx);
            if (idx < g_queue_count) {
                snprintf(item.path, sizeof(item.path), "%s", g_queue[idx].path);
                snprintf(item.title, sizeof(item.title), "%s", g_queue[idx].title);
            }
            mutexUnlock(&g_state_mtx);
            memcpy(out_buf, &item, sizeof(item));
            write_cmif_reply(0, NULL, 0);
            break;
        }
        default: {
            write_cmif_reply(MAKERESULT(Module_Libnx, LibnxError_BadInput), NULL, 0);
            break;
        }
    }
    return 0;
}

static void remove_session(Handle* handles, s32* count, s32 idx) {
    if (idx <= 0 || idx >= *count) return;
    svcCloseHandle(handles[idx]);
    for (s32 i = idx; i + 1 < *count; i++) {
        handles[i] = handles[i + 1];
    }
    (*count)--;
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    mutexInit(&g_state_mtx);
    g_rng = (uint32_t)(armGetSystemTick() ^ 0x51A3BEEFu);

    Handle port = INVALID_HANDLE;
    Result rc = smRegisterService(&port, smEncodeName(SIRENE_IPC_SERVICE_NAME), false, MAX_SESSIONS);
    if (R_FAILED(rc)) {
        return 0;
    }

    Thread audio_thread;
    rc = threadCreate(&audio_thread, audio_thread_fn, NULL, NULL, 0x10000, 0x2C, 3);
    if (R_FAILED(rc)) {
        smUnregisterService(smEncodeName(SIRENE_IPC_SERVICE_NAME));
        svcCloseHandle(port);
        return 0;
    }
    threadStart(&audio_thread);

    Handle handles[1 + MAX_SESSIONS];
    s32 handle_count = 1;
    handles[0] = port;
    Handle reply_target = INVALID_HANDLE;

    while (g_running) {
        s32 idx = -1;
        rc = svcReplyAndReceive(&idx, handles, handle_count, reply_target, UINT64_MAX);
        if (R_FAILED(rc)) {
            if (reply_target != INVALID_HANDLE) {
                for (s32 i = 1; i < handle_count; i++) {
                    if (handles[i] == reply_target) {
                        remove_session(handles, &handle_count, i);
                        break;
                    }
                }
                reply_target = INVALID_HANDLE;
                continue;
            }
            if (idx > 0 && idx < handle_count) {
                remove_session(handles, &handle_count, idx);
                reply_target = INVALID_HANDLE;
                continue;
            }
            reply_target = INVALID_HANDLE;
            svcSleepThread(10000000ULL);
            continue;
        }

        reply_target = INVALID_HANDLE;
        if (idx == 0) {
            Handle sess = INVALID_HANDLE;
            if (R_SUCCEEDED(svcAcceptSession(&sess, port))) {
                if (handle_count < 1 + MAX_SESSIONS) {
                    handles[handle_count++] = sess;
                } else {
                    svcCloseHandle(sess);
                }
            }
        } else if (idx > 0 && idx < handle_count) {
            if (handle_ipc_message()) {
                remove_session(handles, &handle_count, idx);
            } else {
                reply_target = handles[idx];
            }
        }
    }

    for (s32 i = 1; i < handle_count; i++) svcCloseHandle(handles[i]);
    smUnregisterService(smEncodeName(SIRENE_IPC_SERVICE_NAME));
    svcCloseHandle(port);
    threadWaitForExit(&audio_thread);
    threadClose(&audio_thread);
    return 0;
}
