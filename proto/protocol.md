# twin-screen wire protocol v1

Default port: **7200**. Transport: TCP, **Nagle disabled** on both ends
(`TCP_NODELAY`). Endianness: little-endian.

## Framing

Every message is a 24-byte header followed by an optional payload:

```
offset  size  field
0       4     magic = 0x21325053   ("S","P","2","!")
4       2     version = 1
6       2     type
8       4     seq (sender monotonic)
12      8     ts_us (microseconds since connect)
20      4     payload_len
24      ...   payload (payload_len bytes)
```

## Message types

| type | direction        | payload                                      |
|------|------------------|----------------------------------------------|
| 1    | tablet → host    | `sp_handshake`                               |
| 2    | host → tablet    | `sp_handshake_ack`                           |
| 3    | host → tablet    | `sp_video_frame` + NAL units                 |
| 4    | tablet → host    | none                                         |
| 5    | tablet → host    | `sp_input_batch` + events                    |
| 6    | both             | `sp_control`                                 |
| 7    | host → tablet    | `sp_audio_frame` (phase 2)                   |
| 8    | both             | `sp_error`                                   |

## Handshake sequence

1. Tablet connects, sends `SP_MSG_HANDSHAKE` (`sp_handshake`):
   caps (pen/HEVC/touch/multitouch), screen size, model name.
2. Host replies `SP_MSG_HANDSHAKE_ACK` (`sp_handshake_ack`):
   negotiated mode (w/h/refresh), codec, target bitrate, fps.
3. Host streams `SP_MSG_VIDEO_FRAME`. Tablet asks for a keyframe with
   `SP_MSG_KEYFRAME_REQ` whenever it needs one (start, drop, codec error).

## Video frames

`sp_video_frame` is followed by a NAL container:

```
uint32 nals;
for i in 0..nals:
    uint32 len
    uint8  data[len]
```

- **NAL framing**: NAL payloads do **not** include start codes. The host emits
  the raw access-unit payloads (NVENC length prefixes stripped) inside our
  `uint32 len` container; the receiver prepends `00 00 00 01` before feeding
  them to the decoder.
- **H.264**: keyframes carry SPS/PPS + IDR. Host sends keyframes with an
  interval (e.g. 2 s) and on request.
- **Resolution scaling**: host may downscale under congestion; it reports the
  real size in `display_w/display_h` and the tablet scales to fill the screen.

## Input

`sp_input_batch` is followed by `count` × `sp_input_event` (12 bytes each):

```
uint16 x, y        display coordinates (0..mode_w, 0..mode_h)
uint8  type        MOVE / DOWN / UP / STYLUS_DOWN / STYLUS_MOVE / STYLUS_UP / CANCEL
uint8  buttons     bit0 left, bit1 right, bit2 middle
uint8  pointer_id
uint8  pressure    0..255
int16  tilt_x      tenths of a degree (stylus; 0 if unsupported)
int16  tilt_y
```

Rules:
- A DOWN must be followed by an UP or CANCEL for the same pointer_id before a
  new DOWN with that id.
- A stylus contact uses the STYLUS_* types (so the host can route to pen-aware
  injection when available; MVP injects as touch).
- Batch multiple pointers in one `SP_MSG_INPUT_BATCH` to cut overhead.

## Control

`sp_control`:

| type | value meaning            |
|------|--------------------------|
| 0    | (host) force keyframe    |
| 1    | new target bitrate (bps) |
| 2    | toggle stats overlay     |

## Audio

`sp_audio_frame`: sample_rate (u32), codec (u8), channels (u8),
bytes_per_sample (u16), then the audio payload:

| codec | payload                                   | tablet decode path          |
|-------|-------------------------------------------|-----------------------------|
| 0     | raw interleaved s16 PCM                   | `AudioTrack` write-through  |
| 1     | AAC-LC in 7-byte ADTS headers (48 kHz)    | MediaCodec `mp4a-latm` with `KEY_IS_ADTS=1` → `AudioTrack` |

Payloads are 1024-sample frames (AAC one ADTS frame each; PCM one 1024-sample
stereo frame each). The host only captures while the render device is playing,
so silence costs no bandwidth.
