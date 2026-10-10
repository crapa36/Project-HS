#include "renderer_impl.hpp"
#include <hs/render_data/volumetric_medium.hpp>
#include <hs/render_data/material_ir.hpp>
#include <hs/renderer/brixelizer_shaders.hpp>
#include <bit>
#include <map>
#include <set>
#include <stdexcept>

namespace hs
{
namespace
{
void RayWord(std::vector<std::byte> &bytes,std::uint64_t word,unsigned size=4)
{for(unsigned i=0;i<size;++i)bytes.push_back(std::byte(word>>(8*i)));}
render_data::OpacityMicromapDigest RayDigest(std::string_view hex)
{
    if(hex.starts_with("sha256:"))hex.remove_prefix(7);
    if(hex.size()!=64)throw std::runtime_error("Native ray selected product lacks an exact SHA256 digest.");
    render_data::OpacityMicromapDigest digest{};
    const auto nibble=[](char c)->unsigned{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;
        if(c>='A'&&c<='F')return c-'A'+10;throw std::runtime_error("Invalid selected native ray SHA256 digest.");};
    for(std::size_t i=0;i<32;++i)digest[i]=std::byte((nibble(hex[i*2])<<4)|nibble(hex[i*2+1]));return digest;
}
void RayAppend(std::vector<std::byte> &bytes,const render_data::OpacityMicromapDigest &digest)
{bytes.insert(bytes.end(),digest.begin(),digest.end());}
std::string RayMaterialId(std::size_t ordinal)
{
    if(ordinal>=kResidentMaterialCount)throw std::runtime_error("Native OMM material binding is outside actual resident materials.");
    return ordinal>=kAuthoredMaterialNames.size()?"render/material/"+std::string(kHeroMaterialNames[ordinal-kAuthoredMaterialNames.size()])+"/material.rpkg":
        ordinal==kTerrainMaterialOrdinal?"render/material/terrain.rpkg":"render/"+std::string(kAuthoredMaterialNames[ordinal])+".rpkg";
}
}
Result D3D12Renderer::Impl::NativeRayBudget(std::uint64_t &headroom)const
{
    headroom=0;DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
    if(!adapter||!allocator)return Result::Failure(ErrorCode::InvalidState,"hs_renderer_d3d12","Native ray admission has no actual adapter/allocator budget.");
    const auto queried=adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&memory);
    if(FAILED(queried))return HResultFailure("Query native OMM/SER byte budget",queried);
    if(!memory.Budget)return Result::Failure(ErrorCode::InvalidState,"hs_renderer_d3d12","Native ray admission has no observed local memory budget.");
    // Follow the existing residency policy's OS headroom and outstanding reservations.
    const auto limit=memory.Budget-memory.Budget/10;const auto reserved=residency.Accounting().gpu_reserved_total;
    if(memory.CurrentUsage<limit&&reserved<=limit-memory.CurrentUsage)headroom=limit-memory.CurrentUsage-reserved;
    D3D12MA::Budget actual{};allocator->GetBudget(&actual,nullptr);
    headroom=std::min(headroom,actual.BudgetBytes>actual.UsageBytes?actual.BudgetBytes-actual.UsageBytes:0);
    return Result::Success();
}
Result D3D12Renderer::Impl::ResolveNativeRay(const RendererConfig &request,NativeRayPlan &out)const
{
    out={};out.omm.requested=request.opacity_micromaps;out.ser.requested=request.shader_execution_reordering;
    const auto &hardware=feature_capabilities.hardware;
    out.omm.hardware_supported=hardware.opacity_micromap_arrays&&hardware.shader_model69;
    out.ser.hardware_supported=hardware.inline_ray_queries&&hardware.shader_model69&&hardware.ser_actually_reorders;
    const auto plan=ResolveFeatures(request);const bool gi=plan.gi_mode==GraphicsGiMode::RayTraced;
    const bool hybrid=plan.reflection_mode==GraphicsReflectionMode::HybridRT;
    const auto base=[&](GraphicsAutoToggle toggle,bool omm)->std::string
    {
        if(toggle==GraphicsAutoToggle::Off)return "Requested Off.";
        if(!request.ray_tracing_enabled)return "Ray tracing master is disabled.";
        if(!plan.ray_scene_enabled||(!gi&&!hybrid))return "No active RTGI or HybridRT transport consumer.";
        if(!hardware.shader_model69)return "Shader Model6.9 is unavailable.";
        if(omm&&!hardware.opacity_micromap_arrays)return "Native DXR1.2 opacity micromap arrays are unavailable.";
        if(!omm&&!hardware.ser_actually_reorders)return "OPTIONS22 reports no actual shader execution reordering.";
        return {};
    };
    out.omm.reason=base(request.opacity_micromaps,true);out.ser.reason=base(request.shader_execution_reordering,false);
    const auto products=[&](bool omm,bool ser)
    {return (!gi||request.product_paths.contains(ser?(omm?"render/shader/rtgi_trace_omm_ser_lib.dxil":"render/shader/rtgi_trace_ser_lib.dxil"):
            "render/shader/rtgi_trace_omm_cs.dxil"))&&
        (!hybrid||request.product_paths.contains(ser?(omm?"render/shader/hybrid_reflection_trace_omm_ser_lib.dxil":"render/shader/hybrid_reflection_trace_ser_lib.dxil"):
            "render/shader/hybrid_reflection_trace_omm_cs.dxil"));};
    if(out.omm.reason.empty())
    {
        if(!request.content_generation_lease||request.content_generation_id.empty())out.omm.reason="Selected coverage has no immutable Generation lease.";
        else if(std::ranges::none_of(request.product_paths,[](const auto &product){return product.first.starts_with("render/ray/omm/");}))
            out.omm.reason="Selected Generation has no native coverage packages.";
        else if(!products(true,false)&&!(out.ser.reason.empty()&&products(true,true)))out.omm.reason="Selected Generation lacks the active OMM SM6.9 products.";
        else out.selection.use_omms=true;
    }
    if(out.ser.reason.empty())
    {
        if(products(out.selection.use_omms,true))out.selection.use_ser=true;
        else if(out.selection.use_omms&&request.opacity_micromaps==GraphicsAutoToggle::Auto&&request.shader_execution_reordering==GraphicsAutoToggle::On&&products(false,true))
        {out.selection.use_omms=false;out.selection.use_ser=true;out.omm.reason="Combined library is missing; explicit SER uses the ordinary acceleration view.";}
        else out.ser.reason="Selected Generation lacks the active SER lib6.9 products.";
    }
    if(out.selection.use_omms&&!out.selection.use_ser&&!products(true,false))
    {out.selection.use_omms=false;out.omm.reason="Selected Generation lacks the active OMM compute products.";}
    if(out.selection.use_omms||out.selection.use_ser)
    {
        const auto budget=NativeRayBudget(out.budget_bytes);
        if(!budget||!out.budget_bytes)
        {if(out.selection.use_omms)out.omm.reason=budget?"No native OMM byte budget remains.":std::string(budget.Message());
            if(out.selection.use_ser)out.ser.reason=budget?"No native SER byte budget remains.":std::string(budget.Message());out.selection={};}
    }
    out.omm.resolved=out.selection.use_omms;out.ser.resolved=out.selection.use_ser;
    out.omm.budget_bytes=out.ser.budget_bytes=out.budget_bytes;
    if(out.omm.resolved)out.omm.reason="Selected native OMM consumer is admitted; awaiting actual arrays and linkage.";
    if(out.ser.resolved)out.ser.reason="Selected native SER consumer is admitted; awaiting actual DispatchRays.";
    if(request.opacity_micromaps==GraphicsAutoToggle::On&&!out.omm.resolved)
        return Result::Failure(ErrorCode::InvalidState,"hs_renderer_d3d12","Requested OMM On is unavailable: "+out.omm.reason);
    if(request.shader_execution_reordering==GraphicsAutoToggle::On&&!out.ser.resolved)
        return Result::Failure(ErrorCode::InvalidState,"hs_renderer_d3d12","Requested SER On is unavailable: "+out.ser.reason);
    return Result::Success();
}
Result D3D12Renderer::Impl::PrepareRayOmm(const RendererConfig &request,RayOmmCandidate &candidate)const
{
    NativeRayPlan plan;if(auto resolved=ResolveNativeRay(request,plan);!resolved)return resolved;
    RayOmmProfile profile;profile.enabled=plan.selection.use_omms;profile.enhanced_barriers=enhanced;profile.frame_slots=kFrameCount;
    profile.native_byte_budget=plan.budget_bytes;profile.maximum_subdivision=4;
    return ray_omm.Prepare(device.Get(),allocator,profile,[request](std::string_view id,std::vector<std::byte> &bytes)
    {
        std::filesystem::path path;if(auto resolved=request.ResolveProduct(id,path);!resolved)return resolved;
        if(auto read=ReadBinary(path,bytes);!read)return read;
        try{const auto digest=request.product_digests.find(std::string(id));
            if(digest==request.product_digests.end()||render_data::Sha256(bytes)!=RayDigest(digest->second))
                return Result::Failure(ErrorCode::InvalidState,"hs_renderer_d3d12","Selected native OMM package digest changed: "+std::string(id));}
        catch(const std::exception &error){return Result::Failure(ErrorCode::InvalidState,"hs_renderer_d3d12",error.what());}
        return Result::Success();
    },candidate);
}
Result D3D12Renderer::Impl::BuildRayOmmRequest(const GeometryPageGpu &page,const RayHitGeometry &job,
    RayOmmGeometryRequest &request,std::vector<std::uint32_t> &words)const
{
    request={};words.clear();if(page.source_product.empty()||(job.flags&RayGeometryTransmissive))return Result::Success();
    const auto prefix=page.source_product.starts_with("render/geometry/")?16u:page.source_product.starts_with("render/hlod/")?7u:0u;
    if(!prefix||!page.source_product.ends_with(".rpage"))return Result::Success();
    const auto product="render/ray/omm/"+page.source_product.substr(prefix,page.source_product.size()-prefix-6)+".rpkg";
    if(!config.product_paths.contains(product))return Result::Success();
    if(ray_omm_request_revision!=material_parameter_revision||ray_omm_request_pipeline_revision!=pipeline_revision||ray_omm_request_generation!=resource_generation_id)
    {ray_omm_request_cache.clear();ray_omm_request_revision=material_parameter_revision;ray_omm_request_pipeline_revision=pipeline_revision;ray_omm_request_generation=resource_generation_id;}
    const auto key=std::tuple{page.source_asset.value,job.mesh_id,job.environment_ordinal};
    if(const auto found=ray_omm_request_cache.find(key);found!=ray_omm_request_cache.end()&&
        found->second.request.identity.geometry_digest==page.source_digest)
    {request=found->second.request;words=found->second.words;request.actual_opacity_words=words;return Result::Success();}
    try
    {
        const auto read=[&](std::string_view id,std::vector<std::byte> &bytes)->Result
        {std::filesystem::path path;if(auto result=config.ResolveProduct(id,path);!result)return result;
            if(auto result=ReadBinary(path,bytes);!result)return result;
            const auto digest=config.product_digests.find(std::string(id));
            if(digest==config.product_digests.end()||render_data::Sha256(bytes)!=RayDigest(digest->second))
                return Result::Failure(ErrorCode::InvalidState,"hs_renderer_d3d12","Native ray selected immutable product digest changed: "+std::string(id));
            return Result::Success();};
        std::vector<std::byte> coverage_bytes,page_bytes;
        if(auto result=read(product,coverage_bytes);!result)return result;
        const auto coverage=render_data::ReadOpacityMicromapPackage(coverage_bytes,MakeAssetId(product));
        if(auto result=read(page.source_product,page_bytes);!result)return result;
        if(render_data::Sha256(page_bytes)!=page.source_digest)
            return Result::Failure(ErrorCode::InvalidState,"hs_renderer_d3d12","Current canonical ray page differs from its uploaded immutable source.");
        const auto geometry=render_data::DecodeGeometryPage(page_bytes,page.source_asset,page.metadata);
        const bool hero=(job.flags&RayGeometryCachedSkin)!=0;
        std::span<const render_data::CharacterMaterialBinding> bindings;
        if(hero)
        {
            const auto mesh=static_cast<RenderMesh>(job.mesh_id);
            if(mesh==RenderMesh::Archer)bindings=archer_material_bindings;
            else if(job.mesh_id>=static_cast<std::uint32_t>(RenderMesh::MonsterMelee)&&
                job.mesh_id-static_cast<std::uint32_t>(RenderMesh::MonsterMelee)<monster_assets.size())
                bindings=monster_assets[job.mesh_id-static_cast<std::uint32_t>(RenderMesh::MonsterMelee)].material_bindings;
            if(bindings.empty())return Result::Success();
        }
        std::set<std::size_t> ordinals;std::set<std::uint32_t> opaque_slots;
        if(hero)for(const auto &binding:bindings)
        {ordinals.insert(kAuthoredMaterialNames.size()+binding.hero_ordinal);if(binding.domain==render_data::HeroMaterialDomain::Opaque)opaque_slots.insert(binding.slot);}
        else if((job.flags&RayGeometryAuthoredEnvironment)&&job.environment_ordinal<kAuthoredMaterialNames.size())ordinals.insert(job.environment_ordinal);
        else return Result::Success();
        std::map<std::size_t,std::vector<std::uint32_t>> parameter_words;
        std::vector<render_data::OpacityMicromapMaterialWords> tags;
        for(const auto ordinal:ordinals)
        {
            const auto id=RayMaterialId(ordinal);const auto &header=material_headers.at(ordinal);
            const auto &material=header.view;const auto selected=config.product_digests.find(id);
            if(selected==config.product_digests.end()||render_data::Sha256(header.bytes)!=RayDigest(selected->second))
                return Result::Failure(ErrorCode::InvalidState,"hs_renderer_d3d12","Actual resident material differs from native coverage Generation: "+id);
            const auto layout=std::ranges::find(material.chunks,render_data::ChunkType::MaterialParameterLayout,&render_data::ChunkView::type);
            const auto programs=std::ranges::find(material.chunks,render_data::ChunkType::MaterialProgram,&render_data::ChunkView::type);
            if(layout==material.chunks.end()||programs==material.chunks.end())throw std::runtime_error("Native coverage material has no actual reflected layout/programs.");
            const auto layout_hash=render_data::DecodeMaterialParameterLayout(layout->bytes);
            if(layout_hash!=material_parameter_layouts.at(ordinal))throw std::runtime_error("Native coverage actual parameter upload layout disagrees with selected material.");
            std::vector<std::byte> omp;RayWord(omp,0x31504d4fu);RayAppend(omp,RayDigest(selected->second));RayAppend(omp,RayDigest(layout_hash));
            std::map<std::uint64_t,render_data::OpacityMicromapDigest> sorted;
            for(const auto &program:render_data::DecodeMaterialPrograms(programs->bytes,material.dependencies))
            {const auto &shader=material.dependencies.at(program.dependency_index);const auto digest=config.product_digests.find(shader);
                if(digest==config.product_digests.end()||!sorted.emplace(MakeAssetId(shader).value,RayDigest(digest->second)).second)
                    throw std::runtime_error("Native coverage selected material program identity is missing or repeated.");}
            if(sorted.empty())throw std::runtime_error("Native coverage selected material has no actual programs.");
            RayWord(omp,sorted.size());for(const auto &[asset,digest]:sorted){RayWord(omp,asset,8);RayAppend(omp,digest);}
            const auto first=material_parameter_bases.at(ordinal),end=ordinal+1<kResidentMaterialCount?
                material_parameter_bases.at(ordinal+1):material_parameter_slot_count;
            if(first>end||end>material_parameter_slots.size())throw std::runtime_error("Native coverage cannot identify actual full parameter upload words.");
            auto &live=parameter_words[ordinal];for(auto slot=first;slot<end;++slot)
                live.insert(live.end(),material_parameter_slots[slot].words.begin(),material_parameter_slots[slot].words.end());
            tags.push_back({MakeAssetId(id),render_data::Sha256(omp),live,0x3f000000u});
        }
        words=render_data::BuildOpacityMicromapWords(tags);
        std::vector<std::byte> topology,word_bytes;RayWord(topology,0x3154504fu);RayWord(topology,geometry.vertices.size());
        for(const auto &vertex:geometry.vertices){for(float uv:vertex.uv)RayWord(topology,std::bit_cast<std::uint32_t>(uv));RayWord(topology,vertex.material_index);}
        RayWord(topology,geometry.indices.size());for(auto index:geometry.indices)RayWord(topology,index);
        RayWord(topology,geometry.submeshes.size());for(const auto &submesh:geometry.submeshes)
        {RayWord(topology,submesh.first_index);RayWord(topology,submesh.index_count);RayWord(topology,submesh.material_index);}
        for(std::size_t triangle=0;triangle<geometry.indices.size()/3;++triangle)
        {const auto slot=geometry.vertices.at(geometry.indices[triangle*3]).material_index;
            const bool uniform=geometry.vertices.at(geometry.indices[triangle*3+1]).material_index==slot&&geometry.vertices.at(geometry.indices[triangle*3+2]).material_index==slot;
            RayWord(topology,uniform&&(!hero||opaque_slots.contains(slot))?(hero?slot:0u):UINT32_MAX);}
        for(auto word:words)RayWord(word_bytes,word);
        auto identity=coverage.identity;identity.geometry_asset=page.source_asset;identity.geometry_digest=page.source_digest;
        identity.topology_digest=render_data::Sha256(topology);identity.material_digest=render_data::Sha256(word_bytes);identity.triangle_count=page.index_count/3;
        // The producer emits a canonical compact JSON list after OMR1/live words.
        // Every immutable selected dependency must match this actual Generation.
        const auto proof_offset=8+coverage.opacity_word_bytes.size();
        if(proof_offset>coverage.recipe.size())throw std::runtime_error("Native coverage recipe has no immutable proof input.");
        const std::string_view proof(reinterpret_cast<const char*>(coverage.recipe.data()+proof_offset),coverage.recipe.size()-proof_offset);
        const auto marker=proof.find("\"immutable_selected_products\":[");
        const auto end=marker==std::string_view::npos?std::string_view::npos:proof.find(']',marker);
        if(end==std::string_view::npos)throw std::runtime_error("Native coverage recipe omits selected dependency identities.");
        const auto selected_proof=proof.substr(marker,end-marker);
        for(const auto &dependency:coverage.dependencies)
        {const auto digest=config.product_digests.find(dependency);
            if(digest==config.product_digests.end()||!config.product_paths.contains(dependency)||
                selected_proof.find("{\"digest\":\""+digest->second+"\",\"logical_id\":\""+dependency+"\",\"size\":")==std::string_view::npos)
            {words.clear();return Result::Success();}}
        request={product,MakeAssetId(product),identity,MakeAssetId(resource_generation_id).value,resident_generation_lease,words};
        auto cached=request;cached.actual_opacity_words={};ray_omm_request_cache[key]={std::move(cached),words};
        return Result::Success();
    }
    catch(const std::exception &error){return Result::Failure(ErrorCode::InvalidState,"hs_renderer_d3d12",error.what());}
}
Result D3D12Renderer::Impl::PrepareLocalProbes(const RendererConfig &candidate_config,
    LocalProbeRuntimeCandidate &candidate) const
{
    RendererConfig selected;
    if (auto prepared = SelectTextureFeedbackShaders(candidate_config, selected); !prepared) return prepared;
    // Shader/parameter adoption preserves the resident geometry and immutable scene owner.
    return local_probes.Prepare(device.Get(), allocator, root_signature.Get(), selected,
        resource_generation_id, candidate, &pipeline_cache);
}
Result D3D12Renderer::Impl::PrepareVolumetrics(const RendererConfig &candidate_config,
    ViewExtent capacity, VolumetricPrepared &candidate) const
{
    candidate = {};
    if (candidate_config.volumetric_quality != GraphicsOptionalQuality::Off)
    {
        constexpr std::string_view id = "render/lighting/volumetric_media.rpkg";
        std::filesystem::path path;
        if (auto resolved = candidate_config.ResolveProduct(id, path); !resolved) return resolved;
        std::vector<std::byte> bytes;
        if (auto read = ReadBinary(path, bytes); !read) return read;
        try
        {
            const auto package = render_data::ReadPackage(bytes, MakeAssetId(id));
            if (package.chunks.size() != 1 || package.chunks[0].type != render_data::ChunkType::VolumetricVolumes)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Invalid volumetric medium package.");
            const auto media = render_data::DecodeVolumetricMedia(package.chunks[0].bytes);
            for (const auto &source : media)
            {
                VolumetricVolume volume;
                for (std::size_t row = 0; row != 4; ++row)
                    for (std::size_t column = 0; column != 4; ++column)
                        volume.world_to_local.m[row][column] = source.world_to_local[row * 4 + column];
                volume.density_extinction = {source.density_extinction[0], source.density_extinction[1], source.density_extinction[2], 0};
                volume.scattering = {source.scattering[0], source.scattering[1], source.scattering[2], 0};
                volume.identity = {std::uint32_t(source.stable_id), std::uint32_t(source.stable_id >> 32), source.generation, source.shape};
                candidate.media.push_back(volume);
            }
            candidate.revision = 14695981039346656037ull;
            for (const auto byte : bytes) candidate.revision = (candidate.revision ^ std::uint8_t(byte)) * 1099511628211ull;
        }
        catch (const std::exception &error)
        { return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", error.what()); }
    }
    return volumetrics.Prepare(device.Get(), allocator, root_signature.Get(),
        candidate_config.volumetric_quality, QualityProfileFor(candidate_config.volumetric_quality == GraphicsOptionalQuality::Off
            ? GraphicsQuality::Low : static_cast<GraphicsQuality>(static_cast<unsigned>(candidate_config.volumetric_quality) - 1u)), capacity, kFrameCount, 256,
        [candidate_config](std::string_view id, std::vector<std::byte> &bytes) {
            std::filesystem::path path;
            if (auto resolved = candidate_config.ResolveProduct(id, path); !resolved) return resolved;
            return ReadBinary(path, bytes);
        }, candidate.service, &pipeline_cache, candidate_config.vfx_quality == VfxQuality::High);
}
Result D3D12Renderer::Impl::CommitVolumetrics(VolumetricPrepared &&candidate)
{
    if (auto committed = volumetrics.Commit(std::move(candidate.service), graph_queue_executor.Completed()); !committed) return committed;
    volumetric_media = std::move(candidate.media);
    volumetric_revision = candidate.revision;
    previous_volumetric_camera = {};
    return Result::Success();
}
RendererFeaturePlan D3D12Renderer::Impl::ResolveFeatures(const RendererConfig &request) const noexcept
{
    auto available = feature_capabilities;
    // Selected-generation availability is part of activation. A missing
    // optional program resolves to GTAO; corrupt supplied products still fail
    // their normal integrity/PSO preparation instead of being hidden.
    for (const auto id : {"render/shader/ray_geometry_cs.dxil", "render/shader/rtao_trace_cs.dxil",
            "render/shader/rtao_temporal_cs.dxil", "render/shader/rtao_resolve_cs.dxil"})
        available.rtao_consumer = available.rtao_consumer && request.product_paths.contains(id);
    for (const auto id : {"render/shader/ray_geometry_cs.dxil", "render/shader/rtgi_trace_cs.dxil",
            "render/shader/rtgi_temporal_cs.dxil", "render/shader/rtgi_spatial_cs.dxil",
            "render/shader/ray_transport_compose_cs.dxil"})
        available.rtgi_consumer = available.rtgi_consumer && request.product_paths.contains(id);
    for (const auto id : {"render/shader/ray_geometry_cs.dxil", "render/shader/hybrid_depth_init_cs.dxil",
            "render/shader/hybrid_depth_reduce_cs.dxil", "render/shader/hybrid_reflection_trace_cs.dxil",
            "render/shader/hybrid_reflection_temporal_cs.dxil", "render/shader/hybrid_reflection_spatial_cs.dxil",
            "render/shader/ray_transport_compose_cs.dxil"})
        available.rt_reflection_consumer = available.rt_reflection_consumer && request.product_paths.contains(id);
    for (const auto id : {"render/shader/ray_geometry_cs.dxil", "render/shader/ray_shadow_trace_cs.dxil",
            "render/shader/ray_shadow_temporal_cs.dxil", "render/shader/ray_shadow_resolve_cs.dxil"})
        available.rt_shadow_consumer = available.rt_shadow_consumer && request.product_paths.contains(id);
    for (const auto id : {"render/shader/ray_geometry_cs.dxil", "render/shader/brixelizer_guides_cs.dxil",
            "render/shader/brixelizer_gi_resolve_cs.dxil", "render/shader/brixelizer_cache_export_cs.dxil",
            "render/shader/brixelizer_gi_compose_cs.dxil", "render/shader/brixelizer_cache_direct_ps.dxil"})
        available.software_gi_consumer = available.software_gi_consumer && request.product_paths.contains(id);
#if HS_WITH_BRIXELIZER
    if (available.software_gi_consumer &&
        (request.gi_mode == GraphicsGiMode::SoftwareHybrid || request.gi_mode == GraphicsGiMode::RayTraced))
    {
        // Validate the exact pinned official permutation table once per query;
        // lookup does not read or allocate any native context/shader storage.
        try
        {
            static const auto products = [] {
                std::vector<std::string> ids;
                if (!brixelizer_native::EnumerateShaders([&](std::string_view id,std::span<const std::byte>) {
                        ids.emplace_back(id);return true;
                    })) ids.clear();
                for(UINT noise=0;noise<16;++noise)ids.push_back("render/texture/brixelizer_noise_"+std::to_string(noise)+".rpkg");
                return ids;
            }();
            available.software_gi_consumer = products.size() == 748 && std::ranges::all_of(products,
                [&](const auto &id) {return request.product_paths.contains(id);});
        }
        catch (...) { available.software_gi_consumer = false; }
    }
#else
    available.software_gi_consumer = false;
#endif
    available.adaptive_raster_consumer = available.adaptive_raster_consumer &&
        request.product_paths.contains("render/shader/adaptive_shading_classify_cs.dxil") &&
        request.product_paths.contains("render/shader/adaptive_shading_rate_cs.dxil");
    available.adaptive_compute_consumer = available.adaptive_compute_consumer &&
        request.product_paths.contains("render/shader/adaptive_shading_classify_cs.dxil") &&
        request.product_paths.contains("render/shader/adaptive_gtao_horizon_cs.dxil") &&
        request.product_paths.contains("render/shader/adaptive_gtao_resolve_cs.dxil");
    return ResolveRendererFeatures({request.ray_tracing_enabled, request.ao_mode,
        request.gi_mode, request.reflection_mode, request.shadow_mode, request.adaptive_shading}, available);
}
Result D3D12Renderer::Impl::PrepareLightList(const RendererConfig &candidate_config,
    ViewExtent capacity, LightListCandidate &candidate) const
{
    return light_list.Prepare(device.Get(), allocator, capacity, 128, kFrameCount,
        [&](std::string_view logical_id, std::vector<std::byte> &bytes) {
            std::filesystem::path path;
            if (auto resolved = candidate_config.ResolveProduct(logical_id, path); !resolved) return resolved;
            return ReadBinary(path, bytes);
        }, candidate, QualityProfileFor(candidate_config.lighting_quality).lighting_cluster_capacity);
}
Result D3D12Renderer::Impl::PrepareRayScene(const RendererConfig &candidate_config,
    RaySceneCandidate &candidate) const
{
    NativeRayPlan native;if(auto resolved=ResolveNativeRay(candidate_config,native);!resolved)return resolved;
    if(native.selection.use_omms&&native.budget_bytes>UINT64_MAX-ray_scene.AllocatedBytes())
        return Result::Failure(ErrorCode::InvalidState,"hs_renderer_d3d12","Native ray scene budget overflow.");
    return ray_scene.Prepare(device.Get(), allocator,
        ResolveFeatures(candidate_config).ray_scene_enabled, kFrameCount, candidate,enhanced,native.selection.use_omms,
        native.selection.use_omms?native.budget_bytes+ray_scene.AllocatedBytes():UINT64_MAX);
}
Result D3D12Renderer::Impl::PrepareVirtualShadows(const RendererConfig &candidate_config,
    ViewExtent capacity, VirtualShadowCandidate &candidate) const
{
    constexpr std::array products{"render/shader/virtual_shadow_init_cs.dxil",
        "render/shader/virtual_shadow_feedback_cs.dxil", "render/shader/virtual_shadow_resident_cs.dxil",
        "render/shader/virtual_shadow_cull_cs.dxil", "render/shader/virtual_shadow_publish_cs.dxil",
        "render/shader/virtual_shadow_clear_vs.dxil", "render/shader/virtual_shadow_clear_ps.dxil"};
    VirtualShadowProfile profile;
    profile.enabled = (candidate_config.shadow_mode == GraphicsShadowMode::Raster ||
        candidate_config.shadow_mode == GraphicsShadowMode::HybridRT) &&
        std::ranges::all_of(products, [&](const auto id) { return candidate_config.product_paths.contains(id); });
    constexpr std::uint64_t mib = 1024ull * 1024ull;
    const auto cache_bytes = UINT64(QualityProfileFor(candidate_config.shadow_quality).shadow_cache_mib) * mib;
    // Two D32 banks with guarded 132x132 tiles. A rectangular native atlas can
    // add unused tiles, so use complete square banks within the authored limit.
    auto columns = std::min<UINT>(D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION / kVirtualShadowTile,
        static_cast<UINT>(std::floor(std::sqrt(double(cache_bytes / (2ull * kVirtualShadowTile * kVirtualShadowTile * sizeof(float)))))));
    while (columns)
    {
        D3D12_RESOURCE_DESC bank{};
        bank.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        bank.Width = bank.Height = columns * kVirtualShadowTile;
        bank.DepthOrArraySize = bank.MipLevels = 1;
        bank.Format = DXGI_FORMAT_R32_TYPELESS; bank.SampleDesc = {1, 0};
        bank.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        const auto allocation = device->GetResourceAllocationInfo(0, 1, &bank);
        if (allocation.SizeInBytes != UINT64_MAX && allocation.SizeInBytes <= cache_bytes / 2) break;
        --columns;
    }
    if (!columns)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Authored shadow cache cannot admit one native guarded bank pair.");
    profile.physical_pages = columns * columns;
    profile.cache_budget_bytes = cache_bytes;
    DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
    const auto queried = adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory);
    if (FAILED(queried)) return HResultFailure("Query virtual shadow admission budget", queried);
    profile.gpu_budget_bytes = memory.Budget > memory.CurrentUsage ? memory.Budget - memory.CurrentUsage : 0;
    profile.filter_samples = QualityProfileFor(candidate_config.shadow_quality).shadow_filter_samples;
    profile.max_casters = 65536;
    profile.max_receivers = 256;
    profile.max_draw_groups = 128;
    profile.command_capacity = 65536;
    profile.frame_slots = kFrameCount;
    profile.virtual_sides = {32, 8, 8};
    profile.render_capacity = capacity;
    return virtual_shadows.Prepare(device.Get(), allocator, profile,
        [candidate_config](std::string_view logical_id, std::vector<std::byte> &bytes) {
            std::filesystem::path path;
            if (auto resolved = candidate_config.ResolveProduct(logical_id, path); !resolved) return resolved;
            return ReadBinary(path, bytes);
        }, candidate, &pipeline_cache);
}
Result D3D12Renderer::Impl::PrepareHdrPost(const RendererConfig &candidate_config,
    ViewExtent render_capacity, ViewExtent output_extent, HdrPostCandidate &candidate) const
{
    HdrPostSettings settings;
    settings.motion_blur = candidate_config.motion_blur;
    settings.depth_of_field = candidate_config.depth_of_field;
    settings.color_grading = candidate_config.color_grading;
    return hdr_post.Prepare(device.Get(), allocator, render_capacity, output_extent, settings,
        kFrameCount, [&](std::string_view logical_id, std::vector<std::byte> &bytes) {
            std::filesystem::path path;
            if (auto resolved = candidate_config.ResolveProduct(logical_id, path); !resolved) return resolved;
            return ReadBinary(path, bytes);
        }, candidate);
}
Result D3D12Renderer::Impl::PrepareAdditionalShadows(const RendererConfig &candidate_config,
    ViewExtent capacity, AdditionalShadowPrepared &candidate) const
{
    candidate = {};
    const auto shader_reader = [candidate_config](std::string_view id, std::vector<std::byte> &bytes) {
        std::filesystem::path path;
        if (auto resolved = candidate_config.ResolveProduct(id, path); !resolved) return resolved;
        return ReadBinary(path, bytes);
    };
    constexpr std::uint64_t mib = 1024ull * 1024ull;
    LocalShadowProfile local;
    local.enabled = true;
    const auto &quality = QualityProfileFor(candidate_config.shadow_quality);
    local.filter_samples = quality.shadow_filter_samples;
    local.maximum_lights = quality.local_shadow_max_lights;
    local.face_resolution = quality.local_shadow_resolution;
    local.area_samples = quality.local_shadow_area_samples;
    local.byte_budget = UINT64(quality.local_shadow_budget_mib) * mib;
    local.max_casters = 65536; local.max_draw_groups = 128; local.command_capacity = 65536;
    local.frame_slots = kFrameCount;
    if (auto prepared = local_shadows.Prepare(device.Get(), allocator, local, shader_reader,
            candidate.local, &pipeline_cache); !prepared) return prepared;
    RayShadowProfile ray;
    ray.enabled = ResolveFeatures(candidate_config).shadow_mode == GraphicsShadowMode::HybridRT;
    const UINT contexts = quality.ray_shadow_max_lights;
    const UINT64 pixels = UINT64(capacity.width) * capacity.height;
    const UINT64 trace_pixels = UINT64(std::ceil(capacity.width * quality.rt_axis_ratio)) * UINT64(std::ceil(capacity.height * quality.rt_axis_ratio));
    // Cover the larger internal-history layout as well as native SIGMA. The
    // service counts actual allocation sizes against this candidate bound.
    ray.byte_budget = std::max<UINT64>(512 * mib, contexts * (56 * trace_pixels + 4 * pixels) + 64 * mib);
    ray.render_capacity = capacity;
    ray.quality = candidate_config.shadow_quality;
    ray.quality_profile = quality;
    ray.maximum_trace_distance_m = candidate_config.rt_trace_distance_m;
    ray.maximum_lights = kRayShadowMaximumLights; ray.frame_slots = kFrameCount;
    ray.denoiser = {candidate_config.ray_reconstruction_provider, candidate_config.neural_ray_reconstruction};
    return ray_shadows.Prepare(device.Get(), allocator, root_signature.Get(), ray, shader_reader, candidate.ray);
}
Result D3D12Renderer::Impl::CommitAdditionalShadows(AdditionalShadowPrepared &&candidate)
{
    const auto completed = graph_queue_executor.Completed();
    if (auto result = local_shadows.Commit(std::move(candidate.local), completed); !result) return result;
    return ray_shadows.Commit(std::move(candidate.ray), completed);
}
Result D3D12Renderer::Impl::PrepareRayEffects(const RendererConfig &candidate_config,
    ViewExtent capacity, RayGeometryCandidate &geometry, RayAmbientOcclusionCandidate &ambient,
    RayTransportCandidate &transport, RayReconstructionMode mode) const
{
    const auto plan = ResolveFeatures(candidate_config);
    NativeRayPlan native;if(auto resolved=ResolveNativeRay(candidate_config,native);!resolved)return resolved;
    const auto read_shader = [candidate_config,selection=native.selection](std::string_view id, std::vector<std::byte> &bytes) {
        std::filesystem::path path;
        if (auto resolved = candidate_config.ResolveProduct(id, path); !resolved) return resolved;
        if(auto read=ReadBinary(path,bytes);!read)return read;
        if(selection.use_omms||selection.use_ser)
        {
            try{const auto digest=candidate_config.product_digests.find(std::string(id));
                if(digest==candidate_config.product_digests.end()||render_data::Sha256(bytes)!=RayDigest(digest->second))
                    return Result::Failure(ErrorCode::InvalidState,"hs_renderer_d3d12","Selected native ray shader digest changed: "+std::string(id));}
            catch(const std::exception &error){return Result::Failure(ErrorCode::InvalidState,"hs_renderer_d3d12",error.what());}
        }
        return Result::Success();
    };
    if (auto prepared = ray_geometry.Prepare(device.Get(), allocator, root_signature.Get(),
            plan.canonical_geometry_enabled, kFrameCount, read_shader, geometry); !prepared) return prepared;
    if (auto prepared = ray_ambient_occlusion.Prepare(device.Get(), allocator, root_signature.Get(), capacity,
        candidate_config.ao_quality, QualityProfileFor(candidate_config.ao_quality), plan.ao_mode == GraphicsAoMode::RTAO,
        kFrameCount, read_shader, ambient,
        {candidate_config.ray_reconstruction_provider, candidate_config.neural_ray_reconstruction}); !prepared) return prepared;
    return ray_transport.Prepare(device.Get(), allocator, root_signature.Get(),
        plan.gi_mode == GraphicsGiMode::RayTraced, plan.reflection_mode == GraphicsReflectionMode::HybridRT,
        candidate_config.gi_quality, candidate_config.reflection_quality, *quality_profiles, capacity, kFrameCount,
        read_shader, transport, &pipeline_cache,
        {candidate_config.ray_reconstruction_provider, candidate_config.neural_ray_reconstruction},
        mode == RayReconstructionMode::FsrStandalone ? RaySignalConsumer::FsrStandalone :
        mode == RayReconstructionMode::DlssFused ? RaySignalConsumer::DlssFused : RaySignalConsumer::Existing,
        native.selection,native.budget_bytes,0);
}
Result D3D12Renderer::Impl::PrepareLightingProbe(const RendererConfig &candidate_config,
    LightingProbeCandidate &candidate) const
{
    std::filesystem::path path;
    if (auto resolved = candidate_config.ResolveProduct("render/lighting/default_probe.rpkg", path); !resolved)
        return resolved;
    std::vector<std::byte> bytes;
    if (auto read = ReadBinary(path, bytes); !read) return read;
    return lighting_probe.Prepare(device.Get(), allocator, bytes, candidate);
}
Result D3D12Renderer::Impl::PrepareLtcLut(const RendererConfig &candidate_config,
    LtcLutCandidate &candidate) const
{
    std::filesystem::path path;
    if (auto resolved = candidate_config.ResolveProduct("render/lighting/default_ltc.rpkg", path); !resolved)
        return resolved;
    std::vector<std::byte> bytes;
    if (auto read = ReadBinary(path, bytes); !read) return read;
    return ltc_lut.Prepare(device.Get(), allocator, bytes, candidate);
}
Result D3D12Renderer::Impl::PrepareAdaptiveShading(const RendererConfig &candidate_config,
    ViewExtent capacity, AdaptiveShadingCandidate &candidate) const
{
    const auto plan = ResolveFeatures(candidate_config);
    AdaptiveShadingProfile profile;
    profile.raster_enabled = plan.adaptive_raster_enabled;
    profile.compute_enabled = plan.adaptive_compute_enabled;
    profile.render_capacity = capacity;
    profile.frame_slots = kFrameCount;
    profile.maximum_materials = static_cast<std::uint32_t>(kResidentMaterialCount);
    if (profile.raster_enabled || profile.compute_enabled)
    {
        DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
        const auto queried = adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory);
        if (FAILED(queried)) return HResultFailure("Query adaptive shading native byte budget", queried);
        const auto available = memory.Budget > memory.CurrentUsage ? memory.Budget - memory.CurrentUsage : 0;
        // CurrentUsage already includes live storage; candidate allocations must
        // fit the observed free budget while the old completed service survives.
        profile.byte_budget = std::min(profile.byte_budget, available);
        if (!profile.byte_budget)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "No native memory budget remains for adaptive shading preparation.");
    }
    return adaptive_shading.Prepare(device.Get(), allocator, profile,
        [&](std::string_view id, std::vector<std::byte> &bytes) {
            std::filesystem::path path;
            if (auto resolved = candidate_config.ResolveProduct(id, path); !resolved) return resolved;
            return ReadBinary(path, bytes);
        }, candidate);
}
Result D3D12Renderer::Impl::BuildAdaptiveMaterialPolicies(std::vector<AdaptiveShadingMaterialPolicy> &policies) const
{
    policies.clear();
    try
    {
        policies.reserve(kResidentMaterialCount);
        constexpr auto basic = static_cast<std::uint32_t>(render_data::MaterialClosureCapability::Basic);
        for (std::size_t ordinal = 0; ordinal < kAuthoredMaterialNames.size(); ++ordinal)
        {
            bool eligible{};
            const auto &header = material_headers[ordinal];
            const auto metadata = std::ranges::find(header.view.chunks, render_data::ChunkType::MaterialMetadata,
                &render_data::ChunkView::type);
            if (metadata != header.view.chunks.end())
            {
                const auto material = render_data::DecodeRockMaterial(metadata->bytes, header.view.dependencies);
                // Cooked alpha coverage and actual high-frequency authored
                // detail/height/histogram domains stay full rate. Basic base,
                // roughness and normal texture variation is checked per pixel.
                eligible = material_surface_layouts[ordinal].capability_mask == basic &&
                    !material.alpha_mask && !material.two_sided && material.textures[3].empty() &&
                    material.textures[4].empty() && material.textures[5].empty();
            }
            // This exact renderer profile replaces terrain by the unlayered
            // analytic grid. Its actual line/normal/depth edges remain protected
            // by the pixel+halo classifier; authored terrain never inherits it.
            if (ordinal == kTerrainMaterialOrdinal && config.grid_floor_probe) eligible = true;
            policies.push_back({0x10000000u + static_cast<std::uint32_t>(ordinal), eligible ? 1u : 0u});
        }
        for (std::size_t hero = 0; hero < kHeroMaterialNames.size(); ++hero)
            // The current hero package ABI supplies closure/domain bindings but
            // no explicit opacity/frequency policy. A masked opaque binding or
            // advanced closure must never be inferred eligible from its ID.
            policies.push_back({render_data::kHeroMaterialIdBase + static_cast<std::uint32_t>(hero), 0u});
        std::ranges::sort(policies, {}, &AdaptiveShadingMaterialPolicy::material_id);
        return Result::Success();
    }
    catch (const std::exception &error)
    { return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", error.what()); }
}
Result D3D12Renderer::Impl::PrepareAmbientOcclusion(const RendererConfig &candidate_config,
    ViewExtent capacity, AmbientOcclusionCandidate &candidate, bool adaptive_compute) const
{
    return ambient_occlusion.Prepare(device.Get(), allocator, capacity,
        candidate_config.ao_quality, QualityProfileFor(candidate_config.ao_quality), ResolveFeatures(candidate_config).ao_mode == GraphicsAoMode::GTAO,
        candidate_config.contact_shadows, kFrameCount,
        [&](std::string_view logical_id, std::vector<std::byte> &bytes) {
            std::filesystem::path path;
            if (auto resolved = candidate_config.ResolveProduct(logical_id, path); !resolved) return resolved;
            return ReadBinary(path, bytes);
        }, candidate, adaptive_compute);
}
Result D3D12Renderer::Impl::PrepareSurfaceLighting(const RendererConfig &candidate_config,
    ViewExtent capacity, SubsurfaceCandidate &diffusion, ScreenReflectionsCandidate &reflections) const
{
    const auto read_shader = [&](std::string_view logical_id, std::vector<std::byte> &bytes) {
        std::filesystem::path path;
        if (auto resolved = candidate_config.ResolveProduct(logical_id, path); !resolved) return resolved;
        return ReadBinary(path, bytes);
    };
    const bool have_subsurface = std::ranges::any_of(material_surface_layouts, [](const auto &layout) {
        return (layout.capability_mask & static_cast<std::uint32_t>(
            render_data::MaterialClosureCapability::Subsurface)) != 0;
    });
    if (auto prepared = subsurface.Prepare(device.Get(), allocator, capacity, have_subsurface,
            kFrameCount, read_shader, diffusion); !prepared) return prepared;
    const bool screen_space = ResolveFeatures(candidate_config).reflection_mode == GraphicsReflectionMode::ScreenSpace;
    const auto quality = screen_space ? static_cast<GraphicsOptionalQuality>(
        static_cast<unsigned>(candidate_config.reflection_quality) + 1u) : GraphicsOptionalQuality::Off;
    return screen_reflections.Prepare(device.Get(), allocator, capacity, quality, QualityProfileFor(candidate_config.reflection_quality),
        kFrameCount, read_shader, reflections);
}
Result D3D12Renderer::Impl::PrepareHeroTransparency(const RendererConfig &candidate_config,
    ViewExtent capacity, HeroTransparencyCandidate &candidate) const
{
    const bool authored_transmission = std::ranges::any_of(material_surface_layouts, [](const auto &layout) {
        return (layout.capability_mask & static_cast<std::uint32_t>(
            render_data::MaterialClosureCapability::SolidTransmission)) != 0;
    });
    const auto row = candidate_config.refraction_quality == GraphicsOptionalQuality::Off
        ? GraphicsQuality::Low : static_cast<GraphicsQuality>(static_cast<unsigned>(candidate_config.refraction_quality) - 1u);
    const auto layers = candidate_config.refraction_quality == GraphicsOptionalQuality::Off ? 0u :
        std::min(QualityProfileFor(row).hero_refraction_layers,
            QualityProfileFor(candidate_config.transparency_quality).hero_refraction_layers);
    const bool enabled = authored_transmission && layers != 0;
    return hero_transparency.Prepare(device.Get(), allocator, root_signature.Get(), capacity,
        layers, enabled, kFrameCount,
        [&](std::string_view logical_id, std::vector<std::byte> &bytes) {
            std::filesystem::path path;
            if (auto resolved = candidate_config.ResolveProduct(logical_id, path); !resolved) return resolved;
            return ReadBinary(path, bytes);
        }, candidate);
}
} // namespace hs
