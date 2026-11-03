#pragma once

// sapiddk.h, not sapi.h: the engine-side interfaces (ISpTTSEngine, ISpTTSEngineSite,
// ISpObjectTokenInit) live in the DDK half of the Speech SDK.
#include <sapi.h>
#include <sapiddk.h>
#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>

namespace voicevox {

/** Live COM objects, so DllCanUnloadNow can answer honestly. */
extern volatile LONG g_liveObjects;

/**
 * One SAPI engine instance, bound to one style id.
 *
 * <p>SAPI creates it through the voice token's CLSID and asks for ISpTTSEngine; the style id
 * was already decoded from that CLSID by the class factory. Registration uses
 * ThreadingModel=Both, so every method below may be called from any thread — the synthesis
 * itself is serialized inside Core.
 *
 * <p>ISpObjectWithToken is not optional in practice. After the first GetOutputFormat, SAPI
 * queries it to hand the engine the voice token it was created from; an E_NOINTERFACE there
 * makes the voice unusable and ISpVoice::Speak fails with SPERR_VOICE_NOT_FOUND even though
 * every other call succeeded.
 */
class Engine final : public ISpTTSEngine, public ISpObjectWithToken {
public:
    explicit Engine(int styleId);
    ~Engine();

    static HRESULT Create(int styleId, REFIID riid, void** ppv);

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;

    // ISpTTSEngine
    STDMETHOD(GetOutputFormat)(const GUID* targetFormatId, const WAVEFORMATEX* targetFormat,
                               GUID* outputFormatId, WAVEFORMATEX** allocated) override;
    STDMETHOD(Speak)(DWORD speakFlags, REFGUID formatId, const WAVEFORMATEX* format,
                     const SPVTEXTFRAG* fragments, ISpTTSEngineSite* site) override;

    // ISpObjectWithToken
    STDMETHOD(SetObjectToken)(ISpObjectToken* token) override;
    STDMETHOD(GetObjectToken)(ISpObjectToken** token) override;

private:
    HRESULT SpeakFragments(const SPVTEXTFRAG* fragments, ISpTTSEngineSite* site);

    volatile LONG refs_;
    // Both may be touched from two threads at once (ThreadingModel=Both): SAPI can hand over the
    // token on one thread while another is in Speak.
    std::atomic<int> styleId_;
    std::mutex tokenMutex_;
    ISpObjectToken* token_;
};

}  // namespace voicevox
