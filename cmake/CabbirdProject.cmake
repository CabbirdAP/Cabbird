# Project identity, supported configurations and shared compiler policy.


set(CABBIRD_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(CABBIRD_GENERATED_SDK_INCLUDE_DIR "${CABBIRD_GENERATED_DIR}/sdk-build/include")
set(CABBIRD_GENERATED_SDK_INSTALL_INCLUDE_DIR "${CABBIRD_GENERATED_DIR}/sdk-install/include")
file(MAKE_DIRECTORY "${CABBIRD_GENERATED_SDK_INCLUDE_DIR}/cabbird/sdk")
file(MAKE_DIRECTORY "${CABBIRD_GENERATED_SDK_INSTALL_INCLUDE_DIR}/cabbird/sdk")
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/version.h.in"
    "${CABBIRD_GENERATED_SDK_INCLUDE_DIR}/cabbird/sdk/configured_version.h"
    @ONLY)
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/version.h.in"
    "${CABBIRD_GENERATED_SDK_INSTALL_INCLUDE_DIR}/cabbird/sdk/version.h"
    @ONLY)
include_directories(BEFORE "${CABBIRD_GENERATED_SDK_INCLUDE_DIR}")
add_compile_definitions(CABBIRD_USE_CONFIGURED_VERSION=1)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT OR
   CMAKE_INSTALL_PREFIX MATCHES "^[A-Za-z]:/Program Files")
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/game-package"
        CACHE PATH "Install prefix (defaults to this build tree's game package)" FORCE)
endif()

if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "Debug;RelWithDebInfo"
        CACHE STRING "Configurations this project builds" FORCE)
    message(STATUS "Cabbird configurations: ${CMAKE_CONFIGURATION_TYPES}")
else()
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE "RelWithDebInfo"
            CACHE STRING "Build type" FORCE)
    endif()
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        message(FATAL_ERROR
            "CABBIRD_BUILD_CONFIGURATION: `Release` is not a configuration this project builds. "
            "Use RelWithDebInfo (the canonical one -- see build.cmd) or "
            "Debug.  No entry point in this tree produces a `Release` build and nothing ever "
            "installs one.")
    endif()
    message(STATUS "Cabbird build type: ${CMAKE_BUILD_TYPE}")
endif()

message(STATUS "Cabbird version: ${CABBIRD_VERSION_STRING} (generated into the SDK headers)")

option(CABBIRD_ENABLE_ASAN "Instrument Cabbird targets with MSVC AddressSanitizer" OFF)
if(MSVC)
    add_compile_options(
        $<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>
        $<$<COMPILE_LANGUAGE:C,CXX>:/FS>)
endif()
# The manual-map exception bridge requires the dynamic MSVC runtime (/MD).
# Keep CMake's default runtime for Cabbird and its dependencies.
if(CABBIRD_ENABLE_ASAN)
    if(NOT MSVC)
        message(FATAL_ERROR "CABBIRD_ENABLE_ASAN currently requires MSVC")
    endif()
    add_compile_definitions(CABBIRD_ENABLE_ASAN=1)
    add_compile_options(
        $<$<COMPILE_LANGUAGE:C>:/fsanitize=address>
        $<$<COMPILE_LANGUAGE:CXX>:/fsanitize=address>)
    add_link_options(/INCREMENTAL:NO)
endif()

option(CABBIRD_BUILD_BUILTIN_PLUGINS "Build Cabbird bundled plugins" ON)

if(NOT MSVC)
    message(FATAL_ERROR "Cabbird requires the Windows x64 MSVC toolchain and MASM.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The game is x64; configure with -A x64.")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_EXTENSIONS OFF)
option(CABBIRD_BUILD_TEST_FIXTURES "Build offline UI preview and developer injector" OFF)
enable_testing()
