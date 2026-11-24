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
    Canvas* c = (Canvas*)malloc(sizeof(Canvas));
    if (!c) return NULL;
    c->w = w; c->h = h;
    c->lw = w; c->lh = h;
    c->s = 1.0f;
    c->clip_x0 = 0; c->clip_y0 = 0;
    c->clip_x1 = w; c->clip_y1 = h;
    c->px = (uint32_t*)calloc((size_t)w * h, 4);
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
    float sx = (float)sw / (float)dw;
    float sy = (float)sh / (float)dh;
    for (int y = 0; y < dh; y++) {
        float fy = (y + 0.5f) * sy - 0.5f;
        int y0 = (int)fy;
        float ty = fy - y0;
        if (y0 < 0) { y0 = 0; ty = 0; }
        int y1 = y0 + 1; if (y1 >= sh) y1 = sh - 1;
        for (int x = 0; x < dw; x++) {
            float fx = (x + 0.5f) * sx - 0.5f;
            int x0 = (int)fx;
            float tx = fx - x0;
            if (x0 < 0) { x0 = 0; tx = 0; }
            int x1 = x0 + 1; if (x1 >= sw) x1 = sw - 1;
            const uint32_t* p00 = src + (size_t)y0 * sw + x0;
            const uint32_t* p10 = src + (size_t)y0 * sw + x1;
            const uint32_t* p01 = src + (size_t)y1 * sw + x0;
            const uint32_t* p11 = src + (size_t)y1 * sw + x1;
            uint32_t out = 0;
            for (int ch = 0; ch < 4; ch++) {
                int shift = ch * 8;
                float a = (float)((*p00 >> shift) & 0xFF);
                float b = (float)((*p10 >> shift) & 0xFF);
                float cc = (float)((*p01 >> shift) & 0xFF);
                float d = (float)((*p11 >> shift) & 0xFF);
                float top = a + (b - a) * tx;
                float bot = cc + (d - cc) * tx;
                float v = top + (bot - top) * ty;
                int iv = (int)(v + 0.5f);
                if (iv < 0) iv = 0;
                if (iv > 255) iv = 255;
                out |= ((uint32_t)iv) << shift;
            }
            dst[(size_t)y * dw + x] = out;
        }
    }
}

void box_blur(uint32_t* img, int w, int h, int radius, int passes) {
    if (radius < 1) return;
    uint32_t* tmp = (uint32_t*)malloc((size_t)w * h * 4);
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

    int y0 = sy < 0 ? 0 : sy;
    int y1 = (sy + sh > c->h) ? c->h : (sy + sh);
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
            uint32_t* row = c->px + (size_t)y * c->w;
            for (int x = lx; x <= rx_c; x++) row[x] = opaque;
        }
        int e_start = x_in + 1;
        for (int k = e_start; k <= x_out; k++) {
            float ix = (float)k / (float)rx;
            float cov = minr * (1.0f - sqrtf(ix * ix + jy2)) + 0.5f;
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            uint8_t aa = (uint8_t)(cov * 255.0f + 0.5f);
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
    if (x1 <= x0 || y1 <= y0) return;

    float sx = (float)sw / (float)dw;
    float sy = (float)sh / (float)dh;
    for (int j = y0; j < y1; j++) {
        float fy = (j + 0.5f) * sy - 0.5f;
        int iy0 = (int)floorf(fy);
        float ty = fy - (float)iy0;
        if (iy0 < 0) { iy0 = 0; ty = 0.0f; }
        int iy1 = iy0 + 1;
        if (iy1 >= sh) iy1 = sh - 1;
        if (iy0 >= sh) iy0 = sh - 1;
        const uint32_t* sr0 = src + (size_t)iy0 * sw;
        const uint32_t* sr1 = src + (size_t)iy1 * sw;
        uint32_t* drow = dst->px + (size_t)(dy + j) * dst->w + dx;

        int cy_dist = 0;
        if (corner > 0) {
            if (j < corner) cy_dist = corner - j;
            else if (j >= dh - corner) cy_dist = j - (dh - 1 - corner);
        }

        for (int i = x0; i < x1; i++) {
            int cov = 255;
            if (cy_dist > 0) {
                if (i < corner) cov = corner_cov(corner - i, cy_dist, corner);
                else if (i >= dw - corner) cov = corner_cov(i - (dw - 1 - corner), cy_dist, corner);
                if (cov == 0) continue;
            }
            float fx = (i + 0.5f) * sx - 0.5f;
            int ix0 = (int)floorf(fx);
            float tx = fx - (float)ix0;
            if (ix0 < 0) { ix0 = 0; tx = 0.0f; }
            int ix1 = ix0 + 1;
            if (ix1 >= sw) ix1 = sw - 1;
            if (ix0 >= sw) ix0 = sw - 1;

            uint32_t c00 = sr0[ix0], c10 = sr0[ix1], c01 = sr1[ix0], c11 = sr1[ix1];
            float w00 = (1.0f - tx) * (1.0f - ty);
            float w10 = tx * (1.0f - ty);
            float w01 = (1.0f - tx) * ty;
            float w11 = tx * ty;
            uint32_t r = (uint32_t)((c00 & 0xFF) * w00 + (c10 & 0xFF) * w10 + (c01 & 0xFF) * w01 + (c11 & 0xFF) * w11 + 0.5f);
            uint32_t g = (uint32_t)(((c00 >> 8) & 0xFF) * w00 + ((c10 >> 8) & 0xFF) * w10 + ((c01 >> 8) & 0xFF) * w01 + ((c11 >> 8) & 0xFF) * w11 + 0.5f);
            uint32_t b = (uint32_t)(((c00 >> 16) & 0xFF) * w00 + ((c10 >> 16) & 0xFF) * w10 + ((c01 >> 16) & 0xFF) * w01 + ((c11 >> 16) & 0xFF) * w11 + 0.5f);
            uint32_t a = opaque_src ? 255u : (uint32_t)(((c00 >> 24) & 0xFF) * w00 + ((c10 >> 24) & 0xFF) * w10 + ((c01 >> 24) & 0xFF) * w01 + ((c11 >> 24) & 0xFF) * w11 + 0.5f);
            uint32_t px_col = (a << 24) | (b << 16) | (g << 8) | r;
            uint32_t eff = (ga < 255) ? ((uint32_t)cov * ga / 255u) : (uint32_t)cov;
            if (eff >= 255u && opaque_src) drow[i] = px_col | 0xFF000000u;
            else if (eff >= 255u) drow[i] = blend_px(drow[i], px_col);
            else if (eff > 0u) drow[i] = blend_px_a(drow[i], px_col, (uint8_t)eff);
        }
    }
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
    if (fabsf(dst->s - 1.0f) < 0.001f && dw == sw && dh == sh) {
        blit_rgb_round_px(dst, dx, dy, src, sw, sh, corner);
    } else {
        blit_scaled_round_px(dst, sc_i(dst->s, dx), sc_i(dst->s, dy),
                             sc_sz(dst->s, dw), sc_sz(dst->s, dh),
                             src, sw, sh, sc_i(dst->s, corner), 255, 1);
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
