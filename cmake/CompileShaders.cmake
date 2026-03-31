# Find glslc shader compiler
find_program(GLSLC glslc HINTS
    $ENV{VULKAN_SDK}/bin
    /usr/bin
    /usr/local/bin
)

if(NOT GLSLC)
    message(FATAL_ERROR "glslc not found! Please install the Vulkan SDK.")
endif()

function(compile_shaders TARGET)
    set(SHADER_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
    file(MAKE_DIRECTORY ${SHADER_OUTPUT_DIR})

    foreach(SHADER_SOURCE ${ARGN})
        get_filename_component(SHADER_NAME ${SHADER_SOURCE} NAME)
        set(SHADER_OUTPUT ${SHADER_OUTPUT_DIR}/${SHADER_NAME}.spv)

        add_custom_command(
            OUTPUT ${SHADER_OUTPUT}
            COMMAND ${GLSLC} -o ${SHADER_OUTPUT} ${SHADER_SOURCE}
            DEPENDS ${SHADER_SOURCE}
            COMMENT "Compiling shader: ${SHADER_NAME}"
            VERBATIM
        )

        list(APPEND SHADER_OUTPUTS ${SHADER_OUTPUT})
    endforeach()

    add_custom_target(${TARGET}_shaders DEPENDS ${SHADER_OUTPUTS})
    add_dependencies(${TARGET} ${TARGET}_shaders)
endfunction()
