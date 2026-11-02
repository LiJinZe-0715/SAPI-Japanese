// voicevox-sapi-setup — the command line installer.
//
// This is the dev / advanced path: it can point at an unpacked voicevox_core release and pick
// which model packs to copy. End users get VoicevoxSapiSetup.exe instead, which is the same
// install without arguments and without a console.
//
// regsvr32 is the obvious tool for registering a COM server, but it is a LOLBin and is blocked
// on some machines, so this program calls DllRegisterServer itself. It also means the installer
// knows nothing about the registry layout: that stays in the DLL, in one place.
//
//   voicevox-sapi-setup install [--source <core-dir>] [--dll <path>] [--models all|0,3,8]
//                              [--prune] [--only <names>] [--all]
//   voicevox-sapi-setup uninstall [--purge]
//   voicevox-sapi-setup status

#include "install.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace vvsetup;

struct Options {
    std::wstring action;
    std::wstring source;   // unpacked voicevox_core release
    std::wstring dll;      // DLL to install
    std::wstring models;   // "all", "keep", or "0,3,8"
    std::wstring only;     // comma separated style ids or character names -> voices.txt
    std::wstring dest;
    std::wstring data;
    bool purge = false;
    bool prune = false;     // delete model packs the --models selection did not copy
    bool all = false;       // drop voices.txt, i.e. register every voice the packs offer
    bool fromTemp = false;  // set by the copy this tool leaves in %TEMP% while uninstalling
};

void Out(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    std::vfwprintf(stdout, format, args);
    va_end(args);
    std::fputwc(L'\n', stdout);
    std::fflush(stdout);
}

void Err(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    std::vfwprintf(stderr, format, args);
    va_end(args);
    std::fputwc(L'\n', stderr);
    std::fflush(stderr);
}

std::vector<std::wstring> Split(const std::wstring& text, wchar_t separator) {
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t at = text.find(separator, start);
        const std::wstring piece = text.substr(start, at == std::wstring::npos ? std::wstring::npos
                                                                              : at - start);
        if (!piece.empty()) parts.push_back(piece);
        if (at == std::wstring::npos) break;
        start = at + 1;
    }
    return parts;
}

std::vector<std::wstring> ListVvm(const std::wstring& dir) {
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW found = {};
    HANDLE handle = FindFirstFileW(Join(dir, L"*.vvm").c_str(), &found);
    if (handle == INVALID_HANDLE_VALUE) return files;
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        files.push_back(found.cFileName);
    } while (FindNextFileW(handle, &found) != 0);
    FindClose(handle);
    return files;
}

/**
 * Resolves --models into a list of file names.
 *
 * "keep" (the default) copies nothing, so a re-install does not drag 1.6 GB around again;
 * "all" copies every pack in the source; a comma list of ids copies just those.
 */
bool SelectModels(const std::wstring& spec, const std::wstring& vvmsDir,
                  std::vector<std::wstring>* out, std::wstring* error) {
    if (spec == L"keep" || spec == L"none") return true;

    const std::vector<std::wstring> available = ListVvm(vvmsDir);
    if (available.empty()) {
        *error = L"no .vvm found under " + vvmsDir;
        return false;
    }
    if (spec == L"all") {
        *out = available;
        return true;
    }

    for (const std::wstring& id : Split(spec, L',')) {
        const std::wstring name = id + L".vvm";
        if (!Exists(Join(vvmsDir, name))) {
            *error = L"no such model pack: " + name;
            return false;
        }
        out->push_back(name);
    }
    return true;
}

/** Copies the runtime the engine needs at speak time; returns false on the first failure. */
bool InstallRuntime(const std::wstring& source, const std::wstring& data, const Options& options,
                    std::vector<std::wstring>* copiedModels) {
    const std::wstring core = Join(source, L"c_api\\lib\\voicevox_core.dll");
    const std::wstring onnx = Join(source, L"onnxruntime\\lib\\voicevox_onnxruntime.dll");
    if (!Exists(core) || !Exists(onnx)) {
        Err(L"%s does not look like an unpacked voicevox_core release", source.c_str());
        Err(L"expected c_api\\lib\\voicevox_core.dll and onnxruntime\\lib\\voicevox_onnxruntime.dll");
        return false;
    }

    if (!CopyOne(core, Join(data, L"voicevox_core.dll"))) return false;
    if (!CopyOne(onnx, Join(data, L"voicevox_onnxruntime.dll"))) return false;

    int dictFiles = 0;
    if (!CopyTree(Join(source, L"dict"), Join(data, L"dict"), &dictFiles)) return false;
    Out(L"  runtime: voicevox_core.dll, voicevox_onnxruntime.dll, dict\\ (%d file(s))", dictFiles);

    std::vector<std::wstring> models;
    std::wstring error;
    if (!SelectModels(options.models, Join(source, L"models\\vvms"), &models, &error)) {
        Err(L"%s", error.c_str());
        return false;
    }

    unsigned long long total = 0;
    for (const std::wstring& name : models) {
        const std::wstring from = Join(Join(source, L"models\\vvms"), name);
        WIN32_FILE_ATTRIBUTE_DATA info = {};
        if (GetFileAttributesExW(from.c_str(), GetFileExInfoStandard, &info)) {
            total += ((unsigned long long)info.nFileSizeHigh << 32) | info.nFileSizeLow;
        }
        Out(L"  model:   %s", name.c_str());
        if (!CopyOne(from, Join(Join(data, L"models"), name))) return false;
    }
    if (!models.empty()) Out(L"  models:  %zu pack(s), %s", models.size(), HumanSize(total).c_str());
    if (copiedModels != nullptr) *copiedModels = models;
    return true;
}

// Output stays ASCII even though the console is now UTF-8 (see UseUtf8Console): this text is
// meant to be pasted into a bug report, and it is read on machines whose fonts may not have the
// glyphs. Voice names and paths are the exception — those are printed as they are, because
// mangling them would defeat the purpose.
int Usage() {
    std::fwprintf(stderr,
                  L"voicevox-sapi-setup - registers VOICEVOX voices with SAPI5\n"
                  L"\n"
                  L"  install   [--source <core-dir>] [--dll <path>] [--models all|0,3,8]\n"
                  L"            [--only <names>] [--all] [--dest <dir>] [--data <dir>]\n"
                  L"  uninstall [--purge] [--dest <dir>] [--data <dir>]\n"
                  L"  status\n"
                  L"\n"
                  L"  --source   unpacked voicevox_core release (c_api, onnxruntime, dict, models)\n"
                  L"  --models   which packs to copy; default keeps what is already installed\n"
                  L"  --only     register only these voices: style ids or character names, comma\n"
                  L"             separated, e.g. --only \"female1,female2,male1\" or --only 10005,10001\n"
                  L"  --all      forget --only and register every voice the installed packs offer\n"
                  L"  --prune    delete model packs that --models did not copy\n"
                  L"  --purge    uninstall also deletes the runtime and model packs\n");
    return 2;
}

bool ParseOptions(int argc, wchar_t** argv, Options* options) {
    options->action = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::wstring flag = argv[i];
        const bool hasValue = i + 1 < argc;
        if (flag == L"--purge") {
            options->purge = true;
        } else if (flag == L"--from-temp") {
            options->fromTemp = true;
        } else if (flag == L"--source" && hasValue) {
            options->source = argv[++i];
        } else if (flag == L"--dll" && hasValue) {
            options->dll = argv[++i];
        } else if (flag == L"--models" && hasValue) {
            options->models = argv[++i];
        } else if (flag == L"--only" && hasValue) {
            options->only = argv[++i];
        } else if (flag == L"--all") {
            options->all = true;
        } else if (flag == L"--prune") {
            options->prune = true;
        } else if (flag == L"--dest" && hasValue) {
            options->dest = argv[++i];
        } else if (flag == L"--data" && hasValue) {
            options->data = argv[++i];
        } else {
            Err(L"unknown option: %s", flag.c_str());
            return false;
        }
    }
    return true;
}

std::string Utf8Of(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 0) return std::string();
    std::string out((size_t)size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &out[0], size, nullptr, nullptr);
    return out;
}

/**
 * Writes (or removes) voices.txt, the allow-list the engine reads while registering.
 *
 * <p>The file is written before DllRegisterServer runs because the DLL reads it itself; that way
 * the registry layout stays in exactly one place. No BOM: the first line is a name, and a BOM
 * would end up glued to it.
 */
bool ApplySelection(const Options& options) {
    const std::wstring path = Join(options.data, L"voices.txt");

    if (options.all) {
        if (Exists(path) && !DeleteFileW(path.c_str())) {
            Err(L"cannot delete %s", path.c_str());
            return false;
        }
        Out(L"  voices:  all (voices.txt removed)");
        return true;
    }
    if (options.only.empty()) {
        if (Exists(path)) Out(L"  voices:  limited by %s", path.c_str());
        return true;
    }

    std::wstring text;
    for (const std::wstring& item : Split(options.only, L',')) {
        text += item;
        text += L"\r\n";
    }
    const std::string bytes = Utf8Of(text);

    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) {
        Err(L"cannot write %s", path.c_str());
        return false;
    }
    const size_t written = std::fwrite(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    if (written != bytes.size()) {
        Err(L"short write to %s", path.c_str());
        return false;
    }
    Out(L"  voices:  %zu name(s) in %s", Split(options.only, L',').size(), path.c_str());
    return true;
}

int Install(Options& options) {
    if (!IsElevated()) {
        Err(L"install needs administrator rights (it writes HKLM and Program Files)");
        return 1;
    }
    if (options.dll.empty()) options.dll = Join(ExeDir(), L"VoicevoxSapi.dll");
    if (!Exists(options.dll)) {
        Err(L"DLL not found: %s (pass --dll)", options.dll.c_str());
        return 1;
    }

    Out(L"installing VOICEVOX voices");
    Out(L"  dll:     %s", options.dll.c_str());
    Out(L"  dest:    %s", options.dest.c_str());
    Out(L"  data:    %s", options.data.c_str());

    // Runtime first: registration skips voices whose model pack is missing, so the packs have
    // to be in place before DllRegisterServer runs.
    std::vector<std::wstring> copiedModels;
    if (!options.source.empty()) {
        if (!InstallRuntime(options.source, options.data, options, &copiedModels)) return 1;
    } else if (!Exists(Join(options.data, L"voicevox_core.dll"))) {
        Err(L"no runtime under %s and no --source given", options.data.c_str());
        return 1;
    } else {
        Out(L"  runtime: already installed, left alone (pass --source to refresh)");
    }

    // Drop packs this install did not ask for. Without --models there is no list to keep, and
    // guessing "keep whatever is there" would silently do nothing.
    if (options.prune) {
        if (copiedModels.empty()) {
            Out(L"  prune:   skipped (needs an explicit --models)");
        } else {
            const std::set<std::wstring> keep(copiedModels.begin(), copiedModels.end());
            Out(L"  prune:   removed %d pack(s)", PruneModels(options.data, keep));
        }
    }

    const std::wstring installed = Join(options.dest, L"VoicevoxSapi.dll");
    if (!EnsureDir(options.dest)) {
        Err(L"cannot create %s", options.dest.c_str());
        return 1;
    }
    const WriteResult written = PutFile(options.dll, installed);
    if (written == WriteResult::Failed) {
        Err(L"cannot copy the DLL into %s", options.dest.c_str());
        return 1;
    }
    if (written == WriteResult::NeedsReboot) {
        // A browser had the old engine mapped, so the new build is staged for the next boot.
        // Registering now still has to happen, and it will use the DLL that is in place.
        Out(L"  dll:     staged for the next reboot (in use by a running program)");
    } else {
        Out(L"  dll:     copied to %s", installed.c_str());
    }

    // Leave an uninstaller behind: "Apps & features" must keep working after the build
    // directory (or the folder this was run from) is gone.
    const std::wstring guiUninstaller = Join(ExeDir(), L"VoicevoxSapiUninstall.exe");
    if (Exists(guiUninstaller)) {
        CopyOne(guiUninstaller, Join(options.dest, kUninstallerName));
    }
    const std::wstring selfCopy = Join(options.dest, L"voicevox-sapi-setup.exe");
    if (_wcsicmp(ExePath().c_str(), selfCopy.c_str()) != 0) {
        CopyOne(ExePath(), selfCopy);
    }

    if (!ApplySelection(options)) return 1;

    if (!CallDllExport(installed, "DllRegisterServer", nullptr)) {
        Err(L"DllRegisterServer failed");
        return 1;
    }

    const int voices = CountJapaneseVoices();
    if (voices < 0) {
        Err(L"registration succeeded but SAPI enumeration failed");
        return 1;
    }
    Out(L"  registry: %d ja-JP voice(s) visible to SAPI", voices);
    if (voices == 0) {
        Err(L"no voices registered - is models\\ empty?");
        return 1;
    }

    WriteUninstallEntry(options.dest, options.data, voices);
    Out(L"done. Restart Chrome/Edge: the voice list is cached per process.");
    return 0;
}

int Uninstall(Options& options) {
    if (!IsElevated()) {
        Err(L"uninstall needs administrator rights (it writes HKLM and Program Files)");
        return 1;
    }

    // Same handover as the GUI uninstaller: a running image cannot delete its own directory.
    if (!options.fromTemp && RunningInside(options.dest)) {
        std::wstring arguments = L"uninstall --from-temp";
        if (options.purge) arguments += L" --purge";
        if (!RelaunchFromTemp(arguments, L"voicevox-sapi-setup-uninstall.exe")) {
            Err(L"cannot restart the uninstaller from %%TEMP%%");
            return 1;
        }
        Out(L"  handing over to %%TEMP%% to remove %s", options.dest.c_str());
        return 0;
    }

    const std::wstring installed = Join(options.dest, L"VoicevoxSapi.dll");
    if (Exists(installed)) {
        // Unregister before deleting: DllUnregisterServer walks the same catalog the register
        // pass wrote, so a deleted DLL would leave the tokens behind for good.
        if (!CallDllExport(installed, "DllUnregisterServer", nullptr)) {
            Err(L"could not unregister; close Chrome/Edge and try again");
            return 1;
        }
        Out(L"  registry: tokens removed");
    } else {
        Out(L"  registry: %s missing, nothing to unregister", installed.c_str());
    }

    RemoveUninstallEntry();

    if (!RemoveTree(options.dest)) {
        Err(L"could not remove %s (a host may still have the DLL loaded)", options.dest.c_str());
    } else {
        Out(L"  removed  %s", options.dest.c_str());
    }

    if (options.purge) {
        if (RemoveTree(options.data)) {
            Out(L"  removed  %s (runtime and model packs)", options.data.c_str());
        } else {
            Err(L"could not remove %s", options.data.c_str());
        }
    } else {
        Out(L"  kept     %s (--purge also deletes the runtime and model packs)", options.data.c_str());
    }
    return 0;
}

int Status(const Options& options) {
    const std::wstring installed = Join(options.dest, L"VoicevoxSapi.dll");
    Out(L"dll:      %s", Exists(installed) ? installed.c_str() : L"(not installed)");
    Out(L"runtime:  %s", Exists(Join(options.data, L"voicevox_core.dll")) ? L"present" : L"missing");

    const std::vector<std::wstring> models = ListVvm(Join(options.data, L"models"));
    unsigned long long total = 0;
    for (const std::wstring& name : models) {
        WIN32_FILE_ATTRIBUTE_DATA info = {};
        const std::wstring path = Join(Join(options.data, L"models"), name);
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) {
            total += ((unsigned long long)info.nFileSizeHigh << 32) | info.nFileSizeLow;
        }
    }
    Out(L"models:   %zu pack(s), %s", models.size(), HumanSize(total).c_str());

    const std::wstring selection = Join(options.data, L"voices.txt");
    Out(L"voices:   %d ja-JP voice(s) visible to SAPI%s", CountJapaneseVoices(),
        Exists(selection) ? L" (limited by voices.txt)" : L"");
    return CountJapaneseVoices() > 0 ? 0 : 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // Paths and voice names in this output are Japanese and Chinese; UTF-8 keeps them readable
    // instead of turning them into '?'.
    UseUtf8Console();

    if (argc < 2) return Usage();

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    Options options;
    options.dest = ProgramDir();
    options.data = DataDir();
    options.models = L"keep";

    int result = 2;
    if (ParseOptions(argc, argv, &options)) {
        if (options.action == L"install") {
            result = Install(options);
        } else if (options.action == L"uninstall") {
            result = Uninstall(options);
        } else if (options.action == L"status") {
            result = Status(options);
        } else {
            result = Usage();
        }
    }

    CoUninitialize();
    return result;
}
