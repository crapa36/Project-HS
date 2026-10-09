#include "renderer_entity_surface.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <tuple>

namespace hs
{
namespace
{
struct alignas(16) GroupHeader
{
    std::uint32_t magic{0x31534645u}; // "EFS1"
    std::uint32_t count{}, byte_count{}, reserved{};
};
struct alignas(16) SurfaceCommand
{
    std::array<float, 4> color_alpha;
    std::array<float, 4> phase_noise_edge_gradient;
    std::array<std::uint32_t, 4> row_seed_direction_shape;
};
static_assert(sizeof(GroupHeader) == 16 && sizeof(SurfaceCommand) == 48);

auto Identity(const VfxEntitySurfaceCommand &c) noexcept
{
    return std::tuple(c.render_instance_id, c.owner_id, c.status_episode_generation,
                      c.event_sequence, c.output_index);
}
Result Invalid(const char *message)
{
    return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", message);
}
bool Valid(const VfxEntitySurfaceCommand &c) noexcept
{
    const std::array values{c.color.x, c.color.y, c.color.z, c.hdr, c.alpha,
                            c.normalized_phase, c.noise_scale, c.edge_width, c.gradient_age};
    return c.render_instance_id != 0 && c.owner_id != 0 &&
        c.owner_until_tick > c.owner_started_tick &&
        c.current_tick >= c.owner_started_tick && c.current_tick < c.owner_until_tick &&
        std::ranges::all_of(values, [](float value) { return std::isfinite(value); }) &&
        c.color.x >= 0 && c.color.y >= 0 && c.color.z >= 0 && c.hdr >= 0 &&
        c.alpha >= 0 && c.alpha <= 1 && c.normalized_phase >= 0 && c.normalized_phase <= 1 &&
        c.gradient_age >= 0 && c.gradient_age <= 1 && c.noise_scale > 0 && c.edge_width > 0 &&
        c.shape <= VfxEntitySurfaceShape::DissolveEdge && c.direction <= VfxEntitySurfaceDirection::UpAndOut;
}
} // namespace

Result EntitySurfaceFrame::Build(VfxEntitySurfaceInputs commands)
{
    bindings_.clear();
    bytes_.clear();
    if (commands.empty()) return Result::Success();
    std::vector<const VfxEntitySurfaceCommand *> ordered;
    ordered.reserve(commands.size());
    for (const auto &command : commands)
    {
        if (!Valid(command)) return Invalid("Invalid actual entity surface command or owner clock.");
        ordered.push_back(&command);
    }
    std::ranges::sort(ordered, [](const auto *a, const auto *b) { return Identity(*a) < Identity(*b); });
    for (std::size_t i = 1; i < ordered.size(); ++i)
        if (Identity(*ordered[i - 1]) == Identity(*ordered[i]))
            return Invalid("An entity surface source output was submitted twice.");

    for (std::size_t first = 0; first < ordered.size();)
    {
        std::size_t end = first + 1;
        while (end < ordered.size() && ordered[end]->render_instance_id == ordered[first]->render_instance_id) ++end;
        const auto count = end - first;
        constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
        if (count > (maximum - sizeof(GroupHeader)) / sizeof(SurfaceCommand))
            return Invalid("Entity surface group exceeds GPU byte addressing.");
        const auto group_bytes = sizeof(GroupHeader) + count * sizeof(SurfaceCommand);
        if (bytes_.size() >= maximum || group_bytes > maximum - bytes_.size())
            return Invalid("Entity surface payload exceeds GPU byte addressing.");
        const auto offset = bytes_.size();
        bindings_.push_back({ordered[first]->render_instance_id, static_cast<std::uint32_t>(offset + 1)});
        bytes_.resize(offset + group_bytes);
        const GroupHeader header{0x31534645u, static_cast<std::uint32_t>(count),
                                 static_cast<std::uint32_t>(group_bytes), 0};
        std::memcpy(bytes_.data() + offset, &header, sizeof(header));
        for (std::size_t i = first; i < end; ++i)
        {
            const auto &source = *ordered[i];
            const SurfaceCommand command{
                {source.color.x * source.hdr, source.color.y * source.hdr, source.color.z * source.hdr, source.alpha},
                {source.normalized_phase, source.noise_scale, source.edge_width, source.gradient_age},
                {source.gradient_row, source.stable_seed, static_cast<std::uint32_t>(source.direction),
                 static_cast<std::uint32_t>(source.shape)}};
            std::memcpy(bytes_.data() + offset + sizeof(header) + (i - first) * sizeof(command),
                        &command, sizeof(command));
        }
        first = end;
    }
    return Result::Success();
}

std::uint32_t EntitySurfaceFrame::InstancePayload(std::uint64_t render_id) const noexcept
{
    const auto found = std::ranges::lower_bound(bindings_, render_id, {}, &Binding::render_id);
    return found != bindings_.end() && found->render_id == render_id ? found->relative_offset_plus_one : 0;
}
} // namespace hs
