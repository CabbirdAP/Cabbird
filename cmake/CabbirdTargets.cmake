# Host libraries. The runtime kernel is independent of the renderer and Unity adapter.

set(CABBIRD_DEFS UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

set(CABBIRD_IMGUI_CORE_SOURCES
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
)
set(CABBIRD_IMGUI_DX11_SOURCES
    "${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp"
)

set(CABBIRD_IMGUI_WIN32_SOURCES
    "${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
)

add_library(cabbird_imgui STATIC ${CABBIRD_IMGUI_CORE_SOURCES})
target_include_directories(cabbird_imgui PUBLIC "${imgui_SOURCE_DIR}")
target_compile_definitions(cabbird_imgui PUBLIC ${CABBIRD_DEFS})
target_compile_options(cabbird_imgui PRIVATE /W3 /EHsc)

add_library(cabbird_ui STATIC
    ${CABBIRD_GLASS_SHADER_HEADERS}
    src/render/dx11/liquid_glass.cpp
    src/render/dx11/shaders/glass_pass.hlsl
    src/render/dx11/shaders/glass_surface.hlsl
    src/ui/cabbird_ui_theme.cpp
)
target_include_directories(cabbird_ui PUBLIC include PRIVATE src/ui)
target_compile_definitions(cabbird_ui PUBLIC ${CABBIRD_DEFS})
set_target_properties(cabbird_ui PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
target_compile_options(cabbird_ui PRIVATE /W3 /EHsc)
target_link_libraries(cabbird_ui PUBLIC cabbird_imgui d3d11 dxgi)
set_source_files_properties(
    src/render/dx11/shaders/glass_pass.hlsl src/render/dx11/shaders/glass_surface.hlsl
    PROPERTIES HEADER_FILE_ONLY TRUE)

add_library(cabbird_runtime STATIC
    src/runtime/dispatcher.cpp
    src/runtime/runtime_dispatchers.cpp
    src/runtime/service_graph.cpp
    src/runtime/runtime_session.cpp
    src/runtime/service_graph_diagnostics.cpp
    src/diagnostics/json.cpp
)
target_include_directories(cabbird_runtime PUBLIC include)
target_compile_definitions(cabbird_runtime PUBLIC ${CABBIRD_DEFS})
set_target_properties(cabbird_runtime PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
target_compile_options(cabbird_runtime PRIVATE /W3 /EHsc)

set(CABBIRD_CORE_SOURCES
    src/core/log.cpp
    src/core/options.cpp
    src/core/status.cpp
    src/render/dx11/overlay.cpp
    src/render/dx11/embedded_discovery.cpp
    src/render/dx11/embedded_bridge.cpp
    src/render/dx11/embedded_renderer.cpp
    src/render/dx11/embedded_input.cpp
    src/render/dx11/embedded_ui_resource_render_backend.cpp
    src/ui/embedded_ui.cpp

    ${CABBIRD_IMGUI_DX11_SOURCES}

    src/hook/hook_manager.cpp
    src/hook/minhook_backend.cpp
)

add_library(cabbird_core STATIC ${CABBIRD_CORE_SOURCES})

target_include_directories(cabbird_core PUBLIC
    include
    src/core
    src/render/dx11
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends"
)

target_compile_definitions(cabbird_core PUBLIC ${CABBIRD_DEFS} CABBIRD_IMGUI_NO_WIN32)
target_compile_options(cabbird_core PRIVATE
    $<$<COMPILE_LANGUAGE:C,CXX>:/W3 /EHsc>
)
target_link_libraries(cabbird_core PUBLIC d3d11 dxgi dxguid cabbird_imgui cabbird_ui minhook)

add_library(cabbird_plugin STATIC
    src/plugin/plugin_scope.cpp
    src/plugin/plugin_host.cpp
)
target_include_directories(cabbird_plugin PUBLIC include PRIVATE "${imgui_SOURCE_DIR}")
target_compile_definitions(cabbird_plugin PUBLIC ${CABBIRD_DEFS})
set_target_properties(cabbird_plugin PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
target_compile_options(cabbird_plugin PRIVATE /W3 /EHsc)

add_library(cabbird_manual_map STATIC
    src/launcher/manual_map.cpp
    src/launcher/manual_map_remote.asm
)
target_include_directories(cabbird_manual_map PUBLIC include)
target_compile_definitions(cabbird_manual_map PUBLIC ${CABBIRD_DEFS})
set_target_properties(cabbird_manual_map PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
target_compile_options(cabbird_manual_map PRIVATE
    $<$<COMPILE_LANGUAGE:C,CXX>:/W3 /EHsc>
)
target_link_libraries(cabbird_manual_map PUBLIC psapi)

add_library(cabbird_mem STATIC
    src/mem/memory.cpp
    src/mem/pattern.cpp
    src/mem/il2cpp.cpp
    src/mem/il2cpp_dump.cpp
)
target_include_directories(cabbird_mem PUBLIC include)
target_compile_definitions(cabbird_mem PUBLIC ${CABBIRD_DEFS})
set_target_properties(cabbird_mem PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
target_compile_features(cabbird_mem PUBLIC cxx_std_20)
target_compile_options(cabbird_mem PRIVATE
    $<$<COMPILE_LANGUAGE:C,CXX>:/W3 /EHsc>
)
target_link_libraries(cabbird_mem PUBLIC psapi)

set(CABBIRD_PORTED_SOURCES
      src/game/azur/azur_unity_adapter.cpp
      src/game/unity/method_image_filter.cpp
      src/game/unity/dispatch_tick_hook.cpp
      src/game/unity/unity_build_profile.cpp
      src/game/unity/unity_metadata.cpp
    src/plugin/semver.cpp
    src/plugin/plugin_package.cpp
    src/repository/artifact_crypto.cpp
    src/repository/artifact_bundle.cpp
    src/repository/repository_network.cpp
    src/repository/runtime_launch.cpp
    src/repository/plugin_list.cpp
    src/repository/plugin_repository_config.cpp
    src/repository/repository_configuration.cpp
    src/repository/repository_index.cpp
    src/repository/repository_policy.cpp
    src/repository/update_transaction.cpp
    src/repository/repository_coordinator.cpp
    src/services/reliable_storage.cpp
    src/services/signature_updater.cpp
    src/plugin/plugin_config_watcher.cpp
    src/plugin/plugin_native_dependency.cpp
    src/plugin/plugin_runtime.cpp
    src/diagnostics/crash_reporter.cpp
    src/services/structured_logger.cpp
    src/services/log_retention.cpp
    src/i18n/i18n.cpp
    src/plugin/plugin_manifest.cpp
    src/plugin/plugin_profile.cpp
    src/plugin/plugin_enablement.cpp
    src/plugin/plugin_dependency_resolver.cpp
    src/plugin/plugin_capability_policy.cpp
    src/plugin/plugin_file_watcher.cpp
    src/plugin/plugin_catalog.cpp
    src/plugin/plugin_compatibility.cpp
    src/plugin/plugin_manager.cpp
    src/plugin/plugin_shadow_store.cpp
    src/plugin/scoped_platform_services.cpp
    src/plugin/ipc_registry.cpp
    src/game/unity/adapter_service_registry.cpp
    src/game/azur/azur_profile_runtime.cpp
    src/game/azur/azur_player_binding.cpp
    src/ui/input_service.cpp
    src/ui/ui_service_registry.cpp
    src/ui/platform_ui_model.cpp
    src/ui/platform_ui_operations.cpp
    src/platform/windows/module_memory_service.cpp
    src/platform/windows/pattern_service.cpp
    src/platform/windows/config.cpp
    src/platform/windows/websocket_server.cpp
    src/platform/windows/web_server.cpp
    src/ui/platform_settings.cpp
    src/ui/platform_ui_layout.cpp
    src/ui/ui_resource_decoder.cpp
    src/ui/ui_resource_registry.cpp
    src/ui/host_ui_service.cpp
    src/ui/platform_host.cpp
    src/repository/safe_zip.cpp
    src/diagnostics/pipe_server.cpp
    src/diagnostics/diagnostic_pipe_service.cpp
    src/diagnostics/runtime_recovery.cpp
    src/diagnostics/runtime_crash_coordinator.cpp
    src/diagnostics/analyzer.cpp
    src/launcher/configuration.cpp
)
add_library(cabbird_ported STATIC ${CABBIRD_PORTED_SOURCES})
target_include_directories(cabbird_ported PUBLIC include
    PRIVATE "${minhook_SOURCE_DIR}/src/hde")
target_compile_definitions(cabbird_ported PUBLIC ${CABBIRD_DEFS})
target_compile_options(cabbird_ported PRIVATE /W3 /EHsc)
target_link_libraries(cabbird_ported PUBLIC
    bcrypt winhttp pathcch shlwapi dbghelp ws2_32 comdlg32
    cabbird_plugin
    cabbird_ui
    nlohmann_json::nlohmann_json
    nlohmann_json_schema_validator::validator
    cabbird_imgui
    cabbird_miniz
)
target_link_libraries(cabbird_ported PRIVATE cabbird_mem minhook)
