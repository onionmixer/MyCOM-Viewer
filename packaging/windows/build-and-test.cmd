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
if not defined MAKENSIS set "MAKENSIS=%ProgramFiles(x86)%\NSIS\makensis.exe"
if not exist "%MAKENSIS%" (
    where makensis >nul 2>nul
    if not errorlevel 1 set "MAKENSIS=makensis"
)
if not exist "%MAKENSIS%" if /I not "%MAKENSIS%"=="makensis" (
    echo NSIS makensis was not found. Set MAKENSIS to makensis.exe. 1>&2
    exit /b 1
)
if not exist "%BUILD_DIR%\packages" mkdir "%BUILD_DIR%\packages"
"%MAKENSIS%" /DSTAGE_DIR="%STAGE_DIR%" /DOUTFILE="%BUILD_DIR%\packages\mycom-viewer-0.7.1-win64.exe" "%PROJECT_DIR%\packaging\windows\installer.nsi"
if errorlevel 1 exit /b %errorlevel%

echo Windows build, test, staged install, and NSIS package completed.
