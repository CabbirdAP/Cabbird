# Compile DX11 UI shaders ahead of time; rendering performs no HLSL compilation.

file(GLOB CABBIRD_GLASS_FXC_CANDIDATES
    "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/*/x64/fxc.exe")
list(SORT CABBIRD_GLASS_FXC_CANDIDATES COMPARE NATURAL ORDER DESCENDING)
set(CABBIRD_GLASS_FXC_HINTS "")
foreach(candidate IN LISTS CABBIRD_GLASS_FXC_CANDIDATES)
    get_filename_component(candidate_directory "${candidate}" DIRECTORY)
    list(APPEND CABBIRD_GLASS_FXC_HINTS "${candidate_directory}")
endforeach()
find_program(CABBIRD_GLASS_FXC fxc
    HINTS
        "${CMAKE_WINDOWS_KITS_10_DIR}/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
        ${CABBIRD_GLASS_FXC_HINTS}
    REQUIRED)
set(CABBIRD_GLASS_SHADER_DIR "${CABBIRD_GENERATED_DIR}/cabbird/shaders")
file(MAKE_DIRECTORY "${CABBIRD_GLASS_SHADER_DIR}")
set(CABBIRD_GLASS_SHADER_HEADERS "")
function(cabbird_compile_glass_shader source entry profile variable)
    set(output "${CABBIRD_GLASS_SHADER_DIR}/${variable}.h")
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${CABBIRD_GLASS_FXC}" /nologo /O3 /Ges /T ${profile} /E ${entry}
                /Vn ${variable} /Fh "${output}" "${CMAKE_SOURCE_DIR}/${source}"
        DEPENDS "${CMAKE_SOURCE_DIR}/${source}"
        COMMENT "Compiling ${source} (${entry})"
        VERBATIM)
    set(CABBIRD_GLASS_SHADER_HEADERS ${CABBIRD_GLASS_SHADER_HEADERS} "${output}" PARENT_SCOPE)
endfunction()
cabbird_compile_glass_shader(src/render/dx11/shaders/glass_pass.hlsl vs_full vs_4_0 kGlassFullScreenVs)
cabbird_compile_glass_shader(src/render/dx11/shaders/glass_pass.hlsl ps_down ps_4_0 kGlassDownsamplePs)
cabbird_compile_glass_shader(src/render/dx11/shaders/glass_pass.hlsl ps_blur ps_4_0 kGlassBlurPs)
cabbird_compile_glass_shader(src/render/dx11/shaders/glass_surface.hlsl vs_quad vs_4_0 kGlassSurfaceVs)
cabbird_compile_glass_shader(src/render/dx11/shaders/glass_surface.hlsl ps_glass ps_4_0 kGlassSurfacePs)
