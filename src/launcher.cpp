#include <windows.h>
#include <tlhelp32.h>

#include <cstdint>
#include <cwchar>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) : handle_(handle) {}
    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    HANDLE get() const { return handle_; }
    explicit operator bool() const { return handle_ && handle_ != INVALID_HANDLE_VALUE; }

    HANDLE release() {
        HANDLE value = handle_;
        handle_ = nullptr;
        return value;
    }

    void reset(HANDLE value = nullptr) {
        if (handle_ && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = value;
    }

private:
    HANDLE handle_ = nullptr;
};

std::wstring Win32Message(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                        FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD count = FormatMessageW(
        flags, nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);

    std::wstring result = count && buffer ? std::wstring(buffer, count) : L"Unknown error";
    if (buffer) {
        LocalFree(buffer);
    }
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) {
        result.pop_back();
    }
    return result;
}

void PrintWin32Error(const wchar_t* what, DWORD error = GetLastError()) {
    std::wcerr << L"RDPScale: " << what << L" failed (" << error << L"): "
               << Win32Message(error) << L"\n";
}

bool StartsWithInsensitive(const std::wstring& value, const wchar_t* prefix) {
    const size_t n = std::wcslen(prefix);
    return value.size() >= n && _wcsnicmp(value.c_str(), prefix, n) == 0;
}

std::optional<unsigned long> ParseUnsigned(const std::wstring& value) {
    if (value.empty()) {
        return std::nullopt;
    }
    wchar_t* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::wcstoul(value.c_str(), &end, 10);
    if (errno != 0 || !end || *end != L'\0') {
        return std::nullopt;
    }
    return parsed;
}

std::wstring QuoteArg(const std::wstring& arg) {
    if (arg.empty()) {
        return L"\"\"";
    }

    const bool needsQuotes = arg.find_first_of(L" \t\n\v\"") != std::wstring::npos;
    if (!needsQuotes) {
        return arg;
    }

    std::wstring out;
    out.push_back(L'\"');
    size_t backslashes = 0;

    for (const wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }

        if (ch == L'\"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'\"');
            backslashes = 0;
            continue;
        }

        out.append(backslashes, L'\\');
        backslashes = 0;
        out.push_back(ch);
    }

    out.append(backslashes * 2, L'\\');
    out.push_back(L'\"');
    return out;
}

std::wstring ExecutableDirectory() {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }

    std::wstring path(buffer.data(), length);
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

std::wstring SystemMstscPath() {
    std::vector<wchar_t> buffer(32768);
    const UINT length = GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    return std::wstring(buffer.data(), length) + L"\\mstsc.exe";
}

uintptr_t RemoteModuleBase(DWORD pid, const wchar_t* moduleName) {
    UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    if (!snapshot) {
        return 0;
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Module32FirstW(snapshot.get(), &entry)) {
        return 0;
    }

    do {
        if (_wcsicmp(entry.szModule, moduleName) == 0) {
            return reinterpret_cast<uintptr_t>(entry.modBaseAddr);
        }
    } while (Module32NextW(snapshot.get(), &entry));

    return 0;
}

bool InjectLibrary(DWORD pid, HANDLE process, const std::wstring& dllPath) {
    const size_t byteCount = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remoteString = VirtualAllocEx(
        process, nullptr, byteCount, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteString) {
        PrintWin32Error(L"VirtualAllocEx");
        return false;
    }

    auto freeRemote = [&]() { VirtualFreeEx(process, remoteString, 0, MEM_RELEASE); };

    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remoteString, dllPath.c_str(), byteCount, &written) ||
        written != byteCount) {
        PrintWin32Error(L"WriteProcessMemory");
        freeRemote();
        return false;
    }

    HMODULE localKernel32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC localLoadLibrary = localKernel32 ? GetProcAddress(localKernel32, "LoadLibraryW") : nullptr;
    const uintptr_t remoteKernel32 = RemoteModuleBase(pid, L"kernel32.dll");
    if (!localKernel32 || !localLoadLibrary || !remoteKernel32) {
        std::wcerr << L"RDPScale: could not resolve remote LoadLibraryW\n";
        freeRemote();
        return false;
    }

    const uintptr_t loadLibraryOffset =
        reinterpret_cast<uintptr_t>(localLoadLibrary) - reinterpret_cast<uintptr_t>(localKernel32);
    const auto remoteLoadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        remoteKernel32 + loadLibraryOffset);

    UniqueHandle thread(CreateRemoteThread(
        process, nullptr, 0, remoteLoadLibrary, remoteString, 0, nullptr));
    if (!thread) {
        PrintWin32Error(L"CreateRemoteThread");
        freeRemote();
        return false;
    }

    const DWORD wait = WaitForSingleObject(thread.get(), 10000);
    if (wait != WAIT_OBJECT_0) {
        std::wcerr << L"RDPScale: timed out loading hook DLL\n";
        freeRemote();
        return false;
    }

    DWORD loadResult = 0;
    if (!GetExitCodeThread(thread.get(), &loadResult) || loadResult == 0) {
        std::wcerr << L"RDPScale: LoadLibraryW failed inside mstsc.exe\n";
        freeRemote();
        return false;
    }

    freeRemote();
    return true;
}

std::wstring EventName(const wchar_t* kind, DWORD pid) {
    return std::wstring(L"Local\\RDPScale_") + kind + L"_" + std::to_wstring(pid);
}

void PrintUsage() {
    std::wcout
        << L"RDPScale - launch mstsc.exe with an independent DPI scale\n\n"
        << L"Usage:\n"
        << L"  RDPScale.exe /scale:100 <mstsc arguments>\n"
        << L"  RDPScale.exe /scale:125 \"C:\\RDP\\SRV.rdp\"\n"
        << L"  RDPScale.exe /dpi:96 /v:SRV /f\n\n"
        << L"RDPScale options:\n"
        << L"  /scale:N   UI scale in percent (100..500)\n"
        << L"  /dpi:N     effective DPI directly (96..480)\n"
        << L"  /?         show this help\n\n"
        << L"All other arguments are passed unchanged to mstsc.exe.\n";
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc <= 1) {
        PrintUsage();
        return 2;
    }

    std::optional<unsigned> dpi;
    std::optional<unsigned> scale;
    std::vector<std::wstring> mstscArgs;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"/?" || arg == L"--help" || arg == L"-h") {
            PrintUsage();
            return 0;
        }

        if (StartsWithInsensitive(arg, L"/scale:") || StartsWithInsensitive(arg, L"--scale=")) {
            if (dpi || scale) {
                std::wcerr << L"RDPScale: specify only one /scale or /dpi option\n";
                return 2;
            }
            const size_t split = arg.find_first_of(L":=");
            const auto parsed = ParseUnsigned(arg.substr(split + 1));
            if (!parsed || *parsed < 100 || *parsed > 500) {
                std::wcerr << L"RDPScale: scale must be an integer from 100 to 500\n";
                return 2;
            }
            scale = static_cast<unsigned>(*parsed);
            dpi = static_cast<unsigned>((*parsed * 96UL + 50UL) / 100UL);
            continue;
        }

        if (StartsWithInsensitive(arg, L"/dpi:") || StartsWithInsensitive(arg, L"--dpi=")) {
            if (dpi || scale) {
                std::wcerr << L"RDPScale: specify only one /scale or /dpi option\n";
                return 2;
            }
            const size_t split = arg.find_first_of(L":=");
            const auto parsed = ParseUnsigned(arg.substr(split + 1));
            if (!parsed || *parsed < 96 || *parsed > 480) {
                std::wcerr << L"RDPScale: DPI must be an integer from 96 to 480\n";
                return 2;
            }
            dpi = static_cast<unsigned>(*parsed);
            continue;
        }

        mstscArgs.push_back(arg);
    }

    if (!dpi) {
        std::wcerr << L"RDPScale: /scale:N or /dpi:N is required\n\n";
        PrintUsage();
        return 2;
    }

    const std::wstring exeDir = ExecutableDirectory();
    const std::wstring hookPath = exeDir.empty() ? L"" : exeDir + L"\\RDPScaleHook.dll";
    const std::wstring mstscPath = SystemMstscPath();

    if (hookPath.empty() || GetFileAttributesW(hookPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wcerr << L"RDPScale: RDPScaleHook.dll must be next to RDPScale.exe\n";
        return 3;
    }
    if (mstscPath.empty() || GetFileAttributesW(mstscPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wcerr << L"RDPScale: could not find the system mstsc.exe\n";
        return 3;
    }

    std::wstring commandLine = QuoteArg(mstscPath);
    for (const auto& arg : mstscArgs) {
        commandLine.push_back(L' ');
        commandLine += QuoteArg(arg);
    }
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    const wchar_t* envName = L"RDPSCALE_DPI";
    DWORD previousSize = GetEnvironmentVariableW(envName, nullptr, 0);
    std::wstring previousValue;
    const bool hadPrevious = previousSize != 0;
    if (hadPrevious) {
        previousValue.resize(previousSize);
        const DWORD copied = GetEnvironmentVariableW(envName, previousValue.data(), previousSize);
        if (copied < previousSize) {
            previousValue.resize(copied);
        }
    }

    const std::wstring dpiValue = std::to_wstring(*dpi);
    if (!SetEnvironmentVariableW(envName, dpiValue.c_str())) {
        PrintWin32Error(L"SetEnvironmentVariableW");
        return 3;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION processInfo{};

    const BOOL created = CreateProcessW(
        mstscPath.c_str(),
        mutableCommand.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_SUSPENDED,
        nullptr,
        nullptr,
        &startup,
        &processInfo);

    if (hadPrevious) {
        SetEnvironmentVariableW(envName, previousValue.c_str());
    } else {
        SetEnvironmentVariableW(envName, nullptr);
    }

    if (!created) {
        PrintWin32Error(L"CreateProcessW");
        return 4;
    }

    UniqueHandle process(processInfo.hProcess);
    UniqueHandle primaryThread(processInfo.hThread);
    const DWORD pid = processInfo.dwProcessId;

    const std::wstring readyName = EventName(L"READY", pid);
    const std::wstring failedName = EventName(L"FAILED", pid);
    UniqueHandle readyEvent(CreateEventW(nullptr, TRUE, FALSE, readyName.c_str()));
    UniqueHandle failedEvent(CreateEventW(nullptr, TRUE, FALSE, failedName.c_str()));

    if (!readyEvent || !failedEvent) {
        PrintWin32Error(L"CreateEventW");
        TerminateProcess(process.get(), 1);
        return 5;
    }

    if (!InjectLibrary(pid, process.get(), hookPath)) {
        TerminateProcess(process.get(), 1);
        return 6;
    }

    HANDLE events[] = {readyEvent.get(), failedEvent.get()};
    const DWORD hookWait = WaitForMultipleObjects(2, events, FALSE, 10000);
    if (hookWait == WAIT_OBJECT_0 + 1) {
        std::wcerr << L"RDPScale: hook DLL reported initialization failure\n";
        TerminateProcess(process.get(), 1);
        return 7;
    }
    if (hookWait != WAIT_OBJECT_0) {
        std::wcerr << L"RDPScale: timed out waiting for the DPI hook\n";
        TerminateProcess(process.get(), 1);
        return 7;
    }

    if (ResumeThread(primaryThread.get()) == static_cast<DWORD>(-1)) {
        PrintWin32Error(L"ResumeThread");
        TerminateProcess(process.get(), 1);
        return 8;
    }

    std::wcout << L"RDPScale: started mstsc.exe PID " << pid
               << L" with effective DPI " << *dpi;
    if (scale) {
        std::wcout << L" (" << *scale << L"%)";
    }
    std::wcout << L"\n";

    return 0;
}
