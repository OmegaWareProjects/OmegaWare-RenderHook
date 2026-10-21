@echo off
setlocal
cd /d "%~dp0"
set GLSLANG=%VULKAN_SDK%\Bin\glslangValidator.exe
"%GLSLANG%" -V --vn g_aDepthResolveVertexSPIRV -o DepthResolveVertex.hpp DepthResolve.vert || exit /b 1
"%GLSLANG%" -V --vn g_aDepthResolveFragmentSPIRV -o DepthResolveFragment.hpp DepthResolve.frag || exit /b 1
