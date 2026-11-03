// Unit tests for the helpers that need neither voicevox_core nor the registry.
// Run by build.bat after every build; exit code is the number of failed checks.

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "audio_util.h"
#include "selection.h"
#include "sentences.h"
#include "text.h"

namespace {

int g_failed = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        ++g_failed;
        std::printf("FAIL: %s\n", what);
    }
}

}  // namespace

int main() {
    using namespace voicevox;

    // Sentences: punctuation-only tails fold into the previous sentence, offsets point back.
    {
        const std::vector<Sentence> s = SplitSentences(L"やった！！ 次へ。");
        Check(s.size() == 2, "split count");
        Check(s.size() == 2 && s[0].text == L"やった！！", "punctuation folded");
        Check(s.size() == 2 && s[1].offset == 6, "offset of second sentence");
    }
    {
        std::wstring longText(70, L'あ');
        longText[59] = 0xD842;  // high surrogate exactly at the hard cut
        longText[60] = 0xDFB7;
        const std::vector<Sentence> s = SplitSentences(longText);
        Check(s.size() == 2 && s[0].text.back() != 0xD842, "no cut inside a surrogate pair");
    }

    // AudioQuery patching.
    {
        std::string json = "{\"speedScale\":1.0,\"pitchScale\":0.0,\"outputSamplingRate\":44100}";
        Check(PatchNumber(&json, "speedScale", 1.5f), "patch speed");
        Check(PatchInt(&json, "outputSamplingRate", 24000), "patch rate");
        Check(json.find("\"speedScale\":1.500") != std::string::npos, "speed written");
        Check(json.find("24000") != std::string::npos, "rate written");
        Check(!PatchNumber(&json, "missing", 1.0f), "missing key reported");
    }

    // Scale conversions.
    Check(SpeedFromSapiRate(0) == 1.0f, "rate 0 is normal");
    Check(SpeedFromSapiRate(3) > 1.95f && SpeedFromSapiRate(3) <= 2.0f, "Chrome rate 2 -> 2x");
    Check(PitchFromSapi(0) == 0.0f && PitchFromSapi(10) > 0.149f, "pitch mapping");
    Check(VolumeFromSapi(50, 100) == 0.5f, "volume mapping");

    // WAV.
    {
        const unsigned char wav[] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E',
                                     'd', 'a', 't', 'a', 2, 0, 0, 0, 7, 8};
        std::vector<uint8_t> pcm;
        Check(StripWav(wav, sizeof(wav), &pcm) && pcm.size() == 2, "strip wav");
    }

    // voices.txt: UTF-16 with BOM decodes, comments-only means everything.
    {
        const std::string utf16("\xFF\xFE" "1\0" "0\0", 6);
        Check(DecodeText(utf16) == L"10", "utf-16 decode");
        Check(ParseSelection(L"# only a comment\r\n").everything, "comments mean everything");
        const Selection s = ParseSelection(L"﻿10006 = 10004\n");
        Check(!s.everything && s.lines.size() == 1 && s.lines[0].display == L"10004", "override");
    }

    std::printf(g_failed == 0 ? "unit tests passed\n" : "%d unit test(s) failed\n", g_failed);
    return g_failed;
}
