#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <string>

#include "core.h"

namespace voicevox {
namespace {

// Past this size the log is moved to voicevox-sapi.log.1 and a new one is started, so a browser
// that reads aloud all day cannot fill the disk. Two generations are plenty for a bug report.
constexpr long long kMaxLogBytes = 1024 * 1024;

void RotateIfLarge(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA info = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return;
    const long long size = ((long long)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    if (size < kMaxLogBytes) return;
    // Several processes may race here; whichever rename wins, the others just append to the
    // fresh file it leaves behind.
    MoveFileExW(path.c_str(), (path + L".1").c_str(), MOVEFILE_REPLACE_EXISTING);
}

}  // namespace

void Log(const wchar_t* format, ...) {
    wchar_t message[1024] = {};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(message, _countof(message), _TRUNCATE, format, args);
    va_end(args);

    SYSTEMTIME now = {};
    GetLocalTime(&now);

    // logs\ rather than the data directory itself: the installer makes the data directory
    // read-only for standard users (it holds the DLLs the engine loads), and grants them write
    // access to this one subdirectory so a non-admin browser can still leave a trace.
    const std::wstring dir = Core::LogDir();
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = dir + L"\\voicevox-sapi.log";
    RotateIfLarge(path);

    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"a, ccs=UTF-8") != 0 || file == nullptr) return;

    std::fwprintf(file, L"%04u-%02u-%02u %02u:%02u:%02u  %lu  %s\n", now.wYear, now.wMonth,
                  now.wDay, now.wHour, now.wMinute, now.wSecond, GetCurrentProcessId(), message);
    std::fclose(file);
}
}  // namespace voicevox
