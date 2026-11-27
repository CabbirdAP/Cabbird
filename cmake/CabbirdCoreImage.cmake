# Production composition root, manual-map image policy and runtime resource staging.

add_library(cabbird_core_main_probe OBJECT EXCLUDE_FROM_ALL src/app/core_main.cpp)
target_link_libraries(cabbird_core_main_probe PRIVATE cabbird_core)

add_library(cabbird_core_image SHARED
    src/app/core_main.cpp
    src/app/unity_runtime.cpp
    src/runtime/manual_map_exception.cpp
    ${CABBIRD_IMGUI_WIN32_SOURCES}
    "${CABBIRD_PLATFORM_UI_RESOURCES_RC}"
)
target_include_directories(cabbird_core_image PRIVATE include src/core)
target_compile_definitions(cabbird_core_image PRIVATE ${CABBIRD_DEFS}
    IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
target_compile_options(cabbird_core_image PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:/W3 /EHsc>)
set_target_properties(cabbird_core_image PROPERTIES
    OUTPUT_NAME "Cabbird.Core"
    PREFIX ""
    SUFFIX ".dll"
)
target_link_libraries(cabbird_core_image PRIVATE
    cabbird_runtime
    cabbird_core
    cabbird_mem
    cabbird_ported
    cabbird_plugin
    cabbird_ui
    cabbird_imgui
)
target_link_options(cabbird_core_image PRIVATE "/EXPORT:CabbirdManualMapCxxThrow")
add_dependencies(cabbird_core_image cabbird_runtime_resources)

# Cabbird.Core is manually mapped: loader-managed TLS and static-init guards
# cannot be used. Thread-local state uses the FLS-backed ThreadLocalValue.
set(CABBIRD_MANUAL_MAP_TLS_FREE_TARGETS
    cabbird_core_image
    cabbird_runtime
    cabbird_core
    cabbird_ported
    cabbird_plugin
    cabbird_ui
    cabbird_imgui
    cabbird_mem
    nlohmann_json_schema_validator
)
foreach(target IN LISTS CABBIRD_MANUAL_MAP_TLS_FREE_TARGETS)
    if(TARGET ${target})
        target_compile_options(${target} PRIVATE
            $<$<COMPILE_LANGUAGE:C,CXX>:/Zc:threadSafeInit->)
    endif()
endforeach()
