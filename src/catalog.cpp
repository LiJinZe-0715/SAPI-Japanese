#include "catalog.h"

namespace voicevox {
namespace {

// One row per VOICEVOX style, generated from the .vvm metadata:
//
//   voicevox-model-metas <dir-of-vvms> --cpp > src/catalog_table.inc
//
// Hand-editing this table is a bad idea. A wrong style id does not fail loudly — it speaks
// with another character's voice, which is the hardest kind of bug to notice and the reason
// the table is generated instead of transcribed.
const std::vector<VoiceEntry> kVoices = {
#include "catalog_table.inc"
};

// Style ids are 16-bit now. The cast tops out at 126, but the generic voices in n0.vvm start
// at 10000, so the low byte alone would collide. The low byte rides in Data1 and the high byte
// in Data4[3]; builds before this one wrote 0 there, so CLSIDs they registered still decode to
// the same id and do not need to be migrated.
constexpr unsigned kClsidBase = 0x56584B00u;
constexpr unsigned kClsidMask = 0xFFFFFF00u;
constexpr unsigned short kClsidPart2 = 0x9A31;
constexpr unsigned short kClsidPart3 = 0x4C7E;

}  // namespace

const std::vector<VoiceEntry>& Voices() {
    return kVoices;
}

std::wstring DisplayName(const VoiceEntry& entry, UiLang lang) {
    return LocalizedDisplayName(entry.character, entry.style, lang);
}

const VoiceEntry* FindStyle(int styleId) {
    for (const VoiceEntry& entry : kVoices) {
        if (entry.styleId == styleId) return &entry;
    }
    return nullptr;
}

CLSID ClsidForStyle(int styleId) {
    CLSID clsid = {};
    clsid.Data1 = kClsidBase | (unsigned)(styleId & 0xFF);
    clsid.Data2 = kClsidPart2;
    clsid.Data3 = kClsidPart3;
    clsid.Data4[0] = 0x8D;
    clsid.Data4[1] = 0x2B;
    clsid.Data4[2] = 0x51;
    clsid.Data4[3] = (BYTE)((styleId >> 8) & 0xFF);
    clsid.Data4[4] = 0x00;
    clsid.Data4[5] = 0x00;
    clsid.Data4[6] = 0x00;
    clsid.Data4[7] = 0x01;
    return clsid;
}

bool StyleFromClsid(REFCLSID clsid, int* styleId) {
    if ((clsid.Data1 & kClsidMask) != kClsidBase) return false;
    if (clsid.Data2 != kClsidPart2 || clsid.Data3 != kClsidPart3) return false;
    if (clsid.Data4[0] != 0x8D || clsid.Data4[1] != 0x2B || clsid.Data4[2] != 0x51) return false;

    const int decoded = (int)(clsid.Data1 & 0xFF) | ((int)clsid.Data4[3] << 8);
    if (FindStyle(decoded) == nullptr) return false;
    if (styleId != nullptr) *styleId = decoded;
    return true;
}

}  // namespace voicevox
