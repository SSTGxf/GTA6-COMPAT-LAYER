@echo off
rem ScarlettCompat shader pipeline - Phase 5 harness (operands + wave semantics).
rem Builds ONE binary: frontend + ISA decoder + wave module, WITHOUT sc_dxil_emit.cpp.
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

if not exist "out\shader_tests\wave" mkdir "out\shader_tests\wave"

rem sc_isa.cpp supplies both the terminator table and the length rules.
set "SRC=src\shader\sc_sha256.cpp src\shader\sc_container.cpp src\shader\sc_isa.cpp src\shader\sc_xs2.cpp src\shader\sc_frontend.cpp src\shader\sc_wave.cpp"

echo Using MSVC: %MSVC%
echo Using SDK : %SDKVER%

echo.
echo [build] frontend + decoder + wave module, no emitter linked
cl -nologo -std:c++17 -EHsc -W4 -DSC_FRONTEND_NO_DXC -Iinclude ^
   -Fe:out\shader_tests\sc_wave_test.exe ^
   -Fo:out\shader_tests\wave\ ^
   %SRC% tests\shader\sc_wave_main.cpp
if errorlevel 1 (
    echo BUILD FAILED wave-build
    popd
    exit /b 1
)

echo.
echo === running sc_wave_test.exe ===
out\shader_tests\sc_wave_test.exe out\shader_tests\wave
set "R=%ERRORLEVEL%"

echo.
echo wave test exit=%R%
if not "%R%"=="0" ( popd & exit /b 1 )
popd
exit /b 0
