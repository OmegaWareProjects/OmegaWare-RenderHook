@echo off
setlocal
cd /d "%~dp0"
set TARGET=%~1
set MODE=%~2
set ARCH=%~3
if "%TARGET%"=="" (
	echo usage: Inject.bat ^<process name ^| pid^> [debug^|release] [x64^|x86]
	exit /b 1
)
if "%MODE%"=="" set MODE=release
if "%ARCH%"=="" set ARCH=x64
set FOLDER=Debug
if /i "%MODE%"=="release" set FOLDER=Release
set OUT=Build\%FOLDER%\%ARCH%
if not exist "%OUT%\OmegaRenderHook-TestOverlay.dll" call "%~dp0Build.bat" %MODE% %ARCH% || exit /b 1
"%OUT%\OmegaRenderHook-Injector.exe" "%TARGET%" "%OUT%\OmegaRenderHook-TestOverlay.dll" || exit /b 1
echo Log: %OUT%\OmegaRenderHook-TestOverlay.log
