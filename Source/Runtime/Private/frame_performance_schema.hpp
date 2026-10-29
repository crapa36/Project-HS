#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace hs::runtime_detail
{
inline constexpr std::array kFramePerformanceTaskNames{
    std::string_view{"vfx_event_drain"}, std::string_view{"vfx_snapshot_acquire"},
    std::string_view{"vfx_session_dispatch"}, std::string_view{"vfx_legacy_expand"},
    std::string_view{"vfx_frame_adapter"}, std::string_view{"vfx_entity_surface"},
    std::string_view{"vfx_movement"}, std::string_view{"vfx_ballistic"},
    std::string_view{"vfx_status"}, std::string_view{"vfx_ground"},
    std::string_view{"vfx_local_outputs"}, std::string_view{"vfx_ground_events"},
    std::string_view{"vfx_flashes"}, std::string_view{"vfx_motes"},
    std::string_view{"vfx_mesh"}, std::string_view{"vfx_ribbons"},
    std::string_view{"vfx_spawn_sort"}, std::string_view{"vfx_screen"},
    std::string_view{"vfx_volume"}, std::string_view{"vfx_light"},
    std::string_view{"vfx_distortion"}, std::string_view{"vfx_decal"},
    std::string_view{"vfx_fresnel"}, std::string_view{"vfx_profile_prepare"}};

inline constexpr std::size_t kVfxProfilePrepareTaskIndex =
    kFramePerformanceTaskNames.size() - 1;
static_assert(kFramePerformanceTaskNames[kVfxProfilePrepareTaskIndex] == "vfx_profile_prepare");

inline std::string FramePerformanceCsvHeader()
{
    std::string result;
    const auto append = [&](std::string_view name) {
        if (!result.empty()) result.push_back(',');
        result.append(name);
    };
#define HS_FRAME_PERFORMANCE_FIELD(name, expression) append(#name);
#define HS_FRAME_PERFORMANCE_TASKS() \
    for (const auto task : kFramePerformanceTaskNames) \
    { \
        std::string name(task); \
        name += "_us"; \
        append(name); \
        name.resize(task.size()); \
        name += "_calls"; \
        append(name); \
    }
#include "frame_performance_fields.inc"
#undef HS_FRAME_PERFORMANCE_TASKS
#undef HS_FRAME_PERFORMANCE_FIELD
    return result;
}
}
