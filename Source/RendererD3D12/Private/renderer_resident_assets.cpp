#include "renderer_impl.hpp"
#include "renderer_development_catalog.hpp"
#include "renderer_resident_assets.hpp"

#include <hs/render_data/geometry_hierarchy.hpp>
#include <hs/render_data/render_asset_format.hpp>
#include <hs/render_data/scene_hlod.hpp>

#include <algorithm>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace hs
{
namespace
{
Result ResidentFailure(std::string message)
{
    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", std::move(message));
}

Result ReadResidentPackageDirectory(const std::filesystem::path &path, std::uint64_t product_bytes,
    AssetId asset, render_data::PackageDirectory &directory)
{
    constexpr std::size_t kMaxPrefix = 16 * 1024;
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() < 0 || static_cast<std::uint64_t>(input.tellg()) != product_bytes)
        return ResidentFailure("Resident package changed before directory read: " + path.string());
    std::vector<std::byte> prefix(static_cast<std::size_t>(std::min<std::uint64_t>(kMaxPrefix, product_bytes)));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char *>(prefix.data()), static_cast<std::streamsize>(prefix.size())))
        return ResidentFailure("Resident package directory read failed: " + path.string());
    try { directory = render_data::ReadPackageDirectory(prefix, product_bytes, asset); }
    catch (const std::exception &error)
    {
        return ResidentFailure("Invalid resident package directory " + path.string() + ": " + error.what());
    }
    return Result::Success();
}

// Resolve all actual resident/streaming dependencies before any GPU allocation.
// Source identities in package dependency lists are resolved as Generation
// products; their loose authoring files are never opened by this preparation.
Result ResolveResidentClosure(const RendererConfig &config,
                              ResidentPreparationAccounting &accounting)
{
    try
    {
        // Compiled ranges are admitted once; actual consumers enforce their read/decode budgets.
        // Do not reopen and decode the complete shipping dependency graph as a preflight.
        if (config.packed_content)
        {
            if (auto result = ValidatePackedRendererAssets(*config.packed_content, config.packed_bulks); !result) return result;
            if (auto result = ValidatePackedTextureCatalog(config); !result) return result;
            if (auto result = ValidatePackedGeometryCatalog(config); !result) return result;
            if (auto result = ValidatePackedMaterialCatalog(config); !result) return result;
            if (auto result = ValidatePackedCharacterCatalog(config); !result) return result;
            if (auto result = ValidatePackedVfxMeshCatalog(config); !result) return result;
            return ValidatePackedRevisionCatalog(config);
        }
        if (config.development_catalog)
        {
            if (auto result = ValidatePackedTextureCatalog(config); !result) return result;
            if (auto result = ValidatePackedGeometryCatalog(config); !result) return result;
            if (auto result = ValidatePackedMaterialCatalog(config); !result) return result;
            if (auto result = ValidatePackedCharacterCatalog(config); !result) return result;
            // Loose products are staged at their actual reader, and shadow archives
            // may be larger than a single bounded range request.
            return Result::Success();
        }
        if (config.content_generation_id.empty() || !config.content_generation_lease || config.product_paths.empty() ||
            config.product_paths.size() != config.product_digests.size())
            return ResidentFailure("Resident preparation requires complete leased Generation product maps.");
        std::vector<std::string> pending;
        const auto texture = [&](std::string_view name) {
            pending.push_back("render/texture/" + std::string(name) + ".rpkg");
        };
        for (const auto name : kEnvironmentMaterialNames)
            for (std::uint32_t lod = 0; lod < 3; ++lod)
                pending.push_back("render/" + std::string(name) +
                    (lod ? "_lod" + std::to_string(lod) : std::string{}) + ".rpkg");
        for (const auto name : kAuthoredMaterialNames)
            pending.push_back(render_data::MaterialProduct(name));
        for (const auto name : kAuthoredMaterialNames)
            pending.push_back("render/material/" + std::string(name) + "/parameters.rpkg");
        for (const auto name : kHeroMaterialNames)
        {
            pending.push_back("render/material/" + std::string(name) + "/material.rpkg");
            pending.push_back("render/material/" + std::string(name) + "/parameters.rpkg");
        }
        for (const auto name : render_data::kDecalMaterialNames)
        {
            pending.push_back("render/material/" + std::string(name) + "/material.rpkg");
            pending.push_back("render/material/" + std::string(name) + "/parameters.rpkg");
        }
        for (const std::string_view name : {"archer", "enemy_melee", "enemy_ranged", "enemy_suicide",
                "boss_5m", "boss_10m", "boss_final", "enemy_gel_projectile"})
            pending.push_back("render/character/" + std::string(name) + ".rpkg");
        pending.emplace_back("render/animation/archer.rpkg");
        for (const std::string_view name : {"monster_basecolor", "monster_emissive", "monster_ram",
                "enemy_melee_diffuse_0", "enemy_ranged_diffuse_0", "enemy_suicide_diffuse_0",
                "enemy_melee_normal_0", "enemy_ranged_normal_0", "enemy_suicide_normal_0"})
            texture(name);
        for (const std::string_view name : {"terrain_gaussian", "terrain_surface", "terrain_normal", "terrain_detail", "terrain_height",
                "rock_basecolor", "rock_surface", "rock_normal", "rock_detail", "bark_basecolor",
                "bark_surface", "bark_normal", "bark_detail", "grass_basecolor", "grass_normal",
                "grass_roughness", "leaf_basecolor", "leaf_normal", "leaf_roughness", "terrain_histogram"})
            texture("environment_" + std::string(name));
        for (const std::string_view name : {"vfx_masks", "vfx_gradient_lut", "vfx_ribbon_detail_array", "vfx_authored_mask_array",
                "vfx_noise_basis_2d", "vfx_curve_lut", "vfx_smoke6way_pos", "vfx_smoke6way_neg",
                "vfx_smoke_motion_vectors", "vfx_flow_curl_2d", "vfx_stbn_scalar", "vfx_fracture_decal_array",
                "vfx_organic_decal_array", "vfx_noise_volume_64"})
            texture(name);
        pending.emplace_back("vfx/vfx_mesh_atlas.hsbin");
        if (!config.scene_hlod_product_id.empty()) pending.push_back(config.scene_hlod_product_id);
        std::unordered_set<std::string> resolved;
        for (std::size_t next = 0; next < pending.size(); ++next)
        {
            // Appending dependencies can reallocate pending; retain the identity.
            const auto id = pending[next];
            if (!resolved.insert(id).second) continue;
            if (RendererIsSourceRevision(config, id))
            {
                const auto digest = RendererProductDigest(config, id);
                if (digest.size() != 71 || !digest.starts_with("sha256:") ||
                    !std::ranges::all_of(std::string_view(digest).substr(7), [](char value) {
                        return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
                    }))
                    return ResidentFailure("Invalid source revision identity: " + id);
                continue;
            }
            if (!RendererIsAdmittedRuntimeProduct(config, id))
                return ResidentFailure("Dependency is absent from the admitted runtime and provenance catalogs: " + id);
            std::filesystem::path path;
            if (auto result = config.ResolveProduct(id, path); !result) return result;
            const auto digest = config.product_digests.find(id);
            if (!path.is_absolute() || digest == config.product_digests.end() ||
                digest->second.size() != 71 || !digest->second.starts_with("sha256:") ||
                !std::ranges::all_of(std::string_view(digest->second).substr(7), [](char value) {
                    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
                }))
                return ResidentFailure("Invalid immutable Generation product identity: " + id);
            const bool package = id.ends_with(".rpkg") || id.ends_with(".rpage");
            const bool hierarchy = id.ends_with(".ghbin");
            const bool hlod = id == config.scene_hlod_product_id;
            const bool atlas = id == "vfx/vfx_mesh_atlas.hsbin";
            if (!package && !hierarchy && !hlod && !atlas) continue;
            std::error_code error;
            const auto file_bytes = std::filesystem::file_size(path, error);
            if (error || !file_bytes || file_bytes > accounting.host_staging_budget_bytes)
                return ResidentFailure("Resident product exceeds the observed host staging budget: " + id);
            if (file_bytes > UINT64_MAX - accounting.closure_payload_bytes)
                return ResidentFailure("Resident closure payload size overflow.");
            accounting.closure_payload_bytes += file_bytes;
            accounting.largest_payload_bytes = std::max(accounting.largest_payload_bytes, file_bytes);
            if (package)
            {
                render_data::PackageDirectory directory;
                if (auto result = ReadResidentPackageDirectory(path, file_bytes, MakeAssetId(id), directory); !result)
                    return result;
                pending.insert(pending.end(), directory.dependencies.begin(), directory.dependencies.end());
                if (id == "render/character/archer.rpkg")
                {
                    const auto bindings = std::ranges::find(directory.chunks,
                        render_data::ChunkType::CharacterMaterialBindings, &render_data::PackageDirectoryChunk::type);
                    if (bindings == directory.chunks.end() || !bindings->count)
                        return ResidentFailure("Archer package lacks typed character material bindings.");
                    for (std::uint64_t material = 0; material < bindings->count; ++material)
                    {
                        texture("archer_diffuse_" + std::to_string(material));
                        texture("archer_normal_" + std::to_string(material));
                    }
                }
            }
            else if (hierarchy || hlod)
            {
                std::vector<std::byte> bytes;
                // Runtime has verified the complete immutable leased Generation.
                if (auto result = ReadBinary(path, bytes, file_bytes); !result) return result;
                if (hierarchy)
                {
                    for (const auto &page : render_data::DecodeGeometryHierarchy(bytes).pages)
                        pending.push_back(page.logical_id);
                }
                else
                {
                    const auto value = render_data::DecodeSceneHlod(bytes);
                    pending.insert(pending.end(), value.dependencies.begin(), value.dependencies.end());
                    for (const auto &page : value.pages) pending.push_back(page.logical_id);
                }
            }
        }
        return Result::Success();
    }
    catch (const std::exception &error)
    {
        return ResidentFailure("Invalid resident Generation closure: " + std::string(error.what()));
    }
}

bool SubmissionFinished(const ResidentUtilitySubmission &submission) noexcept
{
    // GetCompletedValue returns UINT64_MAX for device removal, not a timeline
    // completion. Query the device explicitly to prove that terminal condition.
    if (submission.device && FAILED(submission.device->GetDeviceRemovedReason())) return true;
    if (!submission.signaled || !submission.fence) return false;
    const auto completed = submission.fence->GetCompletedValue();
    return completed != UINT64_MAX && completed >= submission.fence_value;
}
} // namespace

Result D3D12Renderer::Impl::InitializeResidentAssetContext(const RendererConfig &candidate_config,
                                                        std::unique_ptr<Impl> &candidate)
{
    if (candidate || !device || !adapter || !queue || !allocator)
        return ResidentFailure("Resident preparation requires an empty candidate and the active native device.");
    // Publish ownership immediately. Any later submitted preparation failure is
    // retained by the caller, rather than losing the candidate on stack unwind.
    candidate = std::make_unique<Impl>();
    auto &next = *candidate;
    next.config = candidate_config;
    if (auto loaded = next.LoadQualityProfiles(candidate_config); !loaded) return loaded;
    next.config.shadow_resolution = next.QualityProfileFor(candidate_config.shadow_quality).shadow_resolution;
    const auto particle_quality = last_applied_graphics_request_available ? last_applied_graphics_request.particle_quality :
        candidate_config.requested_graphics ? candidate_config.requested_graphics->particle_quality : GraphicsQuality::High;
    next.config.particle_percentage = static_cast<std::uint32_t>(std::lround(100.0f *
        next.QualityProfileFor(particle_quality).decorative_particle_fraction));
    next.resource_generation_id = candidate_config.content_generation_id;
    next.resident_generation_lease = candidate_config.content_generation_lease;
    next.resident_development_catalog = candidate_config.development_catalog;
    next.device = device;
    next.adapter = adapter;
    next.queue = queue;
    next.resident_allocator_lease = allocator;
    next.allocator = next.resident_allocator_lease.Get();
    next.enhanced = enhanced;
    next.mesh_shader_supported = mesh_shader_supported;
    next.mesh_shader_enabled = mesh_shader_enabled;
    next.feature_capabilities = feature_capabilities;
    auto candidate_main = std::make_unique<OwnedView>();
    auto &candidate_view = *candidate_main;
    const auto &active_main = *owned_views.at(kMainRenderViewId.value);
    candidate_view.view_id = kMainRenderViewId;
    candidate_view.role = RenderViewRole::Main;
    candidate_view.state = active_main.state;
    candidate_view.display_state = active_main.display_state;
    candidate_view.width = active_main.width;
    candidate_view.height = active_main.height;
    candidate_view.render_width = active_main.render_width;
    candidate_view.render_height = active_main.render_height;
    candidate_view.native_viewport_id = active_main.native_viewport_id;
    next.owned_views.emplace(kMainRenderViewId.value, std::move(candidate_main));
    next.active_view_id = kMainRenderViewId;
    next.dred_enabled = dred_enabled;
    next.resident_preparation_accounting.enabled = true;
    D3D12_FEATURE_DATA_ARCHITECTURE1 architecture{};
    auto hr = device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1, &architecture, sizeof(architecture));
    if (FAILED(hr)) return HResultFailure("Query resident preparation memory architecture", hr);
    next.resident_preparation_accounting.unified_memory = architecture.UMA != FALSE;
    if (auto result = next.UpdateResidencyBudget(); !result) return result;
    const auto host_budget = next.residency.Accounting().budget;
    if (!host_budget.host_budget_valid)
        return ResidentFailure("Resident preparation requires an observed host staging budget.");
    next.resident_preparation_accounting.host_staging_budget_bytes = host_budget.host_staging_budget;
    if (auto result = ResolveResidentClosure(next.config, next.resident_preparation_accounting); !result) return result;
    if (auto result = next.LoadMaterialSurfaceProfile(next.config); !result) return result;
    if (auto result = next.UpdateResidencyBudget(); !result) return result;
    if (!next.residency.CanStage(next.resident_preparation_accounting.largest_payload_bytes))
        return ResidentFailure("Resident closure exceeds the host staging budget.");
    for (const auto &header : next.material_headers)
        next.resident_preparation_accounting.retained_host_bytes += header.bytes.capacity();
    if (auto result = next.pipeline_cache.Initialize(device.Get(), adapter.Get(), {}); !result) return result;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&next.frames[0].allocator));
    if (FAILED(hr)) return HResultFailure("Create resident preparation allocator", hr);
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, next.frames[0].allocator.Get(), nullptr,
        IID_PPV_ARGS(&next.command_list));
    if (FAILED(hr)) return HResultFailure("Create resident preparation list", hr);
    if (enhanced)
    {
        hr = next.command_list.As(&next.enhanced_command_list);
        if (FAILED(hr)) return HResultFailure("Query resident enhanced preparation list", hr);
    }
    hr = next.command_list->Close();
    if (FAILED(hr)) return HResultFailure("Close resident preparation list", hr);
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&next.fence));
    if (FAILED(hr)) return HResultFailure("Create resident preparation fence", hr);
    next.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!next.fence_event) return HResultFailure("Create resident preparation event", HRESULT_FROM_WIN32(GetLastError()));
    next.resident_fence_event_owner = std::shared_ptr<void>(next.fence_event, [](void *event) { CloseHandle(event); });
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = kFrameCount * kTextureDescriptorCount;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&next.srv_heap));
    if (FAILED(hr)) return HResultFailure("Create resident preparation descriptor heap", hr);
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    hr = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&next.srv_cpu_heap));
    if (FAILED(hr)) return HResultFailure("Create resident canonical CPU descriptor heap", hr);
    next.InitializeCanonicalDescriptors(next.srv_cpu_heap.Get());
    next.srv_stride = srv_stride;
    next.rtv_stride = rtv_stride;
    next.dsv_stride = dsv_stride;
    next.sampler_stride = sampler_stride;
    const auto seed = [](GeometryPageGpu &target, const GeometryPageGpu &previous) -> Result {
        if (previous.generation == UINT32_MAX) return ResidentFailure("Resident geometry generation exhausted.");
        target.generation = previous.generation + 1u;
        return Result::Success();
    };
    if (auto result = seed(next.archer_geometry, archer_geometry); !result) return result;
    if (auto result = seed(next.gel_projectile_geometry, gel_projectile_geometry); !result) return result;
    if (auto result = seed(next.procedural_geometry, procedural_geometry); !result) return result;
    for (std::size_t index = 0; index < monster_assets.size(); ++index)
        if (auto result = seed(next.monster_assets[index], monster_assets[index]); !result) return result;
    for(std::size_t asset=0;asset<monster_reduced_geometry.size();++asset)
        for(std::size_t lod=0;lod<monster_reduced_geometry[asset].size();++lod)
            if(auto result=seed(next.monster_reduced_geometry[asset][lod],
                monster_reduced_geometry[asset][lod]);!result) return result;
    for (std::size_t index = 0; index < environment_meshes.size(); ++index)
        if (auto result = seed(next.environment_meshes[index], environment_meshes[index]); !result) return result;
    candidate_view.previous_geometry_page_versions = cluster_runtime ? cluster_runtime->physical_versions :
        active_main.previous_geometry_page_versions;
    if (auto result = next.PrepareMaterialParameters(next.config, false); !result) return result;
    next.CommitMaterialParameters();
    return Result::Success();
}

Result D3D12Renderer::Impl::PrepareResidentAssetData(Impl &candidate)
{
    if (!candidate.resident_preparation_accounting.enabled || candidate.resource_generation_id.empty() ||
        candidate.device.Get() != device.Get() || candidate.allocator != allocator ||
        candidate.srv_heap.Get() == srv_heap.Get() || !candidate.srv_cpu_heap ||
        candidate.srv_cpu_heap.Get() == srv_cpu_heap.Get())
        return ResidentFailure("Resident asset preparation requires the isolated active-device context.");
    if (auto result = candidate.CreateGpuData(); !result) return result;
    if (auto result = candidate.CreateCharacterTextures(); !result) return result;
    if (auto result = candidate.CreateEnvironmentTextures(); !result) return result;
    if (auto result = candidate.CreateEnvironmentMeshes(); !result) return result;
    // Environment textures rewrite the legacy t4/space2 slot. The prepared
    // BRDF LUT owns that slot in the current scene pipeline.
    candidate.WriteEnvironmentBrdfLutDescriptors();
    if (cluster_runtime && cluster_runtime->generation == UINT32_MAX)
        return ResidentFailure("Resident cluster generation exhausted.");
    const auto generation = cluster_runtime ? cluster_runtime->generation + 1u : 1u;
    if (auto result = candidate.CreateClusterResources(generation); !result) return result;
    for (std::uint32_t frame = 0; frame < kFrameCount; ++frame)
        candidate.WriteGeometryPageDescriptors(frame);
    // Account retained CPU payloads after transient read/decode vectors have
    // been released. Native upload storage is counted separately above.
    std::uint64_t host_bytes{};
    const auto add = [&](std::uint64_t bytes) {
        if (bytes > UINT64_MAX - host_bytes) throw std::overflow_error("Resident host payload size overflow.");
        host_bytes += bytes;
    };
    const auto vector = [&](const auto &values) { add(values.capacity() * sizeof(values.front())); };
    const auto text = [&](const auto &value) { add(value.size() * sizeof(value.front())); };
    try
    {
        for (const auto &header : candidate.material_headers)
        {
            vector(header.bytes); vector(header.view.chunks); vector(header.view.dependencies);
            for (const auto &dependency : header.view.dependencies) text(dependency);
        }
        const auto character = [&](const render_data::CharacterMetadataPackageData &data) {
            vector(data.clips); vector(data.parents); vector(data.inverse_bind_matrices); vector(data.transforms);
            vector(data.upper_body_weights); vector(data.material_bindings);
            if (data.monster) { vector(data.monster->final_matrices); vector(data.monster->lods); vector(data.monster->interval_bounds); }
        };
        character(candidate.archer_owned_character);
        vector(candidate.archer_packed_transforms); vector(candidate.archer_named_packed_transforms);
        vector(candidate.archer_support_vertices);
        for (const auto &monster : candidate.monster_assets)
        {
            vector(monster.owned_material_bindings); vector(monster.owned_geometry_lods); vector(monster.owned_interval_bounds);
            vector(monster.submeshes);text(monster.source_product);
        }
        for(const auto &asset:candidate.monster_reduced_geometry)
            for(const auto &page:asset) {vector(page.submeshes);text(page.source_product);}
        const auto &animation = candidate.archer_named_animations;
        vector(animation.bone_names); vector(animation.parents); vector(animation.inverse_bind_matrices);
        vector(animation.clips); vector(animation.transforms); vector(animation.sockets);
        for (const auto &name : animation.bone_names) text(name);
        for (const auto &clip : animation.clips) text(clip.name);
        for (const auto &socket : animation.sockets) text(socket.name);
        for (const auto &catalog : candidate.environment_cluster_catalogs)
        {
            const auto &hierarchy = catalog.Hierarchy();
            vector(hierarchy.groups); vector(hierarchy.clusters); vector(hierarchy.pages);
            vector(hierarchy.root_groups); vector(hierarchy.child_groups); vector(hierarchy.source_primitive_order);
            for (const auto &page : hierarchy.pages) text(page.logical_id);
        }
        const auto &cluster = *candidate.cluster_runtime;
        vector(cluster.cpu.groups); vector(cluster.cpu.children); vector(cluster.cpu.clusters); vector(cluster.cpu.pages);
        for (const auto &frame : cluster.frames) vector(frame.snapshot);
        if (candidate.hlod_runtime)
        {
            const auto &hlod = *candidate.hlod_runtime;
            vector(hlod.scene.dependencies); vector(hlod.scene.members); vector(hlod.scene.groups);
            vector(hlod.scene.proxies); vector(hlod.scene.pages); vector(hlod.scene.roots); vector(hlod.scene.children);
            for (const auto &dependency : hlod.scene.dependencies) text(dependency);
            for (const auto &page : hlod.scene.pages) text(page.logical_id);
        }
        vector(candidate.material_parameter_slots);
        vector(candidate.material_parameter_candidate_slots);
        candidate.resident_preparation_accounting.retained_host_bytes = host_bytes;
    }
    catch (const std::exception &error) { return ResidentFailure(error.what()); }
    return Result::Success();
}

Result D3D12Renderer::Impl::PrepareResidentPublication(Impl &candidate)
{
    if (!candidate.cluster_runtime || candidate.HasUnresolvedResidentUtilityUploads() ||
        candidate.material_parameter_upload_pending || candidate.environment_brdf_init_pending)
        return ResidentFailure("Resident publication requires a complete, reliably completed candidate.");
    try
    {
        // These callbacks are never called between preparation and publication.
        // Build them now, targeting their final stable live catalog addresses,
        // so the publication boundary itself cannot allocate std::function data.
        for (auto &stream : candidate.cluster_runtime->streams)
            if (stream.catalog < kClusterRuntimeCatalogs)
            {
                const auto *catalog = &environment_cluster_catalogs.at(stream.catalog);
                const auto local = stream.local_page;
                stream.decode = [catalog, local](std::span<const std::byte> bytes, render_data::GeometryData &data) {
                    return catalog->DecodePage(local, bytes, data);
                };
            }
        return Result::Success();
    }
    catch (const std::exception &error)
    {
        return ResidentFailure("Could not prepare resident decoder publication: " + std::string(error.what()));
    }
}

void D3D12Renderer::Impl::SwapResidentAssetData(Impl &candidate) noexcept
{
    // Publication has drained all queues. Release view banks with their actual
    // accounting owner before that manager and the shared catalog are swapped.
    ResetInactiveClusterViewResources();
    if (cluster_runtime)
        for (std::size_t slot = 0; slot < frames.size(); ++slot)
            if (auto &ticket = cluster_runtime->geometry_records_residency[slot]; ticket)
            {
                frames[slot].geometry_draw_records.Reset();
                static_cast<void>(residency.ReleaseNow(ticket));
                ticket = {};
            }
    using std::swap;
#define HS_SWAP_RESIDENT(field) swap(field, candidate.field)
    HS_SWAP_RESIDENT(archer_geometry);
    HS_SWAP_RESIDENT(gel_projectile_geometry);
    HS_SWAP_RESIDENT(procedural_geometry);
    HS_SWAP_RESIDENT(monster_assets);
    HS_SWAP_RESIDENT(monster_reduced_geometry);
    HS_SWAP_RESIDENT(monster_skin_matrices);
    HS_SWAP_RESIDENT(archer_clips);
    HS_SWAP_RESIDENT(archer_owned_character);
    HS_SWAP_RESIDENT(archer_compiled_animation);
    HS_SWAP_RESIDENT(archer_packed_transforms);
    HS_SWAP_RESIDENT(archer_named_packed_transforms);
    HS_SWAP_RESIDENT(archer_named_animations);
    HS_SWAP_RESIDENT(archer_parents);
    HS_SWAP_RESIDENT(archer_inverse_bind_matrices);
    HS_SWAP_RESIDENT(archer_transforms);
    HS_SWAP_RESIDENT(archer_upper_body_weights);
    HS_SWAP_RESIDENT(archer_support_vertices);
    HS_SWAP_RESIDENT(archer_bone_count);
    HS_SWAP_RESIDENT(archer_material_count);
    HS_SWAP_RESIDENT(archer_material_bindings);
    HS_SWAP_RESIDENT(archer_ground_offset);
    HS_SWAP_RESIDENT(archer_diffuse);
    HS_SWAP_RESIDENT(archer_normal);
    HS_SWAP_RESIDENT(monster_basecolor);
    HS_SWAP_RESIDENT(monster_emissive);
    HS_SWAP_RESIDENT(monster_ram);
    HS_SWAP_RESIDENT(family_diffuse);
    HS_SWAP_RESIDENT(family_normal);
    HS_SWAP_RESIDENT(vfx_masks);
    HS_SWAP_RESIDENT(vfx_sprite_count);
    HS_SWAP_RESIDENT(vfx_mesh_atlas);
    HS_SWAP_RESIDENT(vfx_mesh_vertex_counts);
    HS_SWAP_RESIDENT(vfx_mesh_transverse_radii);
    HS_SWAP_RESIDENT(vfx_mesh_longitudinal_bounds);
    HS_SWAP_RESIDENT(vfx_gradient);
    HS_SWAP_RESIDENT(vfx_gradient_rows);
    HS_SWAP_RESIDENT(ribbon_detail);
    HS_SWAP_RESIDENT(vfx_authored_mask);
    HS_SWAP_RESIDENT(vfx_noise_basis);
    HS_SWAP_RESIDENT(vfx_noise_volume);
    HS_SWAP_RESIDENT(vfx_smoke_pos);
    HS_SWAP_RESIDENT(vfx_smoke_neg);
    HS_SWAP_RESIDENT(vfx_smoke_motion);
    HS_SWAP_RESIDENT(vfx_flow_curl);
    HS_SWAP_RESIDENT(vfx_stbn_scalar);
    HS_SWAP_RESIDENT(vfx_fracture_decals);
    HS_SWAP_RESIDENT(vfx_organic_decals);
    HS_SWAP_RESIDENT(vfx_curve_lut);
    HS_SWAP_RESIDENT(environment_textures);
    HS_SWAP_RESIDENT(environment_texture_ids);
    HS_SWAP_RESIDENT(environment_texture_descriptions);
    HS_SWAP_RESIDENT(environment_material_textures);
    HS_SWAP_RESIDENT(environment_material_bindings);
    HS_SWAP_RESIDENT(environment_material_variants);
    HS_SWAP_RESIDENT(environment_meshes);
    HS_SWAP_RESIDENT(environment_variant_radii);
    HS_SWAP_RESIDENT(environment_cluster_catalogs);
    HS_SWAP_RESIDENT(environment_stream);
    HS_SWAP_RESIDENT(environment_geometry_streams);
    HS_SWAP_RESIDENT(retired_streaming_textures);
    HS_SWAP_RESIDENT(cluster_runtime);
    HS_SWAP_RESIDENT(hlod_runtime);
    HS_SWAP_RESIDENT(residency);
    HS_SWAP_RESIDENT(streaming);
    HS_SWAP_RESIDENT(material_headers);
    HS_SWAP_RESIDENT(material_surface_layouts);
    HS_SWAP_RESIDENT(material_sidecar_layout);
    HS_SWAP_RESIDENT(material_parameters);
    HS_SWAP_RESIDENT(material_parameter_slot_count);
    HS_SWAP_RESIDENT(material_parameter_slots);
    HS_SWAP_RESIDENT(material_parameter_candidate_slots);
    HS_SWAP_RESIDENT(material_parameter_bases);
    HS_SWAP_RESIDENT(material_parameter_layouts);
    HS_SWAP_RESIDENT(hero_material_bindings_gpu);
    HS_SWAP_RESIDENT(hero_binding_ranges);
    HS_SWAP_RESIDENT(environment_brdf_lut);
#undef HS_SWAP_RESIDENT
}

void D3D12Renderer::Impl::PublishResidentAssetDescriptors(const Impl &candidate) noexcept
{
    environment_descriptor_banks = {};
    struct Range { std::uint32_t first, count; };
    constexpr auto environment_first = kPostTextureDescriptorCount + kCharacterDescriptorCount + kMonsterPbrDescriptorCount;
    constexpr auto vfx_first = environment_first + kEnvironmentDescriptorCount;
    constexpr std::array ranges{
        Range{kPostTextureDescriptorCount, environment_first + kEnvironmentTextureCount - kPostTextureDescriptorCount},
        Range{vfx_first, kVfxDistortionDescriptorOffset - vfx_first},
        Range{kVfxDistortionDescriptorOffset + 1u, kBloomTextureDescriptorOffset - kVfxDistortionDescriptorOffset - 1u},
        Range{kMaterialTextureDescriptorOffset, kMaterialTextureDescriptorCount},
        Range{kGeometryDescriptorOffset + 1u, kGeometryDescriptorCount - 1u},
        Range{kMaterialParameterDescriptorOffset, 1u},
        Range{kHeroBindingDescriptorOffset, 1u}};
    // Environment's final two slots are live GBuffer material/motion; the VFX
    // gap is transient distortion. Neither is present in this asset publication.
    for (std::uint32_t frame = 0; frame < kFrameCount; ++frame)
        for (const auto range : ranges)
        {
            auto destination = srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
            auto source = candidate.srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
            const auto offset = static_cast<SIZE_T>(frame * kTextureDescriptorCount + range.first);
            destination.ptr += offset * srv_stride;
            source.ptr += offset * candidate.srv_stride;
            device->CopyDescriptorsSimple(range.count, destination, source, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
    const auto active_start = srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
    for (std::uint32_t frame = 0; frame < kFrameCount; ++frame)
    {
        auto &published = geometry_page_descriptor_banks[frame];
        // Preparation wrote every candidate bank before copying these descriptors.
        published = candidate.geometry_page_descriptor_banks[frame];
        published.heap_owner = srv_cpu_heap;
        published.heap_start = active_start.ptr;
        published.stride = srv_stride;
    }
}

Result D3D12Renderer::Impl::SubmitResidentUtilityUpload(ID3D12GraphicsCommandList *list,
    ID3D12CommandAllocator *command_allocator, std::vector<AllocationResource> &uploads)
{
    CollectResidentUtilityUploads();
    if (!list || !command_allocator || !queue || !fence || !fence_event ||
        HasUnresolvedResidentUtilityUploads() || next_fence == UINT64_MAX)
        return ResidentFailure("Utility upload requires an available owned command context and fence timeline.");
    ResidentUtilitySubmission submission;
    submission.allocator = command_allocator;
    submission.list = list;
    submission.device = device;
    submission.queue = queue;
    submission.fence = fence;
    submission.fence_value = next_fence++;
    // Allocate the lease slot before moving the uploads or submitting work.
    resident_utility_submissions.emplace_back(std::move(submission));
    auto &retained = resident_utility_submissions.back();
    retained.uploads = std::move(uploads);
    ID3D12CommandList *lists[]{list};
    StartupOperationScope submit_timing(RendererStartupOperation::UploadSubmit);
    queue->ExecuteCommandLists(1, lists);
    auto hr = queue->Signal(fence.Get(), retained.fence_value);
    submit_timing.Stop();
    if (FAILED(hr))
    {
        frame_submission_faulted = true;
        return CheckDevice(hr, "Signal resident utility upload completion");
    }
    retained.signaled = true;
    StartupOperationScope wait_timing(RendererStartupOperation::GpuCompletionAdmission);
    if (fence->GetCompletedValue() < retained.fence_value)
    {
        hr = fence->SetEventOnCompletion(retained.fence_value, fence_event);
        if (FAILED(hr))
        {
            frame_submission_faulted = true;
            return HResultFailure("Set resident utility upload completion event", hr);
        }
        const auto wait = MeasureStartupOperation(RendererStartupOperation::GpuFenceWait, [&] { return WaitForSingleObject(fence_event, INFINITE); });
        if (wait != WAIT_OBJECT_0)
        {
            frame_submission_faulted = true;
            return HResultFailure("Wait for resident utility upload completion",
                wait == WAIT_FAILED ? HRESULT_FROM_WIN32(GetLastError()) : E_FAIL);
        }
    }
    wait_timing.Stop();
    if (FAILED(device->GetDeviceRemovedReason()))
    {
        frame_submission_faulted = true;
        return CheckDevice(device->GetDeviceRemovedReason(), "Complete resident utility upload");
    }
    if (!SubmissionFinished(retained))
    {
        frame_submission_faulted = true;
        return ResidentFailure("Utility upload event returned without its post-work fence completion.");
    }
    CollectResidentUtilityUploads();
    return Result::Success();
}

void D3D12Renderer::Impl::CollectResidentUtilityUploads() noexcept
{
    std::erase_if(resident_utility_submissions, [&](const auto &submission) {
        if (!SubmissionFinished(submission)) return false;
        if (resident_preparation_accounting.enabled)
            for (const auto &upload : submission.uploads)
                if (upload.allocation)
                    resident_preparation_accounting.live_native_bytes -= upload.allocation->GetSize();
        return true;
    });
}

bool D3D12Renderer::Impl::HasUnresolvedResidentUtilityUploads() const noexcept
{
    return std::ranges::any_of(resident_utility_submissions,
        [](const auto &submission) { return !SubmissionFinished(submission); });
}

Result D3D12Renderer::Impl::AdmitResidentAllocation(const D3D12_RESOURCE_DESC &description,
    const D3D12MA::ALLOCATION_DESC &allocation)
{
    if (!resident_preparation_accounting.enabled) return Result::Success();
    const auto size = device->GetResourceAllocationInfo(0, 1, &description).SizeInBytes;
    if (size == UINT64_MAX || size > UINT64_MAX - resident_preparation_accounting.live_native_bytes)
        return ResidentFailure("Resident native allocation size overflow.");
    ResidencySegment segment{};
    if (!ResidencySegmentForAllocation(allocation, segment))
        return ResidentFailure("Resident allocation has no resolvable native memory segment.");
    if (auto refreshed = RefreshBudgetObservations(); !refreshed) return refreshed;
    const auto &observation = segment == ResidencySegment::Local
        ? local_budget_observation : nonlocal_budget_observation;
    const auto hr = observation.last_result;
    if (FAILED(hr)) return HResultFailure("Query resident generation overlap budget", hr);
    const auto &memory = observation.last_valid;
    // Match the renderer's established ten-percent device headroom. CurrentUsage
    // includes the old resident state, already prepared state and live SDK/view
    // allocations; do not add the old generation twice or evict it to prepare.
    const auto headroom = memory.Budget / 100u * 10u;
    const auto budget = memory.Budget - headroom;
    if (memory.CurrentUsage > budget || size > budget - memory.CurrentUsage)
        return ResidentFailure("Live plus candidate resident allocation exceeds the native residency budget.");
    return Result::Success();
}

void D3D12Renderer::Impl::RecordResidentAllocation(const AllocationResource &resource,
                                               D3D12_HEAP_TYPE type) noexcept
{
    if (!resident_preparation_accounting.enabled || !resource.allocation) return;
    const auto size = resource.allocation->GetSize();
    auto &accounting = resident_preparation_accounting;
    accounting.live_native_bytes += size;
    accounting.peak_native_bytes = std::max(accounting.peak_native_bytes, accounting.live_native_bytes);
    if (type == D3D12_HEAP_TYPE_DEFAULT) accounting.default_bytes += size;
    else if (type == D3D12_HEAP_TYPE_UPLOAD) accounting.upload_bytes += size;
    else if (type == D3D12_HEAP_TYPE_READBACK) accounting.readback_bytes += size;
}
} // namespace hs
