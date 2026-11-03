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
// delete, so it hands itself over to a copy in a private directory and exits (see
// RelaunchFromTemp).

#include "install.h"

#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    vvsetup::HardenDllSearch();
    vvsetup::UseUtf8Console();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    bool quiet = false;
    bool fromTemp = false;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv != nullptr && i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--quiet") == 0) quiet = true;
        if (_wcsicmp(argv[i], L"--from-temp") == 0) fromTemp = true;
    }
    if (argv != nullptr) LocalFree(argv);

    // Always a full removal: this is the "uninstall" the user asked for, not a partial one.
    const int result = vvsetup::UninstallProduct(quiet, true, fromTemp);

    CoUninitialize();
    return result;
}
