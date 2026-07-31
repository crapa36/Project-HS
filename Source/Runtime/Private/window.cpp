#include "window.hpp"

#include <hidusage.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

namespace hs
{
namespace
{

constexpr wchar_t kWindowClass[] = L"ProjectHS.Stage1";

GameAction ActionForVirtualKey(USHORT key) noexcept
{
    switch (key)
    {
    case 'Q':
        return GameAction::SkillQ;
    case 'W':
        return GameAction::SkillW;
    case 'E':
        return GameAction::SkillE;
    case 'R':
        return GameAction::SkillR;
    default:
        return GameAction::Pause;
    }
}

} // namespace

Window::Window(RuntimeChannels &channels) noexcept : channels_(&channels)
{
}

Window::~Window()
{
    if (window_)
    {
        DestroyWindow(window_);
    }
    if (instance_)
    {
        UnregisterClassW(kWindowClass, instance_);
    }
}

Result Window::Create(std::uint32_t width, std::uint32_t height, bool visible, bool borderless)
{
    instance_ = GetModuleHandleW(nullptr);

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = WindowProcedure;
    window_class.hInstance = instance_;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    window_class.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                               std::format("RegisterClassExW failed: {}", GetLastError()));
    }

    auto style = borderless ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    auto x = CW_USEDEFAULT;
    auto y = CW_USEDEFAULT;
    RECT rectangle{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    if (borderless)
    {
        MONITORINFO monitor_info{sizeof(MONITORINFO)};
        const auto monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        if (!GetMonitorInfoW(monitor, &monitor_info))
        {
            return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                   "Cannot query the primary monitor.");
        }
        rectangle = monitor_info.rcMonitor;
        x = rectangle.left;
        y = rectangle.top;
        width = static_cast<std::uint32_t>(rectangle.right - rectangle.left);
        height = static_cast<std::uint32_t>(rectangle.bottom - rectangle.top);
    }
    else
    {
        AdjustWindowRect(&rectangle, style, FALSE);
    }
    window_ = CreateWindowExW(0, kWindowClass, L"Project HS - Stage 1",
                              style, x, y,
                              rectangle.right - rectangle.left, rectangle.bottom - rectangle.top,
                              nullptr, nullptr, instance_, this);
    if (!window_)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                               std::format("CreateWindowExW failed: {}", GetLastError()));
    }

    RAWINPUTDEVICE devices[] = {
        {HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_KEYBOARD, RIDEV_INPUTSINK, window_},
        {HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_MOUSE, RIDEV_INPUTSINK, window_},
    };
    if (!RegisterRawInputDevices(devices, static_cast<UINT>(std::size(devices)),
                                 sizeof(RAWINPUTDEVICE)))
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                               std::format("RegisterRawInputDevices failed: {}", GetLastError()));
    }

    client_width_ = width;
    client_height_ = height;
    if (visible)
    {
        ShowWindow(window_, SW_SHOW);
    }
    return Result::Success();
}

bool Window::PumpMessages()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        if (message.message == WM_QUIT)
        {
            return false;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (accepting_input_ && window_)
    {
        POINT cursor{};
        if (GetCursorPos(&cursor) && ScreenToClient(window_, &cursor))
        {
            const auto normalized_x =
                static_cast<float>(cursor.x) / static_cast<float>(client_width_) * 2.0f - 1.0f;
            const auto normalized_y =
                1.0f - static_cast<float>(cursor.y) / static_cast<float>(client_height_) * 2.0f;
            held_.aim_world = {normalized_x * 30.0f, 0.0f, normalized_y * 30.0f};
            UpdateHeldInput();
        }
    }
    return true;
}

HWND Window::Handle() const noexcept
{
    return window_;
}

void Window::StopInput() noexcept
{
    accepting_input_ = false;
    forward_ = backward_ = left_ = right_ = false;
    held_ = {};
    channels_->held_input.store(held_, std::memory_order_release);
    channels_->focus_epoch.fetch_add(1, std::memory_order_release);
}

LRESULT CALLBACK Window::WindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    Window *self = reinterpret_cast<Window *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const auto *create = reinterpret_cast<const CREATESTRUCTW *>(lparam);
        self = static_cast<Window *>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->window_ = window;
    }
    return self ? self->HandleMessage(window, message, wparam, lparam)
                : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT Window::HandleMessage(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
    case WM_INPUT:
        if (accepting_input_)
        {
            HandleRawInput(reinterpret_cast<HRAWINPUT>(lparam));
        }
        return 0;
    case WM_KILLFOCUS:
        forward_ = backward_ = left_ = right_ = false;
        held_ = {};
        channels_->held_input.store(held_, std::memory_order_release);
        channels_->focus_epoch.fetch_add(1, std::memory_order_release);
        return 0;
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED)
        {
            client_width_ = std::max<std::uint32_t>(LOWORD(lparam), 1);
            client_height_ = std::max<std::uint32_t>(HIWORD(lparam), 1);
            (void)channels_->graphics_commands.TryPush(
                {GraphicsCommandKind::Resize, client_width_, client_height_});
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        window_ = nullptr;
        channels_->stop_requested.store(true, std::memory_order_release);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

void Window::HandleRawInput(HRAWINPUT input)
{
    std::array<std::byte, 256> storage{};
    UINT size = static_cast<UINT>(storage.size());
    if (GetRawInputData(input, RID_INPUT, storage.data(), &size, sizeof(RAWINPUTHEADER)) == UINT(-1))
    {
        return;
    }

    const auto &raw = *reinterpret_cast<const RAWINPUT *>(storage.data());
    if (raw.header.dwType == RIM_TYPEKEYBOARD)
    {
        const auto key = raw.data.keyboard.VKey;
        const auto pressed = (raw.data.keyboard.Flags & RI_KEY_BREAK) == 0;
        switch (key)
        {
        case 'W':
            forward_ = pressed;
            break;
        case 'S':
            backward_ = pressed;
            break;
        case 'A':
            left_ = pressed;
            break;
        case 'D':
            right_ = pressed;
            break;
        case 'Q':
        case 'E':
        case 'R':
        case VK_ESCAPE:
            PushAction(ActionForVirtualKey(key), pressed ? EdgeKind::Pressed : EdgeKind::Released);
            break;
        default:
            break;
        }
        UpdateHeldInput();
    }
    else if (raw.header.dwType == RIM_TYPEMOUSE)
    {
        if (raw.data.mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN)
        {
            held_.basic_attack_held = true;
            PushAction(GameAction::BasicAttack, EdgeKind::Pressed);
        }
        if (raw.data.mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP)
        {
            held_.basic_attack_held = false;
            PushAction(GameAction::BasicAttack, EdgeKind::Released);
        }
        UpdateHeldInput();
    }
}

void Window::UpdateHeldInput() noexcept
{
    held_.normalized_move = {static_cast<float>(right_) - static_cast<float>(left_),
                             static_cast<float>(forward_) - static_cast<float>(backward_)};
    const auto length_squared = held_.normalized_move.x * held_.normalized_move.x +
                                held_.normalized_move.y * held_.normalized_move.y;
    if (length_squared > 1.0f)
    {
        const auto inverse_length = 1.0f / std::sqrt(length_squared);
        held_.normalized_move.x *= inverse_length;
        held_.normalized_move.y *= inverse_length;
    }
    channels_->held_input.store(held_, std::memory_order_release);
}

void Window::PushAction(GameAction action, EdgeKind kind) noexcept
{
    if (!channels_->action_edges.TryPush({++input_sequence_, action, kind}))
    {
        channels_->dropped_input_edges.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace hs
