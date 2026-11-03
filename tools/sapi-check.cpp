// voicevox-sapi-check — verifies the engine without a browser.
//
// It walks the same path Chrome does (SAPI5 voice token -> our CLSID -> ISpTTSEngine::Speak),
// so a clean run here means the browser will work too.
//
//   voicevox-sapi-check list
//   voicevox-sapi-check say "ずんだもん" "こんにちは。" out.wav
//
// Raw COM on purpose: sphelper.h pulls in ATL, which the Build Tools install here does not have,
// and this tool has no business dragging a UI framework into a console check.

#include <windows.h>
#include <sapi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <io.h>
#include <memory>
#include <set>
#include <string>

namespace {

// 0x411 = ja-JP, the language our tokens declare. Used to narrow enumeration to the voices
// this engine registers, the same way Chrome's voice list is later filtered by language.
constexpr wchar_t kVoiceFilter[] = L"Language=411";

template <class T>
struct ComPtr {
    T* p = nullptr;

    ~ComPtr() { reset(); }
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& other) noexcept : p(other.p) { other.p = nullptr; }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != std::addressof(other)) {
            reset();
            p = other.p;
            other.p = nullptr;
        }
        return *this;
    }

    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }

    void reset() {
        if (p != nullptr) {
            p->Release();
            p = nullptr;
        }
    }
};

std::wstring Description(ISpObjectToken* token) {
    // Same lookup Chromium's tts_win.cc does: the name keyed by the UI language id first, then
    // the token key's default value.
    wchar_t language[8] = {};
    std::swprintf(language, 8, L"%04X", (unsigned)GetUserDefaultUILanguage());

    LPWSTR text = nullptr;
    if (SUCCEEDED(token->GetStringValue(language, &text)) && text != nullptr) {
        std::wstring out = text;
        CoTaskMemFree(text);
        return out;
    }
    if (SUCCEEDED(token->GetStringValue(nullptr, &text)) && text != nullptr) {
        std::wstring out = text;
        CoTaskMemFree(text);
        return out;
    }
    return std::wstring();
}

/**
 * True when any of the token's names contains `wanted`.
 *
 * <p>Every token carries a name per UI language (0411 Japanese, 0409 English, 0804 Chinese), so
 * a command written in Japanese still finds the voice on an English machine — which is what all
 * the examples in the README do.
 */
bool MatchesName(ISpObjectToken* token, const std::wstring& wanted) {
    static const wchar_t* const kNames[] = {nullptr, L"0411", L"0409", L"0804"};
    for (const wchar_t* name : kNames) {
        LPWSTR text = nullptr;
        if (SUCCEEDED(token->GetStringValue(name, &text)) && text != nullptr) {
            const bool hit = std::wstring(text).find(wanted) != std::wstring::npos;
            CoTaskMemFree(text);
            if (hit) return true;
        }
    }
    return false;
}

int List(bool all) {
    ComPtr<ISpObjectTokenCategory> category;
    if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL,
                                IID_ISpObjectTokenCategory, (void**)&category))) {
        return 1;
    }
    if (FAILED(category->SetId(SPCAT_VOICES, FALSE))) return 1;

    ComPtr<IEnumSpObjectTokens> tokens;
    if (FAILED(category->EnumTokens(all ? nullptr : kVoiceFilter, nullptr, &tokens))) return 1;

    ULONG count = 0;
    tokens->GetCount(&count);

    // Characters, not just voices: one character owns a voice per style, and it is the character
    // count that decides whether the picker feels long (0.vvm alone is 10 voices / 4 characters).
    // Token keys are VOICEVOX_<character>_<styleId>, so the leaf without the id suffix is the
    // character. Foreign voices are skipped — they have no such key shape.
    std::set<std::wstring> characters;
    for (ULONG i = 0; i < count; ++i) {
        ComPtr<ISpObjectToken> token;
        if (FAILED(tokens->Item(i, &token))) continue;
        LPWSTR id = nullptr;
        if (FAILED(token->GetId(&id)) || id == nullptr) continue;
        std::wstring leaf = id;
        CoTaskMemFree(id);
        const size_t slash = leaf.find_last_of(L'\\');
        if (slash != std::wstring::npos) leaf.erase(0, slash + 1);
        constexpr size_t kPrefix = 9;  // "VOICEVOX_"
        if (leaf.compare(0, kPrefix, L"VOICEVOX_") != 0) continue;
        leaf.erase(0, kPrefix);
        const size_t styleSep = leaf.find_last_of(L'_');
        if (styleSep != std::wstring::npos) leaf.erase(styleSep);
        characters.insert(leaf);
    }

    std::wprintf(L"%lu voice(s), %d character(s), filter=%s\n", count, (int)characters.size(),
                 all ? L"(none)" : kVoiceFilter);
    for (ULONG i = 0; i < count; ++i) {
        ComPtr<ISpObjectToken> token;
        if (FAILED(tokens->Item(i, &token))) continue;

        LPWSTR id = nullptr;
        token->GetId(&id);

        std::wprintf(L"  id=%s\n     name=%s\n", id != nullptr ? id : L"?",
                     Description(token.p).c_str());

        if (id != nullptr) CoTaskMemFree(id);
    }
    return 0;
}

int Say(const std::wstring& wanted, const std::wstring& text, const std::wstring& outPath,
        long rate, USHORT volume) {
    ComPtr<ISpObjectTokenCategory> category;
    if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL,
                                IID_ISpObjectTokenCategory, (void**)&category))) {
        return 1;
    }
    if (FAILED(category->SetId(SPCAT_VOICES, FALSE))) return 1;

    ComPtr<IEnumSpObjectTokens> tokens;
    if (FAILED(category->EnumTokens(kVoiceFilter, nullptr, &tokens))) return 1;

    ComPtr<ISpObjectToken> chosen;
    ULONG count = 0;
    tokens->GetCount(&count);
    for (ULONG i = 0; i < count; ++i) {
        ComPtr<ISpObjectToken> token;
        if (FAILED(tokens->Item(i, &token))) continue;
        if (!MatchesName(token.p, wanted)) continue;
        chosen = std::move(token);
        std::wprintf(L"voice: %s\n", Description(chosen.p).c_str());
        break;
    }
    if (!chosen) {
        std::fwprintf(stderr, L"no voice matching \"%s\"\n", wanted.c_str());
        return 1;
    }

    ComPtr<ISpVoice> voice;
    if (FAILED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice,
                                (void**)&voice))) {
        return 1;
    }
    if (FAILED(voice->SetVoice(chosen.p))) return 1;

    // Rate and volume go through SAPI, not around it: the engine reads them back from
    // ISpTTSEngineSite, so setting them here exercises the same conversion the browser will.
    voice->SetRate(rate);
    voice->SetVolume(volume);

    // Bound at the format the engine declares (24 kHz / 16 bit / mono) so SAPI does not have to
    // resample; a mismatch here would still work but would hide a wrong GetOutputFormat.
    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = 24000;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 2;
    format.nAvgBytesPerSec = 48000;

    ComPtr<ISpStream> stream;
    if (FAILED(CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL, IID_ISpStream,
                                (void**)&stream))) {
        return 1;
    }
    HRESULT hr = stream->BindToFile(outPath.c_str(), SPFM_CREATE_ALWAYS, &SPDFID_WaveFormatEx,
                                    &format, 0);
    if (FAILED(hr)) {
        std::fwprintf(stderr, L"cannot open %s (0x%08lX)\n", outPath.c_str(), (unsigned long)hr);
        return 1;
    }
    if (FAILED(voice->SetOutput(stream.p, FALSE))) return 1;

    const DWORD started = GetTickCount();
    const HRESULT speakHr = voice->Speak(text.c_str(), SPF_DEFAULT, nullptr);
    if (FAILED(speakHr)) {
        std::fwprintf(stderr, L"Speak failed: 0x%08lX\n", (unsigned long)speakHr);
        return 1;
    }
    voice->WaitUntilDone(60000);
    stream->Close();

    std::wprintf(L"wrote %s in %lu ms\n", outPath.c_str(), GetTickCount() - started);
    return 0;
}

/**
 * Speaks the same line with the first `count` Japanese voices, one after another, in one
 * process.
 *
 * <p>This is the only way to exercise the model-pack eviction: each `say` run is a new process
 * with an empty cache, so a leak or a stale unload would never show up there. It also prints
 * per-voice latency, which is what the streaming split is measured against.
 */
int Bench(int count, const std::wstring& text, const std::wstring& outDir) {
    ComPtr<ISpObjectTokenCategory> category;
    if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL,
                                IID_ISpObjectTokenCategory, (void**)&category))) {
        return 1;
    }
    if (FAILED(category->SetId(SPCAT_VOICES, FALSE))) return 1;

    ComPtr<IEnumSpObjectTokens> tokens;
    if (FAILED(category->EnumTokens(kVoiceFilter, nullptr, &tokens))) return 1;

    ULONG total = 0;
    tokens->GetCount(&total);
    const int limit = count <= 0 ? (int)total : std::min<int>(count, (int)total);

    ComPtr<ISpVoice> voice;
    if (FAILED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice,
                                (void**)&voice))) {
        return 1;
    }

    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = 24000;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 2;
    format.nAvgBytesPerSec = 48000;

    for (int i = 0; i < limit; ++i) {
        ComPtr<ISpObjectToken> token;
        if (FAILED(tokens->Item((ULONG)i, &token))) continue;
        if (FAILED(voice->SetVoice(token.p))) continue;

        wchar_t path[MAX_PATH] = {};
        std::swprintf(path, MAX_PATH, L"%s\\bench-%03d.wav", outDir.c_str(), i);

        ComPtr<ISpStream> stream;
        if (FAILED(CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL, IID_ISpStream,
                                    (void**)&stream))) {
            return 1;
        }
        if (FAILED(stream->BindToFile(path, SPFM_CREATE_ALWAYS, &SPDFID_WaveFormatEx, &format, 0))) {
            return 1;
        }
        if (FAILED(voice->SetOutput(stream.p, FALSE))) return 1;

        const DWORD started = GetTickCount();
        const HRESULT hr = voice->Speak(text.c_str(), SPF_DEFAULT, nullptr);
        if (FAILED(hr)) {
            std::wprintf(L"%3d  FAILED 0x%08lX\n", i, (unsigned long)hr);
        } else {
            voice->WaitUntilDone(60000);
            std::wprintf(L"%3d  %6lu ms  %s\n", i, GetTickCount() - started,
                         Description(token.p).c_str());
        }
        stream->Close();
    }
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // Voice names are Japanese, English and Chinese; the default console stream would replace
    // every one of them with '?' on a Chinese or Japanese code page. UTF-8 output keeps the
    // listing readable both in a console and when piped into a file.
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_U8TEXT);

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;

    int result = 2;
    if (argc >= 2 && std::wstring(argv[1]) == L"list") {
        result = List(false);
    } else if (argc >= 2 && std::wstring(argv[1]) == L"all") {
        result = List(true);
    } else if (argc >= 5 && std::wstring(argv[1]) == L"say") {
        // Optional trailing rate (-10..10) and volume (0..100), for checking the SAPI ->
        // VOICEVOX scale conversion without a browser.
        const long rate = argc >= 6 ? std::wcstol(argv[5], nullptr, 10) : 0;
        const long volume = argc >= 7 ? std::wcstol(argv[6], nullptr, 10) : 100;
        result = Say(argv[2], argv[3], argv[4], rate, (USHORT)std::max(0L, std::min(100L, volume)));
    } else if (argc >= 3 && std::wstring(argv[1]) == L"bench") {
        // bench <out-dir> [count] [text]
        const int count = argc >= 4 ? std::wcstol(argv[3], nullptr, 10) : 0;
        const std::wstring text = argc >= 5 ? argv[4] : L"こんにちは。";
        result = Bench((int)count, text, argv[2]);
    } else {
        std::fwprintf(stderr,
                      L"usage:\n  voicevox-sapi-check list\n"
                      L"  voicevox-sapi-check say <voice> <text> <out.wav> [rate] [volume]\n"
                      L"    rate    -10..10, 0 = normal\n"
                      L"    volume  0..100, 100 = normal\n"
                      L"  voicevox-sapi-check bench <out-dir> [count] [text]\n"
                      L"    speaks the same line with the first <count> voices in one process\n");
    }

    CoUninitialize();
    return result;
}
