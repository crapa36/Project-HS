#include "../Source/Apps/CharacterInterchange/character_interchange.hpp"
#include "../Source/Apps/Content/character_products.hpp"
#include <hs/render_data/texture_package.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
using Json=nlohmann::json;
void Check(bool value,const char *message) { if(!value) throw std::runtime_error(message); }
using Point=std::array<double,3>;
Point Basis(Point p) { return {-p[0],p[2],-p[1]}; }
Point Cross(Point a,Point b)
{
    return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
}
Point Sub(Point a,Point b) {return {a[0]-b[0],a[1]-b[1],a[2]-b[2]};}
void TestHandedness()
{
    // Match renderer_frame.cpp yaw180/pitch0 and DirectXMath LookAtLH:
    // right=cross(up,target-eye)=-X. Reversed-Z changes depth, not X/Y.
    const double focal=std::sqrt(3.0); // fov60, aspect1
    const auto blender=[&](Point p) {return std::array{focal*p[0]/(5+p[1]),focal*p[2]/(5+p[1])};};
    const auto runtime=[&](Point p) {return std::array{-focal*p[0]/(5-p[2]),focal*p[1]/(5-p[2])};};
    const Point left_grip{0.6,-0.25,1.2},right_quiver{-0.35,0.2,1.3};
    Check(Cross({0,1,0},{0,0,-1})==Point{-1,0,0},"runtime front camera right axis changed");
    for(auto p:{left_grip,right_quiver})
    {
        const auto source=blender(p),actual=runtime(Basis(p));
        Check(std::abs(source[0]-actual[0])<1e-12 && std::abs(source[1]-actual[1])<1e-12,
              "asymmetric source/runtime screen projection differs");
        const auto old=runtime({p[0],p[2],-p[1]});
        Check(source[0]*old[0]<0,"fixture must detect old mirrored basis");
    }
    Check(runtime(Basis(left_grip))[0]>0 && runtime(Basis(right_quiver))[0]<0,
          "anatomical left/right sockets mirrored");
    Check(Basis({0,-1,0})==Point{0,0,1},"source front normal reversed");
    const Point a=Basis({-1,0,0}),b=Basis({1,0,0}),c=Basis({0,0,1});
    Check(Cross(Sub(c,a),Sub(b,a))==Point{0,0,2},"reflected triangle requires winding reversal");
    // NDC Y is up; viewport Y is down. Positive signed area is clockwise.
    const auto screen=[&](Point p) {auto q=runtime(p);q[1]=-q[1];return q;};
    const auto sa=screen(a),sb=screen(c),sc=screen(b);
    Check((sb[0]-sa[0])*(sc[1]-sa[1])-(sb[1]-sa[1])*(sc[0]-sa[0])>0,
          "front face does not match clockwise rasterizer");
    const Point normal{0,-1,0},tangent{1,0,0},bitangent=Cross(normal,tangent);
    const auto target_normal=Basis(normal),target_tangent=Basis(tangent);
    auto target_bitangent=Basis(bitangent);
    for(auto &v:target_bitangent) v=-v; // UV V flip
    Check(Cross(target_normal,target_tangent)==target_bitangent,
          "reflection and V flip must cancel tangent sign changes");
    // materials.json canonical NormalDX stores green=-Y. Exported V-down
    // bitangent is -C*B, so those pixels need no additional runtime flip.
    const Point source_ts{0.3,0.4,std::sqrt(0.75)}; // deliberately not flat
    const auto combine=[](Point t,Point b,Point n,Point q) {
        Point result{};
        for(std::size_t i=0;i<3;++i) result[i]=t[i]*q[0]+b[i]*q[1]+n[i]*q[2];
        return result;
    };
    const auto expected=Basis(combine(tangent,bitangent,normal,source_ts));
    const Point normal_dx{source_ts[0],-source_ts[1],source_ts[2]};
    const auto actual=combine(target_tangent,target_bitangent,target_normal,normal_dx);
    const auto wrong=combine(target_tangent,target_bitangent,target_normal,source_ts);
    double correct_error{},wrong_error{};
    for(std::size_t i=0;i<3;++i)
    {
        correct_error+=std::abs(actual[i]-expected[i]);
        wrong_error+=std::abs(wrong[i]-expected[i]);
    }
    Check(correct_error<1e-12 && wrong_error>0.7,
          "NormalDX must pass unchanged; unconverted OpenGL/double flip must differ");
}
struct Fixture
{
    std::filesystem::path root=std::filesystem::temp_directory_path()/
        ("hs-character-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::vector<std::byte> binary;
    Json source;
    Fixture()
    {
        std::filesystem::create_directories(root);
        source={{"schema_version",1},{"id","character.archer.elin"},{"coordinate_system","left_handed_y_up_z_forward_meters"},
            {"bones",{{{"id","root"},{"parent",65535}},{{"id","chest"},{"parent",0}}}},
            {"materials",{{{"id","body"}}}},
            {"meshes",{{{"id","body"},{"first_vertex",0},{"vertex_count",3},{"material","body"}}}},
            {"masks",{{"upper_body",{{"required_bones",{"root","chest"}}}}}},
            {"runtime_clips",{{"idle","idle"},{"run","run_loop"},{"draw","draw_charge"},{"recoil","shoot"},{"death","death"}}},
            {"ground_offset",0.0}, {"morphs",{{"example","retained but unsupported"}}}};
        std::array<hs::render_data::ExpandedVertex,3> vertices{};
        // Asymmetric Blender points (0,0,0),(2,0,0),(0,-3,1)
        // Reflected (-x,z,-y), then reverse vertices1/2 for outward winding.
        vertices[1].position={0,1,3}; vertices[2].position={-2,0,0};
        for(auto &v:vertices) { v.normal={0,-0.9486833f,0.31622777f}; v.bone_weights={1,0,0,0}; v.tangent={-1,0,0,1}; }
        auto encoded=hs::render_data::EncodeExpandedVertices(vertices);
        Buffer("vertices","expanded_vertex_v1",76,encoded);
        std::vector<std::byte> parents{std::byte{255},std::byte{255},std::byte{0},std::byte{0}};
        Buffer("parents","u16",2,parents);
        std::vector<std::byte> inverse;
        for(int bone=0;bone<2;++bone) for(int i=0;i<16;++i) Float(inverse,i%5==0?1.0f:0.0f);
        Buffer("inverse_bind_matrices","f32x16",64,inverse);
        std::vector<std::byte> mask; Float(mask,0);Float(mask,1);Buffer("upper_body_weights","f32",4,mask);
        const std::array names{"idle","run_start","run_loop","run_stop","pivot_90_l","pivot_90_r","pivot_180_l","pivot_180_r","turn_in_place_l","turn_in_place_r","shoot","multishot","draw_charge","release_charge","arrow_rain_cast","roll_forward","retreat_shot","hit","death","revive","victory"};
        std::vector<std::byte> transforms;
        for(std::size_t i=0;i<names.size();++i)
        {
            source["clips"].push_back({{"id",names[i]},{"first_transform",i*4},{"frame_count",2},
                {"duration_seconds",1.0},{"looping",i==0 || i==2},{"markers",Json::array()},
                {"curves",{{"bow_draw",{0.0,1.0}}}}});
            for(int k=0;k<4;++k) for(float f:std::array{0.0f,0.0f,float(i),0.0f,0.0f,0.0f,1.0f,1.0f,1.0f,1.0f}) Float(transforms,f);
        }
        Buffer("local_transforms","trs_f32",40,transforms);
        for(const auto *name:{"weapon_grip","arrow_nock","projectile_visual_origin","quiver_socket","hat_socket"})
            source["sockets"].push_back({{"id",name},{"bone","chest"},{"translation",{0,0,0}},
                                        {"rotation",{0,0,0,1}},{"scale",{1,1,1}}});
        source["sockets"][0]["translation"]={-0.6,1.2,0.25}; // source left weapon grip
        source["sockets"][3]["translation"]={0.35,1.3,-0.2}; // source right/back quiver
    }
    ~Fixture() { std::error_code ignored;std::filesystem::remove_all(root,ignored); }
    static void Float(std::vector<std::byte> &bytes,float value)
    {
        auto bits=std::bit_cast<std::uint32_t>(value);
        for(int i=0;i<4;++i) bytes.push_back(std::byte((bits>>(i*8))&255));
    }
    void Buffer(const char *name,const char *type,std::size_t stride,const std::vector<std::byte> &bytes)
    {
        source["buffers"][name]={{"offset",binary.size()},{"count",bytes.size()/stride},{"stride",stride},{"type",type}};
        binary.insert(binary.end(),bytes.begin(),bytes.end());
    }
    void Save()
    {
        std::string hex;
        for(auto b:hs::render_data::Sha256(binary)) { auto v=std::to_integer<unsigned>(b);hex+="0123456789abcdef"[v>>4];hex+="0123456789abcdef"[v&15]; }
        source["binary"]={{"path","character.bin"},{"byte_length",binary.size()},{"sha256",hex}};
        Write();
        std::ofstream bin(root/"character.bin",std::ios::binary);
        bin.write(reinterpret_cast<const char *>(binary.data()),static_cast<std::streamsize>(binary.size()));
    }
    void Write() { std::ofstream(root/"character.interchange.json")<<source.dump(); }
    auto Load() { return hs::character_interchange::Load(root/"character.interchange.json"); }
};
template<class Mutate> void Reject(Mutate mutate,const char *reason)
{
    Fixture f;f.Save();mutate(f);f.Write();
    try { (void)f.Load(); } catch(const std::exception &) { return; }
    throw std::runtime_error(reason);
}

void TestAnimationExtension(const hs::character_interchange::ImportedCharacter &imported)
{
    namespace rd = hs::render_data;
    const auto asset_id = hs::MakeAssetId("render/animation/archer.rpkg");
    const auto &named = imported.runtime.animation_extension;
    Check(named.sample_rate_hz == 0 && named.bone_names == std::vector<std::string>{"root", "chest"} &&
          named.clips.size() == 21 && named.transforms.size() == 84 && named.sockets.size() == 5,
          "validated named clip or skeleton data lost");
    const auto bytes = rd::BuildCharacterAnimationExtension(asset_id, {}, named);
    const auto decoded = rd::DecodeCharacterAnimationExtension(bytes, asset_id);
    Check(decoded.clips.back().name == "victory" && decoded.clips.back().first_transform == 80 &&
          decoded.clips.back().id.value == hs::MakeAssetId("character/archer/animation/victory").value &&
          decoded.transforms.back().translation[2] == 20.0f && decoded.clips[2].looping &&
          !decoded.clips[3].looping, "named clips outside five-slot projection failed roundtrip");
    Check(decoded.sockets[0].bone_index == 1 && decoded.sockets[0].local_transform.translation[0] == -0.6f &&
          decoded.sockets[3].local_transform.translation[0] == 0.35f &&
          decoded.parents == imported.runtime.parents &&
          decoded.inverse_bind_matrices == imported.runtime.inverse_bind_matrices,
          "exact socket or skeleton binding failed roundtrip");
    auto no_sockets = named;
    no_sockets.sockets.clear();
    Check(rd::ReadPackage(rd::BuildCharacterAnimationExtension(asset_id, {}, no_sockets), asset_id).chunks.size() == 6,
          "empty authored sockets created an empty chunk");
    const auto reject = [](const auto &operation, const char *reason) {
        bool rejected{};
        try { operation(); } catch (const std::exception &) { rejected = true; }
        Check(rejected, reason);
    };
    auto invalid = named;
    invalid.clips[1].id = invalid.clips[0].id;
    reject([&] { (void)rd::BuildCharacterAnimationExtension(asset_id, {}, invalid); },
           "duplicate named clip identity accepted");
    invalid = named;
    invalid.clips.back().first_transform = 0;
    reject([&] { (void)rd::BuildCharacterAnimationExtension(asset_id, {}, invalid); },
           "named clip overlap accepted");
    rd::CharacterMetadataPackageData skeleton;
    skeleton.bone_count = imported.runtime.bone_count;
    skeleton.parents = imported.runtime.parents;
    skeleton.inverse_bind_matrices = imported.runtime.inverse_bind_matrices;
    rd::ValidateCharacterAnimationBinding(decoded, skeleton, named.character_asset_id);
    skeleton.inverse_bind_matrices[1][3] = 1.0f;
    reject([&] { rd::ValidateCharacterAnimationBinding(decoded, skeleton, named.character_asset_id); },
           "same-size different skeleton accepted");
    // Rehash the generic package after corrupting a semantic flag so this
    // exercises the extension decoder, rather than the package checksum gate.
    const auto package = rd::ReadPackage(bytes, asset_id);
    std::vector<rd::ChunkSource> chunks;
    std::vector<std::byte> invalid_clips;
    for (const auto &chunk : package.chunks)
    {
        if (chunk.type == rd::ChunkType::CharacterNamedClips)
        {
            invalid_clips.assign(chunk.bytes.begin(), chunk.bytes.end());
            invalid_clips[28] = std::byte{2};
            chunks.push_back({static_cast<std::uint32_t>(chunk.type), chunk.version,
                              chunk.required, invalid_clips, chunk.count, chunk.stride});
        }
        else chunks.push_back({static_cast<std::uint32_t>(chunk.type), chunk.version,
                               chunk.required, chunk.bytes, chunk.count, chunk.stride});
    }
    const auto malformed = rd::BuildPackage(asset_id, {}, chunks);
    reject([&] { (void)rd::DecodeCharacterAnimationExtension(malformed, asset_id); },
           "invalid named clip flag accepted");
}

void TestDirectCharacterProducts(const hs::character_interchange::ImportedCharacter &imported)
{
    namespace rd = hs::render_data;
    namespace cc = hs::content;
    auto ir = imported.runtime;
    ir.vertices[0].position[0] = -0.0f;
    ir.vertices[1].tangent[3] = -1.0f;
    const auto first_triangle = ir.vertices;
    ir.vertices.insert(ir.vertices.end(), first_triangle.begin(), first_triangle.end());
    Json authoring{{"version", 1}, {"characters", Json::object()}};
    authoring["characters"]["archer"] = Json::array({
        Json{{"name", "body"}, {"material", "archer_skin"}}});
    const auto selections = cc::PrepareCharacterMaterialSelections(ir, "archer", authoring);
    const std::array material_domains{cc::CharacterMaterialDomainSelection{0, rd::HeroMaterialDomain::Opaque}};
    const auto products = cc::BuildCharacterProducts(ir, "archer", selections, material_domains);
    const auto again = cc::BuildCharacterProducts(ir, "archer", selections, material_domains);
    Check(products.size() == 3 && again.size() == products.size(), "direct product set has a legacy exchange or fabricated LOD");
    for (std::size_t i = 0; i < products.size(); ++i)
        Check(products[i].logical_id == again[i].logical_id && products[i].bytes == again[i].bytes &&
            products[i].dependencies == again[i].dependencies, "direct products are not deterministic");
    const auto find = [](const auto &values, std::string_view id) -> const cc::CharacterProduct & {
        const auto found = std::ranges::find(values, id, &cc::CharacterProduct::logical_id);
        Check(found != values.end(), "direct product absent");
        return *found;
    };
    const auto &metadata_product = find(products, "render/character/archer.rpkg");
    const auto prepared = cc::PrepareCharacterBinding(metadata_product.logical_id, metadata_product.bytes);
    Check(prepared.package.dependencies == metadata_product.dependencies,
        "prepared binding lost the original package dependency view");
    Check(std::ranges::none_of(prepared.package.dependencies, [](const auto &id) {
        return id.ends_with(".meshbin") || id.ends_with("_materials.json");
    }), "normal direct metadata still depends on an exchange");
    const auto &page_product = find(products, "render/geometry/character/archer.rpage");
    Check(page_product.dependencies.empty() && rd::ReadPackage(page_product.bytes,
        hs::MakeAssetId(page_product.logical_id)).dependencies.empty(), "direct page retains a legacy source dependency");
    const auto geometry = rd::DecodeGeometryPage(page_product.bytes,
        hs::MakeAssetId(page_product.logical_id), prepared.character.geometry);
    rd::ValidateCharacterGeometry(prepared.character, geometry);
    Check(geometry.indices.size() == 6 && geometry.canonical_primitive_remap.size() == 2,
        "duplicate oriented primitive occurrence was discarded");
    auto occurrences = geometry.canonical_primitive_remap;
    std::ranges::sort(occurrences);
    Check(occurrences == std::vector<std::uint32_t>{0, 1}, "duplicate primitive occurrence identity changed");
    std::vector<rd::ExpandedVertex> expanded, expected;
    for (const auto index : geometry.indices) expanded.push_back(geometry.vertices[index]);
    for (const auto &v : ir.vertices)
        expected.push_back({v.position, v.normal, v.bone_indices, v.bone_weights, v.uv, v.tangent, v.material_index, v.reserved});
    Check(rd::EncodeExpandedVertices(expanded) == rd::EncodeExpandedVertices(expected),
        "direct import changed a 76-byte scalar, signed zero, tangent handedness or primitive order");
    const auto &animation_product = find(products, "render/animation/archer.rpkg");
    const auto animation = rd::DecodeCharacterAnimationExtension(animation_product.bytes,
        hs::MakeAssetId(animation_product.logical_id));
    Check(animation_product.dependencies.empty() && animation.sample_rate_hz == 0 && animation.clips.size() == 21 &&
        animation.sockets.size() == 5 && animation.clips.back().name == "victory",
        "common IR lost undeclared source rate, named clips or sockets");
    Check(ir.source_metadata == imported.runtime.source_metadata &&
        ir.source_metadata["clips"][0]["curves"]["bow_draw"] == Json::array({0.0, 1.0}),
        "direct preparation changed source markers, curves or unsupported metadata");

    const auto reject = [](const auto &operation, const char *reason) {
        try { operation(); } catch (const std::exception &) { return; }
        throw std::runtime_error(reason);
    };
    auto invalid = ir;
    invalid.vertices[0].bone_indices[3] = static_cast<std::uint16_t>(invalid.bone_count);
    reject([&] { (void)cc::BuildCharacterProducts(invalid, "archer", selections, material_domains); }, "zero-weight invalid bone lane reached final products");
    invalid = ir; invalid.vertices[0].reserved = 1;
    invalid.vertices[0].position[0] = std::numeric_limits<float>::quiet_NaN();
    try { (void)cc::BuildCharacterProducts(invalid, "archer", selections, material_domains); Check(false, "invalid reserved reached final products"); }
    catch (const std::exception &error) {
        Check(std::string_view(error.what()).find("material/reserved") != std::string_view::npos,
            "source reserved/finite diagnostic order changed");
    }
    invalid = ir; invalid.vertices[0].bone_weights[0] = 0.995f;
    reject([&] { (void)cc::BuildCharacterProducts(invalid, "archer", selections, material_domains); }, "generic weight tolerance substituted for strict character binding");
    auto ambiguous = authoring;
    ambiguous["characters"]["archer"].push_back(ambiguous["characters"]["archer"][0]);
    reject([&] { (void)cc::PrepareCharacterMaterialSelections(ir, "archer", ambiguous); }, "ambiguous material selection accepted");
    auto unknown = authoring; unknown["characters"]["archer"][0]["name"] = "absent";
    reject([&] { (void)cc::PrepareCharacterMaterialSelections(ir, "archer", unknown); }, "unknown material selection accepted");
    auto jelly = authoring; jelly["characters"]["archer"][0]["material"] = "slime_features";
    const auto opaque_slime_features = cc::PrepareCharacterMaterialSelections(ir, "archer", jelly);
    Check(opaque_slime_features.size() == 1 && opaque_slime_features[0].hero_ordinal == 4,
        "standalone opaque slime feature binding rejected");

    // A current native Archer product has 11 authored clips; the schema1 import
    // above retains 21/rate0. Exercise the actual image-link cross-binding gate
    // with an independently prepared native-profile fixture.
    auto native = ir;
    native.animation_extension.sample_rate_hz = 60;
    native.animation_extension.clips.resize(11);
    native.animation_extension.transforms.resize(44);
    native.animation_extension.sockets.clear();
    auto linked = cc::BuildCharacterProducts(native, "archer", selections, material_domains);
    // Opaque hero packages require the complete native four-program contract,
    // even when this fixture exercises only character dependency bindings.
    const std::array programs{
        rd::MaterialProgramSource{"render/shader/hero_scene_ps.dxil", rd::MaterialProgramKind::HeroScenePS},
        rd::MaterialProgramSource{"render/shader/hero_visibility_ps.dxil", rd::MaterialProgramKind::HeroVisibilityPS},
        rd::MaterialProgramSource{"render/shader/hero_resolve_cs.dxil", rd::MaterialProgramKind::HeroSurfaceResolveCS},
        rd::MaterialProgramSource{"render/shader/hero_shadow_ps.dxil", rd::MaterialProgramKind::HeroShadowPS}};
    linked.push_back({"render/material/archer_skin/material.rpkg", "material.rpkg", {},
        rd::BuildHeroMaterialPackage("archer_skin", {}, {1, 0}, programs)});
    for (const auto *semantic : {"diffuse", "normal"})
    {
        const auto name = "archer_" + std::string(semantic) + "_0";
        const auto id = "render/texture/" + name + ".rpkg";
        const std::array dependencies{"character/" + name + ".dds"};
        const std::array<std::byte, 16> payload{};
        const auto format = std::string_view(semantic) == "normal" ? rd::kTextureFormatBc7 : rd::kTextureFormatBc7Srgb;
        linked.push_back({id, name + ".rpkg", {}, rd::BuildTexturePackage(
            hs::MakeAssetId(id), dependencies, {1, 1, 1, 1, format}, payload)});
    }
    const auto native_prepared = cc::PrepareCharacterBinding("render/character/archer.rpkg",
        find(linked, "render/character/archer.rpkg").bytes);
    const auto validate = [&] {
        std::vector<cc::CharacterDependencyProduct> views;
        for (const auto &product : linked) views.push_back({product.logical_id, product.bytes});
        cc::ValidateCharacterProductBindings(native_prepared, views);
    };
    validate();
    auto &texture = *std::ranges::find(linked, "render/texture/archer_normal_0.rpkg", &cc::CharacterProduct::logical_id);
    const auto good_texture = texture.bytes;
    const std::array wrong_source{std::string("character/another_normal_0.dds")};
    const std::array<std::byte, 16> payload{};
    texture.bytes = rd::BuildTexturePackage(hs::MakeAssetId(texture.logical_id), wrong_source,
        {1, 1, 1, 1, rd::kTextureFormatBc7}, payload);
    reject(validate, "image binding accepted a texture belonging to another slot");
    texture.bytes = good_texture;
    const std::array expected_source{std::string("character/archer_normal_0.dds")};
    const std::array<std::byte, 4> rgba{};
    texture.bytes = rd::BuildTexturePackage(hs::MakeAssetId(texture.logical_id), expected_source,
        {1, 1, 1, 1, rd::kTextureFormatRgba8}, rgba);
    reject(validate, "final character binding accepted legacy RGBA8 storage");
    texture.bytes = rd::BuildTexturePackage(hs::MakeAssetId(texture.logical_id), expected_source,
        {1, 1, 1, 1, rd::kTextureFormatBc7Srgb}, payload);
    reject(validate, "normal/roughness data accepted an sRGB texture");
    texture.bytes = good_texture;
    auto &page = *std::ranges::find(linked, "render/geometry/character/archer.rpage", &cc::CharacterProduct::logical_id);
    const auto good_page = page.bytes;
    const std::array legacy_dependency{std::string("character/archer.meshbin")};
    page.bytes = rd::BuildGeometryPage(hs::MakeAssetId(page.logical_id), legacy_dependency, geometry);
    reject(validate, "image binding accepted a legacy intermediate dependency");
    page.bytes = good_page;
    validate();
}
}
int main()
{
    try
    {
        TestHandedness();
        Fixture f; f.Save();auto result=f.Load();
        TestAnimationExtension(result);
        TestDirectCharacterProducts(result);
        Check(result.runtime.clips.size()==5 && result.runtime.source_metadata["clips"].size()==21,"source clips lost");
        Check(result.runtime.source_metadata["morphs"]==f.source["morphs"] && result.limitations.size()>=3,"unsupported metadata concealed");
        Check(result.runtime.source_metadata["sockets"][0]["translation"][0].get<double>()<0 &&
              result.runtime.source_metadata["sockets"][3]["translation"][0].get<double>()>0,
              "labeled asymmetric socket metadata changed");
        Check(result.runtime.vertices[1].position==std::array{0.0f,1.0f,3.0f} && result.runtime.vertices[2].position[0]==-2.0f,"asymmetric axis fixture changed");
        Check(result.runtime.transforms[4].translation[2]==2.0f && result.runtime.transforms[8].translation[2]==12.0f,"runtime alias projection incorrect");
        auto bytes=hs::render_data::BuildEmbeddedCharacterPackage(hs::MakeAssetId("render/character/archer.rpkg"),{},result.runtime);
        auto decoded=hs::render_data::DecodeEmbeddedCharacterPackage(bytes,hs::MakeAssetId("render/character/archer.rpkg"));
        Check(decoded.vertices[2].position==result.runtime.vertices[2].position && decoded.transforms.size()==20,"actual package reader roundtrip failed");
        Reject([](auto &x){x.source["binary"]["path"]="../character.bin";},"traversal accepted");
        Reject([](auto &x){x.source["binary"]["path"]="C:\\character.bin";},"Windows absolute path accepted");
        Reject([](auto &x){x.source["binary"]["sha256"]=std::string(64,'0');},"hash mismatch accepted");
        Reject([](auto &x){x.source["binary"]["byte_length"]=1;},"size mismatch accepted");
        Reject([](auto &x){x.source["buffers"]["vertices"]["offset"]=1;},"overlap accepted");
        Reject([](auto &x){x.source["buffers"]["vertices"]["count"]=UINT64_MAX;},"count overflow accepted");
        Reject([](auto &x){x.source["buffers"]["vertices"]["stride"]=80;},"wrong stride accepted");
        Reject([](auto &x){x.source["buffers"]["vertices"]["type"]="f64";},"wrong type accepted");
        Reject([](auto &x){x.source["coordinate_system"]="z_up";},"wrong axes accepted");
        Reject([](auto &x){x.source["coordinate_system"]="y_up_z_forward_meters";},"old ambiguous basis accepted");
        Reject([](auto &x){x.source["bones"][1]["parent"]=1;},"bad parent accepted");
        Reject([](auto &x){x.source["masks"]["upper_body"]["required_bones"].push_back("absent");},"missing mask bone accepted");
        Reject([](auto &x){x.source["sockets"][0]["bone"]="absent";},"missing socket bone accepted");
        Reject([](auto &x){x.source["sockets"].erase(0);},"missing socket accepted");
        Reject([](auto &x){x.source["meshes"][0]["vertex_count"]=2;},"nontriangle section accepted");
        Reject([](auto &x){x.source["meshes"][0]["material"]="absent";},"missing material accepted");
        Reject([](auto &x){x.source["clips"][0]["frame_count"]=3;},"clip overlap accepted");
        Reject([](auto &x){x.source["clips"][0]["curves"]["bow_draw"]={0.0};},"curve shape accepted");
        Reject([](auto &x){x.source["clips"][0]["markers"]={{{"id","release"},{"time_seconds",2.0}}};},"marker range accepted");
        Reject([](auto &x){x.source["runtime_clips"]["run"]="absent";},"missing alias accepted");
        Reject([](auto &x){std::swap_ranges(x.binary.begin()+76,x.binary.begin()+152,x.binary.begin()+152);x.Save();},"reversed winding accepted");
        Reject([](auto &x){x.binary[2]=std::byte{0xc0};x.binary[3]=std::byte{0x7f};x.Save();},"NaN accepted");
        Reject([](auto &x){x.binary[24]=std::byte{100};x.Save();},"skin index accepted");
        Reject([](auto &x){x.binary[35]=std::byte{0x40};x.Save();},"skin weight sum accepted");
        Reject([](auto &x){x.source["sockets"][0]["rotation"]={0,0,0,0};},"zero quaternion accepted");
        Reject([](auto &x){x.source["buffers"]["parents"]["count"]=-1;},"negative count accepted");
        Reject([](auto &x){x.source["binary"]["path"]="/tmp/character.bin";},"absolute path accepted");
        Reject([](auto &x){
            const auto offset=x.source["buffers"]["inverse_bind_matrices"]["offset"].template get<std::size_t>();
            std::fill_n(x.binary.begin()+static_cast<std::ptrdiff_t>(offset),12,std::byte{0});x.Save();
        },"singular inverse bind accepted");
        Reject([](auto &x){
            const auto offset=x.source["buffers"]["upper_body_weights"]["offset"].template get<std::size_t>();
            std::fill_n(x.binary.begin()+static_cast<std::ptrdiff_t>(offset),8,std::byte{0});x.Save();
        },"all-zero mask accepted");
        Reject([](auto &x){
            const auto offset=x.source["buffers"]["local_transforms"]["offset"].template get<std::size_t>();
            std::fill_n(x.binary.begin()+static_cast<std::ptrdiff_t>(offset+12),16,std::byte{0});x.Save();
        },"binary zero quaternion accepted");
        Reject([](auto &x){x.source["clips"][1]["id"]="idle";},"duplicate clip accepted");
        Reject([](auto &x){x.source["bones"][1]["id"]="root";},"duplicate bone accepted");
        Reject([](auto &x){x.binary.push_back(std::byte{0});x.Save();},"trailing payload accepted");
        // A symlink is resolved before opening its target and cannot leave the manifest directory.
        {
            Fixture x;x.Save();
            std::error_code error;
            std::filesystem::create_directory_symlink(x.root.parent_path(),x.root/"outside",error);
            if(!error)
            {
                x.source["binary"]["path"]="outside";x.Write();
                bool rejected=false;
                try { (void)x.Load(); } catch(const std::exception &) {rejected=true;}
                Check(rejected,"symlink escape accepted");
            }
        }
        std::cout<<"character interchange: valid projection, roundtrip and rejection cases passed\n";return 0;
    }
    catch(const std::exception &e) {std::cerr<<e.what()<<'\n';return 1;}
}
