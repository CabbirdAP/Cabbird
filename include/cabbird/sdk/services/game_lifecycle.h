#pragma once
#include "cabbird/sdk/base.h"

#define CABBIRD_GAME_LIFECYCLE_SERVICE_V1_ID "cabbird.game-lifecycle"
#define CABBIRD_GAME_LIFECYCLE_SERVICE_V1_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

typedef CabbirdStatusV1 (CABBIRD_CALL *CabbirdGameStopCleanupV1)(void* user);

/* Scope-owned restoration of game state before plugin Stop/Unload. The host
 * freezes ordinary callbacks, removes hooks and drains their leases first, then
 * runs registered cleanups in reverse registration order on Game under a
 * lifecycle lease. No filesystem I/O is allowed in a cleanup. A failed cleanup
 * or deadline quarantines the generation; its DLL/context remain mapped.
 * callback_user must remain alive until unregister succeeds or on_unload.
 * Register during Load/Start, before making persistent game-state changes. */
typedef struct CabbirdGameLifecycleServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    CabbirdStatusV1 (CABBIRD_CALL *register_stop_cleanup)(
        void* user, CabbirdGameStopCleanupV1 callback, void* callback_user,
        CabbirdGenerationHandleV1* handle);
    CabbirdStatusV1 (CABBIRD_CALL *unregister_stop_cleanup)(
        void* user, CabbirdGenerationHandleV1 handle);
} CabbirdGameLifecycleServiceV1;

#ifdef __cplusplus
}
#endif
