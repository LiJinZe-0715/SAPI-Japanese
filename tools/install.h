#pragma once

// Shared plumbing for the two installers:
//
//   voicevox-sapi-setup.exe  the command line tool (dev / advanced, takes --source)
//   VoicevoxSapiSetup.exe    the single-file silent installer (end users double-click it)
//
// They must agree about where files go, what "installed" means and how to uninstall, so that
// logic lives here instead of being copy-pasted into both.

#include <windows.h>
#include <sapi.h>
#include <shlobj.h>

#include <cstdio>
#include <cstdarg>
#include <fcntl.h>
#include <io.h>
#include <set>
#include <string>
#include <vector>

#include "messages.h"

namespace vvsetup {

/**
 * Console output as UTF-8.
 *
 * <p>Without this the C runtime converts wide output to the console's ANSI code page, and every
 * character that page lacks becomes '?' — which is all of Japanese on a Chinese machine and all
 * of Chinese on an English one. Voice names and install paths here are full of both. As a bonus,
 * wide output in UTF-8 mode never truncates, which plain fwprintf does at the first character
 * the code page cannot represent.
 */
inline void UseUtf8Console() {
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_U8TEXT);
    _setmode(_fileno(stderr), _O_U8TEXT);
}

// Both install directories are named after the product, and RemoveTree refuses anything else —
// a recursive delete driven by a command line argument is one typo away from a very bad day.
constexpr wchar_t kLeafName[] = L"VoicevoxSapi";
constexpr wchar_t kUninstallKey[] =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\VoicevoxSapi";
constexpr wchar_t kVersion[] = L"1.0.0";

/**
 * What "Apps & features" runs to remove the product.
 *
 * <p>A separate small GUI executable, not the 176 MB installer and not the console tool: an
 * uninstall should not need a second copy of everything, and double-clicking something in
 * Settings should not flash a black console window at the user.
 */
constexpr wchar_t kUninstallerName[] = L"VoicevoxSapiUninstall.exe";

inline std::wstring Join(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    std::wstring out = a;
    if (out.back() != L'\\' && b.front() != L'\\') out.push_back(L'\\');
    else if (out.back() == L'\\' && b.front() == L'\\') out.pop_back();
    out += b;
    return out;
}

inline std::wstring ExePath() {
    wchar_t buffer[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return buffer;
}

inline std::wstring ExeDir() {
    const std::wstring path = ExePath();
    const size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring(L".") : path.substr(0, slash);
}

inline std::wstring KnownFolder(REFKNOWNFOLDERID id, const wchar_t* fallback) {
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path))) {
        std::wstring out = path;
        CoTaskMemFree(path);
        return out;
    }
    return fallback;
}

inline std::wstring ProgramDir() {
    return Join(KnownFolder(FOLDERID_ProgramFiles, L"C:\\Program Files"), kLeafName);
}

/** Runtime and model packs. %VOICEVOX_SAPI_HOME% overrides it, which is how dev runs work. */
inline std::wstring DataDir() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"VOICEVOX_SAPI_HOME", buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) return buffer;
    return Join(KnownFolder(FOLDERID_ProgramData, L"C:\\ProgramData"), kLeafName);
}

inline bool Exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

inline bool IsDirectory(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

inline bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation = {};
    DWORD size = sizeof(elevation);
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation, size, &size) != 0;
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

inline std::wstring HumanSize(unsigned long long bytes) {
    wchar_t buffer[32];
    if (bytes >= 1024ull * 1024 * 1024) {
        std::swprintf(buffer, 32, L"%.1f GB", (double)bytes / (1024.0 * 1024 * 1024));
    } else {
        std::swprintf(buffer, 32, L"%.0f MB", (double)bytes / (1024.0 * 1024));
    }
    return buffer;
}

inline bool EnsureDir(const std::wstring& path) {
    if (IsDirectory(path)) return true;
    // Create each missing level; a plain CreateDirectory only handles the last one.
    for (size_t i = 3; i <= path.size(); ++i) {
        if (i < path.size() && path[i] != L'\\') continue;
        const std::wstring prefix = path.substr(0, i);
        if (prefix.empty() || prefix.back() == L':') continue;
        if (!IsDirectory(prefix)) CreateDirectoryW(prefix.c_str(), nullptr);
    }
    return IsDirectory(path);
}

/**
 * How a file landed, or why it did not.
 *
 * <p>`NeedsReboot` is not a failure: the file is on disk under a staged name and Windows will
 * move it into place at the next boot. The install is complete, it just is not in effect yet.
 */
enum class WriteResult { Ok, NeedsReboot, Failed };

/**
 * True when `path` is already sitting in the boot-time rename queue.
 *
 * <p>MOVEFILE_DELAY_UNTIL_REBOOT appends to PendingFileRenameOperations without looking at what
 * is already there, so installing twice while a browser holds the DLL used to leave four entries,
 * then six, then eight — 64 of them had piled up on the development machine. They are harmless
 * one at a time (the source is gone by the second boot, so the rename is skipped) but the list
 * grows without bound and every entry is one more chance to install a stale staged copy over a
 * good file.
 *
 * <p>Entries are stored with prefixes that are not worth predicting: `\??\C:\dir\file.new` for a
 * plain move, `!\??\C:\dir\file` for the replace-existing destination, and on this machine every
 * entry additionally carries a `*1` marker written by something other than us. So the path is
 * located by its `\??\` device-name marker instead of by counting prefix characters.
 *
 * <p>The list is walked by its byte length rather than until the first empty string: an entry
 * whose destination is empty means "delete this file at boot", which is how other installers on
 * the machine queue their own cleanup, and stopping there would hide every entry after it.
 */
inline bool QueuedForReboot(const std::wstring& path) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager",
                      0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }

    DWORD type = 0;
    DWORD bytes = 0;
    bool found = false;
    if (RegQueryValueExW(key, L"PendingFileRenameOperations", nullptr, &type, nullptr, &bytes) ==
            ERROR_SUCCESS &&
        type == REG_MULTI_SZ && bytes >= 2 * sizeof(wchar_t)) {
        std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
        if (RegQueryValueExW(key, L"PendingFileRenameOperations", nullptr, &type, (BYTE*)buffer.data(),
                             &bytes) == ERROR_SUCCESS) {
            const size_t count = bytes / sizeof(wchar_t);
            for (size_t at = 0; at < count;) {
                const wchar_t* entry = buffer.data() + at;
                const size_t length = wcslen(entry);
                at += length + 1;
                if (length == 0) continue;

                const wchar_t* leaf = wcsstr(entry, L"\\??\\");
                if (leaf != nullptr) {
                    leaf += 4;
                } else {
                    leaf = entry;
                    if (*leaf == L'!') ++leaf;
                }
                if (*leaf != L'\0' && _wcsicmp(leaf, path.c_str()) == 0) {
                    found = true;
                    break;
                }
            }
        }
    }
    RegCloseKey(key);
    return found;
}

/**
 * Moves an already written file onto `to`, even when a live host has the old file mapped.
 *
 * <p>A rename is allowed to replace a mapped image (the old file is only unlinked; existing
 * mappings keep working), so this is the standard way to update a DLL that is in use.
 */
inline WriteResult PutStaged(const std::wstring& staged, const std::wstring& to) {
    if (MoveFileExW(staged.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) return WriteResult::Ok;

    // Locked hard enough to refuse even a rename: let the next boot do it. If this exact file is
    // already queued, the fresh copy just written is what that pending rename will install, so
    // there is nothing to add — and the staged file has to stay where it is.
    if (QueuedForReboot(staged)) return WriteResult::NeedsReboot;
    if (MoveFileExW(staged.c_str(), to.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_DELAY_UNTIL_REBOOT)) {
        return WriteResult::NeedsReboot;
    }
    DeleteFileW(staged.c_str());
    return WriteResult::Failed;
}

/**
 * Copies `from` to `to`, surviving a destination that is currently loaded by another process.
 *
 * <p>Plain CopyFile fails outright on a locked destination, and that is the normal case here:
 * SAPI loads the engine into the *caller's* process, so merely having Chrome open is enough to
 * block an upgrade. Write the new copy beside the old one first, then rename it into place.
 */
inline WriteResult PutFile(const std::wstring& from, const std::wstring& to) {
    const size_t slash = to.find_last_of(L'\\');
    if (slash != std::wstring::npos && !EnsureDir(to.substr(0, slash))) return WriteResult::Failed;

    if (CopyFileW(from.c_str(), to.c_str(), FALSE)) {
        // The target was free this time, so any staged copy an earlier, blocked attempt left
        // behind is dead weight — and a pending rename pointing at it would later drop the old
        // version back on top of the one just written.
        DeleteFileW((to + L".new").c_str());
        DeleteFileW((to + L".part").c_str());
        return WriteResult::Ok;
    }

    const std::wstring staged = to + L".new";
    if (!CopyFileW(from.c_str(), staged.c_str(), FALSE)) {
        DeleteFileW(staged.c_str());
        return WriteResult::Failed;
    }
    return PutStaged(staged, to);
}

/** CopyOne for the files where a pending reboot is as good as done; false only on real failure. */
inline bool CopyOne(const std::wstring& from, const std::wstring& to) {
    return PutFile(from, to) != WriteResult::Failed;
}

/** Copies a directory tree; `files` counts what it wrote, for the progress display. */
inline bool CopyTree(const std::wstring& from, const std::wstring& to, int* files) {
    if (!IsDirectory(from)) return true;  // optional directory
    if (!EnsureDir(to)) return false;

    WIN32_FIND_DATAW found = {};
    HANDLE handle = FindFirstFileW(Join(from, L"*").c_str(), &found);
    if (handle == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    do {
        const std::wstring name = found.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring src = Join(from, name);
        const std::wstring dst = Join(to, name);
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            ok = CopyTree(src, dst, files) && ok;
        } else {
            ok = CopyOne(src, dst) && ok;
            if (files != nullptr) ++*files;
        }
    } while (FindNextFileW(handle, &found) != 0 && ok);
    FindClose(handle);
    return ok;
}

/** Deletes a tree without questioning the path; only RemoveTree below is allowed to call it. */
inline bool RemoveTreeInner(const std::wstring& path) {
    if (!IsDirectory(path)) return true;

    WIN32_FIND_DATAW found = {};
    HANDLE handle = FindFirstFileW(Join(path, L"*").c_str(), &found);
    if (handle != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = found.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring child = Join(path, name);
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                RemoveTreeInner(child);
            } else {
                SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(handle, &found) != 0);
        FindClose(handle);
    }
    return RemoveDirectoryW(path.c_str()) != 0;
}

/** Deletes a tree, but only one whose leaf is our own directory name. */
inline bool RemoveTree(const std::wstring& path) {
    if (path.size() < 12) return false;
    const size_t slash = path.find_last_of(L'\\');
    const std::wstring leaf = slash == std::wstring::npos ? path : path.substr(slash + 1);
    if (_wcsicmp(leaf.c_str(), kLeafName) != 0) return false;
    return RemoveTreeInner(path);
}

/**
 * Loads the engine DLL and calls one of its COM entry points.
 *
 * <p>regsvr32 is the obvious tool for this, but it is a LOLBin and is blocked on some machines,
 * so both installers call DllRegisterServer themselves. The DLL is released right away: an
 * uninstall that follows has to be able to delete the file.
 */
inline bool CallDllExport(const std::wstring& dllPath, const char* name, HRESULT* result) {
    HMODULE module = LoadLibraryExW(dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module == nullptr) return false;
    auto call = reinterpret_cast<HRESULT(__stdcall*)()>(GetProcAddress(module, name));
    if (call == nullptr) {
        FreeLibrary(module);
        return false;
    }
    const HRESULT hr = call();
    FreeLibrary(module);
    if (result != nullptr) *result = hr;
    return SUCCEEDED(hr);
}

/**
 * Visits every ja-JP voice token SAPI can see. Returns how many were visited, or -1 when the
 * category itself could not be opened (which is different from "zero voices installed").
 */
template <typename Fn>
inline int ForEachJapaneseToken(Fn&& visit) {
    ISpObjectTokenCategory* category = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL,
                                IID_ISpObjectTokenCategory, (void**)&category))) {
        return -1;
    }
    int count = -1;
    if (SUCCEEDED(category->SetId(SPCAT_VOICES, FALSE))) {
        IEnumSpObjectTokens* tokens = nullptr;
        // 411 = ja-JP; the same filter the check tool and Chrome's language list use.
        if (SUCCEEDED(category->EnumTokens(L"Language=411", nullptr, &tokens))) {
            count = 0;
            ISpObjectToken* token = nullptr;
            while (tokens->Next(1, &token, nullptr) == S_OK) {
                visit(token);
                token->Release();
                token = nullptr;
                ++count;
            }
            tokens->Release();
        }
    }
    category->Release();
    return count;
}

/** How many ja-JP voices SAPI can see; -1 when the enumeration itself failed. */
inline int CountJapaneseVoices() {
    return ForEachJapaneseToken([](ISpObjectToken*) {});
}

/**
 * How many distinct characters those voices belong to.
 *
 * <p>The token key is VOICEVOX_<character>_<styleId>, so the character is what sits between the
 * prefix and the last underscore. This is worth a separate number in the "done" dialog: one
 * model pack registers a voice per style, so 0.vvm alone is 10 voices but only 4 characters,
 * and it is the character count that decides whether the voice list feels long.
 */
inline int CountJapaneseCharacters() {
    std::set<std::wstring> characters;
    const int voices = ForEachJapaneseToken([&characters](ISpObjectToken* token) {
        wchar_t* id = nullptr;
        if (FAILED(token->GetId(&id)) || id == nullptr) return;
        std::wstring leaf = id;
        CoTaskMemFree(id);
        const size_t slash = leaf.find_last_of(L'\\');
        if (slash != std::wstring::npos) leaf.erase(0, slash + 1);
        constexpr size_t kPrefix = 9;  // "VOICEVOX_"
        if (leaf.compare(0, kPrefix, L"VOICEVOX_") != 0) return;
        leaf.erase(0, kPrefix);
        const size_t styleSep = leaf.find_last_of(L'_');
        if (styleSep != std::wstring::npos) leaf.erase(styleSep);
        characters.insert(leaf);
    });
    return voices < 0 ? -1 : (int)characters.size();
}

inline bool SetString(HKEY key, const wchar_t* name, const std::wstring& value) {
    return RegSetValueExW(key, name, 0, REG_SZ, (const BYTE*)value.c_str(),
                          (DWORD)((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

inline bool SetDword(HKEY key, const wchar_t* name, DWORD value) {
    return RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE*)&value, sizeof(value)) ==
           ERROR_SUCCESS;
}

/**
 * Registers the product with "Apps & features" so it can be removed like anything else.
 *
 * <p>The uninstall command points at the copy of the installer kept in Program Files, not at
 * wherever the user ran it from — a setup.exe in Downloads is gone by next week.
 */
inline bool WriteUninstallEntry(const std::wstring& programDir, const std::wstring& dataDir,
                                int voiceCount) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS) {
        return false;
    }

    const std::wstring uninstaller = Join(programDir, kUninstallerName);
    const std::wstring cli = Join(programDir, L"voicevox-sapi-setup.exe");

    // Prefer the GUI uninstaller, but never write a command that points at a file that is not
    // there — a dead UninstallString is a support ticket. The command line tool can stand in:
    // it lives next to the DLL and knows the same uninstall.
    std::wstring command;
    if (Exists(uninstaller)) {
        command = L"\"" + uninstaller + L"\"";
    } else {
        command = L"\"" + cli + L"\" uninstall --purge";
    }

    unsigned long long bytes = 0;
    // EstimatedSize is in KB and is only used for the "size" column; the models dominate it.
    const std::wstring models = Join(dataDir, L"models");
    WIN32_FIND_DATAW found = {};
    HANDLE handle = FindFirstFileW(Join(models, L"*.vvm").c_str(), &found);
    if (handle != INVALID_HANDLE_VALUE) {
        do {
            bytes += ((unsigned long long)found.nFileSizeHigh << 32) | found.nFileSizeLow;
        } while (FindNextFileW(handle, &found) != 0);
        FindClose(handle);
    }
    bytes += 125ull * 1024 * 1024;  // runtime + 103 MB dictionary, roughly

    wchar_t count[32] = {};
    std::swprintf(count, 32, L"%d", voiceCount);

    bool ok = SetString(key, L"DisplayName", std::wstring(L"VOICEVOX \u97f3\u58f0 (SAPI5)"));
    ok = ok && SetString(key, L"DisplayVersion", kVersion);
    ok = ok && SetString(key, L"Publisher", L"VOICEVOX");
    ok = ok && SetString(key, L"InstallLocation", programDir);
    ok = ok && SetString(key, L"DisplayIcon", Join(programDir, L"VoicevoxSapi.dll"));
    ok = ok && SetString(key, L"UninstallString", command);
    ok = ok && SetString(key, L"QuietUninstallString", command + L" --quiet");
    ok = ok && SetString(key, L"Comments", std::wstring(L"\u30a4\u30f3\u30b9\u30c8\u30fc\u30eb\u3055\u308c\u3066\u3044\u308b\u97f3\u58f0: ") + count);
    ok = ok && SetDword(key, L"EstimatedSize", (DWORD)(bytes / 1024));
    ok = ok && SetDword(key, L"NoModify", 1);
    ok = ok && SetDword(key, L"NoRepair", 1);
    RegCloseKey(key);
    return ok;
}

inline void RemoveUninstallEntry() {
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, kUninstallKey);
}

/** True when this executable lives inside the directory an uninstall would delete. */
inline bool RunningInside(const std::wstring& dir) {
    const std::wstring exeDir = ExeDir();
    if (exeDir.size() < dir.size()) return false;
    if (_wcsnicmp(exeDir.c_str(), dir.c_str(), dir.size()) != 0) return false;
    // Prefix alone is not enough: "VoicevoxSapiExtra" starts with "VoicevoxSapi" but is a
    // different directory, and treating it as ours would hand the uninstall to a temp copy that
    // then deletes nothing.
    return exeDir.size() == dir.size() || exeDir[dir.size()] == L'\\';
}

/**
 * Uninstalling from inside the directory being deleted cannot work: Windows will not let a
 * running image be removed, so the last file always survives and the directory stays behind.
 * Copy the tool to %TEMP% and let that copy do the removal — this process just gets out of the
 * way. (The temp copy cannot delete itself either; %TEMP% handles that.)
 *
 * @param arguments what to append after the temp copy's path, e.g. L"uninstall --from-temp"
 */
inline bool RelaunchFromTemp(const std::wstring& arguments, const std::wstring& tempName) {
    wchar_t temp[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, temp) == 0) return false;

    const std::wstring copy = Join(temp, tempName);
    if (!CopyFileW(ExePath().c_str(), copy.c_str(), FALSE)) return false;

    std::wstring command = L"\"" + copy + L"\" " + arguments;
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(nullptr, &command[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                        &process)) {
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

/**
 * Deletes model packs that the current install did not bring.
 *
 * <p>A package defines its voice set, so a .vvm left behind by an earlier, larger install is
 * 60–130 MB of dead weight each — and worse than dead weight: it is a voice that comes back the
 * moment something installs without a voices.txt. Only `*.vvm` is touched, so a user's own
 * leftovers of another kind survive.
 */
inline int PruneModels(const std::wstring& dataDir, const std::set<std::wstring>& keep) {
    const std::wstring models = Join(dataDir, L"models");
    WIN32_FIND_DATAW found = {};
    HANDLE handle = FindFirstFileW(Join(models, L"*.vvm").c_str(), &found);
    if (handle == INVALID_HANDLE_VALUE) return 0;

    int removed = 0;
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        const std::wstring name = found.cFileName;
        bool wanted = false;
        for (const std::wstring& kept : keep) {
            if (_wcsicmp(kept.c_str(), name.c_str()) == 0) {
                wanted = true;
                break;
            }
        }
        if (wanted) continue;
        if (DeleteFileW(Join(models, name).c_str())) ++removed;
    } while (FindNextFileW(handle, &found) != 0);
    FindClose(handle);
    return removed;
}

/** Result dialog. Quiet mode is for scripted runs, where a modal box would never be dismissed. */
inline void SayMessage(bool quiet, const std::wstring& text, UINT icon) {
    if (quiet) {
        std::fwprintf(stderr, L"%s\n", text.c_str());
        return;
    }
    MessageBoxW(nullptr, text.c_str(), vvmsg::Title(), MB_OK | icon);
}

/**
 * Removes everything an install put on the machine: the SAPI tokens, the Apps & features entry,
 * the program directory and the runtime/model directory.
 *
 * @param fromTemp set by the copy RelaunchFromTemp left in %TEMP%; without it the tool would
 *                 keep handing itself over forever
 */
inline int UninstallProduct(bool quiet, bool purge, bool fromTemp) {
    const vvmsg::Lang lang = vvmsg::System();

    if (!IsElevated()) {
        SayMessage(quiet, vvmsg::AdminRequired(lang), MB_ICONWARNING);
        return 1;
    }

    const std::wstring programDir = ProgramDir();
    const std::wstring dataDir = DataDir();

    if (!fromTemp && RunningInside(programDir)) {
        std::wstring arguments = L"--from-temp";
        if (quiet) arguments += L" --quiet";
        if (purge) arguments += L" --purge";
        if (!RelaunchFromTemp(arguments, L"voicevox-sapi-uninstall.exe")) {
            SayMessage(quiet, vvmsg::UninstallRestartFailed(lang), MB_ICONERROR);
            return 1;
        }
        return 0;
    }

    const std::wstring installed = Join(programDir, L"VoicevoxSapi.dll");
    if (Exists(installed) && !CallDllExport(installed, "DllUnregisterServer", nullptr)) {
        SayMessage(quiet, vvmsg::UnregisterFailed(lang), MB_ICONERROR);
        return 1;
    }

    RemoveUninstallEntry();
    RemoveTree(programDir);
    if (purge) RemoveTree(dataDir);

    SayMessage(quiet, vvmsg::UninstallDone(lang), MB_ICONINFORMATION);
    return 0;
}

}  // namespace vvsetup
