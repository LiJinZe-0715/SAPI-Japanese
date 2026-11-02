// voicevox-pack-payload — glues the runtime, the dictionary and the model packs onto the back of
// the installer executable, so the result is one file you can double-click.
//
// Appending after the PE image is the same trick 7-Zip's SFX and every "portable" installer use:
// Windows only maps the image described by the headers, the tail is just data the program can
// seek to. No compression on purpose — the payload is a 100 MB binary dictionary and a 60 MB
// ONNX model, which barely compress, and a corrupt byte in a compressed stream means a failed
// install instead of a failed file copy.
//
//   voicevox-pack-payload <spec.txt> <installer.exe> <output.exe>
//
// Spec file (UTF-8, one rule per line, '#' starts a comment):
//   file|<dest relative path>|<source path>
//   dir |<dest relative dir> |<source dir>          (recursive)
//
// Layout written:
//   [installer bytes][file data ...][header][footer]
//   footer: "VVSPAYL1" + u64 headerOffset + u64 headerSize
//   header: u32 count + count * PayloadEntry

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

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

// Must match installer.cpp.
constexpr char kMagic[8] = {'V', 'V', 'S', 'P', 'A', 'Y', 'L', '1'};

struct Item {
    std::wstring dest;
    std::wstring source;
};

std::string ReadAll(const std::wstring& path) {
    std::string text;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr) return text;
    char buffer[8192];
    size_t read = 0;
    while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, read);
    fclose(file);
    return text;
}

std::wstring ToWide(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    std::wstring out((size_t)size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &out[0], size);
    return out;
}

std::wstring Trim(const std::wstring& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == L' ' || text[begin] == L'\t' || text[begin] == L'\r'))
        ++begin;
    while (end > begin && (text[end - 1] == L' ' || text[end - 1] == L'\t' || text[end - 1] == L'\r'))
        --end;
    return text.substr(begin, end - begin);
}

std::vector<std::wstring> Split(const std::wstring& text, wchar_t separator) {
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t at = text.find(separator, start);
        parts.push_back(Trim(text.substr(start, at == std::wstring::npos ? std::wstring::npos
                                                                       : at - start)));
        if (at == std::wstring::npos) break;
        start = at + 1;
    }
    return parts;
}

bool CollectDir(const std::wstring& sourceDir, const std::wstring& destPrefix, std::vector<Item>* out) {
    WIN32_FIND_DATAW found = {};
    HANDLE handle = FindFirstFileW((sourceDir + L"\\*").c_str(), &found);
    if (handle == INVALID_HANDLE_VALUE) {
        std::fwprintf(stderr, L"cannot list %s\n", sourceDir.c_str());
        return false;
    }
    bool ok = true;
    do {
        const std::wstring name = found.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring source = sourceDir + L"\\" + name;
        const std::wstring dest = destPrefix.empty() ? name : destPrefix + L"\\" + name;
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            ok = CollectDir(source, dest, out) && ok;
        } else {
            out->push_back(Item{dest, source});
        }
    } while (FindNextFileW(handle, &found) != 0 && ok);
    FindClose(handle);
    return ok;
}

bool ReadSpec(const std::wstring& path, std::vector<Item>* items) {
    const std::string raw = ReadAll(path);
    if (raw.empty()) {
        std::fwprintf(stderr, L"cannot read %s\n", path.c_str());
        return false;
    }

    // Strip a UTF-8 BOM: Notepad writes one, and it would end up inside the first keyword.
    std::string text = raw;
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF) {
        text = text.substr(3);
    }

    size_t start = 0;
    while (start <= text.size()) {
        const size_t at = text.find('\n', start);
        const std::wstring line =
            Trim(ToWide(text.substr(start, at == std::string::npos ? std::string::npos : at - start)));
        if (!line.empty() && line[0] != L'#') {
            const std::vector<std::wstring> parts = Split(line, L'|');
            if (parts.size() != 3) {
                std::fwprintf(stderr, L"bad spec line: %s\n", line.c_str());
                return false;
            }
            if (parts[0] == L"file") {
                items->push_back(Item{parts[1], parts[2]});
            } else if (parts[0] == L"dir") {
                if (!CollectDir(parts[2], parts[1], items)) return false;
            } else {
                std::fwprintf(stderr, L"unknown rule: %s\n", parts[0].c_str());
                return false;
            }
        }
        if (at == std::string::npos) break;
        start = at + 1;
    }
    return !items->empty();
}

bool AppendFile(HANDLE out, const std::wstring& source, unsigned long long* offset,
                unsigned long long* size) {
    HANDLE in = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (in == INVALID_HANDLE_VALUE) {
        std::fwprintf(stderr, L"cannot open %s (error %lu)\n", source.c_str(), GetLastError());
        return false;
    }
    LARGE_INTEGER length = {};
    GetFileSizeEx(in, &length);
    *offset = 0;
    *size = (unsigned long long)length.QuadPart;

    std::vector<char> buffer(1 << 20);
    unsigned long long left = *size;
    bool ok = true;
    while (left > 0) {
        const DWORD want = (DWORD)(left < buffer.size() ? left : buffer.size());
        DWORD got = 0;
        if (!ReadFile(in, buffer.data(), want, &got, nullptr) || got == 0) {
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
    CloseHandle(in);
    return ok;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) {
        std::fwprintf(stderr, L"usage: voicevox-pack-payload <spec.txt> <installer.exe> <output.exe>\n");
        return 2;
    }

    std::vector<Item> items;
    if (!ReadSpec(argv[1], &items)) return 1;

    HANDLE out = CreateFileW(argv[3], GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        std::fwprintf(stderr, L"cannot create %s (error %lu)\n", argv[3], GetLastError());
        return 1;
    }

    // The installer itself goes in first, unchanged: the payload starts at its end.
    unsigned long long offset = 0;
    unsigned long long size = 0;
    if (!AppendFile(out, argv[2], &offset, &size)) {
        CloseHandle(out);
        return 1;
    }
    unsigned long long cursor = size;

    std::vector<PayloadEntry> entries;
    unsigned long long total = 0;
    for (const Item& item : items) {
        PayloadEntry entry = {};
        wcsncpy_s(entry.path, item.dest.c_str(), _TRUNCATE);
        if (!AppendFile(out, item.source, &entry.offset, &entry.size)) {
            CloseHandle(out);
            return 1;
        }
        // Offsets in the header are absolute in the output file, which is what the installer
        // seeks to; keeping them relative would need the payload start recomputed at run time.
        entry.offset += cursor;
        cursor += entry.size;
        total += entry.size;
        entries.push_back(entry);
        std::wprintf(L"  %-52s %6.1f MB\n", item.dest.c_str(), (double)entry.size / (1024 * 1024));
    }

    const unsigned int count = (unsigned int)entries.size();
    const unsigned long long headerOffset = cursor;
    DWORD written = 0;
    WriteFile(out, &count, sizeof(count), &written, nullptr);
    WriteFile(out, entries.data(), (DWORD)(entries.size() * sizeof(PayloadEntry)), &written, nullptr);

    PayloadFooter footer = {};
    memcpy(footer.magic, kMagic, sizeof(kMagic));
    footer.headerOffset = headerOffset;
    footer.headerSize = sizeof(count) + entries.size() * sizeof(PayloadEntry);
    WriteFile(out, &footer, sizeof(footer), &written, nullptr);

    LARGE_INTEGER final = {};
    GetFileSizeEx(out, &final);
    CloseHandle(out);

    std::wprintf(L"packed %zu file(s), payload %.1f MB, output %.1f MB\n", items.size(),
                 (double)total / (1024 * 1024), (double)final.QuadPart / (1024 * 1024));
    return 0;
}
