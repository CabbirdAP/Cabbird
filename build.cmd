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
if not errorlevel 1 goto :detect_toolset
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :missing_cmake
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT goto :missing_cmake
set "CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "%CMAKE%" goto :missing_cmake

:detect_toolset
rem Pick the generator that matches the installed toolchain. The Visual Studio
rem generator name contains the IDE version, and CMake only ships a given name
rem from a specific release on: "Visual Studio 18 2026" first appeared in
rem CMake 4.1, so an older CMake paired with VS 2026 would fail at configure.
rem Both branches are plain VS generators; there is no second build system.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSMAJOR="
if exist "%VSWHERE%" (
    for /f "usebackq tokens=1 delims=." %%a in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion`) do set "VSMAJOR=%%a"
)
if not defined VSMAJOR goto :preset_vs2022
if %VSMAJOR% LSS 18 goto :preset_vs2022

rem VS 18+ is installed. Only use its generator if this CMake knows the name.
"%CMAKE%" --help > "%TEMP%\cabbird-generators.txt" 2>nul
findstr /c:"Visual Studio 18 2026" "%TEMP%\cabbird-generators.txt" >nul 2>nul
if errorlevel 1 goto :generator_mismatch
del "%TEMP%\cabbird-generators.txt" >nul 2>nul
set "CONFIGURE_PRESET=windows-vs2026"
set "BUILD_PRESET=windows-vs2026-relwithdebinfo"
set "BUILD_DIR=%~dp0.build\windows-vs2026"
goto :run

:preset_vs2022
set "CONFIGURE_PRESET=windows-vs2022"
set "BUILD_PRESET=windows-relwithdebinfo"
set "BUILD_DIR=%~dp0.build\windows-vs2022"

:run
set "PACKAGE_DIR=%BUILD_DIR%\game-package"
"%CMAKE%" --preset %CONFIGURE_PRESET% "-DCABBIRD_BUILD_TEST_FIXTURES=%BUILD_FIXTURES%"
if errorlevel 1 exit /b %errorlevel%
rem MSBuild fails before useful output when --parallel is passed here.
"%CMAKE%" --build --preset %BUILD_PRESET%
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

:generator_mismatch
echo This CMake does not know the "Visual Studio 18 2026" generator.
echo Upgrade to CMake 4.1 or newer: scoop install cmake
exit /b 2
:missing_cmake
echo CMake was not found. Install CMake 4.5+ or Visual Studio C++ Build Tools.
exit /b 2
:usage
echo Usage: build.cmd [fixtures] [package]
exit /b 2
