#pragma once

#include <hs/render_data/geometry_descriptor.hpp>
#include <hs/render_data/character_package.hpp>
#include <hs/render_data/character_animation_extension.hpp>
#include <hs/render_data/geometry_hierarchy.hpp>
#include <hs/render_data/scene_hlod.hpp>
#include <hs/render_data/render_revision.hpp>
#include <hs/render_data/material_schema.hpp>
#include <hs/render_data/texture_descriptor.hpp>
#include <algorithm>
#include <map>
#include <optional>
#include <stdexcept>

namespace hs::apps
{
// One offline admission invocation; owns contexts, never native objects or page bytes.
class RendererGeometryCatalog
{
public:
    // Borrowed bytes must remain alive through descriptor compilation.
    struct PreparedCharacter
    {
        std::string product_id;
        std::span<const std::byte> bytes;
        render_data::PackageView package;
        render_data::CharacterMetadataPackageData data;
    };
    struct PreparedAnimation
    {
        std::string product_id;
        std::span<const std::byte> bytes;
        render_data::PackageView package;
        render_data::CharacterAnimationExtensionData data;
    };
private:
    struct PageContext
    {
        std::uint32_t flags{};
        std::optional<render_data::GeometryMetadata> direct;
        std::vector<std::pair<std::size_t,std::uint32_t>> hierarchies, scenes;
        std::vector<std::size_t> characters;
        std::optional<float> ground_offset;
    };
    const std::map<std::uint64_t,std::string_view> &closure_;
    std::map<std::uint64_t,PageContext> pages_;
    std::vector<render_data::AdmittedGeometryHierarchy> hierarchies_;
    std::vector<render_data::AdmittedSceneHlod> scenes_;
    std::vector<PreparedCharacter> characters_;
    std::vector<PreparedAnimation> animations_;
    static void Require(bool condition,const char *message)
    { if(!condition) throw std::runtime_error(message); }
    AssetId RequireDependency(std::string_view id) const
    {
        const auto asset=MakeAssetId(id);
        const auto found=closure_.find(asset.value);
        Require(found!=closure_.end() && found->second==id,"geometry dependency missing from runtime closure");
        return asset;
    }
    PageContext &Context(std::string_view id,std::uint32_t flags)
    {
        const auto asset=RequireDependency(id);
        Require(id.starts_with("render/") && id.ends_with(".rpage"),"geometry context does not name a renderer page");
        auto [entry,inserted]=pages_.try_emplace(asset.value);
        Require(inserted || entry->second.flags==flags,"conflicting geometry page flags");
        entry->second.flags=flags;
        return entry->second;
    }
    PageContext &Direct(const render_data::GeometryMetadata &m,const render_data::PackageView &package,std::string_view id)
    {
        Require(m.page_dependency_index<package.dependencies.size(),"geometry page dependency out of range");
        auto &entry=Context(package.dependencies[m.page_dependency_index],m.flags);
        if(entry.direct)
        {
            // Compare fields through the codec, never C++ padding. Dependency
            // table ordinals are local to each metadata owner.
            auto previous=*entry.direct, current=m;
            previous.page_dependency_index=current.page_dependency_index;
            previous.fallback_dependency_index=current.fallback_dependency_index;
            previous.terminal_fallback_dependency_index=current.terminal_fallback_dependency_index;
            Require(render_data::EncodeGeometryMetadata(previous,package.dependencies,id)==render_data::EncodeGeometryMetadata(current,package.dependencies,id),
                    "conflicting direct geometry metadata");
        }
        else entry.direct=m;
        return entry;
    }
public:
    explicit RendererGeometryCatalog(const std::map<std::uint64_t,std::string_view> &closure)
        :closure_(closure) {}
    static bool NeedsMetadata(std::string_view id) noexcept
    {
        return !render_data::IsTerrainSurfaceProduct(id) && id.starts_with("render/") && ((id.ends_with(".rpkg") && !id.starts_with("render/texture/")) ||
            id.ends_with(".ghbin") || id.ends_with(".shl1"));
    }
    void AddMetadata(std::string_view id,std::span<const std::byte> bytes)
    {
        namespace rd=render_data;
        if(id.ends_with(".ghbin"))
        {
            auto &owner=hierarchies_.emplace_back(bytes);
            const auto &h=owner.Value();
            for(std::uint32_t i=0;i<h.pages.size();++i)
                Context(h.pages[i].logical_id,h.geometry_flags).hierarchies.emplace_back(hierarchies_.size()-1,i);
        }
        else if(id.ends_with(".shl1"))
        {
            auto &owner=scenes_.emplace_back(bytes);
            const auto &s=owner.Value();
            for(const auto &dependency:s.dependencies) RequireDependency(dependency);
            for(std::uint32_t i=0;i<s.pages.size();++i)
                Context(s.pages[i].logical_id,s.pages[i].geometry_flags).scenes.emplace_back(scenes_.size()-1,i);
        }
        else
        {
            auto package=rd::ReadPackage(bytes,MakeAssetId(id));
            for(const auto &dependency:package.dependencies) RequireDependency(dependency);
            if(std::ranges::find(package.chunks,rd::ChunkType::CharacterAnimationMetadata,&rd::ChunkView::type)!=package.chunks.end())
            {
                auto data=rd::DecodeCharacterAnimationExtension(package);
                animations_.push_back({std::string(id),bytes,std::move(package),std::move(data)});
                return;
            }
            const auto geometry=std::ranges::find(package.chunks,rd::ChunkType::GeometryMetadata,&rd::ChunkView::type);
            if(geometry==package.chunks.end()) return;
            if(std::ranges::find(package.chunks,rd::ChunkType::CharacterMetadata,&rd::ChunkView::type)!=package.chunks.end())
            {
                auto data=rd::DecodeCharacterMetadataPackage(package,id=="render/character/archer.rpkg");
                Require(id.starts_with("render/character/") && id.ends_with(".rpkg"),"invalid character identity");
                Require((id=="render/character/archer.rpkg")==!data.monster,"character product has the wrong deformation representation");
                Require(std::ranges::none_of(package.dependencies,[](const std::string &dependency) {
                    return dependency.ends_with(".meshbin") || dependency.ends_with("_materials.json");
                }),"legacy character exchange dependency");
                auto &character=characters_.emplace_back(PreparedCharacter{std::string(id),bytes,package,std::move(data)}).data;
                const auto attach=[&](const rd::GeometryMetadata &m) {
                    auto &entry=Direct(m,package,id);
                    entry.characters.push_back(characters_.size()-1);
                    if(id=="render/character/archer.rpkg") entry.ground_offset=character.ground_offset;
                };
                attach(character.geometry);
                if(character.monster) for(const auto &lod:character.monster->lods)
                    if(lod.page_dependency_index!=character.geometry.page_dependency_index) attach(lod);
            }
            else Direct(rd::DecodeGeometryMetadata(geometry->bytes,package.dependencies,id),package,id);
        }
    }
    [[nodiscard]] render_data::GeometryDescriptorStorage CompilePage(AssetId asset,
        std::span<const std::byte> bytes,std::uint32_t bulk_index)
    {
        namespace rd=render_data;
        const auto found=pages_.find(asset.value);
        Require(found!=pages_.end(),"geometry page has no validated runtime context");
        auto &entry=found->second;
        auto prepared=entry.direct ? rd::PrepareGeometryPage(bytes,asset,*entry.direct) :
            rd::PrepareGeometryPagePayload(bytes,asset,entry.flags);
        for(const auto &dependency:prepared.package.dependencies) RequireDependency(dependency);
        for(const auto [owner,index]:entry.hierarchies) hierarchies_[owner].ValidatePage(index,prepared);
        for(const auto [owner,index]:entry.scenes) scenes_[owner].ValidatePage(index,prepared);
        for(const auto owner:entry.characters)
        {
            Require(prepared.package.dependencies.empty(),"direct character page has intermediate dependencies");
            auto &character=characters_[owner].data;
            const auto original=character.geometry;
            character.geometry=*entry.direct;
            try { rd::ValidateCharacterGeometry(character,prepared); }
            catch(...) { character.geometry=original;throw; }
            character.geometry=original;
        }
        auto compiled=rd::CompileGeometryDescriptor(bytes,prepared,bulk_index);
        if(entry.ground_offset) compiled.ground_support=rd::BuildGroundSupport(prepared.geometry.vertices,*entry.ground_offset);
        return compiled;
    }
    void ValidateCharacterBindings(std::span<const render_data::TextureDescriptor> textures) const
    {
        namespace rd=render_data;
        Require(!characters_.empty(),"character catalog is empty");
        constexpr std::string_view prefix="render/character/";
        for(const auto &character:characters_)
        {
            const auto stem=character.product_id.substr(prefix.size(),character.product_id.size()-prefix.size()-5);
            const auto texture=[&](const std::string &id,const std::string &source) {
                Require(std::ranges::find(character.package.dependencies,id)!=character.package.dependencies.end(),
                    "missing character texture dependency");
                const auto asset=MakeAssetId(id);
                const auto found=std::ranges::find_if(textures,[&](const auto &value) { return value.asset_id.value==asset.value; });
                Require(found!=textures.end(),"missing character texture descriptor");
                const auto &description=found->description;
                std::uint32_t full_mips=1;
                for(auto side=std::max(description.width,description.height);side>1;side>>=1) ++full_mips;
                const bool linear=source.find("_normal_")!=std::string::npos || source=="character/monster_ram.dds";
                Require(description.array_size==1 && description.mip_count==full_mips &&
                    description.format==(linear ? rd::kTextureFormatBc7 : rd::kTextureFormatBc7Srgb) &&
                    found->dependencies.size()==1 && found->dependencies[0].value==MakeAssetId(source).value,
                    "character texture dependency disagrees with its FBX slot");
            };
            for(std::uint32_t slot=0;slot<character.data.material_count;++slot)
                for(const auto *semantic:{"diffuse","normal"})
                {
                    const auto name=stem+"_"+semantic+"_"+std::to_string(slot);
                    texture("render/texture/"+name+".rpkg","character/"+name+".dds");
                }
            const bool monster_pbr=std::ranges::any_of(character.data.material_bindings,[](const auto &binding) {
                return rd::kCharacterMaterialNames[binding.hero_ordinal]=="monster_pbr";
            });
            if(monster_pbr)
                for(const auto *name:{"monster_basecolor","monster_emissive","monster_ram"})
                    texture("render/texture/"+std::string(name)+".rpkg","character/"+std::string(name)+".dds");
            if(stem=="archer")
            {
                constexpr std::string_view id="render/animation/archer.rpkg";
                Require(std::ranges::find(character.package.dependencies,id)!=character.package.dependencies.end(),
                    "Archer metadata is missing its named-animation dependency");
                const auto animation=std::ranges::find(animations_,id,&PreparedAnimation::product_id);
                Require(animation!=animations_.end(),"missing Archer animation descriptor");
                Require(animation->package.dependencies.empty(),"direct character animation must not depend on a legacy exchange");
                Require(animation->data.sample_rate_hz==60 && animation->data.clips.size()==11 && animation->data.sockets.empty(),
                    "authored Archer animation extension shape mismatch");
            }
        }
    }
    [[nodiscard]] std::vector<PreparedCharacter> TakeCharacters()
    {
        return std::move(characters_);
    }
    [[nodiscard]] std::vector<PreparedAnimation> TakeAnimations()
    {
        return std::move(animations_);
    }
};
}
