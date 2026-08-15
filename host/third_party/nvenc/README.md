# NVIDIA Video Codec SDK header

`twinhost` compiles against NVENC without linking the SDK — it loads
`nvEncodeAPI64.dll` (shipped by the NVIDIA driver) at runtime.

**Do this once:**

1. Download the NVIDIA Video Codec SDK (registration required):
   https://developer.nvidia.com/nvidia-video-codec-sdk
2. Copy `Interface/nvEncodeAPI.h` from the SDK into this directory
   (`host/third_party/nvenc/nvEncodeAPI.h`).
3. Rebuild the host.

Any SDK version >= 10 works (v12 recommended). The code uses the classic
`NV_ENCODE_API_FUNCTION_LIST` interface plus `NV_ENC_TUNING_INFO_LOW_LATENCY`.

Do not commit the NVIDIA header into git if the license prohibits it; the
`.gitignore` ignores it by default.
