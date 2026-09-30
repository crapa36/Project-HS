#include "vfx_persistent_state.hpp"

#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace
{
void Check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

hs::VfxPersistentInput Record(std::uint64_t id, hs::VfxEffectHandle effect, float age)
{
    hs::VfxPersistentInput input;
    input.stable_id = id;
    input.effect_handle = effect;
    input.normalized_age = age;
    return input;
}

void TestAuthoritativePresence()
{
    hs::renderer_detail::VfxPersistentState state;
    std::string error;
    const std::array inputs{Record(12, 9, .5f), Record(7, 8, 1.0f), Record(7, 3, 0.0f)};
    Check(state.Update(inputs, error), "valid frame rejected");
    const auto active = state.Active();
    Check(active.size() == 3, "same owner must retain different effects");
    Check(active[0].stable_id == 7 && active[0].effect_handle == 3 &&
          active[1].stable_id == 7 && active[1].effect_handle == 8 &&
          active[2].stable_id == 12, "records must sort deterministically by compound key");
    const std::array remaining{Record(7, 8, .2f)};
    Check(state.Update(remaining, error), "replacement frame rejected");
    Check(state.Active().size() == 1 && state.Active()[0].effect_handle == 8,
          "omitted effects must disappear in this frame");
    Check(state.Active()[0].normalized_age == .2f, "authoritative age can rewind");
    Check(state.Update({}, error) && state.Active().empty(), "empty frame must remove every effect immediately");
    Check(state.Update(inputs, error) && state.Active().size() == 3, "omitted identity can reappear");
}

void TestInvalidFrameIsAtomic()
{
    hs::renderer_detail::VfxPersistentState state;
    std::string error;
    const std::array original{Record(4, 2, .4f)};
    Check(state.Update(original, error), "initial frame rejected");
    const auto reject = [&](const hs::VfxPersistentInput &invalid)
    {
        const std::array candidate{Record(1, 0, .1f), invalid};
        Check(!state.Update(candidate, error) && !error.empty(), "invalid frame must report failure");
        Check(state.Active().size() == 1 && state.Active()[0].stable_id == 4 &&
              state.Active()[0].effect_handle == 2 && state.Active()[0].normalized_age == .4f,
              "invalid frame changed prior state");
    };
    reject(Record(1, 0, .8f)); // Duplicate identity, despite different age.
    reject(Record(2, hs::kInvalidVfxEffectHandle, .5f));
    reject(Record(2, 3, -.001f));
    reject(Record(2, 3, 1.001f));
    reject(Record(2, 3, std::numeric_limits<float>::quiet_NaN()));
    reject(Record(2, 3, std::numeric_limits<float>::infinity()));
    reject(Record(2, 3, -std::numeric_limits<float>::infinity()));
    Check(state.Update(original, error) && error.empty(), "successful update must clear prior error");
}

void TestGeometryAndTransformsAreRetained()
{
    hs::renderer_detail::VfxPersistentState state;
    std::string error;
    auto input = Record(42, 6, .625f);
    input.current_transform[12] = 17.0f;
    input.previous_transform[12] = -3.0f;
    input.state_flags = 0x35;
    input.stable_seed = 109;
    input.quality = hs::VfxQuality::Low;
    hs::VfxRingGapsPayload geometry;
    geometry.center = {2.0f, 3.0f, 4.0f};
    geometry.inner_radius = 2.5f;
    geometry.outer_radius = 7.25f;
    geometry.gap_half_width_degrees = 12.0f;
    geometry.lifetime01 = .75f;
    geometry.state_flags = 9;
    geometry.gap_angles_degrees = {10.0f, 70.0f, 205.0f};
    input.payload = geometry;
    std::array frame{input};
    Check(state.Update(frame, error), "geometry frame rejected");
    std::get<hs::VfxRingGapsPayload>(frame[0].payload).gap_angles_degrees.clear();
    const auto &result = state.Active()[0];
    const auto &retained = std::get<hs::VfxRingGapsPayload>(result.payload);
    Check(result.current_transform == input.current_transform && result.previous_transform == input.previous_transform,
          "authoritative transforms must not be reconstructed or integrated");
    Check(result.normalized_age == input.normalized_age && result.state_flags == input.state_flags &&
          result.stable_seed == input.stable_seed && result.quality == input.quality, "envelope metadata changed");
    Check(retained.center.x == 2.0f && retained.center.y == 3.0f && retained.center.z == 4.0f &&
          retained.inner_radius == geometry.inner_radius && retained.outer_radius == geometry.outer_radius &&
          retained.gap_half_width_degrees == geometry.gap_half_width_degrees &&
          retained.lifetime01 == geometry.lifetime01 && retained.state_flags == geometry.state_flags &&
          retained.gap_angles_degrees == geometry.gap_angles_degrees, "geometry must be retained as an owned exact copy");
    // Reusing the current frame must also work when its span aliases internal storage.
    Check(state.Update(state.Active(), error), "self-sourced frame rejected");
    Check(state.Active()[0].normalized_age == .625f, "reconciliation must not advance age");
}
}

int main()
{
    try
    {
        TestAuthoritativePresence();
        TestInvalidFrameIsAtomic();
        TestGeometryAndTransformsAreRetained();
        std::cout << "Persistent VFX reconciliation tests passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
