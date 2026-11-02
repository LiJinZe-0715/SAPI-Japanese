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

}  // namespace voicevox
