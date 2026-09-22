@echo off
REM build_test.bat - Build the vijGPU user-mode test application
REM
REM Requirements:
REM   - Visual Studio 2022 (or Build Tools)
REM   - Windows SDK (for setupapi.lib)
REM
REM Run this inside VM 300 or on any Windows x64 machine with MSVC.

echo Building vijgpu_test.exe...

REM Try to find MSVC via vswhere
for /f "tokens=*" %%i in ('"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath 2^>nul') do set VS_PATH=%%i

if defined VS_PATH (
    call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
) else (
    echo WARNING: vswhere not found, assuming MSVC is already in PATH
)

cl /W4 /WX /nologo ^
   vijgpu_test.c ^
   /link ^
   setupapi.lib ^
   /out:vijgpu_test.exe

if %ERRORLEVEL% == 0 (
    echo Build succeeded: vijgpu_test.exe
) else (
    echo Build FAILED
    exit /b 1
)
