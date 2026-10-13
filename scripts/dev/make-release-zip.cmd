@echo off
rem RGO (developers only): build the release ZIP exactly as players get it,
rem from the commit checked out here. Double-click it. The ZIP lands in
rem build\launcher and Explorer opens on it.
setlocal EnableExtensions
title RGO release ZIP
cd /d "%~dp0..\.."
if not exist ".git" (echo This script must stay in scripts\dev inside your RGO-Recomp folder.& goto :fail)

for /f %%s in ('git status --porcelain --untracked-files^=no') do (echo You have local changes to project files. Send this list to Claude:& git status --short --untracked-files=no& goto :fail)
git fetch origin || goto :fail
for /f %%c in ('git rev-parse HEAD') do set "COMMIT=%%c"
rem The launcher downloads this exact commit from GitHub, so it must be pushed.
git branch -r --contains %COMMIT% | findstr /r "." >nul
if errorlevel 1 (echo Commit %COMMIT% is not on GitHub. Push it first, or pull the latest branch.& goto :fail)
git log -1 --format="Packaging commit %%h: %%s"

if exist "build\launcher\RGO-Launcher-*.zip" del /q "build\launcher\RGO-Launcher-*.zip"
if exist "build\launcher\SHA256SUMS.txt" del /q "build\launcher\SHA256SUMS.txt"
if exist "build\launcher\package-inventory.json" del /q "build\launcher\package-inventory.json"

powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build_launcher.ps1 -Test
if errorlevel 1 goto :fail
python scripts\package_launcher.py
if errorlevel 1 goto :fail

set "ZIP="
for %%z in ("build\launcher\RGO-Launcher-*.zip") do set "ZIP=%%~fz"
if not defined ZIP (echo The ZIP was not created.& goto :fail)
echo.
echo Release ZIP: %ZIP%
echo Extract it to a short, empty folder such as D:\RGO-Test and run RGO-Launcher.exe.
explorer /select,"%ZIP%"
pause
exit /b 0

:fail
echo Something went wrong. Copy the text above and send it to Claude.
pause
exit /b 1
