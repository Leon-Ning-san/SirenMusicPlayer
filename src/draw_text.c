#include "draw.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define GLYPH_CACHE_PER_SIZE 768
#define GLYPH_HASH_CAP       1024  // 2 的幂，用于 O(1) 查找
#define MAX_BUCKETS          48

typedef struct {
    uint32_t cp;
    int bw, bh;
    int x0, y0;
    float adv;
    uint8_t* bmp;
} Glyph;

typedef struct {
    int px;
    float scale;
    int used;
    int evict;
    int16_t hash_slot[GLYPH_HASH_CAP];
    Glyph g[GLYPH_CACHE_PER_SIZE];
} Bucket;

struct Font {
    stbtt_fontinfo info;
    int ascent, descent, linegap;
    Bucket* buckets[MAX_BUCKETS];
    int nbuckets;
};

static inline uint32_t cp_hash(uint32_t cp) {
    cp ^= cp >> 16;
    cp *= 0x85EBCA6Bu;
    cp ^= cp >> 13;
    return cp & (GLYPH_HASH_CAP - 1);
}

static void bucket_rehash(Bucket* b) {
    memset(b->hash_slot, 0xFF, sizeof(b->hash_slot));
    for (int i = 0; i < b->used; i++) {
        uint32_t h = cp_hash(b->g[i].cp);
        while (b->hash_slot[h] >= 0) h = (h + 1) & (GLYPH_HASH_CAP - 1);
        b->hash_slot[h] = (int16_t)i;
    }
}

static void raster_glyph(Font* f, Bucket* b, Glyph* g, uint32_t cp) {
    g->cp = cp;
    int advw, lsb;
    stbtt_GetCodepointHMetrics(&f->info, (int)cp, &advw, &lsb);
    g->adv = advw * b->scale;
    int ix0, iy0, ix1, iy1;
    stbtt_GetCodepointBitmapBox(&f->info, (int)cp, b->scale, b->scale, &ix0, &iy0, &ix1, &iy1);
    g->x0 = ix0; g->y0 = iy0;
    g->bw = ix1 - ix0; g->bh = iy1 - iy0;
    if (g->bw > 0 && g->bh > 0) {
        g->bmp = (uint8_t*)malloc((size_t)g->bw * g->bh);
        stbtt_MakeCodepointBitmap(&f->info, g->bmp, g->bw, g->bh, g->bw, b->scale, b->scale, (int)cp);
    } else {
        g->bmp = NULL;
    }
}

static Bucket* get_bucket(Font* f, int px) {
    for (int i = 0; i < f->nbuckets; i++) {
        if (f->buckets[i]->px == px) return f->buckets[i];
    }
    if (f->nbuckets >= MAX_BUCKETS) return NULL;
    Bucket* b = (Bucket*)calloc(1, sizeof(Bucket));
    if (!b) return NULL;
    b->px = px;
    b->scale = stbtt_ScaleForPixelHeight(&f->info, (float)px);
    memset(b->hash_slot, 0xFF, sizeof(b->hash_slot));
    f->buckets[f->nbuckets++] = b;
    return b;
}

static Glyph* glyph_get_in_bucket(Font* f, Bucket* b, uint32_t cp) {
    if (!b) return NULL;
    uint32_t h = cp_hash(cp);
    while (b->hash_slot[h] >= 0) {
        int idx = b->hash_slot[h];
        if (b->g[idx].cp == cp) return &b->g[idx];
        h = (h + 1) & (GLYPH_HASH_CAP - 1);
    }
    if (b->used < GLYPH_CACHE_PER_SIZE) {
        int idx = b->used++;
        Glyph* g = &b->g[idx];
        raster_glyph(f, b, g, cp);
        b->hash_slot[h] = (int16_t)idx;
        return g;
    }
    int idx = b->evict;
    b->evict = (b->evict + 1) % GLYPH_CACHE_PER_SIZE;
    Glyph* g = &b->g[idx];
    free(g->bmp);
    memset(g, 0, sizeof(*g));
    raster_glyph(f, b, g, cp);
    bucket_rehash(b);
    return g;
}

Font* font_load(const void* data, size_t len) {
    (void)len;
    Font* f = (Font*)calloc(1, sizeof(Font));
    int off = stbtt_GetFontOffsetForIndex((const unsigned char*)data, 0);
    if (off < 0 || !stbtt_InitFont(&f->info, (const unsigned char*)data, off)) {
        free(f);
        return NULL;
    }
    stbtt_GetFontVMetrics(&f->info, &f->ascent, &f->descent, &f->linegap);
    return f;
}

void font_free(Font* f) {
    if (!f) return;
    for (int i = 0; i < f->nbuckets; i++) {
        Bucket* b = f->buckets[i];
        for (int j = 0; j < b->used; j++) free(b->g[j].bmp);
        free(b);
    }
    free(f);
}

static inline uint32_t next_cp(const char** p) {
    const unsigned char* s = (const unsigned char*)*p;
    uint32_t c = *s++;
    if (c >= 0xF0 && c <= 0xF7) {
        if (*s) { c = ((c & 0x07) << 18) | ((s[0] & 0x3F) << 12); if (s[1]) c |= ((s[1] & 0x3F) << 6); if (s[2]) c |= (s[2] & 0x3F); if (s[0]) s++; if (*s) s++; if (*s) s++; }
    } else if (c >= 0xE0 && c <= 0xEF) {
        if (*s) { c = ((c & 0x0F) << 12) | ((s[0] & 0x3F) << 6); if (s[1]) c |= (s[1] & 0x3F); if (s[0]) s++; if (*s) s++; }
    } else if (c >= 0xC0 && c <= 0xDF) {
        if (*s) { c = ((c & 0x1F) << 6) | (s[0] & 0x3F); if (s[0]) s++; }
    }
    *p = (const char*)s;
    return c;
}

int text_width(Font* f, float px, const char* utf8) {
    if (!f || !utf8 || !*utf8) return 0;
    int ipx = (int)(px + 0.5f);
    if (ipx < 1) ipx = 1;
    Bucket* b = get_bucket(f, ipx);
    float w = 0.0f;
    const char* p = utf8;
    while (*p) {
        uint32_t cp = next_cp(&p);
        Glyph* g = glyph_get_in_bucket(f, b, cp);
        if (g) w += g->adv;
    }
    return (int)(w + 0.5f);
}

static uint8_t lut_light[256], lut_dark[256];
static int lut_ready = 0;
static void ensure_luts(void) {
    if (lut_ready) return;
    for (int i = 0; i < 256; i++) {
        float vl = powf((float)i / 255.0f, 0.64f) * 255.0f + 0.5f;
        float vd = powf((float)i / 255.0f, 0.78f) * 255.0f + 0.5f;
        lut_light[i] = vl > 255.0f ? 255 : (uint8_t)vl;
        lut_dark[i]  = vd > 255.0f ? 255 : (uint8_t)vd;
    }
    lut_ready = 1;
}

static void draw_text_px(Canvas* c, Font* f, float px, int x, int y, uint32_t col, const char* utf8) {
    if (!c || !f || !utf8 || !*utf8) return;
    int ipx = (int)(px + 0.5f);
    if (ipx < 1) ipx = 1;
    Bucket* b = get_bucket(f, ipx);
    if (!b) return;

    ensure_luts();
    int base_y = y + (int)(f->ascent * b->scale + 0.5f);
    uint32_t cr = col & 0xFFu, cg = (col >> 8) & 0xFFu, cb = (col >> 16) & 0xFFu;
    uint32_t ca = (col >> 24) & 0xFFu;
    if (ca == 0) ca = 255;
    const uint8_t* lut = (((cr * 299u + cg * 587u + cb * 114u) / 1000u) >= 128u) ? lut_light : lut_dark;

    float fcx = (float)x;
    const char* p = utf8;
    while (*p) {
        uint32_t cp = next_cp(&p);
        Glyph* g = glyph_get_in_bucket(f, b, cp);
        if (!g) continue;
        int cx = (int)(fcx + 0.5f);
        if (g->bmp) {
            int gx = cx + g->x0;
            int gy = base_y + g->y0;
            int cx0 = c->clip_x0, cy0 = c->clip_y0, cx1 = c->clip_x1, cy1 = c->clip_y1;
            if (cx0 == 0 && cy0 == 0 && cx1 == 0 && cy1 == 0) { cx1 = c->w; cy1 = c->h; }
            if (cx0 < 0) cx0 = 0;
            if (cy0 < 0) cy0 = 0;
            if (cx1 > c->w) cx1 = c->w;
            if (cy1 > c->h) cy1 = c->h;
            int j0 = gy < cy0 ? (cy0 - gy) : 0;
            int j1 = (gy + g->bh > cy1) ? (cy1 - gy) : g->bh;
            int i0 = gx < cx0 ? (cx0 - gx) : 0;
            int i1 = (gx + g->bw > cx1) ? (cx1 - gx) : g->bw;
            for (int j = j0; j < j1; j++) {
                uint32_t* drow = c->px + (size_t)(gy + j) * c->w + gx;
                const uint8_t* srow = g->bmp + (size_t)j * g->bw;
                for (int i = i0; i < i1; i++) {
                    uint8_t raw = srow[i];
                    if (!raw) continue;
                    uint32_t alpha = lut[raw];
                    if (ca < 255u) alpha = (alpha * ca + 128u) >> 8;
                    if (alpha == 0) continue;
                    if (alpha >= 255u) {
                        drow[i] = 0xFF000000u | (cb << 16) | (cg << 8) | cr;
                        continue;
                    }
                    uint32_t dst = drow[i];
                    uint32_t na = 255u - alpha;
                    uint32_t tr = cr * alpha + (dst & 0xFFu) * na;
                    uint32_t tg = cg * alpha + ((dst >> 8) & 0xFFu) * na;
                    uint32_t tb = cb * alpha + ((dst >> 16) & 0xFFu) * na;
                    uint32_t r  = (tr + 1u + (tr >> 8)) >> 8;
                    uint32_t gg = (tg + 1u + (tg >> 8)) >> 8;
                    uint32_t bb = (tb + 1u + (tb >> 8)) >> 8;
                    drow[i] = 0xFF000000u | (bb << 16) | (gg << 8) | r;
                }
            }
        }
        fcx += g->adv;
    }
}

void draw_text(Canvas* c, Font* f, float px, int x, int y, uint32_t col, const char* utf8) {
    if (!c) return;
    float s = c->s > 0.0f ? c->s : 1.0f;
    draw_text_px(c, f, px * s, (int)floorf((float)x * s + 0.5f), (int)floorf((float)y * s + 0.5f), col, utf8);
}

void draw_text_centered(Canvas* c, Font* f, float px, int x0, int x1, int y, uint32_t col, const char* utf8) {
    if (!c) return;
    float s = c->s > 0.0f ? c->s : 1.0f;
    int px0 = (int)floorf((float)x0 * s + 0.5f);
    int px1 = (int)floorf((float)x1 * s + 0.5f);
    int py  = (int)floorf((float)y * s + 0.5f);
    int w = text_width(f, px * s, utf8);
    draw_text_px(c, f, px * s, px0 + (px1 - px0 - w) / 2, py, col, utf8);
}

void draw_text_right(Canvas* c, Font* f, float px, int x1, int y, uint32_t col, const char* utf8) {
    if (!c) return;
    float s = c->s > 0.0f ? c->s : 1.0f;
    int px1 = (int)floorf((float)x1 * s + 0.5f);
    int py  = (int)floorf((float)y * s + 0.5f);
    int w = text_width(f, px * s, utf8);
    draw_text_px(c, f, px * s, px1 - w, py, col, utf8);
}

int text_width_max(Font* f, float px, const char* utf8, int max_w) {
    if (!f || !utf8 || !*utf8) return 0;
    int ipx = (int)(px + 0.5f);
    if (ipx < 1) ipx = 1;
    Bucket* b = get_bucket(f, ipx);
    float w = 0.0f;
    const char* p = utf8;
    const char* ell = "\xE2\x80\xA6";
    int ellw = text_width(f, px, ell);
    while (*p) {
        uint32_t cp = next_cp(&p);
        Glyph* g = glyph_get_in_bucket(f, b, cp);
        if (!g) continue;
        if (w + g->adv > (float)(max_w - ellw)) {
            return (int)(w + 0.5f) + ellw;
        }
        w += g->adv;
    }
    return (int)(w + 0.5f);
}

int draw_text_max(Canvas* c, Font* f, float px, int x, int y, uint32_t col, const char* utf8, int max_w) {
    if (!c || !f || !utf8 || !*utf8) return 0;
    float s = c->s > 0.0f ? c->s : 1.0f;
    float spx = px * s;
    int sx = (int)floorf((float)x * s + 0.5f);
    int sy = (int)floorf((float)y * s + 0.5f);
    int smax = (int)floorf((float)max_w * s + 0.5f);
    int ipx = (int)(spx + 0.5f);
    if (ipx < 1) ipx = 1;
    Bucket* b = get_bucket(f, ipx);
    const char* ell = "\xE2\x80\xA6";
    int ellw = text_width(f, spx, ell);
    char buf[256];
    const char* p = utf8;
    float w = 0.0f;
    while (*p) {
        const char* save = p;
        uint32_t cp = next_cp(&p);
        Glyph* g = glyph_get_in_bucket(f, b, cp);
        if (!g) continue;
        if (w + g->adv > (float)(smax - ellw)) {
            size_t n = (size_t)(save - utf8);
            if (n >= sizeof(buf)) n = sizeof(buf) - 1;
            memcpy(buf, utf8, n);
            buf[n] = 0;
            char cat[264];
            snprintf(cat, sizeof(cat), "%s%s", buf, ell);
            draw_text_px(c, f, spx, sx, sy, col, cat);
            return 1;
        }
        w += g->adv;
    }
    draw_text_px(c, f, spx, sx, sy, col, utf8);
    return 0;
}
