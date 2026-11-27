#pragma once

#include "cabbird/sdk/base.h"

#define CABBIRD_WEBSOCKET_SERVICE_V1_ID "cabbird.websocket"
#define CABBIRD_WEBSOCKET_SERVICE_V1_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

// This service owns a process-local WebSocket broadcast server. Plugins only
// enqueue UTF-8 text frames; connection management and socket I/O stay in the
// Runtime worker owned by the host.
typedef struct CabbirdWebSocketServerInfoV1 {
    uint32_t struct_size;
    uint16_t port;
    uint16_t reserved;
    uint32_t connected_clients;
    uint64_t published_messages;
    uint64_t dropped_messages;
} CabbirdWebSocketServerInfoV1;

typedef struct CabbirdWebSocketServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    CabbirdStatusV1 (CABBIRD_CALL *publish_text)(
        void* user, CabbirdStringViewV1 message);
    CabbirdStatusV1 (CABBIRD_CALL *server_info)(
        void* user, CabbirdWebSocketServerInfoV1* info);
    // Optional V1 tail. Check struct_size before accessing this operation.
    CabbirdStatusV1 (CABBIRD_CALL *set_port)(void* user, uint16_t port);
} CabbirdWebSocketServiceV1;

#ifdef __cplusplus
}
#endif
