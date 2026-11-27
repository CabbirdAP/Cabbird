# Runtime, tools, SDK and symbols release components and redistribution notices.

install(TARGETS cabbird_launcher
    RUNTIME DESTINATION . COMPONENT GameRuntime)

install(TARGETS cabbird_core_image
    RUNTIME DESTINATION Cabbird COMPONENT GameRuntime
    LIBRARY DESTINATION Cabbird COMPONENT GameRuntime)

# Use the same managed data update as the build tree, retaining user configuration.
install(CODE "
    execute_process(COMMAND \"${CMAKE_COMMAND}\"
        \"-DCABBIRD_SOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}\"
        \"-DCABBIRD_RUNTIME_ROOT=\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/Cabbird\"
        \"-DCABBIRD_RESOURCE_INVENTORY=\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/Cabbird/.cabbird-resources\"
        -P \"${CMAKE_CURRENT_LIST_DIR}/SyncRuntimeResources.cmake\"
        COMMAND_ERROR_IS_FATAL ANY)
    " COMPONENT GameRuntime)

install(TARGETS cabbird_cli cabbird_plugin_tool RUNTIME DESTINATION . COMPONENT Tools)
install(FILES tools/theme_layout_editor/index.html
    DESTINATION tools/theme_layout_editor COMPONENT Tools)

set(CABBIRD_RELEASE_PE_TARGETS
    cabbird_core_image
    cabbird_launcher
    cabbird_cli
    cabbird_plugin_tool)
get_property(CABBIRD_RELEASE_PLUGIN_TARGETS GLOBAL PROPERTY CABBIRD_RELEASE_PLUGIN_TARGETS)
list(REMOVE_DUPLICATES CABBIRD_RELEASE_PLUGIN_TARGETS)
foreach(cabbird_runtime_plugin_target IN LISTS CABBIRD_RELEASE_PLUGIN_TARGETS)
    if(TARGET ${cabbird_runtime_plugin_target})
        list(APPEND CABBIRD_RELEASE_PE_TARGETS ${cabbird_runtime_plugin_target})
    endif()
endforeach()

if(MSVC)
    foreach(cabbird_release_target IN LISTS CABBIRD_RELEASE_PE_TARGETS)
        target_link_options(${cabbird_release_target} PRIVATE
            "/PDBALTPATH:$<TARGET_PDB_FILE_NAME:${cabbird_release_target}>")
    endforeach()
endif()

function(cabbird_install_symbols target pdb_name)
    if(TARGET ${target})
        install(FILES "$<TARGET_PDB_FILE:${target}>"
            DESTINATION symbols RENAME "${pdb_name}" COMPONENT Symbols)
    endif()
endfunction()

cabbird_install_symbols(cabbird_core_image "Cabbird.Core.pdb")
cabbird_install_symbols(cabbird_launcher "CabbirdLauncher.pdb")
cabbird_install_symbols(cabbird_cli "cabbird-cli.pdb")
cabbird_install_symbols(cabbird_plugin_tool "cabbird-plugin.pdb")
foreach(cabbird_plugin_target IN LISTS CABBIRD_RELEASE_PLUGIN_TARGETS)
    get_target_property(cabbird_package_name ${cabbird_plugin_target} CABBIRD_PACKAGE_NAME)
    cabbird_install_symbols(${cabbird_plugin_target} "${cabbird_package_name}.pdb")
endforeach()

set(CABBIRD_THIRD_PARTY_LICENSE_FILES
    third_party/licenses/imgui-LICENSE.txt
    third_party/licenses/minhook-LICENSE.txt
    third_party/licenses/nlohmann-json-LICENSE.txt
    third_party/licenses/json-schema-validator-LICENSE.txt
    third_party/licenses/miniz-LICENSE.txt
    third_party/licenses/noto-sans-cjk-LICENSE.txt)
foreach(cabbird_release_component IN ITEMS GameRuntime SDK Tools Symbols)
    install(FILES LICENSE NOTICE
        DESTINATION . COMPONENT ${cabbird_release_component})
    install(FILES ${CABBIRD_THIRD_PARTY_LICENSE_FILES}
        DESTINATION third_party/licenses COMPONENT ${cabbird_release_component})
endforeach()
