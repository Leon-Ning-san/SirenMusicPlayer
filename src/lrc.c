#include "lrc.h"
#include "util.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <dirent.h>

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#define MAX_LINES 4096

static int cmp_lines(const void* a, const void* b) {
    const LrcLine* x = (const LrcLine*)a;
    const LrcLine* y = (const LrcLine*)b;
    if (x->t < y->t) return -1;
    if (x->t > y->t) return 1;
    return 0;
}

static void add_meta(Lyrics* l, const char* key, const char* val) {
    if (!strcasecmp(key, "ti")) snprintf(l->ti, sizeof(l->ti), "%s", val);
    else if (!strcasecmp(key, "ar")) snprintf(l->ar, sizeof(l->ar), "%s", val);
}

// 严格解析 LRC 时间标签：mm:ss、mm:ss.xx、mm:ss.xxx、mm:ss:xx
static int parse_time_tag(const char* tag, double* out_t) {
    if (!tag || !isdigit((unsigned char)tag[0])) return 0;
    const char* p = tag;
    long mm = 0;
    int m_digits = 0;
    while (isdigit((unsigned char)*p) && m_digits < 5) {
        mm = mm * 10 + (*p - '0');
        p++;
        m_digits++;
    }
    if (*p != ':' || m_digits == 0) return 0;
    p++;
    if (!isdigit((unsigned char)*p)) return 0;
    long ss = 0;
    int s_digits = 0;
    while (isdigit((unsigned char)*p) && s_digits < 3) {
        ss = ss * 10 + (*p - '0');
        p++;
        s_digits++;
    }
    double frac = 0.0;
    if (*p == '.' || *p == ':') {
        p++;
        double div = 10.0;
        while (isdigit((unsigned char)*p)) {
            frac += (*p - '0') / div;
            div *= 10.0;
            p++;
        }
    }
    while (*p == ' ' || *p == '\t') p++;
    if (*p != 0) return 0;
    *out_t = (double)mm * 60.0 + (double)ss + frac;
    return 1;
}

// 清除增强型 LRC 行内逐字时间标签 <00:12.34>
static void strip_inline_tags(const char* in, char* out, size_t cap) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < cap; ) {
        if (in[i] == '<' && isdigit((unsigned char)in[i + 1])) {
            const char* end = strchr(in + i + 1, '>');
            if (end && (size_t)(end - (in + i)) <= 16) {
                i = (size_t)(end - in + 1);
                continue;
            }
        }
        out[j++] = in[i++];
    }
    out[j] = 0;
}

// 把原始字节解码为 UTF-8（支持 UTF-16LE/BE、UTF-8 BOM、UTF-8、GBK 自动回退）
static char* raw_bytes_to_utf8(const unsigned char* raw, size_t sz) {
    if (!raw || sz == 0) return NULL;
    char* text = NULL;
    if (sz >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        size_t cap = sz * 2 + 4;
        text = (char*)malloc(cap);
        if (text) my_utf16_to_utf8((const uint16_t*)(raw + 2), (sz - 2) / 2, text, cap);
    } else if (sz >= 2 && raw[0] == 0xFE && raw[1] == 0xFF) {
        size_t units = (sz - 2) / 2;
        uint16_t* u16 = (uint16_t*)malloc(units * 2 + 2);
        if (u16) {
            for (size_t i = 0; i < units; i++)
                u16[i] = (uint16_t)((raw[2 + i * 2] << 8) | raw[2 + i * 2 + 1]);
            size_t cap = sz * 2 + 4;
            text = (char*)malloc(cap);
            if (text) my_utf16_to_utf8(u16, units, text, cap);
            free(u16);
        }
    } else if (sz >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
        if (is_valid_utf8(raw + 3, sz - 3)) {
            text = (char*)malloc(sz - 2);
            if (text) {
                memcpy(text, raw + 3, sz - 3);
                text[sz - 3] = 0;
            }
        } else {
            size_t cap = sz * 2 + 4;
            text = (char*)malloc(cap);
            if (text) gbk_to_utf8(raw + 3, sz - 3, text, cap);
        }
    } else if (is_valid_utf8(raw, sz)) {
        text = (char*)malloc(sz + 1);
        if (text) {
            memcpy(text, raw, sz);
            text[sz] = 0;
        }
    } else {
        size_t cap = sz * 2 + 4;
        text = (char*)malloc(cap);
        if (text) gbk_to_utf8(raw, sz, text, cap);
    }
    return text;
}

// 解析 UTF-8 歌词文本（支持标准 LRC 时间轴 + 纯文本静态歌词兜底）
static Lyrics* parse_lyrics_utf8(char* text, const char* source_name) {
    if (!text || !*text) return NULL;

    Lyrics* l = (Lyrics*)calloc(1, sizeof(Lyrics));
    if (!l) return NULL;
    l->lines = (LrcLine*)malloc(sizeof(LrcLine) * MAX_LINES);
    if (!l->lines) { free(l); return NULL; }
    snprintf(l->source, sizeof(l->source), "%s", source_name ? source_name : "unknown");

    double offset_sec = 0.0;
    int synced_count = 0;

    // 复制一份供纯文本兜底（strtok_r 会原地修改文本）
    char* copy_for_plain = strdup(text);

    char* save = NULL;
    for (char* line = strtok_r(text, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        while (*line == ' ' || *line == '\t') line++;
        double times[16];
        int ntimes = 0;
        char key[32] = {0};
        char val[256] = {0};
        char* p = line;
        int has_meta = 0;
        while (*p == '[') {
            char* close = strchr(p, ']');
            if (!close || close - p > 48) break;
            char tag[48];
            size_t tn = (size_t)(close - p - 1);
            memcpy(tag, p + 1, tn);
            tag[tn] = 0;
            double tval = 0.0;
            if (parse_time_tag(tag, &tval)) {
                if (ntimes < 16) times[ntimes++] = tval;
            } else {
                char* co = strchr(tag, ':');
                if (co) {
                    *co = 0;
                    snprintf(key, sizeof(key), "%.31s", tag);
                    snprintf(val, sizeof(val), "%s", co + 1);
                    has_meta = 1;
                }
            }
            p = close + 1;
        }
        if (has_meta && key[0] && val[0]) {
            if (!strcasecmp(key, "offset")) offset_sec = atoi(val) / 1000.0;
            else add_meta(l, key, val);
        }
        if (ntimes > 0) {
            while (*p == ' ' || *p == '\t') p++;
            char clean[256];
            strip_inline_tags(p, clean, sizeof(clean));
            size_t tl = strlen(clean);
            while (tl > 0 && (clean[tl - 1] == ' ' || clean[tl - 1] == '\t')) clean[--tl] = 0;
            if (tl > 0) {
                for (int i = 0; i < ntimes; i++) {
                    if (l->n >= MAX_LINES) break;
                    LrcLine* ln = &l->lines[l->n++];
                    ln->t = times[i] - offset_sec;
                    if (ln->t < 0) ln->t = 0;
                    snprintf(ln->text, sizeof(ln->text), "%s", clean);
                    synced_count++;
                }
            }
        }
    }

    if (synced_count > 0) {
        free(copy_for_plain);
        qsort(l->lines, (size_t)l->n, sizeof(LrcLine), cmp_lines);
        return l;
    }

    // 如果没有时间标签但有多行文本，则按静态歌词加载显示
    l->n = 0;
    if (copy_for_plain) {
        char* save2 = NULL;
        for (char* line = strtok_r(copy_for_plain, "\r\n", &save2); line; line = strtok_r(NULL, "\r\n", &save2)) {
            while (*line == ' ' || *line == '\t') line++;
            size_t tl = strlen(line);
            while (tl > 0 && (line[tl - 1] == ' ' || line[tl - 1] == '\t')) line[--tl] = 0;
            if (tl == 0) continue;
            // 跳过纯 [ti:...]/[ar:...] 元数据行
            if (line[0] == '[' && strchr(line, ']')) continue;
            if (l->n >= MAX_LINES) break;
            LrcLine* ln = &l->lines[l->n++];
            ln->t = (double)l->n;
            snprintf(ln->text, sizeof(ln->text), "%s", line);
        }
        free(copy_for_plain);
    }

    if (l->n > 0) {
        l->unsynced = 1;
        return l;
    }

    lyrics_free(l);
    return NULL;
}

static Lyrics* load_lrc_file_path(const char* path, const char* source_label) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 2 * 1024 * 1024) { fclose(f); return NULL; }
    unsigned char* raw = (unsigned char*)malloc((size_t)sz);
    if (!raw) { fclose(f); return NULL; }
    if (fread(raw, 1, (size_t)sz, f) != (size_t)sz) { free(raw); fclose(f); return NULL; }
    fclose(f);

    char* text = raw_bytes_to_utf8(raw, (size_t)sz);
    free(raw);
    if (!text) return NULL;

    Lyrics* l = parse_lyrics_utf8(text, source_label);
    free(text);
    return l;
}

// 在指定目录中不区分大小写匹配 <stem>.lrc
static Lyrics* find_lrc_in_dir_ci(const char* dir, const char* stem, const char* source_label) {
    if (!dir || !*dir || !stem || !*stem) return NULL;
    char want[300];
    snprintf(want, sizeof(want), "%s.lrc", stem);

    char direct[640];
    snprintf(direct, sizeof(direct), "%s/%s", dir, want);
    Lyrics* l = load_lrc_file_path(direct, source_label);
    if (l) return l;

    DIR* d = opendir(dir);
    if (!d) return NULL;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (!strcasecmp(e->d_name, want)) {
            char full[640];
            snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
            closedir(d);
            return load_lrc_file_path(full, source_label);
        }
    }
    closedir(d);
    return NULL;
}

// 解码 ID3 文本载荷（支持 enc=0/1/2/3）为堆分配 UTF-8 字符串
static char* id3_decode_buf(int enc, const unsigned char* t, size_t tn) {
    if (!t || tn == 0) return NULL;
    if (enc == 0) {
        size_t cap = tn * 3 + 4;
        char* out = (char*)malloc(cap);
        if (out) text_to_utf8_auto(t, tn, out, cap);
        return out;
    } else if (enc == 1) {
        if (tn < 2) return NULL;
        int be = 0;
        if (t[0] == 0xFF && t[1] == 0xFE) { t += 2; tn -= 2; }
        else if (t[0] == 0xFE && t[1] == 0xFF) { be = 1; t += 2; tn -= 2; }
        uint16_t* u16 = (uint16_t*)malloc(tn + 2);
        if (!u16) return NULL;
        for (size_t i = 0; i < tn / 2; i++) {
            uint16_t v = (uint16_t)(t[i * 2] | (t[i * 2 + 1] << 8));
            if (be) v = (uint16_t)((v >> 8) | (v << 8));
            u16[i] = v;
        }
        size_t cap = tn * 2 + 4;
        char* out = (char*)malloc(cap);
        if (out) my_utf16_to_utf8(u16, tn / 2, out, cap);
        free(u16);
        return out;
    } else if (enc == 2) {
        uint16_t* u16 = (uint16_t*)malloc(tn + 2);
        if (!u16) return NULL;
        for (size_t i = 0; i < tn / 2; i++)
            u16[i] = (uint16_t)((t[i * 2] << 8) | t[i * 2 + 1]);
        size_t cap = tn * 2 + 4;
        char* out = (char*)malloc(cap);
        if (out) my_utf16_to_utf8(u16, tn / 2, out, cap);
        free(u16);
        return out;
    } else {
        size_t cap = tn * 2 + 4;
        char* out = (char*)malloc(cap);
        if (out) text_to_utf8_auto(t, tn, out, cap);
        return out;
    }
}

static size_t skip_id3_zstr(int enc, const unsigned char* p, const unsigned char* end) {
    if (enc == 1 || enc == 2) {
        const unsigned char* s = p;
        while (s + 1 < end && !(s[0] == 0 && s[1] == 0)) s += 2;
        if (s + 1 < end) s += 2;
        else s = end;
        return (size_t)(s - p);
    } else {
        const unsigned char* s = p;
        while (s < end && *s) s++;
        if (s < end) s++;
        return (size_t)(s - p);
    }
}

static Lyrics* extract_mp3_lyrics(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char h[10];
    if (fread(h, 1, 10, f) != 10 || memcmp(h, "ID3", 3) != 0) { fclose(f); return NULL; }
    int ver = h[3];
    size_t sz = ((size_t)(h[6] & 0x7F) << 21) | ((size_t)(h[7] & 0x7F) << 14) |
                ((size_t)(h[8] & 0x7F) << 7) | (size_t)(h[9] & 0x7F);
    if (sz == 0 || sz > 16u * 1024u * 1024u) { fclose(f); return NULL; }
    unsigned char* body = (unsigned char*)malloc(sz);
    if (!body || fread(body, 1, sz, f) != sz) { free(body); fclose(f); return NULL; }
    fclose(f);

    Lyrics* res = NULL;
    size_t pos = 0;
    while (ver == 2 ? (pos + 6 <= sz) : (pos + 10 <= sz)) {
        char fid[5] = {0};
        size_t fsize = 0;
        if (ver == 2) {
            memcpy(fid, body + pos, 3);
            fsize = ((size_t)body[pos + 3] << 16) | ((size_t)body[pos + 4] << 8) | body[pos + 5];
            pos += 6;
        } else {
            memcpy(fid, body + pos, 4);
            if (ver == 4)
                fsize = ((size_t)(body[pos + 4] & 0x7F) << 21) | ((size_t)(body[pos + 5] & 0x7F) << 14) |
                        ((size_t)(body[pos + 6] & 0x7F) << 7) | (size_t)(body[pos + 7] & 0x7F);
            else
                fsize = ((size_t)body[pos + 4] << 24) | ((size_t)body[pos + 5] << 16) |
                        ((size_t)body[pos + 6] << 8) | (size_t)body[pos + 7];
            pos += 10;
        }
        if (fid[0] == 0 || pos + fsize > sz) break;
        if (fsize == 0) continue;

        const unsigned char* data = body + pos;
        const unsigned char* end = data + fsize;

        // USLT / ULT (Unsynchronised lyrics — 许多播放器也在这里放带 [00:xx.xx] 的 LRC!)
        if ((!strcmp(fid, "USLT") || !strcmp(fid, "ULT")) && fsize > 4) {
            int enc = data[0];
            const unsigned char* p = data + 1 + 3; // skip enc + 3-byte lang
            if (p < end) {
                p += skip_id3_zstr(enc, p, end);
                if (p < end) {
                    char* txt = id3_decode_buf(enc, p, (size_t)(end - p));
                    if (txt) {
                        res = parse_lyrics_utf8(txt, "MP3 USLT 内嵌");
                        free(txt);
                        if (res) break;
                    }
                }
            }
        }
        // SYLT (Synchronised lyrics)
        else if (!strcmp(fid, "SYLT") && fsize > 6) {
            int enc = data[0];
            int ts_fmt = data[4]; // 2 = milliseconds
            const unsigned char* p = data + 6;
            p += skip_id3_zstr(enc, p, end);
            if (ts_fmt == 2 && p < end) {
                Lyrics* l = (Lyrics*)calloc(1, sizeof(Lyrics));
                if (l) {
                    l->lines = (LrcLine*)malloc(sizeof(LrcLine) * MAX_LINES);
                    snprintf(l->source, sizeof(l->source), "MP3 SYLT 内嵌");
                    while (l->lines && p < end && l->n < MAX_LINES) {
                        size_t slen = skip_id3_zstr(enc, p, end);
                        if (p + slen + 4 > end) break;
                        char* txt = id3_decode_buf(enc, p, slen);
                        p += slen;
                        uint32_t ms = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                                      ((uint32_t)p[2] << 8) | (uint32_t)p[3];
                        p += 4;
                        if (txt) {
                            size_t tl = strlen(txt);
                            while (tl > 0 && (txt[tl - 1] == '\r' || txt[tl - 1] == '\n' || txt[tl - 1] == ' '))
                                txt[--tl] = 0;
                            if (tl > 0) {
                                LrcLine* ln = &l->lines[l->n++];
                                ln->t = ms / 1000.0;
                                snprintf(ln->text, sizeof(ln->text), "%s", txt);
                            }
                            free(txt);
                        }
                    }
                    if (l->n > 0) {
                        qsort(l->lines, (size_t)l->n, sizeof(LrcLine), cmp_lines);
                        res = l;
                        break;
                    }
                    lyrics_free(l);
                }
            }
        }
        // TXXX 自定义帧：检查是否为 LYRICS / UNSYNCEDLYRICS
        else if (!strcmp(fid, "TXXX") && fsize > 2) {
            int enc = data[0];
            const unsigned char* p = data + 1;
            size_t dlen = skip_id3_zstr(enc, p, end);
            char* desc = id3_decode_buf(enc, p, dlen);
            if (desc) {
                int is_lyr = (!strcasecmp(desc, "LYRICS") || !strcasecmp(desc, "UNSYNCEDLYRICS") ||
                              !strcasecmp(desc, "UNSYNCED LYRICS") || !strcasecmp(desc, "SYNCEDLYRICS"));
                free(desc);
                if (is_lyr && p + dlen < end) {
                    char* txt = id3_decode_buf(enc, p + dlen, (size_t)(end - (p + dlen)));
                    if (txt) {
                        res = parse_lyrics_utf8(txt, "MP3 TXXX 内嵌");
                        free(txt);
                        if (res) break;
                    }
                }
            }
        }

        pos += fsize;
    }

    free(body);
    return res;
}

static Lyrics* extract_vorbis_lyrics_block(const unsigned char* p, size_t n, const char* source_label) {
    if (n < 8) return NULL;
    uint32_t vendor_len;
    memcpy(&vendor_len, p, 4);
    size_t off = 4 + vendor_len;
    if (off + 4 > n) return NULL;
    uint32_t count;
    memcpy(&count, p + off, 4);
    off += 4;
    for (uint32_t i = 0; i < count && off + 4 <= n; i++) {
        uint32_t len;
        memcpy(&len, p + off, 4);
        off += 4;
        if (off + len > n) break;
        const char* s = (const char*)(p + off);
        const char* eq = (const char*)memchr(s, '=', len);
        if (eq) {
            size_t klen = (size_t)(eq - s);
            char key[40];
            size_t copy_k = klen < sizeof(key) - 1 ? klen : sizeof(key) - 1;
            memcpy(key, s, copy_k);
            key[copy_k] = 0;
            if (!strcasecmp(key, "LYRICS") || !strcasecmp(key, "UNSYNCEDLYRICS") ||
                !strcasecmp(key, "UNSYNCED LYRICS") || !strcasecmp(key, "SYNCEDLYRICS")) {
                size_t vlen = len - klen - 1;
                char* txt = raw_bytes_to_utf8((const unsigned char*)(eq + 1), vlen);
                if (txt) {
                    Lyrics* l = parse_lyrics_utf8(txt, source_label);
                    free(txt);
                    if (l) return l;
                }
            }
        }
        off += len;
    }
    return NULL;
}

static Lyrics* extract_flac_lyrics(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char hdr[8];
    if (fread(hdr, 1, 4, f) != 4) { fclose(f); return NULL; }
    if (!memcmp(hdr, "ID3", 3)) {
        unsigned char idh[6];
        if (fread(idh, 1, 6, f) != 6) { fclose(f); return NULL; }
        unsigned sz = (idh[2] & 0x7F) << 21 | (idh[3] & 0x7F) << 14 | (idh[4] & 0x7F) << 7 | (idh[5] & 0x7F);
        if (fseek(f, (long)sz, SEEK_CUR) != 0) { fclose(f); return NULL; }
        if (fread(hdr, 1, 4, f) != 4) { fclose(f); return NULL; }
    }
    if (memcmp(hdr, "fLaC", 4)) { fclose(f); return NULL; }

    Lyrics* res = NULL;
    for (;;) {
        unsigned char bh[4];
        if (fread(bh, 1, 4, f) != 4) break;
        int last = bh[0] & 0x80;
        int type = bh[0] & 0x7F;
        size_t len = ((size_t)bh[1] << 16) | ((size_t)bh[2] << 8) | bh[3];
        if (type == 4 && len <= 16u * 1024u * 1024u) {
            unsigned char* buf = (unsigned char*)malloc(len);
            if (buf && fread(buf, 1, len, f) == len) {
                res = extract_vorbis_lyrics_block(buf, len, "FLAC 内嵌标签");
            }
            free(buf);
            if (res) break;
        } else {
            if (fseek(f, (long)len, SEEK_CUR) != 0) break;
        }
        if (last) break;
    }
    fclose(f);
    return res;
}

static Lyrics* extract_ogg_lyrics(const char* path) {
    int err;
    stb_vorbis* v = stb_vorbis_open_filename(path, &err, NULL);
    if (!v) return NULL;
    stb_vorbis_comment c = stb_vorbis_get_comment(v);
    Lyrics* res = NULL;
    for (int i = 0; i < c.comment_list_length; i++) {
        const char* s = c.comment_list[i];
        const char* eq = strchr(s, '=');
        if (!eq) continue;
        char key[40];
        size_t klen = (size_t)(eq - s);
        if (klen >= sizeof(key)) klen = sizeof(key) - 1;
        memcpy(key, s, klen);
        key[klen] = 0;
        if (!strcasecmp(key, "LYRICS") || !strcasecmp(key, "UNSYNCEDLYRICS") ||
            !strcasecmp(key, "UNSYNCED LYRICS") || !strcasecmp(key, "SYNCEDLYRICS")) {
            char* txt = raw_bytes_to_utf8((const unsigned char*)(eq + 1), strlen(eq + 1));
            if (txt) {
                res = parse_lyrics_utf8(txt, "OGG 内嵌标签");
                free(txt);
                if (res) break;
            }
        }
    }
    stb_vorbis_close(v);
    return res;
}

Lyrics* lyrics_load_ex(const char* media_path, const char* title, const char* artist) {
    if (!media_path || !*media_path) return NULL;

    char dir[600] = {0};
    char stem[256] = {0};
    const char* slash = strrchr(media_path, '/');
    if (slash) {
        size_t dlen = (size_t)(slash - media_path);
        if (dlen >= sizeof(dir)) dlen = sizeof(dir) - 1;
        memcpy(dir, media_path, dlen);
        dir[dlen] = 0;
    } else {
        snprintf(dir, sizeof(dir), ".");
    }
    basename_no_ext(media_path, stem, sizeof(stem));

    // 1. 同目录同名 .lrc（不区分大小写）
    Lyrics* l = find_lrc_in_dir_ci(dir, stem, "同目录 .lrc");
    if (l) return l;

    // 2. 子目录 /music/lyrics、sdmc:/lyrics、sdmc:/Siren/lyrics
    char sub[640];
    snprintf(sub, sizeof(sub), "%s/lyrics", dir);
    l = find_lrc_in_dir_ci(sub, stem, "lyrics/ 目录 .lrc");
    if (l) return l;

    snprintf(sub, sizeof(sub), "%s/Lyrics", dir);
    l = find_lrc_in_dir_ci(sub, stem, "Lyrics/ 目录 .lrc");
    if (l) return l;

    l = find_lrc_in_dir_ci("sdmc:/music/lyrics", stem, "music/lyrics .lrc");
    if (l) return l;

    l = find_lrc_in_dir_ci("sdmc:/lyrics", stem, "sdmc:/lyrics .lrc");
    if (l) return l;

    l = find_lrc_in_dir_ci("sdmc:/Siren/lyrics", stem, "Siren/lyrics .lrc");
    if (l) return l;

    // 3. 若文件名与歌曲标题不同，按歌名 / 歌手-歌名 匹配 .lrc
    if (title && title[0] && strcasecmp(title, stem) != 0) {
        l = find_lrc_in_dir_ci(dir, title, "标题匹配 .lrc");
        if (l) return l;
        if (artist && artist[0]) {
            char combo[300];
            snprintf(combo, sizeof(combo), "%s - %s", artist, title);
            l = find_lrc_in_dir_ci(dir, combo, "歌手标题 .lrc");
            if (l) return l;
        }
    }

    // 4. 提取音频内嵌歌词（MP3 USLT/SYLT/TXXX、FLAC/OGG LYRICS）
    if (ci_ends_with(media_path, ".mp3")) {
        l = extract_mp3_lyrics(media_path);
        if (l) return l;
    } else if (ci_ends_with(media_path, ".flac")) {
        l = extract_flac_lyrics(media_path);
        if (l) return l;
    } else if (ci_ends_with(media_path, ".ogg")) {
        l = extract_ogg_lyrics(media_path);
        if (l) return l;
    }

    return NULL;
}

Lyrics* lyrics_load(const char* media_path) {
    return lyrics_load_ex(media_path, NULL, NULL);
}

void lyrics_free(Lyrics* l) {
    if (!l) return;
    free(l->lines);
    free(l);
}

int lyrics_index(const Lyrics* l, double t) {
    if (!l || l->n == 0) return -1;
    if (l->unsynced) return 0;
    int lo = 0, hi = l->n - 1;
    if (t < l->lines[0].t) return -1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (l->lines[mid].t <= t) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}
