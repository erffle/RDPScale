# RDPScale

RDPScale launches the built-in Windows Remote Desktop client (`mstsc.exe`) with a DPI scale that can differ from the local Windows display scale.

For example, a local 4K desktop can remain at 175% while an RDP session is started at 100% or 125%.

> **Status:** v0.1.0 working baseline. Field-tested on Windows 11 x64 with the stock Microsoft Remote Desktop client.

## Why

Recent Windows versions may ignore or effectively override the legacy `.rdp` `desktopscalefactor` setting. RDPScale takes a different approach: it starts `mstsc.exe` through Microsoft Detours, causes `RDPScaleHook.dll` to be loaded before the application code runs, and hooks `shcore!GetDpiForMonitor`.

The RDP protocol, credentials, TLS, networking, clipboard, audio, drives, and authentication remain handled by Microsoft's own `mstsc.exe`.

## Usage

```text
RDPScale.exe /scale:100 "C:\RDP\SRV.rdp"
RDPScale.exe /scale:125 /v:SRV /f
RDPScale.exe /scale:150 /v:192.168.1.10
```

Advanced form:

```text
RDPScale.exe /dpi:96  /v:SRV /f
RDPScale.exe /dpi:120 /v:SRV /f
```

Common mappings:

| Scale | DPI |
|---:|---:|
| 100% | 96 |
| 125% | 120 |
| 150% | 144 |
| 175% | 168 |
| 200% | 192 |
| 250% | 240 |
| 300% | 288 |

All arguments other than `/scale:` or `/dpi:` are passed to `mstsc.exe`.

## Design

```text
RDPScale.exe
    |
    +-- DetourCreateProcessWithDllExW(mstsc.exe, RDPScaleHook.dll)
            |
            +-- Windows loader loads RDPScaleHook.dll before mstsc application code
                    |
                    +-- hook shcore!GetDpiForMonitor
                    +-- override MDT_EFFECTIVE_DPI only
```

The hook changes only `MDT_EFFECTIVE_DPI`; raw and angular DPI queries are left untouched.

## Windows 11 connection security warning

Starting with the April 2026 Windows security update, opening an `.rdp` file can show the new **Unknown remote connection** security dialog on every launch.

Microsoft currently provides a compatibility switch that restores the previous dialog behavior. Run an elevated Command Prompt:

```cmd
reg add "HKLM\Software\Policies\Microsoft\Windows NT\Terminal Services\Client" /v RedirectionWarningDialogVersion /t REG_DWORD /d 1 /f
```

Close existing `mstsc.exe` processes and reconnect.

To return to the current Windows default behavior:

```cmd
reg delete "HKLM\Software\Policies\Microsoft\Windows NT\Terminal Services\Client" /v RedirectionWarningDialogVersion /f
```

Microsoft documents this as a temporary compatibility option and notes that a future Windows update may remove it.

## Build

The repository builds on GitHub Actions using Microsoft's Windows runner, MSVC, CMake, and a pinned Microsoft Detours commit. No compiler is required on the machine where RDPScale is used.

Local build, if desired:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

Output:

```text
RDPScale.exe
RDPScaleHook.dll
```

## Security notes

RDPScale uses DLL injection and API hooking for the narrow purpose of changing the DPI value observed by `mstsc.exe`. Security software can treat injection techniques as suspicious even when the use is benign. Source code and CI build instructions are public so the resulting binaries can be audited and rebuilt.

RDPScale does not implement, proxy, or modify the RDP protocol itself.

## License

RDPScale is released under the MIT License. Microsoft Detours is used under its MIT License; see `THIRD_PARTY_NOTICES.md`.
