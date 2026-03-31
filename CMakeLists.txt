cmake_minimum_required(VERSION 3.16)
project(LIME VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find required packages
find_package(Vulkan REQUIRED)
find_package(glfw3 REQUIRED)
find_package(glm REQUIRED)

# Fetch Dear ImGui (docking branch)
include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG docking
)
FetchContent_MakeAvailable(imgui)

# Fetch stb (for image loading)
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG master
)
FetchContent_MakeAvailable(stb)

# Fetch tinygltf (for GLB/GLTF model loading)
FetchContent_Declare(
    tinygltf
    GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
    GIT_TAG v2.8.21
)
FetchContent_MakeAvailable(tinygltf)

# Fetch nlohmann/json
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json)

# Fetch NFD-Extended (native file dialogs)
FetchContent_Declare(
    nfd
    GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended.git
    GIT_TAG v1.1.1
)
set(NFD_PORTAL ON CACHE BOOL "Use xdg-desktop-portal on Linux")
FetchContent_MakeAvailable(nfd)

# Fetch cpp-httplib (header-only HTTP client/server)
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.15.3
)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "")
set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE BOOL "")
FetchContent_MakeAvailable(httplib)

# Create ImGui library
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC Vulkan::Vulkan glfw)

# Include shader compilation
include(cmake/CompileShaders.cmake)

# --- lime_engine static library (vendored EDEN subset) ---
set(LIME_ENGINE_SOURCES
    engine/Camera.cpp
    engine/Input.cpp
    engine/Window.cpp
    engine/Mesh.cpp
    engine/Animation/AnimationPlayer.cpp
    engine/Entity/Entity.cpp
    engine/Entity/Action.cpp
    engine/Entity/ActionSystem.cpp
    engine/Renderer/VulkanApplicationBase.cpp
    engine/Renderer/VulkanContext.cpp
    engine/Renderer/Swapchain.cpp
    engine/Renderer/Buffer.cpp
    engine/Renderer/Pipeline.cpp
    engine/Renderer/PipelineBuilder.cpp
    engine/Renderer/ImGuiManager.cpp
    engine/Renderer/ModelRenderer.cpp
    engine/Renderer/SkinnedModelRenderer.cpp
    engine/Renderer/TextureManager.cpp
    engine/Editor/SceneObject.cpp
    engine/Editor/GLBLoader.cpp
    engine/Editor/SkinnedGLBLoader.cpp
    engine/Editor/LimeLoader.cpp
)

add_library(lime_engine STATIC ${LIME_ENGINE_SOURCES})

target_include_directories(lime_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/include
    ${stb_SOURCE_DIR}
    ${tinygltf_SOURCE_DIR}
    ${httplib_SOURCE_DIR}
)
target_include_directories(lime_engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(lime_engine PUBLIC
    Vulkan::Vulkan
    glfw
    glm::glm
    imgui
    nlohmann_json::nlohmann_json
    nfd
    pthread
)

# --- lime executable ---
set(LIME_SOURCES
    src/main.cpp
    src/EditableMesh.cpp
    src/AnimationMode.cpp
    src/ModelingMode.cpp
    src/ModelingMode_UV.cpp
    src/ModelingMode_Gizmo.cpp
    src/ModelingMode_Ports.cpp
    src/ModelingMode_Paint.cpp
    src/ModelingMode_Snap.cpp
    src/ModelingMode_Retopo.cpp
    src/ModelingMode_AutoRetopo.cpp
    src/ModelingMode_Primitives.cpp
    src/ModelingMode_PathTube.cpp
    src/ModelingMode_Slice.cpp
    src/ModelingMode_Rigging.cpp
    src/PaintBrushRing.cpp
    src/MCPServer.cpp
)

add_executable(lime ${LIME_SOURCES})

# Tracy profiler integration
set(TRACY_DIR "$ENV{HOME}/Desktop/tracy")
if(EXISTS ${TRACY_DIR}/public/tracy/Tracy.hpp)
    option(TRACY_ENABLE "Enable Tracy profiler" ON)
    if(TRACY_ENABLE)
        target_sources(lime_engine PRIVATE ${TRACY_DIR}/public/TracyClient.cpp)
        target_include_directories(lime_engine PUBLIC ${TRACY_DIR}/public)
        target_compile_definitions(lime_engine PUBLIC TRACY_ENABLE)
    endif()
endif()

target_link_libraries(lime PRIVATE lime_engine httplib::httplib)
target_compile_definitions(lime PRIVATE CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

target_include_directories(lime PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

# Compile shaders
compile_shaders(lime
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/default.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/default.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/model.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/model.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/wireframe.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/wireframe.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/selection.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/selection.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/skinned_model.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/skinned_model.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/brush_ring.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/brush_ring.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/gizmo.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/gizmo.frag
)

# Copy compiled shaders to build directory
add_custom_command(TARGET lime POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:lime>/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_BINARY_DIR}/shaders/default.vert.spv
        ${CMAKE_CURRENT_BINARY_DIR}/shaders/default.frag.spv
        ${CMAKE_CURRENT_BINARY_DIR}/shaders/model.vert.spv
        ${CMAKE_CURRENT_BINARY_DIR}/shaders/model.frag.spv
        ${CMAKE_CURRENT_BINARY_DIR}/shaders/wireframe.vert.spv
        ${CMAKE_CURRENT_BINARY_DIR}/shaders/wireframe.frag.spv
        ${CMAKE_CURRENT_BINARY_DIR}/shaders/selection.vert.spv
        ${CMAKE_CURRENT_BINARY_DIR}/shaders/selection.frag.spv
        ${CMAKE_CURRENT_BINARY_DIR}/shaders/skinned_model.vert.spv
        ${CMAKE_CURRENT_BINARY_DIR}/shaders/skinned_model.frag.spv
        ${CMAKE_CURRENT_BINARY_DIR}/shaders/brush_ring.vert.spv
        ${CMAKE_CURRENT_BINARY_DIR}/shaders/brush_ring.frag.spv
        ${CMAKE_CURRENT_BINARY_DIR}/shaders/gizmo.vert.spv
        ${CMAKE_CURRENT_BINARY_DIR}/shaders/gizmo.frag.spv
        $<TARGET_FILE_DIR:lime>/shaders/
)

# Copy library assets to build directory
add_custom_command(TARGET lime POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/library
        $<TARGET_FILE_DIR:lime>/library
)
