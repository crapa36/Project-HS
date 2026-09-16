#pragma once

#include <hs/core/dds_format.hpp>
#include <hs/core/process_info.hpp>
#include <hs/renderer/renderer.hpp>

#include "render_graph.hpp"

#include "d3d12_resources.hpp"
#include "renderer_diagnostics.hpp"

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
#include <limits>
#include <numeric>
#include <span>
#include <string>
#include <tuple>
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
constexpr std::uint32_t kParticleCount = 10'000;
constexpr std::uint32_t kUiWidth = 1'920;
constexpr std::uint32_t kUiHeight = 1'080;
constexpr std::uint32_t kPostTextureDescriptorCount = 10;
constexpr std::uint32_t kCharacterDescriptorCount = 3;
constexpr std::uint32_t kMonsterPbrDescriptorCount = 5;
constexpr std::uint32_t kEnvironmentTextureCount = 20;
constexpr std::uint32_t kEnvironmentDescriptorCount = 21;
constexpr std::uint32_t kCharacterTextureSize = 2'048;
constexpr std::uint32_t kTextureDescriptorCount =
    kPostTextureDescriptorCount + kCharacterDescriptorCount + kMonsterPbrDescriptorCount +
    kEnvironmentDescriptorCount;
constexpr std::uint32_t kInstanceDataOffset = 12 * 1024;
constexpr std::uint32_t kTimestampCountPerFrame =
    static_cast<std::uint32_t>(kRenderPassCount * 2);
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

struct GpuInstance
{
    DirectX::XMFLOAT4 position;
    DirectX::XMFLOAT4 scale;
    std::uint32_t color{};
    std::uint32_t mesh{};
    float yaw{};
    float padding{};
};

static_assert(sizeof(GpuInstance) == 48);

struct MonsterAsset
{
    AllocationResource vertices;
    D3D12_VERTEX_BUFFER_VIEW vertex_view{};
    std::uint32_t vertex_count{};
    std::uint32_t bone_count{};
    std::uint32_t material_count{};
    float ground_offset{};
    std::uint32_t skin_offset{};
    std::array<DirectX::XMUINT4, static_cast<std::size_t>(CharacterAnimationClip::Count)> clip_meta{};
};

struct FrameConstants
{
    DirectX::XMFLOAT4X4 view_projection;
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
};

static_assert(sizeof(FrameConstants) <= kInstanceDataOffset);

struct GpuParticle
{
    DirectX::XMFLOAT4 position_life;
    DirectX::XMFLOAT4 initial_position_spawn_time;
    DirectX::XMFLOAT4 initial_velocity_max_life;
    DirectX::XMFLOAT4 start_color;
    DirectX::XMFLOAT4 end_color;
    DirectX::XMFLOAT4 size_rotation;
    DirectX::XMFLOAT4 physics_metadata;
};
static_assert(sizeof(GpuParticle) == 112);

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
};
static_assert(sizeof(GpuParticleSpawnCommand) == 160);


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
                                std::vector<std::byte> &bytes)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               std::format("Missing shader: {}", path.string()));
    }
    const auto size = stream.tellg();
    bytes.resize(static_cast<std::size_t>(size));
    stream.seekg(0);
    stream.read(reinterpret_cast<char *>(bytes.data()), size);
    return Result::Success();
}

struct DdsHeaderDx10
{
    DXGI_FORMAT format;
    D3D12_RESOURCE_DIMENSION dimension;
    std::uint32_t misc_flag;
    std::uint32_t array_size;
    std::uint32_t misc_flags2;
};

[[nodiscard]] inline Result LoadVfxMaskDds(const std::filesystem::path &path,
                                    DdsHeader &header,
                                    std::uint32_t &sprite_count,
                                    std::span<const std::byte> &pixels,
                                    std::vector<std::byte> &storage)
{
    if (auto loaded = ReadBinary(path, storage); !loaded) return loaded;
    constexpr std::uint32_t dx10 = 0x30315844;
    if (storage.size() < sizeof(kDdsMagic) + sizeof(header) + sizeof(DdsHeaderDx10))
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "VFX mask DDS header is truncated.");
    std::uint32_t magic{};
    DdsHeaderDx10 extension{};
    std::memcpy(&magic, storage.data(), sizeof(magic));
    std::memcpy(&header, storage.data() + sizeof(magic), sizeof(header));
    std::memcpy(&extension, storage.data() + sizeof(magic) + sizeof(header),
                sizeof(extension));
    if (magic != kDdsMagic || header.size != 124 || header.pixel_format.size != 32 ||
        header.pixel_format.four_cc != dx10 || header.width != 512 ||
        header.height != 512 || header.mip_count != 10 ||
        extension.format != DXGI_FORMAT_BC4_UNORM ||
        extension.dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        extension.array_size == 0 ||
        extension.array_size > std::numeric_limits<std::uint16_t>::max())
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "VFX mask DDS layout is invalid.");
    pixels = std::span(storage).subspan(sizeof(magic) + sizeof(header) +
                                        sizeof(extension));
    std::size_t expected{};
    sprite_count = extension.array_size;
    for (std::uint32_t slice = 0; slice < sprite_count; ++slice)
        for (std::uint32_t mip = 0; mip < header.mip_count; ++mip)
        {
            const auto width = std::max(1u, header.width >> mip);
            const auto height = std::max(1u, header.height >> mip);
            expected += static_cast<std::size_t>((width + 3) / 4) *
                        ((height + 3) / 4) * 8;
        }
    if (pixels.size() != expected)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "VFX mask DDS payload is invalid.");
    return Result::Success();
}

[[nodiscard]] inline Result LoadRgbaDds(const std::filesystem::path &path,
                                  std::uint32_t &width, std::uint32_t &height,
                                  std::span<const std::byte> &pixels,
                                  std::vector<std::byte> &storage,
                                  std::uint32_t *mip_count = nullptr)
{
    if (auto loaded = ReadBinary(path, storage); !loaded)
    {
        return loaded;
    }
    if (storage.size() < sizeof(kDdsMagic) + sizeof(DdsHeader))
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Character DDS header is truncated.");
    }
    std::uint32_t magic{};
    DdsHeader header{};
    std::memcpy(&magic, storage.data(), sizeof(magic));
    std::memcpy(&header, storage.data() + sizeof(magic), sizeof(header));
    const auto levels = std::max(header.mip_count, 1u);
    std::uint64_t pixel_bytes{};
    auto mip_width = header.width;
    auto mip_height = header.height;
    if (levels > 32 || (!mip_count && levels != 1))
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Character DDS mip count is invalid.");
    for (std::uint32_t mip = 0; mip < levels; ++mip)
    {
        pixel_bytes += static_cast<std::uint64_t>(mip_width) * mip_height * 4;
        mip_width = std::max(mip_width / 2, 1u);
        mip_height = std::max(mip_height / 2, 1u);
    }
    if (magic != kDdsMagic || header.size != 124 || header.pixel_format.size != 32 ||
        header.pixel_format.rgb_bit_count != 32 || header.width == 0 ||
        header.height == 0 || header.pitch != header.width * 4 ||
        sizeof(magic) + sizeof(header) + pixel_bytes != storage.size())
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Character DDS layout is invalid.");
    }
    width = header.width;
    height = header.height;
    if (mip_count) *mip_count = levels;
    pixels = std::span(storage).subspan(sizeof(magic) + sizeof(header));
    return Result::Success();
}

[[nodiscard]] inline Result LoadCharacterAsset(
    const std::filesystem::path &path, std::vector<SkinnedVertex> &vertices,
    std::vector<CharacterClipHeader> &clips,
    std::vector<std::uint16_t> &parents,
    std::vector<std::array<float, 16>> &inverse_bind_matrices,
    std::vector<float> &upper_body_weights,
    std::vector<CharacterLocalTransform> &transforms, std::uint32_t &bone_count,
    float &ground_offset, std::uint32_t &material_count,
    bool validate_upper_body_mask = true)
{
    std::vector<std::byte> bytes;
    if (auto read = ReadBinary(path, bytes); !read)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               std::format("Missing character asset: {}", path.string()));
    }
    if (bytes.size() < sizeof(CharacterAssetHeader))
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Character asset header is truncated.");
    }
    CharacterAssetHeader header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    const auto expected_clips_offset =
        sizeof(header) + header.vertex_count * sizeof(SkinnedVertex);
    const auto expected_parents_offset =
        expected_clips_offset + header.clip_count * sizeof(CharacterClipHeader);
    const auto expected_inverse_bind_matrices_offset =
        expected_parents_offset + header.bone_count * sizeof(std::uint16_t);
    const auto expected_upper_body_weights_offset =
        expected_inverse_bind_matrices_offset +
        header.bone_count * sizeof(inverse_bind_matrices.front());
    const auto expected_transforms_offset =
        expected_upper_body_weights_offset + header.bone_count * sizeof(float);
    if (header.magic !=
            std::array<char, 8>{'H', 'S', 'C', 'H', 'A', 'R', '1', '\0'} ||
        header.version != kCharacterAssetVersion || header.vertex_count == 0 ||
        header.bone_count == 0 || header.bone_count > kMaxCharacterBones ||
        header.material_count == 0 ||
        header.material_count > kMaxCharacterMaterials ||
        header.clip_count != static_cast<std::uint32_t>(CharacterAnimationClip::Count) ||
        header.vertices_offset != sizeof(header) ||
        header.clips_offset != expected_clips_offset ||
        header.parents_offset != expected_parents_offset ||
        header.inverse_bind_matrices_offset != expected_inverse_bind_matrices_offset ||
        header.upper_body_weights_offset != expected_upper_body_weights_offset ||
        header.transforms_offset != expected_transforms_offset ||
        !std::isfinite(header.bounds_min[1]) ||
        !std::isfinite(header.bounds_max[1]) ||
        header.bounds_min[1] >= header.bounds_max[1] ||
        sizeof(header) + header.payload_size != bytes.size() ||
        Crc32(std::span(bytes.data() + sizeof(header), header.payload_size)) !=
            header.payload_crc32)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Character asset layout or checksum is invalid.");
    }

    vertices.resize(header.vertex_count);
    std::memcpy(vertices.data(), bytes.data() + header.vertices_offset,
                vertices.size() * sizeof(vertices.front()));
    for (const auto &vertex : vertices)
    {
        float weight_sum{};
        for (std::size_t influence = 0; influence < vertex.bone_indices.size(); ++influence)
        {
            if (vertex.bone_indices[influence] >= header.bone_count ||
                !std::isfinite(vertex.bone_weights[influence]) ||
                vertex.bone_weights[influence] < 0.0f)
            {
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                       "Character vertex skin influence is invalid.");
            }
            weight_sum += vertex.bone_weights[influence];
        }
        if (!std::isfinite(weight_sum) || std::abs(weight_sum - 1.0f) > 0.001f)
        {
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Character vertex skin weights are not normalized.");
        }
    }
    clips.resize(header.clip_count);
    std::memcpy(clips.data(), bytes.data() + header.clips_offset,
                clips.size() * sizeof(clips.front()));
    parents.resize(header.bone_count);
    std::memcpy(parents.data(), bytes.data() + header.parents_offset,
                parents.size() * sizeof(parents.front()));
    inverse_bind_matrices.resize(header.bone_count);
    std::memcpy(inverse_bind_matrices.data(),
                bytes.data() + header.inverse_bind_matrices_offset,
                inverse_bind_matrices.size() * sizeof(inverse_bind_matrices.front()));
    for (std::size_t bone_index = 0; bone_index < parents.size(); ++bone_index)
    {
        const auto parent = parents[bone_index];
        if ((parent != std::numeric_limits<std::uint16_t>::max() &&
             parent >= bone_index) ||
            !std::ranges::all_of(inverse_bind_matrices[bone_index], [](float value) {
                return std::isfinite(value);
            }))
        {
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Character skeleton hierarchy is invalid.");
        }
    }
    upper_body_weights.resize(header.bone_count);
    std::memcpy(upper_body_weights.data(), bytes.data() + header.upper_body_weights_offset,
                upper_body_weights.size() * sizeof(float));
    if (!std::ranges::all_of(upper_body_weights, [](float value) {
            return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
        }) ||
        (validate_upper_body_mask &&
         (!std::ranges::any_of(upper_body_weights, [](float value) { return value > 0.0f; }) ||
          !std::ranges::any_of(upper_body_weights, [](float value) { return value == 0.0f; }))))
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Character upper-body mask is invalid.");
    }
    const auto transform_bytes = bytes.size() - header.transforms_offset;
    if (transform_bytes % sizeof(transforms.front()) != 0)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Character animation transforms are misaligned.");
    }
    transforms.resize(transform_bytes / sizeof(transforms.front()));
    std::memcpy(transforms.data(), bytes.data() + header.transforms_offset,
                transform_bytes);
    for (const auto &transform : transforms)
    {
        const auto rotation_length = std::sqrt(std::inner_product(
            transform.rotation.begin(), transform.rotation.end(),
            transform.rotation.begin(), 0.0f));
        if (!std::ranges::all_of(transform.translation, [](float value) {
                return std::isfinite(value);
            }) ||
            !std::ranges::all_of(transform.scale, [](float value) {
                return std::isfinite(value) && std::abs(value) > 0.0001f;
            }) ||
            !std::isfinite(rotation_length) || rotation_length < 0.999f ||
            rotation_length > 1.001f)
        {
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Character animation contains an invalid local pose.");
        }
    }
    std::array<bool, static_cast<std::size_t>(CharacterAnimationClip::Count)> found{};
    for (const auto &clip : clips)
    {
        const auto clip_index = static_cast<std::size_t>(clip.clip);
        const auto transform_count =
            static_cast<std::uint64_t>(clip.frame_count) * header.bone_count;
        if (clip_index >= found.size() || std::exchange(found[clip_index], true) ||
            clip.frame_count < 2 || !(clip.duration_seconds > 0.0f) ||
            static_cast<std::uint64_t>(clip.first_transform) + transform_count >
                transforms.size())
        {
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Character animation clip table is invalid.");
        }
    }
    bone_count = header.bone_count;
    material_count = header.material_count;
    ground_offset = -header.bounds_min[1];
    return Result::Success();
}

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

inline std::filesystem::file_time_type LatestShaderWrite()
{
    std::filesystem::file_time_type latest{};
    std::error_code error;
    const auto directory = CurrentExecutableDirectory() / "Shaders";
    for (const auto &entry : std::filesystem::directory_iterator(directory, error))
    {
        if (error) break;
        if (!entry.is_regular_file(error) || entry.path().extension() != ".dxil")
            continue;
        const auto write = entry.last_write_time(error);
        if (!error) latest = std::max(latest, write);
    }
    return latest;
}

} // namespace renderer_detail

using namespace renderer_detail;

struct D3D12Renderer::Impl
{
    [[nodiscard]] Result CreateDevice(const RendererConfig &configuration);
    [[nodiscard]] Result CreateSwapChainAndTargets();
    [[nodiscard]] Result CreateDepthAndShadow();
    [[nodiscard]] Result CreatePostProcessTargets();
    [[nodiscard]] Result CreatePipeline();
    [[nodiscard]] Result ReloadPipeline();
    [[nodiscard]] Result CreateGpuData();
    [[nodiscard]] Result CreateCharacterTextures();
    [[nodiscard]] Result CreateEnvironmentTextures();
    [[nodiscard]] Result CreateEnvironmentMeshes();
    [[nodiscard]] Result CreateUiTexture();
    [[nodiscard]] Result RasterizeUi(std::uint32_t frame_index,
                                     std::span<const UiModel> models);
    [[nodiscard]] Result CreateAllocation(AllocationResource &output,
                                          const D3D12MA::ALLOCATION_DESC &allocation,
                                          const D3D12_RESOURCE_DESC &resource,
                                          D3D12_RESOURCE_STATES initial_state,
                                          const D3D12_CLEAR_VALUE *clear = nullptr);
    [[nodiscard]] Result WaitForFrame(FrameContext &frame);
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
    ComPtr<IDXGIFactory7> factory;
    ComPtr<IDXGIAdapter4> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain4> swap_chain;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    ComPtr<ID3D12DescriptorHeap> dsv_heap;
    ComPtr<ID3D12DescriptorHeap> srv_heap;
#if defined(HS_DEVELOPMENT_TOOLS)
    ComPtr<ID3D12DescriptorHeap> imgui_heap;
#endif
    ComPtr<ID3D12GraphicsCommandList> command_list;
    ComPtr<ID3D12GraphicsCommandList7> enhanced_command_list;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event{};
    std::uint64_t next_fence{1};
    D3D12MA::Allocator *allocator{};
    std::array<FrameContext, kFrameCount> frames;
    std::array<ComPtr<ID3D12Resource>, kFrameCount> back_buffers;
    AllocationResource depth;
    AllocationResource shadow;
    AllocationResource vertices;
    AllocationResource archer_vertices;
    AllocationResource gel_projectile_vertices;
    D3D12_VERTEX_BUFFER_VIEW gel_projectile_vertex_view{};
    std::uint32_t gel_projectile_vertex_count{};
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
    Tick last_status_visual_tick{std::numeric_limits<Tick>::max()};
    AllocationResource particles;
    std::array<AllocationResource, 2> particle_alive;
    AllocationResource particle_dead;
    AllocationResource particle_counters;
    AllocationResource indirect_arguments;
    AllocationResource gbuffer_base;
    AllocationResource gbuffer_normal;
    AllocationResource gbuffer_position;
    AllocationResource gbuffer_material;
    std::array<AllocationResource, kEnvironmentTextureCount> environment_textures;
    struct EnvironmentMeshGpu
    {
        AllocationResource vertices;
        D3D12_VERTEX_BUFFER_VIEW vertex_view{};
        UINT vertex_count{};
    };
    std::array<EnvironmentMeshGpu, 42> environment_meshes;
    AllocationResource hdr_color;
    AllocationResource oit_accumulation;
    AllocationResource oit_revealage;
    AllocationResource post_a;
    AllocationResource post_b;
    std::array<AllocationResource, kFrameCount> ui_textures;
    std::array<UiCpuSurface, kFrameCount> ui_surfaces;
    ComPtr<IWICImagingFactory> ui_wic_factory;
    ComPtr<ID2D1Factory> ui_d2d_factory;
    ComPtr<IDWriteFactory5> ui_dwrite_factory;
    ComPtr<IDWriteFontCollection1> ui_font_collection;
    std::wstring ui_font_family;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT ui_footprint{};
    std::uint32_t ui_rows{};
    AllocationResource timestamp_readback;
    std::uint64_t *mapped_timestamps{};
    ComPtr<ID3D12QueryHeap> timestamp_heap;
    std::uint64_t timestamp_frequency{};
    ComPtr<ID3D12RootSignature> root_signature;
    ComPtr<ID3D12PipelineState> scene_pipeline;
    ComPtr<ID3D12PipelineState> shadow_pipeline;
    ComPtr<ID3D12PipelineState> particle_pipeline;
    ComPtr<ID3D12PipelineState> slime_pipeline;
    ComPtr<ID3D12PipelineState> particle_compute_pipeline;
    ComPtr<ID3D12PipelineState> deferred_pipeline;
    ComPtr<ID3D12PipelineState> composite_pipeline;
    ComPtr<ID3D12PipelineState> bloom_pipeline;
    ComPtr<ID3D12PipelineState> tone_map_pipeline;
    ComPtr<ID3D12PipelineState> outline_pipeline;
    ComPtr<ID3D12PipelineState> fxaa_pipeline;
    ComPtr<ID3D12PipelineState> ui_pipeline;
    ComPtr<ID3D12CommandSignature> draw_signature;
    D3D12_VERTEX_BUFFER_VIEW vertex_view{};
    D3D12_VERTEX_BUFFER_VIEW archer_vertex_view{};
    std::vector<CharacterClipHeader> archer_clips;
    std::vector<std::uint16_t> archer_parents;
    std::vector<std::array<float, 16>> archer_inverse_bind_matrices;
    std::vector<CharacterLocalTransform> archer_transforms;
    std::vector<float> archer_upper_body_weights;
    std::vector<SkinnedVertex> archer_support_vertices;
    std::uint32_t archer_vertex_count{};
    std::uint32_t archer_bone_count{};
    std::uint32_t archer_material_count{};
    float archer_ground_offset{};
    RenderGraphBuilder graph;
    UINT rtv_stride{};
    UINT dsv_stride{};
    UINT srv_stride{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t render_width{};
    std::uint32_t render_height{};
    std::uint32_t last_presented_index{};
    std::uint64_t frame_number{};
    std::uint64_t validation_errors{};
    std::uint32_t last_particle_count{};
    Tick observed_snapshot_tick{};
    std::chrono::steady_clock::time_point snapshot_arrival{};
    bool enhanced{};
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
    std::filesystem::file_time_type shader_write{};
    std::chrono::steady_clock::time_point next_shader_check{};
#endif
    Tick last_particle_tick{};
};

} // namespace hs
