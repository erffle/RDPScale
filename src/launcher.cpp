#include <windows.h>
#include <detours.h>

#include <cerrno>
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

    HANDLE get() const { return handle_; }
    explicit operator bool() const { return handle_ && handle_ != INVALID_HANDLE_VALUE; }

    void reset(HANDLE value = nullptr) {
        if (handle_ && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = value;
    }

private:
    HANDLE handle_ = nullptr;
};

class ScopedEnvironmentVariable {
public:
    ScopedEnvironmentVariable(const wchar_t* name, const std::wstring& value)
        : name_(name) {
        const DWORD needed = GetEnvironmentVariableW(name_.c_str(), nullptr, 0);
        hadOldValue_ = needed != 0;
        if (hadOldValue_) {
            oldValue_.resize(needed);
            const DWORD copied = GetEnvironmentVariableW(name_.c_str(), oldValue_.data(), needed);
            if (copied < needed) {
                oldValue_.resize(copied);
            }
        }
        ok_ = SetEnvironmentVariableW(name_.c_str(), value.c_str()) != FALSE;
    }

    ~ScopedEnvironmentVariable() {
        if (!ok_) {
            return;
        }
        if (hadOldValue_) {
            SetEnvironmentVariableW(name_.c_str(), oldValue_.c_str());
        } else {
            SetEnvironmentVariableW(name_.c_str(), nullptr);
        }
    }

    bool ok() const { return ok_; }

private:
    std::wstring name_;
    std::wstring oldValue_;
    bool hadOldValue_ = false;
    bool ok_ = false;
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

std::optional<std::string> ToDetoursDllPath(const std::wstring& path) {
    std::wstring candidate = path;

    auto convert = [](const std::wstring& value) -> std::optional<std::string> {
        BOOL usedDefault = FALSE;
        const int needed = WideCharToMultiByte(
            CP_ACP, WC_NO_BEST_FIT_CHARS, value.c_str(), -1,
            nullptr, 0, nullptr, &usedDefault);
        if (needed <= 1 || usedDefault) {
            return std::nullopt;
        }

        std::string result(static_cast<size_t>(needed - 1), '\0');
        usedDefault = FALSE;
        const int converted = WideCharToMultiByte(
            CP_ACP, WC_NO_BEST_FIT_CHARS, value.c_str(), -1,
            result.data(), needed, nullptr, &usedDefault);
        if (converted != needed || usedDefault) {
            return std::nullopt;
        }
        return result;
    };

    if (auto direct = convert(candidate)) {
        return direct;
    }

    std::vector<wchar_t> shortBuffer(32768);
    const DWORD shortLength = GetShortPathNameW(
        path.c_str(), shortBuffer.data(), static_cast<DWORD>(shortBuffer.size()));
    if (shortLength == 0 || shortLength >= shortBuffer.size()) {
        return std::nullopt;
    }

    return convert(std::wstring(shortBuffer.data(), shortLength));
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

    const auto hookPathAnsi = ToDetoursDllPath(hookPath);
    if (!hookPathAnsi) {
        std::wcerr << L"RDPScale: hook DLL path cannot be represented for Detours. "
                   << L"Move RDPScale to a path containing ASCII characters.\n";
        return 3;
    }

    std::wstring commandLine = QuoteArg(mstscPath);
    for (const auto& arg : mstscArgs) {
        commandLine.push_back(L' ');
        commandLine += QuoteArg(arg);
    }
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    const std::wstring token = std::to_wstring(GetCurrentProcessId()) + L"_" +
                               std::to_wstring(GetTickCount64());
    const std::wstring readyEventName = L"Local\\RDPScale_READY_" + token;
    const std::wstring failedEventName = L"Local\\RDPScale_FAILED_" + token;

    UniqueHandle readyEvent(CreateEventW(nullptr, TRUE, FALSE, readyEventName.c_str()));
    UniqueHandle failedEvent(CreateEventW(nullptr, TRUE, FALSE, failedEventName.c_str()));
    if (!readyEvent || !failedEvent) {
        PrintWin32Error(L"CreateEventW");
        return 4;
    }

    ScopedEnvironmentVariable dpiEnv(L"RDPSCALE_DPI", std::to_wstring(*dpi));
    ScopedEnvironmentVariable readyEnv(L"RDPSCALE_READY_EVENT", readyEventName);
    ScopedEnvironmentVariable failedEnv(L"RDPSCALE_FAILED_EVENT", failedEventName);
    if (!dpiEnv.ok() || !readyEnv.ok() || !failedEnv.ok()) {
        PrintWin32Error(L"SetEnvironmentVariableW");
        return 4;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION processInfo{};

    // Microsoft Detours creates the target suspended, patches its in-memory
    // import table so RDPScaleHook.dll is loaded before application code, and
    // then resumes it. This avoids timing races and debugger dependencies.
    const BOOL created = DetourCreateProcessWithDllExW(
        mstscPath.c_str(),
        mutableCommand.data(),
        nullptr,
        nullptr,
        FALSE,
        0,
        nullptr,
        nullptr,
        &startup,
        &processInfo,
        hookPathAnsi->c_str(),
        nullptr);

    if (!created) {
        PrintWin32Error(L"DetourCreateProcessWithDllExW");
        return 5;
    }

    UniqueHandle process(processInfo.hProcess);
    UniqueHandle primaryThread(processInfo.hThread);

    HANDLE waits[] = {readyEvent.get(), failedEvent.get(), process.get()};
    const DWORD hookWait = WaitForMultipleObjects(3, waits, FALSE, 10000);
    if (hookWait == WAIT_OBJECT_0 + 1) {
        std::wcerr << L"RDPScale: hook DLL reported initialization failure\n";
        return 6;
    }
    if (hookWait == WAIT_OBJECT_0 + 2) {
        DWORD exitCode = 0;
        GetExitCodeProcess(process.get(), &exitCode);
        std::wcerr << L"RDPScale: mstsc.exe exited before the DPI hook initialized "
                   << L"(exit code " << exitCode << L")\n";
        return 6;
    }
    if (hookWait != WAIT_OBJECT_0) {
        std::wcerr << L"RDPScale: timed out waiting for the DPI hook\n";
        return 6;
    }

    std::wcout << L"RDPScale: started mstsc.exe PID " << processInfo.dwProcessId
               << L" with effective DPI " << *dpi;
    if (scale) {
        std::wcout << L" (" << *scale << L"%)";
    }
    std::wcout << L"\n";

    return 0;
}
