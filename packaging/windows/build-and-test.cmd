@echo off
setlocal EnableExtensions

rem Native MSVC build, CTest, staged install, and NSIS package for MYCOM Viewer.
rem Optional overrides:
rem   set VSDEVCMD=C:\...\VsDevCmd.bat
rem   set QT_ROOT=C:\Qt\5.15.2\msvc2019_64

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%\..\..") do set "PROJECT_DIR=%%~fI"
if not defined VSDEVCMD set "VSDEVCMD=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat"
if not defined QT_ROOT set "QT_ROOT=C:\Qt\5.15.2\msvc2019_64"

if not exist "%VSDEVCMD%" (
    echo Visual Studio developer command file was not found: "%VSDEVCMD%" 1>&2
    exit /b 1
)
if not exist "%QT_ROOT%\lib\cmake\Qt5\Qt5Config.cmake" (
    echo Qt5 CMake package was not found below: "%QT_ROOT%" 1>&2
    exit /b 1
)

call "%VSDEVCMD%" -arch=amd64 -host_arch=amd64
if errorlevel 1 exit /b %errorlevel%

rem CTest executes Qt-linked test binaries directly from the build tree.
rem Keep the selected kit's DLL directory on PATH until staged deployment.
set "PATH=%QT_ROOT%\bin;%PATH%"

set "BUILD_DIR=%PROJECT_DIR%\build-windows"
set "STAGE_DIR=%PROJECT_DIR%\stage-windows"

cmake -S "%PROJECT_DIR%" -B "%BUILD_DIR%" -A x64 -DCMAKE_PREFIX_PATH="%QT_ROOT%"
if errorlevel 1 exit /b %errorlevel%
cmake --build "%BUILD_DIR%" --config Release --parallel
if errorlevel 1 exit /b %errorlevel%
ctest --test-dir "%BUILD_DIR%" -C Release --output-on-failure
if errorlevel 1 exit /b %errorlevel%
cmake --install "%BUILD_DIR%" --config Release --prefix "%STAGE_DIR%"
if errorlevel 1 exit /b %errorlevel%
pushd "%BUILD_DIR%"
cpack --config CPackConfig.cmake -C Release -G NSIS
set "CPACK_RESULT=%ERRORLEVEL%"
popd
if not "%CPACK_RESULT%"=="0" exit /b %CPACK_RESULT%

echo Windows build, test, staged install, and NSIS package completed.
