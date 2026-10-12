# Standalone test for LLShaderTranslator. NOT part of the viewer build.
#
#   set VULKAN_SDK=C:\VulkanSDK\1.4.357.0
#   set CMAKE_GENERATOR=Visual Studio 17 2022
#   set CMAKE_GENERATOR_PLATFORM=x64
#   cmake -S indra\vulkan_skeleton\translator -B build-translator-test
#   cmake --build build-translator-test --config Release
#   build-translator-test\Release\translator_test.exe C:\phoenix-firestorm\shaderdump [out_dir]
#
# The directory is the one written by the viewer with FS_SHADER_DUMP_DIR (see ..\shader_probe\README.md).
# Exit code 0 = every program stage compiled to valid Vulkan SPIR-V and every program linked.
cmake_minimum_required(VERSION 3.20)
project(translator_test CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED ENV{VULKAN_SDK})
    message(FATAL_ERROR "Set VULKAN_SDK (glslang and SPIRV-Tools come from the Vulkan SDK)")
endif()
set(SDK "$ENV{VULKAN_SDK}")

add_executable(translator_test translator_test.cpp llshadertranslator.cpp llshadercompiler.cpp)
target_include_directories(translator_test PRIVATE "${SDK}/Include" "${SDK}/Include/glslang")
target_compile_definitions(translator_test PRIVATE _CRT_SECURE_NO_WARNINGS)
foreach(lib glslang SPIRV MachineIndependent GenericCodeGen OSDependent glslang-default-resource-limits SPIRV-Tools-opt SPIRV-Tools)
    target_link_libraries(translator_test PRIVATE
        optimized "${SDK}/Lib/${lib}.lib"
        debug "${SDK}/Lib/${lib}d.lib")
endforeach()
