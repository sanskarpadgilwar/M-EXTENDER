package com.twinscreen.tablet

import android.app.Activity
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import java.util.Locale

class MainActivity : Activity() {

    private var client: Client? = null
    private var rendering = false
    private var destroyed = false
    private val uiHandler = Handler(Looper.getMainLooper())
    private var statusView: TextView? = null
    private var statsView: TextView? = null

    private val statsRunnable = object : Runnable {
        override fun run() {
            val c = client ?: return
            val status = when {
                c.reconnectActive -> "Reconnecting (attempt ${c.reconnectAttempt})"
                c.isConnected() -> "Connected ${c.host}:${c.port}"
                else -> "Connecting…"
            }
            val s = c.stats()
            statusView?.text = status
            statsView?.text = String.format(
                Locale.US, "%.1f fps  %.1f Mbps  queue %d", s.fps, s.mbps, s.queueDepth,
            )
            uiHandler.postDelayed(this, 500)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(buildConnectUi())
    }

    override fun onBackPressed() {
        if (client?.isConnected() == true || client?.reconnectActive == true) {
            stopClient()
        } else {
            super.onBackPressed()
        }
    }

    override fun onDestroy() {
        destroyed = true
        uiHandler.removeCallbacks(statsRunnable)
        client?.disconnect()
        super.onDestroy()
    }

    private fun buildConnectUi(): View {
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            setPadding(48, 48, 48, 48)
        }

        val title = TextView(this).apply {
            text = "TwinScreen"
            textSize = 24f
            setPadding(0, 0, 0, 24)
        }
        root.addView(title)

        val host = EditText(this).apply {
            hint = "Host IP (e.g. 127.0.0.1 over ADB)"
            id = View.generateViewId()
        }
        root.addView(host)

        val port = EditText(this).apply {
            hint = "Port"
            setText(Protocol.PORT.toString())
            id = View.generateViewId()
        }
        root.addView(port)

        val connect = Button(this).apply {
            text = "Connect"
            setOnClickListener {
                val h = host.text.toString().trim()
                if (h.isEmpty()) {
                    Toast.makeText(this@MainActivity, "Enter a host", Toast.LENGTH_SHORT).show()
                    return@setOnClickListener
                }
                val p = port.text.toString().toIntOrNull() ?: Protocol.PORT
                connectTo(h, p)
            }
        }
        root.addView(connect)

        return root
    }

    private fun connectTo(host: String, port: Int) {
        lateinit var c: Client
        c = Client(
            host = host,
            port = port,
            onConnected = { runOnUiThread { if (!destroyed) onHostConnected(c) } },
            onReconnecting = { attempt ->
                runOnUiThread { if (!destroyed) onReconnecting(attempt) }
            },
            onDisconnected = {
                runOnUiThread { if (!destroyed && client === c) onClientStopped() }
            },
        )
        client = c
        c.start()
    }

    private fun onHostConnected(c: Client) {
        if (!rendering) {
            rendering = true
            showRenderScreen(c)
        }
    }

    private fun onReconnecting(attempt: Int) {
        if (rendering) {
            statusView?.text = "Reconnecting (attempt $attempt)"
        } else if (attempt == 1) {
            Toast.makeText(this, "Connection lost — reconnecting…", Toast.LENGTH_SHORT).show()
        }
    }

    private fun onClientStopped() {
        rendering = false
        client = null
        statusView = null
        statsView = null
        uiHandler.removeCallbacks(statsRunnable)
        if (!destroyed) setContentView(buildConnectUi())
    }

    private fun stopClient() {
        val c = client ?: return
        c.disconnect()
        onClientStopped() // return to the connect UI immediately; later callback is idempotent
    }

    private fun showRenderScreen(c: Client) {
        val status = TextView(this).apply {
            setTextColor(0xFFFFFFFF.toInt())
        }
        val stats = TextView(this).apply {
            setTextColor(0xFFFFFFFF.toInt())
        }
        statusView = status
        statsView = stats

        val overlay = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.END
            setPadding(24, 16, 16, 16)
            setBackgroundColor(0x99000000.toInt())
            addView(status)
            addView(stats)
            addView(Button(this@MainActivity).apply {
                text = "Disconnect"
                setOnClickListener { stopClient() }
            })
        }

        val frame = FrameLayout(this).apply {
            addView(
                RenderView(this@MainActivity, c),
                FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.MATCH_PARENT,
                ),
            )
            addView(
                overlay,
                FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT,
                    Gravity.TOP or Gravity.END,
                ),
            )
        }
        setContentView(frame)
        uiHandler.post(statsRunnable)
    }
}
