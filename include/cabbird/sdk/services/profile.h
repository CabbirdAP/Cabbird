#pragma once
#include "cabbird/sdk/base.h"

#define CABBIRD_PROFILE_SERVICE_V1_ID "cabbird.profile"
#define CABBIRD_PROFILE_SERVICE_V1_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

// Reads profile/<relative_path> from this plugin's immutable package generation
// and validates plugin-profile.schema.json. Lifecycle/Worker only. The returned
// document is an cabbird.json handle; inspect and release it through that service.
// Both services require the json capability. No paths outside profile/ are allowed.
typedef struct CabbirdProfileServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    CabbirdStatusV1 (CABBIRD_CALL *load)(
        void* user, CabbirdStringViewV1 relative_path,
        CabbirdGenerationHandleV1* document);
} CabbirdProfileServiceV1;

#ifdef __cplusplus
}
#endif
