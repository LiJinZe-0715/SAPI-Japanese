#include "remote.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "log.h"
#include "pipe_io.h"
#include "pipe_protocol.h"
#include "schtask.h"
#include "text.h"

namespace voicevox {
namespace remote {
namespace {

// Waiting for a busy server is worth it: the previous sentence takes a few hundred milliseconds
// and the alternative is falling back to a 13 second cold start. Waiting for one that does not
// exist is not — WaitNamedPipeW returns immediately in that case, so this timeout only ever
// elapses while the server is alive.
constexpr DWORD kConnectTimeoutMs = 5000;

// Generous: the first request after a login can land while the server is still preloading, which
// is the one time this covers a full model load. After that a sentence is under a second.
constexpr DWORD kIoTimeoutMs = 30000;

// A server that is there but does not work is worse than no server: every utterance would pay the
// connect timeout and then the local path anyway. Three strikes, one minute off, then try again —
// an upgrade that fixes whatever was wrong is picked up without restarting anything.
//
// Absent is deliberately not a strike. Nothing is listening is the ordinary state before the
// engine has been started once, and WaitNamedPipe answers it at once, so it costs nothing; counting
// it would punish exactly the case that needs help — an engine that crashed and is reloading its
// model while the next three sentences go local.
constexpr int kFailuresBeforeCooldown = 3;
constexpr ULONGLONG kCooldownMs = 60000;
constexpr ULONGLONG kSpawnThrottleMs = 30000;

constexpr wchar_t kRegistryKey[] = L"Software\\VoicevoxSapi";
constexpr wchar_t kLastStyleValue[] = L"LastStyle";
constexpr wchar_t kServerExe[] = L"VoicevoxSapiServer.exe";

std::atomic<ULONGLONG> g_cooldownUntil{0};
std::atomic<int> g_failures{0};

std::mutex g_spawnMutex;
ULONGLONG g_lastSpawn = 0;

// Its address is inside this module, which is all GetModuleHandleExW needs to find it again.
// A plain variable rather than a function: no function-pointer cast, no code-analysis noise.
const int kModuleAnchor = 0;

std::wstring ModuleDirectory() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&kModuleAnchor), &self) ||
        self == nullptr) {
        return std::wstring();
    }
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(self, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return std::wstring();
    std::wstring full(path, length);
    const size_t slash = full.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : full.substr(0, slash);
}

/**
 * A few lines of diagnosis, then silence.
 *
 * <p>The server is optional, so "cannot connect" is a normal state — a build without it, an
 * install that has not been refreshed. Logging it per sentence would bury everything else, but
 * logging it never leaves "why is it still slow?" with no answer at all.
 */
void NoteFailure(const wchar_t* what, DWORD error) {
    // ERROR_FILE_NOT_FOUND is the expected state before the server has ever been started — it is
    // reported once, as `remote: started server`, on the line below this one.
    if (error == ERROR_FILE_NOT_FOUND) return;
    static std::atomic<int> noted{0};
    if (noted.fetch_add(1, std::memory_order_relaxed) < 5) {
        Log(L"remote: %s failed (error=%lu)", what, error);
    }
}

/** How an attempt to reach the engine ended, for the failure counter's benefit. */
enum class Link {
    Ok,
    Absent,  /**< Nothing is listening. Detected at once, so it says nothing about the engine. */
    Broken,  /**< Something answered, or refused to, and that is worth counting. */
};

Link Connect(Pipe* out) {
    const std::wstring name = PipeName();

    // false with ERROR_FILE_NOT_FOUND means "nothing is listening", which is the common case
    // before the server has been started once. It does not wait.
    if (!WaitNamedPipeW(name.c_str(), kConnectTimeoutMs)) {
        const DWORD error = GetLastError();
        // ERROR_SEM_TIMEOUT is this wait giving up, not the engine being wrong: every instance was
        // busy for five seconds. Counting that as a fault meant three long sentences in a row —
        // two tabs, or a paragraph read aloud while another page speaks — put the engine in
        // cooldown and every following sentence on the 13 second local path. Busy is not broken.
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PIPE_NOT_CONNECTED ||
            error == ERROR_SEM_TIMEOUT) {
            return Link::Absent;
        }
        NoteFailure(L"WaitNamedPipe", error);
        return Link::Broken;
    }

    HANDLE handle = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        // The instance was taken between the wait and the open — a second tab, or an engine that
        // just exited. Neither is a fault of the engine.
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PIPE_NOT_CONNECTED) return Link::Absent;
        NoteFailure(L"CreateFile", error);
        return Link::Broken;
    }

    *out = Pipe(handle);
    return out->valid() ? Link::Ok : Link::Broken;
}

/**
 * Launches the server, throttled.
 *
 * <p>Two ways, in order of how long the result lasts. The scheduled task is launched by the Task
 * Scheduler service, so the engine belongs to no job object and outlives every browser restart —
 * that is the one that actually keeps it warm. CreateProcess from here lands inside this host's
 * job (Chrome kills its whole tree on exit), so it only covers hosts that do not use one and
 * installs from before the task existed; if the executable is not there at all, the install
 * predates the server and the engine simply stays local.
 */
void StartServer() {
    std::lock_guard<std::mutex> guard(g_spawnMutex);
    const ULONGLONG now = GetTickCount64();
    if (g_lastSpawn != 0 && now - g_lastSpawn < kSpawnThrottleMs) return;
    g_lastSpawn = now;

    if (SUCCEEDED(schtask::Start())) {
        Log(L"remote: asked the task scheduler to start the engine");
        return;
    }

    const std::wstring directory = ModuleDirectory();
    if (directory.empty()) return;
    const std::wstring exe = directory + L"\\" + kServerExe;

    std::wstring command = L"\"" + exe + L"\" --background";
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};

    // CREATE_BREAKAWAY_FROM_JOB only works if the job was created with
    // JOB_OBJECT_LIMIT_BREAKAWAY_OK; hosts that did not are refused with ERROR_ACCESS_DENIED,
    // and there the server is started inside the job instead — shorter lived, but still warm for
    // as long as the host lives.
    const DWORD breakaway = CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB;
    if (!CreateProcessW(exe.c_str(), &command[0], nullptr, nullptr, FALSE, breakaway, nullptr,
                        directory.c_str(), &startup, &process)) {
        if (!CreateProcessW(exe.c_str(), &command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, directory.c_str(), &startup, &process)) {
            // Not worth a log line per utterance: a build without the server, or an install that
            // has not been refreshed yet, hits this path constantly and it is not an error.
            NoteFailure(L"CreateProcess", GetLastError());
            return;
        }
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    Log(L"remote: started server %s", exe.c_str());
}

/**
 * One request/response round trip.
 *
 * @param broken set when the engine answered badly, as opposed to not being there — the caller
 *               counts only the former against it.
 */
Result Ask(int styleId, const std::string& utf8Text, float speed, float pitch, float volume,
           std::vector<uint8_t>* pcm, std::string* error, bool* broken) {
    Pipe pipe;
    const Link link = Connect(&pipe);
    if (link != Link::Ok) {
        *broken = link == Link::Broken;
        return Result::Unavailable;
    }

    Request request = {};
    std::memcpy(request.magic, kMagic, sizeof(kMagic));
    request.version = kVersion;
    request.op = kOpSynth;
    request.styleId = styleId;
    request.speed = speed;
    request.pitch = pitch;
    request.volume = volume;
    request.textBytes = (uint32_t)utf8Text.size();

    if (!pipe.WriteExact(&request, sizeof(request), kIoTimeoutMs)) {
        NoteFailure(L"write header", GetLastError());
        *broken = true;
        return Result::Unavailable;
    }
    if (!utf8Text.empty() &&
        !pipe.WriteExact(utf8Text.data(), (DWORD)utf8Text.size(), kIoTimeoutMs)) {
        NoteFailure(L"write text", GetLastError());
        *broken = true;
        return Result::Unavailable;
    }

    Response response = {};
    if (!pipe.ReadExact(&response, sizeof(response), kIoTimeoutMs)) {
        NoteFailure(L"read response", GetLastError());
        *broken = true;
        return Result::Unavailable;
    }
    if (std::memcmp(response.magic, kMagic, sizeof(kMagic)) != 0 ||
        response.version != kVersion) {
        Log(L"remote: protocol mismatch (server version %u, expect %u)", response.version, kVersion);
        *broken = true;
        return Result::Unavailable;
    }
    if (response.pcmBytes > kMaxPcmBytes || response.errorBytes > kMaxErrorBytes) {
        Log(L"remote: response out of bounds (%u pcm, %u error bytes)", response.pcmBytes,
            response.errorBytes);
        *broken = true;
        return Result::Unavailable;
    }

    std::string message(response.errorBytes, '\0');
    if (response.errorBytes != 0 &&
        !pipe.ReadExact(&message[0], (DWORD)message.size(), kIoTimeoutMs)) {
        *broken = true;
        return Result::Unavailable;
    }

    if (response.status != 0) {
        if (error != nullptr) *error = message;
        Log(L"remote: synthesis refused: %s", FromUtf8(message.c_str()).c_str());
        return Result::Failed;
    }

    pcm->assign(response.pcmBytes, 0);
    if (response.pcmBytes != 0 && !pipe.ReadExact(pcm->data(), (DWORD)pcm->size(), kIoTimeoutMs)) {
        NoteFailure(L"read pcm", GetLastError());
        *broken = true;
        return Result::Unavailable;
    }
    return Result::Ok;
}

}  // namespace

Result Synthesize(int styleId, const std::string& utf8Text, float speed, float pitch, float volume,
                  std::vector<uint8_t>* pcm, std::string* error) {
    if (GetTickCount64() < g_cooldownUntil.load(std::memory_order_relaxed)) {
        return Result::Unavailable;
    }

    // The frame has a 64 KB ceiling. A longer one is refused by the server without a reply, and the
    // only thing the client could do about that is sit on the 30 second read timeout and then
    // synthesize locally anyway — so decide here, where it costs one comparison. Rare: SAPI hands
    // over one sentence at a time.
    if (utf8Text.size() > kMaxTextBytes) {
        static std::atomic<int> noted{0};
        if (noted.fetch_add(1, std::memory_order_relaxed) < 3) {
            Log(L"remote: %zu bytes of text is over the %u byte frame limit, staying local",
                utf8Text.size(), kMaxTextBytes);
        }
        return Result::Unavailable;
    }

    bool broken = false;
    const Result result = Ask(styleId, utf8Text, speed, pitch, volume, pcm, error, &broken);
    if (result != Result::Unavailable) {
        // Ok, and also Failed: a refusal means the server is alive and working, so the strikes are
        // about its health rather than this particular sentence.
        g_failures.store(0, std::memory_order_relaxed);
        return result;
    }

    if (broken &&
        g_failures.fetch_add(1, std::memory_order_relaxed) + 1 >= kFailuresBeforeCooldown) {
        g_cooldownUntil.store(GetTickCount64() + kCooldownMs, std::memory_order_relaxed);
        g_failures.store(0, std::memory_order_relaxed);
        Log(L"remote: engine answered badly %d times, staying local for %llu s",
            kFailuresBeforeCooldown, kCooldownMs / 1000);
    }
    // Worth a try on every kind of failure: the engine exits after 30 idle minutes, and this is
    // the only thing that brings it back without waiting for the next logon. Throttled inside.
    StartServer();
    return result;
}

void RememberStyle(int styleId) {
    if (styleId <= 0) return;
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                        &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    const DWORD value = (DWORD)styleId;
    RegSetValueExW(key, kLastStyleValue, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value),
                   sizeof(value));
    RegCloseKey(key);
}

}  // namespace remote
}  // namespace voicevox
