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
//   voicevox-sapi-setup uninstall [--purge] [--dest <dir>]
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
    std::wstring dest;     // program directory
    std::wstring data;     // always DataDir(): the DLL reads the same fixed directory
    bool purge = false;
    bool prune = false;     // delete model packs the --models selection did not copy
    bool all = false;       // drop voices.txt, i.e. register every voice the packs offer
    bool fromTemp = false;  // set on the private copy this tool hands the uninstall to
    bool destGiven = false; // --dest on the command line; otherwise uninstall finds the install
};

/** "D:\Tools\VoicevoxSapi\" and "D:\Tools\VoicevoxSapi" have to mean the same directory. */
std::wstring WithoutTrailingSlash(std::wstring path) {
    // Stopping at 3 keeps a drive root ("D:\") and the start of a UNC path intact.
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    return path;
}

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
    if (dictFiles == 0) {
        // The engine cannot phonemize without it, and the failure it reports ("open_jtalk") does
        // not say which directory it looked in — so say it here, while the source is known.
        Err(L"no dictionary under %s (expected dict\\open_jtalk_dic_*)",
            Join(source, L"dict").c_str());
        return false;
    }
    Out(L"  runtime: voicevox_core.dll, voicevox_onnxruntime.dll, dict\\ (%d file(s))", dictFiles);

    std::vector<std::wstring> models;
    std::wstring error;
    const std::wstring sourceModels = Join(source, L"models\\vvms");
    if (options.models == L"keep" && ListVvm(Join(data, L"models")).empty()) {
        // "keep" means "leave what is installed alone", and an empty models directory leaves
        // nothing to keep: taking it literally produced an install that registered no voices at
        // all and only said so much later. The generic pack is what the shipping package carries,
        // so it is the default here too — copying all 1.7 GB of packs without being asked would be
        // the other way to surprise somebody.
        if (Exists(Join(sourceModels, L"n0.vvm"))) {
            models.push_back(L"n0.vvm");
            Out(L"  models:  none installed yet, copying n0.vvm (--models all for every voice)");
        } else {
            models = ListVvm(sourceModels);
            if (!models.empty()) {
                Out(L"  models:  none installed yet, copying all %zu pack(s)", models.size());
            }
        }
    } else if (!SelectModels(options.models, sourceModels, &models, &error)) {
        Err(L"%s", error.c_str());
        return false;
    }
    if (models.empty() && ListVvm(Join(data, L"models")).empty()) {
        // Not fatal here — the voice count check after registration is the authority — but this is
        // where the reason is still obvious, so it is worth saying once.
        Err(L"no model packs: the install will register no voices (pass --models all)");
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
                  L"            [--only <names>] [--all] [--prune] [--dest <dir>]\n"
                  L"  uninstall [--purge] [--dest <dir>]\n"
                  L"  status\n"
                  L"\n"
                  L"  --source   unpacked voicevox_core release (c_api, onnxruntime, dict, models)\n"
                  L"  --models   which packs to copy; default keeps what is already installed,\n"
                  L"             and copies n0.vvm when nothing is installed yet\n"
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
        } else if (flag == L"--quiet") {
            // Accepted for the QuietUninstallString; this tool never shows a dialog anyway.
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
            options->destGiven = true;
            // Normalised here, once: a trailing backslash made RemoveTree see an empty leaf name
            // and refuse to delete, and made RunningInside miss the handover to a private copy —
            // so an install made with "D:\Tools\VoicevoxSapi\" could not be uninstalled.
            options->dest = WithoutTrailingSlash(argv[++i]);
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
 * Quotes one argument so CommandLineToArgvW turns it back into exactly the same string.
 *
 * <p>Wrapping in quotes is not enough: a backslash is special in front of a quote, so
 * `--dest "D:\Tools\VoicevoxSapi\"` loses its closing quote and swallows the arguments after it —
 * and a trailing backslash is exactly what people type for a directory. Backslashes are doubled
 * where they precede a quote, and an embedded quote is escaped, which is the rule the C runtime
 * (and therefore the relaunched copy) applies in reverse.
 */
std::wstring QuoteArg(const std::wstring& arg) {
    std::wstring out;
    out.push_back(L'"');
    size_t backslashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
            continue;
        }
        out.append(backslashes, L'\\');
        backslashes = 0;
        out.push_back(c);
    }
    // The closing quote counts as a character a preceding backslash would escape.
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

/**
 * Re-quotes argv for a relaunch.
 *
 * <p>Rebuilt from argv rather than reused from GetCommandLineW: the shell's quoting is already
 * gone by then and re-applying it is guesswork.
 */
std::wstring QuotedArgs(int argc, wchar_t** argv, int from) {
    std::wstring out;
    for (int i = from; i < argc; ++i) {
        if (!out.empty()) out.push_back(L' ');
        out += QuoteArg(argv[i]);
    }
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

    // Checked before anything is written: without it the DLL cannot even be loaded for
    // registration, and the error that produced ("DllRegisterServer failed") sent people looking
    // at the wrong thing entirely.
    if (!HasVcRuntime()) {
        Err(L"the Microsoft Visual C++ 2015-2022 runtime is missing "
            L"(msvcp140.dll / vcruntime140_1.dll not found in System32)");
        Err(L"the engine cannot load without it. Install it, then run this again:");
        Err(L"  https://aka.ms/vs/17/release/vc_redist.x64.exe");
        return 1;
    }

    // RemoveTree refuses to delete a directory that is not named after the product (see
    // install.h), so a --dest elsewhere would produce an install nobody can uninstall. Caught
    // here, where the offending argument is still in front of the user.
    std::wstring leaf = options.dest;
    while (!leaf.empty() && leaf.back() == L'\\') leaf.pop_back();
    const size_t slash = leaf.find_last_of(L'\\');
    const std::wstring leafName = slash == std::wstring::npos ? leaf : leaf.substr(slash + 1);
    if (_wcsicmp(leafName.c_str(), kLeafName) != 0) {
        Err(L"--dest must end in \\%s (got %s)", kLeafName, options.dest.c_str());
        return 1;
    }

    Out(L"installing VOICEVOX voices");
    Out(L"  dll:     %s", options.dll.c_str());
    Out(L"  dest:    %s", options.dest.c_str());
    Out(L"  data:    %s", options.data.c_str());

    // Before anything is copied in: the data directory holds DLLs every speaking process loads,
    // so it must not stay writable by standard users (see SecureDataDir).
    std::wstring why;
    if (!SecureDataDir(options.data, &why)) {
        Err(L"cannot secure the data directory: %s", why.c_str());
        return 1;
    }

    // The resident engine is about to be replaced. It holds its own executable and the core DLL
    // open, and stopping it is what makes the copies below plain overwrites instead of
    // rename-aside plus a reboot note — and, more importantly, an engine started from the old
    // files would keep serving the old build until it exited on idle, so an upgrade would not take
    // effect for up to half an hour. Started again at the end (StartEngine), which also makes the
    // install warm before the first sentence.
    StopServer();

    // Runtime first: registration skips voices whose model pack is missing, so the packs have
    // to be in place before DllRegisterServer runs.
    std::vector<std::wstring> copiedModels;
    if (!options.source.empty()) {
        if (!InstallRuntime(options.source, options.data, options, &copiedModels)) return 1;
    } else {
        // Checked as a whole, not just the core DLL: a dictionary left behind by an older core
        // (or a missing one) is the usual reason a complete-looking install stays silent.
        std::wstring missing;
        if (!RuntimeComplete(options.data, &missing)) {
            Err(L"runtime under %s is incomplete (%s is missing); pass --source to install it",
                options.data.c_str(), missing.c_str());
            return 1;
        }
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

    // The resident engine. Absent in a checkout that has not been rebuilt since it was added, and
    // optional at runtime — the DLL synthesizes locally when it is missing, only slower.
    const std::wstring server = Join(ExeDir(), kServerName);
    if (Exists(server)) {
        const WriteResult writtenServer = PutFile(server, Join(options.dest, kServerName));
        if (writtenServer == WriteResult::Failed) {
            Out(L"  server:  could not be copied, the engine will synthesize locally");
        } else if (writtenServer == WriteResult::NeedsReboot) {
            // A server from an older build is still running; it holds its own image open.
            Out(L"  server:  staged for the next reboot (in use by the running engine)");
        } else {
            Out(L"  server:  copied to %s", Join(options.dest, kServerName).c_str());
        }
    } else {
        Out(L"  server:  not found next to %s, skipping", ExePath().c_str());
    }

    const std::wstring selfCopy = Join(options.dest, L"voicevox-sapi-setup.exe");
    if (_wcsicmp(ExePath().c_str(), selfCopy.c_str()) != 0) {
        CopyOne(ExePath(), selfCopy);
    }

    if (!ApplySelection(options)) return 1;

    // Removable through "Apps & features" from here on, even if registration below fails.
    WriteUninstallEntry(options.dest, options.data, 0);

    // Said out loud rather than folded into WriteUninstallEntry's result: without this task
    // everything still works, but the first sentence after every login is slow, and that is the
    // one symptom this whole install exists to remove.
    const HRESULT startup = WriteStartupEntry(options.dest);
    if (FAILED(startup)) {
        Out(L"  startup: logon task not registered (0x%08lX); first sentence after each login "
            L"will be slow",
            (unsigned long)startup);
    }

    if (!CallDllExport(installed, "DllRegisterServer", nullptr)) {
        Err(L"DllRegisterServer failed");
        return 1;
    }

    const int legacy = CountVoicevoxVoices(SPCAT_VOICES);
    const int oneCore = CountVoicevoxVoices(kOneCoreVoices);
    if (legacy < 0 || oneCore < 0) {
        Err(L"registration succeeded but SAPI enumeration failed");
        return 1;
    }
    Out(L"  registry: %d VOICEVOX voice(s) in Speech, %d in Speech_OneCore", legacy, oneCore);
    const int voices = legacy < oneCore ? legacy : oneCore;
    if (voices == 0) {
        Err(L"no voices registered - is models\\ empty? (see logs\\voicevox-sapi.log)");
        return 1;
    }

    WriteUninstallEntry(options.dest, options.data, voices);
    // Warm before the first sentence: the engine takes ~13 seconds to load its model, and paying
    // that while the user opens a browser is free.
    StartEngine(options.dest);
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
        // --dest goes along explicitly: the temp copy would otherwise look up the default.
        std::wstring arguments = L"uninstall --from-temp --dest \"" + options.dest + L"\"";
        if (options.purge) arguments += L" --purge";
        // A console tool: wait for the copy, so its output lands in this console and its exit
        // code is ours. This executable's own file is then left for the next boot.
        Out(L"  handing over to a private copy to remove %s", options.dest.c_str());
        DWORD copyResult = 0;
        if (!RelaunchFromTemp(arguments, L"voicevox-sapi-setup-uninstall.exe", &copyResult)) {
            Err(L"cannot restart the uninstaller from a private copy");
            return 1;
        }
        return (int)copyResult;
    }
    if (options.fromTemp) CleanUpTempCopy();

    // Same order as the GUI uninstaller, and for the same two reasons. The task first: left
    // behind, it starts an executable that no longer exists at every logon. Then the engine, which
    // holds its own image open — without this RemoveTree can only stage the whole program
    // directory for the next reboot, and the tool reports a pending uninstall instead of a clean
    // one. Both are no-ops on an install that never had them.
    voicevox::schtask::Remove();
    StopServer();

    // Through the DLL when it loads, then a DLL-free sweep either way, so a missing or broken
    // DLL can neither block the uninstall nor leave voices behind.
    const UnregisterResult unregistered = UnregisterProduct(options.dest);
    if (unregistered == UnregisterResult::Failed) {
        Err(L"could not remove every VOICEVOX voice token; try again as administrator");
        return 1;
    }
    if (unregistered == UnregisterResult::Unverified) {
        Out(L"  registry: tokens swept, but SAPI enumeration failed - check the voice list by hand");
    } else {
        Out(L"  registry: tokens removed");
    }

    RemoveUninstallEntry();

    bool pending = false;
    int result = 0;
    if (RemoveTree(options.dest, &pending)) {
        Out(L"  removed  %s", options.dest.c_str());
    } else if (pending) {
        Out(L"  removing %s at the next reboot (a host still has files loaded)", options.dest.c_str());
    } else {
        // The exit code is what a script sees. Printing the error and returning 0 reported success
        // for an uninstall that left the program directory in place.
        Err(L"could not remove %s", options.dest.c_str());
        result = 1;
    }

    if (options.purge) {
        if (RemoveTree(options.data, &pending)) {
            Out(L"  removed  %s (runtime and model packs)", options.data.c_str());
        } else if (pending) {
            Out(L"  removing %s at the next reboot (a host still has files loaded)", options.data.c_str());
        } else {
            Err(L"could not remove %s", options.data.c_str());
            result = 1;
        }
    } else {
        Out(L"  kept     %s (--purge also deletes the runtime and model packs)", options.data.c_str());
    }
    return result;
}

int Status(const Options& options) {
    const std::wstring installed = Join(options.dest, L"VoicevoxSapi.dll");
    Out(L"dll:      %s", Exists(installed) ? installed.c_str() : L"(not installed)");
    // The same completeness check the installer makes: "core DLL is there" was true for installs
    // whose dictionary was missing, which is exactly the state that produces a silent voice.
    std::wstring missing;
    if (RuntimeComplete(options.data, &missing)) {
        Out(L"runtime:  present");
    } else {
        Out(L"runtime:  incomplete (%s is missing)", missing.c_str());
    }

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
    const int voices = CountRegisteredVoices();
    Out(L"voices:   %d VOICEVOX voice(s) visible to SAPI%s", voices,
        Exists(selection) ? L" (limited by voices.txt)" : L"");

    // Part of the diagnosis rather than a footnote: this is the one missing piece that makes every
    // other line here look fine while nothing speaks.
    const bool vcRuntime = HasVcRuntime();
    Out(L"vc++:     %s", vcRuntime ? L"runtime present" : L"MISSING (install vc_redist.x64.exe)");
    return voices > 0 && vcRuntime ? 0 : 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // Paths and voice names in this output are Japanese and Chinese; UTF-8 keeps them readable
    // instead of turning them into '?'.
    HardenDllSearch();
    UseUtf8Console();

    if (argc < 2) return Usage();

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    Options options;
    options.dest = ProgramDir();
    options.data = DataDir();
    options.models = L"keep";

    int result = 2;
    if (ParseOptions(argc, argv, &options)) {
        // Uninstall and status look at the install that exists, wherever --dest put it.
        if (!options.destGiven && options.action != L"install") options.dest = InstalledProgramDir();

        // This tool has no requireAdministrator manifest, on purpose: `status` has to run as
        // whoever invoked it. "Apps & features" starts it without elevation whenever the GUI
        // uninstaller is missing, and a double-clicked install has no elevated context either, so
        // both restart through the UAC prompt instead of failing into a console that closes.
        if (!IsElevated() && (options.action == L"install" || options.action == L"uninstall")) {
            Out(L"administrator rights are needed; restarting elevated");
            DWORD elevated = 1;
            if (RelaunchElevated(QuotedArgs(argc, argv, 1), &elevated)) {
                result = (int)elevated;
            } else {
                Err(L"%s needs administrator rights (it writes HKLM and Program Files)",
                    options.action.c_str());
                result = 1;
            }
        } else if (options.action == L"install") {
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
