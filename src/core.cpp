#include "core.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "catalog.h"
#include "log.h"
#include "text.h"

namespace voicevox {
namespace {

constexpr wchar_t kHomeEnv[] = L"VOICEVOX_SAPI_HOME";
constexpr wchar_t kCoreDll[] = L"voicevox_core.dll";
constexpr wchar_t kOrtDll[] = L"voicevox_onnxruntime.dll";
constexpr wchar_t kDictDir[] = L"dict\\open_jtalk_dic_utf_8-1.11";
constexpr wchar_t kModelsDir[] = L"models";

std::wstring Join(const std::wstring& a, const wchar_t* b) {
    std::wstring out = a;
    if (!out.empty() && out.back() != L'\\') out.push_back(L'\\');
    out += b;
    return out;
}

/**
 * Replaces the value that follows "key" with a literal.
 *
 * <p>Speed and pitch only exist inside the AudioQuery JSON — the C API has no field for them on
 * VoicevoxSynthesisOptions — so the JSON is patched in place. The key names come from the
 * AudioQuery schema and are stable across 0.17.x.
 */
void PatchLiteral(std::string* json, const char* key, const char* literal) {
    const std::string needle = std::string("\"") + key + "\"";
    const size_t at = json->find(needle);
    if (at == std::string::npos) return;

    size_t colon = json->find(':', at + needle.size());
    if (colon == std::string::npos) return;

    size_t start = colon + 1;
    while (start < json->size() && std::isspace((unsigned char)(*json)[start])) ++start;

    size_t end = start;
    while (end < json->size()) {
        const char c = (*json)[end];
        const bool part = std::isdigit((unsigned char)c) || c == '-' || c == '+' || c == '.' ||
                          c == 'e' || c == 'E';
        if (!part) break;
        ++end;
    }
    if (end == start) return;

    json->replace(start, end - start, literal);
}

/**
 * Writes a float into the AudioQuery JSON.
 *
 * <p>Formatted with snprintf and then repaired, rather than trusting the C locale: this code runs
 * inside whatever host loaded us, and a host that called setlocale(LC_ALL, "") on a machine with
 * a decimal comma gets "1,500" here. The core parses the query as JSON, so that single character
 * turns every utterance into a parse error that looks exactly like a broken install.
 *
 * <p>Non-finite values are floored to 0 first: "%.3f" of inf or nan prints "inf"/"nan", which is
 * not a JSON number either.
 */
void PatchNumber(std::string* json, const char* key, float value) {
    if (!std::isfinite(value)) value = 0.0f;
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.3f", value);
    for (char* at = buffer; *at != '\0'; ++at) {
        if (*at == ',') *at = '.';
    }
    PatchLiteral(json, key, buffer);
}

/** Integer fields (outputSamplingRate is a u32 in the schema) must not carry a decimal point. */
void PatchInt(std::string* json, const char* key, int value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%d", value);
    PatchLiteral(json, key, buffer);
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

/** Finds the `data` chunk of a RIFF/WAVE buffer and copies the PCM out of it. */
bool StripWav(const uint8_t* data, size_t size, std::vector<uint8_t>* pcm) {
    if (size < 12 || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0) {
        return false;
    }
    size_t pos = 12;
    while (pos + 8 <= size) {
        const uint8_t* id = data + pos;
        const uint32_t length = (uint32_t)data[pos + 4] | ((uint32_t)data[pos + 5] << 8) |
                                ((uint32_t)data[pos + 6] << 16) | ((uint32_t)data[pos + 7] << 24);
        pos += 8;
        if (std::memcmp(id, "data", 4) == 0) {
            const size_t available = std::min<size_t>(length, size - pos);
            pcm->assign(data + pos, data + pos + available);
            return !pcm->empty();
        }
        pos += length + (length & 1);
    }
    return false;
}

}  // namespace

Core& Core::Instance() {
    static Core instance;
    return instance;
}

std::wstring Core::HomeDir() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(kHomeEnv, buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) return buffer;

    std::wstring base;
    PWSTR programData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &programData))) {
        base = programData;
        CoTaskMemFree(programData);
    }
    if (base.empty()) base = L"C:\\ProgramData";
    return Join(base, L"VoicevoxSapi");
}

bool Core::Fail(const char* what, int resultCode) {
    lastError_ = std::string(what) + ": " + voicevox_error_result_to_message((VoicevoxResultCode)resultCode);
    return false;
}

bool Core::EnsureReady() {
    if (ready_) return true;
    if (attempted_) return false;
    attempted_ = true;

    const std::wstring home = HomeDir();

    // Loaded by absolute path: SAPI loads this DLL inside the host process (Chrome), where the
    // dependency search would otherwise look in Chrome's directory.
    if (LoadLibraryExW(Join(home, kCoreDll).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) ==
        nullptr) {
        lastError_ = "voicevox_core.dll not found under " + ToUtf8(home);
        Log(L"init: core dll load failed, error=%lu, home=%s", GetLastError(), home.c_str());
        return false;
    }
    Log(L"init: core dll loaded from %s", home.c_str());

    const std::string ortPath = ToUtf8(Join(home, kOrtDll));
    VoicevoxLoadOnnxruntimeOptions loadOptions = voicevox_make_default_load_onnxruntime_options();
    loadOptions.filename = ortPath.c_str();
    int code = voicevox_onnxruntime_load_once(loadOptions, &onnxruntime_);
    if (code != VOICEVOX_RESULT_OK) {
        Log(L"init: onnxruntime failed (%d) %s", code, FromUtf8(ortPath.c_str()).c_str());
        return Fail("onnxruntime", code);
    }

    const std::string dictDir = ToUtf8(Join(home, kDictDir));
    code = voicevox_open_jtalk_rc_new(dictDir.c_str(), &openJtalk_);
    if (code != VOICEVOX_RESULT_OK) {
        Log(L"init: open_jtalk failed (%d) %s", code, FromUtf8(dictDir.c_str()).c_str());
        return Fail("open_jtalk", code);
    }

    VoicevoxInitializeOptions initOptions = voicevox_make_default_initialize_options();
    initOptions.acceleration_mode = VOICEVOX_ACCELERATION_MODE_CPU;
    initOptions.cpu_num_threads = 0;
    code = voicevox_synthesizer_new(onnxruntime_, openJtalk_, initOptions, &synthesizer_);
    if (code != VOICEVOX_RESULT_OK) {
        Log(L"init: synthesizer failed (%d)", code);
        return Fail("synthesizer", code);
    }

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
                      float volume, std::vector<uint8_t>* pcm) {
    std::lock_guard<std::mutex> guard(mutex_);
    pcm->clear();

    if (!EnsureReady()) return false;
    if (!EnsureModel(styleId)) return false;

    char* query = nullptr;
    int code = voicevox_synthesizer_create_audio_query(synthesizer_, utf8Text.c_str(),
                                                       (VoicevoxStyleId)styleId, &query);
    if (code != VOICEVOX_RESULT_OK) return Fail("audio query", code);

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
    if (code != VOICEVOX_RESULT_OK) return Fail("synthesis", code);

    const bool ok = StripWav(wav, (size_t)length, pcm);
    Log(L"synth: style=%d speed=%.3f pitch=%.3f volume=%.3f wav=%zu pcm=%zu ok=%d", styleId, speed,
        pitch, volume, (size_t)length, pcm->size(), ok ? 1 : 0);
    voicevox_bytes_free(wav);
    if (!ok) {
        lastError_ = "synthesis returned a buffer without a data chunk";
        return false;
    }
    return true;
}

}  // namespace voicevox
