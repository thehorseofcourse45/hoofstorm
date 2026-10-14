@echo off
rem Builds the standalone Vulkan skeleton (indra\vulkan_skeleton) into build-vulkan-skeleton\Release\vulkan_skeleton.exe.
rem Run it with:  set VK_LAYER_PATH=C:\VulkanSDK\1.4.357.0\Bin
rem   vulkan_skeleton.exe --test --dump C:\phoenix-firestorm\shaderdump        all scenes, exit code 0 = pass, opens a window for about a second
rem   vulkan_skeleton.exe --link-all --dump C:\phoenix-firestorm\shaderdump    every program of the dump through the shader backend
set VULKAN_SDK=C:\VulkanSDK\1.4.357.0
set CMAKE_GENERATOR=Visual Studio 17 2022
set CMAKE_GENERATOR_PLATFORM=x64
set PATH=C:\Program Files\CMake\bin;%PATH%
cd /d C:\phoenix-firestorm
cmake -S indra\vulkan_skeleton -B build-vulkan-skeleton -DLL_GLSLANG_ROOT=C:\phoenix-firestorm\build-vc170-64\packages-vulkan
if errorlevel 1 exit /b 1
cmake --build build-vulkan-skeleton --config Release -- /v:minimal
