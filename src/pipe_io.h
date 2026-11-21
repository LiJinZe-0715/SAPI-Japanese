#pragma once

#include <windows.h>

#include <string>

namespace voicevox {
namespace remote {

/**
 * The pipe name for this session, e.g. `\\.\pipe\VoicevoxSapi-1`.
 *
 * <p>The `\\.\pipe\` namespace is machine-wide, not per-session: two users logged on at once
 * resolve the same name, so the second engine would fail to create its pipe and exit while its
 * browser quietly talked to the first user's engine — with that user's preloaded style and idle
 * timer. The session id keeps one engine per desktop, which is what the rest of the design
 * (per-user preload style, 30 minute idle exit) already assumes.
 *
 * <p>Computed per call rather than cached: the session of a process never changes, and the
 * alternative is a global initialised while the DLL is loading.
 */
std::wstring PipeName();

/**
 * Reads and writes on a pipe handle opened with FILE_FLAG_OVERLAPPED, with a deadline.
 *
 * <p>The deadline is not a nicety. Synchronous from SAPI's point of view, Speak is called on a
 * host thread that is often the browser's UI thread; a ReadFile that never returns is a window
 * that stops repainting. So every operation waits with a timeout and gives up, and the caller
 * falls back to synthesizing locally.
 *
 * <p>Takes ownership of the handle it is given.
 */
class Pipe {
public:
    Pipe() = default;
    explicit Pipe(HANDLE handle);
    ~Pipe();

    Pipe(Pipe&& other) noexcept;
    Pipe& operator=(Pipe&& other) noexcept;
    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;

    bool valid() const { return pipe_ != INVALID_HANDLE_VALUE; }
    HANDLE handle() const { return pipe_; }
    void Close();

    bool ReadExact(void* buffer, DWORD bytes, DWORD timeoutMs);
    bool WriteExact(const void* buffer, DWORD bytes, DWORD timeoutMs);

private:
    /**
     * Waits for an operation that returned ERROR_IO_PENDING.
     *
     * <p>On timeout the operation is cancelled *and waited for*: the OVERLAPPED lives on this
     * frame, and returning while the kernel still holds a pointer to it corrupts the stack of the
     * next operation. CancelIoEx cannot fail in a way that matters here — if the operation
     * completed in the meantime, this just collects its result.
     */
    bool Complete(OVERLAPPED* overlapped, DWORD timeoutMs, DWORD* transferred);

    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    HANDLE event_ = nullptr;
};

}  // namespace remote
}  // namespace voicevox
