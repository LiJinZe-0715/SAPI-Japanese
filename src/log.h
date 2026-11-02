#pragma once

namespace voicevox {

/**
 * Appends one line to %ProgramData%\VoicevoxSapi\voicevox-sapi.log.
 *
 * <p>The engine runs inside somebody else's process (Chrome, a console tool), so there is no
 * console to print to and OutputDebugString needs a debugger attached. A file is the only thing
 * that works in every host, and it is what makes "it does not speak" diagnosable at all.
 */
void Log(const wchar_t* format, ...);

}  // namespace voicevox
