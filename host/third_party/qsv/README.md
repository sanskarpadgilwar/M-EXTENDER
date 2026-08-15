# oneVPL headers (QSV encoder)

`twinhost` compiles against the legacy Intel Media SDK / oneVPL C API without
linking any SDK — the runtime DLL is loaded at runtime:

- `libmfxhw64.dll` (Intel driver runtime)
- `libmfx64.dll` (legacy dispatcher)
- `vpl.dll` (oneVPL dispatcher)

**Do this once:**

```
fetch.bat
```

This clones the [oneVPL](https://github.com/oneapi-src/oneVPL) repository and
copies `api/vpl/` (the `mfx*.h` header set) into `api/vpl/` here. The headers
are MIT-licensed and gitignored by default.

**Requirements**

- An Intel GPU with a driver newer than ~2019 (`libmfxhw64.dll` in
  `System32`), or an install of the Intel oneVPL dispatcher (`vpl.dll`).
- `git` on PATH.

**Notes**

- The encoder feeds system-memory NV12 frames (the native hardware AVC input)
  with a CPU BGRA→NV12 conversion, so no D3D11 allocator is required.
- `MFXVideoENCODE_Reset` is used for `SetBitrate`; if a runtime lacks the
  export (only the old dispatcher does), adaptive bitrate is disabled.
