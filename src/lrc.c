#include "lrc.h"
#include "util.h"
#include "plat.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#if !defined(_WIN32) && !defined(__SWITCH__)
#include <unistd.h>
#include <sys/wait.h>
#endif

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#define MAX_LINES 4096

static char g_lrc_diag[192] = "尚未加载曲目";

const char* lyrics_last_diag(void) {
    return g_lrc_diag;
}

static void set_diag(const char* s) {
    snprintf(g_lrc_diag, sizeof(g_lrc_diag), "%s", s ? s : "");
}

static uint32_t lrc_be32(const unsigned char* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static uint32_t lrc_syncsafe32(const unsigned char* p) {
    return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
           ((uint32_t)(p[2] & 0x7F) << 7)  | (uint32_t)(p[3] & 0x7F);
}

static size_t lrc_deunsync(const unsigned char* src, size_t n, unsigned char* dst) {
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        dst[j++] = src[i];
        if (src[i] == 0xFF && i + 1 < n && src[i + 1] == 0x00) i++;
    }
    return j;
}

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

// 解析 LRC 时间标签：mm:ss、mm:ss.xx、mm:ss.xxx、mm:ss:xx、mm:ss-xx、hh:mm:ss.xx
static int parse_time_tag(const char* tag, double* out_t) {
    if (!tag) return 0;
    while (*tag == ' ' || *tag == '\t') tag++;
    if (!isdigit((unsigned char)tag[0])) return 0;
    const char* p = tag;
    long a = 0;
    int ad = 0;
    while (isdigit((unsigned char)*p) && ad < 5) { a = a * 10 + (*p - '0'); p++; ad++; }
    if (*p != ':' || ad == 0) return 0;
    p++;
    if (!isdigit((unsigned char)*p)) return 0;
    long b = 0;
    int bd = 0;
    while (isdigit((unsigned char)*p) && bd < 3) { b = b * 10 + (*p - '0'); p++; bd++; }

    long hh = 0, mm = a, ss = b;
    double frac = 0.0;
    if (*p == ':') {
        const char* q = p + 1;
        long c = 0;
        int cd = 0;
        while (isdigit((unsigned char)*q) && cd < 4) { c = c * 10 + (*q - '0'); q++; cd++; }
        if (cd > 0 && (*q == '.' || (ad <= 2 && bd == 2 && cd == 2 && c < 60 && a <= 5 && *q == 0 && 0))) {
            hh = a; mm = b; ss = c; p = q;
        }
    }
    if (*p == '.' || *p == ':' || *p == '-' || *p == ',') {
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
    *out_t = (double)hh * 3600.0 + (double)mm * 60.0 + (double)ss + frac;
    return 1;
}

// 清除增强型 LRC 行内时间标签 <00:12.34> 或 {00:12.34}
static void strip_inline_tags(const char* in, char* out, size_t cap) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < cap; ) {
        if ((in[i] == '<' || in[i] == '{') && isdigit((unsigned char)in[i + 1])) {
            char close_ch = (in[i] == '<') ? '>' : '}';
            const char* end = strchr(in + i + 1, close_ch);
            if (end && (size_t)(end - (in + i)) <= 18) {
                i = (size_t)(end - in + 1);
                continue;
            }
        }
        out[j++] = in[i++];
    }
    out[j] = 0;
}

// 把原始字节安全解码为 UTF-8（支持 UTF-16LE/BE 有无 BOM、UTF-8、GBK）
static char* raw_bytes_to_utf8(const unsigned char* raw, size_t sz) {
    if (!raw || sz == 0) return NULL;
    while (sz > 0 && raw[sz - 1] == 0) sz--;
    if (sz == 0) return NULL;

    int is_u16le = (sz >= 2 && raw[0] == 0xFF && raw[1] == 0xFE);
    int is_u16be = (sz >= 2 && raw[0] == 0xFE && raw[1] == 0xFF);
    if (!is_u16le && !is_u16be && sz >= 6) {
        if (raw[0] == '[' && raw[1] == 0 && isdigit(raw[2]) && raw[3] == 0) is_u16le = 2;
        else if (raw[0] == 0 && raw[1] == '[' && raw[2] == 0 && isdigit(raw[3])) is_u16be = 2;
    }

    if (is_u16le || is_u16be) {
        size_t off = (is_u16le == 1 || is_u16be == 1) ? 2 : 0;
        size_t units = (sz - off) / 2;
        uint16_t* u16 = (uint16_t*)malloc(units * 2 + 2);
        if (!u16) return NULL;
        size_t w = 0;
        for (size_t i = 0; i < units; i++) {
            uint16_t v = is_u16be
                ? (uint16_t)((raw[off + i * 2] << 8) | raw[off + i * 2 + 1])
                : (uint16_t)(raw[off + i * 2] | (raw[off + i * 2 + 1] << 8));
            if (v == 0xFEFF) continue;
            if (v == 0) v = '\n';
            u16[w++] = v;
        }
        size_t cap = w * 3 + 8;
        char* text = (char*)malloc(cap);
        if (text) my_utf16_to_utf8(u16, w, text, cap);
        free(u16);
        return text;
    }

    // 去除内部 NUL 字节，避免截断多段歌词
    unsigned char* clean = (unsigned char*)malloc(sz + 1);
    if (!clean) return NULL;
    size_t clen = 0;
    for (size_t i = 0; i < sz; i++) {
        if (raw[i] == 0xEF && i + 2 < sz && raw[i + 1] == 0xBB && raw[i + 2] == 0xBF) {
            i += 2;
            continue;
        }
        clean[clen++] = (raw[i] == 0) ? '\n' : raw[i];
    }
    clean[clen] = 0;

    char* text = NULL;
    if (is_valid_utf8(clean, clen)) {
        text = (char*)malloc(clen + 1);
        if (text) { memcpy(text, clean, clen); text[clen] = 0; }
    } else {
        size_t cap = clen * 2 + 8;
        text = (char*)malloc(cap);
        if (text) gbk_to_utf8(clean, clen, text, cap);
    }
    free(clean);
    return text;
}

// 规范化歌词文本：解包 JSON "lyric":"..."、还原字面量 \n、替换全角【】与全角冒号
static char* normalize_lrc_text(const char* src) {
    if (!src || !*src) return NULL;
    const char* p = src;
    // 若是网易云/QQ 导出的 JSON 格式 {"lrc":{"lyric":"[00:..."}}，定位到第一个 [00: 或 "lyric"
    if (p[0] == '{' && strstr(p, "\"lyric\"")) {
        const char* lk = strstr(p, "\"lyric\"");
        const char* lb = strchr(lk + 7, '[');
        if (lb) p = lb;
    }
    size_t slen = strlen(p);
    char* out = (char*)malloc(slen + 8);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < slen; ) {
        // 跳过残留 UTF-8 BOM
        if ((unsigned char)p[i] == 0xEF && i + 2 < slen &&
            (unsigned char)p[i + 1] == 0xBB && (unsigned char)p[i + 2] == 0xBF) {
            i += 3;
            continue;
        }
        // 全角 【 -> [ , 】 -> ] , ： -> :
        if ((unsigned char)p[i] == 0xE3 && i + 2 < slen && (unsigned char)p[i + 1] == 0x80 &&
            ((unsigned char)p[i + 2] == 0x90 || (unsigned char)p[i + 2] == 0x91)) {
            out[j++] = ((unsigned char)p[i + 2] == 0x90) ? '[' : ']';
            i += 3;
            continue;
        }
        if ((unsigned char)p[i] == 0xEF && i + 2 < slen &&
            (unsigned char)p[i + 1] == 0xBC && (unsigned char)p[i + 2] == 0x9A) {
            out[j++] = ':';
            i += 3;
            continue;
        }
        // 转义换行 \r\n 或 \n
        if (p[i] == '\\' && i + 1 < slen) {
            if (p[i + 1] == 'n') { out[j++] = '\n'; i += 2; continue; }
            if (p[i + 1] == 'r') { out[j++] = '\r'; i += 2; continue; }
            if (p[i + 1] == '"' && src[0] == '{') { break; }
        }
        out[j++] = p[i++];
    }
    out[j] = 0;
    return out;
}

static Lyrics* parse_lyrics_utf8(const char* raw_text, const char* source_name) {
    char* norm = normalize_lrc_text(raw_text);
    if (!norm || !*norm) { free(norm); return NULL; }

    Lyrics* l = (Lyrics*)calloc(1, sizeof(Lyrics));
    if (!l) { free(norm); return NULL; }
    l->lines = (LrcLine*)malloc(sizeof(LrcLine) * MAX_LINES);
    if (!l->lines) { free(l); free(norm); return NULL; }
    snprintf(l->source, sizeof(l->source), "%s", source_name ? source_name : "unknown");

    double offset_sec = 0.0;
    int synced_count = 0;
    char* copy_for_plain = strdup(norm);

    char* save = NULL;
    for (char* line = strtok_r(norm, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        char* cur = line;
        while (cur && *cur) {
            while (*cur == ' ' || *cur == '\t') cur++;
            if (*cur != '[') break;

            double times[24];
            int ntimes = 0;
            char* p = cur;
            while (*p == '[') {
                char* close = strchr(p, ']');
                if (!close || close - p > 52) break;
                char tag[56];
                size_t tn = (size_t)(close - p - 1);
                memcpy(tag, p + 1, tn);
                tag[tn] = 0;
                double tval = 0.0;
                if (parse_time_tag(tag, &tval)) {
                    if (ntimes < 24) times[ntimes++] = tval;
                } else {
                    char* co = strchr(tag, ':');
                    if (co) {
                        *co = 0;
                        if (!strcasecmp(tag, "offset")) offset_sec = atoi(co + 1) / 1000.0;
                        else add_meta(l, tag, co + 1);
                    }
                }
                p = close + 1;
                while (*p == ' ' || *p == '\t') p++;
            }

            // 检查单行内是否紧接着下一段 [mm:ss] 压缩歌词
            char* next_seg = NULL;
            for (char* q = p; *q; q++) {
                if (*q == '[' && isdigit((unsigned char)q[1])) {
                    char* qc = strchr(q, ']');
                    if (qc && qc - q <= 16) {
                        char tchk[20];
                        size_t cn = (size_t)(qc - q - 1);
                        memcpy(tchk, q + 1, cn);
                        tchk[cn] = 0;
                        double dummy = 0.0;
                        if (parse_time_tag(tchk, &dummy)) { next_seg = q; break; }
                    }
                }
            }

            char seg_buf[256];
            size_t seg_len = next_seg ? (size_t)(next_seg - p) : strlen(p);
            if (seg_len >= sizeof(seg_buf)) seg_len = sizeof(seg_buf) - 1;
            memcpy(seg_buf, p, seg_len);
            seg_buf[seg_len] = 0;

            if (ntimes > 0) {
                char clean[256];
                strip_inline_tags(seg_buf, clean, sizeof(clean));
                char* cp = clean;
                while (*cp == ' ' || *cp == '\t') cp++;
                size_t tl = strlen(cp);
                while (tl > 0 && (cp[tl - 1] == ' ' || cp[tl - 1] == '\t')) cp[--tl] = 0;
                if (tl > 0) {
                    for (int i = 0; i < ntimes; i++) {
                        if (l->n >= MAX_LINES) break;
                        LrcLine* ln = &l->lines[l->n++];
                        memset(ln, 0, sizeof(*ln));
                        ln->t = times[i] - offset_sec + synced_count * 1e-6;
                        if (ln->t < 0) ln->t = 0;
                        snprintf(ln->text, sizeof(ln->text), "%s", cp);
                        snprintf(ln->orig, sizeof(ln->orig), "%s", cp);
                        synced_count++;
                    }
                }
            }
            cur = next_seg;
        }
    }
    free(norm);

    if (synced_count > 0) {
        free(copy_for_plain);
        qsort(l->lines, (size_t)l->n, sizeof(LrcLine), cmp_lines);
        // 合并相同时间戳的双语歌词（原文 + 翻译同时间戳）
        int w = 0;
        for (int i = 0; i < l->n; i++) {
            if (w > 0 && fabs(l->lines[w - 1].t - l->lines[i].t) < 0.015) {
                if (strcmp(l->lines[w - 1].text, l->lines[i].text) != 0) {
                    if (!l->lines[w - 1].orig[0])
                        snprintf(l->lines[w - 1].orig, sizeof(l->lines[w - 1].orig), "%s", l->lines[w - 1].text);
                    if (!l->lines[w - 1].sub[0])
                        snprintf(l->lines[w - 1].sub, sizeof(l->lines[w - 1].sub), "%s", l->lines[i].text);
                    char merged[256];
                    snprintf(merged, sizeof(merged), "%.120s · %.120s", l->lines[w - 1].orig, l->lines[w - 1].sub);
                    memcpy(l->lines[w - 1].text, merged, sizeof(l->lines[w - 1].text));
                }
                continue;
            }
            if (w != i) l->lines[w] = l->lines[i];
            w++;
        }
        l->n = w;
        char dmsg[128];
        snprintf(dmsg, sizeof(dmsg), "成功：%s (%d行时间轴)", l->source, l->n);
        set_diag(dmsg);
        return l;
    }

    // 纯文本静态歌词兜底
    l->n = 0;
    if (copy_for_plain) {
        char* save2 = NULL;
        for (char* line = strtok_r(copy_for_plain, "\r\n", &save2); line; line = strtok_r(NULL, "\r\n", &save2)) {
            while (*line == ' ' || *line == '\t') line++;
            size_t tl = strlen(line);
            while (tl > 0 && (line[tl - 1] == ' ' || line[tl - 1] == '\t')) line[--tl] = 0;
            if (tl == 0) continue;
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
        char dmsg[128];
        snprintf(dmsg, sizeof(dmsg), "成功：%s (%d行纯文本)", l->source, l->n);
        set_diag(dmsg);
        return l;
    }

    lyrics_free(l);
    return NULL;
}

static void join_lrc_path(char* dst, size_t cap, const char* dir, const char* name) {
    if (!dir || !*dir) { snprintf(dst, cap, "%s", name); return; }
    size_t dlen = strlen(dir);
    while (dlen > 0 && (dir[dlen - 1] == '/' || dir[dlen - 1] == '\\')) dlen--;
    if (dlen == 0) snprintf(dst, cap, "/%s", name);
    else snprintf(dst, cap, "%.*s/%s", (int)dlen, dir, name);
}

static Lyrics* load_lrc_file_path(const char* path, const char* source_label) {
    FILE* f = plat_fopen(path, "rb");
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

static int ci_contains_sub(const char* hay, const char* needle) {
    if (!hay || !needle || !*needle) return 0;
    size_t nl = strlen(needle);
    for (const char* p = hay; *p; p++) {
        size_t k = 0;
        while (k < nl && tolower((unsigned char)p[k]) == tolower((unsigned char)needle[k])) k++;
        if (k == nl) return 1;
    }
    return 0;
}

static Lyrics* find_lrc_in_dir_ci(const char* dir, const char* stem, const char* source_label) {
    if (!dir || !*dir || !stem || !*stem) return NULL;
    char open_dir[600];
    if (!strcmp(dir, "sdmc:")) snprintf(open_dir, sizeof(open_dir), "sdmc:/");
    else snprintf(open_dir, sizeof(open_dir), "%s", dir);

    char want[340];
    snprintf(want, sizeof(want), "%.300s.lrc", stem);

    char direct[680];
    join_lrc_path(direct, sizeof(direct), open_dir, want);
    Lyrics* l = load_lrc_file_path(direct, source_label);
    if (l) return l;

    PlatDir* d = plat_opendir(open_dir);
    if (!d) return NULL;
    PlatDirent e;
    while (plat_readdir(d, &e)) {
        if (!strcasecmp(e.d_name, want)) {
            char full[680];
            join_lrc_path(full, sizeof(full), open_dir, e.d_name);
            plat_closedir(d);
            return load_lrc_file_path(full, source_label);
        }
    }
    plat_closedir(d);
    return NULL;
}

// 模糊匹配目录中的 .lrc（应对 "01. 歌名 (Live).flac" vs "歌手 - 歌名.lrc" 或 "歌名 (Live)" vs "歌名.lrc"）
static void strip_title_parens(const char* in, char* out, size_t cap) {
    if (!in || !*in) { if (cap) out[0] = 0; return; }
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < cap; i++) {
        if (in[i] == '(' || in[i] == '[' ||
            ((unsigned char)in[i] == 0xEF && (unsigned char)in[i + 1] == 0xBC && (unsigned char)in[i + 2] == 0x88)) {
            break;
        }
        out[j++] = in[i];
    }
    while (j > 0 && (out[j - 1] == ' ' || out[j - 1] == '-' || out[j - 1] == '_')) j--;
    out[j] = 0;
}

static Lyrics* find_lrc_fuzzy_in_dir(const char* dir, const char* key_title, const char* key_artist, const char* source_label) {
    if (!dir || !*dir || !key_title || strlen(key_title) < 2) return NULL;
    char open_dir[600];
    if (!strcmp(dir, "sdmc:")) snprintf(open_dir, sizeof(open_dir), "sdmc:/");
    else snprintf(open_dir, sizeof(open_dir), "%s", dir);

    char base_title[300];
    strip_title_parens(key_title, base_title, sizeof(base_title));
    if (strlen(base_title) < 2) snprintf(base_title, sizeof(base_title), "%s", key_title);

    PlatDir* d = plat_opendir(open_dir);
    if (!d) return NULL;
    char best_name[520] = {0};
    int best_score = 0;
    PlatDirent e;
    while (plat_readdir(d, &e)) {
        if (!ci_ends_with(e.d_name, ".lrc")) continue;
        char fstem[300];
        basename_no_ext(e.d_name, fstem, sizeof(fstem));
        int fwd = ci_contains_sub(fstem, key_title) || ci_contains_sub(fstem, base_title);
        int rev = (strlen(fstem) >= 3 && (ci_contains_sub(key_title, fstem) || ci_contains_sub(base_title, fstem)));
        if (!fwd && !rev) continue;
        int score = fwd ? 10 : 8;
        if (key_artist && key_artist[0] && ci_contains_sub(fstem, key_artist)) score += 20;
        if (strlen(fstem) <= strlen(base_title) + 4) score += 5;
        if (score > best_score) {
            best_score = score;
            snprintf(best_name, sizeof(best_name), "%s", e.d_name);
        }
    }
    plat_closedir(d);
    if (best_score >= 15 || (best_score >= 10 && strlen(base_title) >= 4) || (best_score >= 8 && strlen(best_name) >= 6)) {
        char full[680];
        join_lrc_path(full, sizeof(full), open_dir, best_name);
        return load_lrc_file_path(full, source_label);
    }
    return NULL;
}

static void strip_track_prefix(const char* in, char* out, size_t cap) {
    const char* p = in;
    while (*p == ' ' || *p == '[' || *p == '(') {
        if ((*p == '[' || *p == '(') && isdigit((unsigned char)p[1])) {
            char cl = (*p == '[') ? ']' : ')';
            const char* c = strchr(p, cl);
            if (c && c - p <= 6) { p = c + 1; continue; }
        }
        break;
    }
    const char* q = p;
    int nd = 0;
    while (isdigit((unsigned char)*q) && nd < 4) { q++; nd++; }
    if (nd > 0 && (*q == '.' || *q == '-' || *q == '_' || *q == ' ')) {
        while (*q == '.' || *q == '-' || *q == '_' || *q == ' ') q++;
        if (*q) p = q;
    }
    snprintf(out, cap, "%s", p);
}

static char* id3_decode_buf(int enc, const unsigned char* t, size_t tn) {
    if (!t || tn == 0) return NULL;
    if (enc == 1 || enc == 2) {
        int be = (enc == 2);
        if (tn >= 2 && t[0] == 0xFF && t[1] == 0xFE) { be = 0; t += 2; tn -= 2; }
        else if (tn >= 2 && t[0] == 0xFE && t[1] == 0xFF) { be = 1; t += 2; tn -= 2; }
        size_t units = tn / 2;
        uint16_t* u16 = (uint16_t*)malloc(units * 2 + 2);
        if (!u16) return NULL;
        size_t w = 0;
        for (size_t i = 0; i < units; i++) {
            uint16_t v = be ? (uint16_t)((t[i * 2] << 8) | t[i * 2 + 1])
                            : (uint16_t)(t[i * 2] | (t[i * 2 + 1] << 8));
            if (v == 0xFEFF) continue;
            if (v == 0) break;
            u16[w++] = v;
        }
        size_t cap = w * 3 + 8;
        char* out = (char*)malloc(cap);
        if (out) my_utf16_to_utf8(u16, w, out, cap);
        free(u16);
        return out;
    }
    return raw_bytes_to_utf8(t, tn);
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

static int is_id3_fid4(const unsigned char* p) {
    for (int i = 0; i < 4; i++) {
        unsigned char c = p[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return 0;
    }
    return 1;
}

static int is_lyric_key_name(const char* key) {
    if (!key || !*key) return 0;
    if (!strcasecmp(key, "LYRICIST") || !strcasecmp(key, "LYRICIST_SORT")) return 0;
    if (!strncasecmp(key, "LYRIC", 5)) return 1;
    if (!strncasecmp(key, "UNSYNCED", 8)) return 1;
    if (!strncasecmp(key, "SYNCED", 6)) return 1;
    if (!strcasecmp(key, "LRC") || !strcasecmp(key, "USLT") || !strcasecmp(key, "KRC")) return 1;
    return 0;
}

static Lyrics* extract_id3v2_block(const unsigned char* hdr10, const unsigned char* raw_body, size_t raw_sz, const char* tag_src) {
    if (!hdr10 || !raw_body || raw_sz == 0 || memcmp(hdr10, "ID3", 3) != 0) return NULL;
    int ver = hdr10[3];
    int flags = hdr10[5];

    unsigned char* alloc_body = NULL;
    const unsigned char* body = raw_body;
    size_t sz = raw_sz;

    if (flags & 0x80) {
        alloc_body = (unsigned char*)malloc(raw_sz);
        if (!alloc_body) return NULL;
        sz = lrc_deunsync(raw_body, raw_sz, alloc_body);
        body = alloc_body;
    }

    size_t pos = 0;
    if ((flags & 0x40) && pos + 4 <= sz) {
        size_t ext_sz = (ver == 4) ? lrc_syncsafe32(body + pos) : lrc_be32(body + pos);
        if (ver == 3) ext_sz += 4;
        if (ext_sz >= 4 && pos + ext_sz <= sz) pos += ext_sz;
    }

    Lyrics* res = NULL;
    Lyrics* comm_candidate = NULL;

    while (ver == 2 ? (pos + 6 <= sz) : (pos + 10 <= sz)) {
        char fid[5] = {0};
        size_t fsize = 0;
        unsigned char flag2 = 0;
        if (ver == 2) {
            memcpy(fid, body + pos, 3);
            fsize = ((size_t)body[pos + 3] << 16) | ((size_t)body[pos + 4] << 8) | body[pos + 5];
            pos += 6;
        } else {
            if (!is_id3_fid4(body + pos)) break;
            memcpy(fid, body + pos, 4);
            flag2 = body[pos + 9];
            if (ver == 4) {
                size_t ss = lrc_syncsafe32(body + pos + 4);
                size_t be = lrc_be32(body + pos + 4);
                fsize = ss;
                if (ss != be && pos + 10 + ss < sz && !is_id3_fid4(body + pos + 10 + ss) &&
                    body[pos + 10 + ss] != 0 && pos + 10 + be <= sz &&
                    (pos + 10 + be == sz || body[pos + 10 + be] == 0 || is_id3_fid4(body + pos + 10 + be))) {
                    fsize = be;
                }
            } else {
                fsize = lrc_be32(body + pos + 4);
            }
            pos += 10;
        }
        if (fid[0] == 0 || pos + fsize > sz) break;
        if (fsize == 0) continue;

        const unsigned char* data = body + pos;
        size_t dlen = fsize;
        unsigned char* frame_unsync = NULL;

        if (ver == 4) {
            if (flag2 & 0x40) { if (dlen > 1) { data++; dlen--; } }
            if (flag2 & 0x01) { if (dlen > 4) { data += 4; dlen -= 4; } }
            if ((flag2 & 0x02) && !(flags & 0x80) && dlen > 0) {
                frame_unsync = (unsigned char*)malloc(dlen);
                if (frame_unsync) {
                    dlen = lrc_deunsync(data, dlen, frame_unsync);
                    data = frame_unsync;
                }
            }
        }
        const unsigned char* end = data + dlen;

        if ((!strcmp(fid, "USLT") || !strcmp(fid, "ULT")) && dlen > 4) {
            int enc = data[0];
            const unsigned char* p = data + 1;
            // 标准 USLT 有 3 字节语言码 + 描述符\0，但少数非标工具直接写 [00: / BOM 或单字节 0x00 结束描述符
            if (!((p + 1 < end && ((p[0] == 0xFF && p[1] == 0xFE) || (p[0] == 0xFE && p[1] == 0xFF))) ||
                  (p + 1 < end && p[0] == '[' && isdigit(p[1])))) {
                if (p + 3 <= end) p += 3;
                if ((enc == 1 || enc == 2) && p + 2 < end && p[0] == 0 &&
                    ((p[1] == 0xFF && p[2] == 0xFE) || (p[1] == 0xFE && p[2] == 0xFF) || p[1] == '[')) {
                    p += 1;
                } else if (p < end) {
                    p += skip_id3_zstr(enc, p, end);
                }
            }
            if (p < end) {
                char* txt = id3_decode_buf(enc, p, (size_t)(end - p));
                if (txt) {
                    res = parse_lyrics_utf8(txt, tag_src);
                    free(txt);
                }
            }
        } else if ((!strcmp(fid, "SYLT") || !strcmp(fid, "SLT")) && dlen > 6) {
            int enc = data[0];
            int ts_fmt = data[4];
            const unsigned char* p = data + 6;
            p += skip_id3_zstr(enc, p, end);
            if (ts_fmt == 2 && p < end) {
                Lyrics* l = (Lyrics*)calloc(1, sizeof(Lyrics));
                if (l) {
                    l->lines = (LrcLine*)malloc(sizeof(LrcLine) * MAX_LINES);
                    snprintf(l->source, sizeof(l->source), "%s SYLT", tag_src);
                    while (l->lines && p < end && l->n < MAX_LINES) {
                        size_t slen = skip_id3_zstr(enc, p, end);
                        if (p + slen + 4 > end) break;
                        char* txt = id3_decode_buf(enc, p, slen);
                        p += slen;
                        uint32_t ms = lrc_be32(p);
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
                    } else {
                        lyrics_free(l);
                    }
                }
            }
        } else if ((!strcmp(fid, "TXXX") || !strcmp(fid, "TXX")) && dlen > 2) {
            int enc = data[0];
            const unsigned char* p = data + 1;
            size_t zlen = skip_id3_zstr(enc, p, end);
            char* desc = id3_decode_buf(enc, p, zlen);
            if (desc) {
                int is_lyr = is_lyric_key_name(desc);
                free(desc);
                if (is_lyr && p + zlen < end) {
                    char* txt = id3_decode_buf(enc, p + zlen, (size_t)(end - (p + zlen)));
                    if (txt) {
                        res = parse_lyrics_utf8(txt, "ID3 TXXX 内嵌");
                        free(txt);
                    }
                }
            }
        } else if (!comm_candidate && (!strcmp(fid, "COMM") || !strcmp(fid, "COM")) && dlen > 6) {
            int enc = data[0];
            const unsigned char* p = data + 4;
            if (p < end) p += skip_id3_zstr(enc, p, end);
            if (p < end) {
                char* txt = id3_decode_buf(enc, p, (size_t)(end - p));
                if (txt) {
                    if (strstr(txt, "[00:") || strstr(txt, "[01:")) {
                        comm_candidate = parse_lyrics_utf8(txt, "ID3 COMM 时间轴");
                    }
                    free(txt);
                }
            }
        }

        if (frame_unsync) free(frame_unsync);
        if (res) break;
        pos += fsize;
    }

    if (alloc_body) free(alloc_body);
    if (res) {
        if (comm_candidate) lyrics_free(comm_candidate);
        return res;
    }
    return comm_candidate;
}

// 解析文件尾部的 APEv2 歌词标签（Foobar2000 / 千千静听 / MusicBee 常用）
static Lyrics* extract_apev2_eof(FILE* f) {
    if (fseek(f, 0, SEEK_END) != 0) return NULL;
    long fsz = ftell(f);
    if (fsz < 64) return NULL;

    long tail_off[2] = { fsz - 32, fsz - 128 - 32 };
    for (int t = 0; t < 2; t++) {
        if (tail_off[t] < 0) continue;
        unsigned char ftr[32];
        if (fseek(f, tail_off[t], SEEK_SET) != 0 || fread(ftr, 1, 32, f) != 32) continue;
        if (memcmp(ftr, "APETAGEX", 8) != 0) continue;
        uint32_t tag_sz = 0, item_cnt = 0;
        memcpy(&tag_sz, ftr + 12, 4);
        memcpy(&item_cnt, ftr + 16, 4);
        if (tag_sz < 32 || tag_sz > 4u * 1024u * 1024u || (long)tag_sz > tail_off[t] + 32) continue;
        long items_start = tail_off[t] + 32 - (long)tag_sz;
        size_t items_len = tag_sz - 32;
        unsigned char* buf = (unsigned char*)malloc(items_len);
        if (!buf) return NULL;
        if (fseek(f, items_start, SEEK_SET) != 0 || fread(buf, 1, items_len, f) != items_len) {
            free(buf);
            continue;
        }
        size_t off = 0;
        for (uint32_t i = 0; i < item_cnt && off + 9 <= items_len; i++) {
            uint32_t vlen = 0;
            memcpy(&vlen, buf + off, 4);
            off += 8;
            const char* key = (const char*)(buf + off);
            const unsigned char* kend = (const unsigned char*)memchr(key, 0, items_len - off);
            if (!kend) break;
            off = (size_t)(kend - buf) + 1;
            if (vlen > items_len || off + vlen > items_len) break;
            if (is_lyric_key_name(key) && vlen > 0) {
                char* txt = raw_bytes_to_utf8(buf + off, vlen);
                if (txt) {
                    Lyrics* l = parse_lyrics_utf8(txt, "APEv2 内嵌标签");
                    free(txt);
                    if (l) { free(buf); return l; }
                }
            }
            off += vlen;
        }
        free(buf);
    }
    return NULL;
}

static Lyrics* extract_mp3_lyrics(const char* path) {
    FILE* f = plat_fopen(path, "rb");
    if (!f) return NULL;
    unsigned char h[10];
    if (fread(h, 1, 10, f) == 10 && !memcmp(h, "ID3", 3)) {
        size_t sz = lrc_syncsafe32(h + 6);
        if (sz > 0 && sz <= 16u * 1024u * 1024u) {
            unsigned char* body = (unsigned char*)malloc(sz);
            if (body && fread(body, 1, sz, f) == sz) {
                Lyrics* l = extract_id3v2_block(h, body, sz, "MP3 ID3v2 内嵌");
                free(body);
                if (l) { fclose(f); return l; }
            } else {
                free(body);
            }
        }
    }
    Lyrics* ape = extract_apev2_eof(f);
    fclose(f);
    return ape;
}

static Lyrics* extract_wav_lyrics(const char* path) {
    FILE* f = plat_fopen(path, "rb");
    if (!f) return NULL;
    unsigned char rh[12];
    if (fread(rh, 1, 12, f) != 12 || memcmp(rh, "RIFF", 4) != 0) { fclose(f); return NULL; }
    while (!feof(f)) {
        unsigned char ch[8];
        if (fread(ch, 1, 8, f) != 8) break;
        uint32_t csz = 0;
        memcpy(&csz, ch + 4, 4);
        if ((!memcmp(ch, "ID3 ", 4) || !memcmp(ch, "id3 ", 4)) && csz > 10 && csz <= 16u * 1024u * 1024u) {
            unsigned char h[10];
            if (fread(h, 1, 10, f) == 10 && !memcmp(h, "ID3", 3)) {
                size_t sz = lrc_syncsafe32(h + 6);
                if (sz > 0 && sz <= csz - 10) {
                    unsigned char* body = (unsigned char*)malloc(sz);
                    if (body && fread(body, 1, sz, f) == sz) {
                        Lyrics* l = extract_id3v2_block(h, body, sz, "WAV ID3 内嵌");
                        free(body);
                        if (l) { fclose(f); return l; }
                    } else {
                        free(body);
                    }
                }
            }
            break;
        } else {
            long skip = (long)csz + (csz & 1);
            if (fseek(f, skip, SEEK_CUR) != 0) break;
        }
    }
    fclose(f);
    return NULL;
}

static Lyrics* extract_vorbis_lyrics_block(const unsigned char* p, size_t n, const char* source_label) {
    if (n < 8) return NULL;
    uint32_t vendor_len;
    memcpy(&vendor_len, p, 4);
    size_t off = 4 + (size_t)vendor_len;
    if (off + 4 > n) return NULL;
    uint32_t count;
    memcpy(&count, p + off, 4);
    off += 4;
    Lyrics* fallback_comm = NULL;
    for (uint32_t i = 0; i < count && off + 4 <= n; i++) {
        uint32_t len;
        memcpy(&len, p + off, 4);
        off += 4;
        if (len > n || off + len > n) break;
        const char* s = (const char*)(p + off);
        const char* eq = (const char*)memchr(s, '=', len);
        if (eq) {
            size_t klen = (size_t)(eq - s);
            char key[48];
            size_t copy_k = klen < sizeof(key) - 1 ? klen : sizeof(key) - 1;
            memcpy(key, s, copy_k);
            key[copy_k] = 0;
            size_t vlen = len - klen - 1;
            if (is_lyric_key_name(key) && vlen > 0) {
                char* txt = raw_bytes_to_utf8((const unsigned char*)(eq + 1), vlen);
                if (txt) {
                    Lyrics* l = parse_lyrics_utf8(txt, source_label);
                    free(txt);
                    if (l) {
                        if (fallback_comm) lyrics_free(fallback_comm);
                        return l;
                    }
                }
            } else if (!fallback_comm && (!strcasecmp(key, "COMMENT") || !strcasecmp(key, "DESCRIPTION")) && vlen > 10) {
                char* txt = raw_bytes_to_utf8((const unsigned char*)(eq + 1), vlen);
                if (txt) {
                    if (strstr(txt, "[00:") || strstr(txt, "[01:"))
                        fallback_comm = parse_lyrics_utf8(txt, source_label);
                    free(txt);
                }
            }
        }
        off += len;
    }
    return fallback_comm;
}

static Lyrics* extract_flac_lyrics(const char* path) {
    FILE* f = plat_fopen(path, "rb");
    if (!f) return NULL;
    unsigned char hdr[10];
    if (fread(hdr, 1, 4, f) != 4) { fclose(f); return NULL; }
    Lyrics* id3_lrc = NULL;
    if (!memcmp(hdr, "ID3", 3)) {
        if (fread(hdr + 4, 1, 6, f) != 6) { fclose(f); return NULL; }
        size_t sz = lrc_syncsafe32(hdr + 6);
        if (sz > 0 && sz <= 8u * 1024u * 1024u) {
            unsigned char* body = (unsigned char*)malloc(sz);
            if (body && fread(body, 1, sz, f) == sz) {
                id3_lrc = extract_id3v2_block(hdr, body, sz, "FLAC ID3v2 内嵌");
            }
            free(body);
        } else if (fseek(f, (long)sz, SEEK_CUR) != 0) {
            fclose(f);
            return NULL;
        }
        if (fread(hdr, 1, 4, f) != 4) { fclose(f); return id3_lrc; }
    }
    if (memcmp(hdr, "fLaC", 4)) { fclose(f); return id3_lrc; }

    Lyrics* res = NULL;
    for (;;) {
        unsigned char bh[4];
        if (fread(bh, 1, 4, f) != 4) break;
        int last = bh[0] & 0x80;
        int type = bh[0] & 0x7F;
        size_t len = ((size_t)bh[1] << 16) | ((size_t)bh[2] << 8) | bh[3];
        if (type == 4 && len <= 16u * 1024u * 1024u) {
            unsigned char* buf = (unsigned char*)malloc(len ? len : 1);
            if (buf && fread(buf, 1, len, f) == len) {
                res = extract_vorbis_lyrics_block(buf, len, "FLAC Vorbis 内嵌");
            }
            free(buf);
            if (res) break;
        } else {
            if (fseek(f, (long)len, SEEK_CUR) != 0) break;
        }
        if (last) break;
    }
    if (!res) res = extract_apev2_eof(f);
    fclose(f);
    if (res) {
        if (id3_lrc) lyrics_free(id3_lrc);
        return res;
    }
    return id3_lrc;
}

static Lyrics* extract_ogg_lyrics(const char* path) {
    int err = 0;
    FILE* fp = plat_fopen(path, "rb");
    stb_vorbis* v = fp ? stb_vorbis_open_file(fp, 1, &err, NULL) : NULL;
    if (!v && fp) fclose(fp);
    if (!v) return NULL;
    stb_vorbis_comment c = stb_vorbis_get_comment(v);
    Lyrics* res = NULL;
    for (int i = 0; i < c.comment_list_length; i++) {
        const char* s = c.comment_list[i];
        const char* eq = strchr(s, '=');
        if (!eq) continue;
        char key[48];
        size_t klen = (size_t)(eq - s);
        if (klen >= sizeof(key)) klen = sizeof(key) - 1;
        memcpy(key, s, klen);
        key[klen] = 0;
        if (is_lyric_key_name(key)) {
            char* txt = raw_bytes_to_utf8((const unsigned char*)(eq + 1), strlen(eq + 1));
            if (txt) {
                res = parse_lyrics_utf8(txt, "OGG Vorbis 内嵌");
                free(txt);
                if (res) break;
            }
        }
    }
    stb_vorbis_close(v);
    return res;
}

// 终极二进制特征兜底扫描：扫描文件首 512KB 与尾 256KB 中的 [00:xx / [01:xx 时间轴块
static Lyrics* scan_buf_for_lrc(const unsigned char* buf, size_t n, const char* label) {
    if (!buf || n < 16) return NULL;
    for (size_t i = 0; i + 12 < n; i++) {
        // UTF-8 / GBK 特征：[00:xx 或 [01:xx 或 [ti:
        if (buf[i] == '[' &&
            ((buf[i + 1] == '0' && (buf[i + 2] == '0' || buf[i + 2] == '1' || buf[i + 2] == '2') &&
              buf[i + 3] == ':' && isdigit(buf[i + 4]) && isdigit(buf[i + 5])) ||
             (!strncasecmp((const char*)(buf + i), "[ti:", 4) || !strncasecmp((const char*)(buf + i), "[ar:", 4)))) {
            size_t end = i;
            while (end < n && buf[end] != 0) {
                // 若遇到大段连续不可打印控制字节则截断
                if (buf[end] < 0x09 || (buf[end] > 0x0D && buf[end] < 0x20)) break;
                end++;
            }
            if (end - i >= 24) {
                char* txt = raw_bytes_to_utf8(buf + i, end - i);
                if (txt) {
                    Lyrics* l = parse_lyrics_utf8(txt, label);
                    free(txt);
                    if (l && !l->unsynced && l->n >= 2) return l;
                    if (l) lyrics_free(l);
                }
            }
            i = end;
        }
        // UTF-16LE 特征：'[' 0 '0' 0 ('0'|'1') 0 ':' 0
        else if (i + 16 < n && buf[i] == '[' && buf[i + 1] == 0 &&
                 buf[i + 2] == '0' && buf[i + 3] == 0 &&
                 (buf[i + 4] == '0' || buf[i + 4] == '1') && buf[i + 5] == 0 &&
                 buf[i + 6] == ':' && buf[i + 7] == 0) {
            size_t start = (i >= 2 && buf[i - 2] == 0xFF && buf[i - 1] == 0xFE) ? (i - 2) : i;
            size_t end = i;
            while (end + 1 < n && !(buf[end] == 0 && buf[end + 1] == 0)) end += 2;
            if (end - start >= 32) {
                char* txt = raw_bytes_to_utf8(buf + start, end - start);
                if (txt) {
                    Lyrics* l = parse_lyrics_utf8(txt, label);
                    free(txt);
                    if (l && !l->unsynced && l->n >= 2) return l;
                    if (l) lyrics_free(l);
                }
            }
            i = end;
        }
    }
    return NULL;
}

static Lyrics* fallback_scan_embedded_lrc(const char* path) {
    FILE* f = plat_fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsz <= 32) { fclose(f); return NULL; }

    size_t head_n = (size_t)(fsz < 512 * 1024 ? fsz : 512 * 1024);
    unsigned char* buf = (unsigned char*)malloc(head_n);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, head_n, f);
    Lyrics* l = scan_buf_for_lrc(buf, got, "内嵌二进制特征扫描");
    free(buf);
    if (l) { fclose(f); return l; }

    if (fsz > 512 * 1024) {
        size_t tail_n = 256 * 1024;
        if ((long)tail_n > fsz) tail_n = (size_t)fsz;
        unsigned char* tbuf = (unsigned char*)malloc(tail_n);
        if (tbuf) {
            if (fseek(f, fsz - (long)tail_n, SEEK_SET) == 0) {
                size_t tgot = fread(tbuf, 1, tail_n, f);
                l = scan_buf_for_lrc(tbuf, tgot, "尾部内嵌特征扫描");
            }
            free(tbuf);
        }
    }
    fclose(f);
    return l;
}

// ---------------- MP4 / M4A ©lyr 内嵌歌词 ----------------
static int mp4_find_atom(FILE* f, long start, long end, const char tag[4], long* out_dstart, long* out_dend) {
    long pos = start;
    unsigned char hdr[16];
    while (pos + 8 <= end) {
        if (fseek(f, pos, SEEK_SET) != 0) break;
        if (fread(hdr, 1, 8, f) != 8) break;
        uint32_t sz32 = lrc_be32(hdr);
        long hdr_len = 8;
        long sz = (long)sz32;
        if (sz32 == 1) {
            if (pos + 16 > end || fread(hdr + 8, 1, 8, f) != 8) break;
            uint64_t sz64 = ((uint64_t)lrc_be32(hdr + 8) << 32) | lrc_be32(hdr + 12);
            if (sz64 < 16 || sz64 > 0x7FFFFFFFULL) break;
            sz = (long)sz64;
            hdr_len = 16;
        } else if (sz32 == 0) {
            sz = end - pos;
        }
        if (sz < hdr_len || pos + sz > end) break;
        if (memcmp(hdr + 4, tag, 4) == 0) {
            *out_dstart = pos + hdr_len;
            *out_dend = pos + sz;
            return 1;
        }
        pos += sz;
    }
    return 0;
}

static Lyrics* extract_mp4_lyrics(const char* path) {
    FILE* f = plat_fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    if (fsz < 16) { fclose(f); return NULL; }
    long s0, e0, s1, e1;
    if (!mp4_find_atom(f, 0, fsz, "moov", &s0, &e0)) { fclose(f); return NULL; }
    if (!mp4_find_atom(f, s0, e0, "udta", &s1, &e1)) { fclose(f); return NULL; }
    if (!mp4_find_atom(f, s1, e1, "meta", &s0, &e0) || s0 + 4 > e0) { fclose(f); return NULL; }
    if (!mp4_find_atom(f, s0 + 4, e0, "ilst", &s1, &e1)) { fclose(f); return NULL; }
    const char lyr_tag[4] = { (char)0xA9, 'l', 'y', 'r' };
    if (!mp4_find_atom(f, s1, e1, lyr_tag, &s0, &e0)) { fclose(f); return NULL; }
    if (!mp4_find_atom(f, s0, e0, "data", &s1, &e1) || s1 + 8 >= e1) { fclose(f); return NULL; }
    long pay_off = s1 + 8;
    long pay_len = e1 - pay_off;
    if (pay_len <= 0 || pay_len > 512 * 1024) { fclose(f); return NULL; }
    char* buf = (char*)malloc((size_t)pay_len + 1);
    if (!buf) { fclose(f); return NULL; }
    fseek(f, pay_off, SEEK_SET);
    size_t got = fread(buf, 1, (size_t)pay_len, f);
    fclose(f);
    buf[got] = 0;
    Lyrics* l = parse_lyrics_utf8(buf, "M4A 内嵌 ©lyr");
    free(buf);
    return l;
}

// ---------------- 缓存路径 + 网易云 163 key AES-128-ECB 解密 + 在线匹配 ----------------
static char g_lrc_cache_dir[512] = {0};

void lyrics_set_cache_dir(const char* dir) {
    if (dir && *dir) snprintf(g_lrc_cache_dir, sizeof(g_lrc_cache_dir), "%s", dir);
}

static uint64_t lrc_fnv1a64(const char* s) {
    uint64_t h = 14695981039346656037ULL;
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    return h;
}

static int build_cache_lrc_path(const char* media_path, char* out, size_t cap) {
    if (!g_lrc_cache_dir[0] || !media_path || !*media_path) return 0;
    uint64_t h = lrc_fnv1a64(media_path);
    snprintf(out, cap, "%s/%08x%08x.lrc", g_lrc_cache_dir, (unsigned)(h >> 32), (unsigned)(h & 0xFFFFFFFFu));
    return 1;
}

static Lyrics* load_cached_online_lrc(const char* media_path) {
    char cp[600];
    if (!build_cache_lrc_path(media_path, cp, sizeof(cp))) return NULL;
    return load_lrc_file_path(cp, "在线歌词缓存");
}

#ifndef __SWITCH__
static const unsigned char aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const unsigned char aes_rsbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

static inline unsigned char aes_xtime(unsigned char x) {
    return (unsigned char)((x << 1) ^ (((x >> 7) & 1) * 0x1b));
}

static inline unsigned char aes_mul(unsigned char x, unsigned char y) {
    unsigned char r = 0;
    for (int i = 0; i < 8; i++) {
        if (y & 1) r ^= x;
        x = aes_xtime(x);
        y >>= 1;
    }
    return r;
}

static void aes128_expand_key(const unsigned char key[16], unsigned char rk[176]) {
    static const unsigned char rcon[11] = { 0x8d,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36 };
    memcpy(rk, key, 16);
    for (int i = 4; i < 44; i++) {
        unsigned char t[4];
        memcpy(t, rk + (i - 1) * 4, 4);
        if (i % 4 == 0) {
            unsigned char tmp = t[0];
            t[0] = aes_sbox[t[1]] ^ rcon[i / 4];
            t[1] = aes_sbox[t[2]];
            t[2] = aes_sbox[t[3]];
            t[3] = aes_sbox[tmp];
        }
        for (int j = 0; j < 4; j++) rk[i * 4 + j] = rk[(i - 4) * 4 + j] ^ t[j];
    }
}

static void aes128_decrypt_block(const unsigned char rk[176], unsigned char s[16]) {
    for (int i = 0; i < 16; i++) s[i] ^= rk[160 + i];
    for (int r = 9; r >= 0; r--) {
        // InvShiftRows
        unsigned char t = s[13]; s[13] = s[9]; s[9] = s[5]; s[5] = s[1]; s[1] = t;
        t = s[2]; s[2] = s[10]; s[10] = t; t = s[6]; s[6] = s[14]; s[14] = t;
        t = s[3]; s[3] = s[7]; s[7] = s[11]; s[11] = s[15]; s[15] = t;
        // InvSubBytes
        for (int i = 0; i < 16; i++) s[i] = aes_rsbox[s[i]];
        // AddRoundKey
        for (int i = 0; i < 16; i++) s[i] ^= rk[r * 16 + i];
        // InvMixColumns (skip round 0)
        if (r > 0) {
            for (int c = 0; c < 4; c++) {
                unsigned char a0 = s[c*4+0], a1 = s[c*4+1], a2 = s[c*4+2], a3 = s[c*4+3];
                s[c*4+0] = aes_mul(a0,0x0e) ^ aes_mul(a1,0x0b) ^ aes_mul(a2,0x0d) ^ aes_mul(a3,0x09);
                s[c*4+1] = aes_mul(a0,0x09) ^ aes_mul(a1,0x0e) ^ aes_mul(a2,0x0b) ^ aes_mul(a3,0x0d);
                s[c*4+2] = aes_mul(a0,0x0d) ^ aes_mul(a1,0x09) ^ aes_mul(a2,0x0e) ^ aes_mul(a3,0x0b);
                s[c*4+3] = aes_mul(a0,0x0b) ^ aes_mul(a1,0x0d) ^ aes_mul(a2,0x09) ^ aes_mul(a3,0x0e);
            }
        }
    }
}

static int b64_val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static size_t b64_decode(const char* in, size_t inlen, unsigned char* out, size_t maxout) {
    size_t i = 0, o = 0;
    while (i + 3 < inlen) {
        if (in[i] == '=' || b64_val((unsigned char)in[i]) < 0) break;
        int v0 = b64_val((unsigned char)in[i]);
        int v1 = b64_val((unsigned char)in[i + 1]);
        int v2 = (in[i + 2] == '=') ? -2 : b64_val((unsigned char)in[i + 2]);
        int v3 = (in[i + 3] == '=') ? -2 : b64_val((unsigned char)in[i + 3]);
        if (v0 < 0 || v1 < 0 || v2 == -1 || v3 == -1) break;
        if (o >= maxout) break;
        out[o++] = (unsigned char)((v0 << 2) | (v1 >> 4));
        if (v2 >= 0 && o < maxout) out[o++] = (unsigned char)(((v1 & 0x0F) << 4) | (v2 >> 2));
        if (v3 >= 0 && o < maxout) out[o++] = (unsigned char)(((v2 & 0x03) << 6) | v3);
        i += 4;
        if (v2 == -2 || v3 == -2) break;
    }
    return o;
}

static int decode_163_payload(const unsigned char* b64, size_t b64_len, char* id_out, size_t id_cap) {
    if (b64_len < 16) return 0;
    size_t clean_len = 0;
    while (clean_len < b64_len) {
        unsigned char ch = b64[clean_len];
        if (ch == '=') {
            clean_len++;
            if (clean_len < b64_len && b64[clean_len] == '=') clean_len++;
            break;
        }
        if (b64_val(ch) < 0) break;
        clean_len++;
    }
    clean_len &= ~(size_t)3;
    if (clean_len < 16 || clean_len > 8192) return 0;
    unsigned char dec[6144];
    size_t dlen = b64_decode((const char*)b64, clean_len, dec, sizeof(dec) - 1);
    if (dlen < 16 || (dlen % 16) != 0) return 0;

    static const unsigned char key163[16] = {
        '#','1','4','l','j','k','_','!','\\',']','&','0','U','<','\'','('
    };
    unsigned char rk[176];
    aes128_expand_key(key163, rk);
    for (size_t off = 0; off < dlen; off += 16) aes128_decrypt_block(rk, dec + off);
    unsigned char pad = dec[dlen - 1];
    if (pad >= 1 && pad <= 16) dlen -= pad;
    dec[dlen] = 0;
    const char* mid = strstr((const char*)dec, "\"musicId\":");
    if (!mid) return 0;
    mid += 10;
    while (*mid == ' ' || *mid == '\t' || *mid == '"') mid++;
    size_t k = 0;
    while (isdigit((unsigned char)mid[k]) && k + 1 < id_cap) {
        id_out[k] = mid[k];
        k++;
    }
    id_out[k] = 0;
    return k > 0;
}

static int extract_163_music_id(const char* path, char* id_out, size_t id_cap) {
    id_out[0] = 0;
    FILE* f = plat_fopen(path, "rb");
    if (!f) return 0;
    unsigned char h10[10];
    if (fread(h10, 1, 10, f) == 10 && memcmp(h10, "ID3", 3) == 0 && (h10[3] == 3 || h10[3] == 4)) {
        uint32_t tsz = lrc_syncsafe32(h10 + 6);
        if (tsz > 0 && tsz <= 16u * 1024u * 1024u) {
            unsigned char* tag = (unsigned char*)malloc(tsz);
            if (tag) {
                size_t got = fread(tag, 1, tsz, f);
                size_t pos = 0;
                int ver = h10[3];
                if ((h10[5] & 0x40) && got >= 4) {
                    uint32_t ex = (ver == 4) ? lrc_syncsafe32(tag) : (lrc_be32(tag) + 4);
                    pos += ex;
                }
                while (pos + 10 <= got) {
                    if (tag[pos] == 0) break;
                    uint32_t fsz = (ver == 4) ? lrc_syncsafe32(tag + pos + 4) : lrc_be32(tag + pos + 4);
                    if (fsz == 0 || pos + 10 + fsz > got) break;
                    const unsigned char* body = tag + pos + 10;
                    if (memcmp(tag + pos, "COMM", 4) == 0 || memcmp(tag + pos, "TXXX", 4) == 0) {
                        const char* sig = "163 key(Don't modify):";
                        size_t slen = strlen(sig);
                        for (size_t j = 0; j + slen < fsz; j++) {
                            if (memcmp(body + j, sig, slen) == 0) {
                                size_t rem = fsz - (j + slen);
                                if (decode_163_payload(body + j + slen, rem, id_out, id_cap)) {
                                    free(tag);
                                    fclose(f);
                                    return 1;
                                }
                            }
                        }
                    }
                    pos += 10 + fsz;
                }
                free(tag);
            }
        }
    }
    // 兜底：FLAC/OGG 等前 512KB 内扫描
    fseek(f, 0, SEEK_SET);
    size_t scan_n = 512 * 1024;
    unsigned char* buf = (unsigned char*)malloc(scan_n);
    int found = 0;
    if (buf) {
        size_t got = fread(buf, 1, scan_n, f);
        const char* sig = "163 key(Don't modify):";
        size_t slen = strlen(sig);
        for (size_t j = 0; j + slen + 16 < got; j++) {
            if (memcmp(buf + j, sig, slen) == 0) {
                size_t rem = got - (j + slen);
                if (decode_163_payload(buf + j + slen, rem, id_out, id_cap)) {
                    found = 1;
                    break;
                }
            }
        }
        free(buf);
    }
    fclose(f);
    return found;
}

static void url_encode(const char* s, char* out, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char* p = (const unsigned char*)s; *p && o + 4 < cap; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[(c >> 4) & 0xF];
            out[o++] = hex[c & 0xF];
        }
    }
    out[o] = 0;
}

static int http_get_utf8(const char* host, const char* path_query, char** out_buf, size_t* out_len) {
    *out_buf = NULL;
    *out_len = 0;
#ifdef _WIN32
    typedef void* HINTERNET;
    typedef HINTERNET (WINAPI *PFN_WinHttpOpen)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
    typedef HINTERNET (WINAPI *PFN_WinHttpConnect)(HINTERNET, LPCWSTR, WORD, DWORD);
    typedef HINTERNET (WINAPI *PFN_WinHttpOpenRequest)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR*, DWORD);
    typedef BOOL (WINAPI *PFN_WinHttpSendRequest)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);
    typedef BOOL (WINAPI *PFN_WinHttpReceiveResponse)(HINTERNET, LPVOID);
    typedef BOOL (WINAPI *PFN_WinHttpQueryDataAvailable)(HINTERNET, LPDWORD);
    typedef BOOL (WINAPI *PFN_WinHttpReadData)(HINTERNET, LPVOID, DWORD, LPDWORD);
    typedef BOOL (WINAPI *PFN_WinHttpCloseHandle)(HINTERNET);
    typedef BOOL (WINAPI *PFN_WinHttpSetTimeouts)(HINTERNET, int, int, int, int);

    HMODULE hMod = LoadLibraryW(L"winhttp.dll");
    if (!hMod) return 0;
    PFN_WinHttpOpen pOpen = (PFN_WinHttpOpen)(void*)GetProcAddress(hMod, "WinHttpOpen");
    PFN_WinHttpConnect pConn = (PFN_WinHttpConnect)(void*)GetProcAddress(hMod, "WinHttpConnect");
    PFN_WinHttpOpenRequest pReq = (PFN_WinHttpOpenRequest)(void*)GetProcAddress(hMod, "WinHttpOpenRequest");
    PFN_WinHttpSendRequest pSend = (PFN_WinHttpSendRequest)(void*)GetProcAddress(hMod, "WinHttpSendRequest");
    PFN_WinHttpReceiveResponse pRecv = (PFN_WinHttpReceiveResponse)(void*)GetProcAddress(hMod, "WinHttpReceiveResponse");
    PFN_WinHttpQueryDataAvailable pAvail = (PFN_WinHttpQueryDataAvailable)(void*)GetProcAddress(hMod, "WinHttpQueryDataAvailable");
    PFN_WinHttpReadData pRead = (PFN_WinHttpReadData)(void*)GetProcAddress(hMod, "WinHttpReadData");
    PFN_WinHttpCloseHandle pClose = (PFN_WinHttpCloseHandle)(void*)GetProcAddress(hMod, "WinHttpCloseHandle");
    PFN_WinHttpSetTimeouts pTimeouts = (PFN_WinHttpSetTimeouts)(void*)GetProcAddress(hMod, "WinHttpSetTimeouts");
    if (!pOpen || !pConn || !pReq || !pSend || !pRecv || !pAvail || !pRead || !pClose) return 0;

    wchar_t whost[256], wpath[2048];
    plat_utf8_to_wide(host, whost, 256);
    plat_utf8_to_wide(path_query, wpath, 2048);

    HINTERNET hSes = pOpen(L"Mozilla/5.0 Sirene/1.2.6", 0, NULL, NULL, 0);
    if (!hSes) return 0;
    if (pTimeouts) pTimeouts(hSes, 3000, 4000, 4000, 5000);
    HINTERNET hCon = pConn(hSes, whost, 443, 0);
    HINTERNET hReq = hCon ? pReq(hCon, L"GET", wpath, NULL, L"https://music.163.com/", NULL, 0x00800000u) : NULL;
    int ok = 0;
    if (hReq && pSend(hReq, NULL, 0, NULL, 0, 0, 0) && pRecv(hReq, NULL)) {
        size_t cap = 4096, len = 0;
        char* buf = (char*)malloc(cap);
        if (buf) {
            DWORD avail = 0;
            while (pAvail(hReq, &avail) && avail > 0) {
                if (len + avail + 1 > 512 * 1024) break;
                if (len + avail + 1 > cap) {
                    cap = (len + avail + 1) * 2;
                    char* nb = (char*)realloc(buf, cap);
                    if (!nb) break;
                    buf = nb;
                }
                DWORD rd = 0;
                if (!pRead(hReq, buf + len, avail, &rd) || rd == 0) break;
                len += rd;
            }
            buf[len] = 0;
            if (len > 0) { *out_buf = buf; *out_len = len; ok = 1; }
            else free(buf);
        }
    }
    if (hReq) pClose(hReq);
    if (hCon) pClose(hCon);
    pClose(hSes);
    return ok;
#else
    char url[2304];
    snprintf(url, sizeof(url), "https://%s%s", host, path_query);
    int pfd[2];
    if (pipe(pfd) != 0) return 0;
    pid_t pid = fork();
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[1]);
        char* argv[] = {
            "curl", "-fsSL", "--max-time", "6",
            "-A", "Mozilla/5.0", "-H", "Referer: https://music.163.com/",
            url, NULL
        };
        execvp("curl", argv);
        _exit(127);
    }
    close(pfd[1]);
    if (pid < 0) { close(pfd[0]); return 0; }
    size_t cap = 4096, len = 0;
    char* buf = (char*)malloc(cap);
    if (buf) {
        for (;;) {
            if (len + 1024 + 1 > 512 * 1024) break;
            if (len + 1024 + 1 > cap) {
                cap *= 2;
                char* nb = (char*)realloc(buf, cap);
                if (!nb) break;
                buf = nb;
            }
            ssize_t r = read(pfd[0], buf + len, 1024);
            if (r <= 0) break;
            len += (size_t)r;
        }
        buf[len] = 0;
    }
    close(pfd[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    if (buf && len > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0) {
        *out_buf = buf;
        *out_len = len;
        return 1;
    }
    free(buf);
    return 0;
#endif
}

static char* json_extract_str_after(const char* start, const char* key) {
    if (!start || !key) return NULL;
    const char* k = strstr(start, key);
    if (!k) return NULL;
    const char* p = strchr(k + strlen(key), ':');
    if (!p) return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return NULL;
    p++;
    size_t cap = 4096, len = 0;
    char* out = (char*)malloc(cap);
    if (!out) return NULL;
    while (*p && *p != '"') {
        if (len + 8 >= cap) {
            cap *= 2;
            char* nb = (char*)realloc(out, cap);
            if (!nb) { free(out); return NULL; }
            out = nb;
        }
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'n') { out[len++] = '\n'; p++; }
            else if (*p == 'r') { p++; }
            else if (*p == 't') { out[len++] = '\t'; p++; }
            else if (*p == '"' || *p == '\\' || *p == '/') { out[len++] = *p++; }
            else if (*p == 'u' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2]) &&
                     isxdigit((unsigned char)p[3]) && isxdigit((unsigned char)p[4])) {
                uint32_t cp = 0;
                for (int i = 1; i <= 4; i++) {
                    char c = p[i];
                    uint32_t d = (c >= '0' && c <= '9') ? (uint32_t)(c - '0')
                               : (c >= 'a' && c <= 'f') ? (uint32_t)(c - 'a' + 10)
                               : (uint32_t)(c - 'A' + 10);
                    cp = (cp << 4) | d;
                }
                p += 5;
                if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u' &&
                    isxdigit((unsigned char)p[2]) && isxdigit((unsigned char)p[3]) &&
                    isxdigit((unsigned char)p[4]) && isxdigit((unsigned char)p[5])) {
                    uint32_t lo = 0;
                    for (int i = 2; i <= 5; i++) {
                        char c = p[i];
                        uint32_t d = (c >= '0' && c <= '9') ? (uint32_t)(c - '0')
                                   : (c >= 'a' && c <= 'f') ? (uint32_t)(c - 'a' + 10)
                                   : (uint32_t)(c - 'A' + 10);
                        lo = (lo << 4) | d;
                    }
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000u + (((cp - 0xD800u) << 10) | (lo - 0xDC00u));
                        p += 6;
                    }
                }
                if (cp < 0x80u) {
                    out[len++] = (char)cp;
                } else if (cp < 0x800u) {
                    out[len++] = (char)(0xC0u | (cp >> 6));
                    out[len++] = (char)(0x80u | (cp & 0x3Fu));
                } else if (cp < 0x10000u) {
                    out[len++] = (char)(0xE0u | (cp >> 12));
                    out[len++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
                    out[len++] = (char)(0x80u | (cp & 0x3Fu));
                } else {
                    out[len++] = (char)(0xF0u | (cp >> 18));
                    out[len++] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
                    out[len++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
                    out[len++] = (char)(0x80u | (cp & 0x3Fu));
                }
            } else {
                out[len++] = *p++;
            }
        } else {
            out[len++] = *p++;
        }
    }
    out[len] = 0;
    return out;
}

static char* fetch_netease_by_id(const char* music_id) {
    char pq[256], *json = NULL;
    size_t jlen = 0;
    snprintf(pq, sizeof(pq), "/api/song/lyric?id=%s&lv=1&tv=-1", music_id);
    if (!http_get_utf8("music.163.com", pq, &json, &jlen)) return NULL;
    const char* lrc_sec = strstr(json, "\"lrc\":");
    char* raw_lrc = json_extract_str_after(lrc_sec ? lrc_sec : json, "\"lyric\"");
    const char* tl_sec = strstr(json, "\"tlyric\":");
    char* raw_tl = tl_sec ? json_extract_str_after(tl_sec, "\"lyric\"") : NULL;
    free(json);
    if (raw_lrc && raw_lrc[0] && raw_tl && raw_tl[0]) {
        size_t n1 = strlen(raw_lrc), n2 = strlen(raw_tl);
        char* both = (char*)malloc(n1 + n2 + 4);
        if (both) {
            snprintf(both, n1 + n2 + 4, "%s\n%s", raw_lrc, raw_tl);
            free(raw_lrc); free(raw_tl);
            return both;
        }
    }
    if (raw_lrc && raw_lrc[0]) { free(raw_tl); return raw_lrc; }
    free(raw_lrc);
    if (raw_tl && raw_tl[0]) return raw_tl;
    free(raw_tl);
    return NULL;
}

static void clean_search_term(const char* in, char* out, size_t cap) {
    if (!in) { out[0] = 0; return; }
    snprintf(out, cap, "%s", in);
    // 去掉扩展名或多余前后空格
    char* p = out;
    while (*p == ' ') p++;
    if (p != out) memmove(out, p, strlen(p) + 1);
    size_t n = strlen(out);
    while (n > 0 && out[n - 1] == ' ') out[--n] = 0;
}

static char* fetch_online_lrc_text(const char* path, const char* title, const char* artist, double dur, const char** out_src) {
    char mid[32] = {0};
    if (extract_163_music_id(path, mid, sizeof(mid))) {
        char* raw = fetch_netease_by_id(mid);
        if (raw) { *out_src = "网易云歌词"; return raw; }
    }

    char ct[256], ca[256], et[768], ea[768], pq[2048];
    clean_search_term(title, ct, sizeof(ct));
    clean_search_term(artist, ca, sizeof(ca));
    if (!ct[0]) return NULL;
    url_encode(ct, et, sizeof(et));
    url_encode(ca, ea, sizeof(ea));

    char* json = NULL;
    size_t jlen = 0;
    if (ca[0]) {
        if (dur > 5.0)
            snprintf(pq, sizeof(pq), "/api/get?artist_name=%s&track_name=%s&duration=%d", ea, et, (int)(dur + 0.5));
        else
            snprintf(pq, sizeof(pq), "/api/get?artist_name=%s&track_name=%s", ea, et);
        if (http_get_utf8("lrclib.net", pq, &json, &jlen)) {
            char* raw = json_extract_str_after(json, "\"syncedLyrics\"");
            if (!raw || !raw[0]) { free(raw); raw = json_extract_str_after(json, "\"plainLyrics\""); }
            free(json);
            if (raw && raw[0]) { *out_src = "LRCLIB 在线"; return raw; }
            free(raw);
        }
    }

    char q[520], eq[1560];
    if (ca[0]) snprintf(q, sizeof(q), "%s %s", ca, ct);
    else snprintf(q, sizeof(q), "%s", ct);
    url_encode(q, eq, sizeof(eq));

    snprintf(pq, sizeof(pq), "/api/search?q=%s", eq);
    if (http_get_utf8("lrclib.net", pq, &json, &jlen)) {
        char* raw = json_extract_str_after(json, "\"syncedLyrics\"");
        if (!raw || !raw[0]) { free(raw); raw = json_extract_str_after(json, "\"plainLyrics\""); }
        free(json);
        if (raw && raw[0]) { *out_src = "LRCLIB 在线"; return raw; }
        free(raw);
    }

    snprintf(pq, sizeof(pq), "/api/search/get/web?s=%s&type=1&limit=3", eq);
    if (http_get_utf8("music.163.com", pq, &json, &jlen)) {
        const char* songs = strstr(json, "\"songs\":");
        const char* idp = songs ? strstr(songs, "\"id\":") : NULL;
        if (idp) {
            idp += 5;
            while (*idp == ' ') idp++;
            size_t k = 0;
            while (isdigit((unsigned char)idp[k]) && k + 1 < sizeof(mid)) { mid[k] = idp[k]; k++; }
            mid[k] = 0;
            if (k > 0) {
                char* raw = fetch_netease_by_id(mid);
                if (raw) { free(json); *out_src = "网易云搜索"; return raw; }
            }
        }
        free(json);
    }
    return NULL;
}

static PlatMutex g_onl_mtx;
static PlatThread g_onl_th;
static int g_onl_inited = 0;
static int g_onl_req_seq = 0;
static int g_onl_has_job = 0;
static int g_onl_done_id = 0;
static char g_onl_path[600];
static char g_onl_title[256];
static char g_onl_artist[256];
static double g_onl_dur = 0;
static int g_onl_ready_id = 0;
static Lyrics* g_onl_ready_lyr = NULL;

static void online_worker_fn(void* arg) {
    (void)arg;
    for (;;) {
        char path[600], title[256], artist[256];
        double dur = 0;
        int my_id = 0;

        plat_mutex_lock(&g_onl_mtx);
        if (g_onl_has_job) {
            my_id = g_onl_req_seq;
            snprintf(path, sizeof(path), "%s", g_onl_path);
            snprintf(title, sizeof(title), "%s", g_onl_title);
            snprintf(artist, sizeof(artist), "%s", g_onl_artist);
            dur = g_onl_dur;
            g_onl_has_job = 0;
        }
        plat_mutex_unlock(&g_onl_mtx);

        if (!my_id) {
            plat_sleep_ms(30);
            continue;
        }

        const char* src = "在线歌词";
        char* raw = fetch_online_lrc_text(path, title, artist, dur, &src);
        Lyrics* parsed = NULL;
        if (raw) {
            parsed = parse_lyrics_utf8(raw, src);
            if (parsed && parsed->n > 0 && g_lrc_cache_dir[0]) {
                plat_mkdir_p(g_lrc_cache_dir);
                char cp[600];
                if (build_cache_lrc_path(path, cp, sizeof(cp))) {
                    FILE* cf = plat_fopen(cp, "wb");
                    if (cf) {
                        fwrite(raw, 1, strlen(raw), cf);
                        fclose(cf);
                    }
                }
            }
            free(raw);
        }

        plat_mutex_lock(&g_onl_mtx);
        g_onl_done_id = my_id;
        if (my_id == g_onl_req_seq) {
            if (g_onl_ready_lyr) lyrics_free(g_onl_ready_lyr);
            g_onl_ready_lyr = parsed;
            g_onl_ready_id = my_id;
        } else if (parsed) {
            lyrics_free(parsed);
        }
        plat_mutex_unlock(&g_onl_mtx);
    }
}

int lyrics_start_online(const char* media_path, const char* title, const char* artist, double duration_sec) {
    if (!media_path || !*media_path) return 0;
    if (!g_onl_inited) {
        plat_mutex_init(&g_onl_mtx);
        if (plat_thread_create(&g_onl_th, online_worker_fn, NULL) != 0) return 0;
        g_onl_inited = 1;
    }
    plat_mutex_lock(&g_onl_mtx);
    int id = ++g_onl_req_seq;
    snprintf(g_onl_path, sizeof(g_onl_path), "%s", media_path);
    snprintf(g_onl_title, sizeof(g_onl_title), "%s", title ? title : "");
    snprintf(g_onl_artist, sizeof(g_onl_artist), "%s", artist ? artist : "");
    g_onl_dur = duration_sec;
    g_onl_has_job = 1;
    if (g_onl_ready_lyr) { lyrics_free(g_onl_ready_lyr); g_onl_ready_lyr = NULL; }
    g_onl_ready_id = 0;
    plat_mutex_unlock(&g_onl_mtx);
    return id;
}

int lyrics_poll_online(int req_id, Lyrics** out) {
    if (!g_onl_inited || req_id <= 0 || !out) return 0;
    *out = NULL;
    int got = 0;
    plat_mutex_lock(&g_onl_mtx);
    if (g_onl_ready_id == req_id) {
        *out = g_onl_ready_lyr;
        g_onl_ready_lyr = NULL;
        g_onl_ready_id = 0;
        got = 1;
    }
    plat_mutex_unlock(&g_onl_mtx);
    return got;
}

int lyrics_online_busy(int req_id) {
    if (!g_onl_inited || req_id <= 0) return 0;
    plat_mutex_lock(&g_onl_mtx);
    int busy = (req_id == g_onl_req_seq && g_onl_done_id != req_id);
    plat_mutex_unlock(&g_onl_mtx);
    return busy;
}
#else
int  lyrics_start_online(const char* p, const char* t, const char* a, double d) { (void)p;(void)t;(void)a;(void)d; return 0; }
int  lyrics_poll_online(int req_id, Lyrics** out) { (void)req_id; if (out) *out = NULL; return 0; }
int  lyrics_online_busy(int req_id) { (void)req_id; return 0; }
#endif

Lyrics* lyrics_load_ex(const char* media_path, const char* title, const char* artist) {
    if (!media_path || !*media_path) {
        set_diag("路径为空");
        return NULL;
    }

    char dir[560] = {0};
    char parent_lyrics[600] = {0};
    char parent_Lyrics[600] = {0};
    char parent_lrc[600] = {0};
    char stem[256] = {0};
    char stem_clean[256] = {0};

    const char* slash = path_last_sep(media_path);
    if (slash) {
        size_t dlen = (size_t)(slash - media_path);
        if (dlen >= sizeof(dir)) dlen = sizeof(dir) - 1;
        memcpy(dir, media_path, dlen);
        dir[dlen] = 0;
        while (dlen > 0 && (dir[dlen - 1] == '/' || dir[dlen - 1] == '\\')) dir[--dlen] = 0;
        if (!strcmp(dir, "sdmc:")) snprintf(dir, sizeof(dir), "sdmc:/");
        else if (dir[0] == 0) snprintf(dir, sizeof(dir), (media_path[0] == '/') ? "/" : ".");
        const char* pslash = path_last_sep(dir);
        if (pslash && pslash > dir + 2) {
            size_t plen = (size_t)(pslash - dir);
            snprintf(parent_lyrics, sizeof(parent_lyrics), "%.*s/lyrics", (int)plen, dir);
            snprintf(parent_Lyrics, sizeof(parent_Lyrics), "%.*s/Lyrics", (int)plen, dir);
            snprintf(parent_lrc,    sizeof(parent_lrc),    "%.*s/lrc",    (int)plen, dir);
        }
    } else {
        snprintf(dir, sizeof(dir), ".");
    }

    basename_no_ext(media_path, stem, sizeof(stem));
    strip_track_prefix(stem, stem_clean, sizeof(stem_clean));

    // 构建候选文件名列表：原文件名、去曲号、标题、歌手 - 标题（含无空格写法）
    char stems[7][300];
    int nstems = 0;
    snprintf(stems[nstems++], sizeof(stems[0]), "%s", stem);
    if (stem_clean[0] && strcasecmp(stem_clean, stem) != 0)
        snprintf(stems[nstems++], sizeof(stems[0]), "%s", stem_clean);
    if (title && title[0] && strcasecmp(title, stem) != 0 && strcasecmp(title, stem_clean) != 0)
        snprintf(stems[nstems++], sizeof(stems[0]), "%s", title);
    if (title && title[0] && artist && artist[0]) {
        snprintf(stems[nstems++], sizeof(stems[0]), "%s - %s", artist, title);
        snprintf(stems[nstems++], sizeof(stems[0]), "%s - %s", title, artist);
        snprintf(stems[nstems++], sizeof(stems[0]), "%s-%s", artist, title);
        snprintf(stems[nstems++], sizeof(stems[0]), "%s-%s", title, artist);
    }

    char dir_lyrics[620], dir_Lyrics[620], dir_lrc[620];
    join_lrc_path(dir_lyrics, sizeof(dir_lyrics), dir, "lyrics");
    join_lrc_path(dir_Lyrics, sizeof(dir_Lyrics), dir, "Lyrics");
    join_lrc_path(dir_lrc,    sizeof(dir_lrc),    dir, "lrc");

    const char* search_dirs[] = {
        dir, dir_lyrics, dir_Lyrics, dir_lrc,
        parent_lyrics, parent_Lyrics, parent_lrc,
        "lyrics", "Lyrics", "lrc",
        "sdmc:/music/lyrics", "sdmc:/Music/lyrics", "sdmc:/lyrics",
        "sdmc:/Sirene/lyrics", "sdmc:/Siren/lyrics"
    };
    const char* dir_labels[] = {
        "同目录 .lrc", "同级 lyrics/ .lrc", "同级 Lyrics/ .lrc", "同级 lrc/ .lrc",
        "上级 lyrics/ .lrc", "上级 Lyrics/ .lrc", "上级 lrc/ .lrc",
        "当前 lyrics/ .lrc", "当前 Lyrics/ .lrc", "当前 lrc/ .lrc",
        "music/lyrics .lrc", "Music/lyrics .lrc", "sdmc:/lyrics .lrc",
        "Sirene/lyrics .lrc", "Siren/lyrics .lrc"
    };

    for (unsigned d = 0; d < sizeof(search_dirs) / sizeof(search_dirs[0]); d++) {
        if (!search_dirs[d] || !search_dirs[d][0]) continue;
        for (int s = 0; s < nstems; s++) {
            Lyrics* l = find_lrc_in_dir_ci(search_dirs[d], stems[s], dir_labels[d]);
            if (l) return l;
        }
    }

    // 同目录与 lyrics 子目录模糊匹配（如歌名包含匹配）
    const char* fz_title = (title && title[0]) ? title : stem_clean;
    Lyrics* fl = find_lrc_fuzzy_in_dir(dir, fz_title, artist, "同目录模糊匹配 .lrc");
    if (fl) return fl;
    fl = find_lrc_fuzzy_in_dir(dir_lyrics, fz_title, artist, "lyrics/模糊匹配 .lrc");
    if (fl) return fl;
    fl = find_lrc_fuzzy_in_dir(dir_Lyrics, fz_title, artist, "Lyrics/模糊匹配 .lrc");
    if (fl) return fl;
    fl = find_lrc_fuzzy_in_dir(parent_lyrics, fz_title, artist, "上级 lyrics/模糊匹配 .lrc");
    if (fl) return fl;

    // 音频内嵌歌词提取
    Lyrics* l = NULL;
    if (ci_ends_with(media_path, ".mp3")) l = extract_mp3_lyrics(media_path);
    else if (ci_ends_with(media_path, ".flac")) l = extract_flac_lyrics(media_path);
    else if (ci_ends_with(media_path, ".ogg") || ci_ends_with(media_path, ".oga")) l = extract_ogg_lyrics(media_path);
    else if (ci_ends_with(media_path, ".m4a") || ci_ends_with(media_path, ".mp4") || ci_ends_with(media_path, ".m4b") || ci_ends_with(media_path, ".alac"))
        l = extract_mp4_lyrics(media_path);
    else if (ci_ends_with(media_path, ".wav") || ci_ends_with(media_path, ".wave"))
        l = extract_wav_lyrics(media_path);
    if (l) return l;

    // 终极二进制扫描兜底
    l = fallback_scan_embedded_lrc(media_path);
    if (l) return l;

    // 命中本地在线歌词缓存
    l = load_cached_online_lrc(media_path);
    if (l) return l;

    set_diag("未检测到同名.lrc或音频内嵌歌词标签");
    return NULL;
}

Lyrics* lyrics_load_file(const char* lrc_path) {
    if (!lrc_path || !*lrc_path) return NULL;
    Lyrics* l = load_lrc_file_path(lrc_path, "拖入外部 .lrc");
    if (!l) set_diag("外部 .lrc 文件无法解析或为空");
    return l;
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
