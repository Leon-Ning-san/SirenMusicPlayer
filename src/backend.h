#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BE_W 1280
#define BE_H 720

int be_init(void);
void be_exit(void);
// returns current backbuffer + stride (in pixels)
uint32_t* be_begin(int* stride);
void be_end(void);
void be_input_update(void);
uint64_t be_btn_down(void);
uint64_t be_btn_held(void);
void be_sleep_ms(int ms);
int be_should_run(void);

// 触摸屏 / 鼠标状态（桌面端已映射为 720x1280 竖屏坐标）
typedef struct {
    int down;
    int held;
    int up;
    int x, y;
    int dx, dy;
} TouchInfo;

void be_touch_update(TouchInfo* out);
void be_touch_reset(void);

// -------- 桌面扩展接口（Switch 端为空实现） --------
int be_is_desktop(void);
int be_wheel_delta(void);
const char* be_poll_dropped_path(void);
int be_pick_folder(char* out, size_t cap);
void be_text_input(int enable);
// 返回 1 表示收到文本或按键动作: action 0=普通字符(utf8_out), 1=退格, 2=回车, 3=ESC
int be_poll_text_char(char* utf8_out, size_t cap, int* action);
// 桌面快捷键事件: 'f'=选目录, 's'=搜索, 't'=切换主题, 'm'=播放模式, 'c'=自检, 'v'=音量
int be_poll_hotkey(void);
const char* be_config_dir(void);
int be_default_music_dirs(char dirs[][600], int max_dirs);
void be_set_cli_path(const char* p);
void be_mouse_pos(int* x, int* y);
void be_set_cursor(int hand);
void be_viewport_size(int* out_w, int* out_h);
void be_screen_metrics(int* out_phys_w, int* out_phys_h, float* out_scale);
void be_set_dark_titlebar(int dark);
void be_toggle_fullscreen(void);
int  be_is_fullscreen(void);
int  be_ensure_desktop_shortcut(int only_repair);
void be_buffer_size(int* out_w, int* out_h);
int  be_is_minimized(void);
// 媒体控制命令队列（任务栏缩略图按钮 / 系统媒体键）：0=无, 1=播放/暂停, 2=上一首, 3=下一首, 4=暂停/停止
int  be_poll_media_cmd(void);
void be_set_media_state(int has_track, int playing, float frac, const char* title, const char* artist);
int  be_input_is_touch(void);
void be_set_idle_mode(int idle);

#ifdef __cplusplus
}
#endif
