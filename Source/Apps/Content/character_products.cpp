#include "character_products.hpp"
#include "geometry_cooker.hpp"
#include <hs/render_data/material_ir.hpp>
#include <hs/render_data/texture_package.hpp>
#include <hs/core/sha256.hpp>
#ifdef _WIN32
#include <DirectXMath.h>
#endif

#include <algorithm>
#include <cmath>
#include <bit>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>
#include <map>
#include <tuple>
#ifndef NDEBUG
#include <cstdio>
#endif

namespace hs::content
{
namespace
{
namespace rd = render_data;
void Require(bool value, const std::string &message)
{
    if (!value) throw std::runtime_error("Character products: " + message);
}

std::uint32_t HeroOrdinal(const std::string &name)
{
    const auto found = std::ranges::find(rd::kCharacterMaterialNames, name);
    Require(found != rd::kCharacterMaterialNames.end(), "unknown authored hero material: " + name);
    return static_cast<std::uint32_t>(found - rd::kCharacterMaterialNames.begin());
}

void AppendMonsterIntervalBounds(rd::MonsterCharacterData &data,const CharacterIR &character,
    const rd::GeometryData &geometry,std::uint16_t lod)
{
    // Each shader skin row is linear between two stored final matrices. The
    // extrema of every vertex coordinate therefore occur at interval ends.
    constexpr double u=0x1p-24;
    constexpr double gamma=(64*u)/(1-64*u);
    for(const auto &clip:character.clips)
        for(std::uint32_t frame=0;frame<clip.frame_count;++frame)
        {
            rd::CharacterIntervalBound bound; bound.lod=lod;bound.clip=clip.clip;bound.frame=frame;
            std::array<double,3> low{INFINITY,INFINITY,INFINITY},high{-INFINITY,-INFINITY,-INFINITY},magnitude{};
            double position_l1{};
            for(const auto &vertex:geometry.vertices)
            {
                position_l1=std::max(position_l1,std::abs(double(vertex.position[0]))+
                    std::abs(double(vertex.position[1]))+std::abs(double(vertex.position[2])));
                for(const auto endpoint:{frame,std::min(frame+1,clip.frame_count-1)})
                    for(std::size_t row=0;row<3;++row)
                    {
                        double position{},absolute{};
                        for(std::size_t column=0;column<4;++column)
                        {
                            double matrix{},matrix_absolute{};
                            for(std::size_t lane=0;lane<4;++lane)
                            {
                                const auto &m=data.final_matrices[clip.first_transform+endpoint*character.bone_count+vertex.bone_indices[lane]];
                                const double term=double(m[row*4+column])*vertex.bone_weights[lane];
                                matrix+=term; matrix_absolute+=std::abs(term);
                            }
                            const double p=column==3?1.0:double(vertex.position[column]);
                            position+=matrix*p;absolute+=matrix_absolute*std::abs(p);
                        }
                        low[row]=std::min(low[row],position);high[row]=std::max(high[row],position);
                        magnitude[row]=std::max(magnitude[row],absolute);
                    }
            }
            for(std::size_t row=0;row<3;++row)
            {
                const double margin=gamma*magnitude[row]+64*double(std::numeric_limits<float>::min())*(1+position_l1);
                const double pad=margin+data.bubble_amplitude;
                if(!std::isfinite(low[row]) || !std::isfinite(high[row]) ||
                    magnitude[row]>=std::numeric_limits<float>::max()/128 ||
                    std::max(std::abs(low[row]-pad),std::abs(high[row]+pad))>=std::numeric_limits<float>::max()/4)
                {
                    data.flags&=~rd::kMonsterBoundsPositionEnclosure;
                    bound.minimum[row]=-std::numeric_limits<float>::max();
                    bound.maximum[row]=std::numeric_limits<float>::max();
                    continue;
                }
                data.numerical_margin=std::max(data.numerical_margin,
                    std::nextafter(static_cast<float>(margin),std::numeric_limits<float>::infinity()));
                bound.minimum[row]=std::nextafter(static_cast<float>(low[row]-pad),-std::numeric_limits<float>::infinity());
                bound.maximum[row]=std::nextafter(static_cast<float>(high[row]+pad),std::numeric_limits<float>::infinity());
            }
            data.interval_bounds.push_back(bound);
        }
}

std::vector<rd::ExpandedVertex> ExpandGeometry(const rd::GeometryData &geometry)
{
    std::vector<rd::ExpandedVertex> expanded;expanded.reserve(geometry.indices.size());
    for(const auto index:geometry.indices) expanded.push_back(geometry.vertices[index]);
    return expanded;
}

rd::GeometryMetadata DescribeGeometry(const rd::GeometryData &geometry,
    std::uint16_t lod,std::uint16_t page_dependency,float error)
{
    rd::GeometryMetadata metadata;
    metadata.flags=geometry.flags;
    metadata.vertex_count=static_cast<std::uint32_t>(geometry.vertices.size());
    metadata.index_count=static_cast<std::uint32_t>(geometry.indices.size());
    metadata.submesh_count=static_cast<std::uint32_t>(geometry.submeshes.size());
    metadata.meshlet_count=static_cast<std::uint32_t>(geometry.meshlets.size());
    metadata.vertex_ref_count=static_cast<std::uint32_t>(geometry.meshlet_vertex_refs.size());
    metadata.bounds=geometry.bounds;metadata.lod=lod;
    metadata.page_dependency_index=page_dependency;metadata.geometric_error=error;
    metadata.error_method_abi=lod?rd::kSkinnedPoseErrorMethodAbi:rd::kGeometryErrorMethodAbi;
    for(const auto &vertex:geometry.vertices)
        metadata.max_material_index=std::max(metadata.max_material_index,std::uint32_t(vertex.material_index));
    return metadata;
}

std::array<double,3> SkinPosition(const rd::ExpandedVertex &vertex,
    std::span<const std::array<float,16>> palette)
{
    std::array<double,3> result{};
    for(std::size_t row=0;row<3;++row)
        for(std::size_t column=0;column<4;++column)
        {
            double matrix{};
            for(std::size_t lane=0;lane<4;++lane)
                matrix+=double(palette[vertex.bone_indices[lane]][row*4+column])*vertex.bone_weights[lane];
            result[row]+=matrix*(column==3?1.0:double(vertex.position[column]));
        }
    return result;
}

struct SkinnedLodCertificate { double position{},uv{},normal{},tangent{}; bool early_rejection{}; };
SkinnedLodCertificate CertifyMonsterLod(const CharacterIR &character,
    const rd::MonsterCharacterData &monster,
    std::span<const rd::ExpandedVertex> fine,std::span<const rd::ExpandedVertex> coarse,
    double position_limit)
{
    SkinnedLodCertificate certificate;
    const double margin=4.0*monster.numerical_margin+2.0*monster.bubble_amplitude+
        1e-5*std::max(1.0,double(monster.lods.front().bounds.sphere[3]));
    using Bary=std::array<double,3>;
    struct Patch { std::size_t triangle; std::array<Bary,3> corners; };
    const auto direction=[&](std::span<const rd::ExpandedVertex> source,
                             std::span<const rd::ExpandedVertex> target) {
        std::vector<Patch> patches;
        for(std::size_t triangle=0;triangle<source.size()/3;++triangle)
            patches.push_back({triangle,{{Bary{1,0,0},Bary{0,1,0},Bary{0,0,1}}}});
        for(unsigned depth=0;!patches.empty();++depth)
        {
            std::vector<rd::ExpandedVertex> expanded;
            expanded.reserve(patches.size()*3);
            for(const auto &patch:patches)
                for(const auto &q:patch.corners)
                {
                    auto v=source[patch.triangle*3];
                    for(std::size_t axis=0;axis<3;++axis) {
                        double p{},n{};for(std::size_t k=0;k<3;++k) {
                            p+=q[k]*source[patch.triangle*3+k].position[axis];
                            n+=q[k]*source[patch.triangle*3+k].normal[axis];}
                        v.position[axis]=static_cast<float>(p);v.normal[axis]=static_cast<float>(n);
                    }
                    for(std::size_t axis=0;axis<2;++axis) {
                        double u{};for(std::size_t k=0;k<3;++k) u+=q[k]*source[patch.triangle*3+k].uv[axis];
                        v.uv[axis]=static_cast<float>(u);
                    }
                    for(std::size_t axis=0;axis<4;++axis) {
                        double t{};for(std::size_t k=0;k<3;++k) t+=q[k]*source[patch.triangle*3+k].tangent[axis];
                        v.tangent[axis]=static_cast<float>(t);
                    }
                    expanded.push_back(v);
                }
            const auto witnesses=geometry_content::BuildSurfaceWitnesses(expanded,target);
            std::vector<SkinnedLodCertificate> errors(patches.size());
            for(std::size_t triangle=0;triangle<patches.size();++triangle)
            {
                const auto &patch=patches[triangle];const auto &w=witnesses[triangle];
                const auto *a=source.data()+patch.triangle*3;
                const auto *b=target.data()+w.target_triangle*3;
                for(std::size_t corner=0;corner<3;++corner) {
                    const auto &p=patch.corners[corner];const auto &q=w.target_barycentric[corner];
                    double uv{},normal{},tangent{};
                    for(std::size_t axis=0;axis<2;++axis) {
                        double delta{};for(std::size_t k=0;k<3;++k) delta+=p[k]*a[k].uv[axis]-q[k]*b[k].uv[axis];
                        uv+=delta*delta;
                    }
                    for(std::size_t axis=0;axis<4;++axis) {
                        double n{},t{};for(std::size_t k=0;k<3;++k) {
                            if(axis<3) n+=p[k]*a[k].normal[axis]-q[k]*b[k].normal[axis];
                            t+=p[k]*a[k].tangent[axis]-q[k]*b[k].tangent[axis];}
                        normal+=n*n;tangent+=t*t;
                    }
                    errors[triangle].uv=std::max(errors[triangle].uv,std::sqrt(uv));
                    errors[triangle].normal=std::max(errors[triangle].normal,std::sqrt(normal));
                    errors[triangle].tangent=std::max(errors[triangle].tangent,std::sqrt(tangent));
                }
            }
            if(depth==4)
                for(const auto &e:errors)
                    if(e.uv>0.02 || e.normal>0.25 || e.tangent>0.35) {
                        certificate=e;certificate.early_rejection=true;return false;
                    }
            std::vector<std::array<double,3>> posed_source(source.size()),posed_target(target.size());
            for(const auto &clip:character.clips)
                for(std::uint32_t frame=0;frame<clip.frame_count;++frame)
                {
                    const auto palette=std::span(monster.final_matrices).subspan(
                        clip.first_transform+std::size_t(frame)*character.bone_count,character.bone_count);
                    for(std::size_t i=0;i<source.size();++i) posed_source[i]=SkinPosition(source[i],palette);
                    for(std::size_t i=0;i<target.size();++i) posed_target[i]=SkinPosition(target[i],palette);
                    for(std::size_t triangle=0;triangle<patches.size();++triangle) {
                        const auto &patch=patches[triangle];const auto &w=witnesses[triangle];
                        const auto *a=posed_source.data()+patch.triangle*3;
                        const auto *b=posed_target.data()+w.target_triangle*3;
                        for(std::size_t corner=0;corner<3;++corner) {
                            const auto &p=patch.corners[corner];const auto &q=w.target_barycentric[corner];
                            double squared{};
                            for(std::size_t axis=0;axis<3;++axis) {
                                double delta{};for(std::size_t k=0;k<3;++k) delta+=p[k]*a[k][axis]-q[k]*b[k][axis];
                                squared+=delta*delta;
                            }
                            errors[triangle].position=std::max(errors[triangle].position,std::sqrt(squared));
                            if(depth==4 && errors[triangle].position+margin>position_limit) {
                                certificate=errors[triangle];certificate.early_rejection=true;return false;
                            }
                        }
                    }
                }
            std::vector<Patch> next;
            const auto midpoint=[](const Bary &a,const Bary &b) {
                return Bary{(a[0]+b[0])*0.5,(a[1]+b[1])*0.5,(a[2]+b[2])*0.5};
            };
            for(std::size_t i=0;i<patches.size();++i)
            {
                const auto &e=errors[i];const auto &patch=patches[i];
                if(depth<4 && (e.position+margin>position_limit || e.uv>0.02 || e.normal>0.25 || e.tangent>0.35))
                {
                    const auto &a=patch.corners[0],&b=patch.corners[1],&c=patch.corners[2];
                    const auto ab=midpoint(a,b),bc=midpoint(b,c),ca=midpoint(c,a);
                    next.push_back({patch.triangle,{a,ab,ca}});
                    next.push_back({patch.triangle,{ab,b,bc}});
                    next.push_back({patch.triangle,{ca,bc,c}});
                    next.push_back({patch.triangle,{ab,bc,ca}});
                }
                else {
                    certificate.position=std::max(certificate.position,e.position);
                    certificate.uv=std::max(certificate.uv,e.uv);
                    certificate.normal=std::max(certificate.normal,e.normal);
                    certificate.tangent=std::max(certificate.tangent,e.tangent);
                }
            }
            patches=std::move(next);
        }
        return true;
    };
    // Each patch maps affinely into one target triangle, including after
    // skinning and palette interpolation; endpoint norms bound its interior.
    if(direction(fine,coarse)) direction(coarse,fine);
    certificate.position=std::nextafter(certificate.position+margin,INFINITY);
    return certificate;
}
} // namespace

render_data::MonsterCharacterData CookMonsterCharacterData(const CharacterIR &character,
    const render_data::GeometryData &geometry, const render_data::GeometryMetadata &metadata,
    bool suicide_bubble)
{
#ifndef _WIN32
    (void)character; (void)geometry; (void)metadata; (void)suicide_bubble;
    throw std::runtime_error("Monster palette cooking requires the native DirectXMath float contract");
#else
    rd::ValidateCharacterSource(character,false);
    rd::ValidateGeometryPage(metadata,geometry);
    Require(character.bone_names.size()==character.bone_count,"monster skeleton name count mismatch");
    for (const auto &vertex : geometry.vertices)
    {
        float sum{};
        for (std::size_t lane=0;lane<4;++lane)
        {
            Require(vertex.bone_indices[lane]<character.bone_count &&
                std::isfinite(vertex.bone_weights[lane]) && vertex.bone_weights[lane]>=0 && vertex.bone_weights[lane]<=1,
                "invalid final geometry skin influence");
            sum+=vertex.bone_weights[lane];
        }
        Require(std::abs(sum-1.0f)<=0.001f,"final geometry skin weights are not normalized");
    }
    rd::MonsterCharacterData data; data.bone_count=character.bone_count; data.lods.push_back(metadata);
    data.flags=suicide_bubble?rd::kMonsterDeformationSuicideBubble:rd::kMonsterBoundsPositionEnclosure;
    data.bubble_amplitude=suicide_bubble?0.006f:0.0f;
    hs::Sha256Stream identity;
    const auto integer=[&](std::uint64_t value,std::size_t width) {
        std::array<std::byte,8> bytes{};
        for(std::size_t i=0;i<width;++i) bytes[i]=std::byte((value>>(i*8))&255);
        identity.Add(std::span(bytes).first(width));
    };
    integer(rd::kMonsterDeformationAbi,4); integer(character.bone_count,4);
    for(std::size_t bone=0;bone<character.bone_count;++bone)
    {
        const auto &name=character.bone_names[bone]; integer(name.size(),4);
        identity.Add(std::as_bytes(std::span(name.data(),name.size())));
        integer(character.parents[bone],2);
        for(float value:character.inverse_bind_matrices[bone]) integer(std::bit_cast<std::uint32_t>(value),4);
    }
    data.skeleton_binding=identity.Finish();
    data.final_matrices.reserve(character.transforms.size());
    // This is the former renderer initialization sequence, including its float
    // stores between parent steps, matrix convention and positive determinant
    // admission. Raw FBX double globals are not a substitute for these values.
    for(const auto &clip:character.clips)
    {
        Require(clip.first_transform==data.final_matrices.size(),"monster source clip ranges are not contiguous");
        for(std::uint32_t frame=0;frame<clip.frame_count;++frame)
        {
            std::array<DirectX::XMFLOAT4X4,kMaxCharacterBones> globals{};
            for(std::uint32_t bone=0;bone<character.bone_count;++bone)
            {
                const auto &local=character.transforms[clip.first_transform+frame*character.bone_count+bone];
                const auto local_matrix=DirectX::XMMatrixScaling(local.scale[0],local.scale[1],local.scale[2]) *
                    DirectX::XMMatrixRotationQuaternion(DirectX::XMVectorSet(local.rotation[0],local.rotation[1],local.rotation[2],local.rotation[3])) *
                    DirectX::XMMatrixTranslation(local.translation[0],local.translation[1],local.translation[2]);
                const auto parent=character.parents[bone];
                const auto global=parent==std::numeric_limits<std::uint16_t>::max()?local_matrix:
                    local_matrix*DirectX::XMLoadFloat4x4(&globals[parent]);
                DirectX::XMStoreFloat4x4(&globals[bone],global);
                DirectX::XMFLOAT4X4 inverse_bind{};
                std::memcpy(&inverse_bind,character.inverse_bind_matrices[bone].data(),sizeof(inverse_bind));
                const auto skin=DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&inverse_bind))*global;
                const auto determinant=DirectX::XMVectorGetX(DirectX::XMMatrixDeterminant(skin));
                Require(std::isfinite(determinant) && determinant>0.0001f &&
                    !DirectX::XMMatrixIsNaN(skin) && !DirectX::XMMatrixIsInfinite(skin),"Monster animation produced an invalid skin pose.");
                DirectX::XMFLOAT4X4 stored{};
                DirectX::XMStoreFloat4x4(&stored,DirectX::XMMatrixTranspose(skin));
                auto &matrix=data.final_matrices.emplace_back(); std::memcpy(matrix.data(),&stored,sizeof(stored));
            }
        }
    }
    AppendMonsterIntervalBounds(data,character,geometry,0);
    return data;
#endif
}

std::vector<CharacterMaterialSelection> PrepareCharacterMaterialSelections(
    const CharacterIR &character, std::string_view stem, const nlohmann::json &authoring)
{
    Require(authoring.is_object() && authoring.size() == 2 && authoring.at("version") == 1 &&
        authoring.at("characters").is_object() && authoring.at("characters").contains(stem),
        "missing authored character material table for " + std::string(stem));
    const auto &rules = authoring.at("characters").at(stem);
    Require(rules.is_array() && !rules.empty(), "expected material identity rules");
    Require(character.materials.size() == character.material_count, "imported material slot count mismatch");
    std::vector<CharacterMaterialSelection> result;
    for (std::size_t slot = 0; slot < character.materials.size(); ++slot)
    {
        const auto &material = character.materials[slot];
        const nlohmann::json *selected{};
        for (const auto &rule : rules)
        {
            Require(rule.is_object() && (rule.contains("name") != rule.contains("diffuse")) &&
                (rule.contains("material") != rule.contains("materials")) && rule.size() == 2,
                "invalid material identity rule");
            const auto key = rule.contains("name") ? "name" : "diffuse";
            Require(rule.at(key).is_string(), "material identity key must be a string");
            const auto matches = rule.at(key) == (rule.contains("name") ? material.name : material.diffuse.filename().string());
            if (!matches) continue;
            Require(selected == nullptr, "ambiguous material identity for slot " + std::to_string(slot));
            selected = &rule;
        }
        Require(selected != nullptr, "unknown FBX material slot " + std::to_string(slot) +
            " name=" + material.name + " diffuse=" + material.diffuse.filename().string());
        const auto append_material = [&](const std::string &material_name) {
            const auto ordinal = HeroOrdinal(material_name);
            const auto existing = std::ranges::find_if(result, [&](const auto &binding) {
                return binding.slot == slot && binding.hero_ordinal == ordinal;
            });
            Require(existing == std::ranges::end(result), "duplicate material identity for FBX slot " + std::to_string(slot));
            result.push_back({static_cast<std::uint16_t>(slot), ordinal});
        };
        if (selected->contains("material"))
        {
            Require(selected->at("material").is_string(), "material identity must be a string");
            append_material(selected->at("material").get<std::string>());
        }
        else
        {
            const auto &materials = selected->at("materials");
            Require(materials.is_array() && !materials.empty() && materials.size() <= 2,
                "materials identity list must contain one or two materials");
            for (const auto &material_name : materials)
            {
                Require(material_name.is_string(), "material identity must be a string");
                append_material(material_name.get<std::string>());
            }
        }
    }
    return result;
}

std::vector<CharacterProduct> BuildCharacterProducts(
    const CharacterIR &character, std::string_view stem_view,
    std::span<const CharacterMaterialSelection> materials,
    std::span<const CharacterMaterialDomainSelection> material_domains)
{
    const std::string stem(stem_view);
    Require(!stem.empty() && std::ranges::all_of(stem, [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    }), "invalid character product stem");
    rd::ValidateCharacterSource(character, stem == "archer");
    const auto metadata_id = "render/character/" + stem + ".rpkg";
    const auto page_id = "render/geometry/character/" + stem + ".rpage";
    const auto animation_id = "render/animation/" + stem + ".rpkg";
    std::vector<rd::ExpandedVertex> expanded;
    expanded.reserve(character.vertices.size());
    for (const auto &v : character.vertices)
        expanded.push_back({v.position, v.normal, v.bone_indices, v.bone_weights,
            v.uv, v.tangent, v.material_index, v.reserved});
    using CompatibleProfile = std::tuple<std::uint8_t, std::uint32_t, std::array<std::uint32_t, 4>>;
    std::map<CompatibleProfile, std::uint32_t> profile_groups;
    std::vector<geometry_content::OpticalMaterialProfile> optical_profiles(character.material_count);
    for (std::uint32_t slot = 0; slot < character.material_count; ++slot)
    {
        std::vector<std::uint32_t> ordinals;
        for (const auto &binding : materials) if (binding.slot == slot) ordinals.push_back(binding.hero_ordinal);
        std::ranges::sort(ordinals);
        ordinals.erase(std::unique(ordinals.begin(), ordinals.end()), ordinals.end());
        if (ordinals.empty()) continue;
        std::optional<CompatibleProfile> compatible;
        bool valid = true;
        for (const auto ordinal : ordinals)
        {
            const auto domain = std::ranges::find(material_domains, ordinal,
                &CharacterMaterialDomainSelection::hero_ordinal);
            if (domain == material_domains.end())
            { valid = false; break; }
            // Opaque feature overlays never participate in the volume pass.
            if (domain->domain != rd::CharacterMaterialDomain::Translucent) continue;
            if ((domain->surface.capability_mask & static_cast<std::uint32_t>(
                    rd::MaterialClosureCapability::SolidTransmission)) == 0)
            { valid = false; break; }
            const CompatibleProfile current = domain->surface.has_static_solid_transmission_profile
                ? CompatibleProfile{1, 0, domain->surface.solid_transmission_profile}
                : CompatibleProfile{2, ordinal, {}};
            if (compatible && *compatible != current) { valid = false; break; }
            compatible = current;
        }
        if (!valid || !compatible) continue;
        auto [group, inserted] = profile_groups.try_emplace(*compatible,
            static_cast<std::uint32_t>(profile_groups.size()) + 1u);
        (void)inserted;
        optical_profiles[slot] = {group->second, true};
    }
    const auto geometry = geometry_content::CookGeometry(expanded, rd::kGeometryFlagSkinned,
        optical_profiles);
    std::vector<rd::GeometryData> lod_geometries;
    std::vector<std::pair<double,float>> lod_errors;
    std::optional<rd::MonsterCharacterData> monster;
    if(stem!="archer")
    {
        monster=CookMonsterCharacterData(character,geometry,DescribeGeometry(geometry,0,0,0),stem=="enemy_suicide");
        // Screen selection uses the certified absolute pose error. Generate
        // candidates by error budget rather than an unconditional face ratio.
        const auto fine=ExpandGeometry(geometry);
        const double radius=geometry.bounds.sphere[3];
        constexpr std::array<std::array<double,4>,2> attempts{{
            {{0.0025,0.005,0.01,0.02}},{{0.01,0.02,0.035,0.05}}}};
        constexpr std::array<double,2> pose_limits{0.025,0.06};
        for(std::uint16_t lod=1;radius>0 && lod<=2;++lod)
        {
            std::optional<rd::GeometryData> selected;
            std::optional<rd::MonsterCharacterData> selected_monster;
            std::pair<double,float> selected_error{};
            std::size_t best_indices=lod_geometries.empty()?geometry.indices.size():lod_geometries.back().indices.size();
            for(const double fraction:attempts[lod-1])
            {
                const auto simplified=geometry_content::SimplifySkinnedGeometry(geometry,static_cast<float>(radius*fraction));
#ifndef NDEBUG
                std::fprintf(stderr,"monster_lod %s lod=%u fraction=%.4f indices=%zu/%zu radius=%.9g",stem.c_str(),unsigned(lod),fraction,simplified.size(),best_indices,radius);
#endif
                if(simplified.size()>=best_indices) {
#ifndef NDEBUG
                    std::fprintf(stderr," no reduction\n");
#endif
                    continue;
                }
                auto candidate=geometry_content::CookGeometry(simplified,rd::kGeometryFlagSkinned,
                    optical_profiles);
                if(candidate.indices.size()>=best_indices ||
                    candidate.vertices.size()>=(lod_geometries.empty()?geometry.vertices.size():lod_geometries.back().vertices.size())) {
#ifndef NDEBUG
                    std::fprintf(stderr," no vertex reduction %zu\n",candidate.vertices.size());
#endif
                    continue;
                }
                auto with_bounds=*monster;
                AppendMonsterIntervalBounds(with_bounds,character,candidate,lod);
                const auto coarse=ExpandGeometry(candidate);
                const auto certificate=CertifyMonsterLod(character,with_bounds,fine,coarse,radius*pose_limits[lod-1]);
#ifndef NDEBUG
                std::fprintf(stderr," position=%.9g/%.9g uv=%.9g/0.02 normal=%.9g/0.25 tangent=%.9g/0.35 enclosure=%u%s\n",certificate.position,radius*pose_limits[lod-1],certificate.uv,certificate.normal,certificate.tangent,unsigned(bool(with_bounds.flags&rd::kMonsterBoundsPositionEnclosure)),certificate.early_rejection?" early rejection":"");
#endif
                // The same fixed surface witnesses bound the interpolated UV,
                // normal and tangent fields. Their thresholds prevent a small
                // geometric error from hiding a damaged material appearance.
                if(!(certificate.position<=radius*pose_limits[lod-1] &&
                     certificate.uv<=0.02 && certificate.normal<=0.25 &&
                     certificate.tangent<=0.35)) continue;
                if(stem!="enemy_suicide" &&
                    !(with_bounds.flags&rd::kMonsterBoundsPositionEnclosure)) continue;
                best_indices=candidate.indices.size();
                selected_error={certificate.position,with_bounds.numerical_margin};
                selected=std::move(candidate);selected_monster=std::move(with_bounds);
            }
            if(!selected) break;
            monster=std::move(*selected_monster);
            lod_geometries.push_back(std::move(*selected));
            lod_errors.push_back(selected_error);
        }
    }
    std::vector<std::string> dependencies{page_id};
    for(std::size_t lod=0;lod<lod_geometries.size();++lod)
        dependencies.push_back("render/geometry/character/"+stem+"_lod"+std::to_string(lod+1)+".rpage");
    for (std::uint32_t slot = 0; slot < character.material_count; ++slot)
        for (const auto *semantic : {"diffuse", "normal"})
        {
            const auto name = stem + "_" + semantic + "_" + std::to_string(slot);
            dependencies.push_back("character/" + name + ".dds");
            dependencies.push_back("render/texture/" + name + ".rpkg");
        }
    bool monster_pbr{};
    for (const auto &binding : materials)
    {
        Require(binding.hero_ordinal < rd::kCharacterMaterialNames.size(), "invalid hero material ordinal");
        const auto name = rd::kCharacterMaterialNames[binding.hero_ordinal];
        dependencies.push_back("render/material/" + std::string(name) + "/material.rpkg");
        monster_pbr |= name == "monster_pbr";
    }
    if (monster_pbr)
        for (const auto *name : {"monster_basecolor", "monster_emissive", "monster_ram"})
            dependencies.push_back("render/texture/" + std::string(name) + ".rpkg");
    const auto &named = character.animation_extension;
    if (!named.clips.empty())
    {
        rd::ValidateCharacterAnimationBinding(named, character, MakeAssetId(metadata_id));
        dependencies.push_back(animation_id);
    }
    std::ranges::sort(dependencies);
    dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
    Require(dependencies.size() < 65536u, "too many character dependencies");
    std::vector<rd::CharacterMaterialBinding> bindings;
    for (const auto &binding : materials)
    {
        const auto id = "render/material/" + std::string(rd::kCharacterMaterialNames[binding.hero_ordinal]) + "/material.rpkg";
        const auto domain = std::ranges::find(material_domains, binding.hero_ordinal,
            &CharacterMaterialDomainSelection::hero_ordinal);
        Require(domain != material_domains.end(), "character material domain was not derived from its upstream package");
        bindings.push_back({binding.slot,
            static_cast<std::uint16_t>(std::ranges::find(dependencies, id) - dependencies.begin()),
            binding.hero_ordinal, domain->domain});
    }
    (void)rd::EncodeCharacterMaterialBindings(bindings, dependencies, character.material_count);

    const auto page_dependency=static_cast<std::uint16_t>(
        std::ranges::find(dependencies,page_id)-dependencies.begin());
    const auto metadata=DescribeGeometry(geometry,0,page_dependency,0);
    std::vector<CharacterProduct> products;
    if(monster)
    {
        monster->lods.front()=metadata;
        for(std::size_t index=0;index<lod_geometries.size();++index)
        {
            const auto lod=static_cast<std::uint16_t>(index+1);
            const auto id="render/geometry/character/"+stem+"_lod"+std::to_string(lod)+".rpage";
            const auto dep=static_cast<std::uint16_t>(std::ranges::find(dependencies,id)-dependencies.begin());
            const double final_error=lod_errors[index].first+
                4.0*std::max(0.0,double(monster->numerical_margin)-lod_errors[index].second);
            const auto rounded=std::nextafter(static_cast<float>(final_error),std::numeric_limits<float>::infinity());
            monster->lods.push_back(DescribeGeometry(lod_geometries[index],lod,dep,
                std::max(rounded,monster->lods.back().geometric_error)));
        }
    }
    products.push_back({metadata_id, stem + ".rpkg", dependencies,
        rd::BuildCharacterMetadataPackage(MakeAssetId(metadata_id), dependencies, character, metadata, bindings,
            monster?&*monster:nullptr)});
    products.push_back({page_id, stem + ".rpage", {},
        rd::BuildGeometryPage(MakeAssetId(page_id), {}, geometry)});
    for(std::size_t index=0;index<lod_geometries.size();++index)
    {
        const auto lod=index+1;
        const auto id="render/geometry/character/"+stem+"_lod"+std::to_string(lod)+".rpage";
        products.push_back({id,stem+"_lod"+std::to_string(lod)+".rpage",{},
            rd::BuildGeometryPage(MakeAssetId(id),{},lod_geometries[index])});
    }
    if (!named.clips.empty())
        products.push_back({animation_id, stem + "_animation.rpkg", {},
            rd::BuildCharacterAnimationExtension(MakeAssetId(animation_id), {}, named)});
    return products;
}

PreparedCharacterBinding PrepareCharacterBinding(
    std::string_view logical_id, std::span<const std::byte> metadata_bytes)
{
    const auto prefix = std::string_view("render/character/");
    Require(logical_id.starts_with(prefix) && logical_id.ends_with(".rpkg"), "invalid character identity");
    auto package = rd::ReadPackage(metadata_bytes, MakeAssetId(logical_id));
    auto character = rd::DecodeCharacterMetadataPackage(package, logical_id == "render/character/archer.rpkg");
    Require((logical_id == "render/character/archer.rpkg") == !character.monster,
        "character product has the wrong deformation representation; recook the character product");
    Require(std::ranges::none_of(package.dependencies, [](const std::string &id) {
        return id.ends_with(".meshbin") || id.ends_with("_materials.json");
    }), "legacy character exchange dependency");
    return {std::string(logical_id), std::move(package), std::move(character)};
}

void ValidateCharacterProductBindings(const PreparedCharacterBinding &prepared,
    std::span<const CharacterDependencyProduct> products)
{
    const std::string_view logical_id(prepared.logical_id);
    constexpr std::string_view prefix = "render/character/";
    const std::string stem(logical_id.substr(prefix.size(), logical_id.size() - prefix.size() - 5));
    const auto bytes = [&](std::string_view id) {
        const auto found = std::ranges::find(products, id, &CharacterDependencyProduct::logical_id);
        Require(found != products.end(), "missing checked product " + std::string(id));
        Require(std::ranges::count(products, id, &CharacterDependencyProduct::logical_id) == 1,
            "duplicate checked product " + std::string(id));
        return found->bytes;
    };
    const auto &package = prepared.package;
    const auto &character = prepared.character;
    const auto &page_id = package.dependencies[character.geometry.page_dependency_index];
    const auto page = bytes(page_id);
    const auto prepared_page=rd::PrepareGeometryPage(page,MakeAssetId(page_id),character.geometry);
    Require(prepared_page.package.dependencies.empty(),
        "direct character geometry must not depend on a legacy exchange");
    rd::ValidateCharacterGeometry(character,prepared_page);
    if(character.monster)
        for(std::size_t lod=1;lod<character.monster->lods.size();++lod)
        {
            const auto &entry=character.monster->lods[lod];
            const auto &id=package.dependencies[entry.page_dependency_index];
            const auto payload=bytes(id);
            const auto prepared_lod=rd::PrepareGeometryPage(payload,MakeAssetId(id),entry);
            Require(prepared_lod.package.dependencies.empty(),
                "skinned LOD page must be an immutable direct product");
            rd::ValidateCharacterGeometry(character,entry,prepared_lod);
        }
    if (stem == "archer")
    {
        constexpr std::string_view animation_id = "render/animation/archer.rpkg";
        Require(std::ranges::find(package.dependencies, animation_id) != package.dependencies.end(),
            "Archer metadata is missing its named-animation dependency");
        const auto animation_bytes = bytes(animation_id);
        const auto animation_package=rd::ReadPackage(animation_bytes, MakeAssetId(animation_id));
        Require(animation_package.dependencies.empty(),
            "direct character animation must not depend on a legacy exchange");
        const auto animation = rd::DecodeCharacterAnimationExtension(animation_package);
        rd::ValidateCharacterAnimationBinding(animation, character, MakeAssetId(logical_id));
        Require(animation.sample_rate_hz == 60 && animation.clips.size() == 11 && animation.sockets.empty(),
            "authored Archer animation extension shape mismatch");
    }
    bool monster_pbr{};
    for (const auto &binding : character.material_bindings)
    {
        const auto name = rd::kCharacterMaterialNames[binding.hero_ordinal];
        const auto &id = package.dependencies[binding.dependency_index];
        const auto material = rd::ReadCharacterMaterialPackage(bytes(id), name);
        Require(material.domain == binding.domain, "character material binding disagrees with the cooked closure domain");
        monster_pbr |= name == "monster_pbr";
    }
    const auto texture = [&](const std::string &id, const std::string &source) {
        Require(std::ranges::find(package.dependencies, id) != package.dependencies.end(),
            "missing character texture dependency " + id);
        const auto value = rd::ReadTexturePackage(bytes(id), MakeAssetId(id));
        std::uint32_t full_mips = 1;
        for (auto side = std::max(value.description.width, value.description.height); side > 1; side >>= 1) ++full_mips;
        const bool linear = source.find("_normal_") != std::string::npos || source == "character/monster_ram.dds";
        Require(value.description.array_size == 1 && value.description.mip_count == full_mips &&
            value.description.format == (linear ? rd::kTextureFormatBc7 : rd::kTextureFormatBc7Srgb) &&
            value.dependencies == std::vector<std::string>{source},
            "character texture dependency disagrees with its FBX slot");
    };
    for (std::uint32_t slot = 0; slot < character.material_count; ++slot)
        for (const auto *semantic : {"diffuse", "normal"})
        {
            const auto name = stem + "_" + semantic + "_" + std::to_string(slot);
            texture("render/texture/" + name + ".rpkg", "character/" + name + ".dds");
        }
    if (monster_pbr)
        for (const auto *name : {"monster_basecolor", "monster_emissive", "monster_ram"})
            texture("render/texture/" + std::string(name) + ".rpkg", "character/" + std::string(name) + ".dds");
}
} // namespace hs::content
