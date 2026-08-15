package com.twinscreen.tablet

import android.content.Context
import android.util.AttributeSet
import android.view.SurfaceHolder
import android.view.SurfaceView

/**
 * Fullscreen surface wired to a [VideoPlayer] (decode+render) and an
 * [InputSender] (touch back to the host). Handles its own lifecycle so the
 * decoder is tied to surface creation.
 */
class RenderView @JvmOverloads constructor(
    context: Context,
    private val client: Client,
    attrs: AttributeSet? = null,
) : SurfaceView(context, attrs), SurfaceHolder.Callback {

    private var player: VideoPlayer? = null

    init {
        holder.addCallback(this)
        setOnTouchListener(InputSender(client, { client.ack?.modeW ?: 1 }, { client.ack?.modeH ?: 1 }))
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        val p = VideoPlayer(holder.surface, client) {
            client.requestKeyframe()
        }
        p.start()
        player = p
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        // Nothing to do: MediaCodec scales to the surface.
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        player?.stop()
        player = null
    }
}
