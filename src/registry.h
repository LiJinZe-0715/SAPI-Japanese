#pragma once

#include <string>

namespace voicevox {

/** What a registration pass did, for the installer to report. */
struct RegisterStats {
    int registered = 0;
    int missingModel = 0;
    int filtered = 0;  // dropped by voices.txt
};

/**
 * Writes the CLSID entries and one SAPI voice token per catalog entry whose model pack is
 * present.
 *
 * <p>Voices whose .vvm is not installed are skipped rather than registered: a token without a
 * model shows up in the browser's voice list and then says nothing, which reads as a broken
 * install. Registering again after adding models is enough to pick them up.
 *
 * <p>`%ProgramData%\VoicevoxSapi\voices.txt`, when present, narrows that set further — one style
 * id or character name per line. Registration is a full replace, not an append: entries that are
 * no longer selected (or whose pack was removed) have their tokens deleted in the same pass.
 */
bool RegisterVoices(const std::wstring& modulePath, std::wstring* error,
                    RegisterStats* stats = nullptr);

/** Removes every token the catalog could have written, including ones whose model is gone. */
bool UnregisterVoices(std::wstring* error);

}  // namespace voicevox
