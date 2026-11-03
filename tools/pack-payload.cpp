// voicevox-pack-payload — glues the runtime, the dictionary and the model packs onto the back of
// the installer executable, so the result is one file you can double-click.
//
// Appending after the PE image is the same trick 7-Zip's SFX and every "portable" installer use:
// Windows only maps the image described by the headers, the tail is just data the program can
// seek to. No compression on purpose — the payload is a 100 MB binary dictionary and a 60 MB
// ONNX model, which barely compress, and a corrupt byte in a compressed stream means a failed
// install instead of a failed file copy. Each file carries a checksum instead (see payload.h).
//
// The output can be Authenticode-signed afterwards: signtool appends the signature after the
// footer, and the installer finds the footer through the PE security directory.
//
//   voicevox-pack-payload <spec.txt> <installer.exe> <output.exe>
//
// Spec file (UTF-8, one rule per line, '#' starts a comment). Relative source paths are taken
// relative to the spec file's directory, not the current directory:
//   file|<dest relative path>|<source path>
//   dir |<dest relative dir> |<source dir>          (recursive)

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "payload.h"

namespace {

using vvpayload::PayloadEntry;
using vvpayload::PayloadFooter;

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

bool IsAbsolute(const std::wstring& path) {
    return (path.size() >= 2 && path[1] == L':') || (!path.empty() && path[0] == L'\\');
}

std::wstring DirectoryOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring(L".") : path.substr(0, slash);
}

/** A destination the installer will accept: relative, no "..", short enough for the header. */
bool ValidDest(const std::wstring& dest) {
    if (dest.empty() || dest.size() >= 260 || IsAbsolute(dest)) return false;
    if (dest.find(L'/') != std::wstring::npos || dest.find(L':') != std::wstring::npos) return false;
    for (const std::wstring& part : Split(dest, L'\\')) {
        if (part.empty() || part == L"." || part == L"..") return false;
    }
    return true;
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
    const std::wstring base = DirectoryOf(path);

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
            const std::wstring source = IsAbsolute(parts[2]) ? parts[2] : base + L"\\" + parts[2];
            if (parts[0] == L"file") {
                items->push_back(Item{parts[1], source});
            } else if (parts[0] == L"dir") {
                if (!CollectDir(source, parts[1], items)) return false;
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

bool WriteAll(HANDLE out, const void* data, size_t size) {
    DWORD written = 0;
    return WriteFile(out, data, (DWORD)size, &written, nullptr) != 0 && written == size;
}

bool AppendFile(HANDLE out, const std::wstring& source, unsigned long long* size,
                unsigned long long* hash) {
    HANDLE in = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (in == INVALID_HANDLE_VALUE) {
        std::fwprintf(stderr, L"cannot open %s (error %lu)\n", source.c_str(), GetLastError());
        return false;
    }
    LARGE_INTEGER length = {};
    GetFileSizeEx(in, &length);
    *size = (unsigned long long)length.QuadPart;
    *hash = vvpayload::kFnvBasis;

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
        *hash = vvpayload::Fnv1a(*hash, buffer.data(), got);
        if (!WriteAll(out, buffer.data(), got)) {
            ok = false;
            break;
        }
        left -= got;
    }
    CloseHandle(in);
    if (!ok) std::fwprintf(stderr, L"cannot copy %s\n", source.c_str());
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
    for (const Item& item : items) {
        if (!ValidDest(item.dest)) {
            std::fwprintf(stderr, L"bad destination (absolute, '..' or too long): %s\n",
                          item.dest.c_str());
            return 1;
        }
    }

    HANDLE out = CreateFileW(argv[3], GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        std::fwprintf(stderr, L"cannot create %s (error %lu)\n", argv[3], GetLastError());
        return 1;
    }
    const auto fail = [&]() {
        CloseHandle(out);
        DeleteFileW(argv[3]);
        return 1;
    };

    // The installer itself goes in first, unchanged: the payload starts at its end.
    unsigned long long size = 0;
    unsigned long long hash = 0;
    if (!AppendFile(out, argv[2], &size, &hash)) return fail();
    unsigned long long cursor = size;

    std::vector<PayloadEntry> entries;
    unsigned long long total = 0;
    for (const Item& item : items) {
        PayloadEntry entry = {};
        wcscpy_s(entry.path, item.dest.c_str());
        if (!AppendFile(out, item.source, &entry.size, &entry.fnv1a)) return fail();
        // Offsets in the header are absolute in the output file, which is what the installer
        // seeks to; keeping them relative would need the payload start recomputed at run time.
        entry.offset = cursor;
        cursor += entry.size;
        total += entry.size;
        entries.push_back(entry);
        std::wprintf(L"  %-52s %6.1f MB\n", item.dest.c_str(), (double)entry.size / (1024 * 1024));
    }

    const unsigned int count = (unsigned int)entries.size();
    PayloadFooter footer = {};
    memcpy(footer.magic, vvpayload::kMagic, sizeof(vvpayload::kMagic));
    footer.headerOffset = cursor;
    footer.headerSize = sizeof(count) + entries.size() * sizeof(PayloadEntry);
    if (!WriteAll(out, &count, sizeof(count)) ||
        !WriteAll(out, entries.data(), entries.size() * sizeof(PayloadEntry)) ||
        !WriteAll(out, &footer, sizeof(footer))) {
        std::fwprintf(stderr, L"cannot write the payload header\n");
        return fail();
    }

    LARGE_INTEGER final = {};
    GetFileSizeEx(out, &final);
    CloseHandle(out);

    std::wprintf(L"packed %zu file(s), payload %.1f MB, output %.1f MB\n", items.size(),
                 (double)total / (1024 * 1024), (double)final.QuadPart / (1024 * 1024));
    return 0;
}
