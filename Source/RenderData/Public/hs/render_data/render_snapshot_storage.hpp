#pragma once

#include <hs/render_data/render_snapshot_values.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace hs
{

// Mutable preparation only. Bind after the final allocation and all vector writes.
struct StaticRenderSceneStorage final
{
    using EligibleStaticMember = StaticRenderScene::EligibleStaticMember;
    std::vector<RenderInstance> instances;
    AssetId scene_asset_id{};
    std::string content_generation_id;
    std::uint32_t layout_abi{};
    std::array<std::byte, 32> eligible_layout_digest{};
    std::vector<EligibleStaticMember> eligible_static_members;
    std::vector<RenderSceneInstanceKey> static_keys;
    StaticRenderScene exported_view;

    StaticRenderSceneStorage() = default;
    explicit StaticRenderSceneStorage(const StaticRenderScene &view)
        : instances(view.instances.begin(), view.instances.end()), scene_asset_id(view.scene_asset_id),
          content_generation_id(view.content_generation_id), layout_abi(view.layout_abi),
          eligible_layout_digest(view.eligible_layout_digest),
          eligible_static_members(view.eligible_static_members.begin(), view.eligible_static_members.end()),
          static_keys(view.static_keys.begin(), view.static_keys.end()) {}
    void BindExportedView()
    {
        exported_view = {instances, scene_asset_id, content_generation_id, layout_abi,
                         eligible_layout_digest, eligible_static_members, static_keys};
    }
};

// Development/native fixtures use the same stable-address, immutable lease contract.
[[nodiscard]] inline std::shared_ptr<const StaticRenderScene> FreezeStaticRenderScene(
    std::unique_ptr<StaticRenderSceneStorage> candidate)
{
    candidate->static_keys.clear();
    candidate->static_keys.reserve(candidate->instances.size());
    for (std::size_t index = 0; index < candidate->instances.size(); ++index)
    {
        PopulateRenderInstanceRevisions(candidate->instances[index]);
        candidate->static_keys.push_back(MakeRenderSceneInstanceKey(
            candidate->instances[index].stable_id, static_cast<std::uint32_t>(index)));
    }
    std::ranges::sort(candidate->static_keys, [](const auto &left, const auto &right) {
        return std::pair{left.stable_id, left.fallback_ordinal} <
               std::pair{right.stable_id, right.fallback_ordinal};
    });
    candidate->BindExportedView();
    const auto *view = &candidate->exported_view;
    std::shared_ptr<const StaticRenderSceneStorage> owner(std::move(candidate));
    return {std::move(owner), view};
}

class RenderSnapshotStorage
{
  public:
    RenderSnapshotStorage(std::size_t instance_capacity, std::size_t pose_capacity,
                          std::size_t light_capacity, std::size_t ui_capacity);
    RenderSnapshotStorage(const RenderSnapshotStorage &) = default;
    RenderSnapshotStorage &operator=(const RenderSnapshotStorage &other);
    RenderSnapshotStorage(RenderSnapshotStorage &&other) noexcept;
    RenderSnapshotStorage &operator=(RenderSnapshotStorage &&other) noexcept;

    void Clear() noexcept;
    // Copies a different retained sample; UI belongs only to the current packet.
    void CopyDynamicFrom(const RenderSnapshot &sample);
    // A nonzero session_id and dynamic_pair_revision, together with the actual
    // tick (including tick0), must identify immutable dynamic arrays. Returns true if
    // those arrays were copied; the header, camera, static-scene lease and UI
    // clearing are refreshed on every call. Unknown session/revision always copy.
    [[nodiscard]] bool CopyRetainedDynamicFrom(const RenderSnapshot &sample);
    void AddInstance(const RenderInstance &instance);
    void AddPose(const AnimationPoseRef &pose);
    void AddCapturedInstance(const CapturedRenderInstance &capture);
    void AddLight(const LightView &light);
    void AddUi(const UiModel &ui);
    void AddPersistentVfx(const PersistentVfxVisual &visual);
    [[nodiscard]] std::vector<UiModel> TakeUiModels();
    [[nodiscard]] std::size_t InstanceCount() const noexcept;
    [[nodiscard]] RenderSnapshot View() const noexcept;
    void SetStaticScene(std::shared_ptr<const StaticRenderScene> scene) noexcept;

    RenderSnapshotHeader header{};
    CameraView camera{};

  private:
    std::vector<RenderInstance> instances_;
    std::vector<AnimationPoseRef> poses_;
    std::vector<CapturedRenderInstance> captured_instances_;
    std::vector<LightView> lights_;
    std::vector<UiModel> ui_;
    std::vector<PersistentVfxVisual> persistent_vfx_;
    std::shared_ptr<const StaticRenderScene> static_scene_;
    bool retained_dynamic_identity_valid_{};
    std::uint64_t retained_session_id_{}, retained_dynamic_pair_revision_{};
    Tick retained_tick_{};
};

} // namespace hs
