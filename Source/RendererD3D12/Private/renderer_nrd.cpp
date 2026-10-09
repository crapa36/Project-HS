#include "renderer_nrd.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>

#ifndef HS_WITH_NRD
#define HS_WITH_NRD 0
#endif

#if HS_WITH_NRD
#include "d3d12_resources.hpp"
#include <Windows.h>
#include <NRD.h>
static_assert(NRD_VERSION_MAJOR == 4 && NRD_VERSION_MINOR == 17 && NRD_VERSION_BUILD == 3,
    "The native NRD adapter requires the pinned 4.17.3 headers.");
#endif

namespace hs
{
namespace
{
Result Failure(ErrorCode code, std::string message)
{
    return Result::Failure(code, "hs_renderer_nrd", std::move(message));
}
bool Enabled(const NrdSettings &settings)
{
    return settings.radiance != NrdRadianceMethod::Off || settings.ambient_occlusion || settings.shadow;
}
#if HS_WITH_NRD
using Microsoft::WRL::ComPtr;
constexpr nrd::Identifier kSelectedDenoiser = 1;
nrd::Denoiser NativeDenoiser(NrdSelection selection)
{
    if (selection.signal == NrdSignal::AmbientOcclusion) return nrd::Denoiser::REBLUR_DIFFUSE_OCCLUSION;
    if (selection.signal == NrdSignal::Shadow) return nrd::Denoiser::SIGMA_SHADOW;
    if (selection.method == NrdRadianceMethod::Relax)
        return selection.signal == NrdSignal::Diffuse ? nrd::Denoiser::RELAX_DIFFUSE :
            selection.signal == NrdSignal::Specular ? nrd::Denoiser::RELAX_SPECULAR : nrd::Denoiser::RELAX_DIFFUSE_SPECULAR;
    return selection.signal == NrdSignal::Diffuse ? nrd::Denoiser::REBLUR_DIFFUSE :
        selection.signal == NrdSignal::Specular ? nrd::Denoiser::REBLUR_SPECULAR : nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR;
}
bool SelectedOutput(NrdSelection selection, std::uint32_t index)
{
    return (index == 0 && (selection.signal == NrdSignal::Diffuse || selection.signal == NrdSignal::DiffuseSpecular)) ||
        (index == 1 && (selection.signal == NrdSignal::Specular || selection.signal == NrdSignal::DiffuseSpecular)) ||
        (index == 2 && selection.signal == NrdSignal::AmbientOcclusion) || (index == 3 && selection.signal == NrdSignal::Shadow);
}
constexpr std::uint32_t kOutputCount = 4;
constexpr std::array<DXGI_FORMAT, kOutputCount> kOutputFormats{
    DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT};

Result NativeFailure(std::string_view operation, HRESULT native)
{
    return Failure(ErrorCode::TaskFailed, std::string(operation) + " failed (HRESULT " +
        std::to_string(static_cast<std::int32_t>(native)) + ").");
}
Result NrdFailure(std::string_view operation, nrd::Result native)
{
    return Failure(ErrorCode::TaskFailed, std::string(operation) + " failed (NRD result " +
        std::to_string(static_cast<std::uint32_t>(native)) + ").");
}
DXGI_FORMAT NativeFormat(nrd::Format format)
{
#define HS_NRD_FORMAT(nrd_format, dxgi_format) case nrd::Format::nrd_format: return DXGI_FORMAT_##dxgi_format
    switch (format)
    {
    HS_NRD_FORMAT(R8_UNORM, R8_UNORM); HS_NRD_FORMAT(R8_SNORM, R8_SNORM);
    HS_NRD_FORMAT(R8_UINT, R8_UINT); HS_NRD_FORMAT(R8_SINT, R8_SINT);
    HS_NRD_FORMAT(RG8_UNORM, R8G8_UNORM); HS_NRD_FORMAT(RG8_SNORM, R8G8_SNORM);
    HS_NRD_FORMAT(RG8_UINT, R8G8_UINT); HS_NRD_FORMAT(RG8_SINT, R8G8_SINT);
    HS_NRD_FORMAT(RGBA8_UNORM, R8G8B8A8_UNORM); HS_NRD_FORMAT(RGBA8_SNORM, R8G8B8A8_SNORM);
    HS_NRD_FORMAT(RGBA8_UINT, R8G8B8A8_UINT); HS_NRD_FORMAT(RGBA8_SINT, R8G8B8A8_SINT);
    HS_NRD_FORMAT(RGBA8_SRGB, R8G8B8A8_UNORM_SRGB);
    HS_NRD_FORMAT(R16_UNORM, R16_UNORM); HS_NRD_FORMAT(R16_SNORM, R16_SNORM);
    HS_NRD_FORMAT(R16_UINT, R16_UINT); HS_NRD_FORMAT(R16_SINT, R16_SINT); HS_NRD_FORMAT(R16_SFLOAT, R16_FLOAT);
    HS_NRD_FORMAT(RG16_UNORM, R16G16_UNORM); HS_NRD_FORMAT(RG16_SNORM, R16G16_SNORM);
    HS_NRD_FORMAT(RG16_UINT, R16G16_UINT); HS_NRD_FORMAT(RG16_SINT, R16G16_SINT); HS_NRD_FORMAT(RG16_SFLOAT, R16G16_FLOAT);
    HS_NRD_FORMAT(RGBA16_UNORM, R16G16B16A16_UNORM); HS_NRD_FORMAT(RGBA16_SNORM, R16G16B16A16_SNORM);
    HS_NRD_FORMAT(RGBA16_UINT, R16G16B16A16_UINT); HS_NRD_FORMAT(RGBA16_SINT, R16G16B16A16_SINT);
    HS_NRD_FORMAT(RGBA16_SFLOAT, R16G16B16A16_FLOAT);
    HS_NRD_FORMAT(R32_UINT, R32_UINT); HS_NRD_FORMAT(R32_SINT, R32_SINT); HS_NRD_FORMAT(R32_SFLOAT, R32_FLOAT);
    HS_NRD_FORMAT(RG32_UINT, R32G32_UINT); HS_NRD_FORMAT(RG32_SINT, R32G32_SINT); HS_NRD_FORMAT(RG32_SFLOAT, R32G32_FLOAT);
    HS_NRD_FORMAT(RGB32_UINT, R32G32B32_UINT); HS_NRD_FORMAT(RGB32_SINT, R32G32B32_SINT); HS_NRD_FORMAT(RGB32_SFLOAT, R32G32B32_FLOAT);
    HS_NRD_FORMAT(RGBA32_UINT, R32G32B32A32_UINT); HS_NRD_FORMAT(RGBA32_SINT, R32G32B32A32_SINT);
    HS_NRD_FORMAT(RGBA32_SFLOAT, R32G32B32A32_FLOAT);
    HS_NRD_FORMAT(R10_G10_B10_A2_UNORM, R10G10B10A2_UNORM); HS_NRD_FORMAT(R10_G10_B10_A2_UINT, R10G10B10A2_UINT);
    HS_NRD_FORMAT(R11_G11_B10_UFLOAT, R11G11B10_FLOAT); HS_NRD_FORMAT(R9_G9_B9_E5_UFLOAT, R9G9B9E5_SHAREDEXP);
    default: return DXGI_FORMAT_UNKNOWN;
    }
#undef HS_NRD_FORMAT
}
bool ValidExtent(ViewExtent extent)
{
    return extent.width && extent.height && extent.width <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
        extent.height <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
}
bool SameDevice(ID3D12Device *device, ID3D12DeviceChild *child)
{
    ComPtr<ID3D12Device> candidate;
    return child && SUCCEEDED(child->GetDevice(IID_PPV_ARGS(&candidate))) && candidate.Get() == device;
}
bool Fits(ID3D12Device *device, const NrdNativeResource &resource, ViewExtent capacity, DXGI_FORMAT format)
{
    if (!resource.version.resource || resource.version.resource.kind != ResourceKind::Texture || !SameDevice(device, resource.resource)) return false;
    const auto desc = resource.resource->GetDesc();
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Width == capacity.width && desc.Height == capacity.height &&
        desc.DepthOrArraySize == 1 && desc.MipLevels == 1 && desc.SampleDesc.Count == 1 && desc.Format == format &&
        !(desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
}
bool ValidMatrix(const DirectX::XMFLOAT4X4 &matrix)
{
    for (const auto &row : matrix.m) for (const auto value : row) if (!std::isfinite(value)) return false;
    const auto determinant = DirectX::XMVectorGetX(DirectX::XMMatrixDeterminant(DirectX::XMLoadFloat4x4(&matrix)));
    return std::isfinite(determinant) && determinant != 0.0f;
}
void ColumnVectorMatrix(float (&destination)[16], const DirectX::XMFLOAT4X4 &source)
{
    // NRD uses column vectors and column-major memory; DirectX packet matrices
    // use row vectors and row-major memory. Logical transpose + storage change
    // cancel in memory. Do not memcpy an XMMatrixTranspose result into NRD.
    for (unsigned column = 0; column != 4; ++column)
        for (unsigned row = 0; row != 4; ++row) destination[column * 4 + row] = source.m[column][row];
}
bool ValidSettings(const NrdSettings &settings)
{
    const auto history_limit = settings.radiance == NrdRadianceMethod::Relax && !settings.ambient_occlusion ?
        nrd::RELAX_MAX_HISTORY_FRAME_NUM : nrd::REBLUR_MAX_HISTORY_FRAME_NUM;
    return (settings.radiance == NrdRadianceMethod::Off || settings.radiance == NrdRadianceMethod::Reblur || settings.radiance == NrdRadianceMethod::Relax) &&
        std::isfinite(settings.denoising_range_m) && settings.denoising_range_m > 0 &&
        std::isfinite(settings.disocclusion_threshold) && settings.disocclusion_threshold > 0 && settings.disocclusion_threshold <= 1 &&
        std::isfinite(settings.hit_distance_A) && settings.hit_distance_A > 0 &&
        std::isfinite(settings.hit_distance_B) && settings.hit_distance_B > 0 &&
        std::isfinite(settings.hit_distance_C) && settings.hit_distance_C >= 1 &&
        settings.radiance_history_frames <= history_limit &&
        settings.fast_history_frames <= settings.radiance_history_frames && settings.shadow_history_frames <= nrd::SIGMA_MAX_HISTORY_FRAME_NUM &&
        std::isfinite(settings.shadow_light_direction.x) && std::isfinite(settings.shadow_light_direction.y) && std::isfinite(settings.shadow_light_direction.z);
}
Result Allocate(D3D12MA::Allocator *allocator, D3D12_HEAP_TYPE heap_type, D3D12_RESOURCE_DESC desc,
    D3D12_RESOURCE_STATES initial, AllocationResource &resource)
{
    D3D12MA::ALLOCATION_DESC allocation{}; allocation.HeapType = heap_type;
    const auto native = allocator->CreateResource(&allocation, &desc, initial, nullptr, &resource.allocation,
        IID_PPV_ARGS(resource.resource.ReleaseAndGetAddressOf()));
    return FAILED(native) ? NativeFailure("Allocate NRD resource", native) : Result::Success();
}
Result CreateTexture(ID3D12Device *device, D3D12MA::Allocator *allocator, ViewExtent extent,
    DXGI_FORMAT format, AllocationResource &resource)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
    auto native = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
    if (FAILED(native)) return NativeFailure("Query NRD native pool format", native);
    if (!(support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) || !(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD) ||
        !(support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) ||
        !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) || !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
        return Failure(ErrorCode::TaskFailed, "The selected device does not support an actual NRD pool SRV/UAV format.");
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = extent.width; desc.Height = extent.height; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.Format = format; desc.SampleDesc.Count = 1; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (auto result = Allocate(allocator, D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON, resource); !result) return result;
    resource.graph_access = Access::Common;
    return Result::Success();
}
#endif
} // namespace

NrdRadianceMethod ResolveRayDenoiser(const RayDenoiserRequest &request) noexcept
{
#if HS_WITH_NRD
    if (request.enabled == GraphicsAutoToggle::Off || request.provider.value == "off" || request.provider.value == "internal")
        return NrdRadianceMethod::Off;
    if (request.provider.value == "nrd_relax") return NrdRadianceMethod::Relax;
    if (request.provider.value == "auto" || request.provider.value == "nrd" || request.provider.value == "nrd_reblur")
        return NrdRadianceMethod::Reblur;
#else
    static_cast<void>(request);
#endif
    return NrdRadianceMethod::Off;
}
RayDenoiserStatus DescribeRayDenoiser(const RayDenoiserRequest &request)
{
    RayDenoiserStatus status;status.requested_provider=request.provider;
    if(request.enabled==GraphicsAutoToggle::Off||request.provider.value=="off"||request.provider.value=="internal")
    {status.availability=ReconstructionAvailability::Available;status.reason="Internal ray signal filters selected.";return status;}
    if(request.provider.value!="auto"&&request.provider.value!="nrd"&&request.provider.value!="nrd_reblur"&&request.provider.value!="nrd_relax")
    {status.availability=ReconstructionAvailability::Unsupported;status.reason="Unknown ray denoiser provider: "+request.provider.value;return status;}
#if HS_WITH_NRD
    status.reason="No active native ray signal instance has been prepared.";
#else
    status.availability=ReconstructionAvailability::CompiledOut;status.reason="NRD was explicitly excluded by HS_WITH_NRD=OFF; internal ray filters are active.";
#endif
    return status;
}

RayDenoiserStatus QueryNrdCapability(ID3D12Device *device, NrdSelection selection)
{
    RayDenoiserStatus status;
    status.requested_provider.value = selection.method == NrdRadianceMethod::Relax ? "nrd_relax" : "nrd_reblur";
#if !HS_WITH_NRD
    static_cast<void>(device);
    status.availability = ReconstructionAvailability::CompiledOut;
    status.reason = "NRD was explicitly excluded by HS_WITH_NRD=OFF.";
#else
    const auto unsupported = [&](std::string reason) {
        status.availability = ReconstructionAvailability::Unsupported;
        status.reason = std::move(reason);
    };
    if (!device) { status.reason = "NRD capability query requires the actual device."; return status; }
    D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_0};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model))) ||
        model.HighestShaderModel < D3D_SHADER_MODEL_6_0)
    { unsupported("NRD requires Shader Model 6.0 on the selected device."); return status; }
    // These are the integrated NRD guide/output formats, not a vendor-ID guess.
    for (const auto format : {DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R10G10B10A2_UNORM,
                             DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16_FLOAT})
    {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
            !(support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) || !(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD) ||
            !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
        { unsupported("The selected device lacks an integrated NRD guide/output format."); return status; }
    }
    std::wstring executable(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size())
    { status.reason = "Cannot resolve pinned NRD runtime directory."; return status; }
    executable.resize(length);
    const auto runtime = std::filesystem::path(executable).parent_path() / L"NRD.dll";
    const auto module = LoadLibraryExW(runtime.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module)
    { status.reason = "Cannot load pinned NRD.dll (Win32 " + std::to_string(GetLastError()) + ")."; return status; }
    const auto query = reinterpret_cast<decltype(&nrd::GetLibraryDesc)>(GetProcAddress(module, "GetLibraryDesc"));
    const auto *library = query ? query() : nullptr;
    bool exports = query != nullptr;
    for (const auto *name : {"CreateInstance", "DestroyInstance", "GetInstanceDesc", "SetCommonSettings",
                             "SetDenoiserSettings", "GetComputeDispatches"})
        exports = exports && GetProcAddress(module, name) != nullptr;
    if (!exports) status.reason = "Pinned NRD.dll is missing a required official ABI export.";
    else if (!library || library->versionMajor != NRD_VERSION_MAJOR || library->versionMinor != NRD_VERSION_MINOR ||
        library->versionBuild != NRD_VERSION_BUILD)
        status.reason = "Pinned NRD library/header versions do not match 4.17.3.";
    else if (library->normalEncoding != nrd::NormalEncoding::R10_G10_B10_A2_UNORM ||
             library->roughnessEncoding != nrd::RoughnessEncoding::LINEAR)
        unsupported("NRD library does not support the integrated guide encodings.");
    else if (!library->supportedDenoisers || std::find(library->supportedDenoisers,
        library->supportedDenoisers + library->supportedDenoisersNum, NativeDenoiser(selection)) ==
        library->supportedDenoisers + library->supportedDenoisersNum)
        unsupported("NRD library does not support the requested native denoiser.");
    else { status.availability = ReconstructionAvailability::Available; status.reason.clear(); }
    if (!FreeLibrary(module))
    { status.availability = ReconstructionAvailability::Unavailable; status.reason = "Cannot unload NRD capability-query runtime."; }
#endif
    return status;
}

struct NrdStorage
{
    NrdSelection selection;
    std::uint64_t bytes{};
    std::vector<std::string> shader_products;
#if HS_WITH_NRD
    HMODULE module{};
    nrd::Instance *instance{};
    decltype(&nrd::CreateInstance) create_instance{};
    decltype(&nrd::DestroyInstance) destroy_instance{};
    decltype(&nrd::GetLibraryDesc) library_desc{};
    decltype(&nrd::GetInstanceDesc) instance_desc{};
    decltype(&nrd::SetCommonSettings) common_settings{};
    decltype(&nrd::SetDenoiserSettings) denoiser_settings{};
    decltype(&nrd::GetComputeDispatches) dispatches{};
    DWORD owner_thread{};
    ComPtr<ID3D12Device> device;
    ComPtr<D3D12MA::Allocator> allocator;
    ViewExtent capacity, previous_active;
    std::uint32_t frames{}, dispatch_capacity{}, descriptors_per_dispatch{}, descriptors_per_frame{}, constant_stride{}, constant_data_max{};
    ComPtr<ID3D12DescriptorHeap> descriptors, exports;
    std::uint32_t descriptor_stride{};
    AllocationResource constants;
    std::byte *mapped{};
    struct Pipeline
    {
        ComPtr<ID3D12RootSignature> root;
        ComPtr<ID3D12PipelineState> pso;
        std::vector<nrd::ResourceRangeDesc> ranges;
        std::vector<std::uint32_t> table_parameters;
        std::uint32_t constant_parameter{UINT32_MAX}, descriptor_count{};
    };
    std::vector<Pipeline> pipelines;
    std::vector<AllocationResource> permanent, transient;
    std::array<AllocationResource, kOutputCount> outputs;
    CompletionSet completion, observed;
    std::vector<CompletionSet> slot_completion;
    struct SlotLease
    {
        std::vector<ComPtr<ID3D12Resource>> resources;
        std::vector<std::shared_ptr<const void>> owners;
    };
    std::vector<SlotLease> slot_leases;
    bool usable{true}, history_valid{};
    std::uint64_t previous_render_id{};
    std::uint32_t previous_methods{};
    float previous_jitter[2]{};
    std::array<float, 4> previous_normalization{};

    ~NrdStorage()
    {
        if (mapped) constants.resource->Unmap(0, nullptr);
        if (instance) destroy_instance(*instance);
        if (module && !FreeLibrary(module)) std::fprintf(stderr, "hs.nrd.shutdown FreeLibrary failed Win32=%lu\n", GetLastError());
    }
    template <typename Function> bool Load(Function &function, const char *name)
    {
        function = reinterpret_cast<Function>(GetProcAddress(module, name));
        return function != nullptr;
    }
    Result RequireOwner() const
    {
        return owner_thread == GetCurrentThreadId() ? Result::Success() :
            Failure(ErrorCode::InvalidState, "NRD native API called outside the Prepare owner thread.");
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(std::uint32_t index) const
    {
        auto value = descriptors->GetCPUDescriptorHandleForHeapStart(); value.ptr += SIZE_T(index) * descriptor_stride; return value;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(std::uint32_t index) const
    {
        auto value = descriptors->GetGPUDescriptorHandleForHeapStart(); value.ptr += UINT64(index) * descriptor_stride; return value;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Export(std::uint32_t index) const
    {
        auto value = exports->GetCPUDescriptorHandleForHeapStart(); value.ptr += SIZE_T(index) * descriptor_stride; return value;
    }
    void View(ID3D12Resource *resource, nrd::DescriptorType type, D3D12_CPU_DESCRIPTOR_HANDLE destination) const
    {
        if (type == nrd::DescriptorType::TEXTURE)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{}; view.Format = resource->GetDesc().Format;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2D.MipLevels = 1; device->CreateShaderResourceView(resource, &view, destination);
        }
        else
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC view{}; view.Format = resource->GetDesc().Format;
            view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; device->CreateUnorderedAccessView(resource, nullptr, &view, destination);
        }
    }
#endif
};

struct NrdSystem::Impl
{
    std::shared_ptr<NrdStorage> storage;
    ReconstructionAvailability availability{
#if HS_WITH_NRD
        ReconstructionAvailability::Unavailable
#else
        ReconstructionAvailability::CompiledOut
#endif
    };
    std::string reason{
#if HS_WITH_NRD
        "NRD has not been prepared."
#else
        "NRD was explicitly excluded by HS_WITH_NRD=OFF."
#endif
    };
    struct Pending
    {
        const RenderGraphBuilder *graph{};
        std::shared_ptr<NrdStorage> storage;
#if HS_WITH_NRD
        struct Import { AllocationResource *allocation{}; ResourceHandle handle; };
        std::vector<Import> imports;
        std::vector<ResourceHandle> input_handles;
        std::uint32_t slot{}, methods{};
        std::uint64_t render_id{};
        ViewExtent active;
        float jitter[2]{};
        std::array<float, 4> normalization{};
#endif
    } pending;
};

NrdSystem::NrdSystem() : impl_(std::make_unique<Impl>()) {}
NrdSystem::~NrdSystem()
{
#if HS_WITH_NRD
    if (impl_->pending.graph || (impl_->storage && !impl_->storage->completion.IsSatisfiedBy(impl_->storage->observed)))
    {
        std::fprintf(stderr, "hs.nrd.shutdown NRD leases were not completed before destruction\n");
        static_cast<void>(impl_.release());
    }
#endif
}
std::uint64_t NrdCandidate::AllocatedBytes() const noexcept { return storage_ ? storage_->bytes : 0; }
std::span<const std::string> NrdCandidate::ShaderProducts() const noexcept
{
    return storage_ ? std::span<const std::string>(storage_->shader_products) : std::span<const std::string>();
}

Result NrdSystem::Prepare(ID3D12Device *device, D3D12MA::Allocator *allocator, ViewExtent extent,
    std::uint32_t frame_slots, const ShaderReader &read_shader, NrdCandidate &candidate, NrdSelection selection) const
{
    candidate = {};
#if !HS_WITH_NRD
    static_cast<void>(device); static_cast<void>(allocator); static_cast<void>(extent);
    static_cast<void>(frame_slots); static_cast<void>(read_shader);
    static_cast<void>(selection);
    return Failure(ErrorCode::InvalidState, impl_->reason);
#else
    const auto failed = [&](Result result, ReconstructionAvailability availability = ReconstructionAvailability::Unavailable) -> Result
    {
        // Candidate preparation is transactional; a failed replacement leaves
        // the successfully committed native service and its status intact.
        if (!impl_->storage || !impl_->storage->usable) { impl_->availability = availability; impl_->reason = result.Message(); }
        return result;
    };
    if (!device || !allocator || !ValidExtent(extent) || !frame_slots || !read_shader || selection.method == NrdRadianceMethod::Off ||
        (selection.signal == NrdSignal::AmbientOcclusion && selection.method != NrdRadianceMethod::Reblur))
        return failed(Failure(ErrorCode::InvalidArgument, "NRD Prepare requires a device, allocator, extent, frame slots and DXIL shader reader."));
    auto storage = std::make_shared<NrdStorage>(); storage->device = device; storage->allocator = allocator;
    storage->capacity = extent; storage->frames = frame_slots; storage->owner_thread = GetCurrentThreadId();
    storage->selection = selection;
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size()) return failed(Failure(ErrorCode::TaskFailed, "Cannot resolve pinned NRD runtime directory."));
    executable.resize(length);
    const auto runtime = std::filesystem::path(executable).parent_path() / L"NRD.dll";
    storage->module = LoadLibraryExW(runtime.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!storage->module) return failed(Failure(ErrorCode::TaskFailed, "Cannot load pinned NRD.dll (Win32 " + std::to_string(GetLastError()) + ")."));
    if (!storage->Load(storage->create_instance, "CreateInstance") || !storage->Load(storage->destroy_instance, "DestroyInstance") ||
        !storage->Load(storage->library_desc, "GetLibraryDesc") || !storage->Load(storage->instance_desc, "GetInstanceDesc") ||
        !storage->Load(storage->common_settings, "SetCommonSettings") || !storage->Load(storage->denoiser_settings, "SetDenoiserSettings") ||
        !storage->Load(storage->dispatches, "GetComputeDispatches"))
        return failed(Failure(ErrorCode::TaskFailed, "Pinned NRD.dll is missing a required official ABI export."));
    const auto *library = storage->library_desc();
    if (!library || library->versionMajor != NRD_VERSION_MAJOR || library->versionMinor != NRD_VERSION_MINOR || library->versionBuild != NRD_VERSION_BUILD)
        return failed(Failure(ErrorCode::InvalidState, "Pinned NRD library/header versions do not match 4.17.3."));
    if (library->normalEncoding != nrd::NormalEncoding::R10_G10_B10_A2_UNORM || library->roughnessEncoding != nrd::RoughnessEncoding::LINEAR)
        return failed(Failure(ErrorCode::InvalidState, "NRD producers require actual library normal/roughness encodings 2/1."), ReconstructionAvailability::Unsupported);
    const nrd::DenoiserDesc denoiser{kSelectedDenoiser, NativeDenoiser(selection)};
    if (!library->supportedDenoisers || std::find(library->supportedDenoisers, library->supportedDenoisers + library->supportedDenoisersNum,
        denoiser.denoiser) == library->supportedDenoisers + library->supportedDenoisersNum)
        return failed(Failure(ErrorCode::InvalidState, "Pinned NRD does not support the selected native denoiser."), ReconstructionAvailability::Unsupported);
    nrd::InstanceCreationDesc creation{}; creation.denoisers = &denoiser; creation.denoisersNum = 1;
    const auto created = storage->create_instance(creation, storage->instance);
    if (created != nrd::Result::SUCCESS) return failed(NrdFailure("CreateInstance", created), created == nrd::Result::UNSUPPORTED ?
        ReconstructionAvailability::Unsupported : ReconstructionAvailability::Unavailable);
    if (!storage->instance) return failed(Failure(ErrorCode::TaskFailed, "NRD CreateInstance returned no instance."));
    const auto *description = storage->instance_desc(*storage->instance);
    if (!description || !description->pipelines || !description->pipelinesNum || !description->constantBufferMaxDataSize ||
        !description->descriptorPoolDesc.setsMaxNum || !description->samplers || !description->samplersNum ||
        description->samplersNum > D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE)
        return failed(Failure(ErrorCode::TaskFailed, "NRD returned invalid native instance descriptions."));
    storage->dispatch_capacity = description->descriptorPoolDesc.setsMaxNum;
    const auto table_size = UINT64(description->descriptorPoolDesc.perSetTexturesMaxNum) + description->descriptorPoolDesc.perSetStorageTexturesMaxNum;
    const auto frame_descriptors = UINT64(storage->dispatch_capacity) * table_size;
    if (!table_size || frame_descriptors > D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1 / frame_slots)
        return failed(Failure(ErrorCode::InvalidArgument, "NRD per-frame descriptor/dispatch requirements exceed native limits."));
    storage->descriptors_per_dispatch = static_cast<std::uint32_t>(table_size);
    storage->descriptors_per_frame = static_cast<std::uint32_t>(frame_descriptors);
    storage->constant_stride = (description->constantBufferMaxDataSize + 255U) & ~255U;
    storage->constant_data_max = description->constantBufferMaxDataSize;
    if (!storage->constant_stride || description->constantBufferMaxDataSize > D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16U)
        return failed(Failure(ErrorCode::TaskFailed, "NRD constant buffer exceeds D3D12 limits."));
    storage->pipelines.resize(description->pipelinesNum);
    for (std::uint32_t i = 0; i != description->pipelinesNum; ++i)
    {
        const auto &source = description->pipelines[i]; auto &pipeline = storage->pipelines[i];
        if (!source.resourceRanges || !source.resourceRangesNum || source.resourceRangesNum > 2)
            return failed(Failure(ErrorCode::TaskFailed, "NRD pipeline has unsupported native descriptor ranges."));
        pipeline.ranges.assign(source.resourceRanges, source.resourceRanges + source.resourceRangesNum);
        std::vector<D3D12_DESCRIPTOR_RANGE> ranges(source.resourceRangesNum);
        std::vector<D3D12_ROOT_PARAMETER> parameters;
        if (source.hasConstantData)
        {
            D3D12_ROOT_PARAMETER parameter{}; parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            parameter.Descriptor = {description->constantBufferRegisterIndex, description->constantBufferAndSamplersSpaceIndex};
            pipeline.constant_parameter = 0; parameters.push_back(parameter);
        }
        for (std::uint32_t r = 0; r != source.resourceRangesNum; ++r)
        {
            const auto &range = source.resourceRanges[r];
            if (!range.descriptorsNum || (range.descriptorType != nrd::DescriptorType::TEXTURE && range.descriptorType != nrd::DescriptorType::STORAGE_TEXTURE))
                return failed(Failure(ErrorCode::TaskFailed, "NRD pipeline has an invalid native descriptor type/count."));
            ranges[r] = {range.descriptorType == nrd::DescriptorType::TEXTURE ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV : D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
                range.descriptorsNum, description->resourcesBaseRegisterIndex, description->resourcesSpaceIndex, 0};
            D3D12_ROOT_PARAMETER parameter{}; parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameter.DescriptorTable = {1, &ranges[r]}; pipeline.table_parameters.push_back(static_cast<std::uint32_t>(parameters.size()));
            parameters.push_back(parameter); pipeline.descriptor_count += range.descriptorsNum;
        }
        if (pipeline.descriptor_count > table_size) return failed(Failure(ErrorCode::TaskFailed, "NRD pipeline exceeds the actual per-set descriptor bound."));
        std::vector<D3D12_STATIC_SAMPLER_DESC> samplers(description->samplersNum);
        for (std::uint32_t s = 0; s != description->samplersNum; ++s)
        {
            auto &sampler = samplers[s];
            if (description->samplers[s] != nrd::Sampler::NEAREST_CLAMP && description->samplers[s] != nrd::Sampler::LINEAR_CLAMP)
                return failed(Failure(ErrorCode::TaskFailed, "NRD requests an unsupported native sampler."));
            sampler.Filter = description->samplers[s] == nrd::Sampler::NEAREST_CLAMP ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.MaxAnisotropy = 1; sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS; sampler.MaxLOD = D3D12_FLOAT32_MAX;
            sampler.ShaderRegister = description->samplersBaseRegisterIndex + s;
            sampler.RegisterSpace = description->constantBufferAndSamplersSpaceIndex;
        }
        D3D12_ROOT_SIGNATURE_DESC root{}; root.NumParameters = static_cast<UINT>(parameters.size()); root.pParameters = parameters.data();
        root.NumStaticSamplers = static_cast<UINT>(samplers.size()); root.pStaticSamplers = samplers.data();
        ComPtr<ID3DBlob> signature, diagnostics;
        auto native = D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &diagnostics);
        if (FAILED(native))
        {
            auto message = std::string(NativeFailure("Serialize NRD root signature", native).Message());
            if (diagnostics && diagnostics->GetBufferSize()) message += " " + std::string(static_cast<const char *>(diagnostics->GetBufferPointer()), diagnostics->GetBufferSize());
            return failed(Failure(ErrorCode::TaskFailed, std::move(message)));
        }
        native = device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&pipeline.root));
        if (FAILED(native)) return failed(NativeFailure("Create NRD root signature", native));
        const auto identifier_end = std::find(std::begin(source.shaderIdentifier), std::end(source.shaderIdentifier), '\0');
        if (identifier_end == std::end(source.shaderIdentifier) || identifier_end == std::begin(source.shaderIdentifier))
            return failed(Failure(ErrorCode::TaskFailed, "NRD shader identifier is not a nonempty terminated string."));
        const auto product = std::string("nrd/") + std::string(source.shaderIdentifier, identifier_end);
        storage->shader_products.push_back(product);
        std::vector<std::byte> bytes;
        if (auto read = read_shader(product, bytes); !read) return failed(read);
        if (bytes.empty()) return failed(Failure(ErrorCode::TaskFailed, "NRD shader product is empty: " + product));
        // When the pinned library embeds DXIL, demand the identical generated
        // permutation. A bytecode/container or shader-config mismatch is a fault.
        if (source.computeShaderDXIL.bytecode && source.computeShaderDXIL.size &&
            (source.computeShaderDXIL.size != bytes.size() || std::memcmp(source.computeShaderDXIL.bytecode, bytes.data(), bytes.size())))
            return failed(Failure(ErrorCode::InvalidState, "NRD shader product differs from the pinned native DXIL permutation: " + product));
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{}; pso.pRootSignature = pipeline.root.Get(); pso.CS = {bytes.data(), bytes.size()};
        native = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipeline.pso));
        if (FAILED(native)) return failed(NativeFailure("Create NRD PSO " + product, native));
    }
    const auto pool = [&](const nrd::TextureDesc *descs, std::uint32_t count, std::vector<AllocationResource> &resources) -> Result
    {
        if (count && !descs) return Failure(ErrorCode::TaskFailed, "NRD returned a missing native texture pool.");
        resources.resize(count);
        for (std::uint32_t i = 0; i != count; ++i)
        {
            const auto factor = descs[i].downsampleFactor; const auto format = NativeFormat(descs[i].format);
            if (!factor || format == DXGI_FORMAT_UNKNOWN) return Failure(ErrorCode::TaskFailed, "NRD pool format/downsampling is invalid.");
            const ViewExtent dimensions{(extent.width + factor - 1U) / factor, (extent.height + factor - 1U) / factor};
            if (auto allocated = CreateTexture(device, allocator, dimensions, format, resources[i]); !allocated) return allocated;
            storage->bytes += resources[i].allocation->GetSize();
        }
        return Result::Success();
    };
    if (auto result = pool(description->permanentPool, description->permanentPoolSize, storage->permanent); !result) return failed(result);
    if (auto result = pool(description->transientPool, description->transientPoolSize, storage->transient); !result) return failed(result);
    for (std::uint32_t i = 0; i != kOutputCount; ++i)
    {
        if (!SelectedOutput(selection, i)) continue;
        if (auto result = CreateTexture(device, allocator, extent, kOutputFormats[i], storage->outputs[i]); !result) return failed(result);
        storage->bytes += storage->outputs[i].allocation->GetSize();
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap{}; heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = storage->descriptors_per_frame * frame_slots; heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    auto native = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&storage->descriptors));
    if (FAILED(native)) return failed(NativeFailure("Create NRD per-frame descriptors", native));
    storage->descriptor_stride = device->GetDescriptorHandleIncrementSize(heap.Type);
    heap.NumDescriptors = kOutputCount; heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    native = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&storage->exports));
    if (FAILED(native)) return failed(NativeFailure("Create NRD output SRVs", native));
    for (std::uint32_t i = 0; i != kOutputCount; ++i)
        if (SelectedOutput(selection, i)) storage->View(storage->outputs[i].resource.Get(), nrd::DescriptorType::TEXTURE, storage->Export(i));
    D3D12_RESOURCE_DESC upload{}; upload.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upload.Width = UINT64(frame_slots) * storage->dispatch_capacity * storage->constant_stride;
    upload.Height = 1; upload.DepthOrArraySize = 1; upload.MipLevels = 1; upload.SampleDesc.Count = 1;
    upload.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (auto result = Allocate(allocator, D3D12_HEAP_TYPE_UPLOAD, upload, D3D12_RESOURCE_STATE_GENERIC_READ, storage->constants); !result) return failed(result);
    D3D12_RANGE empty{0, 0}; native = storage->constants.resource->Map(0, &empty, reinterpret_cast<void **>(&storage->mapped));
    if (FAILED(native)) return failed(NativeFailure("Map NRD per-frame constants", native));
    storage->bytes += storage->constants.allocation->GetSize(); storage->slot_completion.resize(frame_slots);
    storage->slot_leases.resize(frame_slots);
    candidate.storage_ = std::move(storage);
    return Result::Success();
#endif
}

Result NrdSystem::Commit(NrdCandidate &&candidate, const CompletionSet &completed)
{
    if (!candidate.storage_) return Failure(ErrorCode::InvalidArgument, "NRD candidate is empty.");
    if (impl_->pending.graph) return Failure(ErrorCode::InvalidState, "Complete the registered NRD graph before committing a candidate.");
#if HS_WITH_NRD
    if (auto owner = candidate.storage_->RequireOwner(); !owner) return owner;
    if (impl_->storage && !impl_->storage->completion.IsSatisfiedBy(completed))
        return Failure(ErrorCode::InvalidState, "NRD replacement requires all old resource/queue completions.");
    candidate.storage_->observed = completed;
#else
    static_cast<void>(completed);
#endif
    impl_->storage = std::move(candidate.storage_); impl_->availability = ReconstructionAvailability::Available; impl_->reason.clear();
    return Result::Success();
}

Result NrdSystem::Register(RenderGraphBuilder &graph, const NrdNativeInputs &inputs,
    const NrdSettings &settings, NrdNativeOutputs &outputs)
{
    outputs = {};
    if (impl_->pending.graph) return Failure(ErrorCode::InvalidState, "Complete the prior NRD graph before registering another.");
    if (!Enabled(settings))
    {
        // Off never queries the native library or allocates GPU storage.
        impl_->pending.graph = &graph;
        return Result::Success();
    }
#if !HS_WITH_NRD
    static_cast<void>(inputs);
    return Failure(ErrorCode::InvalidState, impl_->reason);
#else
    const auto storage = impl_->storage;
    if (!storage || !storage->usable) return Failure(ErrorCode::InvalidState, impl_->reason);
    const auto selected = storage->selection;
    const bool diffuse = SelectedOutput(selected, 0), specular = SelectedOutput(selected, 1);
    if ((settings.radiance != NrdRadianceMethod::Off) != (diffuse || specular) ||
        ((diffuse || specular) && settings.radiance != selected.method) ||
        settings.ambient_occlusion != (selected.signal == NrdSignal::AmbientOcclusion) ||
        settings.shadow != (selected.signal == NrdSignal::Shadow))
        return Failure(ErrorCode::InvalidArgument, "NRD settings must match the explicitly prepared signal and method.");
    if (auto owner = storage->RequireOwner(); !owner) return owner;
    if (!ValidSettings(settings) || !ValidExtent(inputs.active) || inputs.active.width > storage->capacity.width ||
        inputs.active.height > storage->capacity.height || inputs.frame_slot >= storage->frames || inputs.identity.viewport_id != 0 ||
        inputs.identity.render_id > UINT32_MAX || !std::isfinite(inputs.camera.delta_seconds) || inputs.camera.delta_seconds <= 0 ||
        !std::isfinite(inputs.camera.delta_seconds * 1000.0f) || !ValidMatrix(inputs.camera.view) || !ValidMatrix(inputs.camera.projection) ||
        !std::isfinite(inputs.camera.jitter_pixels[0]) || std::abs(inputs.camera.jitter_pixels[0]) > 0.5f ||
        !std::isfinite(inputs.camera.jitter_pixels[1]) || std::abs(inputs.camera.jitter_pixels[1]) > 0.5f || inputs.owners.empty() ||
        std::any_of(inputs.owners.begin(), inputs.owners.end(), [](const auto &owner) { return !owner; }))
        return Failure(ErrorCode::InvalidArgument, "NRD requires a valid single-view native camera/settings packet, active extent, frame slot and producer allocation owners.");
    if (!storage->slot_completion[inputs.frame_slot].IsSatisfiedBy(inputs.completed))
        return Failure(ErrorCode::InvalidState, "NRD frame descriptors/constants/producers are still in use by a submitted queue.");
    if (UINT64(inputs.first_pass_id) + storage->dispatch_capacity + 1U > UINT32_MAX)
        return Failure(ErrorCode::InvalidArgument, "NRD pass ID range overflows.");
    const auto fits = [&](const NrdNativeResource &resource, DXGI_FORMAT format)
    { return Fits(storage->device.Get(), resource, storage->capacity, format); };
    if (!inputs.native_geometry_packed || !fits(inputs.motion, DXGI_FORMAT_R16G16_FLOAT) ||
        !fits(inputs.normal_roughness, DXGI_FORMAT_R10G10B10A2_UNORM) || !fits(inputs.view_z, DXGI_FORMAT_R32_FLOAT))
        return Failure(ErrorCode::InvalidArgument, "NRD needs dedicated native-packed motion, normal/roughness and linear view-Z producer resources.");
    if (settings.radiance != NrdRadianceMethod::Off)
    {
        const auto packing = settings.radiance == NrdRadianceMethod::Reblur ? NrdRadiancePacking::ReblurRadianceAndNormHitDistance :
            NrdRadiancePacking::RelaxRadianceAndHitDistance;
        if (inputs.radiance_packing != packing || (diffuse && !fits(inputs.diffuse_radiance_hit_distance, DXGI_FORMAT_R16G16B16A16_FLOAT)) ||
            (specular && !fits(inputs.specular_radiance_hit_distance, DXGI_FORMAT_R16G16B16A16_FLOAT)))
            return Failure(ErrorCode::InvalidArgument, "Selected NRD radiance method requires its exact native diffuse/specular packing.");
    }
    if (settings.ambient_occlusion && (!inputs.normalized_hit_distance_packed || !fits(inputs.diffuse_normalized_hit_distance, DXGI_FORMAT_R16_FLOAT)))
        return Failure(ErrorCode::InvalidArgument, "REBLUR occlusion requires its normalized diffuse hit-distance producer.");
    if (settings.shadow && (!inputs.sigma_penumbra_packed || !fits(inputs.shadow_penumbra, DXGI_FORMAT_R16_FLOAT)))
        return Failure(ErrorCode::InvalidArgument, "SIGMA shadow requires its native penumbra producer.");
    const std::array producers{inputs.motion, inputs.normal_roughness, inputs.view_z,
        inputs.diffuse_radiance_hit_distance, inputs.specular_radiance_hit_distance, inputs.diffuse_normalized_hit_distance, inputs.shadow_penumbra};
    for (std::size_t i = 0; i != producers.size(); ++i)
    {
        if (!producers[i].resource) continue;
        for (std::size_t j = 0; j != i; ++j)
            if (producers[j].resource && ((producers[i].version.resource.value == producers[j].version.resource.value &&
                producers[i].version.resource.kind == producers[j].version.resource.kind) !=
                (producers[i].resource == producers[j].resource)))
                return Failure(ErrorCode::InvalidArgument, "NRD native producer identities do not match their graph handles.");
        for (const auto &output : storage->outputs)
            if (output.resource.Get() == producers[i].resource)
                return Failure(ErrorCode::InvalidArgument, "NRD native input producers cannot alias its owned output history.");
    }

    const std::uint32_t methods = (settings.radiance == NrdRadianceMethod::Reblur ? 1U : settings.radiance == NrdRadianceMethod::Relax ? 2U : 0U) |
        (settings.ambient_occlusion ? 4U : 0U) | (settings.shadow ? 8U : 0U);
    const std::array normalization{settings.hit_distance_A, settings.hit_distance_B, settings.hit_distance_C, settings.denoising_range_m};
    const bool reset = !storage->history_valid || inputs.camera.reset_history || storage->previous_methods != methods ||
        storage->previous_normalization != normalization ||
        storage->previous_active.width != inputs.active.width || storage->previous_active.height != inputs.active.height ||
        inputs.identity.render_id != storage->previous_render_id + 1U;
    if (!reset && (!ValidMatrix(inputs.camera.previous_view) || !ValidMatrix(inputs.camera.previous_projection)))
        return Failure(ErrorCode::InvalidArgument, "NRD temporal accumulation requires actual previous unjittered camera matrices.");
    const auto native_failed = [&](Result result) -> Result
    {
        // GetComputeDispatches updates CPU ping-pong/history state. After any
        // failure in this transaction, continuing that instance is invalid.
        storage->usable = false; storage->history_valid = false;
        impl_->availability = ReconstructionAvailability::Unavailable; impl_->reason = result.Message();
        return result;
    };
    nrd::CommonSettings common{};
    ColumnVectorMatrix(common.worldToViewMatrix, inputs.camera.view);
    ColumnVectorMatrix(common.viewToClipMatrix, inputs.camera.projection);
    ColumnVectorMatrix(common.worldToViewMatrixPrev, reset ? inputs.camera.view : inputs.camera.previous_view);
    ColumnVectorMatrix(common.viewToClipMatrixPrev, reset ? inputs.camera.projection : inputs.camera.previous_projection);
    common.resourceSize[0] = common.resourceSizePrev[0] = static_cast<std::uint16_t>(storage->capacity.width);
    common.resourceSize[1] = common.resourceSizePrev[1] = static_cast<std::uint16_t>(storage->capacity.height);
    common.rectSize[0] = static_cast<std::uint16_t>(inputs.active.width); common.rectSize[1] = static_cast<std::uint16_t>(inputs.active.height);
    common.rectSizePrev[0] = static_cast<std::uint16_t>(reset ? inputs.active.width : storage->previous_active.width);
    common.rectSizePrev[1] = static_cast<std::uint16_t>(reset ? inputs.active.height : storage->previous_active.height);
    for (unsigned i = 0; i != 2; ++i)
    {
        common.cameraJitter[i] = inputs.camera.jitter_pixels[i];
        common.cameraJitterPrev[i] = reset ? inputs.camera.jitter_pixels[i] : storage->previous_jitter[i];
    }
    common.timeDeltaBetweenFrames = inputs.camera.delta_seconds * 1000.0f;
    common.denoisingRange = settings.denoising_range_m; common.disocclusionThreshold = settings.disocclusion_threshold;
    common.frameIndex = static_cast<std::uint32_t>(inputs.identity.render_id);
    common.accumulationMode = reset ? nrd::AccumulationMode::CLEAR_AND_RESTART : nrd::AccumulationMode::CONTINUE;
    auto native = storage->common_settings(*storage->instance, common);
    if (native != nrd::Result::SUCCESS) return native_failed(NrdFailure("SetCommonSettings", native));
    std::vector<nrd::Identifier> identifiers;
    nrd::ReblurSettings reblur{};
    reblur.hitDistanceParameters.A = settings.hit_distance_A; reblur.hitDistanceParameters.B = settings.hit_distance_B;
    reblur.hitDistanceParameters.C = settings.hit_distance_C;
    reblur.maxAccumulatedFrameNum = settings.radiance_history_frames; reblur.maxFastAccumulatedFrameNum = settings.fast_history_frames;
    reblur.maxStabilizedFrameNum = settings.radiance_history_frames;
    reblur.historyFixFrameNum = settings.fast_history_frames ? std::min(3U, settings.fast_history_frames - 1U) : 0U;
    if (settings.probabilistic_lobe_selection) reblur.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::AREA_3X3;
    const auto set = [&](nrd::Identifier identifier, const void *configuration) -> Result
    {
        const auto result = storage->denoiser_settings(*storage->instance, identifier, configuration);
        if (result != nrd::Result::SUCCESS) return NrdFailure("SetDenoiserSettings", result);
        identifiers.push_back(identifier); return Result::Success();
    };
    if (settings.radiance == NrdRadianceMethod::Reblur)
    {
        if (auto result = set(kSelectedDenoiser, &reblur); !result) return native_failed(result);
    }
    else if (settings.radiance == NrdRadianceMethod::Relax)
    {
        nrd::RelaxSettings relax{};
        relax.diffuseMaxAccumulatedFrameNum = relax.specularMaxAccumulatedFrameNum = settings.radiance_history_frames;
        relax.diffuseMaxFastAccumulatedFrameNum = relax.specularMaxFastAccumulatedFrameNum = settings.fast_history_frames;
        relax.historyFixFrameNum = reblur.historyFixFrameNum;
        relax.hitDistanceReconstructionMode = reblur.hitDistanceReconstructionMode;
        if (auto result = set(kSelectedDenoiser, &relax); !result) return native_failed(result);
    }
    if (settings.ambient_occlusion)
        if (auto result = set(kSelectedDenoiser, &reblur); !result) return native_failed(result);
    if (settings.shadow)
    {
        nrd::SigmaSettings sigma{}; sigma.maxStabilizedFrameNum = settings.shadow_history_frames;
        sigma.lightDirection[0] = settings.shadow_light_direction.x; sigma.lightDirection[1] = settings.shadow_light_direction.y;
        sigma.lightDirection[2] = settings.shadow_light_direction.z;
        if (auto result = set(kSelectedDenoiser, &sigma); !result) return native_failed(result);
    }
    const nrd::DispatchDesc *native_dispatches{}; std::uint32_t native_count{};
    native = storage->dispatches(*storage->instance, identifiers.data(), static_cast<std::uint32_t>(identifiers.size()), native_dispatches, native_count);
    if (native != nrd::Result::SUCCESS) return native_failed(NrdFailure("GetComputeDispatches", native));
    if (!native_dispatches || !native_count || native_count > storage->dispatch_capacity)
        return native_failed(Failure(ErrorCode::TaskFailed, "NRD returned invalid native dispatch count/storage."));
    struct CopiedDispatch
    {
        std::string name;
        std::vector<nrd::ResourceDesc> resources;
        std::vector<std::byte> constants;
        std::uint32_t pipeline{}, width{}, height{};
    };
    std::vector<CopiedDispatch> dispatches; dispatches.reserve(native_count);
    // The SDK invalidates all these pointers on its next GetComputeDispatches.
    // Snapshot every dispatch immediately; graph callbacks never read SDK data.
    for (std::uint32_t i = 0; i != native_count; ++i)
    {
        const auto &source = native_dispatches[i];
        if (source.pipelineIndex >= storage->pipelines.size() || !source.resources || !source.resourcesNum || !source.gridWidth || !source.gridHeight ||
            source.constantBufferDataSize > storage->constant_data_max || (source.constantBufferDataSize && !source.constantBufferData))
            return native_failed(Failure(ErrorCode::TaskFailed, "NRD returned an invalid native dispatch description."));
        const auto &pipeline = storage->pipelines[source.pipelineIndex];
        if (source.resourcesNum != pipeline.descriptor_count ||
            (pipeline.constant_parameter != UINT32_MAX && !source.constantBufferDataSize))
            return native_failed(Failure(ErrorCode::TaskFailed, "NRD dispatch resources/constants disagree with its actual pipeline."));
        CopiedDispatch copy; copy.name = source.name ? source.name : "NRD dispatch";
        copy.pipeline = source.pipelineIndex; copy.width = source.gridWidth; copy.height = source.gridHeight;
        copy.resources.assign(source.resources, source.resources + source.resourcesNum);
        if (source.constantBufferDataSize)
            copy.constants.assign(reinterpret_cast<const std::byte *>(source.constantBufferData),
                reinterpret_cast<const std::byte *>(source.constantBufferData) + source.constantBufferDataSize);
        dispatches.push_back(std::move(copy));
    }

    struct Binding
    {
        ID3D12Resource *native{};
        AllocationResource *owned{};
        ResourceVersion version;
    };
    std::vector<Binding> bindings;
    const auto bind = [&](const NrdNativeResource &resource) -> std::size_t
    {
        const auto found = std::find_if(bindings.begin(), bindings.end(), [&](const auto &binding) { return binding.native == resource.resource; });
        if (found != bindings.end()) return static_cast<std::size_t>(found - bindings.begin());
        bindings.push_back({resource.resource, nullptr, resource.version}); return bindings.size() - 1U;
    };
    const auto own = [&](AllocationResource &resource) -> std::size_t
    {
        const auto found = std::find_if(bindings.begin(), bindings.end(), [&](const auto &binding) { return binding.native == resource.resource.Get(); });
        if (found != bindings.end()) return static_cast<std::size_t>(found - bindings.begin());
        bindings.push_back({resource.resource.Get(), &resource, {}}); return bindings.size() - 1U;
    };
    struct DispatchBinding { std::size_t binding{}; nrd::DescriptorType type{}; };
    std::vector<std::vector<DispatchBinding>> dispatch_bindings(native_count);
    for (std::uint32_t i = 0; i != native_count; ++i)
    {
        const auto &dispatch = dispatches[i]; const auto &pipeline = storage->pipelines[dispatch.pipeline];
        std::uint32_t offset{};
        for (const auto &range : pipeline.ranges)
            for (std::uint32_t r = 0; r != range.descriptorsNum; ++r, ++offset)
            {
                const auto &resource = dispatch.resources[offset]; std::size_t index = SIZE_MAX;
                if (resource.descriptorType != range.descriptorType)
                    return native_failed(Failure(ErrorCode::TaskFailed, "NRD dispatch descriptor type disagrees with its native range."));
                switch (resource.type)
                {
                case nrd::ResourceType::IN_MV: index = bind(inputs.motion); break;
                case nrd::ResourceType::IN_NORMAL_ROUGHNESS: index = bind(inputs.normal_roughness); break;
                case nrd::ResourceType::IN_VIEWZ: index = bind(inputs.view_z); break;
                case nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST:
                    if (settings.radiance != NrdRadianceMethod::Off) index = bind(inputs.diffuse_radiance_hit_distance); break;
                case nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST:
                    if (settings.radiance != NrdRadianceMethod::Off) index = bind(inputs.specular_radiance_hit_distance); break;
                case nrd::ResourceType::IN_DIFF_HITDIST:
                    if (settings.ambient_occlusion) index = bind(inputs.diffuse_normalized_hit_distance); break;
                case nrd::ResourceType::IN_PENUMBRA:
                    if (settings.shadow) index = bind(inputs.shadow_penumbra); break;
                case nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST: index = own(storage->outputs[0]); break;
                case nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST: index = own(storage->outputs[1]); break;
                case nrd::ResourceType::OUT_DIFF_HITDIST: index = own(storage->outputs[2]); break;
                case nrd::ResourceType::OUT_SHADOW_TRANSLUCENCY: index = own(storage->outputs[3]); break;
                case nrd::ResourceType::PERMANENT_POOL:
                    if (resource.indexInPool < storage->permanent.size()) index = own(storage->permanent[resource.indexInPool]); break;
                case nrd::ResourceType::TRANSIENT_POOL:
                    if (resource.indexInPool < storage->transient.size()) index = own(storage->transient[resource.indexInPool]); break;
                default: break;
                }
                if (index == SIZE_MAX || !bindings[index].native)
                    return native_failed(Failure(ErrorCode::TaskFailed, "NRD requested an absent/unsupported native resource type or pool index."));
                const auto flags = bindings[index].native->GetDesc().Flags;
                if ((range.descriptorType == nrd::DescriptorType::TEXTURE && (flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) ||
                    (range.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE && !(flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)))
                    return native_failed(Failure(ErrorCode::TaskFailed, "NRD native dispatch needs an unsupported resource view."));
                for (const auto &prior : dispatch_bindings[i])
                    if (prior.binding == index && prior.type != range.descriptorType)
                        return native_failed(Failure(ErrorCode::TaskFailed, "NRD dispatch aliases the same physical texture as both SRV and UAV."));
                dispatch_bindings[i].push_back({index, range.descriptorType});
            }
    }
    // Output history (SIGMA especially) is owned rather than borrowed from a
    // caller's recycled output. The actual SDK's CLEAR_AND_RESTART dispatches
    // clear all selected storage resources, including these native outputs.
    std::array<std::size_t, kOutputCount> output_bindings{};
    for (std::uint32_t i = 0; i != kOutputCount; ++i)
        if (SelectedOutput(selected, i)) output_bindings[i] = own(storage->outputs[i]);
    auto &pending = impl_->pending; pending.graph = &graph; pending.storage = storage;
    pending.slot = inputs.frame_slot; pending.methods = methods; pending.render_id = inputs.identity.render_id;
    pending.normalization = normalization;
    pending.active = inputs.active; pending.jitter[0] = inputs.camera.jitter_pixels[0]; pending.jitter[1] = inputs.camera.jitter_pixels[1];
    storage->observed.Merge(inputs.completed);
    auto &lease = storage->slot_leases[inputs.frame_slot]; lease.resources.clear(); lease.owners = inputs.owners;
    for (auto &binding : bindings)
    {
        if (binding.owned)
        {
            auto &allocation = *binding.owned;
            const auto handle = graph.ImportTexture({allocation.resource.Get(), allocation.graph_completion, allocation.graph_queue},
                allocation.graph_access, "NRD native pool/output");
            binding.version = graph.Version(handle); pending.imports.push_back({&allocation, handle});
        }
        else
        {
            // Retain COM identity AND caller's placed heap/allocation owners.
            lease.resources.emplace_back(binding.native); pending.input_handles.push_back(binding.version.resource);
        }
    }
    const auto slot_base = inputs.frame_slot * storage->descriptors_per_frame;
    std::uint32_t next_pass = inputs.first_pass_id; std::uint32_t previous_pass{}; bool has_previous_pass{};
    for (std::uint32_t i = 0; i != native_count; ++i)
    {
        const auto &dispatch = dispatches[i]; const auto &pipeline = storage->pipelines[dispatch.pipeline];
        const auto descriptor_base = slot_base + i * storage->descriptors_per_dispatch;
        const UINT64 constant_offset = (UINT64(inputs.frame_slot) * storage->dispatch_capacity + i) * storage->constant_stride;
        if (!dispatch.constants.empty()) std::memcpy(storage->mapped + constant_offset, dispatch.constants.data(), dispatch.constants.size());
        const auto id = next_pass++; auto pass = graph.AddPass(id, dispatch.name, QueueHint::Compute);
        if (has_previous_pass) graph.AddOrderingDependency(previous_pass, id);
        std::vector<std::pair<ResourceHandle, ID3D12Resource *>> callback_resources;
        std::vector<std::size_t> declared;
        for (std::uint32_t r = 0; r != dispatch_bindings[i].size(); ++r)
        {
            const auto reference = dispatch_bindings[i][r]; auto &binding = bindings[reference.binding];
            storage->View(binding.native, reference.type, storage->Cpu(descriptor_base + r));
            if (std::find(declared.begin(), declared.end(), reference.binding) != declared.end()) continue;
            declared.push_back(reference.binding); callback_resources.emplace_back(binding.version.resource, binding.native);
            if (reference.type == nrd::DescriptorType::TEXTURE) pass.Read(binding.version, Access::ComputeRead);
            else
            {
                binding.version = pass.ReadWrite(binding.version, Access::UnorderedWrite);
            }
        }
        pass.SetExecuteChecked([storage, resources = std::move(callback_resources), pipeline_index = dispatch.pipeline,
            width = dispatch.width, height = dispatch.height, descriptor_base, constant_offset](RenderPassContext &context) -> Result
        {
            auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            if (!list) return Failure(ErrorCode::InvalidState, "NRD dispatch has no native command list.");
            for (const auto &[handle, native_resource] : resources)
                if (context.Resolve(handle) != native_resource)
                    return Failure(ErrorCode::InvalidState, "NRD producer/pool graph resource identity changed.");
            const auto &selected = storage->pipelines[pipeline_index];
            ID3D12DescriptorHeap *heap = storage->descriptors.Get(); list->SetDescriptorHeaps(1, &heap);
            list->SetComputeRootSignature(selected.root.Get()); list->SetPipelineState(selected.pso.Get());
            if (selected.constant_parameter != UINT32_MAX)
                list->SetComputeRootConstantBufferView(selected.constant_parameter, storage->constants.resource->GetGPUVirtualAddress() + constant_offset);
            std::uint32_t offset{};
            for (std::uint32_t r = 0; r != selected.ranges.size(); ++r)
            {
                list->SetComputeRootDescriptorTable(selected.table_parameters[r], storage->Gpu(descriptor_base + offset));
                offset += selected.ranges[r].descriptorsNum;
            }
            list->Dispatch(width, height, 1);
            return Result::Success();
        });
        previous_pass = id; has_previous_pass = true;
    }
    if (settings.radiance != NrdRadianceMethod::Off)
    {
        const auto motion = std::find_if(bindings.begin(), bindings.end(), [&](const auto &binding)
        { return binding.native == inputs.motion.resource; });
        if (motion == bindings.end())
            return native_failed(Failure(ErrorCode::TaskFailed, "NRD radiance dispatches omitted their canonical motion binding."));
        outputs.final_motion = motion->version;
    }
    const auto output = [&](std::uint32_t i) -> NrdNativeOutput
    {
        const auto &binding = bindings[output_bindings[i]];
        // Compute lists cannot transition to PIXEL_SHADER_RESOURCE. A raster
        // consumer declares its own ShaderRead/AllShaderRead on this version;
        // graph cross-queue ordering supplies that transition and queue wait.
        graph.SetFinalAccess(binding.version, Access::ComputeRead);
        return {binding.version, binding.native, storage->Export(i)};
    };
    if (settings.radiance != NrdRadianceMethod::Off)
    {
        if (diffuse) outputs.diffuse = output(0);
        if (specular) outputs.specular = output(1);
        outputs.radiance_packing = inputs.radiance_packing;
    }
    if (settings.ambient_occlusion) outputs.ambient_occlusion = output(2);
    if (settings.shadow) outputs.shadow = output(3);
    return Result::Success();
#endif
}

Result NrdSystem::CompleteSubmission(const RenderGraphBuilder &graph, const CompletionSet &completion, bool success)
{
    if (impl_->pending.graph != &graph) return Failure(ErrorCode::InvalidState, "NRD completion does not match its registered graph.");
#if HS_WITH_NRD
    auto &pending = impl_->pending; const auto storage = pending.storage;
    if (storage)
    {
        if (auto owner = storage->RequireOwner(); !owner) return owner;
        auto required = completion;
        for (const auto &import : pending.imports) required.Merge(graph.ResourceCompletion(import.handle));
        for (const auto handle : pending.input_handles) required.Merge(graph.ResourceCompletion(handle));
        // Keep the pending lease intact so the caller can supply the real fence;
        // allowing reset here would retire resources with unqualified GPU work.
        if (success && !(required.direct || required.compute || required.copy))
            return Failure(ErrorCode::InvalidState, "NRD submitted work requires actual queue fence completions.");
        for (const auto &import : pending.imports)
        {
            const auto resource_completion = graph.ResourceCompletion(import.handle);
            required.Merge(resource_completion); import.allocation->graph_completion.Merge(resource_completion);
            import.allocation->graph_completion.Merge(completion);
            if (success)
            {
                import.allocation->graph_access = graph.FinalAccess(import.handle);
                import.allocation->graph_queue = graph.FinalQueue(import.handle);
            }
        }
        storage->completion.Merge(required); storage->slot_completion[pending.slot].Merge(required);
        const bool qualified = success;
        storage->history_valid = qualified;
        if (qualified)
        {
            storage->previous_render_id = pending.render_id; storage->previous_methods = pending.methods;
            storage->previous_normalization = pending.normalization;
            storage->previous_active = pending.active; storage->previous_jitter[0] = pending.jitter[0]; storage->previous_jitter[1] = pending.jitter[1];
        }
        else
        {
            storage->usable = false; impl_->availability = ReconstructionAvailability::Unavailable;
            impl_->reason = "NRD graph failed after native dispatch generation; drain and prepare a fresh instance.";
        }
        pending = {};
        return Result::Success();
    }
#else
    static_cast<void>(completion); static_cast<void>(success);
#endif
    impl_->pending = {};
    return Result::Success();
}

Result NrdSystem::ResetAfterCompletion(const CompletionSet &completed)
{
    if (impl_->pending.graph) return Failure(ErrorCode::InvalidState, "Complete the registered NRD graph before reset.");
#if HS_WITH_NRD
    if (impl_->storage)
    {
        if (auto owner = impl_->storage->RequireOwner(); !owner) return owner;
        if (!impl_->storage->completion.IsSatisfiedBy(completed))
            return Failure(ErrorCode::InvalidState, "NRD reset requires all actual pool/output/producer queue completions.");
        impl_->storage->observed.Merge(completed);
    }
#else
    static_cast<void>(completed);
#endif
    impl_->storage.reset();
#if HS_WITH_NRD
    impl_->availability = ReconstructionAvailability::Unavailable; impl_->reason = "NRD has not been prepared.";
#else
    impl_->availability = ReconstructionAvailability::CompiledOut;
    impl_->reason = "NRD was explicitly excluded by HS_WITH_NRD=OFF.";
#endif
    return Result::Success();
}
std::uint64_t NrdSystem::AllocatedBytes() const noexcept { return impl_->storage ? impl_->storage->bytes : 0; }
ReconstructionAvailability NrdSystem::Availability() const noexcept { return impl_->availability; }
std::string_view NrdSystem::FailureReason() const noexcept { return impl_->reason; }
ID3D12Resource *NrdSystem::PreparedOutput(NrdSignal signal) const noexcept
{
#if HS_WITH_NRD
    if (auto storage = impl_->storage)
    {
        const std::uint32_t index = signal == NrdSignal::Diffuse ? 0U : signal == NrdSignal::Specular ? 1U :
            signal == NrdSignal::AmbientOcclusion ? 2U : 3U;
        return storage->outputs[index].resource.Get();
    }
#else
    static_cast<void>(signal);
#endif
    return nullptr;
}
} // namespace hs
