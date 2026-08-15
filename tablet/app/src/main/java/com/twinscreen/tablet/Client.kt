package com.twinscreen.tablet

import android.os.Build
import android.util.Log
import java.io.DataInputStream
import java.io.EOFException
import java.io.IOException
import java.net.InetSocketAddress
import java.net.Socket
import java.nio.ByteBuffer
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.atomic.AtomicBoolean

/**
 * TCP client speaking the twin-screen protocol.
 *
 * - Sends the handshake on connect, then streams input batches from the UI thread.
 * - Receive loop decodes frames into [frames] (bounded to the newest N).
 * - [requestKeyframe] asks the host for an IDR whenever the decoder needs one.
 */
class Client(
    private val host: String,
    private val port: Int,
    private val onConnected: (Protocol.Ack) -> Unit,
    private val onDisconnected: () -> Unit,
) : Thread("twin-client") {

    companion object {
        private const val TAG = "TwinClient"
        private const val CONNECT_TIMEOUT_MS = 5000
        private const val MAX_QUEUED_FRAMES = 4
    }

    private val socketLock = Object()
    private var socket: Socket? = null
    private var out: java.io.OutputStream? = null
    private var seq = 0L
    private var connected = AtomicBoolean(false)

    val frames = ConcurrentLinkedQueue<Protocol.VideoFrame>()
    @Volatile
    var ack: Protocol.Ack? = null
        private set

    fun isConnected(): Boolean = connected.get()

    fun nextFrame(): Protocol.VideoFrame? = frames.poll()

    fun disconnect() {
        connected.set(false)
        synchronized(socketLock) {
            try {
                socket?.close()
            } catch (_: IOException) {
            }
            socket = null
        }
    }

    override fun run() {
        val sock = Socket()
        try {
            sock.connect(InetSocketAddress(host, port), CONNECT_TIMEOUT_MS)
            sock.tcpNoDelay = true
            sock.soTimeout = 0
            synchronized(socketLock) {
                socket = sock
                out = sock.getOutputStream()
            }

            val caps = Protocol.CAP_TOUCH or Protocol.CAP_MULTITOUCH
            val dm = android.content.res.Resources.getSystem().displayMetrics
            sendRaw(
                Protocol.packHeader(Protocol.MSG_HANDSHAKE, Protocol.HANDSHAKE_SIZE),
                Protocol.packHandshake(caps, dm.widthPixels, dm.heightPixels, Build.MODEL),
            )

            val input = DataInputStream(sock.getInputStream())
            val ack = readAck(input)
            if (ack.protoVersion != Protocol.VERSION) {
                Log.e(TAG, "version mismatch: host ${ack.protoVersion}")
                onDisconnected()
                return
            }
            this.ack = ack
            connected.set(true)
            onConnected(ack)
            requestKeyframe()
            receiveLoop(input)
        } catch (e: Exception) {
            Log.i(TAG, "connection ended: $e")
        } finally {
            connected.set(false)
            try {
                sock.close()
            } catch (_: IOException) {
            }
            onDisconnected()
        }
    }

    private fun receiveLoop(input: DataInputStream) {
        val header = ByteArray(Protocol.HEADER_SIZE)
        while (connected.get()) {
            try {
                input.readFully(header)
            } catch (e: EOFException) {
                return
            }
            val b = ByteBuffer.wrap(header).order(java.nio.ByteOrder.LITTLE_ENDIAN)
            val magic = b.int
            b.short // version
            val type = b.short.toInt() and 0xFFFF
            b.int // seq
            b.long // ts_us
            val len = b.int

            if (magic != Protocol.MAGIC) {
                Log.e(TAG, "bad magic 0x${magic.toString(16)}")
                return
            }

            val payload = ByteArray(len)
            input.readFully(payload)
            when (type) {
                Protocol.MSG_VIDEO_FRAME -> {
                    val f = Protocol.parseVideoFrame(payload)
                    while (frames.size >= MAX_QUEUED_FRAMES) frames.poll()
                    frames.add(f)
                }
                Protocol.MSG_CONTROL -> { /* bitrate/stats updates; ignore for now */ }
                Protocol.MSG_ERROR -> Log.w(TAG, "host error frame")
            }
        }
    }

    private fun readAck(input: DataInputStream): Protocol.Ack {
        val header = ByteArray(Protocol.HEADER_SIZE)
        input.readFully(header)
        val b = ByteBuffer.wrap(header).order(java.nio.ByteOrder.LITTLE_ENDIAN)
        if (b.int != Protocol.MAGIC) throw IOException("bad magic in ack")
        b.short // version
        val type = b.short.toInt() and 0xFFFF
        b.int // seq
        b.long // ts_us
        val len = b.int
        if (type != Protocol.MSG_HANDSHAKE_ACK) throw IOException("expected handshake ack, got $type")
        val payload = ByteArray(len)
        input.readFully(payload)
        return Protocol.readAck(payload)
    }

    fun requestKeyframe() {
        sendRaw(Protocol.packHeader(Protocol.MSG_KEYFRAME_REQ, 0), ByteArray(0))
    }

    fun sendInputBatch(events: List<Protocol.InputEvent>) {
        if (!connected.get() || events.isEmpty()) return
        sendRaw(
            Protocol.packHeader(Protocol.MSG_INPUT_BATCH, 4 + events.size * 12),
            Protocol.packInputBatch(events),
        )
    }

    private fun sendRaw(header: ByteBuffer, payload: ByteArray) {
        val os = synchronized(socketLock) { out } ?: return
        try {
            synchronized(os) {
                val body = header
                while (body.hasRemaining()) os.write(body.get().toInt())
                os.write(payload)
                os.flush()
            }
        } catch (e: IOException) {
            Log.w(TAG, "send failed: $e")
            disconnect()
        }
    }
}
