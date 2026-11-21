// VoicevoxSapiServer.exe — the resident engine.
//
// <p>Why this exists: building a synthesizer costs about 13 seconds on a cold machine, almost all
// of it inside voicevox_synthesizer_load_voice_model (an ONNX Runtime session for a 72 MB model).
// SAPI loads the engine DLL into whichever process wants to speak, so that cost is paid again for
// every new host process — a browser restart, a TTS process, a second application. Paying it once
// at login and keeping the result here turns the first utterance into a pipe round trip plus
// inference, under a second.
//
// <p>The DLL keeps working without this process. That is the point of the design: a missing
// server, a crashed server or a protocol from the future all degrade to "synthesize locally",
// which is slow and correct rather than fast and fragile.
//
// <p>Runs at the user's own integrity level, started at logon (and on demand) by the scheduled task
// that the installer registers. No service: a service would need SYSTEM, a per-session instance,
// and a whole extra surface for a program whose only job is to hold a model in memory for the
// person sitting in front of the screen.

#include <windows.h>
#include <sddl.h>

#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "core.h"
#include "log.h"
#include "pipe_io.h"
#include "pipe_protocol.h"

using namespace voicevox;
using namespace voicevox::remote;

namespace {

// Per session, like the pipe it serves (see remote::PipeName): two users logged in at once each
// get their own engine, with their own preloaded style and their own idle timer. Not Global\, which
// would make the second session's engine exit as a duplicate.
constexpr wchar_t kInstanceMutex[] = L"Local\\VoicevoxSapiServer";

constexpr wchar_t kRegistryKey[] = L"Software\\VoicevoxSapi";
constexpr wchar_t kLastStyleValue[] = L"LastStyle";

// Long enough that the model stays warm across a browser restart, short enough that a machine
// left alone overnight is not holding 600 MB of weights. The next utterance pays 13 seconds once
// and the server is back.
constexpr DWORD kIdleExitMs = 30 * 60 * 1000;

// The first request after a login can arrive while the preload is still running, so this covers
// a full model load plus the client's own patience.
constexpr DWORD kIoTimeoutMs = 30000;

// A sentence is a few hundred KB of PCM at most; 64 KB keeps the usual case to a single write
// while still letting a long one stream through the loop in Pipe::WriteExact.
constexpr DWORD kPipeBufferBytes = 64 * 1024;

// How long the client gets to read what was written. A response crosses a pipe in microseconds
// once the reader is running at all; this only ever elapses when it has stopped running.
constexpr DWORD kFlushTimeoutMs = 5000;

// Who may connect.
//
// <p>Allow-list of authenticated users rather than "creator only": the caller is whatever process
// SAPI was asked to speak from, and that is not always the interactive shell — a browser renderer
// runs at low integrity, and a pipe that only answered the main process would fall back to the
// 13 second path exactly where the delay is most obvious. Every interactive logon shares one user
// on a desktop machine anyway, so this widens nothing that a same-user process could not already
// do; the SYNCHRONIZE|READ_CONTROL|FILE_* mask is the standard one for a duplex pipe.
constexpr wchar_t kPipeSddl[] =
    L"D:(A;;0x12019f;;;SY)(A;;0x12019f;;;BA)(A;;0x12019f;;;AU)S:(ML;;NW;;;LW)";

ULONGLONG g_lastActivity = 0;

int LastStyle() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryKey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return 0;
    }
    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    const LONG result = RegQueryValueExW(key, kLastStyleValue, nullptr, &type, (LPBYTE)&value, &size);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(value)) return 0;
    return (int)value;
}

/**
 * Loads the model the previous session used, before anyone asks for it.
 *
 * <p>Throws the audio away: what is being warmed up is the ONNX Runtime session, and a two
 * character query is the cheapest way to make Core build one. Runs on its own thread so the main
 * loop can start accepting connections immediately — a request that arrives now simply waits on
 * the same mutex, which is no worse than the state before this process existed.
 */
DWORD WINAPI PreloadThread(void*) {
    const int style = LastStyle();
    if (style <= 0) {
        Log(L"server: no previous style recorded, nothing to preload");
        return 0;
    }

    const ULONGLONG started = GetTickCount64();
    std::vector<uint8_t> pcm;
    std::string error;
    const bool ok = Core::Instance().Synthesize(style, "\xe3\x81\x82", 1.0f, 0.0f, 1.0f, &pcm,
                                                &error);
    Log(L"server: preload style %d %s after %llu ms", style, ok ? L"ready" : L"failed",
        GetTickCount64() - started);
    return 0;
}

bool IdleExpired() { return GetTickCount64() - g_lastActivity >= kIdleExitMs; }

/** How one wait for a client ended. */
enum class Accept {
    Connected,
    Idle,    /**< Nobody said anything for kIdleExitMs; time to go. */
    Failed,  /**< The pipe itself is unusable, so waiting again would only spin. */
};

/**
 * Waits for a client, waking once a second to check whether anyone still wants us.
 *
 * <p>ConnectNamedPipe has no timeout of its own. Blocking on it forever is how a background
 * process ends up holding 600 MB until the next reboot, and polling with a short timeout is the
 * documented way around it without a second thread.
 */
Accept AcceptClient(HANDLE pipe) {
    OVERLAPPED overlapped = {};
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr) return Accept::Failed;
    overlapped.hEvent = event;

    Accept result = Accept::Failed;
    if (ConnectNamedPipe(pipe, &overlapped)) {
        result = Accept::Connected;
    } else {
        const DWORD error = GetLastError();
        if (error == ERROR_PIPE_CONNECTED) {
            result = Accept::Connected;
        } else if (error == ERROR_IO_PENDING) {
            result = Accept::Idle;
            for (;;) {
                if (IdleExpired()) break;
                if (WaitForSingleObject(event, 1000) == WAIT_OBJECT_0) {
                    result = Accept::Connected;
                    break;
                }
            }
            if (result != Accept::Connected) {
                // The OVERLAPPED is about to go out of scope; the kernel must be done with it.
                CancelIoEx(pipe, &overlapped);
                WaitForSingleObject(event, 2000);
            }
        }
    }

    CloseHandle(event);
    return result;
}

DWORD WINAPI FlushThread(void* param) {
    FlushFileBuffers(*static_cast<HANDLE*>(param));
    return 0;
}

/**
 * Hands the response over before the connection is broken.
 *
 * <p>FlushFileBuffers before DisconnectNamedPipe is not optional. DisconnectNamedPipe throws away
 * whatever the client has not read yet, and a 60 KB PCM response is easily still in flight when
 * Serve returns: the client then reports ERROR_PIPE_NOT_CONNECTED halfway through, falls back to
 * synthesizing locally, and the whole point of this process is lost for that sentence. The flush
 * blocks until the reader has caught up, and returns immediately if it never will.
 *
 * <p>That last clause is the danger, and the reason this runs on a thread with a deadline. A
 * reader that has stopped reading — a suspended browser, a host parked in a debugger — would
 * otherwise block this thread forever. With a single pipe instance that takes the engine down for
 * the rest of the session: it holds the instance mutex, so the on-demand start in the DLL can only
 * spawn a duplicate that exits at once, and every later sentence pays the 13 second local path.
 * One sentence of lost audio is the right price for not having that failure mode.
 */
void FlushThenDisconnect(HANDLE pipe) {
    auto* context = new (std::nothrow) HANDLE(pipe);
    if (context == nullptr) {
        DisconnectNamedPipe(pipe);
        return;
    }

    HANDLE thread = CreateThread(nullptr, 0, &FlushThread, context, 0, nullptr);
    if (thread == nullptr) {
        delete context;
        DisconnectNamedPipe(pipe);
        return;
    }

    if (WaitForSingleObject(thread, kFlushTimeoutMs) == WAIT_OBJECT_0) {
        delete context;
    } else {
        CancelSynchronousIo(thread);
        // If even the cancellation is not collected, the thread is still going to read the
        // context; leaking eight bytes beats freeing memory another thread is about to touch.
        if (WaitForSingleObject(thread, 2000) == WAIT_OBJECT_0) delete context;
    }
    CloseHandle(thread);
    DisconnectNamedPipe(pipe);
}

/** Serves one connection: exactly one request, one response, then the client closes. */
void Serve(Pipe& pipe) {
    Request request = {};
    if (!pipe.ReadExact(&request, sizeof(request), kIoTimeoutMs)) {
        // A client that connected and left without saying anything is not an error: a probe, or a
        // host that gave up while this process was still preloading.
        const DWORD error = GetLastError();
        if (error != ERROR_BROKEN_PIPE && error != ERROR_PIPE_NOT_CONNECTED) {
            Log(L"server: client sent nothing (error=%lu)", error);
        }
        return;
    }
    if (std::memcmp(request.magic, kMagic, sizeof(kMagic)) != 0) {
        Log(L"server: ignoring a frame that is not ours");
        return;
    }
    if (request.version != kVersion) {
        Log(L"server: client speaks version %u, this build speaks %u", request.version, kVersion);
        return;
    }
    if (request.op != kOpSynth) return;
    if (request.textBytes > kMaxTextBytes) {
        Log(L"server: text of %u bytes is over the limit", request.textBytes);
        return;
    }

    std::string text(request.textBytes, '\0');
    if (request.textBytes != 0 &&
        !pipe.ReadExact(&text[0], (DWORD)text.size(), kIoTimeoutMs)) {
        return;
    }

    std::vector<uint8_t> pcm;
    std::string error;
    const bool ok = Core::Instance().Synthesize(request.styleId, text, request.speed, request.pitch,
                                                request.volume, &pcm, &error);

    Response response = {};
    std::memcpy(response.magic, kMagic, sizeof(kMagic));
    response.version = kVersion;
    response.status = ok ? 0 : 1;
    response.pcmBytes = ok ? (uint32_t)pcm.size() : 0;
    response.errorBytes = ok ? 0 : (uint32_t)error.size();

    if (!pipe.WriteExact(&response, sizeof(response), kIoTimeoutMs)) return;
    if (response.pcmBytes != 0 &&
        !pipe.WriteExact(pcm.data(), response.pcmBytes, kIoTimeoutMs)) {
        return;
    }
    if (response.errorBytes != 0 &&
        !pipe.WriteExact(error.data(), response.errorBytes, kIoTimeoutMs)) {
        return;
    }
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE previous, LPWSTR commandLine, int show) {
    (void)instance;
    (void)previous;
    (void)commandLine;
    (void)show;

    HANDLE mutex = CreateMutexW(nullptr, FALSE, kInstanceMutex);
    if (mutex == nullptr) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        Log(L"server: another instance is already running, nothing to do");
        return 0;
    }

    SECURITY_ATTRIBUTES attributes = {};
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kPipeSddl, SDDL_REVISION_1,
                                                              &descriptor, nullptr)) {
        Log(L"server: cannot build the pipe ACL (error=%lu)", GetLastError());
        return 1;
    }
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;

    Log(L"server: starting (pid %lu)", GetCurrentProcessId());
    g_lastActivity = GetTickCount64();

    const std::wstring pipeName = PipeName();
    bool failed = false;

    // One instance for the life of the process, not one per connection. DisconnectNamedPipe leaves
    // the handle ready for the next ConnectNamedPipe, and that is what keeps the name in the
    // namespace continuously. A fresh CreateNamedPipe per connection left a gap of a few hundred
    // microseconds in which \\.\pipe\VoicevoxSapi-<session> did not exist at all, and a client that
    // called WaitNamedPipe in that gap was told "nothing is listening" — so it paid the 13 second
    // local path for a sentence the engine was about to be ready for.
    HANDLE raw = CreateNamedPipeW(
        pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
        kPipeBufferBytes, kPipeBufferBytes, 0, &attributes);
    if (raw == INVALID_HANDLE_VALUE) {
        Log(L"server: cannot create the pipe (error=%lu)", GetLastError());
        LocalFree(descriptor);
        return 1;
    }

    Pipe pipe(raw);
    if (!pipe.valid()) {
        Log(L"server: cannot create the pipe's event");
        LocalFree(descriptor);
        return 1;
    }

    HANDLE preload = CreateThread(nullptr, 0, &PreloadThread, nullptr, 0, nullptr);
    if (preload != nullptr) CloseHandle(preload);

    while (!IdleExpired()) {
        const Accept accepted = AcceptClient(pipe.handle());
        if (accepted == Accept::Idle) break;
        if (accepted == Accept::Failed) {
            Log(L"server: cannot wait for a client (error=%lu)", GetLastError());
            failed = true;
            break;
        }

        Serve(pipe);
        g_lastActivity = GetTickCount64();
        FlushThenDisconnect(pipe.handle());
    }

    LocalFree(descriptor);
    if (!failed) Log(L"server: idle for %llu s, exiting", kIdleExitMs / 1000);
    return failed ? 1 : 0;
}
