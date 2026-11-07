#include "renderer_impl.hpp"

#if defined(HS_OFFLINE_PROBE_BAKER)
#include <hs/render_data/terrain_surface.hpp>

#include <DirectXPackedVector.h>
#include <algorithm>
#include <cstring>
#include <exception>
#include <limits>

namespace hs
{
namespace
{
constexpr UINT kEdge=render_data::kTerrainSurfacePageEdge, kMips=render_data::kTerrainSurfacePageMips;
constexpr UINT kApron=kEdge/16, kInterior=kEdge-2*kApron;
static_assert((1u<<(kMips-1))==kEdge);
constexpr DXGI_FORMAT kFormat=DXGI_FORMAT_R8G8B8A8_UNORM;

Result TerrainFailure(std::string message)
{
    return Result::Failure(ErrorCode::InvalidState,"hs_renderer_d3d12",std::move(message));
}
void Transition(ID3D12GraphicsCommandList *list,ID3D12Resource *resource,
    D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after,
    UINT subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={resource,subresource,before,after};
    list->ResourceBarrier(1,&barrier);
}
// Both packed and loose LOD0 geometry use the same expanded vertex/triangle ABI.
std::vector<UINT> MeadowControls(std::span<const render_data::ExpandedVertex> vertices,
    std::span<const UINT> indices,const render_data::TerrainSurfaceTile &tile)
{
    std::vector<UINT> controls(kEdge*kEdge);
    const float min_tile_x=-12.f+24.f*float(tile.x)/float(tile.count_x);
    const float min_tile_z=-12.f+24.f*float(tile.y)/float(tile.count_y);
    const float span_x=24.f/float(tile.count_x),span_z=24.f/float(tile.count_y);
    for(size_t i=0;i<indices.size();i+=3)
    {
        const auto &a=vertices[indices[i]],&b=vertices[indices[i+1]],&c=vertices[indices[i+2]];
        const float bx=b.position[0]-a.position[0],bz=b.position[2]-a.position[2];
        const float cx=c.position[0]-a.position[0],cz=c.position[2]-a.position[2];
        const float det=bx*cz-bz*cx;
        if(det==0) continue;
        const auto pixel_x=[&](float x) {return float(kApron)+(x-min_tile_x)/span_x*float(kInterior)-.5f;};
        const auto pixel_z=[&](float z) {return float(kApron)+(z-min_tile_z)/span_z*float(kInterior)-.5f;};
        const float minx=std::min({a.position[0],b.position[0],c.position[0]});
        const float minz=std::min({a.position[2],b.position[2],c.position[2]});
        const float maxx=std::max({a.position[0],b.position[0],c.position[0]});
        const float maxz=std::max({a.position[2],b.position[2],c.position[2]});
        const int x0=minx<=-12.f?0:std::max(0,int(std::ceil(pixel_x(minx))));
        const int z0=minz<=-12.f?0:std::max(0,int(std::ceil(pixel_z(minz))));
        const int x1=maxx>=12.f?int(kEdge)-1:std::min(int(kEdge)-1,int(std::floor(pixel_x(maxx))));
        const int z1=maxz>=12.f?int(kEdge)-1:std::min(int(kEdge)-1,int(std::floor(pixel_z(maxz))));
        for(int z=z0;z<=z1;++z) for(int x=x0;x<=x1;++x)
        {
            const float wx=min_tile_x+(x+.5f-kApron)/kInterior*span_x;
            const float wz=min_tile_z+(z+.5f-kApron)/kInterior*span_z;
            const float px=std::clamp(wx,-12.f,12.f)-a.position[0];
            const float pz=std::clamp(wz,-12.f,12.f)-a.position[2];
            const float pu=(px*cz-pz*cx)/det,pv=(bx*pz-bz*px)/det;
            if(pu<-.00001f || pv<-.00001f || pu+pv>1.00001f) continue;
            // Match raster interpolation's affine continuation in helper lanes;
            // the material's normal [0,1] control clamp is retained below.
            const float rx=wx-a.position[0],rz=wz-a.position[2];
            const float u=(rx*cz-rz*cx)/det,v=(bx*rz-bz*rx)/det;
            const auto channel=[&](size_t k){return UINT(DirectX::PackedVector::XMConvertFloatToHalf(
                std::clamp(a.uv[k]+u*(b.uv[k]-a.uv[k])+v*(c.uv[k]-a.uv[k]),0.f,1.f)));};
            controls[size_t(z)*kEdge+x]=channel(0)|(channel(1)<<16);
        }
    }
    return controls;
}
}

struct D3D12Renderer::Impl::TerrainPagePreparation
{
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> bake,mip;
    ComPtr<ID3D12DescriptorHeap> heap;
    AllocationResource output,controls,readback;
    std::array<AllocationResource,6> sources;
    std::vector<TerrainSection> sections;
    std::vector<render_data::TerrainSurfaceTile> tiles;
    std::vector<render_data::ExpandedVertex> meadow_vertices;
    std::vector<UINT> meadow_indices;
};

Result D3D12Renderer::Impl::BakeTerrainPages(const std::shared_ptr<const StaticRenderScene> &scene, std::shared_ptr<TerrainPagePreparation> &lifetime)
{
    const auto append=[](std::vector<TerrainSection> &sections,const RenderInstance &r) {
        if(auto section=TerrainSectionFrom(r)) sections.push_back(*section);
    };
    auto candidate=std::make_shared<TerrainPagePreparation>();
    if(scene) for(const auto &r:scene->instances) append(candidate->sections,r);
    const auto &sections=candidate->sections;
    if(sections.empty() || sections.size()>UINT_MAX/3u)
        return TerrainFailure("Terrain page bake requires a representable nonempty section set.");
    for(const auto &s:sections)
        if(!std::isfinite(s.x)||!std::isfinite(s.z)||!std::isfinite(s.span_x)||!std::isfinite(s.span_z)||
            !std::isfinite(s.cosine)||!std::isfinite(s.sine)) return TerrainFailure("Nonfinite terrain page transform.");
    candidate->tiles=render_data::BuildTerrainSurfaceTiles(sections);
    candidate->sections.clear();
    for(const auto &tile:candidate->tiles) candidate->sections.push_back(tile.section);
    if(sections.empty()) return TerrainFailure("Terrain page bake requires a nonempty tile set.");
    if(auto waited=WaitForGpu();!waited) return waited;
    
    constexpr UINT64 kMaxSectionPageBytes=256ull<<20;
    constexpr UINT slices=1;
    D3D12_RESOURCE_DESC output_desc{};
    output_desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    output_desc.Width=kEdge;output_desc.Height=kEdge;output_desc.DepthOrArraySize=slices;
    output_desc.MipLevels=kMips;output_desc.Format=DXGI_FORMAT_R8G8B8A8_TYPELESS;output_desc.SampleDesc.Count=1;
    output_desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const auto page_bytes=device->GetResourceAllocationInfo(0,1,&output_desc).SizeInBytes;
    if(page_bytes==UINT64_MAX || page_bytes>kMaxSectionPageBytes)
        return TerrainFailure("Native terrain section exceeds the bounded allocation size.");
    const auto allocate=[&](AllocationResource &r,const D3D12_RESOURCE_DESC &d,D3D12_HEAP_TYPE heap,
                             D3D12_RESOURCE_STATES state){
        D3D12MA::ALLOCATION_DESC allocation{}; allocation.HeapType=heap;
        return CreateAllocation(r,allocation,d,state);
    };
    const auto upload=[&](AllocationResource &r,size_t size,const auto &write)->Result{
        if(auto result=allocate(r,BufferDescription(size),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);!result)
            return result;
        void *dst{}; D3D12_RANGE empty{};
        const auto mapped=r.resource->Map(0,&empty,&dst);
        if(FAILED(mapped)) return HResultFailure("Map terrain page data",mapped);
        write(static_cast<std::byte *>(dst));r.resource->Unmap(0,nullptr);return Result::Success();
    };
    ComPtr<ID3D12CommandAllocator> cmd_allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    auto hr=device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&cmd_allocator));
    if(FAILED(hr)) return HResultFailure("Create terrain page allocator",hr);
    hr=device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,cmd_allocator.Get(),nullptr,IID_PPV_ARGS(&list));
    if(FAILED(hr)) return HResultFailure("Create terrain page list",hr);
    std::vector<AllocationResource> uploads;
    const bool has_meadow=std::ranges::any_of(sections,[](const auto &s){return s.mesh==static_cast<UINT>(RenderMesh::MeadowSurface);});
    if(has_meadow)
    {
        const auto &geo=environment_geometry_streams[render_data::kMeadowGeometryBegin];
        std::vector<std::byte> bytes;
        if(auto r=ReadBinary(geo.source,bytes);!r) return r;
        PreparedGeometryUpload geometry;
        if(auto r=PrepareReadGeometry(config,geo.logical_id,bytes,geo.metadata,geometry);!r) return r;
        try
        {
            if(geometry.descriptor)
            {
                const auto view=render_data::BindGeometryDescriptor(bytes,*geometry.descriptor);
                candidate->meadow_vertices=render_data::DecodeExpandedVertices(view.chunks[0]);
                candidate->meadow_indices.resize(geometry.descriptor->index_count);
                if(view.chunks[1].size()!=candidate->meadow_indices.size()*sizeof(UINT))
                    return TerrainFailure("Terrain page index ABI mismatch.");
                std::memcpy(candidate->meadow_indices.data(),view.chunks[1].data(),view.chunks[1].size());
            }
            else
            {
                candidate->meadow_vertices=std::move(geometry.page.geometry.vertices);
                candidate->meadow_indices=std::move(geometry.page.geometry.indices);
            }
        }
        catch(const std::exception &e){return TerrainFailure(e.what());}
    }
    if(auto r=allocate(candidate->controls,BufferDescription(has_meadow?UINT64(kEdge)*kEdge*sizeof(UINT):sizeof(UINT)),
        D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);!r) return r;

    D3D12_DESCRIPTOR_RANGE ranges[]{
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV,6,0,0,0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,6,0,0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0}};
    D3D12_ROOT_PARAMETER params[5]{};
    for(UINT i=0;i<3;++i){params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[i].DescriptorTable={1,&ranges[i]};}
    params[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[3].Descriptor={7,0};
    params[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[4].Constants={0,0,10};
    D3D12_STATIC_SAMPLER_DESC samplers[2]{};
    for(UINT i=0;i<2;++i)
    {
        auto &s=samplers[i];s.Filter=i?D3D12_FILTER_MIN_MAG_MIP_LINEAR:D3D12_FILTER_ANISOTROPIC;
        s.AddressU=s.AddressV=s.AddressW=i?D3D12_TEXTURE_ADDRESS_MODE_CLAMP:D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        s.MaxAnisotropy=i?1:D3D12_MAX_MAXANISOTROPY;s.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;s.MaxLOD=D3D12_FLOAT32_MAX;s.ShaderRegister=i;
    }
    D3D12_ROOT_SIGNATURE_DESC rs{5,params,2,samplers,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob,error;
    hr=D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error);
    if(FAILED(hr)) return HResultFailure("Serialize terrain page root",hr);
    hr=device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&candidate->root));
    if(FAILED(hr)) return HResultFailure("Create terrain page root",hr);
    const auto pipeline=[&](const char *id,ComPtr<ID3D12PipelineState> &pso)->Result {
        RendererAssetSource src;
        if(auto r=ResolveRendererAsset(config,id,src);!r) return r;
        std::vector<std::byte> dxil;
        if(auto r=ReadBinary(src,dxil);!r) return r;
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};desc.pRootSignature=candidate->root.Get();desc.CS={dxil.data(),dxil.size()};
        const auto code=device->CreateComputePipelineState(&desc,IID_PPV_ARGS(&pso));
        return FAILED(code)?HResultFailure("Create terrain page compute pipeline",code):Result::Success();
    };
    if(auto r=pipeline("terrain/producer/bake_cs.dxil",candidate->bake);!r) return r;
    if(auto r=pipeline("terrain/producer/mip_cs.dxil",candidate->mip);!r) return r;
    D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors=6+2*kMips;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr=device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&candidate->heap));
    if(FAILED(hr)) return HResultFailure("Create terrain page descriptors",hr);
    const auto cpu=[&](UINT i){auto h=candidate->heap->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(i)*srv_stride;return h;};
    const auto srv=[&](ID3D12Resource *resource,DXGI_FORMAT format,UINT start,UINT mips,UINT slices,D3D12_CPU_DESCRIPTOR_HANDLE handle){
        D3D12_SHADER_RESOURCE_VIEW_DESC s{};s.Format=format;s.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;s.Texture2DArray.MostDetailedMip=start;
        s.Texture2DArray.MipLevels=mips;s.Texture2DArray.ArraySize=slices;
        device->CreateShaderResourceView(resource,&s,handle);
    };
    // The synthesis input is the admitted material's full immutable source, independent of camera streaming.
    for(UINT slot=0;slot<candidate->sources.size();++slot)
    {
        auto &source=candidate->sources[slot];
        const auto &id=environment_material_bindings[kTerrainMaterialOrdinal].product_ids[slot];
        RendererAssetSource path;
        if(auto r=ResolveRendererAsset(config,id,path);!r) return r;
        std::vector<std::byte> bytes;
        if(auto r=ReadBinary(path,bytes);!r) return r;
        render_data::TexturePackageView package;
        try{package=ReadRendererTexturePackage(config,id,bytes);}
        catch(const std::exception &e){return TerrainFailure(e.what());}
        const auto &shape=package.description;
        D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width=shape.width;d.Height=shape.height;d.DepthOrArraySize=static_cast<UINT16>(shape.array_size);
        d.MipLevels=static_cast<UINT16>(shape.mip_count);d.Format=static_cast<DXGI_FORMAT>(shape.format);d.SampleDesc.Count=1;
        if(auto r=allocate(source,d,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON);!r) return r;
        Transition(list.Get(),source.resource.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        const auto count=static_cast<UINT>(package.subresources.size());
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> placements(count);
        std::vector<UINT> rows(count);std::vector<UINT64> widths(count);
        UINT64 size{};
        device->GetCopyableFootprints(&d,0,count,0,placements.data(),rows.data(),widths.data(),&size);
        auto &staging=uploads.emplace_back();
        if(auto r=allocate(staging,BufferDescription(size),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);!r) return r;
        std::byte *dst{};D3D12_RANGE empty{};
        hr=staging.resource->Map(0,&empty,reinterpret_cast<void **>(&dst));
        if(FAILED(hr)) return HResultFailure("Map terrain page source",hr);
        for(UINT sub=0;sub<count;++sub)
        {
            const auto &input=package.subresources[sub];
            if(widths[sub]!=input.row_pitch || UINT64(rows[sub])*input.row_pitch!=input.bytes.size())
                return TerrainFailure("Terrain page source footprint mismatch.");
            for(UINT y=0;y<rows[sub];++y)
                std::memcpy(dst+placements[sub].Offset+UINT64(y)*placements[sub].Footprint.RowPitch,
                    input.bytes.data()+UINT64(y)*input.row_pitch,input.row_pitch);
            D3D12_TEXTURE_COPY_LOCATION to{},from{};to.pResource=source.resource.Get();
            to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.SubresourceIndex=sub;
            from.pResource=staging.resource.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=placements[sub];
            list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        }
        staging.resource->Unmap(0,nullptr);
        Transition(list.Get(),source.resource.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        srv(source.resource.Get(),d.Format,0,d.MipLevels,d.DepthOrArraySize,cpu(slot));
    }

    if(auto r=allocate(candidate->output,output_desc,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON);!r) return r;
    auto *output=candidate->output.resource.Get();
    for(UINT level=0;level<kMips;++level)
    {
        srv(output,kFormat,level,1,slices,cpu(6+2*level));
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};u.Format=kFormat;u.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        u.Texture2DArray.MipSlice=level;u.Texture2DArray.ArraySize=slices;
        device->CreateUnorderedAccessView(output,nullptr,&u,cpu(7+2*level));
    }
    hr=list->Close();
    if(FAILED(hr)) return HResultFailure("Close terrain page generation",hr);
    lifetime=candidate;
    pending_terrain_pages=candidate;
    if(auto r=SubmitResidentUtilityUpload(list.Get(),cmd_allocator.Get(),uploads);!r) return r;
    pending_terrain_pages.reset();
    return Result::Success();
}

Result D3D12Renderer::BakeTerrainSurfacePages(const RendererConfig &configuration,
    const std::shared_ptr<const StaticRenderScene> &scene,
    const std::function<Result(std::span<const render_data::TerrainSurfaceSection>,
        render_data::TextureDescription, std::array<std::byte,32>)> &begin,
    const std::function<Result(std::uint32_t, std::span<const std::byte>)> &section_sink)
{
    if (!scene || scene->instances.empty() || !begin || !section_sink)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
            "Terrain page bake requires a nonempty scene and both callbacks.");
    std::shared_ptr<Impl::TerrainPagePreparation> lifetime;
    D3D12Renderer owner;
    const auto finish = [&](Result primary) -> Result {
        // Submitted work also owns pending_terrain_pages; release the local lease before its allocator.
        lifetime.reset();
        const auto cleanup = owner.Shutdown();
        if (!cleanup)
        {
            std::string message;
            if (!primary) message = std::string(primary.Message()) + "; ";
            message += "cleanup failed: ";
            message += cleanup.Message();
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", message);
        }
        if (!primary) return primary;
        if (owner.ValidationErrorCount())
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "Terrain page producer reported native validation errors.");
        return Result::Success();
    };
    try
    {
      const auto work = [&]() -> Result {
        if (auto initialized = owner.InitializeWorldTransportBake(configuration, WorldTransportBakeKind::Terrain); !initialized)
            return initialized;
        auto &impl = *owner.impl_;
        if (auto prepared = impl.BakeTerrainPages(scene, lifetime); !prepared) return prepared;
        auto &page = *lifetime;
        const auto &sections = page.sections;
        const auto desc = page.output.resource->GetDesc();
        constexpr UINT count = kMips;
        const render_data::TextureDescription texture{kEdge, kEdge,
            static_cast<UINT>(sections.size()) * 3u, kMips, render_data::kTextureFormatRgba8};
        std::array<std::byte,32> identity{};
        if (auto identified = impl.TerrainPageSourceIdentity(identity); !identified) return identified;
        if (auto started = begin(sections, texture, identity); !started) return started;

        std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, count> footprints{};
        std::array<UINT, count> rows{};
        std::array<UINT64, count> row_bytes{};
        UINT64 readback_size{};
        impl.device->GetCopyableFootprints(&desc, 0, count, 0, footprints.data(), rows.data(),
            row_bytes.data(), &readback_size);
        if (!readback_size || readback_size == UINT64_MAX || readback_size > SIZE_MAX ||
            readback_size > (256ull << 20))
            return TerrainFailure("Terrain page readback footprint is invalid.");
        size_t tight_size = 0;
        for (UINT sub = 0; sub < count; ++sub)
        {
            const UINT mip = sub % kMips;
            const UINT width = std::max(1u, kEdge >> mip), height = std::max(1u, kEdge >> mip);
            if (row_bytes[sub] != UINT64(width) * 4 || rows[sub] != height ||
                footprints[sub].Footprint.RowPitch < row_bytes[sub] ||
                tight_size > std::numeric_limits<size_t>::max() - size_t(row_bytes[sub]) * height)
                return TerrainFailure("Terrain page readback row layout is invalid.");
            tight_size += size_t(row_bytes[sub]) * height;
        }
        D3D12MA::ALLOCATION_DESC allocation{};
        allocation.HeapType = D3D12_HEAP_TYPE_READBACK;
        if (auto created = impl.CreateAllocation(page.readback, allocation, BufferDescription(readback_size),
                D3D12_RESOURCE_STATE_COPY_DEST); !created) return created;
        if (tight_size > std::numeric_limits<size_t>::max() / 3)
            return TerrainFailure("Terrain three-plane payload size overflows.");
        std::vector<std::byte> payload(tight_size * 3);
        const auto gpu = [&](UINT i) {
            auto h=page.heap->GetGPUDescriptorHandleForHeapStart(); h.ptr+=UINT64(i)*impl.srv_stride; return h;
        };
        for (UINT index = 0; index < sections.size(); ++index)
        {
          if(sections[index].mesh==static_cast<UINT>(RenderMesh::MeadowSurface))
          {
            const auto controls=MeadowControls(page.meadow_vertices,page.meadow_indices,page.tiles[index]);
            void *mapped_controls{};D3D12_RANGE no_read{};
            const auto mapped=page.controls.resource->Map(0,&no_read,&mapped_controls);
            if(FAILED(mapped)) return HResultFailure("Map terrain meadow controls",mapped);
            std::memcpy(mapped_controls,controls.data(),controls.size()*sizeof(UINT));
            page.controls.resource->Unmap(0,nullptr);
          }
          for (UINT plane = 0; plane < 3; ++plane)
          {
            ComPtr<ID3D12CommandAllocator> allocator;
            ComPtr<ID3D12GraphicsCommandList> list;
            auto hr = impl.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
            if (FAILED(hr)) return HResultFailure("Create terrain section allocator", hr);
            hr = impl.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list));
            if (FAILED(hr)) return HResultFailure("Create terrain section list", hr);
            auto *output = page.output.resource.Get();
            Transition(list.Get(), output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            auto *heap = page.heap.Get(); list->SetDescriptorHeaps(1, &heap);
            list->SetComputeRootSignature(page.root.Get());
            list->SetComputeRootDescriptorTable(0, gpu(0));
            list->SetComputeRootDescriptorTable(1, gpu(0));
            list->SetComputeRootDescriptorTable(2, gpu(7));
            list->SetComputeRootShaderResourceView(3, page.controls.resource->GetGPUVirtualAddress());
            list->SetPipelineState(page.bake.Get());
            list->SetComputeRoot32BitConstants(4, 8, &sections[index], 0);
            list->SetComputeRoot32BitConstant(4, plane, 8);
            list->SetComputeRoot32BitConstant(4, kEdge, 9);
            list->Dispatch(kEdge / 8, kEdge / 8, 1);
            list->SetPipelineState(page.mip.Get());
            for (UINT level = 0; level < kMips; ++level)
            {
                Transition(list.Get(), output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, level);
                if (level + 1 == kMips) break;
                const UINT edge = kEdge >> (level + 1);
                list->SetComputeRootDescriptorTable(1, gpu(6 + 2 * level));
                list->SetComputeRootDescriptorTable(2, gpu(7 + 2 * (level + 1)));
                list->SetComputeRoot32BitConstant(4, edge, 9);
                list->Dispatch((edge + 7) / 8, (edge + 7) / 8, 1);
            }
            Transition(list.Get(), output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            for (UINT sub = 0; sub < count; ++sub)
            {
                D3D12_TEXTURE_COPY_LOCATION from{}, to{};
                from.pResource = output; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; from.SubresourceIndex = sub;
                to.pResource = page.readback.resource.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                to.PlacedFootprint = footprints[sub];
                list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            }
            Transition(list.Get(), output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            hr = list->Close();
            if (FAILED(hr)) return HResultFailure("Close terrain section list", hr);
            std::vector<AllocationResource> retained;
            impl.pending_terrain_pages = lifetime;
            if (auto submitted = impl.SubmitResidentUtilityUpload(list.Get(), allocator.Get(), retained); !submitted)
                return submitted;
            impl.pending_terrain_pages.reset();
            void *mapped_ptr{};
            const D3D12_RANGE read_range{0, static_cast<SIZE_T>(readback_size)};
            hr = page.readback.resource->Map(0, &read_range, &mapped_ptr);
            if (FAILED(hr)) return HResultFailure("Map terrain page readback", hr);
            const auto *mapped = static_cast<const std::byte *>(mapped_ptr);
            size_t offset = size_t(plane) * tight_size;
            for (UINT sub = 0; sub < count; ++sub)
                for (UINT y = 0; y < rows[sub]; ++y)
                {
                    const auto width = static_cast<size_t>(row_bytes[sub]);
                    std::memcpy(payload.data() + offset, mapped + footprints[sub].Offset +
                        UINT64(y) * footprints[sub].Footprint.RowPitch, width);
                    offset += width;
                }
            const D3D12_RANGE no_write{};
            page.readback.resource->Unmap(0, &no_write);
          }
            if (auto saved = section_sink(index, payload); !saved) return saved;
        }
        return Result::Success();
      };
      return finish(work());
    }
    catch (const std::exception &error)
    {
        return finish(Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", error.what()));
    }
}
} // namespace hs
#endif
