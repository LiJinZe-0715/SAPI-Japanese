#include "schtask.h"

#include <taskschd.h>

#include <string>

namespace voicevox {
namespace schtask {
namespace {

// The name is part of the contract with the uninstaller and with nothing else, so it can be
// version-free: an upgrade refreshes the same task instead of leaving an old one behind.
constexpr wchar_t kTaskName[] = L"VoicevoxSapiServer";
// Every Task Scheduler call takes a full path, and the root folder is "\".
constexpr wchar_t kTaskPath[] = L"\\VoicevoxSapiServer";
// BUILTIN\Users. The SDDL form is stable across languages; "Users" is not.
constexpr wchar_t kUsersGroup[] = L"S-1-5-32-545";

/** Owns a BSTR for the duration of one call. */
class Bstr {
public:
    explicit Bstr(const wchar_t* text) : value_(SysAllocString(text)) {}
    ~Bstr() { SysFreeString(value_); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;

    BSTR bstr() const { return value_; }

private:
    BSTR value_ = nullptr;
};

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() {
        if (value_ != nullptr) value_->Release();
    }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    T* operator->() const { return value_; }
    T* get() const { return value_; }
    T** put() { return &value_; }

private:
    T* value_ = nullptr;
};

/**
 * Initializes COM for the duration of the call.
 *
 * <p>RPC_E_CHANGED_MODE is not a failure: SAPI hosts have usually initialized COM as STA already,
 * and refusing to continue there would mean no engine in any browser. The Task Scheduler
 * interfaces are agile enough for this use.
 */
class Com {
public:
    Com() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owned_ = SUCCEEDED(hr);
        ready_ = owned_ || hr == RPC_E_CHANGED_MODE;
    }
    ~Com() {
        if (owned_) CoUninitialize();
    }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;

    bool ready() const { return ready_; }

private:
    bool owned_ = false;
    bool ready_ = false;
};

HRESULT OpenService(ComPtr<ITaskService>* service) {
    const HRESULT created = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                             IID_ITaskService, reinterpret_cast<void**>(service->put()));
    if (FAILED(created)) return created;

    // An empty VARIANT set means "this machine, the caller's credentials".
    VARIANT none;
    VariantInit(&none);
    return (*service)->Connect(none, none, none, none);
}

HRESULT OpenFolder(ITaskService* service, ComPtr<ITaskFolder>* folder) {
    const Bstr path(L"\\");
    return service->GetFolder(path.bstr(), folder->put());
}

std::wstring XmlEscape(const std::wstring& text) {
    std::wstring out;
    out.reserve(text.size());
    for (const wchar_t c : text) {
        switch (c) {
            case L'&': out += L"&amp;"; break;
            case L'<': out += L"&lt;"; break;
            case L'>': out += L"&gt;"; break;
            case L'"': out += L"&quot;"; break;
            case L'\'': out += L"&apos;"; break;
            default: out += c; break;
        }
    }
    return out;
}

/**
 * The task definition, as XML.
 *
 * <p>Passed as text because the alternative is the ITaskDefinition object model, which is half a
 * dozen interfaces to say four things. Settings that matter:
 *
 *   IgnoreNew          two browsers starting at once must not get two engines (the server's own
 *                      mutex covers the rest, but this keeps the scheduler from queueing them)
 *   ExecutionTimeLimit PT0S, otherwise the scheduler kills the engine after the three day default
 *   LeastPrivilege     the engine reads ProgramData and writes its log directory; nothing there
 *                      needs elevation, and running elevated would widen what a bug can reach
 */
std::wstring TaskXml(const std::wstring& exe, const std::wstring& arguments) {
    std::wstring xml;
    xml.reserve(1024);
    xml += L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n";
    xml += L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n";
    xml += L"  <RegistrationInfo>\n";
    xml += L"    <Description>VOICEVOX SAPI resident engine</Description>\n";
    xml += L"  </RegistrationInfo>\n";
    xml += L"  <Triggers>\n";
    xml += L"    <LogonTrigger><Enabled>true</Enabled></LogonTrigger>\n";
    xml += L"  </Triggers>\n";
    xml += L"  <Principals>\n";
    xml += L"    <Principal id=\"Author\">\n";
    xml += L"      <GroupId>";
    xml += kUsersGroup;
    xml += L"</GroupId>\n";
    xml += L"      <RunLevel>LeastPrivilege</RunLevel>\n";
    xml += L"    </Principal>\n";
    xml += L"  </Principals>\n";
    xml += L"  <Settings>\n";
    xml += L"    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\n";
    xml += L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\n";
    xml += L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\n";
    xml += L"    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\n";
    // Logon is the busiest moment the machine has; a trigger the scheduler could not act on
    // immediately (it happens on slow disks and with a dozen startup apps) must still run once it
    // can, instead of leaving the session without an engine until something speaks.
    xml += L"    <StartWhenAvailable>true</StartWhenAvailable>\n";
    xml += L"    <Enabled>true</Enabled>\n";
    xml += L"  </Settings>\n";
    xml += L"  <Actions Context=\"Author\">\n";
    xml += L"    <Exec>\n";
    xml += L"      <Command>";
    xml += XmlEscape(exe);
    xml += L"</Command>\n";
    xml += L"      <Arguments>";
    xml += XmlEscape(arguments);
    xml += L"</Arguments>\n";
    xml += L"    </Exec>\n";
    xml += L"  </Actions>\n";
    xml += L"</Task>\n";
    return xml;
}

}  // namespace

HRESULT Register(const std::wstring& exe, const std::wstring& arguments) {
    if (exe.empty()) return E_INVALIDARG;

    Com com;
    if (!com.ready()) return CO_E_NOTINITIALIZED;

    ComPtr<ITaskService> service;
    HRESULT hr = OpenService(&service);
    if (FAILED(hr)) return hr;

    ComPtr<ITaskFolder> folder;
    hr = OpenFolder(service.get(), &folder);
    if (FAILED(hr)) return hr;

    const std::wstring xml = TaskXml(exe, arguments);
    const Bstr path(kTaskPath);
    const Bstr definition(xml.c_str());
    if (path.bstr() == nullptr || definition.bstr() == nullptr) return E_OUTOFMEMORY;

    VARIANT none;
    VariantInit(&none);

    // Explicit DACL, because the default one grants the account that registered the task and not
    // much else. The browser that needs to wake the engine usually runs as a standard user, and a
    // refused IRegisteredTask::Run there means 13 seconds per browser restart — silently, since
    // nobody watches the log of a process they did not know was involved.
    //
    // The scheduler rejects a descriptor that does not grant the caller full control (E_ACCESSDENIED
    // on every attempt), so administrators and SYSTEM keep GA. AU gets GRGX: generic read plus
    // execute, which is what "run this task" needs and nothing more.
    const Bstr sddl(L"D:(A;;GA;;;BA)(A;;GA;;;SY)(A;;GRGX;;;AU)");
    if (sddl.bstr() == nullptr) return E_OUTOFMEMORY;
    VARIANT security;
    VariantInit(&security);
    security.vt = VT_BSTR;
    security.bstrVal = sddl.bstr();  // borrowed: the Bstr outlives every call below

    // Delete first, then create: the security descriptor only takes effect on creation, and an
    // upgrade has to replace a task registered by an older build that carried the default DACL.
    folder->DeleteTask(path.bstr(), 0);

    // The logon type is already in the XML principal; these flags only say how the *caller* is
    // authenticating. GROUP fits a group principal and NONE means "take it from the definition",
    // and which one the service accepts has varied between Windows versions, so ask both. The
    // OR_UPDATE retries exist for the one case delete cannot win: a task that is running right
    // now (the usual state during an upgrade) may refuse to go, and a stale DACL beats no task.
    const auto create = [&](TASK_LOGON_TYPE logon, LONG flags) -> HRESULT {
        ComPtr<IRegisteredTask> registered;
        return folder->RegisterTask(path.bstr(), definition.bstr(), flags, none, none, logon,
                                    security, registered.put());
    };
    hr = create(TASK_LOGON_GROUP, TASK_CREATE);
    if (FAILED(hr)) hr = create(TASK_LOGON_NONE, TASK_CREATE);
    if (FAILED(hr)) hr = create(TASK_LOGON_GROUP, TASK_CREATE_OR_UPDATE);
    if (FAILED(hr)) hr = create(TASK_LOGON_NONE, TASK_CREATE_OR_UPDATE);
    return hr;
}

HRESULT Start() {
    Com com;
    if (!com.ready()) return CO_E_NOTINITIALIZED;

    ComPtr<ITaskService> service;
    HRESULT hr = OpenService(&service);
    if (FAILED(hr)) return hr;

    ComPtr<ITaskFolder> folder;
    hr = OpenFolder(service.get(), &folder);
    if (FAILED(hr)) return hr;

    const Bstr path(kTaskPath);
    ComPtr<IRegisteredTask> task;
    // ERROR_FILE_NOT_FOUND here means an install from before the task existed, and the caller
    // falls back to CreateProcess for those.
    hr = folder->GetTask(path.bstr(), task.put());
    if (FAILED(hr)) return hr;

    // An engine that is already up needs nothing. Asking anyway is not free of surprises: the
    // scheduler may answer SCHED_E_TASK_ATTEMPTED, which reads like a failure to the caller even
    // though it is the best possible state.
    TASK_STATE state = TASK_STATE_UNKNOWN;
    if (SUCCEEDED(task->get_State(&state)) && state == TASK_STATE_RUNNING) return S_OK;

    // A disabled task exists and will never run, and Run() answering S_OK for it would leave the
    // caller waiting for an engine that is not coming — the one symptom the whole mechanism exists
    // to remove. Reporting a failure is what makes the DLL start the executable itself instead.
    VARIANT_BOOL enabled = VARIANT_FALSE;
    if (SUCCEEDED(task->get_Enabled(&enabled)) && enabled == VARIANT_FALSE) {
        return SCHED_E_TASK_DISABLED;
    }

    VARIANT none;
    VariantInit(&none);
    ComPtr<IRunningTask> running;
    // IgnoreNew can answer S_OK without starting anything when an instance is already running,
    // and pRunning is allowed to come back null in that case. Both are "the engine is up".
    return task->Run(none, running.put());
}

HRESULT Remove() {
    Com com;
    if (!com.ready()) return CO_E_NOTINITIALIZED;

    ComPtr<ITaskService> service;
    HRESULT hr = OpenService(&service);
    if (FAILED(hr)) return hr;

    ComPtr<ITaskFolder> folder;
    hr = OpenFolder(service.get(), &folder);
    if (FAILED(hr)) return hr;

    const Bstr path(kTaskPath);
    return folder->DeleteTask(path.bstr(), 0);
}

}  // namespace schtask
}  // namespace voicevox
