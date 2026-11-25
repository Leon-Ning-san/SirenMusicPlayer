#include "draw.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static inline void canvas_clip_bounds(const Canvas* c, int* x0, int* y0, int* x1, int* y1) {
    int cx0 = c->clip_x0, cy0 = c->clip_y0, cx1 = c->clip_x1, cy1 = c->clip_y1;
    if (cx0 == 0 && cy0 == 0 && cx1 == 0 && cy1 == 0) {
        cx1 = c->w;
        cy1 = c->h;
    }
    *x0 = cx0 < 0 ? 0 : cx0;
    *y0 = cy0 < 0 ? 0 : cy0;
    *x1 = cx1 > c->w ? c->w : cx1;
    *y1 = cy1 > c->h ? c->h : cy1;
}

Canvas* canvas_new(int w, int h) {
    if (w <= 0 || h <= 0) return NULL;
    Canvas* c = (Canvas*)malloc(sizeof(Canvas));
    if (!c) return NULL;
    c->w = w; c->h = h;
    c->lw = w; c->lh = h;
    c->s = 1.0f;
    c->clip_x0 = 0; c->clip_y0 = 0;
    c->clip_x1 = w; c->clip_y1 = h;
    c->px = (uint32_t*)calloc((size_t)w * h, 4);
    if (!c->px) { free(c); return NULL; }
    return c;
}

Canvas* canvas_new_scaled(int lw, int lh, float s) {
    if (s < 0.25f) s = 0.25f;
    Canvas* c = canvas_new((int)(lw * s + 0.5f), (int)(lh * s + 0.5f));
    if (!c) return NULL;
    c->lw = lw; c->lh = lh;
    c->s = s;
    return c;
}

void canvas_set_clip_px(Canvas* c, int x0, int y0, int x1, int y1) {
    if (!c) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > c->w) x1 = c->w;
    if (y1 > c->h) y1 = c->h;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    c->clip_x0 = x0; c->clip_y0 = y0;
    c->clip_x1 = x1; c->clip_y1 = y1;
}

void canvas_reset_clip(Canvas* c) {
    if (!c) return;
    c->clip_x0 = 0; c->clip_y0 = 0;
    c->clip_x1 = c->w; c->clip_y1 = c->h;
}

void canvas_free(Canvas* c) {
    if (c) { free(c->px); free(c); }
}

void canvas_clear(Canvas* c, uint32_t col) {
    if (!c || !c->px || c->w <= 0 || c->h <= 0) return;
    size_t n = (size_t)c->w * c->h;
    uint32_t* px = c->px;
    for (size_t i = 0; i < n; i++) px[i] = col;
}

void fill_rect_px(Canvas* c, int x, int y, int w, int h, uint32_t col) {
    int cx0, cy0, cx1, cy1;
    canvas_clip_bounds(c, &cx0, &cy0, &cx1, &cy1);
    if (x < cx0) { w += x - cx0; x = cx0; }
    if (y < cy0) { h += y - cy0; y = cy0; }
    if (x + w > cx1) w = cx1 - x;
    if (y + h > cy1) h = cy1 - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        uint32_t* row = c->px + (size_t)(y + j) * c->w + x;
        for (int i = 0; i < w; i++) row[i] = col;
    }
}

static inline uint32_t blend_px(uint32_t dst, uint32_t src) {
    uint32_t a = (src >> 24) & 0xFF;
    if (a == 255) return src;
    if (a == 0) return dst;
    uint32_t na = 255 - a;
    uint32_t rb = ((src & 0x00FF00FFu) * a + (dst & 0x00FF00FFu) * na) >> 8;
    uint32_t g  = ((src & 0x0000FF00u) * a + (dst & 0x0000FF00u) * na) >> 8;
    return 0xFF000000u | (rb & 0x00FF00FFu) | (g & 0x0000FF00u);
}

static inline uint32_t blend_px_a(uint32_t dst, uint32_t src, uint8_t ga) {
    if (ga == 0) return dst;
    uint32_t a = (src >> 24) & 0xFF;
    if (ga < 255) a = (a * ga) >> 8;
    if (a == 0) return dst;
    if (a >= 255) return src | 0xFF000000u;
    uint32_t na = 255 - a;
    uint32_t rb = ((src & 0x00FF00FFu) * a + (dst & 0x00FF00FFu) * na) >> 8;
    uint32_t g  = ((src & 0x0000FF00u) * a + (dst & 0x0000FF00u) * na) >> 8;
    return 0xFF000000u | (rb & 0x00FF00FFu) | (g & 0x0000FF00u);
}

void fill_rect_a_px(Canvas* c, int x, int y, int w, int h, uint32_t col, uint8_t alpha) {
    if (alpha == 0) return;
    if (alpha == 255) { fill_rect_px(c, x, y, w, h, col | 0xFF000000u); return; }
    int cx0, cy0, cx1, cy1;
    canvas_clip_bounds(c, &cx0, &cy0, &cx1, &cy1);
    if (x < cx0) { w += x - cx0; x = cx0; }
    if (y < cy0) { h += y - cy0; y = cy0; }
    if (x + w > cx1) w = cx1 - x;
    if (y + h > cy1) h = cy1 - y;
    if (w <= 0 || h <= 0) return;

    uint32_t a = alpha;
    uint32_t na = 255u - a;
    uint32_t s_rb = (col & 0x00FF00FFu) * a;
    uint32_t s_g  = (col & 0x0000FF00u) * a;
    for (int j = 0; j < h; j++) {
        uint32_t* row = c->px + (size_t)(y + j) * c->w + x;
        for (int i = 0; i < w; i++) {
            uint32_t d = row[i];
            uint32_t rb = (s_rb + (d & 0x00FF00FFu) * na) >> 8;
            uint32_t g  = (s_g  + (d & 0x0000FF00u) * na) >> 8;
            row[i] = 0xFF000000u | (rb & 0x00FF00FFu) | (g & 0x0000FF00u);
        }
    }
}

static void hline_a(Canvas* c, int x, int y, int len, uint32_t s_rb, uint32_t s_g, uint32_t na) {
    int cx0, cy0, cx1, cy1;
    canvas_clip_bounds(c, &cx0, &cy0, &cx1, &cy1);
    if (y < cy0 || y >= cy1 || len <= 0) return;
    if (x < cx0) { len += x - cx0; x = cx0; }
    if (x + len > cx1) len = cx1 - x;
    if (len <= 0) return;
    uint32_t* row = c->px + (size_t)y * c->w + x;
    for (int i = 0; i < len; i++) {
        uint32_t d = row[i];
        uint32_t rb = (s_rb + (d & 0x00FF00FFu) * na) >> 8;
        uint32_t g  = (s_g  + (d & 0x0000FF00u) * na) >> 8;
        row[i] = 0xFF000000u | (rb & 0x00FF00FFu) | (g & 0x0000FF00u);
    }
}

void hline_px(Canvas* c, int x, int y, int len, uint32_t col) {
    fill_rect_px(c, x, y, len, 1, col);
}
void vline_px(Canvas* c, int x, int y, int len, uint32_t col) {
    fill_rect_px(c, x, y, 1, len, col);
}

void fill_tri_px(Canvas* c, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t col) {
    int miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    int minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
    int maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    if (miny < 0) miny = 0;
    if (maxy >= c->h) maxy = c->h - 1;
    if (minx < 0) minx = 0;
    if (maxx >= c->w) maxx = c->w - 1;
    int area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (area == 0) return;
    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            int w0 = (x1 - x0) * (y - y0) - (x - x0) * (y1 - y0);
            int w1 = (x2 - x1) * (y - y1) - (x - x1) * (y2 - y1);
            int w2 = (x0 - x2) * (y - y2) - (x - x2) * (y0 - y2);
            int s = (w0 >= 0 ? 1 : -1) + (w1 >= 0 ? 1 : -1) + (w2 >= 0 ? 1 : -1);
            if (s == 3 || s == -3) c->px[(size_t)y * c->w + x] = col;
        }
    }
}

void blit_rgba_px(Canvas* dst, int dx, int dy, const uint32_t* src, int sw, int sh) {
    int cx0, cy0, cx1, cy1;
    canvas_clip_bounds(dst, &cx0, &cy0, &cx1, &cy1);
    int x0 = dx < cx0 ? cx0 - dx : 0;
    int y0 = dy < cy0 ? cy0 - dy : 0;
    int x1 = (dx + sw > cx1) ? cx1 - dx : sw;
    int y1 = (dy + sh > cy1) ? cy1 - dy : sh;
    if (x1 <= x0 || y1 <= y0) return;
    for (int j = y0; j < y1; j++) {
        uint32_t* d = dst->px + (size_t)(dy + j) * dst->w + dx + x0;
        const uint32_t* s = src + (size_t)j * sw + x0;
        for (int i = x0; i < x1; i++) {
            *d = blend_px(*d, *s);
            d++; s++;
        }
    }
}

void blit_canvas_opaque_px(Canvas* dst, int dx, int dy, const Canvas* src) {
    if (!dst || !src) return;
    int x0 = dx < 0 ? -dx : 0;
    int y0 = dy < 0 ? -dy : 0;
    int x1 = (dx + src->w > dst->w) ? dst->w - dx : src->w;
    int y1 = (dy + src->h > dst->h) ? dst->h - dy : src->h;
    int cnt = x1 - x0;
    if (cnt <= 0 || y1 <= y0) return;
    size_t row_bytes = (size_t)cnt * 4;
    for (int j = y0; j < y1; j++) {
        memcpy(dst->px + (size_t)(dy + j) * dst->w + dx + x0,
               src->px + (size_t)j * src->w + x0, row_bytes);
    }
}

void blend_over_opaque(uint32_t* dst, const uint32_t* src, size_t n, uint8_t a) {
    if (a == 0) return;
    if (a == 255) { memcpy(dst, src, n * 4); return; }
    uint32_t ua = a;
    uint32_t na = 255u - ua;
    for (size_t i = 0; i < n; i++) {
        uint32_t s = src[i];
        uint32_t d = dst[i];
        uint32_t rb = ((s & 0x00FF00FFu) * ua + (d & 0x00FF00FFu) * na) >> 8;
        uint32_t g  = ((s & 0x0000FF00u) * ua + (d & 0x0000FF00u) * na) >> 8;
        dst[i] = 0xFF000000u | (rb & 0x00FF00FFu) | (g & 0x0000FF00u);
    }
}

void blit_rgba_a_px(Canvas* dst, int dx, int dy, const uint32_t* src, int sw, int sh, uint8_t a) {
    if (a == 0) return;
    if (a == 255) { blit_rgba_px(dst, dx, dy, src, sw, sh); return; }
    int x0 = dx < 0 ? -dx : 0;
    int y0 = dy < 0 ? -dy : 0;
    int x1 = (dx + sw > dst->w) ? dst->w - dx : sw;
    int y1 = (dy + sh > dst->h) ? dst->h - dy : sh;
    if (x1 <= x0 || y1 <= y0) return;
    for (int j = y0; j < y1; j++) {
        uint32_t* d = dst->px + (size_t)(dy + j) * dst->w + dx + x0;
        const uint32_t* s = src + (size_t)j * sw + x0;
        for (int i = x0; i < x1; i++) {
            *d = blend_px_a(*d, *s, a);
            d++; s++;
        }
    }
}

static inline int corner_cov(int dx_c, int dy_c, int r) {
    float d = sqrtf((float)(dx_c * dx_c + dy_c * dy_c));
    float cov = (float)r - d + 0.5f;
    if (cov <= 0.0f) return 0;
    if (cov >= 1.0f) return 255;
    return (int)(cov * 255.0f + 0.5f);
}

void blit_rgb_round_px(Canvas* dst, int dx, int dy, const uint32_t* src, int sw, int sh, int corner) {
    if (corner > sw / 2) corner = sw / 2;
    if (corner > sh / 2) corner = sh / 2;
    int cx0, cy0, cx1, cy1;
    canvas_clip_bounds(dst, &cx0, &cy0, &cx1, &cy1);
    int x0 = dx < cx0 ? cx0 - dx : 0;
    int y0 = dy < cy0 ? cy0 - dy : 0;
    int x1 = (dx + sw > cx1) ? cx1 - dx : sw;
    int y1 = (dy + sh > cy1) ? cy1 - dy : sh;
    if (x1 <= x0 || y1 <= y0) return;

    for (int j = y0; j < y1; j++) {
        uint32_t* drow = dst->px + (size_t)(dy + j) * dst->w + dx;
        const uint32_t* srow = src + (size_t)j * sw;
        int cy_dist = 0;
        if (j < corner) cy_dist = corner - j;
        else if (j >= sh - corner) cy_dist = j - (sh - 1 - corner);

        if (cy_dist == 0) {
            memcpy(drow + x0, srow + x0, (size_t)(x1 - x0) * 4);
            continue;
        }
        int mid_s = x0 < corner ? corner : x0;
        int mid_e = x1 > sw - corner ? sw - corner : x1;
        if (mid_e > mid_s) {
            memcpy(drow + mid_s, srow + mid_s, (size_t)(mid_e - mid_s) * 4);
        }
        for (int i = x0; i < corner && i < x1; i++) {
            int cov = corner_cov(corner - i, cy_dist, corner);
            if (cov >= 255) drow[i] = srow[i] | 0xFF000000u;
            else if (cov > 0) drow[i] = blend_px_a(drow[i], srow[i] | 0xFF000000u, (uint8_t)cov);
        }
        int rx0 = (sw - corner) > x0 ? (sw - corner) : x0;
        for (int i = rx0; i < x1; i++) {
            int cov = corner_cov(i - (sw - 1 - corner), cy_dist, corner);
            if (cov >= 255) drow[i] = srow[i] | 0xFF000000u;
            else if (cov > 0) drow[i] = blend_px_a(drow[i], srow[i] | 0xFF000000u, (uint8_t)cov);
        }
    }
}

void blit_rgba_round_a_px(Canvas* dst, int dx, int dy, const uint32_t* src, int sw, int sh, int corner, uint8_t a) {
    if (a == 0) return;
    if (corner > sw / 2) corner = sw / 2;
    if (corner > sh / 2) corner = sh / 2;
    int cx0, cy0, cx1, cy1;
    canvas_clip_bounds(dst, &cx0, &cy0, &cx1, &cy1);
    int x0 = dx < cx0 ? cx0 - dx : 0;
    int y0 = dy < cy0 ? cy0 - dy : 0;
    int x1 = (dx + sw > cx1) ? cx1 - dx : sw;
    int y1 = (dy + sh > cy1) ? cy1 - dy : sh;
    if (x1 <= x0 || y1 <= y0) return;

    for (int j = y0; j < y1; j++) {
        uint32_t* drow = dst->px + (size_t)(dy + j) * dst->w + dx;
        const uint32_t* srow = src + (size_t)j * sw;
        int cy_dist = 0;
        if (j < corner) cy_dist = corner - j;
        else if (j >= sh - corner) cy_dist = j - (sh - 1 - corner);

        int mid_s = (cy_dist == 0) ? x0 : (x0 < corner ? corner : x0);
        int mid_e = (cy_dist == 0) ? x1 : (x1 > sw - corner ? sw - corner : x1);
        for (int i = mid_s; i < mid_e; i++) {
            drow[i] = blend_px_a(drow[i], srow[i], a);
        }
        if (cy_dist > 0) {
            for (int i = x0; i < corner && i < x1; i++) {
                int cov = corner_cov(corner - i, cy_dist, corner);
                if (cov > 0) drow[i] = blend_px_a(drow[i], srow[i], (uint8_t)((cov * a) >> 8));
            }
            int rx0 = (sw - corner) > x0 ? (sw - corner) : x0;
            for (int i = rx0; i < x1; i++) {
                int cov = corner_cov(i - (sw - 1 - corner), cy_dist, corner);
                if (cov > 0) drow[i] = blend_px_a(drow[i], srow[i], (uint8_t)((cov * a) >> 8));
            }
        }
    }
}

void blit_rgba_round_px(Canvas* dst, int dx, int dy, const uint32_t* src, int sw, int sh, int corner) {
    if (corner > sw / 2) corner = sw / 2;
    if (corner > sh / 2) corner = sh / 2;
    int cx0, cy0, cx1, cy1;
    canvas_clip_bounds(dst, &cx0, &cy0, &cx1, &cy1);
    int x0 = dx < cx0 ? cx0 - dx : 0;
    int y0 = dy < cy0 ? cy0 - dy : 0;
    int x1 = (dx + sw > cx1) ? cx1 - dx : sw;
    int y1 = (dy + sh > cy1) ? cy1 - dy : sh;
    if (x1 <= x0 || y1 <= y0) return;

    for (int j = y0; j < y1; j++) {
        uint32_t* drow = dst->px + (size_t)(dy + j) * dst->w + dx;
        const uint32_t* srow = src + (size_t)j * sw;
        int cy_dist = 0;
        if (j < corner) cy_dist = corner - j;
        else if (j >= sh - corner) cy_dist = j - (sh - 1 - corner);

        int mid_s = (cy_dist == 0) ? x0 : (x0 < corner ? corner : x0);
        int mid_e = (cy_dist == 0) ? x1 : (x1 > sw - corner ? sw - corner : x1);
        for (int i = mid_s; i < mid_e; i++) {
            drow[i] = blend_px(drow[i], srow[i]);
        }
        if (cy_dist > 0) {
            for (int i = x0; i < corner && i < x1; i++) {
                int cov = corner_cov(corner - i, cy_dist, corner);
                if (cov >= 255) drow[i] = blend_px(drow[i], srow[i]);
                else if (cov > 0) drow[i] = blend_px_a(drow[i], srow[i], (uint8_t)cov);
            }
            int rx0 = (sw - corner) > x0 ? (sw - corner) : x0;
            for (int i = rx0; i < x1; i++) {
                int cov = corner_cov(i - (sw - 1 - corner), cy_dist, corner);
                if (cov >= 255) drow[i] = blend_px(drow[i], srow[i]);
                else if (cov > 0) drow[i] = blend_px_a(drow[i], srow[i], (uint8_t)cov);
            }
        }
    }
}

void resample_bilinear(const uint32_t* src, int sw, int sh, uint32_t* dst, int dw, int dh) {
    if (!src || !dst || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    int stk_x0[2048], stk_x1[2048], stk_wx[2048];
    int* ix0 = (dw <= 2048) ? stk_x0 : (int*)malloc((size_t)dw * sizeof(int));
    int* ix1 = (dw <= 2048) ? stk_x1 : (int*)malloc((size_t)dw * sizeof(int));
    int* wx  = (dw <= 2048) ? stk_wx : (int*)malloc((size_t)dw * sizeof(int));
    if (!ix0 || !ix1 || !wx) {
        if (ix0 != stk_x0) free(ix0);
        if (ix1 != stk_x1) free(ix1);
        if (wx  != stk_wx) free(wx);
        return;
    }

    float sx = (float)sw / (float)dw;
    float sy = (float)sh / (float)dh;
    for (int x = 0; x < dw; x++) {
        float fx = ((float)x + 0.5f) * sx - 0.5f;
        int x0 = (int)floorf(fx);
        int wxi;
        if (x0 < 0) { x0 = 0; wxi = 0; }
        else        { wxi = (int)((fx - (float)x0) * 256.0f + 0.5f); }
        if (x0 >= sw) x0 = sw - 1;
        int x1 = x0 + 1;
        if (x1 >= sw) { x1 = sw - 1; wxi = 0; }
        if (wxi < 0) wxi = 0; else if (wxi > 256) wxi = 256;
        ix0[x] = x0; ix1[x] = x1; wx[x] = wxi;
    }

    for (int y = 0; y < dh; y++) {
        float fy = ((float)y + 0.5f) * sy - 0.5f;
        int y0 = (int)floorf(fy);
        int wy;
        if (y0 < 0) { y0 = 0; wy = 0; }
        else        { wy = (int)((fy - (float)y0) * 256.0f + 0.5f); }
        if (y0 >= sh) y0 = sh - 1;
        int y1 = y0 + 1;
        if (y1 >= sh) { y1 = sh - 1; wy = 0; }
        if (wy < 0) wy = 0; else if (wy > 256) wy = 256;
        uint32_t uwy = (uint32_t)wy;
        uint32_t unwy = 256u - uwy;

        const uint32_t* sr0 = src + (size_t)y0 * sw;
        const uint32_t* sr1 = src + (size_t)y1 * sw;
        uint32_t* drow = dst + (size_t)y * dw;
        for (int x = 0; x < dw; x++) {
            int p0 = ix0[x], p1 = ix1[x];
            uint32_t uwx = (uint32_t)wx[x];
            uint32_t unwx = 256u - uwx;
            uint32_t c00 = sr0[p0], c10 = sr0[p1];
            uint32_t c01 = sr1[p0], c11 = sr1[p1];

            uint32_t top_rb = (((c00 & 0x00FF00FFu) * unwx + (c10 & 0x00FF00FFu) * uwx + 0x00800080u) >> 8) & 0x00FF00FFu;
            uint32_t bot_rb = (((c01 & 0x00FF00FFu) * unwx + (c11 & 0x00FF00FFu) * uwx + 0x00800080u) >> 8) & 0x00FF00FFu;
            uint32_t out_rb = ((top_rb * unwy + bot_rb * uwy + 0x00800080u) >> 8) & 0x00FF00FFu;

            uint32_t top_ag = ((((c00 >> 8) & 0x00FF00FFu) * unwx + ((c10 >> 8) & 0x00FF00FFu) * uwx + 0x00800080u) >> 8) & 0x00FF00FFu;
            uint32_t bot_ag = ((((c01 >> 8) & 0x00FF00FFu) * unwx + ((c11 >> 8) & 0x00FF00FFu) * uwx + 0x00800080u) >> 8) & 0x00FF00FFu;
            uint32_t out_ag = (top_ag * unwy + bot_ag * uwy + 0x00800080u) & 0xFF00FF00u;

            drow[x] = out_ag | out_rb;
        }
    }

    if (ix0 != stk_x0) free(ix0);
    if (ix1 != stk_x1) free(ix1);
    if (wx  != stk_wx) free(wx);
}

void box_blur(uint32_t* img, int w, int h, int radius, int passes) {
    if (!img || w <= 0 || h <= 0 || radius < 1) return;
    uint32_t* tmp = (uint32_t*)malloc((size_t)w * h * 4);
    if (!tmp) return;
    for (int p = 0; p < passes; p++) {
        for (int y = 0; y < h; y++) {
            uint32_t* row = img + (size_t)y * w;
            uint32_t* trow = tmp + (size_t)y * w;
            for (int x = 0; x < w; x++) {
                uint32_t sr = 0, sg = 0, sb = 0;
                int n = 0;
                for (int k = -radius; k <= radius; k++) {
                    int xx = x + k;
                    if (xx < 0 || xx >= w) continue;
                    sr += (row[xx]) & 0xFF; sg += (row[xx] >> 8) & 0xFF; sb += (row[xx] >> 16) & 0xFF;
                    n++;
                }
                trow[x] = 0xFF000000u | ((sr / n)) | ((sg / n) << 8) | ((sb / n) << 16);
            }
        }
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                uint32_t sr = 0, sg = 0, sb = 0;
                int n = 0;
                for (int k = -radius; k <= radius; k++) {
                    int yy = y + k;
                    if (yy < 0 || yy >= h) continue;
                    sr += (tmp[(size_t)yy * w + x]) & 0xFF; sg += (tmp[(size_t)yy * w + x] >> 8) & 0xFF; sb += (tmp[(size_t)yy * w + x] >> 16) & 0xFF;
                    n++;
                }
                img[(size_t)y * w + x] = 0xFF000000u | ((sr / n)) | ((sg / n) << 8) | ((sb / n) << 16);
            }
        }
    }
    free(tmp);
}

void scale_rgb(uint32_t* img, int n, float f) {
    for (int i = 0; i < n; i++) {
        uint32_t c = img[i];
        uint32_t r = (uint32_t)(((c) & 0xFF) * f);
        uint32_t g = (uint32_t)(((c >> 8) & 0xFF) * f);
        uint32_t b = (uint32_t)(((c >> 16) & 0xFF) * f);
        img[i] = 0xFF000000u | (r > 255 ? 255 : r) | ((g > 255 ? 255 : g) << 8) | ((b > 255 ? 255 : b) << 16);
    }
}

// ================= 快速抗锯齿原语 =================
static inline int px_in_clip(const Canvas* c, int x, int y) {
    int cx0, cy0, cx1, cy1;
    canvas_clip_bounds(c, &cx0, &cy0, &cx1, &cy1);
    return x >= cx0 && y >= cy0 && x < cx1 && y < cy1;
}

static inline void px_blend(Canvas* c, int x, int y, uint32_t col, uint8_t a) {
    if (a == 0 || !px_in_clip(c, x, y)) return;
    if (a >= 255) {
        c->px[(size_t)y * c->w + x] = col | 0xFF000000u;
        return;
    }
    uint32_t* d = &c->px[(size_t)y * c->w + x];
    uint32_t dst = *d;
    uint32_t ua = a;
    uint32_t na = 255u - ua;
    uint32_t rb = ((col & 0x00FF00FFu) * ua + (dst & 0x00FF00FFu) * na) >> 8;
    uint32_t g  = ((col & 0x0000FF00u) * ua + (dst & 0x0000FF00u) * na) >> 8;
    *d = 0xFF000000u | (rb & 0x00FF00FFu) | (g & 0x0000FF00u);
}

void fill_round_rect_aa_a_px(Canvas* c, int x, int y, int w, int h, int r, uint32_t col, uint8_t alpha);
void fill_round_rect_aa_px(Canvas* c, int x, int y, int w, int h, int r, uint32_t col) {
    uint8_t ca = (uint8_t)(col >> 24);
    fill_round_rect_aa_a_px(c, x, y, w, h, r, col, ca ? ca : 255);
}

void fill_round_rect_aa_a_px(Canvas* c, int x, int y, int w, int h, int r, uint32_t col, uint8_t alpha) {
    if (w <= 0 || h <= 0 || alpha == 0) return;
    if (r < 1) { fill_rect_a_px(c, x, y, w, h, col, alpha); return; }
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    fill_rect_a_px(c, x + r, y, w - 2 * r, h, col, alpha);
    fill_rect_a_px(c, x, y + r, r, h - 2 * r, col, alpha);
    fill_rect_a_px(c, x + w - r, y + r, r, h - 2 * r, col, alpha);

    uint32_t opaque = col | 0xFF000000u;
    float r_in2 = (r > 1.5f) ? ((float)r - 1.5f) * ((float)r - 1.5f) : 0.0f;
    float r_out = (float)r + 0.5f;
    float r_out2 = r_out * r_out;

    for (int j = 0; j < r; j++) {
        float dy = (float)(r - j) - 0.5f;
        float dy2 = dy * dy;
        for (int i = 0; i < r; i++) {
            float dx = (float)(r - i) - 0.5f;
            float d2 = dx * dx + dy2;
            if (d2 >= r_out2) continue;
            uint8_t a;
            if (d2 <= r_in2) {
                a = alpha;
            } else {
                float cov = (float)r - sqrtf(d2) + 0.5f;
                if (cov <= 0.0f) continue;
                if (cov > 1.0f) cov = 1.0f;
                a = (uint8_t)(cov * (float)alpha + 0.5f);
            }
            if (a == 0) continue;
            if (a >= 255) {
                if (px_in_clip(c, x + i, y + j))                 c->px[(size_t)(y + j) * c->w + (x + i)] = opaque;
                if (px_in_clip(c, x + w - 1 - i, y + j))         c->px[(size_t)(y + j) * c->w + (x + w - 1 - i)] = opaque;
                if (px_in_clip(c, x + i, y + h - 1 - j))         c->px[(size_t)(y + h - 1 - j) * c->w + (x + i)] = opaque;
                if (px_in_clip(c, x + w - 1 - i, y + h - 1 - j)) c->px[(size_t)(y + h - 1 - j) * c->w + (x + w - 1 - i)] = opaque;
            } else {
                px_blend(c, x + i,         y + j,         col, a);
                px_blend(c, x + w - 1 - i, y + j,         col, a);
                px_blend(c, x + i,         y + h - 1 - j, col, a);
                px_blend(c, x + w - 1 - i, y + h - 1 - j, col, a);
            }
        }
    }
}

void stroke_round_rect_aa_px(Canvas* c, int x, int y, int w, int h, int r, int thickness, uint32_t col, uint8_t alpha) {
    if (w <= 0 || h <= 0 || thickness <= 0 || alpha == 0) return;
    if (r < 1) r = 1;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (thickness > r) thickness = r;

    fill_rect_a_px(c, x + r, y, w - 2 * r, thickness, col, alpha);
    fill_rect_a_px(c, x + r, y + h - thickness, w - 2 * r, thickness, col, alpha);
    fill_rect_a_px(c, x, y + r, thickness, h - 2 * r, col, alpha);
    fill_rect_a_px(c, x + w - thickness, y + r, thickness, h - 2 * r, col, alpha);

    float r_out = (float)r;
    float r_in = (float)(r - thickness);
    for (int j = 0; j < r; j++) {
        float dy = (float)(r - j) - 0.5f;
        for (int i = 0; i < r; i++) {
            float dx = (float)(r - i) - 0.5f;
            float d = sqrtf(dx * dx + dy * dy);
            float cov_out = r_out - d + 0.5f;
            if (cov_out <= 0.0f) continue;
            if (cov_out > 1.0f) cov_out = 1.0f;
            float cov_in = r_in - d + 0.5f;
            if (cov_in < 0.0f) cov_in = 0.0f;
            if (cov_in > 1.0f) cov_in = 1.0f;
            float ring = cov_out - cov_in;
            if (ring <= 0.0f) continue;
            uint8_t a = (uint8_t)(ring * (float)alpha + 0.5f);
            if (a == 0) continue;
            px_blend(c, x + i,         y + j,         col, a);
            px_blend(c, x + w - 1 - i, y + j,         col, a);
            px_blend(c, x + i,         y + h - 1 - j, col, a);
            px_blend(c, x + w - 1 - i, y + h - 1 - j, col, a);
        }
    }
}

// 阴影快速绘制：自动跳过被上层不透明卡片完全遮挡的内部像素，耗时降低 20~30 倍
static void fill_round_layer_skip(Canvas* c, int sx, int sy, int sw, int sh, int sr, uint8_t alpha,
                                  int hx, int hy, int hw, int hh) {
    if (sw <= 0 || sh <= 0 || alpha == 0) return;
    if (sr > sw / 2) sr = sw / 2;
    if (sr > sh / 2) sr = sh / 2;
    if (hw <= 0 || hh <= 0) {
        fill_round_rect_aa_a_px(c, sx, sy, sw, sh, sr, COL(0, 0, 0, 255), alpha);
        return;
    }
    uint32_t ua = alpha;
    uint32_t na = 255u - ua;
    int hy1 = hy + hh;
    int hx1 = hx + hw;

    int cx0, cy0, cx1, cy1;
    canvas_clip_bounds(c, &cx0, &cy0, &cx1, &cy1);
    int y0 = sy < cy0 ? cy0 : sy;
    int y1 = (sy + sh > cy1) ? cy1 : (sy + sh);
    for (int y = y0; y < y1; y++) {
        int j = y - sy;
        int in_corner_y = (j < sr || j >= sh - sr);
        if (!in_corner_y) {
            if (y >= hy && y < hy1) {
                hline_a(c, sx, y, hx - sx, 0, 0, na);
                hline_a(c, hx1, y, (sx + sw) - hx1, 0, 0, na);
            } else {
                hline_a(c, sx, y, sw, 0, 0, na);
            }
        } else {
            int dy_c = (j < sr) ? (sr - j) : (j - (sh - 1 - sr));
            float fy = (float)dy_c - 0.5f;
            float max_dx2 = ((float)sr + 0.5f) * ((float)sr + 0.5f) - fy * fy;
            if (max_dx2 <= 0.0f) continue;
            int arc_w = (int)floorf(sqrtf(max_dx2));
            int row_x0 = sx + sr - arc_w;
            int row_x1 = sx + sw - sr + arc_w;
            if (row_x0 < sx) row_x0 = sx;
            if (row_x1 > sx + sw) row_x1 = sx + sw;

            int left_end = sx + sr;
            int right_beg = sx + sw - sr;
            for (int x = row_x0; x < left_end; x++) {
                float dx = (float)(sr - (x - sx)) - 0.5f;
                float cov = (float)sr - sqrtf(dx * dx + fy * fy) + 0.5f;
                if (cov <= 0.0f) continue;
                if (cov > 1.0f) cov = 1.0f;
                px_blend(c, x, y, 0, (uint8_t)(cov * (float)alpha + 0.5f));
            }
            if (y >= hy && y < hy1) {
                hline_a(c, left_end, y, hx - left_end, 0, 0, na);
                hline_a(c, hx1, y, right_beg - hx1, 0, 0, na);
            } else {
                hline_a(c, left_end, y, right_beg - left_end, 0, 0, na);
            }
            for (int x = right_beg; x < row_x1; x++) {
                float dx = (float)((x - sx) - (sw - 1 - sr)) - 0.5f;
                float cov = (float)sr - sqrtf(dx * dx + fy * fy) + 0.5f;
                if (cov <= 0.0f) continue;
                if (cov > 1.0f) cov = 1.0f;
                px_blend(c, x, y, 0, (uint8_t)(cov * (float)alpha + 0.5f));
            }
        }
    }
}

void fill_round_rect_shadow_px(Canvas* c, int x, int y, int w, int h, int r, uint8_t max_alpha) {
    if (max_alpha == 0) return;
    float k = c->s > 0.0f ? c->s : 1.0f;
    int k1 = (int)(1 * k + 0.5f), k2 = (int)(2 * k + 0.5f), k3 = (int)(3 * k + 0.5f);
    int k4 = (int)(4 * k + 0.5f), k6 = (int)(6 * k + 0.5f), k8 = (int)(8 * k + 0.5f);
    int inset = (r + 1) / 2;
    if (inset < k4) inset = k4;
    int hx = x + inset, hy = y + inset, hw = w - 2 * inset, hh = h - 2 * inset;
    fill_round_layer_skip(c, x - k4, y + k6, w + k8, h + k8, r + k4, (uint8_t)(max_alpha / 3), hx, hy, hw, hh);
    fill_round_layer_skip(c, x - k1, y + k4, w + k2, h + k4, r + k2, (uint8_t)(max_alpha / 2), hx, hy, hw, hh);
    fill_round_layer_skip(c, x + k2, y + k3, w - k2, h,      r,      max_alpha,                  hx, hy, hw, hh);
}

void stroke_circle_aa_px(Canvas* c, int cx, int cy, int r, int thickness, uint32_t col, uint8_t alpha) {
    if (r < 1 || thickness < 1 || alpha == 0) return;
    float r_out = (float)r;
    float r_in = (float)(r - thickness);
    float r_max = r_out + 0.6f;
    float r_min = (r_in > 0.6f) ? (r_in - 0.6f) : 0.0f;
    float r_max2 = r_max * r_max;
    float r_min2 = r_min * r_min;

    int cx0, cy0, cx1, cy1;
    canvas_clip_bounds(c, &cx0, &cy0, &cx1, &cy1);
    int j0 = (cy - r - 1 < cy0) ? (cy0 - cy) : (-r - 1);
    int j1 = (cy + r + 1 >= cy1) ? (cy1 - 1 - cy) : (r + 1);
    for (int j = j0; j <= j1; j++) {
        int y = cy + j;
        float j2 = (float)(j * j);
        if (j2 > r_max2) continue;
        int x_out = (int)ceilf(sqrtf(r_max2 - j2));
        int x_in = (j2 < r_min2) ? (int)floorf(sqrtf(r_min2 - j2)) : 0;

        for (int side = -1; side <= 1; side += 2) {
            int start = (x_in == 0 && side == 1) ? 1 : x_in;
            for (int k = start; k <= x_out; k++) {
                int i = side * k;
                int x = cx + i;
                if (x < 0 || x >= c->w) continue;
                float d = sqrtf((float)(i * i) + j2);
                float cov_out = r_out - d + 0.5f;
                if (cov_out <= 0.0f) continue;
                if (cov_out > 1.0f) cov_out = 1.0f;
                float cov_in = r_in - d + 0.5f;
                if (cov_in < 0.0f) cov_in = 0.0f;
                if (cov_in > 1.0f) cov_in = 1.0f;
                float ring = cov_out - cov_in;
                if (ring <= 0.0f) continue;
                uint8_t aa = (uint8_t)(ring * (float)alpha + 0.5f);
                if (aa > 0) px_blend(c, x, y, col, aa);
            }
        }
    }
}

static inline double tri_edge_dist(double ax, double ay, double bx, double by,
                                   double px, double py) {
    double ex = bx - ax, ey = by - ay;
    double len = sqrt(ex * ex + ey * ey);
    if (len < 1e-9) return 1e9;
    return (ex * (py - ay) - ey * (px - ax)) / len;
}

void fill_tri_aa_px(Canvas* c, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t col) {
    double area = (double)(x1 - x0) * (y2 - y0) - (double)(x2 - x0) * (y1 - y0);
    if (area == 0) return;
    double sgn = area > 0 ? 1.0 : -1.0;
    int minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
    int maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    int miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    int cx0, cy0, cx1, cy1;
    canvas_clip_bounds(c, &cx0, &cy0, &cx1, &cy1);
    if (minx < cx0) minx = cx0;
    if (miny < cy0) miny = cy0;
    if (maxx >= cx1) maxx = cx1 - 1;
    if (maxy >= cy1) maxy = cy1 - 1;
    for (int y = miny; y <= maxy; y++) {
        double py = y + 0.5;
        for (int x = minx; x <= maxx; x++) {
            double px = x + 0.5;
            double d0 = tri_edge_dist(x0, y0, x1, y1, px, py) * sgn;
            double d1 = tri_edge_dist(x1, y1, x2, y2, px, py) * sgn;
            double d2 = tri_edge_dist(x2, y2, x0, y0, px, py) * sgn;
            double dmin = d0 < d1 ? d0 : d1;
            if (d2 < dmin) dmin = d2;
            double cov = 0.5 + dmin;
            if (cov <= 0) continue;
            if (cov > 1) cov = 1;
            px_blend(c, x, y, col, (uint8_t)(cov * 255.0 + 0.5));
        }
    }
}

void fill_ellipse_aa_px(Canvas* c, int cx, int cy, int rx, int ry, uint32_t col) {
    if (rx < 1 || ry < 1) return;
    float minr = (float)(rx < ry ? rx : ry);
    float d_in = 1.0f - 0.65f / minr;
    float d_out = 1.0f + 0.65f / minr;
    float din2 = d_in > 0.0f ? d_in * d_in : 0.0f;
    float dout2 = d_out * d_out;
    uint32_t ca = (col >> 24) & 0xFFu;
    uint8_t ga = ca ? (uint8_t)ca : 255u;
    uint32_t opaque = col | 0xFF000000u;

    int cx0, cy0, cx1, cy1;
    canvas_clip_bounds(c, &cx0, &cy0, &cx1, &cy1);
    int j0 = (cy - ry < cy0) ? (cy0 - cy) : -ry;
    int j1 = (cy + ry >= cy1) ? (cy1 - 1 - cy) : ry;
    for (int j = j0; j <= j1; j++) {
        int y = cy + j;
        float jy = (float)j / (float)ry;
        float jy2 = jy * jy;
        float rem_out = dout2 - jy2;
        if (rem_out <= 0.0f) continue;
        int x_out = (int)ceilf((float)rx * sqrtf(rem_out));
        if (x_out > rx) x_out = rx;
        float rem_in = din2 - jy2;
        int x_in = (rem_in > 0.0f) ? (int)floorf((float)rx * sqrtf(rem_in)) : -1;

        if (x_in >= 0) {
            int lx = cx - x_in; if (lx < cx0) lx = cx0;
            int rx_c = cx + x_in; if (rx_c >= cx1) rx_c = cx1 - 1;
            if (ga >= 255) {
                uint32_t* row = c->px + (size_t)y * c->w;
                for (int x = lx; x <= rx_c; x++) row[x] = opaque;
            } else {
                for (int x = lx; x <= rx_c; x++) px_blend(c, x, y, col, ga);
            }
        }
        int e_start = x_in + 1;
        for (int k = e_start; k <= x_out; k++) {
            float ix = (float)k / (float)rx;
            float cov = minr * (1.0f - sqrtf(ix * ix + jy2)) + 0.5f;
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            uint8_t aa = (uint8_t)((cov * (float)ga) + 0.5f);
            px_blend(c, cx - k, y, col, aa);
            if (k > 0) px_blend(c, cx + k, y, col, aa);
        }
    }
}

void fill_circle_a_px(Canvas* c, int cx, int cy, int r, uint32_t col, uint8_t a) {
    if (r < 1 || a == 0) return;
    if (a == 255) { fill_ellipse_aa_px(c, cx, cy, r, r, col); return; }

    float r_in = (r > 1.0f) ? ((float)r - 0.65f) : 0.0f;
    float r_out = (float)r + 0.65f;
    float rin2 = r_in * r_in;
    float rout2 = r_out * r_out;
    uint32_t ua = a, na = 255u - ua;
    uint32_t s_rb = (col & 0x00FF00FFu) * ua;
    uint32_t s_g  = (col & 0x0000FF00u) * ua;

    int cx0, cy0, cx1, cy1;
    canvas_clip_bounds(c, &cx0, &cy0, &cx1, &cy1);
    int j0 = (cy - r - 1 < cy0) ? (cy0 - cy) : (-r - 1);
    int j1 = (cy + r + 1 >= cy1) ? (cy1 - 1 - cy) : (r + 1);
    for (int j = j0; j <= j1; j++) {
        int y = cy + j;
        float j2 = (float)(j * j);
        if (j2 >= rout2) continue;
        int x_out = (int)ceilf(sqrtf(rout2 - j2));
        int x_in = (j2 < rin2) ? (int)floorf(sqrtf(rin2 - j2)) : -1;

        if (x_in >= 0) {
            hline_a(c, cx - x_in, y, 2 * x_in + 1, s_rb, s_g, na);
        }
        int e_start = x_in + 1;
        for (int k = e_start; k <= x_out; k++) {
            float cov = (float)r - sqrtf((float)(k * k) + j2) + 0.5f;
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            uint8_t aa = (uint8_t)(cov * (float)a + 0.5f);
            if (aa > 0) {
                px_blend(c, cx - k, y, col, aa);
                if (k > 0) px_blend(c, cx + k, y, col, aa);
            }
        }
    }
}

void fill_circle_clip_a_px(Canvas* c, int cx, int cy, int r, uint32_t col, uint8_t a,
                        int clipx, int clipy, int clipw, int cliph) {
    if (r < 1 || a == 0 || clipw <= 0 || cliph <= 0) return;
    int x0 = cx - r > clipx ? cx - r : clipx;
    int y0 = cy - r > clipy ? cy - r : clipy;
    int x1 = cx + r < clipx + clipw - 1 ? cx + r : clipx + clipw - 1;
    int y1 = cy + r < clipy + cliph - 1 ? cy + r : clipy + cliph - 1;
    for (int j = y0; j <= y1; j++) {
        for (int i = x0; i <= x1; i++) {
            float d = sqrtf((float)((i - cx) * (i - cx) + (j - cy) * (j - cy)));
            float cov = (float)r - d + 0.5f;
            if (cov <= 0) continue;
            if (cov > 1) cov = 1;
            uint8_t aa = (uint8_t)(cov * (float)a + 0.5f);
            if (aa > 0) px_blend(c, i, j, col, aa);
        }
    }
}

void draw_m3_wavy_progress_px(Canvas* c, int x0, int x1, int mid_y, float frac,
                           float phase, float wave_amp, float wave_period,
                           int wave_thick, int track_thick, int draw_thumb,
                           uint32_t col_active, uint32_t col_track) {
    if (!c || x1 <= x0 + 2 || wave_thick < 1) return;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    if (wave_amp < 0.0f) wave_amp = 0.0f;
    if (track_thick < 2) track_thick = 2;

    int knob_x = x0 + (int)(frac * (float)(x1 - x0) + 0.5f);
    int gap = draw_thumb ? 7 : 2;
    int track_start = knob_x + gap;
    int wave_end = knob_x - gap;

    // 1. 未播放段圆角底轨
    if (track_start < x1) {
        fill_round_rect_aa_px(c, track_start, mid_y - track_thick / 2,
                           x1 - track_start, track_thick, track_thick / 2, col_track);
    }

    // 2. 已播放段 Material You 粗波浪进度带（法向等距抗锯齿 + 两端柔和收敛圆头）
    if (wave_end > x0) {
        if (wave_amp < 0.35f) {
            fill_round_rect_aa_px(c, x0, mid_y - wave_thick / 2,
                               wave_end - x0 + 1, wave_thick, wave_thick / 2, col_active);
        } else {
            float r = (float)wave_thick * 0.5f;
            float freq = (wave_period > 2.0f) ? (6.2831853f / wave_period) : 0.22f;
            int cx0, cy0, cx1, cy1;
            canvas_clip_bounds(c, &cx0, &cy0, &cx1, &cy1);

            int pad = (int)ceilf(r) + 1;
            int xmin = x0 - pad;
            int xmax = wave_end + pad;
            if (xmin < cx0) xmin = cx0;
            if (xmax >= cx1) xmax = cx1 - 1;

            int ymin = mid_y - (int)ceilf(wave_amp + r) - 2;
            int ymax = mid_y + (int)ceilf(wave_amp + r) + 2;
            if (ymin < cy0) ymin = cy0;
            if (ymax >= cy1) ymax = cy1 - 1;

            uint32_t opaque = col_active | 0xFF000000u;
            for (int x = xmin; x <= xmax; x++) {
                float yc = (float)mid_y;
                float inv_n = 1.0f;
                if (x >= x0 && x <= wave_end) {
                    float d_left = (float)(x - x0);
                    float d_right = (float)(wave_end - x);
                    float edge = d_left < d_right ? d_left : d_right;
                    float env = edge / 7.0f;
                    float env_d = 0.0f;
                    if (env >= 1.0f) {
                        env = 1.0f;
                    } else if (env <= 0.0f) {
                        env = 0.0f;
                    } else {
                        float t = env;
                        env = t * t * (3.0f - 2.0f * t);
                        env_d = 6.0f * t * (1.0f - t) / 7.0f * (d_left < d_right ? 1.0f : -1.0f);
                    }
                    float ang = d_left * freq - phase;
                    float s = sinf(ang);
                    float cs = cosf(ang);
                    yc = (float)mid_y + wave_amp * env * s;
                    float dy_dx = wave_amp * (env * freq * cs + env_d * s);
                    inv_n = sqrtf(1.0f + dy_dx * dy_dx);
                }

                float ry = (r + 0.7f) * inv_n;
                int ylo = (int)floorf(yc - ry);
                int yhi = (int)ceilf(yc + ry);
                if (ylo < ymin) ylo = ymin;
                if (yhi > ymax) yhi = ymax;

                for (int y = ylo; y <= yhi; y++) {
                    float dist;
                    if (x < x0) {
                        float dx = (float)x - (float)x0;
                        float dy = (float)y + 0.5f - (float)mid_y;
                        dist = sqrtf(dx * dx + dy * dy);
                    } else if (x > wave_end) {
                        float dx = (float)x - (float)wave_end;
                        float dy = (float)y + 0.5f - (float)mid_y;
                        dist = sqrtf(dx * dx + dy * dy);
                    } else {
                        dist = fabsf((float)y + 0.5f - yc) / inv_n;
                    }
                    float cov = r + 0.55f - dist;
                    if (cov <= 0.0f) continue;
                    if (cov >= 1.0f) {
                        c->px[(size_t)y * c->w + x] = opaque;
                    } else {
                        px_blend(c, x, y, col_active, (uint8_t)(cov * 255.0f + 0.5f));
                    }
                }
            }
        }
    }

    // 3. Material You 竖向圆角胶囊游标滑块
    if (draw_thumb) {
        float k = c->s > 0.0f ? c->s : 1.0f;
        int tw = (int)(6 * k + 0.5f), th = (int)(24 * k + 0.5f);
        if (tw < 4) tw = 4;
        if (th < 12) th = 12;
        fill_round_rect_aa_px(c, knob_x - tw / 2, mid_y - th / 2, tw, th, tw / 2, col_active);
    }
}

// ================= 逻辑坐标 -> 物理像素缩放包装层 =================
static inline int sc_i(float s, int v) {
    return (int)floorf((float)v * s + 0.5f);
}

static inline int sc_sz(float s, int v) {
    if (v <= 0) return 0;
    int r = (int)floorf((float)v * s + 0.5f);
    return r < 1 ? 1 : r;
}

static void blit_scaled_round_px(Canvas* dst, int dx, int dy, int dw, int dh,
                                 const uint32_t* src, int sw, int sh,
                                 int corner, uint8_t ga, int opaque_src) {
    if (!dst || !src || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0 || ga == 0) return;
    if (corner > dw / 2) corner = dw / 2;
    if (corner > dh / 2) corner = dh / 2;
    int cx0, cy0, cx1, cy1;
    canvas_clip_bounds(dst, &cx0, &cy0, &cx1, &cy1);
    int x0 = dx < cx0 ? cx0 - dx : 0;
    int y0 = dy < cy0 ? cy0 - dy : 0;
    int x1 = (dx + dw > cx1) ? cx1 - dx : dw;
    int y1 = (dy + dh > cy1) ? cy1 - dy : dh;
    int span = x1 - x0;
    if (span <= 0 || y1 <= y0) return;

    int stk_x0[2048], stk_x1[2048], stk_wx[2048];
    int* ix0 = (span <= 2048) ? stk_x0 : (int*)malloc((size_t)span * sizeof(int));
    int* ix1 = (span <= 2048) ? stk_x1 : (int*)malloc((size_t)span * sizeof(int));
    int* wx  = (span <= 2048) ? stk_wx : (int*)malloc((size_t)span * sizeof(int));
    if (!ix0 || !ix1 || !wx) {
        if (ix0 != stk_x0) free(ix0);
        if (ix1 != stk_x1) free(ix1);
        if (wx  != stk_wx) free(wx);
        return;
    }

    float sx = (float)sw / (float)dw;
    float sy = (float)sh / (float)dh;
    for (int k = 0; k < span; k++) {
        int i = x0 + k;
        float fx = ((float)i + 0.5f) * sx - 0.5f;
        int x_a = (int)floorf(fx);
        int wxi;
        if (x_a < 0) { x_a = 0; wxi = 0; }
        else         { wxi = (int)((fx - (float)x_a) * 256.0f + 0.5f); }
        if (x_a >= sw) x_a = sw - 1;
        int x_b = x_a + 1;
        if (x_b >= sw) { x_b = sw - 1; wxi = 0; }
        if (wxi < 0) wxi = 0; else if (wxi > 256) wxi = 256;
        ix0[k] = x_a; ix1[k] = x_b; wx[k] = wxi;
    }

    for (int j = y0; j < y1; j++) {
        float fy = ((float)j + 0.5f) * sy - 0.5f;
        int iy0 = (int)floorf(fy);
        int wy;
        if (iy0 < 0) { iy0 = 0; wy = 0; }
        else         { wy = (int)((fy - (float)iy0) * 256.0f + 0.5f); }
        if (iy0 >= sh) iy0 = sh - 1;
        int iy1 = iy0 + 1;
        if (iy1 >= sh) { iy1 = sh - 1; wy = 0; }
        if (wy < 0) wy = 0; else if (wy > 256) wy = 256;
        uint32_t uwy = (uint32_t)wy;
        uint32_t unwy = 256u - uwy;

        const uint32_t* sr0 = src + (size_t)iy0 * sw;
        const uint32_t* sr1 = src + (size_t)iy1 * sw;
        uint32_t* drow = dst->px + (size_t)(dy + j) * dst->w + dx;

        int cy_dist = 0;
        if (corner > 0) {
            if (j < corner) cy_dist = corner - j;
            else if (j >= dh - corner) cy_dist = j - (dh - 1 - corner);
        }

        if (cy_dist == 0 && opaque_src && ga == 255) {
            for (int k = 0; k < span; k++) {
                int p0 = ix0[k], p1 = ix1[k];
                uint32_t uwx = (uint32_t)wx[k], unwx = 256u - uwx;
                uint32_t c00 = sr0[p0], c10 = sr0[p1], c01 = sr1[p0], c11 = sr1[p1];
                uint32_t top_rb = (((c00 & 0x00FF00FFu) * unwx + (c10 & 0x00FF00FFu) * uwx + 0x00800080u) >> 8) & 0x00FF00FFu;
                uint32_t bot_rb = (((c01 & 0x00FF00FFu) * unwx + (c11 & 0x00FF00FFu) * uwx + 0x00800080u) >> 8) & 0x00FF00FFu;
                uint32_t out_rb = ((top_rb * unwy + bot_rb * uwy + 0x00800080u) >> 8) & 0x00FF00FFu;
                uint32_t top_g  = ((((c00 >> 8) & 0xFFu) * unwx + ((c10 >> 8) & 0xFFu) * uwx + 128u) >> 8);
                uint32_t bot_g  = ((((c01 >> 8) & 0xFFu) * unwx + ((c11 >> 8) & 0xFFu) * uwx + 128u) >> 8);
                uint32_t out_g  = (top_g * unwy + bot_g * uwy + 128u) & 0x0000FF00u;
                drow[x0 + k] = 0xFF000000u | out_g | out_rb;
            }
            continue;
        }

        int mid_s = (cy_dist == 0) ? x0 : (x0 < corner ? corner : x0);
        int mid_e = (cy_dist == 0) ? x1 : (x1 > dw - corner ? dw - corner : x1);
        for (int i = x0; i < x1; i++) {
            int cov = 255;
            if (cy_dist > 0 && (i < mid_s || i >= mid_e)) {
                if (i < corner) cov = corner_cov(corner - i, cy_dist, corner);
                else            cov = corner_cov(i - (dw - 1 - corner), cy_dist, corner);
                if (cov == 0) continue;
            }
            int k = i - x0;
            int p0 = ix0[k], p1 = ix1[k];
            uint32_t uwx = (uint32_t)wx[k], unwx = 256u - uwx;
            uint32_t c00 = sr0[p0], c10 = sr0[p1], c01 = sr1[p0], c11 = sr1[p1];

            uint32_t top_rb = (((c00 & 0x00FF00FFu) * unwx + (c10 & 0x00FF00FFu) * uwx + 0x00800080u) >> 8) & 0x00FF00FFu;
            uint32_t bot_rb = (((c01 & 0x00FF00FFu) * unwx + (c11 & 0x00FF00FFu) * uwx + 0x00800080u) >> 8) & 0x00FF00FFu;
            uint32_t out_rb = ((top_rb * unwy + bot_rb * uwy + 0x00800080u) >> 8) & 0x00FF00FFu;

            uint32_t top_ag = ((((c00 >> 8) & 0x00FF00FFu) * unwx + ((c10 >> 8) & 0x00FF00FFu) * uwx + 0x00800080u) >> 8) & 0x00FF00FFu;
            uint32_t bot_ag = ((((c01 >> 8) & 0x00FF00FFu) * unwx + ((c11 >> 8) & 0x00FF00FFu) * uwx + 0x00800080u) >> 8) & 0x00FF00FFu;
            uint32_t out_ag = (top_ag * unwy + bot_ag * uwy + 0x00800080u) & 0xFF00FF00u;

            uint32_t px_col = opaque_src ? (0xFF000000u | (out_ag & 0x0000FF00u) | out_rb) : (out_ag | out_rb);
            uint32_t eff = (ga < 255) ? ((uint32_t)cov * ga / 255u) : (uint32_t)cov;
            if (eff >= 255u && opaque_src) drow[i] = px_col;
            else if (eff >= 255u) drow[i] = blend_px(drow[i], px_col);
            else if (eff > 0u) drow[i] = blend_px_a(drow[i], px_col, (uint8_t)eff);
        }
    }

    if (ix0 != stk_x0) free(ix0);
    if (ix1 != stk_x1) free(ix1);
    if (wx  != stk_wx) free(wx);
}

void canvas_set_clip(Canvas* c, int x0, int y0, int x1, int y1) {
    if (!c) return;
    if (fabsf(c->s - 1.0f) < 0.001f) canvas_set_clip_px(c, x0, y0, x1, y1);
    else canvas_set_clip_px(c, sc_i(c->s, x0), sc_i(c->s, y0), sc_i(c->s, x1), sc_i(c->s, y1));
}
void fill_rect(Canvas* c, int x, int y, int w, int h, uint32_t col) {
    if (!c) return;
    if (fabsf(c->s - 1.0f) < 0.001f) fill_rect_px(c, x, y, w, h, col);
    else fill_rect_px(c, sc_i(c->s, x), sc_i(c->s, y), sc_i(c->s, x + w) - sc_i(c->s, x), sc_i(c->s, y + h) - sc_i(c->s, y), col);
}
void fill_rect_a(Canvas* c, int x, int y, int w, int h, uint32_t col, uint8_t alpha) {
    if (!c) return;
    if (fabsf(c->s - 1.0f) < 0.001f) fill_rect_a_px(c, x, y, w, h, col, alpha);
    else fill_rect_a_px(c, sc_i(c->s, x), sc_i(c->s, y), sc_i(c->s, x + w) - sc_i(c->s, x), sc_i(c->s, y + h) - sc_i(c->s, y), col, alpha);
}
void hline(Canvas* c, int x, int y, int len, uint32_t col) {
    fill_rect(c, x, y, len, 1, col);
}
void vline(Canvas* c, int x, int y, int len, uint32_t col) {
    fill_rect(c, x, y, 1, len, col);
}
void fill_tri(Canvas* c, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t col) {
    if (!c) return;
    if (fabsf(c->s - 1.0f) < 0.001f) fill_tri_px(c, x0, y0, x1, y1, x2, y2, col);
    else fill_tri_px(c, sc_i(c->s, x0), sc_i(c->s, y0), sc_i(c->s, x1), sc_i(c->s, y1), sc_i(c->s, x2), sc_i(c->s, y2), col);
}
void blit_rgba(Canvas* dst, int dx, int dy, const uint32_t* src, int sw, int sh) {
    if (!dst) return;
    if (fabsf(dst->s - 1.0f) < 0.001f) blit_rgba_px(dst, dx, dy, src, sw, sh);
    else blit_scaled_round_px(dst, sc_i(dst->s, dx), sc_i(dst->s, dy), sc_sz(dst->s, sw), sc_sz(dst->s, sh), src, sw, sh, 0, 255, 0);
}
void blit_canvas_opaque(Canvas* dst, int dx, int dy, const Canvas* src) {
    if (!dst || !src) return;
    if (fabsf(dst->s - 1.0f) < 0.001f) blit_canvas_opaque_px(dst, dx, dy, src);
    else blit_scaled_round_px(dst, sc_i(dst->s, dx), sc_i(dst->s, dy), sc_sz(dst->s, src->w), sc_sz(dst->s, src->h), src->px, src->w, src->h, 0, 255, 1);
}
void blit_rgba_a(Canvas* dst, int dx, int dy, const uint32_t* src, int sw, int sh, uint8_t a) {
    if (!dst) return;
    if (fabsf(dst->s - 1.0f) < 0.001f) blit_rgba_a_px(dst, dx, dy, src, sw, sh, a);
    else blit_scaled_round_px(dst, sc_i(dst->s, dx), sc_i(dst->s, dy), sc_sz(dst->s, sw), sc_sz(dst->s, sh), src, sw, sh, 0, a, 0);
}
void blit_rgb_round(Canvas* dst, int dx, int dy, const uint32_t* src, int sw, int sh, int corner) {
    if (!dst) return;
    if (fabsf(dst->s - 1.0f) < 0.001f) blit_rgb_round_px(dst, dx, dy, src, sw, sh, corner);
    else blit_scaled_round_px(dst, sc_i(dst->s, dx), sc_i(dst->s, dy), sc_sz(dst->s, sw), sc_sz(dst->s, sh), src, sw, sh, sc_i(dst->s, corner), 255, 1);
}
void blit_rgb_round_fit(Canvas* dst, int dx, int dy, int dw, int dh,
                        const uint32_t* src, int sw, int sh, int corner) {
    if (!dst || !src || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    float s = dst->s > 0.0f ? dst->s : 1.0f;
    if (fabsf(s - 1.0f) < 0.001f) {
        if (dw == sw && dh == sh) blit_rgb_round_px(dst, dx, dy, src, sw, sh, corner);
        else blit_scaled_round_px(dst, dx, dy, dw, dh, src, sw, sh, corner, 255, 1);
    } else {
        int px = sc_i(s, dx), py = sc_i(s, dy);
        int pdw = sc_sz(s, dw), pdh = sc_sz(s, dh), pr = sc_i(s, corner);
        if (pdw == sw && pdh == sh) blit_rgb_round_px(dst, px, py, src, sw, sh, pr);
        else blit_scaled_round_px(dst, px, py, pdw, pdh, src, sw, sh, pr, 255, 1);
    }
}
void blit_rgba_round_a(Canvas* dst, int dx, int dy, const uint32_t* src, int sw, int sh, int corner, uint8_t a) {
    if (!dst) return;
    if (fabsf(dst->s - 1.0f) < 0.001f) blit_rgba_round_a_px(dst, dx, dy, src, sw, sh, corner, a);
    else blit_scaled_round_px(dst, sc_i(dst->s, dx), sc_i(dst->s, dy), sc_sz(dst->s, sw), sc_sz(dst->s, sh), src, sw, sh, sc_i(dst->s, corner), a, 0);
}
void blit_rgba_round(Canvas* dst, int dx, int dy, const uint32_t* src, int sw, int sh, int corner) {
    if (!dst) return;
    if (fabsf(dst->s - 1.0f) < 0.001f) blit_rgba_round_px(dst, dx, dy, src, sw, sh, corner);
    else blit_scaled_round_px(dst, sc_i(dst->s, dx), sc_i(dst->s, dy), sc_sz(dst->s, sw), sc_sz(dst->s, sh), src, sw, sh, sc_i(dst->s, corner), 255, 0);
}
void fill_round_rect_aa(Canvas* c, int x, int y, int w, int h, int r, uint32_t col) {
    if (!c) return;
    if (fabsf(c->s - 1.0f) < 0.001f) fill_round_rect_aa_px(c, x, y, w, h, r, col);
    else fill_round_rect_aa_px(c, sc_i(c->s, x), sc_i(c->s, y), sc_sz(c->s, w), sc_sz(c->s, h), sc_i(c->s, r), col);
}
void fill_round_rect_aa_a(Canvas* c, int x, int y, int w, int h, int r, uint32_t col, uint8_t alpha) {
    if (!c) return;
    if (fabsf(c->s - 1.0f) < 0.001f) fill_round_rect_aa_a_px(c, x, y, w, h, r, col, alpha);
    else fill_round_rect_aa_a_px(c, sc_i(c->s, x), sc_i(c->s, y), sc_sz(c->s, w), sc_sz(c->s, h), sc_i(c->s, r), col, alpha);
}
void stroke_round_rect_aa(Canvas* c, int x, int y, int w, int h, int r, int thickness, uint32_t col, uint8_t alpha) {
    if (!c) return;
    if (fabsf(c->s - 1.0f) < 0.001f) stroke_round_rect_aa_px(c, x, y, w, h, r, thickness, col, alpha);
    else stroke_round_rect_aa_px(c, sc_i(c->s, x), sc_i(c->s, y), sc_sz(c->s, w), sc_sz(c->s, h), sc_i(c->s, r), sc_sz(c->s, thickness), col, alpha);
}
void fill_round_rect_shadow(Canvas* c, int x, int y, int w, int h, int r, uint8_t max_alpha) {
    if (!c) return;
    if (fabsf(c->s - 1.0f) < 0.001f) fill_round_rect_shadow_px(c, x, y, w, h, r, max_alpha);
    else fill_round_rect_shadow_px(c, sc_i(c->s, x), sc_i(c->s, y), sc_sz(c->s, w), sc_sz(c->s, h), sc_i(c->s, r), max_alpha);
}

void fill_round_rect_shadow_fast(Canvas* c, int x, int y, int w, int h, int r, uint8_t max_alpha) {
    if (!c || max_alpha == 0) return;
    float s = c->s > 0.0f ? c->s : 1.0f;
    int px = (fabsf(s - 1.0f) < 0.001f) ? x : sc_i(s, x);
    int py = (fabsf(s - 1.0f) < 0.001f) ? y : sc_i(s, y);
    int pw = (fabsf(s - 1.0f) < 0.001f) ? w : sc_sz(s, w);
    int ph = (fabsf(s - 1.0f) < 0.001f) ? h : sc_sz(s, h);
    int pr = (fabsf(s - 1.0f) < 0.001f) ? r : sc_i(s, r);
    int k1 = (int)(1 * s + 0.5f), k2 = (int)(2 * s + 0.5f), k4 = (int)(4 * s + 0.5f);
    int inset = (pr + 1) / 2;
    if (inset < k4) inset = k4;
    fill_round_layer_skip(c, px - k1, py + k2, pw + k2, ph + k4, pr + k2,
                          (uint8_t)(max_alpha / 2), px + inset, py + inset, pw - 2 * inset, ph - 2 * inset);
}

static inline void frost_fill_span(uint32_t* drow, int vx0, int k0, int k1,
                                   const uint32_t* sr0, const uint32_t* sr1,
                                   const int* p_ix0, const int* p_ix1, const int* p_wx,
                                   uint32_t uwy, uint32_t unwy) {
    for (int k = k0; k < k1; k++) {
        int p0 = p_ix0[k], p1 = p_ix1[k];
        uint32_t uwx = (uint32_t)p_wx[k], unwx = 256u - uwx;
        uint32_t c00 = sr0[p0], c10 = sr0[p1], c01 = sr1[p0], c11 = sr1[p1];
        uint32_t top_rb = (((c00 & 0x00FF00FFu) * unwx + (c10 & 0x00FF00FFu) * uwx + 0x00800080u) >> 8) & 0x00FF00FFu;
        uint32_t bot_rb = (((c01 & 0x00FF00FFu) * unwx + (c11 & 0x00FF00FFu) * uwx + 0x00800080u) >> 8) & 0x00FF00FFu;
        uint32_t out_rb = ((top_rb * unwy + bot_rb * uwy + 0x00800080u) >> 8) & 0x00FF00FFu;
        uint32_t top_g  = ((((c00 >> 8) & 0xFFu) * unwx + ((c10 >> 8) & 0xFFu) * uwx + 128u) >> 8);
        uint32_t bot_g  = ((((c01 >> 8) & 0xFFu) * unwx + ((c11 >> 8) & 0xFFu) * uwx + 128u) >> 8);
        uint32_t out_g  = (top_g * unwy + bot_g * uwy + 128u) & 0x0000FF00u;
        drow[vx0 + k] = 0xFF000000u | out_g | out_rb;
    }
}

void fill_frosted_round_rect_skip(Canvas* c, int x, int y, int w, int h, int r,
                                  int skip_x, int skip_y, int skip_w, int skip_h,
                                  uint32_t tint_col, uint8_t tint_alpha, int blur_radius, uint8_t rim_alpha) {
    if (!c || !c->px || w <= 0 || h <= 0) return;
    float s = c->s > 0.0f ? c->s : 1.0f;
    int px = (fabsf(s - 1.0f) < 0.001f) ? x : sc_i(s, x);
    int py = (fabsf(s - 1.0f) < 0.001f) ? y : sc_i(s, y);
    int pw = (fabsf(s - 1.0f) < 0.001f) ? w : sc_sz(s, w);
    int ph = (fabsf(s - 1.0f) < 0.001f) ? h : sc_sz(s, h);
    int pr = (fabsf(s - 1.0f) < 0.001f) ? r : sc_i(s, r);
    if (pw <= 0 || ph <= 0) return;
    if (pr < 0) pr = 0;
    if (pr > pw / 2) pr = pw / 2;
    if (pr > ph / 2) pr = ph / 2;

    int cx0, cy0, cx1, cy1;
    canvas_clip_bounds(c, &cx0, &cy0, &cx1, &cy1);
    int vx0 = px < cx0 ? cx0 : px;
    int vy0 = py < cy0 ? cy0 : py;
    int vx1 = (px + pw > cx1) ? cx1 : (px + pw);
    int vy1 = (py + ph > cy1) ? cy1 : (py + ph);
    int vw = vx1 - vx0;
    int vh = vy1 - vy0;
    if (vw <= 0 || vh <= 0) return;

    int has_skip_rect = (skip_w > 0 && skip_h > 0);
    int skx0 = 0, sky0 = 0, skx1 = 0, sky1 = 0;
    if (has_skip_rect) {
        skx0 = (fabsf(s - 1.0f) < 0.001f) ? skip_x : sc_i(s, skip_x);
        sky0 = (fabsf(s - 1.0f) < 0.001f) ? skip_y : sc_i(s, skip_y);
        skx1 = skx0 + ((fabsf(s - 1.0f) < 0.001f) ? skip_w : sc_sz(s, skip_w));
        sky1 = sky0 + ((fabsf(s - 1.0f) < 0.001f) ? skip_h : sc_sz(s, skip_h));
        if (skx1 <= vx0 || skx0 >= vx1 || sky1 <= vy0 || sky0 >= vy1) has_skip_rect = 0;
    }

    if (vw < 8 || vh < 8 || blur_radius <= 0) {
        if (has_skip_rect && sky0 >= py + pr && sky1 <= py + ph - pr && skx0 > px && skx1 < px + pw) {
            int ocx0 = c->clip_x0, ocy0 = c->clip_y0, ocx1 = c->clip_x1, ocy1 = c->clip_y1;
            int ny0 = sky0 < vy1 ? sky0 : vy1;
            int ny1 = sky1 > vy0 ? sky1 : vy0;
            if (vy0 < ny0) {
                canvas_set_clip_px(c, cx0, cy0, cx1, ny0);
                fill_round_rect_aa_a_px(c, px, py, pw, ph, pr, tint_col, tint_alpha);
            }
            canvas_set_clip_px(c, cx0, cy0, cx1, cy1);
            if (ny1 > ny0) {
                fill_rect_a_px(c, px, ny0, skx0 - px, ny1 - ny0, tint_col, tint_alpha);
                fill_rect_a_px(c, skx1, ny0, (px + pw) - skx1, ny1 - ny0, tint_col, tint_alpha);
            }
            if (ny1 < vy1) {
                canvas_set_clip_px(c, cx0, ny1, cx1, cy1);
                fill_round_rect_aa_a_px(c, px, py, pw, ph, pr, tint_col, tint_alpha);
            }
            c->clip_x0 = ocx0; c->clip_y0 = ocy0; c->clip_x1 = ocx1; c->clip_y1 = ocy1;
        } else {
            fill_round_rect_aa_a_px(c, px, py, pw, ph, pr, tint_col, tint_alpha);
        }
        if (rim_alpha > 0 && pr > 0) {
            stroke_round_rect_aa_px(c, px, py, pw, ph, pr, sc_sz(s, 1), COL(255, 255, 255, 255), rim_alpha);
        }
        return;
    }

    int max_bw = (has_skip_rect && blur_radius <= 1) ? 36 : 72;
    int max_bh = (has_skip_rect && blur_radius <= 1) ? 24 : 44;
    int ds = (int)(s * (blur_radius >= 3 ? 6.0f : (has_skip_rect ? 7.0f : 5.0f)) + 0.5f);
    if (ds < 4) ds = 4;
    while ((vw / ds) > max_bw || (vh / ds) > max_bh) ds++;
    int bw = vw / ds; if (bw < 6) bw = (vw < 6 ? vw : 6); if (bw > max_bw) bw = max_bw;
    int bh = vh / ds; if (bh < 6) bh = (vh < 6 ? vh : 6); if (bh > max_bh) bh = max_bh;

    uint32_t buf_a[72 * 44], buf_b[72 * 44];
    for (int by = 0; by < bh; by++) {
        int sy0 = vy0 + (by * vh) / bh;
        int sy1 = vy0 + ((by + 1) * vh - 1) / bh;
        if (sy0 >= c->h) sy0 = c->h - 1;
        if (sy1 >= c->h) sy1 = c->h - 1;
        const uint32_t* r0 = c->px + (size_t)sy0 * c->w;
        const uint32_t* r1 = c->px + (size_t)sy1 * c->w;
        for (int bx = 0; bx < bw; bx++) {
            int sx0 = vx0 + (bx * vw) / bw;
            int sx1 = vx0 + ((bx + 1) * vw - 1) / bw;
            if (sx0 >= c->w) sx0 = c->w - 1;
            if (sx1 >= c->w) sx1 = c->w - 1;
            uint32_t p0 = r0[sx0], p3 = r1[sx1];
            buf_a[(size_t)by * bw + bx] = ((p0 & p3) + (((p0 ^ p3) & 0xFEFEFEFEu) >> 1)) | 0xFF000000u;
        }
    }

    static const uint32_t kRecip[8] = { 0u, 65536u, 32768u, 21846u, 16384u, 13108u, 10923u, 9363u };
    int rad = (blur_radius > 3) ? 3 : blur_radius;
    int passes = (has_skip_rect && blur_radius == 1) ? 1 : 2;
    for (int p = 0; p < passes; p++) {
        for (int by = 0; by < bh; by++) {
            const uint32_t* sr = buf_a + (size_t)by * bw;
            uint32_t* dr = buf_b + (size_t)by * bw;
            for (int bx = 0; bx < bw; bx++) {
                uint32_t rsum = 0, gsum = 0, bsum = 0;
                int k0 = bx - rad; if (k0 < 0) k0 = 0;
                int k1 = bx + rad; if (k1 >= bw) k1 = bw - 1;
                for (int k = k0; k <= k1; k++) {
                    uint32_t c0 = sr[k];
                    rsum += c0 & 0xFFu; gsum += (c0 >> 8) & 0xFFu; bsum += (c0 >> 16) & 0xFFu;
                }
                uint32_t rec = kRecip[k1 - k0 + 1];
                dr[bx] = 0xFF000000u | (((bsum * rec) >> 16) << 16) | (((gsum * rec) >> 16) << 8) | ((rsum * rec) >> 16);
            }
        }
        for (int by = 0; by < bh; by++) {
            int k0 = by - rad; if (k0 < 0) k0 = 0;
            int k1 = by + rad; if (k1 >= bh) k1 = bh - 1;
            uint32_t rec = kRecip[k1 - k0 + 1];
            uint32_t* dr = buf_a + (size_t)by * bw;
            for (int bx = 0; bx < bw; bx++) {
                uint32_t rsum = 0, gsum = 0, bsum = 0;
                for (int k = k0; k <= k1; k++) {
                    uint32_t c0 = buf_b[(size_t)k * bw + bx];
                    rsum += c0 & 0xFFu; gsum += (c0 >> 8) & 0xFFu; bsum += (c0 >> 16) & 0xFFu;
                }
                dr[bx] = 0xFF000000u | (((bsum * rec) >> 16) << 16) | (((gsum * rec) >> 16) << 8) | ((rsum * rec) >> 16);
            }
        }
    }

    uint32_t ua = tint_alpha;
    uint32_t na = 255u - ua;
    uint32_t tr_a = (tint_col & 0x00FF00FFu) * ua;
    uint32_t tg_a = (tint_col & 0x0000FF00u) * ua;
    int bn = bw * bh;
    for (int i = 0; i < bn; i++) {
        uint32_t d = buf_a[i];
        uint32_t rb = (tr_a + (d & 0x00FF00FFu) * na) >> 8;
        uint32_t g  = (tg_a + (d & 0x0000FF00u) * na) >> 8;
        buf_a[i] = 0xFF000000u | (rb & 0x00FF00FFu) | (g & 0x0000FF00u);
    }

    int ix0[2048], ix1[2048], wx[2048];
    int use_stk = (vw <= 2048);
    int* p_ix0 = use_stk ? ix0 : (int*)malloc((size_t)vw * sizeof(int));
    int* p_ix1 = use_stk ? ix1 : (int*)malloc((size_t)vw * sizeof(int));
    int* p_wx  = use_stk ? wx  : (int*)malloc((size_t)vw * sizeof(int));
    if (!p_ix0 || !p_ix1 || !p_wx) {
        if (!use_stk) { free(p_ix0); free(p_ix1); free(p_wx); }
        return;
    }

    float fsx = (float)bw / (float)vw;
    float fsy = (float)bh / (float)vh;
    for (int k = 0; k < vw; k++) {
        float fx = ((float)k + 0.5f) * fsx - 0.5f;
        int xa = (int)floorf(fx);
        int wxi = (xa < 0) ? (xa = 0, 0) : (int)((fx - (float)xa) * 256.0f + 0.5f);
        if (xa >= bw) xa = bw - 1;
        int xb = xa + 1;
        if (xb >= bw) { xb = bw - 1; wxi = 0; }
        if (wxi < 0) wxi = 0; else if (wxi > 256) wxi = 256;
        p_ix0[k] = xa; p_ix1[k] = xb; p_wx[k] = wxi;
    }

    for (int gy = vy0; gy < vy1; gy++) {
        int lk = gy - vy0;
        float fy = ((float)lk + 0.5f) * fsy - 0.5f;
        int ya = (int)floorf(fy);
        int wyi = (ya < 0) ? (ya = 0, 0) : (int)((fy - (float)ya) * 256.0f + 0.5f);
        if (ya >= bh) ya = bh - 1;
        int yb = ya + 1;
        if (yb >= bh) { yb = bh - 1; wyi = 0; }
        if (wyi < 0) wyi = 0; else if (wyi > 256) wyi = 256;
        uint32_t uwy = (uint32_t)wyi, unwy = 256u - uwy;

        const uint32_t* sr0 = buf_a + (size_t)ya * bw;
        const uint32_t* sr1 = buf_a + (size_t)yb * bw;
        uint32_t* drow = c->px + (size_t)gy * c->w;

        int j = gy - py;
        int cy_dist = 0;
        if (pr > 0) {
            if (j < pr) cy_dist = pr - j;
            else if (j >= ph - pr) cy_dist = j - (ph - 1 - pr);
        }

        int row_has_skip = (has_skip_rect && gy >= sky0 && gy < sky1);
        int ks0 = 0, ks1 = 0;
        if (row_has_skip) {
            ks0 = skx0 - vx0; if (ks0 < 0) ks0 = 0; if (ks0 > vw) ks0 = vw;
            ks1 = skx1 - vx0; if (ks1 < ks0) ks1 = ks0; if (ks1 > vw) ks1 = vw;
            if (ks1 <= ks0) row_has_skip = 0;
        }

        if (cy_dist == 0) {
            if (!row_has_skip) {
                frost_fill_span(drow, vx0, 0, vw, sr0, sr1, p_ix0, p_ix1, p_wx, uwy, unwy);
            } else {
                if (ks0 > 0) frost_fill_span(drow, vx0, 0, ks0, sr0, sr1, p_ix0, p_ix1, p_wx, uwy, unwy);
                if (ks1 < vw) frost_fill_span(drow, vx0, ks1, vw, sr0, sr1, p_ix0, p_ix1, p_wx, uwy, unwy);
            }
        } else {
            for (int k = 0; k < vw; k++) {
                if (row_has_skip && k == ks0) { k = ks1 - 1; continue; }
                int gx = vx0 + k;
                int li = gx - px;
                int cov = 255;
                if (li < pr) cov = corner_cov(pr - li, cy_dist, pr);
                else if (li >= pw - pr) cov = corner_cov(li - (pw - 1 - pr), cy_dist, pr);
                if (cov <= 0) continue;

                int p0 = p_ix0[k], p1 = p_ix1[k];
                uint32_t uwx = (uint32_t)p_wx[k], unwx = 256u - uwx;
                uint32_t c00 = sr0[p0], c10 = sr0[p1], c01 = sr1[p0], c11 = sr1[p1];
                uint32_t top_rb = (((c00 & 0x00FF00FFu) * unwx + (c10 & 0x00FF00FFu) * uwx + 0x00800080u) >> 8) & 0x00FF00FFu;
                uint32_t bot_rb = (((c01 & 0x00FF00FFu) * unwx + (c11 & 0x00FF00FFu) * uwx + 0x00800080u) >> 8) & 0x00FF00FFu;
                uint32_t out_rb = ((top_rb * unwy + bot_rb * uwy + 0x00800080u) >> 8) & 0x00FF00FFu;
                uint32_t top_g  = ((((c00 >> 8) & 0xFFu) * unwx + ((c10 >> 8) & 0xFFu) * uwx + 128u) >> 8);
                uint32_t bot_g  = ((((c01 >> 8) & 0xFFu) * unwx + ((c11 >> 8) & 0xFFu) * uwx + 128u) >> 8);
                uint32_t out_g  = (top_g * unwy + bot_g * uwy + 128u) & 0x0000FF00u;
                uint32_t frost_col = 0xFF000000u | out_g | out_rb;

                if (cov >= 255) drow[gx] = frost_col;
                else            px_blend(c, gx, gy, frost_col, (uint8_t)cov);
            }
        }
    }

    if (!use_stk) { free(p_ix0); free(p_ix1); free(p_wx); }

    if (rim_alpha > 0 && pr > 0) {
        stroke_round_rect_aa_px(c, px, py, pw, ph, pr, sc_sz(s, 1), COL(255, 255, 255, 255), rim_alpha);
    }
}

void fill_frosted_round_rect(Canvas* c, int x, int y, int w, int h, int r,
                             uint32_t tint_col, uint8_t tint_alpha, int blur_radius, uint8_t rim_alpha) {
    fill_frosted_round_rect_skip(c, x, y, w, h, r, 0, 0, 0, 0, tint_col, tint_alpha, blur_radius, rim_alpha);
}
void stroke_circle_aa(Canvas* c, int cx, int cy, int r, int thickness, uint32_t col, uint8_t alpha) {
    if (!c) return;
    if (fabsf(c->s - 1.0f) < 0.001f) stroke_circle_aa_px(c, cx, cy, r, thickness, col, alpha);
    else stroke_circle_aa_px(c, sc_i(c->s, cx), sc_i(c->s, cy), sc_sz(c->s, r), sc_sz(c->s, thickness), col, alpha);
}
void fill_tri_aa(Canvas* c, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t col) {
    if (!c) return;
    if (fabsf(c->s - 1.0f) < 0.001f) fill_tri_aa_px(c, x0, y0, x1, y1, x2, y2, col);
    else fill_tri_aa_px(c, sc_i(c->s, x0), sc_i(c->s, y0), sc_i(c->s, x1), sc_i(c->s, y1), sc_i(c->s, x2), sc_i(c->s, y2), col);
}
void fill_ellipse_aa(Canvas* c, int cx, int cy, int rx, int ry, uint32_t col) {
    if (!c) return;
    if (fabsf(c->s - 1.0f) < 0.001f) fill_ellipse_aa_px(c, cx, cy, rx, ry, col);
    else fill_ellipse_aa_px(c, sc_i(c->s, cx), sc_i(c->s, cy), sc_sz(c->s, rx), sc_sz(c->s, ry), col);
}
void fill_circle_a(Canvas* c, int cx, int cy, int r, uint32_t col, uint8_t a) {
    if (!c) return;
    if (fabsf(c->s - 1.0f) < 0.001f) fill_circle_a_px(c, cx, cy, r, col, a);
    else fill_circle_a_px(c, sc_i(c->s, cx), sc_i(c->s, cy), sc_sz(c->s, r), col, a);
}
void fill_circle_clip_a(Canvas* c, int cx, int cy, int r, uint32_t col, uint8_t a,
                        int clipx, int clipy, int clipw, int cliph) {
    if (!c) return;
    if (fabsf(c->s - 1.0f) < 0.001f) fill_circle_clip_a_px(c, cx, cy, r, col, a, clipx, clipy, clipw, cliph);
    else fill_circle_clip_a_px(c, sc_i(c->s, cx), sc_i(c->s, cy), sc_sz(c->s, r), col, a,
                               sc_i(c->s, clipx), sc_i(c->s, clipy), sc_sz(c->s, clipw), sc_sz(c->s, cliph));
}
void draw_m3_wavy_progress(Canvas* c, int x0, int x1, int mid_y, float frac,
                           float phase, float wave_amp, float wave_period,
                           int wave_thick, int track_thick, int draw_thumb,
                           uint32_t col_active, uint32_t col_track) {
    if (!c) return;
    if (fabsf(c->s - 1.0f) < 0.001f) {
        draw_m3_wavy_progress_px(c, x0, x1, mid_y, frac, phase, wave_amp, wave_period,
                                 wave_thick, track_thick, draw_thumb, col_active, col_track);
    } else {
        float s = c->s;
        draw_m3_wavy_progress_px(c, sc_i(s, x0), sc_i(s, x1), sc_i(s, mid_y), frac, phase,
                                 wave_amp * s, wave_period * s, sc_sz(s, wave_thick), sc_sz(s, track_thick),
                                 draw_thumb, col_active, col_track);
    }
}

void fill_m3_scallop(Canvas* c, int cx, int cy, int r, int lobes, float rot, uint32_t col) {
    if (!c || r < 4 || lobes < 3) return;
    float s = c->s > 0.0f ? c->s : 1.0f;
    int px = sc_i(s, cx), py = sc_i(s, cy), pr = sc_sz(s, r);
    int cx0, cy0, cx1, cy1;
    canvas_clip_bounds(c, &cx0, &cy0, &cx1, &cy1);
    int x0 = px - pr - 2; if (x0 < cx0) x0 = cx0;
    int y0 = py - pr - 2; if (y0 < cy0) y0 = cy0;
    int x1 = px + pr + 2; if (x1 >= cx1) x1 = cx1 - 1;
    int y1 = py + pr + 2; if (y1 >= cy1) y1 = cy1 - 1;
    uint8_t ga = (uint8_t)(col >> 24);
    if (ga == 0) ga = 255;
    float r_base = (float)pr * 0.90f;
    float r_amp  = (float)pr * 0.10f;
    float r_in2  = (r_base - r_amp - 1.0f); r_in2 = (r_in2 > 0.0f) ? (r_in2 * r_in2) : 0.0f;
    float r_out  = (float)pr + 1.5f;
    float r_out2 = r_out * r_out;
    float cr = cosf((float)lobes * rot), sr = sinf((float)lobes * rot);
    uint32_t opaque = col | 0xFF000000u;

    for (int y = y0; y <= y1; y++) {
        float dy = (float)(y - py) + 0.5f;
        float dy2 = dy * dy;
        for (int x = x0; x <= x1; x++) {
            float dx = (float)(x - px) + 0.5f;
            float d2 = dx * dx + dy2;
            if (d2 > r_out2) continue;
            if (d2 <= r_in2) {
                if (ga >= 255) c->px[(size_t)y * c->w + x] = opaque;
                else px_blend(c, x, y, col, ga);
                continue;
            }
            float dist = sqrtf(d2);
            float u = (dist > 1e-4f) ? (dx / dist) : 1.0f;
            float v = (dist > 1e-4f) ? (dy / dist) : 0.0f;
            float zr = 1.0f, zi = 0.0f;
            int e = lobes;
            while (e > 0) {
                if (e & 1) {
                    float nr = zr * u - zi * v;
                    float ni = zr * v + zi * u;
                    zr = nr; zi = ni;
                }
                e >>= 1;
                if (e) {
                    float nu = u * u - v * v;
                    float nv = 2.0f * u * v;
                    u = nu; v = nv;
                }
            }
            float target_r = r_base + r_amp * (zr * cr - zi * sr);
            float cov = target_r - dist + 0.55f;
            if (cov <= 0.0f) continue;
            if (cov >= 1.0f && ga >= 255) c->px[(size_t)y * c->w + x] = opaque;
            else {
                if (cov > 1.0f) cov = 1.0f;
                px_blend(c, x, y, col, (uint8_t)(cov * (float)ga + 0.5f));
            }
        }
    }
}

void fill_m3_wave_panel_right(Canvas* c, int x, int y, int w, int h, int r,
                              int split_x, float amp, float period, float phase,
                              uint32_t col, uint8_t alpha) {
    if (!c || w <= 0 || h <= 0 || alpha == 0) return;
    float s = c->s > 0.0f ? c->s : 1.0f;
    int sx = sc_i(s, x), sy = sc_i(s, y), sw = sc_sz(s, w), sh = sc_sz(s, h);
    int sr = sc_i(s, r);
    if (sr > sw / 2) sr = sw / 2;
    if (sr > sh / 2) sr = sh / 2;
    float psplit = (float)sc_i(s, split_x);
    float pamp = amp * s;
    float pfreq = (period > 4.0f) ? (6.2831853f / (period * s)) : 0.08f;

    int cx0, cy0, cx1, cy1;
    canvas_clip_bounds(c, &cx0, &cy0, &cx1, &cy1);
    int y0 = sy < cy0 ? cy0 : sy;
    int y1 = (sy + sh > cy1) ? cy1 : (sy + sh);
    for (int py = y0; py < y1; py++) {
        float wedge = psplit + pamp * sinf((float)(py - sy) * pfreq + phase);
        int j = py - sy;
        int cy_d = (j < sr) ? (sr - j) : ((j >= sh - sr) ? (j - (sh - 1 - sr)) : 0);
        int right_limit = sx + sw;
        int x_start = (int)floorf(wedge - 1.0f);
        if (x_start < sx + (int)(pamp + 2)) x_start = sx;
        if (x_start < cx0) x_start = cx0;
        int x_end = right_limit < cx1 ? right_limit : cx1;
        for (int px = x_start; px < x_end; px++) {
            float cov_l = ((float)px + 0.5f) - wedge + 0.5f;
            if (cov_l <= 0.0f) continue;
            if (cov_l > 1.0f) cov_l = 1.0f;
            float cov = cov_l;
            if (cy_d > 0 && px >= sx + sw - sr) {
                float dx = (float)(px - (sx + sw - 1 - sr)) + 0.5f;
                float dy = (float)cy_d - 0.5f;
                float cc = (float)sr - sqrtf(dx * dx + dy * dy) + 0.5f;
                if (cc <= 0.0f) continue;
                if (cc < cov) cov = cc;
            }
            uint8_t aa = (uint8_t)(cov * (float)alpha + 0.5f);
            if (aa > 0) px_blend(c, px, py, col, aa);
        }
    }
}

void draw_m3_water_progress(Canvas* c, int x0, int x1, int mid_y, float frac,
                            float phase, int bar_thick, int draw_thumb,
                            uint32_t col_active, uint32_t col_track) {
    if (!c || x1 <= x0 + 6) return;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;

    float s = c->s > 0.0f ? c->s : 1.0f;
    int px0 = sc_i(s, x0);
    int px1 = sc_i(s, x1);
    int py  = sc_i(s, mid_y);
    int th  = sc_sz(s, bar_thick);
    if (th < 4) th = 4;
    float r = (float)th * 0.5f;

    int cx0, cy0, cx1, cy1;
    canvas_clip_bounds(c, &cx0, &cy0, &cx1, &cy1);

    int iy0 = py - (int)ceilf(r + 1.0f); if (iy0 < cy0) iy0 = cy0;
    int iy1 = py + (int)ceilf(r + 1.0f); if (iy1 >= cy1) iy1 = cy1 - 1;
    int ix0 = px0 - 1; if (ix0 < cx0) ix0 = cx0;
    int ix1 = px1 + 1; if (ix1 >= cx1) ix1 = cx1 - 1;
    int span = ix1 - ix0 + 1;
    if (span <= 0 || iy1 < iy0) return;

    uint8_t ta = (uint8_t)(col_track >> 24);
    if (ta == 0) ta = 255;
    uint8_t aa_base = (uint8_t)(col_active >> 24);
    if (aa_base == 0) aa_base = 255;

    float fx0 = (float)px0 + r;
    float fx1 = (float)px1 - r;
    if (fx1 < fx0) fx1 = fx0;
    float thumb_x = (float)px0 + (float)(px1 - px0) * frac;

    uint32_t ar = col_active & 0xFFu, ag = (col_active >> 8) & 0xFFu, ab = (col_active >> 16) & 0xFFu;
    uint32_t tr = col_track & 0xFFu,  tg = (col_track >> 8) & 0xFFu,  tb = (col_track >> 16) & 0xFFu;
    uint32_t track_rgb = COL(tr, tg, tb, 255);

    float wx1[2048], sx2[2048], cx2[2048];
    int use_lut = (span <= 2048);
    if (use_lut && frac > 0.002f) {
        int active_end = (int)ceilf(thumb_x + 1.0f);
        if (active_end > ix1) active_end = ix1;
        for (int x = ix0; x <= active_end; x++) {
            float fx = (float)x + 0.5f;
            float nx = (fx - (float)px0) / s;
            float ang2 = nx * 0.25f + phase * 1.5f;
            int k = x - ix0;
            wx1[k] = sinf(nx * 0.14f - phase * 2.6f);
            sx2[k] = sinf(ang2);
            cx2[k] = cosf(ang2);
        }
    }

    for (int y = iy0; y <= iy1; y++) {
        float dy = (float)y + 0.5f - (float)py;
        float abs_dy = fabsf(dy);
        if (abs_dy > r + 1.0f) continue;
        float ny = dy / r;
        float siny = sinf(ny * 1.8f), cosy = cosf(ny * 1.8f);
        float top_sheen = (ny < 0.0f ? (-ny) * 0.14f : 0.0f);

        for (int x = ix0; x <= ix1; x++) {
            float fx = (float)x + 0.5f;
            float dist = abs_dy;
            if (fx < fx0) {
                float dx = fx0 - fx;
                dist = sqrtf(dx * dx + dy * dy);
            } else if (fx > fx1) {
                float dx = fx - fx1;
                dist = sqrtf(dx * dx + dy * dy);
            }
            float cov = r - dist + 0.5f;
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;

            if (frac > 0.002f && fx <= thumb_x + 0.5f) {
                float acov = cov;
                if (fx > thumb_x - 0.5f) {
                    float ec = (thumb_x + 0.5f) - fx;
                    if (ec < acov) acov = ec < 0.0f ? 0.0f : ec;
                }
                if (cov > acov && acov < 1.0f) {
                    px_blend(c, x, y, track_rgb, (uint8_t)((cov - acov) * (float)ta + 0.5f));
                }
                if (acov > 0.0f) {
                    float w1, w2;
                    if (use_lut) {
                        int k = x - ix0;
                        w1 = wx1[k];
                        w2 = sx2[k] * cosy + cx2[k] * siny;
                    } else {
                        float nx = (fx - (float)px0) / s;
                        w1 = sinf(nx * 0.14f - phase * 2.6f);
                        w2 = sinf(nx * 0.25f + phase * 1.5f + ny * 1.8f);
                    }
                    float ripple = (w1 + w2 * 0.6f) * 0.5f;
                    float sheen = top_sheen + (ripple > 0.0f ? ripple * 0.24f : ripple * 0.10f);
                    float dhead = fabsf(fx - thumb_x) / s;
                    if (dhead < 24.0f) sheen += (1.0f - dhead / 24.0f) * 0.15f;

                    float wr = (float)ar, wg = (float)ag, wb = (float)ab;
                    if (sheen >= 0.0f) {
                        float t = sheen > 0.42f ? 0.42f : sheen;
                        wr += (255.0f - wr) * t;
                        wg += (255.0f - wg) * t;
                        wb += (255.0f - wb) * t;
                    } else {
                        float k = 1.0f + sheen;
                        if (k < 0.78f) k = 0.78f;
                        wr *= k; wg *= k; wb *= k;
                    }
                    uint32_t wc = COL((uint8_t)wr, (uint8_t)wg, (uint8_t)wb, 255);
                    px_blend(c, x, y, wc, (uint8_t)(acov * (float)aa_base + 0.5f));
                }
            } else {
                px_blend(c, x, y, track_rgb, (uint8_t)(cov * (float)ta + 0.5f));
            }
        }
    }

    if (draw_thumb) {
        int ttx = (int)(thumb_x + 0.5f);
        float hr = r + 3.2f * s;
        fill_circle_a_px(c, ttx, py, (int)(hr + 3.0f * s), col_active, 48);
        fill_ellipse_aa_px(c, ttx, py, (int)hr, (int)hr, COL(250, 252, 255, 255));
        int inner_r = (int)(hr - 2.0f * s) > 1 ? (int)(hr - 2.0f * s) : 1;
        fill_ellipse_aa_px(c, ttx, py, inner_r, inner_r, col_active);
    }
}
