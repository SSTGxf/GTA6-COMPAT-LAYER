@echo off
rem ScarlettCompat shader pipeline - GPU decode verification for BCn textures.
rem Creates its own D3D12 device and decodes textures on the GPU. Needs a D3D12 adapter;
rem does not build the layer and never boots the game.
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

if not exist "out\shader_tests\bc_gpu" mkdir "out\shader_tests\bc_gpu"

echo Using MSVC: %MSVC%
echo Using SDK : %SDKVER%

echo.
echo [build] the GPU decode verifier, /W4
cl -nologo -std:c++17 -EHsc -W4 -Iinclude -Ithird_party -Itests\shader ^
   -Fe:out\shader_tests\sc_bc_gpu_verify.exe ^
   -Fo:out\shader_tests\bc_gpu\ ^
   src\shader\sc_bc_decode.cpp tests\shader\sc_bc_gpu_verify.cpp ^
   d3d12.lib dxgi.lib d3dcompiler.lib
if errorlevel 1 (
    echo BUILD FAILED bc-gpu-verify-build
    popd
    exit /b 1
)

echo.
echo === running sc_bc_gpu_verify.exe ===
out\shader_tests\sc_bc_gpu_verify.exe
set "R=%ERRORLEVEL%"

echo.
echo bc gpu verify exit=%R%
if not "%R%"=="0" ( popd & exit /b 1 )
popd
exit /b 0
