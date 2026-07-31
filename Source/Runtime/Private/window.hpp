#pragma once

#include "runtime_channels.hpp"

#include <hs/core/result.hpp>

#include <Windows.h>

#include <array>
#include <cstdint>

namespace hs
{

class Window
{
  public:
    explicit Window(RuntimeChannels &channels) noexcept;
    ~Window();

    Window(const Window &) = delete;
    Window &operator=(const Window &) = delete;

    [[nodiscard]] Result Create(std::uint32_t width, std::uint32_t height, bool visible,
                                bool borderless);
    [[nodiscard]] bool PumpMessages();
    [[nodiscard]] HWND Handle() const noexcept;
    void StopInput() noexcept;

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
    Sequence input_sequence_{};
    bool accepting_input_{true};
    bool forward_{};
    bool backward_{};
    bool left_{};
    bool right_{};
    std::uint32_t client_width_{1};
    std::uint32_t client_height_{1};
};

} // namespace hs
