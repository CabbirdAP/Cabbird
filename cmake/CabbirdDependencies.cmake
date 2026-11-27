# Pinned dependencies; CABBIRD_DEPS_SOURCE_DIR optionally reuses local source trees.

include(FetchContent)

if(NOT CABBIRD_DEPS_SOURCE_DIR)
    get_filename_component(_cabbird_sibling "${CMAKE_SOURCE_DIR}/.." ABSOLUTE)
    set(_cabbird_candidate "${_cabbird_sibling}/Anomaly/.build/windows-vs2022/_deps")
    if(EXISTS "${_cabbird_candidate}/nlohmann_json-src/include/nlohmann/json.hpp")
        set(CABBIRD_DEPS_SOURCE_DIR "${_cabbird_candidate}")
        message(STATUS
            "Cabbird: reusing locally cached dependency sources from "
            "pre-fetched dependencies at ${CABBIRD_DEPS_SOURCE_DIR}")
    endif()
endif()

if(CABBIRD_DEPS_SOURCE_DIR)
    if(EXISTS "${CABBIRD_DEPS_SOURCE_DIR}/nlohmann_json-src/include/nlohmann/json.hpp")
        set(FETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON
            "${CABBIRD_DEPS_SOURCE_DIR}/nlohmann_json-src" CACHE PATH "" FORCE)
    else()
        message(STATUS
            "Cabbird: ${CABBIRD_DEPS_SOURCE_DIR} has no nlohmann_json-src; it will be "
            "fetched from the network")
    endif()
    if(EXISTS "${CABBIRD_DEPS_SOURCE_DIR}/json_schema_validator-src/CMakeLists.txt")
        set(FETCHCONTENT_SOURCE_DIR_JSON_SCHEMA_VALIDATOR
            "${CABBIRD_DEPS_SOURCE_DIR}/json_schema_validator-src" CACHE PATH "" FORCE)
    endif()
    if(EXISTS "${CABBIRD_DEPS_SOURCE_DIR}/miniz-src/miniz.c")
        set(FETCHCONTENT_SOURCE_DIR_MINIZ
            "${CABBIRD_DEPS_SOURCE_DIR}/miniz-src" CACHE PATH "" FORCE)
    endif()
    if(EXISTS "${CABBIRD_DEPS_SOURCE_DIR}/imgui-src/imgui.cpp")
        set(FETCHCONTENT_SOURCE_DIR_IMGUI
            "${CABBIRD_DEPS_SOURCE_DIR}/imgui-src" CACHE PATH "" FORCE)
    else()
        message(STATUS
            "Cabbird: ${CABBIRD_DEPS_SOURCE_DIR} has no imgui-src; imgui will be fetched "
            "from the network")
    endif()
    if(EXISTS "${CABBIRD_DEPS_SOURCE_DIR}/minhook-src/src/hook.c")
        set(FETCHCONTENT_SOURCE_DIR_MINHOOK
            "${CABBIRD_DEPS_SOURCE_DIR}/minhook-src" CACHE PATH "" FORCE)
    else()
        message(STATUS
            "Cabbird: ${CABBIRD_DEPS_SOURCE_DIR} has no minhook-src; MinHook will be "
            "fetched from the network")
    endif()
endif()

FetchContent_Declare(
    imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(
    minhook
    URL https://github.com/TsudaKageyu/minhook/archive/refs/tags/v1.3.4.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(minhook)

set(JSON_BuildTests OFF)
set(JSON_Install OFF)
set(JSON_MultipleHeaders OFF)
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)
unset(JSON_BuildTests)
unset(JSON_Install)
unset(JSON_MultipleHeaders)

set(JSON_VALIDATOR_INSTALL OFF)
set(JSON_VALIDATOR_BUILD_TESTS OFF)
set(JSON_VALIDATOR_BUILD_EXAMPLES OFF)
set(JSON_VALIDATOR_SHARED_LIBS OFF)
FetchContent_Declare(
    json_schema_validator
    URL https://github.com/pboettch/json-schema-validator/archive/refs/tags/2.3.0.tar.gz
    URL_HASH SHA256=2c00b50023c7d557cdaa71c0777f5bcff996c4efd7a539e58beaa4219fa2a5e1
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(json_schema_validator)
unset(JSON_VALIDATOR_INSTALL)
unset(JSON_VALIDATOR_BUILD_TESTS)
unset(JSON_VALIDATOR_BUILD_EXAMPLES)
unset(JSON_VALIDATOR_SHARED_LIBS)

FetchContent_Declare(
    miniz
    URL https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip
    URL_HASH SHA256=ada38db0b703a56d3dd6d57bf84a9c5d664921d870d8fea4db153979fb5332c5
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(miniz)

add_library(cabbird_miniz STATIC "${miniz_SOURCE_DIR}/miniz.c")
target_include_directories(cabbird_miniz PUBLIC "${miniz_SOURCE_DIR}")
target_compile_options(cabbird_miniz PRIVATE /w)
