#include <hs/renderer/renderer.hpp>

#include <hs/renderer/render_graph.hpp>

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
namespace
{

using Microsoft::WRL::ComPtr;

constexpr std::uint32_t kFrameCount = 3;
constexpr std::uint32_t kParticleCount = 10'000;
constexpr std::uint32_t kUiWidth = 1'920;
constexpr std::uint32_t kUiHeight = 1'080;
constexpr std::uint32_t kPostTextureDescriptorCount = 10;
constexpr std::uint32_t kCharacterDescriptorCount = 3;
constexpr std::uint32_t kCharacterTextureSize = 2'048;
constexpr std::uint32_t kTextureDescriptorCount =
    kPostTextureDescriptorCount + kCharacterDescriptorCount;
constexpr std::uint32_t kInstanceDataOffset = 12 * 1024;
constexpr std::uint32_t kFrameUploadSize = 576 * 1024;
constexpr std::uint32_t kTimestampCountPerFrame =
    static_cast<std::uint32_t>(kStage1RenderPassCount * 2);
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
constexpr std::array<const char *, 12> kDebugRelicNames{
    "피의 회복", "번지는 불꽃", "한기 폭발", "피와 불", "팔방 사격", "추격 본능",
    "잔상 사격", "연계 숙련", "교차 사격", "충격 반격", "위기 회복", "경험의 파동"};
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

struct FrameConstants
{
    DirectX::XMFLOAT4X4 view_projection;
    DirectX::XMFLOAT4 camera_time;
    DirectX::XMFLOAT4 light_direction_intensity;
    DirectX::XMFLOAT4 light_color;
    DirectX::XMFLOAT4 screen_size;
    DirectX::XMFLOAT4 camera_forward_softness;
    DirectX::XMFLOAT4X4 shadow_view_projection[3];
    DirectX::XMFLOAT4X4 archer_bones[kMaxCharacterBones];
    DirectX::XMFLOAT4 render_options;
    DirectX::XMUINT4 particle_options;
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


struct AllocationResource
{
    ComPtr<ID3D12Resource> resource;
    D3D12MA::Allocation *allocation{};

    AllocationResource() = default;
    ~AllocationResource()
    {
        Reset();
    }
    AllocationResource(const AllocationResource &) = delete;
    AllocationResource &operator=(const AllocationResource &) = delete;
    AllocationResource(AllocationResource &&other) noexcept
        : resource(std::move(other.resource)), allocation(std::exchange(other.allocation, nullptr))
    {
    }
    AllocationResource &operator=(AllocationResource &&other) noexcept
    {
        if (this != &other)
        {
            Reset();
            resource = std::move(other.resource);
            allocation = std::exchange(other.allocation, nullptr);
        }
        return *this;
    }

    void Reset() noexcept
    {
        resource.Reset();
        if (allocation)
        {
            allocation->Release();
            allocation = nullptr;
        }
    }
};

struct FrameContext
{
    ComPtr<ID3D12CommandAllocator> allocator;
    AllocationResource upload;
    AllocationResource ui_upload;
    std::byte *mapped{};
    std::size_t upload_size{kFrameUploadSize};
    std::byte *ui_mapped{};
    std::uint64_t fence_value{};
    bool timestamps_recorded{};
    bool ui_initialized{};
};

struct UiCpuSurface
{
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1RenderTarget> target;
    ComPtr<ID2D1SolidColorBrush> brush;
};

[[nodiscard]] Result HResultFailure(std::string_view operation, HRESULT result)
{
    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                           std::format("{} failed: 0x{:08X}", operation,
                                       static_cast<std::uint32_t>(result)));
}

[[nodiscard]] D3D12_RESOURCE_DESC BufferDescription(std::uint64_t size,
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

[[nodiscard]] D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

[[nodiscard]] std::filesystem::path ExecutableDirectory()
{
    std::array<wchar_t, 32'768> path{};
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    return std::filesystem::path(std::wstring_view(path.data(), length)).parent_path();
}

[[nodiscard]] std::string Utf8(std::wstring_view text)
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

[[nodiscard]] Result ReadBinary(const std::filesystem::path &path,
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

struct DdsPixelFormat
{
    std::uint32_t size;
    std::uint32_t flags;
    std::uint32_t four_cc;
    std::uint32_t rgb_bit_count;
    std::uint32_t red_mask;
    std::uint32_t green_mask;
    std::uint32_t blue_mask;
    std::uint32_t alpha_mask;
};

struct DdsHeader
{
    std::uint32_t size;
    std::uint32_t flags;
    std::uint32_t height;
    std::uint32_t width;
    std::uint32_t pitch;
    std::uint32_t depth;
    std::uint32_t mip_count;
    std::array<std::uint32_t, 11> reserved;
    DdsPixelFormat pixel_format;
    std::uint32_t caps;
    std::array<std::uint32_t, 4> remaining_caps;
};

struct DdsHeaderDx10
{
    DXGI_FORMAT format;
    D3D12_RESOURCE_DIMENSION dimension;
    std::uint32_t misc_flag;
    std::uint32_t array_size;
    std::uint32_t misc_flags2;
};

[[nodiscard]] Result LoadVfxMaskDds(const std::filesystem::path &path,
                                    DdsHeader &header,
                                    std::uint32_t &sprite_count,
                                    std::span<const std::byte> &pixels,
                                    std::vector<std::byte> &storage)
{
    if (auto loaded = ReadBinary(path, storage); !loaded) return loaded;
    constexpr std::uint32_t dds_magic = 0x20534444;
    constexpr std::uint32_t dx10 = 0x30315844;
    if (storage.size() < sizeof(dds_magic) + sizeof(header) + sizeof(DdsHeaderDx10))
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "VFX mask DDS header is truncated.");
    std::uint32_t magic{};
    DdsHeaderDx10 extension{};
    std::memcpy(&magic, storage.data(), sizeof(magic));
    std::memcpy(&header, storage.data() + sizeof(magic), sizeof(header));
    std::memcpy(&extension, storage.data() + sizeof(magic) + sizeof(header),
                sizeof(extension));
    if (magic != dds_magic || header.size != 124 || header.pixel_format.size != 32 ||
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

[[nodiscard]] Result LoadRgbaDds(const std::filesystem::path &path,
                                 std::uint32_t &width, std::uint32_t &height,
                                 std::span<const std::byte> &pixels,
                                 std::vector<std::byte> &storage)
{
    if (auto loaded = ReadBinary(path, storage); !loaded)
    {
        return loaded;
    }
    constexpr std::uint32_t dds_magic = 0x20534444;
    if (storage.size() < sizeof(dds_magic) + sizeof(DdsHeader))
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Character DDS header is truncated.");
    }
    std::uint32_t magic{};
    DdsHeader header{};
    std::memcpy(&magic, storage.data(), sizeof(magic));
    std::memcpy(&header, storage.data() + sizeof(magic), sizeof(header));
    const auto pixel_bytes = static_cast<std::uint64_t>(header.width) * header.height * 4;
    if (magic != dds_magic || header.size != 124 || header.pixel_format.size != 32 ||
        header.pixel_format.rgb_bit_count != 32 || header.width == 0 ||
        header.height == 0 || header.pitch != header.width * 4 ||
        sizeof(magic) + sizeof(header) + pixel_bytes != storage.size())
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Character DDS layout is invalid.");
    }
    width = header.width;
    height = header.height;
    pixels = std::span(storage).subspan(sizeof(magic) + sizeof(header));
    return Result::Success();
}

[[nodiscard]] Result LoadCharacterAsset(
    const std::filesystem::path &path, std::vector<SkinnedVertex> &vertices,
    std::vector<CharacterClipHeader> &clips,
    std::vector<std::uint16_t> &parents,
    std::vector<std::array<float, 16>> &inverse_bind_matrices,
    std::vector<float> &upper_body_weights,
    std::vector<CharacterLocalTransform> &transforms, std::uint32_t &bone_count,
    float &ground_offset, std::uint32_t &material_count)
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
    if (!std::ranges::any_of(upper_body_weights, [](float value) { return value > 0.0f; }) ||
        !std::ranges::any_of(upper_body_weights, [](float value) { return value == 0.0f; }) ||
        !std::ranges::all_of(upper_body_weights, [](float value) {
            return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
        }))
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

std::filesystem::file_time_type LatestShaderWrite()
{
    std::filesystem::file_time_type latest{};
    std::error_code error;
    const auto directory = ExecutableDirectory() / "Shaders";
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

} // namespace

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
    AllocationResource archer_diffuse;
    AllocationResource archer_normal;
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

Result D3D12Renderer::Impl::CreateAllocation(AllocationResource &output,
                                            const D3D12MA::ALLOCATION_DESC &allocation,
                                            const D3D12_RESOURCE_DESC &resource,
                                            D3D12_RESOURCE_STATES initial_state,
                                            const D3D12_CLEAR_VALUE *clear)
{
    const auto effective_state =
        enhanced && resource.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER
            ? D3D12_RESOURCE_STATE_COMMON
            : initial_state;
    const auto result = allocator->CreateResource(&allocation, &resource, effective_state, clear,
                                                  &output.allocation,
                                                  IID_PPV_ARGS(output.resource.ReleaseAndGetAddressOf()));
    return SUCCEEDED(result) ? Result::Success() : HResultFailure("D3D12MA::CreateResource", result);
}

Result D3D12Renderer::Impl::CreateDevice(const RendererConfig &configuration)
{
    config = configuration;
    width = std::max(configuration.width, 1u);
    height = std::max(configuration.height, 1u);
    const auto render_scale = std::clamp(configuration.render_scale_percent, 75u, 100u);
    render_width = std::max(width * render_scale / 100u, 1u);
    render_height = std::max(height * render_scale / 100u, 1u);

    const auto com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    com_initialized = SUCCEEDED(com_result);
    if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE)
    {
        return HResultFailure("CoInitializeEx", com_result);
    }

    if (configuration.validation)
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        {
            debug->EnableDebugLayer();
            if (configuration.gpu_validation)
            {
                ComPtr<ID3D12Debug1> debug1;
                if (SUCCEEDED(debug.As(&debug1)))
                {
                    debug1->SetEnableGPUBasedValidation(TRUE);
                }
            }
        }
    }

    ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> dred_settings;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred_settings))))
    {
        dred_settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred_settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred_settings->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred_enabled = true;
    }
    std::ofstream(config.artifact_directory / "dred_config.json", std::ios::trunc)
        << std::format(
               "{{\"enabled\":{},\"auto_breadcrumbs\":{},\"page_fault\":{},"
               "\"breadcrumb_context\":{}}}\n",
               dred_enabled ? "true" : "false", dred_enabled ? "true" : "false",
               dred_enabled ? "true" : "false", dred_enabled ? "true" : "false");

    const auto factory_flags = configuration.validation ? DXGI_CREATE_FACTORY_DEBUG : 0u;
    auto result = CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory));
    if (FAILED(result))
    {
        return HResultFailure("CreateDXGIFactory2", result);
    }

    if (configuration.warp)
    {
        ComPtr<IDXGIAdapter> warp;
        result = factory->EnumWarpAdapter(IID_PPV_ARGS(&warp));
        if (SUCCEEDED(result))
        {
            result = warp.As(&adapter);
        }
    }
    else
    {
        for (UINT index = 0;
             factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                 IID_PPV_ARGS(&adapter)) !=
             DXGI_ERROR_NOT_FOUND;
             ++index)
        {
            DXGI_ADAPTER_DESC3 description{};
            adapter->GetDesc3(&description);
            if (!(description.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE) &&
                SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                            __uuidof(ID3D12Device), nullptr)))
            {
                break;
            }
            adapter.Reset();
        }
        result = adapter ? S_OK : DXGI_ERROR_NOT_FOUND;
    }
    if (FAILED(result) || !adapter)
    {
        return HResultFailure("SelectAdapter", result);
    }

    DXGI_ADAPTER_DESC3 adapter_description{};
    adapter->GetDesc3(&adapter_description);
    LARGE_INTEGER driver_version{};
    const auto has_driver = SUCCEEDED(
        adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver_version));
    const auto driver = static_cast<std::uint64_t>(driver_version.QuadPart);
    std::ofstream(config.artifact_directory / "adapter.txt", std::ios::trunc)
        << Utf8(adapter_description.Description) << '\n'
        << (has_driver
                ? std::format("{}.{}.{}.{}", (driver >> 48) & 0xFFFF,
                              (driver >> 32) & 0xFFFF, (driver >> 16) & 0xFFFF,
                              driver & 0xFFFF)
                : std::string("unknown"))
        << '\n';

    result = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
    if (FAILED(result))
    {
        return HResultFailure("D3D12CreateDevice FL12_0", result);
    }

    if (configuration.validation)
    {
        ComPtr<ID3D12InfoQueue> info_queue;
        if (SUCCEEDED(device.As(&info_queue)))
        {
            info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
            info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
        }
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12{};
    const auto supports_enhanced =
        SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12,
                                              sizeof(options12))) &&
        options12.EnhancedBarriersSupported;
    if (configuration.barrier_mode == BarrierMode::Enhanced && !supports_enhanced)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Enhanced barriers requested but unsupported.");
    }
    enhanced = supports_enhanced && configuration.barrier_mode != BarrierMode::Legacy;

    D3D12_COMMAND_QUEUE_DESC queue_description{};
    queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    result = device->CreateCommandQueue(&queue_description, IID_PPV_ARGS(&queue));
    if (FAILED(result))
    {
        return HResultFailure("CreateCommandQueue", result);
    }

    D3D12MA::ALLOCATOR_DESC allocator_description{};
    allocator_description.pDevice = device.Get();
    allocator_description.pAdapter = adapter.Get();
    result = D3D12MA::CreateAllocator(&allocator_description, &allocator);
    if (FAILED(result))
    {
        return HResultFailure("D3D12MA::CreateAllocator", result);
    }

    for (auto &frame : frames)
    {
        result =
            device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.allocator));
        if (FAILED(result))
        {
            return HResultFailure("CreateCommandAllocator", result);
        }

        D3D12MA::ALLOCATION_DESC upload_allocation{};
        upload_allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        const auto upload_description = BufferDescription(kFrameUploadSize);
        if (auto created = CreateAllocation(frame.upload, upload_allocation, upload_description,
                                            D3D12_RESOURCE_STATE_GENERIC_READ);
            !created)
        {
            return created;
        }
        D3D12_RANGE no_read{};
        result = frame.upload.resource->Map(0, &no_read, reinterpret_cast<void **>(&frame.mapped));
        if (FAILED(result))
        {
            return HResultFailure("Map frame upload", result);
        }
    }

    result = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                       frames[0].allocator.Get(), nullptr,
                                       IID_PPV_ARGS(&command_list));
    if (FAILED(result))
    {
        return HResultFailure("CreateCommandList", result);
    }
    if (enhanced && FAILED(command_list.As(&enhanced_command_list)))
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Enhanced barrier command list interface unavailable.");
    }
    command_list->Close();

    result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(result))
    {
        return HResultFailure("CreateFence", result);
    }
    fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "CreateEventW failed.");
    }

    D3D12_QUERY_HEAP_DESC query_description{};
    query_description.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    query_description.Count = kFrameCount * kTimestampCountPerFrame;
    result = device->CreateQueryHeap(&query_description, IID_PPV_ARGS(&timestamp_heap));
    if (FAILED(result))
    {
        return HResultFailure("Create timestamp query heap", result);
    }
    D3D12MA::ALLOCATION_DESC readback_allocation{};
    readback_allocation.HeapType = D3D12_HEAP_TYPE_READBACK;
    if (auto created = CreateAllocation(
            timestamp_readback, readback_allocation,
            BufferDescription(static_cast<std::uint64_t>(query_description.Count) *
                              sizeof(std::uint64_t)),
            D3D12_RESOURCE_STATE_COPY_DEST);
        !created)
    {
        return created;
    }
    D3D12_RANGE read_range{
        0, static_cast<SIZE_T>(query_description.Count) * sizeof(std::uint64_t)};
    result = timestamp_readback.resource->Map(
        0, &read_range, reinterpret_cast<void **>(&mapped_timestamps));
    if (FAILED(result))
    {
        return HResultFailure("Map timestamp readback", result);
    }
    result = queue->GetTimestampFrequency(&timestamp_frequency);
    if (FAILED(result) || timestamp_frequency == 0)
    {
        return HResultFailure("GetTimestampFrequency", FAILED(result) ? result : E_FAIL);
    }
    return Result::Success();
}

Result D3D12Renderer::Impl::CreateSwapChainAndTargets()
{
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = width;
    description.Height = height;
    description.Format = kBackBufferFormat;
    description.SampleDesc = {1, 0};
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = kFrameCount;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    ComPtr<IDXGISwapChain1> swap_chain1;
    auto result = factory->CreateSwapChainForHwnd(
        queue.Get(), static_cast<HWND>(config.window), &description, nullptr, nullptr, &swap_chain1);
    if (FAILED(result))
    {
        return HResultFailure("CreateSwapChainForHwnd", result);
    }
    result = swap_chain1.As(&swap_chain);
    if (FAILED(result))
    {
        return HResultFailure("Query IDXGISwapChain4", result);
    }
    factory->MakeWindowAssociation(static_cast<HWND>(config.window), DXGI_MWA_NO_ALT_ENTER);

    D3D12_DESCRIPTOR_HEAP_DESC rtv_description{};
    rtv_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_description.NumDescriptors = 11;
    result = device->CreateDescriptorHeap(&rtv_description, IID_PPV_ARGS(&rtv_heap));
    if (FAILED(result))
    {
        return HResultFailure("Create RTV heap", result);
    }
    rtv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    auto handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (std::uint32_t index = 0; index < kFrameCount; ++index)
    {
        result = swap_chain->GetBuffer(index, IID_PPV_ARGS(&back_buffers[index]));
        if (FAILED(result))
        {
            return HResultFailure("Get swap-chain buffer", result);
        }
        device->CreateRenderTargetView(back_buffers[index].Get(), nullptr, handle);
        handle.ptr += rtv_stride;
    }
    return Result::Success();
}

Result D3D12Renderer::Impl::CreatePostProcessTargets()
{
    transient_textures_common = enhanced;
    gbuffer_base.Reset();
    gbuffer_normal.Reset();
    gbuffer_position.Reset();
    hdr_color.Reset();
    oit_accumulation.Reset();
    oit_revealage.Reset();
    post_a.Reset();
    post_b.Reset();

    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    auto create_target = [&](AllocationResource &output, DXGI_FORMAT format,
                             const float *clear_color) -> Result {
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = render_width;
        description.Height = render_height;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.Format = format;
        description.SampleDesc = {1, 0};
        description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = format;
        std::copy_n(clear_color, 4, clear.Color);
        return CreateAllocation(output, allocation, description,
                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear);
    };

    constexpr float black[4] = {0, 0, 0, 0};
    constexpr float normal_clear[4] = {0.5f, 1.0f, 0.5f, 0};
    constexpr float reveal_clear[4] = {1, 1, 1, 1};
    for (auto [resource, format, clear] : {
             std::tuple{&gbuffer_base, DXGI_FORMAT_R8G8B8A8_UNORM, black},
             std::tuple{&gbuffer_normal, DXGI_FORMAT_R16G16B16A16_FLOAT, normal_clear},
             std::tuple{&gbuffer_position, DXGI_FORMAT_R16G16B16A16_FLOAT, black},
             std::tuple{&hdr_color, DXGI_FORMAT_R16G16B16A16_FLOAT, black},
             std::tuple{&oit_accumulation, DXGI_FORMAT_R16G16B16A16_FLOAT, black},
             std::tuple{&oit_revealage, DXGI_FORMAT_R16_FLOAT, reveal_clear},
             std::tuple{&post_a, DXGI_FORMAT_R16G16B16A16_FLOAT, black},
             std::tuple{&post_b, DXGI_FORMAT_R16G16B16A16_FLOAT, black},
         })
    {
        if (auto created = create_target(*resource, format, clear); !created)
        {
            return created;
        }
    }

    auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(kFrameCount) * rtv_stride;
    for (auto [resource, format] : {
             std::pair{gbuffer_base.resource.Get(), DXGI_FORMAT_R8G8B8A8_UNORM},
             std::pair{gbuffer_normal.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT},
             std::pair{gbuffer_position.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT},
             std::pair{hdr_color.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT},
             std::pair{oit_accumulation.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT},
             std::pair{oit_revealage.resource.Get(), DXGI_FORMAT_R16_FLOAT},
             std::pair{post_a.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT},
             std::pair{post_b.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT},
         })
    {
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = format;
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(resource, &view, rtv);
        rtv.ptr += rtv_stride;
    }

    if (!srv_heap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC description{};
        description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        description.NumDescriptors = kTextureDescriptorCount * kFrameCount;
        description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        const auto result = device->CreateDescriptorHeap(&description, IID_PPV_ARGS(&srv_heap));
        if (FAILED(result))
        {
            return HResultFailure("Create SRV heap", result);
        }
        srv_stride =
            device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    for (std::uint32_t frame_index = 0; frame_index < kFrameCount; ++frame_index)
    {
        auto srv = srv_heap->GetCPUDescriptorHandleForHeapStart();
        srv.ptr += static_cast<SIZE_T>(frame_index) * kTextureDescriptorCount * srv_stride;
        auto create_srv = [&](ID3D12Resource *resource, DXGI_FORMAT format) {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = format;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(resource, &view, srv);
            srv.ptr += srv_stride;
        };
        create_srv(gbuffer_base.resource.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
        create_srv(gbuffer_normal.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
        create_srv(gbuffer_position.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
        D3D12_SHADER_RESOURCE_VIEW_DESC shadow_view{};
        shadow_view.Format = DXGI_FORMAT_R32_FLOAT;
        shadow_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        shadow_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        shadow_view.Texture2DArray.MipLevels = 1;
        shadow_view.Texture2DArray.ArraySize = 3;
        device->CreateShaderResourceView(shadow.resource.Get(), &shadow_view, srv);
        srv.ptr += srv_stride;
        create_srv(hdr_color.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
        create_srv(oit_accumulation.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
        create_srv(oit_revealage.resource.Get(), DXGI_FORMAT_R16_FLOAT);
        create_srv(post_a.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
        create_srv(post_b.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
        create_srv(ui_textures[frame_index].resource.Get(), DXGI_FORMAT_B8G8R8A8_UNORM);
    }
    return Result::Success();
}

Result D3D12Renderer::Impl::CreateDepthAndShadow()
{
    transient_textures_common = enhanced;
    D3D12_DESCRIPTOR_HEAP_DESC dsv_description{};
    dsv_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsv_description.NumDescriptors = 4;
    auto result = device->CreateDescriptorHeap(&dsv_description, IID_PPV_ARGS(&dsv_heap));
    if (FAILED(result))
    {
        return HResultFailure("Create DSV heap", result);
    }
    dsv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    D3D12_CLEAR_VALUE clear{};
    clear.Format = kDepthFormat;
    clear.DepthStencil = {0.0f, 0};
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC depth_description{};
    depth_description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depth_description.Width = render_width;
    depth_description.Height = render_height;
    depth_description.DepthOrArraySize = 1;
    depth_description.MipLevels = 1;
    depth_description.Format = kDepthFormat;
    depth_description.SampleDesc = {1, 0};
    depth_description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depth_description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    if (auto created = CreateAllocation(depth, allocation, depth_description,
                                        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear);
        !created)
    {
        return created;
    }
    device->CreateDepthStencilView(depth.resource.Get(), nullptr,
                                   dsv_heap->GetCPUDescriptorHandleForHeapStart());

    auto shadow_description = depth_description;
    shadow_description.Format = DXGI_FORMAT_R32_TYPELESS;
    shadow_description.Width = std::clamp(config.shadow_resolution, 1024u, 2048u);
    shadow_description.Height = static_cast<UINT>(shadow_description.Width);
    shadow_description.DepthOrArraySize = 3;
    if (auto created = CreateAllocation(shadow, allocation, shadow_description,
                                        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear);
        !created)
    {
        return created;
    }
    auto shadow_handle = dsv_heap->GetCPUDescriptorHandleForHeapStart();
    shadow_handle.ptr += dsv_stride;
    for (std::uint32_t cascade = 0; cascade < 3; ++cascade)
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC view{};
        view.Format = kDepthFormat;
        view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.ArraySize = 1;
        view.Texture2DArray.FirstArraySlice = cascade;
        device->CreateDepthStencilView(shadow.resource.Get(), &view, shadow_handle);
        shadow_handle.ptr += dsv_stride;
    }
    return Result::Success();
}

Result D3D12Renderer::Impl::CreatePipeline()
{
    D3D12_DESCRIPTOR_RANGE1 texture_range{};
    texture_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    texture_range.NumDescriptors = kPostTextureDescriptorCount;
    texture_range.BaseShaderRegister = 2;
    texture_range.RegisterSpace = 0;
    texture_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE |
                          D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

    D3D12_DESCRIPTOR_RANGE1 character_range{};
    character_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    character_range.NumDescriptors = kCharacterDescriptorCount;
    character_range.BaseShaderRegister = 14;
    character_range.RegisterSpace = 0;
    character_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;

    std::array<D3D12_ROOT_PARAMETER1, 15> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[0].Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[1].Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[2].Descriptor = {1, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[3].Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
    parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[4].Descriptor = {1, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
    parameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[5].DescriptorTable = {1, &texture_range};
    parameters[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[6].Constants = {1, 0, 1};
    parameters[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    for (std::uint32_t index = 0; index < 4; ++index)
    {
        auto &parameter = parameters[7 + index];
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameter.Descriptor = {2 + index, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    parameters[11].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[11].Descriptor = {12, 0,
                                 D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
    parameters[11].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[12].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[12].Descriptor = {13, 0,
                                 D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
    parameters[12].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[13].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[13].DescriptorTable = {1, &character_range};
    parameters[13].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[14].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[14].Descriptor = {17, 0,
                                 D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
    parameters[14].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    std::array<D3D12_STATIC_SAMPLER_DESC, 3> samplers{};
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[0].ShaderRegister = 0;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    samplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[1].ShaderRegister = 1;
    samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samplers[2] = samplers[0];
    samplers[2].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[2].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[2].ShaderRegister = 2;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_description{};
    root_description.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    root_description.Desc_1_1.NumParameters = static_cast<UINT>(parameters.size());
    root_description.Desc_1_1.pParameters = parameters.data();
    root_description.Desc_1_1.NumStaticSamplers = static_cast<UINT>(samplers.size());
    root_description.Desc_1_1.pStaticSamplers = samplers.data();
    root_description.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> root_blob;
    ComPtr<ID3DBlob> error_blob;
    auto result = D3D12SerializeVersionedRootSignature(&root_description, &root_blob, &error_blob);
    if (FAILED(result))
    {
        const auto message = error_blob
                                 ? std::string(static_cast<const char *>(error_blob->GetBufferPointer()),
                                               error_blob->GetBufferSize())
                                 : "unknown root signature error";
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", message);
    }
    result = device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                         IID_PPV_ARGS(&root_signature));
    if (FAILED(result))
    {
        return HResultFailure("CreateRootSignature", result);
    }

    const auto shader_directory = ExecutableDirectory() / "Shaders";
    std::vector<std::byte> scene_vertex;
    std::vector<std::byte> scene_pixel;
    std::vector<std::byte> shadow_vertex;
    std::vector<std::byte> particle_vertex;
    std::vector<std::byte> particle_pixel;
    std::vector<std::byte> particle_compute;
    std::vector<std::byte> full_screen_vertex;
    std::vector<std::byte> deferred_pixel;
    std::vector<std::byte> composite_pixel;
    std::vector<std::byte> bloom_pixel;
    std::vector<std::byte> tone_map_pixel;
    std::vector<std::byte> outline_pixel;
    std::vector<std::byte> fxaa_pixel;
    std::vector<std::byte> ui_pixel;
    for (auto [name, bytes] : {
             std::pair{"scene_vs.dxil", &scene_vertex},
             std::pair{"scene_ps.dxil", &scene_pixel},
             std::pair{"shadow_vs.dxil", &shadow_vertex},
             std::pair{"particle_vs.dxil", &particle_vertex},
             std::pair{"particle_ps.dxil", &particle_pixel},
             std::pair{"particle_cs.dxil", &particle_compute},
             std::pair{"fullscreen_vs.dxil", &full_screen_vertex},
             std::pair{"deferred_ps.dxil", &deferred_pixel},
             std::pair{"composite_ps.dxil", &composite_pixel},
             std::pair{"bloom_ps.dxil", &bloom_pixel},
             std::pair{"tonemap_ps.dxil", &tone_map_pixel},
             std::pair{"outline_ps.dxil", &outline_pixel},
             std::pair{"fxaa_ps.dxil", &fxaa_pixel},
             std::pair{"ui_ps.dxil", &ui_pixel},
         })
    {
        if (auto read = ReadBinary(shader_directory / name, *bytes); !read)
        {
            return read;
        }
    }

    constexpr D3D12_INPUT_ELEMENT_DESC input_layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
         0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BLENDINDICES", 0, DXGI_FORMAT_R16G16B16A16_UINT, 0, 24,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 48,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 56,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"MATERIAL", 0, DXGI_FORMAT_R16_UINT, 0, 72,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC scene{};
    scene.pRootSignature = root_signature.Get();
    scene.VS = {scene_vertex.data(), scene_vertex.size()};
    scene.PS = {scene_pixel.data(), scene_pixel.size()};
    scene.BlendState.AlphaToCoverageEnable = FALSE;
    scene.BlendState.IndependentBlendEnable = FALSE;
    scene.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    scene.SampleMask = UINT_MAX;
    scene.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    scene.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    scene.RasterizerState.FrontCounterClockwise = FALSE;
    scene.RasterizerState.DepthClipEnable = TRUE;
    scene.DepthStencilState.DepthEnable = TRUE;
    scene.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    scene.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    scene.InputLayout = {input_layout, static_cast<UINT>(std::size(input_layout))};
    scene.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    scene.NumRenderTargets = 3;
    scene.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    scene.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    scene.RTVFormats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    scene.DSVFormat = kDepthFormat;
    scene.SampleDesc = {1, 0};
    result = device->CreateGraphicsPipelineState(&scene, IID_PPV_ARGS(&scene_pipeline));
    if (FAILED(result))
    {
        return HResultFailure("Create scene PSO", result);
    }

    auto shadow_state = scene;
    shadow_state.VS = {shadow_vertex.data(), shadow_vertex.size()};
    shadow_state.PS = {};
    shadow_state.NumRenderTargets = 0;
    std::fill(std::begin(shadow_state.RTVFormats), std::end(shadow_state.RTVFormats),
              DXGI_FORMAT_UNKNOWN);
    shadow_state.RasterizerState.DepthBias = 800;
    shadow_state.RasterizerState.SlopeScaledDepthBias = 1.5f;
    result = device->CreateGraphicsPipelineState(&shadow_state, IID_PPV_ARGS(&shadow_pipeline));
    if (FAILED(result))
    {
        return HResultFailure("Create shadow PSO", result);
    }

    auto particle = scene;
    particle.VS = {particle_vertex.data(), particle_vertex.size()};
    particle.PS = {particle_pixel.data(), particle_pixel.size()};
    particle.InputLayout = {};
    particle.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    particle.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    particle.BlendState.RenderTarget[0].BlendEnable = TRUE;
    particle.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    particle.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
    particle.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    particle.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    particle.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
    particle.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    particle.BlendState.IndependentBlendEnable = TRUE;
    particle.BlendState.RenderTarget[1].BlendEnable = TRUE;
    particle.BlendState.RenderTarget[1].SrcBlend = D3D12_BLEND_ZERO;
    particle.BlendState.RenderTarget[1].DestBlend = D3D12_BLEND_INV_SRC_COLOR;
    particle.BlendState.RenderTarget[1].BlendOp = D3D12_BLEND_OP_ADD;
    particle.BlendState.RenderTarget[1].SrcBlendAlpha = D3D12_BLEND_ZERO;
    particle.BlendState.RenderTarget[1].DestBlendAlpha = D3D12_BLEND_ONE;
    particle.BlendState.RenderTarget[1].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    particle.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    particle.NumRenderTargets = 2;
    particle.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    particle.RTVFormats[1] = DXGI_FORMAT_R16_FLOAT;
    particle.RTVFormats[2] = DXGI_FORMAT_UNKNOWN;
    result = device->CreateGraphicsPipelineState(&particle, IID_PPV_ARGS(&particle_pipeline));
    if (FAILED(result))
    {
        return HResultFailure("Create particle PSO", result);
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};
    compute.pRootSignature = root_signature.Get();
    compute.CS = {particle_compute.data(), particle_compute.size()};
    result =
        device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&particle_compute_pipeline));
    if (FAILED(result))
    {
        return HResultFailure("Create particle compute PSO", result);
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC post{};
    post.pRootSignature = root_signature.Get();
    post.VS = {full_screen_vertex.data(), full_screen_vertex.size()};
    post.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    post.SampleMask = UINT_MAX;
    post.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    post.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    post.RasterizerState.DepthClipEnable = TRUE;
    post.DepthStencilState.DepthEnable = FALSE;
    post.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    post.NumRenderTargets = 1;
    post.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    post.SampleDesc = {1, 0};
    auto create_post_pipeline = [&](std::span<const std::byte> pixel_shader,
                                    ID3D12PipelineState **pipeline) -> Result {
        post.PS = {pixel_shader.data(), pixel_shader.size()};
        const auto create_result = device->CreateGraphicsPipelineState(
            &post, IID_PPV_ARGS(pipeline));
        return SUCCEEDED(create_result) ? Result::Success()
                                        : HResultFailure("Create post-process PSO", create_result);
    };
    for (auto [shader, pipeline] :
         std::array{
             std::pair{std::span<const std::byte>(deferred_pixel),
                       deferred_pipeline.ReleaseAndGetAddressOf()},
             std::pair{std::span<const std::byte>(composite_pixel),
                       composite_pipeline.ReleaseAndGetAddressOf()},
             std::pair{std::span<const std::byte>(bloom_pixel),
                       bloom_pipeline.ReleaseAndGetAddressOf()},
             std::pair{std::span<const std::byte>(tone_map_pixel),
                       tone_map_pipeline.ReleaseAndGetAddressOf()},
             std::pair{std::span<const std::byte>(outline_pixel),
                       outline_pipeline.ReleaseAndGetAddressOf()},
         })
    {
        if (auto pipeline_result = create_post_pipeline(shader, pipeline); !pipeline_result)
        {
            return pipeline_result;
        }
    }

    post.RTVFormats[0] = kBackBufferFormat;
    if (auto pipeline_result =
            create_post_pipeline(fxaa_pixel, fxaa_pipeline.ReleaseAndGetAddressOf());
        !pipeline_result)
    {
        return pipeline_result;
    }
    post.BlendState.RenderTarget[0].BlendEnable = TRUE;
    post.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    post.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    post.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    post.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    post.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    post.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    if (auto pipeline_result =
            create_post_pipeline(ui_pixel, ui_pipeline.ReleaseAndGetAddressOf());
        !pipeline_result)
    {
        return pipeline_result;
    }

    D3D12_INDIRECT_ARGUMENT_DESC argument{};
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC signature{};
    signature.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
    signature.NumArgumentDescs = 1;
    signature.pArgumentDescs = &argument;
    result = device->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&draw_signature));
    return SUCCEEDED(result) ? Result::Success()
                             : HResultFailure("CreateCommandSignature", result);
}

Result D3D12Renderer::Impl::ReloadPipeline()
{
    if (auto waited = WaitForGpu(); !waited)
        return waited;
    auto previous_root = std::move(root_signature);
    auto previous_scene = std::move(scene_pipeline);
    auto previous_shadow = std::move(shadow_pipeline);
    auto previous_particle = std::move(particle_pipeline);
    auto previous_particle_compute = std::move(particle_compute_pipeline);
    auto previous_deferred = std::move(deferred_pipeline);
    auto previous_composite = std::move(composite_pipeline);
    auto previous_bloom = std::move(bloom_pipeline);
    auto previous_tone_map = std::move(tone_map_pipeline);
    auto previous_outline = std::move(outline_pipeline);
    auto previous_fxaa = std::move(fxaa_pipeline);
    auto previous_ui = std::move(ui_pipeline);
    auto previous_signature = std::move(draw_signature);

    auto result = CreatePipeline();
    if (!result)
    {
        root_signature = std::move(previous_root);
        scene_pipeline = std::move(previous_scene);
        shadow_pipeline = std::move(previous_shadow);
        particle_pipeline = std::move(previous_particle);
        particle_compute_pipeline = std::move(previous_particle_compute);
        deferred_pipeline = std::move(previous_deferred);
        composite_pipeline = std::move(previous_composite);
        bloom_pipeline = std::move(previous_bloom);
        tone_map_pipeline = std::move(previous_tone_map);
        outline_pipeline = std::move(previous_outline);
        fxaa_pipeline = std::move(previous_fxaa);
        ui_pipeline = std::move(previous_ui);
        draw_signature = std::move(previous_signature);
    }
    return result;
}

Result D3D12Renderer::Impl::CreateGpuData()
{
    D3D12MA::ALLOCATION_DESC upload_allocation{};
    upload_allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    const auto vertex_description = BufferDescription(sizeof(kCubeVertices));
    if (auto created = CreateAllocation(vertices, upload_allocation, vertex_description,
                                        D3D12_RESOURCE_STATE_GENERIC_READ);
        !created)
    {
        return created;
    }
    std::byte *mapped{};
    D3D12_RANGE no_read{};
    auto result = vertices.resource->Map(0, &no_read, reinterpret_cast<void **>(&mapped));
    if (FAILED(result))
    {
        return HResultFailure("Map vertex buffer", result);
    }
    std::memcpy(mapped, kCubeVertices.data(), sizeof(kCubeVertices));
    vertices.resource->Unmap(0, nullptr);
    vertex_view = {vertices.resource->GetGPUVirtualAddress(), sizeof(kCubeVertices),
                   sizeof(Vertex)};

    std::vector<SkinnedVertex> cooked_vertices;
    if (auto loaded = LoadCharacterAsset(
            ExecutableDirectory() / "Cooked" / "stage1_archer.meshbin",
            cooked_vertices, archer_clips, archer_parents,
            archer_inverse_bind_matrices, archer_upper_body_weights,
            archer_transforms, archer_bone_count,
            archer_ground_offset, archer_material_count);
        !loaded)
    {
        return loaded;
    }
    archer_vertex_count = static_cast<std::uint32_t>(cooked_vertices.size());
    const auto archer_vertex_bytes = cooked_vertices.size() * sizeof(cooked_vertices.front());
    const auto archer_description = BufferDescription(archer_vertex_bytes);
    if (auto created = CreateAllocation(archer_vertices, upload_allocation,
                                        archer_description,
                                        D3D12_RESOURCE_STATE_GENERIC_READ);
        !created)
    {
        return created;
    }
    mapped = nullptr;
    result = archer_vertices.resource->Map(0, &no_read,
                                           reinterpret_cast<void **>(&mapped));
    if (FAILED(result))
    {
        return HResultFailure("Map archer vertex buffer", result);
    }
    std::memcpy(mapped, cooked_vertices.data(), archer_vertex_bytes);
    archer_vertices.resource->Unmap(0, nullptr);
    archer_vertex_view = {archer_vertices.resource->GetGPUVirtualAddress(),
                          static_cast<UINT>(archer_vertex_bytes),
                          sizeof(SkinnedVertex)};

    D3D12MA::ALLOCATION_DESC default_allocation{};
    default_allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    const auto particle_description =
        BufferDescription(kParticleCount * sizeof(GpuParticle),
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (auto created = CreateAllocation(particles, default_allocation, particle_description,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        !created)
    {
        return created;
    }
    const auto particle_index_description =
        BufferDescription(kParticleCount * sizeof(std::uint32_t),
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    for (auto &alive : particle_alive)
    {
        if (auto created = CreateAllocation(alive, default_allocation,
                                            particle_index_description,
                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            !created)
        {
            return created;
        }
    }
    if (auto created = CreateAllocation(particle_dead, default_allocation,
                                        particle_index_description,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        !created)
    {
        return created;
    }
    if (auto created = CreateAllocation(
            particle_counters, default_allocation,
            BufferDescription(16, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        !created)
    {
        return created;
    }
    const auto indirect_description =
        BufferDescription(sizeof(D3D12_DRAW_ARGUMENTS),
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    return CreateAllocation(indirect_arguments, default_allocation, indirect_description,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

Result D3D12Renderer::Impl::CreateCharacterTextures()
{
    auto result = frames[0].allocator->Reset();
    if (FAILED(result))
    {
        return HResultFailure("Reset character texture allocator", result);
    }
    result = command_list->Reset(frames[0].allocator.Get(), nullptr);
    if (FAILED(result))
    {
        return HResultFailure("Reset character texture command list", result);
    }

    std::vector<AllocationResource> uploads;
    uploads.reserve(static_cast<std::size_t>(archer_material_count) * 2);
    const auto cooked = ExecutableDirectory() / "Cooked";
    auto upload_array = [&](AllocationResource &texture,
                            std::wstring_view prefix) -> Result {
        D3D12_RESOURCE_DESC texture_description{};
        texture_description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture_description.Width = kCharacterTextureSize;
        texture_description.Height = kCharacterTextureSize;
        texture_description.DepthOrArraySize =
            static_cast<std::uint16_t>(archer_material_count);
        texture_description.MipLevels = 1;
        texture_description.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        texture_description.SampleDesc = {1, 0};
        texture_description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        D3D12MA::ALLOCATION_DESC default_allocation{};
        default_allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        if (auto created = CreateAllocation(texture, default_allocation,
                                            texture_description,
                                            D3D12_RESOURCE_STATE_COPY_DEST);
            !created)
        {
            return created;
        }

        for (std::uint32_t index = 0; index < archer_material_count; ++index)
        {
            std::vector<std::byte> storage;
            std::span<const std::byte> pixels;
            std::uint32_t source_width{};
            std::uint32_t source_height{};
            const auto path = cooked / (std::wstring(prefix) + std::to_wstring(index) +
                                        L".dds");
            if (auto loaded = LoadRgbaDds(path, source_width, source_height, pixels,
                                          storage);
                !loaded)
            {
                return loaded;
            }
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            UINT rows{};
            UINT64 row_size{};
            UINT64 upload_size{};
            device->GetCopyableFootprints(&texture_description, index, 1, 0,
                                          &footprint, &rows, &row_size,
                                          &upload_size);
            uploads.emplace_back();
            D3D12MA::ALLOCATION_DESC upload_allocation{};
            upload_allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
            if (auto created = CreateAllocation(uploads.back(), upload_allocation,
                                                BufferDescription(upload_size),
                                                D3D12_RESOURCE_STATE_GENERIC_READ);
                !created)
            {
                return created;
            }
            std::byte *mapped{};
            D3D12_RANGE no_read{};
            result = uploads.back().resource->Map(
                0, &no_read, reinterpret_cast<void **>(&mapped));
            if (FAILED(result))
            {
                return HResultFailure("Map character texture upload", result);
            }
            for (std::uint32_t row = 0; row < kCharacterTextureSize; ++row)
            {
                auto *destination = mapped + footprint.Offset +
                                    static_cast<std::size_t>(row) *
                                        footprint.Footprint.RowPitch;
                const auto source_row = static_cast<std::uint64_t>(row) *
                                        source_height / kCharacterTextureSize;
                for (std::uint32_t column = 0; column < kCharacterTextureSize;
                     ++column)
                {
                    const auto source_column =
                        static_cast<std::uint64_t>(column) * source_width /
                        kCharacterTextureSize;
                    std::memcpy(destination + static_cast<std::size_t>(column) * 4,
                                pixels.data() +
                                    (source_row * source_width + source_column) * 4,
                                4);
                }
            }
            uploads.back().resource->Unmap(0, nullptr);

            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = texture.resource.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = index;
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = uploads.back().resource.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = footprint;
            command_list->CopyTextureRegion(&destination, 0, 0, 0, &source,
                                            nullptr);
        }
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture.resource.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        command_list->ResourceBarrier(1, &barrier);
        return Result::Success();
    };

    if (auto uploaded = upload_array(archer_diffuse, L"archer_diffuse_");
        !uploaded)
    {
        return uploaded;
    }
    if (auto uploaded = upload_array(archer_normal, L"archer_normal_"); !uploaded)
    {
        return uploaded;
    }
    {
        DdsHeader header{};
        std::vector<std::byte> storage;
        std::span<const std::byte> pixels;
        if (auto loaded = LoadVfxMaskDds(cooked / "vfx_masks.dds", header,
                                         vfx_sprite_count,
                                         pixels, storage);
            !loaded)
            return loaded;
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = header.width;
        description.Height = header.height;
        description.DepthOrArraySize = static_cast<UINT16>(vfx_sprite_count);
        description.MipLevels = static_cast<std::uint16_t>(header.mip_count);
        description.Format = DXGI_FORMAT_BC4_UNORM;
        description.SampleDesc = {1, 0};
        D3D12MA::ALLOCATION_DESC default_allocation{};
        default_allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        if (auto created = CreateAllocation(vfx_masks, default_allocation,
                                            description,
                                            D3D12_RESOURCE_STATE_COPY_DEST);
            !created)
            return created;

        const auto subresource_count = vfx_sprite_count * header.mip_count;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresource_count);
        std::vector<UINT> rows(subresource_count);
        std::vector<UINT64> row_sizes(subresource_count);
        UINT64 upload_size{};
        device->GetCopyableFootprints(&description, 0, subresource_count, 0,
                                      footprints.data(), rows.data(),
                                      row_sizes.data(), &upload_size);
        uploads.emplace_back();
        D3D12MA::ALLOCATION_DESC upload_allocation{};
        upload_allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        if (auto created = CreateAllocation(uploads.back(), upload_allocation,
                                            BufferDescription(upload_size),
                                            D3D12_RESOURCE_STATE_GENERIC_READ);
            !created)
            return created;
        std::byte *mapped{};
        D3D12_RANGE no_read{};
        result = uploads.back().resource->Map(
            0, &no_read, reinterpret_cast<void **>(&mapped));
        if (FAILED(result)) return HResultFailure("Map VFX mask upload", result);
        std::size_t source_offset{};
        for (std::uint32_t subresource = 0; subresource < subresource_count;
             ++subresource)
        {
            for (std::uint32_t row = 0; row < rows[subresource]; ++row)
            {
                std::memcpy(mapped + footprints[subresource].Offset +
                                static_cast<std::size_t>(row) *
                                    footprints[subresource].Footprint.RowPitch,
                            pixels.data() + source_offset,
                            static_cast<std::size_t>(row_sizes[subresource]));
                source_offset += static_cast<std::size_t>(row_sizes[subresource]);
            }
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = vfx_masks.resource.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = subresource;
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = uploads.back().resource.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = footprints[subresource];
            command_list->CopyTextureRegion(&destination, 0, 0, 0, &source,
                                            nullptr);
        }
        uploads.back().resource->Unmap(0, nullptr);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = vfx_masks.resource.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        command_list->ResourceBarrier(1, &barrier);
    }
    result = command_list->Close();
    if (FAILED(result))
    {
        return HResultFailure("Close character texture upload", result);
    }
    ID3D12CommandList *lists[] = {command_list.Get()};
    queue->ExecuteCommandLists(1, lists);
    if (auto waited = WaitForGpu(); !waited)
    {
        return waited;
    }

    for (std::uint32_t frame_index = 0; frame_index < kFrameCount; ++frame_index)
    {
        auto handle = srv_heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += (static_cast<SIZE_T>(frame_index) * kTextureDescriptorCount +
                       kPostTextureDescriptorCount) * srv_stride;
        auto create_view = [&](ID3D12Resource *texture, DXGI_FORMAT format) {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = format;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2DArray.MipLevels = 1;
            view.Texture2DArray.ArraySize = archer_material_count;
            device->CreateShaderResourceView(texture, &view, handle);
            handle.ptr += srv_stride;
        };
        create_view(archer_diffuse.resource.Get(),
                    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
        create_view(archer_normal.resource.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
        D3D12_SHADER_RESOURCE_VIEW_DESC vfx_view{};
        vfx_view.Format = DXGI_FORMAT_BC4_UNORM;
        vfx_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        vfx_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        vfx_view.Texture2DArray.MipLevels = 10;
        vfx_view.Texture2DArray.ArraySize = vfx_sprite_count;
        device->CreateShaderResourceView(vfx_masks.resource.Get(), &vfx_view, handle);
    }
    return Result::Success();
}

Result D3D12Renderer::Impl::CreateUiTexture()
{
    D3D12_RESOURCE_DESC texture_description{};
    texture_description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture_description.Width = kUiWidth;
    texture_description.Height = kUiHeight;
    texture_description.DepthOrArraySize = 1;
    texture_description.MipLevels = 1;
    texture_description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texture_description.SampleDesc = {1, 0};
    texture_description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12MA::ALLOCATION_DESC default_allocation{};
    default_allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    UINT64 row_size{};
    UINT64 total_size{};
    device->GetCopyableFootprints(&texture_description, 0, 1, 0, &ui_footprint, &ui_rows,
                                  &row_size, &total_size);
    D3D12MA::ALLOCATION_DESC upload_allocation{};
    upload_allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    for (std::size_t frame_index = 0; frame_index < frames.size(); ++frame_index)
    {
        if (auto created = CreateAllocation(ui_textures[frame_index], default_allocation,
                                            texture_description, D3D12_RESOURCE_STATE_COMMON);
            !created)
            return created;
        auto &frame = frames[frame_index];
        if (auto created = CreateAllocation(frame.ui_upload, upload_allocation,
                                            BufferDescription(total_size),
                                            D3D12_RESOURCE_STATE_GENERIC_READ);
            !created)
            return created;
        D3D12_RANGE no_read{};
        const auto map_result = frame.ui_upload.resource->Map(
            0, &no_read, reinterpret_cast<void **>(&frame.ui_mapped));
        if (FAILED(map_result)) return HResultFailure("Map UI upload", map_result);
    }

    auto result = CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&ui_wic_factory));
    if (FAILED(result)) return HResultFailure("Create WIC UI factory", result);
    result = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                               ui_d2d_factory.ReleaseAndGetAddressOf());
    if (FAILED(result)) return HResultFailure("Create Direct2D UI factory", result);

    ComPtr<IDWriteFactory> base_dwrite_factory;
    result = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown **>(base_dwrite_factory.ReleaseAndGetAddressOf()));
    if (FAILED(result) || FAILED(base_dwrite_factory.As(&ui_dwrite_factory)))
        return HResultFailure("Create DirectWrite UI factory", FAILED(result) ? result : E_FAIL);

    const auto font_path = ExecutableDirectory() / L"Fonts" / L"NotoSansKR.ttf";
    ComPtr<IDWriteFontFile> font_file;
    ComPtr<IDWriteFontSetBuilder1> font_builder;
    ComPtr<IDWriteFontSet> font_set;
    result = ui_dwrite_factory->CreateFontFileReference(font_path.c_str(), nullptr, &font_file);
    if (SUCCEEDED(result)) result = ui_dwrite_factory->CreateFontSetBuilder(&font_builder);
    if (SUCCEEDED(result)) result = font_builder->AddFontFile(font_file.Get());
    if (SUCCEEDED(result)) result = font_builder->CreateFontSet(&font_set);
    if (SUCCEEDED(result))
        result = ui_dwrite_factory->CreateFontCollectionFromFontSet(font_set.Get(),
                                                                    &ui_font_collection);
    if (FAILED(result))
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Bundled Noto Sans KR font is missing or invalid.");

    ComPtr<IDWriteFontFamily> font_family;
    ComPtr<IDWriteLocalizedStrings> family_names;
    if (ui_font_collection->GetFontFamilyCount() == 0 ||
        FAILED(ui_font_collection->GetFontFamily(0, &font_family)) ||
        FAILED(font_family->GetFamilyNames(&family_names)))
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Bundled Noto Sans KR font has no family name.");
    UINT32 family_name_index{};
    BOOL family_name_exists{};
    family_names->FindLocaleName(L"ko-kr", &family_name_index, &family_name_exists);
    if (!family_name_exists) family_name_index = 0;
    UINT32 family_name_length{};
    family_names->GetStringLength(family_name_index, &family_name_length);
    ui_font_family.resize(family_name_length + 1);
    family_names->GetString(family_name_index, ui_font_family.data(), family_name_length + 1);
    ui_font_family.resize(family_name_length);

    D2D1_RENDER_TARGET_PROPERTIES properties{};
    properties.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;
    properties.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED};
    properties.dpiX = properties.dpiY = 96.0f;
    properties.minLevel = D2D1_FEATURE_LEVEL_DEFAULT;
    for (auto &surface : ui_surfaces)
    {
        result = ui_wic_factory->CreateBitmap(kUiWidth, kUiHeight,
                                              GUID_WICPixelFormat32bppPBGRA,
                                              WICBitmapCacheOnLoad, &surface.bitmap);
        if (SUCCEEDED(result))
            result = ui_d2d_factory->CreateWicBitmapRenderTarget(
                surface.bitmap.Get(), properties, &surface.target);
        if (SUCCEEDED(result))
            result = surface.target->CreateSolidColorBrush(
                D2D1_COLOR_F{1, 1, 1, 1}, &surface.brush);
        if (FAILED(result)) return HResultFailure("Create DirectWrite UI surface", result);
        surface.target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    return Result::Success();
}

Result D3D12Renderer::Impl::RasterizeUi(std::uint32_t frame_index,
                                        std::span<const UiModel> models)
{
    auto &surface = ui_surfaces[frame_index];
    surface.target->BeginDraw();
    surface.target->SetTransform(D2D1_MATRIX_3X2_F{1, 0, 0, 1, 0, 0});
    surface.target->Clear(D2D1_COLOR_F{0, 0, 0, 0});

    const auto color_of = [](std::uint32_t packed) {
        constexpr float inverse_byte = 1.0f / 255.0f;
        return D2D1_COLOR_F{static_cast<float>(packed & 0xff) * inverse_byte,
                            static_cast<float>((packed >> 8) & 0xff) * inverse_byte,
                            static_cast<float>((packed >> 16) & 0xff) * inverse_byte,
                            static_cast<float>((packed >> 24) & 0xff) * inverse_byte};
    };
    for (const auto &model : models.first(std::min<std::size_t>(models.size(), 32)))
    {
        const auto element_width = std::max(model.size_pixels.x, 0.0f);
        const auto element_height = std::max(model.size_pixels.y, 0.0f);
        const D2D1_RECT_F rectangle{model.anchor_pixels.x, model.anchor_pixels.y,
                                    model.anchor_pixels.x + element_width,
                                    model.anchor_pixels.y + element_height};
        surface.brush->SetColor(color_of(model.color_rgba));
        switch (model.kind)
        {
        case UiModel::Kind::Panel:
            if (element_width > 0 && element_height > 0)
                surface.target->FillRectangle(rectangle, surface.brush.Get());
            break;
        case UiModel::Kind::Button:
            if (element_width > 0 && element_height > 0)
            {
                const D2D1_ROUNDED_RECT rounded{rectangle, 6.0f, 6.0f};
                surface.target->FillRoundedRectangle(rounded, surface.brush.Get());
            }
            break;
        case UiModel::Kind::Bar:
            if (element_width > 0 && element_height > 0)
            {
                surface.brush->SetColor(D2D1_COLOR_F{0.02f, 0.025f, 0.035f, 0.72f});
                surface.target->FillRectangle(rectangle, surface.brush.Get());
                auto filled = rectangle;
                filled.right = filled.left +
                               element_width * std::clamp(model.value, 0.0f, 1.0f);
                surface.brush->SetColor(color_of(model.color_rgba));
                surface.target->FillRectangle(filled, surface.brush.Get());
            }
            break;
        case UiModel::Kind::Text: break;
        }

        const auto end = std::find(model.utf8_text.begin(), model.utf8_text.end(), '\0');
        const auto byte_count = static_cast<int>(end - model.utf8_text.begin());
        if (byte_count == 0) continue;
        const auto wide_count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                                     model.utf8_text.data(), byte_count,
                                                     nullptr, 0);
        if (wide_count <= 0) continue;
        std::wstring text(static_cast<std::size_t>(wide_count), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, model.utf8_text.data(),
                            byte_count, text.data(), wide_count);
        ComPtr<IDWriteTextFormat> format;
        const auto font_size = static_cast<float>(
            std::clamp<std::uint16_t>(model.font_pixels, 8, 128));
        const auto format_result = ui_dwrite_factory->CreateTextFormat(
            ui_font_family.c_str(), ui_font_collection.Get(), DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, font_size, L"ko-kr",
            &format);
        if (FAILED(format_result))
            return HResultFailure("Create UI text format", format_result);
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        const D2D1_RECT_F text_rectangle{
            model.anchor_pixels.x, model.anchor_pixels.y,
            model.anchor_pixels.x +
                (element_width > 0 ? element_width : kUiWidth - model.anchor_pixels.x),
            model.anchor_pixels.y +
                (element_height > 0 ? element_height : font_size * 1.6f)};
        surface.brush->SetColor(D2D1_COLOR_F{1, 1, 1, 1});
        surface.target->DrawText(text.data(), static_cast<UINT32>(text.size()), format.Get(),
                                 text_rectangle, surface.brush.Get(),
                                 D2D1_DRAW_TEXT_OPTIONS_CLIP,
                                 DWRITE_MEASURING_MODE_NATURAL);
    }

    const auto draw_result = surface.target->EndDraw();
    if (FAILED(draw_result)) return HResultFailure("Rasterize UI", draw_result);

    WICRect lock_rectangle{0, 0, static_cast<INT>(kUiWidth), static_cast<INT>(kUiHeight)};
    ComPtr<IWICBitmapLock> bitmap_lock;
    auto result = surface.bitmap->Lock(&lock_rectangle, WICBitmapLockRead, &bitmap_lock);
    UINT stride{};
    UINT byte_count{};
    BYTE *pixels{};
    if (SUCCEEDED(result)) result = bitmap_lock->GetStride(&stride);
    if (SUCCEEDED(result)) result = bitmap_lock->GetDataPointer(&byte_count, &pixels);
    if (FAILED(result)) return HResultFailure("Lock UI pixels", result);
    auto &frame = frames[frame_index];
    for (std::uint32_t row = 0; row < ui_rows; ++row)
    {
        std::memcpy(frame.ui_mapped + ui_footprint.Offset + row * ui_footprint.Footprint.RowPitch,
                    pixels + static_cast<std::size_t>(row) * stride, kUiWidth * 4);
    }
    return Result::Success();
}

Result D3D12Renderer::Impl::WaitForFrame(FrameContext &frame)
{
    if (frame.fence_value && fence->GetCompletedValue() < frame.fence_value)
    {
        auto result = fence->SetEventOnCompletion(frame.fence_value, fence_event);
        if (FAILED(result))
        {
            return HResultFailure("SetEventOnCompletion", result);
        }
        WaitForSingleObject(fence_event, INFINITE);
    }
    return Result::Success();
}

Result D3D12Renderer::Impl::WaitForGpu()
{
    const auto value = next_fence++;
    auto result = queue->Signal(fence.Get(), value);
    if (FAILED(result))
    {
        return CheckDevice(result, "Queue signal");
    }
    if (fence->GetCompletedValue() < value)
    {
        result = fence->SetEventOnCompletion(value, fence_event);
        if (FAILED(result))
        {
            return HResultFailure("SetEventOnCompletion", result);
        }
        WaitForSingleObject(fence_event, INFINITE);
    }
    return Result::Success();
}

Result D3D12Renderer::Impl::CheckDevice(HRESULT result, std::string_view operation)
{
    if (SUCCEEDED(result))
    {
        return Result::Success();
    }
    const auto reason = device ? device->GetDeviceRemovedReason() : result;
    if (reason == DXGI_ERROR_DEVICE_REMOVED || reason == DXGI_ERROR_DEVICE_RESET ||
        result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET)
    {
        WriteDred(reason);
    }
    return HResultFailure(operation, result);
}

void D3D12Renderer::Impl::TransitionTexture(ID3D12Resource *resource,
                                            D3D12_RESOURCE_STATES before,
                                            D3D12_RESOURCE_STATES after,
                                            D3D12_BARRIER_LAYOUT before_layout,
                                            D3D12_BARRIER_LAYOUT after_layout)
{
    if (enhanced)
    {
        D3D12_TEXTURE_BARRIER barrier{};
        barrier.SyncBefore = before == D3D12_RESOURCE_STATE_PRESENT ? D3D12_BARRIER_SYNC_NONE
                                                                    : D3D12_BARRIER_SYNC_ALL;
        barrier.SyncAfter = after == D3D12_RESOURCE_STATE_PRESENT ? D3D12_BARRIER_SYNC_NONE
                                                                  : D3D12_BARRIER_SYNC_ALL;
        barrier.AccessBefore =
            before == D3D12_RESOURCE_STATE_PRESENT
                ? D3D12_BARRIER_ACCESS_NO_ACCESS
                : (before == D3D12_RESOURCE_STATE_RENDER_TARGET
                       ? D3D12_BARRIER_ACCESS_RENDER_TARGET
                       : (before == D3D12_RESOURCE_STATE_COPY_SOURCE
                              ? D3D12_BARRIER_ACCESS_COPY_SOURCE
                              : (before == D3D12_RESOURCE_STATE_COPY_DEST
                                     ? D3D12_BARRIER_ACCESS_COPY_DEST
                                     : (before == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                                            ? D3D12_BARRIER_ACCESS_SHADER_RESOURCE
                                            : D3D12_BARRIER_ACCESS_COMMON))));
        barrier.AccessAfter =
            after == D3D12_RESOURCE_STATE_PRESENT
                ? D3D12_BARRIER_ACCESS_NO_ACCESS
                : (after == D3D12_RESOURCE_STATE_RENDER_TARGET
                       ? D3D12_BARRIER_ACCESS_RENDER_TARGET
                       : (after == D3D12_RESOURCE_STATE_COPY_SOURCE
                              ? D3D12_BARRIER_ACCESS_COPY_SOURCE
                              : (after == D3D12_RESOURCE_STATE_COPY_DEST
                                     ? D3D12_BARRIER_ACCESS_COPY_DEST
                                     : (after == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                                            ? D3D12_BARRIER_ACCESS_SHADER_RESOURCE
                                            : D3D12_BARRIER_ACCESS_COMMON))));
        barrier.LayoutBefore = before_layout;
        barrier.LayoutAfter = after_layout;
        barrier.pResource = resource;
        const auto description = resource->GetDesc();
        barrier.Subresources = {0, description.MipLevels, 0, description.DepthOrArraySize, 0, 1};
        D3D12_BARRIER_GROUP group{D3D12_BARRIER_TYPE_TEXTURE, 1};
        group.pTextureBarriers = &barrier;
        enhanced_command_list->Barrier(1, &group);
    }
    else
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
        command_list->ResourceBarrier(1, &barrier);
    }
}

void D3D12Renderer::Impl::TransitionBuffer(ID3D12Resource *resource,
                                           D3D12_RESOURCE_STATES before,
                                           D3D12_RESOURCE_STATES after,
                                           D3D12_BARRIER_SYNC before_sync,
                                           D3D12_BARRIER_SYNC after_sync,
                                           D3D12_BARRIER_ACCESS before_access,
                                           D3D12_BARRIER_ACCESS after_access)
{
    if (enhanced)
    {
        D3D12_BUFFER_BARRIER barrier{};
        barrier.SyncBefore = before_sync;
        barrier.SyncAfter = after_sync;
        barrier.AccessBefore = before_access;
        barrier.AccessAfter = after_access;
        barrier.pResource = resource;
        barrier.Offset = 0;
        barrier.Size = UINT64_MAX;
        D3D12_BARRIER_GROUP group{D3D12_BARRIER_TYPE_BUFFER, 1};
        group.pBufferBarriers = &barrier;
        enhanced_command_list->Barrier(1, &group);
    }
    else
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
        command_list->ResourceBarrier(1, &barrier);
    }
}

void D3D12Renderer::Impl::CountValidationErrors()
{
    if (!config.validation || !device)
    {
        return;
    }
    ComPtr<ID3D12InfoQueue> info_queue;
    if (FAILED(device.As(&info_queue)))
    {
        return;
    }
    const auto count = info_queue->GetNumStoredMessages();
    std::ofstream log(config.artifact_directory / "d3d12_validation.log", std::ios::app);
    for (std::uint64_t index = 0; index < count; ++index)
    {
        SIZE_T size{};
        info_queue->GetMessage(index, nullptr, &size);
        std::vector<std::byte> storage(size);
        auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
        if (SUCCEEDED(info_queue->GetMessage(index, message, &size)) &&
            (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
             message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION))
        {
            ++validation_errors;
            log << "id=" << message->ID << " severity=" << message->Severity << ' '
                << message->pDescription << '\n';
        }
    }
    info_queue->ClearStoredMessages();
}

void D3D12Renderer::Impl::WriteDred(HRESULT reason) const
{
    std::error_code error;
    std::filesystem::create_directories(config.artifact_directory, error);
    std::ofstream stream(config.artifact_directory / "dred.txt", std::ios::trunc);
    stream << std::format("device_removed_reason=0x{:08X}\n",
                          static_cast<std::uint32_t>(reason));
    ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
    if (device && SUCCEEDED(device.As(&dred)))
    {
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs{};
        D3D12_DRED_PAGE_FAULT_OUTPUT1 page_fault{};
        if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&breadcrumbs)))
        {
            stream << "breadcrumbs=" << (breadcrumbs.pHeadAutoBreadcrumbNode ? "present" : "none")
                   << '\n';
        }
        if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&page_fault)))
        {
            stream << "page_fault_va=" << page_fault.PageFaultVA << '\n';
        }
    }
}

D3D12Renderer::D3D12Renderer() : impl_(std::make_unique<Impl>())
{
}

D3D12Renderer::~D3D12Renderer()
{
    (void)Shutdown();
}

Result D3D12Renderer::Initialize(const RendererConfig &config)
{
    if (impl_->initialized)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Renderer already initialized.");
    }
    if (!config.window)
    {
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Renderer requires HWND.");
    }
    if (auto result = impl_->CreateDevice(config); !result)
    {
        return result;
    }
    if (auto result = impl_->CreateSwapChainAndTargets(); !result)
    {
        return result;
    }
    if (auto result = impl_->CreateDepthAndShadow(); !result)
    {
        return result;
    }
    if (auto result = impl_->CreateUiTexture(); !result)
    {
        return result;
    }
    if (auto result = impl_->CreatePostProcessTargets(); !result)
    {
        return result;
    }
    if (auto result = impl_->CreatePipeline(); !result)
    {
        return result;
    }
    if (auto result = impl_->CreateGpuData(); !result)
    {
        return result;
    }
    if (auto result = impl_->CreateCharacterTextures(); !result)
    {
        return result;
    }
#if defined(HS_DEVELOPMENT_TOOLS)
    D3D12_DESCRIPTOR_HEAP_DESC description{};
    description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    description.NumDescriptors = 1;
    description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(impl_->device->CreateDescriptorHeap(&description,
                                                    IID_PPV_ARGS(&impl_->imgui_heap))))
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_devtools",
                               "Cannot create Dear ImGui descriptor heap.");
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    const auto imgui_font_path =
        (ExecutableDirectory() / L"Fonts" / L"NotoSansKR.ttf").string();
    ImGui::GetIO().Fonts->AddFontFromFileTTF(
        imgui_font_path.c_str(), 16.0f, nullptr,
        ImGui::GetIO().Fonts->GetGlyphRangesKorean());
    ImGui::StyleColorsDark();
    if (!ImGui_ImplWin32_Init(static_cast<HWND>(config.window)))
    {
        ImGui::DestroyContext();
        return Result::Failure(ErrorCode::InvalidState, "hs_devtools",
                               "Cannot initialize Dear ImGui Win32 backend.");
    }
    ImGui_ImplDX12_InitInfo info{};
    info.Device = impl_->device.Get();
    info.CommandQueue = impl_->queue.Get();
    info.NumFramesInFlight = kFrameCount;
    info.RTVFormat = kBackBufferFormat;
    info.DSVFormat = kDepthFormat;
    info.SrvDescriptorHeap = impl_->imgui_heap.Get();
    info.LegacySingleSrvCpuDescriptor =
        impl_->imgui_heap->GetCPUDescriptorHandleForHeapStart();
    info.LegacySingleSrvGpuDescriptor =
        impl_->imgui_heap->GetGPUDescriptorHandleForHeapStart();
    if (!ImGui_ImplDX12_Init(&info))
    {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return Result::Failure(ErrorCode::InvalidState, "hs_devtools",
                               "Cannot initialize Dear ImGui D3D12 backend.");
    }
    impl_->imgui_initialized = true;
    impl_->shader_write = LatestShaderWrite();
    impl_->next_shader_check = std::chrono::steady_clock::now();
#endif
    impl_->initialized = true;
    return Result::Success();
}

void D3D12Renderer::HandleWindowMessage(const NativeWindowMessage &message) noexcept
{
#if defined(HS_DEVELOPMENT_TOOLS)
    if (impl_->imgui_initialized)
    {
        ImGui_ImplWin32_WndProcHandler(static_cast<HWND>(impl_->config.window),
                                       message.message,
                                       static_cast<WPARAM>(message.wparam),
                                       static_cast<LPARAM>(message.lparam));
    }
#else
    (void)message;
#endif
}

Result D3D12Renderer::Resize(std::uint32_t width, std::uint32_t height)
{
    if (!impl_->initialized || width == 0 || height == 0 ||
        (width == impl_->width && height == impl_->height))
    {
        return Result::Success();
    }
    if (auto result = impl_->WaitForGpu(); !result)
    {
        return result;
    }

    impl_->depth.Reset();
    impl_->shadow.Reset();
    for (auto &back_buffer : impl_->back_buffers)
    {
        back_buffer.Reset();
    }
    const auto result = impl_->swap_chain->ResizeBuffers(kFrameCount, width, height,
                                                         kBackBufferFormat, 0);
    if (FAILED(result))
    {
        return impl_->CheckDevice(result, "ResizeBuffers");
    }
    impl_->width = width;
    impl_->height = height;
    const auto render_scale = std::clamp(impl_->config.render_scale_percent, 75u, 100u);
    impl_->render_width = std::max(width * render_scale / 100u, 1u);
    impl_->render_height = std::max(height * render_scale / 100u, 1u);

    auto handle = impl_->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (std::uint32_t index = 0; index < kFrameCount; ++index)
    {
        const auto get_result =
            impl_->swap_chain->GetBuffer(index, IID_PPV_ARGS(&impl_->back_buffers[index]));
        if (FAILED(get_result))
        {
            return HResultFailure("Get resized swap-chain buffer", get_result);
        }
        impl_->device->CreateRenderTargetView(impl_->back_buffers[index].Get(), nullptr, handle);
        handle.ptr += impl_->rtv_stride;
    }
    if (auto depth_result = impl_->CreateDepthAndShadow(); !depth_result)
    {
        return depth_result;
    }
    return impl_->CreatePostProcessTargets();
}

Result D3D12Renderer::ApplyOptions(const RendererOptions &options)
{
    if (!impl_->initialized)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Renderer not initialized.");

    const auto render_scale = std::clamp(options.render_scale_percent, 75u, 100u);
    const auto shadow_resolution = std::clamp(options.shadow_resolution, 1024u, 2048u);
    const auto recreate_targets = render_scale != impl_->config.render_scale_percent ||
                                  shadow_resolution != impl_->config.shadow_resolution;
    impl_->config.vsync = options.vsync;
    impl_->config.bloom = options.bloom;
    impl_->config.outline = options.outline;
    impl_->config.render_scale_percent = render_scale;
    impl_->config.shadow_resolution = shadow_resolution;
    impl_->config.particle_percentage =
        std::clamp(options.particle_percentage, 50u, 100u);
    if (!recreate_targets)
        return Result::Success();
    if (auto result = impl_->WaitForGpu(); !result)
        return result;

    impl_->render_width = std::max(impl_->width * render_scale / 100u, 1u);
    impl_->render_height = std::max(impl_->height * render_scale / 100u, 1u);
    impl_->depth.Reset();
    impl_->shadow.Reset();
    if (auto result = impl_->CreateDepthAndShadow(); !result)
        return result;
    return impl_->CreatePostProcessTargets();
}

Result D3D12Renderer::Render(const RenderSnapshotExchange::ReadPair &snapshots,
                             std::span<const PresentationEvent> events,
                             std::span<const ParticleSpawnCommand> particle_spawns,
                             std::span<const EffectLineSpawnCommand> effect_lines,
                             const DevToolsFrameData &devtools,
                             RendererFrameResult &frame_result)
{
    static_cast<void>(effect_lines);
    if (!impl_->initialized)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Renderer not initialized.");
    }

#if defined(HS_DEVELOPMENT_TOOLS)
    if (std::chrono::steady_clock::now() >= impl_->next_shader_check)
    {
        impl_->next_shader_check = std::chrono::steady_clock::now() +
                                   std::chrono::milliseconds(250);
        const auto write = LatestShaderWrite();
        if (write != std::filesystem::file_time_type{} && write != impl_->shader_write)
        {
            impl_->shader_write = write;
            const auto reloaded = impl_->ReloadPipeline();
            std::ofstream(impl_->config.artifact_directory / "shader_hot_reload.log",
                          std::ios::app)
                << (reloaded ? "applied\n"
                             : std::format("rejected {}; previous PSO retained\n",
                                           reloaded.Message()));
        }
    }
#endif

    const auto back_buffer_index = impl_->swap_chain->GetCurrentBackBufferIndex();
    auto &frame = impl_->frames[back_buffer_index];
    if (auto result = impl_->WaitForFrame(frame); !result)
    {
        return result;
    }

    auto snapshot = snapshots.has_current ? snapshots.current : RenderSnapshot{};
    std::vector<RenderInstance> render_instances(snapshot.instances.begin(),
                                                 snapshot.instances.end());
    std::vector<ParticleSpawnCommand> frame_particle_spawns(particle_spawns.begin(),
                                                             particle_spawns.end());
    if (snapshot.header.tick != impl_->last_status_visual_tick)
    {
        impl_->last_status_visual_tick = snapshot.header.tick;
        for (const auto &source : snapshot.instances)
        {
            const auto add_status = [&](StatusVisual status,
                                        const ParticleSpriteBinding &binding,
                                        ParticleFacing facing, VfxRenderer renderer,
                                        VfxPrimitive primitive, float height,
                                        float size, Float4 color, std::uint32_t count) {
                if ((source.status_visual_mask & static_cast<std::uint32_t>(status)) == 0)
                    return;
                ParticleSpawnCommand command;
                command.sequence = source.stable_id ^ snapshot.header.tick ^
                                   static_cast<std::uint32_t>(status);
                command.tick = snapshot.header.tick;
                command.position = {source.position.x, source.position.y + height,
                                    source.position.z};
                command.shape = ParticleShape::Point;
                command.velocity_mode = ParticleVelocity::Direction;
                command.facing = facing;
                command.renderer = renderer;
                command.primitive = primitive;
                command.sprite = binding.sprite;
                command.frame_columns = binding.frame_columns;
                command.frame_rows = binding.frame_rows;
                command.direction = {0.0f, 1.0f, 0.0f};
                command.lifetime_min = command.lifetime_max = 2.0f / 60.0f;
                command.start_color = command.end_color = color;
                command.start_size_min = command.start_size_max = size;
                command.end_size_min = command.end_size_max = size;
                command.count = count;
                command.seed = static_cast<std::uint32_t>(command.sequence);
                frame_particle_spawns.push_back(command);
            };
            add_status(StatusVisual::Bleed, impl_->config.bleed_status_sprite,
                       ParticleFacing::Velocity, VfxRenderer::Mesh,
                       VfxPrimitive::Shard, source.scale.y * 0.55f,
                       source.scale.y * 0.16f, {1.8f, 0.04f, 0.05f, 0.42f}, 1);
            add_status(StatusVisual::Burn, impl_->config.burn_status_sprite,
                       ParticleFacing::Velocity, VfxRenderer::Mesh,
                       VfxPrimitive::Ember, source.scale.y * 0.5f,
                       source.scale.y * 0.18f, {2.2f, 0.7f, 0.08f, 0.38f}, 1);
            add_status(StatusVisual::Slow, impl_->config.slow_status_sprite,
                       ParticleFacing::Ground, VfxRenderer::Ground,
                       VfxPrimitive::Rune, 0.025f, source.scale.x * 0.72f,
                       {0.25f, 0.85f, 1.8f, 0.28f}, 1);
            add_status(StatusVisual::Mark, impl_->config.mark_status_sprite,
                       ParticleFacing::Velocity, VfxRenderer::Mesh,
                       VfxPrimitive::Spike, source.scale.y * 1.1f,
                       source.scale.y * 0.22f, {2.1f, 1.0f, 0.15f, 0.5f}, 1);
        }
        for (const auto &visual : snapshot.persistent_vfx)
        {
            ParticleSpawnCommand command;
            command.sequence = visual.stable_id ^ snapshot.header.tick;
            command.tick = snapshot.header.tick;
            command.position = visual.position;
            command.shape = ParticleShape::Point;
            command.velocity_mode = ParticleVelocity::Direction;
            command.facing = ParticleFacing::Ground;
            command.renderer = VfxRenderer::Ground;
            command.direction = {0.0f, 1.0f, 0.0f};
            command.lifetime_min = command.lifetime_max = 2.0f / 60.0f;
            command.start_size_min = command.start_size_max = visual.radius;
            command.end_size_min = command.end_size_max = visual.radius;
            command.rotation_min = command.rotation_max = visual.yaw;
            switch (visual.kind)
            {
            case PersistentVfxKind::TrapPending:
                command.primitive = VfxPrimitive::Ring;
                command.start_color = command.end_color = {0.45f, 0.8f, 1.3f, 0.18f};
                break;
            case PersistentVfxKind::TrapArmed:
                command.primitive = VfxPrimitive::Rune;
                command.start_color = command.end_color = {1.55f, 0.8f, 0.16f, 0.24f};
                break;
            case PersistentVfxKind::FireArea:
                command.primitive = VfxPrimitive::Ring;
                command.start_color = command.end_color = {3.2f, 0.62f, 0.035f, 0.34f};
                break;
            case PersistentVfxKind::SlowArea:
                command.primitive = VfxPrimitive::Ring;
                command.start_color = command.end_color = {0.2f, 0.75f, 1.65f, 0.2f};
                break;
            case PersistentVfxKind::ArrowRainArea:
                command.primitive = VfxPrimitive::Ring;
                command.start_color = command.end_color = {1.55f, 1.05f, 0.28f, 0.18f};
                break;
            case PersistentVfxKind::DamageTrail:
            case PersistentVfxKind::ChargeGuide:
            {
                command.renderer = VfxRenderer::Segment;
                command.primitive = VfxPrimitive::SolidTrail;
                const auto direction = Float3{std::sin(visual.yaw), 0.0f,
                                              std::cos(visual.yaw)};
                command.direction = direction;
                command.rotation_min = command.rotation_max = 0.0f;
                command.start_size_min = command.start_size_max = visual.radius;
                command.end_size_min = command.end_size_max = visual.radius;
                command.stretch = visual.length / std::max(visual.radius * 2.0f, 0.001f);
                command.start_color = command.end_color =
                    visual.kind == PersistentVfxKind::ChargeGuide
                        ? Float4{1.8f, 1.8f, 1.8f, 0.34f}
                        : Float4{0.35f, 1.0f, 1.8f, 0.16f};
                break;
            }
            }
            command.count = 1;
            command.seed = static_cast<std::uint32_t>(command.sequence);
            frame_particle_spawns.push_back(command);
        }
    }
    for (const auto &line : effect_lines)
    {
        const auto dx = line.end.x - line.start.x;
        const auto dz = line.end.z - line.start.z;
        const auto length = std::hypot(dx, dz);
        if (length <= 0.0001f) continue;
        ParticleSpawnCommand command;
        command.sequence = line.sequence;
        command.tick = line.tick;
        command.position = {(line.start.x + line.end.x) * 0.5f, 0.035f,
                            (line.start.z + line.end.z) * 0.5f};
        command.shape = ParticleShape::Line;
        command.velocity_mode = ParticleVelocity::Direction;
        command.facing = ParticleFacing::Ground;
        command.renderer = VfxRenderer::Segment;
        command.primitive = line.primitive;
        command.sprite = line.sprite;
        command.frame_columns = line.frame_columns;
        command.frame_rows = line.frame_rows;
        command.shape_extent = {length * 0.5f, 0.0f, 0.0f};
        command.direction = {dx / length, 0.0f, dz / length};
        command.lifetime_min = command.lifetime_max = line.lifetime;
        command.start_color = command.end_color = line.color;
        command.start_size_min = command.start_size_max = line.width * 2.5f;
        command.end_size_min = command.end_size_max = line.width * 2.5f;
        command.stretch = length / std::max(line.width * 5.0f, 0.001f);
        command.rotation_min = command.rotation_max = 0.0f;
        command.count = 1;
        command.seed = static_cast<std::uint32_t>(line.sequence);
        frame_particle_spawns.push_back(command);
    }
    const auto original_instance_count = snapshot.instances.size();
    const auto particle_spawn_data_offset =
        (kInstanceDataOffset + sizeof(GpuInstance) * render_instances.size() + 255u) &
        ~std::size_t{255u};
    const auto particle_owner_data_offset =
        (particle_spawn_data_offset + sizeof(GpuParticleSpawnCommand) *
             std::max<std::size_t>(frame_particle_spawns.size(), 1) + 255u) & ~std::size_t{255u};
    const auto required_upload_size = particle_owner_data_offset +
        sizeof(std::uint32_t) * kParticleCount;
    if (required_upload_size > frame.upload_size)
    {
        std::size_t new_size = frame.upload_size;
        while (new_size < required_upload_size) new_size *= 2;
        frame.upload.resource->Unmap(0, nullptr);
        frame.mapped = nullptr;
        frame.upload.Reset();
        D3D12MA::ALLOCATION_DESC upload_allocation{};
        upload_allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        if (auto result = impl_->CreateAllocation(
                frame.upload, upload_allocation, BufferDescription(new_size),
                D3D12_RESOURCE_STATE_GENERIC_READ);
            !result)
        {
            return result;
        }
        D3D12_RANGE no_read{};
        const auto mapped = frame.upload.resource->Map(
            0, &no_read, reinterpret_cast<void **>(&frame.mapped));
        if (FAILED(mapped)) return HResultFailure("Map grown frame upload", mapped);
        frame.upload_size = new_size;
    }
    std::uint8_t debug_command{};
    std::uint64_t debug_value{};
    std::uint32_t debug_secondary{};
#if defined(HS_DEVELOPMENT_TOOLS)
    if (impl_->config.devtools_visible)
    {
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowSize(ImVec2(430.0f, 720.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Project HS DevTools");
    ImGui::Text("Tick: %llu", static_cast<unsigned long long>(snapshot.header.tick));
    ImGui::Text("Checksum: %llu",
                static_cast<unsigned long long>(snapshot.header.checksum));
    ImGui::Text("Instances: %zu", snapshot.instances.size());
    ImGui::Text("Render entities: %zu", snapshot.instances.size());
    ImGui::Text("UI models: %zu", snapshot.ui.size());
    ImGui::Text("GPU particles: %u / %u", impl_->last_particle_count,
                kParticleCount * std::clamp(impl_->config.particle_percentage, 50u, 100u) /
                    100u);
    D3D12MA::Budget local_budget{};
    impl_->allocator->GetBudget(&local_budget, nullptr);
    ImGui::Text("Video memory: %.1f / %.1f MiB",
                static_cast<double>(local_budget.UsageBytes) / (1024.0 * 1024.0),
                static_cast<double>(local_budget.BudgetBytes) / (1024.0 * 1024.0));
    ImGui::Text("Threads: Main / Simulation / Render + %u workers",
                devtools.worker_count);
    ImGui::Text("Queues I/P/V/G/D: %u/%u/%u/%u/%u",
                devtools.input_queue_depth, devtools.presentation_queue_depth,
                devtools.particle_queue_depth, devtools.graphics_queue_depth,
                devtools.debug_queue_depth);
    ImGui::Text("Dropped I/P/V: %llu/%llu/%llu",
                static_cast<unsigned long long>(devtools.dropped_input),
                static_cast<unsigned long long>(devtools.dropped_presentation),
                static_cast<unsigned long long>(devtools.dropped_particles));
    ImGui::SeparatorText("Character animation inspection");
    ImGui::Checkbox("Close-up preview", &impl_->config.character_preview);
    if (impl_->config.character_preview)
    {
        ImGui::SliderFloat("Camera distance", &impl_->preview_distance, 2.5f, 8.0f);
        ImGui::SliderFloat("Camera yaw", &impl_->preview_yaw, 0.0f, 360.0f);
        ImGui::SliderFloat("Camera pitch", &impl_->preview_pitch, -20.0f, 60.0f);
        ImGui::Checkbox("Override pose", &impl_->preview_pose_override);
        if (impl_->preview_pose_override)
        {
            constexpr const char *clips = "Idle\0Run\0Draw\0Recoil\0Death\0";
            ImGui::Combo("Base clip", &impl_->preview_base_clip, clips);
            ImGui::SliderFloat("Base time", &impl_->preview_base_time, 0.0f, 1.0f);
            ImGui::Combo("Blend clip", &impl_->preview_secondary_clip, clips);
            ImGui::SliderFloat("Blend time", &impl_->preview_secondary_time, 0.0f, 1.0f);
            ImGui::SliderFloat("Blend weight", &impl_->preview_secondary_weight, 0.0f, 1.0f);
            ImGui::Combo("Upper clip", &impl_->preview_upper_clip, clips);
            ImGui::SliderFloat("Upper time", &impl_->preview_upper_time, 0.0f, 1.0f);
            ImGui::SliderFloat("Upper weight", &impl_->preview_upper_weight, 0.0f, 1.0f);
        }
    }
    ImGui::SeparatorText("Simulation");
    if (ImGui::Button("Start Session")) debug_command = 1;
    if (ImGui::Button("Pause / Resume")) debug_command = 15;
    if (ImGui::Button("Grant 100 XP")) { debug_command = 6; debug_value = 100; }
    if (ImGui::Button("Damage Player 10")) { debug_command = 3; debug_value = 10; }
    ImGui::SameLine();
    if (ImGui::Button("Heal Player 10")) { debug_command = 4; debug_value = 10; }
    if (ImGui::Button("Damage Final Boss 1000")) { debug_command = 5; debug_value = 1000; }
    if (ImGui::Button("Spawn Melee")) { debug_command = 10; debug_value = 0; }
    ImGui::SameLine();
    if (ImGui::Button("Spawn Ranged")) { debug_command = 10; debug_value = 1; }
    ImGui::SameLine();
    if (ImGui::Button("Spawn Suicide")) { debug_command = 10; debug_value = 2; }
    if (ImGui::Button("Growth 5m")) { debug_command = 2; debug_value = 18'000; }
    ImGui::SameLine();
    if (ImGui::Button("Growth 10m")) { debug_command = 2; debug_value = 36'000; }
    ImGui::SameLine();
    if (ImGui::Button("Growth 15m")) { debug_command = 2; debug_value = 54'000; }
    if (ImGui::Button("Spawn 5m Boss")) { debug_command = 11; debug_value = 0; }
    ImGui::SameLine();
    if (ImGui::Button("Spawn 10m Boss")) { debug_command = 11; debug_value = 1; }
    ImGui::SameLine();
    if (ImGui::Button("Spawn Final Boss")) { debug_command = 11; debug_value = 2; }

    static int skill{};
    static int upgrade{};
    static int relic{};
    static int stat{};
    ImGui::SeparatorText("Build controls");
    ImGui::Combo("Skill", &skill, kDebugSkillNames.data(),
                 static_cast<int>(kDebugSkillNames.size()));
    if (ImGui::Button("Grant Skill")) { debug_command = 7; debug_value = skill + 1; }
    ImGui::Combo("Upgrade", &upgrade, kDebugUpgradeNames[skill].data(),
                 static_cast<int>(kDebugUpgradeNames[skill].size()));
    if (ImGui::Button("Grant Upgrade")) {
        debug_command = 8;
        debug_value = skill + 1;
        debug_secondary = static_cast<std::uint32_t>(upgrade);
    }
    ImGui::Combo("Relic", &relic, kDebugRelicNames.data(),
                 static_cast<int>(kDebugRelicNames.size()));
    if (ImGui::Button("Grant Relic")) { debug_command = 9; debug_value = relic; }
    ImGui::Combo("Stat", &stat, kDebugStatNames.data(),
                 static_cast<int>(kDebugStatNames.size()));
    if (ImGui::Button("Assign Stat")) { debug_command = 13; debug_value = stat; }
    ImGui::SameLine();
    if (ImGui::Button("Reroll")) debug_command = 14;
    ImGui::SeparatorText("RenderGraph");
    constexpr std::string_view passes[]{
        "GPU Particle Spawn/Update", "3-cascade Directional Shadow", "GBuffer+Depth",
        "Deferred Cel Lighting", "Forward Transparent/OIT", "OIT Composite", "Bloom",
        "ToneMap", "Screen-space Outline", "FXAA", "Game UI"};
    for (const auto pass : passes)
        ImGui::BulletText("%.*s", static_cast<int>(pass.size()), pass.data());
    ImGui::End();
    ImGui::Render();
    }
#endif
    if (auto result = impl_->RasterizeUi(back_buffer_index, snapshot.ui); !result)
    {
        return result;
    }
    const auto instance_count = render_instances.size();

    auto *constants = reinterpret_cast<FrameConstants *>(frame.mapped);
    auto *instances = reinterpret_cast<GpuInstance *>(frame.mapped + kInstanceDataOffset);
    auto *gpu_particle_spawns =
        reinterpret_cast<GpuParticleSpawnCommand *>(frame.mapped + particle_spawn_data_offset);
    auto *gpu_particle_owners =
        reinterpret_cast<std::uint32_t *>(frame.mapped + particle_owner_data_offset);
    constexpr auto particle_capacity = kParticleCount;
    std::uint32_t gpu_particle_spawn_count{};
    std::uint32_t total_particles_to_spawn{};
    for (const auto &source : frame_particle_spawns)
    {
        if (total_particles_to_spawn == particle_capacity)
        {
            break;
        }
        const auto age_ticks =
            snapshot.header.tick > source.tick ? snapshot.header.tick - source.tick : 0;
        const auto age = static_cast<float>(age_ticks) / 60.0f;
        if (age >= source.lifetime_max || source.count == 0)
        {
            continue;
        }
        const auto count = std::min(source.count, particle_capacity - total_particles_to_spawn);
        auto &target_spawn = gpu_particle_spawns[gpu_particle_spawn_count++];
        target_spawn.position_lifetime_min = {source.position.x, source.position.y,
                                              source.position.z, source.lifetime_min};
        target_spawn.direction_lifetime_max = {source.direction.x, source.direction.y,
                                               source.direction.z, source.lifetime_max};
        target_spawn.shape_extent_speed_min = {source.shape_extent.x, source.shape_extent.y,
                                               source.shape_extent.z, source.speed_min};
        target_spawn.speed_cone_gravity_stretch = {source.speed_max, source.cone_radians,
                                                   source.gravity, source.stretch};
        target_spawn.start_color = {source.start_color.x, source.start_color.y,
                                    source.start_color.z, source.start_color.w};
        target_spawn.end_color = {source.end_color.x, source.end_color.y,
                                  source.end_color.z, source.end_color.w};
        target_spawn.size_range = {source.start_size_min, source.start_size_max,
                                   source.end_size_min, source.end_size_max};
        target_spawn.rotation_range = {source.rotation_min, source.rotation_max,
                                       source.angular_velocity_min,
                                       source.angular_velocity_max};
        const auto sprite_metadata = static_cast<std::uint32_t>(source.sprite) |
                                     (static_cast<std::uint32_t>(source.frame_columns) << 16u) |
                                     (static_cast<std::uint32_t>(source.frame_rows) << 24u);
        const auto visual_metadata = static_cast<std::uint32_t>(source.facing) |
                                     (static_cast<std::uint32_t>(source.renderer) << 8u) |
                                     (static_cast<std::uint32_t>(source.primitive) << 16u);
        target_spawn.modes = {static_cast<std::uint32_t>(source.shape),
                              static_cast<std::uint32_t>(source.velocity_mode),
                              visual_metadata,
                              sprite_metadata};
        target_spawn.metadata = {count, source.seed, total_particles_to_spawn,
                                 std::bit_cast<std::uint32_t>(age)};
        std::fill_n(gpu_particle_owners + total_particles_to_spawn, count,
                    gpu_particle_spawn_count - 1);
        total_particles_to_spawn += count;
    }
    const auto particle_delta_ticks =
        impl_->particles_initialized && snapshot.header.tick > impl_->last_particle_tick
            ? snapshot.header.tick - impl_->last_particle_tick
            : 0;
    const auto now = std::chrono::steady_clock::now();
    if (snapshot.header.tick != impl_->observed_snapshot_tick)
    {
        impl_->observed_snapshot_tick = snapshot.header.tick;
        impl_->snapshot_arrival = now;
    }
    const auto interpolation =
        impl_->config.interpolate && snapshots.has_previous
            ? std::clamp(
                  std::chrono::duration<float>(now - impl_->snapshot_arrival).count() * 60.0f,
                  0.0f, 1.0f)
            : 1.0f;
    const auto target_height = impl_->config.character_preview ? 1.0f
                                                                : snapshot.camera.target.y;
    const auto target = DirectX::XMVectorSet(snapshot.camera.target.x, target_height,
                                             snapshot.camera.target.z, 1.0f);
#if defined(HS_DEVELOPMENT_TOOLS)
    const auto camera_yaw = impl_->config.character_preview ? impl_->preview_yaw
                                                             : snapshot.camera.yaw_degrees;
    const auto camera_pitch = impl_->config.character_preview ? impl_->preview_pitch
                                                               : snapshot.camera.pitch_degrees;
    const auto camera_distance = impl_->config.character_preview
                                     ? impl_->preview_distance
                                     : snapshot.camera.distance;
#else
    const auto camera_yaw = snapshot.camera.yaw_degrees;
    const auto camera_pitch = snapshot.camera.pitch_degrees;
    const auto camera_distance = snapshot.camera.distance;
#endif
    const auto yaw = DirectX::XMConvertToRadians(camera_yaw);
    const auto pitch = DirectX::XMConvertToRadians(camera_pitch);
    const auto direction = DirectX::XMVector3Normalize(
        DirectX::XMVectorSet(std::cos(pitch) * std::sin(yaw), -std::sin(pitch),
                             std::cos(pitch) * std::cos(yaw), 0.0f));
    const auto eye = DirectX::XMVectorSubtract(
        target, DirectX::XMVectorScale(direction, camera_distance));
    const auto view =
        DirectX::XMMatrixLookAtLH(eye, target, DirectX::XMVectorSet(0, 1, 0, 0));
    const auto projection = DirectX::XMMatrixPerspectiveFovLH(
        DirectX::XMConvertToRadians(snapshot.camera.vertical_fov_degrees),
        static_cast<float>(impl_->render_width) / static_cast<float>(impl_->render_height), 500.0f,
        0.1f);
    DirectX::XMStoreFloat4x4(&constants->view_projection,
                             DirectX::XMMatrixTranspose(view * projection));
    if (impl_->frame_number == 0)
    {
        DirectX::XMFLOAT3 projected_origin{};
        DirectX::XMFLOAT3 projected_forward{};
        DirectX::XMStoreFloat3(
            &projected_origin,
            DirectX::XMVector3TransformCoord(DirectX::XMVectorZero(), view * projection));
        DirectX::XMStoreFloat3(
            &projected_forward,
            DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(0, 0, 20, 1),
                                             view * projection));
        std::ofstream camera_log(impl_->config.artifact_directory / "camera.json",
                                 std::ios::trunc);
        camera_log << std::format(
            "{{\"origin\":[{},{},{}],\"forward\":[{},{},{}],\"yaw\":{},\"pitch\":{}}}\n",
            projected_origin.x, projected_origin.y, projected_origin.z, projected_forward.x,
            projected_forward.y, projected_forward.z, camera_yaw, camera_pitch);
    }
    DirectX::XMStoreFloat4(&constants->camera_time, eye);
    constants->camera_time.w =
        static_cast<float>(snapshot.header.simulation_time.count()) / 1'000'000'000.0f;
    const auto light = snapshot.lights.empty() ? LightView{{-0.4f, -0.8f, 0.3f}, 3.0f, {1, 1, 1}}
                                                : snapshot.lights.front();
    constants->light_direction_intensity = {light.direction.x, light.direction.y,
                                             light.direction.z, light.intensity};
    constants->light_color = {light.color.x, light.color.y, light.color.z, 1.0f};
    constants->screen_size = {
        static_cast<float>(impl_->render_width), static_cast<float>(impl_->render_height),
        1.0f / static_cast<float>(impl_->render_width),
        1.0f / static_cast<float>(impl_->render_height)};
    DirectX::XMFLOAT3 camera_forward;
    DirectX::XMStoreFloat3(&camera_forward, direction);
    constants->camera_forward_softness = {camera_forward.x, camera_forward.y,
                                          camera_forward.z, 0.35f};
    constexpr float cascade_extent[] = {18.0f, 36.0f, 72.0f};
    const auto light_direction = DirectX::XMVector3Normalize(
        DirectX::XMVectorSet(light.direction.x, light.direction.y, light.direction.z, 0.0f));
    const auto light_eye =
        DirectX::XMVectorSubtract(target, DirectX::XMVectorScale(light_direction, 60.0f));
    const auto light_view =
        DirectX::XMMatrixLookAtLH(light_eye, target, DirectX::XMVectorSet(0, 1, 0, 0));
    for (std::size_t cascade = 0; cascade < std::size(cascade_extent); ++cascade)
    {
        const auto light_projection = DirectX::XMMatrixOrthographicLH(
            cascade_extent[cascade], cascade_extent[cascade], 120.0f, 0.1f);
        DirectX::XMStoreFloat4x4(
            &constants->shadow_view_projection[cascade],
            DirectX::XMMatrixTranspose(light_view * light_projection));
    }
    for (auto &bone : constants->archer_bones)
    {
        DirectX::XMStoreFloat4x4(
            &bone, DirectX::XMMatrixTranspose(DirectX::XMMatrixIdentity()));
    }
    auto pose = snapshot.poses.empty() ? AnimationPoseRef{} : snapshot.poses.front();
#if defined(HS_DEVELOPMENT_TOOLS)
    if (impl_->config.character_preview && impl_->preview_pose_override)
    {
        pose.clip = static_cast<CharacterAnimationClip>(impl_->preview_base_clip);
        pose.normalized_time = impl_->preview_base_time;
        pose.secondary_clip =
            static_cast<CharacterAnimationClip>(impl_->preview_secondary_clip);
        pose.secondary_normalized_time = impl_->preview_secondary_time;
        pose.secondary_weight = impl_->preview_secondary_weight;
        pose.upper_body_clip =
            static_cast<CharacterAnimationClip>(impl_->preview_upper_clip);
        pose.upper_body_normalized_time = impl_->preview_upper_time;
        pose.upper_body_weight = impl_->preview_upper_weight;
    }
#endif
    const auto eased_weight = [](float weight) {
        const auto clamped = std::clamp(weight, 0.0f, 1.0f);
        return clamped * clamped * (3.0f - 2.0f * clamped);
    };
    const auto blend_transform = [](const CharacterLocalTransform &first,
                                    const CharacterLocalTransform &second,
                                    float weight) {
        CharacterLocalTransform output;
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            output.translation[axis] = std::lerp(first.translation[axis],
                                                 second.translation[axis], weight);
            output.scale[axis] = std::lerp(first.scale[axis], second.scale[axis], weight);
        }
        auto first_rotation = DirectX::XMQuaternionNormalize(DirectX::XMVectorSet(
            first.rotation[0], first.rotation[1], first.rotation[2], first.rotation[3]));
        auto second_rotation = DirectX::XMQuaternionNormalize(DirectX::XMVectorSet(
            second.rotation[0], second.rotation[1], second.rotation[2], second.rotation[3]));
        if (DirectX::XMVectorGetX(
                DirectX::XMQuaternionDot(first_rotation, second_rotation)) < 0.0f)
        {
            second_rotation = DirectX::XMVectorNegate(second_rotation);
        }
        DirectX::XMFLOAT4 rotation;
        DirectX::XMStoreFloat4(
            &rotation, DirectX::XMQuaternionNormalize(DirectX::XMQuaternionSlerp(
                           first_rotation, second_rotation, weight)));
        output.rotation = {rotation.x, rotation.y, rotation.z, rotation.w};
        return output;
    };
    const auto sample = [&](CharacterAnimationClip clip, float time,
                            std::uint32_t bone_index,
                            CharacterLocalTransform &output) {
        const auto found = std::ranges::find(impl_->archer_clips, clip,
                                              &CharacterClipHeader::clip);
        if (found == impl_->archer_clips.end()) return false;
        const auto normalized = found->looping ? time - std::floor(time)
                                               : std::clamp(time, 0.0f, 1.0f);
        const auto frame_position = normalized * static_cast<float>(found->frame_count - 1);
        const auto first_frame = static_cast<std::uint32_t>(frame_position);
        const auto second_frame = std::min(first_frame + 1, found->frame_count - 1);
        const auto &first = impl_->archer_transforms[
            found->first_transform + first_frame * impl_->archer_bone_count + bone_index];
        const auto &second = impl_->archer_transforms[
            found->first_transform + second_frame * impl_->archer_bone_count + bone_index];
        output = blend_transform(first, second,
                                 frame_position - static_cast<float>(first_frame));
        return true;
    };
    std::array<DirectX::XMFLOAT4X4, kMaxCharacterBones> global_transforms{};
    for (std::uint32_t bone_index = 0; bone_index < impl_->archer_bone_count; ++bone_index)
    {
        CharacterLocalTransform base, secondary, upper;
        if (!sample(pose.clip, pose.normalized_time * std::max(0.0f, pose.playback_rate),
                    bone_index, base) ||
            !sample(pose.secondary_clip,
                    pose.secondary_normalized_time *
                        std::max(0.0f, pose.secondary_playback_rate),
                    bone_index, secondary))
        {
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Cooked animation transform cannot be blended.");
        }
        base = blend_transform(base, secondary, eased_weight(pose.secondary_weight));
        const auto upper_weight = eased_weight(pose.upper_body_weight) *
                                  impl_->archer_upper_body_weights[bone_index];
        if (upper_weight > 0.0f &&
            (!sample(pose.upper_body_clip,
                     pose.upper_body_normalized_time *
                         std::max(0.0f, pose.upper_body_playback_rate),
                     bone_index, upper)))
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Cooked upper-body animation cannot be blended.");
        if (upper_weight > 0.0f)
        {
            base = blend_transform(base, upper, upper_weight);
        }
        const auto local =
            DirectX::XMMatrixScaling(base.scale[0], base.scale[1], base.scale[2]) *
            DirectX::XMMatrixRotationQuaternion(DirectX::XMVectorSet(
                base.rotation[0], base.rotation[1], base.rotation[2], base.rotation[3])) *
            DirectX::XMMatrixTranslation(base.translation[0], base.translation[1],
                                         base.translation[2]);
        const auto parent = impl_->archer_parents[bone_index];
        const auto global = parent == std::numeric_limits<std::uint16_t>::max()
                                ? local
                                : local * DirectX::XMLoadFloat4x4(
                                              &global_transforms[parent]);
        DirectX::XMStoreFloat4x4(&global_transforms[bone_index], global);
        DirectX::XMFLOAT4X4 inverse_bind;
        std::memcpy(&inverse_bind,
                    impl_->archer_inverse_bind_matrices[bone_index].data(),
                    sizeof(inverse_bind));
        const auto skin = DirectX::XMMatrixTranspose(
                              DirectX::XMLoadFloat4x4(&inverse_bind)) *
                          global;
        const auto determinant = DirectX::XMVectorGetX(DirectX::XMMatrixDeterminant(skin));
        if (!std::isfinite(determinant) || std::abs(determinant) <= 0.0001f ||
            DirectX::XMMatrixIsNaN(skin) || DirectX::XMMatrixIsInfinite(skin))
        {
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Blended animation produced an invalid skin pose.");
        }
        DirectX::XMStoreFloat4x4(&constants->archer_bones[bone_index],
                                 DirectX::XMMatrixTranspose(skin));
    }
    constants->render_options = {
        static_cast<float>(particle_capacity), impl_->config.bloom ? 1.0f : 0.0f,
        impl_->config.outline ? 1.0f : 0.0f,
        static_cast<float>(particle_delta_ticks) / 60.0f};
    constants->particle_options = {particle_capacity, gpu_particle_spawn_count,
                                   total_particles_to_spawn,
                                   impl_->archer_material_count};
    for (std::size_t index = 0; index < instance_count; ++index)
    {
        const auto &source = render_instances[index];
        auto position = source.position;
        auto yaw_value = source.yaw;
        if (snapshots.has_previous &&
            index < original_instance_count &&
            snapshots.previous.instances.size() == original_instance_count &&
            source.stable_id != 0 &&
            snapshots.previous.instances[index].stable_id == source.stable_id)
        {
            const auto &previous = snapshots.previous.instances[index];
            position.x = std::lerp(previous.position.x, source.position.x, interpolation);
            position.y = std::lerp(previous.position.y, source.position.y, interpolation);
            position.z = std::lerp(previous.position.z, source.position.z, interpolation);
            const auto yaw_delta = std::remainder(
                source.yaw - previous.yaw,
                2.0f * DirectX::XM_PI);
            yaw_value = previous.yaw + yaw_delta * interpolation;
        }
        const auto vertical_offset = source.mesh == RenderMesh::Archer
                                         ? impl_->archer_ground_offset
                                         : source.scale.y * 0.5f;
        instances[index] = {{position.x, position.y + vertical_offset, position.z, 1.0f},
                            {source.scale.x, source.scale.y, source.scale.z, 0.0f},
                            source.color_rgba,
                            static_cast<std::uint32_t>(source.mesh),
                            yaw_value,
                            0.0f};
    }

    auto result = frame.allocator->Reset();
    if (FAILED(result))
    {
        return HResultFailure("Reset command allocator", result);
    }
    result = impl_->command_list->Reset(frame.allocator.Get(), nullptr);
    if (FAILED(result))
    {
        return HResultFailure("Reset command list", result);
    }

    auto &ui_texture = impl_->ui_textures[back_buffer_index];
    impl_->TransitionTexture(
        ui_texture.resource.Get(),
        frame.ui_initialized ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                             : D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_COPY_DEST,
        frame.ui_initialized ? D3D12_BARRIER_LAYOUT_SHADER_RESOURCE
                             : D3D12_BARRIER_LAYOUT_COMMON,
        D3D12_BARRIER_LAYOUT_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION ui_destination{};
    ui_destination.pResource = ui_texture.resource.Get();
    ui_destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION ui_source{};
    ui_source.pResource = frame.ui_upload.resource.Get();
    ui_source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    ui_source.PlacedFootprint = impl_->ui_footprint;
    impl_->command_list->CopyTextureRegion(&ui_destination, 0, 0, 0, &ui_source, nullptr);
    impl_->TransitionTexture(ui_texture.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                             D3D12_BARRIER_LAYOUT_COPY_DEST,
                             D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);
    frame.ui_initialized = true;

    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(impl_->render_width),
                                  static_cast<float>(impl_->render_height), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(impl_->render_width),
                             static_cast<LONG>(impl_->render_height)};
    const D3D12_VIEWPORT output_viewport{0.0f, 0.0f, static_cast<float>(impl_->width),
                                         static_cast<float>(impl_->height), 0.0f, 1.0f};
    const D3D12_RECT output_scissor{0, 0, static_cast<LONG>(impl_->width),
                                    static_cast<LONG>(impl_->height)};
    impl_->command_list->RSSetViewports(1, &viewport);
    impl_->command_list->RSSetScissorRects(1, &scissor);
    impl_->command_list->SetGraphicsRootSignature(impl_->root_signature.Get());
    impl_->command_list->SetComputeRootSignature(impl_->root_signature.Get());
    impl_->command_list->SetGraphicsRootConstantBufferView(
        0, frame.upload.resource->GetGPUVirtualAddress());
    impl_->command_list->SetComputeRootConstantBufferView(
        0, frame.upload.resource->GetGPUVirtualAddress());

    const auto rtv_start = impl_->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    const auto rtv_at = [&](std::uint32_t index) {
        return D3D12_CPU_DESCRIPTOR_HANDLE{
            rtv_start.ptr + static_cast<SIZE_T>(index) * impl_->rtv_stride};
    };
    const auto rtv = rtv_at(back_buffer_index);
    const auto gbuffer_base_rtv = rtv_at(kFrameCount);
    const auto gbuffer_normal_rtv = rtv_at(kFrameCount + 1);
    const auto gbuffer_position_rtv = rtv_at(kFrameCount + 2);
    const auto hdr_rtv = rtv_at(kFrameCount + 3);
    const auto oit_accumulation_rtv = rtv_at(kFrameCount + 4);
    const auto oit_revealage_rtv = rtv_at(kFrameCount + 5);
    const auto post_a_rtv = rtv_at(kFrameCount + 6);
    const auto post_b_rtv = rtv_at(kFrameCount + 7);
    const auto dsv = impl_->dsv_heap->GetCPUDescriptorHandleForHeapStart();
    ID3D12DescriptorHeap *descriptor_heaps[] = {impl_->srv_heap.Get()};
    impl_->command_list->SetDescriptorHeaps(1, descriptor_heaps);
    auto texture_table = impl_->srv_heap->GetGPUDescriptorHandleForHeapStart();
    texture_table.ptr += static_cast<UINT64>(back_buffer_index) *
                         kTextureDescriptorCount * impl_->srv_stride;
    auto character_table = texture_table;
    character_table.ptr += static_cast<UINT64>(kPostTextureDescriptorCount) *
                           impl_->srv_stride;
    impl_->command_list->SetGraphicsRootDescriptorTable(13, character_table);
    const auto draw_fullscreen =
        [&](ID3D12PipelineState *pipeline, D3D12_CPU_DESCRIPTOR_HANDLE target) {
            impl_->command_list->OMSetRenderTargets(1, &target, FALSE, nullptr);
            impl_->command_list->SetPipelineState(pipeline);
            impl_->command_list->SetGraphicsRootDescriptorTable(5, texture_table);
            impl_->command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            impl_->command_list->DrawInstanced(3, 1, 0, 0);
        };

    impl_->graph.Reset();
    const auto particles = impl_->graph.ImportBuffer(
        {impl_->particles.resource.Get()}, Access::UnorderedWrite, "Particles");
    const auto particle_input_index = impl_->particle_input_is_a ? 0u : 1u;
    const auto particle_output_index = particle_input_index ^ 1u;
    const auto particle_alive_input = impl_->graph.ImportBuffer(
        {impl_->particle_alive[particle_input_index].resource.Get()}, Access::UnorderedWrite,
        "ParticleAliveInput");
    const auto particle_alive_output = impl_->graph.ImportBuffer(
        {impl_->particle_alive[particle_output_index].resource.Get()}, Access::UnorderedWrite,
        "ParticleAliveOutput");
    const auto particle_dead = impl_->graph.ImportBuffer(
        {impl_->particle_dead.resource.Get()}, Access::UnorderedWrite, "ParticleDead");
    const auto particle_counters = impl_->graph.ImportBuffer(
        {impl_->particle_counters.resource.Get()}, Access::UnorderedWrite, "ParticleCounters");
    const auto indirect_arguments = impl_->graph.ImportBuffer(
        {impl_->indirect_arguments.resource.Get()}, Access::UnorderedWrite, "IndirectArguments");
    const auto initial_depth_access =
        impl_->transient_textures_common ? Access::Common : Access::DepthWrite;
    const auto initial_color_access =
        impl_->transient_textures_common ? Access::Common : Access::ShaderRead;
    const auto shadow =
        impl_->graph.ImportTexture({impl_->shadow.resource.Get()}, initial_depth_access, "Shadow");
    const auto depth =
        impl_->graph.ImportTexture({impl_->depth.resource.Get()}, initial_depth_access, "Depth");
    const auto gbuffer_base = impl_->graph.ImportTexture(
        {impl_->gbuffer_base.resource.Get()}, initial_color_access, "GBufferBase");
    const auto gbuffer_normal = impl_->graph.ImportTexture(
        {impl_->gbuffer_normal.resource.Get()}, initial_color_access, "GBufferNormal");
    const auto gbuffer_position = impl_->graph.ImportTexture(
        {impl_->gbuffer_position.resource.Get()}, initial_color_access, "GBufferPosition");
    const auto hdr = impl_->graph.ImportTexture(
        {impl_->hdr_color.resource.Get()}, initial_color_access, "HdrColor");
    const auto oit_accumulation = impl_->graph.ImportTexture(
        {impl_->oit_accumulation.resource.Get()}, initial_color_access, "OitAccumulation");
    const auto oit_revealage = impl_->graph.ImportTexture(
        {impl_->oit_revealage.resource.Get()}, initial_color_access, "OitRevealage");
    const auto post_a = impl_->graph.ImportTexture(
        {impl_->post_a.resource.Get()}, initial_color_access, "PostA");
    const auto post_b = impl_->graph.ImportTexture(
        {impl_->post_b.resource.Get()}, initial_color_access, "PostB");
    const auto ui = impl_->graph.ImportTexture(
        {ui_texture.resource.Get()}, Access::ShaderRead, "UiTexture");
    const auto back_buffer = impl_->graph.ImportTexture(
        {impl_->back_buffers[back_buffer_index].Get()}, Access::Present, "BackBuffer");

    auto particle_pass = impl_->graph.AddPass("GPU Particle Spawn/Update", QueueHint::Direct);
    particle_pass.ReadWrite(particles, Access::UnorderedWrite);
    particle_pass.ReadWrite(particle_alive_input, Access::UnorderedWrite);
    particle_pass.ReadWrite(particle_alive_output, Access::UnorderedWrite);
    particle_pass.ReadWrite(particle_dead, Access::UnorderedWrite);
    particle_pass.ReadWrite(particle_counters, Access::UnorderedWrite);
    particle_pass.ReadWrite(indirect_arguments, Access::UnorderedWrite);
    const auto initialize_particles = !impl_->particles_initialized;
    particle_pass.SetExecute([&](RenderPassContext &context) {
        impl_->command_list->SetPipelineState(impl_->particle_compute_pipeline.Get());
        impl_->command_list->SetComputeRootUnorderedAccessView(
            3, impl_->particles.resource->GetGPUVirtualAddress());
        impl_->command_list->SetComputeRootUnorderedAccessView(
            4, impl_->indirect_arguments.resource->GetGPUVirtualAddress());
        impl_->command_list->SetComputeRootUnorderedAccessView(
            7, impl_->particle_alive[particle_input_index].resource->GetGPUVirtualAddress());
        impl_->command_list->SetComputeRootUnorderedAccessView(
            8, impl_->particle_alive[particle_output_index].resource->GetGPUVirtualAddress());
        impl_->command_list->SetComputeRootUnorderedAccessView(
            9, impl_->particle_dead.resource->GetGPUVirtualAddress());
        impl_->command_list->SetComputeRootUnorderedAccessView(
            10, impl_->particle_counters.resource->GetGPUVirtualAddress());
        impl_->command_list->SetComputeRootShaderResourceView(
            11, frame.upload.resource->GetGPUVirtualAddress() + particle_spawn_data_offset);
        impl_->command_list->SetComputeRootShaderResourceView(
            14, frame.upload.resource->GetGPUVirtualAddress() + particle_owner_data_offset);

        const auto dispatch_phase = [&](std::uint32_t phase, std::uint32_t item_count) {
            impl_->command_list->SetComputeRoot32BitConstant(6, phase, 0);
            impl_->command_list->Dispatch((std::max(item_count, 1u) + 255) / 256, 1, 1);
        };
        const auto synchronize_particle_state = [&] {
            context.UavBarrier(particles);
            context.UavBarrier(particle_alive_output);
            context.UavBarrier(particle_dead);
            context.UavBarrier(particle_counters);
        };

        if (initialize_particles)
        {
            dispatch_phase(0, particle_capacity);
            context.UavBarrier(particle_dead);
            context.UavBarrier(particle_counters);
            context.UavBarrier(indirect_arguments);
        }
        dispatch_phase(1, 1);
        context.UavBarrier(particle_counters);
        context.UavBarrier(indirect_arguments);
        dispatch_phase(2, particle_capacity);
        synchronize_particle_state();
        if (total_particles_to_spawn != 0)
        {
            dispatch_phase(3, total_particles_to_spawn);
            synchronize_particle_state();
        }
        dispatch_phase(4, 1);
        context.UavBarrier(particle_counters);
        context.UavBarrier(indirect_arguments);
    });

    auto shadow_pass =
        impl_->graph.AddPass("3-cascade Directional Shadow", QueueHint::Direct);
    shadow_pass.Write(shadow, Access::DepthWrite);
    shadow_pass.SetExecute([&](RenderPassContext &) {
        const auto shadow_size =
            static_cast<float>(std::clamp(impl_->config.shadow_resolution, 1024u, 2048u));
        const D3D12_VIEWPORT shadow_viewport{0, 0, shadow_size, shadow_size, 0, 1};
        const D3D12_RECT shadow_scissor{0, 0, static_cast<LONG>(shadow_size),
                                        static_cast<LONG>(shadow_size)};
        impl_->command_list->RSSetViewports(1, &shadow_viewport);
        impl_->command_list->RSSetScissorRects(1, &shadow_scissor);
        impl_->command_list->SetPipelineState(impl_->shadow_pipeline.Get());
        impl_->command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        auto handle = impl_->dsv_heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += impl_->dsv_stride;
        for (std::uint32_t cascade = 0; cascade < 3; ++cascade)
        {
            impl_->command_list->ClearDepthStencilView(handle, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0,
                                                        0, nullptr);
            impl_->command_list->OMSetRenderTargets(0, nullptr, FALSE, &handle);
            impl_->command_list->SetGraphicsRoot32BitConstant(6, cascade, 0);
            if (instance_count != 0)
            {
                impl_->command_list->SetGraphicsRootShaderResourceView(
                    1, frame.upload.resource->GetGPUVirtualAddress() +
                           kInstanceDataOffset);
                impl_->command_list->IASetVertexBuffers(
                    0, 1, &impl_->archer_vertex_view);
                impl_->command_list->DrawInstanced(impl_->archer_vertex_count, 1, 0, 0);
            }
            if (instance_count > 1)
            {
                impl_->command_list->SetGraphicsRootShaderResourceView(
                    1, frame.upload.resource->GetGPUVirtualAddress() +
                           kInstanceDataOffset + sizeof(GpuInstance));
                impl_->command_list->IASetVertexBuffers(0, 1, &impl_->vertex_view);
                impl_->command_list->DrawInstanced(
                    static_cast<UINT>(kCubeVertices.size()),
                    static_cast<UINT>(instance_count - 1), 0, 0);
            }
            handle.ptr += impl_->dsv_stride;
        }
        impl_->command_list->RSSetViewports(1, &viewport);
        impl_->command_list->RSSetScissorRects(1, &scissor);
    });

    auto gbuffer_pass = impl_->graph.AddPass("GBuffer+Depth", QueueHint::Direct);
    gbuffer_pass.Write(gbuffer_base, Access::RenderTarget);
    gbuffer_pass.Write(gbuffer_normal, Access::RenderTarget);
    gbuffer_pass.Write(gbuffer_position, Access::RenderTarget);
    gbuffer_pass.Write(depth, Access::DepthWrite);
    gbuffer_pass.SetExecute([&](RenderPassContext &) {
        constexpr float clear_base[] = {0, 0, 0, 0};
        constexpr float clear_normal[] = {0.5f, 1.0f, 0.5f, 0};
        constexpr float clear_position[] = {0, 0, 0, 0};
        impl_->command_list->ClearRenderTargetView(gbuffer_base_rtv, clear_base, 0, nullptr);
        impl_->command_list->ClearRenderTargetView(gbuffer_normal_rtv, clear_normal, 0, nullptr);
        impl_->command_list->ClearRenderTargetView(gbuffer_position_rtv, clear_position, 0,
                                                   nullptr);
        impl_->command_list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0,
                                                   nullptr);
        const D3D12_CPU_DESCRIPTOR_HANDLE targets[] = {
            gbuffer_base_rtv, gbuffer_normal_rtv, gbuffer_position_rtv};
        impl_->command_list->OMSetRenderTargets(3, targets, FALSE, &dsv);
        impl_->command_list->SetPipelineState(impl_->scene_pipeline.Get());
        impl_->command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (instance_count != 0)
        {
            impl_->command_list->SetGraphicsRootShaderResourceView(
                1, frame.upload.resource->GetGPUVirtualAddress() +
                       kInstanceDataOffset);
            impl_->command_list->IASetVertexBuffers(0, 1,
                                                    &impl_->archer_vertex_view);
            impl_->command_list->DrawInstanced(impl_->archer_vertex_count, 1, 0, 0);
        }
        if (instance_count > 1)
        {
            impl_->command_list->SetGraphicsRootShaderResourceView(
                1, frame.upload.resource->GetGPUVirtualAddress() +
                       kInstanceDataOffset + sizeof(GpuInstance));
            impl_->command_list->IASetVertexBuffers(0, 1, &impl_->vertex_view);
            impl_->command_list->DrawInstanced(
                static_cast<UINT>(kCubeVertices.size()),
                static_cast<UINT>(instance_count - 1), 0, 0);
        }
    });

    auto lighting_pass = impl_->graph.AddPass("Deferred Cel Lighting", QueueHint::Direct);
    lighting_pass.Read(gbuffer_base, Access::ShaderRead);
    lighting_pass.Read(gbuffer_normal, Access::ShaderRead);
    lighting_pass.Read(gbuffer_position, Access::ShaderRead);
    lighting_pass.Read(shadow, Access::ShaderRead);
    lighting_pass.Write(hdr, Access::RenderTarget);
    lighting_pass.SetExecute([&](RenderPassContext &) {
        constexpr float clear[] = {0, 0, 0, 0};
        impl_->command_list->ClearRenderTargetView(hdr_rtv, clear, 0, nullptr);
        draw_fullscreen(impl_->deferred_pipeline.Get(), hdr_rtv);
    });

    auto transparent_pass =
        impl_->graph.AddPass("Forward Transparent/OIT", QueueHint::Direct);
    transparent_pass.Read(depth, Access::DepthRead);
    transparent_pass.Read(particles, Access::ShaderRead);
    transparent_pass.Read(particle_alive_output, Access::ShaderRead);
    transparent_pass.Read(indirect_arguments, Access::IndirectArgs);
    transparent_pass.Read(gbuffer_position, Access::ShaderRead);
    transparent_pass.Read(gbuffer_normal, Access::ShaderRead);
    transparent_pass.Write(oit_accumulation, Access::RenderTarget);
    transparent_pass.Write(oit_revealage, Access::RenderTarget);
    transparent_pass.SetExecute([&](RenderPassContext &) {
        constexpr float clear_accumulation[] = {0, 0, 0, 0};
        constexpr float clear_revealage[] = {1, 1, 1, 1};
        impl_->command_list->ClearRenderTargetView(
            oit_accumulation_rtv, clear_accumulation, 0, nullptr);
        impl_->command_list->ClearRenderTargetView(
            oit_revealage_rtv, clear_revealage, 0, nullptr);
        const D3D12_CPU_DESCRIPTOR_HANDLE targets[] = {
            oit_accumulation_rtv, oit_revealage_rtv};
        impl_->command_list->OMSetRenderTargets(2, targets, FALSE, &dsv);
        impl_->command_list->SetPipelineState(impl_->particle_pipeline.Get());
        impl_->command_list->SetGraphicsRootShaderResourceView(
            2, impl_->particles.resource->GetGPUVirtualAddress());
        impl_->command_list->SetGraphicsRootShaderResourceView(
            12,
            impl_->particle_alive[particle_output_index].resource->GetGPUVirtualAddress());
        impl_->command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        impl_->command_list->ExecuteIndirect(
            impl_->draw_signature.Get(), 1, impl_->indirect_arguments.resource.Get(), 0, nullptr,
            0);
    });

    auto composite_pass = impl_->graph.AddPass("OIT Composite", QueueHint::Direct);
    composite_pass.Read(hdr, Access::ShaderRead);
    composite_pass.Read(oit_accumulation, Access::ShaderRead);
    composite_pass.Read(oit_revealage, Access::ShaderRead);
    composite_pass.Write(post_a, Access::RenderTarget);
    composite_pass.SetExecute([&](RenderPassContext &) {
        constexpr float clear[] = {0, 0, 0, 0};
        impl_->command_list->ClearRenderTargetView(post_a_rtv, clear, 0, nullptr);
        draw_fullscreen(impl_->composite_pipeline.Get(), post_a_rtv);
    });

    auto bloom_pass = impl_->graph.AddPass("Bloom", QueueHint::Direct);
    bloom_pass.Read(post_a, Access::ShaderRead);
    bloom_pass.Write(post_b, Access::RenderTarget);
    bloom_pass.SetExecute([&](RenderPassContext &) {
        constexpr float clear[] = {0, 0, 0, 0};
        impl_->command_list->ClearRenderTargetView(post_b_rtv, clear, 0, nullptr);
        draw_fullscreen(impl_->bloom_pipeline.Get(), post_b_rtv);
    });

    auto tone_map_pass = impl_->graph.AddPass("ToneMap", QueueHint::Direct);
    tone_map_pass.Read(post_b, Access::ShaderRead);
    tone_map_pass.Write(post_a, Access::RenderTarget);
    tone_map_pass.SetExecute([&](RenderPassContext &) {
        draw_fullscreen(impl_->tone_map_pipeline.Get(), post_a_rtv);
    });

    auto outline_pass =
        impl_->graph.AddPass("Screen-space Outline", QueueHint::Direct);
    outline_pass.Read(post_a, Access::ShaderRead);
    outline_pass.Read(gbuffer_position, Access::ShaderRead);
    outline_pass.Write(post_b, Access::RenderTarget);
    outline_pass.SetExecute([&](RenderPassContext &) {
        draw_fullscreen(impl_->outline_pipeline.Get(), post_b_rtv);
    });

    auto fxaa_pass = impl_->graph.AddPass("FXAA", QueueHint::Direct);
    fxaa_pass.Read(post_b, Access::ShaderRead);
    fxaa_pass.Write(back_buffer, Access::RenderTarget);
    fxaa_pass.SetExecute([&](RenderPassContext &) {
        impl_->command_list->RSSetViewports(1, &output_viewport);
        impl_->command_list->RSSetScissorRects(1, &output_scissor);
        draw_fullscreen(impl_->fxaa_pipeline.Get(), rtv);
    });

    auto ui_pass = impl_->graph.AddPass("Game UI", QueueHint::Direct);
    ui_pass.Read(ui, Access::ShaderRead);
    ui_pass.ReadWrite(back_buffer, Access::RenderTarget);
    ui_pass.SetExecute([&](RenderPassContext &) {
        (void)events;
        draw_fullscreen(impl_->ui_pipeline.Get(), rtv);
    });

    impl_->graph.SetFinalAccess(particles, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(particle_alive_input, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(particle_alive_output, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(particle_dead, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(particle_counters, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(indirect_arguments, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(shadow, Access::DepthWrite);
    impl_->graph.SetFinalAccess(depth, Access::DepthWrite);
    impl_->graph.SetFinalAccess(gbuffer_base, Access::ShaderRead);
    impl_->graph.SetFinalAccess(gbuffer_normal, Access::ShaderRead);
    impl_->graph.SetFinalAccess(gbuffer_position, Access::ShaderRead);
    impl_->graph.SetFinalAccess(hdr, Access::ShaderRead);
    impl_->graph.SetFinalAccess(oit_accumulation, Access::ShaderRead);
    impl_->graph.SetFinalAccess(oit_revealage, Access::ShaderRead);
    impl_->graph.SetFinalAccess(post_a, Access::ShaderRead);
    impl_->graph.SetFinalAccess(post_b, Access::ShaderRead);
#if defined(HS_DEVELOPMENT_TOOLS)
    impl_->graph.SetFinalAccess(back_buffer, Access::RenderTarget);
#else
    impl_->graph.SetFinalAccess(back_buffer, Access::Present);
#endif

    if (auto graph_result = impl_->graph.Execute(
            impl_->command_list.Get(), impl_->enhanced_command_list.Get(),
            impl_->enhanced ? BarrierMode::Enhanced : BarrierMode::Legacy,
            impl_->timestamp_heap.Get(), impl_->timestamp_readback.resource.Get(),
            back_buffer_index * kTimestampCountPerFrame);
        !graph_result)
    {
        return graph_result;
    }
#if defined(HS_DEVELOPMENT_TOOLS)
    if (impl_->config.devtools_visible)
    {
        impl_->command_list->RSSetViewports(1, &output_viewport);
        impl_->command_list->RSSetScissorRects(1, &output_scissor);
        impl_->command_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        ID3D12DescriptorHeap *imgui_heaps[] = {impl_->imgui_heap.Get()};
        impl_->command_list->SetDescriptorHeaps(1, imgui_heaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), impl_->command_list.Get());
    }
    impl_->TransitionTexture(impl_->back_buffers[back_buffer_index].Get(),
                             D3D12_RESOURCE_STATE_RENDER_TARGET,
                             D3D12_RESOURCE_STATE_PRESENT,
                             D3D12_BARRIER_LAYOUT_RENDER_TARGET,
                             D3D12_BARRIER_LAYOUT_PRESENT);
#endif
    impl_->transient_textures_common = false;
    impl_->particles_initialized = true;
    impl_->particle_input_is_a = !impl_->particle_input_is_a;
    impl_->last_particle_tick = snapshot.header.tick;
    frame.timestamps_recorded = true;

    result = impl_->command_list->Close();
    if (FAILED(result))
    {
        return HResultFailure("Close command list", result);
    }
    ID3D12CommandList *lists[] = {impl_->command_list.Get()};
    impl_->queue->ExecuteCommandLists(1, lists);
    impl_->last_presented_index = back_buffer_index;
    result = impl_->swap_chain->Present(impl_->config.vsync ? 1 : 0, 0);
    if (FAILED(result))
    {
        return impl_->CheckDevice(result, "Present");
    }

    frame.fence_value = impl_->next_fence++;
    result = impl_->queue->Signal(impl_->fence.Get(), frame.fence_value);
    if (FAILED(result))
    {
        return impl_->CheckDevice(result, "Signal frame fence");
    }

    ++impl_->frame_number;
    frame_result = {impl_->frame_number, snapshot.header.tick,
#if defined(HS_DEVELOPMENT_TOOLS)
                    impl_->config.devtools_visible && ImGui::GetIO().WantCaptureMouse,
                    impl_->config.devtools_visible && ImGui::GetIO().WantCaptureKeyboard,
#else
                    false, false,
#endif
                    debug_command, debug_value, debug_secondary};
    impl_->CountValidationErrors();
    return Result::Success();
}

Result D3D12Renderer::Impl::WritePng(const std::filesystem::path &path,
                                     const std::byte *pixels, std::uint32_t row_pitch) const
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);

    ComPtr<IWICImagingFactory> imaging_factory;
    auto result = CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&imaging_factory));
    if (FAILED(result))
    {
        return HResultFailure("Create WIC factory", result);
    }
    ComPtr<IWICStream> stream;
    result = imaging_factory->CreateStream(&stream);
    if (FAILED(result))
    {
        return HResultFailure("Create WIC stream", result);
    }
    result = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (FAILED(result))
    {
        return HResultFailure("Open PNG output", result);
    }
    ComPtr<IWICBitmapEncoder> encoder;
    result = imaging_factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(result))
    {
        return HResultFailure("Create PNG encoder", result);
    }
    result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(result))
    {
        return HResultFailure("Initialize PNG encoder", result);
    }
    ComPtr<IWICBitmapFrameEncode> frame;
    result = encoder->CreateNewFrame(&frame, nullptr);
    if (FAILED(result) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(width, height)))
    {
        return HResultFailure("Initialize PNG frame", FAILED(result) ? result : E_FAIL);
    }
    auto format = GUID_WICPixelFormat32bppRGBA;
    result = frame->SetPixelFormat(&format);
    if (FAILED(result))
    {
        return HResultFailure("Set PNG format", result);
    }
    result = frame->WritePixels(height, row_pitch, row_pitch * height,
                                const_cast<BYTE *>(reinterpret_cast<const BYTE *>(pixels)));
    if (FAILED(result) || FAILED(frame->Commit()) || FAILED(encoder->Commit()))
    {
        return HResultFailure("Write PNG", FAILED(result) ? result : E_FAIL);
    }
    return Result::Success();
}

Result D3D12Renderer::CapturePng(const std::filesystem::path &path)
{
    if (!impl_->initialized)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Renderer not initialized.");
    }
    if (auto result = impl_->WaitForGpu(); !result)
    {
        return result;
    }

    const auto description = impl_->back_buffers[impl_->last_presented_index]->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows{};
    UINT64 row_size{};
    UINT64 total_size{};
    impl_->device->GetCopyableFootprints(&description, 0, 1, 0, &footprint, &rows, &row_size,
                                         &total_size);

    AllocationResource readback;
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_READBACK;
    if (auto result = impl_->CreateAllocation(
            readback, allocation,
            BufferDescription(total_size + sizeof(D3D12_DRAW_ARGUMENTS)),
            D3D12_RESOURCE_STATE_COPY_DEST);
        !result)
    {
        return result;
    }

    auto &frame = impl_->frames[0];
    auto result = frame.allocator->Reset();
    if (FAILED(result))
    {
        return HResultFailure("Reset capture allocator", result);
    }
    result = impl_->command_list->Reset(frame.allocator.Get(), nullptr);
    if (FAILED(result))
    {
        return HResultFailure("Reset capture list", result);
    }
    impl_->TransitionTexture(impl_->back_buffers[impl_->last_presented_index].Get(),
                             D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE,
                             D3D12_BARRIER_LAYOUT_PRESENT,
                             D3D12_BARRIER_LAYOUT_COPY_SOURCE);

    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback.resource.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = impl_->back_buffers[impl_->last_presented_index].Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    impl_->command_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    impl_->TransitionBuffer(
        impl_->indirect_arguments.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_BARRIER_SYNC_COMPUTE_SHADING,
        D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
        D3D12_BARRIER_ACCESS_COPY_SOURCE);
    impl_->command_list->CopyBufferRegion(readback.resource.Get(), total_size,
                                          impl_->indirect_arguments.resource.Get(), 0,
                                          sizeof(D3D12_DRAW_ARGUMENTS));
    impl_->TransitionBuffer(
        impl_->indirect_arguments.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_BARRIER_SYNC_COPY,
        D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_COPY_SOURCE,
        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
    impl_->TransitionTexture(impl_->back_buffers[impl_->last_presented_index].Get(),
                             D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT,
                             D3D12_BARRIER_LAYOUT_COPY_SOURCE,
                             D3D12_BARRIER_LAYOUT_PRESENT);
    impl_->command_list->Close();
    ID3D12CommandList *lists[] = {impl_->command_list.Get()};
    impl_->queue->ExecuteCommandLists(1, lists);
    if (auto wait = impl_->WaitForGpu(); !wait)
    {
        return wait;
    }

    void *mapped_data{};
    D3D12_RANGE read_range{
        0, static_cast<SIZE_T>(total_size + sizeof(D3D12_DRAW_ARGUMENTS))};
    result = readback.resource->Map(0, &read_range, &mapped_data);
    if (FAILED(result))
    {
        return HResultFailure("Map capture", result);
    }
    const auto write = impl_->WritePng(path, static_cast<const std::byte *>(mapped_data),
                                       footprint.Footprint.RowPitch);
    const auto *draw_arguments = reinterpret_cast<const D3D12_DRAW_ARGUMENTS *>(
        static_cast<const std::byte *>(mapped_data) + total_size);
    impl_->last_particle_count = draw_arguments->InstanceCount;
    std::ofstream(path.parent_path() / "particle_stats.json", std::ios::trunc)
        << std::format("{{\"alive\":{},\"capacity\":{}}}\n", impl_->last_particle_count,
                       kParticleCount * std::clamp(impl_->config.particle_percentage, 50u,
                                                   100u) /
                           100u);
    D3D12_RANGE no_write{};
    readback.resource->Unmap(0, &no_write);
    return write;
}

Result D3D12Renderer::Shutdown()
{
    if (!impl_)
    {
        return Result::Success();
    }
    Result result = Result::Success();
    if (impl_->initialized)
    {
        result = impl_->WaitForGpu();
        impl_->CountValidationErrors();
    }
#if defined(HS_DEVELOPMENT_TOOLS)
    if (impl_->imgui_initialized)
    {
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        impl_->imgui_initialized = false;
    }
    impl_->imgui_heap.Reset();
#endif
    for (auto &frame : impl_->frames)
    {
        if (frame.upload.resource && frame.mapped)
        {
            frame.upload.resource->Unmap(0, nullptr);
            frame.mapped = nullptr;
        }
        if (frame.ui_upload.resource && frame.ui_mapped)
        {
            frame.ui_upload.resource->Unmap(0, nullptr);
            frame.ui_mapped = nullptr;
        }
        frame.upload.Reset();
        frame.ui_upload.Reset();
    }
    impl_->depth.Reset();
    impl_->shadow.Reset();
    impl_->vertices.Reset();
    impl_->archer_vertices.Reset();
    impl_->archer_diffuse.Reset();
    impl_->archer_normal.Reset();
    impl_->vfx_masks.Reset();
    impl_->particles.Reset();
    for (auto &alive : impl_->particle_alive)
    {
        alive.Reset();
    }
    impl_->particle_dead.Reset();
    impl_->particle_counters.Reset();
    impl_->indirect_arguments.Reset();
    impl_->gbuffer_base.Reset();
    impl_->gbuffer_normal.Reset();
    impl_->gbuffer_position.Reset();
    impl_->hdr_color.Reset();
    impl_->oit_accumulation.Reset();
    impl_->oit_revealage.Reset();
    impl_->post_a.Reset();
    impl_->post_b.Reset();
    for (auto &texture : impl_->ui_textures)
    {
        texture.Reset();
    }
    for (auto &surface : impl_->ui_surfaces)
    {
        surface.brush.Reset();
        surface.target.Reset();
        surface.bitmap.Reset();
    }
    impl_->ui_font_collection.Reset();
    impl_->ui_dwrite_factory.Reset();
    impl_->ui_d2d_factory.Reset();
    impl_->ui_wic_factory.Reset();
    if (impl_->timestamp_readback.resource && impl_->mapped_timestamps)
    {
        D3D12_RANGE no_write{};
        impl_->timestamp_readback.resource->Unmap(0, &no_write);
        impl_->mapped_timestamps = nullptr;
    }
    impl_->timestamp_readback.Reset();
    impl_->timestamp_heap.Reset();
    for (auto &buffer : impl_->back_buffers)
    {
        buffer.Reset();
    }
    if (impl_->allocator)
    {
        impl_->allocator->Release();
        impl_->allocator = nullptr;
    }
    if (impl_->fence_event)
    {
        CloseHandle(impl_->fence_event);
        impl_->fence_event = nullptr;
    }
    impl_->initialized = false;
    if (impl_->com_initialized)
    {
        CoUninitialize();
        impl_->com_initialized = false;
    }
    return result;
}

bool D3D12Renderer::UsesEnhancedBarriers() const noexcept
{
    return impl_->enhanced;
}

std::uint64_t D3D12Renderer::ValidationErrorCount() const noexcept
{
    return impl_->validation_errors;
}

std::uint32_t D3D12Renderer::LastParticleCount() const noexcept
{
    return impl_->last_particle_count;
}

GpuPassTimings D3D12Renderer::LastGpuPassTimings() const noexcept
{
    GpuPassTimings timings;
    if (!impl_->mapped_timestamps || impl_->timestamp_frequency == 0 ||
        !impl_->frames[impl_->last_presented_index].timestamps_recorded)
    {
        return timings;
    }
    const auto base = impl_->last_presented_index * kTimestampCountPerFrame;
    for (std::size_t pass = 0; pass < timings.nanoseconds.size(); ++pass)
    {
        const auto begin = impl_->mapped_timestamps[base + pass * 2];
        const auto end = impl_->mapped_timestamps[base + pass * 2 + 1];
        if (end < begin)
        {
            return {};
        }
        timings.nanoseconds[pass] =
            (end - begin) * 1'000'000'000ull / impl_->timestamp_frequency;
    }
    timings.valid = true;
    return timings;
}

} // namespace hs
