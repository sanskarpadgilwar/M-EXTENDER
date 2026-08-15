# TwinScreen virtual display driver (IddCx)

A minimal UMDF2 indirect display driver that presents one (or more) virtual
monitors to Windows. Frames are captured in user mode by `twinhost` via Desktop
Duplication on the virtual monitor, so the driver only **ACKs and drops** every
swap-chain frame - it never renders and never touches the network.

API usage and structure are grounded against
[`VirtualDrivers/Virtual-Display-Driver`](https://github.com/VirtualDrivers/Virtual-Display-Driver)
(MIT, a fork of `itsmikethetech/IddSampleDriver`, itself derived from
Microsoft's `IndirectDisplay` sample). The embedded EDID and the INF structure
are taken verbatim from that project.

## Layout

```
driver/
├── src/Driver.cpp        # DriverEntry, device add, adapter, monitors, swap chain
├── src/Driver.h          # context classes (Direct3DDevice, SwapChainProcessor, ...)
├── src/Trace.h           # OutputDebugString-based logging
├── TwinScreen.inf        # UMDF + IddCx install metadata (Root\TwinScreen)
├── TwinScreen.vcxproj    # WDK x64 build (WindowsUserModeDriver10.0, IddCx 1.10)
├── option.txt            # virtual display count + mode list (w,h,refresh)
├── build.ps1             # locate VS/WDK, msbuild, stage driver\out\pkg
├── install.bat           # test-signing + pnputil + stage option.txt
└── uninstall.bat         # help find and delete the oem##.inf package
```

## How it works

1. On D0 entry the driver calls `IddCxAdapterInitAsync` and, once the OS
   finishes adapter init (`EvtIddCxAdapterInitFinished`), creates a monitor per
   configured display count with a fixed EDID and calls `IddCxMonitorArrival`.
2. Available resolutions come from `option.txt` and are served via
   `EvtIddCxMonitorQueryTargetModes`.
3. When the display is enabled, the OS assigns a swap chain
   (`EvtIddCxMonitorAssignSwapChain`); a `SwapChainProcessor` thread binds a
   D3D11 device (`IddCxSwapChainSetDevice`) and consumes frames via
   `IddCxSwapChainReleaseAndAcquireBuffer(2)`, waiting on the new-frame event.
   Each frame is immediately released (`IddCxSwapChainFinishedProcessingFrame`)
   - ack-and-drop.
4. `twinhost --monitor <n>` captures the virtual monitor's pixels through
   Desktop Duplication and streams them to the tablet.

## Build

Prerequisites:

- Visual Studio 2022 (Desktop C++ workload, MSBuild)
- Windows 10/11 SDK
- **WDK for Windows 10/11, 10.0.22621 or newer**

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
# → driver\out\pkg\TwinScreen.{dll,inf,cat} + option.txt
```

If the WDK you have does not ship `iddcx\1.10` / `wdf\umdf\2.25` headers, bump
`IDDCX_VERSION_MINOR` / `UMDF_VERSION_MINOR` in `TwinScreen.vcxproj` to a
version your WDK contains.

## Install (test-signed)

```bat
:: run install.bat as Administrator (or follow the steps below)
:: 1. enable test signing once, then reboot
bcdedit /set testsigning on

:: 2. install the driver package
pnputil /add-driver out\pkg\TwinScreen.inf /install

:: 3. if the monitor did not appear: Device Manager > Action > Add legacy
::    hardware > choose from a list > Display adapters > Have Disk > browse to
::    out\pkg\TwinScreen.inf
```

Then in **Settings > Display**: select *TwinScreen Virtual Display*, set it to
your tablet's native resolution (e.g. 1600x2560 for a portrait 10-11" tablet,
2560x1600 landscape, 2000x1200 for 12-13"), and **Extend these displays**.

`install.bat` also copies `option.txt` to `C:\ProgramData\TwinScreen\` so the
driver can read it at start; edit it to change the monitor count or mode list
and reboot (or disable/re-enable the display) to apply.

Test signing shows a watermark on the desktop and is not available on Secure
Boot machines without extra steps. For a personal rig that is fine.

## Uninstall / recovery

```bat
:: find the package (look for publisher TwinScreen)
pnputil /enum-drivers
pnputil /delete-driver oem##.inf /uninstall
```

If the machine ever fails to boot into a usable desktop: reboot into Safe Mode
(`shift`+restart > Troubleshoot > Startup Settings), which disables third-party
display drivers, then delete the driver with `pnputil`.

## Debugging

The driver logs to the debugger output (DbgView / WinDbg / VS Output) with an
`[TwinScreen]` prefix. Watch for:

- `IddCxAdapterInitAsync failed` / `IddCxMonitorCreate failed` - adapter or
  EDID problem.
- `QueryModes: buffer too small` - the OS asked for fewer modes than offered;
  harmless (the OS calls again with a larger buffer).
- The monitor not appearing at all usually means the driver did not load; check
  Device Manager for a yellow-bang on "TwinScreen Virtual Display" and the
  **UMDF host process** (`WUDFHost.exe`) for load failures.

## Notes

- `EvtIddCxMonitorGetDefaultDescriptionModes` returns `STATUS_NOT_IMPLEMENTED`
  by design: every monitor ships a full EDID, and target modes are served by
  `EvtIddCxMonitorQueryTargetModes`.
- The EDID is the battle-tested 256-byte blob from VirtualDrivers (base
  block + CEA extension); the checksum byte is recomputed at load.
- Driver signing for a personal rig is out of scope (test signing only).
