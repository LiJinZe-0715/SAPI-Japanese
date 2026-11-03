#include "audio_util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace voicevox {

bool PatchLiteral(std::string* json, const char* key, const char* literal) {
    const std::string needle = std::string("\"") + key + "\"";
    const size_t at = json->find(needle);
    if (at == std::string::npos) return false;

    size_t colon = json->find(':', at + needle.size());
    if (colon == std::string::npos) return false;

    size_t start = colon + 1;
    while (start < json->size() && std::isspace((unsigned char)(*json)[start])) ++start;

    size_t end = start;
    while (end < json->size()) {
        const char c = (*json)[end];
        const bool part = std::isdigit((unsigned char)c) || c == '-' || c == '+' || c == '.' ||
                          c == 'e' || c == 'E';
        if (!part) break;
        ++end;
    }
    if (end == start) return false;

    json->replace(start, end - start, literal);
    return true;
}

bool PatchNumber(std::string* json, const char* key, float value) {
    if (!std::isfinite(value)) value = 0.0f;
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.3f", value);
    for (char* at = buffer; *at != '\0'; ++at) {
        if (*at == ',') *at = '.';
    }
    return PatchLiteral(json, key, buffer);
}

bool PatchInt(std::string* json, const char* key, int value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%d", value);
    return PatchLiteral(json, key, buffer);
}

float SpeedFromSapiRate(long rate) {
    rate = std::max(-10L, std::min(10L, rate));
    const float speed = std::pow(10.0f, (float)rate / 10.0f);
    return std::min(2.0f, std::max(0.5f, speed));
}

float VolumeFromSapi(unsigned long volume, unsigned long xmlVolume) {
    const float base = (float)std::min(100UL, volume) / 100.0f;
    const float level = (float)std::min(100UL, xmlVolume) / 100.0f;
    return base * level;
}

float PitchFromSapi(long pitch) {
    pitch = std::max(-10L, std::min(10L, pitch));
    return 0.15f * (float)pitch / 10.0f;
}

bool StripWav(const uint8_t* data, size_t size, std::vector<uint8_t>* pcm) {
    pcm->clear();
    if (data == nullptr || size < 12 || std::memcmp(data, "RIFF", 4) != 0 ||
        std::memcmp(data + 8, "WAVE", 4) != 0) {
        return false;
    }
    size_t pos = 12;
    while (pos + 8 <= size) {
        const uint8_t* id = data + pos;
        const uint32_t length = (uint32_t)data[pos + 4] | ((uint32_t)data[pos + 5] << 8) |
                                ((uint32_t)data[pos + 6] << 16) | ((uint32_t)data[pos + 7] << 24);
        pos += 8;
        if (std::memcmp(id, "data", 4) == 0) {
            const size_t available = std::min<size_t>(length, size - pos);
            pcm->assign(data + pos, data + pos + available);
            return !pcm->empty();
        }
        pos += (size_t)length + (length & 1);
    }
    return false;
}

}  // namespace voicevox
