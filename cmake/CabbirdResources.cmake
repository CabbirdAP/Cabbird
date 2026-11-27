# Generate embedded catalogs, schemas, resources and source provenance.

set(CABBIRD_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${CABBIRD_GENERATED_DIR}/cabbird")

set(CABBIRD_HOST_EN_US_CATALOG_FILE "${CMAKE_SOURCE_DIR}/locales/host/en-US.json")
if(NOT EXISTS "${CABBIRD_HOST_EN_US_CATALOG_FILE}")
    message(FATAL_ERROR
        "Missing i18n catalogue data: ${CABBIRD_HOST_EN_US_CATALOG_FILE}\n"
        "It is real source material (copied verbatim from Anomaly), not a build product, so a "
        "missing copy means the i18n port is incomplete -- not that it should be skipped.")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CABBIRD_HOST_EN_US_CATALOG_FILE}")

file(READ "${CABBIRD_HOST_EN_US_CATALOG_FILE}" CABBIRD_HOST_EN_US_CATALOG_JSON)
set(CABBIRD_HOST_EN_US_CATALOG_LITERALS "")
string(LENGTH "${CABBIRD_HOST_EN_US_CATALOG_JSON}" CABBIRD_HOST_EN_US_CATALOG_LENGTH)
set(CABBIRD_HOST_EN_US_CATALOG_OFFSET 0)
while(CABBIRD_HOST_EN_US_CATALOG_OFFSET LESS CABBIRD_HOST_EN_US_CATALOG_LENGTH)
    math(EXPR CABBIRD_HOST_EN_US_CATALOG_REMAINING
        "${CABBIRD_HOST_EN_US_CATALOG_LENGTH} - ${CABBIRD_HOST_EN_US_CATALOG_OFFSET}")
    if(CABBIRD_HOST_EN_US_CATALOG_REMAINING GREATER 8000)
        set(CABBIRD_HOST_EN_US_CATALOG_CHUNK_LENGTH 8000)
    else()
        set(CABBIRD_HOST_EN_US_CATALOG_CHUNK_LENGTH ${CABBIRD_HOST_EN_US_CATALOG_REMAINING})
    endif()
    string(SUBSTRING "${CABBIRD_HOST_EN_US_CATALOG_JSON}"
        ${CABBIRD_HOST_EN_US_CATALOG_OFFSET}
        ${CABBIRD_HOST_EN_US_CATALOG_CHUNK_LENGTH}
        CABBIRD_HOST_EN_US_CATALOG_CHUNK)
    string(APPEND CABBIRD_HOST_EN_US_CATALOG_LITERALS
        "    R\"cabbird_catalog(${CABBIRD_HOST_EN_US_CATALOG_CHUNK})cabbird_catalog\"\n")
    math(EXPR CABBIRD_HOST_EN_US_CATALOG_OFFSET
        "${CABBIRD_HOST_EN_US_CATALOG_OFFSET} + ${CABBIRD_HOST_EN_US_CATALOG_CHUNK_LENGTH}")
endwhile()

configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/i18n_embedded_catalog.hpp.in"
    "${CABBIRD_GENERATED_DIR}/cabbird/i18n_embedded_catalog.hpp"
    @ONLY)

set(CABBIRD_SCHEMA_DIR "${CMAKE_SOURCE_DIR}/schemas")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CABBIRD_SCHEMA_DIR}/plugin-manifest.schema.json"
    "${CABBIRD_SCHEMA_DIR}/plugin-profile.schema.json"
    "${CABBIRD_SCHEMA_DIR}/repository-cache.schema.json"
    "${CABBIRD_SCHEMA_DIR}/repository-config.schema.json"
    "${CABBIRD_SCHEMA_DIR}/repository-index.schema.json")

file(READ "${CABBIRD_SCHEMA_DIR}/plugin-manifest.schema.json" CABBIRD_PLUGIN_MANIFEST_SCHEMA_JSON)
file(READ "${CABBIRD_SCHEMA_DIR}/plugin-profile.schema.json" CABBIRD_PLUGIN_PROFILE_SCHEMA_JSON)
file(READ "${CABBIRD_SCHEMA_DIR}/repository-cache.schema.json" CABBIRD_REPOSITORY_CACHE_SCHEMA_JSON)
file(READ "${CABBIRD_SCHEMA_DIR}/repository-config.schema.json" CABBIRD_REPOSITORY_CONFIG_SCHEMA_JSON)
file(READ "${CABBIRD_SCHEMA_DIR}/repository-index.schema.json" CABBIRD_REPOSITORY_INDEX_SCHEMA_JSON)

foreach(_schema_var
        CABBIRD_PLUGIN_MANIFEST_SCHEMA_JSON
        CABBIRD_PLUGIN_PROFILE_SCHEMA_JSON
        CABBIRD_REPOSITORY_CACHE_SCHEMA_JSON
        CABBIRD_REPOSITORY_CONFIG_SCHEMA_JSON
        CABBIRD_REPOSITORY_INDEX_SCHEMA_JSON)
    if("${${_schema_var}}" STREQUAL "")
        message(FATAL_ERROR
            "Schema variable ${_schema_var} is empty. Its schemas/*.json input is missing or "
            "unreadable; those schemas are real source material copied from Anomaly.")
    endif()
endforeach()

configure_file("${CMAKE_SOURCE_DIR}/cmake/plugin_manifest_schema.hpp.in"
    "${CABBIRD_GENERATED_DIR}/plugin_manifest_schema.hpp" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/cmake/plugin_profile_schema.hpp.in"
    "${CABBIRD_GENERATED_DIR}/plugin_profile_schema.hpp" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/cmake/repository_cache_schema.hpp.in"
    "${CABBIRD_GENERATED_DIR}/repository_cache_schema.hpp" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/cmake/repository_config_schema.hpp.in"
    "${CABBIRD_GENERATED_DIR}/repository_config_schema.hpp" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/cmake/repository_index_schema.hpp.in"
    "${CABBIRD_GENERATED_DIR}/repository_index_schema.hpp" @ONLY)

file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/logo-header.png" CABBIRD_PLATFORM_LOGO_PATH)
file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/logo.png" CABBIRD_LAUNCHER_LOGO_PATH)
file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/logo.png" CABBIRD_ESC_MENU_LOGO_PATH)
file(TO_CMAKE_PATH
    "${CMAKE_SOURCE_DIR}/apps/launcher/CabbirdLauncher.ico"
    CABBIRD_LAUNCHER_ICON_PATH)
set(CABBIRD_LAUNCHER_RESOURCES_RC "${CABBIRD_GENERATED_DIR}/CabbirdLauncher.rc")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/logo-header.png"
    "${CMAKE_SOURCE_DIR}/logo.png"
    "${CMAKE_SOURCE_DIR}/apps/launcher/CabbirdLauncher.ico")
configure_file(
    "${CMAKE_SOURCE_DIR}/apps/launcher/CabbirdLauncher.rc.in"
    "${CABBIRD_LAUNCHER_RESOURCES_RC}" @ONLY)

set(CABBIRD_PLATFORM_UI_RESOURCES_RC "${CABBIRD_GENERATED_DIR}/platform_ui_resources.rc")
configure_file("${CMAKE_SOURCE_DIR}/cmake/platform_ui_resources.rc.in"
    "${CABBIRD_PLATFORM_UI_RESOURCES_RC}" @ONLY)

set(CABBIRD_SOURCE_COMMIT "")
set(CABBIRD_SOURCE_DIRTY true)
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE CABBIRD_GIT_REVISION_RESULT
        OUTPUT_VARIABLE CABBIRD_SOURCE_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE CABBIRD_GIT_STATUS_RESULT
        OUTPUT_VARIABLE CABBIRD_GIT_STATUS
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(NOT CABBIRD_GIT_REVISION_RESULT EQUAL 0)
        set(CABBIRD_SOURCE_COMMIT "")
    endif()
    if(CABBIRD_GIT_REVISION_RESULT EQUAL 0 AND
       CABBIRD_GIT_STATUS_RESULT EQUAL 0 AND
       CABBIRD_GIT_STATUS STREQUAL "")
        set(CABBIRD_SOURCE_DIRTY false)
    endif()
endif()
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/build_provenance.hpp.in"
    "${CABBIRD_GENERATED_DIR}/build_provenance.hpp"
    @ONLY)

include_directories("${CABBIRD_GENERATED_DIR}")

# Data changes must be published even when no executable needs relinking.
function(cabbird_add_runtime_resources target directory)
    add_custom_target(${target}
        COMMAND "${CMAKE_COMMAND}"
            "-DCABBIRD_SOURCE_ROOT=${CMAKE_SOURCE_DIR}"
            "-DCABBIRD_RUNTIME_ROOT=${directory}"
            "-DCABBIRD_RESOURCE_INVENTORY=${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${target}-$<CONFIG>.resources"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/SyncRuntimeResources.cmake"
        VERBATIM)
endfunction()
