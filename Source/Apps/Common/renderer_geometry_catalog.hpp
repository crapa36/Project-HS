#pragma once

#include <hs/render_data/geometry_descriptor.hpp>
#include <hs/render_data/character_package.hpp>
#include <hs/render_data/character_animation_extension.hpp>
#include <hs/render_data/geometry_hierarchy.hpp>
#include <hs/render_data/scene_hlod.hpp>
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
        bool compiled{};
    };
    std::map<std::uint64_t,std::string> closure_;
    std::map<std::uint64_t,PageContext> pages_;
    std::vector<render_data::AdmittedGeometryHierarchy> hierarchies_;
    std::vector<render_data::AdmittedSceneHlod> scenes_;
    std::vector<PreparedCharacter> characters_;
    std::vector<PreparedAnimation> animations_;
    static void Require(bool condition,const char *message)
    { if(!condition) throw std::runtime_error(message); }
    void RequireDependency(std::string_view id) const
    {
        const auto found=closure_.find(MakeAssetId(id).value);
        Require(found!=closure_.end() && found->second==id,"geometry dependency missing from runtime closure");
    }
    PageContext &Context(std::string_view id,std::uint32_t flags)
    {
        RequireDependency(id);
        Require(id.ends_with(".rpage"),"geometry context does not name a page");
        auto [entry,inserted]=pages_.try_emplace(MakeAssetId(id).value);
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
    explicit RendererGeometryCatalog(std::span<const std::string> closure_product_ids)
    {
        for(const auto &id:closure_product_ids)
        {
            const auto [entry,inserted]=closure_.emplace(MakeAssetId(id).value,id);
            Require(inserted || entry->second==id,"runtime closure AssetId collision");
        }
    }
    static bool NeedsMetadata(std::string_view id) noexcept
    {
        return id.starts_with("render/") && ((id.ends_with(".rpkg") && !id.starts_with("render/texture/")) ||
            id.ends_with(".ghbin") || id.ends_with(".shl1"));
    }
    void AddMetadata(std::string_view id,std::span<const std::byte> bytes)
    {
        namespace rd=render_data;
        RequireDependency(id);
        Require(NeedsMetadata(id),"unsupported geometry context product");
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
    [[nodiscard]] render_data::GeometryDescriptorStorage CompilePage(std::string_view id,
        std::span<const std::byte> bytes,std::uint32_t bulk_index)
    {
        namespace rd=render_data;
        RequireDependency(id);
        const auto found=pages_.find(MakeAssetId(id).value);
        Require(found!=pages_.end(),"geometry page has no validated runtime context");
        auto &entry=found->second;
        auto prepared=entry.direct ? rd::PrepareGeometryPage(bytes,MakeAssetId(id),*entry.direct) :
            rd::PrepareGeometryPagePayload(bytes,MakeAssetId(id),entry.flags);
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
        entry.compiled=true;
        return compiled;
    }
    [[nodiscard]] std::vector<PreparedCharacter> TakeCharacters()
    {
        for(const auto &[id,page]:pages_) Require(page.compiled,"character admission precedes geometry completion");
        return std::move(characters_);
    }
    [[nodiscard]] std::vector<PreparedAnimation> TakeAnimations()
    {
        for(const auto &[id,page]:pages_) Require(page.compiled,"animation admission precedes geometry completion");
        return std::move(animations_);
    }
};
}
