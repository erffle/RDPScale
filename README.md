# RDPScale

RDPScale launches the built-in Windows Remote Desktop client (`mstsc.exe`) with a DPI scale that can differ from the local Windows display scale.

For example, a local 4K desktop can remain at 175% while an RDP session is started at 100% or 125%.

> **Status:** early experimental implementation. CI build and real-machine validation are the next steps.

## Why

Recent Windows versions may ignore or effectively override the legacy `.rdp` `desktopscalefactor` setting. RDPScale takes a different approach: it starts `mstsc.exe` suspended, injects a very small helper DLL, installs a hook for `shcore!GetDpiForMonitor`, and only then resumes `mstsc.exe`.

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
    +-- CreateProcess(mstsc.exe, CREATE_SUSPENDED)
    |
    +-- inject RDPScaleHook.dll
    |       |
    |       +-- hook shcore!GetDpiForMonitor
    |
    +-- wait until the hook reports READY
    |
    +-- ResumeThread(mstsc)
```

The hook only overrides `MDT_EFFECTIVE_DPI`; raw and angular DPI queries are left untouched.

## Build

The repository is intended to build on GitHub Actions using Microsoft's Windows runner, MSVC, CMake, and a pinned MinHook release. No compiler is required on the machine where RDPScale is used.

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

RDPScale uses DLL injection and API hooking for the narrow purpose of changing the DPI value observed by `mstsc.exe`. Security software can treat injection techniques as suspicious even when the use is benign. Source code and CI build instructions are kept public so the resulting binaries can be audited and rebuilt.

## License

RDPScale is released under the MIT License. MinHook is used under its BSD 2-Clause license; see `THIRD_PARTY_NOTICES.md`.
