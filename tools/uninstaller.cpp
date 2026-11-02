// VoicevoxSapiUninstall — the small uninstaller that Settings runs.
//
// It exists as its own executable instead of pointing "Apps & features" at the installer for two
// reasons: uninstalling should not require a 176 MB copy of everything to still be on disk, and
// the console installer would flash a black window when the user clicks Remove.
//
//   VoicevoxSapiUninstall.exe             remove voices, runtime and model packs
//   VoicevoxSapiUninstall.exe --quiet     no dialog, result in the exit code
//
// It normally lives in C:\Program Files\VoicevoxSapi, i.e. inside the directory it has to
// delete, so it hands itself over to a copy in %TEMP% and exits (see RelaunchFromTemp).

#include "install.h"

#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    vvsetup::UseUtf8Console();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const std::wstring command = GetCommandLineW();
    const bool quiet = command.find(L"--quiet") != std::wstring::npos;
    const bool fromTemp = command.find(L"--from-temp") != std::wstring::npos;

    // Always a full removal: this is the "uninstall" the user asked for, not a partial one.
    const int result = vvsetup::UninstallProduct(quiet, true, fromTemp);

    CoUninitialize();
    return result;
}
