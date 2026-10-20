#pragma once

#include <hs/core/graphics_settings.hpp>
#include <hs/core/result.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace hs
{

// Cause is part of the transport contract so recovery can be distinguished
// from an ordinary user edit without carrying UI or renderer state into Core.
enum class GraphicsSettingsRequestCause : std::uint8_t
{
    User,
    System,
    Recovery,
};

struct GraphicsSettingsRequest
{
    std::uint64_t generation{};
    GraphicsSettingsRequestCause cause{GraphicsSettingsRequestCause::User};
    GraphicsSettings requested{};
    // Canonical requested settings retain the user's "auto" intent.  Runtime
    // may carry the concrete output selected by the native window/display
    // path separately for this apply transaction.
    std::optional<GraphicsDisplayId> resolved_display_output;
};

enum class SettingsApplyState : std::uint8_t
{
    Pending,
    Preparing,
    Applied,
    AwaitingDisplayConfirmation,
    Confirmed,
    Rejected,
    Restored,
    Recovering,
};

enum class GraphicsCapabilityReason : std::uint8_t
{
    None,
    Unsupported,
    Unavailable,
    MissingProvider,
    MissingDisplay,
    Incompatible,
    BudgetLimited,
    Rejected,
};

struct GraphicsOptionAvailability
{
    GraphicsModeId option{};
    bool available{};
    GraphicsCapabilityReason reason{GraphicsCapabilityReason::None};
};

struct GraphicsProviderAvailability
{
    GraphicsProviderId provider{};
    bool available{};
    GraphicsCapabilityReason reason{GraphicsCapabilityReason::None};
};

struct GraphicsDisplayAvailability
{
    GraphicsDisplayId display{};
    bool available{};
    GraphicsCapabilityReason reason{GraphicsCapabilityReason::None};
};

struct GraphicsCapabilitiesView
{
    std::vector<GraphicsOptionAvailability> options;
    std::vector<GraphicsProviderAvailability> providers;
    std::vector<GraphicsDisplayAvailability> displays;
};

// This is intentionally a compact, portable summary. Detailed resolved graph
// and provider state stays in the renderer-owned status types.
struct GraphicsSettingsResolutionSummary
{
    GraphicsPreset resolved_profile{GraphicsPreset::Custom};
    GraphicsProviderId native_provider{};
    bool used_native_fallback{};
    GraphicsCapabilityReason native_fallback_reason{GraphicsCapabilityReason::None};
};

struct SettingsApplyResult
{
    std::uint64_t generation{};
    SettingsApplyState state{SettingsApplyState::Pending};
    GraphicsSettings requested{};
    GraphicsSettings actual_applied{};
    bool actual_applied_available{};
    GraphicsSettingsResolutionSummary resolution{};
    bool confirmation_required{};
    ErrorCode error{ErrorCode::None};
};

} // namespace hs
