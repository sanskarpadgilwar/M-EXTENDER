#include "input/inject.h"

#include <windows.h>

#include "util/log.h"

namespace twin {

namespace {

/* Converts a tablet stylus event into the matching touch pointer flags. */
DWORD TouchFlagsForType(uint8_t type) {
    switch (type) {
        case SP_INPUT_STYLUS_DOWN:
            return POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE |
                   POINTER_FLAG_INCONTACT;
        case SP_INPUT_STYLUS_MOVE:
            return POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE |
                   POINTER_FLAG_INCONTACT;
        case SP_INPUT_STYLUS_UP:
            return POINTER_FLAG_UP;
        case SP_INPUT_CANCEL:
            return POINTER_FLAG_CANCELED | POINTER_FLAG_UP;
        default:
            return 0;
    }
}

}  // namespace

bool InputInjector::Init(int32_t offset_x, int32_t offset_y, uint32_t monitor_w,
                         uint32_t monitor_h) {
    offset_x_ = offset_x;
    offset_y_ = offset_y;
    monitor_w_ = monitor_w;
    monitor_h_ = monitor_h;

    /* 16 simultaneous contacts is plenty for a stylus; feedback rings make
     * injected touch visible. */
    if (!InitializeTouchInjection(16, TOUCH_FEEDBACK_INDIRECT)) {
        Log("touch injection init failed: %u", GetLastError());
        touch_ok_ = false;
    } else {
        touch_ok_ = true;
    }
    return true;
}

void InputInjector::InjectBatch(const sp_input_batch* batch) {
    if (!batch) return;
    const sp_input_event* ev =
        reinterpret_cast<const sp_input_event*>(batch + 1);
    for (uint32_t i = 0; i < batch->count; ++i, ++ev) {
        switch (ev->type) {
            case SP_INPUT_DOWN:
            case SP_INPUT_MOVE:
            case SP_INPUT_UP:
                InjectMouse(ev->x, ev->y, ev->buttons, ev->type);
                break;
            case SP_INPUT_STYLUS_DOWN:
            case SP_INPUT_STYLUS_MOVE:
            case SP_INPUT_STYLUS_UP:
                /* MVP: stylus acts as touch. TODO(pen): WISP injection. */
                InjectTouch(ev->x, ev->y, ev->type, ev->pointer_id);
                break;
            default:
                break;
        }
    }
}

void InputInjector::InjectMouse(uint16_t x, uint16_t y, uint8_t buttons,
                                uint8_t type) {
    int vsx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vsy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vsw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vsh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vsw <= 0 || vsh <= 0) return;

    int abs_x = offset_x_ + x;
    int abs_y = offset_y_ + y;
    UINT norm_x = (UINT)((abs_x - vsx) * 65536.0 / vsw) + 1;
    UINT norm_y = (UINT)((abs_y - vsy) * 65536.0 / vsh) + 1;

    DWORD down = 0;
    if (buttons & SP_BTN_LEFT) down |= MOUSEEVENTF_LEFTDOWN;
    if (buttons & SP_BTN_RIGHT) down |= MOUSEEVENTF_RIGHTDOWN;
    if (buttons & SP_BTN_MIDDLE) down |= MOUSEEVENTF_MIDDLEDOWN;

    DWORD up = 0;
    if (buttons & SP_BTN_LEFT) up |= MOUSEEVENTF_LEFTUP;
    if (buttons & SP_BTN_RIGHT) up |= MOUSEEVENTF_RIGHTUP;
    if (buttons & SP_BTN_MIDDLE) up |= MOUSEEVENTF_MIDDLEUP;

    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = norm_x;
    in.mi.dy = norm_y;
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE |
                    (type == SP_INPUT_UP ? up : down);
    SendInput(1, &in, sizeof(in));
}

void InputInjector::InjectTouch(uint16_t x, uint16_t y, uint8_t type,
                                uint32_t contact) {
    if (!touch_ok_) return;

    /* InjectTouchInput uses physical screen pixels; pointerId must stay within
     * the 0..maxCount-1 range configured at init. */
    POINTER_TOUCH_INFO pt{};
    pt.pointerInfo.pointerType = PT_TOUCH;
    pt.pointerInfo.pointerId = contact % 16;
    pt.pointerInfo.pointerFlags = TouchFlagsForType(type);
    pt.pointerInfo.ptPixelLocation.x = offset_x_ + x;
    pt.pointerInfo.ptPixelLocation.y = offset_y_ + y;
    pt.touchFlags = TOUCH_FLAG_NONE;
    pt.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_PRESSURE;
    pt.pressure = 32000;
    pt.rcContact.left = pt.pointerInfo.ptPixelLocation.x - 2;
    pt.rcContact.top = pt.pointerInfo.ptPixelLocation.y - 2;
    pt.rcContact.right = pt.pointerInfo.ptPixelLocation.x + 2;
    pt.rcContact.bottom = pt.pointerInfo.ptPixelLocation.y + 2;

    if (!InjectTouchInput(1, &pt)) {
        /* ERROR_NOT_READY just means frames arrived <0.1 ms apart; retry the
         * next one. Anything else is a real failure. */
        DWORD err = GetLastError();
        if (err != ERROR_NOT_READY) {
            Log("touch injection failed: %u", err);
        }
    }
}

InputInjector::~InputInjector() = default;

}  // namespace twin
