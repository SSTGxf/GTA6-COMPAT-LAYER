@echo off
REM ScarlettCompat build — Visual Studio 2022 BuildTools + Windows SDK 10.0.26100.0
REM Builds: layer DLL, shim import libs, synthetic guest title, verify harness,
REM then runs the harness against the guest. No game code is built here and
REM nothing in this script touches D:\game\game.
REM RUN THIS IN AN INTERACTIVE SESSION (session 1). The scheduled task
REM ScarlettPhase4Build is created with schtasks /it for exactly this reason.
REM In session 0 (SYSTEM, non-interactive) the guests crash after the TLS
REM stage: EXCEPTION_ACCESS_VIOLATION in gdi32full.dll+0x8984a, reading
REM address 0x20 — a NULL+0x20 dereference on a thread whose stack holds
REM the AMD driver (amdxc64), RTSSHooks64, combase/imm32. The layer never
REM calls GDI; it is the session-0 window station + graphics stack, not a
REM layer bug. Proven: the same binaries pass ALL PASS in session 1 and in
REM a foreground SSH run, and crash 100% in session 0. (Phase 4, 2026-11-23.)
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 (echo vcvarsall failed & exit /b 1)

set ROOT=%~dp0
set OUT=%ROOT%out
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OUT%\lib" mkdir "%OUT%\lib"

echo === 1. ScarlettCompat.dll (layer) ===
cl /nologo /EHsc /MD /O2 /W3 /I"%ROOT%include" "%ROOT%src\scarlett.cpp" "%ROOT%src\pe_loader.cpp" "%ROOT%src\hle_registry.cpp" /LD /Fe"%OUT%\ScarlettCompat.dll" /Fo%OUT%\ /Fd%OUT%\ScarlettCompat.pdb /link d3d12.lib winmm.lib
if errorlevel 1 exit /b 1

echo === 2. Shim import libs (Xbox DLL names mapped to layer exports) ===
for %%D in (xgameruntime xmem d3d12_xs XAudio2_9 dstorage_xs gameinput) do (
  lib /nologo /DEF:"%ROOT%defs\%%D.def" /OUT:"%OUT%\lib\%%D.lib" || exit /b 1
)
REM Ordinal import libs (TEST ONLY — synthetic ordinals for the ordinal guest).
for %%D in (xgameruntime_ord xmem_ord) do (
  lib /nologo /DEF:"%ROOT%defs\%%D.def" /OUT:"%OUT%\lib\%%D.lib" || exit /b 1
)

echo === 3. Synthetic guest title (Xbox-style PE) ===
cl /nologo /EHsc /MD /O2 /W3 /I"%ROOT%include" "%ROOT%tests\guest_minimal.cpp" /Fe"%OUT%\guest_minimal.exe" /Fo%OUT%\ /Fd%OUT%\guest_minimal.pdb /link /OPT:NOREF /BASE:0x180000000 /LIBPATH:"%OUT%\lib" xgameruntime.lib xmem.lib d3d12_xs.lib XAudio2_9.lib dstorage_xs.lib gameinput.lib d3d12.lib dxgi.lib
if errorlevel 1 exit /b 1
if not exist "%OUT%\ordinal" mkdir "%OUT%\ordinal"
cl /nologo /EHsc /MD /O2 /W3 /I"%ROOT%include" "%ROOT%tests\guest_ordinal.cpp" /Fe"%OUT%\ordinal\guest_ordinal.exe" /Fo%OUT%\ /Fd%OUT%\guest_ordinal.pdb /link /OPT:NOREF /BASE:0x180000000 /LIBPATH:"%OUT%\lib" xgameruntime_ord.lib xmem_ord.lib
if errorlevel 1 exit /b 1
REM Phase 4: realistic proof guest (full boot + triangle + 60-frame loop).
cl /nologo /EHsc /MD /O2 /W3 /I"%ROOT%include" "%ROOT%tests\guest_proof.cpp" /Fe"%OUT%\guest_proof.exe" /Fo%OUT%\ /Fd%OUT%\guest_proof.pdb /link /OPT:NOREF /BASE:0x180000000 /LIBPATH:"%OUT%\lib" xgameruntime.lib xmem.lib d3d12_xs.lib XAudio2_9.lib dstorage_xs.lib gameinput.lib d3d12.lib dxgi.lib d3dcompiler.lib
if errorlevel 1 exit /b 1

echo === 4. Verification harness ===
cl /nologo /EHsc /MD /O2 /W3 /I"%ROOT%include" "%ROOT%src\verify.cpp" /Fe"%OUT%\scarlett_verify.exe" /Fo%OUT%\ /Fd%OUT%\scarlett_verify.pdb /link "%OUT%\ScarlettCompat.lib"
if errorlevel 1 exit /b 1

echo === 5. Verify: guests through the layer ===
copy /Y "%OUT%\ScarlettCompat.dll" "%OUT%\ScarlettCompat.dll" >nul
cd /d "%OUT%"
"%OUT%\scarlett_verify.exe" "%OUT%\guest_minimal.exe" 60000
set RC1=%ERRORLEVEL%
"%OUT%\scarlett_verify.exe" "%OUT%\ordinal\guest_ordinal.exe" 60000
set RC2=%ERRORLEVEL%
"%OUT%\scarlett_verify.exe" "%OUT%\guest_proof.exe" 120000
set RC3=%ERRORLEVEL%
cd /d "%ROOT%"
if %RC1% neq 0 (echo guest_minimal FAILED & exit /b 1)
if %RC2% neq 0 (echo guest_ordinal FAILED & exit /b 1)
if %RC3% neq 0 (echo guest_proof FAILED & exit /b 1)
echo === verify exit code: 0 ===
exit /b 0
