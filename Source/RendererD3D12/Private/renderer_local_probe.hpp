#pragma once

#include "render_graph.hpp"
#include <hs/render_data/render_snapshot.hpp>
#include <hs/render_data/scene_probe.hpp>
#include <DirectXMath.h>
#include <d3d12.h>

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace hs
{
inline constexpr std::uint32_t kLocalProbeSrvCount = 5;
inline constexpr std::uint32_t kLocalProbeGpuRecordStride = 128;

struct LocalProbeSceneRead
{
    ResourceVersion version;
    Access access{Access::AllShaderRead};
};

struct LocalProbeCaptureView
{
    std::shared_ptr<const StaticRenderScene> scene;
    render_data::SceneProbeRevisions revisions;
    std::uint32_t probe_index{}, face{}, edge{};
    Float3 position{};
    float near_distance{0.05f}, far_distance{};
    // Absolute-world, unjittered LH row-vector camera with reversed depth
    // (near=1, far=0; clear D32 to0 and use GREATER_EQUAL). No main-view
    // history/visibility results are valid for this independent 90 degree view.
    DirectX::XMFLOAT4X4 world_to_view{}, projection{}, view_projection{};
    ID3D12Resource *radiance_resource{}, *distance_resource{}, *depth_resource{};
    D3D12_CPU_DESCRIPTOR_HANDLE radiance_rtv{}, distance_rtv{}, depth_dsv{};
    std::uint32_t array_slice{};
};

// Called inside each native capture graph pass after its targets are cleared.
// Record actual static geometry and the same material/opacity/light/shadow path.
// MRT0 = scene-linear Rec709 RGB radiance, alpha1; MRT1 = first visible hit's
// radial world distance, or far_distance for sky. Render global environment sky
// once; capture excludes dynamic characters/VFX/UI/post, exposure and recursive
// local probes. Bind the supplied per-face RTVs/DSV; use the supplied camera.
// Preserve every eligible static source's contribution even when main-view
// culled. Only conservative bounds outside this capture view may skip commands.
using LocalProbeDrawCallback = std::function<Result(RenderPassContext &, const LocalProbeCaptureView &)>;

struct LocalProbeInputs
{
    std::shared_ptr<const StaticRenderScene> scene;
    render_data::SceneProbeRevisions revisions;
    // Actual native dependencies used by draw_static: geometry/index/instance,
    // material/texture/terrain, lights, shadows, baseline environment and LUTs.
    // Register declares every read in every capture pass. Caller retains their
    // native owners through completion in addition to the retained scene lease.
    std::span<const LocalProbeSceneRead> scene_reads;
    LocalProbeDrawCallback draw_static;
    CompletionSet completed;
    std::uint32_t max_cube_views{6}; // 1..6 per graph; zero pauses bake work.
    std::function<void(PassBuilder &)> declare_material_sampling;
};

struct LocalProbeOutputs
{
    ResourceVersion radiance, prefiltered, irradiance, first_hit_distance, records;
    ID3D12Resource *radiance_resource{}, *prefiltered_resource{}, *irradiance_resource{},
        *distance_resource{}, *record_resource{};
    // Append t0..t4/space23 to existing table32. 5 descriptors, no extra native
    // root DWORDs. CubeArray RGBA32F/raw, CubeArray RGBA32F/full GGX, SH9 float4,
    // Texture2DArray R32F distance, StructuredBuffer<8 float4> records (128 bytes).
    // Disabled exports are typed null SRVs; helpers evaluate global fallback.
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kLocalProbeSrvCount> srvs{};
    render_data::SceneProbeRevisions revisions;
    std::uint32_t probe_count{}, edge{}, mip_count{}, captured_cube_views{}, total_cube_views{};
    bool enabled{}, update_pending{};
};
struct LocalProbeFramePlan
{
    const LocalProbeInputs *source{};
    std::shared_ptr<const StaticRenderScene> requested_scene;
    render_data::SceneProbeRevisions requested_revisions;
    CompletionSet completed;
    std::uint32_t max_cube_views{};
    const LocalProbeSceneRead *scene_reads{};
    std::size_t scene_read_count{};
    bool draw_requested{},sampling_requested{};
    std::shared_ptr<const StaticRenderScene> scene;
    render_data::SceneProbeRevisions revisions;
    std::shared_ptr<const StaticRenderScene> before_scene;
    render_data::SceneProbeRevisions before_revisions;
    std::uint64_t storage_generation{};
    std::uint32_t before_captured{},capture_first{},capture_count{};
    bool before_uploaded{},before_bake_submitted{},before_published{};
    bool bake_submitted{},published{},current{},upload{},import_cubes{};
    bool integrate{},finished_bake{},material_sampling{},valid{};
};
struct LocalProbeLayout
{
    struct Pass { std::uint32_t index{},id{}; };
    std::uint64_t storage_generation{};
    std::array<ResourceHandle,8> imports{};
    std::array<ResourceVersion,7> outputs{}; // raw, GGX, SH, distance, records, disabled, depth
    std::array<Pass,9> passes{};
    std::vector<LocalProbeSceneRead> scene_reads;
    std::uint32_t pass_count{},capture_first{},capture_count{};
    bool upload{},package_baked{},import_cubes{},integrate{},material_sampling{},published{},finished_bake{};
};

struct LocalProbeStorage;
class LocalProbeCandidate
{
  public:
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
  private:
    std::shared_ptr<LocalProbeStorage> storage_;
    friend class LocalProbeSystem;
};

class LocalProbeSystem
{
  public:
    using ShaderReader = std::function<Result(std::string_view, std::vector<std::byte> &)>;
    LocalProbeSystem();
    ~LocalProbeSystem();
    LocalProbeSystem(const LocalProbeSystem &) = delete;
    LocalProbeSystem &operator=(const LocalProbeSystem &) = delete;
    [[nodiscard]] Result Prepare(ID3D12Device *, D3D12MA::Allocator *,
        std::span<const std::byte> package_bytes, const ShaderReader &, LocalProbeCandidate &) const;
    [[nodiscard]] Result Commit(LocalProbeCandidate &&, const CompletionSet &completed);
    [[nodiscard]] Result PrepareCapture(const ShaderReader &, const CompletionSet &completed);
    [[nodiscard]] Result ExportBakedPackage(RenderGraphQueueExecutor &, TransientResourcePool &,
        std::uint32_t frame_slot, BarrierMode, std::vector<std::byte> &package_bytes);
    [[nodiscard]] Result Register(RenderGraphBuilder &, const LocalProbeInputs &, LocalProbeOutputs &);
    [[nodiscard]] Result PrepareFrameRecordData(const LocalProbeInputs &,LocalProbeFramePlan &) const;
    [[nodiscard]] Result Register(RenderGraphBuilder &,const LocalProbeInputs &,
        const LocalProbeFramePlan &,LocalProbeOutputs &,LocalProbeLayout *);
    [[nodiscard]] bool MatchesLayout(const LocalProbeInputs &,const LocalProbeFramePlan &,
        const LocalProbeLayout &) const noexcept;
    [[nodiscard]] Result BindRetained(RenderGraphBuilder &,const LocalProbeInputs &,
        const LocalProbeFramePlan &,const LocalProbeLayout &,LocalProbeOutputs &);
    // Lets the concrete draw adapter allocate private face bindings only while
    // actual capture can advance. Publication/pause frames need no such upload.
    [[nodiscard]] bool NativeCaptureRequired(const std::shared_ptr<const StaticRenderScene> &,
        render_data::SceneProbeRevisions, const CompletionSet &completed) const noexcept;
    // Submission success records completion requirements; it does not publish
    // new cubes. Register publishes only after those requirements are completed,
    // with exactly the retained scene and revisions still current. This also
    // invalidates a stale generation if replacement occurred after recording.
    [[nodiscard]] Result CompleteSubmission(const RenderGraphBuilder &, const CompletionSet &,
        bool success, const std::shared_ptr<const StaticRenderScene> &current_scene,
        render_data::SceneProbeRevisions current_revisions);
    [[nodiscard]] Result ResetAfterCompletion(const CompletionSet &completed);
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
  private:
    [[nodiscard]] Result RegisterImpl(RenderGraphBuilder &,const LocalProbeInputs &,
        const LocalProbeFramePlan &,LocalProbeOutputs &,LocalProbeLayout *,const LocalProbeLayout *);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
