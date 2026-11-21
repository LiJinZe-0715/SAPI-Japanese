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
rem Everything is logged to build\logs\build-<time>.log (see tools\buildlog.cmd): the toolchain
rem it picked, the environment it built with, every compiler command and its output. The console
rem shows one line per step; on failure it shows the error lines and where the log is.
rem
rem vcvars64.bat is not called because it shells out to reg.exe, which is on this machine's
rem command blacklist; it aborts half way and leaves the SDK paths off PATH ("rc.exe not found").
setlocal EnableExtensions
set "LOGCMD=%~dp0tools\buildlog.cmd"
set "VV_ARGS=%*"
call "%LOGCMD%" init build

rem --- Visual Studio: vswhere if present, otherwise the usual install locations ---------------
call "%LOGCMD%" step "find toolchain"
set "VS="
if defined VSINSTALLDIR set "VS=%VSINSTALLDIR%"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not defined VS if exist "%VSWHERE%" for /f "usebackq delims=" %%D in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%D"
if not defined VS for %%E in (2022 2019) do if not defined VS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\%%E\BuildTools\VC\Tools\MSVC" set "VS=%ProgramFiles(x86)%\Microsoft Visual Studio\%%E\BuildTools"
if not defined VS for %%E in (2022 2019) do if not defined VS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\%%E\Community\VC\Tools\MSVC" set "VS=%ProgramFiles(x86)%\Microsoft Visual Studio\%%E\Community"
if not defined VS for %%E in (2022 2019) do if not defined VS if exist "%ProgramFiles%\Microsoft Visual Studio\%%E\BuildTools\VC\Tools\MSVC" set "VS=%ProgramFiles%\Microsoft Visual Studio\%%E\BuildTools"
set "WHY=no Visual Studio with the C++ toolset was found. Install Visual Studio Build Tools 2022 with the Desktop development with C++ workload, or set VSINSTALLDIR to an existing installation."
if not defined VS goto :toolchain_error

rem --- MSVC toolset: newest installed unless MSVCVER names one -------------------------------
set "MSVC="
if defined MSVCVER set "MSVC=%VS%\VC\Tools\MSVC\%MSVCVER%"
if not defined MSVC for /f "delims=" %%V in ('dir /b /ad /o-n "%VS%\VC\Tools\MSVC" 2^>nul') do if not defined MSVC set "MSVC=%VS%\VC\Tools\MSVC\%%V"
set "WHY=cl.exe not found at %MSVC%\bin\Hostx64\x64\cl.exe. Set MSVCVER to the toolset you want, for example: set MSVCVER=14.44.35207"
if not exist "%MSVC%\bin\Hostx64\x64\cl.exe" goto :toolchain_error

rem --- Windows SDK: newest 10.* unless WINSDKVER names one -----------------------------------
set "SDK=%ProgramFiles(x86)%\Windows Kits\10"
set "SDKVER="
if defined WINSDKVER set "SDKVER=%WINSDKVER%"
if not defined SDKVER for /f "delims=" %%V in ('dir /b /ad /o-n "%SDK%\Include\10.*" 2^>nul') do if not defined SDKVER set "SDKVER=%%V"
set "WHY=rc.exe not found under %SDK%\bin\%SDKVER%. Install the Windows 10/11 SDK, or set WINSDKVER to a version that is installed."
if not exist "%SDK%\bin\%SDKVER%\x64\rc.exe" goto :toolchain_error

set "PATH=%MSVC%\bin\Hostx64\x64;%SDK%\bin\%SDKVER%\x64;%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
set "INCLUDE=%MSVC%\include;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\um;%SDK%\Include\%SDKVER%\shared;%SDK%\Include\%SDKVER%\winrt"
set "LIB=%MSVC%\lib\x64;%SDK%\Lib\%SDKVER%\ucrt\x64;%SDK%\Lib\%SDKVER%\um\x64"

rem English diagnostics: the default is the system UI language, which arrives here as GBK.
set "VSLANG=1033"

rem voicevox_core: argument, then %VOICEVOX_CORE_DIR%, then third_party. Resolved here and always
rem passed to CMake: CMake caches the directory after the first configure, so leaving the choice
rem to CMakeLists.txt meant a changed environment variable was silently ignored from then on.
set "CORE=%~1"
if "%CORE%"=="" set "CORE=%VOICEVOX_CORE_DIR%"
if "%CORE%"=="" set "CORE=%~dp0third_party\voicevox_core"

echo [build] Visual Studio  %VS%
echo [build] MSVC toolset   %MSVC%
echo [build] Windows SDK    %SDKVER%
echo [build] voicevox_core  %CORE%

rem What this build was made with, so a log from another machine can be compared line by line.
>>"%VV_BUILD_LOG%" echo Visual Studio  %VS%
>>"%VV_BUILD_LOG%" echo MSVC toolset   %MSVC%
>>"%VV_BUILD_LOG%" echo Windows SDK    %SDKVER%
>>"%VV_BUILD_LOG%" echo voicevox_core  %CORE%
>>"%VV_BUILD_LOG%" echo VSINSTALLDIR=%VSINSTALLDIR%  MSVCVER=%MSVCVER%  WINSDKVER=%WINSDKVER%  VOICEVOX_CORE_DIR=%VOICEVOX_CORE_DIR%
set PATH >>"%VV_BUILD_LOG%" 2>&1
set INCLUDE >>"%VV_BUILD_LOG%" 2>&1
set LIB >>"%VV_BUILD_LOG%" 2>&1
>>"%VV_BUILD_LOG%" echo ---- tool versions
cmake --version >>"%VV_BUILD_LOG%" 2>&1
ninja --version >>"%VV_BUILD_LOG%" 2>&1
rem cl.exe prints its version banner when run without arguments, then exits with an error.
cl.exe >>"%VV_BUILD_LOG%" 2>&1
>>"%VV_BUILD_LOG%" echo ---- voicevox_core
if exist "%CORE%\c_api\include\voicevox_core.h" (>>"%VV_BUILD_LOG%" echo header present) else (>>"%VV_BUILD_LOG%" echo header MISSING: c_api\include\voicevox_core.h)
dir /b "%CORE%" >>"%VV_BUILD_LOG%" 2>&1

rem Forward slashes on purpose: a backslash path is written verbatim into CMakeRCCompiler.cmake
rem and CMake then chokes on the escapes.
set "SDK_FWD=%SDK:\=/%"
set "STEP=configure (cmake)"
call "%LOGCMD%" step "%STEP%"
cmake -S "%~dp0." -B "%~dp0build" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
    -DVOICEVOX_CORE_DIR="%CORE:\=/%" ^
    -DCMAKE_RC_COMPILER="%SDK_FWD%/bin/%SDKVER%/x64/rc.exe" ^
    -DCMAKE_MT="%SDK_FWD%/bin/%SDKVER%/x64/mt.exe" >>"%VV_BUILD_LOG%" 2>&1 || goto :failed

rem --verbose: the log gets every compiler and linker command line, not just ninja's progress.
set "STEP=compile and link (ninja)"
call "%LOGCMD%" step "%STEP%"
cmake --build "%~dp0build" --verbose >>"%VV_BUILD_LOG%" 2>&1 || goto :failed

rem The pure helpers (sentence splitting, JSON patching, voices.txt parsing) have unit tests; a
rem build that breaks them is not one to package.
set "STEP=unit tests"
call "%LOGCMD%" step "%STEP%"
"%~dp0build\voicevox-unit-tests.exe" >>"%VV_BUILD_LOG%" 2>&1 || goto :failed

call "%LOGCMD%" ok "built %~dp0build\VoicevoxSapi.dll"
exit /b 0

:failed
set "RC=%ERRORLEVEL%"
call "%LOGCMD%" fail "%STEP%" %RC%
exit /b %RC%

:toolchain_error
echo [build] %WHY%
>>"%VV_BUILD_LOG%" echo [build] cannot build: %WHY%
call "%LOGCMD%" fail "find toolchain" 1
exit /b 1
