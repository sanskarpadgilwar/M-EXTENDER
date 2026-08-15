#ifndef TWIN_SCREEN_PROTOCOL_H
#define TWIN_SCREEN_PROTOCOL_H

/*
 * twin-screen wire protocol.
 * Shared C contract; the Android side mirrors this in tablet/.../Protocol.kt.
 * All integers little-endian on the wire (x86/x64 native; Java ByteBuffer LE).
 * Keep this file and Protocol.kt in sync.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SP_MAGIC            0x21325053u  /* bytes 'S','P','2','!' */
#define SP_VERSION          1
#define SP_PROTO_PORT       7200

/* Capability flags (used in handshake and ack). */
#define SP_CAP_PEN          0x00000001u
#define SP_CAP_HEVC         0x00000002u
#define SP_CAP_TOUCH        0x00000004u
#define SP_CAP_MULTITOUCH   0x00000008u

/* Video codecs. */
#define SP_CODEC_H264       0
#define SP_CODEC_HEVC       1

/* Message types (sp_header.type). */
#define SP_MSG_HANDSHAKE      1  /* tablet -> host */
#define SP_MSG_HANDSHAKE_ACK  2  /* host -> tablet */
#define SP_MSG_VIDEO_FRAME    3  /* host -> tablet */
#define SP_MSG_KEYFRAME_REQ   4  /* tablet -> host */
#define SP_MSG_INPUT_BATCH    5  /* tablet -> host */
#define SP_MSG_CONTROL        6  /* both */
#define SP_MSG_AUDIO_FRAME    7  /* host -> tablet (phase 2) */
#define SP_MSG_ERROR          8  /* both */

/* Input event types (sp_input_event.type). */
#define SP_INPUT_MOVE         0
#define SP_INPUT_DOWN         1
#define SP_INPUT_UP           2
#define SP_INPUT_STYLUS_DOWN  3
#define SP_INPUT_STYLUS_MOVE  4
#define SP_INPUT_STYLUS_UP    5
#define SP_INPUT_CANCEL       6

/* Input buttons (sp_input_event.buttons). */
#define SP_BTN_LEFT           0x01
#define SP_BTN_RIGHT          0x02
#define SP_BTN_MIDDLE         0x04

/* Control sub-types (sp_control.type). */
#define SP_CTL_KEYFRAME_NOW   0  /* host -> tablet: force a keyframe */
#define SP_CTL_SET_BITRATE    1  /* both: adjust target bitrate */
#define SP_CTL_SHOW_STATS     2  /* host -> tablet: show/hide stats overlay */

#pragma pack(push, 1)

/* 24-byte message header, always present. */
typedef struct sp_header {
    uint32_t magic;        /* SP_MAGIC */
    uint16_t version;      /* SP_VERSION */
    uint16_t type;         /* SP_MSG_* */
    uint32_t seq;          /* sender-side sequence */
    uint64_t ts_us;        /* microseconds since connect */
    uint32_t payload_len;  /* bytes following the header */
} sp_header;

/* SP_MSG_HANDSHAKE payload. */
typedef struct sp_handshake {
    uint16_t proto_version;  /* SP_VERSION */
    uint32_t caps;           /* SP_CAP_* */
    uint16_t screen_w;       /* tablet screen, px */
    uint16_t screen_h;
    uint8_t  name[32];       /* device model, NUL-terminated */
} sp_handshake;

/* SP_MSG_HANDSHAKE_ACK payload. */
typedef struct sp_handshake_ack {
    uint16_t proto_version;  /* SP_VERSION */
    uint32_t caps;           /* SP_CAP_* */
    uint16_t mode_w;         /* negotiated display mode, px */
    uint16_t mode_h;
    uint16_t mode_refresh;   /* Hz */
    uint8_t  codec;          /* SP_CODEC_* */
    uint8_t  profile;        /* 0=baseline,1=main,2=high */
    uint32_t bitrate;        /* target bps */
    uint16_t fps;
} sp_handshake_ack;

/*
 * SP_MSG_VIDEO_FRAME payload. Video data follows in NAL units:
 *     uint32_t nals;
 *     repeated { uint32_t len; uint8_t data[len]; }
 * For H.264 the SPS/PPS must be present in every keyframe.
 */
typedef struct sp_video_frame {
    uint8_t  keyframe;       /* 1 = IDR/IRAP */
    uint8_t  codec;          /* SP_CODEC_* */
    uint16_t reserved;
    uint32_t display_w;      /* content size (resolution scaling) */
    uint32_t display_h;
} sp_video_frame;

/* One input event; 12 bytes. Coordinates in display (mode_w/mode_h) space. */
typedef struct sp_input_event {
    uint16_t x;
    uint16_t y;
    uint8_t  type;           /* SP_INPUT_* */
    uint8_t  buttons;        /* SP_BTN_* */
    uint8_t  pointer_id;
    uint8_t  pressure;       /* 0..255 */
    int16_t  tilt_x;         /* tenths of degree (stylus) */
    int16_t  tilt_y;
} sp_input_event;

/* SP_MSG_INPUT_BATCH payload: one event list atom. */
typedef struct sp_input_batch {
    uint32_t count;
    /* sp_input_event events[count]; */
} sp_input_batch;

/* SP_MSG_CONTROL payload. */
typedef struct sp_control {
    uint8_t  type;           /* SP_CTL_* */
    uint8_t  reserved[3];
    uint32_t value;
} sp_control;

/* SP_MSG_AUDIO_FRAME payload (phase 2). */
typedef struct sp_audio_frame {
    uint32_t sample_rate;
    uint8_t  channels;
    uint8_t  bytes_per_sample;
    uint16_t reserved;
    /* raw PCM samples follow */
} sp_audio_frame;

/* SP_MSG_ERROR payload. */
typedef struct sp_error {
    uint32_t code;
    uint8_t  message[128];
} sp_error;

#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif /* TWIN_SCREEN_PROTOCOL_H */
