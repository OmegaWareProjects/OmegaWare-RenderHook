@echo off
setlocal
cd /d "%~dp0"
set MODE=%~1
set ARCH=%~2
if "%MODE%"=="" set MODE=debug
if "%ARCH%"=="" set ARCH=x64
set FOLDER=Debug
if /i "%MODE%"=="release" set FOLDER=Release
call "%~dp0Build.bat" %MODE% %ARCH% || exit /b 1
"Build\%FOLDER%\%ARCH%\OmegaRenderHook-SelfTest.exe" %3
