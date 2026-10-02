cmake_minimum_required(VERSION 3.24)
project(PbrRenderer VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/MP /permissive-)
endif()

# ------------------------------------------------------------------- Vulkan 1.4
find_package(Vulkan REQUIRED)
if(Vulkan_VERSION AND Vulkan_VERSION VERSION_LESS 1.4)
    message(FATAL_ERROR
        "Vulkan SDK ${Vulkan_VERSION} found, but this engine requires the Vulkan 1.4+ SDK "
        "(it uses core Vulkan 1.4 features: push descriptors, store-op NONE).")
endif()

option(PBR_VALIDATION "Enable Vulkan validation layers at runtime" ON)

# ----------------------------------------------------------------- dependencies
include(FetchContent)

set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw     GIT_REPOSITORY https://github.com/glfw/glfw.git        GIT_TAG 3.4    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(glfw)

FetchContent_Declare(glm      GIT_REPOSITORY https://github.com/g-truc/glm.git       GIT_TAG 1.0.1  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(glm)

FetchContent_Declare(imgui    GIT_REPOSITORY https://github.com/ocornut/imgui.git    GIT_TAG v1.91.9 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC glfw Vulkan::Vulkan)

FetchContent_Declare(stb GIT_REPOSITORY https://github.com/nothings/stb.git GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(stb)
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE ${stb_SOURCE_DIR})

set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_ImplicitConversions OFF CACHE BOOL "" FORCE)
FetchContent_Declare(json GIT_REPOSITORY https://github.com/nlohmann/json.git GIT_TAG v3.11.3 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(json)

FetchContent_Declare(tinygltf GIT_REPOSITORY https://github.com/syoyo/tinygltf.git GIT_SHALLOW TRUE SOURCE_SUBDIR src)
FetchContent_MakeAvailable(tinygltf)
add_library(tinygltf INTERFACE)
target_include_directories(tinygltf INTERFACE ${tinygltf_SOURCE_DIR}/src)
target_link_libraries(tinygltf INTERFACE nlohmann_json::nlohmann_json)

FetchContent_Declare(vma GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git GIT_TAG v3.0.1 GIT_SHALLOW TRUE SOURCE_SUBDIR include)
FetchContent_MakeAvailable(vma)
add_library(vma INTERFACE)
target_include_directories(vma INTERFACE ${vma_SOURCE_DIR}/include)

# ------------------------------------------------------------------ executable
add_executable(PbrRenderer
    src/main.cpp
    src/core/Application.cpp
    src/core/JobSystem.cpp
    src/core/Log.cpp
    src/platform/HttpDownloader.cpp
    src/assets/AssetManager.cpp
    src/assets/GltfLoader.cpp
    src/assets/TextureLoader.cpp
    src/render/UploadQueue.cpp
    src/scene/Camera.cpp
    src/scene/Frustum.cpp
    src/scene/Scene.cpp
    src/render/DebugUi.cpp
    src/render/DescriptorAllocator.cpp
    src/render/GpuProfiler.cpp
    src/render/PipelineBuilder.cpp
    src/render/Renderer.cpp
    src/render/ShadowMap.cpp
    src/render/Swapchain.cpp
    src/render/VmaImpl.cpp
    src/render/VulkanContext.cpp)

target_include_directories(PbrRenderer PRIVATE src)
target_link_libraries(PbrRenderer PRIVATE
    Vulkan::Vulkan glfw glm::glm imgui stb nlohmann_json::nlohmann_json tinygltf vma)
if(WIN32)
    target_link_libraries(PbrRenderer PRIVATE winhttp)
endif()

target_compile_definitions(PbrRenderer PRIVATE
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    RENDERER_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders"
    RENDERER_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets")
if(PBR_VALIDATION)
    target_compile_definitions(PbrRenderer PRIVATE PBR_VALIDATION=1)
endif()

# -------------------------------------------------------- SPIR-V (Vulkan 1.4)
function(pbr_compile_shaders target)
    find_program(PBR_GLSLC NAMES glslc HINTS "$ENV{VULKAN_SDK}" PATH_SUFFIXES Bin)
    find_program(PBR_GLSLANG NAMES glslangValidator HINTS "$ENV{VULKAN_SDK}" PATH_SUFFIXES Bin)

    set(outdir "${CMAKE_BINARY_DIR}/shaders")
    file(MAKE_DIRECTORY ${outdir})

    file(GLOB shader_sources
         "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.vert"
         "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.frag")

    set(outputs)
    foreach(src ${shader_sources})
        get_filename_component(name ${src} NAME)
        set(out "${outdir}/${name}.spv")
        if(PBR_GLSLC)
            add_custom_command(OUTPUT ${out}
                COMMAND ${PBR_GLSLC} --target-env=vulkan1.4 -O "${src}" -o "${out}"
                DEPENDS ${src} VERBATIM)
        elseif(PBR_GLSLANG)
            add_custom_command(OUTPUT ${out}
                COMMAND ${PBR_GLSLANG} -V --target-env vulkan1.4 -o "${out}" "${src}"
                DEPENDS ${src} VERBATIM)
        else()
            message(FATAL_ERROR "glslc/glslangValidator not found. Install the Vulkan SDK and set VULKAN_SDK.")
        endif()
        list(APPEND outputs ${out})
    endforeach()

    add_custom_target(${target}_shaders ALL DEPENDS ${outputs})
    add_dependencies(${target} ${target}_shaders)
endfunction()

pbr_compile_shaders(PbrRenderer)
set_target_properties(PbrRenderer PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")