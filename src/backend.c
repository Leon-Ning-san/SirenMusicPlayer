#include "backend.h"
#include <switch.h>
#include <string.h>

static Framebuffer fb;
static PadState pad;

int be_init(void) {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    Result rc = framebufferCreate(&fb, nwindowGetDefault(), BE_W, BE_H, PIXEL_FORMAT_RGBA_8888, 3);
    if (R_FAILED(rc)) return -1;
    // 关键：默认 framebuffer 是 Tegra block-linear 布局，必须转为线性，
    // 否则按行 memcpy 写入的数据在屏幕上会错乱/无法显示。
    rc = framebufferMakeLinear(&fb);
    if (R_FAILED(rc)) return -1;
    return 0;
}

void be_exit(void) {
    framebufferClose(&fb);
}

uint32_t* be_begin(int* stride) {
    u32 s = 0;
    void* p = framebufferBegin(&fb, &s);
    if (stride) *stride = (int)(s / 4);
    return (uint32_t*)p;
}

void be_end(void) {
    framebufferEnd(&fb);
}

void be_input_update(void) {
    // libnx 必须每帧调用 padUpdate 刷新手柄状态，
    // 否则 padGetButtonsDown / padGetButtons 永远返回旧值（按键无响应）。
    padUpdate(&pad);
}

uint64_t be_btn_down(void) {
    return padGetButtonsDown(&pad);
}

uint64_t be_btn_held(void) {
    return padGetButtons(&pad);
}

void be_sleep_ms(int ms) {
    svcSleepThread((uint64_t)ms * 1000000ULL);
}

int be_should_run(void) {
    return appletMainLoop();
}

// 触摸状态重置标志（软键盘等系统界面关闭后消费一次）
static int g_touch_reset = 0;

void be_touch_update(TouchInfo* out) {
    memset(out, 0, sizeof(*out));

    static int was_down = 0;
    static int prev_x = 0, prev_y = 0;

    if (g_touch_reset) {
        // 丢弃当帧事件并强制回到"未按下"基准，只消费一次标志
        was_down = 0;
        prev_x = 0; prev_y = 0;
        g_touch_reset = 0;
        return;
    }

    HidTouchScreenState st;
    memset(&st, 0, sizeof(st));
    size_t n = hidGetTouchScreenStates(&st, 1);
    int touching = (n > 0 && st.count > 0);

    if (touching) {
        int x = (int)st.touches[0].x;
        int y = (int)st.touches[0].y;
        // 坐标裁剪到画面范围
        if (x < 0) x = 0;
        if (x >= BE_W) x = BE_W - 1;
        if (y < 0) y = 0;
        if (y >= BE_H) y = BE_H - 1;

        out->held = 1;
        out->x = x;
        out->y = y;
        if (!was_down) {
            out->down = 1;
            prev_x = x; prev_y = y;
        }
        out->dx = x - prev_x;
        out->dy = y - prev_y;
        prev_x = x; prev_y = y;
        was_down = 1;
    } else {
        if (was_down) {
            out->up = 1;
            // 松开帧保留最后触点，方便点击判定
            out->x = prev_x;
            out->y = prev_y;
        }
        was_down = 0;
    }
}

// 重置内部触摸状态：软键盘等阻塞式系统界面关闭后调用。
// 用显式标志让下一帧 be_touch_update 强制清零 was_down 并丢弃当帧事件，
// 防止残留触摸状态产生带旧坐标的幽灵 up/down 导致误触。
void be_touch_reset(void) {
    g_touch_reset = 1;
}

int be_is_desktop(void) { return 0; }
int be_wheel_delta(void) { return 0; }
const char* be_poll_dropped_path(void) { return NULL; }
int be_pick_folder(char* out, size_t cap) { (void)out; (void)cap; return 0; }
void be_text_input(int enable) { (void)enable; }
int be_poll_text_char(char* utf8_out, size_t cap, int* action) { (void)utf8_out; (void)cap; (void)action; return 0; }
int be_poll_hotkey(void) { return 0; }
const char* be_config_dir(void) { return "sdmc:/Sirene"; }
int be_default_music_dirs(char dirs[][600], int max_dirs) { (void)dirs; (void)max_dirs; return 0; }
void be_set_cli_path(const char* p) { (void)p; }
void be_mouse_pos(int* x, int* y) { if (x) *x = -1; if (y) *y = -1; }
void be_set_cursor(int hand) { (void)hand; }
