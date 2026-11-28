#include "sirene_ipc.h"
#include <string.h>

#ifdef __SWITCH__
#include <switch.h>

static Service g_sir_srv;
static int g_sir_connected = 0;

int sirene_ipc_connect(void) {
    if (g_sir_connected && serviceIsActive(&g_sir_srv)) return 0;
    memset(&g_sir_srv, 0, sizeof(g_sir_srv));
    Result rc = smGetService(&g_sir_srv, SIRENE_IPC_SERVICE_NAME);
    if (R_FAILED(rc)) {
        g_sir_connected = 0;
        return -1;
    }
    g_sir_connected = 1;
    return 0;
}

void sirene_ipc_disconnect(void) {
    if (g_sir_connected && serviceIsActive(&g_sir_srv)) {
        serviceClose(&g_sir_srv);
    }
    memset(&g_sir_srv, 0, sizeof(g_sir_srv));
    g_sir_connected = 0;
}

int sirene_ipc_is_connected(void) {
    return (g_sir_connected && serviceIsActive(&g_sir_srv)) ? 1 : 0;
}

static int ensure_connected(void) {
    if (sirene_ipc_is_connected()) return 0;
    return sirene_ipc_connect();
}

int sirene_ipc_get_api_version(uint32_t* out_version) {
    if (!out_version) return -1;
    if (ensure_connected() != 0) return -1;
    u32 ver = 0;
    Result rc = serviceDispatchOut(&g_sir_srv, SIR_CMD_GET_API_VERSION, ver);
    if (R_FAILED(rc)) {
        sirene_ipc_disconnect();
        return -1;
    }
    *out_version = ver;
    return 0;
}

int sirene_ipc_get_status(SireneBgStatus* out_status) {
    if (!out_status) return -1;
    memset(out_status, 0, sizeof(*out_status));
    if (ensure_connected() != 0) return -1;
    Result rc = serviceDispatch(&g_sir_srv, SIR_CMD_GET_STATUS,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { out_status, sizeof(*out_status) } },
    );
    if (R_FAILED(rc)) {
        sirene_ipc_disconnect();
        return -1;
    }
    out_status->path[SIRENE_BG_MAX_PATH - 1] = '\0';
    out_status->title[SIRENE_BG_MAX_TITLE - 1] = '\0';
    out_status->artist[SIRENE_BG_MAX_ARTIST - 1] = '\0';
    return (out_status->magic == SIRENE_BG_STATUS_MAGIC) ? 0 : -1;
}

int sirene_ipc_play_path(const char* path, double start_sec) {
    if (!path || !path[0]) return -1;
    if (ensure_connected() != 0) return -1;
    char buf[SIRENE_BG_MAX_PATH];
    memset(buf, 0, sizeof(buf));
    strncpy(buf, path, sizeof(buf) - 1);
    Result rc = serviceDispatchIn(&g_sir_srv, SIR_CMD_PLAY_PATH, start_sec,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
        .buffers = { { buf, sizeof(buf) } },
    );
    return R_SUCCEEDED(rc) ? 0 : -1;
}

int sirene_ipc_set_paused(uint32_t mode) {
    if (ensure_connected() != 0) return -1;
    u32 m = mode;
    Result rc = serviceDispatchIn(&g_sir_srv, SIR_CMD_SET_PAUSED, m);
    return R_SUCCEEDED(rc) ? 0 : -1;
}

int sirene_ipc_stop(void) {
    if (ensure_connected() != 0) return -1;
    Result rc = serviceDispatch(&g_sir_srv, SIR_CMD_STOP);
    return R_SUCCEEDED(rc) ? 0 : -1;
}

int sirene_ipc_next(void) {
    if (ensure_connected() != 0) return -1;
    Result rc = serviceDispatch(&g_sir_srv, SIR_CMD_NEXT);
    return R_SUCCEEDED(rc) ? 0 : -1;
}

int sirene_ipc_prev(void) {
    if (ensure_connected() != 0) return -1;
    Result rc = serviceDispatch(&g_sir_srv, SIR_CMD_PREV);
    return R_SUCCEEDED(rc) ? 0 : -1;
}

int sirene_ipc_seek(double sec) {
    if (ensure_connected() != 0) return -1;
    Result rc = serviceDispatchIn(&g_sir_srv, SIR_CMD_SEEK, sec);
    return R_SUCCEEDED(rc) ? 0 : -1;
}

int sirene_ipc_set_volume(float vol) {
    if (ensure_connected() != 0) return -1;
    Result rc = serviceDispatchIn(&g_sir_srv, SIR_CMD_SET_VOLUME, vol);
    return R_SUCCEEDED(rc) ? 0 : -1;
}

int sirene_ipc_set_mode(uint32_t mode) {
    if (ensure_connected() != 0) return -1;
    u32 m = mode;
    Result rc = serviceDispatchIn(&g_sir_srv, SIR_CMD_SET_MODE, m);
    return R_SUCCEEDED(rc) ? 0 : -1;
}

int sirene_ipc_queue_clear(void) {
    if (ensure_connected() != 0) return -1;
    Result rc = serviceDispatch(&g_sir_srv, SIR_CMD_QUEUE_CLEAR);
    return R_SUCCEEDED(rc) ? 0 : -1;
}

int sirene_ipc_queue_add(const char* path) {
    if (!path || !path[0]) return -1;
    if (ensure_connected() != 0) return -1;
    char buf[SIRENE_BG_MAX_PATH];
    memset(buf, 0, sizeof(buf));
    strncpy(buf, path, sizeof(buf) - 1);
    Result rc = serviceDispatch(&g_sir_srv, SIR_CMD_QUEUE_ADD,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
        .buffers = { { buf, sizeof(buf) } },
    );
    return R_SUCCEEDED(rc) ? 0 : -1;
}

int sirene_ipc_queue_play_index(int32_t index, double start_sec) {
    if (ensure_connected() != 0) return -1;
    SireneBgPlayIndexArgs args;
    args.index = index;
    args.reserved = 0;
    args.start_sec = start_sec;
    Result rc = serviceDispatchIn(&g_sir_srv, SIR_CMD_QUEUE_PLAY_INDEX, args);
    return R_SUCCEEDED(rc) ? 0 : -1;
}

int sirene_ipc_scan_library(uint32_t* out_count) {
    if (ensure_connected() != 0) return -1;
    u32 cnt = 0;
    Result rc = serviceDispatchOut(&g_sir_srv, SIR_CMD_SCAN_LIBRARY, cnt);
    if (R_FAILED(rc)) return -1;
    if (out_count) *out_count = cnt;
    return 0;
}

int sirene_ipc_get_queue_item(uint32_t index, SireneBgQueueItem* out_item) {
    if (!out_item) return -1;
    memset(out_item, 0, sizeof(*out_item));
    if (ensure_connected() != 0) return -1;
    u32 idx = index;
    Result rc = serviceDispatchIn(&g_sir_srv, SIR_CMD_GET_QUEUE_ITEM, idx,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { out_item, sizeof(*out_item) } },
    );
    if (R_FAILED(rc)) return -1;
    out_item->path[SIRENE_BG_MAX_PATH - 1] = '\0';
    out_item->title[SIRENE_BG_MAX_TITLE - 1] = '\0';
    return 0;
}

#else

int  sirene_ipc_connect(void) { return -1; }
void sirene_ipc_disconnect(void) {}
int  sirene_ipc_is_connected(void) { return 0; }
int  sirene_ipc_get_api_version(uint32_t* out_version) { (void)out_version; return -1; }
int  sirene_ipc_get_status(SireneBgStatus* out_status) { (void)out_status; return -1; }
int  sirene_ipc_play_path(const char* path, double start_sec) { (void)path; (void)start_sec; return -1; }
int  sirene_ipc_set_paused(uint32_t mode) { (void)mode; return -1; }
int  sirene_ipc_stop(void) { return -1; }
int  sirene_ipc_next(void) { return -1; }
int  sirene_ipc_prev(void) { return -1; }
int  sirene_ipc_seek(double sec) { (void)sec; return -1; }
int  sirene_ipc_set_volume(float vol) { (void)vol; return -1; }
int  sirene_ipc_set_mode(uint32_t mode) { (void)mode; return -1; }
int  sirene_ipc_queue_clear(void) { return -1; }
int  sirene_ipc_queue_add(const char* path) { (void)path; return -1; }
int  sirene_ipc_queue_play_index(int32_t index, double start_sec) { (void)index; (void)start_sec; return -1; }
int  sirene_ipc_scan_library(uint32_t* out_count) { (void)out_count; return -1; }
int  sirene_ipc_get_queue_item(uint32_t index, SireneBgQueueItem* out_item) { (void)index; (void)out_item; return -1; }

#endif
