package com.twinscreen.tablet

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.media.MediaCodec
import android.media.MediaFormat
import android.util.Log
import kotlin.concurrent.thread

/**
 * Plays the host's audio stream through an [AudioTrack].
 *
 * AAC ADTS frames (codec=1) are decoded by a MediaCodec ("audio/mp4a-latm"
 * with KEY_IS_ADTS) before being written to the track; raw PCM (codec=0) is
 * written straight through. A single worker thread drains the client's audio
 * queue and drives the codec synchronously.
 */
class AudioPlayer(private val client: Client) {
    companion object {
        private const val TAG = "AudioPlayer"
        private const val DEQUEUE_TIMEOUT_US = 20_000L
        private const val TRACK_BUFFER_MS = 400
    }

    @Volatile
    private var running = false
    private var codec: MediaCodec? = null
    private var track: AudioTrack? = null
    private var trackSampleRate = 0
    private var trackChannels = 0
    private var aacTimeUs = 0L
    private var aacFramesPerUs = 0L

    private val worker = thread(start = false, name = "audio-play") { loop() }

    fun start() {
        if (running) return
        running = true
        worker.start()
    }

    fun stop() {
        running = false
        try {
            codec?.stop()
        } catch (_: Exception) {
        }
        try {
            codec?.release()
        } catch (_: Exception) {
        }
        codec = null
        try {
            track?.stop()
        } catch (_: Exception) {
        }
        try {
            track?.release()
        } catch (_: Exception) {
        }
        track = null
        trackSampleRate = 0
        trackChannels = 0
        aacTimeUs = 0L
    }

    private fun loop() {
        while (running) {
            val f = client.nextAudioFrame()
            if (f == null) {
                Thread.sleep(2)
                continue
            }
            if (f.codec == Protocol.CODE_AUDIO_AAC) {
                playAac(f)
            } else {
                playPcm(f)
            }
        }
    }

    private fun playAac(f: Protocol.AudioFrame) {
        if (codec == null) {
            if (!configureAac(f)) return
        }
        val c = codec ?: return
        val index = try {
            c.dequeueInputBuffer(DEQUEUE_TIMEOUT_US)
        } catch (e: IllegalStateException) {
            Log.e(TAG, "dequeue input: $e")
            codec = null
            return
        }
        if (index < 0) return
        val buf = c.getInputBuffer(index) ?: return
        buf.clear()
        buf.put(f.data)
        c.queueInputBuffer(index, 0, f.data.size, aacTimeUs, 0)
        aacTimeUs += aacFramesPerUs
        drainOutput(c, f.sampleRate, f.channels)
    }

    private fun configureAac(f: Protocol.AudioFrame): Boolean {
        val fmt = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_AAC, f.sampleRate, f.channels)
        fmt.setInteger(MediaFormat.KEY_IS_ADTS, 1)
        val c = try {
            MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_AUDIO_AAC)
        } catch (e: Exception) {
            Log.e(TAG, "no AAC decoder: $e")
            return false
        }
        try {
            c.configure(fmt, null, null, 0)
            c.start()
        } catch (e: Exception) {
            Log.e(TAG, "AAC codec configure/start failed: $e")
            c.release()
            return false
        }
        codec = c
        aacTimeUs = 0L
        aacFramesPerUs = 1024L * 1_000_000L / f.sampleRate
        Log.i(TAG, "AAC decoder configured: ${f.sampleRate} Hz ${f.channels} ch (ADTS)")
        return true
    }

    private fun drainOutput(c: MediaCodec, sampleRate: Int, channels: Int) {
        val info = MediaCodec.BufferInfo()
        while (true) {
            val index = try {
                c.dequeueOutputBuffer(info, 0)
            } catch (e: IllegalStateException) {
                Log.e(TAG, "dequeue output: $e")
                return
            }
            when {
                index >= 0 -> {
                    val buf = c.getOutputBuffer(index)
                    if (buf != null && info.size > 0) {
                        val bytes = ByteArray(info.size)
                        buf.get(bytes)
                        writeTrack(bytes, sampleRate, channels)
                    }
                    c.releaseOutputBuffer(index, false)
                }
                index == MediaCodec.INFO_TRY_AGAIN_LATER -> return
                index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                    val of = c.outputFormat
                    val rate = of.getInteger(MediaFormat.KEY_SAMPLE_RATE)
                    val ch = of.getInteger(MediaFormat.KEY_CHANNEL_COUNT)
                    if (rate > 0 && ch > 0 && (rate != sampleRate || ch != channels)) {
                        writeTrack(ByteArray(0), rate, ch)
                    }
                }
            }
        }
    }

    private fun playPcm(f: Protocol.AudioFrame) {
        writeTrack(f.data, f.sampleRate, f.channels)
    }

    private fun writeTrack(data: ByteArray, sampleRate: Int, channels: Int) {
        if (track == null || trackSampleRate != sampleRate || trackChannels != channels) {
            try {
                track?.release()
            } catch (_: Exception) {
            }
            track = buildTrack(sampleRate, channels)
            trackSampleRate = sampleRate
            trackChannels = channels
        }
        val t = track ?: return
        try {
            if (data.isNotEmpty()) t.write(data, 0, data.size)
        } catch (e: Exception) {
            Log.e(TAG, "track write failed: $e")
        }
    }

    private fun buildTrack(sampleRate: Int, channels: Int): AudioTrack {
        val channelMask = if (channels >= 2) {
            AudioFormat.CHANNEL_OUT_STEREO
        } else {
            AudioFormat.CHANNEL_OUT_MONO
        }
        val minBuf = AudioTrack.getMinBufferSize(sampleRate, channelMask, AudioFormat.ENCODING_PCM_16BIT)
        val bufSize = maxOf(minBuf, sampleRate * channels * 2 * TRACK_BUFFER_MS / 1000)
        val attrs = AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_MEDIA)
            .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
            .build()
        val fmt = AudioFormat.Builder()
            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
            .setSampleRate(sampleRate)
            .setChannelMask(channelMask)
            .build()
        return AudioTrack.Builder()
            .setAudioAttributes(attrs)
            .setAudioFormat(fmt)
            .setBufferSizeInBytes(bufSize)
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build()
    }
}
