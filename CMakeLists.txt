cmake_minimum_required(VERSION 3.22)

set(CABBIRD_RELEASE_VERSION "1.0.0" CACHE STRING "Release version used to configure Cabbird")
include(cmake/cabbird_version.cmake)
cabbird_resolve_version("${CABBIRD_RELEASE_VERSION}" CABBIRD_PROJECT_VERSION CABBIRD_VERSION_STRING)
project(Cabbird VERSION "${CABBIRD_PROJECT_VERSION}" LANGUAGES C CXX ASM_MASM)

# Keep orchestration here; each module owns one build-system boundary.
include(cmake/CabbirdProject.cmake)
include(cmake/CabbirdResources.cmake)
include(cmake/CabbirdDependencies.cmake)
include(cmake/CabbirdShaders.cmake)
include(cmake/CabbirdTargets.cmake)
include(cmake/CabbirdApplications.cmake)
include(cmake/CabbirdSDK.cmake)
include(cmake/CabbirdBuiltinPlugins.cmake)
include(cmake/CabbirdCoreImage.cmake)
include(cmake/CabbirdPackaging.cmake)
