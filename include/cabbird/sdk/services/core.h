#pragma once
#include "cabbird/sdk/base.h"
#define CABBIRD_CORE_SERVICE_V1_ID "cabbird.core"
#define CABBIRD_CORE_SERVICE_V1_VERSION 1u
#ifdef __cplusplus
extern "C" {
#endif
typedef enum CabbirdCoreLogLevelV1 {
    CABBIRD_CORE_LOG_LEVEL_V1_TRACE = 0,
    CABBIRD_CORE_LOG_LEVEL_V1_INFO = 1,
    CABBIRD_CORE_LOG_LEVEL_V1_WARNING = 2,
    CABBIRD_CORE_LOG_LEVEL_V1_ERROR = 3
} CabbirdCoreLogLevelV1;

typedef struct CabbirdCoreServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    void (CABBIRD_CALL *log)(void* user, uint32_t level, CabbirdStringViewV1 message);
    CabbirdStatusV1 (CABBIRD_CALL *read_memory)(void* user, uintptr_t address, CabbirdMutableByteSpanV1 destination);
    CabbirdStatusV1 (CABBIRD_CALL *write_memory)(void* user, uintptr_t address, CabbirdByteSpanV1 source);
    CabbirdStatusV1 (CABBIRD_CALL *plugin_directory)(void* user, char* destination, size_t* inout_size);
    /* Cabbird additions follow the shared Core V1 prefix. Data writes retain
     * existing protection; patch_memory explicitly permits a protection change. */
    CabbirdStatusV1 (CABBIRD_CALL *patch_memory)(
        void* user, uintptr_t address, CabbirdByteSpanV1 source);
    uintptr_t (CABBIRD_CALL *module_base)(void* user, CabbirdStringViewV1 module_name);
    /* Borrowed until the next host call on this thread. */
    CabbirdStatusV1 (CABBIRD_CALL *last_status)(void* user);
} CabbirdCoreServiceV1;
#ifdef __cplusplus
}
#endif
