#include "pipe_io.h"

#include <utility>

namespace voicevox {
namespace remote {
namespace {

// Slack added to the deadline after a cancellation, to collect the aborted operation's result
// before the OVERLAPPED it points at goes out of scope.
constexpr DWORD kCancelGraceMs = 2000;

// Base name, without the session suffix PipeName() appends.
constexpr wchar_t kPipeBase[] = L"\\\\.\\pipe\\VoicevoxSapi";

}  // namespace

std::wstring PipeName() {
    DWORD session = 0;
    // 0 on the rare failure, which is the same bucket as session 0 — a machine-wide name is what
    // every earlier build used, so that fallback is the old behaviour rather than a silent miss.
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) session = 0;
    return std::wstring(kPipeBase) + L"-" + std::to_wstring(session);
}

Pipe::Pipe(HANDLE handle) : pipe_(handle) {
    if (pipe_ != INVALID_HANDLE_VALUE && pipe_ != nullptr) {
        event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (event_ == nullptr) Close();
    } else {
        pipe_ = INVALID_HANDLE_VALUE;
    }
}

Pipe::~Pipe() { Close(); }

Pipe::Pipe(Pipe&& other) noexcept : pipe_(other.pipe_), event_(other.event_) {
    other.pipe_ = INVALID_HANDLE_VALUE;
    other.event_ = nullptr;
}

Pipe& Pipe::operator=(Pipe&& other) noexcept {
    if (this != &other) {
        Close();
        pipe_ = other.pipe_;
        event_ = other.event_;
        other.pipe_ = INVALID_HANDLE_VALUE;
        other.event_ = nullptr;
    }
    return *this;
}

void Pipe::Close() {
    if (pipe_ != INVALID_HANDLE_VALUE && pipe_ != nullptr) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
    if (event_ != nullptr) {
        CloseHandle(event_);
        event_ = nullptr;
    }
}

bool Pipe::Complete(OVERLAPPED* overlapped, DWORD timeoutMs, DWORD* transferred) {
    const DWORD wait = WaitForSingleObject(event_, timeoutMs);
    if (wait != WAIT_OBJECT_0) {
        CancelIoEx(pipe_, overlapped);
        // Collect the cancellation. Without this the kernel may still be writing into *overlapped
        // after this frame is gone.
        if (WaitForSingleObject(event_, kCancelGraceMs) == WAIT_OBJECT_0) {
            DWORD ignored = 0;
            GetOverlappedResult(pipe_, overlapped, &ignored, FALSE);
        }
        return false;
    }
    return GetOverlappedResult(pipe_, overlapped, transferred, FALSE) != 0;
}

bool Pipe::ReadExact(void* buffer, DWORD bytes, DWORD timeoutMs) {
    auto* cursor = static_cast<unsigned char*>(buffer);
    DWORD done = 0;
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;

    while (done < bytes) {
        OVERLAPPED overlapped = {};
        overlapped.hEvent = event_;
        ResetEvent(event_);

        DWORD transferred = 0;
        if (!ReadFile(pipe_, cursor + done, bytes - done, nullptr, &overlapped)) {
            if (GetLastError() != ERROR_IO_PENDING) return false;
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) return false;
            if (!Complete(&overlapped, (DWORD)(deadline - now), &transferred)) return false;
        } else {
            // Completed inline; a synchronous completion still has to report how much it read.
            if (!GetOverlappedResult(pipe_, &overlapped, &transferred, FALSE)) return false;
        }
        if (transferred == 0) return false;  // peer closed mid-frame
        done += transferred;
    }
    return true;
}

bool Pipe::WriteExact(const void* buffer, DWORD bytes, DWORD timeoutMs) {
    const auto* cursor = static_cast<const unsigned char*>(buffer);
    DWORD done = 0;
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;

    while (done < bytes) {
        OVERLAPPED overlapped = {};
        overlapped.hEvent = event_;
        ResetEvent(event_);

        DWORD transferred = 0;
        if (!WriteFile(pipe_, cursor + done, bytes - done, nullptr, &overlapped)) {
            if (GetLastError() != ERROR_IO_PENDING) return false;
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) return false;
            if (!Complete(&overlapped, (DWORD)(deadline - now), &transferred)) return false;
        } else {
            if (!GetOverlappedResult(pipe_, &overlapped, &transferred, FALSE)) return false;
        }
        if (transferred == 0) return false;
        done += transferred;
    }
    return true;
}

}  // namespace remote
}  // namespace voicevox
