# Standalone Vulkan skeleton. NOT part of the viewer build: indra/CMakeLists.txt does not reference this directory.
#
#   set VULKAN_SDK=C:\VulkanSDK\1.4.357.0
#   cmake -S indra/vulkan_skeleton -B build-vulkan-skeleton -G "Visual Studio 17 2022" -A x64
#   cmake --build build-vulkan-skeleton --config Release
#   set VK_LAYER_PATH=C:\VulkanSDK\1.4.357.0\Bin
#   build-vulkan-skeleton\Release\vulkan_skeleton.exe --test --dump C:\phoenix-firestorm\shaderdump
#
# The ui scene compiles a real viewer program from a shader dump (see shader_probe/README.md) with the shader translator
# and glslang from the local autobuild compiler package.
cmake_minimum_required(VERSION 3.20)
project(vulkan_skeleton CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Vulkan REQUIRED)
find_program(GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin" "${Vulkan_INCLUDE_DIR}/../Bin" REQUIRED)
# Only the dependency-free backend interface headers are used from the viewer (they need nothing but stdtypes.h).
get_filename_component(INDRA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

add_subdirectory("${INDRA_DIR}/llrender/vulkan" "${CMAKE_CURRENT_BINARY_DIR}/llvulkan")
add_executable(vulkan_skeleton main.cpp framebuffer_test.cpp window_test.cpp "${INDRA_DIR}/llrender/vulkan/tests/shader_test.cpp")
target_include_directories(vulkan_skeleton PRIVATE
    "${INDRA_DIR}/llrender"
    "${INDRA_DIR}/llcommon"
    "${CMAKE_CURRENT_SOURCE_DIR}/translator")
target_link_libraries(vulkan_skeleton PRIVATE llvulkan opengl32)

target_compile_definitions(vulkan_skeleton PRIVATE _CRT_SECURE_NO_WARNINGS)

foreach(stage vert frag)
    set(spv "$<TARGET_FILE_DIR:vulkan_skeleton>/tri.${stage}.spv")
    add_custom_command(TARGET vulkan_skeleton POST_BUILD
        COMMAND "${GLSLC}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/tri.${stage}" -o "${spv}"
        COMMENT "glslc tri.${stage}")
endforeach()
