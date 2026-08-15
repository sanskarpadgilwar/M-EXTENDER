package com.twinscreen.tablet

import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Build
import android.util.Log
import android.view.Surface
import kotlin.concurrent.thread

/**
 * Hardware H.264/HEVC decoder feeding [surface] from a [Client]'s frame queue.
 *
 * Two worker threads drive the MediaCodec synchronously: one feeds input
 * buffers from the queue (dropping stale frames), one renders output. On any
 * decode trouble it asks the host for a keyframe via [onKeyframeRequest].
 */
class VideoPlayer(
    private val surface: Surface,
    private val client: Client,
    private val onKeyframeRequest: () -> Unit,
) {
    companion object {
        private const val TAG = "VideoPlayer"
        private const val DEQUEUE_TIMEOUT_US = 20_000L
        private const val MAX_STALE_FRAMES = 4
        private val START_CODE = byteArrayOf(0x00, 0x00, 0x00, 0x01)
    }

    @Volatile
    private var running = false
    private var codec: MediaCodec? = null
    private var configured = false

    private val inputThread = thread(start = false, name = "decode-feed") { feedLoop() }
    private val outputThread = thread(start = false, name = "decode-render") { renderLoop() }

    fun start() {
        if (running) return
        running = true
        inputThread.start()
        outputThread.start()
    }

    fun stop() {
        running = false
        configured = false
        try {
            codec?.stop()
        } catch (_: Exception) {
        }
        try {
            codec?.release()
        } catch (_: Exception) {
        }
        codec = null
    }

    private fun feedLoop() {
        var idleStreak = 0
        while (running) {
            val f = client.nextFrame() ?: run {
                if (++idleStreak == 30) onKeyframeRequest() // no signal after ~1s
                Thread.sleep(2)
                continue
            }
            idleStreak = 0

            if (!configured) {
                if (!configure(f)) {
                    onKeyframeRequest()
                    Thread.sleep(50)
                    continue
                }
            }

            val c = codec ?: continue
            val index = try {
                c.dequeueInputBuffer(DEQUEUE_TIMEOUT_US)
            } catch (e: IllegalStateException) {
                Log.e(TAG, "dequeue input: $e")
                onKeyframeRequest()
                continue
            }
            if (index < 0) {
                // Decoder saturated; drop the newest instead of queueing forever.
                while (client.frames.size > MAX_STALE_FRAMES) client.frames.poll()
                continue
            }

            val buf = c.getInputBuffer(index) ?: continue
            buf.clear()
            var bytes = 0
            // Wire NALs carry no start codes; prepend them for annex-B.
            for (nal in f.nals) {
                buf.put(START_CODE)
                bytes += START_CODE.size
                buf.put(nal)
                bytes += nal.size
            }
            c.queueInputBuffer(
                index, 0, bytes,
                System.nanoTime() / 1000,
                0,
            )
        }
    }

    private fun configure(f: Protocol.VideoFrame): Boolean {
        val mime = if (f.codec == Protocol.CODEC_HEVC) {
            MediaFormat.MIMETYPE_VIDEO_HEVC
        } else {
            MediaFormat.MIMETYPE_VIDEO_AVC
        }
        val fmt = MediaFormat.createVideoFormat(mime, f.displayW, f.displayH)
        if (Build.VERSION.SDK_INT >= 29) {
            fmt.setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
            fmt.setInteger(MediaFormat.KEY_PRIORITY, 0) // realtime
        }
        val c = try {
            MediaCodec.createDecoderByType(mime)
        } catch (e: Exception) {
            Log.e(TAG, "no decoder for $mime: $e")
            return false
        }
        try {
            c.configure(fmt, surface, null, 0)
            c.start()
        } catch (e: Exception) {
            Log.e(TAG, "codec configure/start failed: $e")
            c.release()
            return false
        }
        codec = c
        configured = true
        Log.i(TAG, "decoder configured: $mime ${f.displayW}x${f.displayH}")
        return true
    }

    private fun renderLoop() {
        val info = MediaCodec.BufferInfo()
        while (running) {
            val c = codec ?: run {
                Thread.sleep(2)
                continue
            }
            val index = try {
                c.dequeueOutputBuffer(info, DEQUEUE_TIMEOUT_US)
            } catch (e: IllegalStateException) {
                Log.e(TAG, "dequeue output: $e")
                continue
            }
            when {
                index >= 0 -> {
                    val render = (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) == 0
                    c.releaseOutputBuffer(index, render)
                    if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) {
                        Log.i(TAG, "EOS")
                    }
                }
                index == MediaCodec.INFO_TRY_AGAIN_LATER -> {
                    // nothing ready
                }
                index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                    val fmt = c.outputFormat
                    Log.i(TAG, "output format: ${fmt.getString(MediaFormat.KEY_MIME)} " +
                        "${fmt.getInteger(MediaFormat.KEY_WIDTH)}x${fmt.getInteger(MediaFormat.KEY_HEIGHT)}")
                }
            }
        }
    }
}
