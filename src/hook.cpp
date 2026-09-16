#include <windows.h>
#include <shellscalingapi.h>
#include <detours.h>

#include <cwchar>

namespace {

using GetDpiForMonitorFn = HRESULT(WINAPI*)(
    HMONITOR,
    MONITOR_DPI_TYPE,
    UINT*,
    UINT*);

GetDpiForMonitorFn g_originalGetDpiForMonitor = GetDpiForMonitor;
UINT g_effectiveDpi = 96;

void SignalFromEnvironment(const wchar_t* variableName) {
    wchar_t eventName[512]{};
    const DWORD chars = GetEnvironmentVariableW(
        variableName, eventName, static_cast<DWORD>(std::size(eventName)));
    if (chars == 0 || chars >= std::size(eventName)) {
        return;
    }

    HANDLE eventHandle = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName);
    if (eventHandle) {
        SetEvent(eventHandle);
        CloseHandle(eventHandle);
    }
}

bool ReadDpiFromEnvironment() {
    wchar_t dpiBuffer[32]{};
    const DWORD chars = GetEnvironmentVariableW(
        L"RDPSCALE_DPI", dpiBuffer, static_cast<DWORD>(std::size(dpiBuffer)));
    if (chars == 0 || chars >= std::size(dpiBuffer)) {
        return false;
    }

    wchar_t* end = nullptr;
    const unsigned long dpi = std::wcstoul(dpiBuffer, &end, 10);
    if (!end || *end != L'\0' || dpi < 96 || dpi > 480) {
        return false;
    }

    g_effectiveDpi = static_cast<UINT>(dpi);
    return true;
}

HRESULT WINAPI HookGetDpiForMonitor(
    HMONITOR monitor,
    MONITOR_DPI_TYPE dpiType,
    UINT* dpiX,
    UINT* dpiY) {

    const HRESULT hr = g_originalGetDpiForMonitor(
        monitor, dpiType, dpiX, dpiY);

    if (SUCCEEDED(hr) && dpiType == MDT_EFFECTIVE_DPI) {
        if (dpiX) {
            *dpiX = g_effectiveDpi;
        }
        if (dpiY) {
            *dpiY = g_effectiveDpi;
        }
    }

    return hr;
}

bool InstallHook() {
    if (!ReadDpiFromEnvironment()) {
        return false;
    }

    if (DetourTransactionBegin() != NO_ERROR) {
        return false;
    }
    if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR) {
        DetourTransactionAbort();
        return false;
    }
    if (DetourAttach(
            reinterpret_cast<PVOID*>(&g_originalGetDpiForMonitor),
            reinterpret_cast<PVOID>(HookGetDpiForMonitor)) != NO_ERROR) {
        DetourTransactionAbort();
        return false;
    }
    if (DetourTransactionCommit() != NO_ERROR) {
        return false;
    }

    return true;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (DetourIsHelperProcess()) {
        return TRUE;
    }

    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);

        // DetourCreateProcessWithDllEx temporarily modifies mstsc's in-memory
        // import table. Restore that temporary scaffolding before installing
        // the actual GetDpiForMonitor detour.
        DetourRestoreAfterWith();

        if (!InstallHook()) {
            SignalFromEnvironment(L"RDPSCALE_FAILED_EVENT");
            return FALSE;
        }

        SignalFromEnvironment(L"RDPSCALE_READY_EVENT");
    }

    return TRUE;
}
