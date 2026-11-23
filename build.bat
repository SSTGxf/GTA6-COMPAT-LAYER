@echo off
REM ScarlettCompat build — Visual Studio 2022 BuildTools + Windows SDK 10.0.26100.0
REM Builds: layer DLL, shim import libs, synthetic guest title, verify harness,
REM then runs the harness against the guest. No game code is built here and
REM nothing in this script touches D:\game\game.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 (echo vcvarsall failed & exit /b 1)

set ROOT=%~dp0
set OUT=%ROOT%out
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OUT%\lib" mkdir "%OUT%\lib"

echo === 1. ScarlettCompat.dll (layer) ===
cl /nologo /EHsc /MD /O2 /W3 /I"%ROOT%include" "%ROOT%src\scarlett.cpp" "%ROOT%src\pe_loader.cpp" "%ROOT%src\hle_registry.cpp" /LD /Fe"%OUT%\ScarlettCompat.dll" /Fo%OUT%\ /Fd%OUT%\ScarlettCompat.pdb
if errorlevel 1 exit /b 1

echo === 2. Shim import libs (Xbox DLL names mapped to layer exports) ===
for %%D in (xgameruntime xmem d3d12_xs XAudio2_9 dstorage_xs gameinput) do (
  lib /nologo /DEF:"%ROOT%defs\%%D.def" /OUT:"%OUT%\lib\%%D.lib" || exit /b 1
)

echo === 3. Synthetic guest title (Xbox-style PE) ===
cl /nologo /EHsc /MD /O2 /W3 /I"%ROOT%include" "%ROOT%tests\guest_minimal.cpp" /Fe"%OUT%\guest_minimal.exe" /Fo%OUT%\ /Fd%OUT%\guest_minimal.pdb /link /BASE:0x180000000 /LIBPATH:"%OUT%\lib" xgameruntime.lib xmem.lib d3d12_xs.lib XAudio2_9.lib dstorage_xs.lib gameinput.lib d3d12.lib
if errorlevel 1 exit /b 1

echo === 4. Verification harness ===
cl /nologo /EHsc /MD /O2 /W3 /I"%ROOT%include" "%ROOT%src\verify.cpp" /Fe"%OUT%\scarlett_verify.exe" /Fo%OUT%\ /Fd%OUT%\scarlett_verify.pdb /link "%OUT%\ScarlettCompat.lib"
if errorlevel 1 exit /b 1

echo === 5. Verify: guest through the layer ===
copy /Y "%OUT%\ScarlettCompat.dll" "%OUT%\ScarlettCompat.dll" >nul
cd /d "%OUT%"
"%OUT%\scarlett_verify.exe" "%OUT%\guest_minimal.exe" 60000
set RC=%ERRORLEVEL%
cd /d "%ROOT%"
echo === verify exit code: %RC% ===
exit /b %RC%
