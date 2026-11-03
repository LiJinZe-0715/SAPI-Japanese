#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Small, dependency-free helpers around the AudioQuery JSON and the WAV the core returns. They
// live apart from core.cpp so the unit tests can exercise them without loading voicevox_core.

namespace voicevox {

/**
 * Replaces the number that follows "key" with a literal.
 *
 * <p>Speed and pitch only exist inside the AudioQuery JSON — the C API has no field for them on
 * VoicevoxSynthesisOptions — so the JSON is patched in place. The key names come from the
 * AudioQuery schema and are stable across 0.17.x. Returns false when the key is missing.
 */
bool PatchLiteral(std::string* json, const char* key, const char* literal);

/**
 * Writes a float into the AudioQuery JSON.
 *
 * <p>Formatted with snprintf and then repaired, rather than trusting the C locale: this code runs
 * inside whatever host loaded us, and a host that called setlocale(LC_ALL, "") on a machine with
 * a decimal comma gets "1,500" here. Non-finite values are floored to 0 first: "%.3f" of inf or
 * nan prints "inf"/"nan", which is not a JSON number either.
 */
bool PatchNumber(std::string* json, const char* key, float value);

/** Integer fields (outputSamplingRate is a u32 in the schema) must not carry a decimal point. */
bool PatchInt(std::string* json, const char* key, int value);

/**
 * SAPI rate (-10..10, 0 = normal) to VOICEVOX speedScale (1.0 = normal).
 *
 * <p>10^(rate/10), clamped to 0.5..2.0. That is the inverse of what Chrome does with a Web Speech
 * rate — it sends `int(10 * log10(rate))` — so utterance.rate = 2 arrives as 3 and comes out at
 * 2x, as the page asked. (2^(rate/10), used before, turned that same request into 1.23x.)
 */
float SpeedFromSapiRate(long rate);

/** SAPI volume 0..100 (100 = normal) times an XML <volume> level 0..100, to volumeScale 0..1. */
float VolumeFromSapi(unsigned long volume, unsigned long xmlVolume);

/**
 * SAPI pitch (-10..10, 0 = normal; what <pitch absmiddle> and Chrome's utterance.pitch arrive as)
 * to VOICEVOX pitchScale.
 *
 * <p>pitchScale is a log2 offset whose neutral value is 0.0 and which only stays clean within
 * about ±0.15, so ±10 maps onto ±0.15. A hardcoded 1.0 once raised every utterance a full octave
 * and the vocoder turned to noise.
 */
float PitchFromSapi(long pitch);

/** Finds the `data` chunk of a RIFF/WAVE buffer and copies the PCM out of it. */
bool StripWav(const uint8_t* data, size_t size, std::vector<uint8_t>* pcm);

}  // namespace voicevox
