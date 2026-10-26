#pragma once

#include <hs/render_data/render_snapshot.hpp>
#include <hs/core/result.hpp>
#include <hs/render_data/character_animation_extension.hpp>
#include <hs/render_data/character_descriptor.hpp>

#include <DirectXMath.h>
#include <span>

namespace hs
{
// CPU-only full-pose sampler. The extension must already have passed
// ValidateCharacterAnimationBinding against the instance's actual character.
// Explicit selection is required; missing clips fail and never use a legacy
// slot. A successful sample uses the existing Archer shader matrix convention.
// Failure leaves the output span untouched. Successful sampling uses fixed
// stack storage; it does not mutate renderer state, correct grounding or
// advance an implicit clock.
[[nodiscard]] Result SampleNamedCharacterAnimation(
    const render_data::CharacterAnimationExtensionData &animation,
    const RenderInstance &instance,
    std::span<DirectX::XMFLOAT4X4> transposed_skin_matrices);
[[nodiscard]] Result SampleNamedCharacterAnimation(
    const render_data::AnimationDescriptor &animation,
    std::span<const render_data::PackedCharacterTransform> transforms,
    const RenderInstance &instance,
    std::span<DirectX::XMFLOAT4X4> transposed_skin_matrices);
} // namespace hs
