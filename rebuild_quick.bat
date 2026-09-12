@echo off
REM Rebuild runner.cpp + ee_runtime, relink (reuses recomp_obj.obj).
cd /d E:\PS2-Recomp-Tool
cmake --build build --config Release --target ee_runtime > rt.txt 2>&1 || (echo RUNTIME-FAILED & exit /b 1)
set MSVC_VER=14.51.36231
set VS_PATH=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC\%MSVC_VER%\bin\HostX64\x64
set MSVC_INC=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC\%MSVC_VER%\Include
set MSVC_LIB=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC\%MSVC_VER%\Lib\x64
set SDK_BASE=C:\Program Files (x86)\Windows Kits\10\Include
set SDK_INC=
if exist "%SDK_BASE%\10.0.26100.0\ucrt" set SDK_INC=%SDK_BASE%\10.0.26100.0
if "%SDK_INC%"=="" if exist "%SDK_BASE%\10.0.22621.0\ucrt" set SDK_INC=%SDK_BASE%\10.0.22621.0
if "%SDK_INC%"=="" if exist "%SDK_BASE%\10.0.19041.0\ucrt" set SDK_INC=%SDK_BASE%\10.0.19041.0
set SDK_LIB=%SDK_INC:Include\=Lib\%
set CMAKE_DIR=E:\PS2-Recomp-Tool\build
set SRC_DIR=E:\PS2-Recomp-Tool
set INCLUDE=%SRC_DIR%\runtime\include;%SRC_DIR%\libs\ee-base\include;%SRC_DIR%\libs\elf\include;%SRC_DIR%\libs\r5900\include;%SRC_DIR%\libs\analysis\include;%SRC_DIR%\libs\codegen\include;%MSVC_INC%;%SDK_INC%\ucrt;%SDK_INC%\shared;%SDK_INC%\um;%SDK_INC%\winrt
set LIB=%CMAKE_DIR%\runtime\Release;%CMAKE_DIR%\libs\elf\Release;%CMAKE_DIR%\libs\ee-base\Release;%CMAKE_DIR%\libs\r5900\Release;%CMAKE_DIR%\libs\analysis\Release;%CMAKE_DIR%\libs\codegen\Release;%MSVC_LIB%;%SDK_LIB%\ucrt\x64;%SDK_LIB%\um\x64
"%VS_PATH%\cl.exe" /std:c++20 /O2 /MD /EHsc /c /I"%SRC_DIR%\runtime\include" /I"%SRC_DIR%\libs\ee-base\include" /I"%SRC_DIR%\libs\elf\include" /I"%SRC_DIR%\libs\r5900\include" /I"%SRC_DIR%\libs\analysis\include" /I"%SRC_DIR%\libs\codegen\include" "%SRC_DIR%\runner.cpp" /Fo"%SRC_DIR%\runner_obj.obj"
if errorlevel 1 (echo RUNNER-FAILED & exit /b 1)
call link_runner.bat
