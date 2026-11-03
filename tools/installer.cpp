// VoicevoxSapiSetup — the single-file silent installer.
//
// Double-click, approve the UAC prompt, wait a few seconds, get a "done" dialog. Everything the
// engine needs is glued to the back of this executable (see tools/pack-payload.cpp), so there is
// nothing to unzip, no folder to keep next to it and no command line to remember.
//
// It stays quiet during the install on purpose: the payload is a straight file copy, and a
// progress bar that flashes by in three seconds is worse than no window at all. Only the result
// is worth a dialog.
//
//   VoicevoxSapiSetup.exe              install (or reinstall over an existing copy)
//   VoicevoxSapiSetup.exe --uninstall  remove everything this program put on the machine

#include "install.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "payload.h"

namespace {

using namespace vvsetup;
using vvpayload::PayloadEntry;
using vvpayload::PayloadFooter;

// Files whose payload path starts with this go next to the DLL instead of into the data
// directory: the engine itself and the command line uninstaller.
constexpr wchar_t kAppPrefix[] = L"app\\";

// The package's own voices.txt lands under this name; see ApplyDefaultSelection.
constexpr wchar_t kSelectionFile[] = L"voices.txt";
constexpr wchar_t kDefaultSelectionFile[] = L"voices.default.txt";

struct Payload {
    HANDLE file = INVALID_HANDLE_VALUE;
    std::vector<PayloadEntry> entries;
    unsigned long long bytes = 0;

    ~Payload() {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
};

bool g_quiet = false;

bool ReadAt(HANDLE file, unsigned long long offset, void* buffer, DWORD size) {
    LARGE_INTEGER at = {};
    at.QuadPart = (LONGLONG)offset;
    DWORD got = 0;
    return SetFilePointerEx(file, at, nullptr, FILE_BEGIN) &&
           ReadFile(file, buffer, size, &got, nullptr) && got == size;
}

/**
 * Where the payload ends: the start of the Authenticode signature when the file is signed,
 * otherwise the end of the file.
 *
 * <p>signtool appends the certificate table after everything else and records it in the PE
 * security directory (whose "virtual address" is a file offset). Reading the footer from the
 * last bytes of a signed file finds certificate data, so the footer is looked for just before it.
 */
unsigned long long PayloadEnd(HANDLE file, unsigned long long size) {
    IMAGE_DOS_HEADER dos = {};
    if (!ReadAt(file, 0, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) return size;
    IMAGE_NT_HEADERS64 nt = {};
    if (!ReadAt(file, (unsigned long long)dos.e_lfanew, &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return size;
    }
    const IMAGE_DATA_DIRECTORY& security =
        nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY];
    if (security.VirtualAddress == 0 || security.Size == 0) return size;
    if ((unsigned long long)security.VirtualAddress + security.Size > size) return size;
    return security.VirtualAddress;
}

bool OpenPayload(Payload* payload) {
    payload->file = CreateFileW(ExePath().c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (payload->file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(payload->file, &size)) return false;
    const unsigned long long end = PayloadEnd(payload->file, (unsigned long long)size.QuadPart);

    // signtool pads what it signs to a multiple of 8 bytes, so the footer may sit a few zero
    // bytes before the signature.
    PayloadFooter footer = {};
    unsigned long long footerAt = 0;
    bool found = false;
    for (unsigned long long pad = 0; pad < 8 && end >= sizeof(footer) + pad; ++pad) {
        footerAt = end - pad - sizeof(footer);
        if (ReadAt(payload->file, footerAt, &footer, sizeof(footer)) &&
            memcmp(footer.magic, vvpayload::kMagic, sizeof(vvpayload::kMagic)) == 0) {
            found = true;
            break;
        }
    }
    if (!found) return false;

    // Everything below is checked before it is trusted: a damaged download must produce an
    // error dialog, not a gigabyte allocation or a read past the end.
    if (footer.headerOffset > footerAt || footer.headerSize != footerAt - footer.headerOffset ||
        footer.headerSize < sizeof(unsigned int) || footer.headerSize > (64u << 20)) {
        return false;
    }
    std::vector<char> header((size_t)footer.headerSize);
    if (!ReadAt(payload->file, footer.headerOffset, header.data(), (DWORD)header.size())) {
        return false;
    }

    unsigned int count = 0;
    memcpy(&count, header.data(), sizeof(count));
    if (sizeof(count) + (unsigned long long)count * sizeof(PayloadEntry) != header.size()) return false;

    payload->entries.resize(count);
    memcpy(payload->entries.data(), header.data() + sizeof(count), count * sizeof(PayloadEntry));
    for (PayloadEntry& entry : payload->entries) {
        if (wmemchr(entry.path, L'\0', 260) == nullptr) return false;
        const std::wstring path = entry.path;
        if (path.empty() || path[0] == L'\\' || path.find(L':') != std::wstring::npos ||
            path.find(L"..") != std::wstring::npos) {
            return false;
        }
        if (entry.offset > footer.headerOffset || entry.size > footer.headerOffset - entry.offset) {
            return false;
        }
        payload->bytes += entry.size;
    }
    return count > 0;
}

std::wstring DirectoryOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring(L".") : path.substr(0, slash);
}

enum class ExtractResult { Ok, WriteFailed, Corrupt };

/**
 * Streams one payload file out to disk; 1 MB at a time so 60 MB models do not need a buffer.
 *
 * <p>The bytes land in a `.part` file beside the target and are renamed into place afterwards,
 * and only once their checksum matches. Writing the target directly would fail on any file a
 * running program has mapped — and the engine DLL is mapped by every browser that has spoken
 * Japanese since the last install.
 */
ExtractResult Extract(Payload& payload, const PayloadEntry& entry, const std::wstring& dest,
                      bool* needsReboot) {
    const std::wstring staged = dest + L".part";
    if (!EnsureDir(DirectoryOf(staged))) return ExtractResult::WriteFailed;

    HANDLE out = CreateFileW(staged.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) return ExtractResult::WriteFailed;

    LARGE_INTEGER at = {};
    at.QuadPart = (LONGLONG)entry.offset;
    ExtractResult result =
        SetFilePointerEx(payload.file, at, nullptr, FILE_BEGIN) ? ExtractResult::Ok : ExtractResult::Corrupt;

    std::vector<char> buffer(1 << 20);
    unsigned long long left = entry.size;
    unsigned long long hash = vvpayload::kFnvBasis;
    while (result == ExtractResult::Ok && left > 0) {
        const DWORD want = (DWORD)(left < buffer.size() ? left : buffer.size());
        DWORD got = 0;
        if (!ReadFile(payload.file, buffer.data(), want, &got, nullptr) || got == 0) {
            result = ExtractResult::Corrupt;
            break;
        }
        hash = vvpayload::Fnv1a(hash, buffer.data(), got);
        DWORD written = 0;
        if (!WriteFile(out, buffer.data(), got, &written, nullptr) || written != got) {
            result = ExtractResult::WriteFailed;
            break;
        }
        left -= got;
    }
    CloseHandle(out);
    if (result == ExtractResult::Ok && hash != entry.fnv1a) result = ExtractResult::Corrupt;
    if (result != ExtractResult::Ok) {
        DeleteFileW(staged.c_str());
        return result;
    }

    const WriteResult moved = PutStaged(staged, dest);
    if (moved == WriteResult::Failed) return ExtractResult::WriteFailed;
    // The file is in place, so a `.new` left by the command line tool's earlier attempt is both
    // useless and dangerous: its pending rename would put the older binary back at next boot.
    if (moved == WriteResult::Ok) DeleteFileW((dest + L".new").c_str());
    if (moved == WriteResult::NeedsReboot && needsReboot != nullptr) *needsReboot = true;
    return ExtractResult::Ok;
}

std::string ReadBytes(const std::wstring& path) {
    std::string bytes;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr) return bytes;
    char buffer[4096];
    size_t got = 0;
    while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0) bytes.append(buffer, got);
    fclose(file);
    return bytes;
}

/**
 * Installs the package's voices.txt — unless the user has edited theirs.
 *
 * <p>The package's list is extracted as voices.default.txt. voices.txt is replaced with it only
 * when it is missing or still exactly what an earlier package put there; a list the user wrote
 * by hand survives an upgrade. (If that list then selects nothing this package installs, the
 * engine ignores it rather than registering zero voices.)
 */
void ApplyDefaultSelection(const std::wstring& dataDir, const std::string& previousDefault) {
    const std::wstring current = Join(dataDir, kSelectionFile);
    const std::wstring fresh = Join(dataDir, kDefaultSelectionFile);
    if (!Exists(fresh)) return;
    const std::string currentBytes = ReadBytes(current);
    if (!Exists(current) || currentBytes == previousDefault || currentBytes == ReadBytes(fresh)) {
        CopyFileW(fresh.c_str(), current.c_str(), FALSE);
    }
}

void Say(const std::wstring& text, UINT icon) {
    SayMessage(g_quiet, text, icon);
}

int Install() {
    const vvmsg::Lang lang = vvmsg::System();

    Payload payload;
    if (!OpenPayload(&payload)) {
        Say(vvmsg::PayloadMissing(lang), MB_ICONERROR);
        return 1;
    }

    const std::wstring programDir = ProgramDir();
    const std::wstring dataDir = DataDir();

    // Before anything is written into it, so every file lands with the locked-down ACL.
    std::wstring why;
    if (!SecureDataDir(dataDir, &why)) {
        Say(vvmsg::WriteFailed(lang, why), MB_ICONERROR);
        return 1;
    }
    const std::string previousDefault = ReadBytes(Join(dataDir, kDefaultSelectionFile));

    bool needsReboot = false;
    for (const PayloadEntry& entry : payload.entries) {
        const std::wstring path = entry.path;
        std::wstring dest;
        if (path.compare(0, 4, kAppPrefix) == 0) {
            dest = Join(programDir, path.substr(4));
        } else if (_wcsicmp(path.c_str(), kSelectionFile) == 0) {
            dest = Join(dataDir, kDefaultSelectionFile);
        } else {
            dest = Join(dataDir, path);
        }
        const ExtractResult result = Extract(payload, entry, dest, &needsReboot);
        if (result == ExtractResult::Corrupt) {
            Say(vvmsg::PayloadCorrupt(lang, path), MB_ICONERROR);
            return 1;
        }
        if (result != ExtractResult::Ok) {
            Say(vvmsg::WriteFailed(lang, dest), MB_ICONERROR);
            // Whatever did land stays removable, provided the uninstaller itself made it.
            if (Exists(Join(programDir, kUninstallerName))) WriteUninstallEntry(programDir, dataDir, 0);
            return 1;
        }
    }
    ApplyDefaultSelection(dataDir, previousDefault);

    // Registered with "Apps & features" now, before anything below can fail: an install that
    // stops at registration still has files and possibly some voices on the machine, and the
    // user must be able to remove them the normal way. The voice count is filled in at the end.
    WriteUninstallEntry(programDir, dataDir, 0);

    // This package defines which voices exist, so packs from an earlier, bigger install are
    // dropped here rather than left to be registered again by the next install without a
    // voices.txt. Runs after extraction: the packs this build brings are already on disk.
    std::set<std::wstring> keep;
    for (const PayloadEntry& entry : payload.entries) {
        const std::wstring path = entry.path;
        constexpr size_t kModelPrefix = 7;  // "models\"
        if (path.compare(0, kModelPrefix, L"models\\") == 0) keep.insert(path.substr(kModelPrefix));
    }
    if (!keep.empty()) {
        const int pruned = PruneModels(dataDir, keep);
        if (pruned > 0 && g_quiet) {
            std::fwprintf(stderr, L"removed %d model pack(s) this build does not use\n", pruned);
        }
    }

    // Registration reads the model packs to decide which voices exist, so it runs after the
    // files are on disk, never before.
    if (!CallDllExport(Join(programDir, L"VoicevoxSapi.dll"), "DllRegisterServer", nullptr)) {
        Say(vvmsg::RegisterFailed(lang), MB_ICONERROR);
        return 1;
    }

    const int voices = CountRegisteredVoices();
    if (voices <= 0) {
        Say(vvmsg::NoVoices(lang), MB_ICONERROR);
        return 1;
    }

    WriteUninstallEntry(programDir, dataDir, voices);
    if (needsReboot) {
        // Rare since files in use are renamed aside first; this is the case where even that
        // was refused.
        Say(vvmsg::RebootNeeded(lang), MB_ICONINFORMATION);
        return 0;
    }
    Say(vvmsg::InstallDone(lang, voices, CountVoicevoxCharacters()), MB_ICONINFORMATION);
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    HardenDllSearch();
    UseUtf8Console();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Flags are matched as whole arguments; a substring search over the raw command line also
    // matched the executable's own path.
    bool uninstall = false;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv != nullptr && i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--quiet") == 0) g_quiet = true;
        if (_wcsicmp(argv[i], L"--uninstall") == 0) uninstall = true;
    }
    if (argv != nullptr) LocalFree(argv);

    int result = 0;
    if (uninstall) {
        // Uninstalling from this 176 MB file is the escape hatch; the normal path is the small
        // uninstaller the payload drops next to the DLL, which is what Settings points at.
        result = UninstallProduct(g_quiet, true, false);
    } else if (!IsElevated()) {
        // The manifest asks for administrator, so this only happens if elevation was refused.
        Say(vvmsg::AdminRequired(vvmsg::System()), MB_ICONWARNING);
        result = 1;
    } else {
        result = Install();
    }

    CoUninitialize();
    return result;
}
