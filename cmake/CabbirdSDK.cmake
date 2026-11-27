# Public header-only C ABI SDK and its standalone CMake package.

add_library(cabbird_sdk INTERFACE)
add_library(Cabbird::sdk ALIAS cabbird_sdk)
set_target_properties(cabbird_sdk PROPERTIES EXPORT_NAME sdk)
target_include_directories(cabbird_sdk INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>")
target_compile_definitions(cabbird_sdk INTERFACE ${CABBIRD_DEFS})
target_compile_features(cabbird_sdk INTERFACE cxx_std_20)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/sdk/CabbirdPlugin.cmake")

include(CMakePackageConfigHelpers)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/sdk/CabbirdSDKConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/CabbirdSDKConfig.cmake"
    INSTALL_DESTINATION "lib/cmake/CabbirdSDK")
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/CabbirdSDKConfigVersion.cmake"
    VERSION "${CABBIRD_VERSION_STRING}"
    COMPATIBILITY SameMajorVersion)

install(TARGETS cabbird_sdk EXPORT CabbirdSDKTargets COMPONENT SDK)
install(EXPORT CabbirdSDKTargets
    NAMESPACE Cabbird::
    DESTINATION "lib/cmake/CabbirdSDK" COMPONENT SDK)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/CabbirdSDKConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/CabbirdSDKConfigVersion.cmake"
    DESTINATION "lib/cmake/CabbirdSDK" COMPONENT SDK)
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/sdk/CabbirdPlugin.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/sdk/SyncPluginPackage.cmake"
    DESTINATION "lib/cmake/CabbirdSDK" COMPONENT SDK)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/cabbird/sdk/"
    DESTINATION "include/cabbird/sdk"
    COMPONENT SDK
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
    PATTERN "version.h" EXCLUDE)
install(FILES "${CABBIRD_GENERATED_SDK_INSTALL_INCLUDE_DIR}/cabbird/sdk/version.h"
    DESTINATION "include/cabbird/sdk"
    COMPONENT SDK)

# Ship the public contracts and standalone consumers with the headers.
install(FILES
    schemas/plugin-manifest.schema.json
    schemas/plugin-profile.schema.json
    schemas/plugin-enablement.schema.json
    schemas/repository-config.schema.json
    schemas/repository-index.schema.json
    DESTINATION "share/cabbird/schemas" COMPONENT SDK)
install(FILES abi/cabbird-sdk-v1-windows-x64.json
    DESTINATION "share/cabbird/abi" COMPONENT SDK)
install(FILES examples/CMakeLists.txt examples/README.md
    DESTINATION "share/cabbird/examples" COMPONENT SDK)
foreach(cabbird_sdk_example IN ITEMS hello_ui tick_counter reliable_config unity_entity_inspector)
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/examples/${cabbird_sdk_example}/"
        DESTINATION "share/cabbird/examples/${cabbird_sdk_example}" COMPONENT SDK
        FILES_MATCHING PATTERN "*.c" PATTERN "*.cpp" PATTERN "*.h" PATTERN "*.hpp"
        PATTERN "*.json" PATTERN "*.md"
        PATTERN "build" EXCLUDE PATTERN ".build" EXCLUDE PATTERN "CMakeFiles" EXCLUDE)
endforeach()
