#pragma once

#include <hs/render_data/render_view_input.hpp>

#include <cstdint>
#include <filesystem>
#include <iosfwd>

namespace hs
{

enum class CaptureKind : std::uint8_t
{
    Color,
    SurfaceDiagnostics,
    GeometryDiagnostics,
    VisibilityDiagnostics,
    // Reconstructed scene-linear HDR before post processing/display encoding;
    // the published source includes transparent and VFX composition.
    HdrColor,
    ParticleCount,
};

struct CaptureRequest
{
    ViewId view_id{kMainRenderViewId};
    std::filesystem::path path;
    CaptureKind kind{CaptureKind::Color};
    // Return the surface summary directly when the caller only needs readback values.
    std::ostream *surface_summary{};
};

} // namespace hs
