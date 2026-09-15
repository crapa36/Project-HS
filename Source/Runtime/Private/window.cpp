#include "window.hpp"
#include "camera_pose.hpp"

#include <hidusage.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

namespace hs
{
namespace
{

constexpr wchar_t kWindowClass[] = L"ProjectHS.Window";


} // namespace

Window::Window(RuntimeChannels &channels,
               std::array<std::uint16_t, 4> skill_virtual_keys,
               PresentationCamera camera) noexcept
    : channels_(&channels), skill_virtual_keys_(skill_virtual_keys), camera_(camera)
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

    borderless_ = borderless;
    windowed_client_width_ = width;
    windowed_client_height_ = height;
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
    window_ = CreateWindowExW(0, kWindowClass, L"Project HS",
                              style, x, y,
                              rectangle.right - rectangle.left, rectangle.bottom - rectangle.top,
                              nullptr, nullptr, instance_, this);
    if (!window_)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                               std::format("CreateWindowExW failed: {}", GetLastError()));
    }
    if (!borderless && GetWindowRect(window_, &windowed_rect_))
        has_windowed_rect_ = true;

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
            held_.cursor_normalized = {normalized_x, normalized_y};
            constexpr float reference_aspect = 1920.0f / 1080.0f;
            const auto output_aspect = static_cast<float>(client_width_) /
                                       static_cast<float>(client_height_);
            auto reference_u = (normalized_x + 1.0f) * 0.5f;
            auto reference_v = (1.0f - normalized_y) * 0.5f;
            if (output_aspect > reference_aspect)
                reference_u = (reference_u - 0.5f) * output_aspect /
                              reference_aspect + 0.5f;
            else
                reference_v = (reference_v - 0.5f) * reference_aspect /
                              output_aspect + 0.5f;
            ui_cursor_normalized_ = {reference_u * 2.0f - 1.0f,
                                     1.0f - reference_v * 2.0f};
            const auto yaw = camera_.yaw_degrees * 3.14159265358979323846f / 180.0f;
            const auto zoom_percent = static_cast<float>(channels_->camera_zoom_percent.load(std::memory_order_acquire));
            const auto pose = ComputeRuntimeCameraPose(camera_.distance_m, camera_.pitch_degrees, zoom_percent);
            const auto pitch = pose.pitch_degrees * 3.14159265358979323846f / 180.0f;
            const auto vertical_fov = camera_.vertical_fov_degrees *
                                      3.14159265358979323846f / 180.0f;
            const auto target_x = channels_->camera_target_x.load(std::memory_order_acquire);
            const auto target_z = channels_->camera_target_z.load(std::memory_order_acquire);
            const auto target_y = pose.target_height_m;
            const auto distance = pose.distance_m;
            const auto forward_x = std::cos(pitch) * std::sin(yaw);
            const auto forward_y = -std::sin(pitch);
            const auto forward_z = std::cos(pitch) * std::cos(yaw);
            const auto right_x = forward_z / std::cos(pitch);
            const auto right_z = -forward_x / std::cos(pitch);
            const auto up_x = forward_y * right_z;
            const auto up_y = forward_z * right_x - forward_x * right_z;
            const auto up_z = -forward_y * right_x;
            const auto aspect = static_cast<float>(client_width_) /
                                static_cast<float>(client_height_);
            const auto tangent = std::tan(vertical_fov * 0.5f);
            auto ray_x = forward_x + right_x * normalized_x * aspect * tangent +
                         up_x * normalized_y * tangent;
            auto ray_y = forward_y + up_y * normalized_y * tangent;
            auto ray_z = forward_z + right_z * normalized_x * aspect * tangent +
                         up_z * normalized_y * tangent;
            const auto ray_length = std::sqrt(ray_x * ray_x + ray_y * ray_y +
                                              ray_z * ray_z);
            ray_x /= ray_length;
            ray_y /= ray_length;
            ray_z /= ray_length;
            const auto eye_x = target_x - forward_x * distance;
            const auto eye_y = target_y - forward_y * distance;
            const auto eye_z = target_z - forward_z * distance;
            const auto ray_time = ray_y < -0.0001f ? -eye_y / ray_y : distance;
            held_.aim_world = {eye_x + ray_x * ray_time, 0.0f,
                               eye_z + ray_z * ray_time};
            held_.move_target_world = held_.aim_world;
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
    held_ = {};
    channels_->held_input.store(held_, std::memory_order_release);
    channels_->focus_epoch.fetch_add(1, std::memory_order_release);
}

void Window::BeginSkillRebind(std::uint32_t slot) noexcept
{
    if (slot < skill_virtual_keys_.size())
    {
        pending_rebind_slot_ = static_cast<std::uint8_t>(slot);
    }
}

void Window::CancelSkillRebind() noexcept
{
    pending_rebind_slot_.reset();
}

void Window::SetUiClickHandler(std::function<void(Float2)> handler)
{
    ui_click_handler_ = std::move(handler);
}

void Window::SetUiHoverHandler(std::function<void(Float2)> handler)
{
    ui_hover_handler_ = std::move(handler);
}

bool Window::ConsumeReboundSkillKeys(std::array<std::uint16_t, 4> &keys) noexcept
{
    if (!rebound_skill_keys_)
        return false;
    keys = *rebound_skill_keys_;
    rebound_skill_keys_.reset();
    return true;
}

Result Window::SetBorderless(bool borderless)
{
    if (!window_ || borderless == borderless_)
        return Result::Success();

    RECT target{};
    DWORD style{};
    if (borderless)
    {
        if (GetWindowRect(window_, &windowed_rect_))
            has_windowed_rect_ = true;
        MONITORINFO monitor{sizeof(MONITORINFO)};
        if (!GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor))
            return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                   "Cannot query the target monitor.");
        target = monitor.rcMonitor;
        style = WS_POPUP;
    }
    else
    {
        style = WS_OVERLAPPEDWINDOW;
        if (has_windowed_rect_)
        {
            target = windowed_rect_;
        }
        else
        {
            target = {0, 0, static_cast<LONG>(windowed_client_width_),
                      static_cast<LONG>(windowed_client_height_)};
            AdjustWindowRect(&target, style, FALSE);
        }
    }

    SetLastError(ERROR_SUCCESS);
    const auto previous = SetWindowLongPtrW(window_, GWL_STYLE, style);
    if (previous == 0 && GetLastError() != ERROR_SUCCESS)
        return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                               std::format("SetWindowLongPtrW failed: {}", GetLastError()));
    if (!SetWindowPos(window_, nullptr, target.left, target.top,
                      target.right - target.left, target.bottom - target.top,
                      SWP_FRAMECHANGED | SWP_NOOWNERZORDER))
        return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                               std::format("SetWindowPos failed: {}", GetLastError()));
    borderless_ = borderless;
    return Result::Success();
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
#if !defined(NDEBUG)
    switch (message)
    {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MOUSEWHEEL:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_CHAR:
        (void)channels_->window_messages.TryPush(
            {message, static_cast<std::uintptr_t>(wparam),
             static_cast<std::intptr_t>(lparam)});
        break;
    }
#endif
    switch (message)
    {
    case WM_MOUSEMOVE:
        if (ui_hover_handler_)
        {
            const auto x = static_cast<float>(static_cast<short>(LOWORD(lparam)));
            const auto y = static_cast<float>(static_cast<short>(HIWORD(lparam)));
            const Float2 cursor{(x / static_cast<float>(std::max(client_width_, 1u))) * 2.0f - 1.0f,
                                1.0f - (y / static_cast<float>(std::max(client_height_, 1u))) * 2.0f};
            ui_hover_handler_(cursor);
        }
        return 0;
    case WM_INPUT:
        if (accepting_input_)
        {
            HandleRawInput(reinterpret_cast<HRAWINPUT>(lparam));
        }
        return 0;
    case WM_KILLFOCUS:
        held_ = {};
        channels_->held_input.store(held_, std::memory_order_release);
        channels_->focus_epoch.fetch_add(1, std::memory_order_release);
        return 0;
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED)
        {
            client_width_ = std::max<std::uint32_t>(LOWORD(lparam), 1);
            client_height_ = std::max<std::uint32_t>(HIWORD(lparam), 1);
            (void)channels_->resize_commands.TryPush(
                {client_width_, client_height_});
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
        if (channels_->devtools_capture_keyboard.load(std::memory_order_acquire)) return;
        const auto key = raw.data.keyboard.VKey;
        const auto pressed = (raw.data.keyboard.Flags & RI_KEY_BREAK) == 0;
        if (pending_rebind_slot_)
        {
            if (pressed)
            {
                if (key != VK_ESCAPE && key != 0 && key <= 0xFE)
                {
                    const auto slot = static_cast<std::size_t>(*pending_rebind_slot_);
                    const auto existing = std::ranges::find(skill_virtual_keys_, key);
                    if (existing != skill_virtual_keys_.end())
                        std::swap(*existing, skill_virtual_keys_[slot]);
                    else
                        skill_virtual_keys_[slot] = static_cast<std::uint16_t>(key);
                }
                pending_rebind_slot_.reset();
                rebound_skill_keys_ = skill_virtual_keys_;
            }
            return;
        }
        if (key == VK_ESCAPE)
        {
            PushAction(GameAction::Pause,
                       pressed ? EdgeKind::Pressed : EdgeKind::Released);
        }
        else if (key == VK_TAB)
        {
            PushAction(GameAction::CharacterPage,
                       pressed ? EdgeKind::Pressed : EdgeKind::Released);
        }
        else if (const auto iterator = std::ranges::find(skill_virtual_keys_, key);
                 iterator != skill_virtual_keys_.end())
        {
            const auto slot = static_cast<std::size_t>(iterator - skill_virtual_keys_.begin());
            PushAction(static_cast<GameAction>(static_cast<unsigned>(GameAction::SkillQ) + slot),
                       pressed ? EdgeKind::Pressed : EdgeKind::Released);
        }
        UpdateHeldInput();
    }
    else if (raw.header.dwType == RIM_TYPEMOUSE)
    {
        if (channels_->devtools_capture_mouse.load(std::memory_order_acquire)) return;
        if (raw.data.mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN)
        {
            held_.basic_attack_held = true;
            if (ui_click_handler_) ui_click_handler_(ui_cursor_normalized_);
            PushAction(GameAction::BasicAttack, EdgeKind::Pressed);
        }
        if (raw.data.mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP)
        {
            held_.basic_attack_held = false;
            PushAction(GameAction::BasicAttack, EdgeKind::Released);
        }
        if (raw.data.mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN)
        {
            held_.move_held = true;
        }
        if (raw.data.mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP)
        {
            held_.move_held = false;
        }
        if (raw.data.mouse.usButtonFlags & RI_MOUSE_WHEEL)
        {
            const auto delta = static_cast<SHORT>(raw.data.mouse.usButtonData);
            auto zoom = channels_->camera_zoom_percent.load(std::memory_order_relaxed);
            const auto step = delta > 0 ? -10 : 10;
            const auto next = static_cast<std::int64_t>(zoom) + step;
            zoom = static_cast<std::uint32_t>(std::clamp(
                next,
                static_cast<std::int64_t>(std::min<std::uint32_t>(camera_.minimum_distance_percent, 15u)),
                static_cast<std::int64_t>(camera_.maximum_distance_percent)));
            channels_->camera_zoom_percent.store(zoom, std::memory_order_release);
        }
        UpdateHeldInput();
    }
}

void Window::UpdateHeldInput() noexcept
{
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
