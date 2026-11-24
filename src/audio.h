#pragma once
#include <stddef.h>
#include <stdint.h>

int  audio_init(void);
void audio_exit(void);

// async: decoder thread opens file & starts filling. 0 = command queued
int  audio_play(const char* path);
void audio_pause(void);
void audio_resume(void);
int  audio_paused(void);
void audio_stop(void);

void audio_set_volume(float v);
float audio_volume(void);

double audio_duration(void);   // seconds (0 while opening)
double audio_position(void);   // seconds
int    audio_seek(double sec); // 0 ok
uint32_t audio_sample_rate(void);
uint32_t audio_channels(void);

// 0 = idle, 1 = opening, 2 = ready, 3 = failed
int  audio_status(void);

// track fully ended (decoder EOF, ring drained, no buffers in flight)
int  audio_finished(void);

// 28 log-spaced band magnitudes 0..1
void audio_get_spectrum(float out[28]);

// feed audout from ring; call once per frame
void audio_pump(void);

// 自检诊断：把音频链路状态写入 buf，供界面显示
void audio_get_diag(char* buf, size_t cap);
