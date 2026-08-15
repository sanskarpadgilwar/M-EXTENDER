#include "encode/nvenc.h"

#include <cstring>

#include "nvEncodeAPI.h"
#include "util/log.h"

namespace twin {

namespace {

/* NVENCSTATUS values are ints; keep a readable printer for the common ones. */
const char* NvErrorToString(NVENCSTATUS s) {
    switch (s) {
        case NV_ENC_SUCCESS: return "SUCCESS";
        case NV_ENC_ERR_INVALID_VERSION: return "INVALID_VERSION";
        case NV_ENC_ERR_INVALID_PTR: return "INVALID_PTR";
        case NV_ENC_ERR_INVALID_ENCODERDEVICE: return "INVALID_ENCODERDEVICE";
        case NV_ENC_ERR_UNSUPPORTED_DEVICE: return "UNSUPPORTED_DEVICE";
        case NV_ENC_ERR_INVALID_PARAM: return "INVALID_PARAM";
        case NV_ENC_ERR_ENCODER_NOT_INITIALIZED: return "ENCODER_NOT_INITIALIZED";
        case NV_ENC_ERR_NO_ENCODE_DEVICE: return "NO_ENCODE_DEVICE";
        case NV_ENC_ERR_OUT_OF_MEMORY: return "OUT_OF_MEMORY";
        case NV_ENC_ERR_RESOURCE_REGISTER_FAILED: return "RESOURCE_REGISTER_FAILED";
        case NV_ENC_ERR_RESOURCE_NOT_REGISTERED: return "RESOURCE_NOT_REGISTERED";
        case NV_ENC_ERR_MAP_FAILED: return "MAP_FAILED";
        case NV_ENC_ERR_UNIMPLEMENTED: return "UNIMPLEMENTED";
        case NV_ENC_ERR_GENERIC: return "GENERIC";
        default: return "?";
    }
}

}  // namespace

bool NvEncoder::LoadApi() {
    nvenc_dll_ = LoadLibraryA("nvEncodeAPI64.dll");
    if (!nvenc_dll_) {
        Log("NVENC: nvEncodeAPI64.dll not found (NVIDIA driver?)");
        return false;
    }
    auto create = reinterpret_cast<NVENCSTATUS(NVENCAPI*)(void**)>(
        GetProcAddress(static_cast<HMODULE>(nvenc_dll_), "NvEncodeAPICreateInstance"));
    if (!create) {
        Log("NVENC: NvEncodeAPICreateInstance missing");
        return false;
    }
    void* list = nullptr;
    NVENCSTATUS s = create(&list);
    if (s != NV_ENC_SUCCESS || !list) {
        Log("NVENC: NvEncodeAPICreateInstance failed %d (%s)", s,
            NvErrorToString(s));
        return false;
    }
    api_ = static_cast<NV_ENCODE_API_FUNCTION_LIST*>(list);
    Log("NVENC: API version 0x%08x loaded", api_->version);
    return true;
}

bool NvEncoder::OpenSession(ID3D11Device* device) {
    device_ = device;
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params{};
    params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    params.device = device_.Get();
    params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    params.apiVersion = NVENCAPI_VERSION;
    NVENCSTATUS s = api_->nvEncOpenEncodeSessionEx(&params, &encoder_);
    if (s != NV_ENC_SUCCESS) {
        Log("NVENC: nvEncOpenEncodeSessionEx failed %d (%s)", s,
            NvErrorToString(s));
        return false;
    }
    return true;
}

bool NvEncoder::Configure() {
    NV_ENC_INITIALIZE_PARAMS params{};
    params.version = NV_ENC_INITIALIZE_PARAMS_VER;
    params.encodeGUID = NV_ENC_CODEC_H264_GUID;
    params.presetGUID = NV_ENC_PRESET_P3_GUID;
    params.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;
    params.encodeWidth = w_;
    params.encodeHeight = h_;
    params.darWidth = w_;
    params.darHeight = h_;
    params.frameRateNum = fps_;
    params.frameRateDen = 1;
    params.enableEncodeAsync = 0;
    params.enablePTD = 1;

    config_.version = NV_ENC_CONFIG_VER;
    config_.profileGUID = NV_ENC_H264_PROFILE_MAIN_GUID;
    config_.gopLength = fps_ * 2;   /* IDR every 2 s */
    config_.frameIntervalP = 1;     /* no B-frames */
    params.encodeConfig = &config_;

    config_.rcParams.version = NV_ENC_RC_PARAMS_VER;
    config_.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    config_.rcParams.averageBitRate = bitrate_;
    config_.rcParams.maxBitRate = bitrate_ * 12 / 10;
    config_.rcParams.vbvBufferSize = bitrate_ / fps_;
    config_.rcParams.vbvInitialDelay = config_.rcParams.vbvBufferSize;
    config_.rcParams.enableLookahead = 0;
    config_.rcParams.enableTemporalAQ = 1;

    config_.encodeCodecConfig.h264Config.disableSPSPPS = 0;
    config_.encodeCodecConfig.h264Config.repeatSPSPPS = 1; /* SPS/PPS on every IDR */
    config_.encodeCodecConfig.h264Config.level = NV_ENC_LEVEL_AUTOSELECT;
    config_.encodeCodecConfig.h264Config.idrPeriod = fps_ * 2;

    NVENCSTATUS s = api_->nvEncInitializeEncoder(encoder_, &params);
    if (s != NV_ENC_SUCCESS) {
        Log("NVENC: nvEncInitializeEncoder failed %d (%s)", s,
            NvErrorToString(s));
        return false;
    }
    init_params_ = params;
    configured_ = true;
    Log("NVENC: configured %ux%u @ %u fps, %u bps", w_, h_, fps_, bitrate_);
    return true;
}

bool NvEncoder::RegisterInput(ID3D11Texture2D* frame) {
    NV_ENC_REGISTER_RESOURCE res{};
    res.version = NV_ENC_REGISTER_RESOURCE_VER;
    res.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    res.resourceToRegister = frame;
    res.width = w_;
    res.height = h_;
    res.pitch = 0; /* driver queries pitch from the D3D11 texture */
    res.bufferFormat = NV_ENC_BUFFER_FORMAT_ABGR; /* D3D B8G8R8A8_UNORM */
    res.bufferUsage = NV_ENC_INPUT_IMAGE;
    res.subResourceIndex = 0;
    NVENCSTATUS s = api_->nvEncRegisterResource(encoder_, &res);
    if (s != NV_ENC_SUCCESS) {
        Log("NVENC: nvEncRegisterResource failed %d (%s)", s, NvErrorToString(s));
        return false;
    }
    registered_input_ = res.registeredResource;
    return true;
}

bool NvEncoder::CreateBitstream() {
    NV_ENC_CREATE_BITSTREAM_BUFFER b{};
    b.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    /* Keyframes can exceed the per-frame budget; size generously. */
    b.size = bitrate_ / fps_ * 4;
    if (b.size < w_ * h_ / 2) b.size = w_ * h_ / 2;
    b.memoryHeap = NV_ENC_MEMORY_HEAP_AUTOSELECT;
    NVENCSTATUS s = api_->nvEncCreateBitstreamBuffer(encoder_, &b);
    if (s != NV_ENC_SUCCESS) {
        Log("NVENC: nvEncCreateBitstreamBuffer failed %d (%s)", s,
            NvErrorToString(s));
        return false;
    }
    bitstream_buffer_ = b.bitstreamBuffer;
    return true;
}

bool NvEncoder::Start(uint32_t w, uint32_t h, uint32_t fps, uint32_t bitrate) {
    w_ = w;
    h_ = h;
    fps_ = fps;
    bitrate_ = bitrate;
    keyframe_requested_ = true;
    return LoadApi();
}

bool NvEncoder::Encode(ID3D11Texture2D* frame, EncodedFrame& out) {
    if (!api_) return false;

    if (!device_) {
        frame->GetDevice(&device_);
        if (!OpenSession(device_.Get())) return false;
    }
    if (!configured_ && !Configure()) return false;
    if (!registered_input_ && !RegisterInput(frame)) return false;
    if (!bitstream_buffer_ && !CreateBitstream()) return false;

    void* mapped = nullptr;
    NV_ENC_MAP_INPUT_RESOURCE map{};
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = registered_input_;
    NVENCSTATUS s = api_->nvEncMapInputResource(encoder_, &map);
    if (s != NV_ENC_SUCCESS) {
        Log("NVENC: nvEncMapInputResource failed %d (%s)", s, NvErrorToString(s));
        return false;
    }
    mapped = map.mappedResource;

    NV_ENC_PIC_PARAMS pp{};
    pp.version = NV_ENC_PIC_PARAMS_VER;
    pp.inputBuffer = mapped;
    pp.inputWidth = w_;
    pp.inputHeight = h_;
    pp.inputPitch = w_ * 4;
    pp.bufferFmt = NV_ENC_BUFFER_FORMAT_ABGR;
    pp.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pp.outputBitstream = bitstream_buffer_;
    pp.frameIdx = frame_index_;
    pp.inputDuration = 10'000'000u / fps_; /* 100 ns units */
    pp.inputTimeStamp = frame_index_ * pp.inputDuration;
    ++frame_index_;
    if (keyframe_requested_) {
        pp.encodePicFlags |= NV_ENC_PIC_FLAG_FORCEIDR;
        keyframe_requested_ = false;
    }

    s = api_->nvEncEncodePicture(encoder_, &pp);
    if (s != NV_ENC_SUCCESS) {
        Log("NVENC: nvEncEncodePicture failed %d (%s)", s, NvErrorToString(s));
        api_->nvEncUnmapInputResource(encoder_, mapped);
        return false;
    }

    /* Sync mode: lock blocks until this frame's output is ready. */
    NV_ENC_LOCK_BITSTREAM bs{};
    bs.version = NV_ENC_LOCK_BITSTREAM_VER;
    bs.outputBitstream = bitstream_buffer_;
    s = api_->nvEncLockBitstream(encoder_, &bs);
    if (s != NV_ENC_SUCCESS) {
        Log("NVENC: nvEncLockBitstream failed %d (%s)", s, NvErrorToString(s));
        api_->nvEncUnmapInputResource(encoder_, mapped);
        return false;
    }

    out.keyframe = (bs.pictureType == NV_ENC_PIC_TYPE_IDR);
    out.codec = 0;
    out.nals.clear();
    out.raw.clear();

    /* Parse NVENC's length-prefixed NAL container:
     *   repeated { uint32 LE length, payload }  (+ optional trailing count) */
    const uint8_t* data = static_cast<const uint8_t*>(bs.bitstreamBufferPtr);
    const uint32_t size = bs.bitstreamSizeInBytes;
    uint32_t pos = 0;
    while (size - pos >= 4) {
        uint32_t len = static_cast<uint32_t>(data[pos]) |
                       (static_cast<uint32_t>(data[pos + 1]) << 8) |
                       (static_cast<uint32_t>(data[pos + 2]) << 16) |
                       (static_cast<uint32_t>(data[pos + 3]) << 24);
        pos += 4;
        if (len == 0 || pos + len > size) break; /* guard trailing count quirk */
        out.nals.emplace_back(data + pos, data + pos + len);
        pos += len;
    }

    api_->nvEncUnlockBitstream(encoder_, bitstream_buffer_);
    api_->nvEncUnmapInputResource(encoder_, mapped);
    return !out.nals.empty();
}

bool NvEncoder::SetBitrate(uint32_t bitrate) {
    if (!configured_ || !api_) return false;
    bitrate_ = bitrate;
    NV_ENC_RECONFIGURE_PARAMS rp{};
    rp.version = NV_ENC_RECONFIGURE_PARAMS_VER;
    rp.forceIDR = 1;
    rp.reInitEncodeParams = init_params_;
    rp.reInitEncodeParams.encodeConfig->rcParams.averageBitRate = bitrate;
    rp.reInitEncodeParams.encodeConfig->rcParams.maxBitRate = bitrate * 12 / 10;
    rp.reInitEncodeParams.encodeConfig->rcParams.vbvBufferSize = bitrate / fps_;
    rp.reInitEncodeParams.encodeConfig->rcParams.vbvInitialDelay = bitrate / fps_;
    NVENCSTATUS s = api_->nvEncReconfigureEncoder(encoder_, &rp);
    if (s != NV_ENC_SUCCESS) {
        Log("NVENC: nvEncReconfigureEncoder failed %d (%s)", s, NvErrorToString(s));
        return false;
    }
    return true;
}

bool NvEncoder::Resize(uint32_t w, uint32_t h) {
    if (!configured_)
        return false;
    Stop();
    const uint32_t fps = fps_;
    const uint32_t bitrate = bitrate_;
    return Start(w, h, fps, bitrate);
}

void NvEncoder::Stop() {
    if (!api_) return;
    if (registered_input_) {
        api_->nvEncUnregisterResource(encoder_, registered_input_);
        registered_input_ = nullptr;
    }
    if (bitstream_buffer_) {
        api_->nvEncDestroyBitstreamBuffer(encoder_, bitstream_buffer_);
        bitstream_buffer_ = nullptr;
    }
    if (encoder_) {
        api_->nvEncDestroyEncoder(encoder_);
        encoder_ = nullptr;
    }
    api_ = nullptr;
    if (nvenc_dll_) {
        FreeLibrary(static_cast<HMODULE>(nvenc_dll_));
        nvenc_dll_ = nullptr;
    }
    configured_ = false;
}

}  // namespace twin
