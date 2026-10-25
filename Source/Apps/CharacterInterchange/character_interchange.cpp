#include "character_interchange.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>

namespace hs::character_interchange
{
namespace
{
using Json = nlohmann::json;
void Require(bool condition, std::string_view reason)
{
    if (!condition) throw std::runtime_error("character interchange: " + std::string(reason));
}
std::vector<std::byte> Read(const std::filesystem::path &path, std::uintmax_t limit)
{
    Require(std::filesystem::is_regular_file(path), "not a regular file");
    const auto size = std::filesystem::file_size(path);
    Require(size > 0 && size <= limit, "file exceeds size limit or is empty");
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    std::ifstream stream(path, std::ios::binary);
    Require(bool(stream.read(reinterpret_cast<char *>(bytes.data()),
                             static_cast<std::streamsize>(bytes.size()))), "cannot read file");
    return bytes;
}
std::uint64_t Integer(const Json &value)
{
    Require(value.is_number_integer() &&
            (value.is_number_unsigned() || value.get<std::int64_t>() >= 0), "expected nonnegative integer");
    return value.get<std::uint64_t>();
}
float Number(const Json &value)
{
    Require(value.is_number(), "expected numeric value");
    const float result = value.get<float>();
    Require(std::isfinite(result), "nonfinite number");
    return result;
}
std::string Id(const Json &value)
{
    Require(value.is_string(), "expected identifier");
    auto result = value.get<std::string>();
    Require(!result.empty() && result.size() <= 128 &&
            std::ranges::all_of(result, [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
            }), "invalid identifier");
    return result;
}
std::string Hex(std::span<const std::byte> bytes)
{
    std::string result;
    for (auto b : bytes)
    {
        const auto v = std::to_integer<unsigned>(b);
        result += "0123456789abcdef"[v >> 4];
        result += "0123456789abcdef"[v & 15];
    }
    return result;
}
std::filesystem::path RelativeFile(const std::filesystem::path &root, const Json &value)
{
    Require(value.is_string(), "binary path must be a string");
    const auto text = value.get<std::string>();
    Require(!text.empty() && text.find('\\') == std::string::npos &&
            text.find(':') == std::string::npos && text.find('\0') == std::string::npos,
            "binary path is not portable");
    const std::filesystem::path relative(text);
    Require(!relative.is_absolute() && !relative.has_root_path(), "absolute binary path");
    for (const auto &part : relative)
        Require(part != ".." && part != "." && !part.empty(), "unsafe binary path");
    const auto base = std::filesystem::canonical(root);
    const auto resolved = std::filesystem::canonical(base / relative);
    const auto below = resolved.lexically_relative(base);
    Require(!below.empty() && *below.begin() != "..", "binary symlink escapes source directory");
    return resolved;
}
std::uint32_t U32(std::span<const std::byte> bytes, std::size_t offset, std::size_t width = 4)
{
    Require(offset <= bytes.size() && width <= bytes.size() - offset, "binary read out of bounds");
    std::uint32_t value{};
    for (std::size_t i = 0; i < width; ++i)
        value |= std::to_integer<std::uint32_t>(bytes[offset + i]) << (i * 8);
    return value;
}
float F32(std::span<const std::byte> bytes, std::size_t offset)
{
    const auto value = std::bit_cast<float>(U32(bytes, offset));
    Require(std::isfinite(value), "nonfinite payload scalar");
    return value;
}
void Transform(const CharacterLocalTransform &value)
{
    float length{};
    for (float q : value.rotation) length += q * q;
    Require(std::isfinite(length) && std::abs(length - 1.0f) <= 0.002f, "quaternion is not normalized");
    for (float s : value.scale) Require(std::abs(s) > 0.0001f, "zero transform scale");
}
template<std::size_t N>
std::array<float, N> Vector(const Json &value)
{
    Require(value.is_array() && value.size() == N, "invalid vector shape");
    std::array<float, N> result{};
    for (std::size_t i = 0; i < N; ++i) result[i] = Number(value[i]);
    return result;
}
}

ImportedCharacter Load(const std::filesystem::path &manifest)
{
    const auto document = Read(manifest, 32 * 1024 * 1024);
    ImportedCharacter output;
    output.runtime.source_metadata = Json::parse(reinterpret_cast<const char *>(document.data()),
                               reinterpret_cast<const char *>(document.data() + document.size()));
    const auto &j = output.runtime.source_metadata;
    Require(Integer(j.at("schema_version")) == 1, "unsupported schema version");
    Require(j.at("id") == "character.archer.elin", "unexpected character identity");
    Require(j.at("coordinate_system") == "left_handed_y_up_z_forward_meters", "unsupported coordinate system");
    const auto &binary = j.at("binary");
    const auto bytes = Read(RelativeFile(std::filesystem::absolute(manifest).parent_path(), binary.at("path")),
                            256 * 1024 * 1024);
    Require(Integer(binary.at("byte_length")) == bytes.size(), "binary size mismatch");
    Require(binary.at("sha256") == Hex(render_data::Sha256(bytes)), "binary SHA-256 mismatch");
    struct Slice { std::uint64_t start, end; };
    std::vector<Slice> slices;
    const auto &buffers = j.at("buffers");
    Require(buffers.is_object() && buffers.size() == 5, "unexpected buffers");
    const auto buffer = [&](const char *name, const char *type, std::size_t stride,
                            std::size_t limit) {
        const auto &desc = buffers.at(name);
        const auto count = Integer(desc.at("count"));
        const auto offset = Integer(desc.at("offset"));
        Require(desc.at("type") == type && Integer(desc.at("stride")) == stride &&
                count > 0 && count <= limit && offset <= bytes.size() &&
                count <= (bytes.size() - offset) / stride, "invalid buffer type, count, stride or range");
        slices.push_back({offset, offset + count * stride});
        return std::span(bytes).subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(count) * stride);
    };
    auto vertices = buffer("vertices", "expanded_vertex_v1", 76, 3'000'000);
    auto parents = buffer("parents", "u16", 2, kMaxCharacterBones);
    auto inverse = buffer("inverse_bind_matrices", "f32x16", 64, kMaxCharacterBones);
    auto mask = buffer("upper_body_weights", "f32", 4, kMaxCharacterBones);
    auto transforms = buffer("local_transforms", "trs_f32", 40, 800'000);
    std::ranges::sort(slices, {}, &Slice::start);
    std::uint64_t end{};
    for (const auto &slice : slices)
    {
        Require(slice.start == end, "overlap, gap or unreferenced binary payload");
        end = slice.end;
    }
    Require(end == bytes.size(), "trailing binary payload");
    auto &runtime = output.runtime;
    auto &named = runtime.animation_extension;
    named.character_asset_id = MakeAssetId("render/character/archer.rpkg");
    // schema v1 declares durations and samples but has no nominal sample rate.
    named.sample_rate_hz = 0;
    const auto &bones = j.at("bones");
    Require(bones.is_array() && !bones.empty() && bones.size() <= kMaxCharacterBones &&
            parents.size() == bones.size() * 2 && inverse.size() == bones.size() * 64 &&
            mask.size() == bones.size() * 4, "skeleton buffer shape mismatch");
    runtime.bone_count = static_cast<std::uint32_t>(bones.size());
    std::map<std::string, std::size_t> bone_ids;
    std::size_t roots{};
    for (std::size_t i = 0; i < bones.size(); ++i)
    {
        Require(bone_ids.emplace(Id(bones[i].at("id")), i).second, "duplicate bone");
        named.bone_names.push_back(Id(bones[i].at("id")));
        runtime.bone_names.push_back(named.bone_names.back());
        const auto parent = U32(parents, i * 2, 2);
        Require(parent == Integer(bones[i].at("parent")) &&
                (parent == UINT16_MAX || parent < i), "invalid parent or missing ancestor");
        roots += parent == UINT16_MAX;
        runtime.parents.push_back(static_cast<std::uint16_t>(parent));
        std::array<float, 16> matrix{};
        for (std::size_t k = 0; k < 16; ++k) matrix[k] = F32(inverse, i * 64 + k * 4);
        Require(std::abs(matrix[12]) < 1e-5f && std::abs(matrix[13]) < 1e-5f &&
                std::abs(matrix[14]) < 1e-5f && std::abs(matrix[15] - 1.0f) < 1e-5f,
                "inverse bind must be affine column-vector row-major matrix");
        const float determinant = matrix[0]*(matrix[5]*matrix[10]-matrix[6]*matrix[9]) -
            matrix[1]*(matrix[4]*matrix[10]-matrix[6]*matrix[8]) +
            matrix[2]*(matrix[4]*matrix[9]-matrix[5]*matrix[8]);
        Require(std::isfinite(determinant) && std::abs(determinant) > 1e-8f, "singular inverse bind");
        runtime.inverse_bind_matrices.push_back(matrix);
        const auto weight = F32(mask, i * 4);
        Require(weight >= 0 && weight <= 1, "mask weight out of range");
        runtime.upper_body_weights.push_back(weight);
    }
    Require(roots == 1, "skeleton requires exactly one root");
    named.parents = runtime.parents;
    named.inverse_bind_matrices = runtime.inverse_bind_matrices;
    const auto &required = j.at("masks").at("upper_body").at("required_bones");
    Require(required.is_array() && !required.empty(), "required mask bones absent");
    std::set<std::string> required_ids;
    for (const auto &bone : required)
        Require(bone_ids.contains(Id(bone)) && required_ids.insert(Id(bone)).second,
                "required mask bone missing or duplicated");
    const auto &materials = j.at("materials");
    Require(materials.is_array() && !materials.empty() && materials.size() <= kMaxCharacterMaterials,
            "invalid material count");
    std::map<std::string, std::size_t> material_ids;
    for (std::size_t i = 0; i < materials.size(); ++i)
    {
        Require(material_ids.emplace(Id(materials[i].at("id")), i).second, "duplicate material");
        runtime.materials.push_back({{}, {}, Id(materials[i].at("id"))});
    }
    runtime.material_count = static_cast<std::uint32_t>(materials.size());
    for (const auto &v : render_data::DecodeExpandedVertices(vertices))
        runtime.vertices.push_back({v.position, v.normal, v.bone_indices, v.bone_weights,
                                    v.uv, v.tangent, v.material_index, v.reserved});
    Require(runtime.vertices.size() % 3 == 0, "nontriangular vertex count");
    for (const auto &vertex : runtime.vertices)
        for (std::size_t lane = 0; lane < 3; ++lane)
        {
            runtime.bounds_min[lane] = std::min(runtime.bounds_min[lane], vertex.position[lane]);
            runtime.bounds_max[lane] = std::max(runtime.bounds_max[lane], vertex.position[lane]);
        }
    const auto &meshes = j.at("meshes");
    Require(meshes.is_array() && !meshes.empty(), "no mesh sections");
    runtime.mesh_count = static_cast<std::uint32_t>(meshes.size());
    std::size_t next_vertex{};
    std::set<std::string> mesh_ids;
    for (const auto &mesh : meshes)
    {
        Require(mesh_ids.insert(Id(mesh.at("id"))).second, "duplicate mesh");
        const auto count = Integer(mesh.at("vertex_count"));
        Require(Integer(mesh.at("first_vertex")) == next_vertex && count > 0 && count % 3 == 0 &&
                count <= runtime.vertices.size() - next_vertex, "invalid mesh section range");
        const auto material = material_ids.find(Id(mesh.at("material")));
        Require(material != material_ids.end(), "unknown mesh material");
        for (std::size_t i = next_vertex; i < next_vertex + count; ++i)
            Require(runtime.vertices[i].material_index == material->second, "vertex/section material mismatch");
        next_vertex += static_cast<std::size_t>(count);
    }
    Require(next_vertex == runtime.vertices.size(), "unreferenced mesh vertices");
    for (std::size_t i = 0; i < runtime.vertices.size(); i += 3)
    {
        const auto &a = runtime.vertices[i], &b = runtime.vertices[i+1], &c = runtime.vertices[i+2];
        std::array<float,3> u{}, v{}, n{};
        for (std::size_t k=0;k<3;++k)
        {
            u[k]=b.position[k]-a.position[k]; v[k]=c.position[k]-a.position[k];
            n[k]=a.normal[k]+b.normal[k]+c.normal[k];
        }
        const float facing=(u[1]*v[2]-u[2]*v[1])*n[0]+(u[2]*v[0]-u[0]*v[2])*n[1]+(u[0]*v[1]-u[1]*v[0])*n[2];
        Require(std::isfinite(facing) && facing > 0, "degenerate or reversed triangle winding");
    }
    std::vector<CharacterLocalTransform> all_transforms;
    all_transforms.reserve(transforms.size()/40);
    for (std::size_t offset=0;offset<transforms.size();offset+=40)
    {
        CharacterLocalTransform t;
        for (std::size_t k=0;k<3;++k) { t.translation[k]=F32(transforms,offset+k*4); t.scale[k]=F32(transforms,offset+28+k*4); }
        for (std::size_t k=0;k<4;++k) t.rotation[k]=F32(transforms,offset+12+k*4);
        Transform(t); all_transforms.push_back(t);
    }
    const auto &clips=j.at("clips");
    Require(clips.is_array() && clips.size()==21, "Elin requires all 21 source clips");
    const std::set<std::string> expected_clips{"idle","run_start","run_loop","run_stop","pivot_90_l","pivot_90_r","pivot_180_l","pivot_180_r","turn_in_place_l","turn_in_place_r","shoot","multishot","draw_charge","release_charge","arrow_rain_cast","roll_forward","retreat_shot","hit","death","revive","victory"};
    std::map<std::string,std::size_t> clip_ids;
    std::size_t next_transform{};
    for (std::size_t i=0;i<clips.size();++i)
    {
        const auto &clip=clips[i]; const auto id=Id(clip.at("id"));
        Require(expected_clips.contains(id) && clip_ids.emplace(id,i).second,"unknown or duplicate source clip");
        const auto frames=Integer(clip.at("frame_count")); const float duration=Number(clip.at("duration_seconds"));
        Require(clip.at("looping").is_boolean() && frames>=2 && duration>0 &&
                Integer(clip.at("first_transform"))==next_transform &&
                frames<=(all_transforms.size()-next_transform)/runtime.bone_count,"invalid clip range or duration");
        named.clips.push_back({MakeAssetId("character/archer/animation/" + id), id,
            static_cast<std::uint32_t>(next_transform), static_cast<std::uint32_t>(frames),
            duration, clip.at("looping").get<bool>()});
        next_transform+=static_cast<std::size_t>(frames)*runtime.bone_count;
        const auto &markers=clip.at("markers");
        Require(markers.is_array(),"invalid markers");
        std::set<std::string> marker_ids;
        for (const auto &marker:markers)
        {
            Require(marker_ids.insert(Id(marker.at("id"))).second,"duplicate marker");
            const auto time=Number(marker.at("time_seconds"));
            Require(time>=0 && time<=duration,"marker outside clip");
        }
        const auto &curves=clip.at("curves"); Require(curves.is_object(),"invalid curves");
        for (const auto &[name,values]:curves.items())
        {
            (void)Id(name); Require(values.is_array() && values.size()==frames,"curve sample count mismatch");
            for (const auto &value:values) (void)Number(value);
        }
    }
    Require(next_transform==all_transforms.size(),"unreferenced transforms");
    const auto &aliases=j.at("runtime_clips");
    Require(aliases.is_object() && aliases.size()==5,"five runtime aliases required");
    const std::array alias_names{"idle","run","draw","recoil","death"};
    for (std::size_t i=0;i<alias_names.size();++i)
    {
        const auto found=clip_ids.find(Id(aliases.at(alias_names[i])));
        Require(found!=clip_ids.end(),"runtime alias refers to missing clip");
        const auto &clip=clips[found->second];
        const auto first=Integer(clip.at("first_transform")); const auto frames=Integer(clip.at("frame_count"));
        const auto count=frames*runtime.bone_count;
        Require(runtime.transforms.size()+count<=800'000,"runtime transform budget exceeded");
        runtime.clips.push_back({static_cast<CharacterAnimationClip>(i),static_cast<std::uint8_t>(clip.at("looping").get<bool>()),0,
            static_cast<std::uint32_t>(runtime.transforms.size()),static_cast<std::uint32_t>(frames),Number(clip.at("duration_seconds"))});
        runtime.transforms.insert(runtime.transforms.end(),all_transforms.begin()+static_cast<std::ptrdiff_t>(first),
                                  all_transforms.begin()+static_cast<std::ptrdiff_t>(first+count));
    }
    const auto &sockets=j.at("sockets"); Require(sockets.is_array(),"invalid sockets");
    std::set<std::string> socket_ids;
    for (const auto &socket:sockets)
    {
        Require(socket_ids.insert(Id(socket.at("id"))).second && bone_ids.contains(Id(socket.at("bone"))),"invalid socket bone or duplicate socket");
        CharacterLocalTransform t; t.translation=Vector<3>(socket.at("translation"));
        t.rotation=Vector<4>(socket.at("rotation")); t.scale=Vector<3>(socket.at("scale")); Transform(t);
        named.sockets.push_back({Id(socket.at("id")),
            static_cast<std::uint32_t>(bone_ids.at(Id(socket.at("bone")))), t});
    }
    for (const auto *name:{"weapon_grip","arrow_nock","projectile_visual_origin","quiver_socket","hat_socket"})
        Require(socket_ids.contains(name),"required socket absent");
    runtime.ground_offset=Number(j.at("ground_offset"));
    named.transforms = std::move(all_transforms);
    output.limitations={"five-slot compatibility projection remains separate from all 21 typed named clips and sockets",
        "curves and markers are validated and retained in source metadata; runtime playback does not consume them",
        "morphs, corrective shapes, LODs, eight influences, pose search, IK and secondary simulation unsupported",
        "materials contain identities only; texture cooking and game build integration are separate"};
    render_data::ValidateCharacterSource(runtime);
    render_data::ValidateCharacterAnimationBinding(named, runtime, named.character_asset_id);
    return output;
}
}
