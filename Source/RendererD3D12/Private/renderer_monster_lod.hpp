#pragma once

#include <hs/render_data/geometry_package.hpp>
#include <hs/render_data/render_view_input.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

namespace hs
{
struct MonsterLodConsumer
{
    // The caller supplies every camera, shadow and probe consumer of this
    // instance. Projection is pixels per world unit at the conservative
    // nearest point of its animated interval bound.
    float pixels_per_world_unit{};
    float maximum_error_pixels{};
};

struct MonsterLodWorldSphere
{
    // Global coordinates; caller supplies a qualified interval enclosure for
    // every candidate LOD, including the current and previous pose if needed.
    RenderWorldPosition center{};
    double radius{};
    bool pose_bounds_qualified{};
};

struct MonsterLodProjectionInputs
{
    std::span<const RenderViewInput> camera_views;
    std::span<const LightView> local_lights;
    RenderWorldPosition local_light_scene_origin{};
    std::array<double, 3> csm_world_texels{}, vsm_world_texels{};
    double finest_gi_voxel_m{}; // From the active SDK context, not config defaults.
    double maximum_error_pixels{}; // Existing geometry quality policy.
    std::uint32_t local_shadow_face_resolution{};
    bool csm_active{}, vsm_active{}, local_shadows_active{}, gi_active{};
    // The caller has enumerated every active view and requested local light.
    // Missing consumers cannot silently qualify a reduced page.
    bool camera_views_complete{}, local_lights_complete{};
};

struct MonsterLodProjection
{
    // max(pixels_per_world_unit / permitted_pixels) over every consumer.
    double maximum_normalized_projection{};
    bool valid{true}, has_consumer{};

    void Include(double pixels_per_world_unit, double permitted_pixels) noexcept
    {
        if (!std::isfinite(pixels_per_world_unit) || pixels_per_world_unit < 0 ||
            !std::isfinite(permitted_pixels) || permitted_pixels <= 0)
        { valid = false; return; }
        const double ratio = pixels_per_world_unit / permitted_pixels;
        if (!std::isfinite(ratio)) { valid = false; return; }
        maximum_normalized_projection = std::max(maximum_normalized_projection, ratio);
        has_consumer = true;
    }
};

struct MonsterLodPreparedCamera
{
    std::array<double,3> eye{}, forward{}, right{}, up{};
    double near_plane{}, far_plane{}, tangent_x{}, tangent_y{},
        focal_x_pixels{}, focal_y_pixels{};
};
struct MonsterLodPreparedLocalLight
{
    std::array<double,3> center{};
    double near_plane{}, far_plane{}, emitter_bound{}, angular{}, focal_pixels{};
};
struct MonsterLodPreparedConsumers
{
    std::vector<MonsterLodPreparedCamera> cameras;
    std::vector<MonsterLodPreparedLocalLight> local_lights;
    MonsterLodProjection baseline;
    double maximum_error_pixels{};
};

namespace monster_lod_detail
{
using Vec3 = std::array<double, 3>;
[[nodiscard]] inline Vec3 Vector(RenderWorldPosition value) noexcept
{ return {value.x, value.y, value.z}; }
[[nodiscard]] inline Vec3 Difference(Vec3 a, Vec3 b) noexcept
{ return {a[0]-b[0], a[1]-b[1], a[2]-b[2]}; }
[[nodiscard]] inline double Dot(Vec3 a, Vec3 b) noexcept
{ return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
[[nodiscard]] inline Vec3 Cross(Vec3 a, Vec3 b) noexcept
{ return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]}; }
[[nodiscard]] inline bool Unit(Vec3 &value) noexcept
{
    const double length = std::sqrt(Dot(value,value));
    if (!std::isfinite(length) || length <= 0) return false;
    for (double &component : value) component /= length;
    return true;
}
[[nodiscard]] inline bool CameraProjection(MonsterLodProjection &out,
    const MonsterLodWorldSphere &sphere, const MonsterLodPreparedCamera &view,
    double permitted_pixels) noexcept
{
    const Vec3 delta = Difference(Vector(sphere.center),view.eye);
    const double depth = Dot(delta,view.forward);
    const double x = Dot(delta,view.right), y = Dot(delta,view.up);
    if (!std::isfinite(depth) || !std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(view.tangent_x) || view.tangent_x <= 0) return false;
    if (depth+sphere.radius < view.near_plane || depth-sphere.radius > view.far_plane ||
        std::abs(x) > depth*view.tangent_x+sphere.radius*std::hypot(1.0,view.tangent_x) ||
        std::abs(y) > depth*view.tangent_y+sphere.radius*std::hypot(1.0,view.tangent_y))
        return true;
    // A near-plane crossing can have unbounded perspective displacement.
    const double nearest = depth-sphere.radius;
    if (nearest <= view.near_plane) return false;
    const double xmax = std::abs(x)+sphere.radius, ymax = std::abs(y)+sphere.radius;
    // Frobenius norm bounds the 2x3 perspective Jacobian everywhere inside
    // the qualified sphere; off-axis depth derivatives are included.
    const double xscale = view.focal_x_pixels/nearest;
    const double yscale = view.focal_y_pixels/nearest;
    out.Include(std::sqrt(xscale*xscale+yscale*yscale+
        (xscale*xmax/nearest)*(xscale*xmax/nearest)+
        (yscale*ymax/nearest)*(yscale*ymax/nearest)),permitted_pixels);
    return out.valid;
}
[[nodiscard]] inline double EmitterBound(const LightView &light) noexcept
{
    if (light.type == LightType::Rectangle)
        return std::hypot(double(light.shape.x)*0.5,double(light.shape.y)*0.5);
    if (light.type == LightType::Disk)
        return std::max(double(light.shape.x),double(light.shape.y));
    return double(light.emitter_radius);
}
[[nodiscard]] inline bool LocalShadowProjection(MonsterLodProjection &out,
    const MonsterLodWorldSphere &sphere, const MonsterLodPreparedLocalLight &light,
    double permitted_pixels) noexcept
{
    const Vec3 delta = Difference(Vector(sphere.center),light.center);
    const double distance = std::sqrt(Dot(delta,delta));
    if (!std::isfinite(distance)) return false;
    const double nearest = distance-sphere.radius-light.emitter_bound;
    if (nearest > light.far_plane) return true;
    // Face selection bounds z by nearest radial distance, including cube-edge
    // and spot-cone angles. An emitter's every sample lies within its bound.
    const double minimum_depth = std::max(light.near_plane,
        nearest/std::sqrt(1.0+light.angular));
    out.Include(light.focal_pixels/minimum_depth*std::sqrt(2.0+light.angular),
        permitted_pixels);
    return out.valid;
}
} // namespace monster_lod_detail

// Compute camera bases, light projections and global texel budgets once per
// frame. The actor pass below only measures its qualified world sphere.
[[nodiscard]] inline MonsterLodPreparedConsumers PrepareMonsterLodConsumers(
    const MonsterLodProjectionInputs &inputs)
{
    MonsterLodPreparedConsumers prepared;
    prepared.maximum_error_pixels = inputs.maximum_error_pixels;
    auto &projection = prepared.baseline;
    if (!inputs.camera_views_complete ||
        (inputs.local_shadows_active && !inputs.local_lights_complete) ||
        !std::isfinite(inputs.maximum_error_pixels) || inputs.maximum_error_pixels <= 0)
    { projection.valid = false; return prepared; }
    for (const auto &view : inputs.camera_views)
    {
        if (!view.render_active.width || !view.render_active.height) continue;
        if (!view.output_extent.width || !view.output_extent.height)
        { projection.valid = false; return prepared; }
        const double tangent_y = std::tan(double(view.camera.vertical_fov_degrees)*
            std::numbers::pi / 360.0);
        const double tangent_x = tangent_y*double(view.render_active.width)/view.render_active.height;
        if (!std::isfinite(view.near_plane) || !std::isfinite(view.far_plane) ||
            view.near_plane <= 0 || view.far_plane <= view.near_plane ||
            !std::isfinite(tangent_y) || tangent_y <= 0 ||
            !std::isfinite(tangent_x) || tangent_x <= 0)
        { projection.valid = false; return prepared; }
        MonsterLodPreparedCamera camera;
        camera.eye = monster_lod_detail::Vector(view.camera.position);
        camera.forward = monster_lod_detail::Difference(
            monster_lod_detail::Vector(view.camera.target),camera.eye);
        camera.right = {view.camera.up.x,view.camera.up.y,view.camera.up.z};
        if (!monster_lod_detail::Unit(camera.forward))
        { projection.valid = false; return prepared; }
        camera.right = monster_lod_detail::Cross(camera.right,camera.forward);
        if (!monster_lod_detail::Unit(camera.right))
        { projection.valid = false; return prepared; }
        camera.up = monster_lod_detail::Cross(camera.forward,camera.right);
        camera.near_plane = view.near_plane;
        camera.far_plane = view.far_plane;
        camera.tangent_x = tangent_x;
        camera.tangent_y = tangent_y;
        camera.focal_x_pixels = 1.001*double(view.output_extent.width)/(2.0*tangent_x);
        camera.focal_y_pixels = 1.001*double(view.output_extent.height)/(2.0*tangent_y);
        prepared.cameras.push_back(camera);
    }
    const auto add_texels = [&](bool active, const std::array<double,3> &texels) {
        if (!active) return;
        for (double world_texel : texels)
            if (!std::isfinite(world_texel) || world_texel <= 0)
                projection.valid = false;
            else projection.Include(1.001/world_texel,inputs.maximum_error_pixels);
    };
    add_texels(inputs.csm_active,inputs.csm_world_texels);
    add_texels(inputs.vsm_active,inputs.vsm_world_texels);
    if (!projection.valid) return prepared;
    if (inputs.local_shadows_active)
        for (const auto &light : inputs.local_lights)
        {
            if (light.type == LightType::Directional ||
                (light.shadow_flags & kLightShadowRequested) == 0) continue;
            if (light.type != LightType::Point && light.type != LightType::Spot &&
                light.type != LightType::Rectangle && light.type != LightType::Disk)
            { projection.valid = false; return prepared; }
            const double emitter = monster_lod_detail::EmitterBound(light);
            const double tangent = light.type == LightType::Spot
                ? std::tan(double(light.outer_cone_radians)) : 1.0;
            if (!inputs.local_shadow_face_resolution || !std::isfinite(light.range) ||
                light.range <= 0.02f || !std::isfinite(emitter) || emitter < 0 ||
                !std::isfinite(tangent) || tangent <= 0 ||
                (light.type == LightType::Spot &&
                    (!std::isfinite(light.outer_cone_radians) ||
                     light.outer_cone_radians >= std::numbers::pi/2)))
            { projection.valid = false; return prepared; }
            MonsterLodPreparedLocalLight shadow;
            shadow.center = {
                inputs.local_light_scene_origin.x+double(light.position.x),
                inputs.local_light_scene_origin.y+double(light.position.y),
                inputs.local_light_scene_origin.z+double(light.position.z)};
            if (!std::isfinite(shadow.center[0]) || !std::isfinite(shadow.center[1]) ||
                !std::isfinite(shadow.center[2]))
            { projection.valid = false; return prepared; }
            shadow.emitter_bound = emitter;
            const float native_near = std::max(0.005f,
                std::min(0.05f,light.range*0.001f));
            shadow.near_plane = std::nextafter(double(native_near),0.0);
            shadow.far_plane = double(light.range)+
                ((light.type == LightType::Rectangle || light.type == LightType::Disk) ? emitter : 0.0);
            shadow.angular = light.type == LightType::Spot ? tangent*tangent : 2.0;
            shadow.focal_pixels = 1.001*double(inputs.local_shadow_face_resolution)/(2.0*tangent);
            prepared.local_lights.push_back(shadow);
        }
    if (inputs.gi_active)
        // Half of the finest native SDF voxel is a stricter world-position
        // budget than the authored raster error policy, without a new setting.
        if (!std::isfinite(inputs.finest_gi_voxel_m) || inputs.finest_gi_voxel_m <= 0)
            projection.valid = false;
        else projection.Include(1.001/inputs.finest_gi_voxel_m,
            std::min(inputs.maximum_error_pixels,0.5));
    return prepared;
}

[[nodiscard]] inline MonsterLodProjection PrepareMonsterLodProjection(
    const MonsterLodWorldSphere &sphere,
    const MonsterLodPreparedConsumers &consumers) noexcept
{
    MonsterLodProjection projection = consumers.baseline;
    const auto center = monster_lod_detail::Vector(sphere.center);
    if (!projection.valid || !sphere.pose_bounds_qualified ||
        !std::isfinite(sphere.radius) || sphere.radius < 0 ||
        !std::isfinite(center[0]) || !std::isfinite(center[1]) ||
        !std::isfinite(center[2])) { projection.valid = false; return projection; }
    for (const auto &view : consumers.cameras)
        if (!monster_lod_detail::CameraProjection(projection,sphere,view,
            consumers.maximum_error_pixels))
        { projection.valid = false; return projection; }
    for (const auto &light : consumers.local_lights)
        if (!monster_lod_detail::LocalShadowProjection(projection,sphere,light,
            consumers.maximum_error_pixels))
        { projection.valid = false; return projection; }
    return projection;
}

[[nodiscard]] inline std::uint16_t SelectMonsterLod(
    std::span<const render_data::GeometryMetadata> lods,
    std::uint32_t own_resident_mask, float maximum_world_scale,
    const MonsterLodProjection &projection) noexcept
{
    if (!projection.valid || !projection.has_consumer ||
        !std::isfinite(projection.maximum_normalized_projection) ||
        lods.empty() || lods.front().lod != 0 ||
        (lods.front().flags & render_data::kGeometryFlagSkinned) == 0 ||
        lods.front().error_method_abi != render_data::kGeometryErrorMethodAbi ||
        !std::isfinite(maximum_world_scale) || maximum_world_scale <= 0)
        return 0;
    for (std::size_t index = lods.size(); index > 1;)
    {
        --index;
        if (index >= 32 || (own_resident_mask & (1u << index)) == 0) continue;
        const auto &metadata = lods[index];
        if (metadata.lod != index ||
            metadata.error_method_abi != render_data::kSkinnedPoseErrorMethodAbi ||
            (metadata.flags & render_data::kGeometryFlagSkinned) == 0 ||
            !std::isfinite(metadata.geometric_error) ||
            metadata.geometric_error < lods[index-1].geometric_error) continue;
        if (double(metadata.geometric_error)*maximum_world_scale*
            projection.maximum_normalized_projection <= 1.0)
            return static_cast<std::uint16_t>(index);
    }
    return 0;
}

// The mask contains only pages with their own completed resident allocation;
// a descriptor that currently resolves to its LOD0 fallback is not resident.
[[nodiscard]] inline std::uint16_t SelectMonsterLod(
    std::span<const render_data::GeometryMetadata> lods,
    std::uint32_t own_resident_mask, float maximum_world_scale,
    std::span<const MonsterLodConsumer> consumers) noexcept
{
    MonsterLodProjection projection;
    for(const auto &consumer:consumers)
    {
        projection.Include(consumer.pixels_per_world_unit,consumer.maximum_error_pixels);
        if (!projection.valid) return 0;
    }
    return SelectMonsterLod(lods,own_resident_mask,maximum_world_scale,projection);
}
} // namespace hs
