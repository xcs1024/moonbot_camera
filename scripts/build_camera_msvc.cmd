@echo off
setlocal EnableExtensions

rem Normalize duplicated PATH/Path keys before running MSBuild.
rem Some shells inject both names, and MSBuild can fail when starting CL.exe.
set "KEEP_PATH=%PATH%"
set "PATH="
set "Path=%KEEP_PATH%"

set "ROOT=%~dp0.."
set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

if not exist "%VCVARS%" (
    echo [error] Cannot find vcvars64.bat:
    echo         "%VCVARS%"
    echo [hint] Install Visual Studio 2022 Build Tools with "Desktop development with C++".
    exit /b 1
)

call "%VCVARS%"
if errorlevel 1 exit /b 1

pushd "%ROOT%"

if "%~1"=="" (
    cmake -S src\camera -B build\camera_msvc -A x64
) else (
    cmake -S src\camera -B build\camera_msvc -A x64 -DOpenCV_DIR="%~1"
)
if errorlevel 1 (
    popd
    exit /b 1
)

cmake --build build\camera_msvc --config Release
set "BUILD_EXIT=%ERRORLEVEL%"

popd
exit /b %BUILD_EXIT%
