#pragma once

#include <hs/core/render_snapshot.hpp>
#include <hs/render_data/scene_hlod.hpp>

#include <algorithm>
#include <memory>
#include <numeric>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hs
{
// One immutable binding per HLOD/content owner; view history and residency stay separate.
struct HlodScenePreparation
{
    std::shared_ptr<const StaticRenderScene> source;
    std::vector<std::uint32_t> member_slots;
    std::vector<RenderInstance> proxies;
    std::vector<std::uint32_t> proxy_order;

    [[nodiscard]] bool Resolve(const std::shared_ptr<const StaticRenderScene> &candidate,
        const render_data::SceneHlod &scene, std::string_view content_generation,
        std::span<const std::uint32_t> materials)
    {
        if (source == candidate && source) return true;
        if (!candidate || candidate->content_generation_id != content_generation ||
            candidate->layout_abi != scene.layout_abi ||
            candidate->eligible_layout_digest != scene.layout_digest ||
            candidate->eligible_static_members.size() != scene.members.size() ||
            materials.size() != scene.proxies.size()) return false;

        HlodScenePreparation next;
        std::unordered_map<std::uint64_t, std::uint32_t> slots;
        slots.reserve(candidate->eligible_static_members.size());
        std::vector<render_data::SceneHlodMember> actual;
        actual.reserve(candidate->eligible_static_members.size());
        for (const auto &member : candidate->eligible_static_members)
        {
            if (member.instance_index >= candidate->instances.size() ||
                !slots.emplace(member.stable_id, member.instance_index).second) return false;
            const auto &instance = candidate->instances[member.instance_index];
            render_data::SceneHlodMember value;
            value.stable_id = instance.stable_id; value.object_id = member.object_id;
            value.mesh_semantic = static_cast<std::uint32_t>(instance.mesh);
            value.variant = instance.environment_variant; value.tint_rgba = instance.color_rgba;
            value.position = instance.position; value.scale = instance.scale; value.yaw = instance.yaw;
            if (value.stable_id != member.stable_id) return false;
            actual.push_back(value);
        }
        if (render_data::ComputeSceneHlodLayoutDigest(candidate->layout_abi, actual) != scene.layout_digest)
            return false;
        next.member_slots.reserve(scene.members.size());
        for (const auto &member : scene.members)
        {
            const auto slot = slots.find(member.stable_id);
            if (slot == slots.end() ||
                candidate->instances[slot->second].asset_id.value != member.asset_id.value) return false;
            next.member_slots.push_back(slot->second);
        }
        next.proxies.reserve(scene.proxies.size());
        for (std::uint32_t index = 0; index < scene.proxies.size(); ++index)
        {
            const auto &proxy = scene.proxies[index];
            RenderInstance instance;
            instance.stable_id = (0x7ull << 60u) | index;
            instance.mesh = static_cast<RenderMesh>(proxy.mesh_semantic);
            instance.environment_variant = proxy.variant; instance.color_rgba = proxy.tint_rgba;
            instance.position = {}; instance.yaw = 0; instance.scale = {1, 1, 1};
            instance.asset_id = MakeAssetId(scene.pages.at(proxy.page_index).logical_id);
            instance.material_id = MakeAssetId(scene.dependencies.at(proxy.material_dependency));
            next.proxies.push_back(instance);
        }
        next.proxy_order.resize(scene.proxies.size());
        std::iota(next.proxy_order.begin(), next.proxy_order.end(), 0u);
        std::ranges::sort(next.proxy_order, [&](auto left, auto right) {
            return std::pair{materials[left], left} < std::pair{materials[right], right};
        });
        next.source = candidate;
        *this = std::move(next);
        return true;
    }
};
} // namespace hs
