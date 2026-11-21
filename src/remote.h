#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace voicevox {
namespace remote {

/**
 * What a call to the resident engine means for the caller.
 *
 * <p>The difference between `Failed` and `Unavailable` is worth a whole type: on `Failed` the
 * server already tried and reported why, so synthesizing again locally would spend 13 seconds
 * to fail the same way. On `Unavailable` nobody tried for us, and the local path is exactly what
 * this engine did before the server existed.
 */
enum class Result {
    Ok,
    Failed,       /**< The server answered, and refused. Its reason is in `error`. */
    Unavailable,  /**< No server, a wedged one, or a protocol we do not speak. */
};

/**
 * Synthesizes through VoicevoxSapiServer.exe, which already has the model loaded.
 *
 * <p>Never throws and never blocks for longer than its timeouts; a false answer is a hint to use
 * the local path, not an error to report. Starts the server in the background when it is missing,
 * at most once every 30 seconds.
 */
Result Synthesize(int styleId, const std::string& utf8Text, float speed, float pitch, float volume,
                  std::vector<uint8_t>* pcm, std::string* error);

/**
 * Records the style that just spoke, so the server can preload it at the next login.
 *
 * <p>Per user (HKCU), not per machine: the engine runs inside processes owned by the person
 * listening, and a shared value would have two users fighting over which voice to warm up.
 */
void RememberStyle(int styleId);

}  // namespace remote
}  // namespace voicevox
