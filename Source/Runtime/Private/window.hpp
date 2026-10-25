#pragma once

#include "runtime_channels.hpp"

#include <hs/core/result.hpp>
#include <hs/presentation/presentation_catalog.hpp>

#include <Windows.h>

#include <array>
#include <cstdint>
#include <optional>
#include <functional>
#include <vector>

namespace hs
{
namespace runtime_detail
{
inline bool PeekMainMessage(MSG &message, bool sample_input, UINT flags = PM_REMOVE) noexcept
{
    if (sample_input) return PeekMessageW(&message, nullptr, 0, 0, flags) != 0;
    // Keep raw/key/client-mouse input for the renderer's sampling token. System
    // close/focus/DPI/size messages remain serviceable while that token is parked.
    return PeekMessageW(&message, nullptr, 0, WM_INPUT - 1, flags) ||
        PeekMessageW(&message, nullptr, WM_KEYLAST + 1, WM_MOUSEFIRST - 1, flags) ||
        PeekMessageW(&message, nullptr, WM_MOUSELAST + 1, 0xffffffffu, flags);
}
inline DWORD MainMessageMask(bool input_gated) noexcept
{ return input_gated ? (QS_ALLINPUT & ~QS_INPUT) : QS_ALLINPUT; }
inline DWORD MainMessageFlags(bool input_gated) noexcept
{
    // System peeking has marked deferred input old. Do not repeatedly wake on
    // that retained input; the native request event restores the normal mask.
    return input_gated ? 0 : MWMO_INPUTAVAILABLE;
}
}

// Desktop placement only: IDs and physical rectangles are valid; native HDR
// and presentation modes are queried by the initialized renderer separately.
// This Win32 query does not create a DXGI object before vendor initialization.
[[nodiscard]] Result EnumerateWindowDisplayPlacements(std::vector<RendererDisplayOutput> &);
// Current active CCD timing for the HWND's actual monitor. This read-only Win32
// query does not create DXGI objects or substitute an assumed refresh frequency.
[[nodiscard]] Result QueryWindowDisplayRefresh(HWND window, double &refresh_hz);

class Window
{
  public:
    Window(RuntimeChannels &channels,
           std::array<std::uint16_t, 4> skill_virtual_keys,
           PresentationCamera camera) noexcept;
    ~Window();

    Window(const Window &) = delete;
    Window &operator=(const Window &) = delete;

    [[nodiscard]] Result Create(std::uint32_t width, std::uint32_t height, bool visible,
                                bool borderless);
    [[nodiscard]] bool PumpMessages(Result *failure = nullptr, bool sample_input = true);
    [[nodiscard]] HWND Handle() const noexcept;
    void StopInput() noexcept;
    void BeginSkillRebind(std::uint32_t slot) noexcept;
    void CancelSkillRebind() noexcept;
    [[nodiscard]] bool ConsumeReboundSkillKeys(
        std::array<std::uint16_t, 4> &keys) noexcept;
    [[nodiscard]] Result SetBorderless(bool borderless);
    // UI/window owner thread; complete placement before renderer request publication.
    [[nodiscard]] Result ApplyDisplayPlacement(const GraphicsDisplayId &output, bool borderless,
                                               std::uint32_t client_width,
                                               std::uint32_t client_height);
    void SetUiClickHandler(std::function<void(Float2)> handler);
    void SetUiHoverHandler(std::function<void(Float2)> handler);

  private:
    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wparam,
                                            LPARAM lparam);
    LRESULT HandleMessage(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    void HandleRawInput(HRAWINPUT input);
    void UpdateHeldInput() noexcept;
    void PushAction(GameAction action, EdgeKind kind) noexcept;

    RuntimeChannels *channels_{};
    HWND window_{};
    HINSTANCE instance_{};
    DWORD dpi_error_{};
    HeldInputState held_{};
    std::array<std::uint16_t, 4> skill_virtual_keys_{};
    PresentationCamera camera_{};
    std::optional<std::uint8_t> pending_rebind_slot_;
    std::optional<std::array<std::uint16_t, 4>> rebound_skill_keys_;
    std::function<void(Float2)> ui_click_handler_;
    std::function<void(Float2)> ui_hover_handler_;
    Sequence input_sequence_{};
    bool accepting_input_{true};
    bool borderless_{};
    bool has_windowed_rect_{};
    RECT windowed_rect_{};
    std::uint32_t windowed_client_width_{1280};
    std::uint32_t windowed_client_height_{720};
    std::uint32_t client_width_{1};
    std::uint32_t client_height_{1};
};

} // namespace hs
