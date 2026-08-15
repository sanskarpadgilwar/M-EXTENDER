#include "input/inject.h"

#include <windows.h>

#include "util/log.h"

namespace twin {

namespace {

/* Converts a tablet stylus event into the matching pen pointer flags. */
DWORD PenFlagsForType(uint8_t type) {
    switch (type) {
        case SP_INPUT_STYLUS_DOWN:
            return POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE |
                   POINTER_FLAG_INCONTACT | POINTER_FLAG_FIRSTBUTTON |
                   POINTER_FLAG_PRIMARY;
        case SP_INPUT_STYLUS_MOVE:
            return POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE |
                   POINTER_FLAG_INCONTACT | POINTER_FLAG_FIRSTBUTTON |
                   POINTER_FLAG_PRIMARY;
        case SP_INPUT_STYLUS_UP:
            return POINTER_FLAG_UP;
        case SP_INPUT_CANCEL:
            return POINTER_FLAG_UP | POINTER_FLAG_CANCELED;
        default:
            return 0;
    }
}

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

    /* Real pen input via the synthetic-pointer pipeline (Win10 1809+).
     * NULL on older systems; stylus then falls back to touch routing. */
    pen_dev_ = CreateSyntheticPointerDevice(PT_PEN, 1, POINTER_FEEDBACK_NONE);
    if (!pen_dev_) {
        Log("pen injection init failed: %u (stylus will act as touch)",
            GetLastError());
        pen_ok_ = false;
    } else {
        pen_ok_ = true;
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
            case SP_INPUT_CANCEL:
                if (pen_ok_ && ev->type != SP_INPUT_CANCEL) {
                    InjectPen(ev->x, ev->y, ev->type, ev->pressure,
                              ev->tilt_x, ev->tilt_y);
                } else if (pen_ok_) {
                    /* Cancel releases the pen; drop the stale coordinates. */
                    InjectPen(0, 0, ev->type, 0, 0, 0);
                } else {
                    /* Fallback: stylus acts as touch (no pen pipeline). */
                    InjectTouch(ev->x, ev->y, ev->type, ev->pointer_id);
                }
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

void InputInjector::InjectPen(uint16_t x, uint16_t y, uint8_t type,
                              uint8_t pressure, int16_t tilt_x,
                              int16_t tilt_y) {
    if (!pen_ok_) return;

    POINTER_PEN_INFO ppi{};
    ppi.pointerInfo.pointerType = PT_PEN;
    ppi.pointerInfo.pointerId = 0;
    ppi.pointerInfo.pointerFlags = PenFlagsForType(type);
    ppi.pointerInfo.ptPixelLocation.x = offset_x_ + x;
    ppi.pointerInfo.ptPixelLocation.y = offset_y_ + y;
    ppi.penMask = PEN_MASK_PRESSURE | PEN_MASK_TILT_X | PEN_MASK_TILT_Y;
    ppi.pressure = (static_cast<uint32_t>(pressure) * 1024u) / 255u;
    ppi.tiltX = tilt_x / 10; /* wire: tenths of degree -> degrees */
    ppi.tiltY = tilt_y / 10;

    POINTER_TYPE_INFO info{};
    info.type = PT_PEN;
    info.penInfo = ppi;

    if (!InjectSyntheticPointerInput(pen_dev_, &info, 1)) {
        /* ERROR_NOT_READY means events arrived too close together; the next
         * one will carry the state. */
        DWORD err = GetLastError();
        if (err != ERROR_NOT_READY) {
            Log("pen injection failed: %u", err);
        }
    }
}

InputInjector::~InputInjector() {
    if (pen_dev_) {
        DestroySyntheticPointerDevice(pen_dev_);
        pen_dev_ = nullptr;
    }
}

}  // namespace twin
