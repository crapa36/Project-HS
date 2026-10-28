#include <hs/runtime/vfx_image_io.hpp>
#include "../Source/Runtime/Private/vfx_typed_entity_surface_commands.hpp"

#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace
{
constexpr std::uint32_t Hash32(std::string_view value)
{
    std::uint32_t hash = 2166136261u;
    for (const unsigned char character : value) hash = (hash ^ character) * 16777619u;
    return hash;
}
constexpr std::uint64_t Hash64(std::string_view value)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char character : value) hash = (hash ^ character) * 1099511628211ull;
    return hash;
}
void Check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

hs::VfxImageData Image()
{
    hs::VfxImageData image;
    image.entity_surface_effect_recipes.push_back({
        Hash64("particle.boss.death"), 5, Hash32("PresentationContextPayload"),
        0, 0, 2.0f, {0, 1}, true});
    image.entity_surface_recipes.push_back({
        1, 0, 7, 0, 13, hs::VfxEntitySurfaceShape::DissolveEdge,
        hs::VfxEntitySurfaceDirection::UpAndOut, 0.0f, 1.0f, 1.5f, .2f,
        {.8f, .3f, .1f, .72f}, 5.5f, .72f, false, true});
    return image;
}

hs::runtime_detail::VfxEntitySurfaceEventCapture Capture(
    std::uint64_t owner_id, std::uint64_t sequence, std::uint32_t seed)
{
    constexpr std::uint64_t render_prefix = 2ull << 60;
    hs::runtime_detail::VfxEntitySurfaceEventCapture capture;
    capture.input.effect_handle = 1;
    capture.input.event_tick = 100;
    capture.input.sequence = sequence;
    capture.input.stable_seed = seed;
    capture.input.quality = hs::VfxQuality::High;
    capture.input.payload = hs::VfxContextPayload{owner_id};
    capture.instance.stable_id = render_prefix | owner_id;
    capture.instance.mesh = hs::RenderMesh::BossFinal;
    capture.instance.asset_id.value = 41;
    capture.instance.material_id.value = 42;
    capture.instance.position = {1, 2, 3};
    capture.instance.scale = {2, 3, 4};
    capture.instance.yaw = .5f;
    capture.instance.animation_time_seconds = 1.25;
    capture.instance.animation_playback_rate = .75f;
    capture.retained_instance = true;
    capture.has_animation_pose = true;
    capture.animation_pose.clip = hs::CharacterAnimationClip::Idle;
    capture.animation_pose.normalized_time = .4f;
    capture.animation_pose.revision = 17;
    capture.generation_lease = std::make_shared<int>(42);
    return capture;
}

void AppendRetainsCapturedOwnerAndPrefix()
{
    auto image = Image();
    auto first = Capture(31, 101, 7);
    auto second = Capture(32, 102, 9);
    const std::array captures{first, second};
    std::vector<hs::VfxEntitySurfaceCommand> result;
    result.reserve(4);
    hs::VfxEntitySurfaceCommand prefix;
    prefix.owner_id = 77;
    result.push_back(prefix);
    const auto capacity = result.capacity();

    hs::runtime_detail::AppendVfxTypedEntitySurfaceCommands(
        image.View(), {}, {}, 120, 0, result);
    auto rejected = Capture(33, 0, 11);
    hs::runtime_detail::AppendVfxTypedEntitySurfaceCommands(
        image.View(), std::span(&rejected, 1), {}, 120, 0, result);
    Check(result.size() == 1 && result[0].owner_id == 77 && result.capacity() == capacity,
          "empty/rejected entity-surface append changed prefix or capacity");

    hs::runtime_detail::AppendVfxTypedEntitySurfaceCommands(
        image.View(), captures, {}, 120, 0, result);
    Check(result.size() == 3 && result.capacity() == capacity &&
          result[1].owner_id == 31 && result[1].event_sequence == 101 &&
          result[1].stable_seed == 7 && result[2].owner_id == 32 &&
          result[2].event_sequence == 102 && result[2].stable_seed == 9,
          "entity-surface append changed event order or captured identity");
    Check(result[1].has_retained_instance &&
          result[1].retained_instance.stable_id == first.instance.stable_id &&
          result[1].retained_instance.mesh == hs::RenderMesh::BossFinal &&
          result[1].retained_instance.asset_id.value == 41 &&
          result[1].retained_instance.material_id.value == 42 &&
          result[1].has_retained_animation_pose &&
          result[1].retained_animation_pose.revision == 17,
          "death surface did not preserve the captured mesh or pose");
}
} // namespace

int RunVfxEntitySurfaceTests()
{
    try
    {
        AppendRetainsCapturedOwnerAndPrefix();
        std::cout << "typed entity-surface append tests passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
