package com.twinscreen.tablet

import android.os.Build
import android.util.Log
import java.io.DataInputStream
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
 * - Reconnects automatically with exponential backoff until [disconnect] is
 *   called; [onReconnecting] reports each retry attempt.
 * - Tracks receive-side stats ([stats]) for the on-screen overlay.
 */
class Client(
    val host: String,
    val port: Int,
    private val onConnected: (Protocol.Ack) -> Unit,
    private val onReconnecting: (attempt: Int) -> Unit,
    private val onDisconnected: () -> Unit,
) : Thread("twin-client") {

    companion object {
        private const val TAG = "TwinClient"
        private const val CONNECT_TIMEOUT_MS = 5000
        private const val MAX_QUEUED_FRAMES = 4
        private const val MAX_QUEUED_AUDIO_FRAMES = 80
        private const val RECONNECT_BASE_MS = 1000L
        private const val RECONNECT_MAX_MS = 8000L
        private const val STATS_WINDOW_MS = 1000L
    }

    private val socketLock = Object()
    private var socket: Socket? = null
    private var out: java.io.OutputStream? = null
    private var seq = 0L
    private var connected = AtomicBoolean(false)
    private val stopRequested = AtomicBoolean(false)

    val frames = ConcurrentLinkedQueue<Protocol.VideoFrame>()
    val audioFrames = ConcurrentLinkedQueue<Protocol.AudioFrame>()
    @Volatile
    var ack: Protocol.Ack? = null
        private set

    // ---- receive stats (updated on the client thread) ----
    @Volatile
    var recvFps = 0f
        private set
    @Volatile
    var recvMbps = 0f
        private set
    @Volatile
    var queueDepth = 0
        private set
    @Volatile
    var reconnectActive = false
        private set
    @Volatile
    var reconnectAttempt = 0
        private set

    private var windowStartMs = 0L
    private var windowFrames = 0
    private var windowBytes = 0L

    data class Stats(val fps: Float, val mbps: Float, val queueDepth: Int)

    fun stats(): Stats = Stats(recvFps, recvMbps, queueDepth)

    fun isConnected(): Boolean = connected.get()

    fun nextFrame(): Protocol.VideoFrame? = frames.poll()

    fun nextAudioFrame(): Protocol.AudioFrame? = audioFrames.poll()

    /** Stops the client for good; the reconnect loop exits and [onDisconnected] fires. */
    fun disconnect() {
        stopRequested.set(true)
        connected.set(false)
        synchronized(socketLock) {
            try {
                socket?.close()
            } catch (_: IOException) {
            }
            socket = null
        }
        interrupt() // wake up from reconnect backoff sleep
    }

    override fun run() {
        while (!stopRequested.get()) {
            val input = tryConnect()
            if (input != null) {
                // Connected: block in the receive loop until the link drops.
                reconnectActive = false
                reconnectAttempt = 0
                requestKeyframe()
                receiveLoop(input)
                connected.set(false)
                synchronized(socketLock) {
                    try {
                        socket?.close()
                    } catch (_: IOException) {
                    }
                    socket = null
                    out = null
                }
                frames.clear()
                audioFrames.clear()
                queueDepth = 0
            }
            if (stopRequested.get()) break
            scheduleReconnect()
        }
        onDisconnected()
    }

    private fun tryConnect(): DataInputStream? {
        val sock = Socket()
        try {
            sock.connect(InetSocketAddress(host, port), CONNECT_TIMEOUT_MS)
            if (stopRequested.get()) {
                sock.close()
                return null
            }
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
                throw IOException("protocol version mismatch")
            }
            this.ack = ack
            connected.set(true)
            onConnected(ack)
            return input
        } catch (e: Exception) {
            Log.i(TAG, "connect failed: $e")
            connected.set(false)
            try {
                sock.close()
            } catch (_: IOException) {
            }
            synchronized(socketLock) {
                if (socket === sock) {
                    socket = null
                    out = null
                }
            }
            return null
        }
    }

    private fun scheduleReconnect() {
        reconnectActive = true
        val delayMs = (RECONNECT_BASE_MS shl reconnectAttempt.coerceAtMost(3))
            .coerceAtMost(RECONNECT_MAX_MS)
        reconnectAttempt++
        Log.w(TAG, "connection lost; retrying in ${delayMs}ms (attempt $reconnectAttempt)")
        onReconnecting(reconnectAttempt)
        try {
            Thread.sleep(delayMs)
        } catch (_: InterruptedException) {
            stopRequested.set(true)
        }
    }

    private fun receiveLoop(input: DataInputStream) {
        val header = ByteArray(Protocol.HEADER_SIZE)
        windowStartMs = System.currentTimeMillis()
        while (connected.get() && !stopRequested.get()) {
            try {
                input.readFully(header)
            } catch (_: IOException) {
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
            try {
                input.readFully(payload)
            } catch (_: IOException) {
                return
            }
            when (type) {
                Protocol.MSG_VIDEO_FRAME -> {
                    val f = Protocol.parseVideoFrame(payload)
                    while (frames.size >= MAX_QUEUED_FRAMES) frames.poll()
                    frames.add(f)
                    queueDepth = frames.size
                    accumulateStats(payload.size)
                }
                Protocol.MSG_AUDIO_FRAME -> {
                    val f = Protocol.parseAudioFrame(payload)
                    while (audioFrames.size >= MAX_QUEUED_AUDIO_FRAMES) audioFrames.poll()
                    audioFrames.add(f)
                }
                Protocol.MSG_CONTROL -> { /* bitrate/stats updates; ignore for now */ }
                Protocol.MSG_ERROR -> Log.w(TAG, "host error frame")
            }
        }
    }

    private fun accumulateStats(bytes: Int) {
        windowFrames++
        windowBytes += bytes.toLong()
        val now = System.currentTimeMillis()
        if (now - windowStartMs >= STATS_WINDOW_MS) {
            val secs = (now - windowStartMs) / 1000f
            if (secs > 0f) {
                recvFps = windowFrames / secs
                recvMbps = windowBytes * 8f / (secs * 1_000_000f)
            }
            windowFrames = 0
            windowBytes = 0
            windowStartMs = now
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
