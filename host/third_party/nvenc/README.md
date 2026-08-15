# NVIDIA Video Codec SDK header

`twinhost` compiles against NVENC without linking the SDK — it loads
`nvEncodeAPI64.dll` (shipped by the NVIDIA driver) at runtime.

**Do this once** — either way below:

1. **Official (registration required):** download the NVIDIA Video Codec SDK
   from https://developer.nvidia.com/nvidia-video-codec-sdk and copy
   `Interface/nvEncodeAPI.h` from the SDK into this directory
   (`host/third_party/nvenc/nvEncodeAPI.h`).

   **Or, no registration:** FFmpeg mirrors the same header in
   `nv-codec-headers` (the NVENC API header is independent of FFmpeg itself):

   ```
   curl -L -o nvEncodeAPI.h ^
     https://raw.githubusercontent.com/FFmpeg/nv-codec-headers/master/include/ffnvcodec/nvEncodeAPI.h
   ```

2. Rebuild the host.

The code targets the modern NVENC API (SDK 12+, as shipped in current NVIDIA
drivers): `nvEncEncodePicture` with `NV_ENC_PIC_PARAMS`, the
`NV_ENC_INITIALIZE_PARAMS::encodeConfig` pointer, `NV_ENC_RC_PARAMS.averageBitRate`,
preset `NV_ENC_PRESET_P3_GUID` + `NV_ENC_TUNING_INFO_LOW_LATENCY`.

Do not commit the NVIDIA header into git if the license prohibits it; the
`.gitignore` ignores it by default.
