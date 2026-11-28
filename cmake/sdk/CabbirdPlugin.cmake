include(CMakeParseArguments)

# One declaration owns the DLL, package data, install layout and release inventory.
# Standard data directories live beside MANIFEST: locales/, assets/ and profile/.
function(cabbird_add_plugin target)
    set(options C_ONLY NO_RELEASE)
    set(oneValueArgs MANIFEST PACKAGE_NAME OUTPUT_DIRECTORY)
    set(multiValueArgs SOURCES)
    cmake_parse_arguments(CABBIRD "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    if(NOT CABBIRD_SOURCES OR NOT CABBIRD_MANIFEST)
        message(FATAL_ERROR "cabbird_add_plugin(${target}) requires SOURCES and MANIFEST")
    endif()
    if(NOT IS_ABSOLUTE "${CABBIRD_MANIFEST}")
        set(CABBIRD_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/${CABBIRD_MANIFEST}")
    endif()
    if(NOT EXISTS "${CABBIRD_MANIFEST}")
        message(FATAL_ERROR "plugin manifest does not exist: ${CABBIRD_MANIFEST}")
    endif()
    if(NOT CABBIRD_PACKAGE_NAME)
        set(CABBIRD_PACKAGE_NAME "${target}")
    endif()
    if(NOT CABBIRD_OUTPUT_DIRECTORY)
        set(CABBIRD_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/package/${CABBIRD_PACKAGE_NAME}")
    endif()

    add_library(${target} SHARED ${CABBIRD_SOURCES})
    target_link_libraries(${target} PRIVATE Cabbird::sdk)
    set_target_properties(${target} PROPERTIES
        PREFIX ""
        OUTPUT_NAME "plugin"
        PDB_NAME "${CABBIRD_PACKAGE_NAME}"
        RUNTIME_OUTPUT_DIRECTORY "$<1:${CABBIRD_OUTPUT_DIRECTORY}>"
        LIBRARY_OUTPUT_DIRECTORY "$<1:${CABBIRD_OUTPUT_DIRECTORY}>"
        ARCHIVE_OUTPUT_DIRECTORY "$<1:${CABBIRD_OUTPUT_DIRECTORY}>"
        PDB_OUTPUT_DIRECTORY "$<1:${CABBIRD_OUTPUT_DIRECTORY}>"
        CABBIRD_PACKAGE_NAME "${CABBIRD_PACKAGE_NAME}"
        CABBIRD_PACKAGE_DIRECTORY "${CABBIRD_OUTPUT_DIRECTORY}"
        CABBIRD_PACKAGE_MANIFEST "${CABBIRD_MANIFEST}")
    if(MSVC)
        # Plugins are manually mapped in-process like Cabbird.Core: disable
        # compiler-generated _Init_thread_epoch static TLS for function-local statics.
        target_compile_options(${target} PRIVATE
            /W4
            $<$<COMPILE_LANGUAGE:C,CXX>:/Zc:threadSafeInit->
            $<$<COMPILE_LANGUAGE:CXX>:/permissive->)
        if(NOT CABBIRD_C_ONLY)
            target_compile_options(${target} PRIVATE /EHsc)
        endif()
    endif()

    get_filename_component(cabbird_package_source "${CABBIRD_MANIFEST}" DIRECTORY)
    set(cabbird_sync_script "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/SyncPluginPackage.cmake")
    # Always synchronize the small, owned data set. This also handles deleted files
    # and removed directories, which a timestamped copy command cannot observe.
    add_custom_target(${target}_package_data
        COMMAND "${CMAKE_COMMAND}"
            "-DCABBIRD_PACKAGE_SOURCE=${cabbird_package_source}"
            "-DCABBIRD_PACKAGE_MANIFEST=${CABBIRD_MANIFEST}"
            "-DCABBIRD_PACKAGE_DIRECTORY=$<TARGET_FILE_DIR:${target}>"
            -P "${cabbird_sync_script}"
        VERBATIM)
    add_dependencies(${target} ${target}_package_data)

    if(CABBIRD_NO_RELEASE)
        return()
    endif()
    set_property(GLOBAL APPEND PROPERTY CABBIRD_RELEASE_PLUGIN_TARGETS "${target}")
    set(cabbird_install_directory "Cabbird/plugins/${CABBIRD_PACKAGE_NAME}")
    install(TARGETS ${target}
        RUNTIME DESTINATION "${cabbird_install_directory}" COMPONENT GameRuntime
        LIBRARY DESTINATION "${cabbird_install_directory}" COMPONENT GameRuntime)
    install(CODE "
        execute_process(COMMAND \"${CMAKE_COMMAND}\"
            \"-DCABBIRD_PACKAGE_SOURCE=${cabbird_package_source}\"
            \"-DCABBIRD_PACKAGE_MANIFEST=${CABBIRD_MANIFEST}\"
            \"-DCABBIRD_PACKAGE_DIRECTORY=\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${cabbird_install_directory}\"
            -P \"${cabbird_sync_script}\"
            COMMAND_ERROR_IS_FATAL ANY)
        " COMPONENT GameRuntime)
endfunction()
