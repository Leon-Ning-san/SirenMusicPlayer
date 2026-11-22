#include "draw.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int in_round_mask(int px, int py, int sw, int sh, int r);

Canvas* canvas_new(int w, int h) {
    Canvas* c = (Canvas*)malloc(sizeof(Canvas));
    c->w = w; c->h = h;
    c->px = (uint32_t*)calloc((size_t)w * h, 4);
    return c;
}

void canvas_free(Canvas* c) {
    if (c) { free(c->px); free(c); }
}

void canvas_clear(Canvas* c, uint32_t col) {
    size_t n = (size_t)c->w * c->h;
    for (size_t i = 0; i < n; i++) c->px[i] = col;
}

void fill_rect(Canvas* c, int x, int y, int w, int h, uint32_t col) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > c->w) w = c->w - x;
    if (y + h > c->h) h = c->h - y;
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
    uint32_t rb = ((src & 0x00FF00FF) * a + (dst & 0x00FF00FF) * (255 - a)) >> 8;
    uint32_t g  = (((src >> 8) & 0xFF) * a + ((dst >> 8) & 0xFF) * (255 - a)) >> 8;
    return 0xFF000000u | (rb & 0x00FF00FF) | (g << 8);
}

void fill_rect_a(Canvas* c, int x, int y, int w, int h, uint32_t col, uint8_t alpha) {
    uint32_t sc = (col & 0x00FFFFFF) | ((uint32_t)alpha << 24);
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > c->w) w = c->w - x;
    if (y + h > c->h) h = c->h - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        uint32_t* row = c->px + (size_t)(y + j) * c->w + x;
        for (int i = 0; i < w; i++) row[i] = blend_px(row[i], sc);
    }
}

void hline(Canvas* c, int x, int y, int len, uint32_t col) {
    fill_rect(c, x, y, len, 1, col);
}
void vline(Canvas* c, int x, int y, int len, uint32_t col) {
    fill_rect(c, x, y, 1, len, col);
}

void fill_tri(Canvas* c, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t col) {
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

void blit_rgba(Canvas* dst, int dx, int dy, const uint32_t* src, int sw, int sh) {
    int x0 = dx < 0 ? -dx : 0;
    int y0 = dy < 0 ? -dy : 0;
    int x1 = (dx + sw > dst->w) ? dst->w - dx : sw;
    int y1 = (dy + sh > dst->h) ? dst->h - dy : sh;
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

static inline uint32_t blend_px_a(uint32_t dst, uint32_t src, uint8_t ga) {
    if (ga == 0) return dst;
    uint32_t a = (src >> 24) & 0xFF;
    if (ga < 255) a = (a * ga) >> 8;   // 用移位代替除法，全屏混合更快
    if (a == 0) return dst;
    if (a >= 255) return src;
    uint32_t rb = ((src & 0x00FF00FF) * a + (dst & 0x00FF00FF) * (255 - a)) >> 8;
    uint32_t g  = (((src >> 8) & 0xFF) * a + ((dst >> 8) & 0xFF) * (255 - a)) >> 8;
    return 0xFF000000u | (rb & 0x00FF00FF) | (g << 8);
}

void blit_rgba_a(Canvas* dst, int dx, int dy, const uint32_t* src, int sw, int sh, uint8_t a) {
    for (int j = 0; j < sh; j++) {
        int ty = dy + j;
        if (ty < 0 || ty >= dst->h) continue;
        for (int i = 0; i < sw; i++) {
            int tx = dx + i;
            if (tx < 0 || tx >= dst->w) continue;
            uint32_t* d = &dst->px[(size_t)ty * dst->w + tx];
            *d = blend_px_a(*d, src[(size_t)j * sw + i], a);
        }
    }
}

// 圆角覆盖度蒙版（0..255 抗锯齿）：内部 255，角部按距离计算覆盖度
static int round_mask_cov(int px, int py, int sw, int sh, int r) {
    if (r <= 0) return 255;
    if (px >= r && px < sw - r) return 255;
    if (py >= r && py < sh - r) return 255;
    int cx, cy;
    if (px < r && py < r) { cx = r; cy = r; }
    else if (px >= sw - r && py < r) { cx = sw - r; cy = r; }
    else if (px < r && py >= sh - r) { cx = r; cy = sh - r; }
    else { cx = sw - r; cy = sh - r; }
    float ddx = (float)(px - cx), ddy = (float)(py - cy);
    float d = sqrtf(ddx * ddx + ddy * ddy);
    float cov = (float)r - d + 0.5f;
    if (cov <= 0) return 0;
    if (cov > 1) cov = 1;
    return (int)(cov * 255.0f + 0.5f);
}

void blit_rgba_round_a(Canvas* dst, int dx, int dy, const uint32_t* src, int sw, int sh, int corner, uint8_t a) {
    for (int j = 0; j < sh; j++) {
        int ty = dy + j;
        if (ty < 0 || ty >= dst->h) continue;
        for (int i = 0; i < sw; i++) {
            int cov = round_mask_cov(i, j, sw, sh, corner);
            if (cov == 0) continue;
            int tx = dx + i;
            if (tx < 0 || tx >= dst->w) continue;
            uint32_t* d = &dst->px[(size_t)ty * dst->w + tx];
            uint8_t ea = (uint8_t)((cov * a) / 255);
            *d = blend_px_a(*d, src[(size_t)j * sw + i], ea);
        }
    }
}

void blit_rgba_round(Canvas* dst, int dx, int dy, const uint32_t* src, int sw, int sh, int corner) {
    for (int j = 0; j < sh; j++) {
        int ty = dy + j;
        if (ty < 0 || ty >= dst->h) continue;
        for (int i = 0; i < sw; i++) {
            int cov = round_mask_cov(i, j, sw, sh, corner);
            if (cov == 0) continue;
            int tx = dx + i;
            if (tx < 0 || tx >= dst->w) continue;
            uint32_t* d = &dst->px[(size_t)ty * dst->w + tx];
            uint32_t s = src[(size_t)j * sw + i];
            if (cov >= 255) {
                *d = blend_px(*d, s);
            } else {
                *d = blend_px_a(*d, s, (uint8_t)cov);
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
            // channel-wise bilinear
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
                if (iv < 0) iv = 0; if (iv > 255) iv = 255;
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
        // horizontal
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
        // vertical
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

// ================= 抗锯齿原语 =================
// 单像素混合（col 为不带 alpha 的 RGB，a 为覆盖度 0..255）
static inline void px_blend(Canvas* c, int x, int y, uint32_t col, uint8_t a) {
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) return;
    if (a >= 255) {
        c->px[(size_t)y * c->w + x] = col | 0xFF000000u;
        return;
    }
    if (a == 0) return;
    uint32_t sc = (col & 0x00FFFFFF) | ((uint32_t)a << 24);
    uint32_t* d = &c->px[(size_t)y * c->w + x];
    *d = blend_px(*d, sc);
}

void fill_round_rect_aa(Canvas* c, int x, int y, int w, int h, int r, uint32_t col) {
    if (w <= 0 || h <= 0) return;
    if (r < 1) { fill_rect(c, x, y, w, h, col); return; }
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    // 内部与直边走快速路径（不透明）
    fill_rect(c, x + r, y, w - 2 * r, h, col);
    fill_rect(c, x, y + r, r, h - 2 * r, col);
    fill_rect(c, x + w - r, y + r, r, h - 2 * r, col);
    // 四个角按覆盖度抗锯齿
    for (int j = 0; j < r; j++) {
        for (int i = 0; i < r; i++) {
            float dx = (float)(r - i) - 0.5f;
            float dy = (float)(r - j) - 0.5f;
            float d = sqrtf(dx * dx + dy * dy);
            float cov = (float)r - d + 0.5f;
            if (cov <= 0) continue;
            if (cov > 1) cov = 1;
            uint8_t a = (uint8_t)(cov * 255.0f + 0.5f);
            px_blend(c, x + i,         y + j,         col, a);   // 左上
            px_blend(c, x + w - 1 - i, y + j,         col, a);   // 右上
            px_blend(c, x + i,         y + h - 1 - j, col, a);   // 左下
            px_blend(c, x + w - 1 - i, y + h - 1 - j, col, a);   // 右下
        }
    }
}

void fill_ellipse_aa(Canvas* c, int cx, int cy, int rx, int ry, uint32_t col) {
    if (rx < 1 || ry < 1) return;
    float minr = (rx < ry ? rx : ry);
    for (int j = -ry; j <= ry; j++) {
        for (int i = -rx; i <= rx; i++) {
            float d = sqrtf((float)(i * i) / (float)(rx * rx) +
                            (float)(j * j) / (float)(ry * ry));
            float cov = minr * (1.0f - d) + 0.5f;
            if (cov <= 0) continue;
            if (cov > 1) cov = 1;
            px_blend(c, cx + i, cy + j, col, (uint8_t)(cov * 255.0f + 0.5f));
        }
    }
}

void fill_circle_a(Canvas* c, int cx, int cy, int r, uint32_t col, uint8_t a) {
    if (r < 1 || a == 0) return;
    for (int j = -r; j <= r; j++) {
        int y = cy + j;
        if (y < 0 || y >= c->h) continue;
        for (int i = -r; i <= r; i++) {
            int x = cx + i;
            if (x < 0 || x >= c->w) continue;
            float d = sqrtf((float)(i * i + j * j));
            float cov = (float)r - d + 0.5f;
            if (cov <= 0) continue;
            if (cov > 1) cov = 1;
            uint8_t aa = (uint8_t)(cov * (a / 255.0f) * 255.0f + 0.5f);
            if (aa == 0) continue;
            px_blend(c, x, y, col, aa);
        }
    }
}

void fill_circle_clip_a(Canvas* c, int cx, int cy, int r, uint32_t col, uint8_t a,
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
            uint8_t aa = (uint8_t)(cov * (a / 255.0f) * 255.0f + 0.5f);
            if (aa == 0) continue;
            px_blend(c, i, j, col, aa);
        }
    }
}
