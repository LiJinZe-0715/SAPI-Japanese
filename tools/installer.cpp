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

namespace {

using namespace vvsetup;

#pragma pack(push, 1)
struct PayloadEntry {
    wchar_t path[260];
    unsigned long long offset;
    unsigned long long size;
};

struct PayloadFooter {
    char magic[8];
    unsigned long long headerOffset;
    unsigned long long headerSize;
};
#pragma pack(pop)

// Must match tools/pack-payload.cpp.
constexpr char kMagic[8] = {'V', 'V', 'S', 'P', 'A', 'Y', 'L', '1'};

// Files whose payload path starts with this go next to the DLL instead of into the data
// directory: the engine itself and the command line uninstaller.
constexpr wchar_t kAppPrefix[] = L"app\\";

struct Payload {
    HANDLE file = INVALID_HANDLE_VALUE;
    std::vector<PayloadEntry> entries;
    unsigned long long bytes = 0;

    ~Payload() {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
};

bool g_quiet = false;

bool OpenPayload(Payload* payload) {
    payload->file = CreateFileW(ExePath().c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (payload->file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(payload->file, &size)) return false;
    if (size.QuadPart < (LONGLONG)sizeof(PayloadFooter)) return false;

    PayloadFooter footer = {};
    LARGE_INTEGER at = {};
    at.QuadPart = size.QuadPart - (LONGLONG)sizeof(footer);
    if (!SetFilePointerEx(payload->file, at, nullptr, FILE_BEGIN)) return false;
    DWORD got = 0;
    if (!ReadFile(payload->file, &footer, sizeof(footer), &got, nullptr) ||
        got != sizeof(footer)) {
        return false;
    }
    if (memcmp(footer.magic, kMagic, sizeof(kMagic)) != 0) return false;
    if (footer.headerOffset >= (unsigned long long)size.QuadPart) return false;

    // The header is small (a few hundred bytes per file); read it in one go and walk it.
    std::vector<char> header((size_t)footer.headerSize);
    at.QuadPart = (LONGLONG)footer.headerOffset;
    if (!SetFilePointerEx(payload->file, at, nullptr, FILE_BEGIN)) return false;
    if (!ReadFile(payload->file, header.data(), (DWORD)header.size(), &got, nullptr) ||
        got != header.size()) {
        return false;
    }

    unsigned int count = 0;
    memcpy(&count, header.data(), sizeof(count));
    const size_t expected = sizeof(count) + count * sizeof(PayloadEntry);
    if (expected > header.size()) return false;

    payload->entries.resize(count);
    memcpy(payload->entries.data(), header.data() + sizeof(count), count * sizeof(PayloadEntry));
    for (const PayloadEntry& entry : payload->entries) payload->bytes += entry.size;
    return count > 0;
}

std::wstring DirectoryOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring(L".") : path.substr(0, slash);
}

/**
 * Streams one payload file out to disk; 1 MB at a time so 60 MB models do not need a buffer.
 *
 * <p>The bytes land in a `.part` file beside the target and are renamed into place afterwards.
 * Writing the target directly would fail on any file a running program has mapped — and the
 * engine DLL is mapped by every browser that has spoken Japanese since the last install.
 */
bool Extract(Payload& payload, const PayloadEntry& entry, const std::wstring& dest,
             bool* needsReboot) {
    const std::wstring staged = dest + L".part";
    if (!EnsureDir(DirectoryOf(staged))) return false;

    HANDLE out = CreateFileW(staged.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER at = {};
    at.QuadPart = (LONGLONG)entry.offset;
    bool ok = SetFilePointerEx(payload.file, at, nullptr, FILE_BEGIN) != 0;

    std::vector<char> buffer(1 << 20);
    unsigned long long left = entry.size;
    while (ok && left > 0) {
        const DWORD want = (DWORD)(left < buffer.size() ? left : buffer.size());
        DWORD got = 0;
        if (!ReadFile(payload.file, buffer.data(), want, &got, nullptr) || got == 0) {
            ok = false;
            break;
        }
        DWORD written = 0;
        if (!WriteFile(out, buffer.data(), got, &written, nullptr) || written != got) {
            ok = false;
            break;
        }
        left -= got;
    }
    CloseHandle(out);
    if (!ok) {
        DeleteFileW(staged.c_str());
        return false;
    }

    const WriteResult moved = PutStaged(staged, dest);
    if (moved == WriteResult::Failed) return false;
    // The file is in place, so a `.new` left by the command line tool's earlier attempt is both
    // useless and dangerous: its pending rename would put the older binary back at next boot.
    if (moved == WriteResult::Ok) DeleteFileW((dest + L".new").c_str());
    if (moved == WriteResult::NeedsReboot && needsReboot != nullptr) *needsReboot = true;
    return true;
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

    bool needsReboot = false;
    for (const PayloadEntry& entry : payload.entries) {
        const std::wstring path = entry.path;
        std::wstring dest;
        if (path.compare(0, 4, kAppPrefix) == 0) {
            dest = Join(programDir, path.substr(4));
        } else {
            dest = Join(dataDir, path);
        }
        if (!Extract(payload, entry, dest, &needsReboot)) {
            Say(vvmsg::WriteFailed(lang, dest), MB_ICONERROR);
            return 1;
        }
    }

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

    const int voices = CountJapaneseVoices();
    if (voices <= 0) {
        Say(vvmsg::NoVoices(lang), MB_ICONERROR);
        return 1;
    }

    WriteUninstallEntry(programDir, dataDir, voices);
    if (needsReboot) {
        // The voices are already usable; only the engine binary itself is pending.
        Say(vvmsg::RebootNeeded(lang), MB_ICONINFORMATION);
        return 0;
    }
    Say(vvmsg::InstallDone(lang, voices, CountJapaneseCharacters()), MB_ICONINFORMATION);
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    UseUtf8Console();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const std::wstring command = GetCommandLineW();
    g_quiet = command.find(L"--quiet") != std::wstring::npos;

    int result = 0;
    if (command.find(L"--uninstall") != std::wstring::npos) {
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
