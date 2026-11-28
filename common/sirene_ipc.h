#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SIRENE_IPC_SERVICE_NAME  "sir:bg"
#define SIRENE_IPC_API_VERSION   0x00010000u
#define SIRENE_BG_STATUS_MAGIC   0x47425253u /* 'SRBG' */
#define SIRENE_BG_MAX_PATH       512
#define SIRENE_BG_MAX_TITLE      192
#define SIRENE_BG_MAX_ARTIST     128
#define SIRENE_BG_MAX_QUEUE      512

typedef enum SireneBgState {
    SIR_STATE_IDLE    = 0,
    SIR_STATE_OPENING = 1,
    SIR_STATE_PLAYING = 2,
    SIR_STATE_PAUSED  = 3,
    SIR_STATE_ERROR   = 4
} SireneBgState;

typedef enum SireneBgMode {
    SIR_MODE_LOOP_ALL = 0,
    SIR_MODE_LOOP_ONE = 1,
    SIR_MODE_SHUFFLE  = 2,
    SIR_MODE_COUNT    = 3
} SireneBgMode;

typedef enum SireneIpcCmd {
    SIR_CMD_GET_API_VERSION  = 0,
    SIR_CMD_GET_STATUS       = 1,
    SIR_CMD_PLAY_PATH        = 2,
    SIR_CMD_SET_PAUSED       = 3,
    SIR_CMD_STOP             = 4,
    SIR_CMD_NEXT             = 5,
    SIR_CMD_PREV             = 6,
    SIR_CMD_SEEK             = 7,
    SIR_CMD_SET_VOLUME       = 8,
    SIR_CMD_SET_MODE         = 9,
    SIR_CMD_QUEUE_CLEAR      = 10,
    SIR_CMD_QUEUE_ADD        = 11,
    SIR_CMD_QUEUE_PLAY_INDEX = 12,
    SIR_CMD_SCAN_LIBRARY     = 13,
    SIR_CMD_GET_QUEUE_ITEM   = 14
} SireneIpcCmd;

typedef struct SireneBgStatus {
    uint32_t magic;
    uint32_t state;
    uint32_t play_mode;
    int32_t  queue_index;
    uint32_t queue_count;
    float    volume;
    double   position_sec;
    double   duration_sec;
    uint32_t sample_rate;
    uint32_t channels;
    char     path[SIRENE_BG_MAX_PATH];
    char     title[SIRENE_BG_MAX_TITLE];
    char     artist[SIRENE_BG_MAX_ARTIST];
} SireneBgStatus;

typedef struct SireneBgQueueItem {
    char path[SIRENE_BG_MAX_PATH];
    char title[SIRENE_BG_MAX_TITLE];
} SireneBgQueueItem;

typedef struct SireneBgPlayIndexArgs {
    int32_t  index;
    uint32_t reserved;
    double   start_sec;
} SireneBgPlayIndexArgs;

/* Client API (available on __SWITCH__ builds: Sirène.nro & SireneOverlay.ovl) */
int  sirene_ipc_connect(void);
void sirene_ipc_disconnect(void);
int  sirene_ipc_is_connected(void);

int  sirene_ipc_get_api_version(uint32_t* out_version);
int  sirene_ipc_get_status(SireneBgStatus* out_status);
int  sirene_ipc_play_path(const char* path, double start_sec);
int  sirene_ipc_set_paused(uint32_t mode); /* 0=resume, 1=pause, 2=toggle */
int  sirene_ipc_stop(void);
int  sirene_ipc_next(void);
int  sirene_ipc_prev(void);
int  sirene_ipc_seek(double sec);
int  sirene_ipc_set_volume(float vol);
int  sirene_ipc_set_mode(uint32_t mode);
int  sirene_ipc_queue_clear(void);
int  sirene_ipc_queue_add(const char* path);
int  sirene_ipc_queue_play_index(int32_t index, double start_sec);
int  sirene_ipc_scan_library(uint32_t* out_count);
int  sirene_ipc_get_queue_item(uint32_t index, SireneBgQueueItem* out_item);

#ifdef __cplusplus
}
#endif
