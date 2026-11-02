#include "engine.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <new>
#include <vector>

#include "catalog.h"
#include "core.h"
#include "log.h"
#include "text.h"

namespace voicevox {
namespace {

// VOICEVOX always renders 24 kHz / 16-bit / mono; GetOutputFormat promises exactly that and
// Speak writes nothing else, because SAPI trusts the header it was given.
constexpr WORD kSampleRate = 24000;
constexpr WORD kChannels = 1;
constexpr WORD kBitsPerSample = 16;

// 8 KB per Write keeps an abort responsive without paying for a call per sample.
constexpr size_t kWriteChunk = 8192;

// SAPI rate counts steps from 0 (0 = normal) while VOICEVOX speedScale is a multiplier from
// 1.0. 2^(rate/10) maps 10 -> 2x and -10 -> 0.5x, which is what the SAPI documentation
// describes and what Chrome's own rate mapping assumes.
float SpeedScale(long sapiRate) {
    const float speed = std::pow(2.0f, (float)sapiRate / 10.0f);
    return std::min(2.0f, std::max(0.5f, speed));
}

// SAPI volume is 0..100 with 100 = normal; VOICEVOX volumeScale is 0..2 with 1.0 = normal.
float VolumeScale(USHORT sapiVolume) {
    return std::min(1.0f, (float)sapiVolume / 100.0f);
}

// Sentence ends. Anything past one of these is a safe place to synthesize separately without
// hearing the seam, which is the whole point: a five-sentence paragraph used to be one 2-second
// wait before the first sample.
constexpr wchar_t kSentenceEnds[] = L"。．！？!?\n";

// Where a run-on sentence may still be cut. Commas are audible seams, so they are only used
// when a single sentence would otherwise be too long to start speaking promptly.
constexpr wchar_t kSoftBreaks[] = L"、，,；;：:　 ";

// Beyond this many characters a single sentence is cut at the last soft break. Roughly five
// seconds of speech, which is already a long silence to sit through.
constexpr size_t kMaxSentence = 60;

std::wstring Trimmed(const std::wstring& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && iswspace(text[begin])) ++begin;
    while (end > begin && iswspace(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

void PushIfSpoken(std::vector<std::wstring>* out, const std::wstring& piece) {
    const std::wstring trimmed = Trimmed(piece);
    if (!trimmed.empty()) out->push_back(trimmed);
}

/** Splits on sentence ends, then on soft breaks for sentences that are too long to wait for. */
std::vector<std::wstring> SplitSentences(const std::wstring& text) {
    std::vector<std::wstring> sentences;
    std::wstring current;
    for (const wchar_t c : text) {
        current.push_back(c);
        if (wcschr(kSentenceEnds, c) != nullptr) {
            PushIfSpoken(&sentences, current);
            current.clear();
        }
    }
    PushIfSpoken(&sentences, current);

    std::vector<std::wstring> out;
    for (const std::wstring& sentence : sentences) {
        if (sentence.size() <= kMaxSentence) {
            out.push_back(sentence);
            continue;
        }
        size_t start = 0;
        while (start < sentence.size()) {
            size_t cut = std::min(start + kMaxSentence, sentence.size());
            if (cut < sentence.size()) {
                const size_t soft = sentence.find_last_of(kSoftBreaks, cut - 1);
                if (soft != std::wstring::npos && soft + 1 > start) cut = soft + 1;
            }
            PushIfSpoken(&out, sentence.substr(start, cut - start));
            start = cut;
        }
    }
    return out;
}

}  // namespace

Engine::Engine(int styleId) : refs_(1), styleId_(styleId), token_(nullptr) {
    InterlockedIncrement(&g_liveObjects);
}

Engine::~Engine() {
    if (token_ != nullptr) token_->Release();
}

HRESULT Engine::Create(int styleId, REFIID riid, void** ppv) {
    if (ppv == nullptr) return E_POINTER;
    *ppv = nullptr;

    Engine* engine = new (std::nothrow) Engine(styleId);
    if (engine == nullptr) return E_OUTOFMEMORY;

    const HRESULT hr = engine->QueryInterface(riid, ppv);
    engine->Release();
    return hr;
}

STDMETHODIMP Engine::QueryInterface(REFIID riid, void** ppv) {
    if (ppv == nullptr) return E_POINTER;
    {
        wchar_t text[64] = {};
        StringFromGUID2(riid, text, 64);
        Log(L"qi: %s", text);
    }
    // Both bases derive from IUnknown, so IUnknown must answer with one fixed pointer — the
    // first base — or callers comparing pointers across casts would see two different objects.
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, __uuidof(ISpTTSEngine))) {
        *ppv = static_cast<ISpTTSEngine*>(this);
        AddRef();
        return S_OK;
    }
    if (IsEqualIID(riid, __uuidof(ISpObjectWithToken))) {
        *ppv = static_cast<ISpObjectWithToken*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) Engine::AddRef() {
    return (ULONG)InterlockedIncrement(&refs_);
}

STDMETHODIMP_(ULONG) Engine::Release() {
    const LONG left = InterlockedDecrement(&refs_);
    if (left == 0) {
        InterlockedDecrement(&g_liveObjects);
        delete this;
    }
    return (ULONG)left;
}

STDMETHODIMP Engine::GetOutputFormat(const GUID* targetFormatId, const WAVEFORMATEX* targetFormat,
                                     GUID* outputFormatId, WAVEFORMATEX** allocated) {
    Log(L"getoutputformat: called");
    if (outputFormatId == nullptr || allocated == nullptr) return E_POINTER;
    *allocated = nullptr;

    WAVEFORMATEX* format =
        static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
    if (format == nullptr) return E_OUTOFMEMORY;

    ZeroMemory(format, sizeof(WAVEFORMATEX));
    format->wFormatTag = WAVE_FORMAT_PCM;
    format->nChannels = kChannels;
    format->nSamplesPerSec = kSampleRate;
    format->wBitsPerSample = kBitsPerSample;
    format->nBlockAlign = (WORD)(kChannels * kBitsPerSample / 8);
    format->nAvgBytesPerSec = kSampleRate * format->nBlockAlign;
    format->cbSize = 0;

    *outputFormatId = SPDFID_WaveFormatEx;
    *allocated = format;
    return S_OK;
}

STDMETHODIMP Engine::Speak(DWORD speakFlags, REFGUID formatId, const WAVEFORMATEX* format,
                           const SPVTEXTFRAG* fragments, ISpTTSEngineSite* site) {
    if (site == nullptr) return E_POINTER;

    std::wstring text;
    for (const SPVTEXTFRAG* fragment = fragments; fragment != nullptr; fragment = fragment->pNext) {
        if (fragment->State.eAction != SPVA_Speak) continue;
        if (fragment->pTextStart == nullptr || fragment->ulTextLen == 0) continue;
        text.append(fragment->pTextStart, fragment->ulTextLen);
    }
    if (text.empty()) return S_OK;

    // Both SAPI knobs are converted here, next to the site calls, so the baseline mismatch
    // stays in one place.
    long rate = 0;
    if (FAILED(site->GetRate(&rate))) rate = 0;
    const float speed = SpeedScale(rate);

    USHORT volume = 100;
    if (FAILED(site->GetVolume(&volume))) volume = 100;
    const float volumeScale = VolumeScale(volume);

    // Pitch is deliberately not read: ISpTTSEngineSite exposes Write, GetRate, GetVolume,
    // GetSkipInfo and CompleteSkip, and nothing else — there is no pitch channel in the SAPI
    // engine contract. VOICEVOX's pitchScale is a log2 multiplier whose neutral value is 0.0
    // (not 1.0), and it only stays usable within about ±0.15; a hardcoded 1.0 raised every
    // utterance a full octave and the vocoder turned to noise. Leave it neutral.
    constexpr float kNeutralPitch = 0.0f;

    // Sentence by sentence, not all at once: the core synthesizes a whole query before it
    // returns a single sample, so one big utterance means one long silence up front. Splitting
    // at sentence ends trades a barely audible seam for audio starting after the first sentence
    // instead of after the last.
    const std::vector<std::wstring> sentences = SplitSentences(text);

    Log(L"speak: style=%d chars=%zu sentences=%zu rate=%ld volume=%u -> speed=%.3f volumeScale=%.3f",
        styleId_, text.size(), sentences.size(), rate, (unsigned)volume, speed, volumeScale);

    for (size_t index = 0; index < sentences.size(); ++index) {
        // GetActions() returns the flag set directly; SAPI raises it when the caller stops.
        if ((site->GetActions() & SPVES_ABORT) != 0) return S_OK;

        std::vector<uint8_t> pcm;
        const DWORD started = GetTickCount();
        if (!Core::Instance().Synthesize(styleId_, ToUtf8(sentences[index]), speed, kNeutralPitch,
                                         volumeScale, &pcm)) {
            Log(L"speak: synthesis failed on sentence %zu: %s", index,
                FromUtf8(Core::Instance().LastError().c_str()).c_str());
            // Silent failure on purpose: returning an error here still speaks nothing, but it also
            // makes some hosts surface a dialog. The reason is kept for the diagnostics tool.
            return S_OK;
        }
        if (index == 0) {
            Log(L"speak: first audio after %lu ms", GetTickCount() - started);
        }

        for (size_t offset = 0; offset < pcm.size(); offset += kWriteChunk) {
            if ((site->GetActions() & SPVES_ABORT) != 0) return S_OK;

            const ULONG size = (ULONG)std::min(kWriteChunk, pcm.size() - offset);
            ULONG written = 0;
            if (FAILED(site->Write(pcm.data() + offset, size, &written))) return S_OK;
        }
    }
    return S_OK;
}

STDMETHODIMP Engine::SetObjectToken(ISpObjectToken* token) {
    if (token == nullptr) return E_INVALIDARG;

    // Trust the token over the CLSID when both are available: they are written by the same
    // installer, and if they ever disagree the token is what SAPI will show the user.
    LPWSTR clsidText = nullptr;
    if (SUCCEEDED(token->GetStringValue(L"CLSID", &clsidText)) && clsidText != nullptr) {
        CLSID clsid = {};
        int styleId = 0;
        if (SUCCEEDED(CLSIDFromString(clsidText, &clsid)) && StyleFromClsid(clsid, &styleId) &&
            styleId != styleId_) {
            Log(L"settoken: style %d -> %d", styleId_, styleId);
            styleId_ = styleId;
        }
        CoTaskMemFree(clsidText);
    }

    if (token_ != nullptr) token_->Release();
    token_ = token;
    token_->AddRef();
    Log(L"settoken: style=%d", styleId_);
    return S_OK;
}

STDMETHODIMP Engine::GetObjectToken(ISpObjectToken** token) {
    if (token == nullptr) return E_POINTER;
    *token = token_;
    if (token_ == nullptr) return S_FALSE;
    token_->AddRef();
    return S_OK;
}

}  // namespace voicevox
