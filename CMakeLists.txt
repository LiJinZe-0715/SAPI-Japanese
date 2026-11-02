cmake_minimum_required(VERSION 3.20)
project(SAPI-Japanese LANGUAGES CXX)

# The icon and the version block live in .rc files (src/res). Without this the resource files are
# treated as sources to be ignored and every binary ships with the default blank icon.
enable_language(RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Reproducible output. Without /Brepro the compiler and linker stamp every binary with the build
# time, so building the same sources twice produces two different files and "did this rebuild
# change anything?" has no answer. With it, rebuilding in the same checkout is byte for byte
# stable; a build in a different directory still differs by a few dozen bytes, because the linker
# records a hash of the absolute input paths. (/pathmap does not remove that one.)
add_compile_options(/Brepro)
add_link_options(/Brepro)

# Where the unpacked voicevox_core release lives (c_api/, dict/, models/, onnxruntime/).
#
# Not part of this repository: the runtime, the dictionary and the model packs come to about
# 1.6 GB, which has no business in a git history. Resolution order:
#
#   -DVOICEVOX_CORE_DIR=...   >   %VOICEVOX_CORE_DIR%   >   <repo>/third_party/voicevox_core
if(NOT DEFINED VOICEVOX_CORE_DIR AND DEFINED ENV{VOICEVOX_CORE_DIR})
    file(TO_CMAKE_PATH "$ENV{VOICEVOX_CORE_DIR}" VOICEVOX_CORE_DIR)
endif()
# `if(NOT VOICEVOX_CORE_DIR)` rather than DEFINED: a variable that exists but is empty (an env var
# set to nothing) is as good as missing, and would otherwise produce an error naming no directory.
if(NOT VOICEVOX_CORE_DIR)
    set(VOICEVOX_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/voicevox_core")
endif()
set(VOICEVOX_CORE_DIR "${VOICEVOX_CORE_DIR}" CACHE PATH "voicevox_core release directory")

if(NOT EXISTS "${VOICEVOX_CORE_DIR}/c_api/include/voicevox_core.h")
    message(FATAL_ERROR
        "voicevox_core not found under: ${VOICEVOX_CORE_DIR}\n"
        "This repository holds source only; the runtime and the model packs are external.\n"
        "Download both, then point the build at the unpacked directory:\n"
        "  1. voicevox_core-windows-x64-<version>.zip\n"
        "     https://github.com/VOICEVOX/voicevox_core/releases\n"
        "     unpack it somewhere - it provides c_api/, onnxruntime/, dict/\n"
        "  2. the *.vvm packs you want to ship\n"
        "     https://github.com/VOICEVOX/voicevox_vvm/releases\n"
        "     put them under models/vvms/ (needed to package and to run, not to compile)\n"
        "Then run:  build.bat <that directory>   or set VOICEVOX_CORE_DIR=<that directory>")
endif()

add_library(VoicevoxSapi SHARED
    src/catalog.cpp
    src/core.cpp
    src/dllmain.cpp
    src/engine.cpp
    src/log.cpp
    src/registry.cpp
    src/voices_i18n.cpp
    src/res/VoicevoxSapi.rc
    src/VoicevoxSapi.def)

target_compile_definitions(VoicevoxSapi PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

# The catalog carries Japanese string literals and the sources are UTF-8; without this MSVC
# reads them as the system code page (936 here) and the character names come out mangled.
target_compile_options(VoicevoxSapi PRIVATE /utf-8)

target_include_directories(VoicevoxSapi PRIVATE
    "${VOICEVOX_CORE_DIR}/c_api/include")

target_link_libraries(VoicevoxSapi PRIVATE
    "${VOICEVOX_CORE_DIR}/c_api/lib/voicevox_core.lib"
    ole32 oleaut32 uuid shell32 advapi32 delayimp)

# Delay-loaded on purpose: SAPI loads this DLL into the *caller's* process (Chrome), and the
# default search order for a dependency looks at the host executable's directory, not ours.
# The hook in dllmain.cpp supplies the absolute path instead.
target_link_options(VoicevoxSapi PRIVATE "/DELAYLOAD:voicevox_core.dll")

set_target_properties(VoicevoxSapi PROPERTIES
    OUTPUT_NAME "VoicevoxSapi"
    PREFIX "")

# The installer: copies the runtime and model packs, then loads the DLL and calls
# DllRegisterServer, so the registry layout lives in exactly one place. It links SAPI only to
# verify the result by enumerating what it just registered.
add_executable(voicevox-sapi-setup tools/setup.cpp src/res/voicevox-sapi-setup.rc)
target_compile_definitions(voicevox-sapi-setup PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(voicevox-sapi-setup PRIVATE /utf-8)
target_link_libraries(voicevox-sapi-setup PRIVATE sapi ole32 oleaut32 uuid shell32)
set_target_properties(voicevox-sapi-setup PROPERTIES OUTPUT_NAME "voicevox-sapi-setup")

# The single-file silent installer end users double-click. Same install as voicevox-sapi-setup,
# but no console and no arguments; the payload is glued to the executable by
# voicevox-pack-payload (see package.bat), and the UAC prompt comes from the manifest.
add_executable(VoicevoxSapiSetup WIN32 tools/installer.cpp src/res/VoicevoxSapiSetup.rc)
target_compile_definitions(VoicevoxSapiSetup PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(VoicevoxSapiSetup PRIVATE /utf-8)
target_link_libraries(VoicevoxSapiSetup PRIVATE sapi ole32 oleaut32 uuid shell32 advapi32)
target_link_options(VoicevoxSapiSetup PRIVATE "/MANIFESTUAC:level='requireAdministrator'")
set_target_properties(VoicevoxSapiSetup PROPERTIES OUTPUT_NAME "VoicevoxSapiSetup")

# What "Apps & features" runs. Small on purpose: it must not need a copy of the installer, and
# it must not flash a console window when the user clicks Remove.
add_executable(VoicevoxSapiUninstall WIN32 tools/uninstaller.cpp src/res/VoicevoxSapiUninstall.rc)
target_compile_definitions(VoicevoxSapiUninstall PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(VoicevoxSapiUninstall PRIVATE /utf-8)
target_link_libraries(VoicevoxSapiUninstall PRIVATE sapi ole32 oleaut32 uuid shell32 advapi32)
target_link_options(VoicevoxSapiUninstall PRIVATE "/MANIFESTUAC:level='requireAdministrator'")
set_target_properties(VoicevoxSapiUninstall PROPERTIES OUTPUT_NAME "VoicevoxSapiUninstall")

# Build-time tool: appends the runtime, dictionary and model packs to the installer above.
add_executable(voicevox-pack-payload tools/pack-payload.cpp)
target_compile_definitions(voicevox-pack-payload PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(voicevox-pack-payload PRIVATE /utf-8)
set_target_properties(voicevox-pack-payload PROPERTIES OUTPUT_NAME "voicevox-pack-payload")

# Verification without a browser: lists the SAPI voices and renders a WAV through the same path
# Chrome uses.
add_executable(voicevox-sapi-check tools/sapi-check.cpp)
target_compile_definitions(voicevox-sapi-check PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(voicevox-sapi-check PRIVATE /utf-8)
target_link_libraries(voicevox-sapi-check PRIVATE sapi ole32 oleaut32 uuid)
set_target_properties(voicevox-sapi-check PROPERTIES OUTPUT_NAME "voicevox-sapi-check")

# Reads the style ids straight out of the .vvm packs, so the catalog table is generated from
# the models rather than transcribed from a wiki page.
add_executable(voicevox-model-metas tools/model-metas.cpp)
target_compile_definitions(voicevox-model-metas PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(voicevox-model-metas PRIVATE /utf-8)
target_include_directories(voicevox-model-metas PRIVATE "${VOICEVOX_CORE_DIR}/c_api/include")
target_link_libraries(voicevox-model-metas PRIVATE
    "${VOICEVOX_CORE_DIR}/c_api/lib/voicevox_core.lib")
set_target_properties(voicevox-model-metas PROPERTIES OUTPUT_NAME "voicevox-model-metas")

# Writes the character reference sheet (docs/voices.html). It compiles the same name tables the
# DLL registers from, so the page and the voice list cannot drift apart.
add_executable(voicevox-voices-page tools/voices-page.cpp src/voices_i18n.cpp)
target_compile_definitions(voicevox-voices-page PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(voicevox-voices-page PRIVATE /utf-8)
set_target_properties(voicevox-voices-page PROPERTIES OUTPUT_NAME "voicevox-voices-page")

# Draws src/res/app.ico. Deliberately not wired into the build: rc.exe needs the .ico before
# anything is compiled, so the file is committed and only regenerated when the geometry in
# tools/make-icon.cpp changes:
#   ./build/voicevox-make-icon.exe src/res/app.ico
add_executable(voicevox-make-icon tools/make-icon.cpp)
target_compile_definitions(voicevox-make-icon PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(voicevox-make-icon PRIVATE /utf-8)
set_target_properties(voicevox-make-icon PROPERTIES OUTPUT_NAME "voicevox-make-icon")
