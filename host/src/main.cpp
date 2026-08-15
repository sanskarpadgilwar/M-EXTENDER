#include <windows.h>
#include <winsock2.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "protocol.h"
#include "capture/capture.h"
#include "encode/encoder.h"
#include "encode/nvenc.h"
#include "input/inject.h"
#include "net/server.h"
#include "util/log.h"
#ifdef TWIN_HAVE_QSV
#include "encode/qsv.h"
#endif
#ifdef TWIN_HAVE_AMF
#include "encode/amf.h"
#endif

namespace {

struct Args {
    uint16_t port = SP_PROTO_PORT;
    uintptr_t monitor = 0; /* 0 = primary */
    bool keep_raw = false; /* send NullEncoder RGBA frames (test only) */
    std::string encoder;   /* force a specific encoder by Name() */
};

Args ParseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            a.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--monitor") == 0 && i + 1 < argc) {
            a.monitor = static_cast<uintptr_t>(std::strtoull(argv[++i], nullptr, 0));
        } else if (std::strcmp(argv[i], "--encoder") == 0 && i + 1 < argc) {
            a.encoder = argv[++i];
        } else if (std::strcmp(argv[i], "--raw") == 0) {
            a.keep_raw = true;
        }
    }
    return a;
}

HMONITOR ResolveMonitor(uintptr_t idx) {
    if (idx == 0) return MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    HMONITOR found = nullptr;
    struct Ctx {
        uintptr_t idx;
        HMONITOR found;
    } ctx{idx, nullptr};
    auto cb = [](HMONITOR h, HDC, LPRECT, LPARAM lp) -> BOOL {
        Ctx* c = reinterpret_cast<Ctx*>(lp);
        if (c->idx-- == 1) {
            c->found = h;
            return FALSE;
        }
        return TRUE;
    };
    EnumDisplayMonitors(nullptr, nullptr, cb, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

/* Serializes NAL units into a video-frame payload:
 *   uint32 nals; repeated { uint32 len; uint8 data[len]; } */
std::vector<uint8_t> BuildVideoPayload(const EncodedFrame& f) {
    std::vector<uint8_t> out;
    auto append_u32 = [&out](uint32_t v) {
        const uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8),
                              static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)};
        out.insert(out.end(), b, b + 4);
    };
    append_u32(static_cast<uint32_t>(f.nals.size()));
    for (const auto& nal : f.nals) {
        append_u32(static_cast<uint32_t>(nal.size()));
        out.insert(out.end(), nal.begin(), nal.end());
    }
    return out;
}

/* Picks an encoder: hardware first (NVENC, then QSV, then AMF), NullEncoder
 * (raw RGBA) as the CPU fallback. --raw forces the NullEncoder path; --encoder
 * overrides the candidate order. */
template <typename T>
void AddHw(std::vector<T*>& out, T& enc, const Args& args) {
    if (args.keep_raw)
        return;
    if (!args.encoder.empty() && args.encoder != enc.Name())
        return;
    out.push_back(&enc);
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = ParseArgs(argc, argv);

    HMONITOR hmon = ResolveMonitor(args.monitor);
    if (!hmon) {
        twin::Log("could not resolve monitor");
        return 1;
    }

    twin::ScreenCapture cap;
    if (!cap.Start(hmon)) {
        twin::Log("capture start failed");
        return 1;
    }

    /* Hardware encoders first (NVENC, then QSV, then AMF), NullEncoder as CPU
     * fallback (raw RGBA). --raw forces the raw path; --encoder forces one. */
    twin::NvEncoder nvenc;
    twin::NullEncoder nullenc;
#ifdef TWIN_HAVE_QSV
    twin::QsvEncoder qsv;
#endif
#ifdef TWIN_HAVE_AMF
    twin::AmfEncoder amf;
#endif

    std::vector<twin::Encoder*> candidates;
    AddHw(candidates, nvenc, args);
#ifdef TWIN_HAVE_QSV
    AddHw(candidates, qsv, args);
#endif
#ifdef TWIN_HAVE_AMF
    AddHw(candidates, amf, args);
#endif

    twin::Encoder* enc = nullptr;
    for (twin::Encoder* e : candidates) {
        e->SetD3D11Device(cap.Device());
        if (e->Start(cap.Width(), cap.Height(), 30, 12'000'000)) {
            twin::Log("using encoder: %s", e->Name());
            enc = e;
            break;
        }
        twin::Log("encoder %s unavailable", e->Name());
    }
    if (!enc) {
        twin::Log("no hardware encoder; using %s (raw)", nullenc.Name());
        enc = &nullenc;
        if (!enc->Start(cap.Width(), cap.Height(), 30, 12'000'000)) {
            twin::Log("encoder start failed");
            return 1;
        }
    }

    twin::TcpServer srv;
    if (!srv.Start(args.port)) {
        twin::Log("failed to listen on port %u", args.port);
        return 1;
    }
    twin::Log("waiting for tablet on port %u...", args.port);

    if (!srv.WaitForClient(30000)) {
        twin::Log("no client within 30s");
        return 1;
    }

    /* ---- handshake ---- */
    uint16_t type = 0;
    std::vector<uint8_t> payload;
    if (!srv.ReadFrame(type, payload, 10000) || type != SP_MSG_HANDSHAKE ||
        payload.size() < sizeof(sp_handshake)) {
        twin::Log("bad handshake");
        return 1;
    }
    const auto* hs = reinterpret_cast<const sp_handshake*>(payload.data());
    twin::Log("tablet: %s (%ux%u, caps 0x%08x)",
              reinterpret_cast<const char*>(hs->name), hs->screen_w,
              hs->screen_h, hs->caps);

    sp_handshake_ack ack{};
    ack.proto_version = SP_VERSION;
    ack.caps = 0;
    ack.mode_w = static_cast<uint16_t>(cap.Width());
    ack.mode_h = static_cast<uint16_t>(cap.Height());
    ack.mode_refresh = 60;
    ack.codec = SP_CODEC_H264;
    ack.profile = 0;
    ack.bitrate = 12'000'000;
    ack.fps = 30;
    if (!srv.SendFrame(SP_MSG_HANDSHAKE_ACK, &ack, sizeof(ack))) {
        twin::Log("handshake ack failed");
        return 1;
    }
    twin::Log("handshake done: %ux%u h264 @30fps", ack.mode_w, ack.mode_h);

    twin::InputInjector inject;
    inject.Init(cap.OffsetX(), cap.OffsetY(), cap.Width(), cap.Height());

    /* ---- main loop ---- */
    bool keyframe_pending = true;
    while (true) {
        ID3D11Texture2D* tex = nullptr;
        if (cap.Capture(&tex)) {
            twin::EncodedFrame ef;
            if (enc->Encode(tex, ef)) {
                if (!ef.nals.empty() || args.keep_raw) {
                    ef.keyframe = keyframe_pending || ef.keyframe;
                    keyframe_pending = false;

                    std::vector<uint8_t> body = BuildVideoPayload(ef);
                    sp_video_frame vf{};
                    vf.keyframe = ef.keyframe ? 1 : 0;
                    vf.codec = static_cast<uint8_t>(ef.codec);
                    vf.display_w = cap.Width();
                    vf.display_h = cap.Height();

                    std::vector<uint8_t> frame;
                    frame.reserve(sizeof(vf) + body.size());
                    frame.insert(frame.end(), reinterpret_cast<uint8_t*>(&vf),
                                 reinterpret_cast<uint8_t*>(&vf) + sizeof(vf));
                    frame.insert(frame.end(), body.begin(), body.end());
                    if (!srv.SendFrame(SP_MSG_VIDEO_FRAME, frame.data(),
                                       static_cast<uint32_t>(frame.size()))) {
                        twin::Log("send failed; client gone");
                        break;
                    }
                }
            }
        }

        /* Drain inbound control/input without blocking the capture loop. */
        while (srv.ReadFrame(type, payload, 0)) {
            if (type == SP_MSG_INPUT_BATCH && payload.size() >= sizeof(sp_input_batch)) {
                inject.InjectBatch(reinterpret_cast<const sp_input_batch*>(payload.data()));
            } else if (type == SP_MSG_KEYFRAME_REQ) {
                keyframe_pending = true;
                enc->RequestKeyframe();
                twin::Log("keyframe requested by tablet");
            } else if (type == SP_MSG_CONTROL) {
                /* bitrate/preset changes land here */
            } else if (type == SP_MSG_ERROR) {
                twin::Log("tablet error frame");
            }
        }

        if (!srv.HasClient()) break;
        Sleep(2);
    }

    twin::Log("shutting down");
    return 0;
}
