#pragma once

#include <string>
#include <vector>

namespace voicevox {

// The optional allow-list in %ProgramData%\VoicevoxSapi\voices.txt, parsed. Kept apart from
// registry.cpp so the unit tests can feed it text without a registry or a catalog.

struct SelectionLine {
    std::wstring match;    // style id or character name
    std::wstring display;  // what to call it instead; empty means "as the catalog says"
};

struct Selection {
    bool everything = true;
    std::vector<SelectionLine> lines;
};

/**
 * One entry per line: a style id (10005) or a character name (女声1 / Female 1). `10006 = 10004`
 * selects 女声6 but shows 女声3's name. `#` starts a comment, `*` alone means "everything".
 * Empty text, or text that is only comments, also means "everything".
 */
Selection ParseSelection(const std::wstring& text);

}  // namespace voicevox
