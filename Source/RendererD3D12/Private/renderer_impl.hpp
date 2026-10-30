#pragma once

#include <hs/core/dds_format.hpp>
#include <hs/core/process_info.hpp>
#include <hs/render_data/character_package.hpp>
#include <hs/render_data/character_animation_extension.hpp>
#include <hs/render_data/character_descriptor.hpp>
#include <hs/render_data/geometry_package.hpp>
#include <hs/render_data/material_schema.hpp>
#include <hs/render_data/material_sidecar.hpp>
#include <hs/render_data/surface_layout.hpp>
#include <hs/render_data/texture_package.hpp>
#include <hs/render_data/quality_profiles.hpp>
#include <hs/renderer/renderer.hpp>
#include <hs/render_data/render_frame_input.hpp>
#include <hs/render_data/ui_draw_content.hpp>

#include "renderer_options.hpp"
#include "renderer_caster_storage.hpp"
#include "renderer_geometry.hpp"
#include "renderer_geometry_demand.hpp"
#include "renderer_skin_ranges.hpp"
#include "render_graph.hpp"
#include "render_graph_membership.hpp"

#include "d3d12_resources.hpp"
#include "persistent_gpu_scene.hpp"
#include "renderer_entity_surface.hpp"
#include "renderer_instance_order.hpp"
#include "renderer_monster_lod.hpp"
#include "render_instance_lookup.hpp"
#include "renderer_diagnostics.hpp"
#include "renderer_lod.hpp"
#include "renderer_visibility.hpp"
#include "residency_manager.hpp"
#include "skin_cache.hpp"
#include "streaming_manager.hpp"
#include "renderer_asset_io.hpp"
#include "renderer_content.hpp"
#include "renderer_startup.hpp"
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
#include "renderer_post_subgraphs.hpp"
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
#include <chrono>

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
constexpr std::array<DXGI_FORMAT, kPostTextureDescriptorCount> kPostTextureSrvFormats{
    DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16_SNORM, DXGI_FORMAT_R32G32_UINT,
    DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
    DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R16_FLOAT,
    DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
    DXGI_FORMAT_B8G8R8A8_UNORM};
constexpr std::array<D3D12_SRV_DIMENSION, kCharacterDescriptorCount> kCharacterSrvDimensions{
    D3D12_SRV_DIMENSION_TEXTURE2DARRAY, D3D12_SRV_DIMENSION_TEXTURE2DARRAY,
    D3D12_SRV_DIMENSION_TEXTURE2DARRAY};
constexpr std::array<DXGI_FORMAT, kCharacterDescriptorCount> kCharacterSrvFormats{
    DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8_UNORM};
constexpr std::array<D3D12_SRV_DIMENSION, kMonsterPbrDescriptorCount> kMonsterPbrSrvDimensions{
    D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_SRV_DIMENSION_TEXTURE2D,
    D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_SRV_DIMENSION_TEXTURE2DARRAY,
    D3D12_SRV_DIMENSION_TEXTURE2DARRAY};
constexpr std::array<DXGI_FORMAT, kMonsterPbrDescriptorCount> kMonsterPbrSrvFormats{
    DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
    DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
    DXGI_FORMAT_R8G8B8A8_UNORM};
constexpr std::array<D3D12_SRV_DIMENSION, kEnvironmentDescriptorCount> kEnvironmentSrvDimensions{
    D3D12_SRV_DIMENSION_TEXTURE2DARRAY, D3D12_SRV_DIMENSION_TEXTURE2DARRAY,
    D3D12_SRV_DIMENSION_TEXTURE2DARRAY, D3D12_SRV_DIMENSION_TEXTURE2DARRAY,
    D3D12_SRV_DIMENSION_TEXTURE2D,
    D3D12_SRV_DIMENSION_TEXTURE2DARRAY, D3D12_SRV_DIMENSION_TEXTURE2DARRAY,
    D3D12_SRV_DIMENSION_TEXTURE2DARRAY, D3D12_SRV_DIMENSION_TEXTURE2DARRAY,
    D3D12_SRV_DIMENSION_TEXTURE2DARRAY, D3D12_SRV_DIMENSION_TEXTURE2DARRAY,
    D3D12_SRV_DIMENSION_TEXTURE2DARRAY, D3D12_SRV_DIMENSION_TEXTURE2DARRAY,
    D3D12_SRV_DIMENSION_TEXTURE2DARRAY, D3D12_SRV_DIMENSION_TEXTURE2DARRAY,
    D3D12_SRV_DIMENSION_TEXTURE2DARRAY, D3D12_SRV_DIMENSION_TEXTURE2DARRAY,
    D3D12_SRV_DIMENSION_TEXTURE2DARRAY, D3D12_SRV_DIMENSION_TEXTURE2DARRAY,
    D3D12_SRV_DIMENSION_TEXTURE2DARRAY,
    D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_SRV_DIMENSION_TEXTURE2D};
constexpr std::array<DXGI_FORMAT, kEnvironmentDescriptorCount> kEnvironmentSrvFormats{
    DXGI_FORMAT_BC7_UNORM, DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_BC5_UNORM,
    DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT,
    DXGI_FORMAT_BC7_UNORM_SRGB, DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_BC5_UNORM,
    DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_BC7_UNORM_SRGB, DXGI_FORMAT_BC5_UNORM,
    DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_BC7_UNORM_SRGB,
    DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_BC7_UNORM_SRGB,
    DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_R32G32B32A32_FLOAT,
    DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16_FLOAT};
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
constexpr std::uint32_t kBrixelizerFrameTableOffset = kVolumetricFrameTableOffset + kVolumetricFrameDescriptorCount;
constexpr std::uint32_t kViewSkinDescriptorOffset = kBrixelizerFrameTableOffset + 4 * kSceneFrameTableCount;
constexpr std::uint32_t kTextureDescriptorCount = kViewSkinDescriptorOffset + 4;
struct VolumetricFrameInputCopy { std::uint32_t target, source, count; };
constexpr std::array kVolumetricFrameInputCopies{
    VolumetricFrameInputCopy{kVolumetricGradientOffset, kPostTextureDescriptorCount + kCharacterDescriptorCount +
        kMonsterPbrDescriptorCount + kEnvironmentDescriptorCount, 1},
    VolumetricFrameInputCopy{kVolumetricLightOffset, kLightListDescriptorOffset, 3},
    VolumetricFrameInputCopy{kVolumetricShadowOffset, kVirtualShadowDescriptorOffset, 4},
    VolumetricFrameInputCopy{kVolumetricShadowRootOffset, kVirtualShadowDescriptorOffset + 5, 2},
    VolumetricFrameInputCopy{kVolumetricLocalShadowOffset, kLocalShadowDescriptorOffset, 4}};
constexpr std::uint32_t kArcherGeometryPage =
    static_cast<std::uint32_t>(kEnvironmentMaterialNames.size() * render_data::kEnvironmentGeometryLodCount);
constexpr std::uint32_t kMonsterGeometryPage = kArcherGeometryPage + 1;
constexpr std::size_t kGpuVisibilityBucketCount = kMonsterGeometryPage + 18;
constexpr std::uint32_t kProjectileGeometryPage = kMonsterGeometryPage + 6;
constexpr std::uint32_t kProceduralGeometryPage = kProjectileGeometryPage + 1;
constexpr std::uint32_t kMonsterReducedGeometryPage = kProceduralGeometryPage + 1;
static_assert(kMonsterReducedGeometryPage + 12u < kGeometryPageCapacity);
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
constexpr std::uint32_t kOutputRtvOffset = kLightingAuxRtvOffset + kLightingAuxRtvCount;
constexpr std::uint32_t kRtvBankStride = kOutputRtvOffset + kFrameCount;
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
    AllocationResource storage;
    std::array<std::uint64_t,4> offsets{}, sizes{};
    ResidencyTicket residency;
    D3D12_VERTEX_BUFFER_VIEW vertex_view{};
    D3D12_INDEX_BUFFER_VIEW index_view{};
    std::uint32_t raster_index_offset{};
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
        storage.Reset(); offsets={}; sizes={};
        residency = {};
        vertex_view = {}; index_view = {};
        raster_index_offset = 0;
        vertex_count = index_count = meshlet_count = triangle_offset = remap_offset = 0;
        page_slot = fallback_page_slot = UINT32_MAX;
        generation = 1;
        metadata = {};
        submeshes.clear();
        source_asset = {}; source_product.clear(); source_digest = {};
    }
};

struct GeometryPageDescriptorCache
{
    struct View
    {
        std::uint64_t offset{}, size{};
    };
    ComPtr<ID3D12Resource> resource_owner;
    std::array<View, kGeometryBuffersPerPage> views{};
    std::uint32_t resolved_page_slot{UINT32_MAX};
    bool valid{};
};

struct GeometryPageDescriptorBank
{
    ComPtr<ID3D12DescriptorHeap> heap_owner;
    SIZE_T heap_start{};
    UINT stride{};
    std::array<GeometryPageDescriptorCache, kGeometryPageCapacity> pages{};
};

struct MonsterAsset : GeometryPageGpu
{
    std::vector<render_data::CharacterMaterialBinding> owned_material_bindings;
    std::vector<render_data::GeometryMetadata> owned_geometry_lods;
    std::vector<render_data::CharacterIntervalBound> owned_interval_bounds;
    std::span<const render_data::CharacterMaterialBinding> material_bindings;
    std::uint32_t bone_count{};
    std::uint32_t material_count{};
    float ground_offset{};
    std::uint32_t skin_offset{};
    std::array<DirectX::XMUINT4, static_cast<std::size_t>(CharacterAnimationClip::Count)> clip_meta{};
    std::array<std::byte,32> skeleton_binding{};
    std::uint32_t deformation_flags{};
    std::span<const render_data::GeometryMetadata> geometry_lods;
    std::span<const render_data::CharacterIntervalBound> interval_bounds;
    std::array<std::size_t, static_cast<std::size_t>(CharacterAnimationClip::Count)> interval_clip_offsets{};
    std::size_t interval_rows_per_lod{};
    std::array<double, 3> interval_maximum_extent{};
    bool interval_lookup_valid{};

    void PrepareIntervalLookup() noexcept
    {
        interval_lookup_valid = false;
        interval_clip_offsets = {};
        interval_rows_per_lod = 0;
        interval_maximum_extent = {};
        if (!bone_count || geometry_lods.empty() || geometry_lods.size() > 3) return;
        bool gap{};
        for (std::size_t clip = 0; clip < clip_meta.size(); ++clip)
        {
            const auto &meta = clip_meta[clip];
            interval_clip_offsets[clip] = interval_rows_per_lod;
            if (!meta.y) { gap = true; continue; }
            if (gap || meta.y < 2 || meta.w != bone_count ||
                interval_rows_per_lod > UINT32_MAX / bone_count ||
                meta.x != interval_rows_per_lod * bone_count ||
                interval_rows_per_lod > interval_bounds.size() ||
                meta.y > interval_bounds.size() - interval_rows_per_lod)
                return;
            interval_rows_per_lod += meta.y;
        }
        if (interval_rows_per_lod == 0 ||
            interval_rows_per_lod > interval_bounds.size() / geometry_lods.size() ||
            interval_rows_per_lod * geometry_lods.size() != interval_bounds.size()) return;
        for (std::size_t lod = 0; lod < geometry_lods.size(); ++lod)
        {
            if (geometry_lods[lod].lod != lod) return;
            for (std::size_t clip = 0; clip < clip_meta.size(); ++clip)
                for (std::uint32_t frame = 0; frame < clip_meta[clip].y; ++frame)
                {
                    const auto &bound = interval_bounds[lod * interval_rows_per_lod +
                        interval_clip_offsets[clip] + frame];
                    if (bound.lod != lod || static_cast<std::size_t>(bound.clip) != clip ||
                        bound.frame != frame) return;
                    for (std::size_t axis = 0; axis < 3; ++axis)
                    {
                        if (!std::isfinite(bound.minimum[axis]) ||
                            !std::isfinite(bound.maximum[axis]) ||
                            bound.minimum[axis] > bound.maximum[axis]) return;
                        interval_maximum_extent[axis] = std::max(interval_maximum_extent[axis],
                            std::max(std::abs(double(bound.minimum[axis])),
                                std::abs(double(bound.maximum[axis]))));
                    }
                }
        }
        interval_lookup_valid = true;
    }

    [[nodiscard]] const render_data::CharacterIntervalBound *IntervalBound(
        std::uint16_t lod, CharacterAnimationClip clip, std::uint32_t frame) const noexcept
    {
        const auto clip_index = static_cast<std::size_t>(clip);
        if (!interval_lookup_valid || lod >= geometry_lods.size() ||
            clip_index >= clip_meta.size() || frame >= clip_meta[clip_index].y) return nullptr;
        return &interval_bounds[std::size_t(lod) * interval_rows_per_lod +
            interval_clip_offsets[clip_index] + frame];
    }

    void Reset() noexcept
    {
        GeometryPageGpu::Reset(); material_bindings = {}; geometry_lods = {}; interval_bounds = {};
        owned_material_bindings.clear(); owned_geometry_lods.clear(); owned_interval_bounds.clear();
        bone_count=material_count=skin_offset=deformation_flags=0; ground_offset=0;
        clip_meta={}; skeleton_binding={}; interval_clip_offsets={};
        interval_rows_per_lod=0; interval_maximum_extent={}; interval_lookup_valid=false;
    }
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
    DirectX::XMFLOAT4X4 previous_instance_unjittered_view_projection;
};

constexpr std::uint32_t kDisabledVolumetricConstantsOffset = (sizeof(FrameConstants) + 255u) & ~255u;
constexpr std::uint32_t kDisabledVolumetricConstantsSize = (sizeof(VolumetricConstants) + 255u) & ~255u;
constexpr std::uint32_t kExposureConstantsOffset =
    kDisabledVolumetricConstantsOffset + kDisabledVolumetricConstantsSize;
constexpr std::uint32_t kInstanceDataOffset = kExposureConstantsOffset + 256u;
static_assert(sizeof(FrameConstants) == 4720);
static_assert(offsetof(FrameConstants, screen_size) == 192);
static_assert(offsetof(FrameConstants, lod_clock) == 3008);
static_assert(offsetof(FrameConstants, output_size) == 3200);
static_assert(offsetof(FrameConstants, probe_options) == 3520);
static_assert(offsetof(FrameConstants, previous_grass_benders) == 3536);
static_assert(offsetof(FrameConstants, previous_grass_state) == 4048);
static_assert(offsetof(FrameConstants, previous_grass_origin_delta) == 4064);
static_assert(offsetof(FrameConstants, scene_table_bounds) == 4080);
static_assert(offsetof(FrameConstants, grass_bender_grid) == 4096);
static_assert(offsetof(FrameConstants, grass_bender_masks) == 4112);
static_assert(offsetof(FrameConstants, previous_grass_bender_grid) == 4368);
static_assert(offsetof(FrameConstants, previous_grass_bender_masks) == 4384);
static_assert(offsetof(FrameConstants, particle_terrain) == 4640);
static_assert(offsetof(FrameConstants, previous_instance_unjittered_view_projection) == 4656);

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


struct UiTextLayout
{
    std::string utf8;
    UiModel::Kind kind{};
    float font_size{}, width{}, height{};
    ComPtr<IDWriteTextLayout> layout;
};

struct UiCpuSurface
{
    bool bitmap_valid{};
    std::uint64_t bitmap_version{}, submitted_version{}, completed_version{};
    CompletionSet copy_completion;
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
    StartupOperationScope timing(RendererStartupOperation::CheckedBinaryRead);
    return ReadCheckedBinary(path, bytes, expected_bytes);
}

[[nodiscard]] inline Result ReadBinary(const RendererAssetSource &source,
                                       std::vector<std::byte> &bytes,
                                       std::optional<std::uint64_t> expected_bytes = std::nullopt)
{
    StartupOperationScope timing(RendererStartupOperation::CheckedBinaryRead);
    return source.Read(bytes, expected_bytes);
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

struct StaticShadowBakeFrame;

struct ExposureGraphLayout
{
    std::array<std::uint32_t, 4> pass_indices{};
    ResourceVersion scene_work{}, histogram{}, exposure_write{};
};

struct FinalGraphLayout
{
    bool output_dependency_interval{}, output_ready{}, presentation{};
    std::array<std::uint32_t, 2> pass_indices{};
    ResourceVersion back_buffer{};
};

struct FrameUploadGraphLayout
{
    std::uint32_t pass_index{};
    ResourceVersion dynamic_lod{}, static_lod{}, geometry_records{}, dynamic_instances{};
    ResourceVersion static_instances{}, static_refs{}, environment_stream{};
    std::vector<ResourceVersion> geometry_streams;
};

struct ParticleGraphLayout
{
    std::uint32_t pass_index{};
    std::array<ResourceVersion, 9> outputs{};
};

struct SingleOutputGraphLayout
{
    std::uint32_t pass_index{};
    ResourceVersion output{};
};

struct DoubleOutputGraphLayout
{
    std::uint32_t pass_index{};
    ResourceVersion first{}, second{};
};

struct InitialGraphLayout
{
    DoubleOutputGraphLayout exposure;
    SingleOutputGraphLayout ui;
    std::uint32_t temporal_pass_index{};
    std::array<ResourceVersion, 6> temporal{};
};

struct VisibilityGraphLayout
{
    std::uint32_t pass_index{};
    std::array<ResourceVersion, 9> outputs{};
};

struct RetestGraphLayout
{
    std::uint32_t pass_index{};
    ResourceVersion records{}, refs{}, args{};
};

struct LightingGraphLayout
{
    std::uint32_t pass_index{};
    std::array<ResourceVersion, 4> outputs{};
};

struct SurfacePassGraphLayout
{
    std::uint32_t pass_index{};
    std::array<ResourceVersion, 10> versions{};
    std::vector<ResourceVersion> sidecars;
};

struct FrameGraphLayout
{
    struct ResourceShape
    {
        D3D12_RESOURCE_DIMENSION dimension{D3D12_RESOURCE_DIMENSION_UNKNOWN};
        DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
        std::uint64_t width{};
        std::uint32_t height{}, sample_count{}, sample_quality{};
        std::uint16_t depth_or_array{}, mip_levels{};
        D3D12_TEXTURE_LAYOUT layout{D3D12_TEXTURE_LAYOUT_UNKNOWN};
        D3D12_RESOURCE_FLAGS flags{D3D12_RESOURCE_FLAG_NONE};
        D3D12_HEAP_TYPE buffer_heap{D3D12_HEAP_TYPE_DEFAULT};
        std::uint32_t alias_group{}; // Current equivalence pattern, never a retained pointer.
        [[nodiscard]] bool operator==(const ResourceShape &) const noexcept = default;
    };
    struct ImportSlot
    {
        std::string name;
        ResourceHandle handle;
        D3D12_RESOURCE_DESC description{};
        D3D12_HEAP_TYPE buffer_heap{D3D12_HEAP_TYPE_DEFAULT};
        bool certified{};
    };
    struct PassSlot { std::uint32_t id{}, declaration_index{}; };
    // Fast rejection only; helper variants, import descriptors and use ranges must also match.
    struct EarlyKey
    {
        std::uint32_t frame_slot{}, render_width{}, render_height{}, output_width{}, output_height{};
        std::uint32_t material_sidecar_planes{};
        bool presented{}, visibility{}, enhanced{}, skinned{}, previous_skin{}, static_scene_present{};
        bool exposure_initialization{}, ui_upload{}, temporal_initialization{}, offscreen_initialization{};
        bool static_scene_upload{}, static_refs_upload{}, dynamic_scene_upload{}, environment_upload{}, geometry_upload{};
        bool separate_previous_instance_scene{};
        bool geometry_draw_upload{}, hero_contributors{}, hero_occupancy{}, hero_capture{};
        bool composite_gradient_read{};
        bool dynamic_lod_initialized{}, static_lod_initialized{}, dynamic_lod_prefix{}, static_lod_prefix{};
        bool visibility_target_initialization{}, csm{}, authored_material{}, masked_material{};
        bool static_authored_material{}, static_masked_material{}, native_reconstruction{};
        bool bloom{}, outline{}, fxaa_identity{}, post_target_initialization{};
        bool screen_overlay_requested{}, entity_surface{}, decal_commands{};
        bool hdr_motion{}, hdr_dof{}, hdr_grade{}, sdk_presentation{}, devtools_ui{};
        bool output_dependency_interval{}, output_ready{};
        bool software_gi{}, contact_shadows{}, adaptive_raster{}, adaptive_compute{}, overlap_ao_csm{};
        GraphicsAoMode ao_mode{};
        GraphicsGiMode gi_mode{};
        GraphicsReflectionMode reflection_mode{};
        GraphicsShadowMode shadow_mode{};
        std::uint32_t geometry_upload_count{}, hero_work_source_mask{};
        [[nodiscard]] bool operator==(const EarlyKey &) const noexcept = default;
    } early;
    RenderGraphBuilder::PlanHandle handle{};
    std::uint64_t last_used{};
    bool certified_imports{true};
    std::vector<std::uint32_t> geometry_page_slots, software_scene_page_slots;
    // One representative logical read pair per GI leaf source, indexed by
    // resident page and vertex source kind. Actor multiplicity is frame data.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> brixelizer_leaf_keys;
    std::vector<ResourceShape> direct_import_shapes;
    std::vector<ImportSlot> imports;
    std::vector<PassSlot> passes;
    bool output_closure_cached{};
    std::vector<std::uint32_t> output_closure_slots;
    TextureStreamingLayout texture_streaming;
    TextureFeedbackLayout texture_feedback;
    TextureHandle distortion{}, bloom_half{}, bloom_quarter{}, bloom_combined{}, back_buffer{};
    ExposureGraphLayout exposure;
    InitialGraphLayout initial;
    FinalGraphLayout final;
    FrameUploadGraphLayout upload;
    ParticleGraphLayout particle;
    DoubleOutputGraphLayout skin;
    VisibilityGraphLayout visibility;
    SurfacePassGraphLayout visibility_discard, visibility_clear, gbuffer;
    SingleOutputGraphLayout depth_pyramid, final_depth_pyramid;
    RetestGraphLayout retest;
    SurfacePassGraphLayout gbuffer_retest, surface_resolve;
    SingleOutputGraphLayout raster_records, material_count;
    DoubleOutputGraphLayout material_prefix, material_scatter;
    LightingGraphLayout lighting;
    SingleOutputGraphLayout cascaded_shadow;
    SingleOutputGraphLayout devtools_ui, sdk_copy;
    SingleOutputGraphLayout distortion_pass;
    DoubleOutputGraphLayout transparent_pass;
    SingleOutputGraphLayout composite_pass;
    AdaptiveShadingLayout adaptive;
    AmbientOcclusionLayout ambient;
    SubsurfaceLayout subsurface;
    ScreenReflectionsLayout reflections;
    ScreenOverlayLayout screen_overlay;
    VolumetricLayout volumetrics;
    HeroTransparencyLayout hero;
    PostSubgraphLayout post;
    LightListLayout light_list;
    LtcLutLayout ltc;
    DecalSurfaceLayout decal_surface;
    VirtualShadowLayout virtual_shadows;
    LocalShadowLayout local_shadows;
    HdrPostMotionLayout hdr_motion;
    HdrPostGradeLayout hdr_grade;
    ClusterGraphLayout cluster;
    HlodRuntimeFrameLayout hlod;
    ResourceVersion hlod_input_geometry_records{}, hlod_input_static_instances{};
    LightingProbeLayout probe;
    LocalProbeRuntimeLayout local_probe;
    ReconstructionGuidesLayout reconstruction_guides;
    PresentationGuidesLayout presentation_guides;
    PresentationTagLayout presentation_tag;
    SceneGeometryGraphLayout scene_geometry;
    BrixelizerGraphLayout brixelizer;
};

// CPU/device discovery for one output extent. No SDK or device object is
// retained; the option catalog is rebuilt from these values when policy changes.
struct NativeGraphicsCapabilitySnapshot
{
    struct Admission
    {
        bool available{};
        GraphicsCapabilityReason reason{GraphicsCapabilityReason::Unavailable};
    };

    bool valid{};
    ViewExtent output{};
    std::array<Admission, 7> dlss_quality{}, fsr_quality{}, xess_quality{}, dsr_quality{};
    Admission dlss_sr{}, fsr_sr{}, xess_sr{}, directsr{};
    Admission fsr_fg{}, dlss_fg{}, reflex{}, xell_fg{}, xell_standalone{}, intel_fg{}, software_gi{};
    std::uint32_t dlss_fg_maximum_multiplier{};
    std::uint32_t dlss_fg_minimum_dimension{};
    std::uint32_t xess_fg_maximum_generated_frames{};
    bool dlss_fg_vsync_supported{};
    bool dlss_fg_dynamic_supported{};
};

struct D3D12Renderer::Impl
{
    struct NativeBudgetObservation
    {
        DXGI_QUERY_VIDEO_MEMORY_INFO last_valid{};
        std::chrono::steady_clock::time_point observed_at{};
        std::chrono::steady_clock::time_point attempted_at{};
        std::uint64_t success_sequence{};
        HRESULT last_result{E_PENDING};
        bool valid{}, supported{true};
    };
    struct HostBudgetObservation
    {
        MEMORYSTATUSEX last_valid{};
        std::chrono::steady_clock::time_point observed_at{};
        std::chrono::steady_clock::time_point attempted_at{};
        std::uint64_t success_sequence{};
        DWORD last_error{};
        bool valid{}, last_attempt_succeeded{};
    };
    RendererStartupTimings startup_timings;
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
    struct StaticShadowCasterBatch
    {
        struct Binding
        {
            const GeometryPageGpu *page{};
            ID3D12PipelineState *pipeline{};
            UINT material{};
            VirtualShadowFrontend frontend{};
            std::uint64_t key{};
        };
        std::shared_ptr<const StaticRenderScene> source;
        std::uint32_t generation{};
        float density{}, wind_radius{};
        std::uint64_t material_revision{}, pipeline_revision{}, revision{};
        std::size_t leaf_count{};
        std::array<std::uint64_t,kGpuVisibilityBucketCount+1> geometry{};
        std::array<std::uint32_t,kGpuVisibilityBucketCount+1> passes{};
        std::vector<VirtualShadowCaster> records;
        std::vector<ShadowCasterMap::key_type> keys;
        std::vector<Binding> bindings;
        std::optional<VirtualShadowReceiver> water;
        std::vector<VirtualShadowReceiver> receivers;
        std::vector<ShadowSurfaceReceiver> surface_receivers;
        std::optional<ShadowSurfaceReceiver> surface_aggregate;
        std::size_t receiver_proxy_count{};
        bool settled{}, mesh_shader{};
    };
    struct UiResources
    {
        std::array<AllocationResource, kFrameCount> textures;
        std::array<UiCpuSurface, kFrameCount> surfaces;
        std::vector<UiTextLayout> text_layouts;
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
        std::size_t skin_cache_vertex_capacity{}, skin_cache_bound_capacity{};
        std::uint32_t vertex_stride{SkinVertexRanges::kVertexBytes};
        SkinVertexRanges vertex_ranges;
        bool skin_cache_valid{};
        std::uint64_t package_id{};
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
    struct InstanceHistory
    {
        std::uint32_t generation{}, mesh{};
        Sequence motion_epoch{};
    };
    struct SubmittedInstanceFrame
    {
        AllocationResource scene;
        std::vector<InstanceHistory> identities;
        ID3D12Device *device{};
        std::uint64_t package_id{};
        std::uint64_t uploaded_revision{}, surface_package_id{};
        std::uint64_t clock_epoch{};
        RenderClockMode clock_mode{RenderClockMode::Scripted};
        std::chrono::nanoseconds simulation_time{};
        std::size_t capacity{}, slot_count{}, entity_surface_bytes{};
        bool submitted{}, initialized{};
    };
    // View targets/history persist across selections; submission slots own the waited banks.
    // Scene/assets/canonical deformation remain common owners.
    struct OwnedView
    {
        ViewId view_id{kMainRenderViewId};
        RenderViewRole role{RenderViewRole::Main};
        OptionTargets targets;
        UiResources ui;
        std::vector<ParticleSpawnCommand> particle_spawn_scratch;
        std::array<ComPtr<ID3D12Resource>, kFrameCount> outputs;
        std::array<AllocationResource, kFrameCount> offscreen_allocations;
        std::array<CompletionSet, kFrameCount> output_completion{};
        std::array<NativeResourceContract, kFrameCount> output_contracts{};
        std::array<std::uint64_t, kFrameCount> output_contract_generations{};
        std::array<ID3D12Device *, kFrameCount> output_contract_devices{};
        AllocationResource hdr_capture_source;
        ViewExtent hdr_capture_extent;
        bool hdr_capture_valid{}, hdr_capture_pre_exposed{};
        std::uint64_t hdr_capture_submission_id{}, hdr_capture_package_id{}, hdr_capture_snapshot_tick{};
        std::string hdr_capture_source_name{"reconstructed_hdr_before_post"};
        std::array<AllocationResource, 2> exposure_values;
        AllocationResource exposure_histogram, initial_exposure;
        std::array<std::vector<UiModel>, kFrameCount> ui_models;
        std::array<std::shared_ptr<const UiDrawContent>, kFrameCount> ui_draw_content;
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
        bool decal_surface_active{};
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
        std::uint64_t timing_source_sequence{};
        CompletionSet timing_source_output_completion;
        ViewExtent timing_last_active;
        const ReconstructionProvider *timing_last_provider{};
        SkinCache skin_history;
        ClusterViewRuntime cluster_view;
        std::shared_ptr<CanonicalSkinFrame> previous_skin_deformation;
        std::shared_ptr<SubmittedInstanceFrame> previous_instances;
        RenderSnapshotHeader last_rendered_sample;
        bool has_rendered_sample{};
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
        std::shared_ptr<StaticShadowCasterBatch> static_shadow_casters;
        std::uint64_t static_shadow_caster_revision{};
#ifndef NDEBUG
        std::unordered_map<ShadowCasterMap::key_type, std::uint64_t, ShadowCasterKeyHash> shadow_caster_key_oracle;
        bool shadow_caster_key_oracle_prepared{};
#endif
        std::uint64_t shadow_caster_preparation_epoch{};
        std::array<std::uint32_t, kGeometryPageCapacity> previous_geometry_page_versions{};
        std::uint32_t width{}, height{}, render_width{}, render_height{}, last_presented_index{};
        std::uint32_t last_submitted_frame_slot{UINT32_MAX};
        std::uint64_t last_submitted_frame_id{};
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
        ViewExtent history_output_extent;
        bool ready{};
    };
    [[nodiscard]] Result PrepareOwnedView(const RenderViewInput &, std::unique_ptr<OwnedView> &, bool decal_work);
    [[nodiscard]] Result PrepareDecalSurface(const RendererConfig &, ViewExtent, bool, DecalSurfaceCandidate &) const;
    [[nodiscard]] Result ActivateView(ViewId);
    [[nodiscard]] Result ResetOwnedViewAfterCompletion(OwnedView &);
    [[nodiscard]] Result ResetOwnedViewsAfterCompletion();
    void InvalidateCanonicalDeformationAfterCompletion() noexcept;
    [[nodiscard]] Result ValidateFrameInput(const RenderFrameInput &);
    [[nodiscard]] Result ValidateFramePayload(const RenderFrameInput &);
#if defined(HS_DEVELOPMENT_TOOLS)
    [[nodiscard]] RendererDiagnosticsMemory DiagnosticsMemory() const;
#endif
    [[nodiscard]] Result RefreshBudgetObservations();
    [[nodiscard]] ResidencySegment ResidencySegmentForHeap(D3D12_HEAP_TYPE type) const noexcept;
    [[nodiscard]] bool ResidencySegmentForAllocation(
        const D3D12MA::ALLOCATION_DESC &, ResidencySegment &) const noexcept;
    void SampleAllocatorBudgetStats(bool local_absorbed, bool nonlocal_absorbed) noexcept;
    [[nodiscard]] ResidencyTicket RegisterNativeAllocation(ResidencyCategory category,
        std::uint64_t bytes, bool pinned, ResidencySegment segment = ResidencySegment::Local);
    struct StaticDrawData
    {
        // The leaf prefix is immutable until foliage density changes; HLOD owns only the tail.
        std::vector<GpuStaticDrawRef> refs;
        std::size_t leaf_count{};
        std::array<std::pair<std::size_t, std::size_t>, kEnvironmentMaterialNames.size() * 3> ranges{};
        std::pair<std::size_t, std::size_t> ground_range{};
        std::array<std::shared_ptr<const std::vector<GpuClusterCandidate>>, kEnvironmentMaterialNames.size()> candidates;
        std::array<std::array<std::uint32_t, 3>, kEnvironmentMaterialNames.size()> candidate_catalogs{};
        std::array<bool, kEnvironmentMaterialNames.size()> candidates_valid{};
        std::array<std::vector<GeometryDemandCell>, kEnvironmentMaterialNames.size()> geometry_demand_cells;
        float foliage_density{};
        bool valid{};
    };
    struct StaticGpuScene
    {
        struct SoftwareSceneBase
        {
            std::size_t source_index{};
            GpuInstance draw{};
            std::uint32_t page_slot{};
            std::uint32_t material_ordinal{UINT32_MAX};
            std::uint32_t flags{};
            BrixelizerGeometryDomain domain{BrixelizerGeometryDomain::Opaque};
        };
        struct SoftwareSceneCache
        {
            std::vector<SoftwareSceneBase> rows;
            std::array<render_data::MaterialSurfaceLayout, kResidentMaterialCount> material_layouts{};
            std::array<render_data::EnvironmentMaterialVariant, kAuthoredMaterialNames.size()> material_variants{};
            float foliage_density{};
            bool valid{};
        };
        struct RefTable
        {
            std::shared_ptr<const std::vector<GpuStaticDrawRef>> cpu;
            AllocationResource storage, upload;
            CompletionSet last_completion{}, upload_completion{};
            bool submitted{};
        };
        std::shared_ptr<const StaticRenderScene> source;
        StaticDrawData draws;
        SoftwareSceneCache software_scene;
        std::shared_ptr<RefTable> refs;
        std::vector<std::shared_ptr<RefTable>> retired_refs;
        bool refs_dirty{};
        AllocationResource instances;
        AllocationResource upload;
        GpuSceneTableLayout layout{};
        GpuLodStateBuffer lod_states;
        CompletionSet last_completion{};
        CompletionSet upload_completion{};
        bool submitted{};
        std::uint32_t instance_generation{};
        bool has_authored_material{}, has_masked_material{};
    };
    struct RetiredStreamingTexture
    {
        AllocationResource texture;
        ResidencyTicket residency;
        CompletionSet completion{};
    };
    struct PreparedGeometryUpload
    {
        render_data::PreparedGeometryPage page;
        const render_data::GeometryDescriptor *descriptor{};
        std::string source_product;
        std::array<std::byte, 32> digest{};
    };
    [[nodiscard]] Result PrepareReadGeometry(const RendererConfig &, std::string_view,
        std::span<const std::byte>, const render_data::GeometryMetadata &, PreparedGeometryUpload &) const;
    struct EnvironmentTextureStream
    {
        RendererAssetSource source;
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
            source.clear();
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
        RendererAssetSource source;
        std::shared_ptr<const RendererDevelopmentCatalog> development_catalog;
        std::string logical_id;
        std::string source_id;
        std::string content_generation;
        StreamingRequestId request;
        std::future<std::vector<std::byte>> read_future;
        std::vector<std::byte> decoded_storage;
        std::shared_ptr<PreparedGeometryUpload> preparation;
        ResidencyTicket decoded_residency;
        ResidencyTicket decode_reservation;
        std::uint64_t read_bytes{}, last_demand_frame{};
        GeometryPageGpu candidate;
        AllocationResource upload;
        render_data::GeometryMetadata metadata;
        render_data::GeometryHandle compiled_geometry;
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
            source.clear();
            logical_id.clear();
            source_id.clear();
            content_generation.clear();
            request = {};
            if (read_future.valid())
                read_future.wait();
            read_future = {};
            std::vector<std::byte>{}.swap(decoded_storage);
            preparation.reset();
            decoded_residency = {};
            candidate.Reset();
            decode_reservation = {};
            read_bytes = last_demand_frame = 0;
            upload.Reset();
            upload_completion = {};
            metadata = {};
            compiled_geometry = {};
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
    [[nodiscard]] Result CreateDevice(const RendererConfig &configuration, OwnedView &view);
    [[nodiscard]] Result CreateRootSignature();
    [[nodiscard]] Result CreateStaticShadowBakeGeometry();
    [[nodiscard]] Result CreateSwapChainAndTargets(OwnedView &view);
    [[nodiscard]] Result CreateDepthAndShadow(OwnedView &view);
    [[nodiscard]] Result CreatePostProcessTargets(OwnedView &view);
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
    [[nodiscard]] Result PrepareExposureResources(OwnedView &view);
    void WriteViewDescriptors(OwnedView &owner, std::uint32_t frame, std::uint32_t history_read,
        std::uint32_t history_write, std::uint32_t exposure_read, std::uint32_t exposure_write,
        D3D12_GPU_VIRTUAL_ADDRESS exposure_constants) noexcept;
    void CommitOptionTargets(OwnedView &owner, OptionTargets &targets) noexcept;
    [[nodiscard]] Result PrepareLodStateBuffer(GpuLodStateBuffer &buffer, std::size_t capacity);
    void CommitLodStateBuffer(GpuLodStateBuffer &buffer);
    void SwapOptionPools(OwnedView &owner, std::array<TransientResourcePool, kFrameCount> &pools) noexcept;
    void ClearTransientDescriptors() noexcept;
    void WriteOptionTargetDescriptors(OwnedView &owner, std::uint32_t frame) noexcept;
    [[nodiscard]] Result CreatePipeline(const RendererConfig &pipeline_config);
    [[nodiscard]] Result SelectTextureFeedbackShaders(const RendererConfig &, RendererConfig &) const;
    void InitializeCanonicalDescriptors(ID3D12DescriptorHeap *) const noexcept;
    [[nodiscard]] Result CreateSharedDescriptorHeaps();
    void InitializeFrameSamplerDescriptors(std::uint32_t frame) noexcept;
    void PublishFrameDescriptors(std::uint32_t frame_slot) const noexcept;
    [[nodiscard]] Result CreateClusterPipelines(const RendererConfig &pipeline_config);
    [[nodiscard]] Result PrepareReconstructionProvider(const RendererReconstructionRequest *,
        bool dynamic_resolution, float minimum_percent, float maximum_percent, ViewExtent output,
        ReconstructionPlan &, ViewExtent &capacity, std::unique_ptr<ReconstructionProvider> &candidate,
        const RendererConfig *candidate_config = nullptr);
    [[nodiscard]] Result InitializeReconstructionSessionsBeforeDevice();
    [[nodiscard]] Result BindReconstructionSessionsToDevice();
    [[nodiscard]] Result CommitReconstructionProvider(OwnedView &owner, std::unique_ptr<ReconstructionProvider> &candidate,
        ReconstructionGuidesCandidate &&guides, ViewExtent capacity);
    [[nodiscard]] Result PrepareReconstructionGuides(const OwnedView &, const RendererConfig &, ViewExtent capacity,
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
    [[nodiscard]] Result AdmitResidentAllocation(const D3D12_RESOURCE_DESC &,
        const D3D12MA::ALLOCATION_DESC &);
    void RecordResidentAllocation(const AllocationResource &, D3D12_HEAP_TYPE) noexcept;
    [[nodiscard]] Result PrepareResidentServices(Impl &candidate);
    void SwapResidentServices(Impl &candidate) noexcept;
    void SwapResidentPipelines(Impl &candidate) noexcept;
    [[nodiscard]] Result ReloadResidentGeneration(RendererConfig candidate_config);
    [[nodiscard]] Result CreateGpuData();
    [[nodiscard]] Result CreateCharacterTextures();
    [[nodiscard]] Result UploadVfxNoiseVolume(std::vector<AllocationResource> &uploads);
    [[nodiscard]] Result CreateEnvironmentTextures();
    enum class MaterialPreparationScope : std::uint8_t { Resident, StaticEnvironment };
    [[nodiscard]] Result PrepareMaterialParameters(const RendererConfig &candidate,
        bool require_compatible_layout, MaterialPreparationScope scope = MaterialPreparationScope::Resident);
    void CommitMaterialParameters() noexcept;
    [[nodiscard]] Result PrepareDecalMaterialParameters(const RendererConfig &, bool require_compatible_layout,
        std::vector<render_data::MaterialParameterSlot> &slots);
    void CommitDecalMaterialParameters() noexcept;
    [[nodiscard]] Result LoadMaterialSurfaceProfile(const RendererConfig &candidate,
        MaterialPreparationScope scope = MaterialPreparationScope::Resident);
    [[nodiscard]] Result PrepareMaterialSidecars(std::array<AllocationResource, 2> &targets,
                                               std::uint32_t width, std::uint32_t height);
    [[nodiscard]] DXGI_FORMAT MaterialSidecarFormat(std::size_t plane) const noexcept;
    void WriteMaterialSidecarDescriptors(OwnedView &owner, std::uint32_t frame) noexcept;
    [[nodiscard]] Result CreateEnvironmentBrdfLut(const RendererConfig &lut_config,
                                                AllocationResource &lut,
                                                std::vector<AllocationResource> *upload_batch = nullptr);
    void WriteEnvironmentBrdfLutDescriptors();
    enum class EnvironmentMaterialResidency : std::uint8_t { RuntimeFallback, CaptureOnly };
    [[nodiscard]] Result CreateEnvironmentMeshes();
    [[nodiscard]] Result CreateEnvironmentMeshes(std::span<const std::uint32_t> selected_page_indices,
        EnvironmentMaterialResidency residency);
    [[nodiscard]] Result CreateClusterResources(std::uint32_t generation_tag);
    [[nodiscard]] Result CreateClusterViewResources(ClusterViewRuntime &);
    void ResetClusterViewResources(ClusterViewRuntime &) noexcept;
    void ResetInactiveClusterViewResources() noexcept;
    [[nodiscard]] Result PrepareAmbientOcclusion(const OwnedView &, const RendererConfig &,
        ViewExtent capacity, AmbientOcclusionCandidate &, bool adaptive_compute = false) const;
    [[nodiscard]] Result PrepareAdaptiveShading(const OwnedView &, const RendererConfig &,
        ViewExtent capacity, AdaptiveShadingCandidate &) const;
    [[nodiscard]] Result BuildAdaptiveMaterialPolicies(std::vector<AdaptiveShadingMaterialPolicy> &) const;
    [[nodiscard]] Result PrepareSurfaceLighting(const OwnedView &, const RendererConfig &,
        ViewExtent capacity, SubsurfaceCandidate &, ScreenReflectionsCandidate &) const;
    [[nodiscard]] Result PrepareHeroTransparency(const OwnedView &, const RendererConfig &,
        ViewExtent capacity, HeroTransparencyCandidate &) const;
    [[nodiscard]] Result PrepareLightingProbe(const RendererConfig &, LightingProbeCandidate &) const;
    [[nodiscard]] Result PrepareLtcLut(const RendererConfig &, LtcLutCandidate &) const;
    [[nodiscard]] Result PrepareHdrPost(const OwnedView &, const RendererConfig &, ViewExtent render_capacity,
        ViewExtent output_extent, HdrPostCandidate &) const;
    [[nodiscard]] Result PrepareScreenOverlay(const RendererConfig &, ViewExtent,
        ScreenOverlayCandidate &) const;
    [[nodiscard]] Result PrepareSceneGeometry(const RendererConfig &, SceneGeometryCandidate &) const;
    [[nodiscard]] Result PrepareBrixelizerGi(const OwnedView &, const RendererConfig &, ViewExtent, BrixelizerCandidate &) const;
    [[nodiscard]] Result PrepareVirtualShadows(const OwnedView &, const RendererConfig &, ViewExtent, VirtualShadowCandidate &) const;
    struct AdditionalShadowPrepared { LocalShadowCandidate local; };
    [[nodiscard]] Result PrepareAdditionalShadows(const OwnedView &, const RendererConfig &, ViewExtent, AdditionalShadowPrepared &) const;
    [[nodiscard]] Result CommitAdditionalShadows(OwnedView &owner, AdditionalShadowPrepared &&);
    [[nodiscard]] Result PrepareLocalProbes(const RendererConfig &, LocalProbeRuntimeCandidate &) const;
    [[nodiscard]] Result AppendHlodPages(ClusterRuntimeState &);
    [[nodiscard]] Result BeginHlodViewPackage(const RenderFrameInput &);
    [[nodiscard]] Result RetireHlodView(ViewId);
    [[nodiscard]] HlodViewRuntime *ActiveHlodView() noexcept;
    [[nodiscard]] const HlodViewRuntime *ActiveHlodView() const noexcept;
    [[nodiscard]] Result PrepareHlodStaticScene(const std::shared_ptr<const StaticRenderScene> &, std::span<const RenderInstance> &);
    [[nodiscard]] Result PrepareHlodFrame(std::uint32_t, std::vector<GpuStaticDrawRef> &, bool &);
    [[nodiscard]] Result PrepareHlodFrameRecordData(std::uint32_t frame_slot,
        std::uint32_t active_view_mask, ResourceVersion geometry_records,
        ResourceVersion static_scene, ResourceVersion proxy_refs,
        HlodRuntimeFrameRecordData &);
    [[nodiscard]] bool MatchesHlodFrameLayout(const HlodRuntimeFrameRecordData &,
        const HlodRuntimeFrameLayout &) const noexcept;
    [[nodiscard]] Result AddHlodFramePasses(std::uint32_t, std::uint32_t,
        ResourceVersion, ResourceVersion, ResourceVersion, HlodFrameGraph &,
        HlodRuntimeFrameLayout *capture = nullptr,
        HlodRuntimeFrameRecordData *prepared = nullptr,
        const HlodRuntimeFrameLayout *retained = nullptr);
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
    [[nodiscard]] Result PrepareVolumetrics(const OwnedView &, const RendererConfig &, ViewExtent, VolumetricPrepared &) const;
    [[nodiscard]] Result CommitVolumetrics(OwnedView &owner, VolumetricPrepared &&);
    [[nodiscard]] RendererFeaturePlan ResolveFeatures(const RendererConfig &) const noexcept;
    [[nodiscard]] Result PrepareLightList(const OwnedView &, const RendererConfig &, ViewExtent, LightListCandidate &) const;
    [[nodiscard]] Result PrepareHeroBindings(ID3D12GraphicsCommandList *,
        std::vector<AllocationResource> &uploads);
    [[nodiscard]] ClusterFrameRecordData PrepareClusterFrameRecordData(std::uint32_t frame_slot,
        bool visibility) const;
    [[nodiscard]] bool MatchesClusterGraphLayout(const ClusterFrameRecordData &,
        const ClusterGraphLayout &) const noexcept;
    [[nodiscard]] Result AddClusterFramePasses(std::uint32_t frame_slot,
        ResourceVersion &geometry_records, ResourceVersion *raster_records,
        ResourceVersion static_scene, ClusterFrameGraph &state,
        const ClusterFrameRecordData &, ClusterGraphLayout *capture = nullptr,
        const ClusterGraphLayout *retained = nullptr);
    [[nodiscard]] Result AddClusterOcclusionPass(OwnedView &, std::uint32_t frame_slot,
        ClusterFrameGraph &, ResourceVersion hzb, bool retest,
        ClusterGraphLayout *capture = nullptr, const ClusterGraphLayout *retained = nullptr);
    void ReadClusterFrame(PassBuilder &, const ClusterFrameGraph &, bool draw, bool retest = false) const;
    [[nodiscard]] Result DrawClusterGeometry(ID3D12GraphicsCommandList *,
        std::uint32_t frame_slot, std::uint32_t view, bool retest = false);
    [[nodiscard]] Result CommitClusterFrameGraph(OwnedView &, std::uint32_t frame_slot,
        ClusterFrameGraph &, const CompletionSet &);
    [[nodiscard]] Result PrepareClusterFrame(OwnedView &, std::uint32_t frame_slot,
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
    void WriteClusterCutDescriptors(OwnedView &, std::uint32_t frame_slot, std::uint32_t ordinal);
    void MarkClusterFrameUse(std::uint32_t frame_slot, const CompletionSet &) noexcept;
    [[nodiscard]] const GeometryPageGpu *FindClusterGeometryPage(std::uint32_t physical_slot) const noexcept;
    [[nodiscard]] bool ClusterHierarchyReady(std::uint32_t ordinal) const noexcept;
    void ResetClusterResources() noexcept;
    [[nodiscard]] Result CreateGeometryPage(
        GeometryPageGpu &page, const render_data::GeometryMetadata &metadata,
        std::span<const std::byte> package_bytes, AssetId page_asset,
        std::uint32_t page_slot, ID3D12GraphicsCommandList *list,
        std::vector<AllocationResource> &uploads,
        PreparedGeometryUpload *preparation = nullptr);
    [[nodiscard]] Result CreateProceduralGeometry(
        ID3D12GraphicsCommandList *list, std::vector<AllocationResource> &uploads);
    [[nodiscard]] Result CreateVisibilityTargets(OwnedView &view);
    [[nodiscard]] Result PrepareVisibilityTargets(OptionTargets &targets,
        std::uint32_t target_width, std::uint32_t target_height);
    [[nodiscard]] Result WriteVisibilityDescriptors(OwnedView &, std::uint32_t frame_slot,
        ID3D12Resource *static_scene, ID3D12Resource *dynamic_scene,
        ID3D12Resource *static_refs, std::uint64_t static_ref_offset,
        std::uint32_t static_ref_count,
        std::uint64_t dynamic_ref_offset, std::uint32_t dynamic_ref_count);
    [[nodiscard]] Result PrepareGeometryPageUpload(
        GeometryPageGpu &page, const render_data::GeometryMetadata &metadata,
        std::span<const std::byte> package_bytes, AssetId page_asset,
        std::uint32_t page_slot, AllocationResource &upload,
        PreparedGeometryUpload *preparation = nullptr);
    void RecordGeometryPageUpload(ID3D12GraphicsCommandList *list,
        const GeometryPageGpu &page, const AllocationResource &upload) const;
    [[nodiscard]] const GeometryPageGpu *FindGeometryPage(std::uint32_t page_slot) const noexcept;
    void WriteGeometryPageDescriptors(std::uint32_t frame_slot);
    [[nodiscard]] FrameContext *SubmittedFrame(const OwnedView &owner) noexcept;
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
    [[nodiscard]] Result CreateUiTexture(OwnedView &view);
    [[nodiscard]] Result PrepareUiResources(UiResources &, std::uint32_t output_width,
                                            std::uint32_t output_height);
    void CommitUiResources(OwnedView &owner, UiResources &) noexcept;
    [[nodiscard]] Result RasterizeUi(OwnedView &owner, std::uint32_t frame_index,
                                       std::span<const UiModel> models,
                                       std::shared_ptr<const UiDrawContent> content = {});
    [[nodiscard]] Result CreateAllocation(AllocationResource &output,
                                          const D3D12MA::ALLOCATION_DESC &allocation,
                                          const D3D12_RESOURCE_DESC &resource,
                                          D3D12_RESOURCE_STATES initial_state,
                                          const D3D12_CLEAR_VALUE *clear = nullptr);
    [[nodiscard]] Result UpdateResidencyBudget();
    [[nodiscard]] Result TryRelieveStreamingPressure(std::uint64_t bytes,
        std::uint32_t priority, bool need_cluster_slot = false,
        bool for_hlod_admission = false);
    [[nodiscard]] Result TryRelieveStreamingPressure(std::uint64_t local_bytes,
        std::uint64_t nonlocal_bytes, std::uint32_t priority, bool need_cluster_slot = false,
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
        std::vector<AllocationResource> &uploads, std::uint32_t &texture_index,
        render_data::TextureHandle compiled_texture = {});
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
    std::shared_ptr<const RendererDevelopmentCatalog> resident_development_catalog;
    std::shared_ptr<StaticShadowBakeFrame> pending_static_shadow_bake;
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
    EssentialBootstrapAdmission essential_bootstrap_admission;
    bool essential_bootstrap_active{};
    NativeBudgetObservation local_budget_observation, nonlocal_budget_observation;
    HostBudgetObservation host_budget_observation;
    bool adapter_unified_memory{};
    bool adapter_architecture_observed{};
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
    PersistentGpuSceneSlots dynamic_scene_slots;
    SkinCache skin_cache;
#if defined(HS_DEVELOPMENT_TOOLS)
    std::ofstream dynamic_instance_upload_log;
#endif
    std::array<ComPtr<ID3D12Resource>, kFrameCount> back_buffers;
    // DXGI owns these swapchain surfaces independently of application placed heaps.
    // Identities are borrowed from the matching back_buffers entry and rebuilt on acquisition.
    std::array<NativeResourceContract, kFrameCount> back_buffer_contracts{};
    std::array<std::uint64_t, kFrameCount> back_buffer_contract_generations{};
    std::array<ID3D12Device *, kFrameCount> back_buffer_contract_devices{};
    std::array<CompletionSet, kFrameCount> back_buffer_completion{};
    GeometryPageGpu archer_geometry;
    GeometryPageGpu gel_projectile_geometry;
    GeometryPageGpu procedural_geometry;
    std::array<MonsterAsset, 6> monster_assets;
    std::array<std::array<GeometryPageGpu, 2>, 6> monster_reduced_geometry;
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
    std::uint64_t baked_generation_material_revision{};
    std::uint64_t baked_generation_pipeline_revision{};
    mutable std::shared_ptr<const VirtualShadowBakeData> baked_shadow_source;
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
    AllocationResource environment_brdf_init_upload;
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
    struct EnvironmentDescriptorBank
    {
        struct Entry
        {
            ComPtr<ID3D12Resource> resource;
            bool written{};
        };
        ComPtr<ID3D12DescriptorHeap> heap;
        D3D12_CPU_DESCRIPTOR_HANDLE first{};
        UINT stride{};
        std::array<Entry, kAuthoredMaterialNames.size() * render_data::kMaterialTextureSlots> entries;
    };
    mutable std::array<EnvironmentDescriptorBank, kFrameCount> environment_descriptor_banks;
    std::array<render_data::EnvironmentMaterialVariant, kAuthoredMaterialNames.size()> environment_material_variants;
    struct EnvironmentMeshGpu : GeometryPageGpu
    {
        float bounding_radius{};
    };
    std::array<EnvironmentMeshGpu, kEnvironmentMaterialNames.size() * render_data::kEnvironmentGeometryLodCount> environment_meshes;
    std::array<EnvironmentClusterCatalog, kEnvironmentMaterialNames.size()> environment_cluster_catalogs;
    std::unique_ptr<ClusterRuntimeState> cluster_runtime;
    LightingProbeSystem lighting_probe;
    LtcLutSystem ltc_lut;
    SceneGeometrySystem scene_geometry;
    LocalProbeRuntime local_probes;
    std::unique_ptr<HlodRuntime> hlod_runtime;
    std::vector<VolumetricVolume> volumetric_media;
    std::uint64_t volumetric_revision{};
    std::unique_ptr<StreamlineSession> streamline_session;
    NativeGraphicsCapabilitySnapshot native_graphics_capabilities;
    std::unique_ptr<FsrSession> fsr_session;
    SdkPresentation sdk_presentation;
    PresentationGuides presentation_guides;
    ComPtr<ID3D12Device> streamline_device_proxy;
    ComPtr<ID3D12CommandQueue> streamline_queue_proxy;
    ComPtr<IDXGIFactory2> streamline_factory_proxy;
    std::uint8_t native_input_phase{};
    std::map<std::uint32_t, std::unique_ptr<OwnedView>> owned_views;
    std::vector<std::unique_ptr<OwnedView>> unresolved_view_candidates;
    ViewId active_view_id{kMainRenderViewId};
    bool auto_fg_cadence_suppressed{};
    bool auto_fg_cadence_tracking{};
    std::uint64_t auto_fg_cadence_sample_id{};
    struct AutoFgCadenceSample
    {
        std::uint64_t sequence{}, frame_id{}, submit_qpc{}, qpc_frequency{};
        bool validation{}, gpu_validation{};
    };
    std::array<AutoFgCadenceSample, kRendererDiagnosticsFrameCapacity> auto_fg_cadence_samples{};
    std::uint64_t auto_fg_cadence_sequence{}, auto_fg_cadence_completed_sequence{};
    RenderViewRole active_view_role{RenderViewRole::Main};
    const RenderViewInput *explicit_view{};
    const RenderFrameInput *explicit_frame{};
    bool render_scene_inputs{true};
    std::uint32_t active_native_viewport_id{}, next_native_viewport_id{1};
    std::uint64_t view_configuration_revision{1}, rendered_submission_count{}, render_package_id{}, active_render_package_id{};
    std::chrono::steady_clock::time_point render_package_time;
    float render_package_alpha{1.0f};
    RenderSceneSample render_scene_sample;
    struct PreparedScenePackage
    {
        std::vector<RenderInstance> instances;
        std::vector<RenderInstance> ordered_instances;
        // Bound to owned ordered_instances, refreshed after packet ordering.
        RenderInstanceLookup instance_lookup;
        std::vector<std::uint64_t> skin_attachment_ids;
        std::vector<std::uint32_t> ordered_source_indices, order_scratch;
        std::vector<std::uint16_t> selected_lods, ordered_lods;
        std::vector<PersistentGpuSceneSlot> gpu_slots;
        std::optional<MonsterLodInstanceOrder> order;
        std::unordered_map<std::uint64_t, const AnimationPoseRef *> poses_by_id;
        std::vector<const AnimationPoseRef *> poses_by_source;
        EntitySurfaceFrame entity_surfaces;
        std::vector<std::pair<std::uint64_t,std::uint8_t>> vfx_replacements;
        std::vector<MonsterLodPreparedLocalLight> local_shadow_consumers;
        std::shared_ptr<CanonicalSkinFrame> skin_deformation;
        bool monster_lod_consumers_valid{}, vsm_skin_consumer{};
        bool ready{}, gpu_slots_ready{};

        void ReleaseBorrowed() noexcept
        {
            poses_by_id.clear();
            poses_by_source.clear();
            gpu_slots.clear();
            local_shadow_consumers.clear();
            skin_deformation.reset();
            order.reset();
            monster_lod_consumers_valid = false;
            vsm_skin_consumer = false;
            ready = false;
            gpu_slots_ready = false;
        }
    } prepared_scene_package;
    struct PreparedArcherPalette
    {
        RenderPoseKey key;
        std::array<DirectX::XMFLOAT4X4, kMaxCharacterBones> bones{};
    };
    std::vector<PreparedArcherPalette> prepared_archer_palettes;
    std::shared_ptr<CanonicalSkinFrame> canonical_skin_current;
    std::shared_ptr<SubmittedInstanceFrame> canonical_instance_current;
    std::vector<std::shared_ptr<SubmittedInstanceFrame>> instance_history_frames;
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
#if defined(HS_DEVELOPMENT_TOOLS)
    RendererDiagnosticsRecorder diagnostics;
#endif
    GpuPassTimings completed_gpu_timings;
    struct SourceTimingHistory
    {
        SourceTimingEndpoint latest;
        DynamicResolutionSample qualified;
    };
    mutable std::unordered_map<std::uint32_t, SourceTimingHistory> timing_previous_source_endpoints;
#if defined(HS_DEVELOPMENT_TOOLS)
    // Capture-only pending completion snapshots. Both frame count and pass count are bounded.
    mutable std::vector<GpuPassTimings> pending_gpu_timings;
    mutable std::size_t pending_gpu_timing_passes{};
    mutable bool gpu_timing_stream_overflowed{};
    std::vector<RendererHeroWorkSample> hero_work_pending;
    RendererHeroWorkStatus hero_work_retired;
#endif
    [[nodiscard]] Result RetireHeroTransparency(HeroTransparencySystem &);
    ComPtr<IWICImagingFactory> ui_wic_factory;
    ComPtr<ID2D1Factory> ui_d2d_factory;
    ComPtr<IDWriteFactory5> ui_dwrite_factory;
    ComPtr<IDWriteInMemoryFontFileLoader> ui_font_loader;
    ComPtr<IDWriteFontCollection1> ui_font_collection;
    std::wstring ui_font_family;
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
    render_data::CharacterMetadataPackageData archer_owned_character;
    std::span<const CharacterClipHeader> archer_clips;
    render_data::CharacterAnimationExtensionData archer_named_animations;
    const render_data::AnimationDescriptor *archer_compiled_animation{};
    std::vector<render_data::PackedCharacterTransform> archer_packed_transforms, archer_named_packed_transforms;
    std::span<const std::uint16_t> archer_parents;
    std::span<const std::array<float, 16>> archer_inverse_bind_matrices;
    std::span<const CharacterLocalTransform> archer_transforms;
    std::span<const float> archer_upper_body_weights;
    std::vector<SkinnedVertex> archer_support_vertices;
    std::uint32_t archer_bone_count{};
    std::uint32_t archer_material_count{};
    std::span<const render_data::CharacterMaterialBinding> archer_material_bindings;
    float archer_ground_offset{};
    RenderGraphBuilder graph;
    std::array<FrameGraphLayout, 8> graph_layouts{};
    std::vector<FrameGraphLayout::ResourceShape> graph_direct_shapes_scratch;
    std::vector<render_graph::ResourceAlias> graph_alias_scratch;
    std::uint64_t graph_layout_use_serial{};
    RenderGraphQueueExecutor graph_queue_executor;
    UINT rtv_stride{};
    UINT dsv_stride{};
    UINT srv_stride{};
    bool frame_submission_faulted{};
    std::array<GeometryPageDescriptorBank, kFrameCount> geometry_page_descriptor_banks{};
    std::uint64_t validation_errors{};
    RendererValidationStatus validation_status{};
    std::uint32_t last_particle_count{};
    bool enhanced{};
    bool mesh_shader_supported{};
    bool mesh_shader_enabled{};
    bool dred_enabled{};
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
    Tick last_particle_tick{};
    std::uint64_t render_session_id{};
};

} // namespace hs
