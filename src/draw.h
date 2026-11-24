#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    int w, h;
    uint32_t* px; // RGBA8888, memory bytes: R,G,B,A
    int clip_x0, clip_y0, clip_x1, clip_y1;
} Canvas;

#define COL(r,g,b,a) (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(g)<<8)|(uint32_t)(r))

Canvas* canvas_new(int w, int h);
void canvas_free(Canvas* c);
void canvas_set_clip(Canvas* c, int x0, int y0, int x1, int y1);
void canvas_reset_clip(Canvas* c);
void canvas_clear(Canvas* c, uint32_t col);
void fill_rect(Canvas* c, int x, int y, int w, int h, uint32_t col);
void fill_rect_a(Canvas* c, int x, int y, int w, int h, uint32_t col, uint8_t alpha);
void hline(Canvas* c, int x, int y, int len, uint32_t col);
void vline(Canvas* c, int x, int y, int len, uint32_t col);
void fill_tri(Canvas* c, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t col);
// blend RGBA src onto canvas
void blit_rgba(Canvas* dst, int dx, int dy, const uint32_t* src, int sw, int sh);
// fast row-memcpy blit for opaque Canvas sprite
void blit_canvas_opaque(Canvas* dst, int dx, int dy, const Canvas* src);
// blend with rounded-corner mask (radius = corner)
void blit_rgba_round(Canvas* dst, int dx, int dy, const uint32_t* src, int sw, int sh, int corner);
// fast path for opaque RGBA images with rounded-corner mask (memcpy middle rows)
void blit_rgb_round(Canvas* dst, int dx, int dy, const uint32_t* src, int sw, int sh, int corner);
// fast full-screen opaque cross-fade: dst = src * a/255 + dst * (255-a)/255
void blend_over_opaque(uint32_t* dst, const uint32_t* src, size_t n, uint8_t a);
// 带整体透明度的 blit（a=0 完全透明，255 完全不透明），用于交叉淡化
void blit_rgba_a(Canvas* dst, int dx, int dy, const uint32_t* src, int sw, int sh, uint8_t a);
void blit_rgba_round_a(Canvas* dst, int dx, int dy, const uint32_t* src, int sw, int sh, int corner, uint8_t a);
// bilinear resample RGBA
void resample_bilinear(const uint32_t* src, int sw, int sh, uint32_t* dst, int dw, int dh);
// horizontal+vertical box blur in place (RGBA, alpha ignored)
void box_blur(uint32_t* img, int w, int h, int radius, int passes);
// multiply rgb by factor, keep alpha
void scale_rgb(uint32_t* img, int n, float f);

// ---- 抗锯齿原语（消除 UI 锯齿） ----
// 抗锯齿圆角矩形填充（内部快速路径，边缘按覆盖度混合）
void fill_round_rect_aa(Canvas* c, int x, int y, int w, int h, int r, uint32_t col);
// 抗锯齿圆角矩形 + 整体透明度（Material 半透明卡片）
void fill_round_rect_aa_a(Canvas* c, int x, int y, int w, int h, int r, uint32_t col, uint8_t alpha);
// 抗锯齿圆角矩形描边（空心边框，不会遮挡卡片内部内容）
void stroke_round_rect_aa(Canvas* c, int x, int y, int w, int h, int r, int thickness, uint32_t col, uint8_t alpha);
// 柔和多层半透明投影（替代纯黑阴影方块）
void fill_round_rect_shadow(Canvas* c, int x, int y, int w, int h, int r, uint8_t max_alpha);
// 抗锯齿椭圆填充
void fill_ellipse_aa(Canvas* c, int cx, int cy, int rx, int ry, uint32_t col);
// 抗锯齿空心圆环
void stroke_circle_aa(Canvas* c, int cx, int cy, int r, int thickness, uint32_t col, uint8_t alpha);
// 抗锯齿三角形填充（播放/切歌等图标）
void fill_tri_aa(Canvas* c, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t col);
// 抗锯齿圆形（带透明度），用于 Material 水波纹 / 触点反馈
void fill_circle_a(Canvas* c, int cx, int cy, int r, uint32_t col, uint8_t a);
// 抗锯齿圆形（带透明度 + 矩形裁剪）：水波纹只在卡片范围内扩散
void fill_circle_clip_a(Canvas* c, int cx, int cy, int r, uint32_t col, uint8_t a,
                        int clipx, int clipy, int clipw, int cliph);
// Material You 粗波浪进度条（已播放段正弦波浪 + 未播放段圆角轨 + 竖向胶囊滑块）
void draw_m3_wavy_progress(Canvas* c, int x0, int x1, int mid_y, float frac,
                           float phase, float wave_amp, float wave_period,
                           int wave_thick, int track_thick, int draw_thumb,
                           uint32_t col_active, uint32_t col_track);

// ---- text ----
typedef struct Font Font;
Font* font_load(const void* data, size_t len);
void font_free(Font* f);
int  text_width(Font* f, float px, const char* utf8);
// draw single line, top-anchored at y
void draw_text(Canvas* c, Font* f, float px, int x, int y, uint32_t col, const char* utf8);
// centered horizontally in [x0,x1]
void draw_text_centered(Canvas* c, Font* f, float px, int x0, int x1, int y, uint32_t col, const char* utf8);
// right-aligned at x1
void draw_text_right(Canvas* c, Font* f, float px, int x1, int y, uint32_t col, const char* utf8);
// truncate with ellipsis if wider than max_w; returns 1 if truncated
int  draw_text_max(Canvas* c, Font* f, float px, int x, int y, uint32_t col, const char* utf8, int max_w);
int  text_width_max(Font* f, float px, const char* utf8, int max_w);
