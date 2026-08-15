#include <windows.h>
#include <winsock2.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>

#include "protocol.h"
#include "audio/aac.h"
#include "audio/capture.h"
#include "capture/capture.h"
#include "encode/encoder.h"
#include "encode/nvenc.h"
#include "input/inject.h"
#include "net/server.h"
#include "util/log.h"
#include "util/scaler.h"
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
std::vector<uint8_t> BuildVideoPayload(const twin::EncodedFrame& f) {
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
void AddHw(std::vector<twin::Encoder*>& out, twin::Encoder& enc, const Args& args) {
    if (args.keep_raw)
        return;
    if (!args.encoder.empty() && args.encoder != enc.Name())
        return;
    out.push_back(&enc);
}

/* Keeps SP_CTL_SET_BITRATE requests within what the encoders accept. */
uint32_t ClampBitrate(uint32_t b) {
    constexpr uint32_t kMin = 250'000;     /* 0.25 Mbps */
    constexpr uint32_t kMax = 40'000'000;  /* 40 Mbps */
    return b < kMin ? kMin : (b > kMax ? kMax : b);
}

constexpr uint32_t kAudioRate = 48000;
constexpr uint16_t kAudioChannels = 2;
constexpr uint32_t kAudioBitrate = 128'000;

/* Adaptive resolution. Congestion is detected as the smoothed time a video
 * frame spends in SendFrame (TCP backpressure when the tablet can't drain);
 * the encode size steps down to cut bits per frame, and back up when clear.
 * Scaling happens on the GPU (util/scaler.h) before the encoder sees the
 * frame, so every encoder path is covered. */
constexpr double kScaleSteps[] = {1.0, 0.75, 0.5, 0.375};
constexpr int kScaleCount =
    static_cast<int>(sizeof(kScaleSteps) / sizeof(kScaleSteps[0]));
constexpr double kSendUpMs = 18.0;   /* avg send time above this = congested */
constexpr double kSendDownMs = 6.0;  /* below this = clear */
constexpr int kSendUpFrames = 45;    /* ~1.5 s of congested video frames */
constexpr int kSendDownFrames = 150; /* ~5 s of clear video frames */

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

    /* ---- adaptive resolution (GPU downscale under congestion) ---- */
    twin::GpuScaler scaler;
    const bool adaptive = scaler.Init(cap.Device());
    int scale_idx = 0;             /* index into kScaleSteps (0 = native) */
    uint32_t cur_w = cap.Width();  /* size the encoder is configured for */
    uint32_t cur_h = cap.Height();
    double avg_send_ms = 0.0;
    int send_up_streak = 0, send_down_streak = 0;
    if (!adaptive)
        twin::Log("adaptive resolution disabled (scaler init failed)");
    else
        twin::Log("adaptive resolution on (%ux%u native)", cur_w, cur_h);

    /* ---- audio: WASAPI loopback -> AAC ADTS ---- */
    twin::AudioCapture audio_cap;
    twin::AacEncoder aac;
    bool audio_on = false;
    if (audio_cap.Start(kAudioRate) && aac.Init(kAudioRate, kAudioChannels, kAudioBitrate)) {
        audio_on = true;
        twin::Log("audio pipeline on (%u Hz %u ch, AAC %u bps)",
                  kAudioRate, kAudioChannels, kAudioBitrate);
    } else {
        twin::Log("audio unavailable; running without sound");
    }

    /* ---- main loop ---- */
    bool keyframe_pending = true;
    while (true) {
        ID3D11Texture2D* tex = nullptr;
        if (cap.Capture(&tex)) {
            /* Apply the current scale step: encode a GPU-downscaled copy so a
             * smaller resolution carries the same screen content cheaper. */
            ID3D11Texture2D* enc_tex = tex;
            if (adaptive && scale_idx > 0) {
                const double f = kScaleSteps[scale_idx];
                const uint32_t nw = static_cast<uint32_t>(cap.Width() * f) & ~1u;
                const uint32_t nh = static_cast<uint32_t>(cap.Height() * f) & ~1u;
                if (nw != cur_w || nh != cur_h) {
                    if (enc->Resize(nw, nh)) {
                        cur_w = nw;
                        cur_h = nh;
                        keyframe_pending = true; /* tablet must resync to new SPS */
                        twin::Log("adaptive: scaling to %ux%u (%s)", cur_w, cur_h,
                                  enc->Name());
                    } else {
                        twin::Log("adaptive: %s cannot resize; staying native",
                                  enc->Name());
                        scale_idx = 0;
                        cur_w = cap.Width();
                        cur_h = cap.Height();
                    }
                }
                enc_tex = scaler.Scale(tex, cur_w, cur_h);
                if (!enc_tex)
                    enc_tex = tex; /* fall back to native-size encode */
            } else if (adaptive && scale_idx == 0 &&
                       (cur_w != cap.Width() || cur_h != cap.Height())) {
                /* Back to native: re-initialize at the original size. */
                if (enc->Resize(cap.Width(), cap.Height())) {
                    cur_w = cap.Width();
                    cur_h = cap.Height();
                    keyframe_pending = true;
                    twin::Log("adaptive: scaling back to %ux%u", cur_w, cur_h);
                }
            }

            twin::EncodedFrame ef;
            const bool enc_ok = enc->Encode(enc_tex, ef);
            if (enc_ok) {
                if (!ef.nals.empty() || args.keep_raw) {
                    ef.keyframe = keyframe_pending || ef.keyframe;
                    keyframe_pending = false;

                    std::vector<uint8_t> body = BuildVideoPayload(ef);
                    sp_video_frame vf{};
                    vf.keyframe = ef.keyframe ? 1 : 0;
                    vf.codec = static_cast<uint8_t>(ef.codec);
                    vf.display_w = cur_w;
                    vf.display_h = cur_h;

                    std::vector<uint8_t> frame;
                    frame.reserve(sizeof(vf) + body.size());
                    frame.insert(frame.end(), reinterpret_cast<uint8_t*>(&vf),
                                 reinterpret_cast<uint8_t*>(&vf) + sizeof(vf));
                    frame.insert(frame.end(), body.begin(), body.end());

                    /* Congestion signal: how long the frame sat in the socket.
                     * High while the tablet can't drain; low when it keeps up. */
                    const auto send_t0 = std::chrono::steady_clock::now();
                    const bool sent = srv.SendFrame(SP_MSG_VIDEO_FRAME, frame.data(),
                                                    static_cast<uint32_t>(frame.size()));
                    const double send_ms =
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - send_t0)
                            .count();
                    if (!sent) {
                        twin::Log("send failed; client gone");
                        break;
                    }

                    if (adaptive) {
                        avg_send_ms = avg_send_ms == 0.0
                                          ? send_ms
                                          : avg_send_ms * 0.9 + send_ms * 0.1;
                        if (avg_send_ms > kSendUpMs) {
                            ++send_up_streak;
                            send_down_streak = 0;
                        } else if (avg_send_ms < kSendDownMs) {
                            ++send_down_streak;
                            send_up_streak = 0;
                        } else {
                            send_up_streak = 0;
                            send_down_streak = 0;
                        }
                        if (send_up_streak >= kSendUpFrames && scale_idx < kScaleCount - 1) {
                            ++scale_idx;
                            send_up_streak = 0;
                            twin::Log("adaptive: congested (avg send %.1f ms); "
                                      "scale %.2f -> %.2f",
                                      avg_send_ms, kScaleSteps[scale_idx - 1],
                                      kScaleSteps[scale_idx]);
                        } else if (send_down_streak >= kSendDownFrames &&
                                   scale_idx > 0) {
                            --scale_idx;
                            send_down_streak = 0;
                            twin::Log("adaptive: clear (avg send %.1f ms); "
                                      "scale %.2f -> %.2f",
                                      avg_send_ms, kScaleSteps[scale_idx + 1],
                                      kScaleSteps[scale_idx]);
                        }
                    }
                }
            }
        }

        /* Drain captured audio and stream it as AAC ADTS frames. */
        if (audio_on) {
            std::vector<int16_t> pcm;
            std::vector<uint8_t> aac_frame;
            while (audio_cap.PopFrame(pcm)) {
                aac_frame.clear();
                if (aac.Encode(pcm.data(), static_cast<uint32_t>(pcm.size() / 2), aac_frame)) {
                    sp_audio_frame af{};
                    af.sample_rate = kAudioRate;
                    af.codec = 1; /* AAC ADTS */
                    af.channels = kAudioChannels;
                    af.bytes_per_sample = 0;

                    std::vector<uint8_t> frame;
                    frame.reserve(sizeof(af) + aac_frame.size());
                    frame.insert(frame.end(), reinterpret_cast<uint8_t*>(&af),
                                 reinterpret_cast<uint8_t*>(&af) + sizeof(af));
                    frame.insert(frame.end(), aac_frame.begin(), aac_frame.end());
                    if (!srv.SendFrame(SP_MSG_AUDIO_FRAME, frame.data(),
                                       static_cast<uint32_t>(frame.size()))) {
                        twin::Log("audio send failed; client gone");
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
                if (payload.size() >= sizeof(sp_control)) {
                    const auto* ctl =
                        reinterpret_cast<const sp_control*>(payload.data());
                    if (ctl->type == SP_CTL_SET_BITRATE) {
                        const uint32_t bitrate = ClampBitrate(ctl->value);
                        if (enc->SetBitrate(bitrate))
                            twin::Log("bitrate set to %u bps", bitrate);
                        else
                            twin::Log("bitrate control unsupported on %s",
                                      enc->Name());
                    }
                }
            } else if (type == SP_MSG_ERROR) {
                twin::Log("tablet error frame");
            }
        }

        if (!srv.HasClient()) break;
        Sleep(2);
    }

    twin::Log("shutting down");
    audio_cap.Stop();
    aac.Shutdown();
    return 0;
}
