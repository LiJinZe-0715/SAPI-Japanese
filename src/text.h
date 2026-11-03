#pragma once

#include <windows.h>

#include <string>

namespace voicevox {

/** UTF-16 → UTF-8, which is what the C API expects for text and paths. */
inline std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) return std::string();
    std::string out((size_t)size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &out[0], size, nullptr, nullptr);
    return out;
}

/** UTF-8 → UTF-16, for anything the C API hands back. */
inline std::wstring FromUtf8(const char* text) {
    if (text == nullptr || *text == '\0') return std::wstring();
    const int size = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (size <= 1) return std::wstring();
    std::wstring out((size_t)size - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, &out[0], size);
    return out;
}

/**
 * The contents of a hand-edited text file, whatever Notepad saved it as.
 *
 * <p>UTF-8 with or without a BOM, UTF-16 LE/BE with a BOM ("Unicode" in Notepad's save dialog),
 * and as a last resort the ANSI code page for a file that is not valid UTF-8. Reading UTF-16 as
 * UTF-8 used to stop at the first zero byte and leave a line of garbage — which, in voices.txt,
 * selected nothing at all.
 */
inline std::wstring DecodeText(const std::string& bytes) {
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    const size_t size = bytes.size();
    if (size >= 2 && ((data[0] == 0xFF && data[1] == 0xFE) || (data[0] == 0xFE && data[1] == 0xFF))) {
        const bool bigEndian = data[0] == 0xFE;
        std::wstring out;
        out.reserve(size / 2);
        for (size_t at = 2; at + 1 < size; at += 2) {
            out.push_back(bigEndian ? (wchar_t)((data[at] << 8) | data[at + 1])
                                    : (wchar_t)(data[at] | (data[at + 1] << 8)));
        }
        return out;
    }

    size_t start = 0;
    if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) start = 3;
    if (start == size) return std::wstring();

    const char* text = bytes.data() + start;
    const int length = (int)(size - start);
    UINT codePage = CP_UTF8;
    int wide = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, length, nullptr, 0);
    if (wide <= 0) {
        codePage = CP_ACP;
        wide = MultiByteToWideChar(CP_ACP, 0, text, length, nullptr, 0);
        if (wide <= 0) return std::wstring();
    }
    std::wstring out((size_t)wide, L'\0');
    MultiByteToWideChar(codePage, 0, text, length, &out[0], wide);
    return out;
}

}  // namespace voicevox
