@echo off
rem ScarlettCompat shader pipeline - link-contract check.
rem
rem Proves the one cross-object dependency the layer has to satisfy, both ways:
rem the no-DXC source set alone must fail to link with LNK2019
rem sc_make_dxil_emitter, and adding src\shader\sc_dxil_emit.cpp must link and
rem run. Uses the same MSVC/SDK discovery and flags as build_all_no_dxc.bat.
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
for /f "delims=" %%v in ('dir /b /ad /o-n "%SDKROOT%\Lib"') do (
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

if not exist "out\shader_tests\link_smoke" mkdir "out\shader_tests\link_smoke"
if not exist "out\shader_tests\link_smoke\nodxc" mkdir "out\shader_tests\link_smoke\nodxc"
if not exist "out\shader_tests\link_smoke\emit" mkdir "out\shader_tests\link_smoke\emit"
if exist "out\shader_tests\link_smoke\nodxc\*.obj" del /q "out\shader_tests\link_smoke\nodxc\*.obj"
if exist "out\shader_tests\link_smoke\emit\*.obj" del /q "out\shader_tests\link_smoke\emit\*.obj"
if exist "out\shader_tests\link_smoke\link_no_emit.txt" del /q "out\shader_tests\link_smoke\link_no_emit.txt"
if exist "out\shader_tests\link_smoke\link_full.exe" del /q "out\shader_tests\link_smoke\link_full.exe"

rem The no-DXC set, exactly as build_all_no_dxc.bat archives it.
set "NODXC=src\shader\sc_sha256.cpp src\shader\sc_container.cpp src\shader\sc_xs2.cpp src\shader\sc_frontend.cpp src\shader\sc_isa.cpp src\shader\sc_wave.cpp src\shader\sc_capture.cpp src\shader\sc_dxil_backend.cpp src\shader\sc_gpu_render_test.cpp src\shader\sc_mesh_compute.cpp src\shader\sc_pipeline_binding.cpp src\shader\sc_bc_decode.cpp"

echo Using MSVC: %MSVC%
echo Using SDK : %SDKVER%

echo.
echo [1/4] compile the no-DXC set plus tests\shader\link_smoke_main.cpp at /W4 /WX
cl -nologo -std:c++17 -EHsc -W4 -WX -DSC_FRONTEND_NO_DXC -Iinclude -Ithird_party -c ^
   -Fo:out\shader_tests\link_smoke\nodxc\ %NODXC% tests\shader\link_smoke_main.cpp
if errorlevel 1 (
    echo LINK SMOKE FAILED: the no-DXC set or the smoke main did not compile cleanly
    popd
    exit /b 1
)

echo.
echo [2/4] compile src\shader\sc_dxil_emit.cpp at /W4 /WX (real configuration)
cl -nologo -std:c++17 -EHsc -W4 -WX -Iinclude -Ithird_party -c ^
   -Fo:out\shader_tests\link_smoke\emit\ src\shader\sc_dxil_emit.cpp
if errorlevel 1 (
    echo LINK SMOKE FAILED: sc_dxil_emit.cpp did not compile cleanly
    popd
    exit /b 1
)

echo.
echo [3/4] negative link: the no-DXC set alone must not link
link -nologo -OUT:out\shader_tests\link_smoke\link_no_emit.exe ^
   out\shader_tests\link_smoke\nodxc\*.obj > out\shader_tests\link_smoke\link_no_emit.txt 2>&1
if not errorlevel 1 (
    echo LINK CONTRACT FAILED: the no-DXC set linked without sc_dxil_emit.cpp
    echo expected an unresolved sc_make_dxil_emitter; the doc contract would be wrong
    popd
    exit /b 1
)
findstr /c:"LNK2019" /c:"sc_make_dxil_emitter" out\shader_tests\link_smoke\link_no_emit.txt > out\shader_tests\link_smoke\lnk2019.txt
if errorlevel 1 (
    echo LINK CONTRACT FAILED: the link failed, but not on sc_make_dxil_emitter
    type out\shader_tests\link_smoke\link_no_emit.txt
    popd
    exit /b 1
)
echo    expected failure:
type out\shader_tests\link_smoke\lnk2019.txt

echo.
echo [4/4] positive link: add sc_dxil_emit.obj, link and run
link -nologo -OUT:out\shader_tests\link_smoke\link_full.exe ^
   out\shader_tests\link_smoke\nodxc\*.obj out\shader_tests\link_smoke\emit\sc_dxil_emit.obj
if errorlevel 1 (
    echo LINK SMOKE FAILED: still unresolved with sc_dxil_emit.obj added
    popd
    exit /b 1
)
out\shader_tests\link_smoke\link_full.exe
if errorlevel 1 (
    echo LINK SMOKE FAILED: the linked binary did not decode the FLAT row
    popd
    exit /b 1
)

echo.
echo link contract verified: LNK2019 for sc_make_dxil_emitter without
echo sc_dxil_emit.obj, clean link and correct decode with it (/W4 /WX, C++17)
popd
exit /b 0
