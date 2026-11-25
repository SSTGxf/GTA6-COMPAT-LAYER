@echo off
rem ScarlettCompat shader pipeline - Phase 6 harness (resources and pipeline).
rem Builds ONE binary from sc_pipeline_binding.cpp + sc_sha256.cpp, WITHOUT
rem sc_dxil_emit.cpp or any D3D header. Does not build the layer and never boots the game.
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

if not exist "out\shader_tests\pipeline_binding" mkdir "out\shader_tests\pipeline_binding"

rem The module needs the hasher only: no D3D, no frontend, no emitter.
set "SRC=src\shader\sc_pipeline_binding.cpp src\shader\sc_sha256.cpp"

echo Using MSVC: %MSVC%
echo Using SDK : %SDKVER%

echo.
echo [build] the resources/pipeline binding module and its harness, /W4 /WX
cl -nologo -std:c++17 -EHsc -W4 -WX -DSC_FRONTEND_NO_DXC -Iinclude -Itests\shader ^
   -Fe:out\shader_tests\sc_pipeline_binding_test.exe ^
   -Fo:out\shader_tests\pipeline_binding\ ^
   %SRC% tests\shader\sc_pipeline_binding_main.cpp
if errorlevel 1 (
    echo BUILD FAILED pipeline-binding-build
    popd
    exit /b 1
)

echo.
echo === running sc_pipeline_binding_test.exe ===
out\shader_tests\sc_pipeline_binding_test.exe
set "R=%ERRORLEVEL%"

echo.
echo pipeline binding test exit=%R%
if not "%R%"=="0" ( popd & exit /b 1 )
popd
exit /b 0
