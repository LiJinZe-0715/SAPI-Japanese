#include "registry.h"

#include <windows.h>
#include <objbase.h>
#include <oleauto.h>

#include <string>
#include <vector>

#include "catalog.h"
#include "core.h"
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
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
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
        name += L" \u00B7 ";
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
// "everything". An unreadable or empty list is treated the same way on purpose — registering
// nothing looks exactly like a broken install, and a typo should not be able to produce that.
//
// <p>It lives next to the model packs rather than in the registry for two reasons: a light
// package can carry its own list inside the payload, and the user can edit it with Notepad and
// re-run install instead of hunting for a registry value.
constexpr wchar_t kSelectionFile[] = L"\\voices.txt";

std::wstring Trimmed(const std::wstring& text) {
    const wchar_t* const blanks = L" \t\r\n";
    const size_t first = text.find_first_not_of(blanks);
    if (first == std::wstring::npos) return std::wstring();
    const size_t last = text.find_last_not_of(blanks);
    return text.substr(first, last - first + 1);
}

// Ordinal compare with case folding, so "female 1" matches "Female 1" but 女声1 does not match
// some other name that happens to have the same length.
bool SameName(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) ==
           CSTR_EQUAL;
}

struct SelectionLine {
    std::wstring match;    // style id or character name
    std::wstring display;  // what to call it instead; empty means "as the catalog says"
};

struct Selection {
    bool everything = true;
    std::vector<SelectionLine> lines;
};

Selection ReadSelection() {
    Selection selection;
    const std::wstring path = Core::HomeDir() + kSelectionFile;

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return selection;

    std::string bytes;
    char buffer[4096];
    DWORD got = 0;
    while (ReadFile(file, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
        bytes.append(buffer, got);
    }
    CloseHandle(file);

    std::wstring text = FromUtf8(bytes.c_str());
    // Notepad's "UTF-8" option writes a BOM. Left in place it becomes a character on the first
    // line, which turns a leading `#` comment into a selection that matches nothing — and since
    // one non-comment line is enough to switch the list from "everything" to "exactly these",
    // a file of pure comments would silently register zero voices.
    if (!text.empty() && text[0] == L'\uFEFF') text.erase(0, 1);

    for (size_t at = 0; at < text.size();) {
        size_t end = text.find(L'\n', at);
        if (end == std::wstring::npos) end = text.size();
        const std::wstring line = Trimmed(text.substr(at, end - at));
        at = end + 1;
        if (line.empty() || line[0] == L'#') continue;
        if (line == L"*") {
            selection.lines.clear();
            selection.everything = true;
            return selection;
        }
        // "10006 = 10004": what to select, then what to call it.
        SelectionLine parsed;
        const size_t equals = line.find(L'=');
        if (equals == std::wstring::npos) {
            parsed.match = line;
        } else {
            parsed.match = Trimmed(line.substr(0, equals));
            parsed.display = Trimmed(line.substr(equals + 1));
        }
        if (parsed.match.empty()) continue;
        selection.lines.push_back(parsed);
    }
    selection.everything = selection.lines.empty();
    return selection;
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

}  // namespace

bool RegisterVoices(const std::wstring& modulePath, std::wstring* error, RegisterStats* stats) {
    // The default (and Attributes\Name) follow the machine's UI language; the per-language
    // values written by WriteToken cover the others.
    const UiLang systemLanguage = SystemUiLang();
    const Selection selection = ReadSelection();

    // Clear the catalog's own entries before writing. Registration used to be add-only, which
    // meant a token survived the removal of its model pack and a voice stayed in the list after
    // the selection file stopped asking for it. UnregisterVoices does the same sweep, so the two
    // paths cannot disagree about what "this product's tokens" means.
    for (const VoiceEntry& entry : Voices()) {
        RegDeleteTreeW(HKEY_LOCAL_MACHINE,
                       (std::wstring(kClsidRoot) + L"\\" + GuidText(ClsidForStyle(entry.styleId)))
                           .c_str());
        for (const wchar_t* root : kTokensRoots) {
            RegDeleteTreeW(HKEY_LOCAL_MACHINE,
                           (std::wstring(root) + L"\\" + TokenName(entry)).c_str());
        }
    }

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

        // The COM server: one CLSID per style, all served by this DLL.
        const std::wstring clsidKey = std::wstring(kClsidRoot) + L"\\" + clsid;
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
        if (stats != nullptr) ++stats->registered;
    }
    return true;
}

bool UnregisterVoices(std::wstring* error) {
    for (const VoiceEntry& entry : Voices()) {
        RegDeleteTreeW(HKEY_LOCAL_MACHINE,
                       (std::wstring(kClsidRoot) + L"\\" + GuidText(ClsidForStyle(entry.styleId)))
                           .c_str());
        for (const wchar_t* root : kTokensRoots) {
            RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(root) + L"\\" + TokenName(entry)).c_str());
        }
    }
    // Earlier builds nested the tokens under a vendor key; drop that shell if it is still there.
    for (const wchar_t* root : kTokensRoots) {
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(root) + L"\\VOICEVOX").c_str());
    }
    (void)error;
    return true;
}

}  // namespace voicevox
