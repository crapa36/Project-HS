#include "renderer_impl.hpp"
#include <hs/core/sha256.hpp>
#include <hs/render_data/terrain_surface.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace hs
{
namespace
{
constexpr std::string_view kTerrainArchive="render/terrain/arena_surface.rpkg";
Result TerrainFailure(std::string message)
{ return Result::Failure(ErrorCode::InvalidState,"hs_renderer_d3d12",std::move(message)); }
void Transition(ID3D12GraphicsCommandList *list,ID3D12Resource *resource,
    D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};
    list->ResourceBarrier(1,&barrier);
}
}

auto D3D12Renderer::Impl::TerrainSectionFrom(const RenderInstance &r) -> std::optional<TerrainSection>
{
    const bool meadow=render_data::EnvironmentGeometryOrdinal(r.mesh,r.environment_variant)==render_data::kMeadowGeometryBegin;
    if(!meadow && r.mesh!=RenderMesh::Ground && r.mesh!=RenderMesh::DirtPatch) return {};
    const float span=meadow?24.f:1.f;
    return TerrainSection{r.position.x,r.position.z,r.scale.x*span,r.scale.z*span,
        std::cos(r.yaw),std::sin(r.yaw),static_cast<UINT>(r.mesh),r.environment_seed};
}

Result D3D12Renderer::Impl::TerrainPageSourceIdentity(std::array<std::byte,32> &identity) const
{
    Sha256Stream hash;
    auto canonical_config=config;
    canonical_config.selected_texture_feedback=false;
    const auto add=[&](std::string_view value) {
        hash.Add(std::as_bytes(std::span(value.data(),value.size())));
        constexpr std::byte separator{}; hash.Add(std::span(&separator,1));
    };
    add("project-hs.terrain-surface.source.v2");
    const auto digest=[&](std::string_view id)->Result {
        const auto value=RendererProductDigest(canonical_config,id);
        if(value.empty()) return TerrainFailure("Missing terrain source digest: "+std::string(id));
        add(id); add(value); return Result::Success();
    };
    for(const auto &id:environment_material_bindings[kTerrainMaterialOrdinal].product_ids)
        if(auto r=digest(id);!r) return r;
    for(const auto id:{"render/shader/terrain_resolve.dxil","render/shader/terrain_resolve_feedback.dxil",
                       "render/meadow_ground.rpkg"})
        if(auto r=digest(id);!r) return r;
    identity=hash.Finish(); return Result::Success();
}

UINT D3D12Renderer::Impl::TerrainSectionIndex(const RenderInstance &r) const
{
    const auto section=TerrainSectionFrom(r);
    if(!section) return r.environment_seed;
    return static_cast<UINT>(std::ranges::find(terrain_sections,*section)-terrain_sections.begin());
}

Result D3D12Renderer::Impl::PrepareTerrainPages(const std::shared_ptr<const StaticRenderScene> &scene,
    std::span<const RenderInstance> dynamic)
{
    const auto append=[](std::vector<TerrainSection> &sections,const RenderInstance &r) {
        if(auto section=TerrainSectionFrom(r)) sections.push_back(*section);
    };
    const bool scene_changed=terrain_scene!=scene;
    if(scene_changed)
    {
        static_terrain_sections.clear();
        if(scene) for(const auto &r:scene->instances) append(static_terrain_sections,r);
        terrain_scene=scene;
    }
    std::vector<TerrainSection> with_dynamic;
    for(const auto &r:dynamic) if(auto section=TerrainSectionFrom(r))
    {
        if(with_dynamic.empty()) with_dynamic=static_terrain_sections;
        with_dynamic.push_back(*section);
    }
    const auto &sections=with_dynamic.empty()?static_terrain_sections:with_dynamic;
    if(terrain_sections==sections && (sections.empty() ||
        (!scene_changed && terrain_section_buffer.resource && terrain_pipeline_revision==pipeline_revision))) return Result::Success();
    if(sections.size()>(1ull<<D3D12_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP))
        return TerrainFailure("Terrain section table exceeds the native buffer element capacity.");
    for(const auto &s:sections)
        if(!std::isfinite(s.x)||!std::isfinite(s.z)||!std::isfinite(s.span_x)||!std::isfinite(s.span_z)||
           !std::isfinite(s.cosine)||!std::isfinite(s.sine)) return TerrainFailure("Nonfinite terrain page transform.");
    if(auto waited=WaitForGpu();!waited) return waited;
    if(sections.empty())
    {
        terrain_sections.clear();
        if(terrain_page_residency) static_cast<void>(residency.ReleaseNow(terrain_page_residency));
        terrain_page_residency={};terrain_pages.Reset();terrain_section_buffer.Reset();
        WriteTerrainPageDescriptors();return Result::Success();
    }

    render_data::TerrainSurfacePackageView archive;
    bool use_pages=false;
    if(RendererIsAdmittedRuntimeProduct(config,kTerrainArchive))
    {
        RendererAssetSource source;
        if(auto r=ResolveRendererAsset(config,kTerrainArchive,source);!r) return r;
        std::vector<std::byte> bytes;
        if(auto r=ReadBinary(source,bytes);!r) return r;
        try{archive=render_data::ReadTerrainSurfacePackage(bytes,MakeAssetId(kTerrainArchive));}
        catch(const std::exception &e){return TerrainFailure(e.what());}
        if(archive.layout_version==render_data::kTerrainSurfaceLayoutVersion && archive.sections==sections)
        {
            std::array<std::byte,32> identity{};
            if(auto r=TerrainPageSourceIdentity(identity);!r) return r;
            use_pages=identity==archive.source_identity;
        }
    }
    constexpr UINT64 kMaxSectionPageBytes=256ull<<20;
    const UINT64 slices64=UINT64(sections.size())*3;
    D3D12_RESOURCE_DESC output_desc{};
    const auto segment=ResidencySegmentForHeap(D3D12_HEAP_TYPE_DEFAULT);
    if(use_pages)
    {
        const auto &d=archive.description;
        if(d.array_size!=slices64 || archive.section_products.size()!=sections.size())
            return TerrainFailure("Terrain archive texture description mismatch.");
        use_pages=slices64<=D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION;
        if(use_pages)
        {
            output_desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;output_desc.Width=d.width;output_desc.Height=d.height;
            output_desc.DepthOrArraySize=static_cast<UINT16>(slices64);output_desc.MipLevels=static_cast<UINT16>(d.mip_count);
            output_desc.Format=d.format==render_data::kTextureFormatBc7 ? DXGI_FORMAT_BC7_TYPELESS : DXGI_FORMAT_R8G8B8A8_TYPELESS;
            output_desc.SampleDesc.Count=1;
            const auto bytes=device->GetResourceAllocationInfo(0,1,&output_desc).SizeInBytes;
            if(bytes==UINT64_MAX) return TerrainFailure("Invalid native terrain page resource description.");
            if(auto r=UpdateResidencyBudget();!r) return r;
            use_pages=bytes<=kMaxSectionPageBytes && residency.CanAdmit(bytes,10,segment);
        }
    }
    AllocationResource section_buffer,output;
    D3D12MA::ALLOCATION_DESC allocation{};allocation.HeapType=D3D12_HEAP_TYPE_UPLOAD;
    const auto section_bytes=std::as_bytes(std::span(sections));
    if(auto r=CreateAllocation(section_buffer,allocation,BufferDescription(section_bytes.size()),D3D12_RESOURCE_STATE_GENERIC_READ);!r) return r;
    void *mapped{};D3D12_RANGE empty{};
    auto hr=section_buffer.resource->Map(0,&empty,&mapped);
    if(FAILED(hr)) return HResultFailure("Map terrain sections",hr);
    auto *dst=static_cast<std::byte *>(mapped);
    if(use_pages) std::memcpy(dst,section_bytes.data(),section_bytes.size());
    else for(auto section:sections) {section.span_x=section.span_z=0;std::memcpy(dst,&section,sizeof(section));dst+=sizeof(section);}
    section_buffer.resource->Unmap(0,nullptr);

    ResidencyTicket ticket{};
    if(use_pages)
    {
        allocation.HeapType=D3D12_HEAP_TYPE_DEFAULT;
        if(auto r=CreateAllocation(output,allocation,output_desc,D3D12_RESOURCE_STATE_COMMON);!r) return r;
        ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
        hr=device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator));
        if(FAILED(hr)) return HResultFailure("Create terrain upload allocator",hr);
        hr=device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list));
        if(FAILED(hr)) return HResultFailure("Create terrain upload list",hr);
        Transition(list.Get(),output.resource.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        std::vector<AllocationResource> retained;
        for(UINT i=0;i<archive.section_products.size();++i)
        {
            const auto &id=archive.section_products[i]; RendererAssetSource source;
            if(auto r=ResolveRendererAsset(config,id,source);!r) return r;
            std::vector<std::byte> bytes;if(auto r=ReadBinary(source,bytes);!r) return r;
            render_data::TexturePackageView package;
            try{package=ReadRendererTexturePackage(config,id,bytes);}catch(const std::exception &e){return TerrainFailure(e.what());}
            const auto &shape=package.description;
            const auto &expected=archive.description;
            const UINT count=3*expected.mip_count;
            if(shape.width!=expected.width || shape.height!=expected.height || shape.array_size!=3 ||
               shape.mip_count!=expected.mip_count || shape.format!=expected.format || package.subresources.size()!=count)
                return TerrainFailure("Terrain section texture description mismatch.");
            std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
            std::vector<UINT> rows(count);std::vector<UINT64> row_bytes(count);UINT64 size{};
            device->GetCopyableFootprints(&output_desc,i*count,count,0,footprints.data(),rows.data(),row_bytes.data(),&size);
            if(!size || size==UINT64_MAX) return TerrainFailure("Invalid terrain upload footprint.");
            auto &upload=retained.emplace_back();allocation.HeapType=D3D12_HEAP_TYPE_UPLOAD;
            if(auto r=CreateAllocation(upload,allocation,BufferDescription(size),D3D12_RESOURCE_STATE_GENERIC_READ);!r) return r;
            void *pointer{};hr=upload.resource->Map(0,&empty,&pointer);
            if(FAILED(hr)) return HResultFailure("Map terrain section upload",hr);
            auto *out=static_cast<std::byte *>(pointer);
            for(UINT sub=0;sub<count;++sub)
            {
                const auto &input=package.subresources[sub];
                if(row_bytes[sub]!=input.row_pitch || UINT64(rows[sub])*input.row_pitch!=input.bytes.size())
                {upload.resource->Unmap(0,nullptr);return TerrainFailure("Terrain section upload footprint mismatch.");}
                for(UINT y=0;y<rows[sub];++y)
                    std::memcpy(out+footprints[sub].Offset+UINT64(y)*footprints[sub].Footprint.RowPitch,
                        input.bytes.data()+UINT64(y)*input.row_pitch,input.row_pitch);
                D3D12_TEXTURE_COPY_LOCATION to{},from{};to.pResource=output.resource.Get();
                to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.SubresourceIndex=i*count+sub;
                from.pResource=upload.resource.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=footprints[sub];
                list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
            }
            upload.resource->Unmap(0,nullptr);
        }
        Transition(list.Get(),output.resource.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        hr=list->Close();if(FAILED(hr)) return HResultFailure("Close terrain upload list",hr);
        auto &lease=retained.emplace_back();lease.resource=output.resource;lease.allocation=output.allocation;lease.allocation->AddRef();
        if(auto r=SubmitResidentUtilityUpload(list.Get(),allocator.Get(),retained);!r) return r;
        ticket=RegisterNativeAllocation(ResidencyCategory::Texture,output.allocation->GetSize(),true,segment);
        if(!ticket) return TerrainFailure("Could not account terrain page residency.");
        output.graph_access=Access::Common;
    }
    auto accepted_sections=sections;
    terrain_sections=std::move(accepted_sections);
    if(terrain_page_residency) static_cast<void>(residency.ReleaseNow(terrain_page_residency));
    terrain_page_residency=ticket;terrain_pages=std::move(output);terrain_section_buffer=std::move(section_buffer);
    terrain_pipeline_revision=pipeline_revision;WriteTerrainPageDescriptors();return Result::Success();
}

void D3D12Renderer::Impl::WriteTerrainPageDescriptors() const
{
    const auto count=static_cast<UINT>(terrain_sections.size());
    const auto description=terrain_pages.resource ? terrain_pages.resource->GetDesc() : D3D12_RESOURCE_DESC{};
    const bool compressed=description.Format==DXGI_FORMAT_BC7_TYPELESS;
    for(UINT f=0;f<kFrameCount;++f)
    {
        auto h=srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr+=(SIZE_T(f)*kTextureDescriptorCount+kTerrainPageDescriptorOffset)*srv_stride;
        D3D12_SHADER_RESOURCE_VIEW_DESC pages{};
        pages.Format=compressed ? DXGI_FORMAT_BC7_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
        pages.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        pages.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        pages.Texture2DArray.MipLevels=terrain_pages.resource?description.MipLevels:1;
        pages.Texture2DArray.ArraySize=terrain_pages.resource?count*3:1;
        device->CreateShaderResourceView(terrain_pages.resource.Get(),&pages,h);
        h.ptr+=srv_stride;
        D3D12_SHADER_RESOURCE_VIEW_DESC table{};table.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
        table.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        table.Buffer.StructureByteStride=sizeof(TerrainSection);table.Buffer.NumElements=std::max(1u,count);
        device->CreateShaderResourceView(terrain_section_buffer.resource.Get(),&table,h);
        h.ptr+=srv_stride;
        pages.Format=compressed ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        device->CreateShaderResourceView(terrain_pages.resource.Get(),&pages,h);
    }
}
} // namespace hs
