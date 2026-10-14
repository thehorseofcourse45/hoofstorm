@echo off
rem Overlap stress for multiple frames in flight. Build target llvulkan_frames_test first (build-vc170-64, Release).
rem Usage: run-frames-test.cmd [1|2]   (frames in flight, default 2). Must end with FRAMES_CHECKS=<n> PASS.
set VK_LAYER_PATH=C:\VulkanSDK\1.4.357.0\Bin
set VK_LAYER_VALIDATE_SYNC=1
if "%1"=="" (set FS_VK_FRAMES_IN_FLIGHT=2) else (set FS_VK_FRAMES_IN_FLIGHT=%1)
C:\phoenix-firestorm\build-vc170-64\llrender\vulkan\Release\llvulkan_frames_test.exe
