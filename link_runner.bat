@echo off
REM Relink ee-runner.exe only (reuses existing recomp_obj.obj / runner_obj.obj).
setlocal
set MSVC_VER=14.51.36231
set VS_PATH=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC\%MSVC_VER%\bin\HostX64\x64
set MSVC_LIB=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC\%MSVC_VER%\Lib\x64
set CMAKE_DIR=E:\PS2-Recomp-Tool\build
set SRC_DIR=E:\PS2-Recomp-Tool
set SDK_BASE=C:\Program Files (x86)\Windows Kits\10\Include
set SDK_INC=
if exist "%SDK_BASE%\10.0.26100.0\ucrt" set SDK_INC=%SDK_BASE%\10.0.26100.0
if "%SDK_INC%"=="" if exist "%SDK_BASE%\10.0.22621.0\ucrt" set SDK_INC=%SDK_BASE%\10.0.22621.0
if "%SDK_INC%"=="" if exist "%SDK_BASE%\10.0.19041.0\ucrt" set SDK_INC=%SDK_BASE%\10.0.19041.0
set SDK_LIB=%SDK_INC:Include\=Lib\%
set LIB=%CMAKE_DIR%\runtime\Release;%CMAKE_DIR%\libs\elf\Release;%CMAKE_DIR%\libs\ee-base\Release;%CMAKE_DIR%\libs\r5900\Release;%CMAKE_DIR%\libs\analysis\Release;%CMAKE_DIR%\libs\codegen\Release;%MSVC_LIB%;%SDK_LIB%\ucrt\x64;%SDK_LIB%\um\x64

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
    /MAP:"%SRC_DIR%\ee-runner.map" ^
    /INCREMENTAL:NO ^
    /SUBSYSTEM:CONSOLE ^
    /MACHINE:X64
if errorlevel 1 (echo RELINK-FAILED) else echo RELINK-OK
