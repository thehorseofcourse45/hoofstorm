@echo off
rem Builds the viewer (target firestorm-bin). Run from cmd.exe or: cmd.exe //c "C:\phoenix-firestorm\doc\vulkan\tools\build-viewer.cmd"
rem Result: build-vc170-64\newview\Release\firestorm-bin.exe. The log is build-vc170-64\viewer_build.log.
rem ALWAYS read the log: the last line must be BUILD_EXIT=0 and "error C" / "error LNK" must not occur.
rem The exit code of this wrapper only says the wrapper ran.
set AUTOBUILD_VSVER=170
set AUTOBUILD_VARIABLES_FILE=C:\fs-build-variables\variables
set PATH=C:\fs-build-venv\Scripts;C:\Program Files\CMake\bin;C:\cygwin64\bin;%PATH%
cmake --build C:\phoenix-firestorm\build-vc170-64 --config Release --target firestorm-bin -- /m /v:minimal > C:\phoenix-firestorm\build-vc170-64\viewer_build.log 2>&1
echo BUILD_EXIT=%ERRORLEVEL% >> C:\phoenix-firestorm\build-vc170-64\viewer_build.log
