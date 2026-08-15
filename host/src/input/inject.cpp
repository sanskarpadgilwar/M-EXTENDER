#include "input/inject.h"

#include <windows.h>

#include <vector>

#include "util/log.h"

#ifdef TWIN_HAVE_CPPWINRT
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Input.Injection.h>
#endif

namespace twin {

#ifdef TWIN_HAVE_CPPWINRT

namespace {

constexpr UINT WM_INJECT_TOUCH = WM_APP + 1;

using winrt::Windows::Foundation::Rect;
using winrt::Windows::UI::Input::Injection::InjectedInputPointerInfo;
using winrt::Windows::UI::Input::Injection::InjectedInputPointerOptions;
using winrt::Windows::UI::Input::Injection::InjectedInputPoint;
using winrt::Windows::UI::Input::Injection::InjectedInputTouchInfo;
using winrt::Windows::UI::Input::Injection::InjectedInputVisualizer;

struct TouchPayload {
    std::vector<InjectedInputTouchInfo> items;
};

InjectedInputVisualizer g_visualizer{nullptr};
HANDLE g_thread_ready = nullptr;
DWORD g_thread_id = 0;
volatile bool g_running = false;

/*
 * The injection API demands an STA thread with a message queue. Run one and
 * marshal injections to it with a window message so the call always happens on
 * that thread.
 */
DWORD WINAPI TouchThreadProc(LPVOID) {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    try {
        g_visualizer = InjectedInputVisualizer::CreateInputInjector();
    } catch (winrt::hresult_error const& e) {
        Log("touch injection init failed: 0x%08x", e.code().value);
        g_visualizer = nullptr;
    }
    SetEvent(g_thread_ready);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_INJECT_TOUCH) {
            auto* p = reinterpret_cast<TouchPayload*>(msg.lParam);
            if (p) {
                if (g_visualizer) g_visualizer.InjectTouchInput(p->items);
                delete p;
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_running = false;
    return 0;
}

InjectedInputPointerOptions ToPointerOptions(uint8_t type) {
    switch (type) {
        case SP_INPUT_STYLUS_DOWN:
            return InjectedInputPointerOptions::New |
                   InjectedInputPointerOptions::InContact;
        case SP_INPUT_STYLUS_MOVE:
            return InjectedInputPointerOptions::InContact;
        case SP_INPUT_STYLUS_UP:
            return InjectedInputPointerOptions::Up;
        case SP_INPUT_CANCEL:
            return InjectedInputPointerOptions::Canceled;
        default:
            return InjectedInputPointerOptions::None;
    }
}

}  // namespace

InputInjector::~InputInjector() {
    if (g_running && g_thread_id) {
        PostThreadMessageW(g_thread_id, WM_QUIT, 0, 0);
    }
    if (g_thread_ready) {
        CloseHandle(g_thread_ready);
        g_thread_ready = nullptr;
    }
}

#else

InputInjector::~InputInjector() = default;

#endif

bool InputInjector::Init(int32_t offset_x, int32_t offset_y, uint32_t monitor_w,
                         uint32_t monitor_h) {
    offset_x_ = offset_x;
    offset_y_ = offset_y;
    monitor_w_ = monitor_w;
    monitor_h_ = monitor_h;
    StartTouchThread();
    return true;
}

void InputInjector::StartTouchThread() {
#ifdef TWIN_HAVE_CPPWINRT
    if (touch_thread_started_) return;
    touch_thread_started_ = true;
    g_thread_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_thread_ready) return;
    HANDLE h = CreateThread(nullptr, 0, TouchThreadProc, nullptr, 0, &g_thread_id);
    if (!h) {
        Log("touch thread create failed");
        return;
    }
    WaitForSingleObject(g_thread_ready, 5000);
    g_running = true;
    CloseHandle(h);
#endif
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
#ifdef TWIN_HAVE_CPPWINRT
    if (!g_visualizer) return;

    int dpi = GetDpiForSystem();
    if (dpi <= 0) dpi = 96;
    float sx = static_cast<float>(offset_x_ + x) * 96.0f / dpi;
    float sy = static_cast<float>(offset_y_ + y) * 96.0f / dpi;

    auto* p = new TouchPayload();
    InjectedInputTouchInfo info;
    auto pi = info.PointerInfo();
    pi.PointerId(contact);
    pi.PointerOptions(ToPointerOptions(type));
    pi.PixelLocation(InjectedInputPoint{sx, sy});
    info.PointerInfo(pi);
    info.Pressure(0.5f);
    info.Contact(Rect{0, 0, 1, 1});
    p->items.push_back(info);

    if (!PostThreadMessageW(g_thread_id, WM_INJECT_TOUCH, 0,
                            reinterpret_cast<LPARAM>(p))) {
        delete p;
        Log("touch injection post failed: %u", GetLastError());
    }
#else
    static bool warned = false;
    if (!warned) {
        warned = true;
        Log("touch injection disabled (C++/WinRT headers not found)");
    }
    (void)x;
    (void)y;
    (void)type;
    (void)contact;
#endif
}

}  // namespace twin
