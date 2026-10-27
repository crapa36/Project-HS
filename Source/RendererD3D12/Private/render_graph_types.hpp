#pragma once

#include <cstdint>

namespace hs
{
enum class Access : std::uint8_t
{
    Undefined,
    Common,
    ShaderRead,
    ComputeRead,
    RenderTarget,
    DepthRead,
    DepthWrite,
    UnorderedRead,
    UnorderedWrite,
    CopySource,
    CopyDest,
    VertexBuffer,
    IndexBuffer,
    // Packed geometry: vertex/index input and non-pixel shader reads.
    VertexShaderRead,
    IndirectArgs,
    Present,
    // Combined read-only DSV + SRV access; keep existing enum ordinals stable.
    DepthShaderRead,
    // Material textures shared by raster and compute retain both read states.
    AllShaderRead,
    // Native VRS image consumed by rasterization on the direct queue.
    ShadingRateSource,
};

enum class ResourceKind : std::uint8_t
{
    Texture,
    Buffer,
};

struct ResourceHandle
{
    std::uint32_t value{};
    ResourceKind kind{};

    [[nodiscard]] bool operator==(const ResourceHandle &) const noexcept = default;
    [[nodiscard]] explicit operator bool() const noexcept { return value != 0; }
};

using TextureHandle = ResourceHandle;
using BufferHandle = ResourceHandle;

// A physical resource may be written repeatedly. Ordinal zero is its imported
// contents (or undefined for a transient); each write produces the next version.
// A partial write preserves all other ranges from its predecessor. Reads require
// their complete range to have been initialized in that version. Each successor
// has one writer; disjoint ranges do not create independent physical resources.
struct ResourceVersion
{
    ResourceHandle resource{};
    std::uint32_t ordinal{};

    [[nodiscard]] bool operator==(const ResourceVersion &) const noexcept = default;
};
} // namespace hs
