@echo off
rem Builds the single-file installer: dist\VoicevoxSapiSetup.exe
rem
rem What ends up inside (see tools/pack-payload.cpp for the container format):
rem   app\...  -> C:\Program Files\VoicevoxSapi\   engine DLL + the command line uninstaller
rem   rest     -> C:\ProgramData\VoicevoxSapi\      runtime, open_jtalk dictionary, model packs
rem
rem Three packages, because "which voices" is the only real choice here:
rem
rem   package.bat            generic  n0.vvm + voices.txt = female 1-3, male 1-3 (6 voices)
rem   package.bat classic    0.vvm    = Shikoku Metan / Zundamon / Kasukabe Tsumugi /
rem                                    Amehare Hau (4 characters, 10 voices, all four styles)
rem   package.bat all        every .vvm = 136 voices, ~1.8 GB
rem
rem Everything in this file stays ASCII on purpose: cmd.exe reads batch files in the OEM code
rem page, so a UTF-8 character name turns into garbage that ends the rem and starts new commands.
rem The same rule applies to the generated voices.txt below, which is why it carries style ids.
rem
rem The default is generic: six voices is what most people want in the picker, and every extra
rem model pack costs ~60 MB of download. The dictionary alone is 98 MB and cannot be trimmed.
rem
rem One run, one log: build\logs\package-<time>.log holds this script's steps and the whole
rem build.bat run it starts, plus the payload spec and the SHA-256 of the installer it produced.
rem
rem   package.bat [generic|classic|all] [path-to-voicevox_core]
setlocal
set "LOGCMD=%~dp0tools\buildlog.cmd"
set "VV_ARGS=%*"
call "%LOGCMD%" init package

set "MODE=generic"
set "CORE="
if /i "%~1"=="generic" (
    set "MODE=generic"
) else if /i "%~1"=="classic" (
    set "MODE=classic"
) else if /i "%~1"=="all" (
    set "MODE=all"
) else (
    rem Backwards compatible with the old one-argument form: package.bat <core-dir>
    set "CORE=%~1"
)
if not "%~2"=="" set "CORE=%~2"

rem Same resolution order as CMakeLists.txt: argument, then the environment, then third_party.
if "%CORE%"=="" if defined VOICEVOX_CORE_DIR set "CORE=%VOICEVOX_CORE_DIR%"
if "%CORE%"=="" set "CORE=%~dp0third_party\voicevox_core"
>>"%VV_BUILD_LOG%" echo mode=%MODE%  core=%CORE%
if not exist "%CORE%\c_api\lib\voicevox_core.dll" goto :missing_core

rem build.bat writes into this same log and reports its own failure; only pass the code on.
call "%~dp0build.bat" "%CORE%" || exit /b 1

rem src\catalog_table.inc holds every style id and is generated from the .vvm set, then committed
rem so that compiling needs no model packs at all. The catch: a different pack version renumbers
rem styles without any error, and a stale table keeps every name working while speaking with the
rem wrong character. Check the table against the packs on disk and refuse to pack on a mismatch.
rem The check is per pack: only the packs present are compared, so a generic build with nothing
rem but n0.vvm downloaded passes as long as n0.vvm's rows are right.
set "STEP=verify catalog against model packs"
call "%LOGCMD%" step "%STEP%"
set "PATH=%CORE%\c_api\lib;%PATH%"
>>"%VV_BUILD_LOG%" echo packs present:
dir /b "%CORE%\models\vvms\*.vvm" >>"%VV_BUILD_LOG%" 2>&1
"%~dp0build\voicevox-model-metas.exe" "%CORE%\models\vvms" --verify "%~dp0src\catalog_table.inc" >>"%VV_BUILD_LOG%" 2>&1
if errorlevel 1 goto :stale_catalog

rem Keep the character sheet in step with the build: it compiles the same name tables the DLL
rem registers from, so regenerating it here means the docs can never describe a stale voice list.
set "STEP=regenerate docs\voices.html"
call "%LOGCMD%" step "%STEP%"
if not exist "%~dp0docs" mkdir "%~dp0docs"
"%~dp0build\voicevox-voices-page.exe" "%~dp0docs\voices.html" >>"%VV_BUILD_LOG%" 2>&1 || goto :failed

rem The allow-list the engine reads while registering. Style ids rather than names: this file is
rem written by a batch script, and a UTF-8 Japanese/Chinese name does not survive cmd's code page.
rem The DLL matches ids and names alike, so a human editing it later can still write "female 1".
if /i "%MODE%"=="generic" (
    > "%~dp0build\voices.txt" (
        echo # generic voices only: female 1-3, male 1-3
        echo # 10006 = 10004 registers female 6 under female 3's name
        echo 10000
        echo 10001
        echo 10002
        echo 10005
        echo 10007
        echo 10006 = 10004
    )
) else (
    rem "*" = no filter. Written explicitly so an install never inherits a selection from an
    rem older one that shipped a different list.
    > "%~dp0build\voices.txt" echo *
)

set "SPEC=%~dp0build\payload.spec.txt"
set "DIST=%~dp0dist"
if not exist "%DIST%" mkdir "%DIST%"

rem '^|' escapes the pipe: the spec format uses it as the field separator. One line per command
rem rather than a parenthesised block, and absolute sources: a path containing ")" (say, under
rem "Program Files (x86)") would end a block early, and a relative path would depend on the
rem directory package.bat was started from.
set "STEP=write payload spec (%MODE%)"
call "%LOGCMD%" step "%STEP%"
set "B=%~dp0build"
> "%SPEC%" echo # payload for VoicevoxSapiSetup.exe - generated by package.bat
>> "%SPEC%" echo file^|app\VoicevoxSapi.dll^|%B%\VoicevoxSapi.dll
>> "%SPEC%" echo file^|app\VoicevoxSapiUninstall.exe^|%B%\VoicevoxSapiUninstall.exe
>> "%SPEC%" echo file^|app\voicevox-sapi-setup.exe^|%B%\voicevox-sapi-setup.exe
>> "%SPEC%" echo file^|voicevox_core.dll^|%CORE%\c_api\lib\voicevox_core.dll
>> "%SPEC%" echo file^|voicevox_onnxruntime.dll^|%CORE%\onnxruntime\lib\voicevox_onnxruntime.dll
>> "%SPEC%" echo dir^|dict^|%CORE%\dict
>> "%SPEC%" echo file^|voices.txt^|%B%\voices.txt

if /i "%MODE%"=="all" goto :spec_all
set "PACK=0.vvm"
if /i "%MODE%"=="generic" set "PACK=n0.vvm"
if not exist "%CORE%\models\vvms\%PACK%" goto :missing_pack
>> "%SPEC%" echo file^|models\%PACK%^|%CORE%\models\vvms\%PACK%
goto :spec_done
:spec_all
for %%F in ("%CORE%\models\vvms\*.vvm") do >> "%SPEC%" echo file^|models\%%~nxF^|%%~fF
:spec_done
type "%SPEC%" >>"%VV_BUILD_LOG%"

set "STEP=pack installer"
call "%LOGCMD%" step "%STEP%"
"%~dp0build\voicevox-pack-payload.exe" "%SPEC%" "%~dp0build\VoicevoxSapiSetup.exe" "%DIST%\VoicevoxSapiSetup.exe" >>"%VV_BUILD_LOG%" 2>&1 || goto :failed

rem The hash ties a file someone reports a problem with back to this log.
set "HASH="
for /f "skip=1 delims=" %%H in ('certutil -hashfile "%DIST%\VoicevoxSapiSetup.exe" SHA256 2^>nul') do if not defined HASH set "HASH=%%H"
for %%A in ("%DIST%\VoicevoxSapiSetup.exe") do set "SIZE=%%~zA"
>>"%VV_BUILD_LOG%" echo output  %DIST%\VoicevoxSapiSetup.exe
>>"%VV_BUILD_LOG%" echo size    %SIZE% bytes
>>"%VV_BUILD_LOG%" echo sha256  %HASH%

echo.
if /i "%MODE%"=="generic" (
    echo Generic package: 6 voices - female 1-3, male 1-3 ^(n0.vvm only^)
) else if /i "%MODE%"=="classic" (
    echo Classic package: 4 characters / 10 voices ^(0.vvm only^)
) else (
    echo Full package: 136 voices from every .vvm
)
echo   dist\VoicevoxSapiSetup.exe
echo   %SIZE% bytes
echo   sha256 %HASH%
echo.
echo Want a different set? Edit %%ProgramData%%\VoicevoxSapi\voices.txt
echo (one style id or character name per line) and run:
echo   build\voicevox-sapi-setup.exe install
call "%LOGCMD%" ok "packaged %DIST%\VoicevoxSapiSetup.exe"
echo [package] log: %VV_BUILD_LOG%
exit /b 0

:failed
set "RC=%ERRORLEVEL%"
call "%LOGCMD%" fail "%STEP%" %RC%
exit /b %RC%

:missing_core
echo [package] voicevox_core not found under "%CORE%"
echo           download voicevox_core-windows-x64-^<version^>.zip from
echo           https://github.com/VOICEVOX/voicevox_core/releases
echo           and the .vvm packs you want to ship from
echo           https://github.com/VOICEVOX/voicevox_vvm/releases
echo usage: package.bat [generic^|classic^|all] [path-to-voicevox_core]
>>"%VV_BUILD_LOG%" echo [package] cannot package: voicevox_core not found under %CORE%
call "%LOGCMD%" fail "find voicevox_core" 1
exit /b 1

:stale_catalog
echo.
echo [package] src\catalog_table.inc does not match the packs under
echo           "%CORE%\models\vvms"
echo           Either the table is stale -- pack versions renumber style ids, and a stale
echo           table registers the wrong voice under each name with no error -- or
echo           voicevox-model-metas could not run. Regenerate it from the full pack set:
echo.
echo   build\voicevox-model-metas.exe "%CORE%\models\vvms" --cpp ^> src\catalog_table.inc
echo   build.bat "%CORE%"
call "%LOGCMD%" fail "%STEP%" 1
exit /b 1

:missing_pack
echo [package] "%CORE%\models\vvms\%PACK%" is missing
echo           download it from https://github.com/VOICEVOX/voicevox_vvm/releases
>>"%VV_BUILD_LOG%" echo [package] cannot package: model pack not found: %CORE%\models\vvms\%PACK%
call "%LOGCMD%" fail "%STEP%" 1
exit /b 1
