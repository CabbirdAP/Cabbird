@echo off
setlocal EnableExtensions
cd /d "%~dp0"

rem Build the canonical preset. Optional modes: fixtures, package.
set "CMAKE=cmake"
set "BUILD_FIXTURES=OFF"
set "STAGE_ALL=OFF"
:arguments
if "%~1"=="" goto :find_cmake
if /i "%~1"=="fixtures" set "BUILD_FIXTURES=ON"
if /i "%~1"=="package" set "STAGE_ALL=ON"
if /i not "%~1"=="fixtures" if /i not "%~1"=="package" goto :usage
shift
goto :arguments

:find_cmake
where cmake >nul 2>nul
if not errorlevel 1 goto :run
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :missing_cmake
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT goto :missing_cmake
set "CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "%CMAKE%" goto :missing_cmake

:run
set "BUILD_DIR=%~dp0.build\windows-vs2022"
set "PACKAGE_DIR=%BUILD_DIR%\game-package"
"%CMAKE%" --preset windows-vs2022 "-DCABBIRD_BUILD_TEST_FIXTURES=%BUILD_FIXTURES%"
if errorlevel 1 exit /b %errorlevel%
rem MSBuild 17.14 fails before useful output when --parallel is passed here.
"%CMAKE%" --build --preset windows-relwithdebinfo
if errorlevel 1 exit /b %errorlevel%
echo Installing GameRuntime to "%PACKAGE_DIR%"
"%CMAKE%" --install "%BUILD_DIR%" --config RelWithDebInfo --prefix "%PACKAGE_DIR%" --component GameRuntime
if errorlevel 1 exit /b %errorlevel%
echo Launcher: "%PACKAGE_DIR%\CabbirdLauncher.exe"
if /i not "%STAGE_ALL%"=="ON" exit /b 0
"%CMAKE%" --install "%BUILD_DIR%" --config RelWithDebInfo --prefix "%BUILD_DIR%\package\tools" --component Tools
if errorlevel 1 exit /b %errorlevel%
"%CMAKE%" --install "%BUILD_DIR%" --config RelWithDebInfo --prefix "%BUILD_DIR%\package\sdk" --component SDK
exit /b %errorlevel%

:missing_cmake
echo CMake was not found. Install CMake 3.22+ or Visual Studio 2022 C++ Build Tools.
exit /b 2
:usage
echo Usage: build.cmd [fixtures] [package]
exit /b 2
