@echo off
rem ScarlettCompat shader pipeline - standalone build gate.
rem
rem A phase is not done because its harness passes: the sources have to compile as
rem a unit. This builds EVERY shader source at /W4 /WX (warnings are errors) and
rem then archives the no-DXC set into a static library, which proves the decode
rem path links without sc_dxil_emit.cpp (the DXC-facing file is compiled but not
rem archived or linked, so no dxcompiler import is pulled in).
rem
rem Does not build the layer and never boots the game.
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

if not exist "out\shader_tests\all" mkdir "out\shader_tests\all"
del /q "out\shader_tests\all\*.obj" 2>nul

rem The no-DXC set: everything the decode path may use. sc_bc_decode.cpp needs
rem -Ithird_party for the vendored bcdec.h it owns; no other source includes it.
set "NODXC=src\shader\sc_sha256.cpp src\shader\sc_container.cpp src\shader\sc_xs2.cpp src\shader\sc_frontend.cpp src\shader\sc_isa.cpp src\shader\sc_wave.cpp src\shader\sc_capture.cpp src\shader\sc_dxil_backend.cpp src\shader\sc_gpu_render_test.cpp src\shader\sc_mesh_compute.cpp src\shader\sc_pipeline_binding.cpp src\shader\sc_bc_decode.cpp"

echo Using MSVC: %MSVC%
echo Using SDK : %SDKVER%

echo.
echo [1/4] compile the no-DXC source set at /W4 /WX
cl -nologo -std:c++17 -EHsc -W4 -WX -DSC_FRONTEND_NO_DXC -Iinclude -Ithird_party -c ^
   -Fo:out\shader_tests\all\ %NODXC%
if errorlevel 1 (
    echo BUILD FAILED all-sources: at least one shader source did not compile cleanly
    popd
    exit /b 1
)

echo.
echo [2/4] compile the DXC-facing file on its own at /W4 /WX (not linked)
rem Deliberately WITHOUT -DSC_FRONTEND_NO_DXC: the emitter must be compiled in its
rem real configuration. (It currently has no such branch, but compiling it under
rem the no-DXC define would stop catching a stub if one was ever added.)
cl -nologo -std:c++17 -EHsc -W4 -WX -Iinclude -Ithird_party -c ^
   -Fo:out\shader_tests\all\ src\shader\sc_dxil_emit.cpp
if errorlevel 1 (
    echo BUILD FAILED all-sources: sc_dxil_emit.cpp did not compile cleanly
    popd
    exit /b 1
)

echo.
echo [3/4] archive the no-DXC set (proves the decode path needs no emitter)
del /q "out\shader_tests\all\sc_dxil_emit.obj" 2>nul
lib -nologo -OUT:out\shader_tests\sc_shader_nodxc.lib out\shader_tests\all\*.obj
if errorlevel 1 (
    echo BUILD FAILED archive
    popd
    exit /b 1
)

echo.
echo [4/4] compile the no-DXC set again at /std:c++14 (the layer's toolchain may not pass /std:c++17)
if not exist "out\shader_tests\all14" mkdir "out\shader_tests\all14"
del /q "out\shader_tests\all14\*.obj" 2>nul
cl -nologo -std:c++14 -EHsc -W4 -WX -DSC_FRONTEND_NO_DXC -Iinclude -Ithird_party -c ^
   -Fo:out\shader_tests\all14\ %NODXC%
if errorlevel 1 (
    echo BUILD FAILED cxx14: the shader sources must not require C++17
    popd
    exit /b 1
)

echo.
echo all shader sources compiled at /W4 /WX with zero warnings (C++17 and C++14); no-DXC library built
popd
exit /b 0
