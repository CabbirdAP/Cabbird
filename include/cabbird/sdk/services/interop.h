#pragma once

#include "cabbird/sdk/base.h"

#define CABBIRD_SIGNATURE_SERVICE_V1_ID "cabbird.interop.signature"
#define CABBIRD_SIGNATURE_SERVICE_V1_VERSION 1u
#define CABBIRD_SIGNATURE_SERVICE_V2_ID CABBIRD_SIGNATURE_SERVICE_V1_ID
#define CABBIRD_SIGNATURE_SERVICE_V2_VERSION 2u
#define CABBIRD_SIGNATURE_SERVICE_V3_ID CABBIRD_SIGNATURE_SERVICE_V1_ID
#define CABBIRD_SIGNATURE_SERVICE_V3_VERSION 3u
#define CABBIRD_SIGNATURE_BATCH_MAX_PATTERNS_V3 128u
#define CABBIRD_SIGNATURE_LAYOUT_MAX_FIELDS_V2 32u
#define CABBIRD_SIGNATURE_LAYOUT_MAX_WITNESSES_V2 32u
#define CABBIRD_HOOK_SERVICE_V1_ID "cabbird.interop.hook"
#define CABBIRD_HOOK_SERVICE_V1_VERSION 1u
#define CABBIRD_PATCH_SERVICE_V1_ID "cabbird.interop.patch"
#define CABBIRD_PATCH_SERVICE_V1_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

typedef enum CabbirdHookKindV1 {
    CABBIRD_HOOK_V1_FUNCTION = 1,
    CABBIRD_HOOK_V1_IAT = 2,
    CABBIRD_HOOK_V1_EXPORT = 3,
    CABBIRD_HOOK_V1_VTABLE = 4
} CabbirdHookKindV1;

typedef struct CabbirdHookRequestV1 {
    uint32_t struct_size;
    uint32_t kind;
    uintptr_t target;
    void* detour;
    CabbirdStringViewV1 label;
} CabbirdHookRequestV1;

typedef struct CabbirdSignatureServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    CabbirdStatusV1 (CABBIRD_CALL *resolve)(
        void* user,
        CabbirdStringViewV1 module_name,
        CabbirdStringViewV1 section_name,
        CabbirdStringViewV1 pattern,
        uintptr_t* address);
} CabbirdSignatureServiceV1;

typedef enum CabbirdSignatureMemberAccessV2 {
    CABBIRD_SIGNATURE_MEMBER_READ_V2 = 1,
    CABBIRD_SIGNATURE_MEMBER_WRITE_V2 = 2,
    CABBIRD_SIGNATURE_MEMBER_ADDRESS_V2 = 4
} CabbirdSignatureMemberAccessV2;

/* Seeds identify fields in the original instructions; they are never fallback values. */
typedef struct CabbirdSignatureFieldV2 {
    uint32_t struct_size;
    CabbirdStringViewV1 id;
    uint32_t seed_offset;
    uint32_t alignment;
} CabbirdSignatureFieldV2;

typedef struct CabbirdSignatureMemberV2 {
    uint32_t struct_size;
    uint32_t field_index;
    int32_t addend; /* instruction member offset = field offset + addend */
    uint32_t width; /* bytes read/written; address-only uses do not access the field */
    uint32_t access; /* CabbirdSignatureMemberAccessV2 flags */
} CabbirdSignatureMemberV2;

/* Witnesses begin at function entry with the object in RCX (Windows x64). */
typedef struct CabbirdSignatureWitnessV2 {
    uint32_t struct_size;
    CabbirdStringViewV1 id;
    CabbirdStringViewV1 pattern;
    const CabbirdSignatureMemberV2* members;
    uint32_t member_count;
} CabbirdSignatureWitnessV2;

typedef struct CabbirdSignatureLayoutRequestV2 {
    uint32_t struct_size;
    CabbirdStringViewV1 module_name;
    CabbirdStringViewV1 section_name;
    const CabbirdSignatureFieldV2* fields;
    uint32_t field_count;
    const CabbirdSignatureWitnessV2* witnesses;
    uint32_t witness_count;
} CabbirdSignatureLayoutRequestV2;

typedef struct CabbirdSignatureServiceV2 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    CabbirdStatusV1 (CABBIRD_CALL *resolve)(
        void* user, CabbirdStringViewV1 module_name, CabbirdStringViewV1 section_name,
        CabbirdStringViewV1 pattern, uintptr_t* address);
    /* Lifecycle/Worker only. Caller owns the arrays, in declaration order.
       Counts must equal request counts. For a live plugin context, once the request prefix and output
       counts/pointers are valid, failure clears both arrays. Success publishes
       a complete, validated layout: one offset for every declared field and one address per declared
       witness, where a witness the module no longer contains is published as zero and contributes no
       field evidence. An ambiguous witness fails the call with CABBIRD_STATUS_V1_CONFLICT.
       No handle or host object escapes. */
    CabbirdStatusV1 (CABBIRD_CALL *resolve_layout)(
        void* user, const CabbirdSignatureLayoutRequestV2* request,
        uintptr_t* addresses, uint32_t address_count,
        uint32_t* offsets, uint32_t offset_count);
} CabbirdSignatureServiceV2;

typedef struct CabbirdSignatureServiceV3 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    CabbirdStatusV1 (CABBIRD_CALL *resolve)(
        void* user, CabbirdStringViewV1 module_name, CabbirdStringViewV1 section_name,
        CabbirdStringViewV1 pattern, uintptr_t* address);
    CabbirdStatusV1 (CABBIRD_CALL *resolve_layout)(
        void* user, const CabbirdSignatureLayoutRequestV2* request,
        uintptr_t* addresses, uint32_t address_count,
        uint32_t* offsets, uint32_t offset_count);
    /* Lifecycle/Worker only. Each result is independently OK, NOT_FOUND, or
       CONFLICT; addresses are zero unless the corresponding result is OK. */
    CabbirdStatusV1 (CABBIRD_CALL *resolve_batch)(
        void* user, CabbirdStringViewV1 module_name, CabbirdStringViewV1 section_name,
        const CabbirdStringViewV1* patterns, uint32_t count,
        uint32_t* status_codes, uintptr_t* addresses);
} CabbirdSignatureServiceV3;

typedef struct CabbirdHookServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    CabbirdStatusV1 (CABBIRD_CALL *create)(
        void* user,
        const CabbirdHookRequestV1* request,
        uintptr_t* original,
        CabbirdGenerationHandleV1* handle);
    CabbirdStatusV1 (CABBIRD_CALL *release)(
        void* user, CabbirdGenerationHandleV1 handle);
    CabbirdStatusV1 (CABBIRD_CALL *begin_callback)(
        void* user,
        CabbirdGenerationHandleV1 hook,
        CabbirdGenerationHandleV1* callback_lease);
    CabbirdStatusV1 (CABBIRD_CALL *end_callback)(
        void* user, CabbirdGenerationHandleV1 callback_lease);
} CabbirdHookServiceV1;

typedef struct CabbirdPatchServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    CabbirdStatusV1 (CABBIRD_CALL *apply)(
        void* user,
        uintptr_t address,
        CabbirdByteSpanV1 replacement,
        CabbirdStringViewV1 label,
        CabbirdGenerationHandleV1* handle);
    CabbirdStatusV1 (CABBIRD_CALL *release)(
        void* user, CabbirdGenerationHandleV1 handle);
} CabbirdPatchServiceV1;

#ifdef __cplusplus
}
#endif
