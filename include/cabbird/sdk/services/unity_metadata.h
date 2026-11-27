#pragma once

#include "cabbird/sdk/base.h"

#define CABBIRD_UNITY_METADATA_SERVICE_V1_ID "cabbird.unity.metadata"
#define CABBIRD_UNITY_METADATA_SERVICE_V1_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

typedef enum CabbirdUnityMethodKindV1 {
    CABBIRD_UNITY_METHOD_V1_INSTANCE = 0,
    CABBIRD_UNITY_METHOD_V1_STATIC = 1
} CabbirdUnityMethodKindV1;

typedef struct CabbirdUnityMethodRequestV1 {
    uint32_t struct_size;
    uint32_t method_kind;
    CabbirdStringViewV1 module_name;
    CabbirdStringViewV1 type_name;
    CabbirdStringViewV1 method_name;
    CabbirdStringViewV1 return_type;
    const CabbirdStringViewV1* parameter_types;
    size_t parameter_count;
} CabbirdUnityMethodRequestV1;

typedef struct CabbirdUnityMethodResultV1 {
    uint32_t struct_size;
    uint32_t reserved;
    uintptr_t address;
    uint64_t rva;
} CabbirdUnityMethodResultV1;

typedef struct CabbirdUnityFieldRequestV1 {
    uint32_t struct_size;
    uint32_t storage_size;
    CabbirdStringViewV1 type_name;
    CabbirdStringViewV1 field_name;
    CabbirdStringViewV1 field_type;
} CabbirdUnityFieldRequestV1;

typedef struct CabbirdUnityFieldResultV1 {
    uint32_t struct_size;
    uint32_t offset;
    uint32_t storage_size;
    uint32_t reserved;
} CabbirdUnityFieldResultV1;

// Read-only metadata queries, synchronous on the observed Game thread only.
// Requires unity-metadata. Inputs are borrowed for the call; outputs contain no
// managed object or MethodInfo pointers. Resolution requires a unique complete
// method shape and executable address inside module_name. Instance fields must
// match their declared type, storage size and live class bounds.
typedef struct CabbirdUnityMetadataServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    CabbirdStatusV1 (CABBIRD_CALL *resolve_method)(
        void* user, const CabbirdUnityMethodRequestV1* request,
        CabbirdUnityMethodResultV1* result);
    CabbirdStatusV1 (CABBIRD_CALL *resolve_instance_field)(
        void* user, const CabbirdUnityFieldRequestV1* request,
        CabbirdUnityFieldResultV1* result);
} CabbirdUnityMetadataServiceV1;

#ifdef __cplusplus
}
#endif
