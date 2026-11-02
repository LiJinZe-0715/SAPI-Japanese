// COM entry points for the SAPI engine.
//
// SAPI loads this DLL into whatever process asks for a voice, which in practice means Chrome,
// Edge, Narrator or any other SAPI5 host. Two consequences shape the code below:
//
//  * nothing heavy happens in DllMain (no core init, no ONNX Runtime) — it runs under the
//    loader lock, and blocking there deadlocks the host;
//  * the dependency on voicevox_core.dll is delay-loaded, because the default search order for
//    a dependency starts at the host executable's directory, not ours.

#include <windows.h>
#include <objbase.h>

#include <new>
#include <string>

#include <delayimp.h>

#include "catalog.h"
#include "core.h"
#include "engine.h"
#include "log.h"
#include "registry.h"
#include "text.h"

volatile LONG voicevox::g_liveObjects = 0;

namespace {

HINSTANCE g_module = nullptr;

std::wstring ModulePath() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(g_module, buffer, MAX_PATH);
    // MAX_PATH back means "truncated", and a truncated path written into InprocServer32 is worse
    // than no registration at all: every voice would load a COM server that does not exist.
    if (length == 0 || length >= MAX_PATH) return std::wstring();
    return std::wstring(buffer, length);
}

class Factory final : public IClassFactory {
public:
    explicit Factory(int styleId) : refs_(1), styleId_(styleId) {
        InterlockedIncrement(&voicevox::g_liveObjects);
    }

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
        if (ppv == nullptr) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHOD_(ULONG, AddRef)() override {
        return (ULONG)InterlockedIncrement(&refs_);
    }

    STDMETHOD_(ULONG, Release)() override {
        const LONG left = InterlockedDecrement(&refs_);
        if (left == 0) {
            InterlockedDecrement(&voicevox::g_liveObjects);
            delete this;
        }
        return (ULONG)left;
    }

    STDMETHOD(CreateInstance)(IUnknown* outer, REFIID riid, void** ppv) override {
        if (outer != nullptr) return CLASS_E_NOAGGREGATION;
        return voicevox::Engine::Create(styleId_, riid, ppv);
    }

    STDMETHOD(LockServer)(BOOL lock) override {
        if (lock) {
            InterlockedIncrement(&voicevox::g_liveObjects);
        } else {
            InterlockedDecrement(&voicevox::g_liveObjects);
        }
        return S_OK;
    }

private:
    ~Factory() = default;
    volatile LONG refs_;
    int styleId_;
};

}  // namespace

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (ppv == nullptr) return E_POINTER;
    *ppv = nullptr;

    int styleId = 0;
    if (!voicevox::StyleFromClsid(clsid, &styleId)) {
        voicevox::Log(L"classobject: unknown clsid");
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    voicevox::Log(L"classobject: style=%d", styleId);

    Factory* factory = new (std::nothrow) Factory(styleId);
    if (factory == nullptr) return E_OUTOFMEMORY;

    const HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

extern "C" HRESULT __stdcall DllCanUnloadNow() {
    return voicevox::g_liveObjects == 0 ? S_OK : S_FALSE;
}

extern "C" HRESULT __stdcall DllRegisterServer() {
    const std::wstring module = ModulePath();
    if (module.empty()) {
        voicevox::Log(L"register: cannot resolve own path");
        return E_UNEXPECTED;
    }

    std::wstring error;
    voicevox::RegisterStats stats;
    if (!voicevox::RegisterVoices(module, &error, &stats)) {
        voicevox::Log(L"register: failed: %s", error.c_str());
        OutputDebugStringW((L"VoicevoxSapi: " + error + L"\n").c_str());
        return E_FAIL;
    }
    // `filtered` is the number a support question usually needs: it is the difference between
    // "the install is broken" and "voices.txt asked for six of these on purpose".
    voicevox::Log(L"register: %d voice(s), %d not selected, %d skipped (model not installed)",
                  stats.registered, stats.filtered, stats.missingModel);
    return S_OK;
}

extern "C" HRESULT __stdcall DllUnregisterServer() {
    std::wstring error;
    return voicevox::UnregisterVoices(&error) ? S_OK : E_FAIL;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

// Delay-load hook: resolve voicevox_core.dll by absolute path from the data directory, so the
// host process's directory layout never matters.
extern "C" FARPROC WINAPI VoicevoxDliHook(unsigned notify, PDelayLoadInfo info) {
    if (notify == dliNotePreLoadLibrary && info != nullptr && info->szDll != nullptr &&
        _stricmp(info->szDll, "voicevox_core.dll") == 0) {
        const std::wstring path = voicevox::Core::HomeDir() + L"\\voicevox_core.dll";
        HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        return (FARPROC)module;
    }
    return nullptr;
}

extern "C" const PfnDliHook __pfnDliNotifyHook2 = VoicevoxDliHook;
