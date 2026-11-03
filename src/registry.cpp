#include "registry.h"

#include <windows.h>
#include <objbase.h>
#include <oleauto.h>

#include <string>
#include <vector>

#include "catalog.h"
#include "core.h"
#include "selection.h"
#include "text.h"
#include "voices_i18n.h"

namespace voicevox {
namespace {

// Two voice categories, and a voice has to be in the right one to be seen.
//
// Chrome, Edge and Narrator ask for the OneCore category first and only fall back to the
// legacy SAPI5 one when that enumeration fails — on a normal Windows install it never fails,
// because the built-in voices live there. A voice registered only under Speech\Voices is
// therefore invisible in every browser, while still working for SAPI apps that ask for
// SPCAT_VOICES directly. Registering in both keeps both audiences happy.
const wchar_t* const kTokensRoots[] = {
    L"SOFTWARE\\Microsoft\\Speech\\Voices\\Tokens",
    L"SOFTWARE\\Microsoft\\Speech_OneCore\\Voices\\Tokens",
};

constexpr wchar_t kClsidRoot[] = L"SOFTWARE\\Classes\\CLSID";

// 0x411 = ja-JP. SAPI stores the LCID as hex text without the 0x prefix. (0x804, the value
// this used to carry, is zh-CN — which made SAPI file every VOICEVOX voice under Chinese.)
constexpr wchar_t kJapaneseLcid[] = L"411";

std::wstring GuidText(REFCLSID clsid) {
    wchar_t buffer[64] = {};
    StringFromGUID2(clsid, buffer, (int)(sizeof(buffer) / sizeof(buffer[0])));
    return buffer;
}

bool WriteString(HKEY root, const std::wstring& path, const wchar_t* name,
                 const std::wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        return false;
    }
    const DWORD bytes = (DWORD)((value.size() + 1) * sizeof(wchar_t));
    const LSTATUS status = RegSetValueExW(key, name, 0, REG_SZ, (const BYTE*)value.c_str(), bytes);
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

std::wstring TokenName(const VoiceEntry& entry) {
    // Flat under Tokens\, not Tokens\VOICEVOX\<name>: SAPI's enumeration walks one level and
    // treats every direct subkey as a token, so a vendor subkey is enumerated as a bogus,
    // nameless voice and the real ones below it are never seen.
    return L"VOICEVOX_" + std::wstring(entry.character) + L"_" + std::to_wstring(entry.styleId);
}

bool ModelInstalled(const VoiceEntry& entry) {
    const std::wstring path = Core::HomeDir() + L"\\models\\" + entry.vvm;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool IsDigits(const std::wstring& text) {
    if (text.empty()) return false;
    for (const wchar_t c : text) {
        if (c < L'0' || c > L'9') return false;
    }
    return true;
}

const VoiceEntry* EntryByStyle(int styleId) {
    for (const VoiceEntry& entry : Voices()) {
        if (entry.styleId == styleId) return &entry;
    }
    return nullptr;
}

/**
 * The name a token gets, honouring the optional override from voices.txt.
 *
 * <p>An override is either another style id — `10006 = 10004` registers 女声6 under 女声3's name,
 * which is how you keep a favourite voice while the label stays the one you expect — or a literal
 * string, for someone editing the file by hand. A literal replaces only the character name: the
 * style and the sound tag stay, because "女声3 / 普通 · 通用女声" is what makes the entry mean
 * something in a voice picker.
 */
std::wstring NameFor(const VoiceEntry& entry, const std::wstring& display, UiLang lang) {
    if (display.empty()) return DisplayName(entry, lang);
    if (IsDigits(display)) {
        const VoiceEntry* borrowed = EntryByStyle(_wtoi(display.c_str()));
        if (borrowed != nullptr) return DisplayName(*borrowed, lang);
    }
    std::wstring name = display + L" / " + StyleName(entry.style, lang);
    const wchar_t* tag = CharacterTag(entry.character, lang);
    if (tag != nullptr && tag[0] != L'\0') {
        name += L" · ";
        name += tag;
    }
    return name;
}

bool WriteToken(const std::wstring& root, const VoiceEntry& entry, const std::wstring& display,
                const std::wstring& clsid) {
    const std::wstring token = root + L"\\" + TokenName(entry);
    const std::wstring attributes = token + L"\\Attributes";
    const std::wstring name = NameFor(entry, display, SystemUiLang());

    // Three names per token: the key's default value, plus one value per UI language that SAPI
    // looks up first (see LanguageValueName). A Japanese Windows shows 四国めたん, an English one
    // shows Shikoku Metan, a Chinese one shows 四国梅丹 — without re-registering anything.
    bool ok = WriteString(HKEY_LOCAL_MACHINE, token, nullptr, name);
    for (const UiLang lang : {UiLang::Japanese, UiLang::English, UiLang::Chinese}) {
        ok = ok && WriteString(HKEY_LOCAL_MACHINE, token, LanguageValueName(lang),
                               NameFor(entry, display, lang));
    }

    return ok &&
           WriteString(HKEY_LOCAL_MACHINE, token, L"CLSID", clsid) &&
           WriteString(HKEY_LOCAL_MACHINE, attributes, L"Name", name) &&
           WriteString(HKEY_LOCAL_MACHINE, attributes, L"Language", kJapaneseLcid) &&
           WriteString(HKEY_LOCAL_MACHINE, attributes, L"Gender", entry.female ? L"Female" : L"Male") &&
           WriteString(HKEY_LOCAL_MACHINE, attributes, L"Age", L"Adult") &&
           WriteString(HKEY_LOCAL_MACHINE, attributes, L"Vendor", L"VOICEVOX");
}

// Optional allow-list, so an install can carry a handful of voices instead of the whole cast.
//
// <p>One entry per line: a style id (10005) or a character name in any of the three languages
// (女声1 / Female 1). `10006 = 10004` selects 女声6 but shows 女声3's name. A missing file means
// "everything". So does a list that selects no installed voice at all — a typo, or a list left
// over from a package with other model packs — because registering nothing looks exactly like a
// broken install. RegisterStats::selectionIgnored reports when that happened.
//
// <p>It lives next to the model packs rather than in the registry for two reasons: a light
// package can carry its own list inside the payload, and the user can edit it with Notepad and
// re-run install instead of hunting for a registry value.
constexpr wchar_t kSelectionFile[] = L"\\voices.txt";

// Ordinal compare with case folding, so "female 1" matches "Female 1" but 女声1 does not match
// some other name that happens to have the same length.
bool SameName(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) ==
           CSTR_EQUAL;
}

Selection ReadSelection() {
    const std::wstring path = Core::HomeDir() + kSelectionFile;

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return Selection();

    std::string bytes;
    char buffer[4096];
    DWORD got = 0;
    while (ReadFile(file, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
        bytes.append(buffer, got);
    }
    CloseHandle(file);

    return ParseSelection(DecodeText(bytes));
}

/**
 * nullptr when the entry is not selected, otherwise the display override for it — which may be
 * empty, meaning "no override".
 */
const std::wstring* Selected(const VoiceEntry& entry, const Selection& selection) {
    static const std::wstring kNoOverride;
    if (selection.everything) return &kNoOverride;

    const std::wstring id = std::to_wstring(entry.styleId);
    for (const SelectionLine& line : selection.lines) {
        if (line.match == id || SameName(line.match, entry.character)) return &line.display;
        for (const UiLang lang : {UiLang::Japanese, UiLang::English, UiLang::Chinese}) {
            if (SameName(line.match, CharacterName(entry.character, lang))) return &line.display;
        }
    }
    return nullptr;
}

std::vector<std::wstring> SubkeyNames(HKEY root, const std::wstring& path) {
    std::vector<std::wstring> names;
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_ENUMERATE_SUB_KEYS, &key) != ERROR_SUCCESS) {
        return names;
    }
    wchar_t name[256];
    for (DWORD index = 0;; ++index) {
        DWORD length = 256;
        if (RegEnumKeyExW(key, index, name, &length, nullptr, nullptr, nullptr, nullptr) !=
            ERROR_SUCCESS) {
            break;
        }
        names.emplace_back(name, length);
    }
    RegCloseKey(key);
    return names;
}

std::wstring ReadString(HKEY root, const std::wstring& path, const wchar_t* name) {
    wchar_t buffer[128] = {};
    DWORD bytes = sizeof(buffer) - sizeof(wchar_t);
    if (RegGetValueW(root, path.c_str(), name, RRF_RT_REG_SZ, nullptr, buffer, &bytes) !=
        ERROR_SUCCESS) {
        return std::wstring();
    }
    return buffer;
}

bool SameText(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

bool Contains(const std::vector<std::wstring>& list, const std::wstring& item) {
    for (const std::wstring& entry : list) {
        if (SameText(entry, item)) return true;
    }
    return false;
}

/**
 * Deletes every token and CLSID this product ever wrote, except the ones named in `keepTokens`
 * and `keepClsids` — not just the ones today's catalog knows.
 *
 * <p>The catalog is generated from the model packs, so a regenerated one can rename a character
 * or drop a style. Sweeping only the current catalog's names left those older tokens in the
 * voice list for good, pointing at a DLL an uninstall had already removed. Tokens are found by
 * their VOICEVOX_ key prefix, their CLSIDs by the shape every build encodes (IsOurClsid).
 */
void SweepRegistrations(const std::vector<std::wstring>& keepTokens,
                        const std::vector<std::wstring>& keepClsids) {
    for (const wchar_t* root : kTokensRoots) {
        for (const std::wstring& name : SubkeyNames(HKEY_LOCAL_MACHINE, root)) {
            // Earlier builds nested the tokens under a vendor key; that shell goes too.
            if (name != L"VOICEVOX" && name.compare(0, 9, L"VOICEVOX_") != 0) continue;
            if (Contains(keepTokens, name)) continue;
            const std::wstring token = std::wstring(root) + L"\\" + name;
            const std::wstring clsidText = ReadString(HKEY_LOCAL_MACHINE, token, L"CLSID");
            CLSID clsid = {};
            if (!clsidText.empty() && SUCCEEDED(CLSIDFromString(clsidText.c_str(), &clsid)) &&
                IsOurClsid(clsid) && !Contains(keepClsids, clsidText)) {
                RegDeleteTreeW(HKEY_LOCAL_MACHINE,
                               (std::wstring(kClsidRoot) + L"\\" + clsidText).c_str());
            }
            RegDeleteTreeW(HKEY_LOCAL_MACHINE, token.c_str());
        }
    }
    // A CLSID whose token was already gone is still found for every style the catalog has.
    for (const VoiceEntry& entry : Voices()) {
        const std::wstring clsid = GuidText(ClsidForStyle(entry.styleId));
        if (Contains(keepClsids, clsid)) continue;
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(kClsidRoot) + L"\\" + clsid).c_str());
    }
}

}  // namespace

bool RegisterVoices(const std::wstring& modulePath, std::wstring* error, RegisterStats* stats) {
    // The default (and Attributes\Name) follow the machine's UI language; the per-language
    // values written by WriteToken cover the others.
    const UiLang systemLanguage = SystemUiLang();
    Selection selection = ReadSelection();

    if (!selection.everything) {
        bool any = false;
        for (const VoiceEntry& entry : Voices()) {
            if (Selected(entry, selection) != nullptr && ModelInstalled(entry)) {
                any = true;
                break;
            }
        }
        if (!any) {
            selection = Selection();
            if (stats != nullptr) stats->selectionIgnored = true;
        }
    }

    // Registration is a full replace — a token must not survive the removal of its model pack or
    // its line in voices.txt — but done in the order that survives a failure halfway: write the
    // new set first, then sweep whatever is not in it. Sweeping first meant an error on the
    // third voice left a machine with two. UnregisterVoices does the same sweep with nothing to
    // keep, so the two paths cannot disagree about what "this product's tokens" means.
    std::vector<std::wstring> writtenTokens;
    std::vector<std::wstring> writtenClsids;

    for (const VoiceEntry& entry : Voices()) {
        const std::wstring* display = Selected(entry, selection);
        if (display == nullptr) {
            if (stats != nullptr) ++stats->filtered;
            continue;
        }
        // No model, no token: see the note in registry.h.
        if (!ModelInstalled(entry)) {
            if (stats != nullptr) ++stats->missingModel;
            continue;
        }

        const std::wstring name = NameFor(entry, *display, systemLanguage);
        const std::wstring clsid = GuidText(ClsidForStyle(entry.styleId));

        // Each key is rebuilt from scratch, so no value an older build wrote lingers in it.
        const std::wstring clsidKey = std::wstring(kClsidRoot) + L"\\" + clsid;
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, clsidKey.c_str());
        for (const wchar_t* root : kTokensRoots) {
            RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(root) + L"\\" + TokenName(entry)).c_str());
        }

        // The COM server: one CLSID per style, all served by this DLL.
        if (!WriteString(HKEY_LOCAL_MACHINE, clsidKey, nullptr, L"VOICEVOX " + name) ||
            !WriteString(HKEY_LOCAL_MACHINE, clsidKey + L"\\InprocServer32", nullptr, modulePath) ||
            !WriteString(HKEY_LOCAL_MACHINE, clsidKey + L"\\InprocServer32", L"ThreadingModel",
                         L"Both")) {
            if (error != nullptr) *error = L"cannot write CLSID " + clsid;
            return false;
        }

        // The SAPI voice tokens: what the control panel, SAPI apps and Chrome enumerate.
        for (const wchar_t* root : kTokensRoots) {
            if (!WriteToken(root, entry, *display, clsid)) {
                if (error != nullptr) *error = L"cannot write voice token under " + std::wstring(root);
                return false;
            }
        }
        writtenTokens.push_back(TokenName(entry));
        writtenClsids.push_back(clsid);
        if (stats != nullptr) ++stats->registered;
    }
    SweepRegistrations(writtenTokens, writtenClsids);
    return true;
}

bool UnregisterVoices(std::wstring* error) {
    SweepRegistrations({}, {});
    (void)error;
    return true;
}

}  // namespace voicevox
