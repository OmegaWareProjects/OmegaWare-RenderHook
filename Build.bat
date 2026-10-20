@echo off
setlocal
cd /d "%~dp0"
set MODE=%~1
set ARCH=%~2
if "%MODE%"=="" set MODE=release
if "%ARCH%"=="" set ARCH=x64
xmake config -p windows -a %ARCH% -m %MODE% -y || exit /b 1
xmake build || exit /b 1
echo Built %MODE% %ARCH% into Build\%MODE%\%ARCH%
