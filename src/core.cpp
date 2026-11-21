#include "core.h"

#include <windows.h>
#include <shlobj.h>

#include <delayimp.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#include "audio_util.h"
#include "catalog.h"
#include "log.h"
#include "text.h"

namespace voicevox {
namespace {

constexpr wchar_t kCoreDll[] = L"voicevox_core.dll";
constexpr wchar_t kOrtDll[] = L"voicevox_onnxruntime.dll";
constexpr wchar_t kDictDir[] = L"dict\\open_jtalk_dic_utf_8-1.11";
constexpr wchar_t kModelsDir[] = L"models";
constexpr wchar_t kLogDir[] = L"logs";

// What GetOutputFormat promises SAPI, and therefore what the audio has to be.
constexpr int kOutputSampleRate = 24000;

// A failed bring-up is retried, but not on every utterance: a missing runtime makes every
// attempt cost a LoadLibrary and a log line, and a page reading a paragraph calls Speak per
// sentence. Five seconds is long enough to stay quiet and short enough that an install that was
// still copying files when the first Speak arrived is picked up without restarting the browser.
constexpr ULONGLONG kRetryAfterMs = 5000;

// Stack reserved for the thread that runs the synthesizer.
//
// <p>The Rust side aborts the whole host process when it runs out of stack — fail-fast
// 0xC0000409 with FAST_FAIL_FATAL_APP_EXIT, observed 3 times in 12 cold starts inside Chrome,
// where the engine is reached through SAPI and COM and the 1 MB a host thread reserves is not
// always enough. Reserving our own stack is the part of that depth we can control; 8 MB costs
// address space, not memory, and the page file only backs what is touched.
constexpr DWORD kSynthStackBytes = 8u * 1024u * 1024u;

/**
 * One unit of work for the synthesizer thread.
 *
 * <p>The pointers point at the caller's stack; that is safe because the caller blocks on the
 * thread until it returns.
 */
struct SynthJob {
    Core* core = nullptr;
    int styleId = 0;
    const std::string* text = nullptr;
    float speed = 1.0f;
    float pitch = 0.0f;
    float volume = 1.0f;
    std::vector<uint8_t>* pcm = nullptr;
    std::string* error = nullptr;
    bool ok = false;
};

std::wstring Join(const std::wstring& a, const wchar_t* b) {
    std::wstring out = a;
    if (!out.empty() && out.back() != L'\\') out.push_back(L'\\');
    out += b;
    return out;
}

bool IsDir(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/**
 * The dotted version at the end of a dictionary directory name: -1.11 -> {1, 11}.
 *
 * <p>Empty when the name does not end in one, which is what tells the caller to fall back to a
 * plain string comparison.
 */
std::vector<long long> DictVersion(const std::wstring& name) {
    std::vector<long long> parts;
    const size_t dash = name.find_last_of(L'-');
    if (dash == std::wstring::npos) return parts;

    size_t at = dash + 1;
    if (at >= name.size() || name[at] < L'0' || name[at] > L'9') return parts;
    long long value = 0;
    bool digits = false;
    for (; at < name.size(); ++at) {
        const wchar_t c = name[at];
        if (c >= L'0' && c <= L'9') {
            value = value * 10 + (c - L'0');
            digits = true;
        } else if (c == L'.' && digits) {
            parts.push_back(value);
            value = 0;
            digits = false;
        } else {
            return {};  // not the shape we know; do not pretend to order it
        }
    }
    if (digits) parts.push_back(value);
    return parts;
}

/** True when `a` names a newer dictionary than `b`. */
bool NewerDict(const std::wstring& a, const std::wstring& b) {
    const std::vector<long long> va = DictVersion(a);
    const std::vector<long long> vb = DictVersion(b);
    if (!va.empty() && !vb.empty() && va != vb) {
        // Component-wise, as numbers: plain string order puts -1.9 above -1.11, which would
        // prefer the dictionary an older core left behind over the one that came with this one.
        return std::lexicographical_compare(vb.begin(), vb.end(), va.begin(), va.end());
    }
    return _wcsicmp(a.c_str(), b.c_str()) > 0;
}

/**
 * The open_jtalk dictionary directory, which the core release names after the dictionary version.
 *
 * <p>That name has changed before (-1.10 → -1.11) and will change again; with the name pinned, a
 * core upgrade turned a complete install into "open_jtalk failed" and nothing else. The pinned
 * name is still tried first, then the newest `open_jtalk_dic*` directory under dict\. When nothing
 * matches, the pinned path is returned so the log names something concrete.
 */
std::wstring DictDir(const std::wstring& home) {
    const std::wstring preferred = Join(home, kDictDir);
    if (IsDir(preferred)) return preferred;

    const std::wstring root = Join(home, L"dict");
    WIN32_FIND_DATAW found = {};
    HANDLE handle = FindFirstFileW(Join(root, L"open_jtalk_dic*").c_str(), &found);
    if (handle == INVALID_HANDLE_VALUE) return preferred;

    std::wstring best;
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
        if (best.empty() || NewerDict(found.cFileName, best)) best = found.cFileName;
    } while (FindNextFileW(handle, &found) != 0);
    FindClose(handle);
    return best.empty() ? preferred : Join(root, best.c_str());
}

/**
 * How many model packs stay loaded at once.
 *
 * <p>Four is roughly 600 MB of weights — enough that switching between a few characters does
 * not reload, low enough that a browser does not get blamed for hoarding memory. Overridable
 * for machines where the trade-off is different.
 */
size_t MaxResidentPacks() {
    constexpr size_t kDefault = 4;
    wchar_t buffer[16] = {};
    const DWORD length = GetEnvironmentVariableW(L"VOICEVOX_SAPI_MAX_PACKS", buffer, 16);
    if (length > 0 && length < 16) {
        const int value = _wtoi(buffer);
        if (value > 0) return (size_t)value;
    }
    return kDefault;
}

/**
 * Keeps this DLL mapped for the rest of the process.
 *
 * <p>Once the core is up, voicevox_core.dll holds a synthesizer and hundreds of MB of weights
 * that only this module knows how to reach. If COM unloaded us (DllCanUnloadNow says yes as soon
 * as the last engine is released) all of that would leak, and the next voice would build a
 * second synthesizer next to it.
 */
void PinSelf() {
    static const int kAnchor = 0;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&kAnchor), &self);
}

}  // namespace

Core& Core::Instance() {
    static Core instance;
    return instance;
}

std::wstring Core::HomeDir() {
#ifndef NDEBUG
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"VOICEVOX_SAPI_HOME", buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) return buffer;
#endif

    std::wstring base;
    PWSTR programData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &programData))) {
        base = programData;
        CoTaskMemFree(programData);
    }
    if (base.empty()) base = L"C:\\ProgramData";
    return Join(base, L"VoicevoxSapi");
}

std::wstring Core::LogDir() {
    return Join(HomeDir(), kLogDir);
}

std::string Core::LastError() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return lastError_;
}

bool Core::Fail(const char* what, int resultCode) {
    lastError_ = std::string(what) + ": " + voicevox_error_result_to_message((VoicevoxResultCode)resultCode);
    return false;
}

bool Core::EnsureReady() {
    if (ready_) return true;
    const ULONGLONG now = GetTickCount64();
    if (attempted_ && now - lastAttempt_ < kRetryAfterMs) return false;
    attempted_ = true;
    lastAttempt_ = now;

    // Cold start is paid once per host process and the user sees all of it as silence before the
    // first syllable. Which stage owns those seconds decides what can be done about it: a slow
    // .vvm read is something a logon-time warm-up can help with, ORT building sessions is not.
    // Per-stage deltas, so no arithmetic is needed to read the log.
    const ULONGLONG started = now;
    ULONGLONG last = now;
    const auto stage = [&last]() {
        const ULONGLONG tick = GetTickCount64();
        const ULONGLONG delta = tick - last;
        last = tick;
        return delta;
    };

    const std::wstring home = HomeDir();

    // Loaded by absolute path: SAPI loads this DLL inside the host process (Chrome), where the
    // dependency search would otherwise look in Chrome's directory. Its own dependencies (the VC
    // runtime) are looked up in its directory and System32 only — never in the host's directory,
    // the current directory or PATH.
    if (!coreLoaded_) {
        if (LoadLibraryExW(Join(home, kCoreDll).c_str(), nullptr,
                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32) == nullptr) {
            lastError_ = "voicevox_core.dll not found under " + ToUtf8(home);
            Log(L"init: core dll load failed, error=%lu, home=%s", GetLastError(), home.c_str());
            return false;
        }
        coreLoaded_ = true;
        Log(L"init: core dll loaded from %s (%llu ms)", home.c_str(), stage());
    }

    if (onnxruntime_ == nullptr) {
        const std::string ortPath = ToUtf8(Join(home, kOrtDll));
        VoicevoxLoadOnnxruntimeOptions loadOptions = voicevox_make_default_load_onnxruntime_options();
        loadOptions.filename = ortPath.c_str();
        const int code = voicevox_onnxruntime_load_once(loadOptions, &onnxruntime_);
        if (code != VOICEVOX_RESULT_OK) {
            onnxruntime_ = nullptr;
            Log(L"init: onnxruntime failed (%d) %s", code, FromUtf8(ortPath.c_str()).c_str());
            return Fail("onnxruntime", code);
        }
        Log(L"init: onnxruntime loaded (%llu ms)", stage());
    }

    if (openJtalk_ == nullptr) {
        const std::wstring dict = DictDir(home);
        const std::string dictDir = ToUtf8(dict);
        const int code = voicevox_open_jtalk_rc_new(dictDir.c_str(), &openJtalk_);
        if (code != VOICEVOX_RESULT_OK) {
            openJtalk_ = nullptr;
            Log(L"init: open_jtalk failed (%d) %s", code, dict.c_str());
            return Fail("open_jtalk", code);
        }
        Log(L"init: dictionary %s (%llu ms)", dict.c_str(), stage());
    }

    VoicevoxInitializeOptions initOptions = voicevox_make_default_initialize_options();
    initOptions.acceleration_mode = VOICEVOX_ACCELERATION_MODE_CPU;
    initOptions.cpu_num_threads = 0;
    const int code = voicevox_synthesizer_new(onnxruntime_, openJtalk_, initOptions, &synthesizer_);
    if (code != VOICEVOX_RESULT_OK) {
        synthesizer_ = nullptr;
        Log(L"init: synthesizer failed (%d)", code);
        return Fail("synthesizer", code);
    }

    PinSelf();
    Log(L"init: ready (synthesizer %llu ms, total %llu ms)", stage(), GetTickCount64() - started);
    ready_ = true;
    return true;
}

bool Core::EnsureModel(int styleId) {
    const VoiceEntry* entry = FindStyle(styleId);
    if (entry == nullptr) {
        lastError_ = "unknown style id " + std::to_string(styleId);
        return false;
    }

    // Loading is per pack, not per style: one .vvm carries every style of its characters, so
    // the pack is the unit that costs memory and the unit the core can unload.
    const std::wstring pack = entry->vvm;
    auto loaded = packs_.find(pack);
    if (loaded != packs_.end()) {
        loaded->second.lastUsed = ++clock_;
        return true;
    }

    const std::string path = ToUtf8(Join(Join(HomeDir(), kModelsDir), entry->vvm));
    VoicevoxVoiceModelFile* model = nullptr;
    const ULONGLONG openStart = GetTickCount64();
    int code = voicevox_voice_model_file_open(path.c_str(), &model);
    if (code != VOICEVOX_RESULT_OK) {
        return Fail(("open model " + ToUtf8(entry->vvm)).c_str(), code);
    }
    // Split from the load below on purpose: `open` is read-and-parse, the only part a logon-time
    // warm-up or an antivirus exclusion can shorten. `load` builds ONNX Runtime sessions and is
    // pure CPU in this process, whatever the file cache holds.
    const ULONGLONG opened = GetTickCount64();

    LoadedPack state;
    voicevox_voice_model_file_id(model, &state.id);

    VoicevoxLoadVoiceModelOptions options = voicevox_make_default_load_voice_model_options();
    options.on_existing = VOICEVOX_ON_EXISTING_VOICE_MODEL_ID_SKIP;
    code = voicevox_synthesizer_load_voice_model(synthesizer_, model, options);
    const ULONGLONG loadedAt = GetTickCount64();
    voicevox_voice_model_file_delete(model);
    if (code != VOICEVOX_RESULT_OK) {
        Log(L"model: load failed for style %d (%d)", styleId, code);
        return Fail("load model", code);
    }

    state.lastUsed = ++clock_;
    packs_[pack] = state;
    EvictPacks(pack);
    Log(L"model: loaded %s for style %d (%zu pack(s) resident, open %llu ms, load %llu ms)",
        entry->vvm, styleId, packs_.size(), opened - openStart, loadedAt - opened);
    return true;
}

void Core::EvictPacks(const std::wstring& keep) {
    // The catalog covers 27 packs and each one is well over 100 MB of weights once loaded. A
    // host that reads a paragraph in every voice would grow without bound, so only the most
    // recently used few stay resident — reloading costs about a second, being killed by the
    // OS costs the utterance.
    const size_t limit = MaxResidentPacks();
    while (packs_.size() > limit) {
        auto oldest = packs_.end();
        for (auto it = packs_.begin(); it != packs_.end(); ++it) {
            if (it->first == keep) continue;
            if (oldest == packs_.end() || it->second.lastUsed < oldest->second.lastUsed) oldest = it;
        }
        if (oldest == packs_.end()) break;

        const int code = voicevox_synthesizer_unload_voice_model(
            synthesizer_, reinterpret_cast<VoicevoxVoiceModelId>(oldest->second.id));
        Log(L"model: unloaded %s (%d)", oldest->first.c_str(), code);
        packs_.erase(oldest);
    }
}

bool Core::Synthesize(int styleId, const std::string& utf8Text, float speed, float pitch,
                      float volume, std::vector<uint8_t>* pcm, std::string* error) {
    SynthJob job;
    job.core = this;
    job.styleId = styleId;
    job.text = &utf8Text;
    job.speed = speed;
    job.pitch = pitch;
    job.volume = volume;
    job.pcm = pcm;
    job.error = error;

    // Per utterance, not once per process: a Speak already costs hundreds of milliseconds of
    // synthesis, and CreateThread is tens of microseconds. The thread also gives the synthesizer a
    // stack the host cannot dictate — see kSynthStackBytes.
    HANDLE worker = CreateThread(nullptr, kSynthStackBytes, &Core::WorkerMain, &job, 0, nullptr);
    if (worker == nullptr) {
        // Saying nothing is worse than trying with the stack we were given, and the caller gets
        // the same answer either way.
        Log(L"synth: cannot start worker thread (error=%lu), running inline", GetLastError());
        return SynthesizeInline(styleId, utf8Text, speed, pitch, volume, pcm, error);
    }
    WaitForSingleObject(worker, INFINITE);
    CloseHandle(worker);
    return job.ok;
}

unsigned long __stdcall Core::WorkerMain(void* param) {
    auto* job = static_cast<SynthJob*>(param);
    try {
        job->ok = job->core->SynthesizeInline(job->styleId, *job->text, job->speed, job->pitch,
                                              job->volume, job->pcm, job->error);
    } catch (...) {
        // Nothing may cross a thread entry point: an escaping exception calls std::terminate and
        // takes the host with it, which is exactly the failure this thread exists to prevent.
        if (job->error != nullptr) *job->error = "synthesis threw an exception";
        job->ok = false;
        Log(L"synth: worker thread caught an exception");
    }
    return 0;
}

bool Core::SynthesizeInline(int styleId, const std::string& utf8Text, float speed, float pitch,
                            float volume, std::vector<uint8_t>* pcm, std::string* error) {
    std::lock_guard<std::mutex> guard(mutex_);
    pcm->clear();

    const auto failed = [&]() {
        if (error != nullptr) *error = lastError_;
        return false;
    };

    if (!EnsureReady()) return failed();
    if (!EnsureModel(styleId)) return failed();

    char* query = nullptr;
    // Spans text analysis and inference: the first query of a process also pays the dictionary's
    // first real use, and that belongs in the cold-start budget next to the model load.
    const ULONGLONG synthStart = GetTickCount64();
    int code = voicevox_synthesizer_create_audio_query(synthesizer_, utf8Text.c_str(),
                                                       (VoicevoxStyleId)styleId, &query);
    if (code != VOICEVOX_RESULT_OK) {
        Fail("audio query", code);
        return failed();
    }

    std::string json = query;
    voicevox_string_free(query);
    PatchNumber(&json, "speedScale", speed);
    PatchNumber(&json, "pitchScale", pitch);
    PatchNumber(&json, "volumeScale", volume);
    // Pinned, not inherited: GetOutputFormat promises 24 kHz and Speak writes whatever the core
    // hands back, so the two must not be able to drift apart on a core upgrade. A query without
    // the field means the schema moved, and saying so is better than playing 44.1 kHz data with a
    // 24 kHz header — that sounds like a broken voice, not like a bug with a fix.
    if (!PatchInt(&json, "outputSamplingRate", kOutputSampleRate)) {
        lastError_ = "the audio query has no outputSamplingRate field";
        Log(L"synth: audio query has no outputSamplingRate");
        return failed();
    }

    VoicevoxSynthesisOptions options = voicevox_make_default_synthesis_options();
    uintptr_t length = 0;
    uint8_t* wav = nullptr;
    code = voicevox_synthesizer_synthesis(synthesizer_, json.c_str(), (VoicevoxStyleId)styleId,
                                          options, &length, &wav);
    if (code != VOICEVOX_RESULT_OK) {
        Fail("synthesis", code);
        return failed();
    }

    unsigned rate = 0;
    const bool ok = StripWav(wav, (size_t)length, pcm, &rate);
    Log(L"synth: style=%d speed=%.3f pitch=%.3f volume=%.3f wav=%zu pcm=%zu rate=%u ok=%d (%llu ms)",
        styleId, speed, pitch, volume, (size_t)length, pcm->size(), rate, ok ? 1 : 0,
        GetTickCount64() - synthStart);
    voicevox_bytes_free(wav);
    if (!ok) {
        lastError_ = "synthesis returned a buffer without a data chunk";
        return failed();
    }
    if (rate != (unsigned)kOutputSampleRate) {
        // The core did not honour outputSamplingRate. SAPI was told 24 kHz before this call, so
        // this audio would play at the wrong speed; refusing it turns a pitch bug into a log line.
        lastError_ = "synthesis returned " + std::to_string(rate) + " Hz, expected " +
                     std::to_string(kOutputSampleRate);
        return failed();
    }
    return true;
}

}  // namespace voicevox

// Delay-load hook: resolve voicevox_core.dll by absolute path from the data directory, so the
// host process's directory layout never matters.
//
// Lives here rather than next to DllMain because two modules load it now — the engine DLL inside
// whatever process is speaking, and VoicevoxSapiServer.exe — and "how the core is found" belongs
// with the code that calls it. Both link with /DELAYLOAD:voicevox_core.dll; the hook is per module
// and each gets its own copy of this function.
//
// Returning NULL from dliNotePreLoadLibrary does not mean "the load failed" — it means "not
// handled", and delayhlp then falls through to LoadLibraryA("voicevox_core.dll") with the normal
// search order: the host's own directory, the current directory, PATH. That is exactly the search
// LOAD_LIBRARY_SEARCH_* above exists to rule out, so a failure here has to fail closed: the host
// executable is handed back as the "module", it cannot possibly export voicevox_synthesizer_new,
// and the import fails instead of picking up a same-named DLL from somewhere writable.
extern "C" FARPROC WINAPI VoicevoxDliHook(unsigned notify, PDelayLoadInfo info) {
    if (notify == dliNotePreLoadLibrary && info != nullptr && info->szDll != nullptr &&
        _stricmp(info->szDll, "voicevox_core.dll") == 0) {
        const std::wstring path = voicevox::Core::HomeDir() + L"\\voicevox_core.dll";
        HMODULE module = LoadLibraryExW(path.c_str(), nullptr,
                                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module != nullptr) return (FARPROC)module;

        voicevox::Log(L"delayload: cannot load %s (error=%lu), failing the import",
                      path.c_str(), GetLastError());
        return (FARPROC)GetModuleHandleW(nullptr);
    }
    return nullptr;
}

extern "C" const PfnDliHook __pfnDliNotifyHook2 = VoicevoxDliHook;
