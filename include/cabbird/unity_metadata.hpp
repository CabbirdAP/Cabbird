#pragma once

#include "cabbird/sdk/services/unity_metadata.h"

namespace cabbird {

// Internal implementation; the adapter validates generation and Game admission.
CabbirdStatusV1 ResolveUnityMetadataMethod(
    const CabbirdUnityMethodRequestV1* request, CabbirdUnityMethodResultV1* result);
CabbirdStatusV1 ResolveUnityMetadataInstanceField(
    const CabbirdUnityFieldRequestV1* request, CabbirdUnityFieldResultV1* result);

}  // namespace cabbird
