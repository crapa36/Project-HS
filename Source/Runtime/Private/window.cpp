#include "window.hpp"
#include "camera_pose.hpp"
#include <hs/presentation/projector.hpp>

#include <hidusage.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace hs
{
namespace
{

constexpr wchar_t kWindowClass[] = L"ProjectHS.Window";

Result QueryActiveDisplayConfig(std::vector<DISPLAYCONFIG_PATH_INFO> &paths,
                                std::vector<DISPLAYCONFIG_MODE_INFO> &modes)
{
    LONG queried = ERROR_INSUFFICIENT_BUFFER;
    // CCD requires a fresh size query when hot-plug changes its array size.
    for (unsigned capture = 0; capture < 3 && queried == ERROR_INSUFFICIENT_BUFFER; ++capture)
    {
        UINT32 path_count{}, mode_count{};
        queried = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count);
        if (queried != ERROR_SUCCESS) break;
        if (!path_count) { paths.clear(); modes.clear(); return Result::Success(); }
        paths.resize(path_count); modes.resize(mode_count);
        queried = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(),
                                     &mode_count, modes.data(), nullptr);
        if (queried == ERROR_SUCCESS) { paths.resize(path_count); modes.resize(mode_count); }
    }
    if (queried != ERROR_SUCCESS)
        return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                               "Cannot query desktop placement: " + std::to_string(queried));
    return Result::Success();
}

} // namespace

Result EnumerateWindowDisplayPlacements(std::vector<RendererDisplayOutput> &outputs)
{
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (auto result = QueryActiveDisplayConfig(paths, modes); !result) return result;
    std::vector<RendererDisplayOutput> candidate;
    for (const auto &path:paths)
    {
        const auto index=path.sourceInfo.modeInfoIdx;
        if (index>=modes.size()||modes[index].infoType!=DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE||
            modes[index].id!=path.sourceInfo.id||modes[index].adapterId.HighPart!=path.sourceInfo.adapterId.HighPart||
            modes[index].adapterId.LowPart!=path.sourceInfo.adapterId.LowPart)
            return Result::Failure(ErrorCode::InvalidState,"hs_runtime","Desktop placement source mode is invalid.");
        DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};
        name.header={DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,sizeof(name),path.sourceInfo.adapterId,path.sourceInfo.id};
        const auto queried=DisplayConfigGetDeviceInfo(&name.header);
        if (queried!=ERROR_SUCCESS)
            return Result::Failure(ErrorCode::InvalidState,"hs_runtime","Cannot query desktop source name: "+std::to_string(queried));
        const std::wstring_view device_name(name.viewGdiDeviceName);
        const int count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,device_name.data(),int(device_name.size()),nullptr,0,nullptr,nullptr);
        if (count<=0) return Result::Failure(ErrorCode::InvalidState,"hs_runtime","Desktop source name is not valid UTF-16.");
        std::string utf8(std::size_t(count),'\0');
        if (!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,device_name.data(),int(device_name.size()),utf8.data(),count,nullptr,nullptr))
            return Result::Failure(ErrorCode::InvalidState,"hs_runtime","Cannot encode desktop source name.");
        const auto &source=modes[index].sourceMode;
        if (!source.width||!source.height)
            return Result::Failure(ErrorCode::InvalidState,"hs_runtime","Active desktop source has an empty rectangle.");
        RendererDisplayOutput display;
        display.id=std::format("dxgi:{:08x}{:08x}:{}",std::uint32_t(path.sourceInfo.adapterId.HighPart),path.sourceInfo.adapterId.LowPart,utf8);
        display.left=source.position.x;display.top=source.position.y;display.width=source.width;display.height=source.height;
        if (std::ranges::find(candidate,display.id,&RendererDisplayOutput::id)==candidate.end())
            candidate.push_back(std::move(display));
    }
    outputs.swap(candidate);return Result::Success();
}

Result QueryWindowDisplayRefresh(HWND window, double &refresh_hz)
{
    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!window || !GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST),
                                    reinterpret_cast<MONITORINFO *>(&monitor)))
        return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                               "Cannot identify the actual window monitor for refresh timing.");
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (auto result = QueryActiveDisplayConfig(paths, modes); !result) return result;
    std::optional<DISPLAYCONFIG_RATIONAL> timing;
    for (const auto &path : paths)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source),
                         path.sourceInfo.adapterId, path.sourceInfo.id};
        const auto queried = DisplayConfigGetDeviceInfo(&source.header);
        if (queried != ERROR_SUCCESS)
            return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                   "Cannot query refresh timing source: " + std::to_string(queried));
        if (std::wstring_view(source.viewGdiDeviceName) != std::wstring_view(monitor.szDevice)) continue;
        if (!path.targetInfo.targetAvailable)
            return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                   "The actual window monitor target is currently unavailable.");
        const auto current = path.targetInfo.refreshRate;
        if (!current.Numerator || !current.Denominator)
            return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                   "The actual window monitor has no valid active refresh rational.");
        if (timing && static_cast<std::uint64_t>(timing->Numerator) * current.Denominator !=
                          static_cast<std::uint64_t>(current.Numerator) * timing->Denominator)
            return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                   "The actual window monitor maps to different active refresh timings.");
        timing = current;
    }
    if (!timing)
        return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                               "The actual window monitor has no matching active display path.");
    refresh_hz = static_cast<double>(timing->Numerator) / timing->Denominator;
    return Result::Success();
}

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
    dpi_error_ = ERROR_SUCCESS;
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
        if (!AdjustWindowRectExForDpi(&rectangle, style, FALSE, 0, GetDpiForSystem()))
            return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                std::format("Cannot calculate the initial DPI window frame: {}", GetLastError()));
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
        {HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_KEYBOARD, 0, window_},
        {HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_MOUSE, 0, window_},
    };
    if (!RegisterRawInputDevices(devices, static_cast<UINT>(std::size(devices)),
                                 sizeof(RAWINPUTDEVICE)))
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                               std::format("RegisterRawInputDevices failed: {}", GetLastError()));
    }

    RECT client{};
    if (!GetClientRect(window_, &client) || client.right <= 0 || client.bottom <= 0)
        return Result::Failure(ErrorCode::InvalidState, "hs_runtime", "Cannot query the initial window client extent.");
    client_width_ = static_cast<std::uint32_t>(client.right);
    client_height_ = static_cast<std::uint32_t>(client.bottom);
    if (visible)
    {
        ShowWindow(window_, SW_SHOW);
    }
    if (dpi_error_)
        return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
            std::format("Initial DPI window placement failed: {}", dpi_error_));
    return Result::Success();
}

bool Window::PumpMessages(Result *failure, bool sample_input)
{
    if (failure) *failure = Result::Success();
    MSG message{};
    while (!dpi_error_ && runtime_detail::PeekMainMessage(message, sample_input))
    {
        if (message.message == WM_QUIT)
        {
            // PeekMessage can dispatch a synchronous DPI message before returning WM_QUIT.
            if (dpi_error_) break;
            return false;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (dpi_error_)
    {
        if (failure) *failure = Result::Failure(ErrorCode::InvalidState, "hs_runtime",
            std::format("DPI window placement failed: {}", dpi_error_));
        return false;
    }

    if (sample_input && accepting_input_ && window_)
    {
        POINT cursor{};
        if (GetCursorPos(&cursor) && ScreenToClient(window_, &cursor))
        {
            const auto normalized_x =
                static_cast<float>(cursor.x) / static_cast<float>(client_width_) * 2.0f - 1.0f;
            const auto normalized_y =
                1.0f - static_cast<float>(cursor.y) / static_cast<float>(client_height_) * 2.0f;
            held_.cursor_normalized = {normalized_x, normalized_y};
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

void Window::SetUiClickHandler(std::function<void(Float2, ViewExtent)> handler)
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

    RECT previous_rect{};
    if (!GetWindowRect(window_, &previous_rect))
        return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                               "Cannot query the current window rectangle.");
    RECT target{};
    const DWORD current_style = static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_STYLE));
    const DWORD style = (current_style & ~(WS_OVERLAPPEDWINDOW | WS_POPUP)) |
                        (borderless ? WS_POPUP : WS_OVERLAPPEDWINDOW);
    if (borderless)
    {
        MONITORINFO monitor{sizeof(MONITORINFO)};
        if (!GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor))
            return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                   "Cannot query the target monitor.");
        target = monitor.rcMonitor;
    }
    else
    {
        if (has_windowed_rect_)
        {
            target = windowed_rect_;
        }
        else
        {
            target = {0, 0, static_cast<LONG>(windowed_client_width_),
                      static_cast<LONG>(windowed_client_height_)};
            if (!AdjustWindowRectExForDpi(&target, style, FALSE, 0, GetDpiForWindow(window_)))
                return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                    std::format("Cannot calculate the restored DPI window frame: {}", GetLastError()));
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
    {
        const auto apply_error = GetLastError();
        SetLastError(ERROR_SUCCESS);
        const auto restored_style = SetWindowLongPtrW(window_, GWL_STYLE, previous);
        const auto style_error = restored_style == 0 ? GetLastError() : ERROR_SUCCESS;
        const auto restored_position = SetWindowPos(
            window_, nullptr, previous_rect.left, previous_rect.top,
            previous_rect.right - previous_rect.left,
            previous_rect.bottom - previous_rect.top,
            SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        if (style_error != ERROR_SUCCESS || !restored_position)
            return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                   std::format("SetWindowPos failed: {}; window rollback failed: {} / {}",
                                               apply_error, style_error, GetLastError()));
        return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                               std::format("SetWindowPos failed: {}", apply_error));
    }
    if (borderless)
    {
        windowed_rect_ = previous_rect;
        has_windowed_rect_ = true;
    }
    borderless_ = borderless;
    return Result::Success();
}

Result Window::ApplyDisplayPlacement(const GraphicsDisplayId &output, bool borderless,
                                     std::uint32_t client_width, std::uint32_t client_height)
{
    if (!window_ || !client_width || !client_height ||
        client_width > static_cast<std::uint32_t>(std::numeric_limits<LONG>::max()) ||
        client_height > static_cast<std::uint32_t>(std::numeric_limits<LONG>::max()))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_runtime",
                               "Display placement requires a window and valid client extent.");
    RECT monitor_rect{};
    if (output.value == "auto")
    {
        MONITORINFO monitor{sizeof(MONITORINFO)};
        if (!GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor))
            return Result::Failure(ErrorCode::InvalidState, "hs_runtime", "Cannot query the current display.");
        monitor_rect = monitor.rcMonitor;
    }
    else
    {
        std::vector<RendererDisplayOutput> displays;
        if (auto enumerated = EnumerateWindowDisplayPlacements(displays); !enumerated) return enumerated;
        const auto selected = std::ranges::find(displays, output.value, &RendererDisplayOutput::id);
        if (selected == displays.end())
            return Result::Failure(ErrorCode::InvalidArgument, "hs_runtime", "Unknown display output ID: " + output.value);
        const auto right = static_cast<std::int64_t>(selected->left) + selected->width;
        const auto bottom = static_cast<std::int64_t>(selected->top) + selected->height;
        if (right > std::numeric_limits<LONG>::max() || bottom > std::numeric_limits<LONG>::max())
            return Result::Failure(ErrorCode::InvalidState, "hs_runtime", "Display rectangle exceeds native coordinates.");
        monitor_rect = {selected->left, selected->top, static_cast<LONG>(right), static_cast<LONG>(bottom)};
    }
    RECT previous_rect{};
    if (!GetWindowRect(window_, &previous_rect))
        return Result::Failure(ErrorCode::InvalidState, "hs_runtime", "Cannot query the current window rectangle.");
    const DWORD current_style = static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_STYLE));
    const DWORD style = (current_style & ~(WS_OVERLAPPEDWINDOW | WS_POPUP)) |
                        (borderless ? WS_POPUP : WS_OVERLAPPEDWINDOW);
    const auto windowed_rect = [&](UINT dpi, RECT &target) {
        target = {0, 0, static_cast<LONG>(client_width), static_cast<LONG>(client_height)};
        if (!AdjustWindowRectExForDpi(&target, style, FALSE, 0, dpi)) return false;
        const auto outer_width = static_cast<std::int64_t>(target.right) - target.left;
        const auto outer_height = static_cast<std::int64_t>(target.bottom) - target.top;
        const auto left = static_cast<std::int64_t>(monitor_rect.left) +
            (static_cast<std::int64_t>(monitor_rect.right) - monitor_rect.left - outer_width) / 2;
        const auto top = static_cast<std::int64_t>(monitor_rect.top) +
            (static_cast<std::int64_t>(monitor_rect.bottom) - monitor_rect.top - outer_height) / 2;
        const auto right = left + outer_width;
        const auto bottom = top + outer_height;
        if (outer_width <= 0 || outer_height <= 0 ||
            outer_width > std::numeric_limits<LONG>::max() ||
            outer_height > std::numeric_limits<LONG>::max() ||
            left < std::numeric_limits<LONG>::min() || top < std::numeric_limits<LONG>::min() ||
            right > std::numeric_limits<LONG>::max() || bottom > std::numeric_limits<LONG>::max())
        {
            SetLastError(ERROR_ARITHMETIC_OVERFLOW);
            return false;
        }
        target = {static_cast<LONG>(left), static_cast<LONG>(top),
                  static_cast<LONG>(right), static_cast<LONG>(bottom)};
        return true;
    };
    const auto previous_dpi = GetDpiForWindow(window_);
    RECT target = monitor_rect;
    if (!borderless && !windowed_rect(previous_dpi, target))
        return Result::Failure(ErrorCode::InvalidState, "hs_runtime", "Cannot calculate the requested window frame.");
    SetLastError(ERROR_SUCCESS);
    const auto previous_style = SetWindowLongPtrW(window_, GWL_STYLE, style);
    if (previous_style == 0 && GetLastError() != ERROR_SUCCESS)
        return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                               std::format("SetWindowLongPtrW failed: {}", GetLastError()));
    const auto rollback = [&](DWORD apply_error) {
        SetLastError(ERROR_SUCCESS);
        const auto restored_style = SetWindowLongPtrW(window_, GWL_STYLE, previous_style);
        const auto style_error = restored_style == 0 ? GetLastError() : ERROR_SUCCESS;
        const auto restored = SetWindowPos(window_, nullptr, previous_rect.left, previous_rect.top,
            previous_rect.right - previous_rect.left, previous_rect.bottom - previous_rect.top,
            SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
        if (style_error != ERROR_SUCCESS || !restored)
            return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                std::format("Display placement failed: {}; rollback failed: {} / {}",
                            apply_error, style_error, GetLastError()));
        return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                               std::format("Display placement failed: {}", apply_error));
    };
    if (!SetWindowPos(window_, nullptr, target.left, target.top, target.right - target.left,
            target.bottom - target.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE))
        return rollback(GetLastError());
    if (dpi_error_) return rollback(dpi_error_);
    // Moving across monitors can change the HWND DPI. Recalculate the frame at
    // that DPI so the requested client pixel extent survives the display move.
    const auto target_dpi = GetDpiForWindow(window_);
    if (!borderless && target_dpi != previous_dpi)
    {
        if (!windowed_rect(target_dpi, target)) return rollback(GetLastError());
        if (!SetWindowPos(window_, nullptr, target.left, target.top, target.right - target.left,
                target.bottom - target.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE))
            return rollback(GetLastError());
        if (dpi_error_) return rollback(dpi_error_);
    }
    if (!borderless)
    {
        RECT actual_client{};
        if (!GetClientRect(window_, &actual_client)) return rollback(GetLastError());
        const auto actual_width = actual_client.right - actual_client.left;
        const auto actual_height = actual_client.bottom - actual_client.top;
        if (actual_width != static_cast<LONG>(client_width) ||
            actual_height != static_cast<LONG>(client_height))
        {
            const auto restored = rollback(ERROR_INVALID_DATA);
            return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                std::format("Window placement requested a {}x{} client extent but produced {}x{}; {}",
                    client_width, client_height, actual_width, actual_height, restored.Message()));
        }
    }
    if (borderless && !borderless_)
    {
        windowed_rect_ = previous_rect;
        has_windowed_rect_ = true;
    }
    else if (!borderless)
    {
        windowed_rect_ = target;
        has_windowed_rect_ = true;
    }
    windowed_client_width_ = client_width;
    windowed_client_height_ = client_height;
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
    case WM_DPICHANGED:
    {
        if (!lparam || !LOWORD(wparam) || !HIWORD(wparam))
        {
            dpi_error_ = ERROR_INVALID_PARAMETER;
            return 0;
        }
        auto target = *reinterpret_cast<const RECT *>(lparam);
        const bool popup = (GetWindowLongPtrW(window, GWL_STYLE) & WS_POPUP) != 0;
        if (popup)
        {
            MONITORINFO monitor{sizeof(monitor)};
            if (!GetMonitorInfoW(MonitorFromRect(&target, MONITOR_DEFAULTTONEAREST), &monitor))
            {
                dpi_error_ = GetLastError();
                if (!dpi_error_) dpi_error_ = ERROR_GEN_FAILURE;
                return 0;
            }
            target = monitor.rcMonitor;
        }
        const auto width = static_cast<std::int64_t>(target.right) - target.left;
        const auto height = static_cast<std::int64_t>(target.bottom) - target.top;
        if (width <= 0 || height <= 0 || width > std::numeric_limits<int>::max() ||
            height > std::numeric_limits<int>::max())
        {
            dpi_error_ = ERROR_INVALID_PARAMETER;
            return 0;
        }
        if (!SetWindowPos(window, nullptr, target.left, target.top, static_cast<int>(width),
                static_cast<int>(height), SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED))
        {
            dpi_error_ = GetLastError();
            if (!dpi_error_) dpi_error_ = ERROR_GEN_FAILURE;
            return 0;
        }
        if (!popup) { windowed_rect_ = target; has_windowed_rect_ = true; }
        return 0;
    }
    case WM_DISPLAYCHANGE:
        (void)channels_->window_messages.TryPush(
            {message, static_cast<std::uintptr_t>(wparam),
             static_cast<std::intptr_t>(lparam)});
        return 0;
    case WM_MOUSEMOVE:
        if (ui_hover_handler_)
        {
            const auto x = static_cast<float>(static_cast<short>(LOWORD(lparam)));
            const auto y = static_cast<float>(static_cast<short>(HIWORD(lparam)));
            ui_hover_handler_(ReferenceUiCursor({x, y}, {client_width_, client_height_}));
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (accepting_input_ && ui_click_handler_ && GetForegroundWindow() == window_ &&
            !channels_->devtools_capture_mouse.load(std::memory_order_acquire))
        {
            const auto x = static_cast<float>(static_cast<short>(LOWORD(lparam)));
            const auto y = static_cast<float>(static_cast<short>(HIWORD(lparam)));
            ui_click_handler_({x, y}, {client_width_, client_height_});
        }
        return 0;
    case WM_INPUT:
        if (accepting_input_ && GET_RAWINPUT_CODE_WPARAM(wparam) == RIM_INPUT &&
            GetForegroundWindow() == window_)
        {
            HandleRawInput(reinterpret_cast<HRAWINPUT>(lparam));
        }
        // DefWindowProc must clean up foreground raw input even when discarded.
        return GET_RAWINPUT_CODE_WPARAM(wparam) == RIM_INPUT
                   ? DefWindowProcW(window, message, wparam, lparam) : 0;
    case WM_KILLFOCUS:
    {
        // System messages can overtake deferred input while the native token is
        // parked. Those queued values belong to the closing focus epoch.
        MSG stale{};
        while (PeekMessageW(&stale, window, WM_INPUT, WM_INPUT, PM_REMOVE))
            if (GET_RAWINPUT_CODE_WPARAM(stale.wParam) == RIM_INPUT)
                DefWindowProcW(window, stale.message, stale.wParam, stale.lParam);
        while (PeekMessageW(&stale, window, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE)) {}
        while (PeekMessageW(&stale, window, WM_MOUSEFIRST, WM_MOUSELAST, PM_REMOVE)) {}
        held_ = {};
        channels_->held_input.store(held_, std::memory_order_release);
        channels_->focus_epoch.fetch_add(1, std::memory_order_release);
        channels_->NotifyMain();
        return 0;
    }
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
        // Keep the HWND alive until the render thread drains and releases DXGI.
        // The owning Window destructor runs after RunApplication joins it.
        channels_->stop_requested.store(true, std::memory_order_release);
        channels_->NotifyMain();
        return 0;
    case WM_DESTROY:
        window_ = nullptr;
        channels_->stop_requested.store(true, std::memory_order_release);
        channels_->NotifyMain();
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
