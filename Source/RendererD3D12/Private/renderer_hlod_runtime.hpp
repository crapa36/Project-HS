#pragma once

#include "renderer_asset_source.hpp"

#include "renderer_hlod.hpp"
#include "renderer_hlod_scene.hpp"
#include "d3d12_resources.hpp"
#include "residency_manager.hpp"
#include <hs/render_data/render_snapshot.hpp>
#include <hs/render_data/render_view_input.hpp>

#include <array>
#include <filesystem>
#include <map>

namespace hs
{
struct HlodRuntimeFrame
{
    AllocationResource readback;
    ResidencyTicket readback_residency;
    std::vector<HlodMemberBinding> members;
    std::vector<GpuHlodProxyBinding> bindings;
    std::vector<GpuHlodBucket> buckets;
    std::vector<std::uint32_t> material_ordinals;
    std::array<GpuHlodView, 4> views;
    HlodFrameOutputs outputs;
    CompletionSet completion{}, readback_completion{};
    std::uint64_t frame_id{}, qualification_epoch{};
    std::shared_ptr<const StaticRenderScene> prepared_source;
    std::uint32_t prepared_generation{};
    std::uint32_t prepared_hlod_generation{};
    std::size_t prepared_leaf_count{};
    std::uint64_t prepared_pipeline_revision{};
    bool enabled{}, registered{}, readback_pending{};
};
struct HlodViewRuntime
{
    ViewId view_id;
    std::uint64_t session_id{}, demand_frame_id{}, qualification_epoch{};
    bool participating{};
    std::uint32_t static_capacity{}, proxy_slot_base{};
    std::vector<std::uint32_t> group_priorities;
    std::array<std::uint32_t, 16> last_status{}; // Completed telemetry for this view only.
    std::shared_ptr<const StaticRenderScene> matching_scene;
    HlodSystem system;
    ResidencyTicket residency;
    std::array<HlodRuntimeFrame, 3> frames;
};
struct HlodRuntime
{
    render_data::SceneHlod scene;
    HlodScenePreparation prepared_scene;
    std::string logical_id, content_generation;
    std::vector<RendererAssetSource> page_sources;
    std::vector<std::string> page_digests;
    std::vector<std::uint32_t> proxy_material_ordinals;
    // IO admission is a nonoverlapping forest frontier. Draw selection remains
    // GPU owned; these distinct page sets only reserve the shared stream pool.
    std::vector<std::vector<std::uint32_t>> group_pages;
    std::vector<std::uint32_t> group_priorities;
    std::vector<std::uint8_t> admitted_groups;
    std::uint32_t page_base{}, generation{};
    // Shared union admission: groups, distinct pages, missing slots, optional cap.
    std::array<std::uint32_t, 4> admission_status{};
    std::map<std::pair<std::uint32_t, std::uint64_t>, std::unique_ptr<HlodViewRuntime>> views;
};
struct HlodFrameGraph
{
    HlodViewRuntime *view{};
    HlodFrameOutputs outputs;
    BufferHandle refs{}, readback{};
    bool registered{};
};
struct HlodRuntimeFrameLayout
{
    bool valid{},active{};
    BufferHandle refs,readback;
    std::uint32_t copy_pass{};
    HlodLayout selection;
};
// Keep this input object in place between preparation and registration.
struct HlodRuntimeFrameRecordData
{
    HlodViewRuntime *view{};
    HlodFrameInputs inputs;
    HlodFrameRecordData selection;
};
} // namespace hs
