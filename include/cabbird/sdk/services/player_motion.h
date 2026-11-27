#pragma once
#include "cabbird/sdk/base.h"

#define CABBIRD_UNITY_PLAYER_MOTION_SERVICE_V1_ID "cabbird.unity.player-motion"
#define CABBIRD_UNITY_PLAYER_MOTION_SERVICE_V1_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif
typedef enum CabbirdUnityPlayerMotionModeV1 {
    CABBIRD_UNITY_PLAYER_MOTION_V1_HOLD_POSITION = 1,
    CABBIRD_UNITY_PLAYER_MOTION_V1_OVERRIDE_VELOCITY = 2
} CabbirdUnityPlayerMotionModeV1;
typedef enum CabbirdUnityPlayerMotionFlagsV1 {
    CABBIRD_UNITY_PLAYER_MOTION_V1_FREE_GROUND = 1u
} CabbirdUnityPlayerMotionFlagsV1;
typedef struct CabbirdUnityPlayerMotionRequestV1 {
    uint32_t struct_size;
    uint32_t mode;
    uint32_t flags;
    uint32_t reserved;
    uint64_t entity_id;
    double velocity[3];
} CabbirdUnityPlayerMotionRequestV1;
/* acquire/update run in Game and require the current controlled entity's data
 * identity. Hold wins over velocity; the latest updated velocity request wins
 * among velocity requests. Hold skips native movement and its overlap recovery.
 * FREE_GROUND restores the previous native constraint after each Move call.
 * Hold also suppresses local-player position recovery for that native tick only.
 * release only removes host intent, is valid from any domain and performs no
 * Unity I/O. Handles are owned by the caller's PluginScope and revoked on stop.
 * Scene or controlled-object changes invalidate requests; stale handles return
 * NOT_FOUND. This is not a teleport API and performs no position writes. */
typedef struct CabbirdUnityPlayerMotionServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    CabbirdStatusV1 (CABBIRD_CALL *acquire)(void* user,
        const CabbirdUnityPlayerMotionRequestV1* request, CabbirdGenerationHandleV1* handle);
    CabbirdStatusV1 (CABBIRD_CALL *update)(void* user, CabbirdGenerationHandleV1 handle,
        const CabbirdUnityPlayerMotionRequestV1* request);
    CabbirdStatusV1 (CABBIRD_CALL *release)(void* user, CabbirdGenerationHandleV1 handle);
} CabbirdUnityPlayerMotionServiceV1;
#ifdef __cplusplus
}
#endif
