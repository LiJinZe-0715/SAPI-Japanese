#include "core.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>

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

// A failed bring-up is retried, but not on every utterance: a missing runtime makes every
// attempt cost a LoadLibrary and a log line, and a page reading a paragraph calls Speak per
// sentence. Five seconds is long enough to stay quiet and short enough that an install that was
// still copying files when the first Speak arrived is picked up without restarting the browser.
constexpr ULONGLONG kRetryAfterMs = 5000;

std::wstring Join(const std::wstring& a, const wchar_t* b) {
    std::wstring out = a;
    if (!out.empty() && out.back() != L'\\') out.push_back(L'\\');
    out += b;
    return out;
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
        Log(L"init: core dll loaded from %s", home.c_str());
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
    }

    if (openJtalk_ == nullptr) {
        const std::string dictDir = ToUtf8(Join(home, kDictDir));
        const int code = voicevox_open_jtalk_rc_new(dictDir.c_str(), &openJtalk_);
        if (code != VOICEVOX_RESULT_OK) {
            openJtalk_ = nullptr;
            Log(L"init: open_jtalk failed (%d) %s", code, FromUtf8(dictDir.c_str()).c_str());
            return Fail("open_jtalk", code);
        }
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
    Log(L"init: ready");
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
    int code = voicevox_voice_model_file_open(path.c_str(), &model);
    if (code != VOICEVOX_RESULT_OK) {
        return Fail(("open model " + ToUtf8(entry->vvm)).c_str(), code);
    }

    LoadedPack state;
    voicevox_voice_model_file_id(model, &state.id);

    VoicevoxLoadVoiceModelOptions options = voicevox_make_default_load_voice_model_options();
    options.on_existing = VOICEVOX_ON_EXISTING_VOICE_MODEL_ID_SKIP;
    code = voicevox_synthesizer_load_voice_model(synthesizer_, model, options);
    voicevox_voice_model_file_delete(model);
    if (code != VOICEVOX_RESULT_OK) {
        Log(L"model: load failed for style %d (%d)", styleId, code);
        return Fail("load model", code);
    }

    state.lastUsed = ++clock_;
    packs_[pack] = state;
    EvictPacks(pack);
    Log(L"model: loaded %s for style %d (%zu pack(s) resident)", entry->vvm, styleId, packs_.size());
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
    std::lock_guard<std::mutex> guard(mutex_);
    pcm->clear();

    const auto failed = [&]() {
        if (error != nullptr) *error = lastError_;
        return false;
    };

    if (!EnsureReady()) return failed();
    if (!EnsureModel(styleId)) return failed();

    char* query = nullptr;
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
    // Pinned, not inherited: GetOutputFormat promises 24 kHz and Speak writes whatever the
    // core hands back, so the two must not be able to drift apart on a core upgrade.
    PatchInt(&json, "outputSamplingRate", 24000);

    VoicevoxSynthesisOptions options = voicevox_make_default_synthesis_options();
    uintptr_t length = 0;
    uint8_t* wav = nullptr;
    code = voicevox_synthesizer_synthesis(synthesizer_, json.c_str(), (VoicevoxStyleId)styleId,
                                          options, &length, &wav);
    if (code != VOICEVOX_RESULT_OK) {
        Fail("synthesis", code);
        return failed();
    }

    const bool ok = StripWav(wav, (size_t)length, pcm);
    Log(L"synth: style=%d speed=%.3f pitch=%.3f volume=%.3f wav=%zu pcm=%zu ok=%d", styleId, speed,
        pitch, volume, (size_t)length, pcm->size(), ok ? 1 : 0);
    voicevox_bytes_free(wav);
    if (!ok) {
        lastError_ = "synthesis returned a buffer without a data chunk";
        return failed();
    }
    return true;
}

}  // namespace voicevox
