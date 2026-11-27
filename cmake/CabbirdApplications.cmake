# Desktop launcher, diagnostics CLI and optional offline development fixtures.

add_executable(cabbird_cli apps/cli/main.cpp)
set_target_properties(cabbird_cli PROPERTIES OUTPUT_NAME "cabbird-cli")
target_compile_definitions(cabbird_cli PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(cabbird_cli PRIVATE /W3 /EHsc)

add_executable(cabbird_plugin_tool
    apps/plugin_tool/main.cpp
    src/plugin/plugin_manifest.cpp
    src/plugin/plugin_capability_policy.cpp
    src/plugin/plugin_package.cpp
    src/plugin/semver.cpp
    src/repository/artifact_bundle.cpp
    src/repository/artifact_crypto.cpp)
set_target_properties(cabbird_plugin_tool PROPERTIES OUTPUT_NAME "cabbird-plugin")
target_include_directories(cabbird_plugin_tool PRIVATE include)
target_compile_definitions(cabbird_plugin_tool PRIVATE ${CABBIRD_DEFS})
target_compile_options(cabbird_plugin_tool PRIVATE /W4 /EHsc)
target_link_libraries(cabbird_plugin_tool PRIVATE bcrypt
    nlohmann_json::nlohmann_json nlohmann_json_schema_validator::validator)

if(CABBIRD_BUILD_TEST_FIXTURES)
add_executable(cabbird_platform_preview WIN32
      apps/render_fixture/platform_preview.cpp
      "${CABBIRD_PLATFORM_UI_RESOURCES_RC}"
      ${CABBIRD_IMGUI_DX11_SOURCES}
      ${CABBIRD_IMGUI_WIN32_SOURCES}
)
set_target_properties(cabbird_platform_preview PROPERTIES
      OUTPUT_NAME "cabbird-platform-preview"
      CXX_STANDARD 20
      CXX_STANDARD_REQUIRED ON
)
target_include_directories(cabbird_platform_preview PRIVATE
      include
      "${imgui_SOURCE_DIR}"
      "${imgui_SOURCE_DIR}/backends"
)
target_compile_definitions(cabbird_platform_preview PRIVATE ${CABBIRD_DEFS})
target_compile_options(cabbird_platform_preview PRIVATE /W3 /EHsc)
target_link_libraries(cabbird_platform_preview PRIVATE
      cabbird_ported
      cabbird_runtime
      cabbird_mem
      cabbird_core
      d3d11 dxgi dxguid dwmapi shell32
)
cabbird_add_runtime_resources(cabbird_preview_runtime_resources
    "$<TARGET_FILE_DIR:cabbird_platform_preview>/Cabbird")
add_dependencies(cabbird_platform_preview cabbird_preview_runtime_resources)

add_executable(cabbird_inject src/injector/main.cpp)
target_include_directories(cabbird_inject PRIVATE include)
target_compile_definitions(cabbird_inject PRIVATE ${CABBIRD_DEFS})
set_target_properties(cabbird_inject PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
target_compile_options(cabbird_inject PRIVATE /W3 /EHsc)
target_link_libraries(cabbird_inject PRIVATE cabbird_manual_map)

endif()

add_executable(cabbird_launcher WIN32
    apps/launcher/main.cpp
    apps/launcher/desktop_surface.hpp
    "${CABBIRD_LAUNCHER_RESOURCES_RC}"
    ${CABBIRD_IMGUI_DX11_SOURCES}
    ${CABBIRD_IMGUI_WIN32_SOURCES}
)
set_target_properties(cabbird_launcher PROPERTIES
    OUTPUT_NAME "CabbirdLauncher"
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
target_include_directories(cabbird_launcher PRIVATE
    include
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends"
)
target_compile_definitions(cabbird_launcher PRIVATE ${CABBIRD_DEFS})
target_compile_options(cabbird_launcher PRIVATE
    $<$<COMPILE_LANGUAGE:C,CXX>:/W3 /EHsc /utf-8>
)
target_link_libraries(cabbird_launcher PRIVATE
    cabbird_ported
    cabbird_runtime
    cabbird_mem
    cabbird_core
    cabbird_manual_map
    d3d11 dxgi dxguid dwmapi dcomp shell32 ole32 comdlg32
)

target_link_options(cabbird_launcher PRIVATE
    "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
cabbird_add_runtime_resources(cabbird_runtime_resources "$<TARGET_FILE_DIR:cabbird_launcher>")
add_dependencies(cabbird_launcher cabbird_runtime_resources)

# Preserve the original CLI product-name usage check.
add_test(NAME cabbird_cli_usage COMMAND cabbird_cli)
set_tests_properties(cabbird_cli_usage PROPERTIES
    PASS_REGULAR_EXPRESSION "cabbird-cli --pid" TIMEOUT 300)
