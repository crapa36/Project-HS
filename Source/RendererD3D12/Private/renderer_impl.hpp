#pragma once

#include <hs/core/dds_format.hpp>
#include <hs/core/process_info.hpp>
#include <hs/render_data/character_package.hpp>
#include <hs/render_data/character_animation_extension.hpp>
#include <hs/render_data/geometry_package.hpp>
#include <hs/render_data/material_schema.hpp>
#include <hs/render_data/material_sidecar.hpp>
#include <hs/render_data/surface_layout.hpp>
#include <hs/render_data/texture_package.hpp>
#include <hs/render_data/quality_profiles.hpp>
#include <hs/renderer/renderer.hpp>
#include <hs/renderer/render_frame_input.hpp>

#include "renderer_options.hpp"
#include "renderer_caster_storage.hpp"
#include "render_graph.hpp"

#include "d3d12_resources.hpp"
#include "persistent_gpu_scene.hpp"
#include "render_scene_extractor.hpp"
#include "render_instance_lookup.hpp"
#include "renderer_diagnostics.hpp"
#include "renderer_lod.hpp"
#include "renderer_visibility.hpp"
#include "residency_manager.hpp"
#include "skin_cache.hpp"
#include "streaming_manager.hpp"
#include "renderer_asset_io.hpp"
#include "renderer_texture_streaming.hpp"
#include "renderer_texture_feedback.hpp"
#include "vfx_persistent_state.hpp"
#include "vfx_ribbon_gpu.hpp"
#include "vfx_ribbon_state.hpp"
#include "view_state.hpp"
#include "reconstruction_plan.hpp"
#include "renderer_reconstruction_provider.hpp"
#include "renderer_display.hpp"
#include "renderer_cluster_catalog.hpp"
#include "renderer_cluster_runtime.hpp"
#include "renderer_cluster_graph.hpp"
#include "renderer_ambient_occlusion.hpp"
#include "renderer_adaptive_shading.hpp"
#include "renderer_reconstruction_guides.hpp"
#include "renderer_sdk_presentation.hpp"
#include "presentation-guides.hpp"
#include "renderer_subsurface.hpp"
#include "renderer_reflections.hpp"
#include "renderer_transparency.hpp"
#include "renderer_probe.hpp"
#include "renderer_ltc.hpp"
#include "renderer_hdr_post.hpp"
#include "renderer_screen_overlay.hpp"
#include "renderer_scene_geometry.hpp"
#include "renderer_brixelizer_gi.hpp"
#include "renderer_virtual_shadow.hpp"
#include "renderer_local_shadow.hpp"
#include "renderer_volumetrics.hpp"
#include "renderer_hlod_runtime.hpp"
#include "renderer_local_probe_runtime.hpp"
#include "renderer_feature_resolve.hpp"
#include "renderer_pipeline_cache.hpp"
#include "renderer_light_list.hpp"
#include "renderer_decal_surface.hpp"
#include "renderer_resident_assets.hpp"

#include <D3D12MemAlloc.h>

#include <Windows.h>
#ifdef DrawText
#undef DrawText
#endif
#include <d2d1.h>
#include <d3d12.h>
#include <dwrite_3.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <DirectXMath.h>

#if defined(HS_DEVELOPMENT_TOOLS)
#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM,
                                                             LPARAM);
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace hs
{
namespace renderer_detail
{

using Microsoft::WRL::ComPtr;

constexpr std::uint32_t kFrameCount = 3;
constexpr auto &kEnvironmentMaterialNames = render_data::kEnvironmentMaterialNames;
constexpr auto &kAuthoredMaterialNames = render_data::kAuthoredMaterialNames;
constexpr auto &kHeroMaterialNames = render_data::kHeroMaterialNames;
constexpr auto kResidentMaterialCount = kAuthoredMaterialNames.size() + kHeroMaterialNames.size();
constexpr auto kTerrainMaterialOrdinal = render_data::kTerrainMaterialOrdinal;
constexpr std::uint32_t kVisibilityVariantCount = static_cast<std::uint32_t>(kResidentMaterialCount + 1);
constexpr std::uint32_t kParticleCount = 10'000;
constexpr std::uint32_t kParticleDrawBinCount = 9; // sprite, seven atlas meshes, legacy shell
constexpr std::uint32_t kUiWidth = 1'920;
constexpr std::uint32_t kUiHeight = 1'080;
constexpr std::uint32_t kPostTextureDescriptorCount = 10;
constexpr std::uint32_t kCharacterDescriptorCount = 3;
constexpr std::uint32_t kMonsterPbrDescriptorCount = 5;
constexpr std::uint32_t kEnvironmentTextureCount = 20;
constexpr std::uint32_t kEnvironmentDescriptorCount = 22;
constexpr std::uint32_t kVfxGradientDescriptorCount = 1;
constexpr std::uint32_t kCharacterTextureSize = 2'048;
constexpr std::uint32_t kBaseTextureDescriptorCount =
    kPostTextureDescriptorCount + kCharacterDescriptorCount + kMonsterPbrDescriptorCount +
    kEnvironmentDescriptorCount + kVfxGradientDescriptorCount + 1 + 3 + 3 + 3 + 2 + 3 + 2;
constexpr std::uint32_t kBloomTextureDescriptorOffset = kBaseTextureDescriptorCount - 5;
constexpr std::uint32_t kTemporalTextureDescriptorOffset = kBaseTextureDescriptorCount - 2;
constexpr std::uint32_t kToneMapInputDescriptorOffset = kBaseTextureDescriptorCount;
constexpr std::uint32_t kFxaaInputDescriptorOffset =
    kToneMapInputDescriptorOffset + kPostTextureDescriptorCount;
constexpr std::uint32_t kMaterialTextureDescriptorOffset =
    kBaseTextureDescriptorCount + 2 * kPostTextureDescriptorCount;
constexpr std::uint32_t kMaterialTextureDescriptorCount =
    static_cast<std::uint32_t>(kAuthoredMaterialNames.size() * render_data::kMaterialTextureSlots);
constexpr std::uint32_t kGeometryPageCapacity = render_data::kGeometryPageCapacity;
constexpr std::uint32_t kGeometryBuffersPerPage = render_data::kGeometryBuffersPerPage;
constexpr std::uint32_t kGeometryDescriptorOffset =
    kMaterialTextureDescriptorOffset + kMaterialTextureDescriptorCount;
constexpr std::uint32_t kGeometryDescriptorCount =
    1 + kGeometryPageCapacity * kGeometryBuffersPerPage;
constexpr std::uint32_t kSurfaceDescriptorOffset =
    kGeometryDescriptorOffset + kGeometryDescriptorCount;
constexpr std::uint32_t kSurfaceDescriptorCount = 4;
constexpr std::uint32_t kVisibilitySrvDescriptorOffset =
    kSurfaceDescriptorOffset + kSurfaceDescriptorCount;
constexpr std::uint32_t kVisibilitySrvDescriptorCount = 14;
constexpr std::uint32_t kVisibilityUavDescriptorOffset =
    kVisibilitySrvDescriptorOffset + kVisibilitySrvDescriptorCount;
constexpr std::uint32_t kVisibilityUavDescriptorCount = 14;
constexpr std::uint32_t kMaterialParameterDescriptorOffset =
    kVisibilityUavDescriptorOffset + kVisibilityUavDescriptorCount;
constexpr std::uint32_t kViewDescriptorOffset = kMaterialParameterDescriptorOffset + 1;
constexpr std::uint32_t kViewDescriptorCount = 13;
constexpr std::uint32_t kHeroBindingDescriptorOffset = kViewDescriptorOffset + kViewDescriptorCount;
constexpr std::uint32_t kProbeDescriptorOffset = kHeroBindingDescriptorOffset + 1;
constexpr std::uint32_t kLightListDescriptorOffset = kProbeDescriptorOffset + 3;
constexpr std::uint32_t kSceneGeometryDescriptorOffset = kLightListDescriptorOffset + 6;
constexpr std::uint32_t kVirtualShadowDescriptorOffset = kSceneGeometryDescriptorOffset + kSceneGeometryDescriptorCount;
constexpr std::uint32_t kLocalProbeDescriptorOffset = kVirtualShadowDescriptorOffset + kVirtualShadowDescriptorCount;
constexpr std::uint32_t kVolumetricDescriptorOffset = kLocalProbeDescriptorOffset + 5;
constexpr std::uint32_t kHlodDescriptorOffset = kVolumetricDescriptorOffset + kVolumetricDescriptorCount;
constexpr std::uint32_t kLocalShadowDescriptorOffset = kHlodDescriptorOffset + 2;
constexpr std::uint32_t kBrixelizerDescriptorOffset = kLocalShadowDescriptorOffset + kLocalShadowDescriptorCount;
constexpr std::uint32_t kTextureFeedbackDescriptorOffset = kBrixelizerDescriptorOffset + kBrixelizerDescriptorCount;
constexpr std::uint32_t kVfxScreenReadDescriptorOffset = kTextureFeedbackDescriptorOffset + kTextureFeedbackDescriptorCount;
constexpr std::uint32_t kEntitySurfaceNoiseDescriptorOffset = kVfxScreenReadDescriptorOffset + 1;
constexpr std::uint32_t kDecalSurfaceDescriptorOffset = kEntitySurfaceNoiseDescriptorOffset + 1;
constexpr std::uint32_t kHeroTransparencyDescriptorOffset = kDecalSurfaceDescriptorOffset + kDecalSurfaceDescriptorCount;
constexpr std::uint32_t kSceneFrameTableCount = kHeroTransparencyDescriptorOffset - kTemporalTextureDescriptorOffset;
constexpr std::uint32_t kSceneGeometryTableOffset = kHeroTransparencyDescriptorOffset + kHeroTransparencyDescriptorsPerFrame;
constexpr std::uint32_t kVolumetricFrameTableOffset = kSceneGeometryTableOffset + kSceneFrameTableCount;
constexpr std::uint32_t kBrixelizerFrameTableOffset = kVolumetricFrameTableOffset + kSceneFrameTableCount;
constexpr std::uint32_t kTextureDescriptorCount = kBrixelizerFrameTableOffset + 4 * kSceneFrameTableCount;
constexpr std::uint32_t kArcherGeometryPage =
    static_cast<std::uint32_t>(kEnvironmentMaterialNames.size() * render_data::kEnvironmentGeometryLodCount);
constexpr std::uint32_t kMonsterGeometryPage = kArcherGeometryPage + 1;
constexpr std::uint32_t kProjectileGeometryPage = kMonsterGeometryPage + 6;
constexpr std::uint32_t kProceduralGeometryPage = kProjectileGeometryPage + 1;
static_assert(render_data::kMaterialTextureSlots == 6);
static_assert(kVisibilityVariantCount == 24);
constexpr std::uint32_t kVfxDistortionDescriptorOffset =
    kPostTextureDescriptorCount + kCharacterDescriptorCount +
    kMonsterPbrDescriptorCount + kEnvironmentDescriptorCount +
    kVfxGradientDescriptorCount + 1 + 2 + 1 + 3;
constexpr std::uint32_t kDistortionRtvOffset = 13;
constexpr std::uint32_t kBloomRtvOffset = kDistortionRtvOffset + kFrameCount;
constexpr std::uint32_t kTemporalRtvOffset = kBloomRtvOffset + 3 * kFrameCount;
constexpr std::uint32_t kSurfaceHistoryRtvOffset = kTemporalRtvOffset + 2;
constexpr std::uint32_t kVisibilityRtvOffset = kSurfaceHistoryRtvOffset + 1;
constexpr std::uint32_t kMaterialSidecarRtvOffset = kVisibilityRtvOffset + 2;
constexpr std::uint32_t kViewRtvOffset = kMaterialSidecarRtvOffset + 2;
constexpr std::uint32_t kViewRtvCount = 7;
constexpr std::uint32_t kLightingAuxRtvOffset = kViewRtvOffset + kViewRtvCount;
constexpr std::uint32_t kLightingAuxRtvCount = 3;
constexpr std::uint32_t kInstanceDataOffset = 14 * 1024;
constexpr std::array<std::string_view, 25> kRenderPassNames{
    "Scene Buffer Upload", "GPU Particle Spawn/Update", "3-cascade Directional Shadow",
    "Static Visibility", "GBuffer+Depth", "Deferred Cel Lighting", "VFX Distortion Vectors",
    "Forward Transparent/OIT", "OIT Composite", "Temporal Resolve", "Bloom Half Extract",
    "Bloom Quarter Downsample", "Bloom Half Upsample", "Bloom", "ToneMap",
    "Screen-space Outline", "FXAA", "Game UI", "Depth Pyramid", "Visibility Retest",
    "GBuffer Retest", "Final Depth Pyramid", "Skin Cache", "UI Upload", "Temporal Initialization"};
constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;

#if defined(HS_DEVELOPMENT_TOOLS)
constexpr std::array<const char *, 8> kDebugSkillNames{
    "관통 사격", "다중 사격", "충전 사격", "폭발 화살",
    "도탄 화살", "화살비", "덫", "후퇴 사격"};
constexpr std::array<std::array<const char *, 8>, 8> kDebugUpgradeNames{{
    {{"후속 화살", "사거리 끝 분열", "관통 출혈", "피해 궤적",
      "관통 연쇄 사격", "적 밀어 정렬", "화상 전달", "빠른 재사용"}},
    {{"2차 부채", "적중 분열", "추가 관통", "후방 사격",
      "출혈 부채", "화상 전달", "근거리 집중 사격", "빗나감 재추적"}},
    {{"과충전 폭발", "빠른 충전", "이동 충전", "관통 감쇠 제거",
      "완전 충전 출혈", "관통 분열", "화상 폭발", "다중 처치 쿨타임 회수"}},
    {{"재폭발", "소형 폭탄", "파편 폭발", "화상 지대",
      "출혈 연쇄 폭발", "폭발 흡인", "액티브 연계 표식", "빠른 재사용"}},
    {{"귀환 도탄", "분기 도탄", "출혈 도탄 연장", "화상 전달",
      "처치 소형 화살", "처치 연쇄 갱신", "귀환 쿨타임 회수", "빠른 재사용"}},
    {{"2차 화살비", "첫 타격 흡인", "반복 적중 출혈", "화상 지대",
      "추적 화살", "둔화 지대", "처치 추적 화살", "추가 타격과 둔화"}},
    {{"연속 덫", "착지 둔화", "덫 재활성", "출혈 덫",
      "화상 덫", "흡인 덫", "액티브 연계 표식", "처치 덫"}},
    {{"세 갈래 사격", "출발점 덫", "둔화 궤적", "출혈 추적 화살",
      "착지 충격", "다음 스킬 쿨타임 회수", "다중 적중 회복", "추가 후퇴"}}
}};
constexpr std::array<const char *, 20> kDebugRelicNames{
     "피의 회복", "번지는 불꽃", "한기 폭발", "피와 불", "팔방 사격", "추격 본능",
     "잔상 사격", "연계 숙련", "교차 사격", "충격 반격", "위기 회복", "경험의 파동",
     "Projectile Cadence", "Pre-Damage Guard", "Slow Synergy", "Area Resonance",
     "Boss Pressure", "Hit Streak", "Pickup Reward", "Low-Health Survival"};
constexpr std::array<const char *, 6> kDebugStatNames{
    "최대 체력", "이동속도", "공격력", "공격속도", "쿨타임 감소", "자석 반경"};
#endif

using Vertex = SkinnedVertex;

struct GpuStaticVisibilityRange
{
    std::uint32_t begin{};
    std::uint32_t count{};
    std::uint32_t index_count{};
    std::uint32_t argument_stride{};
    std::uint32_t draw_record_base{};
    std::uint32_t meshlet_count{};
    std::uint32_t flags{};
    std::uint32_t reserved{};
};
static_assert(sizeof(GpuStaticVisibilityRange) == 32);

struct HzbLevelLayout
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint64_t offset{};
};

struct HzbLayout
{
    std::array<HzbLevelLayout, 16> levels{};
    std::uint32_t count{};
    std::uint64_t elements{};
};

[[nodiscard]] inline HzbLayout MakeHzbLayout(std::uint32_t width,
                                             std::uint32_t height) noexcept
{
    HzbLayout layout;
    width = std::max(width, 1u);
    height = std::max(height, 1u);
    while (layout.count < layout.levels.size())
    {
        auto &level = layout.levels[layout.count++];
        level = {width, height, layout.elements};
        layout.elements += static_cast<std::uint64_t>(width) * height;
        if (width == 1 && height == 1) break;
        width = std::max((width + 1u) / 2u, 1u);
        height = std::max((height + 1u) / 2u, 1u);
    }
    return layout;
}

struct alignas(16) GpuFresnelShell
{
    std::array<float, 16> current_transform{};
    std::array<float, 16> previous_transform{};
    DirectX::XMFLOAT4 color{};
    DirectX::XMFLOAT4 geometry{}; // Footprint radius, source progress, elapsed seconds, HDR.
    DirectX::XMFLOAT4 motion{};   // Rate, amplitude, inset fraction, crown rotation Hz.
    DirectX::XMFLOAT4 signature{}; // Transition Fresnel power.
    DirectX::XMUINT4 metadata{};  // Kind, sector count, end crack, gradient row.
    DirectX::XMUINT4 identity{};  // Render-instance and effect stable IDs, split low/high.
};
static_assert(sizeof(GpuFresnelShell) == 224);

// One immutable resident page. The auxiliary buffer packs refs, local triangles,
// then canonical remap; offsets are bytes. A missing page resolves its resident
// fallback before constructing draw records or touching GPU resources.
struct GeometryPageGpu
{
    AssetId source_asset{};
    std::string source_product;
    std::array<std::byte, 32> source_digest{};
    AllocationResource vertices;
    AllocationResource indices;
    AllocationResource meshlets;
    AllocationResource auxiliary;
    ResidencyTicket residency;
    D3D12_VERTEX_BUFFER_VIEW vertex_view{};
    D3D12_INDEX_BUFFER_VIEW index_view{};
    std::uint32_t vertex_count{};
    std::uint32_t index_count{};
    std::uint32_t meshlet_count{};
    std::uint32_t triangle_offset{};
    std::uint32_t remap_offset{};
    std::uint32_t page_slot{UINT32_MAX};
    std::uint32_t fallback_page_slot{UINT32_MAX};
    std::uint32_t generation{1};
    render_data::GeometryMetadata metadata;
    std::vector<render_data::GeometrySubmesh> submeshes;

    void Reset() noexcept
    {
        vertices.Reset(); indices.Reset(); meshlets.Reset(); auxiliary.Reset();
        residency = {};
        vertex_view = {}; index_view = {};
        vertex_count = index_count = meshlet_count = triangle_offset = remap_offset = 0;
        page_slot = fallback_page_slot = UINT32_MAX;
        generation = 1;
        metadata = {};
        submeshes.clear();
        source_asset = {}; source_product.clear(); source_digest = {};
    }
};

struct MonsterAsset : GeometryPageGpu
{
    std::vector<render_data::CharacterMaterialBinding> material_bindings;
    std::uint32_t bone_count{};
    std::uint32_t material_count{};
    float ground_offset{};
    std::uint32_t skin_offset{};
    std::array<DirectX::XMUINT4, static_cast<std::size_t>(CharacterAnimationClip::Count)> clip_meta{};
};

struct FrameConstants
{
    DirectX::XMFLOAT4X4 view_projection;
    DirectX::XMFLOAT4X4 previous_view_projection;
    DirectX::XMFLOAT4 temporal_options;
    DirectX::XMFLOAT4 camera_time;
    DirectX::XMFLOAT4 light_direction_intensity;
    DirectX::XMFLOAT4 light_color;
    DirectX::XMFLOAT4 screen_size;
    DirectX::XMFLOAT4 camera_forward_softness;
    DirectX::XMFLOAT4X4 shadow_view_projection[3];
    DirectX::XMFLOAT4 shadow_atlas_scale_offset[3];
    DirectX::XMFLOAT4 shadow_atlas_texel_size;
    DirectX::XMFLOAT4X4 archer_bones[kMaxCharacterBones];
    DirectX::XMUINT4 monster_asset_meta[6];
    DirectX::XMUINT4 monster_clip_meta[6][static_cast<std::size_t>(CharacterAnimationClip::Count)];
    DirectX::XMFLOAT4 render_options;
    DirectX::XMUINT4 particle_options;
    DirectX::XMFLOAT4 grass_benders[32];
    DirectX::XMUINT4 grass_bender_count;
    DirectX::XMUINT4 authored_particle_clock;
    DirectX::XMFLOAT4 vfx_light_position_radius[32];
    DirectX::XMFLOAT4 vfx_light_color_intensity[32];
    DirectX::XMUINT4 vfx_light_count;
    DirectX::XMUINT4 vfx_decal_count;
    DirectX::XMUINT4 skin_cache_options;
    DirectX::XMFLOAT4X4 inverse_view_projection;
    DirectX::XMFLOAT4X4 current_unjittered_view_projection;
    DirectX::XMFLOAT4X4 previous_unjittered_view_projection;
    DirectX::XMFLOAT4 surface_view_origin;
    DirectX::XMFLOAT4 surface_jitter_uv;
    DirectX::XMFLOAT4 lod_view_right, lod_view_up, lod_view_forward;
    DirectX::XMFLOAT4 lod_projection, lod_options;
    DirectX::XMUINT4 lod_clock;
    std::array<DirectX::XMUINT4, 4> material_parameter_bases;
    std::array<DirectX::XMUINT4, 4> material_varying_masks;
    DirectX::XMUINT4 material_sidecar_offsets[2];
    DirectX::XMUINT4 material_sidecar_info;
    DirectX::XMFLOAT4 output_size;
    DirectX::XMFLOAT4 post_options;
    DirectX::XMFLOAT4 material_quality_options;
    DirectX::XMFLOAT4 display_options;
    DirectX::XMFLOAT4X4 inverse_unjittered_view_projection;
    DirectX::XMFLOAT4 lighting_options;
    std::array<DirectX::XMUINT4, 2> hero_material_parameter_bases;
    std::array<DirectX::XMUINT4, 2> hero_material_varying_masks;
    std::array<DirectX::XMUINT4, 7> hero_binding_ranges;
    DirectX::XMFLOAT4 probe_options;
    std::array<DirectX::XMFLOAT4, 32> previous_grass_benders;
    DirectX::XMFLOAT4 previous_grass_state;
    DirectX::XMFLOAT4 previous_grass_origin_delta;
    DirectX::XMUINT4 scene_table_bounds;
    DirectX::XMFLOAT4 grass_bender_grid;
    std::array<DirectX::XMUINT4, 16> grass_bender_masks;
    DirectX::XMFLOAT4 previous_grass_bender_grid;
    std::array<DirectX::XMUINT4, 16> previous_grass_bender_masks;
    DirectX::XMFLOAT4 particle_terrain;
};

constexpr std::uint32_t kExposureConstantsOffset = kInstanceDataOffset - 256;
static_assert(sizeof(FrameConstants) == 12848);
static_assert(offsetof(FrameConstants, screen_size) == 192);
static_assert(offsetof(FrameConstants, lod_clock) == 11200);
static_assert(offsetof(FrameConstants, output_size) == 11392);
static_assert(offsetof(FrameConstants, probe_options) == 11712);
static_assert(offsetof(FrameConstants, previous_grass_benders) == 11728);
static_assert(offsetof(FrameConstants, previous_grass_state) == 12240);
static_assert(offsetof(FrameConstants, previous_grass_origin_delta) == 12256);
static_assert(offsetof(FrameConstants, scene_table_bounds) == 12272);
static_assert(offsetof(FrameConstants, grass_bender_grid) == 12288);
static_assert(offsetof(FrameConstants, grass_bender_masks) == 12304);
static_assert(offsetof(FrameConstants, previous_grass_bender_grid) == 12560);
static_assert(offsetof(FrameConstants, previous_grass_bender_masks) == 12576);
static_assert(offsetof(FrameConstants, particle_terrain) == 12832);
static_assert(sizeof(FrameConstants) <= kExposureConstantsOffset);

struct ExposureConstants
{
    std::uint32_t width{}, height{};
    float delta_seconds{};
    std::uint32_t reset{};
    float minimum_log_luminance{-12.0f}, maximum_log_luminance{16.0f};
    float lower_percentile{0.02f}, upper_percentile{0.98f};
    float target_grey{0.18f}, minimum{0.0001f}, maximum{64.0f}, adaptation_up{3.0f};
    float adaptation_down{1.0f};
    float padding[3]{};
};
static_assert(sizeof(ExposureConstants) == 64);

struct GpuParticle
{
    DirectX::XMFLOAT4 position_life;
    DirectX::XMFLOAT4 initial_position_spawn_time;
    DirectX::XMFLOAT4 initial_velocity_max_life;
    DirectX::XMFLOAT4 start_color;
    DirectX::XMFLOAT4 end_color;
    DirectX::XMFLOAT4 size_rotation;
    DirectX::XMFLOAT4 physics_metadata;
    DirectX::XMFLOAT4 current_velocity_drag;
    DirectX::XMFLOAT4 collision_material;
    DirectX::XMUINT4 authored;
};
static_assert(sizeof(GpuParticle) == 160);

struct GpuParticleSpawnCommand
{
    DirectX::XMFLOAT4 position_lifetime_min;
    DirectX::XMFLOAT4 direction_lifetime_max;
    DirectX::XMFLOAT4 shape_extent_speed_min;
    DirectX::XMFLOAT4 speed_cone_gravity_stretch;
    DirectX::XMFLOAT4 start_color;
    DirectX::XMFLOAT4 end_color;
    DirectX::XMFLOAT4 size_range;
    DirectX::XMFLOAT4 rotation_range;
    DirectX::XMUINT4 modes;
    DirectX::XMUINT4 metadata;
    DirectX::XMFLOAT4 current_velocity_drag;
    DirectX::XMFLOAT4 collision_material;
    DirectX::XMUINT4 authored;
};
static_assert(sizeof(GpuParticleSpawnCommand) == 208);


struct UiCpuSurface
{
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1RenderTarget> target;
    ComPtr<ID2D1SolidColorBrush> brush;
};

[[nodiscard]] inline D3D12_RESOURCE_DESC BufferDescription(std::uint64_t size,
                                                    D3D12_RESOURCE_FLAGS flags =
                                                        D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Alignment = 0;
    description.Width = size;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_UNKNOWN;
    description.SampleDesc = {1, 0};
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = flags;
    return description;
}

[[nodiscard]] inline D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

[[nodiscard]] inline std::string Utf8(std::wstring_view text)
{
    if (text.empty()) return {};
    const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0,
                                          nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), result.data(), size,
                        nullptr, nullptr);
    return result;
}

[[nodiscard]] inline Result ReadBinary(const std::filesystem::path &path,
                                       std::vector<std::byte> &bytes,
                                       std::optional<std::uint64_t> expected_bytes = std::nullopt)
{
    return ReadCheckedBinary(path, bytes, expected_bytes);
}

struct DdsHeaderDx10
{
    DXGI_FORMAT format;
    D3D12_RESOURCE_DIMENSION dimension;
    std::uint32_t misc_flag;
    std::uint32_t array_size;
    std::uint32_t misc_flags2;
};

constexpr std::array<Vertex, 36> kCubeVertices = {
    Vertex{{-0.5f, -0.5f, -0.5f}, {0, 0, -1}}, Vertex{{0.5f, 0.5f, -0.5f}, {0, 0, -1}},
    Vertex{{0.5f, -0.5f, -0.5f}, {0, 0, -1}},  Vertex{{-0.5f, -0.5f, -0.5f}, {0, 0, -1}},
    Vertex{{-0.5f, 0.5f, -0.5f}, {0, 0, -1}},  Vertex{{0.5f, 0.5f, -0.5f}, {0, 0, -1}},
    Vertex{{-0.5f, -0.5f, 0.5f}, {0, 0, 1}},   Vertex{{0.5f, -0.5f, 0.5f}, {0, 0, 1}},
    Vertex{{0.5f, 0.5f, 0.5f}, {0, 0, 1}},     Vertex{{-0.5f, -0.5f, 0.5f}, {0, 0, 1}},
    Vertex{{0.5f, 0.5f, 0.5f}, {0, 0, 1}},     Vertex{{-0.5f, 0.5f, 0.5f}, {0, 0, 1}},
    Vertex{{-0.5f, -0.5f, -0.5f}, {-1, 0, 0}}, Vertex{{-0.5f, -0.5f, 0.5f}, {-1, 0, 0}},
    Vertex{{-0.5f, 0.5f, 0.5f}, {-1, 0, 0}},   Vertex{{-0.5f, -0.5f, -0.5f}, {-1, 0, 0}},
    Vertex{{-0.5f, 0.5f, 0.5f}, {-1, 0, 0}},   Vertex{{-0.5f, 0.5f, -0.5f}, {-1, 0, 0}},
    Vertex{{0.5f, -0.5f, -0.5f}, {1, 0, 0}},   Vertex{{0.5f, 0.5f, 0.5f}, {1, 0, 0}},
    Vertex{{0.5f, -0.5f, 0.5f}, {1, 0, 0}},    Vertex{{0.5f, -0.5f, -0.5f}, {1, 0, 0}},
    Vertex{{0.5f, 0.5f, -0.5f}, {1, 0, 0}},    Vertex{{0.5f, 0.5f, 0.5f}, {1, 0, 0}},
    Vertex{{-0.5f, 0.5f, -0.5f}, {0, 1, 0}},   Vertex{{-0.5f, 0.5f, 0.5f}, {0, 1, 0}},
    Vertex{{0.5f, 0.5f, 0.5f}, {0, 1, 0}},     Vertex{{-0.5f, 0.5f, -0.5f}, {0, 1, 0}},
    Vertex{{0.5f, 0.5f, 0.5f}, {0, 1, 0}},     Vertex{{0.5f, 0.5f, -0.5f}, {0, 1, 0}},
    Vertex{{-0.5f, -0.5f, -0.5f}, {0, -1, 0}}, Vertex{{0.5f, -0.5f, 0.5f}, {0, -1, 0}},
    Vertex{{-0.5f, -0.5f, 0.5f}, {0, -1, 0}},  Vertex{{-0.5f, -0.5f, -0.5f}, {0, -1, 0}},
    Vertex{{0.5f, -0.5f, -0.5f}, {0, -1, 0}},  Vertex{{0.5f, -0.5f, 0.5f}, {0, -1, 0}},
};

} // namespace renderer_detail

using namespace renderer_detail;

struct D3D12Renderer::Impl
{
    struct ShadowCasterKeyHash
    {
        std::size_t operator()(const std::tuple<bool, std::uint64_t, std::uint32_t> &key) const noexcept
        {
            auto hash = std::hash<std::uint64_t>{}(std::get<1>(key));
            hash ^= std::hash<std::uint32_t>{}(std::get<2>(key)) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
            return hash ^ (std::get<0>(key) + 0x9e3779b9u + (hash << 6) + (hash >> 2));
        }
    };
    struct ShadowCasterRecord
    {
        VirtualShadowCaster caster;
        std::uint64_t prepared_epoch{};
    };
    using ShadowCasterMap = std::unordered_map<std::tuple<bool, std::uint64_t, std::uint32_t>,
        ShadowCasterRecord, ShadowCasterKeyHash>;
    using ShadowCasterNodes = ShadowCasterNodeCache<ShadowCasterMap, std::shared_ptr<const StaticRenderScene>>;
    struct UiResources
    {
        ~UiResources();
        std::array<AllocationResource, kFrameCount> textures, uploads;
        std::array<std::byte *, kFrameCount> mapped{};
        std::array<UiCpuSurface, kFrameCount> surfaces;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        std::uint32_t rows{}, raster_width{}, raster_height{};
        std::uint64_t row_bytes{}, upload_bytes{};
    };
    struct OptionTargets
    {
        AllocationResource depth, shadow;
        AllocationResource gbuffer_base, gbuffer_normal, gbuffer_metadata, gbuffer_material,
            gbuffer_motion, gbuffer_surface_history;
        std::array<AllocationResource, 2> material_sidecars;
        AllocationResource visibility_ids, visibility_front_face, visibility_pixels,
            visibility_counts, visibility_dispatch_args;
        AllocationResource hdr_color, oit_accumulation, oit_revealage;
        AllocationResource subsurface_diffuse, environment_specular, indirect_diffuse;
        AllocationResource post_a, post_b;
        std::array<AllocationResource, 2> temporal_history;
        std::array<AllocationResource, 2> temporal_normal_history, temporal_material_history;
        AllocationResource scene_work, display_linear, composite_hdr;
        std::array<AllocationResource, 2> hzb_history;
        HzbLayout hzb_layout{};
        std::array<TransientResourcePool, kFrameCount> transient_pools;
    };
    struct CanonicalSkinFrame
    {
        AllocationResource skin_cache_vertices, skin_cache_bounds;
        CompletionSet skin_cache_use_completion;
        std::size_t skin_cache_vertex_capacity{}, skin_cache_bound_capacity{}, skin_cache_slot_stride{};
        bool skin_cache_valid{};
        std::uint64_t package_id{}, signature{};
    };
    static AllocationResource RetainViewAllocation(const AllocationResource &source)
    {
        AllocationResource retained;
        retained.resource = source.resource;
        retained.allocation = source.allocation;
        if (retained.allocation) retained.allocation->AddRef();
        retained.graph_access = source.graph_access;
        retained.graph_queue = source.graph_queue;
        retained.graph_completion = source.graph_completion;
        return retained;
    }
    struct SubmittedInstanceFrame
    {
        std::vector<GpuInstance> instances;
        std::vector<std::uint32_t> generations;
        RenderWorldPosition scene_origin;
        std::uint64_t package_id{};
    };
    // Dormant views own their complete GPU/history state. All queues drain
    // before exchange because execution RTV/DSV banks are shared scratch.
    // Scene/assets/canonical deformation remain common owners.
    struct OwnedView
    {
        RenderViewRole role{RenderViewRole::Main};
        OptionTargets targets;
        UiResources ui;
        std::array<ComPtr<ID3D12Resource>, kFrameCount> outputs;
        std::array<AllocationResource, kFrameCount> offscreen_allocations;
        std::array<CompletionSet, kFrameCount> output_completion{};
        AllocationResource hdr_capture_source;
        ViewExtent hdr_capture_extent;
        bool hdr_capture_valid{}, hdr_capture_pre_exposed{};
        std::uint64_t hdr_capture_submission_id{}, hdr_capture_package_id{}, hdr_capture_snapshot_tick{};
        std::string hdr_capture_source_name{"reconstructed_hdr_before_post"};
        std::array<AllocationResource, 2> exposure_values;
        AllocationResource exposure_histogram, initial_exposure;
        std::array<std::vector<UiModel>, kFrameCount> ui_models;
        std::array<bool, kFrameCount> ui_dirty{}, ui_initialized{};
        AmbientOcclusionSystem ambient_occlusion;
        AdaptiveShadingSystem adaptive_shading;
        SubsurfaceSystem subsurface;
        ScreenReflectionsSystem screen_reflections;
        HeroTransparencySystem hero_transparency;
        HdrPostSystem hdr_post;
        ScreenOverlaySystem screen_overlay;
        BrixelizerGiSystem brixelizer_gi;
        VirtualShadowSystem virtual_shadows;
        LocalShadowSystem local_shadows;
        VolumetricSystem volumetrics;
        LightListSystem light_list;
        DecalSurfaceSystem decal_surface;
        ReconstructionGuidesSystem reconstruction_guides;
        std::unique_ptr<ReconstructionProvider> reconstruction_provider;
        ReconstructionPlan reconstruction_plan;
        SdkCamera sdk_camera;
        SdkFrameIdentity sdk_frame_identity;
        ViewState state;
        DisplayState display_state;
        DynamicResolutionController dynamic_resolution_controller;
        GpuPassTimings completed_gpu_timings;
        std::uint64_t dynamic_resolution_sample_id{};
        SkinCache skin_history;
        ClusterViewRuntime cluster_view;
        std::shared_ptr<CanonicalSkinFrame> previous_skin_deformation;
        std::shared_ptr<const SubmittedInstanceFrame> previous_instances;
        std::array<DirectX::XMFLOAT4, 32> previous_grass_benders{};
        DirectX::XMFLOAT4 previous_grass_state{};
        std::shared_ptr<const StaticRenderScene> scene_lease;
        std::uint64_t view_session_id{};
        GpuLodStateBuffer dynamic_lod_states;
        GpuLodStateBuffer static_lod_states;
        std::uint32_t lod_session_epoch{};
        VolumetricCamera previous_volumetric_camera;
        ShadowCasterMap previous_shadow_casters;
        ShadowCasterMap scratch_shadow_casters;
#ifndef NDEBUG
        std::unordered_map<ShadowCasterMap::key_type, std::uint64_t, ShadowCasterKeyHash> shadow_caster_key_oracle;
        bool shadow_caster_key_oracle_prepared{};
#endif
        ShadowCasterNodes previous_shadow_caster_nodes, scratch_shadow_caster_nodes;
        std::uint64_t shadow_caster_preparation_epoch{};
        std::array<std::uint32_t, kGeometryPageCapacity> previous_geometry_page_versions{};
        std::uint32_t width{}, height{}, render_width{}, render_height{}, last_presented_index{};
        std::uint32_t exposure_read_index{}, temporal_read_index{}, hzb_read_index{};
        bool exposure_initialized{}, view_targets_initialized{}, temporal_history_valid{},
            temporal_targets_initialized{}, hzb_history_valid{}, material_sidecars_initialized{},
            visibility_targets_initialized{}, transient_textures_common{}, brixelizer_history_valid{};
        std::chrono::steady_clock::time_point exposure_sample_time{}, render_submission_time{};
        DirectX::XMFLOAT4X4 previous_view_projection{}, previous_unjittered_view_projection{};
        DirectX::XMFLOAT4 previous_projection_depth{};
        DirectX::XMFLOAT2 previous_jitter_uv{};
        Float3 temporal_previous_eye{};
        float temporal_previous_fov{};
        RenderWorldPosition previous_scene_origin{}, previous_view_origin{};
        bool previous_origins_valid{};
        std::uint64_t frame_number{}, configuration_revision{}, history_generation{1};
        std::uint32_t native_viewport_id{};
        std::string content_generation;
        std::uint64_t pipeline_revision{};
        std::vector<ViewHistoryKey> history_keys;
        std::map<ViewHistoryKey, std::array<ID3D12Resource *, 2>> history_resources;
        bool ready{};
    };
    [[nodiscard]] Result PrepareOwnedView(const RenderViewInput &, std::unique_ptr<OwnedView> &, bool decal_work);
    [[nodiscard]] Result PrepareDecalSurface(const RendererConfig &, ViewExtent, bool, DecalSurfaceCandidate &) const;
    [[nodiscard]] Result ActivateView(ViewId);
    void ExchangeView(OwnedView &) noexcept;
    [[nodiscard]] Result ResetOwnedViewAfterCompletion(OwnedView &);
    [[nodiscard]] Result ResetOwnedViewsAfterCompletion();
    void InvalidateCanonicalDeformationAfterCompletion() noexcept;
    [[nodiscard]] Result ValidateFrameInput(const RenderFrameInput &) const;
    [[nodiscard]] Result ValidateFramePayload(const RenderFrameInput &) const;
    void RecordViewHistoryKeys(ViewId);
    [[nodiscard]] RendererDiagnosticsMemory DiagnosticsMemory() const;
    struct StaticGpuScene
    {
        std::shared_ptr<const StaticRenderScene> source;
        AllocationResource instances;
        AllocationResource upload;
        GpuSceneTableLayout layout{};
        GpuLodStateBuffer lod_states;
        CompletionSet last_completion{};
        CompletionSet upload_completion{};
        bool submitted{};
        std::uint32_t instance_generation{};
    };
    struct RetiredDynamicInstanceBuffer
    {
        AllocationResource instances;
        CompletionSet completion{};
    };
    struct RetiredStreamingTexture
    {
        AllocationResource texture;
        ResidencyTicket residency;
        CompletionSet completion{};
    };
    struct EnvironmentTextureStream
    {
        std::filesystem::path path;
        std::string logical_id;
        std::string source_id;
        StreamingRequestId request;
        std::future<std::vector<std::byte>> read_future;
        std::vector<std::byte> decoded_storage;
        ResidencyTicket decoded_residency;
        ResidencyTicket decode_reservation;
        std::uint64_t read_bytes{}, last_demand_frame{};
        std::uint32_t priority{};
        AllocationResource candidate;
        AllocationResource upload;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
        std::uint32_t subresource_count{};
        CompletionSet upload_completion{};
        CompletionSet fallback_use{};
        CompletionSet fine_use{}, retirement{};
        std::array<std::uint64_t, kFrameCount> frame_generation{1, 1, 1};
        ResidencyTicket fallback_residency;
        ResidencyTicket candidate_residency;
        ResidencyTicket upload_residency;
        std::uint64_t generation{1};
        std::uint64_t requests{};
        std::uint64_t uploads{};
        std::uint64_t publications{};
        std::uint64_t reclaims{};
        bool upload_pending{};
        bool upload_submitted{};

        bool fine_resident{}, retiring{};

        void ResetRuntime() noexcept
        {
            path.clear();
            logical_id.clear();
            source_id.clear();
            request = {};
            if (read_future.valid())
                read_future.wait();
            read_future = {};
            candidate.Reset();
            upload.Reset();
            footprints.clear();
            subresource_count = 0;
            upload_completion = {};
            fallback_use = {};
            fine_use = {}; retirement = {};
            frame_generation = {1, 1, 1};
            fallback_residency = {};
            candidate_residency = {};
            upload_residency = {};
            std::vector<std::byte>{}.swap(decoded_storage);
            decoded_residency = {};
            read_bytes = last_demand_frame = 0;
            decode_reservation = {};
            priority = 0;
            generation = 1;
            requests = uploads = publications = reclaims = 0;
            upload_pending = false;
            upload_submitted = false;
            fine_resident = retiring = false;
        }
    };
    struct EnvironmentGeometryStream
    {
        std::filesystem::path path;
        std::string logical_id;
        std::string source_id;
        std::string content_generation;
        StreamingRequestId request;
        std::future<std::vector<std::byte>> read_future;
        std::vector<std::byte> decoded_storage;
        ResidencyTicket decoded_residency;
        ResidencyTicket decode_reservation;
        std::uint64_t read_bytes{}, last_demand_frame{};
        GeometryPageGpu candidate;
        AllocationResource upload;
        std::array<std::uint64_t, kGeometryBuffersPerPage> upload_offsets{};
        render_data::GeometryMetadata metadata;
        CompletionSet upload_completion{};
        CompletionSet prior_use{};
        CompletionSet retirement{};
        std::array<std::uint64_t, kFrameCount> frame_generation{1, 1, 1};
        ResidencyTicket upload_residency;
        std::uint32_t target_index{};
        std::uint32_t fallback_index{};
        std::uint32_t priority{};
        std::uint64_t generation{1};
        std::uint64_t requests{};
        std::uint64_t uploads{};
        std::uint64_t publications{};
        std::uint64_t reclaims{};
        bool upload_pending{};
        bool upload_submitted{};
        bool retiring{};

        void ResetRuntime() noexcept
        {
            path.clear();
            logical_id.clear();
            source_id.clear();
            content_generation.clear();
            request = {};
            if (read_future.valid())
                read_future.wait();
            read_future = {};
            std::vector<std::byte>{}.swap(decoded_storage);
            decoded_residency = {};
            candidate.Reset();
            decode_reservation = {};
            read_bytes = last_demand_frame = 0;
            upload.Reset();
            upload_completion = {};
            upload_offsets = {};
            metadata = {};
            prior_use = {};
            retirement = {};
            frame_generation = {1, 1, 1};
            upload_residency = {};
            target_index = fallback_index = 0;
            priority = 0;
            generation = 1;
            requests = uploads = publications = reclaims = 0;
            upload_pending = false;
            upload_submitted = false;
            retiring = false;
        }
    };
    [[nodiscard]] Result CreateDevice(const RendererConfig &configuration);
    [[nodiscard]] Result CreateSwapChainAndTargets();
    [[nodiscard]] Result CreateDepthAndShadow();
    [[nodiscard]] Result CreatePostProcessTargets();
    [[nodiscard]] std::array<bool, 3> RequiredLightingAuxiliaries(const RendererConfig &) const noexcept;
    [[nodiscard]] Result PrepareLightingAuxiliaries(std::array<AllocationResource *, 3>,
        ViewExtent, std::array<bool, 3>);
    [[nodiscard]] Result PrepareOptionTargets(OptionTargets &targets,
                                              std::uint32_t target_width,
                                              std::uint32_t target_height,
                                              std::uint32_t shadow_resolution,
                                              bool bloom, std::uint32_t output_width,
                                              std::uint32_t output_height,
                                              bool native_resolve, bool outline,
                                              std::array<bool, 3> lighting_auxiliaries);
    [[nodiscard]] Result PrepareOptionPools(
        std::array<TransientResourcePool, kFrameCount> &pools,
        std::uint32_t target_width, std::uint32_t target_height, bool bloom,
        std::uint32_t output_width, std::uint32_t output_height);
    [[nodiscard]] Result PrepareViewTargets(OptionTargets &, std::uint32_t capacity_width,
        std::uint32_t capacity_height, std::uint32_t output_width, std::uint32_t output_height,
        bool native_resolve);
    [[nodiscard]] Result PrepareExposureResources();
    void WriteViewDescriptors(std::uint32_t frame, std::uint32_t history_read,
        std::uint32_t history_write, std::uint32_t exposure_read, std::uint32_t exposure_write,
        D3D12_GPU_VIRTUAL_ADDRESS exposure_constants) noexcept;
    void CommitOptionTargets(OptionTargets &targets) noexcept;
    [[nodiscard]] Result PrepareLodStateBuffer(GpuLodStateBuffer &buffer, std::size_t capacity);
    void CommitLodStateBuffer(GpuLodStateBuffer &buffer);
    void SwapOptionPools(std::array<TransientResourcePool, kFrameCount> &pools) noexcept;
    void ClearTransientDescriptors() noexcept;
    void WriteOptionTargetDescriptors() noexcept;
    [[nodiscard]] Result CreatePipeline(const RendererConfig &pipeline_config);
    [[nodiscard]] Result SelectTextureFeedbackShaders(const RendererConfig &, RendererConfig &) const;
    void InitializeCanonicalDescriptors(ID3D12DescriptorHeap *) const noexcept;
    void PublishFrameDescriptors(std::uint32_t frame_slot) const noexcept;
    [[nodiscard]] Result CreateClusterPipelines(const RendererConfig &pipeline_config);
    [[nodiscard]] Result PrepareReconstructionProvider(const RendererReconstructionRequest *,
        bool dynamic_resolution, float minimum_percent, float maximum_percent, ViewExtent output,
        ReconstructionPlan &, ViewExtent &capacity, std::unique_ptr<ReconstructionProvider> &candidate,
        const RendererConfig *candidate_config = nullptr);
    [[nodiscard]] Result InitializeReconstructionSessionsBeforeDevice();
    [[nodiscard]] Result BindReconstructionSessionsToDevice();
    [[nodiscard]] Result CommitReconstructionProvider(std::unique_ptr<ReconstructionProvider> &candidate,
        ReconstructionGuidesCandidate &&guides, ViewExtent capacity);
    [[nodiscard]] Result PrepareReconstructionGuides(const RendererConfig &, ViewExtent capacity,
        ViewExtent output, ReconstructionGuidesCandidate &, bool composition = false) const;
    [[nodiscard]] Result PrepareDisplayPipeline(const RendererConfig &, DXGI_FORMAT,
                                                 ComPtr<ID3D12PipelineState> &);
    [[nodiscard]] Result ApplyDisplayOptions(const RendererOptions &);
    [[nodiscard]] Result ResolvePresentation(const RendererConfig &, ViewExtent output, ViewExtent capacity,
        const DisplayState &, std::optional<bool> cadence_suppression = std::nullopt);
    [[nodiscard]] Result CreatePresentationChain(const RendererConfig &, ViewExtent output, DisplayState &);
    [[nodiscard]] Result RecreatePresentation(const RendererConfig &, ViewExtent output, ViewExtent capacity,
        DisplayState &, std::optional<bool> cadence_suppression = std::nullopt);
    [[nodiscard]] Result PreparePresentationGuides(const RendererConfig &, ViewExtent output, ViewExtent capacity,
        const DisplayState &, PresentationGuidesCandidate &) const;
    [[nodiscard]] Result ReloadPipeline(RendererConfig candidate_config);
    [[nodiscard]] Result InitializeResidentAssetContext(const RendererConfig &, std::unique_ptr<Impl> &);
    [[nodiscard]] Result PrepareResidentAssetData(Impl &candidate);
    [[nodiscard]] Result PrepareResidentPublication(Impl &candidate);
    void SwapResidentAssetData(Impl &candidate) noexcept;
    void PublishResidentAssetDescriptors(const Impl &candidate) noexcept;
    [[nodiscard]] Result SubmitResidentUtilityUpload(ID3D12GraphicsCommandList *, ID3D12CommandAllocator *,
                                                    std::vector<AllocationResource> &);
    void CollectResidentUtilityUploads() noexcept;
    [[nodiscard]] bool HasUnresolvedResidentUtilityUploads() const noexcept;
    [[nodiscard]] Result AdmitResidentAllocation(const D3D12_RESOURCE_DESC &, D3D12_HEAP_TYPE);
    void RecordResidentAllocation(const AllocationResource &, D3D12_HEAP_TYPE) noexcept;
    [[nodiscard]] Result PrepareResidentServices(Impl &candidate);
    void SwapResidentServices(Impl &candidate) noexcept;
    void SwapResidentPipelines(Impl &candidate) noexcept;
    [[nodiscard]] Result ReloadResidentGeneration(RendererConfig candidate_config);
    [[nodiscard]] Result CreateGpuData();
    [[nodiscard]] Result CreateCharacterTextures();
    [[nodiscard]] Result UploadVfxNoiseVolume(std::vector<AllocationResource> &uploads);
    [[nodiscard]] Result CreateEnvironmentTextures();
    [[nodiscard]] Result PrepareMaterialParameters(const RendererConfig &candidate,
                                                   bool require_compatible_layout);
    void CommitMaterialParameters() noexcept;
    [[nodiscard]] Result PrepareDecalMaterialParameters(const RendererConfig &, bool require_compatible_layout,
        std::vector<render_data::MaterialParameterSlot> &slots);
    void CommitDecalMaterialParameters() noexcept;
    [[nodiscard]] Result LoadMaterialSurfaceProfile(const RendererConfig &candidate);
    [[nodiscard]] Result PrepareMaterialSidecars(std::array<AllocationResource, 2> &targets,
                                               std::uint32_t width, std::uint32_t height);
    [[nodiscard]] DXGI_FORMAT MaterialSidecarFormat(std::size_t plane) const noexcept;
    void WriteMaterialSidecarDescriptors() noexcept;
    [[nodiscard]] Result CreateEnvironmentBrdfLut(const RendererConfig &lut_config,
                                                AllocationResource &lut);
    void WriteEnvironmentBrdfLutDescriptors();
    [[nodiscard]] Result CreateEnvironmentMeshes();
    [[nodiscard]] Result CreateClusterResources(std::uint32_t generation_tag);
    [[nodiscard]] Result CreateClusterViewResources(ClusterViewRuntime &);
    void ResetClusterViewResources(ClusterViewRuntime &) noexcept;
    void ResetInactiveClusterViewResources() noexcept;
    [[nodiscard]] Result PrepareAmbientOcclusion(const RendererConfig &,
        ViewExtent capacity, AmbientOcclusionCandidate &, bool adaptive_compute = false) const;
    [[nodiscard]] Result PrepareAdaptiveShading(const RendererConfig &,
        ViewExtent capacity, AdaptiveShadingCandidate &) const;
    [[nodiscard]] Result BuildAdaptiveMaterialPolicies(std::vector<AdaptiveShadingMaterialPolicy> &) const;
    [[nodiscard]] Result PrepareSurfaceLighting(const RendererConfig &,
        ViewExtent capacity, SubsurfaceCandidate &, ScreenReflectionsCandidate &) const;
    [[nodiscard]] Result PrepareHeroTransparency(const RendererConfig &,
        ViewExtent capacity, HeroTransparencyCandidate &) const;
    [[nodiscard]] Result PrepareLightingProbe(const RendererConfig &, LightingProbeCandidate &) const;
    [[nodiscard]] Result PrepareLtcLut(const RendererConfig &, LtcLutCandidate &) const;
    [[nodiscard]] Result PrepareHdrPost(const RendererConfig &, ViewExtent render_capacity,
        ViewExtent output_extent, HdrPostCandidate &) const;
    [[nodiscard]] Result PrepareScreenOverlay(const RendererConfig &, ViewExtent,
        ScreenOverlayCandidate &) const;
    [[nodiscard]] Result PrepareSceneGeometry(const RendererConfig &, SceneGeometryCandidate &) const;
    [[nodiscard]] Result PrepareBrixelizerGi(const RendererConfig &, ViewExtent, BrixelizerCandidate &) const;
    [[nodiscard]] Result PrepareVirtualShadows(const RendererConfig &, ViewExtent, VirtualShadowCandidate &) const;
    struct AdditionalShadowPrepared { LocalShadowCandidate local; };
    [[nodiscard]] Result PrepareAdditionalShadows(const RendererConfig &, ViewExtent, AdditionalShadowPrepared &) const;
    [[nodiscard]] Result CommitAdditionalShadows(AdditionalShadowPrepared &&);
    [[nodiscard]] Result PrepareLocalProbes(const RendererConfig &, LocalProbeRuntimeCandidate &) const;
    [[nodiscard]] Result AppendHlodPages(ClusterRuntimeState &);
    [[nodiscard]] Result BeginHlodViewPackage(const RenderFrameInput &);
    [[nodiscard]] Result RetireHlodView(ViewId);
    [[nodiscard]] HlodViewRuntime *ActiveHlodView() noexcept;
    [[nodiscard]] const HlodViewRuntime *ActiveHlodView() const noexcept;
    [[nodiscard]] Result PrepareHlodStaticScene(const std::shared_ptr<const StaticRenderScene> &, std::vector<RenderInstance> &);
    [[nodiscard]] Result PrepareHlodFrame(std::uint32_t, std::vector<GpuStaticDrawRef> &);
    [[nodiscard]] Result AddHlodFramePasses(std::uint32_t, ResourceVersion, ResourceVersion, ResourceVersion, HlodFrameGraph &);
    void ReadHlodFrame(PassBuilder &, const HlodFrameGraph &, bool) const;
    [[nodiscard]] Result DrawHlodGeometry(ID3D12GraphicsCommandList *, std::uint32_t, std::uint32_t);
    [[nodiscard]] Result CommitHlodFrameGraph(std::uint32_t, HlodFrameGraph &, const CompletionSet &);
    [[nodiscard]] Result PumpHlodDemand();
    void ResetHlodResources() noexcept;
    struct VolumetricPrepared
    {
        VolumetricCandidate service;
        std::vector<VolumetricVolume> media;
        std::uint64_t revision{};
    };
    [[nodiscard]] Result PrepareVolumetrics(const RendererConfig &, ViewExtent, VolumetricPrepared &) const;
    [[nodiscard]] Result CommitVolumetrics(VolumetricPrepared &&);
    [[nodiscard]] RendererFeaturePlan ResolveFeatures(const RendererConfig &) const noexcept;
    [[nodiscard]] Result PrepareLightList(const RendererConfig &, ViewExtent, LightListCandidate &) const;
    [[nodiscard]] Result PrepareHeroBindings(ID3D12GraphicsCommandList *,
        std::vector<AllocationResource> &uploads);
    [[nodiscard]] Result AddClusterFramePasses(std::uint32_t frame_slot,
        ResourceVersion &geometry_records, ResourceVersion *raster_records,
        ResourceVersion static_scene, ClusterFrameGraph &state);
    [[nodiscard]] Result AddClusterOcclusionPass(std::uint32_t frame_slot,
        ClusterFrameGraph &, ResourceVersion hzb, bool retest);
    void ReadClusterFrame(PassBuilder &, const ClusterFrameGraph &, bool draw, bool retest = false) const;
    [[nodiscard]] Result DrawClusterGeometry(ID3D12GraphicsCommandList *,
        std::uint32_t frame_slot, std::uint32_t view, bool retest = false);
    [[nodiscard]] Result CommitClusterFrameGraph(std::uint32_t frame_slot,
        ClusterFrameGraph &, const CompletionSet &);
    [[nodiscard]] Result PrepareClusterFrame(std::uint32_t frame_slot,
        const std::array<ClusterCutInput, kEnvironmentMaterialNames.size()> &inputs);
    [[nodiscard]] Result CaptureClusterFrame(std::uint32_t frame_slot);
    [[nodiscard]] Result DemandClusterPage(std::uint32_t logical_slot,
        std::uint32_t generation_tag, std::uint32_t priority);
    [[nodiscard]] Result PumpClusterStreaming(std::uint32_t frame_slot);
    [[nodiscard]] Result PrepareClusterPageUpload(std::uint32_t logical_slot,
        std::uint32_t physical_slot);
    [[nodiscard]] Result SubmitClusterPageUpload(std::uint32_t logical_slot,
        const CompletionSet &completion);
    [[nodiscard]] Result SubmitClusterCutReadback(std::uint32_t frame_slot,
        std::uint32_t ordinal, const CompletionSet &completion);
    void RecordClusterPageUpload(ID3D12GraphicsCommandList *, std::uint32_t logical_slot) const;
    void RecordClusterPageTableUpload(ID3D12GraphicsCommandList *, std::uint32_t frame_slot) const;
    void WriteClusterCutDescriptors(std::uint32_t frame_slot, std::uint32_t ordinal);
    void MarkClusterFrameUse(std::uint32_t frame_slot, const CompletionSet &) noexcept;
    [[nodiscard]] const GeometryPageGpu *FindClusterGeometryPage(std::uint32_t physical_slot) const noexcept;
    [[nodiscard]] bool ClusterHierarchyReady(std::uint32_t ordinal) const noexcept;
    void ResetClusterResources() noexcept;
    [[nodiscard]] Result CreateGeometryPage(
        GeometryPageGpu &page, const render_data::GeometryMetadata &metadata,
        std::span<const std::byte> package_bytes, AssetId page_asset,
        std::uint32_t page_slot, ID3D12GraphicsCommandList *list,
        std::vector<AllocationResource> &uploads);
    [[nodiscard]] Result CreateProceduralGeometry(
        ID3D12GraphicsCommandList *list, std::vector<AllocationResource> &uploads);
    [[nodiscard]] Result CreateVisibilityTargets();
    [[nodiscard]] Result PrepareVisibilityTargets(OptionTargets &targets,
        std::uint32_t target_width, std::uint32_t target_height);
    [[nodiscard]] Result WriteVisibilityDescriptors(std::uint32_t frame_slot,
        ID3D12Resource *static_scene, ID3D12Resource *dynamic_scene,
        std::uint64_t static_ref_offset, std::uint32_t static_ref_count,
        std::uint64_t dynamic_ref_offset, std::uint32_t dynamic_ref_count);
    [[nodiscard]] Result PrepareGeometryPageUpload(
        GeometryPageGpu &page, const render_data::GeometryMetadata &metadata,
        std::span<const std::byte> package_bytes, AssetId page_asset,
        std::uint32_t page_slot, AllocationResource &upload,
        std::array<std::uint64_t, kGeometryBuffersPerPage> &upload_offsets);
    void RecordGeometryPageUpload(ID3D12GraphicsCommandList *list,
        const GeometryPageGpu &page, const AllocationResource &upload,
        const std::array<std::uint64_t, kGeometryBuffersPerPage> &upload_offsets) const;
    [[nodiscard]] const GeometryPageGpu *FindGeometryPage(std::uint32_t page_slot) const noexcept;
    void WriteGeometryPageDescriptors(std::uint32_t frame_slot);
    [[nodiscard]] Result BeginGeometryDrawRecords(std::uint32_t frame_slot);
    [[nodiscard]] Result AddGeometryDrawRecord(std::uint32_t frame_slot,
        const GeometryPageGpu &page, std::uint32_t visible_base,
        std::uint32_t instance_count, std::uint32_t flags, std::uint32_t view,
        std::uint32_t mesh_id, std::uint32_t &packed_pass);
    [[nodiscard]] Result UploadGeometryDrawRecords(ID3D12GraphicsCommandList *list,
        std::uint32_t frame_slot);
    [[nodiscard]] bool GeometryUsesMesh(ID3D12PipelineState *indexed_pipeline,
        const GeometryPageGpu &page, std::uint32_t instance_capacity) const noexcept;
    [[nodiscard]] Result DrawGeometry(ID3D12GraphicsCommandList *list,
        std::uint32_t frame_slot, ID3D12PipelineState *indexed_pipeline,
        const GeometryPageGpu &page, std::uint32_t packed_pass,
        ID3D12Resource *arguments = nullptr, std::uint64_t argument_offset = 0);
    [[nodiscard]] Result CreateUiTexture();
    [[nodiscard]] Result PrepareUiResources(UiResources &, std::uint32_t output_width,
                                            std::uint32_t output_height);
    void CommitUiResources(UiResources &) noexcept;
    [[nodiscard]] Result RasterizeUi(std::uint32_t frame_index,
                                     std::span<const UiModel> models);
    [[nodiscard]] Result CreateAllocation(AllocationResource &output,
                                          const D3D12MA::ALLOCATION_DESC &allocation,
                                          const D3D12_RESOURCE_DESC &resource,
                                          D3D12_RESOURCE_STATES initial_state,
                                          const D3D12_CLEAR_VALUE *clear = nullptr);
    [[nodiscard]] Result UpdateResidencyBudget();
    [[nodiscard]] Result TryRelieveStreamingPressure(std::uint64_t bytes,
        std::uint32_t priority, bool need_cluster_slot = false,
        bool for_hlod_admission = false);
    [[nodiscard]] Result PrepareEnvironmentStreaming();
    [[nodiscard]] Result PrepareHardwareTextureStreaming(const RendererAssetIo *live_transport = nullptr);
    [[nodiscard]] Result DemandEnvironmentTexture(std::uint32_t priority);
    [[nodiscard]] Result PumpEnvironmentStreaming(std::uint32_t frame_slot);
    [[nodiscard]] Result PrepareEnvironmentGeometryStreaming();
    [[nodiscard]] Result DemandEnvironmentGeometry(std::uint32_t ordinal,
                                                   std::uint32_t priority);
    [[nodiscard]] Result PumpEnvironmentGeometryStreaming();
    void MarkEnvironmentStreamingUse(std::uint32_t frame_slot,
                                     const CompletionSet &completion) noexcept;
    void ResetEnvironmentStreaming() noexcept;
    void WriteEnvironmentTextureDescriptor(std::uint32_t frame_slot,
                                           std::uint32_t texture_index,
                                           ID3D12Resource *resource,
                                           DXGI_FORMAT format,
                                           std::uint32_t mip_count,
                                           std::uint32_t array_size) const noexcept;
    [[nodiscard]] Result LoadEnvironmentMaterialTexture(
        const render_data::MaterialTextureDefinition &binding, std::uint32_t required_layer,
        std::vector<AllocationResource> &uploads, std::uint32_t &texture_index);
    void WriteEnvironmentMaterialTextureDescriptor(
        std::uint32_t frame_slot, std::uint32_t material, std::uint32_t slot,
        ID3D12Resource *resource) const noexcept;
    void PublishEnvironmentMaterialTextures(std::uint32_t frame_slot) const noexcept;
    [[nodiscard]] Result WaitForFrame(FrameContext &frame);
    [[nodiscard]] Result PrepareFrameTimestamps(FrameContext &frame, std::uint32_t pass_count);
    [[nodiscard]] GpuPassTimings ReadFrameGpuTimings(const FrameContext &frame) const;
    [[nodiscard]] Result WaitForGpu();
    [[nodiscard]] Result CheckDevice(HRESULT result, std::string_view operation);
    void TransitionTexture(ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                           D3D12_RESOURCE_STATES after, D3D12_BARRIER_LAYOUT before_layout,
                           D3D12_BARRIER_LAYOUT after_layout);
    void TransitionBuffer(ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                          D3D12_RESOURCE_STATES after, D3D12_BARRIER_SYNC before_sync,
                          D3D12_BARRIER_SYNC after_sync, D3D12_BARRIER_ACCESS before_access,
                          D3D12_BARRIER_ACCESS after_access);
    void CountValidationErrors();
    void WriteDred(HRESULT reason) const;
    [[nodiscard]] Result WritePng(const std::filesystem::path &path, const std::byte *pixels,
                                  std::uint32_t row_pitch) const;

    RendererConfig config;
    RendererFeatureCapabilities feature_capabilities;
    std::string resource_generation_id;
    std::shared_ptr<const void> resident_generation_lease;
    ComPtr<IDXGIFactory7> factory;
    ComPtr<IDXGIAdapter4> adapter;
    ComPtr<ID3D12Device> device;
    mutable RendererPipelineCache pipeline_cache;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain4> swap_chain;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    ComPtr<ID3D12DescriptorHeap> dsv_heap;
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    // Original/copy-source descriptors live here. srv_heap is GPU destination
    // only; publish a complete waited frame bank once before graph recording.
    ComPtr<ID3D12DescriptorHeap> srv_cpu_heap;
    ComPtr<ID3D12DescriptorHeap> sampler_heap;
    std::uint32_t sampler_stride{};
#if defined(HS_DEVELOPMENT_TOOLS)
    ComPtr<ID3D12DescriptorHeap> imgui_heap;
#endif
    ComPtr<ID3D12GraphicsCommandList> command_list;
    ComPtr<ID3D12GraphicsCommandList7> enhanced_command_list;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event{};
    std::uint64_t next_fence{1};
    D3D12MA::Allocator *allocator{};
    ComPtr<D3D12MA::Allocator> resident_allocator_lease;
    std::shared_ptr<void> resident_fence_event_owner;
    std::vector<ResidentUtilitySubmission> resident_utility_submissions;
    ResidentPreparationAccounting resident_preparation_accounting;
    std::vector<std::unique_ptr<Impl>> unresolved_resident_candidates;
    std::array<FrameContext, kFrameCount> frames;
    std::array<VisibilityFrameBuffers, kFrameCount> visibility_frames;
    ResidencyManager residency;
    StreamingManager streaming;
    RendererAssetIo asset_io;
    RendererTextureStreaming texture_streaming;
    RendererTextureFeedback texture_feedback;
    std::vector<AdaptiveShadingMaterialPolicy> adaptive_material_policies;
    TextureStreamingCapabilities texture_streaming_capabilities;
    std::array<TextureStreamId, kEnvironmentTextureCount> environment_texture_stream_ids;
    std::vector<TextureStreamId> environment_material_texture_stream_ids;
    std::array<TextureStreamId, kTextureFeedbackBindingCount> material_texture_stream_ids;
    EnvironmentTextureStream environment_stream;
    std::array<EnvironmentGeometryStream, kEnvironmentMaterialNames.size()>
        environment_geometry_streams;
    std::vector<RetiredStreamingTexture> retired_streaming_textures;
    std::shared_ptr<StaticGpuScene> static_gpu_scene;
    std::uint32_t next_static_instance_generation{1};
    std::vector<std::shared_ptr<StaticGpuScene>> retired_static_gpu_scenes;
    bool static_scene_fence_unreliable{};
    std::uint64_t static_scene_upload_count{};
    std::array<float, kEnvironmentMaterialNames.size()> environment_variant_radii{};
    AllocationResource dynamic_instances;
    RenderSceneExtractor dynamic_scene_extractor;
    RenderInstanceLookup previous_instance_lookup;
    std::uint64_t dynamic_scene_generation{};
    PersistentGpuSceneSlots dynamic_scene_slots;
    SkinCache skin_cache;
    std::vector<RetiredDynamicInstanceBuffer> retired_dynamic_instance_buffers;
    std::size_t dynamic_instance_capacity{};
    std::size_t dynamic_instance_slot_count{};
    bool dynamic_instances_shader_read{};
    std::ofstream dynamic_instance_upload_log;
    std::array<ComPtr<ID3D12Resource>, kFrameCount> back_buffers;
    std::array<CompletionSet, kFrameCount> back_buffer_completion{};
    AllocationResource depth;
    AllocationResource shadow;
    GeometryPageGpu archer_geometry;
    GeometryPageGpu gel_projectile_geometry;
    GeometryPageGpu procedural_geometry;
    AllocationResource visibility_ids, visibility_front_face;
    bool visibility_targets_initialized{};
    AllocationResource visibility_pixels, visibility_counts, visibility_dispatch_args;
    std::array<MonsterAsset, 6> monster_assets;
    AllocationResource monster_skin_matrices;
    AllocationResource archer_diffuse;
    AllocationResource archer_normal;
    AllocationResource monster_basecolor;
    AllocationResource monster_emissive;
    AllocationResource monster_ram;
    AllocationResource family_diffuse;
    AllocationResource family_normal;
    AllocationResource vfx_masks;
    std::uint32_t vfx_sprite_count{};
    AllocationResource vfx_mesh_atlas;
    std::array<std::uint32_t, 7> vfx_mesh_vertex_counts{};
    std::array<float, 7> vfx_mesh_transverse_radii{};
    std::array<DirectX::XMFLOAT3, 7> vfx_mesh_longitudinal_bounds{}; // min, widest ring, max Z.
    AllocationResource vfx_gradient;
    std::uint32_t vfx_gradient_rows{};
    AllocationResource ribbon_history;
    AllocationResource ribbon_previous;
    AllocationResource ribbon_arguments;
    AllocationResource ribbon_detail;
    AllocationResource vfx_authored_mask;
    AllocationResource vfx_noise_basis;
    AllocationResource vfx_noise_volume;
    AllocationResource vfx_smoke_pos, vfx_smoke_neg, vfx_smoke_motion;
    AllocationResource vfx_flow_curl, vfx_stbn_scalar;
    AllocationResource vfx_fracture_decals, vfx_organic_decals;
    AllocationResource vfx_curve_lut;
    Float3 previous_ribbon_eye{};
    VfxRibbonState ribbon_state;
    AllocationResource particles;
    std::array<AllocationResource, 2> particle_alive;
    AllocationResource particle_dead;
    AllocationResource particle_counters;
    AllocationResource indirect_arguments;
    AllocationResource gbuffer_base;
    AllocationResource gbuffer_normal;
    AllocationResource gbuffer_metadata;
    AllocationResource gbuffer_material;
    AllocationResource gbuffer_motion;
    AllocationResource gbuffer_surface_history;
    std::array<AllocationResource, 2> material_sidecars;
    std::array<render_data::MaterialSurfaceLayout, kResidentMaterialCount> material_surface_layouts;
    render_data::MaterialSidecarLayout material_sidecar_layout;
    bool material_sidecars_initialized{};
    struct MaterialHeader
    {
        std::vector<std::byte> bytes;
        render_data::PackageView view;
    };
    std::array<MaterialHeader, kResidentMaterialCount> material_headers;
    std::array<AllocationResource, kEnvironmentTextureCount> environment_textures;
    AllocationResource environment_brdf_lut;
    AllocationResource material_parameters;
    AllocationResource material_parameter_candidate;
    AllocationResource material_parameter_upload;
    ComPtr<ID3D12Device5> material_parameter_upload_device;
    bool material_parameter_upload_pending{};
    std::uint32_t material_parameter_slot_count{};
    std::uint64_t material_parameter_revision{};
    std::uint64_t pipeline_revision{};
    std::uint32_t material_parameter_candidate_slot_count{};
    std::array<std::uint32_t, kResidentMaterialCount> material_parameter_bases{};
    std::array<std::uint32_t, kResidentMaterialCount> material_parameter_candidate_bases{};
    std::array<std::string, kResidentMaterialCount> material_parameter_layouts{};
    std::array<std::string, kResidentMaterialCount> material_parameter_candidate_layouts{};
    std::vector<render_data::MaterialParameterSlot> material_parameter_slots, material_parameter_candidate_slots;
    std::array<render_data::DecalMaterialPackageData, 2> decal_material_programs{}, decal_material_candidate_programs{};
    std::array<render_data::DecalReceiverPolicy, 2> decal_material_policies{}, decal_material_candidate_policies{};
    std::array<std::uint32_t, 2> decal_parameter_bases{}, decal_parameter_candidate_bases{},
        decal_parameter_counts{}, decal_parameter_candidate_counts{};
    std::array<std::string, 2> decal_parameter_layouts{}, decal_parameter_candidate_layouts{};
    AllocationResource hero_material_bindings_gpu;
    std::array<DirectX::XMUINT4, 7> hero_binding_ranges{};
    ComPtr<ID3D12PipelineState> hero_scene_pipeline, hero_visibility_pipeline, hero_shadow_pipeline;
    ComPtr<ID3D12PipelineState> hero_resolve_pipeline, hero_translucent_pipeline;
    AllocationResource environment_brdf_candidate;
    ComPtr<ID3D12RootSignature> environment_brdf_init_root;
    ComPtr<ID3D12PipelineState> environment_brdf_init_pipeline;
    ComPtr<ID3D12DescriptorHeap> environment_brdf_init_heap;
    ComPtr<ID3D12Device5> environment_brdf_init_device;
    bool environment_brdf_init_pending{};
    std::array<std::string, kEnvironmentTextureCount> environment_texture_ids;
    std::array<render_data::TextureDescription, kEnvironmentTextureCount>
        environment_texture_descriptions;
    struct EnvironmentMaterialTextureGpu
    {
        std::string product_id;
        render_data::TextureDescription description;
        AllocationResource texture;
    };
    std::vector<EnvironmentMaterialTextureGpu> environment_material_textures;
    struct EnvironmentMaterialBindingsGpu
    {
        std::array<std::string, render_data::kMaterialTextureSlots> product_ids;
        std::array<std::uint32_t, render_data::kMaterialTextureSlots> texture_indices{
            UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    };
    std::array<EnvironmentMaterialBindingsGpu, kAuthoredMaterialNames.size()>
        environment_material_bindings;
    render_data::RockMaterial terrain_material;
    struct EnvironmentMeshGpu : GeometryPageGpu
    {
        float bounding_radius{};
        render_data::RockMaterial material;
        std::string program_shader;
        std::string program_shadow_shader;
    };
    std::array<EnvironmentMeshGpu, kEnvironmentMaterialNames.size() * render_data::kEnvironmentGeometryLodCount> environment_meshes;
    std::array<EnvironmentClusterCatalog, kEnvironmentMaterialNames.size()> environment_cluster_catalogs;
    std::unique_ptr<ClusterRuntimeState> cluster_runtime;
    AmbientOcclusionSystem ambient_occlusion;
    AdaptiveShadingSystem adaptive_shading;
    SubsurfaceSystem subsurface;
    LightingProbeSystem lighting_probe;
    LtcLutSystem ltc_lut;
    HdrPostSystem hdr_post;
    ScreenOverlaySystem screen_overlay;
    SceneGeometrySystem scene_geometry;
    BrixelizerGiSystem brixelizer_gi;
    bool brixelizer_history_valid{};
    VirtualShadowSystem virtual_shadows;
    LocalShadowSystem local_shadows;
    LocalProbeRuntime local_probes;
    std::unique_ptr<HlodRuntime> hlod_runtime;
    VolumetricSystem volumetrics;
    std::vector<VolumetricVolume> volumetric_media;
    std::uint64_t volumetric_revision{};
    VolumetricCamera previous_volumetric_camera;
    ShadowCasterMap previous_shadow_casters;
    ShadowCasterNodes previous_shadow_caster_nodes;
    LightListSystem light_list;
    DecalSurfaceSystem decal_surface;
    ScreenReflectionsSystem screen_reflections;
    HeroTransparencySystem hero_transparency;
    ReconstructionPlan reconstruction_plan;
    std::unique_ptr<StreamlineSession> streamline_session;
    // Discovery copies CPU/device query results; it never owns evaluation history.
    GraphicsCapabilitiesView graphics_capabilities;
    std::string graphics_capabilities_key;
    std::unique_ptr<FsrSession> fsr_session;
    std::unique_ptr<ReconstructionProvider> reconstruction_provider;
    SdkCamera sdk_camera;
    SdkFrameIdentity sdk_frame_identity;
    SdkPresentation sdk_presentation;
    PresentationGuides presentation_guides;
    ComPtr<ID3D12Device> streamline_device_proxy;
    ComPtr<ID3D12CommandQueue> streamline_queue_proxy;
    ComPtr<IDXGIFactory2> streamline_factory_proxy;
    std::uint8_t native_input_phase{};
    ReconstructionGuidesSystem reconstruction_guides;
    AllocationResource hdr_color;
    AllocationResource subsurface_diffuse, environment_specular, indirect_diffuse;
    AllocationResource oit_accumulation;
    AllocationResource oit_revealage;
    AllocationResource post_a;
    AllocationResource hdr_capture_source;
    ViewExtent hdr_capture_extent;
    bool hdr_capture_valid{}, hdr_capture_pre_exposed{};
    std::uint64_t hdr_capture_submission_id{}, hdr_capture_package_id{}, hdr_capture_snapshot_tick{};
    std::string hdr_capture_source_name{"reconstructed_hdr_before_post"};
    AllocationResource post_b;
    std::array<AllocationResource, 2> temporal_history;
    std::array<AllocationResource, 2> temporal_normal_history, temporal_material_history;
    AllocationResource scene_work, display_linear, composite_hdr;
    std::array<AllocationResource, 2> exposure_values;
    AllocationResource exposure_histogram, initial_exposure;
    std::uint32_t exposure_read_index{};
    bool exposure_initialized{};
    bool view_targets_initialized{};
    std::chrono::steady_clock::time_point exposure_sample_time{};
    std::chrono::steady_clock::time_point render_submission_time{};
    std::map<std::uint32_t, std::unique_ptr<OwnedView>> owned_views;
    std::vector<std::unique_ptr<OwnedView>> unresolved_view_candidates;
    ViewId active_view_id{kMainRenderViewId};
    bool auto_fg_cadence_suppressed{};
    std::uint64_t auto_fg_cadence_sample_id{};
    RenderViewRole active_view_role{RenderViewRole::Main};
    const RenderViewInput *explicit_view{};
    const RenderFrameInput *explicit_frame{};
    bool legacy_frame_bridge{}, render_scene_inputs{true};
    std::uint32_t active_native_viewport_id{}, next_native_viewport_id{1};
    RenderWorldPosition previous_scene_origin{}, previous_view_origin{};
    bool previous_origins_valid{};
    std::uint64_t view_configuration_revision{1}, rendered_submission_count{}, render_package_id{}, active_render_package_id{};
    std::uint64_t view_session_id{};
    std::chrono::steady_clock::time_point render_package_time;
    std::shared_ptr<CanonicalSkinFrame> previous_skin_deformation, canonical_skin_current;
    std::shared_ptr<const SubmittedInstanceFrame> previous_instances, canonical_instance_current;
    std::array<DirectX::XMFLOAT4, 32> previous_grass_benders{};
    DirectX::XMFLOAT4 previous_grass_state{};
    std::vector<std::shared_ptr<CanonicalSkinFrame>> canonical_skin_frames;
    GraphicsSettings applied_graphics;
    std::shared_ptr<const render_data::QualityProfiles> quality_profiles;
    [[nodiscard]] Result LoadQualityProfiles(const RendererConfig &);
    [[nodiscard]] const render_data::QualityProfile &QualityProfileFor(GraphicsQuality quality) const
    { return quality_profiles->profiles.at(static_cast<std::size_t>(quality)); }
    bool applied_graphics_available{};
    // Canonical request that last completed the native transaction.  The
    // detailed actual snapshot is derived on status query so allocation there
    // cannot turn a committed transaction into a rejection.
    GraphicsSettings last_applied_graphics_request;
    bool last_applied_graphics_request_available{};
    std::uint64_t graphics_settings_generation_high_watermark{};
    SettingsApplyResult graphics_settings_status;
    RendererDiagnosticsRecorder diagnostics;
    ViewState view_state;
    DisplayState display_state;
    DynamicResolutionController dynamic_resolution_controller;
    GpuPassTimings completed_gpu_timings;
    // Capture-only pending completion snapshots. Both frame count and pass count are bounded.
    mutable std::vector<GpuPassTimings> pending_gpu_timings;
    mutable std::size_t pending_gpu_timing_passes{};
    mutable bool gpu_timing_stream_overflowed{};
    std::uint64_t dynamic_resolution_sample_id{};
    std::array<AllocationResource, 2> hzb_history;
    HzbLayout hzb_layout{};
    std::uint32_t hzb_read_index{};
    bool hzb_history_valid{};
    bool temporal_history_valid{};
    bool temporal_targets_initialized{}; // Allocation initialization, independent of history validity.
    std::uint32_t temporal_read_index{};
    DirectX::XMFLOAT4X4 previous_view_projection{};
    DirectX::XMFLOAT4X4 previous_unjittered_view_projection{};
    DirectX::XMFLOAT4 previous_projection_depth{};
    DirectX::XMFLOAT2 previous_jitter_uv{};
    Float3 temporal_previous_eye{};
    float temporal_previous_fov{};
    std::array<AllocationResource, kFrameCount> ui_textures;
    std::array<UiCpuSurface, kFrameCount> ui_surfaces;
    std::array<std::vector<UiModel>, kFrameCount> ui_raster_models;
    std::array<bool, kFrameCount> ui_dirty{};
    ComPtr<IWICImagingFactory> ui_wic_factory;
    ComPtr<ID2D1Factory> ui_d2d_factory;
    ComPtr<IDWriteFactory5> ui_dwrite_factory;
    ComPtr<IDWriteFontCollection1> ui_font_collection;
    std::wstring ui_font_family;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT ui_footprint{};
    std::uint32_t ui_raster_width{}, ui_raster_height{};
    std::uint64_t ui_row_bytes{}, ui_upload_bytes{};
    std::uint32_t ui_rows{};
    ComPtr<ID3D12RootSignature> root_signature;
    ComPtr<ID3D12RootSignature> cluster_root_signature;
    ComPtr<ID3D12PipelineState> cluster_count_pipeline, cluster_prefix_pipeline, cluster_emit_pipeline;
    ComPtr<ID3D12PipelineState> cluster_occlusion_pipeline, cluster_retest_pipeline;
    std::array<ComPtr<ID3D12PipelineState>, kAuthoredMaterialNames.size()> cluster_scene_pipelines,
        cluster_shadow_pipelines, cluster_visibility_pipelines;
    ComPtr<ID3D12PipelineState> scene_pipeline;
    ComPtr<ID3D12PipelineState> shadow_pipeline;
    ComPtr<ID3D12PipelineState> environment_opaque_shadow_pipeline;
    std::array<ComPtr<ID3D12PipelineState>, kAuthoredMaterialNames.size()>
        environment_material_scene_pipelines;
    std::array<ComPtr<ID3D12PipelineState>, kAuthoredMaterialNames.size()>
        environment_material_shadow_pipelines;
    ComPtr<ID3D12PipelineState> particle_pipeline;
    ComPtr<ID3D12PipelineState> ground_ring_pipeline;
    ComPtr<ID3D12PipelineState> ground_add_pipeline;
    ComPtr<ID3D12PipelineState> owner_mesh_pipeline;
    ComPtr<ID3D12PipelineState> vfx_flash_pipeline;
    ComPtr<ID3D12PipelineState> vfx_distortion_pipeline;
    ComPtr<ID3D12PipelineState> vfx_sprite_oit_pipeline;
    ComPtr<ID3D12PipelineState> slime_pipeline;
    ComPtr<ID3D12PipelineState> fresnel_shell_pipeline;
    ComPtr<ID3D12PipelineState> particle_compute_pipeline;
    ComPtr<ID3D12PipelineState> static_visibility_pipeline;
    ComPtr<ID3D12PipelineState> visibility_retest_pipeline;
    ComPtr<ID3D12PipelineState> depth_pyramid_init_pipeline;
    ComPtr<ID3D12PipelineState> depth_pyramid_reduce_pipeline;
    ComPtr<ID3D12PipelineState> skin_cache_pipeline;
    ComPtr<ID3D12PipelineState> skin_bounds_pipeline;
    ComPtr<ID3D12PipelineState> ribbon_update_pipeline;
    ComPtr<ID3D12PipelineState> ribbon_args_pipeline;
    ComPtr<ID3D12PipelineState> ribbon_add_pipeline;
    ComPtr<ID3D12PipelineState> ribbon_oit_pipeline;
    // Auxiliary bits: SSS=1, environment specular=2, indirect diffuse=4; valid masks 0,1,2,3,7.
    std::array<ComPtr<ID3D12PipelineState>, 8> deferred_pipeline;
    ComPtr<ID3D12PipelineState> composite_pipeline;
    ComPtr<ID3D12PipelineState> temporal_pipeline;
    ComPtr<ID3D12PipelineState> pre_exposure_pipeline;
    ComPtr<ID3D12PipelineState> exposure_clear_pipeline, exposure_build_pipeline, exposure_reduce_pipeline;
    ComPtr<ID3D12PipelineState> bloom_extract_pipeline;
    ComPtr<ID3D12PipelineState> bloom_downsample_pipeline;
    ComPtr<ID3D12PipelineState> bloom_upsample_pipeline;
    ComPtr<ID3D12PipelineState> bloom_pipeline;
    ComPtr<ID3D12PipelineState> tone_map_pipeline;
    ComPtr<ID3D12PipelineState> outline_pipeline;
    ComPtr<ID3D12PipelineState> fxaa_pipeline;
    ComPtr<ID3D12PipelineState> ui_pipeline;
    ComPtr<ID3D12CommandSignature> draw_signature;
    ComPtr<ID3D12CommandSignature> indexed_geometry_signature;
    std::unordered_map<ID3D12PipelineState *, ComPtr<ID3D12PipelineState>> virtual_shadow_indexed_pipelines;
    std::unordered_map<ID3D12PipelineState *, ComPtr<ID3D12PipelineState>> virtual_shadow_mesh_pipelines;
    ComPtr<ID3D12CommandSignature> cluster_draw_signature, cluster_mesh_signature;
    ComPtr<ID3D12CommandSignature> mesh_geometry_signature;
    struct MeshPipelinePair
    {
        ID3D12PipelineState *indexed{}; // Owned by the corresponding existing PSO field.
        ComPtr<ID3D12PipelineState> mesh;
    };
    std::vector<MeshPipelinePair> mesh_pipeline_pairs;
    ComPtr<ID3D12CommandSignature> ribbon_draw_signature;
    ComPtr<ID3D12PipelineState> visibility_pipeline, surface_resolve_pipeline;
    std::array<ComPtr<ID3D12PipelineState>, kAuthoredMaterialNames.size()> environment_material_visibility_pipelines;
    std::array<ComPtr<ID3D12PipelineState>, kAuthoredMaterialNames.size()> environment_material_resolve_pipelines;
    ComPtr<ID3D12PipelineState> raster_records_pipeline, visibility_clear_pipeline;
    ComPtr<ID3D12PipelineState> visibility_count_pipeline, visibility_prefix_pipeline;
    ComPtr<ID3D12PipelineState> visibility_scatter_pipeline;
    ComPtr<ID3D12CommandSignature> dispatch_signature;
    std::vector<CharacterClipHeader> archer_clips;
    render_data::CharacterAnimationExtensionData archer_named_animations;
    std::vector<std::uint16_t> archer_parents;
    std::vector<std::array<float, 16>> archer_inverse_bind_matrices;
    std::vector<CharacterLocalTransform> archer_transforms;
    std::vector<float> archer_upper_body_weights;
    std::vector<SkinnedVertex> archer_support_vertices;
    std::uint32_t archer_bone_count{};
    std::uint32_t archer_material_count{};
    std::vector<render_data::CharacterMaterialBinding> archer_material_bindings;
    float archer_ground_offset{};
    RenderGraphBuilder graph;
    RenderGraphQueueExecutor graph_queue_executor;
    std::array<TransientResourcePool,kFrameCount> transient_pools;
    UINT rtv_stride{};
    UINT dsv_stride{};
    UINT srv_stride{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t render_width{};
    std::uint32_t render_height{};
    std::uint32_t last_presented_index{};
    std::uint64_t frame_number{};
    GpuLodStateBuffer dynamic_lod_states;
    std::uint32_t lod_session_epoch{};
    bool frame_submission_faulted{};
    std::array<std::uint32_t, kGeometryPageCapacity> previous_geometry_page_versions{};
    std::uint64_t validation_errors{};
    std::uint32_t last_particle_count{};
    Tick observed_snapshot_tick{};
    std::chrono::steady_clock::time_point snapshot_arrival{};
    bool enhanced{};
    bool mesh_shader_supported{};
    bool mesh_shader_enabled{};
    bool dred_enabled{};
    bool transient_textures_common{};
    bool particles_initialized{};
    bool particle_input_is_a{true};
    bool initialized{};
    bool com_initialized{};
#if defined(HS_DEVELOPMENT_TOOLS)
    bool imgui_initialized{};
    bool preview_pose_override{true};
    int preview_base_clip{};
    int preview_secondary_clip{1};
    int preview_upper_clip{3};
    float preview_base_time{0.37f};
    float preview_secondary_time{0.61f};
    float preview_upper_time{0.5f};
    float preview_secondary_weight{0.5f};
    float preview_upper_weight{0.5f};
    float preview_distance{4.0f};
    float preview_yaw{180.0f};
    float preview_pitch{10.0f};
#endif
    renderer_detail::VfxPersistentState typed_vfx_persistent_state;
    Tick last_particle_tick{};
    std::uint64_t render_session_id{};
};

} // namespace hs
