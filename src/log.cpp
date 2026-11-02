#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <string>

#include "core.h"

namespace voicevox {

void Log(const wchar_t* format, ...) {
    wchar_t message[1024] = {};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(message, _countof(message), _TRUNCATE, format, args);
    va_end(args);

    SYSTEMTIME now = {};
    GetLocalTime(&now);

    const std::wstring path = Core::HomeDir() + L"\\voicevox-sapi.log";
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"a, ccs=UTF-8") != 0 || file == nullptr) return;

    std::fwprintf(file, L"%04u-%02u-%02u %02u:%02u:%02u  %lu  %s\n", now.wYear, now.wMonth,
                  now.wDay, now.wHour, now.wMinute, now.wSecond, GetCurrentProcessId(), message);
    std::fclose(file);
}
}  // namespace voicevox
