#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "voices_i18n.h"

namespace voicevox {

/**
 * One SAPI voice: a character/style pair, i.e. exactly one VOICEVOX style id.
 *
 * <p>The style id is encoded in the CLSID (see ClsidForStyle), so a single DLL serves every
 * voice and DllGetClassObject can tell which one the caller asked for. That avoids
 * ISpObjectTokenInit and keeps the registry entries uniform.
 */
struct VoiceEntry {
    const wchar_t* character;
    const wchar_t* style;
    int styleId;
    const wchar_t* vvm;
    bool female;
};

/** The voices this build registers — one entry per talk style in the model packs. */
const std::vector<VoiceEntry>& Voices();

/** "四国めたん / ノーマル" in the requested language — what the Windows voice list shows. */
std::wstring DisplayName(const VoiceEntry& entry, UiLang lang);

const VoiceEntry* FindStyle(int styleId);

CLSID ClsidForStyle(int styleId);

/** True when the CLSID has our shape, whatever style it encodes — including styles a newer or
 * older catalog had and this one does not. Used to sweep registrations a previous build left. */
bool IsOurClsid(REFCLSID clsid);

/** Decodes a CLSID back into a style id; false when it is not one of ours. */
bool StyleFromClsid(REFCLSID clsid, int* styleId);

}  // namespace voicevox
