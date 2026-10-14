@echo off
rem Builds the shader translator corpus test (indra\vulkan_skeleton\translator) into build-translator-test\Release\translator_test.exe.
rem Run it with:  build-translator-test\Release\translator_test.exe C:\phoenix-firestorm\shaderdump [out_dir]
rem Expected: self-check ok, 210 / 210 programs, 424 / 424 stages and SPIR-V modules, 0 layout mismatches, RESULT: PASS (about 6 s, no GPU).
set VULKAN_SDK=C:\VulkanSDK\1.4.357.0
set CMAKE_GENERATOR=Visual Studio 17 2022
set CMAKE_GENERATOR_PLATFORM=x64
set PATH=C:\Program Files\CMake\bin;%PATH%
cd /d C:\phoenix-firestorm
cmake -S indra\vulkan_skeleton\translator -B build-translator-test -DLL_GLSLANG_ROOT=C:\phoenix-firestorm\build-vc170-64\packages-vulkan
if errorlevel 1 exit /b 1
cmake --build build-translator-test --config Release -- /v:minimal
