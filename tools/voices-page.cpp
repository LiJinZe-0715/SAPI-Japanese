// voicevox-voices-page — writes the character reference sheet as a standalone HTML file.
//
// The voice picker shows one line per voice ("俊达萌 / 普通 · 孩子气高音"), which is enough to
// choose but not enough to browse: 136 entries in a dropdown, in an order nobody chose. This
// tool turns the same tables the engine registers from into a page that can be read, searched
// and compared — one row per character, with the styles they ship.
//
//   voicevox-voices-page <out.html>
//
// It links against src/voices_i18n.cpp rather than duplicating the translations, so the page
// and the registry can never drift apart: regenerate after touching the name tables.

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../src/voices_i18n.h"

namespace {

// Same shape as voicevox::VoiceEntry; the table is shared with the DLL on purpose.
struct Row {
    const wchar_t* character;
    const wchar_t* style;
    int styleId;
    const wchar_t* vvm;
    bool female;
};

const Row kRows[] = {
#include "../src/catalog_table.inc"
};

std::string Utf8(const wchar_t* text) {
    if (text == nullptr || text[0] == L'\0') return std::string();
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return std::string();
    std::string out((size_t)size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, &out[0], size, nullptr, nullptr);
    return out;
}

/** Only three characters need escaping; the rest of the text is names and short tags. */
std::string Escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

struct Character {
    const wchar_t* ja = nullptr;
    std::vector<const Row*> styles;
    const wchar_t* vvm = nullptr;
};

/** Rows arrive grouped by pack, not by character, so build the character list first. */
std::vector<Character> GroupByCharacter() {
    std::vector<Character> characters;
    for (const Row& row : kRows) {
        Character* found = nullptr;
        for (Character& candidate : characters) {
            if (wcscmp(candidate.ja, row.character) == 0) {
                found = &candidate;
                break;
            }
        }
        if (found == nullptr) {
            Character fresh;
            fresh.ja = row.character;
            fresh.vvm = row.vvm;
            characters.push_back(fresh);
            found = &characters.back();
        }
        found->styles.push_back(&row);
    }
    return characters;
}

void Append(std::string* out, const std::string& text) { *out += text; }

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fwprintf(stderr, L"usage: voicevox-voices-page <out.html>\n");
        return 2;
    }

    const std::vector<Character> characters = GroupByCharacter();

    std::string html;
    html.reserve(64 * 1024);
    Append(&html,
           "<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n<meta charset=\"utf-8\">\n"
           "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
           "<title>VOICEVOX 语音对照表</title>\n<style>\n"
           ":root{--bg:#f6f7f9;--card:#fff;--ink:#1a1d21;--muted:#6b7280;--line:#e5e7eb;"
           "--accent:#2f6f4f;--chip:#eef3f0}\n"
           "*{box-sizing:border-box}\n"
           "body{margin:0;padding:32px 24px 64px;background:var(--bg);color:var(--ink);"
           "font:15px/1.6 -apple-system,'Segoe UI','Microsoft YaHei',sans-serif}\n"
           "header{max-width:1080px;margin:0 auto 20px}\n"
           "h1{margin:0 0 6px;font-size:22px;letter-spacing:.01em}\n"
           ".sub{color:var(--muted);font-size:13px}\n"
           ".tools{max-width:1080px;margin:0 auto 14px;display:flex;gap:10px;align-items:center}\n"
           "input{flex:1;max-width:320px;padding:8px 12px;border:1px solid var(--line);"
           "border-radius:8px;font-size:14px;background:var(--card)}\n"
           "input:focus{outline:2px solid var(--accent);outline-offset:-1px}\n"
           ".count{color:var(--muted);font-size:13px}\n"
           "table{width:100%;max-width:1080px;margin:0 auto;border-collapse:separate;"
           "border-spacing:0 8px}\n"
           "th{text-align:left;font-size:12px;font-weight:600;color:var(--muted);"
           "padding:0 14px 6px;letter-spacing:.04em;text-transform:uppercase}\n"
           "td{background:var(--card);padding:12px 14px;vertical-align:top;"
           "border-top:1px solid var(--line);border-bottom:1px solid var(--line)}\n"
           "td:first-child{border-left:1px solid var(--line);border-radius:10px 0 0 10px}\n"
           "td:last-child{border-right:1px solid var(--line);border-radius:0 10px 10px 0}\n"
           ".zh{font-weight:600}\n.ja{color:var(--muted);font-size:12px}\n"
           ".en{color:var(--muted);font-size:12px}\n"
           ".vibe{color:var(--accent);font-size:13px;white-space:nowrap}\n"
           ".styles{display:flex;flex-wrap:wrap;gap:6px}\n"
           ".chip{background:var(--chip);border-radius:999px;padding:2px 10px;font-size:12px}\n"
           ".pack{color:var(--muted);font-size:11px;font-family:ui-monospace,Consolas,monospace}\n"
           // Native hidden attribute rather than a class: the filter script only ever sets
           // `r.hidden`, and a class that no markup mentions reads as dead CSS.
           "tr[hidden]{display:none}\n"
           // Label text that is present for screen readers and form-field checkers but not painted.
           ".vh{position:absolute;width:1px;height:1px;margin:-1px;padding:0;overflow:hidden;"
           "clip:rect(0 0 0 0);white-space:nowrap;border:0}\n"
           "footer{max-width:1080px;margin:28px auto 0;color:var(--muted);font-size:12px}\n"
           "</style>\n</head>\n<body>\n");

    char countText[64] = {};
    std::snprintf(countText, sizeof(countText), "%zu", sizeof(kRows) / sizeof(kRows[0]));

    Append(&html, "<header><h1>VOICEVOX 语音对照表</h1><div class=\"sub\">");
    Append(&html, countText);
    Append(&html, " 个语音 / ");
    std::snprintf(countText, sizeof(countText), "%zu", characters.size());
    Append(&html, countText);
    Append(&html,
           " 位角色 · 名称与音质说明取自 voicevox.hiroshiba.jp，中文为意译，日文为原文<br>"
           "Windows 语音列表里显示的就是「角色 / 风格 · 音质」，例如「俊达萌 / 普通 · 孩子气高音」"
           "</div></header>\n");

    Append(&html,
           "<div class=\"tools\"><label class=\"vh\" for=\"q\">搜索角色名、风格或音质</label>"
           "<input id=\"q\" type=\"search\" "
           "placeholder=\"搜索角色名、风格或音质…\"><span class=\"count\" id=\"n\"></span></div>\n"
           "<table>\n<thead><tr><th style=\"width:22%\">角色</th><th style=\"width:22%\">音质</th>"
           "<th>可用风格</th><th style=\"width:10%\">模型包</th></tr></thead>\n<tbody>\n");

    for (const Character& character : characters) {
        const std::string zh = Escape(Utf8(voicevox::CharacterName(character.ja, voicevox::UiLang::Chinese)));
        const std::string ja = Escape(Utf8(character.ja));
        const std::string en = Escape(Utf8(voicevox::CharacterName(character.ja, voicevox::UiLang::English)));
        const std::string vibe =
            Escape(Utf8(voicevox::CharacterTag(character.ja, voicevox::UiLang::Chinese)));

        Append(&html, "<tr><td><div class=\"zh\">");
        Append(&html, zh);
        Append(&html, "</div><div class=\"ja\">");
        Append(&html, ja);
        Append(&html, "</div><div class=\"en\">");
        Append(&html, en);
        Append(&html, "</div></td><td><div class=\"vibe\">");
        Append(&html, vibe);
        Append(&html, "</div></td><td><div class=\"styles\">");

        for (const Row* style : character.styles) {
            Append(&html, "<span class=\"chip\" title=\"");
            Append(&html, Escape(Utf8(style->style)));
            Append(&html, "\">");
            Append(&html, Escape(Utf8(voicevox::StyleName(style->style, voicevox::UiLang::Chinese))));
            Append(&html, "</span>");
        }

        Append(&html, "</div></td><td><div class=\"pack\">");
        Append(&html, Escape(Utf8(character.vvm)));
        Append(&html, "</div></td></tr>\n");
    }

    Append(&html,
           "</tbody>\n</table>\n<footer>由 voicevox-voices-page 生成 · 与 DLL 注册语音共用同一份名称表，"
           "改动 src/voices_i18n.cpp 后重新生成即可。</footer>\n<script>\n"
           "const q=document.getElementById('q'),n=document.getElementById('n');\n"
           "const rows=[...document.querySelectorAll('tbody tr')];\n"
           "function apply(){const t=q.value.trim().toLowerCase();let hit=0;\n"
           "rows.forEach(r=>{const ok=!t||r.textContent.toLowerCase().includes(t);\n"
           "r.hidden=!ok;if(ok)hit++;});\n"
           "n.textContent=hit+' / '+rows.length;}\n"
           "q.addEventListener('input',apply);apply();\n"
           "</script>\n</body>\n</html>\n");

    // Wide open: the narrow fopen reads the path in the ANSI code page, so a checkout under a
    // directory with Japanese or Chinese in its name could not be written to.
    FILE* file = nullptr;
    if (_wfopen_s(&file, argv[1], L"wb") != 0 || file == nullptr) {
        std::fwprintf(stderr, L"cannot write %s\n", argv[1]);
        return 1;
    }
    fwrite(html.data(), 1, html.size(), file);
    fclose(file);
    std::wprintf(L"wrote %s (%zu characters, %zu voices)\n", argv[1], characters.size(),
                 sizeof(kRows) / sizeof(kRows[0]));
    return 0;
}
