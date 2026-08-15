package com.twinscreen.tablet

import java.nio.ByteBuffer
import java.nio.ByteOrder

/** Mirrors proto/protocol.h. Keep in sync. */
object Protocol {
    const val MAGIC = 0x21325053
    const val VERSION = 1
    const val PORT = 7200

    const val CAP_PEN = 0x00000001
    const val CAP_HEVC = 0x00000002
    const val CAP_TOUCH = 0x00000004
    const val CAP_MULTITOUCH = 0x00000008

    const val CODEC_H264 = 0
    const val CODEC_HEVC = 1

    const val MSG_HANDSHAKE = 1
    const val MSG_HANDSHAKE_ACK = 2
    const val MSG_VIDEO_FRAME = 3
    const val MSG_KEYFRAME_REQ = 4
    const val MSG_INPUT_BATCH = 5
    const val MSG_CONTROL = 6
    const val MSG_AUDIO_FRAME = 7
    const val MSG_ERROR = 8

    const val IN_MOVE = 0
    const val IN_DOWN = 1
    const val IN_UP = 2
    const val IN_STYLUS_DOWN = 3
    const val IN_STYLUS_MOVE = 4
    const val IN_STYLUS_UP = 5
    const val IN_CANCEL = 6

    const val HEADER_SIZE = 24

    data class Ack(
        val protoVersion: Int,
        val caps: Long,
        val modeW: Int,
        val modeH: Int,
        val modeRefresh: Int,
        val codec: Int,
        val profile: Int,
        val bitrate: Long,
        val fps: Int,
    )

    data class VideoFrame(
        val keyframe: Boolean,
        val codec: Int,
        val displayW: Int,
        val displayH: Int,
        val nals: List<ByteArray>,
    )

    data class InputEvent(
        val x: Int,
        val y: Int,
        val type: Int,
        val buttons: Int,
        val pointerId: Int,
        val pressure: Int,
        val tiltX: Int,
        val tiltY: Int,
    )

    private fun buf(n: Int): ByteBuffer =
        ByteBuffer.allocate(n).order(ByteOrder.LITTLE_ENDIAN)

    fun packHeader(type: Int, payloadLen: Int, seq: Long = 0, tsUs: Long = 0): ByteBuffer {
        val b = buf(HEADER_SIZE)
        b.putInt(MAGIC)
        b.putShort(VERSION.toShort())
        b.putShort(type.toShort())
        b.putInt(seq.toInt())
        b.putLong(tsUs)
        b.putInt(payloadLen)
        b.flip()
        return b
    }

    const val HANDSHAKE_SIZE = 42 // u16 + u32 + u16 + u16 + u8[32]

    fun packHandshake(
        caps: Int,
        screenW: Int,
        screenH: Int,
        deviceName: String,
    ): ByteArray {
        val payload = buf(HANDSHAKE_SIZE)
        payload.putShort(VERSION.toShort())
        payload.putInt(caps)
        payload.putShort(screenW.toShort())
        payload.putShort(screenH.toShort())
        val name = deviceName.take(31).toByteArray(Charsets.UTF_8)
        payload.put(name)
        while (payload.position() < HANDSHAKE_SIZE) payload.put(0)
        payload.flip()
        return payload.array()
    }

    fun packInputBatch(events: List<InputEvent>): ByteArray {
        val payload = buf(4 + events.size * 12)
        payload.putInt(events.size)
        for (e in events) {
            payload.putShort(e.x.toShort())
            payload.putShort(e.y.toShort())
            payload.put(e.type.toByte())
            payload.put(e.buttons.toByte())
            payload.put(e.pointerId.toByte())
            payload.put(e.pressure.toByte())
            payload.putShort(e.tiltX.toShort())
            payload.putShort(e.tiltY.toShort())
        }
        payload.flip()
        return payload.array()
    }

    fun readAck(data: ByteArray): Ack {
        val b = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
        return Ack(
            protoVersion = b.short.toInt() and 0xFFFF,
            caps = b.int.toLong() and 0xFFFFFFFFL,
            modeW = b.short.toInt() and 0xFFFF,
            modeH = b.short.toInt() and 0xFFFF,
            modeRefresh = b.short.toInt() and 0xFFFF,
            codec = b.get().toInt(),
            profile = b.get().toInt(),
            bitrate = b.int.toLong() and 0xFFFFFFFFL,
            fps = b.short.toInt() and 0xFFFF,
        )
    }

    fun parseVideoFrame(payload: ByteArray): VideoFrame {
        val b = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)
        val keyframe = b.get().toInt() != 0
        val codec = b.get().toInt()
        b.short // reserved
        val dw = b.int
        val dh = b.int
        val nals = ArrayList<ByteArray>()
        val count = b.int
        for (i in 0 until count) {
            val len = b.int
            val nal = ByteArray(len)
            b.get(nal)
            nals.add(nal)
        }
        return VideoFrame(keyframe, codec, dw, dh, nals)
    }
}
