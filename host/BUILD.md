# Building twinhost (Windows server)

Prerequisites (x64 Windows 10/11):

- Visual Studio 2022 Build Tools (or VS 2022) with the C++ desktop workload and
  the Windows 11 SDK
- CMake >= 3.16
- `git`

## One-time toolchain install

From an **admin** PowerShell:

```powershell
winget install -e --id Microsoft.VisualStudio.2022.BuildTools `
    --override "--add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.Windows11SDK.22621 --quiet --wait"
winget install -e --id Kitware.CMake
```

Reopen the terminal afterwards so `cmake` is on `PATH` (or run the CMake
installer and check "Add CMake to the system PATH").

## Build

```bat
git clone https://github.com/sanskarpadgilwar/M-EXTENDER.git
cd M-EXTENDER

:: fetch the encoder headers once (gitignored; DLLs load from the GPU drivers)
host\third_party\qsv\fetch.bat
host\third_party\amf\fetch.bat

cmake -S host -B host\build -G "Visual Studio 17 2022" -A x64
cmake --build host\build --config Release
```

The binary is `host\build\Release\twinhost.exe`.

Notes:

- The NVENC/QSV/AMF encoders are compiled in automatically when their headers
  are present (the fetch scripts above). Without them the build still succeeds
  with only the NullEncoder fallback.
- C++/WinRT (touch injection) is auto-detected from the installed Windows SDK;
  if absent the build succeeds with touch injection disabled.

## Run

Install the virtual display driver first (see `driver/README.md`), then:

```bat
twinhost.exe --monitor <n>   :: 0 = primary; use the TwinScreen display index
```

`--encoder` forces a specific implementation (`nvenc`, `qsv`, `amf`, or
`null`); the default order is nvenc -> qsv -> amf -> null. `--raw` sends
uncompressed RGBA via NullEncoder (bring-up only).

Connect the tablet (see top-level README) to `127.0.0.1:7200` over ADB port
forward, or to `<host-ip>:7200` over Wi-Fi.
