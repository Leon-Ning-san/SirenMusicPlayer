#include "lrc.h"
#include "util.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_LINES 4096

static int cmp_lines(const void* a, const void* b) {
    const LrcLine* x = (const LrcLine*)a;
    const LrcLine* y = (const LrcLine*)b;
    if (x->t < y->t) return -1;
    if (x->t > y->t) return 1;
    return 0;
}

static void add_meta(Lyrics* l, const char* key, const char* val) {
    if (!strcmp(key, "ti")) snprintf(l->ti, sizeof(l->ti), "%s", val);
    else if (!strcmp(key, "ar")) snprintf(l->ar, sizeof(l->ar), "%s", val);
}

Lyrics* lyrics_load(const char* media_path) {
    char path[600];
    snprintf(path, sizeof(path), "%s", media_path);
    size_t n = strlen(path);
    if (n < 5) return NULL;
    strcpy(path + n - 4, ".lrc");
    FILE* f = fopen(path, "rb");
    if (!f) {
        strcpy(path + n - 4, ".LRC");
        f = fopen(path, "rb");
        if (!f) return NULL;
    }
    // read all
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 2 * 1024 * 1024) { fclose(f); return NULL; }
    unsigned char* raw = (unsigned char*)malloc((size_t)sz);
    if (fread(raw, 1, (size_t)sz, f) != (size_t)sz) { free(raw); fclose(f); return NULL; }
    fclose(f);

    // detect UTF-16
    char* text = NULL;
    if (sz >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        text = (char*)malloc((size_t)sz * 2 + 1);
        my_utf16_to_utf8((const uint16_t*)(raw + 2), ((size_t)sz - 2) / 2, text, (size_t)sz * 2);
    } else if (sz >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
        text = (char*)malloc((size_t)sz);
        memcpy(text, raw + 3, (size_t)sz - 3);
        text[sz - 3] = 0;
    } else {
        text = (char*)malloc((size_t)sz + 1);
        memcpy(text, raw, (size_t)sz);
        text[sz] = 0;
    }
    free(raw);

    Lyrics* l = (Lyrics*)calloc(1, sizeof(Lyrics));
    l->lines = (LrcLine*)malloc(sizeof(LrcLine) * MAX_LINES);

    char* save = NULL;
    for (char* line = strtok_r(text, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        // parse all [tag] prefixes
        double times[16];
        int ntimes = 0;
        char key[16] = {0};
        char val[256] = {0};
        char* p = line;
        int has_meta = 0;
        while (*p == '[') {
            char* close = strchr(p, ']');
            if (!close || close - p > 40) break;
            char tag[40];
            size_t tn = (size_t)(close - p - 1);
            memcpy(tag, p + 1, tn);
            tag[tn] = 0;
            // time tag?
            int mm = 0, ss = 0, ff = 0;
            int ffdigits = 0;
            if (sscanf(tag, "%d:%d.%n", &mm, &ss, &ffdigits) >= 2 || sscanf(tag, "%d:%d:%n", &mm, &ss, &ffdigits) >= 2) {
                double frac = 0;
                if (ffdigits > 0) {
                    const char* fp = tag + ffdigits;
                    int digits = 0;
                    while (*fp >= '0' && *fp <= '9') { frac = frac * 10 + (*fp - '0'); fp++; digits++; }
                    while (digits > 0) { frac /= 10.0; digits--; }
                }
                double t = mm * 60 + ss + frac;
                if (ntimes < 16) times[ntimes++] = t;
            } else {
                char* co = strchr(tag, ':');
                if (co) {
                    *co = 0;
                    snprintf(key, sizeof(key), "%s", tag);
                    snprintf(val, sizeof(val), "%s", co + 1);
                    has_meta = 1;
                }
            }
            p = close + 1;
        }
        if (has_meta && key[0] && val[0] && !strchr(key, ':')) {
            add_meta(l, key, val);
        }
        if (ntimes > 0) {
            char txt[256];
            snprintf(txt, sizeof(txt), "%s", p);
            // trim trailing spaces
            size_t tl = strlen(txt);
            while (tl > 0 && (txt[tl-1] == ' ' || txt[tl-1] == '\t')) txt[--tl] = 0;
            if (tl > 0) {
                for (int i = 0; i < ntimes; i++) {
                    if (l->n >= MAX_LINES) break;
                    LrcLine* ln = &l->lines[l->n++];
                    ln->t = times[i];
                    snprintf(ln->text, sizeof(ln->text), "%s", txt);
                }
            }
        }
    }
    free(text);
    if (l->n == 0) {
        lyrics_free(l);
        return NULL;
    }
    qsort(l->lines, (size_t)l->n, sizeof(LrcLine), cmp_lines);
    return l;
}

void lyrics_free(Lyrics* l) {
    if (!l) return;
    free(l->lines);
    free(l);
}

int lyrics_index(const Lyrics* l, double t) {
    if (!l || l->n == 0) return -1;
    int lo = 0, hi = l->n - 1;
    if (t < l->lines[0].t) return -1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (l->lines[mid].t <= t) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}
