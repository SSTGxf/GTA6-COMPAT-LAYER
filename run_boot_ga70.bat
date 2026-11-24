@echo off
cd /d F:\ScarlettCompat\out
"F:\ScarlettCompat\out\scarlett_verify.exe" "D:\game\game\game_scarlett_master.exe" 300000 > F:\ScarlettCompat\gta_boot70.log 2>&1
echo BOOT_EXIT=%ERRORLEVEL% >> F:\ScarlettCompat\gta_boot70.log
