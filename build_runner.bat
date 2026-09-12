@echo off
REM Build script for PS2 Recomp Runner
REM Compiles urbz.recomp.cpp + runner.cpp against the runtime library

setlocal

set MSVC_VER=14.51.36231
set VS_PATH=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC\%MSVC_VER%\bin\HostX64\x64
set MSVC_INC=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC\%MSVC_VER%\Include
set MSVC_LIB=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC\%MSVC_VER%\Lib\x64
set CMAKE_DIR=E:\PS2-Recomp-Tool\build
set SRC_DIR=E:\PS2-Recomp-Tool

REM Pick the newest Windows 10 SDK present (parens in the path break for /f,
REM so probe versions with if-exist instead).
set SDK_BASE=C:\Program Files (x86)\Windows Kits\10\Include
set SDK_INC=
if exist "%SDK_BASE%\10.0.26100.0\ucrt" set SDK_INC=%SDK_BASE%\10.0.26100.0
if "%SDK_INC%"=="" if exist "%SDK_BASE%\10.0.22621.0\ucrt" set SDK_INC=%SDK_BASE%\10.0.22621.0
if "%SDK_INC%"=="" if exist "%SDK_BASE%\10.0.19041.0\ucrt" set SDK_INC=%SDK_BASE%\10.0.19041.0
if "%SDK_INC%"=="" (
    echo ERROR: Windows 10 SDK not found
    exit /b 1
)
set SDK_LIB=%SDK_INC:Include\=Lib\%
echo Using Windows SDK: %SDK_INC%

echo === PS2 Recomp Runner Build Script ===
echo.

REM Set up include paths and libraries
set INCLUDE=%SRC_DIR%\runtime\include;%SRC_DIR%\libs\ee-base\include;%SRC_DIR%\libs\elf\include;%SRC_DIR%\libs\r5900\include;%SRC_DIR%\libs\analysis\include;%SRC_DIR%\libs\codegen\include;%MSVC_INC%;%SDK_INC%\ucrt;%SDK_INC%\shared;%SDK_INC%\um;%SDK_INC%\winrt
set LIB=%CMAKE_DIR%\runtime\Release;%CMAKE_DIR%\libs\elf\Release;%CMAKE_DIR%\libs\ee-base\Release;%CMAKE_DIR%\libs\r5900\Release;%CMAKE_DIR%\libs\analysis\Release;%CMAKE_DIR%\libs\codegen\Release;%MSVC_LIB%;%SDK_LIB%\ucrt\x64;%SDK_LIB%\um\x64

REM Compile the generated recomp file
echo [1/3] Compiling urbz.recomp.cpp...
"%VS_PATH%\cl.exe" /std:c++20 /O2 /MD /EHsc /c /I"%SRC_DIR%\runtime\include" /I"%SRC_DIR%\libs\ee-base\include" /I"%SRC_DIR%\libs\elf\include" /I"%SRC_DIR%\libs\r5900\include" /I"%SRC_DIR%\libs\analysis\include" /I"%SRC_DIR%\libs\codegen\include" ^
    /D"CMAKE_INTDIR=\"Release\"" ^
    "%SRC_DIR%\urbz.recomp.cpp" ^
    /Fo"%SRC_DIR%\recomp_obj.obj"

if errorlevel 1 (
    echo ERROR: Failed to compile urbz.recomp.cpp
    exit /b 1
)
echo     -^> recomp_obj.obj created

REM Compile the runner
echo.
echo [2/3] Compiling runner.cpp...
"%VS_PATH%\cl.exe" /std:c++20 /O2 /MD /EHsc /c /I"%SRC_DIR%\runtime\include" /I"%SRC_DIR%\libs\ee-base\include" /I"%SRC_DIR%\libs\elf\include" /I"%SRC_DIR%\libs\r5900\include" /I"%SRC_DIR%\libs\analysis\include" /I"%SRC_DIR%\libs\codegen\include" ^
    /D"CMAKE_INTDIR=\"Release\"" ^
    "%SRC_DIR%\runner.cpp" ^
    /Fo"%SRC_DIR%\runner_obj.obj"

if errorlevel 1 (
    echo ERROR: Failed to compile runner.cpp
    exit /b 1
)
echo     -^> runner_obj.obj created

REM Link everything together
echo.
echo [3/3] Linking ee-runner.exe...
"%VS_PATH%\link.exe" ^
    "%SRC_DIR%\recomp_obj.obj" ^
    "%SRC_DIR%\runner_obj.obj" ^
    /LIBPATH:"%CMAKE_DIR%\runtime\Release" ^
    /LIBPATH:"%CMAKE_DIR%\libs\elf\Release" ^
    /LIBPATH:"%CMAKE_DIR%\libs\ee-base\Release" ^
    /LIBPATH:"%CMAKE_DIR%\libs\r5900\Release" ^
    /LIBPATH:"%CMAKE_DIR%\libs\analysis\Release" ^
    /LIBPATH:"%CMAKE_DIR%\libs\codegen\Release" ^
    ee_runtime.lib ee_elf.lib ^
    /OUT:"%SRC_DIR%\ee-runner.exe" ^
    /INCREMENTAL:NO ^
    /SUBSYSTEM:CONSOLE ^
    /MACHINE:X64

if errorlevel 1 (
    echo ERROR: Failed to link ee-runner.exe
    exit /b 1
)

echo.
echo === Build complete! ===
echo Run with: %SRC_DIR%\ee-runner.exe E:\Aura\PS2 Recomp Research\game\SLUS-21066.elf
echo.
