#pragma once

#include <hs/renderer/renderer.hpp>
#include <hs/render_data/static_shadow.hpp>
#include <hs/render_data/static_shadow_archive.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

inline nlohmann::json StaticShadowArchiveProbe(hs::D3D12Renderer &renderer,
    const std::shared_ptr<const hs::StaticRenderScene> &scene, const hs::LightView &sun)
{
    namespace rd = hs::render_data;
    constexpr auto edge = rd::kStaticShadowPageTile, inner = rd::kStaticShadowPageInner, guard = rd::kStaticShadowPageGuard;
    const auto check = [](bool valid, const char *message) { if (!valid) throw std::runtime_error(message); };
    using Tile = std::array<float, rd::kStaticShadowPageTexels>;
    struct Capture
    {
        rd::StaticShadowArchiveDescription description;
        std::vector<Tile> tiles;
        std::vector<bool> seen;
        std::size_t covered{}, clear{}, negative_keys{};
        float minimum{1}, maximum{};
        bool began{}, origin_seen{};
    };
    const auto collect = [&](const std::shared_ptr<const hs::StaticRenderScene> &source) {
        Capture capture;
        const auto result = renderer.BakeStaticShadowPages(source, sun, [&](const auto &description) {
            check(!capture.began, "Archive bake began more than once");
            capture.began = true; capture.description = description;
            const auto &level = description.levels[2];
            const auto count = std::uint64_t(level.width) * level.height;
            check(level.width && level.height && count <= 1024, "Archive probe coarse coverage exceeds bounded fixture");
            capture.tiles.resize(static_cast<std::size_t>(count)); capture.seen.resize(static_cast<std::size_t>(count));
            return hs::Result::Success();
        }, [&](std::uint32_t level, std::int32_t x, std::int32_t y, std::span<const float> tile) {
            check(capture.began && level == 2 && tile.size() == rd::kStaticShadowPageTexels, "Unexpected archive tile");
            const auto &bounds = capture.description.levels[2];
            const auto dx = std::int64_t(x) - bounds.minimum_x, dy = std::int64_t(y) - bounds.minimum_y;
            check(dx >= 0 && dy >= 0 && dx < bounds.width && dy < bounds.height, "Archive tile escapes described coverage");
            const auto index = static_cast<std::size_t>(dy * bounds.width + dx);
            check(!capture.seen[index], "Duplicate archive tile coordinate"); capture.seen[index] = true;
            capture.negative_keys += x < 0 || y < 0; capture.origin_seen |= x == 0 && y == 0;
            std::ranges::copy(tile, capture.tiles[index].begin());
            for (const float value : tile)
            {
                check(std::isfinite(value) && value >= 0 && value <= 1, "Archive tile contains invalid reverse depth");
                capture.covered += value > 0; capture.clear += value == 0;
                capture.minimum = std::min(capture.minimum, value); capture.maximum = std::max(capture.maximum, value);
            }
            return hs::Result::Success();
        });
        if (!result) throw std::runtime_error(std::string(result.Message()));
        check(capture.began && !capture.seen.empty() && std::ranges::all_of(capture.seen, [](bool v) { return v; }),
            "Archive bake omitted a described tile");
        check(capture.covered && capture.clear, "Archive bake requires covered and clear texels");
        return capture;
    };
    const auto arena = collect(scene);
    check(arena.description.layout_digest == rd::StaticShadowLayoutDigest(*scene), "Archive bake changed scene identity");
    auto wind_changed_scene = std::make_unique<hs::StaticRenderSceneStorage>(*scene);
    std::size_t changed_wind_instances{};
    for (auto &instance : wind_changed_scene->instances)
    {
        instance.environment_seed ^= 0x5a5aa5a5u;
        if (hs::HasEnvironmentWind(instance.mesh, instance.environment_variant))
        {
            instance.position.x += 1000.0f;
            ++changed_wind_instances;
        }
    }
    check(changed_wind_instances != 0, "Static shadow probe scene has no wind geometry to exclude");
    {
        const auto repeated = collect(hs::FreezeStaticRenderScene(std::move(wind_changed_scene)));
        const auto same_float = [](float a, float b) { return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b); };
        const auto same_vector = [&](hs::Float3 a, hs::Float3 b) {
            return same_float(a.x, b.x) && same_float(a.y, b.y) && same_float(a.z, b.z);
        };
        const auto &a = arena.description, &b = repeated.description;
        check(a.layout_digest == b.layout_digest && same_vector(a.light_right, b.light_right) &&
            same_vector(a.light_up, b.light_up) && same_vector(a.light_forward, b.light_forward) &&
            same_float(a.depth_minimum, b.depth_minimum) && same_float(a.depth_maximum, b.depth_maximum),
            "Archive basis, depth range or layout changed after moving wind geometry or changing cosmetic seeds");
        for (std::size_t level = 0; level < a.levels.size(); ++level)
        {
            const auto &x = a.levels[level], &y = b.levels[level];
            check(same_float(x.page_world_size, y.page_world_size) && x.minimum_x == y.minimum_x &&
                x.minimum_y == y.minimum_y && x.width == y.width && x.height == y.height,
                "Archive page bounds changed after moving wind geometry or changing cosmetic seeds");
        }
        check(arena.tiles.size() == repeated.tiles.size(), "Repeated archive tile count changed");
        for (std::size_t tile = 0; tile < arena.tiles.size(); ++tile)
            check(std::ranges::equal(std::as_bytes(std::span(arena.tiles[tile])),
                std::as_bytes(std::span(repeated.tiles[tile]))),
                "Archive depth bytes changed after moving wind geometry or changing cosmetic seeds");
    }
    const auto &bounds = arena.description.levels[2];
    check(arena.seen[0] && arena.origin_seen && arena.negative_keys, "Archive probe omitted page zero or negative keys");
    std::size_t guard_checks{};
    const auto same = [&](float a, float b) {
        check(std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b), "Adjacent archive guards disagree");
        ++guard_checks;
    };
    for (std::size_t y = 0; y < bounds.height; ++y) for (std::size_t x = 0; x < bounds.width; ++x)
    {
        const auto &tile = arena.tiles[y * bounds.width + x];
        if (x + 1 < bounds.width)
            for (std::size_t row = 0; row < edge; ++row) for (std::size_t column = 0; column < 2 * guard; ++column)
                same(tile[row * edge + inner + column], arena.tiles[y * bounds.width + x + 1][row * edge + column]);
        if (y + 1 < bounds.height)
            for (std::size_t row = 0; row < 2 * guard; ++row) for (std::size_t column = 0; column < edge; ++column)
                same(tile[row * edge + column], arena.tiles[(y + 1) * bounds.width + x][(inner + row) * edge + column]);
    }
    const auto ground = std::ranges::find(scene->instances, hs::RenderMesh::Ground, &hs::RenderInstance::mesh);
    check(ground != scene->instances.end(), "Archive plane probe requires actual arena Ground");
    auto ground_scene = std::make_unique<hs::StaticRenderSceneStorage>(*scene);
    ground_scene->instances = {*ground}; ground_scene->eligible_static_members.clear();
    const auto plane = collect(hs::FreezeStaticRenderScene(std::move(ground_scene)));
    const auto &d = plane.description; const auto &level = d.levels[2];
    check(d.light_forward.y < -1e-4f, "Archive top-plane probe requires an above-ground light");
    const double texel = double(level.page_world_size) / inner, span = double(d.depth_maximum) - d.depth_minimum;
    const double bias = -1.5 * std::max(std::abs(double(d.light_right.y) / d.light_forward.y),
        std::abs(double(d.light_up.y) / d.light_forward.y)) * texel / span;
    // Ground's ShapePlaceholder scales cube Y by0.1 before the half-scale instance offset.
    const double top = double(ground->position.y) + .55 * ground->scale.y;
    const double cosine = std::cos(double(ground->yaw)), sine = std::sin(double(ground->yaw));
    std::size_t checked_texels{}; double max_plane_error{};
    for (std::size_t y = 0; y < level.height; ++y) for (std::size_t x = 0; x < level.width; ++x)
        for (std::size_t row = guard; row < guard + inner; ++row) for (std::size_t column = guard; column < guard + inner; ++column)
        {
            const double lx = (double(level.minimum_x) + double(x) + (double(column) + .5 - guard) / inner) * level.page_world_size;
            const double ly = (double(level.minimum_y) + double(y) + 1 - (double(row) + .5 - guard) / inner) * level.page_world_size;
            const double lz = (top - d.light_right.y * lx - d.light_up.y * ly) / d.light_forward.y;
            const double wx = d.light_right.x * lx + d.light_up.x * ly + d.light_forward.x * lz - ground->position.x;
            const double wz = d.light_right.z * lx + d.light_up.z * ly + d.light_forward.z * lz - ground->position.z;
            if (std::abs(wx * cosine - wz * sine) >= double(ground->scale.x) * .5 - 2 * texel ||
                std::abs(wx * sine + wz * cosine) >= double(ground->scale.z) * .5 - 2 * texel) continue;
            const double expected = (d.depth_maximum - lz) / span + bias;
            const float actual = plane.tiles[y * level.width + x][row * edge + column];
            const double error = std::abs(double(actual) - expected);
            if (!(actual > 0 && std::isfinite(expected) && error <= 2e-5))
                throw std::runtime_error("Archive depth disagrees with independent Ground plane/bias: actual=" +
                    std::to_string(actual) + " expected=" + std::to_string(expected) + " bias=" + std::to_string(bias) +
                    " light_xy=" + std::to_string(lx) + "," + std::to_string(ly) + " top=" + std::to_string(top) +
                    " z_span=" + std::to_string(span));
            max_plane_error = std::max(max_plane_error, error); ++checked_texels;
        }
    check(checked_texels > 16, "Archive plane probe has insufficient interior samples");
    return {{"pages", arena.tiles.size()}, {"negative_keys", arena.negative_keys}, {"page_zero_valid", true},
        {"repeat_matches", true}, {"wind_geometry_ignored", true}, {"changed_wind_instances", changed_wind_instances},
        {"cosmetic_seed_ignored", true},
        {"covered_texels", arena.covered}, {"clear_texels", arena.clear}, {"depth_range", {arena.minimum, arena.maximum}},
        {"page_range", {bounds.minimum_x, bounds.minimum_y, std::int64_t(bounds.minimum_x) + bounds.width - 1,
            std::int64_t(bounds.minimum_y) + bounds.height - 1}}, {"guard_checks", guard_checks},
        {"checked_texels", checked_texels}, {"max_plane_error", max_plane_error}};
}
