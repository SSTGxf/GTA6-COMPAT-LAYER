@echo off
rem ScarlettCompat shader pipeline - Phase 2 runner: builds and runs the
rem SYNTHETIC TRIANGLE test only. It does not build the layer and never boots
rem the game. Environment is set directly (no vcvars dependency).
setlocal enabledelayedexpansion

set "ROOT=%~dp0..\.."
pushd "%ROOT%" || exit /b 1

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found.
    popd
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo ERROR: no Visual Studio C++ tools found.
    popd
    exit /b 1
)

for /d %%d in ("%VSDIR%\VC\Tools\MSVC\*") do set "MSVC=%%d"

set "SDKROOT=%ProgramFiles(x86)%\Windows Kits\10"
for /f "delims=" %%v in ('dir /b /ad /o-n "%SDKROOT%\Lib" 2^>nul') do (
    if not defined SDKVER if exist "%SDKROOT%\Lib\%%v\ucrt\x64\ucrt.lib" set "SDKVER=%%v"
)
if not defined SDKVER (
    echo ERROR: no Windows SDK with x64 CRT found.
    popd
    exit /b 1
)

set "PATH=%MSVC%\bin\Hostx64\x64;%PATH%"
set "INCLUDE=%MSVC%\include;%SDKROOT%\Include\%SDKVER%\ucrt;%SDKROOT%\Include\%SDKVER%\um;%SDKROOT%\Include\%SDKVER%\shared;%SDKROOT%\Include\%SDKVER%\winrt"
set "LIB=%MSVC%\lib\x64;%SDKROOT%\Lib\%SDKVER%\ucrt\x64;%SDKROOT%\Lib\%SDKVER%\um\x64"

if not exist "out\shader_tests" mkdir "out\shader_tests"

echo Using MSVC: %MSVC%
echo Using SDK : %SDKVER%
cl -nologo -std:c++17 -EHsc -W4 -Iinclude ^
   -Fe:out\shader_tests\sc_triangle_test.exe ^
   -Fo:out\shader_tests\ ^
   src\shader\sc_sha256.cpp ^
   src\shader\sc_container.cpp ^
   src\shader\sc_capture.cpp ^
   src\shader\sc_dxil_emit.cpp ^
   src\shader\sc_dxil_backend.cpp ^
   src\shader\sc_gpu_render_test.cpp ^
   tests\shader\sc_triangle_main.cpp ^
   d3d12.lib dxgi.lib
if errorlevel 1 (
    echo BUILD FAILED
    popd
    exit /b 1
)

out\shader_tests\sc_triangle_test.exe out\shader_tests
set "RESULT=%ERRORLEVEL%"
echo triangle test exit=%RESULT%
popd
exit /b %RESULT%
