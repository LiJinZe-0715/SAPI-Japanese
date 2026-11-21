#pragma once

// Wire format between the SAPI engine DLL and the resident engine process.
//
// <p>The DLL is loaded into whatever process asks for a voice — Chrome's browser process, a
// console tool, Narrator. Building the synthesizer there costs about 13 seconds on a cold
// machine (voicevox_synthesizer_load_voice_model creates an ONNX Runtime session), and every new
// host process pays it again: a browser restart, a new TTS process, a second application. The
// server pays it once per login instead; the DLL then asks it over this pipe.
//
// <p>One connection per utterance, a fixed header, then two length-prefixed blobs. Anything
// cleverer — pipelining, a streaming PCM channel — would have to cope with the server dying
// mid-sentence, and the fallback (say it locally, 13 seconds later) is already the interesting
// part of the design.

#include <cstdint>

namespace voicevox {
namespace remote {

// Both endpoints are local to the machine. A named pipe rather than a socket: no firewall prompt,
// no port to pick. The name itself lives in pipe_io.h (PipeName), because it carries the session
// id — one engine per desktop — and that is an IO-layer detail.

// Both of these are checked on both sides. A DLL from one build talking to a server from another
// is the normal state right after an upgrade — a running browser keeps the old DLL mapped until
// it exits — and a struct that gained a field would otherwise be read as garbage.
constexpr char kMagic[4] = {'V', 'S', 'A', 'P'};
constexpr uint32_t kVersion = 1;

enum Op : uint32_t {
    kOpSynth = 1,
};

/** Followed on the wire by `textBytes` of UTF-8, no terminator. */
struct Request {
    char magic[4];
    uint32_t version;
    uint32_t op;
    int32_t styleId;
    float speed;
    float pitch;
    float volume;
    uint32_t textBytes;
};

/**
 * Followed on the wire by `pcmBytes` of 24 kHz / 16-bit / mono PCM (no RIFF header, exactly what
 * Core produces) and `errorBytes` of UTF-8.
 *
 * <p>`status` is 0 on success. On failure pcmBytes is 0 and the message repeats what the server
 * has already written to its log, because the caller's log and the server's log are the same file
 * and a support question should not need both sides of the pipe to answer.
 */
struct Response {
    char magic[4];
    uint32_t version;
    uint32_t status;
    uint32_t pcmBytes;
    uint32_t errorBytes;
};

static_assert(sizeof(Request) == 32, "written to the pipe as-is");
static_assert(sizeof(Response) == 20, "written to the pipe as-is");

// Upper bounds so that a malformed (or hostile) frame cannot make either side allocate a gigabyte.
// A sentence is never 64 KB: SAPI hands us a paragraph at most, and the engine splits it further.
constexpr uint32_t kMaxTextBytes = 64 * 1024;
constexpr uint32_t kMaxPcmBytes = 64 * 1024 * 1024;
constexpr uint32_t kMaxErrorBytes = 4 * 1024;

}  // namespace remote
}  // namespace voicevox
