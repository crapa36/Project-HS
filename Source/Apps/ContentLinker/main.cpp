#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../AssetPipeline/action.hpp"
#include "vfx_storage_tag.hpp"
#include <hs/render_data/renderer_catalog.hpp>
#include <hs/core/sha256_digest.hpp>
#include <hs/core/sha256.hpp>
#include <hs/runtime/vfx_image_io.hpp>
#include <hs/render_data/texture_descriptor.hpp>
#include <hs/render_data/geometry_descriptor.hpp>
#include <hs/render_data/material_descriptor.hpp>
#include <hs/render_data/character_descriptor.hpp>
#include <hs/render_data/render_revision.hpp>
#include <hs/render_data/vfx_mesh_descriptor.hpp>
#include "renderer_geometry_catalog.hpp"
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

std::array<std::byte, 32> ParseDigest(std::string_view digest)
{
    const auto result = hs::ParseSha256Digest(digest);
    Require(result.has_value(), "invalid SHA-256 digest");
    return *result;
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
                        const std::array<hs::shipping::ShippingRange,2> &vfx_ranges,
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
    const auto font_hash = ParseDigest(font_digest);
    for (std::size_t index=0; index<font_hash.size(); ++index)
    {
        if(index)source+=',';
        source+="std::byte{"+std::to_string(std::to_integer<unsigned>(font_hash[index]))+"}";
    }
    source += "},{{";
    for (std::size_t row=0; row<vfx_ranges.size(); ++row)
    {
        if (row) source += ',';
        source += "{" + std::to_string(vfx_ranges[row].offset) + "ull," + std::to_string(vfx_ranges[row].size) + "ull,{";
        for (std::size_t index=0; index<32; ++index)
        {
            if (index) source += ',';
            source += "std::byte{" + std::to_string(std::to_integer<unsigned>(vfx_ranges[row].digest[index])) + "}";
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

void Link(const fs::path &build_root, const fs::path &font_path,
          const fs::path &renderer_result_list)
{
    Require(build_root.is_absolute(), "build root must be absolute");
    ap::RequireDirectory(build_root);
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
    const auto &presentation = renderer_results.at("game-data/presentation-payload");
    const auto &simulation = renderer_results.at("game-data/simulation-payload");
    const auto &identity = renderer_results.at("game-data/content-identity");
    ap::UpstreamProduct presentation_dependency, simulation_dependency, identity_dependency;
    const auto presentation_payload = presentation.ReadProductBytes("game-data/presentation-payload", presentation_dependency);
    const auto simulation_payload = simulation.ReadProductBytes("game-data/simulation-payload", simulation_dependency);
    const auto hash_bytes = identity.ReadProductBytes("game-data/content-identity", identity_dependency);
    const auto content_hash = LegacyHash(hash_bytes);
    const auto &audio = renderer_results.at("audio");
    ap::UpstreamProduct audio_pcm_dependency;
    ap::UpstreamProduct audio_identity_dependency;
    const auto audio_identity = audio.ReadProductBytes("audio/release-identity", audio_identity_dependency);
    const auto &particles = renderer_results.at("vfx/particle-definitions");
    const auto &program = renderer_results.at("vfx/vfx-runtime");
    ap::UpstreamProduct particles_dependency, program_dependency;
    const auto particle_bytes = particles.ReadProductBytes("vfx/particle_effects.hsbin", particles_dependency);
    const auto program_bytes = program.ReadProductBytes("vfx/vfx_program.hsbin", program_dependency);
    hs::VfxImageData vfx;
    const auto decoded = hs::DecodeVfxImage(Bytes(particle_bytes), Bytes(program_bytes), vfx);
    Require(decoded.Succeeded(), "invalid shipping VFX: " + std::string(decoded.Message()));
    Require(vfx.has_required_sprite_bindings, "shipping VFX omits required status sprite bindings");
    const std::array<std::string,2> vfx_digests{particles_dependency.digest, program_dependency.digest};
    ap::RequireRegularFile(font_path);
    const auto font_bytes = fs::file_size(font_path);
    Require(font_bytes != 0 && font_bytes <= UINT32_MAX, "font extent exceeds DirectWrite input range");
    const std::string font_digest = "sha256:194018e6b2b293a7964f037b25c0249ce1418bc9ab3c971060a03aa57861e252";
    struct Bulk { const ap::ActionResultSnapshot *snapshot; std::string_view id; rd::RenderBulkFormat format; };
    std::map<std::uint64_t,Bulk> bulks;
    std::map<std::uint64_t,std::string_view> closure_ids;
    std::map<std::uint64_t,ap::UpstreamProduct> source_revisions;
    const std::array<std::pair<const ap::ActionResultSnapshot *, const ap::UpstreamProduct *>, 6> authenticated{{
        {&presentation, &presentation_dependency}, {&simulation, &simulation_dependency},
        {&identity, &identity_dependency}, {&audio, &audio_identity_dependency},
        {&particles, &particles_dependency}, {&program, &program_dependency}}};
    std::vector<char> copy_scratch(1024 * 1024);
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
            const auto asset=hs::MakeAssetId(id).value;
            const auto [product,inserted]=closure_ids.emplace(asset,id);
            Require(inserted || product->second==id,"runtime closure AssetId collision");
            if (!id.starts_with("render/") && id!="vfx/vfx_mesh_atlas.hsbin")
            {
                if (&snapshot == &audio && id == "audio/release-pcm") continue;
                ap::UpstreamProduct dependency;
                const auto existing = std::ranges::find_if(authenticated, [&](const auto &entry) {
                    return entry.first == &snapshot && entry.second->logical_id == id;
                });
                if (existing != authenticated.end()) dependency = *existing->second;
                else snapshot.CopyProductBytesTo(id,discard,dependency,copy_scratch);
                const auto [entry,first]=source_revisions.try_emplace(asset,std::move(dependency));
                Require(first || (entry->second.logical_id==dependency.logical_id && entry->second.digest==dependency.digest &&
                    entry->second.size==dependency.size),"source revision duplicate identity disagrees");
                continue;
            }
            using F = rd::RenderBulkFormat;
            std::optional<F> format;
            for (const auto &[suffix,token]:std::array<std::pair<const char *,F>,11>{{
                {".rpkg",F::Package}, {".rpage",F::GeometryPage}, {".ghbin",F::GeometryHierarchy},
                {".shl1",F::SceneHlod}, {".index",F::StaticShadowIndex}, {".pages",F::StaticShadowPages},
                {".dds",F::DdsTexture}, {".spkg",F::SceneProbe}, {".dxil",F::ShaderDxil}, {".hsbin",F::VfxMeshAtlas}, {".abi",F::SurfaceLayoutAbi}}})
                if(id.ends_with(suffix))format=token;
            Require(format.has_value() && (!id.ends_with(".hsbin") || id=="vfx/vfx_mesh_atlas.hsbin") &&
                (!id.ends_with(".abi") || id=="render/abi/surface_layout.abi"), "unknown renderer bulk format");
            Require(bulks.emplace(asset,Bulk{&snapshot,id,*format}).second,
                    "duplicate renderer bulk or AssetId collision");
        }
    Require(!bulks.empty(), "renderer result list contains no bulk products");
    // Retain authenticated metadata until its identical bytes are written below.
    struct MetadataBytes { std::string bytes; ap::UpstreamProduct dependency; };
    std::map<std::uint64_t,MetadataBytes> metadata_bytes;
    hs::apps::RendererGeometryCatalog geometry_catalog(closure_ids);
    for(const auto &[asset,bulk]:bulks)
    {
        if(!hs::apps::RendererGeometryCatalog::NeedsMetadata(bulk.id)) continue;
        auto &retained=metadata_bytes[asset];
        retained.bytes=bulk.snapshot->ReadProductBytes(bulk.id,retained.dependency);
        geometry_catalog.AddMetadata(bulk.id,Bytes(retained.bytes));
    }
    const auto pack_path = build_root / "bin" / HS_SHIPPING_PACK_RELATIVE_PATH;
    ap::MakeDirectory(pack_path.parent_path());
    const auto stage=ap::CreateStage(pack_path.parent_path());
    try
    {
    const auto temporary=stage/"content.hsp";
    std::ofstream output(temporary,std::ios::binary);
    Require(output.is_open(),"cannot create staged shipping pack");
    const auto write=[&](std::string_view bytes){output.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));Require(bool(output),"cannot write shipping pack");};
    write(std::string(64,'\0'));
    audio.CopyProductBytesTo("audio/release-pcm",output,audio_pcm_dependency,copy_scratch);
    const auto [pcm_entry, pcm_first] = source_revisions.emplace(
        hs::MakeAssetId("audio/release-pcm").value, audio_pcm_dependency);
    Require(pcm_first || (pcm_entry->second.logical_id == audio_pcm_dependency.logical_id &&
        pcm_entry->second.digest == audio_pcm_dependency.digest && pcm_entry->second.size == audio_pcm_dependency.size),
        "source revision duplicate identity disagrees");
    const auto pcm_bytes=audio_pcm_dependency.size;
    const auto pcm_digest=audio_pcm_dependency.digest;
    Require(pcm_bytes<=UINT64_MAX-64ull-font_bytes,"shipping prefix extent overflow");
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
    std::vector<hs::AssetId> bulk_assets;
    std::vector<rd::RendererRevisionBulk> revision_bulk_descriptors;
    std::optional<rd::VfxMeshAtlasDescriptor> vfx_mesh_atlas;
    Require(bulks.size()<=UINT32_MAX,"renderer bulk count exceeds index range");
    bulk_descriptors.reserve(bulks.size());
    bulk_assets.reserve(bulks.size());
    for(const auto &[asset,bulk]:bulks)
    {
        Require(extent<=UINT64_MAX-63,"renderer bulk alignment overflow");
        const auto aligned=(extent+63)&~std::uint64_t(63);
        write(std::string(static_cast<std::size_t>(aligned-extent),'\0'));
        ap::UpstreamProduct dependency;
        const auto index=static_cast<std::uint32_t>(bulk_descriptors.size());
        if((bulk.id.starts_with("render/texture/") && bulk.id.ends_with(".rpkg")) ||
           rd::IsTerrainSurfaceSectionProduct(bulk.id))
        {
            const auto bytes=bulk.snapshot->ReadProductBytes(bulk.id,dependency);
            // ReadProductBytes authenticated the entire cooked texture, including its mip digests.
            const auto package = Bytes(bytes);
            const auto directory = rd::ReadPackageDirectory(package, package.size(), hs::AssetId{asset});
            Require(!directory.chunks.empty(), "shipping texture has no metadata");
            const auto &chunk = directory.chunks.front();
            const auto metadata = package.subspan(chunk.offset, chunk.byte_count);
            auto texture = chunk.version == 1
                ? rd::CompileVolumeTextureMetadataDescriptor(directory, metadata, index, package.size())
                : rd::CompileTextureMetadataDescriptor(directory, metadata, index, package.size());
            for(const auto id:texture.dependencies)
                Require(closure_ids.contains(id.value),"texture dependency missing from explicit runtime closure");
            textures.push_back(std::move(texture));
            write(bytes);
        }
        else if(bulk.format==rd::RenderBulkFormat::GeometryPage)
        {
            const auto bytes=bulk.snapshot->ReadProductBytes(bulk.id,dependency);
            auto compiled=geometry_catalog.CompilePage(hs::AssetId{asset},Bytes(bytes),index);
            geometries.push_back(std::move(compiled));
            write(bytes);
        }
        else if(bulk.format==rd::RenderBulkFormat::VfxMeshAtlas)
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
        Require(dependency.size<=UINT64_MAX-aligned,"renderer bulk extent overflow");
        extent=aligned+dependency.size;
        const auto expected_digest=ParseDigest(dependency.digest);
        bulk_descriptors.push_back({hs::AssetId{asset},{aligned,dependency.size,
            bulk.format,expected_digest}});
        bulk_assets.push_back(hs::AssetId{asset});
        if(rd::IsRendererMaterialRevisionProduct(bulk.id))
            revision_bulk_descriptors.push_back({bulk.id,index});
    }
    Require(vfx_mesh_atlas.has_value(),"runtime renderer closure omits VFX mesh atlas");
    std::vector<rd::TextureDescriptor> texture_views;
    for(const auto &texture:textures) texture_views.push_back(texture.View());
    geometry_catalog.ValidateCharacterBindings(texture_views);
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
        const auto found=std::ranges::lower_bound(bulk_assets,id.value,{},&hs::AssetId::value);
        Require(found!=bulk_assets.end() && found->value==id.value,"character bulk missing");
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
        rd::RendererSourceRevision row{hs::AssetId{asset}, revision.logical_id, ParseDigest(revision.digest)};
        revision_descriptors.push_back(row);
    }
    std::vector<rd::CharacterDescriptor> character_views;
    for (const auto &value:characters) character_views.push_back(value.View());
    std::vector<rd::AnimationDescriptor> animation_views;
    for (const auto &value:animations) animation_views.push_back(value.View());
    const auto renderer_payload=rd::EncodeRendererCatalog({bulk_descriptors,texture_views,
        geometry_views,material_views,character_views,animation_views,&*vfx_mesh_atlas,
        revision_descriptors,revision_bulk_descriptors},extent);
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
    auto header = Header(build_id, extent);
    const std::array<hs::shipping::ShippingRange,2> vfx_ranges{{
        {vfx_offsets[0], vfx_sizes[0], ParseDigest(vfx_digests[0])},
        {vfx_offsets[1], vfx_sizes[1], ParseDigest(vfx_digests[1])}}};
    const auto storage_tag = hs::shipping::VfxStorageTag(vfx_ranges);
    std::memcpy(header.data()+56, storage_tag.data(), storage_tag.size());
    output.seekp(0);write(header);output.flush();Require(bool(output),"cannot flush shipping pack");
    output.close();Require(bool(output),"cannot close shipping pack");
    Require(fs::file_size(temporary)==extent,"staged shipping pack extent differs from its catalog");
    const auto source = ImageSource(header, content_hash, font_offset, font_bytes, font_digest, vfx_ranges, tables);
    const auto source_path = build_root / HS_SHIPPING_SOURCE_RELATIVE_PATH;
    ap::MakeDirectory(source_path.parent_path());
    // Atomic replacement preserves previously opened handles. The pair is not a
    // transaction: an interrupted publish is rejected by exact header matching.
    ap::AtomicPublishFile(temporary, pack_path);
    ap::AtomicWriteIfDifferent(source_path, source);
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
                "usage: hs_content_linker --build-root <absolute> --font <ttf> --renderer-result-list <absolute>");
        build_root = fs::path(argv[2]).lexically_normal();
        fs::path renderer_result_list;
        for (int index = 5; index < argc;)
        {
            const std::wstring_view option(argv[index++]);
            if (option == L"--renderer-result-list" && index < argc && renderer_result_list.empty())
                renderer_result_list=fs::path(argv[index++]).lexically_normal();
            else Require(false, "invalid or incomplete argument");
        }
        Require(!renderer_result_list.empty(), "shipping image requires --renderer-result-list");
        Link(build_root, fs::absolute(argv[4]).lexically_normal(), renderer_result_list);
        return 0;
    }
    catch (const std::exception &error)
    {
        ap::ReportFailure("hs_content_linker", error.what(),
                          {{"build_root", ap::DiagnosticPath(build_root)}});
        return 1;
    }
}
