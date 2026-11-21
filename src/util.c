#include "util.h"
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

const char* path_basename(const char* p) {
    const char* slash = strrchr(p, '/');
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
