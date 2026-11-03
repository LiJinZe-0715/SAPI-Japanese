#pragma once

#include <windows.h>

#include <string>

namespace voicevox {

/** Which language the machine's UI is in; anything that is not ja or zh gets English. */
enum class UiLang { Japanese, English, Chinese };

UiLang SystemUiLang();

/**
 * A character's names and how the voice sounds, in three languages.
 *
 * <p>The tag is a compressed form of the one line the official site prints under each character
 * (ずんだもん = 子供っぽい高めの声). It is short enough to sit in a voice name — which matters,
 * because a voice name is the only thing every SAPI host shows: Chrome's voice list, Narrator's
 * settings, the old control panel. Without it the list reads "四国梅丹 / 普通" and says nothing
 * about what the voice actually sounds like.
 */
struct CharacterInfo {
    const wchar_t* ja;
    const wchar_t* en;
    const wchar_t* zh;
    const wchar_t* tagJa;
    const wchar_t* tagEn;
    const wchar_t* tagZh;
};

struct StyleInfo {
    const wchar_t* ja;
    const wchar_t* en;
    const wchar_t* zh;
};

/** nullptr when the character has no translation (a new model pack, say) — callers fall back. */
const CharacterInfo* FindCharacter(const wchar_t* ja);
const StyleInfo* FindStyleName(const wchar_t* ja);

/** Localized names, falling back to the Japanese original when there is no translation. */
const wchar_t* CharacterName(const wchar_t* ja, UiLang lang);
const wchar_t* StyleName(const wchar_t* ja, UiLang lang);
const wchar_t* CharacterTag(const wchar_t* ja, UiLang lang);

/** "四国めたん / ノーマル · 明瞭で芯のある声" in the requested language. */
std::wstring LocalizedDisplayName(const wchar_t* character, const wchar_t* style, UiLang lang);

/** SAPI stores localized descriptions under a value named after the UI language, e.g. "0409". */
const wchar_t* LanguageValueName(UiLang lang);

}  // namespace voicevox
