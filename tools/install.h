#pragma once

// Shared plumbing for the two installers:
//
//   voicevox-sapi-setup.exe  the command line tool (dev / advanced, takes --source)
//   VoicevoxSapiSetup.exe    the single-file silent installer (end users double-click it)
//
// They must agree about where files go, what "installed" means and how to uninstall, so that
// logic lives here instead of being copy-pasted into both.

#include <windows.h>
#include <aclapi.h>
#include <sapi.h>
#include <sddl.h>
#include <shellapi.h>  // CommandLineToArgvW; WIN32_LEAN_AND_MEAN leaves it out of windows.h
#include <shlobj.h>

#include <cstdio>
#include <cstdarg>
#include <fcntl.h>
#include <io.h>
#include <set>
#include <string>
#include <vector>

#include "messages.h"
#include "../src/res/version.h"

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

/**
 * Restricts DLL lookups to System32 for the rest of the process.
 *
 * <p>These tools run elevated from wherever the user put them — Downloads, %TEMP% — and any DLL a
 * system component loads by name later on would otherwise be looked up next to the executable
 * first, where anything else the user downloaded can be waiting. The imports the loader resolves
 * before main are covered at link time (/DEPENDENTLOADFLAG and a static CRT, see CMakeLists.txt);
 * this covers everything loaded after. Call it first thing in main.
 */
inline void HardenDllSearch() {
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    SetDllDirectoryW(L"");
}

// Both install directories are named after the product, and RemoveTree refuses anything else —
// a recursive delete driven by a command line argument is one typo away from a very bad day.
constexpr wchar_t kLeafName[] = L"VoicevoxSapi";
constexpr wchar_t kUninstallKey[] =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\VoicevoxSapi";
// From src/res/version.h, the same macro the VERSIONINFO blocks use.
constexpr wchar_t kVersion[] = L"" VV_VERSION_DISPLAY;

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

/**
 * Runtime and model packs. Must agree with Core::HomeDir() in the DLL: debug builds honour
 * %VOICEVOX_SAPI_HOME% for dev runs, release builds never do (see core.h for why).
 */
inline std::wstring DefaultDataDir() {
    return Join(KnownFolder(FOLDERID_ProgramData, L"C:\\ProgramData"), kLeafName);
}

inline std::wstring DataDir() {
#ifndef NDEBUG
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"VOICEVOX_SAPI_HOME", buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) return buffer;
#endif
    return DefaultDataDir();
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
 * Replaces a directory's DACL with the one in `sddl`, cutting inheritance from the parent.
 * Returns ERROR_SUCCESS or the Win32 error of the step that failed.
 */
inline DWORD ApplySddl(const std::wstring& path, const wchar_t* sddl) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor,
                                                              nullptr)) {
        return GetLastError();
    }
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL dacl = nullptr;
    DWORD result = ERROR_INVALID_SECURITY_DESCR;
    if (GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) && present) {
        result = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                                       DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                       nullptr, nullptr, dacl, nullptr);
    }
    LocalFree(descriptor);
    return result;
}

/**
 * Locks the data directory down to "administrators write, everyone else reads".
 *
 * <p>%ProgramData% lets every standard user create files in any subdirectory, and this one holds
 * voicevox_core.dll, which every process that speaks loads — together with whatever DLL of the
 * right name sits next to it (it imports the VC runtime, which is not shipped here). Inheriting
 * that ACL meant any user could drop a VCRUNTIME140_1.dll here and run code in the next admin's
 * browser. The DACL below is protected (no inheritance), and DLLs that are not ours are removed
 * in case one was planted before this version fixed it.
 *
 * <p>logs\ is the exception: the engine logs from the browser's process, which usually belongs
 * to a standard user, so that one directory stays writable. Nothing is ever loaded from it.
 */
inline bool SecureDataDir(const std::wstring& dataDir, std::wstring* why = nullptr) {
    // SY / BA full control, BU read + execute (0x1200a9) on everything below.
    constexpr wchar_t kDataSddl[] = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)";
    // Same, but BU may modify (0x1301bf): create, append, rotate the log.
    constexpr wchar_t kLogsSddl[] = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1301bf;;;BU)";

    // The first failure is what goes in the error dialog: "cannot write" alone gave no hint
    // which of four steps it was.
    const auto failed = [why](const wchar_t* step, const std::wstring& path, DWORD error) {
        if (why != nullptr && why->empty()) {
            *why = std::wstring(step) + L" " + path + L" (error " + std::to_wstring(error) + L")";
        }
        return false;
    };

    if (!EnsureDir(dataDir)) return failed(L"create", dataDir, GetLastError());

    // Only the real data directory is locked down and swept. A debug build pointed elsewhere by
    // %VOICEVOX_SAPI_HOME% is a developer's own folder — often the unpacked voicevox_core release —
    // and replacing its ACL or deleting "unknown" DLLs in it would damage their working copy.
    // Release builds never get here with anything but the default (DataDir ignores the variable).
    if (CompareStringOrdinal(dataDir.c_str(), -1, DefaultDataDir().c_str(), -1, TRUE) != CSTR_EQUAL) {
        if (!EnsureDir(Join(dataDir, L"logs"))) return failed(L"create", Join(dataDir, L"logs"), GetLastError());
        return true;
    }

    bool ok = true;
    if (const DWORD error = ApplySddl(dataDir, kDataSddl)) ok = failed(L"set ACL on", dataDir, error);

    WIN32_FIND_DATAW found = {};
    HANDLE handle = FindFirstFileW(Join(dataDir, L"*.dll").c_str(), &found);
    if (handle != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = found.cFileName;
            if (_wcsicmp(name.c_str(), L"voicevox_core.dll") == 0 ||
                _wcsicmp(name.c_str(), L"voicevox_onnxruntime.dll") == 0) {
                continue;
            }
            const std::wstring path = Join(dataDir, name);
            SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (!DeleteFileW(path.c_str())) MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        } while (FindNextFileW(handle, &found) != 0);
        FindClose(handle);
    }

    const std::wstring logs = Join(dataDir, L"logs");
    if (!EnsureDir(logs)) {
        ok = failed(L"create", logs, GetLastError());
    } else if (const DWORD error = ApplySddl(logs, kLogsSddl)) {
        ok = failed(L"set ACL on", logs, error);
    }
    // 1.0.0 logged into the data directory itself, where standard users could not append.
    DeleteFileW(Join(dataDir, L"voicevox-sapi.log").c_str());
    return ok;
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
 * Renames `path` to a free `<path>.old[N]` beside it; the name it got goes to `aside`.
 *
 * <p>A leftover from an earlier upgrade is deleted first if nothing maps it any more; one that is
 * still mapped keeps its name and the next number is tried.
 */
inline bool RenameAside(const std::wstring& path, std::wstring* aside) {
    for (int i = 0; i < 32; ++i) {
        const std::wstring candidate = path + L".old" + (i == 0 ? std::wstring() : std::to_wstring(i));
        if (Exists(candidate) && !DeleteFileW(candidate.c_str())) continue;
        if (!MoveFileExW(path.c_str(), candidate.c_str(), 0)) return false;
        *aside = candidate;
        return true;
    }
    return false;
}

/**
 * Moves an already written file onto `to`, even when a live host has the old file mapped.
 *
 * <p>Replacing a mapped image fails with ERROR_ACCESS_DENIED — the replace has to delete the old
 * file, and Windows refuses to delete a file that backs an image section. Renaming that file is
 * allowed, though, and existing mappings keep working from the renamed file. So the old copy is
 * moved aside first and the new one takes its name; the aside copy is deleted at once if the
 * host is already gone, otherwise at the next boot.
 */
inline WriteResult PutStaged(const std::wstring& staged, const std::wstring& to) {
    if (MoveFileExW(staged.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) return WriteResult::Ok;

    std::wstring aside;
    if (Exists(to) && RenameAside(to, &aside)) {
        if (MoveFileExW(staged.c_str(), to.c_str(), 0)) {
            if (!DeleteFileW(aside.c_str()) && !QueuedForReboot(aside)) {
                MoveFileExW(aside.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            }
            return WriteResult::Ok;
        }
        // Put the old copy back so the product keeps working, then fall through.
        MoveFileExW(aside.c_str(), to.c_str(), 0);
    }

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

/**
 * Deletes a tree without questioning the path; only RemoveTree below is allowed to call it.
 *
 * <p>Anything that cannot go now — the engine DLL is mapped by every browser that has spoken —
 * is queued for deletion at the next boot (files first, so the directory is empty by the time
 * its own entry runs) and `pendingReboot` is set.
 */
inline bool RemoveTreeInner(const std::wstring& path, bool* pendingReboot) {
    if (!IsDirectory(path)) return true;

    WIN32_FIND_DATAW found = {};
    HANDLE handle = FindFirstFileW(Join(path, L"*").c_str(), &found);
    if (handle != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = found.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring child = Join(path, name);
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                RemoveTreeInner(child, pendingReboot);
            } else {
                SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                // An upgrade may already have queued this file (a renamed-aside .old); queueing
                // it again only lengthens the boot-time list.
                if (!DeleteFileW(child.c_str()) &&
                    (QueuedForReboot(child) ||
                     MoveFileExW(child.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT))) {
                    *pendingReboot = true;
                }
            }
        } while (FindNextFileW(handle, &found) != 0);
        FindClose(handle);
    }
    if (RemoveDirectoryW(path.c_str())) return true;
    if (QueuedForReboot(path) || MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) {
        *pendingReboot = true;
    }
    return false;
}

/**
 * Deletes a tree, but only one whose leaf is our own directory name.
 *
 * @return true when the tree is gone now; false when something is left, in which case
 *         `pendingReboot` says whether Windows will finish the job at the next boot
 */
inline bool RemoveTree(const std::wstring& path, bool* pendingReboot = nullptr) {
    bool pending = false;
    if (pendingReboot != nullptr) *pendingReboot = false;
    if (path.size() < 12) return false;
    const size_t slash = path.find_last_of(L'\\');
    const std::wstring leaf = slash == std::wstring::npos ? path : path.substr(slash + 1);
    if (_wcsicmp(leaf.c_str(), kLeafName) != 0) return false;
    const bool removed = RemoveTreeInner(path, &pending);
    if (pendingReboot != nullptr) *pendingReboot = pending;
    return removed;
}

/**
 * Loads the engine DLL and calls one of its COM entry points.
 *
 * <p>regsvr32 is the obvious tool for this, but it is a LOLBin and is blocked on some machines,
 * so both installers call DllRegisterServer themselves. The DLL is released right away: an
 * uninstall that follows has to be able to delete the file.
 */
inline bool CallDllExport(const std::wstring& dllPath, const char* name, HRESULT* result) {
    // Its own directory and System32 only (see HardenDllSearch).
    HMODULE module = LoadLibraryExW(dllPath.c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
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

/** The category Chrome, Edge and Narrator enumerate first; see src/registry.cpp. */
constexpr wchar_t kOneCoreVoices[] = L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech_OneCore\\Voices";

/**
 * Visits every VOICEVOX voice token in one SAPI category. Returns how many were visited, or -1
 * when the category itself could not be opened (which is different from "zero voices installed").
 *
 * <p>Only our own tokens count. A plain "Language=411" count also picks up Microsoft's Japanese
 * voices (Haruka ships with every Japanese Windows), which made "at least one voice" pass even
 * when this install had registered nothing. `visit` gets the token and the character part of its
 * key, VOICEVOX_<character>_<styleId>.
 */
template <typename Fn>
inline int ForEachVoicevoxToken(const wchar_t* categoryId, Fn&& visit) {
    ISpObjectTokenCategory* category = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL,
                                IID_ISpObjectTokenCategory, (void**)&category))) {
        return -1;
    }
    int count = -1;
    if (SUCCEEDED(category->SetId(categoryId, FALSE))) {
        IEnumSpObjectTokens* tokens = nullptr;
        // 411 = ja-JP; the same filter the check tool and Chrome's language list use.
        if (SUCCEEDED(category->EnumTokens(L"Language=411", nullptr, &tokens))) {
            count = 0;
            ISpObjectToken* token = nullptr;
            while (tokens->Next(1, &token, nullptr) == S_OK) {
                wchar_t* id = nullptr;
                if (SUCCEEDED(token->GetId(&id)) && id != nullptr) {
                    std::wstring leaf = id;
                    CoTaskMemFree(id);
                    const size_t slash = leaf.find_last_of(L'\\');
                    if (slash != std::wstring::npos) leaf.erase(0, slash + 1);
                    constexpr size_t kPrefix = 9;  // "VOICEVOX_"
                    if (leaf.compare(0, kPrefix, L"VOICEVOX_") == 0) {
                        leaf.erase(0, kPrefix);
                        const size_t styleSep = leaf.find_last_of(L'_');
                        if (styleSep != std::wstring::npos) leaf.erase(styleSep);
                        visit(token, leaf);
                        ++count;
                    }
                }
                token->Release();
                token = nullptr;
            }
            tokens->Release();
        }
    }
    category->Release();
    return count;
}

/** How many VOICEVOX voices a category holds; -1 when the enumeration itself failed. */
inline int CountVoicevoxVoices(const wchar_t* categoryId = SPCAT_VOICES) {
    return ForEachVoicevoxToken(categoryId, [](ISpObjectToken*, const std::wstring&) {});
}

/**
 * How many distinct characters those voices belong to.
 *
 * <p>This is worth a separate number in the "done" dialog: one model pack registers a voice per
 * style, so 0.vvm alone is 10 voices but only 4 characters, and it is the character count that
 * decides whether the voice list feels long.
 */
inline int CountVoicevoxCharacters() {
    std::set<std::wstring> characters;
    const int voices = ForEachVoicevoxToken(
        SPCAT_VOICES,
        [&characters](ISpObjectToken*, const std::wstring& character) { characters.insert(character); });
    return voices < 0 ? -1 : (int)characters.size();
}

/**
 * Voices visible in both categories, or 0 / -1 when either is missing them.
 *
 * <p>Registration writes both; a voice only in the legacy one is invisible to every browser, so
 * checking just SPCAT_VOICES would call a half-registered install a success.
 */
inline int CountRegisteredVoices() {
    const int legacy = CountVoicevoxVoices(SPCAT_VOICES);
    const int oneCore = CountVoicevoxVoices(kOneCoreVoices);
    if (legacy < 0 || oneCore < 0) return -1;
    return legacy < oneCore ? legacy : oneCore;
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

/**
 * Where the product was actually installed: the InstallLocation the install recorded, so an
 * install made with `voicevox-sapi-setup install --dest <dir>` is removed from <dir> and not from
 * the default location. Anything that is not one of our own directories falls back to the
 * default — this value decides what gets deleted.
 */
inline std::wstring InstalledProgramDir() {
    HKEY key = nullptr;
    std::wstring location;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        wchar_t buffer[MAX_PATH] = {};
        DWORD bytes = sizeof(buffer) - sizeof(wchar_t);
        DWORD type = 0;
        if (RegQueryValueExW(key, L"InstallLocation", nullptr, &type, (BYTE*)buffer, &bytes) ==
                ERROR_SUCCESS &&
            type == REG_SZ) {
            location = buffer;
        }
        RegCloseKey(key);
    }
    while (!location.empty() && location.back() == L'\\') location.pop_back();
    const size_t slash = location.find_last_of(L'\\');
    if (slash == std::wstring::npos || _wcsicmp(location.c_str() + slash + 1, kLeafName) != 0 ||
        !IsDirectory(location)) {
        return ProgramDir();
    }
    return location;
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
 * Copy the tool somewhere else and let that copy do the removal — this process just gets out of
 * the way.
 *
 * <p>Not %TEMP%: the copy runs elevated, and %TEMP% is writable by every non-elevated process of
 * the same user, so a DLL dropped there beforehand would be loaded into an administrator process.
 * The copy goes into a fresh directory under %ProgramData% with an unguessable name, created in
 * one call with an administrators-only ACL — there is no moment at which anyone else could put a
 * file in it. The copy removes itself and the directory at the next boot (CleanUpTempCopy).
 *
 * <p>With `exitCode` the caller waits for the copy and gets its exit code, and the copy writes to
 * the caller's console and redirected output. That is what scripted uninstalls need: returning 0
 * at once used to report success for an uninstall that had not even started, let alone worked.
 * The price is that this executable is still running while the copy deletes its directory, so it
 * is left for the next boot. Without `exitCode` (an interactive uninstall, where the copy shows
 * its own dialog) the caller just leaves, and its directory goes at once.
 *
 * @param arguments what to append after the copy's path, e.g. L"uninstall --from-temp"
 */
inline bool RelaunchFromTemp(const std::wstring& arguments, const std::wstring& tempName,
                             DWORD* exitCode = nullptr) {
    GUID guid = {};
    if (FAILED(CoCreateGuid(&guid))) return false;
    wchar_t guidText[64] = {};
    StringFromGUID2(guid, guidText, 64);
    const std::wstring dir =
        Join(KnownFolder(FOLDERID_ProgramData, L"C:\\ProgramData"), std::wstring(L"VoicevoxSapi-uninstall-") + guidText);

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1, &descriptor, nullptr)) {
        return false;
    }
    SECURITY_ATTRIBUTES attributes = {sizeof(attributes), descriptor, FALSE};
    const BOOL created = CreateDirectoryW(dir.c_str(), &attributes);
    LocalFree(descriptor);
    if (!created) return false;

    // From here on a failure must not leave the throwaway directory behind.
    const std::wstring copy = Join(dir, tempName);
    const auto abandon = [&]() {
        DeleteFileW(copy.c_str());
        RemoveDirectoryW(dir.c_str());
        return false;
    };
    if (!CopyFileW(ExePath().c_str(), copy.c_str(), TRUE)) return abandon();

    std::wstring command = L"\"" + copy + L"\" " + arguments;
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    BOOL inherit = FALSE;
    if (exitCode != nullptr) {
        // Hand down this process's console / redirections, so the copy's messages reach whoever
        // is reading ours. A GUI process may have none; then there is nothing to pass.
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        if (startup.hStdOutput != nullptr || startup.hStdError != nullptr) {
            startup.dwFlags |= STARTF_USESTDHANDLES;
            inherit = TRUE;
        }
    }
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(nullptr, &command[0], nullptr, nullptr, inherit, 0, nullptr, nullptr, &startup,
                        &process)) {
        return abandon();
    }
    CloseHandle(process.hThread);
    if (exitCode != nullptr) {
        WaitForSingleObject(process.hProcess, INFINITE);
        if (!GetExitCodeProcess(process.hProcess, exitCode)) *exitCode = 1;
    }
    CloseHandle(process.hProcess);
    return true;
}

/** Run by the copy RelaunchFromTemp made: queues the copy and its directory for deletion. */
inline void CleanUpTempCopy() {
    const std::wstring dir = ExeDir();
    const size_t slash = dir.find_last_of(L'\\');
    const std::wstring leaf = slash == std::wstring::npos ? dir : dir.substr(slash + 1);
    // Only ever our own throwaway directory, never wherever --from-temp was typed by hand.
    if (leaf.compare(0, 23, L"VoicevoxSapi-uninstall-") != 0) return;
    MoveFileExW(ExePath().c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    MoveFileExW(dir.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
}

/** True when `text` is a CLSID of the shape every build of the engine registers. */
inline bool IsVoicevoxClsid(const std::wstring& text) {
    // Must match ClsidForStyle in src/catalog.cpp. Duplicated on purpose: this side has to work
    // when the DLL cannot be loaded at all.
    CLSID clsid = {};
    if (text.empty() || FAILED(CLSIDFromString(text.c_str(), &clsid))) return false;
    return (clsid.Data1 & 0xFFFFFF00u) == 0x56584B00u && clsid.Data2 == 0x9A31 &&
           clsid.Data3 == 0x4C7E && clsid.Data4[0] == 0x8D && clsid.Data4[1] == 0x2B &&
           clsid.Data4[2] == 0x51 && clsid.Data4[4] == 0 && clsid.Data4[5] == 0 &&
           clsid.Data4[6] == 0 && clsid.Data4[7] == 1;
}

/**
 * Deletes every VOICEVOX voice token and the CLSID behind it, without the engine DLL.
 *
 * <p>The uninstall asks the DLL first (DllUnregisterServer), but a DLL that is missing or will
 * not load must not leave the voices registered — pointing at nothing — or make the product
 * impossible to remove. This is the same sweep the DLL does, keyed on the token name prefix and
 * the CLSID shape rather than on a catalog.
 */
inline void SweepVoiceRegistrations() {
    const wchar_t* const roots[] = {
        L"SOFTWARE\\Microsoft\\Speech\\Voices\\Tokens",
        L"SOFTWARE\\Microsoft\\Speech_OneCore\\Voices\\Tokens",
    };
    for (const wchar_t* root : roots) {
        std::vector<std::wstring> names;
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, root, 0, KEY_ENUMERATE_SUB_KEYS, &key) == ERROR_SUCCESS) {
            wchar_t name[256];
            for (DWORD index = 0;; ++index) {
                DWORD length = 256;
                if (RegEnumKeyExW(key, index, name, &length, nullptr, nullptr, nullptr, nullptr) !=
                    ERROR_SUCCESS) {
                    break;
                }
                names.emplace_back(name, length);
            }
            RegCloseKey(key);
        }
        for (const std::wstring& name : names) {
            if (name != L"VOICEVOX" && name.compare(0, 9, L"VOICEVOX_") != 0) continue;
            const std::wstring token = std::wstring(root) + L"\\" + name;
            wchar_t clsid[128] = {};
            DWORD bytes = sizeof(clsid) - sizeof(wchar_t);
            if (RegGetValueW(HKEY_LOCAL_MACHINE, token.c_str(), L"CLSID", RRF_RT_REG_SZ, nullptr,
                             clsid, &bytes) == ERROR_SUCCESS &&
                IsVoicevoxClsid(clsid)) {
                RegDeleteTreeW(HKEY_LOCAL_MACHINE,
                               (std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + clsid).c_str());
            }
            RegDeleteTreeW(HKEY_LOCAL_MACHINE, token.c_str());
        }
    }
}

/**
 * Unregisters the voices: through the DLL when it loads, then the DLL-free sweep regardless.
 * True when no VOICEVOX voice is left in either category.
 */
inline bool UnregisterProduct(const std::wstring& programDir) {
    const std::wstring installed = Join(programDir, L"VoicevoxSapi.dll");
    if (Exists(installed)) CallDllExport(installed, "DllUnregisterServer", nullptr);
    SweepVoiceRegistrations();
    return CountVoicevoxVoices(SPCAT_VOICES) <= 0 && CountVoicevoxVoices(kOneCoreVoices) <= 0;
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
 * @param fromTemp set by the copy RelaunchFromTemp made; without it the tool would
 *                 keep handing itself over forever
 */
inline int UninstallProduct(bool quiet, bool purge, bool fromTemp) {
    const vvmsg::Lang lang = vvmsg::System();

    if (!IsElevated()) {
        SayMessage(quiet, vvmsg::AdminRequired(lang), MB_ICONWARNING);
        return 1;
    }

    const std::wstring programDir = InstalledProgramDir();
    const std::wstring dataDir = DataDir();

    if (!fromTemp && RunningInside(programDir)) {
        std::wstring arguments = L"--from-temp";
        if (quiet) arguments += L" --quiet";
        if (purge) arguments += L" --purge";
        // Quiet runs are scripted: wait, and pass the copy's result on (see RelaunchFromTemp).
        DWORD copyResult = 0;
        if (!RelaunchFromTemp(arguments, L"voicevox-sapi-uninstall.exe", quiet ? &copyResult : nullptr)) {
            SayMessage(quiet, vvmsg::UninstallRestartFailed(lang), MB_ICONERROR);
            return 1;
        }
        return (int)copyResult;
    }

    if (fromTemp) CleanUpTempCopy();

    if (!UnregisterProduct(programDir)) {
        SayMessage(quiet, vvmsg::UnregisterFailed(lang), MB_ICONERROR);
        return 1;
    }

    RemoveUninstallEntry();
    bool pending = false;
    bool pendingData = false;
    RemoveTree(programDir, &pending);
    if (purge) RemoveTree(dataDir, &pendingData);

    SayMessage(quiet,
               pending || pendingData ? vvmsg::UninstallRebootNeeded(lang) : vvmsg::UninstallDone(lang),
               MB_ICONINFORMATION);
    return 0;
}

}  // namespace vvsetup
