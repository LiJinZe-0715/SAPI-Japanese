#include "engine.h"

#include <algorithm>
#include <cwchar>
#include <new>
#include <vector>

#include "audio_util.h"
#include "catalog.h"
#include "core.h"
#include "log.h"
#include "sentences.h"
#include "text.h"

namespace voicevox {
namespace {

// VOICEVOX always renders 24 kHz / 16-bit / mono; GetOutputFormat promises exactly that and
// Speak writes nothing else, because SAPI trusts the header it was given.
constexpr WORD kSampleRate = 24000;
constexpr WORD kChannels = 1;
constexpr WORD kBitsPerSample = 16;
constexpr ULONG kBytesPerMs = kSampleRate / 1000 * kChannels * kBitsPerSample / 8;

// 8 KB per Write keeps an abort responsive without paying for a call per sample.
constexpr size_t kWriteChunk = 8192;

/**
 * What one stretch of the input asks for.
 *
 * <p>SAPI parses the XML in the text (<rate>, <volume>, <pitch>, <silence>, <bookmark>) and hands
 * the engine fragments, each with the state in force for it. Chrome relies on this: it wraps
 * every utterance in <pitch absmiddle="..."> to carry utterance.pitch. Adjacent fragments with
 * the same state are merged back into one text, so a sentence split by a tag is still one
 * sentence to the synthesizer.
 */
struct Segment {
    enum class Kind { Text, Silence, Bookmark } kind = Kind::Text;
    std::wstring text;           // Text: what to say; Bookmark: the bookmark's name
    std::vector<ULONG> source;   // Text: offset in the caller's string of each character
    long rateAdj = 0;            // added to the voice's rate
    ULONG volume = 100;          // XML volume level, scales the voice's volume
    long pitch = 0;              // <pitch absmiddle>, -10..10
    ULONG silenceMs = 0;         // Silence
};

bool SameState(const Segment& segment, const SPVSTATE& state) {
    return segment.rateAdj == state.RateAdj && segment.volume == state.Volume &&
           segment.pitch == state.PitchAdj.MiddleAdj;
}

std::vector<Segment> Segments(const SPVTEXTFRAG* fragments) {
    std::vector<Segment> segments;
    for (const SPVTEXTFRAG* fragment = fragments; fragment != nullptr; fragment = fragment->pNext) {
        const SPVSTATE& state = fragment->State;
        switch (state.eAction) {
            case SPVA_Speak:
            case SPVA_SpellOut: {
                // Spelled-out text is still text to say; VOICEVOX reads letters and digits one
                // by one anyway. Dropping it, as before, made words vanish from the middle of
                // a sentence.
                if (fragment->pTextStart == nullptr || fragment->ulTextLen == 0) break;
                if (segments.empty() || segments.back().kind != Segment::Kind::Text ||
                    !SameState(segments.back(), state)) {
                    Segment segment;
                    segment.rateAdj = state.RateAdj;
                    segment.volume = state.Volume;
                    segment.pitch = state.PitchAdj.MiddleAdj;
                    segments.push_back(segment);
                }
                Segment& segment = segments.back();
                segment.text.append(fragment->pTextStart, fragment->ulTextLen);
                for (ULONG i = 0; i < fragment->ulTextLen; ++i) {
                    segment.source.push_back(fragment->ulTextSrcOffset + i);
                }
                break;
            }
            case SPVA_Silence: {
                Segment segment;
                segment.kind = Segment::Kind::Silence;
                segment.silenceMs = state.SilenceMSecs;
                segments.push_back(segment);
                break;
            }
            case SPVA_Bookmark: {
                Segment segment;
                segment.kind = Segment::Kind::Bookmark;
                if (fragment->pTextStart != nullptr) {
                    segment.text.assign(fragment->pTextStart, fragment->ulTextLen);
                }
                segments.push_back(segment);
                break;
            }
            default:
                // SPVA_Pronounce carries phoneme ids VOICEVOX cannot take; section and unknown
                // tags carry nothing to say.
                break;
        }
    }
    return segments;
}

/** One Speak call: the site, what the host asked for, and how much audio has gone out. */
class Speaker {
public:
    explicit Speaker(ISpTTSEngineSite* site) : site_(site) {
        ULONGLONG interest = 0;
        if (SUCCEEDED(site_->GetEventInterest(&interest))) interest_ = interest;
        RefreshRate();
        RefreshVolume();
    }

    /** False once the host has asked to stop. Also handles rate/volume changes and skips. */
    bool Poll() {
        const DWORD actions = site_->GetActions();
        if ((actions & SPVES_ABORT) != 0) return false;
        if ((actions & SPVES_RATE) != 0) RefreshRate();
        if ((actions & SPVES_VOLUME) != 0) RefreshVolume();
        if ((actions & SPVES_SKIP) != 0) {
            SPVSKIPTYPE type = SPVST_SENTENCE;
            long count = 0;
            // Only forward sentence skips can be honoured: what is behind is already spoken.
            if (SUCCEEDED(site_->GetSkipInfo(&type, &count)) && type == SPVST_SENTENCE && count > 0) {
                skipRequested_ = count;
                skipLeft_ = count;
            } else {
                site_->CompleteSkip(0);
            }
        }
        return true;
    }

    /** True when the next sentence is to be skipped; reports the skip once it is done. */
    bool TakeSkip() {
        if (skipLeft_ <= 0) return false;
        if (--skipLeft_ == 0) FinishSkip();
        return true;
    }

    /** Reports a skip that ran out of sentences before its count was reached. */
    void FinishSkip() {
        if (skipRequested_ <= 0) return;
        site_->CompleteSkip(skipRequested_ - skipLeft_);
        skipRequested_ = 0;
        skipLeft_ = 0;
    }

    bool Write(const std::vector<uint8_t>& pcm) {
        for (size_t offset = 0; offset < pcm.size(); offset += kWriteChunk) {
            if (!Poll()) return false;
            const ULONG size = (ULONG)std::min(kWriteChunk, pcm.size() - offset);
            ULONG written = 0;
            if (FAILED(site_->Write(pcm.data() + offset, size, &written))) return false;
            audioBytes_ += written != 0 ? written : size;
        }
        return true;
    }

    bool Silence(ULONG ms) {
        const std::vector<uint8_t> zeros((size_t)ms * kBytesPerMs, 0);
        return Write(zeros);
    }

    /** Sentence and word boundary at the current audio position, for highlighting. */
    void Boundary(ULONG sourceOffset, ULONG sourceLength) {
        // VOICEVOX does not report word timings, so the best this engine can say is "this
        // sentence starts now" — a word boundary that spans the sentence. Enough for Web Speech
        // onboundary and for readers that follow along sentence by sentence.
        SPEVENT events[2] = {};
        ULONG count = 0;
        for (const SPEVENTENUM id : {SPEI_SENTENCE_BOUNDARY, SPEI_WORD_BOUNDARY}) {
            if ((interest_ & (1ull << id)) == 0) continue;
            SPEVENT& event = events[count++];
            event.eEventId = id;
            event.elParamType = SPET_LPARAM_IS_UNDEFINED;
            event.ullAudioStreamOffset = audioBytes_;
            event.lParam = (LPARAM)sourceOffset;
            event.wParam = (WPARAM)sourceLength;
        }
        if (count > 0) site_->AddEvents(events, count);
    }

    void Bookmark(const std::wstring& name) {
        if ((interest_ & (1ull << SPEI_TTS_BOOKMARK)) == 0) return;
        SPEVENT event = {};
        event.eEventId = SPEI_TTS_BOOKMARK;
        event.elParamType = SPET_LPARAM_IS_STRING;
        event.ullAudioStreamOffset = audioBytes_;
        event.lParam = (LPARAM)name.c_str();  // SAPI copies the string
        event.wParam = (WPARAM)_wtol(name.c_str());
        site_->AddEvents(&event, 1);
    }

    long rate() const { return rate_; }
    USHORT volume() const { return volume_; }

private:
    void RefreshRate() {
        if (FAILED(site_->GetRate(&rate_))) rate_ = 0;
    }
    void RefreshVolume() {
        if (FAILED(site_->GetVolume(&volume_))) volume_ = 100;
    }

    ISpTTSEngineSite* site_;
    ULONGLONG interest_ = 0;
    long rate_ = 0;
    USHORT volume_ = 100;
    long skipRequested_ = 0;
    long skipLeft_ = 0;
    ULONGLONG audioBytes_ = 0;
};

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

    // Nothing may escape into the host: an exception crossing the COM boundary terminates the
    // browser it runs in. Allocation failure on an enormous input is the realistic one.
    try {
        return SpeakFragments(fragments, site);
    } catch (...) {
        Log(L"speak: aborted by an exception");
        return S_OK;
    }
}

HRESULT Engine::SpeakFragments(const SPVTEXTFRAG* fragments, ISpTTSEngineSite* site) {
    const std::vector<Segment> segments = Segments(fragments);
    if (segments.empty()) return S_OK;

    // Read once: SetObjectToken may change it from another thread while this call runs.
    const int styleId = styleId_.load();
    Speaker speaker(site);
    size_t spoken = 0;
    size_t failed = 0;
    bool firstAudio = true;
    const DWORD started = GetTickCount();

    for (const Segment& segment : segments) {
        if (!speaker.Poll()) return S_OK;

        if (segment.kind == Segment::Kind::Bookmark) {
            speaker.Bookmark(segment.text);
            continue;
        }
        if (segment.kind == Segment::Kind::Silence) {
            if (!speaker.Silence(segment.silenceMs)) return S_OK;
            continue;
        }

        const std::vector<Sentence> sentences = SplitSentences(segment.text);
        Log(L"speak: style=%d chars=%zu sentences=%zu rate=%ld%+ld volume=%u*%lu%% pitch=%ld",
            styleId, segment.text.size(), sentences.size(), speaker.rate(), segment.rateAdj,
            (unsigned)speaker.volume(), segment.volume, segment.pitch);

        for (const Sentence& sentence : sentences) {
            if (!speaker.Poll()) return S_OK;
            if (speaker.TakeSkip()) continue;

            // Read per sentence: the host may change rate or volume while we speak.
            const float speed = SpeedFromSapiRate(speaker.rate() + segment.rateAdj);
            const float volume = VolumeFromSapi(speaker.volume(), segment.volume);
            const float pitch = PitchFromSapi(segment.pitch);

            std::vector<uint8_t> pcm;
            std::string error;
            if (!Core::Instance().Synthesize(styleId, ToUtf8(sentence.text), speed, pitch, volume,
                                             &pcm, &error)) {
                // One sentence the core rejects must not silence the rest of the paragraph.
                // Position and length only, never the text: the log directory is readable by
                // every user of the machine, and what someone had read aloud is theirs.
                ++failed;
                Log(L"speak: synthesis failed on sentence at %zu (%zu chars): %s", sentence.offset,
                    sentence.text.size(), FromUtf8(error.c_str()).c_str());
                continue;
            }
            if (firstAudio) {
                Log(L"speak: first audio after %lu ms", GetTickCount() - started);
                firstAudio = false;
            }

            const ULONG first = segment.source[sentence.offset];
            const ULONG last = segment.source[sentence.offset + sentence.text.size() - 1];
            speaker.Boundary(first, last - first + 1);
            if (!speaker.Write(pcm)) return S_OK;
            ++spoken;
        }
    }
    speaker.FinishSkip();

    // Silent failure on purpose: returning an error speaks nothing more, but makes some hosts
    // surface a dialog. The reason is in the log and in Core::LastError for the check tool.
    if (failed > 0) Log(L"speak: %zu sentence(s) spoken, %zu failed", spoken, failed);
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
        if (SUCCEEDED(CLSIDFromString(clsidText, &clsid)) && StyleFromClsid(clsid, &styleId)) {
            const int previous = styleId_.exchange(styleId);
            if (previous != styleId) Log(L"settoken: style %d -> %d", previous, styleId);
        }
        CoTaskMemFree(clsidText);
    }

    token->AddRef();
    ISpObjectToken* previous = nullptr;
    {
        std::lock_guard<std::mutex> guard(tokenMutex_);
        previous = token_;
        token_ = token;
    }
    if (previous != nullptr) previous->Release();
    Log(L"settoken: style=%d", styleId_.load());
    return S_OK;
}

STDMETHODIMP Engine::GetObjectToken(ISpObjectToken** token) {
    if (token == nullptr) return E_POINTER;
    std::lock_guard<std::mutex> guard(tokenMutex_);
    *token = token_;
    if (token_ == nullptr) return S_FALSE;
    token_->AddRef();
    return S_OK;
}

}  // namespace voicevox
