#pragma once

#include "renderer_sdk_frame.hpp"

#include <memory>
#include <string_view>

namespace hs
{
class XessFrameGeneration;

struct XellMode
{
    bool low_latency{};
    // Zero disables the SDK frame limiter. Use this instead of a second limiter.
    std::uint32_t minimum_interval_us{};
};

// Concrete XeLL D3D12 adapter. Construction does not load a DLL or create an SDK
// context. Initialize, mode changes, Sleep and markers run on one owner thread.
// The coordinator selects XeLL exclusively; it must not run beside Reflex.
class XellLatency
{
  public:
    XellLatency();
    ~XellLatency();
    XellLatency(const XellLatency &) = delete;
    XellLatency &operator=(const XellLatency &) = delete;

    [[nodiscard]] Result Initialize(ID3D12Device *device);
    // XeLL requires all GPU activity to have stopped before changing sleep mode.
    // A renderer graph fence alone does not acknowledge an SDK presentation queue.
    [[nodiscard]] Result SetModeAfterCompletion(XellMode mode, bool gpu_idle_acknowledged);
    // Call once before input sampling. SimulationStart is the first marker;
    // SimulationEnd, RenderSubmitStart/End and PresentStart/End are all required.
    // All APIs use the packet's exact presentation_id (XeLL's ABI is uint32_t).
    [[nodiscard]] Result Sleep(const SdkFrameIdentity &identity);
    [[nodiscard]] Result Mark(const SdkFrameIdentity &identity, SdkLatencyMarker marker);
    [[nodiscard]] Result ValidatePresentIdentity(const SdkFrameIdentity &identity) const;
    // Destroy FG first, stop further markers, then acknowledge GPU completion.
    [[nodiscard]] Result ResetAfterCompletion(bool gpu_idle_acknowledged);
    [[nodiscard]] ReconstructionAvailability Availability() const noexcept;
    [[nodiscard]] std::string_view FailureReason() const noexcept;
    [[nodiscard]] bool Initialized() const noexcept;
    [[nodiscard]] bool ConfiguredLowLatency() const noexcept;
    [[nodiscard]] bool RequiresFrameGeneration() const noexcept;

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    friend class XessFrameGeneration;
    // The context and DLL owner remain alive for the entire FG context lifetime.
    [[nodiscard]] Result AttachFrameGeneration(ID3D12Device *device, void *&context,
                                               std::shared_ptr<void> &owner);
    [[nodiscard]] static Result ValidateFrameGeneration(const std::shared_ptr<void> &owner,
                                                       const SdkFrameIdentity *identity);
    static void SetFrameGenerationActive(const std::shared_ptr<void> &owner, bool active);
    static void DetachFrameGeneration(std::shared_ptr<void> &owner);
};
} // namespace hs
