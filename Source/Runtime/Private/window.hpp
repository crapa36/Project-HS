#pragma once

#include "runtime_channels.hpp"

#include <hs/core/result.hpp>
#include <hs/presentation/presentation_catalog.hpp>

#include <Windows.h>

#include <array>
#include <cstdint>
#include <optional>
#include <functional>

namespace hs
{

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
    [[nodiscard]] bool PumpMessages();
    [[nodiscard]] HWND Handle() const noexcept;
    void StopInput() noexcept;
    void BeginSkillRebind(std::uint32_t slot) noexcept;
    void CancelSkillRebind() noexcept;
    [[nodiscard]] bool ConsumeReboundSkillKeys(
        std::array<std::uint16_t, 4> &keys) noexcept;
    [[nodiscard]] Result SetBorderless(bool borderless);
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
    HeldInputState held_{};
    Float2 ui_cursor_normalized_{};
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
