#include "util.h"
#include "gbk_table.h"
#include <string.h>
#include <stdio.h>

static size_t put_utf8(char* out, size_t cap, size_t* pos, uint32_t cp) {
    if (cp <= 0x7F) {
        if (*pos + 1 > cap) return 0;
        out[(*pos)++] = (char)cp;
    } else if (cp <= 0x7FF) {
        if (*pos + 2 > cap) return 0;
        out[(*pos)++] = (char)(0xC0 | (cp >> 6));
        out[(*pos)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        if (*pos + 3 > cap) return 0;
        out[(*pos)++] = (char)(0xE0 | (cp >> 12));
        out[(*pos)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*pos)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        if (*pos + 4 > cap) return 0;
        out[(*pos)++] = (char)(0xF0 | (cp >> 18));
        out[(*pos)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[(*pos)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*pos)++] = (char)(0x80 | (cp & 0x3F));
    }
    return 1;
}

size_t my_utf16_to_utf8(const uint16_t* in, size_t in_units, char* out, size_t out_cap) {
    size_t pos = 0;
    size_t i = 0;
    if (in_units >= 2 && in[0] == 0xFEFF) i = 1; // skip LE BOM
    for (; i < in_units; i++) {
        uint16_t u = in[i];
        uint32_t cp;
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < in_units) {
            uint16_t lo = in[i+1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + (((uint32_t)(u - 0xD800) << 10) | (uint32_t)(lo - 0xDC00));
                i++;
            } else cp = 0xFFFD;
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
            cp = 0xFFFD;
        } else {
            cp = u;
        }
        if (!put_utf8(out, out_cap, &pos, cp)) break;
    }
    if (pos < out_cap) out[pos] = 0;
    return pos;
}

size_t latin1_to_utf8(const uint8_t* in, size_t n, char* out, size_t out_cap) {
    size_t pos = 0;
    for (size_t i = 0; i < n; i++) {
        if (!put_utf8(out, out_cap, &pos, in[i])) break;
        if (in[i] == 0) break;
    }
    if (pos < out_cap) out[pos] = 0;
    return pos;
}

int is_valid_utf8(const uint8_t* s, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        if (c == 0) break;
        if (c < 0x80) { i++; continue; }
        int len = 0;
        uint32_t cp = 0;
        if ((c & 0xE0) == 0xC0 && c >= 0xC2) { len = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07; }
        else return 0;
        if (i + (size_t)len > n) return 0;
        for (int k = 1; k < len; k++) {
            if ((s[i + k] & 0xC0) != 0x80) return 0;
            cp = (cp << 6) | (s[i + k] & 0x3F);
        }
        if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF))) return 0;
        if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
        i += (size_t)len;
    }
    return 1;
}

size_t gbk_to_utf8(const uint8_t* in, size_t n, char* out, size_t out_cap) {
    if (!out || out_cap == 0) return 0;
    size_t pos = 0;
    for (size_t i = 0; i < n; ) {
        uint8_t b0 = in[i];
        if (b0 == 0) break;
        if (b0 < 0x80) {
            if (!put_utf8(out, out_cap - 1, &pos, b0)) break;
            i++;
        } else if (b0 >= 0x81 && b0 <= 0xFE && i + 1 < n) {
            uint8_t b1 = in[i + 1];
            uint32_t cp = 0xFFFD;
            if (b1 >= 0x40 && b1 <= 0xFE && b1 != 0x7F) {
                cp = gbk_ucs2[(size_t)(b0 - 0x81) * 191 + (size_t)(b1 - 0x40)];
            }
            if (!put_utf8(out, out_cap - 1, &pos, cp)) break;
            i += 2;
        } else {
            if (!put_utf8(out, out_cap - 1, &pos, 0xFFFD)) break;
            i++;
        }
    }
    out[pos] = 0;
    return pos;
}

size_t text_to_utf8_auto(const uint8_t* in, size_t n, char* out, size_t out_cap) {
    if (!out || out_cap == 0) return 0;
    if (!in || n == 0) { out[0] = 0; return 0; }
    if (n >= 3 && in[0] == 0xEF && in[1] == 0xBB && in[2] == 0xBF) {
        in += 3; n -= 3;
    }
    const uint8_t* z = (const uint8_t*)memchr(in, 0, n);
    if (z) n = (size_t)(z - in);
    if (is_valid_utf8(in, n)) {
        size_t cp = n < out_cap - 1 ? n : out_cap - 1;
        while (cp > 0 && cp < n && (in[cp] & 0xC0u) == 0x80u) cp--;
        memcpy(out, in, cp);
        out[cp] = 0;
        return cp;
    }
    if (looks_like_utf8_text(in, n)) {
        return sanitize_utf8_text(in, n, out, out_cap);
    }
    return gbk_to_utf8(in, n, out, out_cap);
}

void utf8_copy_safe(char* dst, size_t cap, const char* src, size_t max_bytes) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = 0; return; }
    size_t slen = strlen(src);
    size_t lim = cap - 1;
    if (max_bytes > 0 && max_bytes < lim) lim = max_bytes;
    size_t n = (slen < lim) ? slen : lim;
    while (n > 0 && n < slen && (((unsigned char)src[n]) & 0xC0u) == 0x80u) n--;
    if (n > 0) memcpy(dst, src, n);
    dst[n] = 0;
}

int looks_like_utf8_text(const uint8_t* s, size_t n) {
    if (!s || n == 0) return 0;
    size_t i = 0, valid_mb = 0, bad = 0;
    while (i < n && s[i] != 0) {
        uint8_t c = s[i];
        if (c < 0x80u) { i++; continue; }
        int len = 0;
        uint32_t cp = 0;
        if ((c & 0xE0u) == 0xC0u && c >= 0xC2u) { len = 2; cp = c & 0x1Fu; }
        else if ((c & 0xF0u) == 0xE0u) { len = 3; cp = c & 0x0Fu; }
        else if ((c & 0xF8u) == 0xF0u && c <= 0xF4u) { len = 4; cp = c & 0x07u; }
        else { bad++; i++; continue; }
        if (i + (size_t)len > n) { bad++; break; }
        int ok = 1;
        for (int k = 1; k < len; k++) {
            if ((s[i + k] & 0xC0u) != 0x80u) { ok = 0; break; }
            cp = (cp << 6) | (s[i + k] & 0x3Fu);
        }
        if (!ok || (len == 3 && cp < 0x800u) || (len == 4 && (cp < 0x10000u || cp > 0x10FFFFu)) ||
            (cp >= 0xD800u && cp <= 0xDFFFu)) {
            bad++; i++;
        } else {
            valid_mb++; i += (size_t)len;
        }
    }
    return (valid_mb > 0 && bad * 8 <= valid_mb) ? 1 : 0;
}

size_t sanitize_utf8_text(const uint8_t* in, size_t n, char* out, size_t out_cap) {
    if (!out || out_cap == 0) return 0;
    size_t i = 0, pos = 0, lim = out_cap - 1;
    while (i < n && in[i] != 0 && pos < lim) {
        uint8_t c = in[i];
        if (c < 0x80u) {
            out[pos++] = (char)c;
            i++;
            continue;
        }
        int len = 0;
        uint32_t cp = 0;
        if ((c & 0xE0u) == 0xC0u && c >= 0xC2u) { len = 2; cp = c & 0x1Fu; }
        else if ((c & 0xF0u) == 0xE0u) { len = 3; cp = c & 0x0Fu; }
        else if ((c & 0xF8u) == 0xF0u && c <= 0xF4u) { len = 4; cp = c & 0x07u; }
        else { i++; continue; }
        if (i + (size_t)len > n) break;
        int ok = 1;
        for (int k = 1; k < len; k++) {
            if ((in[i + k] & 0xC0u) != 0x80u) { ok = 0; break; }
            cp = (cp << 6) | (in[i + k] & 0x3Fu);
        }
        if (!ok || (len == 3 && cp < 0x800u) || (len == 4 && (cp < 0x10000u || cp > 0x10FFFFu)) ||
            (cp >= 0xD800u && cp <= 0xDFFFu)) {
            i++;
            continue;
        }
        if (cp == 0x00A0u) {
            out[pos++] = ' ';
        } else if (cp == 0xFEFFu || (cp >= 0x200Bu && cp <= 0x200Fu)) {
            /* skip zero-width BOM/markers */
        } else {
            if (pos + (size_t)len > lim) break;
            for (int k = 0; k < len; k++) out[pos++] = (char)in[i + k];
        }
        i += (size_t)len;
    }
    out[pos] = 0;
    return pos;
}

int utf8_count(const char* s) {
    int n = 0;
    while (*s) {
        unsigned char c = (unsigned char)*s;
        if ((c & 0xC0) != 0x80) n++;
        s++;
    }
    return n;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

int ci_ends_with(const char* s, const char* suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    if (lf > ls) return 0;
    for (size_t i = 0; i < lf; i++)
        if (lower(s[ls - lf + i]) != lower(suf[i])) return 0;
    return 1;
}

const char* path_last_sep(const char* p) {
    if (!p) return NULL;
    const char* s1 = strrchr(p, '/');
    const char* s2 = strrchr(p, '\\');
    if (s1 && s2) return s1 > s2 ? s1 : s2;
    return s1 ? s1 : s2;
}

const char* path_basename(const char* p) {
    const char* slash = path_last_sep(p);
    if (slash) return slash + 1;
    return p;
}

void basename_no_ext(const char* path, char* out, size_t cap) {
    const char* b = path_basename(path);
    size_t n = strlen(b);
    const char* dot = strrchr(b, '.');
    if (dot && dot != b) n = (size_t)(dot - b);
    if (n >= cap) n = cap - 1;
    memcpy(out, b, n);
    out[n] = 0;
}

void fmt_time(double sec, char* out, size_t cap) {
    if (sec < 0) sec = 0;
    int total = (int)(sec + 0.5);
    int m = total / 60;
    int s = total % 60;
    int h = m / 60;
    if (h > 0) snprintf(out, cap, "%d:%02d:%02d", h, m % 60, s);
    else snprintf(out, cap, "%d:%02d", m, s);
}
