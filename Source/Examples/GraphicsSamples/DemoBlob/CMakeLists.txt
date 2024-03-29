set(TARGET_NAME DemoBlob)

add_executable(${TARGET_NAME})
target_sources(${TARGET_NAME} PRIVATE SketchApp.cpp)

# 私有链接库
target_include_directories(${TARGET_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/Source/Launcher)
target_link_libraries(${TARGET_NAME} PRIVATE Launcher)

target_include_directories(${TARGET_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/Source/Sketch)
target_link_libraries(${TARGET_NAME} PRIVATE Sketch)

target_link_libraries(${TARGET_NAME} PRIVATE DirectX-Headers)

target_link_libraries(${TARGET_NAME} PRIVATE dxgi.lib d3d12.lib d3dcompiler.lib)

function(compile_shaders _target_name _shader_files)
    # 寻找fxc
    get_filename_component(WINDOWS_KITS_DIR
        "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]" ABSOLUTE)
    set(_fxc_compiler ${WINDOWS_KITS_DIR}/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64/fxc.exe)

    # shader文件编译所生成头文件的存放路径
    set(_generated_shader_header_dir ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/GeneratedShaderHeaders)
    target_include_directories(${_target_name} PRIVATE ${_generated_shader_header_dir})

    set(_shader_header_files "")

    # fxc编译命令
    set(_fxc_debug ${_fxc_compiler} /nologo /Vi /Od /Zi)
    set(_fxc_release ${_fxc_compiler} /nologo /Vi)

    foreach(_single_shader_file ${_shader_files})
        # 将shader(hlsl)文件加入VS工程
        target_sources(${_target_name} PRIVATE ${_single_shader_file})
        set_property(SOURCE ${_single_shader_file} PROPERTY VS_SETTINGS "ExcludedFromBuild=true")
        source_group("Shader Files" FILES ${_single_shader_file})

        # shader头文件名
        get_filename_component(_shader_name ${_single_shader_file} NAME_WE)
        set(_shader_header_vs ${_generated_shader_header_dir}/${_shader_name}VS.h)
        set(_shader_header_ps ${_generated_shader_header_dir}/${_shader_name}PS.h)

        set(_shader_header_files ${_shader_header_files} ${_shader_header_vs} ${_shader_header_ps})

        set(_fxc_vs_flag /T vs_5_0 /E VSMain /Vn g_${_shader_name}_VSMain)
        set(_fxc_ps_flag /T ps_5_0 /E PSMain /Vn g_${_shader_name}_PSMain)

        add_custom_command(
            OUTPUT ${_shader_header_vs}
            COMMAND if $<CONFIG> equ Debug (${_fxc_debug} ${_single_shader_file} ${_fxc_vs_flag} /Fh ${_shader_header_vs})
            COMMAND if $<CONFIG> neq Debug (${_fxc_release} ${_single_shader_file} ${_fxc_vs_flag} /Fh ${_shader_header_vs})
            DEPENDS ${_single_shader_file}
            VERBATIM
        )
        
        add_custom_command(
            OUTPUT ${_shader_header_ps}
            COMMAND if $<CONFIG> equ Debug (${_fxc_debug} ${_single_shader_file} ${_fxc_ps_flag} /Fh ${_shader_header_ps})
            COMMAND if $<CONFIG> neq Debug (${_fxc_release} ${_single_shader_file} ${_fxc_ps_flag} /Fh ${_shader_header_ps})
            DEPENDS ${_single_shader_file}
            VERBATIM
        )
    endforeach()

    # 实现对shader头文件的依赖
    set(_shader_target ${_target_name}Shader)
    add_custom_target(${_shader_target} DEPENDS ${_shader_header_files})
    set_target_properties(${_shader_target} PROPERTIES FOLDER AutoGeneratedTargets)
    add_dependencies(${_target_name} ${_shader_target})
endfunction(compile_shaders _target_name _shader_files)

compile_shaders(${TARGET_NAME} ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Shaders.hlsl)


# # 寻找fxc
# get_filename_component(WINDOWS_KITS_DIR
#     "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]" ABSOLUTE)
# set(FXC ${WINDOWS_KITS_DIR}/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64/fxc.exe)

# # 将shader(hlsl)文件加入VS工程
# set(SHADER_FILE ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Shaders.hlsl)
# target_sources(${TARGET_NAME} PRIVATE ${SHADER_FILE})
# set_property(SOURCE ${SHADER_FILE} PROPERTY VS_SETTINGS "ExcludedFromBuild=true")
# source_group("Shader Files" FILES ${SHADER_FILE})

# # shader文件编译所生成头文件的存放路径
# set(GENERATED_SHADER_HEADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/GeneratedShaderHeaders)
# get_filename_component(SHADER_NAME ${SHADER_FILE} NAME_WE)
# set(SHADER_HEADER_VS ${GENERATED_SHADER_HEADER_DIR}/${SHADER_NAME}VS.h)
# set(SHADER_HEADER_PS ${GENERATED_SHADER_HEADER_DIR}/${SHADER_NAME}PS.h)

# target_include_directories(${TARGET_NAME} PRIVATE ${GENERATED_SHADER_HEADER_DIR})

# # shader头文件加入target，实现对该文件的依赖，但是这样会加入所有$<CONFIG>中的文件，不太整洁。目前使用add_custom_target实现依赖。
# # target_sources(${TARGET_NAME} PRIVATE ${SHADER_HEADER_VS})
# # target_sources(${TARGET_NAME} PRIVATE ${SHADER_HEADER_PS})

# # 实现对shader头文件的依赖
# set(SHADER_TARGET ${TARGET_NAME}Shader)
# add_custom_target(${SHADER_TARGET} DEPENDS ${SHADER_HEADER_VS} ${SHADER_HEADER_PS})
# set_target_properties(${SHADER_TARGET} PROPERTIES FOLDER AutoGeneratedTargets)
# add_dependencies(${TARGET_NAME} ${SHADER_TARGET})

# # fxc编译命令
# set(FXC_DEBUG ${FXC} /nologo /Vi /Od /Zi)
# set(FXC_RELEASE ${FXC} /nologo /Vi)
# set(FXC_VS_FLAG /T vs_5_0 /E VSMain /Vn g_${SHADER_NAME}_VSMain)
# set(FXC_PS_FLAG /T ps_5_0 /E PSMain /Vn g_${SHADER_NAME}_PSMain)

# add_custom_command(
#     OUTPUT ${SHADER_HEADER_VS}
#     COMMAND if $<CONFIG> equ Debug (${FXC_DEBUG} ${SHADER_FILE} ${FXC_VS_FLAG} /Fh ${SHADER_HEADER_VS})
#     COMMAND if $<CONFIG> neq Debug (${FXC_RELEASE} ${SHADER_FILE} ${FXC_VS_FLAG} /Fh ${SHADER_HEADER_VS})
#     DEPENDS ${SHADER_FILE}
#     VERBATIM
# )

# add_custom_command(
#     OUTPUT ${SHADER_HEADER_PS}
#     COMMAND if $<CONFIG> equ Debug (${FXC_DEBUG} ${SHADER_FILE} ${FXC_PS_FLAG} /Fh ${SHADER_HEADER_PS})
#     COMMAND if $<CONFIG> neq Debug (${FXC_RELEASE} ${SHADER_FILE} ${FXC_PS_FLAG} /Fh ${SHADER_HEADER_PS})
#     DEPENDS ${SHADER_FILE}
#     VERBATIM
# )
