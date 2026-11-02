@echo off
rem Builds VoicevoxSapi.dll with the MSVC toolchain.
rem
rem   build.bat [path-to-voicevox_core]
rem
rem Nothing here is tied to one machine:
rem   - the Visual Studio install, the MSVC toolset and the Windows SDK are discovered at run time,
rem     newest installed wins. VSINSTALLDIR / MSVCVER / WINSDKVER override the discovery.
rem   - voicevox_core comes from the first argument, else %VOICEVOX_CORE_DIR%, else
rem     third_party\voicevox_core next to this script.
rem
rem vcvars64.bat is not called because it shells out to reg.exe, which is on this machine's
rem command blacklist; it aborts half way and leaves the SDK paths off PATH ("rc.exe not found").
setlocal EnableExtensions

rem --- Visual Studio: vswhere if present, otherwise the usual install locations ---------------
set "VS="
if defined VSINSTALLDIR set "VS=%VSINSTALLDIR%"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not defined VS if exist "%VSWHERE%" for /f "usebackq delims=" %%D in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%D"
if not defined VS for %%E in (2022 2019) do if not defined VS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\%%E\BuildTools\VC\Tools\MSVC" set "VS=%ProgramFiles(x86)%\Microsoft Visual Studio\%%E\BuildTools"
if not defined VS for %%E in (2022 2019) do if not defined VS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\%%E\Community\VC\Tools\MSVC" set "VS=%ProgramFiles(x86)%\Microsoft Visual Studio\%%E\Community"
if not defined VS for %%E in (2022 2019) do if not defined VS if exist "%ProgramFiles%\Microsoft Visual Studio\%%E\BuildTools\VC\Tools\MSVC" set "VS=%ProgramFiles%\Microsoft Visual Studio\%%E\BuildTools"
if not defined VS (
    echo [build] no Visual Studio with the C++ toolset was found.
    echo         install Visual Studio Build Tools 2022 with the "Desktop development with C++"
    echo         workload, or set VSINSTALLDIR to an existing installation.
    exit /b 1
)

rem --- MSVC toolset: newest installed unless MSVCVER names one -------------------------------
set "MSVC="
if defined MSVCVER set "MSVC=%VS%\VC\Tools\MSVC\%MSVCVER%"
if not defined MSVC for /f "delims=" %%V in ('dir /b /ad /o-n "%VS%\VC\Tools\MSVC" 2^>nul') do if not defined MSVC set "MSVC=%VS%\VC\Tools\MSVC\%%V"
if not exist "%MSVC%\bin\Hostx64\x64\cl.exe" (
    echo [build] cl.exe not found under "%VS%\VC\Tools\MSVC".
    echo         set MSVCVER to the toolset you want, for example: set MSVCVER=14.44.35207
    exit /b 1
)

rem --- Windows SDK: newest 10.* unless WINSDKVER names one -----------------------------------
set "SDK=%ProgramFiles(x86)%\Windows Kits\10"
set "SDKVER="
if defined WINSDKVER set "SDKVER=%WINSDKVER%"
if not defined SDKVER for /f "delims=" %%V in ('dir /b /ad /o-n "%SDK%\Include\10.*" 2^>nul') do if not defined SDKVER set "SDKVER=%%V"
if not exist "%SDK%\bin\%SDKVER%\x64\rc.exe" (
    echo [build] rc.exe not found under "%SDK%\bin".
    echo         install the Windows 10/11 SDK, or set WINSDKVER to a version that is installed.
    exit /b 1
)

set "PATH=%MSVC%\bin\Hostx64\x64;%SDK%\bin\%SDKVER%\x64;%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
set "INCLUDE=%MSVC%\include;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\um;%SDK%\Include\%SDKVER%\shared;%SDK%\Include\%SDKVER%\winrt"
set "LIB=%MSVC%\lib\x64;%SDK%\Lib\%SDKVER%\ucrt\x64;%SDK%\Lib\%SDKVER%\um\x64"

rem English diagnostics: the default is the system UI language, which arrives here as GBK.
set "VSLANG=1033"

rem Optional: the first argument is the unpacked voicevox_core directory.
set "CORE_ARG="
if not "%~1"=="" set CORE_ARG=-DVOICEVOX_CORE_DIR="%~1"

echo [build] Visual Studio  %VS%
echo [build] MSVC toolset   %MSVC%
echo [build] Windows SDK    %SDKVER%

rem Forward slashes on purpose: a backslash path is written verbatim into CMakeRCCompiler.cmake
rem and CMake then chokes on the escapes.
set "SDK_FWD=%SDK:\=/%"
cmake -S "%~dp0." -B "%~dp0build" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
    %CORE_ARG% ^
    -DCMAKE_RC_COMPILER="%SDK_FWD%/bin/%SDKVER%/x64/rc.exe" ^
    -DCMAKE_MT="%SDK_FWD%/bin/%SDKVER%/x64/mt.exe" || exit /b 1
cmake --build "%~dp0build" || exit /b 1

echo.
echo Built: %~dp0build\VoicevoxSapi.dll
