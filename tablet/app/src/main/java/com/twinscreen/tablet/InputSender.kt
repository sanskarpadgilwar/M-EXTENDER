package com.twinscreen.tablet

import android.view.MotionEvent
import android.view.View

/**
 * Converts [MotionEvent]s on the render view into protocol input batches and
 * pushes them to the host. Handles multi-touch and stylus (as STYLUS_* types).
 */
class InputSender(
    private val client: Client,
    private val displayW: () -> Int,
    private val displayH: () -> Int,
) : View.OnTouchListener {

    private val activePointers = HashMap<Int, Boolean>()

    override fun onTouch(view: View, e: MotionEvent): Boolean {
        val events = ArrayList<Protocol.InputEvent>()
        val action = e.actionMasked
        val actionIndex = e.actionIndex

        when (action) {
            MotionEvent.ACTION_DOWN,
            MotionEvent.ACTION_POINTER_DOWN -> {
                val id = e.getPointerId(actionIndex)
                activePointers[id] = true
                val stylus = isStylus(e, actionIndex)
                events.add(toEvent(view, e, actionIndex, if (stylus) Protocol.IN_STYLUS_DOWN else Protocol.IN_DOWN))
            }

            MotionEvent.ACTION_MOVE -> {
                for (i in 0 until e.pointerCount) {
                    val id = e.getPointerId(i)
                    if (activePointers[id] != true) continue
                    val stylus = isStylus(e, i)
                    events.add(toEvent(view, e, i, if (stylus) Protocol.IN_STYLUS_MOVE else Protocol.IN_MOVE))
                }
            }

            MotionEvent.ACTION_UP,
            MotionEvent.ACTION_POINTER_UP -> {
                val id = e.getPointerId(actionIndex)
                activePointers.remove(id)
                val stylus = isStylus(e, actionIndex)
                events.add(toEvent(view, e, actionIndex, if (stylus) Protocol.IN_STYLUS_UP else Protocol.IN_UP))
            }

            MotionEvent.ACTION_CANCEL -> {
                for ((id, _) in activePointers) {
                    events.add(Protocol.InputEvent(0, 0, Protocol.IN_CANCEL, 0, id, 0, 0, 0))
                }
                activePointers.clear()
            }
        }

        if (events.isNotEmpty()) client.sendInputBatch(events)
        return true
    }

    private fun toEvent(view: View, e: MotionEvent, index: Int, type: Int): Protocol.InputEvent {
        val dx = displayW()
        val dy = displayH()
        val vw = if (view.width > 0) view.width.toFloat() else 1f
        val vh = if (view.height > 0) view.height.toFloat() else 1f

        val x = ((e.getX(index) / vw) * dx).toInt().coerceIn(0, dx)
        val y = ((e.getY(index) / vh) * dy).toInt().coerceIn(0, dy)
        val pressure = (e.getPressure(index).coerceIn(0f, 1f) * 255).toInt()
        val stylus = isStylus(e, index)
        val tiltX = if (stylus) (e.getAxisValue(MotionEvent.AXIS_ORIENTATION, index) * 57.29578).toInt() else 0
        val tiltY = if (stylus) (e.getAxisValue(MotionEvent.AXIS_TILT, index) * 57.29578).toInt() else 0

        return Protocol.InputEvent(
            x = x,
            y = y,
            type = type,
            buttons = 0,
            pointerId = e.getPointerId(index),
            pressure = pressure,
            tiltX = tiltX,
            tiltY = tiltY,
        )
    }

    private fun isStylus(e: MotionEvent, index: Int): Boolean =
        e.getToolType(index) == MotionEvent.TOOL_TYPE_STYLUS
}
