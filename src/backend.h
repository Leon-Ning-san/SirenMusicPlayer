#pragma once
#include <stdint.h>

#define BE_W 1280
#define BE_H 720

int be_init(void);
void be_exit(void);
// returns current backbuffer + stride (in pixels)
uint32_t* be_begin(int* stride);
void be_end(void);
void be_input_update(void);   // 每帧刷新手柄状态（必须在读取按键前调用）
uint64_t be_btn_down(void);
uint64_t be_btn_held(void);
void be_sleep_ms(int ms);
int be_should_run(void);

// 触摸屏状态，坐标已映射到 1280x720 画面空间
typedef struct {
    int down;       // 本帧刚刚按下
    int held;       // 本帧仍按住
    int up;         // 本帧刚刚松开
    int x, y;       // 当前触点
    int dx, dy;     // 相对上一帧的位移
} TouchInfo;

void be_touch_update(TouchInfo* out);
void be_touch_reset(void);   // 软键盘等系统界面关闭后清除残留触摸状态
