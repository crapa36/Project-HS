#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../AssetPipeline/action.hpp"
#include "../Content/character_products.hpp"
#include "vfx_storage_tag.hpp"
#include <hs/game_rules/simulation_rules_io.hpp>
#include <hs/render_data/renderer_catalog.hpp>
#include <hs/presentation/presentation_catalog_io.hpp>
#include <hs/runtime/vfx_image_io.hpp>
#include <hs/render_data/texture_descriptor.hpp>
#include <hs/render_data/geometry_descriptor.hpp>
#include <hs/render_data/material_descriptor.hpp>
#include <hs/render_data/character_descriptor.hpp>
#include <hs/render_data/render_revision.hpp>
#include <hs/render_data/vfx_mesh_descriptor.hpp>
#include "../Common/renderer_geometry_catalog.hpp"
#include <bit>
#include <type_traits>

#include <charconv>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <fstream>
#include <limits>
#include <set>
#include <map>
#include <memory>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace ap = hs::asset_pipeline;
namespace fs = std::filesystem;
namespace rd = hs::render_data;

void Require(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error("hs_content_linker: " + std::string(message));
}

std::span<const std::byte> Bytes(std::string_view text)
{
    return std::as_bytes(std::span(text.data(), text.size()));
}

std::uint64_t LegacyHash(std::string_view text)
{
    if (text.ends_with('\n')) text.remove_suffix(1);
    std::uint64_t hash{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), hash);
    Require(error == std::errc{} && end == text.data() + text.size(),
            "invalid content identity product");
    return hash;
}

std::string Header(std::string_view build_id, std::uint64_t file_bytes)
{
    Require(build_id.size() == 64, "invalid shipping build ID");
    std::string header(64, '\0');
    header.replace(0, 8, "HSCONT01");
    const auto put = [&](std::size_t offset, std::uint64_t value, unsigned width) {
        for (unsigned byte = 0; byte < width; ++byte)
            header[offset + byte] = static_cast<char>((value >> (byte * 8)) & 255);
    };
    put(8, 3, 4);  // Explicit binary tables, independent of C++ object layout.
    put(12, 8, 4); // PCM, font, two VFX products and four immutable table sections.
    for (std::size_t index = 0; index < 32; ++index)
    {
        unsigned value{};
        const auto begin = build_id.data() + index * 2;
        const auto [end, error] = std::from_chars(begin, begin + 2, value, 16);
        Require(error == std::errc{} && end == begin + 2, "non-hex shipping build ID");
        header[16 + index] = static_cast<char>(value);
    }
    put(48, file_bytes, 8); // Entire pack extent including headerless PCM and font bytes.
    put(56, 64, 4); // Semantic identity keeps the original fixed-header marker.
    return header;
}

std::string ImageSource(std::string_view header,
                        std::uint64_t content_hash, std::uint64_t font_offset, std::uint64_t font_bytes, std::string_view font_digest,
                        const std::array<std::uint64_t,2> &vfx_offsets,
                        const std::array<std::uint64_t,2> &vfx_sizes,
                        const std::array<std::string,2> &vfx_digests,
                        const std::array<hs::shipping::ShippingRange,4> &tables)
{
    std::string source =
        "\n#include <hs/runtime/shipping_content.hpp>\n"
        "// Installer reads this section to verify the shipped content image.\n"
        "#if defined(_MSC_VER)\n#pragma section(\".hsimage\", read)\n#define HS_SHIPPING_IMAGE_SECTION __declspec(allocate(\".hsimage\"))\n"
        "#else\n#define HS_SHIPPING_IMAGE_SECTION __attribute__((section(\".hsimage\"), used))\n#endif\n"
        "namespace hs::shipping\n{\nnamespace\n{\n"
        "HS_SHIPPING_IMAGE_SECTION constinit const ShippingImageInfo kImage{{";
    for (std::size_t index = 0; index < header.size(); ++index)
    {
        if (index) source += ',';
        source += "std::byte{" + std::to_string(static_cast<unsigned char>(header[index])) + "}";
    }
    source += "}, L\"" HS_SHIPPING_PACK_RELATIVE_PATH "\", std::uint64_t{";
    source += std::to_string(content_hash) + "ull}," + std::to_string(font_offset) + "ull," + std::to_string(font_bytes) + "ull,{";
    for (std::size_t index=0; index<32; ++index)
    {
        unsigned value{};
        const auto begin=font_digest.data()+7+index*2;
        const auto [end,error]=std::from_chars(begin,begin+2,value,16);
        Require(error==std::errc{} && end==begin+2,"invalid font digest");
        if(index)source+=',';
        source+="std::byte{"+std::to_string(value)+"}";
    }
    source += "},{{";
    for (std::size_t row=0; row<vfx_offsets.size(); ++row)
    {
        if (row) source += ',';
        source += "{" + std::to_string(vfx_offsets[row]) + "ull," + std::to_string(vfx_sizes[row]) + "ull,{";
        Require(vfx_digests[row].starts_with("sha256:") && vfx_digests[row].size()==71, "invalid VFX digest");
        for (std::size_t index=0; index<32; ++index)
        {
            unsigned value{};
            const auto begin=vfx_digests[row].data()+7+index*2;
            const auto [end,error]=std::from_chars(begin,begin+2,value,16);
            Require(error==std::errc{} && end==begin+2,"invalid VFX digest byte");
            if (index) source += ',';
            source += "std::byte{" + std::to_string(value) + "}";
        }
        source += "}}";
    }
    source += "}},{{";
    for (std::size_t i=0; i<tables.size(); ++i)
    {
        if (i) source += ',';
        const auto &table=tables[i];
        source += "{"+std::to_string(table.offset)+"ull,"+std::to_string(table.size)+"ull,{";
        for (std::size_t byte=0; byte<table.digest.size(); ++byte)
        {
            if (byte) source += ',';
            source += "std::byte{"+std::to_string(std::to_integer<unsigned>(table.digest[byte]))+"}";
        }
        source += "}}";
    }
    source += "}}};\n}\nconst ShippingImageInfo &ImageInfo() noexcept { return kImage; }\n}\n";
    return source;
}

void BindCharacters(const fs::path &build_root,
                    std::span<const std::pair<std::string, std::string>> characters,
                    std::span<const std::string> binding_results,
                    ap::ActionSpec &spec)
{
    std::vector<ap::ActionResultSnapshot> snapshots;
    snapshots.reserve(characters.size() + binding_results.size());
    for (const auto &[result, id] : characters) snapshots.emplace_back(build_root, result);
    for (const auto &result : binding_results) snapshots.emplace_back(build_root, result);
    // Node ownership keeps package views valid while later dependencies are read.
    std::map<std::string, std::string> owned;
    const auto read = [&](const std::string &id) {
        if (const auto found = owned.find(id); found != owned.end()) return Bytes(found->second);
        const ap::ActionResultSnapshot *producer{};
        for (const auto &snapshot : snapshots)
            if (snapshot.HasProduct(id))
            {
                Require(producer == nullptr, "ambiguous character dependency " + id);
                producer = &snapshot;
            }
        Require(producer != nullptr, "missing character dependency " + id);
        ap::UpstreamProduct dependency;
        auto bytes = producer->ReadProductBytes(id, dependency);
        spec.upstream.push_back(std::move(dependency));
        return Bytes(owned.emplace(id, std::move(bytes)).first->second);
    };
    for (const auto &[result, id] : characters)
    {
        const auto prepared = hs::content::PrepareCharacterBinding(id, read(id));
        std::vector<hs::content::CharacterDependencyProduct> dependencies;
        dependencies.reserve(prepared.package.dependencies.size());
        for (const auto &dependency : prepared.package.dependencies)
            dependencies.push_back({dependency, read(dependency)});
        hs::content::ValidateCharacterProductBindings(prepared, dependencies);
    }
}

void Link(const fs::path &build_root, const fs::path &font_path,
          std::span<const std::pair<std::string, std::string>> characters,
          std::span<const std::string> binding_results, const fs::path &renderer_result_list)
{
    Require(build_root.is_absolute(), "build root must be absolute");
    ap::RequireDirectory(build_root);
    const ap::ActionResultSnapshot presentation(build_root, "game-data/presentation-payload");
    const ap::ActionResultSnapshot simulation(build_root, "game-data/simulation-payload");
    const ap::ActionResultSnapshot identity(build_root, "game-data/content-identity");
    ap::UpstreamProduct presentation_dependency, simulation_dependency, identity_dependency;
    const auto presentation_payload = presentation.ReadProductBytes("game-data/presentation-payload", presentation_dependency);
    const auto simulation_payload = simulation.ReadProductBytes("game-data/simulation-payload", simulation_dependency);
    const auto hash_bytes = identity.ReadProductBytes("game-data/content-identity", identity_dependency);
    const auto content_hash = LegacyHash(hash_bytes);
    ap::ActionSpec spec;
    spec.action_id = "shipping-image";
    spec.recipe_id = "shipping-image-v1";
    spec.recipe_version = 20;
    spec.upstream = {presentation_dependency, simulation_dependency, identity_dependency};
    auto rules = std::make_unique<hs::SimulationRules>();
    hs::PresentationCatalog catalog;
    const auto rules_decoded=hs::DecodeSimulationRules(Bytes(simulation_payload),*rules);
    Require(rules_decoded.Succeeded(), rules_decoded.Message());
    const auto presentation_decoded=hs::DecodePresentationCatalog(Bytes(presentation_payload),catalog);
    Require(presentation_decoded.Succeeded(), presentation_decoded.Message());
    BindCharacters(build_root, characters, binding_results, spec);
    const ap::ActionResultSnapshot audio(build_root, "audio");
    ap::UpstreamProduct audio_pcm_dependency;
    ap::UpstreamProduct audio_identity_dependency;
    const auto audio_identity = audio.ReadProductBytes("audio/release-identity", audio_identity_dependency);
    spec.upstream.push_back(std::move(audio_identity_dependency));
    const ap::ActionResultSnapshot particles(build_root, "vfx/particle-definitions");
    const ap::ActionResultSnapshot program(build_root, "vfx/vfx-runtime");
    ap::UpstreamProduct particles_dependency, program_dependency;
    const auto particle_bytes = particles.ReadProductBytes("vfx/particle_effects.hsbin", particles_dependency);
    const auto program_bytes = program.ReadProductBytes("vfx/vfx_program.hsbin", program_dependency);
    hs::VfxImageData vfx;
    const auto decoded = hs::DecodeVfxImage(Bytes(particle_bytes), Bytes(program_bytes), vfx);
    Require(decoded.Succeeded(), "invalid shipping VFX: " + std::string(decoded.Message()));
    Require(vfx.has_required_sprite_bindings, "shipping VFX omits required status sprite bindings");
    const std::array<std::string,2> vfx_digests{particles_dependency.digest, program_dependency.digest};
    spec.upstream.push_back(std::move(particles_dependency));
    spec.upstream.push_back(std::move(program_dependency));
    ap::RequireRegularFile(font_path);
    const auto font_bytes = fs::file_size(font_path);
    Require(font_bytes != 0 && font_bytes <= UINT32_MAX, "font extent exceeds DirectWrite input range");
    const std::string font_digest = "sha256:194018e6b2b293a7964f037b25c0249ce1418bc9ab3c971060a03aa57861e252";
    spec.inputs.push_back({"font/noto-sans-kr",font_path,font_digest});
    Require(renderer_result_list.is_absolute(), "renderer result list must be absolute");
    const auto list_bytes = ap::ReadBytes(renderer_result_list);
    std::map<std::string, ap::ActionResultSnapshot> renderer_results;
    std::istringstream lines(list_bytes);
    std::string line;
    while (std::getline(lines,line))
    {
        if (!line.empty() && line.back()=='\r') line.pop_back();
        if (line.empty()) continue;
        const auto path=fs::path(std::u8string(line.begin(),line.end())).lexically_normal();
        Require(path.is_absolute() && path.filename()=="result.json", "invalid renderer result path");
        const auto relative=path.lexically_relative(build_root/"Actions");
        Require(!relative.empty() && !relative.is_absolute() && *relative.begin()!="..", "renderer result escapes Actions");
        const auto name=relative.parent_path().generic_string();
        renderer_results.try_emplace(name,build_root,name);
    }
    std::string renderer_names;
    for (const auto &[name,snapshot]:renderer_results) { renderer_names+=name; renderer_names+='\n'; }
    spec.inputs.push_back({"renderer/result-list", renderer_result_list, ap::Sha256Bytes(Bytes(renderer_names))});
    struct Bulk { const ap::ActionResultSnapshot *snapshot; std::string id,format; };
    std::map<std::uint64_t,Bulk> bulks;
    std::map<std::uint64_t,std::string> closure_ids;
    std::map<std::uint64_t,ap::UpstreamProduct> source_revisions;
    // Authenticate provenance with the existing bounded streaming reader, but
    // retain no source payload in the shipping image or in memory.
    struct DiscardBuffer final : std::streambuf
    {
        std::streamsize xsputn(const char *,std::streamsize count) override { return count; }
        int_type overflow(int_type value) override { return traits_type::not_eof(value); }
    } discard_buffer;
    std::ostream discard(&discard_buffer);
    for (const auto &[name,snapshot]:renderer_results)
        for (const auto id:snapshot.ProductIds())
        {
            const auto [identity,inserted]=closure_ids.emplace(hs::MakeAssetId(id).value,std::string(id));
            Require(inserted || identity->second==id,"runtime closure AssetId collision");
            if (!id.starts_with("render/") && id!="vfx/vfx_mesh_atlas.hsbin")
            {
                ap::UpstreamProduct dependency;
                snapshot.CopyProductBytesTo(id,discard,dependency);
                const auto [entry,first]=source_revisions.emplace(hs::MakeAssetId(id).value,dependency);
                Require(first || (entry->second.logical_id==dependency.logical_id && entry->second.digest==dependency.digest &&
                    entry->second.size==dependency.size),"source revision duplicate identity disagrees");
                spec.upstream.push_back(std::move(dependency));
                continue;
            }
            std::string format;
            for (const auto &[suffix,token]:std::array<std::pair<const char *,const char *>,11>{{
                {".rpkg","Package"}, {".rpage","GeometryPage"}, {".ghbin","GeometryHierarchy"},
                {".shl1","SceneHlod"}, {".index","StaticShadowIndex"}, {".pages","StaticShadowPages"},
                {".dds","DdsTexture"}, {".spkg","SceneProbe"}, {".dxil","ShaderDxil"}, {".hsbin","VfxMeshAtlas"}, {".abi","SurfaceLayoutAbi"}}})
                if(id.ends_with(suffix))format=token;
            Require(!format.empty() && (!id.ends_with(".hsbin") || id=="vfx/vfx_mesh_atlas.hsbin") &&
                (!id.ends_with(".abi") || id=="render/abi/surface_layout.abi"), "unknown renderer bulk format");
            Require(bulks.emplace(hs::MakeAssetId(id).value,Bulk{&snapshot,std::string(id),format}).second,
                    "duplicate renderer bulk or AssetId collision");
        }
    Require(!bulks.empty(), "renderer result list contains no bulk products");
    // Retain authenticated metadata until its identical bytes are written below.
    struct MetadataBytes { std::string bytes; ap::UpstreamProduct dependency; };
    std::map<std::uint64_t,MetadataBytes> metadata_bytes;
    std::vector<std::string> closure_products;
    for(const auto &[asset,id]:closure_ids) closure_products.push_back(id);
    hs::apps::RendererGeometryCatalog geometry_catalog(closure_products);
    for(const auto &[asset,bulk]:bulks)
    {
        if(!hs::apps::RendererGeometryCatalog::NeedsMetadata(bulk.id)) continue;
        auto &retained=metadata_bytes[asset];
        retained.bytes=bulk.snapshot->ReadProductBytes(bulk.id,retained.dependency);
        geometry_catalog.AddMetadata(bulk.id,Bytes(retained.bytes));
    }
    const auto pack_path = build_root / HS_SHIPPING_PACK_RELATIVE_PATH;
    ap::MakeDirectory(pack_path.parent_path());
    const auto stage=ap::CreateStage(pack_path.parent_path());
    try
    {
    const auto temporary=stage/"content.hsp";
    std::ofstream output(temporary,std::ios::binary);
    Require(output.is_open(),"cannot create staged shipping pack");
    const auto write=[&](std::string_view bytes){output.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));Require(bool(output),"cannot write shipping pack");};
    std::vector<char> copy_scratch(1024 * 1024);
    write(std::string(64,'\0'));
    audio.CopyProductBytesTo("audio/release-pcm",output,audio_pcm_dependency,copy_scratch);
    const auto pcm_bytes=audio_pcm_dependency.size;
    const auto pcm_digest=audio_pcm_dependency.digest;
    Require(pcm_bytes<=UINT64_MAX-64ull-font_bytes,"shipping prefix extent overflow");
    spec.upstream.push_back(std::move(audio_pcm_dependency));
    ap::CopyFileBytesTo(font_path,output,font_bytes,font_digest,copy_scratch);
    std::uint64_t extent=64ull+pcm_bytes+font_bytes;
    const std::array<std::uint64_t,2> vfx_sizes{particle_bytes.size(), program_bytes.size()};
    Require(vfx_sizes[0] && vfx_sizes[1] && vfx_sizes[0] <= UINT32_MAX && vfx_sizes[1] <= UINT32_MAX,
        "VFX image exceeds startup read range");
    Require(vfx_sizes[0] <= UINT64_MAX-extent && vfx_sizes[1] <= UINT64_MAX-extent-vfx_sizes[0], "VFX extent overflow");
    const std::array<std::uint64_t,2> vfx_offsets{extent, extent+vfx_sizes[0]};
    write(particle_bytes);
    write(program_bytes);
    extent += vfx_sizes[0]+vfx_sizes[1];
    std::vector<hs::render_data::TextureDescriptorStorage> textures;
    std::vector<rd::GeometryDescriptorStorage> geometries;
    std::vector<rd::RendererBulkDescriptor> bulk_descriptors;
    std::optional<rd::VfxMeshAtlasDescriptor> vfx_mesh_atlas;
    std::uint64_t bulk_index{};
    for(const auto &[asset,bulk]:bulks)
    {
        Require(extent<=UINT64_MAX-63,"renderer bulk alignment overflow");
        const auto aligned=(extent+63)&~std::uint64_t(63);
        write(std::string(static_cast<std::size_t>(aligned-extent),'\0'));
        ap::UpstreamProduct dependency;
        Require(bulk_index<=UINT32_MAX,"renderer bulk index overflow");
        if((bulk.id.starts_with("render/texture/") && bulk.id.ends_with(".rpkg")) ||
           rd::IsTerrainSurfaceSectionProduct(bulk.id))
        {
            const auto bytes=bulk.snapshot->ReadProductBytes(bulk.id,dependency);
            auto texture=hs::render_data::CompileTextureDescriptor(Bytes(bytes),hs::AssetId{asset},static_cast<std::uint32_t>(bulk_index));
            for(const auto id:texture.dependencies)
                Require(closure_ids.contains(id.value),"texture dependency missing from explicit runtime closure");
            textures.push_back(std::move(texture));
            write(bytes);
        }
        else if(bulk.id.ends_with(".rpage"))
        {
            const auto bytes=bulk.snapshot->ReadProductBytes(bulk.id,dependency);
            auto compiled=geometry_catalog.CompilePage(bulk.id,Bytes(bytes),static_cast<std::uint32_t>(bulk_index));
            geometries.push_back(std::move(compiled));
            write(bytes);
        }
        else if(bulk.format=="VfxMeshAtlas")
        {
            const auto bytes=bulk.snapshot->ReadProductBytes(bulk.id,dependency);
            vfx_mesh_atlas=rd::CompileVfxMeshAtlasDescriptor(Bytes(bytes),hs::AssetId{asset});
            write(bytes);
        }
        else if(auto retained=metadata_bytes.find(asset);retained!=metadata_bytes.end())
        {
            dependency=std::move(retained->second.dependency);
            write(retained->second.bytes);
        }
        else bulk.snapshot->CopyProductBytesTo(bulk.id,output,dependency,copy_scratch);
        ++bulk_index;
        Require(dependency.size<=UINT64_MAX-aligned,"renderer bulk extent overflow");
        extent=aligned+dependency.size;
        std::array<std::byte,32> expected_digest{};
        for(std::size_t i=0;i<32;++i)
        {
            unsigned byte{};const auto first=dependency.digest.data()+7+i*2;
            const auto [end,error]=std::from_chars(first,first+2,byte,16);
            Require(error==std::errc{} && end==first+2,"invalid bulk digest");
            expected_digest[i]=static_cast<std::byte>(byte);
        }
        const auto native_format=[&] {
            using F=rd::RenderBulkFormat;
            if(bulk.format=="Package") return F::Package;
            if(bulk.format=="GeometryPage") return F::GeometryPage;
            if(bulk.format=="GeometryHierarchy") return F::GeometryHierarchy;
            if(bulk.format=="SceneHlod") return F::SceneHlod;
            if(bulk.format=="StaticShadowIndex") return F::StaticShadowIndex;
            if(bulk.format=="StaticShadowPages") return F::StaticShadowPages;
            if(bulk.format=="DdsTexture") return F::DdsTexture;
            if(bulk.format=="SceneProbe") return F::SceneProbe;
            if(bulk.format=="VfxMeshAtlas") return F::VfxMeshAtlas;
            if(bulk.format=="SurfaceLayoutAbi") return F::SurfaceLayoutAbi;
            Require(bulk.format=="ShaderDxil","unknown bulk descriptor format");return F::ShaderDxil;
        }();
        bulk_descriptors.push_back({hs::AssetId{asset},{aligned,dependency.size,
            native_format,expected_digest}});
        spec.upstream.push_back(std::move(dependency));
    }
    Require(vfx_mesh_atlas.has_value(),"runtime renderer closure omits VFX mesh atlas");
    std::vector<hs::AssetId> bulk_assets;
    for(const auto &[asset,bulk]:bulks) bulk_assets.push_back(hs::AssetId{asset});
    std::vector<rd::TextureDescriptor> texture_views;
    for(const auto &texture:textures) texture_views.push_back(texture.View());
    std::vector<rd::MaterialDescriptorStorage> materials;
    std::uint32_t material_bulk_index{};
    for(const auto &[asset,bulk]:bulks)
    {
        if(rd::IsMaterialDescriptorProduct(bulk.id))
        {
            const auto parameters=rd::MaterialParametersProduct(bulk.id);
            materials.push_back(rd::CompileMaterialDescriptor(bulk.id,Bytes(metadata_bytes.at(asset).bytes),
                Bytes(metadata_bytes.at(hs::MakeAssetId(parameters).value).bytes),material_bulk_index,bulk_assets,texture_views));
        }
        ++material_bulk_index;
    }
    std::vector<rd::GeometryDescriptor> geometry_views;
    for(const auto &geometry:geometries) geometry_views.push_back(geometry.View());
    std::vector<rd::MaterialDescriptor> material_views;
    for(const auto &material:materials) material_views.push_back(material.View());
    const auto find_bulk=[&](hs::AssetId id) {
        const auto found=std::ranges::find(bulk_assets,id.value,&hs::AssetId::value);
        Require(found!=bulk_assets.end(),"character bulk missing");
        return static_cast<std::uint32_t>(found-bulk_assets.begin());
    };
    std::vector<rd::CharacterDescriptorStorage> characters;
    for(auto &prepared:geometry_catalog.TakeCharacters())
        characters.push_back(rd::CompileCharacterDescriptor(prepared.bytes,prepared.package,std::move(prepared.data),
            find_bulk(prepared.package.asset_id),bulk_descriptors,geometry_views,material_views));
    std::vector<rd::AnimationDescriptorStorage> animations;
    for(auto &prepared:geometry_catalog.TakeAnimations())
    {
        const auto found=std::ranges::find(characters,prepared.data.character_asset_id.value,
            [](const auto &character){return character.value.asset_id.value;});
        Require(found!=characters.end(),"animation character missing");
        animations.push_back(rd::CompileAnimationDescriptor(prepared.bytes,prepared.package,std::move(prepared.data),
            find_bulk(prepared.package.asset_id),bulk_descriptors,rd::CharacterHandle{static_cast<std::uint32_t>(found-characters.begin())},*found));
    }
    std::vector<rd::RendererSourceRevision> revision_descriptors;
    for(const auto &[asset,revision]:source_revisions)
    {
        rd::RendererSourceRevision row{hs::AssetId{asset}, revision.logical_id, {}};
        Require(revision.digest.starts_with("sha256:") && revision.digest.size()==71,"invalid source revision digest");
        for(std::size_t i=0;i<32;++i)
        {
            unsigned byte{};const auto first=revision.digest.data()+7+i*2;
            const auto [end,error]=std::from_chars(first,first+2,byte,16);
            Require(error==std::errc{} && end==first+2,"invalid source revision digest byte");
            row.digest[i] = std::byte(byte);
        }
        revision_descriptors.push_back(row);
    }
    std::uint32_t revision_index{};
    std::vector<rd::RendererRevisionBulk> revision_bulk_descriptors;
    for(const auto &[asset,bulk]:bulks)
    {
        if(rd::IsRendererMaterialRevisionProduct(bulk.id))
        revision_bulk_descriptors.push_back({bulk.id, revision_index});
        ++revision_index;
    }
    std::ranges::sort(spec.upstream,{},&ap::UpstreamProduct::logical_id);
    for(std::size_t i=1;i<spec.upstream.size();++i)
        if(spec.upstream[i-1].logical_id==spec.upstream[i].logical_id)
            Require(spec.upstream[i-1].digest==spec.upstream[i].digest && spec.upstream[i-1].size==spec.upstream[i].size,
                "character and renderer inputs disagree");
    spec.upstream.erase(std::unique(spec.upstream.begin(),spec.upstream.end(),[](const auto &a,const auto &b){return a.logical_id==b.logical_id;}),spec.upstream.end());
    std::vector<rd::CharacterDescriptor> character_views;
    for (const auto &value:characters) character_views.push_back(value.View());
    std::vector<rd::AnimationDescriptor> animation_views;
    for (const auto &value:animations) animation_views.push_back(value.View());
    const auto renderer_payload=rd::EncodeRendererCatalog({bulk_descriptors,texture_views,
        geometry_views,material_views,character_views,animation_views,&*vfx_mesh_atlas,
        revision_descriptors,revision_bulk_descriptors});
    const std::array table_payloads{Bytes(simulation_payload),Bytes(presentation_payload),
        Bytes(audio_identity),std::span<const std::byte>(renderer_payload)};
    std::array<hs::shipping::ShippingRange,4> tables{};
    for (std::size_t i=0;i<tables.size();++i)
    {
        Require(extent<=UINT64_MAX-63,"table alignment overflow");
        const auto aligned=(extent+63)&~std::uint64_t(63);
        write(std::string(static_cast<std::size_t>(aligned-extent),'\0'));
        const auto bytes=table_payloads[i];
        Require(!bytes.empty() && bytes.size()<=UINT32_MAX && bytes.size()<=UINT64_MAX-aligned,
            "immutable table extent overflow");
        tables[i]={aligned,bytes.size(),hs::Sha256(bytes)};
        write(std::string_view(reinterpret_cast<const char *>(bytes.data()),bytes.size()));
        extent=aligned+bytes.size();
    }
    const auto font_offset = 64 + pcm_bytes;
    hs::CanonicalBytes semantic("project-hs-shipping-image", 2);
    // Wire header fields are explicit LE bytes; omit its self-referential identity.
    semantic.Blob(Bytes(Header(std::string(64, '0'), extent)));
    semantic.String(HS_SHIPPING_PACK_RELATIVE_PATH);
    semantic.Fields(content_hash, font_offset, font_bytes, std::uint64_t{64}, pcm_bytes);
    semantic.String(font_digest); semantic.String(pcm_digest);
    for (std::size_t i=0;i<tables.size();++i)
    {
        semantic.Fields(tables[i].offset,tables[i].size);
        semantic.Blob(table_payloads[i]);
    }
    semantic.Blob(hs::CanonicalVfxImageBytes(vfx.View()));
    const auto image_digest = ap::Sha256Bytes(semantic.View());
    const auto build_id = image_digest.substr(7);
    const auto action_key = ap::ActionKey(spec);
    auto header = Header(build_id, extent);
    const std::array<hs::shipping::ShippingRange,2> vfx_ranges{{
        {vfx_offsets[0], vfx_sizes[0], hs::Sha256(Bytes(particle_bytes))},
        {vfx_offsets[1], vfx_sizes[1], hs::Sha256(Bytes(program_bytes))}}};
    const auto storage_tag = hs::shipping::VfxStorageTag(vfx_ranges);
    std::memcpy(header.data()+56, storage_tag.data(), storage_tag.size());
    output.seekp(0);write(header);output.flush();Require(bool(output),"cannot flush shipping pack");
    output.close();Require(bool(output),"cannot close shipping pack");
    Require(fs::file_size(temporary)==extent,"staged shipping pack extent differs from its catalog");
    const auto source = ImageSource(header, content_hash, font_offset, font_bytes, font_digest, vfx_offsets, vfx_sizes, vfx_digests, tables);
    const auto shipping = build_root / "Shipping";
    ap::MakeDirectory(shipping);
    // Atomic replacement preserves previously opened handles. The pair is not a
    // transaction: an interrupted publish is rejected by exact header matching.
    ap::AtomicPublishFile(temporary, pack_path);
    ap::AtomicWriteIfDifferent(shipping / "content.generated.cpp", source);
    ap::Json receipt{{"identity_domain", "project-hs-shipping-image"}, {"identity_version", 2},
        {"content_image_id", build_id}, {"action_key", action_key},
        {"producer_identity", ap::ProducerRecipeIdentity()},
        {"action_id", spec.action_id}, {"recipe_id", spec.recipe_id}, {"recipe_version", spec.recipe_version},
        {"inputs", ap::Json::array()}, {"upstream", ap::Json::array()}};
    for (const auto &input : spec.inputs)
        receipt["inputs"].push_back({{"logical_id", input.logical_id}, {"digest", input.digest}});
    for (const auto &dependency : spec.upstream)
        receipt["upstream"].push_back({{"logical_id", dependency.logical_id}, {"digest", dependency.digest},
            {"size", dependency.size}, {"result", dependency.result_path.generic_string()}});
    ap::AtomicWriteIfDifferent(shipping / "content.receipt.json", receipt.dump(2) + "\n");
    std::cout << "shipping-image published " << build_id << '\n';
    }
    catch (...) { ap::RemoveStageAfterFailure(stage); throw; }
    ap::RemoveStage(stage);
}
}

int wmain(int argc, wchar_t **argv)
{
    fs::path build_root;
    try
    {
        Require(argc >= 5 && std::wstring_view(argv[1]) == L"--build-root" && std::wstring_view(argv[3]) == L"--font",
                "usage: hs_content_linker --build-root <absolute> --font <pinned-ttf> --character <result> <product> [--binding-result <result> ...] --renderer-result-list <absolute>");
        build_root = fs::path(argv[2]).lexically_normal();
        std::vector<std::pair<std::string, std::string>> characters;
        std::vector<std::string> binding_results;
        fs::path renderer_result_list;
        for (int index = 5; index < argc;)
        {
            const std::wstring_view option(argv[index++]);
            if (option == L"--character" && index + 1 < argc)
            {
                auto result = fs::path(argv[index++]).generic_string();
                auto product = fs::path(argv[index++]).generic_string();
                characters.emplace_back(std::move(result), std::move(product));
            }
            else if (option == L"--binding-result" && index < argc)
                binding_results.push_back(fs::path(argv[index++]).generic_string());
            else if (option == L"--renderer-result-list" && index < argc && renderer_result_list.empty())
                renderer_result_list=fs::path(argv[index++]).lexically_normal();
            else Require(false, "invalid or incomplete character binding argument");
        }
        Require(!characters.empty(), "shipping image requires character products");
        Require(!renderer_result_list.empty(), "shipping image requires --renderer-result-list");
        Link(build_root, fs::absolute(argv[4]).lexically_normal(), characters, binding_results,renderer_result_list);
        return 0;
    }
    catch (const std::exception &error)
    {
        ap::ReportFailure("hs_content_linker", error.what(),
                          {{"build_root", ap::DiagnosticPath(build_root)}});
        return 1;
    }
}
