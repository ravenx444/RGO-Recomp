@echo off
rem RGO rebuild: update a branch, build it and play it.
rem Double-click it, or run it from a terminal with extra build options, e.g.
rem   rgo-rebuild.cmd --clean        rebuild everything from zero
rem   rgo-rebuild.cmd --regenerate   generate the game code from the ROM again
rem   rgo-rebuild.cmd --no-frontend  play in the bare game window
rem See docs\rgo\build.md.

rem "git checkout" below can change or delete this very file while cmd.exe is
rem still reading it, so run from a copy in %TEMP% instead.
if "%RGO_REBUILD_STAGE%"=="copy" goto :start
set "RGO_REBUILD_STAGE=copy"
set "RGO_REBUILD_REPO=%~dp0"
set "RGO_REBUILD_COPY=%TEMP%\rgo-rebuild-%RANDOM%%RANDOM%.cmd"
copy /y "%~f0" "%RGO_REBUILD_COPY%" >nul
if errorlevel 1 (set "RGO_REBUILD_STAGE="& echo Could not copy this script to %TEMP%.& pause& exit /b 1)
"%RGO_REBUILD_COPY%" %*

:start
set "RGO_REBUILD_STAGE="
set "RGO_REPO=%RGO_REBUILD_REPO%"
set "RGO_REBUILD_REPO="
set "RGO_REBUILD_COPY="
setlocal EnableExtensions
title RGO rebuild
cd /d "%RGO_REPO%"

rem ---- Settings you may edit -------------------------------------------------
rem Branch built when you just press Enter.
set "DEFAULT_BRANCH=rgo/next"
rem ROM used the first time if it exists. After that the ROM path is remembered
rem in %LOCALAPPDATA%\RGO\rebuild-rom.txt (delete that file to pick another ROM),
rem or set the RGO_ROM environment variable to override it.
set "DEFAULT_ROM=D:\GameDev\Modding\rgo-source\baserom.us.z64"
rem ----------------------------------------------------------------------------

set "ROM_SETTING=%LOCALAPPDATA%\RGO\rebuild-rom.txt"
if not exist ".git" (echo This script must be inside your RGO-Recomp folder, next to the .git folder.& goto :fail)

rem ROM paths may contain spaces, parentheses or "&", so keep them in quotes.
set "ROM="
if defined RGO_ROM set "ROM=%RGO_ROM%"
if not defined ROM if exist "%ROM_SETTING%" set /p "ROM=" <"%ROM_SETTING%"
if defined ROM set "ROM=%ROM:"=%"
if not defined ROM goto :defaultrom
if exist "%ROM%" goto :haverom
echo The remembered ROM was not found: "%ROM%"
set "ROM="
:defaultrom
if exist "%DEFAULT_ROM%" set "ROM=%DEFAULT_ROM%"
:askrom
if defined ROM goto :haverom
echo Where is your Jet Force Gemini ROM (US .z64)? You can drag the file into this window.
set /p "ROM=ROM path: "
if not defined ROM goto :askrom
set "ROM=%ROM:"=%"
if exist "%ROM%" goto :haverom
echo Not found: "%ROM%"
set "ROM="
goto :askrom
:haverom
if defined RGO_ROM goto :romsaved
if not exist "%LOCALAPPDATA%\RGO" mkdir "%LOCALAPPDATA%\RGO"
>"%ROM_SETTING%" echo "%ROM%"
:romsaved
echo ROM: "%ROM%"

:checklauncher
tasklist /FI "IMAGENAME eq JFG-Launcher.exe" 2>nul | find /I "JFG-Launcher.exe" >nul
if not errorlevel 1 (echo The JFG launcher is still open. Close it, then press any key to continue.& pause >nul& goto :checklauncher)
tasklist /FI "IMAGENAME eq jfg-native-boot.exe" 2>nul | find /I "jfg-native-boot.exe" >nul
if not errorlevel 1 (echo The game is still running. Close it, then press any key to continue.& pause >nul& goto :checklauncher)

for /f "delims=" %%b in ('git rev-parse --abbrev-ref HEAD') do set "CURRENT=%%b"
echo Current branch: %CURRENT%
set "BRANCH="
set /p "BRANCH=Branch to build [press Enter for %DEFAULT_BRANCH%]: "
if "%BRANCH%"=="" set "BRANCH=%DEFAULT_BRANCH%"
for /f %%s in ('git status --porcelain --untracked-files^=no') do (echo You have local changes to project files. Send this list to Claude:& git status --short --untracked-files=no& goto :fail)
git fetch origin || goto :fail
git checkout "%BRANCH%" || goto :fail
git pull --ff-only origin "%BRANCH%" || goto :fail
git log -1 --format="Building commit %%h: %%s"
python scripts\build_from_rom.py --rom "%ROM%" --play %*
if errorlevel 1 goto :fail
set "RESULT=0"
goto :end

:fail
echo Something went wrong. Copy the text above and send it to Claude.
pause
set "RESULT=1"

:end
rem Delete this temporary copy and return the result.
(goto) 2>nul & del "%~f0" & cmd /c exit %RESULT%
