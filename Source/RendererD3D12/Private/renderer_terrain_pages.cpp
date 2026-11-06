#include "renderer_impl.hpp"
#include <hs/core/sha256.hpp>
#include <hs/render_data/terrain_surface.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace hs
{
namespace
{
constexpr std::string_view kTerrainArchive="render/terrain/arena_surface.rpkg";
Result TerrainFailure(std::string message)
{ return Result::Failure(ErrorCode::InvalidState,"hs_renderer_d3d12",std::move(message)); }
bool SameTextureShape(const render_data::TextureDescription &a,const render_data::TextureDescription &b)
{
    return a.width==b.width && a.height==b.height && a.array_size==b.array_size &&
        a.mip_count==b.mip_count && a.format==b.format;
}
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
    add("project-hs.terrain-surface.source.v5");
    const auto digest=[&](std::string_view id)->Result {
        const auto value=RendererProductDigest(canonical_config,id);
        if(value.empty()) return TerrainFailure("Missing terrain source digest: "+std::string(id));
        add(id); add(value); return Result::Success();
    };
    for(const auto &id:environment_material_bindings[kTerrainMaterialOrdinal].product_ids)
        if(auto r=digest(id);!r) return r;
    for(const auto id:{"terrain/producer/bake_cs.dxil","terrain/producer/mip_cs.dxil",
                       "render/meadow_ground.rpkg"})
        if(auto r=digest(id);!r) return r;
    identity=hash.Finish(); return Result::Success();
}

UINT D3D12Renderer::Impl::TerrainSectionIndex(bool immutable, std::size_t ordinal, const RenderInstance &r) const
{
    const auto &indices=immutable?static_terrain_indices:dynamic_terrain_indices;
    return ordinal<indices.size() && indices[ordinal]!=UINT_MAX ? indices[ordinal] : r.environment_seed;
}

Result D3D12Renderer::Impl::PrepareTerrainPages(const std::shared_ptr<const StaticRenderScene> &scene,
    std::span<const RenderInstance> dynamic, UINT frame_slot)
{
    if(frame_slot>=kFrameCount) return TerrainFailure("Invalid terrain frame slot.");
    const bool scene_changed=terrain_scene!=scene;
    std::vector<TerrainSection> next_static;
    std::vector<UINT> next_static_indices;
    if(scene_changed)
    {
        next_static_indices.assign(scene?scene->instances.size():0,UINT_MAX);
        if(scene) for(std::size_t i=0;i<scene->instances.size();++i)
            if(auto section=TerrainSectionFrom(scene->instances[i]))
            {
                next_static_indices[i]=static_cast<UINT>(next_static.size());
                next_static.push_back(*section);
            }
    }
    const auto &sections=scene_changed?next_static:static_terrain_sections;
    if(sections.size()>(1ull<<D3D12_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP))
        return TerrainFailure("Terrain section table exceeds the native buffer element capacity.");
    if(scene_changed) for(const auto &section:sections)
        if(!std::isfinite(section.x)||!std::isfinite(section.z)||!std::isfinite(section.span_x)||
            !std::isfinite(section.span_z)||!std::isfinite(section.cosine)||!std::isfinite(section.sine))
            return TerrainFailure("Nonfinite terrain page transform.");
    const bool archive_present=RendererIsAdmittedRuntimeProduct(config,kTerrainArchive);
    const std::string archive_digest=archive_present?RendererProductDigest(config,kTerrainArchive):std::string{};
    if(archive_present && archive_digest.empty()) return TerrainFailure("Missing terrain archive digest.");
    const UINT first_mip=QualityProfileFor(config.texture_quality).texture_finest_mip;
    bool recheck=scene_changed || terrain_parameters_changed || terrain_archive_digest!=archive_digest ||
        terrain_target_first_mip!=first_mip;
    render_data::TerrainSurfacePackageView archive;
    std::vector<render_data::TerrainSurfaceTile> tiles;
    bool use_pages=recheck?false:terrain_static_pages_current;
    if(recheck && terrain_parameters_match && archive_present && !sections.empty())
    {
#if defined(HS_DEVELOPMENT_TOOLS)
        ++terrain_archive_reads;
#endif
        RendererAssetSource source;std::vector<std::byte> bytes;
        if(auto r=ResolveRendererAsset(config,kTerrainArchive,source);!r) return r;
        if(auto r=ReadBinary(source,bytes);!r) return r;
        try{archive=render_data::ReadTerrainSurfacePackage(bytes,MakeAssetId(kTerrainArchive));}
        catch(const std::exception &e){return TerrainFailure(e.what());}
        // A scene outside the fixed cache domain continues through its source material.
        try{tiles=render_data::BuildTerrainSurfaceTiles(sections);}
        catch(const std::runtime_error &){tiles.clear();}
        if(archive.layout_version==render_data::kTerrainSurfaceLayoutVersion &&
            archive.sections.size()==tiles.size() && !tiles.empty())
        {
            use_pages=true;
            for(std::size_t i=0;i<tiles.size();++i)
                if(archive.sections[i]!=tiles[i].section){use_pages=false;break;}
            if(use_pages)
            {
                std::array<std::byte,32> identity{};
                if(auto r=TerrainPageSourceIdentity(identity);!r) return r;
                use_pages=identity==archive.source_identity;
            }
        }
    }
    if(recheck && use_pages)
    {
        const auto &d=archive.description;
        if(d.width!=render_data::kTerrainSurfacePageEdge || d.height!=d.width || d.format!=render_data::kTextureFormatBc7 ||
            d.mip_count!=render_data::kTerrainSurfacePageMips || first_mip>=d.mip_count ||
            d.array_size!=archive.sections.size()*3 || archive.section_products.size()!=archive.sections.size())
            return TerrainFailure("Terrain tile index texture shape mismatch.");
        if(d.array_size>kTerrainPagePlaneCapacity) use_pages=false;
        if(use_pages && terrain_planes_archive_digest!=archive_digest)
        {
            if(resource_generation_id.empty() || !resident_generation_lease)
                return TerrainFailure("Terrain streaming requires its actual resident generation lease.");
            std::vector<std::string> plane_ids;
            plane_ids.reserve(d.array_size);
            for(std::size_t i=0;i<archive.section_products.size();++i)
            {
                const auto &id=archive.section_products[i]; RendererAssetSource source; std::vector<std::byte> bytes;
                if(auto r=ResolveRendererAsset(config,id,source);!r) return r;
                if(auto r=ReadBinary(source,bytes);!r) return r;
                render_data::TerrainSurfacePackageView leaf;
                try { leaf=render_data::ReadTerrainSurfacePackage(bytes,MakeAssetId(id)); }
                catch(const std::exception &e) { return TerrainFailure(e.what()); }
                auto expected=d; expected.array_size=3;
                if(leaf.layout_version!=render_data::kTerrainSurfaceLeafLayoutVersion ||
                    leaf.sections.size()!=1 || leaf.sections[0]!=archive.sections[i] ||
                    !SameTextureShape(leaf.description,expected) || leaf.section_products.size()!=3 ||
                    leaf.source_identity!=archive.source_identity)
                    return TerrainFailure("Terrain tile plane directory mismatch.");
                plane_ids.insert(plane_ids.end(),leaf.section_products.begin(),leaf.section_products.end());
            }
            if(!terrain_plane_stream_ids.empty())
                return TerrainFailure("Terrain plane owner differs from its resident generation.");
            std::vector<TextureStreamRegistration> registrations;
            std::vector<std::shared_ptr<TerrainPlaneBacking>> backings;
            std::vector<TextureStreamId> streams(plane_ids.size());
            registrations.reserve(plane_ids.size()); backings.reserve(plane_ids.size());
            ComPtr<ID3D12CommandAllocator> command_allocator;
            ComPtr<ID3D12GraphicsCommandList> list;
            std::vector<AllocationResource> retained;
            UINT64 batch_bytes{};
            const auto flush=[&]() -> Result {
                if(!list) return Result::Success();
                const auto closed=list->Close();
                if(FAILED(closed)) return HResultFailure("Close terrain tail list",closed);
                if(auto r=SubmitResidentUtilityUpload(list.Get(),command_allocator.Get(),retained);!r) return r;
                list.Reset(); command_allocator.Reset(); batch_bytes=0;
                return Result::Success();
            };
            for(std::size_t plane=0;plane<plane_ids.size();++plane)
            {
                const auto &id=plane_ids[plane];
                const auto handle=FindRendererTexture(config,MakeAssetId(id));
                if(!handle || handle.value>=config.packed_textures.size())
                    return TerrainFailure("Terrain plane lacks authenticated texture metadata.");
                const auto &descriptor=config.packed_textures[handle.value];
                auto expected=d; expected.array_size=1;
                if(!SameTextureShape(descriptor.description,expected) ||
                    descriptor.subresources.size()!=expected.mip_count)
                    return TerrainFailure("Terrain plane texture description mismatch.");
                RendererAssetSource source;
                if(auto r=ResolveRendererAsset(config,id,source);!r) return r;
                auto backing=std::make_shared<TerrainPlaneBacking>();
                backing->generation_lease=resident_generation_lease;
                constexpr UINT tail=4;
                D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                desc.Width=std::max(1u,d.width>>tail); desc.Height=std::max(1u,d.height>>tail);
                desc.DepthOrArraySize=1; desc.MipLevels=static_cast<UINT16>(d.mip_count-tail);
                desc.Format=plane%3==0 ? DXGI_FORMAT_BC7_TYPELESS : static_cast<DXGI_FORMAT>(d.format);
                desc.SampleDesc.Count=1;
                D3D12MA::ALLOCATION_DESC allocation{}; allocation.HeapType=D3D12_HEAP_TYPE_DEFAULT;
                if(auto r=CreateAllocation(backing->fallback,allocation,desc,D3D12_RESOURCE_STATE_COPY_DEST);!r) return r;
                const UINT count=desc.MipLevels;
                std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
                std::vector<UINT> rows(count); std::vector<UINT64> row_bytes(count); UINT64 upload_size{};
                device->GetCopyableFootprints(&desc,0,count,0,footprints.data(),rows.data(),row_bytes.data(),&upload_size);
                if(!upload_size || upload_size==UINT64_MAX) return TerrainFailure("Invalid terrain tail footprint.");
                constexpr UINT64 max_upload_batch=32ull*1024ull*1024ull;
                if(upload_size>max_upload_batch) return TerrainFailure("Terrain tail exceeds upload batch limit.");
                if(batch_bytes>max_upload_batch-upload_size)
                    if(auto r=flush();!r) return r;
                if(!list)
                {
                    auto result=device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&command_allocator));
                    if(FAILED(result)) return HResultFailure("Create terrain tail allocator",result);
                    result=device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,command_allocator.Get(),nullptr,IID_PPV_ARGS(&list));
                    if(FAILED(result)) return HResultFailure("Create terrain tail list",result);
                }
                batch_bytes+=upload_size;
                const auto upload_index=retained.size(); retained.emplace_back();
                allocation.HeapType=D3D12_HEAP_TYPE_UPLOAD;
                if(auto r=CreateAllocation(retained.back(),allocation,BufferDescription(upload_size),D3D12_RESOURCE_STATE_GENERIC_READ);!r) return r;
                void *address{}; D3D12_RANGE empty{};
                auto hr=retained.back().resource->Map(0,&empty,&address);
                if(FAILED(hr)) return HResultFailure("Map terrain tail upload",hr);
                for(UINT mip=0;mip<count;++mip)
                {
                    const auto &input=descriptor.subresources[mip+tail];
                    if(!input.has_digest || !input.byte_count || row_bytes[mip]!=input.row_pitch ||
                        UINT64(rows[mip])*input.row_pitch!=input.byte_count)
                    { retained.back().resource->Unmap(0,nullptr); return TerrainFailure("Invalid terrain tail subresource."); }
                    std::vector<std::byte> bytes(input.byte_count);
                    const auto base=source.IsPacked()?source.Packed().range.offset:0;
                    if(base>UINT64_MAX-input.offset)
                    { retained.back().resource->Unmap(0,nullptr); return TerrainFailure("Terrain tail range overflow."); }
                    if(auto r=source.ReadRange(base+input.offset,bytes,input.digest);!r)
                    { retained.back().resource->Unmap(0,nullptr); return r; }
                    auto *out=static_cast<std::byte *>(address)+footprints[mip].Offset;
                    for(UINT row=0;row<rows[mip];++row)
                        std::memcpy(out+UINT64(row)*footprints[mip].Footprint.RowPitch,
                            bytes.data()+UINT64(row)*input.row_pitch,input.row_pitch);
                }
                retained.back().resource->Unmap(0,nullptr);
                for(UINT mip=0;mip<count;++mip)
                {
                    D3D12_TEXTURE_COPY_LOCATION to{},from{}; to.pResource=backing->fallback.resource.Get();
                    to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; to.SubresourceIndex=mip;
                    from.pResource=retained[upload_index].resource.Get(); from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    from.PlacedFootprint=footprints[mip]; list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
                }
                Transition(list.Get(),backing->fallback.resource.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
                retained.emplace_back(); retained.back().resource=backing->fallback.resource;
                retained.back().allocation=backing->fallback.allocation; retained.back().allocation->AddRef();
                backing->fallback.graph_access=Access::Common;
                if(auto r=backing->fallback.EnsureNativeContract(device.Get());!r) return r;
                TextureStreamRegistration registration;
                registration.generation=resource_generation_id; registration.logical_id=id;
                registration.source=source; registration.generation_lease=backing;
                registration.description=expected; registration.compiled_texture=handle;
                registration.fallback=backing->fallback.resource;
                registration.fallback_contract=*backing->fallback.NativeContract();
                registration.fallback_graph_access=Access::Common; registration.fallback_first_mip=tail;
                registration.fallback_allocation_bytes=backing->fallback.allocation->GetSize();
                registration.opacity_sampling=false; registration.srgb_view=plane%3==0;
                registrations.push_back(std::move(registration));
                backings.push_back(std::move(backing));
            }
            if(auto r=flush();!r) return r;
            bool admitted=false;
            if(auto r=texture_streaming.RegisterBatch(registrations,residency,streams,admitted);!r) return r;
            if(admitted)
            {
                terrain_plane_backings=std::move(backings);
                terrain_plane_stream_ids=std::move(streams);
                terrain_planes_archive_digest=archive_digest;
            }
            else use_pages=false;
        }
        if(use_pages)
        {
            terrain_sections=archive.sections; terrain_tiles=std::move(tiles);
            terrain_tile_counts.assign(sections.size(),{});
            for(const auto &tile:terrain_tiles) terrain_tile_counts[tile.source_index]={tile.count_x,tile.count_y};
        }
    }
    D3D12MA::ALLOCATION_DESC allocation{}; allocation.HeapType=D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RANGE empty{}; HRESULT hr{};
    std::vector<GpuTerrainSection> frame_sections;frame_sections.reserve(sections.size());
    UINT page_index=0;
    for(std::size_t i=0;i<sections.size();++i)
    {
        const auto &section=sections[i];GpuTerrainSection gpu{};
        gpu.center={section.x,section.z};gpu.rotation={section.cosine,section.sine};
        gpu.mesh=section.mesh;gpu.seed=section.seed;gpu.pageSlice=UINT_MAX;
        if(use_pages)
        {
            const auto counts=terrain_tile_counts[i];
            gpu.span={section.span_x,section.span_z};gpu.pageSlice=page_index*3;
            gpu.pageEdge=render_data::kTerrainSurfacePageEdge;gpu.tileCount=counts;
            page_index+=counts[0]*counts[1];
        }
        frame_sections.push_back(gpu);
    }
    dynamic_terrain_indices.assign(dynamic.size(),UINT_MAX);
    for(std::size_t i=0;i<dynamic.size();++i)
    {
        const auto &r=dynamic[i];if(!TerrainSectionFrom(r)) continue;
        if(!std::isfinite(r.position.x)||!std::isfinite(r.position.z)||!std::isfinite(r.scale.x)||
            !std::isfinite(r.scale.z)||!std::isfinite(r.yaw)) return TerrainFailure("Nonfinite terrain page transform.");
        if(frame_sections.size()>=(1ull<<D3D12_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP))
            return TerrainFailure("Terrain section table exceeds the native buffer element capacity.");
        dynamic_terrain_indices[i]=static_cast<UINT>(frame_sections.size());
        GpuTerrainSection fallback{};fallback.seed=r.environment_seed;fallback.pageSlice=UINT_MAX;frame_sections.push_back(fallback);
    }
    auto &table=terrain_frame_sections[frame_slot];const auto section_bytes=std::as_bytes(std::span(frame_sections));
    if(!section_bytes.empty())
    {
        if(!table.resource || table.resource->GetDesc().Width<section_bytes.size())
        {
            AllocationResource candidate;allocation.HeapType=D3D12_HEAP_TYPE_UPLOAD;
            if(auto r=CreateAllocation(candidate,allocation,BufferDescription(section_bytes.size()),D3D12_RESOURCE_STATE_GENERIC_READ);!r) return r;
            table=std::move(candidate);
        }
        void *mapped{};hr=table.resource->Map(0,&empty,&mapped);
        if(FAILED(hr)) return HResultFailure("Map terrain sections",hr);
        std::memcpy(mapped,section_bytes.data(),section_bytes.size());table.resource->Unmap(0,nullptr);
    }
    if(scene_changed)
    {
        static_terrain_indices=std::move(next_static_indices);
        static_terrain_sections=std::move(next_static);terrain_scene=scene;
    }
    terrain_static_pages_current=use_pages;terrain_archive_digest=archive_digest;
    terrain_target_first_mip=first_mip;
    terrain_parameters_changed=false;
    return Result::Success();
}

void D3D12Renderer::Impl::DemandTerrainPages(std::span<const RenderViewInput> views,std::uint64_t frame_number)
{
    if(!terrain_static_pages_current) return;
    const UINT finest=QualityProfileFor(config.texture_quality).texture_finest_mip;
    for(std::size_t index=0;index<terrain_tiles.size();++index)
    {
        const auto &section=terrain_tiles[index].section;
        const double radius=(8.0/7.0)*0.5*std::hypot(double(section.span_x),double(section.span_z));
        bool needed=false;
        double nearest=std::numeric_limits<double>::infinity();
        for(const auto &view:views)
        {
            const double x=view.camera.position.x-view.scene_origin.x;
            const double z=view.camera.position.z-view.scene_origin.z;
            const double dx=double(section.x)-x, dz=double(section.z)-z;
            const double distance=std::hypot(dx,dz);
            if(!std::isfinite(distance) || !std::isfinite(view.far_plane) || distance>double(view.far_plane)+radius) continue;
            const double vx=view.camera.target.x-view.camera.position.x;
            const double vy=view.camera.target.y-view.camera.position.y;
            const double vz=view.camera.target.z-view.camera.position.z;
            const double horizontal=std::hypot(vx,vz);
            const double aspect=view.render_active.height ? double(view.render_active.width)/view.render_active.height : 1.0;
            const double half_y=double(view.camera.vertical_fov_degrees)*3.14159265358979323846/360.0;
            const double diagonal=std::atan(std::hypot(std::tan(half_y),std::tan(half_y)*aspect));
            const double pitch=std::atan2(std::abs(vy),horizontal);
            if(horizontal>0 && std::isfinite(diagonal) && diagonal+pitch<1.5707963267948966 && distance>radius)
            {
                const double half=std::asin(std::min(1.0,std::sin(diagonal)/std::cos(pitch)));
                const double angle=std::abs(std::atan2(dx*vz-dz*vx,dx*vx+dz*vz));
                if(angle>half+std::asin(std::min(1.0,radius/distance))) continue;
            }
            needed=true; nearest=std::min(nearest,std::max(distance-radius,0.0));
        }
        if(!needed) continue;
        const auto priority=std::bit_cast<UINT>(static_cast<float>(1.0/(1.0+nearest)));
        for(std::size_t plane=0;plane<3;++plane)
            texture_streaming.Demand(terrain_plane_stream_ids[index*3+plane],finest,priority,frame_number);
    }
}

Result D3D12Renderer::Impl::PublishTerrainStreaming(UINT frame_slot)
{
    if(frame_slot>=kFrameCount) return TerrainFailure("Invalid terrain frame slot.");
    auto &buffer=terrain_frame_mips[frame_slot];
    const auto bytes=UINT64(kTerrainPagePlaneCapacity)*sizeof(TextureMipMetadata);
    if(!buffer.resource)
    {
        D3D12MA::ALLOCATION_DESC allocation{}; allocation.HeapType=D3D12_HEAP_TYPE_UPLOAD;
        if(auto r=CreateAllocation(buffer,allocation,BufferDescription(bytes),D3D12_RESOURCE_STATE_GENERIC_READ);!r) return r;
    }
    void *address{}; D3D12_RANGE empty{};
    const auto hr=buffer.resource->Map(0,&empty,&address);
    if(FAILED(hr)) return HResultFailure("Map terrain texture metadata",hr);
    auto *entries=static_cast<TextureMipMetadata *>(address);
    std::fill_n(entries,kTerrainPagePlaneCapacity,TextureMipMetadata{});
    if(terrain_static_pages_current)
        for(std::size_t i=0;i<terrain_plane_stream_ids.size();++i)
        {
            const auto view=texture_streaming.View(terrain_plane_stream_ids[i]);
            if(view.resource) entries[i]={view.first_mip,view.resident_min_mip,
                view.logical_description.mip_count,QualityProfileFor(config.texture_quality).texture_finest_mip+1};
        }
    buffer.resource->Unmap(0,nullptr);
    const auto &sections=terrain_frame_sections[frame_slot];
    const auto count=sections.resource ? static_cast<UINT>(sections.resource->GetDesc().Width/sizeof(GpuTerrainSection)) : 0u;
    WriteTerrainPageDescriptors(frame_slot,count);
    return Result::Success();
}

void D3D12Renderer::Impl::WriteTerrainPageDescriptors(UINT frame_slot,UINT section_count) const
{
    auto h=srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr+=(SIZE_T(frame_slot)*kTextureDescriptorCount+kTerrainPageDescriptorOffset)*srv_stride;
    const auto &sections=terrain_frame_sections[frame_slot];
    D3D12_SHADER_RESOURCE_VIEW_DESC table{}; table.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
    table.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    table.Buffer.StructureByteStride=sizeof(GpuTerrainSection); table.Buffer.NumElements=std::max(1u,section_count);
    device->CreateShaderResourceView(section_count?sections.resource.Get():nullptr,&table,h);
    h.ptr+=srv_stride;
    table.Buffer.StructureByteStride=sizeof(TextureMipMetadata); table.Buffer.NumElements=kTerrainPagePlaneCapacity;
    device->CreateShaderResourceView(terrain_frame_mips[frame_slot].resource.Get(),&table,h);
    h.ptr+=srv_stride;
    for(UINT plane=0;plane<kTerrainPagePlaneCapacity;++plane)
    {
        const auto view=terrain_static_pages_current && plane<terrain_plane_stream_ids.size()
            ? texture_streaming.View(terrain_plane_stream_ids[plane]) : TextureStreamView{};
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format=plane%3==0 ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM;
        srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels=view.resource?view.resource->GetDesc().MipLevels:1;
        device->CreateShaderResourceView(view.resource,&srv,h); h.ptr+=srv_stride;
    }
}
} // namespace hs
