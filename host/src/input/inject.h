#pragma once

#include <cstdint>

#include <windows.h>

#include "protocol.h"

namespace twin {

/*
 * Converts tablet input events into Windows input.
 *
 * Mouse: SendInput (absolute, virtual-screen normalized).
 * Touch: USER32 InitializeTouchInjection/InjectTouchInput (winuser.h). This is
 *        the classic desktop touch-injection API: no WinRT, no extra headers.
 *        Coordinates are physical screen pixels (monitor offset + local pos).
 * Stylus: CreateSyntheticPointerDevice(PT_PEN) + InjectSyntheticPointerInput
 *         (winuser.h, Windows 10 1809+). This is real pen input through the
 *         pointer pipeline (Windows Ink sees it), with pressure and tilt.
 *         Falls back to touch routing on systems without the API.
 *
 * Event coordinates are monitor-local (0..mode_w, 0..mode_h); the monitor's
 * virtual-screen offset is added before injection.
 */
class InputInjector {
public:
    InputInjector() = default;
    ~InputInjector();

    InputInjector(const InputInjector&) = delete;
    InputInjector& operator=(const InputInjector&) = delete;

    bool Init(int32_t offset_x, int32_t offset_y, uint32_t monitor_w,
              uint32_t monitor_h);
    void InjectBatch(const sp_input_batch* batch);

private:
    void InjectMouse(uint16_t x, uint16_t y, uint8_t buttons, uint8_t type);
    void InjectTouch(uint16_t x, uint16_t y, uint8_t type, uint32_t contact);
    void InjectPen(uint16_t x, uint16_t y, uint8_t type, uint8_t pressure,
                   int16_t tilt_x, int16_t tilt_y);

    int32_t offset_x_ = 0, offset_y_ = 0;
    uint32_t monitor_w_ = 0, monitor_h_ = 0;
    bool touch_ok_ = false;
    HSYNTHETICPOINTERDEVICE pen_dev_ = nullptr;
    bool pen_ok_ = false;
};

}  // namespace twin
