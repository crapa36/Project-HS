#include "renderer_impl.hpp"
#include <DirectXPackedVector.h>
#include <hs/render_data/terrain_surface.hpp>
#include <hs/render_data/terrain_source_constants.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace hs
{
namespace
{
constexpr std::string_view kTerrainArchive="render/terrain/arena_surface.rpkg";
Result TerrainFailure(std::string message)
{ return Result::Failure(ErrorCode::InvalidState,"hs_renderer_d3d12",std::move(message)); }
bool FiniteTerrainTransform(const render_data::TerrainSurfaceSection &section, const RenderInstance &source)
{
    return std::isfinite(section.x) && std::isfinite(section.z) &&
        std::isfinite(section.span_x) && std::isfinite(section.span_z) &&
        std::isfinite(section.cosine) && std::isfinite(section.sine) &&
        std::isfinite(source.position.y) && std::isfinite(source.scale.y) &&
        (source.mesh!=RenderMesh::MeadowSurface ||
         std::abs(double(source.position.y)) + render_data::kMeadowGroundHeightBound*std::abs(double(source.scale.y)) <= std::numeric_limits<float>::max());
}
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
    const auto &dynamic = prepared_scene_package.dynamic_terrain;
    const auto found = std::ranges::lower_bound(dynamic, ordinal, {}, &PreparedScenePackage::DynamicTerrainSection::source_index);
    return found != dynamic.end() && found->source_index == ordinal
        ? static_cast<UINT>(static_terrain_source_indices.size() + (found - dynamic.begin())) : r.environment_seed;
}

Result D3D12Renderer::Impl::AppendDynamicTerrainSection(std::size_t source_index, const RenderInstance &source)
{
    const auto section = TerrainSectionFrom(source);
    if (!section) return Result::Success();
    if (!FiniteTerrainTransform(*section, source))
        return TerrainFailure("Nonfinite terrain page transform.");
    prepared_scene_package.dynamic_terrain.push_back({source_index, GpuTerrainSection::From(*section, source)});
    return Result::Success();
}

Result D3D12Renderer::Impl::PrepareTerrainPages(const std::shared_ptr<const StaticRenderScene> &scene, UINT frame_slot)
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
                if (!FiniteTerrainTransform(*section, scene->instances[i]))
                    return TerrainFailure("Nonfinite terrain page transform.");
                next_static_source_indices.push_back(static_cast<UINT>(i));
                next_static.push_back(*section);
            }
    }
    const auto &sections=scene_changed?next_static:static_terrain_sections;
    const auto &dynamic = prepared_scene_package.dynamic_terrain;
    const auto section_count = sections.size() + dynamic.size();
    if(section_count>(1ull<<D3D12_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP))
        return TerrainFailure("Terrain section table exceeds the native buffer element capacity.");
    const bool registered_archive=!terrain_plane_stream_ids.empty();
    const bool recheck=scene_changed || (terrain_retry_budget_bytes &&
        (!config.texture_budget_mib ||
            UINT64(config.texture_budget_mib)*1024*1024>=terrain_retry_budget_bytes));
    UINT64 retry_budget_bytes=recheck?0:terrain_retry_budget_bytes;
    render_data::TerrainSurfacePackageView archive;
    std::vector<render_data::TerrainSurfaceTile> tiles;
    bool use_pages=recheck?false:terrain_static_pages_current;
    if(recheck && !sections.empty() &&
        (registered_archive || RendererIsAdmittedRuntimeProduct(config,kTerrainArchive)))
    {
        // Registered planes retain this generation's admitted page layout.
        try{tiles=render_data::BuildTerrainSurfaceTiles(sections);}
        catch(const std::runtime_error &){tiles.clear();}
        if(registered_archive)
            use_pages=!tiles.empty() && tiles.size()==terrain_tiles.size() &&
                std::equal(tiles.begin(),tiles.end(),terrain_tiles.begin(),
                    [](const auto &a,const auto &b){return a.section==b.section;});
        else
        {
            const auto archive_digest=RendererProductDigest(config,kTerrainArchive);
            if(archive_digest.empty()) return TerrainFailure("Missing terrain archive digest.");
            RendererAssetSource source;std::vector<std::byte> bytes;
            if(auto r=ResolveRendererAsset(config,kTerrainArchive,source);!r) return r;
            if(auto r=ReadBinary(source,bytes);!r) return r;
            try{archive=render_data::ReadTerrainSurfacePackage(bytes,MakeAssetId(kTerrainArchive));}
            catch(const std::exception &e){return TerrainFailure(e.what());}
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
    }
    if(recheck && use_pages &&
        QualityProfileFor(config.texture_quality).texture_finest_mip>=render_data::kTerrainSurfacePageMips)
        return TerrainFailure("Terrain plane directory texture shape mismatch.");
    if(recheck && use_pages && !registered_archive)
    {
        const auto &d=archive.description;
        if(d.width!=render_data::kTerrainSurfacePageEdge || d.height!=d.width || d.format!=render_data::kTextureFormatBc7 ||
            d.mip_count!=render_data::kTerrainSurfacePageMips ||
            d.array_size!=archive.sections.size()*3 || archive.section_products.size()!=d.array_size)
            return TerrainFailure("Terrain plane directory texture shape mismatch.");
        if(d.array_size>kTerrainPagePlaneCapacity) use_pages=false;
        if(use_pages)
        {
            if(resource_generation_id.empty() || !resident_generation_lease)
                return TerrainFailure("Terrain streaming requires its actual resident generation lease.");
            const auto &plane_ids=archive.section_products;
            std::vector<TextureStreamRegistration> registrations;
            std::vector<TextureStreamId> streams(plane_ids.size());
            registrations.reserve(plane_ids.size());
            auto expected=d;
            expected.array_size=1;
            for(std::size_t plane=0;plane<plane_ids.size();++plane)
            {
                const auto &id=plane_ids[plane];
                const auto handle=FindRendererTexture(config,MakeAssetId(id));
                if(!handle) return TerrainFailure("Terrain plane lacks authenticated texture metadata.");
                const auto &descriptor=config.packed_textures[handle.value];
                TextureStreamRegistration registration;
                if(auto r=ResolveRendererAsset(config,id,registration.source);!r) return r;
                UINT64 product_bytes{};
                if(auto r=registration.source.ByteCount(product_bytes);!r) return r;
                if(product_bytes!=config.packed_bulks[descriptor.bulk_index].range.byte_count)
                    return TerrainFailure("Terrain plane size differs from its admitted texture.");
                registration.generation=resource_generation_id; registration.logical_id=id;
                registration.generation_lease=resident_generation_lease;
                registration.description=expected; registration.descriptor=descriptor;
                // Unpublished pages use the existing source material, without a second native mip suffix.
                registration.fallback_mode=TextureStreamFallback::SourceMaterial;
                registration.fallback_first_mip=4;
                registration.opacity_sampling=false; registration.srgb_view=plane%3==0;
                registration.sampler_feedback=false;
                registrations.push_back(std::move(registration));
            }
            bool admitted=false;
            if(auto r=texture_streaming.RegisterBatch(registrations,residency,streams,admitted,
                    &retry_budget_bytes);!r) return r;
            if(admitted)
            {
                terrain_plane_stream_ids=std::move(streams);
                retry_budget_bytes=0;
            }
            else use_pages=false;
        }
    }
    if(recheck && use_pages)
    {
        terrain_tile_counts.assign(sections.size(),{});
        terrain_tiles.clear();
        terrain_tiles.reserve(tiles.size());
        const auto &source_indices=scene_changed?next_static_source_indices:static_terrain_source_indices;
        for(const auto &tile:tiles)
        {
            terrain_tile_counts[tile.source_index]={tile.count_x,tile.count_y};
            const auto &section=tile.section;
            const auto &height_source=scene->instances[source_indices[tile.source_index]];
            const DirectX::XMFLOAT2 height{height_source.position.y,height_source.scale.y};
            const float extent_x = std::abs(section.span_x) * (4.f / 7.f);
            const float extent_z = std::abs(section.span_z) * (4.f / 7.f);
            const float height_bound = section.mesh == static_cast<UINT>(RenderMesh::MeadowSurface)
                ? render_data::kMeadowGroundHeightBound : 1.f;
            const DirectX::BoundingBox bounds{{section.x, height.x, section.z},
                {std::abs(section.cosine)*extent_x + std::abs(section.sine)*extent_z,
                 std::abs(height.y)*height_bound,
                 std::abs(section.sine)*extent_x + std::abs(section.cosine)*extent_z}};
            terrain_tiles.push_back({section,bounds});
        }
    }
    const bool rebuild_static=scene_changed || use_pages!=terrain_static_pages_current || (recheck && use_pages);
    if(rebuild_static) terrain_slot_static_sections_valid.fill(false);
    if (!rebuild_static && !recheck && dynamic.empty() && terrain_slot_static_sections_valid[frame_slot])
        return Result::Success();

    bool has_meadow = false;
    float meadow_upper = std::numeric_limits<float>::lowest();
    auto &table = terrain_frame_sections[frame_slot];
    const auto table_bytes = section_count * sizeof(GpuTerrainSection);
    if (table_bytes)
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
        auto *destination = static_cast<std::byte *>(mapped);
        const auto write_section = [&](const GpuTerrainSection &row) {
            std::memcpy(destination, &row, sizeof(row));
            destination += sizeof(row);
            if (row.mesh != static_cast<UINT>(RenderMesh::MeadowSurface) || row.span.x == 0 || row.span.y == 0)
                return;
            has_meadow = true;
            const double upper = double(row.heightTransform.x) +
                std::abs(double(row.heightTransform.y)) * render_data::kMeadowGroundHeightBound;
            float rounded = static_cast<float>(upper);
            if (double(rounded) < upper) rounded = std::nextafter(rounded, std::numeric_limits<float>::infinity());
            meadow_upper = std::max(meadow_upper, rounded);
        };
        UINT plane_index = 0;
        const auto &source_indices = scene_changed ? next_static_source_indices : static_terrain_source_indices;
        for (std::size_t i = 0; i < sections.size(); ++i)
        {
            auto row = GpuTerrainSection::From(sections[i], scene->instances[source_indices[i]]);
            if (use_pages)
            {
                row.pageSlice = plane_index;
                row.pageEdge = render_data::kTerrainSurfacePageEdge;
                row.tileCount = terrain_tile_counts[i];
                plane_index += row.tileCount[0] * row.tileCount[1] * 3;
            }
            write_section(row);
        }
        for (const auto &section : dynamic) write_section(section.row);
        const D3D12_RANGE written{0, table_bytes};
        table.resource->Unmap(0, &written);
    }
    terrain_frame_section_counts[frame_slot]=static_cast<UINT>(section_count);
    terrain_frame_has_meadow[frame_slot]=has_meadow;
    terrain_frame_meadow_upper[frame_slot]=meadow_upper;
    if(scene_changed)
    {
        static_terrain_source_indices=std::move(next_static_source_indices);
        static_terrain_sections=std::move(next_static);terrain_scene=scene;
    }
    terrain_static_pages_current=use_pages;
    terrain_retry_budget_bytes=retry_budget_bytes;
    terrain_slot_static_sections_valid[frame_slot]=dynamic.empty();
    return Result::Success();
}

void D3D12Renderer::Impl::DemandTerrainPages(std::span<const RenderViewSample> views,std::uint64_t frame_number)
{
    terrain_plane_needed.reset();
    if(!terrain_static_pages_current) return;
    const auto detail_end = DirectX::PackedVector::XMConvertHalfToFloat(
        DirectX::PackedVector::XMConvertFloatToHalf(QualityProfileFor(config.material_quality).terrain_detail_fade_end_m));
    for (const auto &view : views)
    {
        using namespace DirectX;
        const auto &camera = view.camera;
        const auto eye = XMVectorSet(camera.position.x, camera.position.y, camera.position.z, 1);
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
            const auto &box = terrain_tiles[index].bounds;
            if (!scene_frustum.Intersects(box)) continue;
            // Distance to the padded 3D box lower-bounds eye distance for every terrain sample.
            const auto offset = XMVectorAbs(XMVectorSubtract(eye, XMLoadFloat3(&box.Center)));
            const auto outside = XMVectorMax(XMVectorSubtract(offset, XMLoadFloat3(&box.Extents)), XMVectorZero());
            const float nearest = XMVectorGetX(XMVector3Length(outside));
            const auto priority = std::bit_cast<UINT>(1.f / (1.f + nearest));
            const std::size_t plane_count = nearest <= detail_end ? 3 : 2;
            for (std::size_t plane = index*3; plane < index*3+plane_count; ++plane)
            {
                terrain_plane_needed[plane] = true;
                // Bootstrap only the tail; completed shader footprints merge finer demand before Pump.
                texture_streaming.Demand(terrain_plane_stream_ids[plane], UINT_MAX, priority, frame_number);
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
    if(bank.entries.empty()) bank.entries.resize(kTerrainPagePlaneCapacity);
    const UINT quality=QualityProfileFor(config.texture_quality).texture_finest_mip+1;
    const auto current=terrain_static_pages_current?static_cast<UINT>(terrain_plane_stream_ids.size()):0u;
    const UINT limit=std::max(current,bank.published_count);
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
    if(!bank.descriptors_initialized)
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
    if(bank.section_resource.Get()!=section_resource || bank.section_count!=section_count)
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
    for(UINT i=0;i<limit;++i)
    {
        auto &entry=bank.entries[i];
        const bool needed=i<current && terrain_plane_needed[i];
        if(!needed && !entry.resource) continue;
        const auto view=needed ? texture_streaming.View(terrain_plane_stream_ids[i]) : TextureStreamView{};
        const auto value=view.resource
            ? GpuTerrainPlaneMip{view.first_mip,view.resident_min_mip,view.logical_description.mip_count,quality}
            : GpuTerrainPlaneMip{};
        if(value!=entry.metadata)
        {
            if(auto result=map();!result) return result;
            mapped[i]=value;
            written.Begin=std::min(written.Begin,SIZE_T(i)*sizeof(GpuTerrainPlaneMip));
            written.End=std::max(written.End,SIZE_T(i+1)*sizeof(GpuTerrainPlaneMip));
            entry.metadata=value;
        }
        if(entry.resource.Get()!=view.resource)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format=i%3==0 ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM;
            srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels=view.resource?view.resource->GetDesc().MipLevels:1;
            h.ptr=base+SIZE_T(HS_TERRAIN_PLANE_SRV+i)*srv_stride;
            device->CreateShaderResourceView(view.resource,&srv,h);
            entry.resource=view.resource;
        }
    }
    if(mapped)
        buffer.resource->Unmap(0,&written);
    bank.descriptors_initialized=true;
    bank.published_count=current;
    return Result::Success();
}
} // namespace hs
