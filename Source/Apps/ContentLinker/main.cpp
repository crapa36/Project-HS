#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../AssetPipeline/action.hpp"
#include "../Content/character_products.hpp"
#include "../Content/release_vfx_codegen.hpp"
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

std::string GeometrySource(std::span<const rd::GeometryDescriptorStorage> geometries)
{
    const auto scalar=[](auto value) {
        using T=decltype(value);
        if constexpr(std::is_same_v<T,float>)
            return "std::bit_cast<float>(std::uint32_t{"+std::to_string(std::bit_cast<std::uint32_t>(value))+"u})";
        else return std::to_string(value)+"u";
    };
    const auto array=[&](const auto &values) {
        std::string text="{";
        for(const auto value:values) { text+=scalar(value);text+=','; }
        return text+'}';
    };
    std::size_t dependencies{},submeshes{},support{};
    for(const auto &g:geometries)
    { dependencies+=g.dependencies.size();submeshes+=g.submeshes.size();support+=g.ground_support.size(); }
    std::string source="\n#include <bit>\n#include <hs/render_data/geometry_descriptor.hpp>\nnamespace hs::shipping { namespace {\n";
    source+="constinit const std::array<AssetId,"+std::to_string(dependencies)+"> kGeometryDependencies{{\n";
    for(const auto &g:geometries)for(const auto id:g.dependencies) source+="{"+std::to_string(id.value)+"ull},\n";
    source+="}};\nconstinit const std::array<render_data::GeometrySubmesh,"+std::to_string(submeshes)+"> kGeometrySubmeshes{{\n";
    for(const auto &g:geometries)for(const auto &s:g.submeshes)
        source+="{"+scalar(s.first_index)+","+scalar(s.index_count)+","+scalar(s.first_meshlet)+","+scalar(s.meshlet_count)+","+scalar(s.material_index)+"},\n";
    source+="}};\nconstinit const std::array<render_data::ExpandedVertex,"+std::to_string(support)+"> kGeometryGroundSupport{{\n";
    for(const auto &g:geometries)for(const auto &v:g.ground_support)
        source+="{"+array(v.position)+","+array(v.normal)+","+array(v.bone_indices)+","+array(v.bone_weights)+","+
            array(v.uv)+","+array(v.tangent)+","+scalar(v.material_index)+","+scalar(v.reserved)+"},\n";
    source+="}};\nconstinit const std::array<render_data::GeometryDescriptor,"+std::to_string(geometries.size())+"> kGeometryDescriptors{{\n";
    std::size_t dependency_first{},submesh_first{},support_first{};
    for(const auto &g:geometries)
    {
        source+="{{"+std::to_string(g.asset_id.value)+"ull},"+scalar(g.bulk_index)+","+scalar(g.flags)+","+
            scalar(g.vertex_count)+","+scalar(g.index_count)+","+scalar(g.meshlet_count)+","+scalar(g.vertex_ref_count)+","+
            scalar(g.max_material_index)+","+scalar(g.max_bone_index)+",{"+array(g.bounds.minimum)+","+
            array(g.bounds.maximum)+","+array(g.bounds.sphere)+"},{{";
        for(const auto &chunk:g.chunks) source+="{"+std::to_string(chunk.offset)+"ull,"+std::to_string(chunk.byte_count)+"ull},";
        source+="}},std::span<const AssetId>(kGeometryDependencies).subspan("+std::to_string(dependency_first)+","+std::to_string(g.dependencies.size())+"),"+
            "std::span<const render_data::GeometrySubmesh>(kGeometrySubmeshes).subspan("+std::to_string(submesh_first)+","+std::to_string(g.submeshes.size())+"),"+
            "std::span<const render_data::ExpandedVertex>(kGeometryGroundSupport).subspan("+std::to_string(support_first)+","+std::to_string(g.ground_support.size())+")},\n";
        dependency_first+=g.dependencies.size();submesh_first+=g.submeshes.size();support_first+=g.ground_support.size();
    }
    source+="}};\n}\nstd::span<const render_data::GeometryDescriptor> RendererGeometry() noexcept { return kGeometryDescriptors; }\n}\n";
    return source;
}

std::string MaterialSource(std::span<const rd::MaterialDescriptorStorage> materials)
{
    const auto number=[](auto value) { return std::to_string(static_cast<std::uint32_t>(value))+"u"; };
    const auto floating=[](float value) { return "std::bit_cast<float>(std::uint32_t{"+std::to_string(std::bit_cast<std::uint32_t>(value))+"u})"; };
    const auto quoted=[](std::string_view value) {
        std::string result="\"";
        for(const unsigned char c:value)
        {
            if(c<32 || c>=127)
            {
                result+='\\';result+=static_cast<char>('0'+(c>>6));
                result+=static_cast<char>('0'+((c>>3)&7));result+=static_cast<char>('0'+(c&7));
                continue;
            }
            if(c=='"' || c=='\\') result+='\\';
            result+=static_cast<char>(c);
        }
        return result+'"';
    };
    const auto enumeration=[&](std::string_view type,auto value) { return "static_cast<render_data::"+std::string(type)+">("+number(value)+")"; };
    std::size_t defaults{},programs{},textures{};
    for(const auto &m:materials) { defaults+=m.defaults.size();programs+=m.programs.size();textures+=m.textures.size(); }
    std::string source="\n#include <hs/render_data/material_descriptor.hpp>\nnamespace hs::shipping { namespace {\n";
    source+="constinit const std::array<render_data::MaterialParameterSlot,"+std::to_string(defaults)+"> kMaterialDefaults{{\n";
    for(const auto &m:materials)for(const auto &slot:m.defaults)
        source+="{{"+number(slot.words[0])+","+number(slot.words[1])+","+number(slot.words[2])+","+number(slot.words[3])+"}},\n";
    source+="}};\nconstinit const std::array<render_data::MaterialProgramDescriptor,"+std::to_string(programs)+"> kMaterialPrograms{{\n";
    for(const auto &m:materials)for(const auto &p:m.programs)
        source+="{"+enumeration("MaterialProgramKind",p.kind)+","+number(p.bulk_index)+",{"+std::to_string(p.asset_id.value)+"ull},"+quoted(p.product_id)+"},\n";
    source+="}};\nconstinit const std::array<render_data::MaterialTextureDescriptor,"+std::to_string(textures)+"> kMaterialTextures{{\n";
    for(const auto &m:materials)for(const auto &t:m.textures)
        source+="{"+number(t.slot)+",{"+number(t.texture.value)+"},"+enumeration("MaterialTextureDimension",t.dimension)+","+
            enumeration("MaterialTextureSemantic",t.semantic)+","+number(t.shader_binding_abi)+","+quoted(t.product_id)+"},\n";
    source+="}};\nconstinit const std::array<render_data::MaterialDescriptor,"+std::to_string(materials.size())+"> kMaterialDescriptors{{\n";
    std::size_t default_first{},program_first{},texture_first{};
    for(const auto &storage:materials)
    {
        const auto m=storage.View();
        source+="{{"+std::to_string(m.asset_id.value)+"ull},"+number(m.bulk_index)+","+enumeration("MaterialKind",m.kind)+",{"+
            number(m.surface.capability_mask)+","+number(m.surface.varying_mask)+"},{";
        for(const auto byte:m.parameter_layout) source+="std::byte{"+number(std::to_integer<unsigned>(byte))+"},";
        source+="},std::span<const render_data::MaterialParameterSlot>(kMaterialDefaults).subspan("+std::to_string(default_first)+","+std::to_string(m.defaults.size())+"),"+
            "std::span<const render_data::MaterialProgramDescriptor>(kMaterialPrograms).subspan("+std::to_string(program_first)+","+std::to_string(m.programs.size())+"),"+
            "std::span<const render_data::MaterialTextureDescriptor>(kMaterialTextures).subspan("+std::to_string(texture_first)+","+std::to_string(m.textures.size())+"),"+
            floating(m.metallic)+","+floating(m.roughness_scale)+","+number(m.flags)+","+enumeration("HeroMaterialDomain",m.domain)+",{"+
            number(m.receivers.model_mask)+","+number(m.receivers.required_surface_flags)+","+number(m.receivers.forbidden_surface_flags)+","+
            enumeration("DecalReceiverNormalFilter",m.receivers.normal_filter)+"}},\n";
        default_first+=m.defaults.size();program_first+=m.programs.size();texture_first+=m.textures.size();
    }
    return source+"}};\n}\nstd::span<const render_data::MaterialDescriptor> RendererMaterials() noexcept { return kMaterialDescriptors; }\n}\n";
}

std::string CharacterSource(std::span<const rd::CharacterDescriptorStorage> characters,
    std::span<const rd::AnimationDescriptorStorage> animations)
{
    const auto u=[](auto x){return std::to_string(static_cast<std::uint64_t>(x))+"u";};
    const auto f=[](float x){return "std::bit_cast<float>(std::uint32_t{"+std::to_string(std::bit_cast<std::uint32_t>(x))+"u})";};
    const auto fa=[&](const auto &v){std::string s="{";for(auto x:v)s+=f(x)+",";return s+"}";};
    const auto id=[](hs::AssetId x){return "{"+std::to_string(x.value)+"ull}";};
    const auto digest=[&](const auto &v){std::string s="{";for(auto x:v)s+="std::byte{"+u(std::to_integer<unsigned>(x))+"},";return s+"}";};
    const auto quote=[](std::string_view v){std::string s="\"";for(unsigned char c:v){
        if(c<32 || c>=127){s+='\\';s+=char('0'+(c>>6));s+=char('0'+((c>>3)&7));s+=char('0'+(c&7));}
        else {if(c=='"'||c=='\\')s+='\\';s+=char(c);}}return s+'"';};
    const auto bounds=[&](const rd::GeometryBounds &b){return "{"+fa(b.minimum)+","+fa(b.maximum)+","+fa(b.sphere)+"}";};
    const auto metadata=[&](const rd::GeometryMetadata &m){return "{"+u(m.abi)+","+u(m.flags)+","+u(m.vertex_count)+","+
        u(m.index_count)+","+u(m.submesh_count)+","+u(m.meshlet_count)+","+u(m.vertex_ref_count)+","+u(m.max_material_index)+","+
        u(m.page_dependency_index)+","+u(m.fallback_dependency_index)+","+u(m.lod)+","+u(m.terminal_fallback_dependency_index)+","+
        bounds(m.bounds)+","+f(m.geometric_error)+","+u(m.error_method_abi)+"}";};
    const auto range=[&](const rd::BulkRange &r){return "{"+std::to_string(r.offset)+"ull,"+std::to_string(r.byte_count)+
        "ull,static_cast<render_data::RenderBulkFormat>("+u(r.format)+"),"+digest(r.expected_digest)+"}";};
    const auto transform=[&](const hs::CharacterLocalTransform &t){return "{"+fa(t.translation)+","+f(t.translation_padding)+","+
        fa(t.rotation)+","+fa(t.scale)+","+f(t.scale_padding)+"}";};
    std::string source="\n#include <hs/render_data/character_descriptor.hpp>\nnamespace hs::shipping { namespace {\n";
    const auto emit=[&](std::string_view type,const std::string &name,const auto &values,const auto &format){
        source+="constinit const std::array<"+std::string(type)+","+std::to_string(values.size())+"> "+name+"{{\n";
        for(const auto &v:values)source+=format(v)+",\n";
        source+="}};\n";return name;
    };
    std::string character_rows,animation_rows;
    std::size_t serial{};
    for(const auto &storage:characters)
    {
        const auto c=storage.View();const auto p="kCharacter"+std::to_string(serial++);
        const auto deps=emit("AssetId",p+"Dependencies",c.dependencies,id);
        const auto bindings=emit("render_data::CharacterMaterialBinding",p+"Bindings",c.material_bindings,[&](const auto &b){
            return "{"+u(b.slot)+","+u(b.dependency_index)+","+u(b.hero_ordinal)+",static_cast<render_data::HeroMaterialDomain>("+u(b.domain)+")}";});
        const auto materials=emit("render_data::MaterialHandle",p+"Materials",c.materials,[&](auto x){return "{"+u(x.value)+"}";});
        const auto clips=emit("CharacterClipHeader",p+"Clips",c.clips,[&](const auto &x){return "{static_cast<CharacterAnimationClip>("+u(x.clip)+"),"+
            u(x.looping)+","+u(x.reserved)+","+u(x.first_transform)+","+u(x.frame_count)+","+f(x.duration_seconds)+"}";});
        const auto parents=emit("std::uint16_t",p+"Parents",c.parents,u);
        const auto inverse=emit("std::array<float,16>",p+"Inverse",c.inverse_bind_matrices,fa);
        const auto weights=emit("float",p+"Weights",c.upper_body_weights,f);
        const auto lods=emit("render_data::GeometryMetadata",p+"Lods",c.lods,metadata);
        const auto intervals=emit("render_data::CharacterIntervalBound",p+"Intervals",c.interval_bounds,[&](const auto &x){return "{"+u(x.lod)+
            ",static_cast<CharacterAnimationClip>("+u(x.clip)+"),"+u(x.frame)+","+fa(x.minimum)+","+fa(x.maximum)+"}";});
        character_rows+="{"+id(c.asset_id)+","+u(c.bulk_index)+",{"+u(c.geometry_page.value)+"},"+metadata(c.geometry)+","+
            deps+","+bindings+","+materials+","+u(c.bone_count)+","+u(c.material_count)+","+f(c.ground_offset)+","+
            clips+","+parents+","+inverse+","+weights+","+range(c.transforms)+","+u(c.deformation_abi)+","+
            u(c.deformation_flags)+","+digest(c.skeleton_binding)+","+f(c.numerical_margin)+","+f(c.bubble_amplitude)+","+
            lods+","+intervals+","+range(c.final_matrices)+"},\n";
    }
    serial=0;
    for(const auto &storage:animations)
    {
        const auto a=storage.View();const auto p="kAnimation"+std::to_string(serial++);
        const auto names=emit("std::string_view",p+"Names",a.bone_names,quote);
        const auto parents=emit("std::uint16_t",p+"Parents",a.parents,u);
        const auto inverse=emit("std::array<float,16>",p+"Inverse",a.inverse_bind_matrices,fa);
        const auto clips=emit("render_data::AnimationClipDescriptor",p+"Clips",a.clips,[&](const auto &x){return "{"+id(x.id)+","+quote(x.name)+","+
            u(x.first_transform)+","+u(x.frame_count)+","+f(x.duration_seconds)+","+(x.looping?"true":"false")+"}";});
        const auto sockets=emit("render_data::AnimationSocketDescriptor",p+"Sockets",a.sockets,[&](const auto &x){return "{"+quote(x.name)+","+u(x.bone_index)+","+transform(x.local_transform)+"}";});
        animation_rows+="{"+id(a.asset_id)+","+u(a.bulk_index)+",{"+u(a.character.value)+"},"+id(a.character_asset_id)+","+
            u(a.sample_rate_hz)+","+names+","+parents+","+inverse+","+clips+","+sockets+","+range(a.transforms)+"},\n";
    }
    source+="constinit const std::array<render_data::CharacterDescriptor,"+std::to_string(characters.size())+"> kCharacters{{\n"+character_rows+"}};\n";
    source+="constinit const std::array<render_data::AnimationDescriptor,"+std::to_string(animations.size())+"> kAnimations{{\n"+animation_rows+"}};\n}\n";
    return source+"std::span<const render_data::CharacterDescriptor> RendererCharacters() noexcept { return kCharacters; }\n"+
        "std::span<const render_data::AnimationDescriptor> RendererAnimations() noexcept { return kAnimations; }\n}\n";
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
    put(8, 2, 4);  // Format version.
    put(12, 13, 4); // Typed tables including compiled source and bulk revision identities.
    for (std::size_t index = 0; index < 32; ++index)
    {
        unsigned value{};
        const auto begin = build_id.data() + index * 2;
        const auto [end, error] = std::from_chars(begin, begin + 2, value, 16);
        Require(error == std::errc{} && end == begin + 2, "non-hex shipping build ID");
        header[16 + index] = static_cast<char>(value);
    }
    put(48, file_bytes, 8); // Entire pack extent including headerless PCM and font bytes.
    put(56, 64, 4); // Header extent; reserved bytes 60..63 remain zero.
    return header;
}

std::string ImageSource(std::string_view header,
                        std::uint64_t content_hash, std::uint64_t font_offset, std::uint64_t font_bytes, std::string_view font_digest)
{
    std::string source =
        "\n#include <hs/runtime/shipping_content.hpp>\n"
        "namespace hs::shipping\n{\nnamespace\n{\n"
        "constinit const ShippingImageInfo kImage{{";
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
    source += "}};\n}\nconst ShippingImageInfo &ImageInfo() noexcept { return kImage; }\n}\n";
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
    auto source = presentation.ReadProductBytes("game-data/presentation-cpp", presentation_dependency);
    source += simulation.ReadProductBytes("game-data/simulation-cpp", simulation_dependency);
    const auto hash_bytes = identity.ReadProductBytes("game-data/content-identity", identity_dependency);
    const auto content_hash = LegacyHash(hash_bytes);
    ap::ActionSpec spec;
    spec.action_id = "shipping-image";
    spec.recipe_id = "shipping-image-v1";
    spec.recipe_version = 15;
    spec.upstream = {presentation_dependency, simulation_dependency, identity_dependency};
    BindCharacters(build_root, characters, binding_results, spec);
    const ap::ActionResultSnapshot audio(build_root, "audio");
    ap::UpstreamProduct audio_cpp_dependency, audio_pcm_dependency;
    source += audio.ReadProductBytes("audio/release-cpp", audio_cpp_dependency);
    auto pcm = audio.ReadProductBytes("audio/release-pcm", audio_pcm_dependency);
    spec.upstream.push_back(std::move(audio_cpp_dependency));
    spec.upstream.push_back(std::move(audio_pcm_dependency));
    const ap::ActionResultSnapshot particles(build_root, "vfx/particle-definitions");
    const ap::ActionResultSnapshot program(build_root, "vfx/vfx-runtime");
    ap::UpstreamProduct particles_dependency, program_dependency;
    const auto particle_bytes = particles.ReadProductBytes("vfx/particle_effects.hsbin", particles_dependency);
    const auto program_bytes = program.ReadProductBytes("vfx/vfx_program.hsbin", program_dependency);
    hs::VfxImageData vfx;
    const auto decoded = hs::DecodeVfxImage(Bytes(particle_bytes), Bytes(program_bytes), vfx);
    Require(decoded.Succeeded(), "invalid shipping VFX: " + std::string(decoded.Message()));
    Require(vfx.has_required_sprite_bindings, "shipping VFX omits required status sprite bindings");
    source += hs::content::EmitReleaseVfxCpp(vfx.View());
    spec.upstream.push_back(std::move(particles_dependency));
    spec.upstream.push_back(std::move(program_dependency));
    ap::RequireRegularFile(font_path);
    const auto font = ap::ReadBytes(font_path);
    Require(!font.empty() && font.size() <= UINT32_MAX, "font extent exceeds DirectWrite input range");
    const auto font_digest = ap::Sha256Bytes(Bytes(font));
    Require(font_digest == "sha256:194018e6b2b293a7964f037b25c0249ce1418bc9ab3c971060a03aa57861e252", "font differs from pinned Noto Sans KR input");
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
            for (const auto &[suffix,token]:std::array<std::pair<const char *,const char *>,10>{{
                {".rpkg","Package"}, {".rpage","GeometryPage"}, {".ghbin","GeometryHierarchy"},
                {".shl1","SceneHlod"}, {".index","StaticShadowIndex"}, {".pages","StaticShadowPages"},
                {".dds","DdsTexture"}, {".spkg","SceneProbe"}, {".dxil","ShaderDxil"}, {".hsbin","VfxMeshAtlas"}}})
                if(id.ends_with(suffix))format=token;
            Require(!format.empty() && (!id.ends_with(".hsbin") || id=="vfx/vfx_mesh_atlas.hsbin"), "unknown renderer bulk format");
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
    write(std::string(64,'\0'));write(pcm);write(font);
    std::uint64_t extent=64ull+pcm.size()+font.size();
    std::string bulk_source="\n#include <hs/render_data/render_bulk.hpp>\nnamespace hs::shipping { namespace {\nconstinit const render_data::RendererBulkDescriptor kRendererBulks[]{\n";
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
        if(bulk.id.starts_with("render/texture/") && bulk.id.ends_with(".rpkg"))
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
        else bulk.snapshot->CopyProductBytesTo(bulk.id,output,dependency);
        ++bulk_index;
        Require(dependency.size<=UINT64_MAX-aligned,"renderer bulk extent overflow");
        extent=aligned+dependency.size;
        bulk_source+="{{"+std::to_string(asset)+"ull},{"+std::to_string(aligned)+"ull,"+std::to_string(dependency.size)+"ull,render_data::RenderBulkFormat::"+bulk.format+",{";
        std::array<std::byte,32> expected_digest{};
        for(std::size_t i=0;i<32;++i)
        {
            unsigned byte{};const auto first=dependency.digest.data()+7+i*2;
            const auto [end,error]=std::from_chars(first,first+2,byte,16);
            Require(error==std::errc{} && end==first+2,"invalid bulk digest");
            expected_digest[i]=static_cast<std::byte>(byte);
            if(i)bulk_source+=',';
            bulk_source+="std::byte{"+std::to_string(byte)+"}";
        }
        bulk_source+="}}},\n";
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
            Require(bulk.format=="ShaderDxil","unknown bulk descriptor format");return F::ShaderDxil;
        }();
        bulk_descriptors.push_back({hs::AssetId{asset},{aligned,dependency.size,
            native_format,expected_digest}});
        spec.upstream.push_back(std::move(dependency));
    }
    bulk_source+="};\n}\nstd::span<const render_data::RendererBulkDescriptor> RendererBulks() noexcept { return kRendererBulks; }\n}\n";
    Require(vfx_mesh_atlas.has_value(),"runtime renderer closure omits VFX mesh atlas");
    bulk_source+="\n#include <bit>\n#include <hs/render_data/vfx_mesh_descriptor.hpp>\nnamespace hs::shipping { namespace {\n"
        "constinit const render_data::VfxMeshAtlasDescriptor kVfxMeshAtlas{{"+
        std::to_string(vfx_mesh_atlas->asset_id.value)+"ull},{{\n";
    for(const auto &mesh:vfx_mesh_atlas->meshes)
    {
        const auto bits=[](float value) { return "std::bit_cast<float>(std::uint32_t{"+
            std::to_string(std::bit_cast<std::uint32_t>(value))+"u})"; };
        bulk_source+="{"+std::to_string(mesh.vertex_count)+"u,"+bits(mesh.transverse_radius)+","+
            bits(mesh.minimum_z)+","+bits(mesh.widest_z)+","+bits(mesh.maximum_z)+"},\n";
    }
    bulk_source+="}}};\n}\nconst render_data::VfxMeshAtlasDescriptor &RendererVfxMeshAtlas() noexcept { return kVfxMeshAtlas; }\n}\n";
    std::size_t dependency_count{},layout_count{};
    for(const auto &texture:textures)
    { dependency_count+=texture.dependencies.size();layout_count+=texture.subresources.size(); }
    bulk_source+="\n#include <array>\n#include <hs/render_data/texture_descriptor.hpp>\nnamespace hs::shipping { namespace {\n";
    bulk_source+="constinit const std::array<AssetId,"+std::to_string(dependency_count)+"> kTextureDependencies{{\n";
    for(const auto &texture:textures)for(const auto id:texture.dependencies)
        bulk_source+="{"+std::to_string(id.value)+"ull},\n";
    bulk_source+="}};\nconstinit const std::array<render_data::TextureSubresourceLayout,"+std::to_string(layout_count)+"> kTextureLayouts{{\n";
    for(const auto &texture:textures)for(const auto &layout:texture.subresources)
        bulk_source+="{"+std::to_string(layout.offset)+"ull,"+std::to_string(layout.row_pitch)+"u,"+std::to_string(layout.slice_pitch)+"u,"+std::to_string(layout.byte_count)+"ull},\n";
    bulk_source+="}};\nconstinit const std::array<render_data::TextureDescriptor,"+std::to_string(textures.size())+"> kTextures{{\n";
    std::size_t dependency_first{},layout_first{};
    for(const auto &texture:textures)
    {
        const auto &d=texture.description;
        bulk_source+="{{"+std::to_string(texture.asset_id.value)+"ull},"+std::to_string(texture.bulk_index)+"u,{"+
            std::to_string(d.width)+"u,"+std::to_string(d.height)+"u,"+std::to_string(d.array_size)+"u,"+
            std::to_string(d.mip_count)+"u,"+std::to_string(d.format)+"u},"+
            "std::span<const AssetId>(kTextureDependencies).subspan("+std::to_string(dependency_first)+","+std::to_string(texture.dependencies.size())+"),"+
            "std::span<const render_data::TextureSubresourceLayout>(kTextureLayouts).subspan("+std::to_string(layout_first)+","+std::to_string(texture.subresources.size())+"),render_data::TextureDimension::"+
            (texture.dimension==hs::render_data::TextureDimension::Texture3D ? "Texture3D" : "Texture2DArray")+","+std::to_string(texture.depth)+"u},\n";
        dependency_first+=texture.dependencies.size();layout_first+=texture.subresources.size();
    }
    bulk_source+="}};\n}\nstd::span<const render_data::TextureDescriptor> RendererTextures() noexcept { return kTextures; }\n}\n";
    bulk_source+=GeometrySource(geometries);
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
    bulk_source+=MaterialSource(materials);
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
    bulk_source+=CharacterSource(characters,animations);
    const auto revision_name=[](std::string_view value) {
        std::string text="\"";
        for(unsigned char c:value)
        {
            if(c<32 || c>=127){text+='\\';text+=char('0'+(c>>6));text+=char('0'+((c>>3)&7));text+=char('0'+(c&7));}
            else {if(c=='"'||c=='\\')text+='\\';text+=char(c);}
        }
        return text+'"';
    };
    bulk_source+="\n#include <hs/render_data/render_revision.hpp>\nnamespace hs::shipping { namespace {\n";
    bulk_source+="constinit const std::array<render_data::RendererSourceRevision,"+std::to_string(source_revisions.size())+"> kRendererSourceRevisions{{\n";
    for(const auto &[asset,revision]:source_revisions)
    {
        Require(revision.digest.starts_with("sha256:") && revision.digest.size()==71,"invalid source revision digest");
        bulk_source+="{{"+std::to_string(asset)+"ull},"+revision_name(revision.logical_id)+",{";
        for(std::size_t i=0;i<32;++i)
        {
            unsigned byte{};const auto first=revision.digest.data()+7+i*2;
            const auto [end,error]=std::from_chars(first,first+2,byte,16);
            Require(error==std::errc{} && end==first+2,"invalid source revision digest byte");
            bulk_source+="std::byte{"+std::to_string(byte)+"},";
        }
        bulk_source+="}},\n";
    }
    std::string revision_rows;std::size_t revision_count{};std::uint32_t revision_index{};
    for(const auto &[asset,bulk]:bulks)
    {
        if(rd::IsRendererMaterialRevisionProduct(bulk.id))
        {revision_rows+="{"+revision_name(bulk.id)+","+std::to_string(revision_index)+"u},\n";++revision_count;}
        ++revision_index;
    }
    bulk_source+="}};\nconstinit const std::array<render_data::RendererRevisionBulk,"+std::to_string(revision_count)+"> kRendererRevisionBulks{{\n"+revision_rows+"}};\n}\n";
    bulk_source+="std::span<const render_data::RendererSourceRevision> RendererSourceRevisions() noexcept { return kRendererSourceRevisions; }\n";
    bulk_source+="std::span<const render_data::RendererRevisionBulk> RendererRevisionBulks() noexcept { return kRendererRevisionBulks; }\n}\n";
    std::ranges::sort(spec.upstream,{},&ap::UpstreamProduct::logical_id);
    for(std::size_t i=1;i<spec.upstream.size();++i)
        if(spec.upstream[i-1].logical_id==spec.upstream[i].logical_id)
            Require(spec.upstream[i-1].digest==spec.upstream[i].digest && spec.upstream[i-1].size==spec.upstream[i].size,
                "character and renderer inputs disagree");
    spec.upstream.erase(std::unique(spec.upstream.begin(),spec.upstream.end(),[](const auto &a,const auto &b){return a.logical_id==b.logical_id;}),spec.upstream.end());
    const auto action_key = ap::Sha256Bytes(Bytes(ap::ActionKey(spec)+bulk_source));
    Require(action_key.starts_with("sha256:") && action_key.size() == 71,
            "invalid shipping action key");
    const auto build_id = action_key.substr(7);
    const auto font_offset = 64 + pcm.size();
    const auto header = Header(build_id, extent);
    output.seekp(0);write(header);output.flush();Require(bool(output),"cannot flush shipping pack");
    output.close();Require(bool(output),"cannot close shipping pack");
    source += ImageSource(header, content_hash, font_offset, font.size(), font_digest);
    source += bulk_source;
    const auto shipping = build_root / "Shipping";
    ap::MakeDirectory(shipping);
    // Atomic replacement preserves previously opened handles. The pair is not a
    // transaction: an interrupted publish is rejected by exact header matching.
    ap::AtomicPublishFile(temporary, pack_path);
    ap::AtomicWriteIfDifferent(shipping / "content.generated.cpp", source);
    std::cout << "shipping-image published " << build_id << '\n';
    }
    catch (...) { ap::RemoveStage(stage); throw; }
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
