#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "voicevox_core.h"

namespace voicevox {

/**
 * The VOICEVOX C API, wrapped so the COM layer never sees a result code.
 *
 * <p>Everything is serialized on one mutex: SAPI objects are registered as ThreadingModel=Both,
 * so Chrome may call Speak from any thread, and the synthesizer is not safe to share.
 *
 * <p>The core is brought up on first use rather than at DllMain — DllMain runs under the loader
 * lock, and loading ONNX Runtime there is a good way to deadlock the host process.
 */
class Core {
public:
    static Core& Instance();

    /**
     * Synthesizes to 24 kHz / 16-bit / mono PCM, with the RIFF header stripped.
     *
     * <p>All three scales are VOICEVOX's, not SAPI's — the two APIs disagree about where
     * "normal" is, and passing one into the other is what broke the first build.
     *
     * @param speed   speedScale: 1.0 = normal. (SAPI rate is 0 = normal.)
     * @param pitch   pitchScale: 0.0 = normal, usable range about ±0.15. (SAPI pitch is
     *                -10..10 with 0 = normal.)
     * @param volume  volumeScale: 1.0 = normal, 0 = silent. (SAPI volume is 0..100.)
     */
    bool Synthesize(int styleId, const std::string& utf8Text, float speed, float pitch,
                    float volume, std::vector<uint8_t>* pcm);

    const std::string& LastError() const { return lastError_; }

    /**
     * Runtime files: %ProgramData%\VoicevoxSapi, or %VOICEVOX_SAPI_HOME% when set (dev runs).
     * Holds voicevox_core.dll, voicevox_onnxruntime.dll, dict\ and models\.
     */
    static std::wstring HomeDir();

private:
    Core() = default;

    bool EnsureReady();
    bool EnsureModel(int styleId);
    void EvictPacks(const std::wstring& keep);
    bool Fail(const char* what, int resultCode);

    /** One loaded model pack, with the id the core needs to unload it again. */
    struct LoadedPack {
        uint8_t id[16] = {};
        uint64_t lastUsed = 0;
    };

    std::mutex mutex_;
    bool attempted_ = false;
    bool ready_ = false;
    const VoicevoxOnnxruntime* onnxruntime_ = nullptr;
    OpenJtalkRc* openJtalk_ = nullptr;
    VoicevoxSynthesizer* synthesizer_ = nullptr;
    std::map<std::wstring, LoadedPack> packs_;
    uint64_t clock_ = 0;
    std::string lastError_;
};

}  // namespace voicevox
