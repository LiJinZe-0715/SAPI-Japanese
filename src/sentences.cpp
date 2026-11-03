#include "sentences.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>

namespace voicevox {
namespace {

// Sentence ends. Anything past one of these is a safe place to synthesize separately without
// hearing the seam, which is the whole point: a five-sentence paragraph used to be one 2-second
// wait before the first sample.
constexpr wchar_t kSentenceEnds[] = L"。．！？!?\n";

// Where a run-on sentence may still be cut. Commas are audible seams, so they are only used
// when a single sentence would otherwise be too long to start speaking promptly.
constexpr wchar_t kSoftBreaks[] = L"、，,；;：:　 ";

// Characters that carry no sound of their own; a piece made only of these is not worth a
// synthesis call (and the core may well reject it).
constexpr wchar_t kSilentMarks[] = L"。．！？!?、，,；;：:・…‥「」『』（）()\"'〜~ー―-";

// Beyond this many characters a single sentence is cut at the last soft break. Roughly five
// seconds of speech, which is already a long silence to sit through.
constexpr size_t kMaxSentence = 60;

struct Span {
    size_t begin;
    size_t end;
};

bool IsSpace(wchar_t c) {
    return iswspace(c) != 0 || c == L'　';
}

bool Trim(const std::wstring& text, size_t* begin, size_t* end) {
    while (*begin < *end && IsSpace(text[*begin])) ++*begin;
    while (*end > *begin && IsSpace(text[*end - 1])) --*end;
    return *begin < *end;
}

bool OnlyMarks(const std::wstring& text, size_t begin, size_t end) {
    for (size_t at = begin; at < end; ++at) {
        if (!IsSpace(text[at]) && wcschr(kSilentMarks, text[at]) == nullptr) return false;
    }
    return true;
}

void Push(const std::wstring& text, size_t begin, size_t end, std::vector<Span>* out) {
    if (!Trim(text, &begin, &end)) return;
    if (OnlyMarks(text, begin, end)) {
        // Fold into the previous piece; with nothing before it there is nothing to say.
        if (!out->empty()) out->back().end = end;
        return;
    }
    out->push_back(Span{begin, end});
}

}  // namespace

std::vector<Sentence> SplitSentences(const std::wstring& text) {
    std::vector<Span> sentences;
    size_t start = 0;
    for (size_t at = 0; at < text.size(); ++at) {
        if (wcschr(kSentenceEnds, text[at]) != nullptr) {
            Push(text, start, at + 1, &sentences);
            start = at + 1;
        }
    }
    Push(text, start, text.size(), &sentences);

    std::vector<Span> pieces;
    for (const Span& sentence : sentences) {
        if (sentence.end - sentence.begin <= kMaxSentence) {
            pieces.push_back(sentence);
            continue;
        }
        size_t from = sentence.begin;
        while (from < sentence.end) {
            size_t cut = std::min(from + kMaxSentence, sentence.end);
            if (cut < sentence.end) {
                const size_t soft = text.find_last_of(kSoftBreaks, cut - 1);
                if (soft != std::wstring::npos && soft > from && soft < cut) cut = soft + 1;
                // Never between the two halves of a surrogate pair (𠮟, emoji, ...).
                if (cut < sentence.end && text[cut - 1] >= 0xD800 && text[cut - 1] <= 0xDBFF) {
                    cut = cut - 1 > from ? cut - 1 : cut + 1;
                }
            }
            Push(text, from, cut, &pieces);
            from = cut;
        }
    }

    std::vector<Sentence> out;
    out.reserve(pieces.size());
    for (const Span& piece : pieces) {
        out.push_back(Sentence{text.substr(piece.begin, piece.end - piece.begin), piece.begin});
    }
    return out;
}

}  // namespace voicevox
