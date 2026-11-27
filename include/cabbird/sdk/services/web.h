#pragma once

#include "cabbird/sdk/base.h"

#define CABBIRD_WEB_SERVICE_V1_ID "cabbird.web"
#define CABBIRD_WEB_SERVICE_V1_VERSION 1u
#define CABBIRD_WEB_MAX_REQUEST_BYTES_V1 65536u
#define CABBIRD_WEB_MAX_RESPONSE_BYTES_V1 1048576u

#ifdef __cplusplus
extern "C" {
#endif

typedef enum CabbirdWebContentTypeV1 {
    CABBIRD_WEB_CONTENT_TYPE_V1_JSON = 0,
    CABBIRD_WEB_CONTENT_TYPE_V1_TEXT = 1
} CabbirdWebContentTypeV1;

typedef struct CabbirdWebRequestV1 {
    uint32_t struct_size;
    uint32_t reserved;
    CabbirdStringViewV1 method;
    // Application-relative path, including a leading slash (for example /api/state).
    CabbirdStringViewV1 path;
    CabbirdStringViewV1 body;
} CabbirdWebRequestV1;

typedef struct CabbirdWebResponseV1 {
    uint32_t struct_size;
    uint32_t http_status;
    uint32_t content_type;
    uint32_t reserved;
    // Host-owned writable storage; capacity is MAX_RESPONSE_BYTES. Do not replace it.
    CabbirdMutableByteSpanV1 body;
    size_t body_size;
} CabbirdWebResponseV1;

// Invoked once per authenticated API request on Runtime Worker, with a generation
// lease. Copy response bytes before returning. Game work must be enqueued for on_update.
typedef CabbirdStatusV1 (CABBIRD_CALL *CabbirdWebCallbackV1)(
    void* user, const CabbirdWebRequestV1* request, CabbirdWebResponseV1* response);

typedef struct CabbirdWebServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    // Lifecycle/Worker only. Assets are relative to this DLL generation's package,
    // preloaded by Runtime Worker. One application per plugin generation.
    CabbirdStatusV1 (CABBIRD_CALL *register_application)(
        void* user, CabbirdStringViewV1 assets_relative_directory,
        CabbirdWebCallbackV1 callback, void* callback_user,
        CabbirdGenerationHandleV1* application);
    CabbirdStatusV1 (CABBIRD_CALL *unregister_application)(
        void* user, CabbirdGenerationHandleV1 application);
    // Two-call NUL-terminated string protocol. URL includes the session token in
    // #token=...; API callers send it as X-Cabbird-Token. Never persist this URL.
    CabbirdStatusV1 (CABBIRD_CALL *application_url)(
        void* user, CabbirdGenerationHandleV1 application,
        char* destination, size_t* inout_size);
} CabbirdWebServiceV1;

#ifdef __cplusplus
}
#endif
