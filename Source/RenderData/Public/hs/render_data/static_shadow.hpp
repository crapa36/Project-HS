#pragma once

#include <hs/render_data/render_snapshot.hpp>

#include <array>
#include <cstddef>

namespace hs::render_data
{
// Source eligibility only; the native baker also qualifies actual material opacity.
[[nodiscard]] bool IsStaticShadowCaster(const RenderInstance &) noexcept;
// Geometry-affecting source fields only, in source order. Product digests belong
// to the content Action; session/Generation/revision counters are not geometry.
[[nodiscard]] std::array<std::byte, 32> StaticShadowLayoutDigest(const StaticRenderScene &);
} // namespace hs::render_data
