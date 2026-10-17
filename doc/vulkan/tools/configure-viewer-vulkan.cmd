@echo off
rem Build number shown in the version (otherwise the git commit count); change it with each release.
if not defined revision set revision=81831
set AUTOBUILD_VSVER=170
set AUTOBUILD_VARIABLES_FILE=C:\fs-build-variables\variables
set VULKAN_SDK=C:\VulkanSDK\1.4.357.0
set PATH=C:\fs-build-venv\Scripts;C:\Program Files\CMake\bin;C:\cygwin64\bin;%PATH%
cd /d C:\phoenix-firestorm
cmake -S C:\phoenix-firestorm\indra -B C:\phoenix-firestorm\build-vc170-64 -DLL_VULKAN_RUNTIME=ON -DLL_GLSLANG_ROOT=C:\phoenix-firestorm\build-vc170-64\packages-vulkan
exit /b %ERRORLEVEL%
