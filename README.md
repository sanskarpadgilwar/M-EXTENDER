# twin-screen

A personal SuperDisplay-style second display: use your Android tablet as a
second monitor for Windows over USB (ADB) or Wi-Fi, with touch/stylus input.

```
┌──────────────────── Windows host (twinhost) ────────────────────┐      ┌────────── Android tablet ──────────┐
│  IddCx virtual display driver  (fork of MS IndirectDisplay)     │      │  MediaCodec HW decoder            │
│  Desktop Duplication API (DXGI) capture                         │      │  SurfaceView render               │
│  HW H.264/HEVC encoder (NVENC/QSV/AMF)                          │◄────►│  Touch/Stylus capture             │
│  TCP transport (USB ADB forward / Wi-Fi)                        │      │  Low-latency AudioTrack (AAC)     │
│  Input injection (SendInput + InjectTouchInput)                 │      └──────────────────────────────────┘
└─────────────────────────────────────────────────────────────────┘
```

## Repository layout

| Path        | Contents                                                        |
|-------------|-----------------------------------------------------------------|
| `proto/`    | Wire protocol: `protocol.h` (C, shared contract) + `protocol.md` |
| `host/`     | Windows server (C++ / CMake): capture, encode, net, input        |
| `tablet/`   | Android app (Kotlin, zero external deps): render, decode, input  |
| `driver/`   | IddCx virtual display driver (UMDF2, ack-and-drop; grounded on the MIT VirtualDrivers/Virtual-Display-Driver) |

## Prerequisites

- **Host**: Windows 10 1809+, Visual Studio 2022 (C++ Desktop + WDK 10.0.22621+),
  CMake ≥ 3.16. GPU with NVENC/QSV/AMF for the low-latency path (optional for bring-up).
- **Tablet**: Android 8.0+ (API 26), hardware H.264 decoder (universal).
- **Driver**: installed with test signing enabled (see `driver/README.md`).

## Build

### Host
Requires the encoder headers, each fetched once (headers are gitignored; the
encoder DLLs are loaded from the GPU drivers at runtime, so there's no SDK
link step):
- NVENC: `host/third_party/nvenc/nvEncodeAPI.h` (see `host/third_party/nvenc/README.md`)
- QSV: run `host/third_party/qsv/fetch.bat` (oneVPL `api/vpl` headers)
- AMF: run `host/third_party/amf/fetch.bat` (AMF public headers)

```bat
cmake -S host -B host/build -G "Visual Studio 17 2022" -A x64
cmake --build host/build --config Release
:: binary at host/build/Release/twinhost.exe
```

Touch injection uses the USER32 `InitializeTouchInjection`/`InjectTouchInput`
API (declared in `winuser.h`, no extra dependencies). The QSV and AMF encoders
are enabled automatically when their headers are present.

### Tablet
Open `tablet/` in Android Studio (it generates the Gradle wrapper) and run on a
device. No external dependencies.

## Run

1. Install the driver (see `driver/README.md`), reboot with test signing on,
   and set *TwinScreen Virtual Display* to your tablet's native resolution with
   **Extend these displays**:
   ```bat
   driver\install.bat
   ```
2. Start the host, pointing at the virtual monitor:
   `twinhost.exe --monitor <n>` (0 = primary; use the index of the TwinScreen
   display, usually 1+).
3. Plug the tablet in via USB (USB debugging on):
   ```bat
   adb forward tcp:7200 tcp:7200
   ```
4. In the tablet app, connect to `127.0.0.1:7200`. You'll get the handshake
   round-trip; with an NVIDIA/Intel/AMD GPU you also get a live H.264 stream.

Wi-Fi: connect to `<host-ip>:7200` directly instead of step 3.

## Status / roadmap

Done:
- **proto/** wire protocol (framing, handshake, video, input, control)
- **host** capture (Desktop Duplication, multi-monitor crop)
- **host** NVENC H.264 encoder (low-latency CBR, keyframe-on-demand, dynamic
  bitrate) with NullEncoder fallback
- **host** QSV H.264 encoder (oneVPL headers, dynamic DLL load, NV12
  system-memory input, keyframe-on-demand, dynamic bitrate via Reset)
- **host** AMF H.264 encoder (VCE via shared DX11 NV12 surfaces, dynamic DLL
  load, keyframe-on-demand, dynamic bitrate)
- **host** adaptive resolution under congestion: D3D11 GPU bilinear downscale
  ladder (1.0 / 0.75 / 0.5 / 0.375) with per-step encoder re-init and a forced
  keyframe so the tablet resyncs; congestion is the smoothed send time of each
  video frame (scale-down after a 45-frame congested streak, scale-up after a
  150-frame clear streak)
- **host** capture recovery: Desktop Duplication re-acquires the output after
  `DXGI_ERROR_ACCESS_LOST` (mode change, lock screen, driver reset) with a 1s
  retry backoff, so streaming resumes instead of going deaf
- **host** input: mouse (`SendInput`) + touch/stylus (USER32 `InjectTouchInput`)
- **host** true pen injection: `CreateSyntheticPointerDevice(PT_PEN)` +
  `InjectSyntheticPointerInput` (winuser.h, Win10 1809+) — real pen through the
  pointer pipeline with pressure (0..1024) and tilt (degrees); stylus falls
  back to touch routing if the API is unavailable
- **tablet** connect UI, TCP client, MediaCodec HW decode, multitouch+stylus
  input sender
- **host+tablet** audio: WASAPI loopback capture → Media Foundation AAC-LC
  encoder → ADTS on the wire → MediaCodec decode → AudioTrack (the tablet also
  accepts raw PCM; capture only produces frames while the render device is
  actually playing, so silence costs no bandwidth)
- **tablet** Wi-Fi polish: auto-reconnect with exponential backoff +
  on-screen stats overlay (fps, Mbps, queue depth, connection state)
- **driver/** UMDF2 IddCx virtual display driver (adapter + EDID monitor +
  `option.txt` mode list, ack-and-drop swap-chain processor, build/install
  scripts). API usage grounded against the MIT VirtualDrivers fork.

Remaining:
1. Build & test-sign the driver on a real machine (host toolchain is in place;
   the WDK 10.0.22621 workload still needs installing), and validate the mode
   list against the target tablet.
2. Runtime validation of the NVENC and AMF paths on machines with NVIDIA/AMD GPUs.
3. Pen-injection validation with a real pressure stylus on the tablet (the API
   path is implemented; tilt mapping is a simplification of Android's single
   AXIS_TILT into Windows tiltX/tiltY).

## Latency budget (USB, hardware encode)

| Stage | ms     |
|-------|--------|
| Capture (DDA)      | ~0–1   |
| HW encode          | 2–4    |
| USB transport      | 1–2    |
| HW decode          | 3–5    |
| Render + vsync     | 8–16   |
| **Total**          | **~15–28** (Wi-Fi +10–20) |

## Risk areas

- A broken virtual display driver can disrupt display startup — keep a recovery
  path (safe mode, `pnputil /delete-driver`).
- `InjectTouchInput` (USER32) sends touch to the interactive desktop of the
  session the host runs in; it must therefore run in the user's session, not a
  service.
- MediaCodec `KEY_LOW_LATENCY` is only honored on API 29+ and varies by device.
