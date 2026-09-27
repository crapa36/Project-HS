#include "vfx_persistent_state.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>

namespace hs::renderer_detail
{
bool VfxPersistentState::Update(std::span<const VfxPersistentInput> inputs, std::string &error)
{
    for (const auto &input : inputs)
    {
        if (input.effect_handle == kInvalidVfxEffectHandle)
        {
            error = "Persistent VFX has an invalid effect handle";
            return false;
        }
        if (!std::isfinite(input.normalized_age) || input.normalized_age < 0.0f || input.normalized_age > 1.0f)
        {
            error = "Persistent VFX age must be finite and within [0, 1]";
            return false;
        }
    }
    std::vector<VfxPersistentInput> next(inputs.begin(), inputs.end());
    const auto key = [](const VfxPersistentInput &input) { return std::tie(input.stable_id, input.effect_handle); };
    std::sort(next.begin(), next.end(), [&](const auto &a, const auto &b) { return key(a) < key(b); });
    for (std::size_t i = 1; i < next.size(); ++i)
    {
        if (key(next[i - 1]) == key(next[i]))
        {
            error = "Persistent VFX contains a duplicate stable ID and effect handle";
            return false;
        }
    }
    active_.swap(next);
    error.clear();
    return true;
}
}
