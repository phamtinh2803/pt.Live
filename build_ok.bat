@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion

echo ====================================================
echo   PT.LIVE - DAW Build Script (fast incremental)
echo ====================================================
echo.

cd /d "%~dp0"
set "SRC_DIR=%CD%"
set "EXITCODE=0"

REM=== [0] Close a running ptLive.exe: Windows locks the .exe, the linker then fails with LNK1104 ===
taskkill /f /im ptLive.exe >nul 2>&1

REM=== [1] Visual Studio 2022 environment (any edition) ===
echo [1] Checking Visual Studio 2022 environment...
set "VCVARS="
for %%E in (Community Professional Enterprise BuildTools) do (
    if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvarsall.bat" (
        set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvarsall.bat"
    )
)
if defined VCVARS (
    call "!VCVARS!" x64 >nul
    echo     OK Found Visual Studio 2022 ^(!VCVARS!^)
) else (
    echo     ^(VS 2022 not found in the default location - assuming the environment is already loaded^)
)

REM=== [2] CMake ===
echo [2] Checking CMake...
where cmake >nul 2>&1
if errorlevel 1 (
    set "PATH=%PATH%;C:\Program Files\CMake\bin"
    where cmake >nul 2>&1
    if errorlevel 1 (
        echo     ERROR: cmake.exe not found. Install CMake or add it to PATH.
        pause
        exit /b 1
    )
    echo     OK CMake added to PATH
) else (
    echo     OK CMake found
)

REM=== [2b] Dependencies (same defaults as CMakeLists.txt; override with env PTLIVE_JUCE_DIR / _TRACKTION_DIR / _ASIO_DIR) ===
echo [2b] Checking dependencies...
set "DEP_JUCE=%PTLIVE_JUCE_DIR%"
set "DEP_TE=%PTLIVE_TRACKTION_DIR%"
set "DEP_ASIO=%PTLIVE_ASIO_DIR%"
if not defined DEP_JUCE set "DEP_JUCE=%SRC_DIR%\..\JUCE-master\JUCE-master"
if not defined DEP_TE   set "DEP_TE=%SRC_DIR%\..\tracktion_engine-develop"
if not defined DEP_ASIO set "DEP_ASIO=%SRC_DIR%\..\ASIOSDK"

set "DEP_OK=1"
if exist "!DEP_JUCE!\CMakeLists.txt" (echo     OK JUCE      : !DEP_JUCE!) else (echo     MISSING JUCE      : !DEP_JUCE! & set "DEP_OK=0")
if exist "!DEP_TE!\VERSION.md"       (echo     OK Tracktion : !DEP_TE!)   else (echo     MISSING Tracktion : !DEP_TE! & set "DEP_OK=0")
if exist "!DEP_ASIO!"                (echo     OK ASIO SDK  : !DEP_ASIO!) else (echo     WARNING ASIO SDK not found: !DEP_ASIO! ^(ASIO build may fail^))
if "!DEP_OK!"=="0" (
    echo.
    echo     ERROR: missing dependency. Put the folders next to the project, or set the
    echo     environment variables PTLIVE_JUCE_DIR / PTLIVE_TRACKTION_DIR / PTLIVE_ASIO_DIR.
    pause
    exit /b 1
)

echo.
echo ====================================================
echo   BUILD OPTIONS   (incremental: only changed files)
echo ====================================================
echo   1 = Debug only
echo   2 = Release only
echo   3 = Both Debug + Release
echo   C = CLEAN rebuild (deletes the build folder first, slow)
echo   Q = Quit (no build)
echo ====================================================
echo.

set "choice="
set /p choice="Your choice [1/2/3/C/Q]: "

if /i "%choice%"=="Q" (
    echo Cancelled.
    exit /b 0
)

set CLEAN=0
if /i "%choice%"=="C" (
    set CLEAN=1
    set "choice="
    set /p choice="Clean rebuild which config? [1/2/3]: "
)

set BUILD_DEBUG=0
set BUILD_RELEASE=0
if "%choice%"=="1" set BUILD_DEBUG=1
if "%choice%"=="2" set BUILD_RELEASE=1
if "%choice%"=="3" set BUILD_DEBUG=1
if "%choice%"=="3" set BUILD_RELEASE=1

if "%BUILD_DEBUG%%BUILD_RELEASE%"=="00" (
    echo Invalid choice.
    pause
    exit /b 1
)

REM=== [3] Stale cache? (project folder moved/copied -> old CMakeCache points elsewhere) ===
if exist build\CMakeCache.txt (
    set "SRC_FWD=%SRC_DIR:\=/%"
    findstr /i /c:"CMAKE_HOME_DIRECTORY:INTERNAL=!SRC_FWD!" build\CMakeCache.txt >nul 2>&1
    if errorlevel 1 (
        echo [3] build\CMakeCache.txt belongs to another folder - forcing a clean configure
        set CLEAN=1
    )
)

if "%CLEAN%"=="1" (
    echo [3] Cleaning previous build...
    if exist build rmdir /s /q build 2>nul
) else (
    echo [3] Keeping previous build folder ^(incremental^)
)
if not exist build mkdir build >nul 2>&1
cd build

REM=== [4] Configure only the first time (CMake re-runs by itself when CMakeLists.txt changes) ===
if not exist CMakeCache.txt (
    echo [4] Configuring with CMake...
    cmake .. -G "Visual Studio 17 2022" -A x64 "-DPTLIVE_JUCE_DIR=!DEP_JUCE!" "-DPTLIVE_TRACKTION_DIR=!DEP_TE!" "-DPTLIVE_ASIO_DIR=!DEP_ASIO!"
    if errorlevel 1 (
        echo.
        echo ERROR: CMake configuration failed!
        echo Please check Visual Studio 2022 install and the dependency paths above.
        pause
        cd ..
        exit /b 1
    )
) else (
    echo [4] Already configured - skipping CMake configure
)

REM=== [5] Parallel compile: many files at once (all CPU cores) ===
set "BUILD_ARGS=--parallel %NUMBER_OF_PROCESSORS% -- /nologo /verbosity:minimal /p:UseMultiToolTask=true /p:CL_MPCount=%NUMBER_OF_PROCESSORS%"

if "%BUILD_DEBUG%"=="1" (
    echo.
    echo [DEBUG] Building...
    cmake --build . --config Debug %BUILD_ARGS%
    if errorlevel 1 (
        echo     FAILED: Debug build error
        set "EXITCODE=1"
    ) else (
        echo     OK Debug build done: build\ptLive_artefacts\Debug\ptLive.exe
    )
)

if "%BUILD_RELEASE%"=="1" (
    echo.
    echo [RELEASE] Building...
    cmake --build . --config Release %BUILD_ARGS%
    if errorlevel 1 (
        echo     FAILED: Release build error
        set "EXITCODE=1"
    ) else (
        echo     OK Release build done: build\ptLive_artefacts\Release\ptLive.exe
    )
)

cd ..

echo.
echo ====================================================
if "%EXITCODE%"=="0" (
    echo   Build script finished OK
) else (
    echo   Build script finished WITH ERRORS - scroll up and look for "error C" / "error LNK"
)
echo ====================================================
pause
exit /b %EXITCODE%
