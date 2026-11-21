#include "selection.h"

namespace voicevox {
namespace {

std::wstring Trimmed(const std::wstring& text) {
    const wchar_t* const blanks = L" \t\r\n";
    const size_t first = text.find_first_not_of(blanks);
    if (first == std::wstring::npos) return std::wstring();
    const size_t last = text.find_last_not_of(blanks);
    return text.substr(first, last - first + 1);
}

}  // namespace

Selection ParseSelection(const std::wstring& input) {
    Selection selection;
    std::wstring text = input;
    // Notepad's "UTF-8" option writes a BOM. Left in place it becomes a character on the first
    // line, which turns a leading `#` comment into a selection that matches nothing.
    if (!text.empty() && text[0] == L'﻿') text.erase(0, 1);

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

}  // namespace voicevox
