#pragma once

#include <cstdint>

#include "protocol.h"

namespace twin {

/*
 * Converts tablet input events into Windows input.
 *
 * Mouse: SendInput (absolute, virtual-screen normalized).
 * Touch: Windows.UI.Input.Injection (InjectTouchInput) on a dedicated STA
 *        thread with a message pump. Requires C++/WinRT headers (auto-detected
 *        by CMake); falls back to a no-op when unavailable.
 * Stylus: MVP routes STYLUS_* through touch; true pen needs WISP/HID-level
 *         injection.
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
    void StartTouchThread();

    int32_t offset_x_ = 0, offset_y_ = 0;
    uint32_t monitor_w_ = 0, monitor_h_ = 0;
    bool touch_thread_started_ = false;
};

}  // namespace twin
