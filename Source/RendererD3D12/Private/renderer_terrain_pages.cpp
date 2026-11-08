#include "renderer_impl.hpp"
#include <DirectXCollision.h>
#include <DirectXPackedVector.h>
#include <hs/core/sha256.hpp>
#include <hs/render_data/terrain_surface.hpp>
#include <hs/render_data/terrain_source_constants.hpp>
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
bool FiniteTerrainHeight(const RenderInstance &source)
{
    return std::isfinite(source.position.y) && std::isfinite(source.scale.y) &&
        (source.mesh!=RenderMesh::MeadowSurface ||
         std::abs(double(source.position.y)) + render_data::kMeadowGroundHeightBound*std::abs(double(source.scale.y)) <= std::numeric_limits<float>::max());
}
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
    for(const auto id:{"terrain/producer/bake_color_cs.dxil","terrain/producer/bake_base_cs.dxil",
                       "terrain/producer/bake_weight_cs.dxil","terrain/producer/mip_cs.dxil",
                       "render/meadow_ground.rpkg"})
        if(auto r=digest(id);!r) return r;
    identity=hash.Finish(); return Result::Success();
}

UINT D3D12Renderer::Impl::TerrainSectionIndex(bool immutable, std::size_t ordinal, const RenderInstance &r) const
{
    if(immutable)
    {
        if(ordinal>UINT_MAX) return r.environment_seed;
        const auto found=std::lower_bound(static_terrain_source_indices.begin(),static_terrain_source_indices.end(),
            static_cast<UINT>(ordinal));
        return found!=static_terrain_source_indices.end() && *found==static_cast<UINT>(ordinal)
            ? static_cast<UINT>(found-static_terrain_source_indices.begin()) : r.environment_seed;
    }
    return ordinal<dynamic_terrain_indices.size() && dynamic_terrain_indices[ordinal]!=UINT_MAX
        ? dynamic_terrain_indices[ordinal] : r.environment_seed;
}

Result D3D12Renderer::Impl::PrepareTerrainPages(const std::shared_ptr<const StaticRenderScene> &scene,
    std::span<const RenderInstance> dynamic, UINT frame_slot)
{
    if(frame_slot>=kFrameCount) return TerrainFailure("Invalid terrain frame slot.");
    const bool scene_changed=terrain_scene!=scene;
    std::vector<TerrainSection> next_static;
    std::vector<UINT> next_static_source_indices;
    if(scene_changed)
    {
        if(scene && scene->instances.size()>UINT32_MAX)
            return TerrainFailure("Static terrain source ordinal exceeds native instance addressing.");
        if(scene) for(std::size_t i=0;i<scene->instances.size();++i)
            if(auto section=TerrainSectionFrom(scene->instances[i]))
            {
                next_static_source_indices.push_back(static_cast<UINT>(i));
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
    bool recheck=scene_changed || terrain_parameters_changed || terrain_archive_digest!=archive_digest;
    const auto segment=ResidencySegmentForHeap(D3D12_HEAP_TYPE_DEFAULT);
    bool budget_updated=false;
    if(!recheck && terrain_retry_budget_bytes)
    {
        if(auto r=UpdateResidencyBudget();!r) return r;
        budget_updated=true;
        recheck=residency.CanAdmit(terrain_retry_budget_bytes,10,segment);
    }
    UINT64 retry_budget_bytes=recheck?0:terrain_retry_budget_bytes;
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
            std::vector<render_data::TextureHandle> handles;
            std::vector<RendererAssetSource> sources;
            handles.reserve(plane_ids.size()); sources.reserve(plane_ids.size());
            for(const auto &id:plane_ids)
            {
                const auto handle=FindRendererTexture(config,MakeAssetId(id));
                if(!handle || handle.value>=config.packed_textures.size())
                    return TerrainFailure("Terrain plane lacks authenticated texture metadata.");
                const auto &descriptor=config.packed_textures[handle.value];
                auto expected=d; expected.array_size=1;
                if(descriptor.asset_id.value!=MakeAssetId(id).value ||
                    descriptor.dimension!=render_data::TextureDimension::Texture2DArray ||
                    !SameTextureShape(descriptor.description,expected) ||
                    descriptor.subresources.size()!=expected.mip_count)
                    return TerrainFailure("Terrain plane texture description mismatch.");
                RendererAssetSource source;
                if(auto r=ResolveRendererAsset(config,id,source);!r) return r;
                UINT64 product_bytes{};
                if(auto r=source.ByteCount(product_bytes);!r) return r;
                try { render_data::ValidateTextureDescriptor(descriptor,product_bytes); }
                catch(const std::exception &e) { return TerrainFailure(e.what()); }
                handles.push_back(handle); sources.push_back(std::move(source));
            }
            D3D12_RESOURCE_DESC tail_desc{}; tail_desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            tail_desc.Width=std::max(1u,d.width>>4); tail_desc.Height=std::max(1u,d.height>>4);
            tail_desc.DepthOrArraySize=1; tail_desc.MipLevels=static_cast<UINT16>(d.mip_count-4);
            tail_desc.Format=DXGI_FORMAT_BC7_TYPELESS; tail_desc.SampleDesc.Count=1;
            const auto color_bytes=device->GetResourceAllocationInfo(0,1,&tail_desc).SizeInBytes;
            tail_desc.Format=DXGI_FORMAT_BC7_UNORM;
            const auto linear_bytes=device->GetResourceAllocationInfo(0,1,&tail_desc).SizeInBytes;
            const UINT64 color_count=plane_ids.size()/3, linear_count=plane_ids.size()-color_count;
            if(!color_bytes || color_bytes==UINT64_MAX || !linear_bytes || linear_bytes==UINT64_MAX ||
                (color_count && color_bytes>UINT64_MAX/color_count) ||
                (linear_count && linear_bytes>UINT64_MAX/linear_count))
                return TerrainFailure("Invalid terrain tail allocation size.");
            const UINT64 color_total=color_bytes*color_count, linear_total=linear_bytes*linear_count;
            if(color_total>UINT64_MAX-linear_total) return TerrainFailure("Terrain tail allocation size overflow.");
            const UINT64 tail_total=color_total+linear_total;
            if(!budget_updated)
            {
                if(auto r=UpdateResidencyBudget();!r) return r;
                budget_updated=true;
            }
            if(!residency.CanAdmit(tail_total,10,segment))
            {
                retry_budget_bytes=tail_total;
                use_pages=false;
            }
            if(use_pages)
            {
            if(!terrain_plane_stream_ids.empty())
                return TerrainFailure("Terrain plane owner differs from its resident generation.");
            std::vector<TextureStreamRegistration> registrations;
            std::vector<std::shared_ptr<TerrainPlaneBacking>> backings;
            std::vector<TextureStreamId> streams(plane_ids.size());
            registrations.reserve(plane_ids.size()); backings.reserve(plane_ids.size());
            ComPtr<ID3D12CommandAllocator> command_allocator;
            ComPtr<ID3D12GraphicsCommandList> list;
            std::vector<AllocationResource> retained;
            std::vector<std::byte> tail_bytes;
            const UINT count=tail_desc.MipLevels;
            std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
            std::vector<UINT> rows(count);
            std::vector<UINT64> row_bytes(count);
            UINT64 upload_size{};
            device->GetCopyableFootprints(&tail_desc,0,count,0,footprints.data(),rows.data(),row_bytes.data(),&upload_size);
            constexpr UINT64 max_upload_batch=32ull*1024*1024;
            if(!upload_size || upload_size>max_upload_batch) return TerrainFailure("Invalid terrain tail footprint.");
            constexpr UINT64 align=D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
            const UINT64 stride=(upload_size+align-1)/align*align;
            const auto planes_per_batch=static_cast<std::size_t>(max_upload_batch/stride);
            const auto flush=[&]() -> Result {
                if(!list) return Result::Success();
                const auto closed=list->Close();
                if(FAILED(closed)) return HResultFailure("Close terrain tail list",closed);
                if(auto r=SubmitResidentUtilityUpload(list.Get(),command_allocator.Get(),retained);!r) return r;
                retained.clear(); list.Reset(); command_allocator.Reset();
                return Result::Success();
            };
            for(std::size_t plane=0;plane<plane_ids.size();++plane)
            {
                const auto &id=plane_ids[plane];
                const auto handle=handles[plane];
                const auto &descriptor=config.packed_textures[handle.value];
                auto expected=d; expected.array_size=1;
                const auto &source=sources[plane];
                auto backing=std::make_shared<TerrainPlaneBacking>();
                backing->generation_lease=resident_generation_lease;
                constexpr UINT tail=4;
                auto desc=tail_desc;
                if(plane%3==0) desc.Format=DXGI_FORMAT_BC7_TYPELESS;
                D3D12MA::ALLOCATION_DESC allocation{}; allocation.HeapType=D3D12_HEAP_TYPE_DEFAULT;
                if(auto r=CreateAllocation(backing->fallback,allocation,desc,D3D12_RESOURCE_STATE_COPY_DEST);!r) return r;
                const auto batch_plane=plane%planes_per_batch;
                if(batch_plane==0)
                {
                    if(auto r=flush();!r) return r;
                    const auto batch_count=std::min(planes_per_batch,plane_ids.size()-plane);
                    retained.emplace_back();
                    allocation.HeapType=D3D12_HEAP_TYPE_UPLOAD;
                    if(auto r=CreateAllocation(retained.front(),allocation,BufferDescription(batch_count*stride),
                        D3D12_RESOURCE_STATE_GENERIC_READ);!r) return r;
                    auto result=device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&command_allocator));
                    if(FAILED(result)) return HResultFailure("Create terrain tail allocator",result);
                    result=device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,command_allocator.Get(),nullptr,IID_PPV_ARGS(&list));
                    if(FAILED(result)) return HResultFailure("Create terrain tail list",result);
                }
                const UINT64 upload_offset=batch_plane*stride;
                // The admitted descriptor lays out the mip suffix contiguously.
                const auto &first=descriptor.subresources[tail];
                const auto &last=descriptor.subresources.back();
                tail_bytes.resize(static_cast<std::size_t>(last.offset-first.offset+last.byte_count));
                ShippedContentOwner loose_file;
                const ShippedContentOwner *file{};
                UINT64 file_offset=first.offset;
                if(source.IsPacked())
                {
                    file=source.Packed().owner.get();
                    file_offset+=source.Packed().range.offset;
                }
                else
                {
                    if(auto r=ShippedContentOwner::Open(source.Loose().path,loose_file);!r) return r;
                    file=&loose_file;
                }
                if(auto r=file->ReadExact(file_offset,tail_bytes);!r) return r;
                for(UINT mip=0;mip<count;++mip)
                {
                    const auto &input=descriptor.subresources[mip+tail];
                    const auto bytes=std::span(tail_bytes).subspan(input.offset-first.offset,input.byte_count);
                    if(row_bytes[mip]!=input.row_pitch || UINT64(rows[mip])*input.row_pitch!=input.byte_count ||
                        Sha256(bytes)!=input.digest)
                        return TerrainFailure("Terrain tail payload or upload footprint mismatch.");
                }
                void *address{}; D3D12_RANGE empty{};
                auto hr=retained.front().resource->Map(0,&empty,&address);
                if(FAILED(hr)) return HResultFailure("Map terrain tail upload",hr);
                for(UINT mip=0;mip<count;++mip)
                {
                    const auto &input=descriptor.subresources[mip+tail];
                    const auto *bytes=tail_bytes.data()+(input.offset-first.offset);
                    auto *out=static_cast<std::byte *>(address)+upload_offset+footprints[mip].Offset;
                    const auto pitch=footprints[mip].Footprint.RowPitch;
                    if(pitch==input.row_pitch)
                        std::memcpy(out,bytes,input.byte_count);
                    else for(UINT row=0;row<rows[mip];++row)
                        std::memcpy(out+UINT64(row)*pitch,bytes+UINT64(row)*input.row_pitch,input.row_pitch);
                }
                retained.front().resource->Unmap(0,nullptr);
                for(UINT mip=0;mip<count;++mip)
                {
                    D3D12_TEXTURE_COPY_LOCATION to{},from{}; to.pResource=backing->fallback.resource.Get();
                    to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; to.SubresourceIndex=mip;
                    from.pResource=retained.front().resource.Get(); from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    from.PlacedFootprint=footprints[mip]; from.PlacedFootprint.Offset+=upload_offset;
                    from.PlacedFootprint.Footprint.Format=desc.Format;
                    list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
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
                registration.sampler_feedback=false;
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
            else { use_pages=false; retry_budget_bytes=tail_total; }
            }
        }
        if(use_pages)
        {
            terrain_tiles=std::move(tiles);
            terrain_tile_counts.assign(sections.size(),{});
            for(const auto &tile:terrain_tiles) terrain_tile_counts[tile.source_index]={tile.count_x,tile.count_y};
        }
    }
    const bool rebuild_static=scene_changed || use_pages!=terrain_static_pages_current || (recheck && use_pages);
    if(rebuild_static) terrain_slot_static_sections_valid.fill(false);
    const auto include_meadow=[](const GpuTerrainSection &section,bool &has_meadow,float &upper_bound) {
        if(section.mesh!=static_cast<UINT>(RenderMesh::MeadowSurface) ||
            section.span.x==0 || section.span.y==0) return;
        has_meadow=true;
        const double upper=double(section.heightTransform.x)+
            std::abs(double(section.heightTransform.y))*render_data::kMeadowGroundHeightBound;
        float rounded=static_cast<float>(upper);
        if(double(rounded)<upper) rounded=std::nextafter(rounded,std::numeric_limits<float>::infinity());
        upper_bound=std::max(upper_bound,rounded);
    };
    auto &rows=terrain_gpu_sections_scratch;
    rows.clear();
    dynamic_terrain_indices.assign(dynamic.size(),UINT_MAX);
    const auto table_element_limit=1ull<<D3D12_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP;
    for(std::size_t i=0;i<dynamic.size();++i)
    {
        const auto &r=dynamic[i];const auto section=TerrainSectionFrom(r);if(!section) continue;
        if(!std::isfinite(r.position.x)||!std::isfinite(r.position.z)||!std::isfinite(r.scale.x)||
            !std::isfinite(r.scale.z)||!FiniteTerrainHeight(r)||
            !std::isfinite(r.yaw)||!std::isfinite(section->span_x)||
            !std::isfinite(section->span_z)) return TerrainFailure("Nonfinite terrain page transform.");
        if(sections.size()+rows.size()>=table_element_limit)
            return TerrainFailure("Terrain section table exceeds the native buffer element capacity.");
        dynamic_terrain_indices[i]=static_cast<UINT>(sections.size()+rows.size());
        GpuTerrainSection fallback{};fallback.center={section->x,section->z};
        fallback.span={section->span_x,section->span_z};fallback.rotation={section->cosine,section->sine};
        fallback.mesh=section->mesh;fallback.seed=section->seed;fallback.pageSlice=UINT_MAX;
        fallback.heightTransform={r.position.y,r.scale.y};rows.push_back(fallback);
    }
    const auto dynamic_count=rows.size();
    if(!rebuild_static && !recheck && dynamic_count==0 &&
        terrain_slot_static_sections_valid[frame_slot] &&
        terrain_frame_section_counts[frame_slot]==sections.size()) return Result::Success();

    UINT page_index=0;
    for(std::size_t i=0;i<sections.size();++i)
    {
        const auto &section=sections[i];GpuTerrainSection gpu{};
        gpu.center={section.x,section.z};gpu.span={section.span_x,section.span_z};
        gpu.rotation={section.cosine,section.sine};
        gpu.mesh=section.mesh;gpu.seed=section.seed;gpu.pageSlice=UINT_MAX;
        if(use_pages)
        {
            const auto counts=terrain_tile_counts[i];
            gpu.pageSlice=page_index*3;
            gpu.pageEdge=render_data::kTerrainSurfacePageEdge;gpu.tileCount=counts;
            page_index+=counts[0]*counts[1];
        }
        const auto &source_indices=scene_changed?next_static_source_indices:static_terrain_source_indices;
        const auto &source=scene->instances[source_indices[i]];
        if(!FiniteTerrainHeight(source))
            return TerrainFailure("Nonfinite terrain height transform.");
        gpu.heightTransform={source.position.y,source.scale.y};
        rows.push_back(gpu);
    }
    if(dynamic_count) std::rotate(rows.begin(),rows.begin()+static_cast<std::ptrdiff_t>(dynamic_count),rows.end());
    const auto section_count=rows.size();
    bool has_meadow=false;
    float meadow_upper=std::numeric_limits<float>::lowest();
    for(const auto &section:rows) include_meadow(section,has_meadow,meadow_upper);
    auto &table=terrain_frame_sections[frame_slot];
    const auto table_bytes=rows.size()*sizeof(GpuTerrainSection);
    if(table_bytes)
    {
        if(!table.resource || table.resource->GetDesc().Width<table_bytes)
        {
            AllocationResource candidate;
            D3D12MA::ALLOCATION_DESC allocation{};allocation.HeapType=D3D12_HEAP_TYPE_UPLOAD;
            if(auto r=CreateAllocation(candidate,allocation,BufferDescription(table_bytes),D3D12_RESOURCE_STATE_GENERIC_READ);!r) return r;
            table=std::move(candidate);
        }
        terrain_slot_static_sections_valid[frame_slot]=false;
        void *mapped{};const D3D12_RANGE empty{};
        const auto hr=table.resource->Map(0,&empty,&mapped);
        if(FAILED(hr)) return HResultFailure("Map terrain sections",hr);
        std::memcpy(mapped,rows.data(),table_bytes);
        const D3D12_RANGE written{0,table_bytes};
        table.resource->Unmap(0,&written);
    }
    terrain_frame_section_counts[frame_slot]=static_cast<UINT>(section_count);
    terrain_frame_has_meadow[frame_slot]=has_meadow;
    terrain_frame_meadow_upper[frame_slot]=meadow_upper;
    if(scene_changed)
    {
        static_terrain_source_indices=std::move(next_static_source_indices);
        static_terrain_sections=std::move(next_static);terrain_scene=scene;
    }
    terrain_static_pages_current=use_pages;terrain_archive_digest=archive_digest;
    terrain_retry_budget_bytes=retry_budget_bytes;
    terrain_slot_static_sections_valid[frame_slot]=dynamic_count==0;
    terrain_parameters_changed=false;
    return Result::Success();
}

void D3D12Renderer::Impl::DemandTerrainPages(std::span<const RenderViewInput> views,std::uint64_t frame_number)
{
    terrain_plane_needed.assign(terrain_plane_stream_ids.size(),0);
    if(!terrain_static_pages_current) return;
    const UINT finest=QualityProfileFor(config.texture_quality).texture_finest_mip;
    const auto detail_end = DirectX::PackedVector::XMConvertHalfToFloat(
        DirectX::PackedVector::XMConvertFloatToHalf(QualityProfileFor(config.material_quality).terrain_detail_fade_end_m));
    for (const auto &view : views)
    {
        using namespace DirectX;
        const auto &camera = view.camera;
        const auto eye = XMVectorSet(float(camera.position.x-view.scene_origin.x),
            float(camera.position.y-view.scene_origin.y), float(camera.position.z-view.scene_origin.z), 1);
        const auto direction = XMVectorSet(float(camera.target.x-camera.position.x),
            float(camera.target.y-camera.position.y), float(camera.target.z-camera.position.z), 0);
        const auto world_to_view = XMMatrixLookToLH(eye, direction,
            XMVectorSet(camera.up.x, camera.up.y, camera.up.z, 0));
        const float aspect = float(view.render_active.width) / float(view.render_active.height);
        const auto projection = XMMatrixPerspectiveFovLH(XMConvertToRadians(camera.vertical_fov_degrees),
            aspect, view.near_plane, view.far_plane);
        BoundingFrustum frustum;
        BoundingFrustum::CreateFromMatrix(frustum, projection);
        // Include a full pixel of temporal jitter at either screen edge.
        frustum.LeftSlope *= 1.f + 2.f / view.render_active.width;
        frustum.RightSlope *= 1.f + 2.f / view.render_active.width;
        frustum.TopSlope *= 1.f + 2.f / view.render_active.height;
        frustum.BottomSlope *= 1.f + 2.f / view.render_active.height;
        BoundingFrustum scene_frustum;
        frustum.Transform(scene_frustum, XMMatrixInverse(nullptr, world_to_view));

        for (std::size_t index = 0; index < terrain_tiles.size(); ++index)
        {
            const auto &tile = terrain_tiles[index];
            const auto &section = tile.section;
            const auto source_ordinal=static_terrain_source_indices[tile.source_index];
            const auto &height_source=terrain_scene->instances[source_ordinal];
            const DirectX::XMFLOAT2 height{height_source.position.y,height_source.scale.y};
            const float extent_x = std::abs(section.span_x) * (4.f / 7.f);
            const float extent_z = std::abs(section.span_z) * (4.f / 7.f);
            const float height_bound = section.mesh == static_cast<UINT>(RenderMesh::MeadowSurface)
                ? render_data::kMeadowGroundHeightBound : 1.f;
            const BoundingBox box{{section.x, height.x, section.z},
                {std::abs(section.cosine)*extent_x + std::abs(section.sine)*extent_z,
                 std::abs(height.y)*height_bound,
                 std::abs(section.sine)*extent_x + std::abs(section.cosine)*extent_z}};
            if (!scene_frustum.Intersects(box)) continue;
            const double dx = double(section.x) - XMVectorGetX(eye);
            const double dz = double(section.z) - XMVectorGetZ(eye);
            const double nearest = std::max(0.0, std::hypot(dx,dz) - std::hypot(extent_x,extent_z));
            const auto priority = std::bit_cast<UINT>(static_cast<float>(1.0/(1.0+nearest)));
            // The horizontal distance to the padded tile bounds is a lower bound on shader eye distance.
            const std::size_t plane_count = nearest <= detail_end ? 3 : 2;
            for (std::size_t plane = index*3; plane < index*3+plane_count; ++plane)
            {
                terrain_plane_needed[plane] = 1;
                texture_streaming.Demand(terrain_plane_stream_ids[plane], finest, priority, frame_number);
            }
        }
    }
}

Result D3D12Renderer::Impl::PublishTerrainStreaming(UINT frame_slot)
{
    if(frame_slot>=kFrameCount) return TerrainFailure("Invalid terrain frame slot.");
    auto &buffer=terrain_frame_mips[frame_slot];
    const auto bytes=UINT64(kTerrainPagePlaneCapacity)*sizeof(GpuTerrainPlaneMip);
    if(!buffer.resource)
    {
        D3D12MA::ALLOCATION_DESC allocation{}; allocation.HeapType=D3D12_HEAP_TYPE_UPLOAD;
        if(auto r=CreateAllocation(buffer,allocation,BufferDescription(bytes),D3D12_RESOURCE_STATE_GENERIC_READ);!r) return r;
    }
    auto &bank=terrain_descriptor_banks[frame_slot];
    const auto heap_start=srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
    if(bank.heap.Get()!=srv_cpu_heap.Get() || bank.first.ptr!=heap_start.ptr || bank.stride!=srv_stride ||
        bank.metadata_resource.Get()!=buffer.resource.Get())
    {
        bank={}; bank.heap=srv_cpu_heap; bank.first=heap_start; bank.stride=srv_stride;
        bank.metadata_resource=buffer.resource;
    }
    if(bank.entries.empty()) bank.entries.resize(kTerrainPagePlaneCapacity);
    const UINT quality=QualityProfileFor(config.texture_quality).texture_finest_mip+1;
    const auto current=terrain_static_pages_current?static_cast<UINT>(terrain_plane_stream_ids.size()):0u;
    const UINT limit=bank.descriptors_initialized?std::max(current,bank.published_count):kTerrainPagePlaneCapacity;
    GpuTerrainPlaneMip *mapped=nullptr;
    D3D12_RANGE written{static_cast<SIZE_T>(bytes),0};
    const auto map=[&]() -> Result
    {
        if(mapped) return Result::Success();
        void *address{}; D3D12_RANGE empty{};
        const auto hr=buffer.resource->Map(0,&empty,&address);
        if(FAILED(hr)) return HResultFailure("Map terrain texture metadata",hr);
        mapped=static_cast<GpuTerrainPlaneMip *>(address);
        return Result::Success();
    };
    if(!bank.metadata_initialized)
    {
        if(auto result=map();!result) return result;
        std::memset(mapped,0,static_cast<std::size_t>(bytes));
        written={0,static_cast<SIZE_T>(bytes)};
    }
    auto h=heap_start;
    h.ptr+=(SIZE_T(frame_slot)*kTextureDescriptorCount+kTerrainPageDescriptorOffset)*srv_stride;
    const auto base=h.ptr;
    h.ptr=base+SIZE_T(HS_TERRAIN_SECTION_SRV)*srv_stride;
    const auto &sections=terrain_frame_sections[frame_slot];
    const UINT section_count=terrain_frame_section_counts[frame_slot];
    D3D12_SHADER_RESOURCE_VIEW_DESC table{}; table.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
    table.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    auto *section_resource=section_count?sections.resource.Get():nullptr;
    if(!bank.descriptors_initialized || bank.section_resource.Get()!=section_resource || bank.section_count!=section_count)
    {
        table.Buffer.StructureByteStride=sizeof(GpuTerrainSection); table.Buffer.NumElements=std::max(1u,section_count);
        device->CreateShaderResourceView(section_resource,&table,h);
        bank.section_resource=section_resource;
        bank.section_count=section_count;
    }
    if(!bank.descriptors_initialized)
    {
        h.ptr=base+SIZE_T(HS_TERRAIN_MIP_SRV)*srv_stride;
        table.Buffer.StructureByteStride=sizeof(GpuTerrainPlaneMip); table.Buffer.NumElements=kTerrainPagePlaneCapacity;
        device->CreateShaderResourceView(buffer.resource.Get(),&table,h);
    }
    h.ptr=base+SIZE_T(HS_TERRAIN_PLANE_SRV)*srv_stride;
    for(UINT i=0;i<limit;++i)
    {
        GpuTerrainPlaneMip value{};
        const auto view=i<current ? texture_streaming.View(terrain_plane_stream_ids[i]) : TextureStreamView{};
        if(i<current && i<terrain_plane_needed.size() && terrain_plane_needed[i] && view.resource)
            value={view.first_mip,view.resident_min_mip,view.logical_description.mip_count,quality};
        auto &entry=bank.entries[i]; auto &previous=entry.metadata;
        if(value!=previous)
        {
            if(auto result=map();!result) return result;
            mapped[i]=value;
            written.Begin=std::min(written.Begin,SIZE_T(i)*sizeof(GpuTerrainPlaneMip));
            written.End=std::max(written.End,SIZE_T(i+1)*sizeof(GpuTerrainPlaneMip));
            previous=value;
        }
        if(!bank.descriptors_initialized || entry.resource.Get()!=view.resource)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format=i%3==0 ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM;
            srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels=view.resource?view.resource->GetDesc().MipLevels:1;
            device->CreateShaderResourceView(view.resource,&srv,h);
            entry.resource=view.resource;
        }
        h.ptr+=srv_stride;
    }
    if(mapped)
        buffer.resource->Unmap(0,&written);
    bank.metadata_initialized=true;
    bank.descriptors_initialized=true;
    bank.published_count=current;
    return Result::Success();
}
} // namespace hs
