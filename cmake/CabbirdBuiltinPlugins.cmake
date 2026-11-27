# Built-in package declarations. cabbird_add_plugin owns staging and installation.

if(CABBIRD_BUILD_BUILTIN_PLUGINS)
cabbird_add_plugin(EntityOverlay
    SOURCES plugins/EntityOverlay/plugin.cpp
    MANIFEST plugins/EntityOverlay/manifest.json
    PACKAGE_NAME EntityOverlay)
target_link_libraries(EntityOverlay PRIVATE nlohmann_json::nlohmann_json)

if(WIN32)
    target_link_libraries(EntityOverlay PRIVATE bcrypt)
endif()

cabbird_add_plugin(UnityDump
    SOURCES plugins/UnityDump/plugin.cpp
    MANIFEST plugins/UnityDump/manifest.json
    PACKAGE_NAME UnityDump)
target_link_libraries(UnityDump PRIVATE nlohmann_json::nlohmann_json)

cabbird_add_plugin(PlayerCoords
    SOURCES plugins/PlayerCoords/plugin.cpp
    MANIFEST plugins/PlayerCoords/manifest.json
    PACKAGE_NAME PlayerCoords)
target_link_libraries(PlayerCoords PRIVATE nlohmann_json::nlohmann_json)

cabbird_add_plugin(FakeUID
    SOURCES plugins/FakeUID/plugin.cpp plugins/FakeUID/uid_runtime.cpp
    MANIFEST plugins/FakeUID/manifest.json
    PACKAGE_NAME FakeUID)
target_link_libraries(FakeUID PRIVATE nlohmann_json::nlohmann_json)
target_compile_options(FakeUID PRIVATE /utf-8)

cabbird_add_plugin(PlayerTeleport
    SOURCES plugins/PlayerTeleport/plugin.cpp
    MANIFEST plugins/PlayerTeleport/manifest.json
    PACKAGE_NAME PlayerTeleport)
target_link_libraries(PlayerTeleport PRIVATE nlohmann_json::nlohmann_json)

cabbird_add_plugin(DamageReplay
    SOURCES plugins/DamageReplay/plugin.cpp
    MANIFEST plugins/DamageReplay/manifest.json
    PACKAGE_NAME DamageReplay)

cabbird_add_plugin(MoveSpeed
    SOURCES plugins/MoveSpeed/plugin.cpp
    MANIFEST plugins/MoveSpeed/manifest.json
    PACKAGE_NAME MoveSpeed)

cabbird_add_plugin(StoryTools
    SOURCES plugins/StoryTools/plugin.cpp
    MANIFEST plugins/StoryTools/manifest.json
    PACKAGE_NAME StoryTools)

cabbird_add_plugin(EntityTeleport
    SOURCES plugins/EntityTeleport/plugin.cpp
    MANIFEST plugins/EntityTeleport/manifest.json
    PACKAGE_NAME EntityTeleport)
target_link_libraries(EntityTeleport PRIVATE nlohmann_json::nlohmann_json)

cabbird_add_plugin(FreeFlight
    SOURCES plugins/FreeFlight/plugin.cpp
    MANIFEST plugins/FreeFlight/manifest.json
    PACKAGE_NAME FreeFlight)
target_link_libraries(FreeFlight PRIVATE nlohmann_json::nlohmann_json)

cabbird_add_plugin(GodMode
    SOURCES plugins/GodMode/plugin.cpp
    MANIFEST plugins/GodMode/manifest.json
    PACKAGE_NAME GodMode)

cabbird_add_plugin(Collectibles
    SOURCES plugins/Collectibles/plugin.cpp
    MANIFEST plugins/Collectibles/manifest.json
    PACKAGE_NAME Collectibles)
target_link_libraries(Collectibles PRIVATE nlohmann_json::nlohmann_json)

cabbird_add_plugin(BetterPose
    SOURCES plugins/BetterPose/plugin.cpp
    MANIFEST plugins/BetterPose/manifest.json
    PACKAGE_NAME BetterPose)
target_link_libraries(BetterPose PRIVATE nlohmann_json::nlohmann_json)
target_compile_options(BetterPose PRIVATE /utf-8)

cabbird_add_plugin(CameraTools
    SOURCES plugins/CameraTools/plugin.cpp
    MANIFEST plugins/CameraTools/manifest.json
    PACKAGE_NAME CameraTools)
target_link_libraries(CameraTools PRIVATE nlohmann_json::nlohmann_json)
target_compile_options(CameraTools PRIVATE /utf-8)

endif()

# Each preview build refreshes the same package inventory as the release.
get_property(CABBIRD_BUILTIN_PLUGIN_TARGETS GLOBAL PROPERTY CABBIRD_RELEASE_PLUGIN_TARGETS)
if(TARGET cabbird_platform_preview)
    foreach(cabbird_plugin_target IN LISTS CABBIRD_BUILTIN_PLUGIN_TARGETS)
        get_target_property(cabbird_package_name ${cabbird_plugin_target} CABBIRD_PACKAGE_NAME)
        get_target_property(cabbird_manifest ${cabbird_plugin_target} CABBIRD_PACKAGE_MANIFEST)
        get_filename_component(cabbird_package_source "${cabbird_manifest}" DIRECTORY)
        set(cabbird_preview_package "$<TARGET_FILE_DIR:cabbird_platform_preview>/Cabbird/plugins/${cabbird_package_name}")
        add_custom_target(${cabbird_plugin_target}_preview_package
            COMMAND "${CMAKE_COMMAND}"
                "-DCABBIRD_PACKAGE_SOURCE=${cabbird_package_source}"
                "-DCABBIRD_PACKAGE_MANIFEST=${cabbird_manifest}"
                "-DCABBIRD_PACKAGE_DIRECTORY=${cabbird_preview_package}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/sdk/SyncPluginPackage.cmake"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:${cabbird_plugin_target}>" "${cabbird_preview_package}/plugin.dll"
            DEPENDS ${cabbird_plugin_target}
            VERBATIM)
        add_dependencies(cabbird_platform_preview ${cabbird_plugin_target}_preview_package)
    endforeach()
endif()
