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
rem   package.bat [generic|classic|all] [path-to-voicevox_core]
setlocal enabledelayedexpansion

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
if not exist "%CORE%\c_api\lib\voicevox_core.dll" (
    echo [package] voicevox_core not found under "%CORE%"
    echo           download voicevox_core-windows-x64-^<version^>.zip from
    echo           https://github.com/VOICEVOX/voicevox_core/releases
    echo           and the .vvm packs you want to ship from
    echo           https://github.com/VOICEVOX/voicevox_vvm/releases
    echo usage: package.bat [generic^|classic^|all] [path-to-voicevox_core]
    exit /b 1
)

call "%~dp0build.bat" "%CORE%" || exit /b 1

rem src\catalog_table.inc holds every style id and is generated from the .vvm set, then committed
rem so that compiling needs no model packs at all. The catch: a different pack version renumbers
rem styles without any error, and a stale table keeps every name working while speaking with the
rem wrong character. Re-derive the table from what is on disk and refuse to pack on a mismatch.
rem `fc` without /b compares line by line, so a CRLF/LF difference in the checkout is not an alarm.
set "REGEN=%TEMP%\voicevox-catalog.inc"
set "PATH=%CORE%\c_api\lib;%PATH%"
"%~dp0build\voicevox-model-metas.exe" "%CORE%\models\vvms" --cpp > "%REGEN%" 2>nul
fc "%REGEN%" "%~dp0src\catalog_table.inc" >nul 2>&1
if errorlevel 1 (
    echo.
    echo [package] src\catalog_table.inc was not reproduced from the packs under
    echo           "%CORE%\models\vvms"
    echo           Either the table is stale -- pack versions renumber style ids, and a stale
    echo           table registers the wrong voice under each name with no error -- or
    echo           voicevox-model-metas could not run. Regenerate it and rebuild:
    echo.
    echo   build\voicevox-model-metas.exe "%CORE%\models\vvms" --cpp ^> src\catalog_table.inc
    echo   build.bat "%CORE%"
    echo.
    del "%REGEN%" >nul 2>&1
    exit /b 1
)
del "%REGEN%" >nul 2>&1

rem Keep the character sheet in step with the build: it compiles the same name tables the DLL
rem registers from, so regenerating it here means the docs can never describe a stale voice list.
if exist "%~dp0build\voicevox-voices-page.exe" (
    if not exist "%~dp0docs" mkdir "%~dp0docs"
    "%~dp0build\voicevox-voices-page.exe" "%~dp0docs\voices.html"
)

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

rem '^|' escapes the pipe: the spec format uses it as the field separator.
> "%SPEC%" (
    echo # payload for VoicevoxSapiSetup.exe - generated by package.bat
    echo file^|app\VoicevoxSapi.dll^|build\VoicevoxSapi.dll
    echo file^|app\VoicevoxSapiUninstall.exe^|build\VoicevoxSapiUninstall.exe
    echo file^|app\voicevox-sapi-setup.exe^|build\voicevox-sapi-setup.exe
    echo file^|voicevox_core.dll^|%CORE%\c_api\lib\voicevox_core.dll
    echo file^|voicevox_onnxruntime.dll^|%CORE%\onnxruntime\lib\voicevox_onnxruntime.dll
    echo dir^|dict^|%CORE%\dict
    echo file^|voices.txt^|build\voices.txt
)

if /i "%MODE%"=="all" (
    for %%F in ("%CORE%\models\vvms\*.vvm") do >> "%SPEC%" echo file^|models\%%~nxF^|%%~fF
) else (
    if /i "%MODE%"=="generic" (set "PACK=n0.vvm") else (set "PACK=0.vvm")
    if not exist "%CORE%\models\vvms\!PACK!" (
        echo [package] %CORE%\models\vvms\!PACK! is missing
        echo           download it from https://github.com/VOICEVOX/voicevox_vvm/releases
        exit /b 1
    )
    >> "%SPEC%" echo file^|models\!PACK!^|%CORE%\models\vvms\!PACK!
)

"%~dp0build\voicevox-pack-payload.exe" "%SPEC%" "%~dp0build\VoicevoxSapiSetup.exe" "%DIST%\VoicevoxSapiSetup.exe" || exit /b 1

echo.
if /i "%MODE%"=="generic" (
    echo Generic package: 6 voices - female 1-3, male 1-3 ^(n0.vvm only^)
) else if /i "%MODE%"=="classic" (
    echo Classic package: 4 characters / 10 voices ^(0.vvm only^)
) else (
    echo Full package: 136 voices from every .vvm
)
echo   dist\VoicevoxSapiSetup.exe
for %%A in ("%DIST%\VoicevoxSapiSetup.exe") do echo   %%~zA bytes
echo.
echo Want a different set? Edit %%ProgramData%%\VoicevoxSapi\voices.txt
echo (one style id or character name per line) and run:
echo   build\voicevox-sapi-setup.exe install
