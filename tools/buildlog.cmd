@echo off
rem Build logging shared by build.bat and package.bat.
rem
rem Called, never run on its own, and deliberately without setlocal: it sets VV_BUILD_LOG in the
rem caller's environment. Every step's full output goes to the log; the console gets one line per
rem step, and on failure a summary - which step, its exit code, the error lines, the log path - so
rem a failed build can be traced afterwards from the file alone.
rem
rem   call tools\buildlog.cmd init <who>             open build\logs\<who>-<time>.log, or join the
rem                                                  one the calling script already opened;
rem                                                  the caller puts its own %* in VV_ARGS first
rem   call tools\buildlog.cmd step <name>            section header in the log, one console line
rem   call tools\buildlog.cmd fail <step> <code>     failure summary on the console and in the log
rem   call tools\buildlog.cmd ok <text>              success line on the console and in the log
rem
rem Everything here stays ASCII and outside parenthesised blocks: the log records paths such as
rem "Program Files (x86)", and a ")" inside a block ends the block early.

if /i "%~1"=="init" goto :init
if /i "%~1"=="step" goto :step
if /i "%~1"=="fail" goto :fail
if /i "%~1"=="ok" goto :ok
echo [buildlog] unknown command: %~1
exit /b 2

:init
set "VVLOG_WHO=%~2"
rem A nested script (build.bat called from package.bat) writes into the caller's log, so one
rem packaging run is one file to read.
if defined VV_BUILD_LOG if exist "%VV_BUILD_LOG%" goto :join

for %%I in ("%~dp0..") do set "VVLOG_ROOT=%%~fI"
set "VVLOG_DIR=%VVLOG_ROOT%\build\logs"
if not exist "%VVLOG_DIR%" mkdir "%VVLOG_DIR%" 2>nul
if not exist "%VVLOG_DIR%" set "VVLOG_DIR=%TEMP%"

rem A sortable timestamp. %DATE% and %TIME% follow the user's locale (2026/11/04 Wed, 9:05:01,23)
rem and do not make a usable file name; PowerShell is asked first, %RANDOM% is the fallback.
set "VVLOG_STAMP="
set "VVLOG_NOW="
for /f "tokens=1,2" %%T in ('powershell -NoProfile -NonInteractive -Command "Get-Date -Format 'yyyyMMdd-HHmmss yyyy-MM-ddTHH:mm:ss'" 2^>nul') do set "VVLOG_STAMP=%%T" & set "VVLOG_NOW=%%U"
if not defined VVLOG_STAMP set "VVLOG_STAMP=%RANDOM%%RANDOM%"
if not defined VVLOG_NOW set "VVLOG_NOW=%TIME%"
set "VV_BUILD_LOG=%VVLOG_DIR%\%VVLOG_WHO%-%VVLOG_STAMP%.log"

rem Keep the 30 newest logs; older ones go.
for /f "skip=30 delims=" %%F in ('dir /b /a-d /o-d "%VVLOG_DIR%\*.log" 2^>nul') do del "%VVLOG_DIR%\%%F" 2>nul

> "%VV_BUILD_LOG%" echo ######## %VVLOG_WHO% started %VVLOG_NOW%
>>"%VV_BUILD_LOG%" echo command:   %VVLOG_WHO% %VV_ARGS%
>>"%VV_BUILD_LOG%" echo directory: %CD%
>>"%VV_BUILD_LOG%" echo computer:  %COMPUTERNAME%  user: %USERNAME%
ver >>"%VV_BUILD_LOG%" 2>&1
where git >nul 2>&1
if errorlevel 1 goto :init_done
>>"%VV_BUILD_LOG%" echo ---- git
git -C "%VVLOG_ROOT%" rev-parse --abbrev-ref HEAD >>"%VV_BUILD_LOG%" 2>&1
git -C "%VVLOG_ROOT%" rev-parse HEAD >>"%VV_BUILD_LOG%" 2>&1
git -C "%VVLOG_ROOT%" status --short >>"%VV_BUILD_LOG%" 2>&1
:init_done
echo [%VVLOG_WHO%] log: %VV_BUILD_LOG%
exit /b 0

:join
>>"%VV_BUILD_LOG%" echo.
>>"%VV_BUILD_LOG%" echo ######## %VVLOG_WHO% started at %TIME%
>>"%VV_BUILD_LOG%" echo command:   %VVLOG_WHO% %VV_ARGS%
exit /b 0

:step
echo [%VVLOG_WHO%] %~2
>>"%VV_BUILD_LOG%" echo.
>>"%VV_BUILD_LOG%" echo ===== [%TIME%] %VVLOG_WHO%: %~2
exit /b 0

:ok
echo [%VVLOG_WHO%] %~2
>>"%VV_BUILD_LOG%" echo ===== [%TIME%] %VVLOG_WHO%: OK - %~2
exit /b 0

:fail
echo.
echo [%VVLOG_WHO%] FAILED at step "%~2" (exit code %~3)
rem The lines that usually name the cause: compiler and linker errors, CMake errors, ninja's
rem FAILED line and the tools' own "cannot ..." messages. Without PowerShell, findstr does a
rem plainer version of the same search.
echo [%VVLOG_WHO%] error lines from the log:
powershell -NoProfile -NonInteractive -Command "$m = Select-String -LiteralPath $env:VV_BUILD_LOG -Pattern ' error |error:|error C|error LNK|error RC|FAILED|CMake Error|fatal|cannot |not found|does not match|differ|mismatch' | Select-Object -Last 25; if ($m) { $m | ForEach-Object { '    ' + $_.Line } } else { '    (none matched; last lines follow)'; Get-Content -LiteralPath $env:VV_BUILD_LOG -Tail 15 | ForEach-Object { '    ' + $_ } }" 2>nul
if errorlevel 1 findstr /i /c:" error " /c:"error:" /c:"FAILED" /c:"CMake Error" /c:"cannot " /c:"not found" "%VV_BUILD_LOG%"
rem Written after the search, so the summary line is not reported as one of the errors.
>>"%VV_BUILD_LOG%" echo.
>>"%VV_BUILD_LOG%" echo ===== [%TIME%] %VVLOG_WHO%: FAILED at step "%~2", exit code %~3
echo [%VVLOG_WHO%] full log: %VV_BUILD_LOG%
exit /b 0
