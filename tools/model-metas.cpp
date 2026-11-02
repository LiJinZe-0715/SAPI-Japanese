// voicevox-model-metas — reads every .vvm in a directory and prints what it contains.
//
// The catalog (src/catalog.cpp) maps a style id to the model pack that holds it, and the only
// trustworthy source for that mapping is the pack itself: guessing an id from the character's
// position in the release notes is how you end up with a voice that speaks with somebody else's
// throat. So this tool asks voicevox_core for the metadata and emits the table that goes into
// src/catalog_table.inc.
//
//   voicevox-model-metas <models-dir>            human readable
//   voicevox-model-metas <models-dir> --cpp      catalog entries
//   voicevox-model-metas <models-dir> --json     raw metadata, for debugging
//
// It links against voicevox_core.lib, so voicevox_core.dll has to be reachable at run time
// (on PATH, or next to this executable).

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "voicevox_core.h"

namespace {

struct Style {
    std::string name;
    int id = 0;
    std::string type;
};

struct Character {
    std::string name;
    std::vector<Style> styles;
};

std::string ToUtf8(const std::wstring& wide) {
    if (wide.empty()) return std::string();
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), nullptr, 0,
                                         nullptr, nullptr);
    std::string out((size_t)size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), &out[0], size, nullptr, nullptr);
    return out;
}

std::wstring ToWide(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    std::wstring out((size_t)size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &out[0], size);
    return out;
}

/** Value of the string field whose key starts at `keyAt`. */
std::string StringAfter(const std::string& json, size_t keyAt) {
    const size_t quote = json.find('"', json.find(':', keyAt) + 1);
    if (quote == std::string::npos) return {};
    const size_t end = json.find('"', quote + 1);
    if (end == std::string::npos) return {};
    return json.substr(quote + 1, end - quote - 1);
}

/**
 * The metadata document is an array of characters, each with a `styles` array:
 *
 *   [{"name":"四国めたん","styles":[{"id":2,"name":"ノーマル","type":"streaming_talk",...}],...}]
 *
 * One .vvm holds several characters (0.vvm has four), so a single "first name" read is not
 * enough — the pairing has to be done per object.
 */
std::vector<Character> ParseMetas(const std::string& json) {
    std::vector<Character> characters;

    size_t cursor = 0;
    while (true) {
        const size_t stylesAt = json.find("\"styles\"", cursor);
        if (stylesAt == std::string::npos) break;

        // The character's own "name" is the last one before its styles array.
        size_t nameAt = std::string::npos;
        for (size_t at = json.rfind("\"name\"", stylesAt); at != std::string::npos;
             at = at == 0 ? std::string::npos : json.rfind("\"name\"", at - 1)) {
            nameAt = at;
            break;
        }
        if (nameAt == std::string::npos) break;

        Character character;
        character.name = StringAfter(json, nameAt);

        const size_t open = json.find('[', stylesAt);
        const size_t close = json.find(']', open == std::string::npos ? stylesAt : open);
        if (open == std::string::npos || close == std::string::npos) break;

        size_t styleAt = open;
        while ((styleAt = json.find("\"id\"", styleAt)) != std::string::npos && styleAt < close) {
            const size_t colon = json.find(':', styleAt);
            if (colon == std::string::npos || colon > close) break;

            Style style;
            style.id = std::atoi(json.c_str() + colon + 1);
            const size_t styleNameAt = json.find("\"name\"", colon);
            if (styleNameAt == std::string::npos || styleNameAt > close) break;
            style.name = StringAfter(json, styleNameAt);

            const size_t typeAt = json.find("\"type\"", styleNameAt);
            if (typeAt != std::string::npos && typeAt < close) style.type = StringAfter(json, typeAt);

            character.styles.push_back(style);
            styleAt = styleNameAt;
        }

        characters.push_back(std::move(character));
        cursor = close + 1;
    }
    return characters;
}

bool ReadModel(const std::wstring& path, std::vector<Character>* characters, std::string* rawJson) {
    VoicevoxVoiceModelFile* file = nullptr;
    const std::string utf8Path = ToUtf8(path);
    const VoicevoxResultCode code = voicevox_voice_model_file_open(utf8Path.c_str(), &file);
    if (code != VOICEVOX_RESULT_OK) {
        std::fwprintf(stderr, L"open failed: %s (%hs)\n", path.c_str(),
                      voicevox_error_result_to_message(code));
        return false;
    }

    char* metas = voicevox_voice_model_file_create_metas_json(file);
    voicevox_voice_model_file_delete(file);
    if (metas == nullptr) {
        std::fwprintf(stderr, L"no metadata: %s\n", path.c_str());
        return false;
    }

    *rawJson = metas;
    voicevox_string_free(metas);
    *characters = ParseMetas(*rawJson);
    if (characters->empty()) {
        std::fwprintf(stderr, L"unparsable metadata: %s\n", path.c_str());
        return false;
    }
    return true;
}

std::vector<std::wstring> ListVvm(const std::wstring& dir) {
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW found = {};
    const std::wstring pattern = dir + L"\\*.vvm";
    HANDLE handle = FindFirstFileW(pattern.c_str(), &found);
    if (handle == INVALID_HANDLE_VALUE) return files;
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        files.push_back(found.cFileName);
    } while (FindNextFileW(handle, &found) != 0);
    FindClose(handle);
    std::sort(files.begin(), files.end());
    return files;
}

// Characters whose voices read as male. The metadata carries no gender field, and SAPI's
// Gender attribute is only used for grouping in the voice picker — so a wrong entry here is
// cosmetic, not audible. Anything unlisted is reported as female, which is the majority of
// the VOICEVOX cast.
bool IsMale(const std::string& character) {
    static const char* const kMale[] = {
        "玄野武宏", "白上虎太郎", "青山龍星", "剣崎雌雄", "ちび式じい",
        "†聖騎士 紅桜†", "雀松朱司", "麒ヶ島宗麟", "あんこもん",
    };
    for (const char* name : kMale) {
        if (character == name) return true;
    }
    // n0.vvm ships six generic female and three generic male voices with no character behind
    // them; they are not in any list above, so without this they all came out Female.
    return character.compare(0, 6, "男声") == 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fwprintf(stderr, L"usage: voicevox-model-metas <models-dir> [--cpp|--json]\n");
        return 2;
    }

    const std::wstring dir = argv[1];
    const std::string mode = argc >= 3 ? ToUtf8(argv[2]) : std::string();
    const bool asCpp = mode == "--cpp";
    const bool asJson = mode == "--json";

    const std::vector<std::wstring> files = ListVvm(dir);
    if (files.empty()) {
        std::fwprintf(stderr, L"no .vvm under %s\n", dir.c_str());
        return 1;
    }

    struct Row {
        int id;
        std::string character;
        std::string style;
        std::string type;
        std::string file;
    };
    std::vector<Row> rows;
    int skipped = 0;

    for (const std::wstring& name : files) {
        std::vector<Character> characters;
        std::string json;
        if (!ReadModel(dir + L"\\" + name, &characters, &json)) {
            std::fwprintf(stderr, L"skipped %s\n", name.c_str());
            ++skipped;
            continue;
        }
        if (asJson) {
            std::printf("== %hs\n%s\n", ToUtf8(name).c_str(), json.c_str());
            continue;
        }
        for (const Character& character : characters) {
            for (const Style& style : character.styles) {
                rows.push_back(Row{style.id, character.name, style.name, style.type,
                                   ToUtf8(name)});
            }
        }
    }

    if (asJson) return 0;

    // One row per style, ordered by style id: the same order the voice list ends up in, and
    // the order that makes a gap in the id space obvious when scanning the table.
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.id < b.id; });

    if (asCpp) {
        std::printf("// Generated by tools/model-metas.cpp from the vvm set. Do not edit by hand.\n");
        std::printf("// {character, style, style id, model pack, female}\n");
        for (const Row& row : rows) {
            // Singing styles use a different synthesis call; the SAPI engine only speaks.
            if (row.type.find("talk") == std::string::npos) continue;
            std::printf("    {L\"%s\", L\"%s\", %d, L\"%s\", %s},\n", row.character.c_str(),
                        row.style.c_str(), row.id, row.file.c_str(),
                        IsMale(row.character) ? "false" : "true");
        }
        return skipped == 0 ? 0 : 1;
    }

    for (const Row& row : rows) {
        std::wprintf(L"%3d  %-20s %-10s %-16s [%s]\n", row.id, ToWide(row.character).c_str(),
                     ToWide(row.style).c_str(), ToWide(row.type).c_str(), ToWide(row.file).c_str());
    }
    std::printf("%zu row(s), %d file(s) skipped\n", rows.size(), skipped);
    return skipped == 0 ? 0 : 1;
}
