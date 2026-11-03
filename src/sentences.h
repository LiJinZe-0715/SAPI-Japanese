#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace voicevox {

/** One piece of text the engine synthesizes on its own, and where it sits in the input. */
struct Sentence {
    std::wstring text;  // trimmed
    size_t offset;      // index of text[0] in the input
};

/**
 * Splits on sentence ends, then on soft breaks for sentences that are too long to wait for.
 *
 * <p>Sentence by sentence, not all at once: the core synthesizes a whole query before it returns
 * a single sample, so one big utterance means one long silence up front. A piece that is nothing
 * but punctuation ("やった！！" ends in a lone "！") is folded into the sentence before it rather
 * than synthesized on its own, and a hard cut never lands inside a surrogate pair.
 */
std::vector<Sentence> SplitSentences(const std::wstring& text);

}  // namespace voicevox
