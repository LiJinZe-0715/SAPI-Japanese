#pragma once

// The resident engine's launch mechanism.
//
// A process started by the DLL itself is a child of whatever host needed a voice, and hosts that
// put their children in a job object — Chrome does, so that a crash cannot leave helpers behind —
// take the server down with them. One started by the Task Scheduler service belongs to no job, so
// it survives every browser restart, which is the entire point of keeping it warm.
//
// The task is the only thing that has to be registered: the DLL and the uninstaller both talk to
// the same task by name, and every function here returns the raw HRESULT instead of logging,
// because the same code runs inside a browser process (quiet, throttled) and in the elevated
// installer (reports to the console).

#include <windows.h>

#include <string>

namespace voicevox {
namespace schtask {

/**
 * Creates or updates the logon task that runs `exe` with `arguments`.
 *
 * <p>Registered for the built-in Users group rather than for the installing account: the SAPI
 * voices are machine-wide (HKLM), so every user who logs in should get an engine, and a per-user
 * task would only ever start for whoever happened to run setup.
 */
HRESULT Register(const std::wstring& exe, const std::wstring& arguments);

/** Asks the scheduler to run the task now. Callers fall back to CreateProcess on failure. */
HRESULT Start();

/** Deletes the task. Called by the uninstaller, and safe when there is no task. */
HRESULT Remove();

}  // namespace schtask
}  // namespace voicevox
