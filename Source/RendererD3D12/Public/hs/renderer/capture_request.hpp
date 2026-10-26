#pragma once

#include <hs/render_data/render_view_input.hpp>

#include <cstdint>
#include <filesystem>

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
    // Completed native SPB1 radiance, filtered radiance, SH9 and first-hit payloads.
    SceneProbePackage,
};

struct CaptureRequest
{
    ViewId view_id{kMainRenderViewId};
    std::filesystem::path path;
    CaptureKind kind{CaptureKind::Color};
};

} // namespace hs
