#include <windows.h>
#include <shellscalingapi.h>
#include <MinHook.h>

#include <cwchar>
#include <string>

namespace {

using GetDpiForMonitorFn = HRESULT(WINAPI*)(
    HMONITOR,
    MONITOR_DPI_TYPE,
    UINT*,
    UINT*);

GetDpiForMonitorFn g_originalGetDpiForMonitor = nullptr;
UINT g_effectiveDpi = 96;

std::wstring EventName(const wchar_t* kind) {
    return std::wstring(L"Local\\RDPScale_") + kind + L"_" +
           std::to_wstring(GetCurrentProcessId());
}

void Signal(const wchar_t* kind) {
    const std::wstring name = EventName(kind);
    HANDLE eventHandle = OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str());
    if (eventHandle) {
        SetEvent(eventHandle);
        CloseHandle(eventHandle);
    }
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

DWORD WINAPI InitializeHook(void*) {
    wchar_t dpiBuffer[32]{};
    const DWORD chars = GetEnvironmentVariableW(
        L"RDPSCALE_DPI", dpiBuffer, static_cast<DWORD>(std::size(dpiBuffer)));

    if (chars == 0 || chars >= std::size(dpiBuffer)) {
        Signal(L"FAILED");
        return 1;
    }

    wchar_t* end = nullptr;
    const unsigned long dpi = std::wcstoul(dpiBuffer, &end, 10);
    if (!end || *end != L'\0' || dpi < 96 || dpi > 480) {
        Signal(L"FAILED");
        return 2;
    }
    g_effectiveDpi = static_cast<UINT>(dpi);

    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (!shcore) {
        Signal(L"FAILED");
        return 3;
    }

    void* target = reinterpret_cast<void*>(
        GetProcAddress(shcore, "GetDpiForMonitor"));
    if (!target) {
        Signal(L"FAILED");
        return 4;
    }

    if (MH_Initialize() != MH_OK) {
        Signal(L"FAILED");
        return 5;
    }

    if (MH_CreateHook(
            target,
            reinterpret_cast<void*>(&HookGetDpiForMonitor),
            reinterpret_cast<void**>(&g_originalGetDpiForMonitor)) != MH_OK) {
        Signal(L"FAILED");
        return 6;
    }

    if (MH_EnableHook(target) != MH_OK) {
        Signal(L"FAILED");
        return 7;
    }

    Signal(L"READY");
    return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);

        // Do the real initialization outside the loader lock. The launcher keeps
        // mstsc's primary thread suspended until this worker signals READY.
        HANDLE thread = CreateThread(nullptr, 0, InitializeHook, nullptr, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
        } else {
            Signal(L"FAILED");
        }
    }
    return TRUE;
}
