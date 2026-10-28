#include "renderer_decal_surface.hpp"
#include "renderer_pipeline_cache.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace hs
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr std::uint32_t kConstantStride = 256;
constexpr std::array kOutputFormats{DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16_SNORM,
    DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT};
Result Failure(ErrorCode code, const char *message)
{ return Result::Failure(code, "hs_decal_surface", message); }
Result Native(const char *operation, HRESULT hr)
{ return Result::Failure(ErrorCode::TaskFailed, "hs_decal_surface", std::string(operation) + ": " + std::to_string(hr)); }
bool Valid(ViewExtent extent)
{ return extent.width && extent.height && extent.width <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION && extent.height <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION; }
bool Fits(const DecalSurfaceGuide &guide, ViewExtent extent, DXGI_FORMAT format)
{
    if (!guide.resource || !guide.version.resource) return false;
    const auto d = guide.resource->GetDesc();
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.DepthOrArraySize == 1 &&
        d.SampleDesc.Count == 1 && d.Width >= extent.width && d.Height >= extent.height && d.Format == format;
}
bool Buffer(const DecalSurfaceGuide &guide)
{ return guide.resource && guide.version.resource && guide.resource->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER; }
}
struct DecalSurfaceTargets
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> exports;
    std::array<AllocationResource, 4> outputs;
    AllocationResource constants;
    std::byte *mapped{};
    ViewExtent capacity;
    std::uint32_t descriptor_stride{};
    std::vector<CompletionSet> slot_completion;
    CompletionSet completion;
    std::uint64_t bytes{};
    bool usable{true};
    ~DecalSurfaceTargets() { if (mapped && constants.resource) constants.resource->Unmap(0, nullptr); }
    D3D12_CPU_DESCRIPTOR_HANDLE Export(std::uint32_t index) const
    { auto h = exports->GetCPUDescriptorHandleForHeapStart(); h.ptr += SIZE_T(index) * descriptor_stride; return h; }
};
std::uint64_t DecalSurfaceCandidate::AllocatedBytes() const noexcept { return targets_ ? targets_->bytes : 0; }
std::uint64_t DecalSurfaceSystem::AllocatedBytes() const noexcept { return targets_ ? targets_->bytes : 0; }

Result DecalSurfaceSystem::Prepare(ID3D12Device *device, D3D12MA::Allocator *allocator,
    ID3D12RootSignature *root, RendererPipelineCache &cache, ViewExtent capacity, std::uint32_t frames, bool has_work,
    const ShaderReader &read, DecalSurfaceCandidate &candidate) const
{
    candidate = {};
    if (!has_work) { candidate.prepared_ = true; return Result::Success(); }
    if (!device || !allocator || !root || !Valid(capacity) || !frames || !read)
        return Failure(ErrorCode::InvalidArgument, "Decal preparation requires a device, main root, capacity, frame banks and selected shader reader.");
    for (const auto format : kOutputFormats)
    {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
        const auto hr = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
        if (FAILED(hr)) return Native("Query decal Surface format", hr);
        if (!(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD) ||
            !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
            return Failure(ErrorCode::TaskFailed, "Decal Surface output requires shader load and typed UAV store support.");
    }
    auto next = std::make_shared<DecalSurfaceTargets>();
    next->device = device; next->root = root; next->capacity = capacity;
    next->slot_completion.resize(frames);
    next->descriptor_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    std::vector<std::byte> shader;
    if (auto r = read("decal_surface_cs.dxil", shader); !r) return r;
    if (shader.size() < 32) return Failure(ErrorCode::InvalidArgument, "Selected decal Surface shader is absent or truncated.");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{}; pso.pRootSignature = root; pso.CS = {shader.data(), shader.size()};
    auto hr = cache.CreateCompute(&pso, IID_PPV_ARGS(&next->pipeline));
    if (FAILED(hr)) return Native("Create decal Surface pipeline", hr);
    D3D12MA::ALLOCATION_DESC allocation{}; allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = capacity.width; d.Height = capacity.height; d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    for (std::size_t index = 0; index < next->outputs.size(); ++index)
    {
        auto &out = next->outputs[index]; d.Format = kOutputFormats[index];
        hr = allocator->CreateResource(&allocation, &d, D3D12_RESOURCE_STATE_COMMON, nullptr,
            &out.allocation, IID_PPV_ARGS(out.resource.ReleaseAndGetAddressOf()));
        if (FAILED(hr)) return Native("Allocate decal Surface image", hr);
        out.graph_access = Access::Common; next->bytes += out.allocation->GetSize();
    }
    d = {}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = UINT64(frames) * kConstantStride;
    d.Height = 1; d.DepthOrArraySize = d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    hr = allocator->CreateResource(&allocation, &d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        &next->constants.allocation, IID_PPV_ARGS(next->constants.resource.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) return Native("Allocate decal Surface constants", hr);
    next->constants.graph_access = Access::AllShaderRead; next->bytes += next->constants.allocation->GetSize();
    D3D12_RANGE no_read{0, 0}; void *mapped{};
    hr = next->constants.resource->Map(0, &no_read, &mapped);
    if (FAILED(hr)) return Native("Map decal Surface constants", hr);
    next->mapped = static_cast<std::byte *>(mapped);
    D3D12_DESCRIPTOR_HEAP_DESC heap{}; heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = kDecalSurfaceExportCount;
    hr = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&next->exports));
    if (FAILED(hr)) return Native("Create decal Surface export descriptors", hr);
    for (std::uint32_t index = 0; index < next->outputs.size(); ++index)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format = kOutputFormats[index];
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(next->outputs[index].resource.Get(), &srv, next->Export(index));
    }
    candidate.targets_ = std::move(next); candidate.prepared_ = true; return Result::Success();
}
Result DecalSurfaceSystem::Commit(DecalSurfaceCandidate &&candidate, const CompletionSet &completed)
{
    if (!candidate.prepared_) return Failure(ErrorCode::InvalidArgument, "Decal candidate was not prepared.");
    if (pending_graph_ || (targets_ && !targets_->completion.IsSatisfiedBy(completed)))
        return Failure(ErrorCode::InvalidState, "Complete and drain all decal Surface queue uses before replacement.");
    targets_ = std::move(candidate.targets_); candidate.prepared_ = false; return Result::Success();
}
Result DecalSurfaceSystem::Register(RenderGraphBuilder &graph, const DecalSurfaceInputs &input,
    DecalSurfaceConstants constants, DecalSurfaceOutputs &output,DecalSurfaceLayout *layout)
{
    return RegisterFrame(graph,input,constants,output,layout,nullptr);
}
bool DecalSurfaceSystem::MatchesShape(const DecalSurfaceLayout &layout, bool active) const noexcept
{
    if(!layout.valid||layout.active!=active)return false;
    if(!layout.active)return true;
    const auto &t=targets_;
    return t&&layout.capacity.width==t->capacity.width&&layout.capacity.height==t->capacity.height&&
        layout.frame_slots==t->slot_completion.size();
}
bool DecalSurfaceSystem::MatchesLayout(const DecalSurfaceInputs &input,const DecalSurfaceLayout &layout)const noexcept
{
    return MatchesShape(layout, input.command_count != 0) && (!layout.active || layout.guides==std::array{
            input.depth.version,input.metadata.version,input.base.version,input.normal.version,input.material.version,
            input.commands.version,input.parameters.version,input.gradient.version,input.textures[0].version,input.textures[1].version});
}
Result DecalSurfaceSystem::BindRetained(RenderGraphBuilder &graph,const DecalSurfaceInputs &input,
    DecalSurfaceConstants constants,const DecalSurfaceLayout &layout,DecalSurfaceOutputs &output)
{
    return RegisterFrame(graph,input,constants,output,nullptr,&layout);
}
Result DecalSurfaceSystem::RegisterFrame(RenderGraphBuilder &graph,const DecalSurfaceInputs &input,
    DecalSurfaceConstants constants,DecalSurfaceOutputs &output,
    DecalSurfaceLayout *capture,const DecalSurfaceLayout *retained)
{
    output = {}; output.base = input.base; output.normal = input.normal; output.material = input.material;
    if(capture)*capture={};
    if(retained&&!MatchesLayout(input,*retained))return Failure(ErrorCode::InvalidState,"Decal retained topology changed.");
    if (!input.command_count){if(capture)capture->valid=true;return Result::Success();}
    const auto t = targets_;
    if (!t || !t->usable || pending_graph_ || !Valid(input.render_active) ||
        input.render_active.width > t->capacity.width || input.render_active.height > t->capacity.height ||
        input.frame_slot >= t->slot_completion.size() || !t->slot_completion[input.frame_slot].IsSatisfiedBy(input.completed) ||
        !input.main_heap || !input.descriptor_suffix_cpu.ptr || !input.main_table_gpu.ptr ||
        !Fits(input.base, input.render_active, DXGI_FORMAT_R8G8B8A8_UNORM) ||
        !Fits(input.normal, input.render_active, DXGI_FORMAT_R16G16_SNORM) ||
        !Fits(input.material, input.render_active, DXGI_FORMAT_R8G8B8A8_UNORM) ||
        !Fits(input.metadata, input.render_active, DXGI_FORMAT_R32G32_UINT) || !input.depth.resource ||
        !Buffer(input.commands) || !Buffer(input.parameters) || !input.parameter_slot_count ||
        input.command_byte_offset % 4u != 0)
        return Failure(ErrorCode::InvalidArgument, "Decal registration requires prepared capacity, a waited frame bank, main descriptors and valid Surface/command guides.");
    const auto depth = input.depth.resource->GetDesc();
    if (!input.depth.version.resource || depth.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || depth.DepthOrArraySize != 1 ||
        depth.SampleDesc.Count != 1 || depth.Width < input.render_active.width || depth.Height < input.render_active.height ||
        (depth.Format != DXGI_FORMAT_D32_FLOAT && depth.Format != DXGI_FORMAT_R32_TYPELESS && depth.Format != DXGI_FORMAT_R32_FLOAT))
        return Failure(ErrorCode::InvalidArgument, "Decal depth guide must be the current full-resolution Surface depth.");
    const auto command_bytes = UINT64(input.command_count) * kDecalSurfaceCommandStride;
    const auto command_width = input.commands.resource->GetDesc().Width;
    if (input.command_count > UINT32_MAX / kDecalSurfaceCommandStride || input.command_byte_offset > command_width ||
        command_bytes > command_width - input.command_byte_offset ||
        UINT64(input.parameter_slot_count) * sizeof(render_data::MaterialParameterSlot) > input.parameters.resource->GetDesc().Width)
        return Failure(ErrorCode::InvalidArgument, "Admitted decal commands or reflected parameters exceed their actual buffer range.");
    if (!input.gradient.resource || !input.gradient.version.resource)
        return Failure(ErrorCode::InvalidArgument, "Decal requires the current authored gradient texture.");
    const auto gradient = input.gradient.resource->GetDesc();
    if (gradient.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || gradient.DepthOrArraySize != 1 ||
        gradient.SampleDesc.Count != 1 || !gradient.Width || !gradient.Height || gradient.Width > UINT32_MAX)
        return Failure(ErrorCode::InvalidArgument, "Decal gradient must be a nonempty 2D texture.");
    std::array<DecalSurfaceGuide, 10> reads{input.depth, input.metadata, input.base, input.normal, input.material,
        input.commands, input.gradient, input.parameters, input.textures[0], input.textures[1]};
    for (const auto &guide : reads)
        if (!guide.resource || !guide.version.resource || graph.Resolve(guide.version.resource) != guide.resource)
            return Failure(ErrorCode::InvalidArgument, "Decal guide resource does not match its graph version.");
    for (std::size_t index = 0; index < input.materials.size(); ++index)
    {
        const auto &m = input.materials[index]; const auto &p = m.package;
        try { render_data::ValidateDecalReceiverPolicy(p.receivers); }
        catch (const std::exception &) { return Failure(ErrorCode::InvalidArgument, "Decal package receiver policy is invalid."); }
        if (p.program.kind != render_data::MaterialProgramKind::DecalSurfaceCS ||
            m.parameter_base > input.parameter_slot_count || !m.parameter_count ||
            m.parameter_count > input.parameter_slot_count - m.parameter_base)
            return Failure(ErrorCode::InvalidArgument, "Decal package program or parameter range is invalid.");
        const auto texture = input.textures[index].resource->GetDesc();
        if (texture.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || texture.SampleDesc.Count != 1 ||
            !texture.Width || !texture.Height || texture.DepthOrArraySize < (index == 0 ? 4u : 8u) ||
            (index == 0 ? texture.Format != DXGI_FORMAT_R8G8B8A8_UNORM && texture.Format != DXGI_FORMAT_BC7_UNORM :
                          texture.Format != DXGI_FORMAT_R8_UNORM && texture.Format != DXGI_FORMAT_BC4_UNORM))
            return Failure(ErrorCode::InvalidArgument, "Decal texture array violates its declared structure/mask channel contract.");
        const auto &r = p.receivers;
        constants.receivers[index] = {r.model_mask, r.required_surface_flags, r.forbidden_surface_flags, std::uint32_t(r.normal_filter)};
    }
    const auto *matrix = &constants.inverse_view_projection._11;
    if (!std::all_of(matrix, matrix + 16, [](float value) { return std::isfinite(value); }) ||
        !std::isfinite(constants.view_origin.x) || !std::isfinite(constants.view_origin.y) || !std::isfinite(constants.view_origin.z))
        return Failure(ErrorCode::InvalidArgument, "Decal camera reconstruction constants must be finite.");
    for (const auto &read : reads) for (const auto &target : t->outputs)
        if (read.resource == target.resource.Get())
            return Failure(ErrorCode::InvalidArgument, "Decal source and destination Surface images must not alias.");
    constants.render_size = {float(input.render_active.width), float(input.render_active.height),
        1.0f / input.render_active.width, 1.0f / input.render_active.height};
    constants.info = {input.command_count, input.parameter_slot_count, UINT(gradient.Width), gradient.Height};
    constants.parameter_bases = {input.materials[0].parameter_base, input.materials[1].parameter_base, 0, 0};
    std::memcpy(t->mapped + SIZE_T(input.frame_slot) * kConstantStride, &constants, sizeof(constants));
    const auto cpu = [&](std::uint32_t offset) { auto h = input.descriptor_suffix_cpu; h.ptr += SIZE_T(offset) * t->descriptor_stride; return h; };
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{t->constants.resource->GetGPUVirtualAddress() + UINT64(input.frame_slot) * kConstantStride, kConstantStride};
    t->device->CreateConstantBufferView(&cbv, cpu(kDecalSurfaceCbvOffset));
    for (std::uint32_t index = 0; index < reads.size(); ++index)
    {
        const auto &guide = reads[index]; const auto d = guide.resource->GetDesc();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (index == 5)
        {
            srv.Format = DXGI_FORMAT_R32_TYPELESS; srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srv.Buffer.FirstElement = input.command_byte_offset / 4u; srv.Buffer.NumElements = UINT(command_bytes / 4u);
            srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        }
        else if (index == 7)
        {
            srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; srv.Buffer.NumElements = input.parameter_slot_count;
            srv.Buffer.StructureByteStride = sizeof(render_data::MaterialParameterSlot);
        }
        else if (index >= 8)
        {
            srv.Format = d.Format; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srv.Texture2DArray.MipLevels = d.MipLevels; srv.Texture2DArray.ArraySize = d.DepthOrArraySize;
        }
        else
        {
            srv.Format = index == 0 ? DXGI_FORMAT_R32_FLOAT : d.Format;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels = 1;
        }
        t->device->CreateShaderResourceView(guide.resource, &srv, cpu(kDecalSurfaceSrvOffset + index));
    }
    std::array<ResourceHandle, 4> handles;
    std::array<ResourceVersion, 4> written;
    for (std::uint32_t index = 0; index < handles.size(); ++index)
    {
        auto &target = t->outputs[index];
        if(auto checked=target.EnsureNativeContract(t->device.Get());!checked)return checked;
        handles[index] = retained?retained->outputs[index]:
            graph.ImportTexture({target.resource.Get(), target.graph_completion, target.graph_queue,{},target.NativeContract()}, target.graph_access,
                index == 0 ? "DecalSurface.Base" : index == 1 ? "DecalSurface.Normal" : index == 2 ? "DecalSurface.Material" : "DecalSurface.Emission");
        if(retained&&!graph.BindTexture(handles[index],{target.resource.Get(),target.graph_completion,target.graph_queue,{},target.NativeContract()},target.graph_access))
            return Failure(ErrorCode::InvalidState,"Decal retained output binding failed.");
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.Format = kOutputFormats[index]; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        t->device->CreateUnorderedAccessView(target.resource.Get(), nullptr, &uav, cpu(kDecalSurfaceUavOffset + index));
        t->device->CopyDescriptorsSimple(1, cpu(kDecalSurfaceExportOffset + index), t->Export(index), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        output.export_srvs[index] = t->Export(index);
    }
    if(auto checked=t->constants.EnsureNativeContract(t->device.Get());!checked)return checked;
    const auto constants_handle = retained?retained->constants:
        graph.ImportBuffer({t->constants.resource.Get(), t->constants.graph_completion, t->constants.graph_queue,{},t->constants.NativeContract()},
            Access::AllShaderRead, "DecalSurface.Constants");
    if(retained&&!graph.BindBuffer(constants_handle,{t->constants.resource.Get(),t->constants.graph_completion,t->constants.graph_queue,{},t->constants.NativeContract()},Access::AllShaderRead))
        return Failure(ErrorCode::InvalidState,"Decal retained constant binding failed.");
    if(capture)capture->pass=graph.PassCount();
    auto passBinding=retained?graph.BindPass(retained->pass,295):
        std::optional<PassBuilder>{graph.AddPass(295,"MaterialIRDecalSurface",QueueHint::Direct)};
    if(!passBinding)return Failure(ErrorCode::InvalidState,"Decal retained pass is missing.");
    auto &pass=*passBinding;
    if(retained)written=retained->written;
    else
    {
        for(const auto &guide:reads)pass.Read(guide.version,Access::ComputeRead);
        pass.Read(graph.Version(constants_handle),Access::AllShaderRead);
        for(std::size_t index=0;index<handles.size();++index)written[index]=pass.Write(graph.Version(handles[index]),Access::UnorderedWrite);
    }
    pass.SetExecute([t, heap = input.main_heap, table = input.main_table_gpu, extent = input.render_active](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        ID3D12DescriptorHeap *heaps[]{heap}; list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(t->root.Get()); list->SetComputeRootDescriptorTable(kDecalSurfaceRootTable, table);
        list->SetPipelineState(t->pipeline.Get()); list->Dispatch((extent.width + 7u) / 8u, (extent.height + 7u) / 8u, 1);
    });
    output.base = {written[0], t->outputs[0].resource.Get()}; output.normal = {written[1], t->outputs[1].resource.Get()};
    output.material = {written[2], t->outputs[2].resource.Get()}; output.emission = {written[3], t->outputs[3].resource.Get()}; output.active = true;
    pending_graph_ = &graph; pending_outputs_ = handles; pending_constants_ = constants_handle; pending_slot_ = input.frame_slot;
    if(capture)
    {
        capture->valid=true;capture->active=true;capture->capacity=t->capacity;
        capture->frame_slots=static_cast<std::uint32_t>(t->slot_completion.size());
        capture->guides={input.depth.version,input.metadata.version,input.base.version,input.normal.version,input.material.version,
            input.commands.version,input.parameters.version,input.gradient.version,input.textures[0].version,input.textures[1].version};
        capture->outputs=handles;capture->written=written;capture->constants=constants_handle;
    }
    return Result::Success();
}
Result DecalSurfaceSystem::CompleteSubmission(const RenderGraphBuilder &graph, const CompletionSet &completion, bool success)
{
    if (!pending_graph_) return Result::Success();
    if (pending_graph_ != &graph) return Failure(ErrorCode::InvalidArgument, "Decal completed a different graph.");
    const auto t = targets_; t->completion.Merge(completion); t->slot_completion[pending_slot_].Merge(completion);
    const auto finish = [&](AllocationResource &allocation, ResourceHandle handle) {
        const auto used = graph.ResourceCompletion(handle); t->completion.Merge(used); t->slot_completion[pending_slot_].Merge(used);
        allocation.graph_completion.Merge(completion); allocation.graph_completion.Merge(used);
        if (success) { allocation.graph_access = graph.FinalAccess(handle); allocation.graph_queue = graph.FinalQueue(handle); }
    };
    for (std::size_t index = 0; index < pending_outputs_.size(); ++index) finish(t->outputs[index], pending_outputs_[index]);
    finish(t->constants, pending_constants_);
    if (!success) t->usable = false;
    pending_graph_ = nullptr; pending_outputs_ = {}; pending_constants_ = {}; return Result::Success();
}
Result DecalSurfaceSystem::ResetAfterCompletion(const CompletionSet &completed)
{
    if (pending_graph_ || (targets_ && !targets_->completion.IsSatisfiedBy(completed)))
        return Failure(ErrorCode::InvalidState, "Complete and drain all decal Surface queue uses before reset.");
    targets_.reset(); return Result::Success();
}
} // namespace hs
