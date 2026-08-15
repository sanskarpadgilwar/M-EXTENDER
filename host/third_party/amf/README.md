# AMF (Advanced Media Framework) headers

The AMF encoder (`src/encode/amf.cpp`) compiles against the AMF public headers
from [GPUOpen-LibrariesAndSDKs/AMF](https://github.com/GPUOpen-LibrariesAndSDKs/AMF)
(MIT license). They are compile-time only:

- The encoder links **no** AMF library — the runtime (`amfrt64.dll`) is loaded
  dynamically at startup and ships with AMD graphics drivers.
- `CMakeLists.txt` only enables the AMF encoder when the headers are present.

## Fetching

Run once:

```
host\third_party\amf\fetch.bat
```

This clones the AMF repository into `repo\` and copies the public headers into
`include\`. The headers are gitignored; delete `repo\` after the copy if you
want to save space.

## CMake

CMake detects the headers via
`third_party/amf/include/components/VideoEncoderVCE.h` and defines
`TWIN_HAVE_AMF=1` when found, adding `src/encode/amf.cpp` to the build.
Without the headers the AMF encoder is silently excluded.
