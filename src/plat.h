#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __SWITCH__
#include <switch.h>
#include <dirent.h>
#include <sys/stat.h>

#define BTN_A             HidNpadButton_A
#define BTN_B             HidNpadButton_B
#define BTN_X             HidNpadButton_X
#define BTN_Y             HidNpadButton_Y
#define BTN_L             HidNpadButton_L
#define BTN_R             HidNpadButton_R
#define BTN_ZL            HidNpadButton_ZL
#define BTN_ZR            HidNpadButton_ZR
#define BTN_PLUS          HidNpadButton_Plus
#define BTN_MINUS         HidNpadButton_Minus
#define BTN_UP            HidNpadButton_Up
#define BTN_DOWN          HidNpadButton_Down
#define BTN_LEFT          HidNpadButton_Left
#define BTN_RIGHT         HidNpadButton_Right
#define BTN_STICK_L_UP    HidNpadButton_StickLUp
#define BTN_STICK_L_DOWN  HidNpadButton_StickLDown
#define BTN_STICK_L_LEFT  HidNpadButton_StickLLeft
#define BTN_STICK_L_RIGHT HidNpadButton_StickLRight
#define BTN_STICK_R_UP    HidNpadButton_StickRUp
#define BTN_STICK_R_DOWN  HidNpadButton_StickRDown
#define BTN_STICK_R_LEFT  HidNpadButton_StickRLeft
#define BTN_STICK_R_RIGHT HidNpadButton_StickRRight

typedef Mutex  PlatMutex;
typedef Thread PlatThread;

static inline void plat_mutex_init(PlatMutex* m)   { mutexInit(m); }
static inline void plat_mutex_lock(PlatMutex* m)   { mutexLock(m); }
static inline void plat_mutex_unlock(PlatMutex* m) { mutexUnlock(m); }

static inline int plat_thread_create(PlatThread* t, void (*fn)(void*), void* arg) {
    if (R_FAILED(threadCreate(t, fn, arg, NULL, 0x40000, 0x3B, -2))) return -1;
    threadStart(t);
    return 0;
}
static inline void plat_thread_join(PlatThread* t) {
    threadWaitForExit(t);
    threadClose(t);
}

static inline void plat_sleep_ms(int ms) {
    svcSleepThread((uint64_t)ms * 1000000ULL);
}

static inline uint64_t plat_ticks_ns(void) {
    uint64_t t = armGetSystemTick();
    uint64_t f = armGetSystemTickFreq();
#if defined(__SIZEOF_INT128__)
    return f ? (uint64_t)(((__uint128_t)t * 1000000000ULL) / f) : 0ULL;
#else
    return f ? (uint64_t)((double)t * 1000000000.0 / (double)f) : 0ULL;
#endif
}

typedef DIR PlatDir;
typedef struct {
    char d_name[512];
    int is_dir;
    int is_reg;
} PlatDirent;

static inline FILE* plat_fopen(const char* path, const char* mode) {
    return fopen(path, mode);
}
static inline int plat_mkdir_p(const char* path) {
    return mkdir(path, 0777);
}
static inline PlatDir* plat_opendir(const char* path) {
    return opendir(path);
}
static inline int plat_readdir(PlatDir* d, PlatDirent* out) {
    if (!d) return 0;
    struct dirent* e = readdir(d);
    if (!e) return 0;
    snprintf(out->d_name, sizeof(out->d_name), "%s", e->d_name);
    out->is_dir = 0;
    out->is_reg = 0;
#ifdef DT_DIR
    if (e->d_type == DT_DIR) out->is_dir = 1;
    else if (e->d_type == DT_REG) out->is_reg = 1;
#endif
    return 1;
}
static inline void plat_closedir(PlatDir* d) {
    if (d) closedir(d);
}
static inline int plat_stat_type(const char* path, int* is_dir, int* is_reg) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    *is_dir = S_ISDIR(st.st_mode);
    *is_reg = S_ISREG(st.st_mode);
    return 0;
}

#else // ================= DESKTOP (Windows / Linux / macOS) =================

#define BTN_A             (1ULL << 0)
#define BTN_B             (1ULL << 1)
#define BTN_X             (1ULL << 2)
#define BTN_Y             (1ULL << 3)
#define BTN_L             (1ULL << 4)
#define BTN_R             (1ULL << 5)
#define BTN_ZL            (1ULL << 6)
#define BTN_ZR            (1ULL << 7)
#define BTN_PLUS          (1ULL << 8)
#define BTN_MINUS         (1ULL << 9)
#define BTN_UP            (1ULL << 10)
#define BTN_DOWN          (1ULL << 11)
#define BTN_LEFT          (1ULL << 12)
#define BTN_RIGHT         (1ULL << 13)
#define BTN_STICK_L_UP    (1ULL << 14)
#define BTN_STICK_L_DOWN  (1ULL << 15)
#define BTN_STICK_L_LEFT  (1ULL << 16)
#define BTN_STICK_L_RIGHT (1ULL << 17)
#define BTN_STICK_R_UP    (1ULL << 18)
#define BTN_STICK_R_DOWN  (1ULL << 19)
#define BTN_STICK_R_LEFT  (1ULL << 20)
#define BTN_STICK_R_RIGHT (1ULL << 21)

typedef struct {
    char d_name[512];
    int is_dir;
    int is_reg;
} PlatDirent;

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#include <sys/stat.h>

typedef CRITICAL_SECTION PlatMutex;
typedef struct {
    HANDLE handle;
    void (*fn)(void*);
    void* arg;
} PlatThread;

static inline void plat_mutex_init(PlatMutex* m)   { InitializeCriticalSection(m); }
static inline void plat_mutex_lock(PlatMutex* m)   { EnterCriticalSection(m); }
static inline void plat_mutex_unlock(PlatMutex* m) { LeaveCriticalSection(m); }

static inline unsigned __stdcall plat_win_trampoline(void* p) {
    PlatThread* t = (PlatThread*)p;
    t->fn(t->arg);
    return 0;
}
static inline int plat_thread_create(PlatThread* t, void (*fn)(void*), void* arg) {
    t->fn = fn; t->arg = arg;
    uintptr_t h = _beginthreadex(NULL, 0, plat_win_trampoline, t, 0, NULL);
    t->handle = (HANDLE)h;
    return h ? 0 : -1;
}
static inline void plat_thread_join(PlatThread* t) {
    if (t->handle) {
        WaitForSingleObject(t->handle, INFINITE);
        CloseHandle(t->handle);
        t->handle = NULL;
    }
}

static inline void plat_sleep_ms(int ms) { Sleep((DWORD)ms); }

static inline uint64_t plat_ticks_ns(void) {
    static LARGE_INTEGER freq;
    static int freq_init = 0;
    if (!freq_init) {
        freq.QuadPart = 0;
        QueryPerformanceFrequency(&freq);
        freq_init = 1;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return freq.QuadPart ? (uint64_t)((double)now.QuadPart * 1000000000.0 / (double)freq.QuadPart) : 0ULL;
}

static inline int plat_utf8_to_wide(const char* s, wchar_t* w, int cap) {
    if (!s || !w || cap <= 0) return 0;
    return MultiByteToWideChar(CP_UTF8, 0, s, -1, w, cap);
}

static inline int plat_wide_to_utf8(const wchar_t* w, char* s, int cap) {
    if (!w || !s || cap <= 0) return 0;
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, s, cap, NULL, NULL);
}

static inline FILE* plat_fopen(const char* path, const char* mode) {
    wchar_t wp[1024], wm[32];
    if (!plat_utf8_to_wide(path, wp, 1024)) return fopen(path, mode);
    plat_utf8_to_wide(mode, wm, 32);
    return _wfopen(wp, wm);
}

static inline int plat_mkdir_p(const char* path) {
    wchar_t wp[1024];
    if (plat_utf8_to_wide(path, wp, 1024)) return CreateDirectoryW(wp, NULL) ? 0 : -1;
    return -1;
}

typedef struct {
    HANDLE h;
    WIN32_FIND_DATAW fd;
    int first;
} PlatDir;

static inline PlatDir* plat_opendir(const char* path) {
    if (!path || !*path) return NULL;
    char pat[1024];
    size_t len = strlen(path);
    while (len > 0 && (path[len - 1] == '/' || path[len - 1] == '\\')) len--;
    if (len == 0) snprintf(pat, sizeof(pat), "\\*");
    else snprintf(pat, sizeof(pat), "%.*s\\*", (int)len, path);
    wchar_t wpat[1024];
    if (!plat_utf8_to_wide(pat, wpat, 1024)) return NULL;
    PlatDir* d = (PlatDir*)calloc(1, sizeof(PlatDir));
    if (!d) return NULL;
    d->h = FindFirstFileW(wpat, &d->fd);
    if (d->h == INVALID_HANDLE_VALUE) { free(d); return NULL; }
    d->first = 1;
    return d;
}

static inline int plat_readdir(PlatDir* d, PlatDirent* out) {
    if (!d) return 0;
    if (d->first) {
        d->first = 0;
    } else {
        if (!FindNextFileW(d->h, &d->fd)) return 0;
    }
    plat_wide_to_utf8(d->fd.cFileName, out->d_name, sizeof(out->d_name));
    out->is_dir = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
    out->is_reg = !out->is_dir;
    return 1;
}

static inline void plat_closedir(PlatDir* d) {
    if (d) {
        if (d->h != INVALID_HANDLE_VALUE) FindClose(d->h);
        free(d);
    }
}

static inline int plat_stat_type(const char* path, int* is_dir, int* is_reg) {
    wchar_t wp[1024];
    if (!plat_utf8_to_wide(path, wp, 1024)) return -1;
    DWORD attr = GetFileAttributesW(wp);
    if (attr == INVALID_FILE_ATTRIBUTES) return -1;
    *is_dir = (attr & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
    *is_reg = !(*is_dir);
    return 0;
}

#else // POSIX (Linux / macOS)
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>

typedef pthread_mutex_t PlatMutex;
typedef struct {
    pthread_t pt;
    void (*fn)(void*);
    void* arg;
} PlatThread;

static inline void plat_mutex_init(PlatMutex* m)   { pthread_mutex_init(m, NULL); }
static inline void plat_mutex_lock(PlatMutex* m)   { pthread_mutex_lock(m); }
static inline void plat_mutex_unlock(PlatMutex* m) { pthread_mutex_unlock(m); }

static inline void* plat_posix_trampoline(void* p) {
    PlatThread* t = (PlatThread*)p;
    t->fn(t->arg);
    return NULL;
}
static inline int plat_thread_create(PlatThread* t, void (*fn)(void*), void* arg) {
    t->fn = fn; t->arg = arg;
    return pthread_create(&t->pt, NULL, plat_posix_trampoline, t) == 0 ? 0 : -1;
}
static inline void plat_thread_join(PlatThread* t) {
    pthread_join(t->pt, NULL);
}

static inline void plat_sleep_ms(int ms) {
    usleep((useconds_t)ms * 1000u);
}

static inline uint64_t plat_ticks_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

typedef DIR PlatDir;
static inline FILE* plat_fopen(const char* path, const char* mode) { return fopen(path, mode); }
static inline int plat_mkdir_p(const char* path) { return mkdir(path, 0777); }
static inline PlatDir* plat_opendir(const char* path) { return opendir(path); }
static inline int plat_readdir(PlatDir* d, PlatDirent* out) {
    if (!d) return 0;
    struct dirent* e = readdir(d);
    if (!e) return 0;
    snprintf(out->d_name, sizeof(out->d_name), "%s", e->d_name);
    out->is_dir = 0; out->is_reg = 0;
#ifdef DT_DIR
    if (e->d_type == DT_DIR) out->is_dir = 1;
    else if (e->d_type == DT_REG) out->is_reg = 1;
#endif
    return 1;
}
static inline void plat_closedir(PlatDir* d) { if (d) closedir(d); }
static inline int plat_stat_type(const char* path, int* is_dir, int* is_reg) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    *is_dir = S_ISDIR(st.st_mode);
    *is_reg = S_ISREG(st.st_mode);
    return 0;
}
#endif
#endif
