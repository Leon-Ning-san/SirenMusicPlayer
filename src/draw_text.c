#include "draw.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define GLYPH_CACHE_PER_SIZE 768
// 字号缓存桶上限：应用横屏+竖屏合计用到 15+ 种字号，
// 上限过低会导致 glyph_get 返回 NULL、文字静默丢失（竖屏字体错误）。
#define MAX_BUCKETS 48

typedef struct {
    uint32_t cp;
    int bw, bh;       // bitmap size
    int x0, y0;       // offset relative to baseline-origin
    float adv;        // horizontal advance (px units at bucket size)
    uint8_t* bmp;     // A8 bitmap (owned)
} Glyph;

typedef struct {
    int px;           // pixel size
    float scale;
    int used;
    int evict;        // round-robin evict cursor
    Glyph g[GLYPH_CACHE_PER_SIZE];
} Bucket;

struct Font {
    stbtt_fontinfo info;
    int ascent, descent, linegap; // in font units
    Bucket* buckets[MAX_BUCKETS];
    int nbuckets;
};

static Glyph* glyph_get(Font* f, int px, uint32_t cp) {
    Bucket* b = NULL;
    for (int i = 0; i < f->nbuckets; i++) {
        if (f->buckets[i]->px == px) { b = f->buckets[i]; break; }
    }
    if (!b) {
        if (f->nbuckets >= MAX_BUCKETS) return NULL;
        b = (Bucket*)calloc(1, sizeof(Bucket));
        b->px = px;
        b->scale = stbtt_ScaleForPixelHeight(&f->info, (float)px);
        f->buckets[f->nbuckets++] = b;
    }
    for (int i = 0; i < b->used; i++) {
        if (b->g[i].cp == cp) return &b->g[i];
    }
    if (b->used < GLYPH_CACHE_PER_SIZE) {
        Glyph* g = &b->g[b->used++];
        g->cp = cp;
        int advw, lsb;
        stbtt_GetCodepointHMetrics(&f->info, cp, &advw, &lsb);
        g->adv = advw * b->scale;
        int ix0, iy0, ix1, iy1;
        stbtt_GetCodepointBitmapBox(&f->info, cp, b->scale, b->scale, &ix0, &iy0, &ix1, &iy1);
        g->x0 = ix0; g->y0 = iy0;
        g->bw = ix1 - ix0; g->bh = iy1 - iy0;
        if (g->bw > 0 && g->bh > 0) {
            g->bmp = (uint8_t*)malloc((size_t)g->bw * g->bh);
            stbtt_MakeCodepointBitmap(&f->info, g->bmp, g->bw, g->bh, g->bw * 1, b->scale, b->scale, cp);
        } else {
            g->bmp = NULL;
        }
        return g;
    }
    // evict round-robin
    Glyph* g = &b->g[b->evict];
    b->evict = (b->evict + 1) % GLYPH_CACHE_PER_SIZE;
    free(g->bmp);
    memset(g, 0, sizeof(*g));
    g->cp = cp;
    int advw, lsb;
    stbtt_GetCodepointHMetrics(&f->info, cp, &advw, &lsb);
    g->adv = advw * b->scale;
    int ix0, iy0, ix1, iy1;
    stbtt_GetCodepointBitmapBox(&f->info, cp, b->scale, b->scale, &ix0, &iy0, &ix1, &iy1);
    g->x0 = ix0; g->y0 = iy0;
    g->bw = ix1 - ix0; g->bh = iy1 - iy0;
    if (g->bw > 0 && g->bh > 0) {
        g->bmp = (uint8_t*)malloc((size_t)g->bw * g->bh);
        stbtt_MakeCodepointBitmap(&f->info, g->bmp, g->bw, g->bh, g->bw * 1, b->scale, b->scale, cp);
    } else g->bmp = NULL;
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

// decode next UTF-8 code point, advance *p
static uint32_t next_cp(const char** p) {
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
    float w = 0.0f;
    const char* p = utf8;
    while (*p) {
        uint32_t cp = next_cp(&p);
        Glyph* g = glyph_get(f, ipx, cp);
        if (g) w += g->adv;
    }
    return (int)(w + 0.5f);
}

void draw_text(Canvas* c, Font* f, float px, int x, int y, uint32_t col, const char* utf8) {
    if (!c || !f || !utf8 || !*utf8) return;
    int ipx = (int)(px + 0.5f);
    if (ipx < 1) ipx = 1;
    // find bucket scale for baseline computation
    float scale = stbtt_ScaleForPixelHeight(&f->info, (float)ipx);
    float baseline = y + f->ascent * scale;
    int cr = col & 0xFF, cg = (col >> 8) & 0xFF, cb = (col >> 16) & 0xFF;
    uint8_t ca = (col >> 24) & 0xFF;
    if (ca == 0) ca = 255;

    // 浅色字在深底提亮(0.64)防发虚；深色字在亮底适度加深(0.78)让细笔画同样扎实清晰
    static uint8_t lut_light[256], lut_dark[256];
    static int lut_ready = 0;
    if (!lut_ready) {
        for (int i = 0; i < 256; i++) {
            float vl = powf((float)i / 255.0f, 0.64f) * 255.0f + 0.5f;
            float vd = powf((float)i / 255.0f, 0.78f) * 255.0f + 0.5f;
            lut_light[i] = vl > 255.0f ? 255 : (uint8_t)vl;
            lut_dark[i]  = vd > 255.0f ? 255 : (uint8_t)vd;
        }
        lut_ready = 1;
    }
    int is_light_fg = ((cr * 299 + cg * 587 + cb * 114) / 1000) >= 128;

    float fcx = (float)x;
    const char* p = utf8;
    while (*p) {
        uint32_t cp = next_cp(&p);
        Glyph* g = glyph_get(f, ipx, cp);
        if (!g) continue;
        int cx = (int)(fcx + 0.5f);
        if (g->bmp) {
            int gx = cx + g->x0;
            int gy = (int)(baseline + 0.5f) + g->y0;
            for (int j = 0; j < g->bh; j++) {
                int ty = gy + j;
                if (ty < 0 || ty >= c->h) continue;
                for (int i = 0; i < g->bw; i++) {
                    int tx = gx + i;
                    if (tx < 0 || tx >= c->w) continue;
                    uint8_t raw = g->bmp[(size_t)j * g->bw + i];
                    if (!raw) continue;
                    uint8_t a8 = is_light_fg ? lut_light[raw] : lut_dark[raw];
                    uint32_t alpha = ((uint32_t)a8 * ca + 127) / 255;
                    uint32_t* d = &c->px[(size_t)ty * c->w + tx];
                    uint32_t dst = *d;
                    uint32_t na = 255 - alpha;
                    uint32_t r = ((cr * alpha + (dst & 0xFF) * na) / 255);
                    uint32_t gg = ((cg * alpha + ((dst >> 8) & 0xFF) * na) / 255);
                    uint32_t b = ((cb * alpha + ((dst >> 16) & 0xFF) * na) / 255);
                    *d = 0xFF000000u | (b << 16) | (gg << 8) | r;
                }
            }
        }
        fcx += g->adv;
    }
}

void draw_text_centered(Canvas* c, Font* f, float px, int x0, int x1, int y, uint32_t col, const char* utf8) {
    int w = text_width(f, px, utf8);
    int x = x0 + (x1 - x0 - w) / 2;
    draw_text(c, f, px, x, y, col, utf8);
}

void draw_text_right(Canvas* c, Font* f, float px, int x1, int y, uint32_t col, const char* utf8) {
    int w = text_width(f, px, utf8);
    draw_text(c, f, px, x1 - w, y, col, utf8);
}

int text_width_max(Font* f, float px, const char* utf8, int max_w) {
    if (!f || !utf8 || !*utf8) return 0;
    int ipx = (int)(px + 0.5f);
    if (ipx < 1) ipx = 1;
    float w = 0.0f;
    const char* p = utf8;
    const char* ell = "\xE2\x80\xA6"; // …
    int ellw = text_width(f, px, ell);
    while (*p) {
        uint32_t cp = next_cp(&p);
        Glyph* g = glyph_get(f, ipx, cp);
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
    int ipx = (int)(px + 0.5f);
    if (ipx < 1) ipx = 1;
    const char* ell = "\xE2\x80\xA6";
    int ellw = text_width(f, px, ell);
    char buf[256];
    const char* p = utf8;
    float w = 0.0f;
    while (*p) {
        const char* save = p;
        uint32_t cp = next_cp(&p);
        Glyph* g = glyph_get(f, ipx, cp);
        if (!g) continue;
        if (w + g->adv > (float)(max_w - ellw)) {
            size_t n = (size_t)(save - utf8);
            if (n >= sizeof(buf)) n = sizeof(buf) - 1;
            memcpy(buf, utf8, n);
            buf[n] = 0;
            char cat[264];
            snprintf(cat, sizeof(cat), "%s%s", buf, ell);
            draw_text(c, f, px, x, y, col, cat);
            return 1;
        }
        w += g->adv;
    }
    draw_text(c, f, px, x, y, col, utf8);
    return 0;
}
