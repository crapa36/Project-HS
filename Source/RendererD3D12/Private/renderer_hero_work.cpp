#include "renderer_hero_work.hpp"
#include "d3d12_resources.hpp"
#include <hs/core/sha256.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <iterator>
#include <stdexcept>
#include <utility>
#include <wrl/client.h>

namespace hs
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr UINT kContributors = kHeroWorkContributors, kPhases = kHeroWorkPhases, kRows = kContributors * kPhases;
constexpr UINT64 kRowBytes = kHeroWorkRowBytes, kPhaseBytes = kContributors * kRowBytes, kArgumentsBytes = kContributors * 24;
static_assert(sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS) == 11 * sizeof(UINT64));
static_assert(sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS1) == 14 * sizeof(UINT64));
static_assert(sizeof(UINT) + sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) == 24);
static_assert(sizeof(std::array<UINT, 6>) == 24);
static_assert(kHeroWorkStatisticsOffset + sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS1) == kHeroWorkOcclusionOffset);
static_assert(kHeroWorkOcclusionOffset + sizeof(UINT64) == kRowBytes);
constexpr std::size_t kPendingLimit = 240 * kRows;
Result Fail(std::string message) { return Result::Failure(ErrorCode::InvalidState, "hs_hero_work", std::move(message)); }
Result Native(std::string_view what, HRESULT hr)
{
    return Result::Failure(ErrorCode::TaskFailed, "hs_hero_work", std::string(what) +
        " failed (HRESULT " + std::to_string(static_cast<std::int32_t>(hr)) + ").");
}
Result Readback(D3D12MA::Allocator *allocator, UINT64 bytes, AllocationResource &out)
{
    D3D12MA::ALLOCATION_DESC allocation{}; allocation.HeapType = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes; desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const auto hr = allocator->CreateResource(&allocation, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        &out.allocation, IID_PPV_ARGS(out.resource.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) return Native("Create hero contributor readback", hr);
    out.graph_access = Access::CopyDest;
    return Result::Success();
}
Result ShaderBuffer(D3D12MA::Allocator *allocator, UINT64 bytes, bool upload, AllocationResource &out)
{
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = upload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes; desc.Height = 1; desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (!upload) desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const auto hr = allocator->CreateResource(&allocation, &desc, upload ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COMMON,
        nullptr, &out.allocation, IID_PPV_ARGS(out.resource.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) return Native("Create bounded hero shader diagnostic buffer", hr);
    out.graph_access = upload ? Access::CopySource : Access::Common;
    return Result::Success();
}
UINT PipelineIndex(HeroTransparencyPhase phase, UINT layer)
{
    // Product3 now shades the nearest composite and writes its six surface guides.
    return phase == HeroTransparencyPhase::Composite && layer == 0 ? 3u : static_cast<UINT>(phase);
}
} // namespace

struct HeroWorkCapture::Impl
{
    struct Slot
    {
        ComPtr<ID3D12QueryHeap> timestamps, statistics, occlusion;
        std::array<AllocationResource, kPhases> readbacks;
        AllocationResource arguments, shader_zero;
        std::array<AllocationResource, kPhases> shader_gpu, shader_readbacks;
        ResourceHandle shader_zero_handle;
        std::array<ResourceHandle, kPhases> shader_handles, shader_readback_handles;
        std::array<bool, kPhases> shader_clear_recorded{}, shader_copy_recorded{};
        std::array<ResourceHandle, kPhases> phase_handles;
        ResourceHandle arguments_handle;
        std::array<bool, kRows> recorded{};
        std::array<RendererHeroWorkSample, kRows> rows;
        std::vector<HeroWorkContributorInput> contributors;
        CompletionSet completion;
        ComPtr<ID3D12CommandQueue> queue;
        std::uint64_t frame{}, tick{}, frequency{};
        std::uint32_t view{}, width{}, height{};
        bool pending{}, success{};
    };
    std::vector<std::unique_ptr<Slot>> slots;
    ComPtr<ID3D12Device> device;
    std::array<RendererHeroWorkPipeline, 5> pipelines;
    RendererHeroWorkStatus status;
    HeroWorkShaderLayout shader_layout;
    HeroWorkDiagnosticMode mode{};
    std::uint64_t qpc_frequency{};
    D3D12_QUERY_TYPE statistics_type{D3D12_QUERY_TYPE_PIPELINE_STATISTICS};
    UINT statistics_bytes{sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS)};
};

HeroWorkCapture::HeroWorkCapture() : impl_(std::make_unique<Impl>()) {}
HeroWorkCapture::~HeroWorkCapture() = default;
Result HeroWorkCapture::Initialize(ID3D12Device *device, D3D12MA::Allocator *allocator, std::uint32_t count, ViewExtent capacity, HeroWorkDiagnosticMode mode)
{
    if (!device || !allocator || !count || !impl_->slots.empty()) return Fail("Invalid hero capture initialization.");
    if (!HeroWorkMakeShaderLayout(capacity.width, capacity.height, count, impl_->shader_layout))
        return Fail("Hero shader diagnostics exceed the checked 512MiB GPU/readback/zero-upload limit or pixel index range.");
    static_assert(32 + kHeroWorkShaderCounterNames.size() * 4 <= kHeroWorkShaderHeaderBytes);
    if (mode > HeroWorkDiagnosticMode::CoverageOnly) return Fail("Invalid hero work diagnostic mode.");
    impl_->mode = mode;
    impl_->status.shader_counters_enabled = true;
    impl_->device = device;
    D3D12_FEATURE_DATA_D3D12_OPTIONS9 options{};
    const bool stats1 = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &options, sizeof(options))) &&
        options.MeshShaderPipelineStatsSupported;
    impl_->status.enabled = true; impl_->status.mesh_statistics_supported = stats1;
    if (stats1) { impl_->statistics_type = D3D12_QUERY_TYPE_PIPELINE_STATISTICS1; impl_->statistics_bytes = 14 * sizeof(UINT64); }
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) return Fail("Hero capture QPC frequency is unavailable.");
    impl_->qpc_frequency = static_cast<std::uint64_t>(frequency.QuadPart);
    for (UINT index = 0; index < count; ++index)
    {
        auto slot = std::make_unique<Impl::Slot>();
        auto heap = [&](D3D12_QUERY_HEAP_TYPE type, UINT queries, ComPtr<ID3D12QueryHeap> &out) -> Result {
            D3D12_QUERY_HEAP_DESC desc{}; desc.Type = type; desc.Count = queries;
            const auto hr = device->CreateQueryHeap(&desc, IID_PPV_ARGS(&out));
            if (FAILED(hr)) return Native("Create hero contributor query heap", hr);
            ++impl_->status.query_heap_count; impl_->status.query_count += queries;
            return Result::Success();
        };
        if (auto r = heap(D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kRows * 2, slot->timestamps); !r) return r;
        if (auto r = heap(stats1 ? D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS1 : D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS,
                kRows, slot->statistics); !r) return r;
        if (auto r = heap(D3D12_QUERY_HEAP_TYPE_OCCLUSION, kRows, slot->occlusion); !r) return r;
        for (auto &readback : slot->readbacks)
        {
            if (auto r = Readback(allocator, kPhaseBytes, readback); !r) return r;
            impl_->status.readback_bytes += readback.allocation->GetSize();
        }
        if (auto r = Readback(allocator, kArgumentsBytes, slot->arguments); !r) return r;
        impl_->status.readback_bytes += slot->arguments.allocation->GetSize();
        if (auto r = ShaderBuffer(allocator, impl_->shader_layout.phase_bytes, true, slot->shader_zero); !r) return r;
        impl_->status.shader_zero_upload_bytes += slot->shader_zero.allocation->GetSize();
        for (UINT phase = 0; phase < kPhases; ++phase)
        {
            if (auto r = ShaderBuffer(allocator, impl_->shader_layout.phase_bytes, false, slot->shader_gpu[phase]); !r) return r;
            if (auto r = Readback(allocator, kContributors * kHeroWorkShaderHeaderBytes, slot->shader_readbacks[phase]); !r) return r;
            impl_->status.shader_gpu_bytes += slot->shader_gpu[phase].allocation->GetSize();
            impl_->status.shader_readback_bytes += slot->shader_readbacks[phase].allocation->GetSize();
            if (impl_->status.readback_bytes + impl_->status.shader_gpu_bytes + impl_->status.shader_readback_bytes + impl_->status.shader_zero_upload_bytes > kHeroWorkShaderStorageLimit)
                return Fail("Actual hero shader diagnostic allocation sizes exceed 512MiB.");
        }
        for (auto *resource : {&slot->arguments, &slot->shader_zero})
            if (auto certified = resource->EnsureNativeContract(device); !certified)
                return certified;
        for (UINT phase = 0; phase < kPhases; ++phase)
            for (auto *resource : {&slot->readbacks[phase], &slot->shader_gpu[phase],
                    &slot->shader_readbacks[phase]})
                if (auto certified = resource->EnsureNativeContract(device); !certified)
                    return certified;
        impl_->slots.push_back(std::move(slot));
    }
    return Result::Success();
}
void HeroWorkCapture::Pipeline(std::uint32_t index, const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc,
    std::span<const std::byte> vertex, std::span<const std::byte> pixel, std::string_view product)
{
    auto &out = impl_->pipelines.at(index);
    out.vertex_product = index == 3 ? "render/shader/hero_transparency_vs.dxil"
                                    : "render/shader/hero_transparency_current_vs.dxil";
    out.pixel_product = product;
    out.vertex_sha256 = Sha256(vertex); out.pixel_sha256 = Sha256(pixel);
    out.source_requested_flags = "RenderMaterialShader: -nologo -WX -O3 -Ges; VS vs_6_0 / PS ps_6_0; HS_GENERATED_HERO_MATERIAL=1; HS_CAPTURE_HERO_WORK=1; product/source flags, not binary attestation";
    out.cull_mode = desc.RasterizerState.CullMode; out.fill_mode = desc.RasterizerState.FillMode;
    out.front_counter_clockwise = desc.RasterizerState.FrontCounterClockwise != FALSE;
    out.depth_clip_enabled = desc.RasterizerState.DepthClipEnable != FALSE;
    out.depth_bias = desc.RasterizerState.DepthBias; out.depth_bias_clamp = desc.RasterizerState.DepthBiasClamp;
    out.slope_scaled_depth_bias = desc.RasterizerState.SlopeScaledDepthBias;
    out.depth_enabled = desc.DepthStencilState.DepthEnable != FALSE; out.depth_func = desc.DepthStencilState.DepthFunc;
    out.depth_write_mask = desc.DepthStencilState.DepthWriteMask; out.stencil_enabled = desc.DepthStencilState.StencilEnable != FALSE;
    out.sample_count = desc.SampleDesc.Count; out.sample_mask = desc.SampleMask;
    out.independent_blend = desc.BlendState.IndependentBlendEnable != FALSE;
    out.alpha_to_coverage = desc.BlendState.AlphaToCoverageEnable != FALSE;
    out.rtv_count = desc.NumRenderTargets; out.dsv_format = desc.DSVFormat;
    for (UINT i = 0; i < 8; ++i)
    {
        const auto &blend = desc.BlendState.RenderTarget[i];
        out.rtv_formats[i] = desc.RTVFormats[i]; out.blend_enabled[i] = blend.BlendEnable;
        out.src_blend[i] = blend.SrcBlend; out.dst_blend[i] = blend.DestBlend; out.blend_op[i] = blend.BlendOp;
        out.src_blend_alpha[i] = blend.SrcBlendAlpha; out.dst_blend_alpha[i] = blend.DestBlendAlpha;
        out.blend_op_alpha[i] = blend.BlendOpAlpha; out.write_mask[i] = blend.RenderTargetWriteMask;
    }
}
Result HeroWorkCapture::Begin(RenderGraphBuilder &graph, const HeroTransparencyInputs &inputs,
    HeroWorkRetainedLayout *layout)
{
    if (inputs.frame_slot >= impl_->slots.size() || inputs.work_contributors.size() > kContributors || !inputs.work_draw ||
        !inputs.direct_queue || !inputs.direct_timestamp_frequency || !inputs.submitted_frame)
        return Fail("Hero contributor frame requires an indexed contributor callback, native direct queue and frame identity.");
    auto &slot = *impl_->slots[inputs.frame_slot];
    if (slot.pending) return Fail("Collect completed hero contributor slot before reusing its native queries.");
    if (!slot.completion.IsSatisfiedBy(inputs.completed)) return Fail("Hero contributor query slot is still in GPU use.");
    for (const auto &source : inputs.work_contributors)
    {
        if (!source.argument_resource || !source.arguments.resource || source.arguments.resource.kind != ResourceKind::Buffer)
            return Fail("Hero contributor requires an actual graph-owned indirect argument buffer.");
        const auto desc = source.argument_resource->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || source.argument_offset % 4 ||
            source.argument_offset > desc.Width || 24 > desc.Width - source.argument_offset)
            return Fail("Hero indexed indirect argument range is invalid.");
    }
    slot.recorded.fill(false); slot.phase_handles.fill({}); slot.arguments_handle = {};
    slot.shader_handles.fill({}); slot.shader_readback_handles.fill({}); slot.shader_zero_handle = {};
    slot.shader_clear_recorded.fill(false); slot.shader_copy_recorded.fill(false);
    if (!inputs.render_active.width || !inputs.render_active.height ||
        std::uint64_t(inputs.render_active.width) * inputs.render_active.height > impl_->shader_layout.pixels)
        return Fail("Hero shader active extent exceeds the checked bitmap capacity.");
    slot.contributors.assign(inputs.work_contributors.begin(), inputs.work_contributors.end());
    if (layout)
    {
        *layout = {};
        layout->contributor_count = static_cast<std::uint32_t>(slot.contributors.size());
        for (std::size_t index = 0; index < slot.contributors.size(); ++index)
            layout->argument_versions[index] = slot.contributors[index].arguments;
    }
    slot.queue = inputs.direct_queue; slot.frame = inputs.submitted_frame; slot.tick = inputs.tick;
    slot.view = inputs.view_id; slot.width = inputs.render_active.width; slot.height = inputs.render_active.height;
    slot.frequency = inputs.direct_timestamp_frequency; slot.success = false;
    if (slot.contributors.empty()) return Result::Success();
    void *zero{}; const D3D12_RANGE no_read{0, 0};
    auto zero_result = slot.shader_zero.resource->Map(0, &no_read, &zero);
    if (FAILED(zero_result)) return Native("Map hero shader zero-upload owner", zero_result);
    std::memset(zero, 0, static_cast<std::size_t>(impl_->shader_layout.phase_bytes));
    for (UINT contributor = 0; contributor < kContributors; ++contributor)
    {
        const std::array<UINT, 8> header{kHeroWorkShaderMagic, kHeroWorkShaderVersion, slot.width, slot.height,
            impl_->shader_layout.bitmap_words, impl_->shader_layout.pixels, 0, static_cast<UINT>(impl_->mode)};
        std::memcpy(static_cast<std::byte *>(zero) + contributor * impl_->shader_layout.contributor_bytes, header.data(), sizeof(header));
    }
    const D3D12_RANGE zero_written{0, static_cast<SIZE_T>(impl_->shader_layout.phase_bytes)};
    slot.shader_zero.resource->Unmap(0, &zero_written);
    for (auto *resource : {&slot.shader_zero, &slot.arguments})
        if (auto certified = resource->EnsureNativeContract(impl_->device.Get()); !certified)
            return certified;
    slot.shader_zero_handle = graph.ImportBuffer({slot.shader_zero.resource.Get(), slot.shader_zero.graph_completion,
        slot.shader_zero.graph_queue, {}, slot.shader_zero.NativeContract()},
        slot.shader_zero.graph_access, "HeroWork.ShaderZeroUpload");
    // Query results and visible arguments are separate graph destinations. The
    // graph owns CopySource -> IndirectArgs hazards; no hand-written transitions.
    slot.arguments_handle = graph.ImportBuffer({slot.arguments.resource.Get(), slot.arguments.graph_completion,
        slot.arguments.graph_queue, {}, slot.arguments.NativeContract()},
        slot.arguments.graph_access, "HeroWork.IndexedArgumentsReadback");
    if (layout)
    {
        layout->zero_upload = slot.shader_zero_handle;
        layout->arguments_readback = slot.arguments_handle;
        layout->argument_copy_pass = graph.PassCount();
    }
    auto pass = graph.AddPass(118, "HeroWorkCopyIndexedArguments", QueueHint::Direct);
    for (std::size_t i = 0; i < slot.contributors.size(); ++i)
    {
        const auto &source = slot.contributors[i];
        const auto previous = std::span(slot.contributors).first(i);
        if (std::ranges::any_of(previous, [&](const auto &other) {
                return other.arguments.resource.value == source.arguments.resource.value &&
                    other.arguments.ordinal != source.arguments.ordinal;
            })) return Fail("Hero contributors reference different versions of one argument buffer.");
        if (std::none_of(previous.begin(), previous.end(), [&](const auto &other) {
                return other.arguments.resource.value == source.arguments.resource.value;
            }))
            pass.Read(source.arguments, Access::CopySource);
    }
    const auto written = pass.Write(graph.Version(slot.arguments_handle), Access::CopyDest);
    graph.SetFinalAccess(written, Access::CopyDest);
    pass.SetExecute([&slot](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        for (std::size_t i = 0; i < slot.contributors.size(); ++i)
            list->CopyBufferRegion(slot.arguments.resource.Get(), i * 24, slot.contributors[i].argument_resource,
                slot.contributors[i].argument_offset, 24);
    });
    return Result::Success();
}
void HeroWorkCapture::DeclarePhase(RenderGraphBuilder &graph, PassBuilder &pass,
    std::uint32_t index, std::uint32_t phase, HeroWorkRetainedLayout *layout)
{
    auto &slot = *impl_->slots.at(index); auto &readback = slot.readbacks.at(phase);
    if (slot.contributors.empty()) return;
    auto &handle = slot.phase_handles.at(phase);
    handle = graph.ImportBuffer({readback.resource.Get(), readback.graph_completion,
        readback.graph_queue, {}, readback.NativeContract()},
        readback.graph_access, "HeroWork.NativeQueriesReadback");
    if (layout) layout->native_readback[phase] = handle;
    const auto written = pass.Write(graph.Version(handle), Access::CopyDest);
    graph.SetFinalAccess(written, Access::CopyDest);
    auto &gpu = slot.shader_gpu.at(phase); auto &shader_readback = slot.shader_readbacks.at(phase);
    auto &gpu_handle = slot.shader_handles.at(phase);
    gpu_handle = graph.ImportBuffer({gpu.resource.Get(), gpu.graph_completion,
        gpu.graph_queue, {}, gpu.NativeContract()}, gpu.graph_access, "HeroWork.ShaderCountersBitmap");
    if (layout)
    {
        layout->shader_gpu[phase] = gpu_handle;
        layout->shader_initialize_pass[phase] = graph.PassCount();
    }
    auto initialize = graph.AddPass(0x48570000u + phase * 2, "HeroWorkInitializeShaderCounters", QueueHint::Direct);
    initialize.Read(graph.Version(slot.shader_zero_handle), Access::CopySource);
    const auto initialized = initialize.Write(graph.Version(gpu_handle), Access::CopyDest);
    const auto bytes = impl_->shader_layout.phase_bytes;
    initialize.SetExecute([&slot, phase, bytes](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        list->CopyBufferRegion(slot.shader_gpu[phase].resource.Get(), 0, slot.shader_zero.resource.Get(), 0, bytes);
        slot.shader_clear_recorded[phase] = true;
    });
    const auto counted = pass.ReadWrite(initialized, Access::UnorderedWrite);
    auto &shader_handle = slot.shader_readback_handles.at(phase);
    shader_handle = graph.ImportBuffer({shader_readback.resource.Get(), shader_readback.graph_completion,
        shader_readback.graph_queue, {}, shader_readback.NativeContract()},
        shader_readback.graph_access, "HeroWork.ShaderHeaderReadback");
    if (layout)
    {
        layout->shader_readback[phase] = shader_handle;
        layout->shader_copy_pass[phase] = graph.PassCount();
    }
    auto resolve = graph.AddPass(0x48570001u + phase * 2, "HeroWorkCopyShaderCounters", QueueHint::Direct);
    resolve.Read(counted, Access::CopySource);
    const auto resolved = resolve.Write(graph.Version(shader_handle), Access::CopyDest);
    graph.SetFinalAccess(counted, Access::CopySource); graph.SetFinalAccess(resolved, Access::CopyDest);
    const auto stride = impl_->shader_layout.contributor_bytes;
    resolve.SetExecute([&slot, phase, stride](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        for (std::size_t i = 0; i < slot.contributors.size(); ++i)
            list->CopyBufferRegion(slot.shader_readbacks[phase].resource.Get(), i * kHeroWorkShaderHeaderBytes,
                slot.shader_gpu[phase].resource.Get(), i * stride, kHeroWorkShaderHeaderBytes);
        slot.shader_copy_recorded[phase] = true;
    });
}
Result HeroWorkCapture::BindRetainedBegin(RenderGraphBuilder &graph,
    const HeroTransparencyInputs &inputs, const HeroWorkRetainedLayout &layout)
{
    static_assert(kContributors == HeroWorkRetainedLayout::kMaxContributors);
    if (inputs.frame_slot >= impl_->slots.size() || layout.contributor_count > kContributors ||
        inputs.work_contributors.size() != layout.contributor_count ||
        !inputs.work_draw || !inputs.direct_queue || !inputs.direct_timestamp_frequency ||
        !inputs.submitted_frame ||
        (layout.contributor_count &&
            (layout.argument_copy_pass >= graph.PassCount() ||
             graph.DeclaredPassId(layout.argument_copy_pass) != 118 ||
             !graph.BindPass(layout.argument_copy_pass, 118))))
        return Fail("Hero retained contributor layout or frame is invalid.");
    auto &slot = *impl_->slots[inputs.frame_slot];
    if (slot.pending || !slot.completion.IsSatisfiedBy(inputs.completed))
        return Fail("Hero retained contributor slot is still in GPU use.");
    if (!slot.shader_zero.NativeContract() || !slot.arguments.NativeContract())
        return Fail("Hero retained contributor native owner changed.");
    for (std::size_t index = 0; index < inputs.work_contributors.size(); ++index)
    {
        const auto &source = inputs.work_contributors[index];
        const auto expected = layout.argument_versions[index];
        if (!source.argument_resource || !source.arguments.resource ||
            source.arguments.resource.kind != ResourceKind::Buffer ||
            source.arguments.resource.value != expected.resource.value ||
            source.arguments.ordinal != expected.ordinal)
            return Fail("Hero retained contributor argument topology changed.");
        const auto desc = source.argument_resource->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || source.argument_offset % 4 ||
            source.argument_offset > desc.Width || 24 > desc.Width - source.argument_offset)
            return Fail("Hero retained indexed argument range is invalid.");
    }
    if (!inputs.render_active.width || !inputs.render_active.height ||
        std::uint64_t(inputs.render_active.width) * inputs.render_active.height > impl_->shader_layout.pixels)
        return Fail("Hero retained shader active extent exceeds bitmap capacity.");
    slot.recorded.fill(false); slot.phase_handles.fill({});
    slot.shader_handles.fill({}); slot.shader_readback_handles.fill({});
    slot.shader_clear_recorded.fill(false); slot.shader_copy_recorded.fill(false);
    slot.contributors.assign(inputs.work_contributors.begin(), inputs.work_contributors.end());
    slot.queue = inputs.direct_queue; slot.frame = inputs.submitted_frame; slot.tick = inputs.tick;
    slot.view = inputs.view_id; slot.width = inputs.render_active.width;
    slot.height = inputs.render_active.height;
    slot.frequency = inputs.direct_timestamp_frequency; slot.success = false;
    if (slot.contributors.empty()) return Result::Success();
    void *zero{}; const D3D12_RANGE no_read{0, 0};
    const auto mapped = slot.shader_zero.resource->Map(0, &no_read, &zero);
    if (FAILED(mapped)) return Native("Map retained hero shader zero upload", mapped);
    std::memset(zero, 0, static_cast<std::size_t>(impl_->shader_layout.phase_bytes));
    for (UINT contributor = 0; contributor < kContributors; ++contributor)
    {
        const std::array<UINT, 8> header{kHeroWorkShaderMagic, kHeroWorkShaderVersion,
            slot.width, slot.height, impl_->shader_layout.bitmap_words,
            impl_->shader_layout.pixels, 0, static_cast<UINT>(impl_->mode)};
        std::memcpy(static_cast<std::byte *>(zero) + contributor *
            impl_->shader_layout.contributor_bytes, header.data(), sizeof(header));
    }
    const D3D12_RANGE zero_written{0, static_cast<SIZE_T>(impl_->shader_layout.phase_bytes)};
    slot.shader_zero.resource->Unmap(0, &zero_written);
    if (!graph.BindBuffer(layout.zero_upload, {slot.shader_zero.resource.Get(),
            slot.shader_zero.graph_completion, slot.shader_zero.graph_queue, {},
            slot.shader_zero.NativeContract()},
            slot.shader_zero.graph_access) ||
        !graph.BindBuffer(layout.arguments_readback, {slot.arguments.resource.Get(),
            slot.arguments.graph_completion, slot.arguments.graph_queue, {},
            slot.arguments.NativeContract()},
            slot.arguments.graph_access))
        return Fail("Hero retained contributor imports changed.");
    slot.shader_zero_handle = layout.zero_upload;
    slot.arguments_handle = layout.arguments_readback;
    auto pass = graph.BindPass(layout.argument_copy_pass, 118);
    if (!pass) return Fail("Hero retained argument copy pass was already bound.");
    pass->SetExecute([&slot](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        for (std::size_t index = 0; index < slot.contributors.size(); ++index)
            list->CopyBufferRegion(slot.arguments.resource.Get(), index * 24,
                slot.contributors[index].argument_resource,
                slot.contributors[index].argument_offset, 24);
    });
    return Result::Success();
}
Result HeroWorkCapture::BindRetainedPhase(RenderGraphBuilder &graph, std::uint32_t index,
    std::uint32_t phase, const HeroWorkRetainedLayout &layout)
{
    if (index >= impl_->slots.size() || phase >= kPhases)
        return Fail("Hero retained phase slot is invalid.");
    auto &slot = *impl_->slots[index];
    if (slot.contributors.empty()) return Result::Success();
    if (
        !layout.native_readback[phase] || !layout.shader_gpu[phase] ||
        !layout.shader_readback[phase] ||
        !graph.BindPass(layout.shader_initialize_pass[phase], 0x48570000u + phase * 2) ||
        !graph.BindPass(layout.shader_copy_pass[phase], 0x48570001u + phase * 2))
        return Fail("Hero retained phase topology changed.");
    auto &readback = slot.readbacks[phase];
    auto &gpu = slot.shader_gpu[phase];
    auto &shader_readback = slot.shader_readbacks[phase];
    if (!readback.NativeContract() || !gpu.NativeContract() ||
        !shader_readback.NativeContract())
        return Fail("Hero retained phase native owner changed.");
    if (!graph.BindBuffer(layout.native_readback[phase], {readback.resource.Get(),
            readback.graph_completion, readback.graph_queue, {}, readback.NativeContract()},
            readback.graph_access) ||
        !graph.BindBuffer(layout.shader_gpu[phase], {gpu.resource.Get(),
            gpu.graph_completion, gpu.graph_queue, {}, gpu.NativeContract()}, gpu.graph_access) ||
        !graph.BindBuffer(layout.shader_readback[phase], {shader_readback.resource.Get(),
            shader_readback.graph_completion, shader_readback.graph_queue, {},
            shader_readback.NativeContract()},
            shader_readback.graph_access))
        return Fail("Hero retained phase imports changed.");
    slot.phase_handles[phase] = layout.native_readback[phase];
    slot.shader_handles[phase] = layout.shader_gpu[phase];
    slot.shader_readback_handles[phase] = layout.shader_readback[phase];
    const auto bytes = impl_->shader_layout.phase_bytes;
    auto initialize = graph.BindPass(layout.shader_initialize_pass[phase], 0x48570000u + phase * 2);
    auto resolve = graph.BindPass(layout.shader_copy_pass[phase], 0x48570001u + phase * 2);
    if (!initialize || !resolve) return Fail("Hero retained phase passes were already bound.");
    initialize->SetExecute([&slot, phase, bytes](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        list->CopyBufferRegion(slot.shader_gpu[phase].resource.Get(), 0,
            slot.shader_zero.resource.Get(), 0, bytes);
        slot.shader_clear_recorded[phase] = true;
    });
    const auto stride = impl_->shader_layout.contributor_bytes;
    resolve->SetExecute([&slot, phase, stride](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        for (std::size_t contributor = 0; contributor < slot.contributors.size(); ++contributor)
            list->CopyBufferRegion(slot.shader_readbacks[phase].resource.Get(),
                contributor * kHeroWorkShaderHeaderBytes,
                slot.shader_gpu[phase].resource.Get(), contributor * stride,
                kHeroWorkShaderHeaderBytes);
        slot.shader_copy_recorded[phase] = true;
    });
    return Result::Success();
}
void HeroWorkCapture::Record(ID3D12GraphicsCommandList *list, ID3D12PipelineState *pipeline,
    const HeroTransparencyInputs &inputs, HeroTransparencyPhase semantic, std::uint32_t layer, std::uint32_t phase,
    D3D12_GPU_DESCRIPTOR_HANDLE table)
{
    auto &slot = *impl_->slots.at(inputs.frame_slot);
    for (UINT contributor = 0; contributor < slot.contributors.size(); ++contributor)
    {
        const UINT query = phase * kContributors + contributor;
        auto &row = slot.rows[query]; row = {};
        row.completed_frame = slot.frame; row.tick = slot.tick; row.view_id = slot.view;
        row.phase = static_cast<UINT>(semantic); row.layer = layer; row.contributor = contributor;
        row.width = slot.width; row.height = slot.height;
        row.source = slot.contributors[contributor].source; row.pipeline = impl_->pipelines[PipelineIndex(semantic, layer)];
        row.gpu_frequency = slot.frequency; row.cpu_qpc_frequency = impl_->qpc_frequency;
        list->EndQuery(slot.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query * 2);
        list->BeginQuery(slot.statistics.Get(), impl_->statistics_type, query);
        list->BeginQuery(slot.occlusion.Get(), D3D12_QUERY_TYPE_OCCLUSION, query);
        LARGE_INTEGER begin{}, end{}; QueryPerformanceCounter(&begin);
        HeroWorkDrawCalls calls;
        try { calls = inputs.work_draw(list, pipeline, semantic, layer, table, contributor,
            slot.shader_gpu[phase].resource->GetGPUVirtualAddress() + contributor * impl_->shader_layout.contributor_bytes); }
        catch (...)
        {
            list->EndQuery(slot.occlusion.Get(), D3D12_QUERY_TYPE_OCCLUSION, query);
            list->EndQuery(slot.statistics.Get(), impl_->statistics_type, query);
            list->EndQuery(slot.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query * 2 + 1);
            throw; // failed graph never publishes this partial native query set
        }
        QueryPerformanceCounter(&end);
        list->EndQuery(slot.occlusion.Get(), D3D12_QUERY_TYPE_OCCLUSION, query);
        list->EndQuery(slot.statistics.Get(), impl_->statistics_type, query);
        list->EndQuery(slot.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query * 2 + 1);
        row.cpu_begin_qpc = static_cast<UINT64>(begin.QuadPart); row.cpu_end_qpc = static_cast<UINT64>(end.QuadPart);
        row.execute_indirect_calls = calls.execute_indirect; row.draw_indexed_calls = calls.draw_indexed;
        row.dispatch_mesh_calls = calls.dispatch_mesh;
        const UINT64 offset = contributor * kRowBytes;
        auto *readback = slot.readbacks[phase].resource.Get();
        // Only queries begun/ended in this callback are resolved. CPU recording
        // may be parallel; no final callback races against another phase's flags.
        list->ResolveQueryData(slot.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query * 2, 2, readback, offset);
        list->ResolveQueryData(slot.statistics.Get(), impl_->statistics_type, query, 1, readback, offset + 16);
        list->ResolveQueryData(slot.occlusion.Get(), D3D12_QUERY_TYPE_OCCLUSION, query, 1, readback, offset + 128);
        slot.recorded[query] = true;
    }
}
void HeroWorkCapture::Submitted(const RenderGraphBuilder &graph, std::uint32_t index,
    const CompletionSet &completion, bool success)
{
    auto &slot = *impl_->slots.at(index);
    slot.completion.Merge(completion);
    auto retain = [&](AllocationResource &resource, ResourceHandle handle) {
        if (!handle) return;
        const auto used = graph.ResourceCompletion(handle); slot.completion.Merge(used);
        resource.graph_completion.Merge(completion); resource.graph_completion.Merge(used);
        if (success) { resource.graph_access = graph.FinalAccess(handle); resource.graph_queue = graph.FinalQueue(handle); }
    };
    retain(slot.arguments, slot.arguments_handle);
    retain(slot.shader_zero, slot.shader_zero_handle);
    for (UINT i = 0; i < kPhases; ++i)
    {
        retain(slot.readbacks[i], slot.phase_handles[i]);
        retain(slot.shader_gpu[i], slot.shader_handles[i]);
        retain(slot.shader_readbacks[i], slot.shader_readback_handles[i]);
        impl_->status.shader_clear_commands += slot.shader_clear_recorded[i];
        if (slot.shader_copy_recorded[i]) impl_->status.shader_copy_commands += slot.contributors.size();
    }
    slot.pending = true; slot.success = success; ++impl_->status.submitted_frames;
    if (!success) ++impl_->status.failed_frames;
}
Result HeroWorkCapture::Collect(const CompletionSet &completed, std::vector<RendererHeroWorkSample> &output)
{
    for (auto &owned : impl_->slots)
    {
        auto &slot = *owned;
        if (!slot.pending || !slot.completion.IsSatisfiedBy(completed)) continue;
        if (!slot.success) { slot.pending = false; continue; }
        const auto deviceStatus = impl_->device->GetDeviceRemovedReason();
        if (FAILED(deviceStatus))
        {
            slot.success = false; ++impl_->status.failed_frames;
            return Native("Read hero contributor queries on removed device", deviceStatus);
        }
        const auto count = std::count(slot.recorded.begin(), slot.recorded.end(), true);
        if (output.size() + count > kPendingLimit)
        { impl_->status.overflowed = true; return Fail("Hero contributor capture exceeded its bounded pending rows; drain every frame."); }
        UINT64 gpuAnchor{}, qpcAnchor{};
        const bool calibration = SUCCEEDED(slot.queue->GetClockCalibration(&gpuAnchor, &qpcAnchor));
        std::vector<RendererHeroWorkSample> ready;
        ready.reserve(static_cast<std::size_t>(count));
        std::array<std::array<UINT, 6>, kContributors> arguments{};
        void *mapped{}; const D3D12_RANGE argumentRange{0, static_cast<SIZE_T>(slot.contributors.size() * 24)};
        if (!slot.contributors.empty())
        {
            auto hr = slot.arguments.resource->Map(0, &argumentRange, &mapped);
            if (FAILED(hr)) return Native("Map hero indexed arguments", hr);
            std::memcpy(arguments.data(), mapped, argumentRange.End);
            const D3D12_RANGE noWrite{0, 0}; slot.arguments.resource->Unmap(0, &noWrite);
        }
        for (UINT phase = 0; phase < kPhases; ++phase)
        {
            const auto first = slot.recorded.begin() + phase * kContributors;
            if (std::none_of(first, first + kContributors, [](bool value) { return value; })) continue;
            std::array<std::array<UINT, kHeroWorkShaderHeaderBytes / 4>, kContributors> headers{};
            const D3D12_RANGE shader_range{0, static_cast<SIZE_T>(slot.contributors.size() * kHeroWorkShaderHeaderBytes)};
            auto shader_hr = slot.shader_readbacks[phase].resource->Map(0, &shader_range, &mapped);
            if (FAILED(shader_hr)) { ++impl_->status.shader_invalid_samples; return Native("Map completed hero shader counters", shader_hr); }
            std::memcpy(headers.data(), mapped, shader_range.End);
            const D3D12_RANGE shader_no_write{0, 0}; slot.shader_readbacks[phase].resource->Unmap(0, &shader_no_write);
            const D3D12_RANGE range{0, static_cast<SIZE_T>(kPhaseBytes)};
            auto hr = slot.readbacks[phase].resource->Map(0, &range, &mapped);
            if (FAILED(hr)) return Native("Map hero native queries", hr);
            for (UINT contributor = 0; contributor < slot.contributors.size(); ++contributor)
            {
                const UINT query = phase * kContributors + contributor;
                if (!slot.recorded[query]) continue;
                auto row = slot.rows[query];
                const auto *bytes = static_cast<const std::byte *>(mapped) + contributor * kRowBytes;
                std::memcpy(&row.gpu_begin, bytes, 8); std::memcpy(&row.gpu_end, bytes + 8, 8);
                // Classic copies exactly88 bytes. The unused24 bytes remain zero
                // in the value-initialized row and are explicitly unavailable.
                std::memcpy(row.pipeline_statistics.data(), bytes + 16, impl_->statistics_bytes);
                std::memcpy(&row.occlusion_samples, bytes + 128, 8);
                row.timestamp_valid = row.gpu_end >= row.gpu_begin && row.gpu_frequency != 0;
                row.statistics_valid = row.occlusion_valid = true;
                row.mesh_statistics_available = impl_->status.mesh_statistics_supported;
                row.calibration_valid = calibration && HeroWorkCalibrate(row.gpu_begin, gpuAnchor, qpcAnchor,
                    row.gpu_frequency, row.cpu_qpc_frequency, row.calibrated_begin_qpc) &&
                    HeroWorkCalibrate(row.gpu_end, gpuAnchor, qpcAnchor, row.gpu_frequency,
                        row.cpu_qpc_frequency, row.calibrated_end_qpc);
                row.indexed_arguments = arguments[contributor];
                row.indirect_arguments_valid = true; // bytes were copied and read after completed submission
                row.indirect_arguments_in_range = HeroWorkIndexedArgumentsValid(row.indexed_arguments,
                    row.source.geometry_indices, row.source.candidate_instances);
                const auto &header = headers[contributor];
                std::copy_n(header.begin() + 8, row.shader_counters.size(), row.shader_counters.begin());
                row.shader_counter_flags = header[6]; row.shader_bitmap_pixels = header[5];
                row.shader_counters_available = true;
                row.diagnostic_mode = impl_->mode;
                row.shader_counters_valid = slot.shader_clear_recorded[phase] && slot.shader_copy_recorded[phase] &&
                    header[0] == kHeroWorkShaderMagic && header[1] == kHeroWorkShaderVersion &&
                    header[2] == slot.width && header[3] == slot.height && header[4] == impl_->shader_layout.bitmap_words &&
                    header[5] == impl_->shader_layout.pixels && header[6] == 0 && header[7] == static_cast<UINT>(impl_->mode) &&
                    row.shader_counters[2] <= std::uint64_t(slot.width) * slot.height &&
                    row.shader_counters[17] <= row.shader_counters[2] && row.shader_counters[15] <= row.shader_counters[0] &&
                    std::uint64_t(row.shader_counters[22]) + row.shader_counters[23] == row.shader_counters[21] &&
                    std::uint64_t(row.shader_counters[20]) * 32 == row.shader_counters[21];
                row.unique_pixels_available = row.layer_rejections_available = row.shader_counters_valid;
                // Logical evaluator calls are exported; DCE-retained full instruction work and sampled mip remain unavailable.
                if (!row.shader_counters_valid) ++impl_->status.shader_invalid_samples;
                ready.push_back(std::move(row));
            }
            const D3D12_RANGE noWrite{0, 0}; slot.readbacks[phase].resource->Unmap(0, &noWrite);
        }
        output.insert(output.end(), std::make_move_iterator(ready.begin()), std::make_move_iterator(ready.end()));
        impl_->status.samples += ready.size();
        ++impl_->status.completed_frames; slot.pending = false;
    }
    return Result::Success();
}
RendererHeroWorkStatus HeroWorkCapture::Status() const
{
    auto status = impl_->status;
    for (const auto &slot : impl_->slots) if (slot->pending) ++status.pending_frames;
    return status;
}
} // namespace hs
