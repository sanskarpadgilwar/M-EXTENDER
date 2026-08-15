package com.twinscreen.tablet

import android.app.Activity
import android.os.Bundle
import android.view.Gravity
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast

class MainActivity : Activity() {

    private var client: Client? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(buildConnectUi())
    }

    override fun onBackPressed() {
        if (client?.isConnected() == true) {
            client?.disconnect()
            showConnectUi()
        } else {
            super.onBackPressed()
        }
    }

    override fun onDestroy() {
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
        val c = Client(
            host = host,
            port = port,
            onConnected = { runOnUiThread { startRendering(c) } },
            onDisconnected = { runOnUiThread { showConnectUi() } },
        )
        client = c
        c.start()
    }

    private fun startRendering(c: Client) {
        setContentView(RenderView(this, c))
    }

    private fun showConnectUi() {
        if (client?.isConnected() == false) {
            setContentView(buildConnectUi())
        }
    }
}
