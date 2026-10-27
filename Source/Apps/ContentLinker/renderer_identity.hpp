#pragma once

#include <hs/core/canonical_bytes.hpp>
#include <hs/render_data/character_descriptor.hpp>
#include <hs/render_data/render_revision.hpp>
#include <hs/render_data/vfx_mesh_descriptor.hpp>

namespace hs::content_identity
{
template <typename T, std::size_t N>
void Numbers(CanonicalBytes &b, const std::array<T, N> &values)
{
    b.Sequence(values, [](CanonicalBytes &out, T value) { out.Scalar(value); });
}
inline void Bounds(CanonicalBytes &b, const render_data::GeometryBounds &v)
{ Numbers(b, v.minimum); Numbers(b, v.maximum); Numbers(b, v.sphere); }
inline void Bulk(CanonicalBytes &b, const render_data::BulkRange &v)
{ b.Fields(v.offset, v.byte_count, v.format); b.Blob(v.expected_digest); }
inline void Metadata(CanonicalBytes &b, const render_data::GeometryMetadata &v)
{
    b.Fields(v.abi, v.flags, v.vertex_count, v.index_count, v.submesh_count, v.meshlet_count,
        v.vertex_ref_count, v.max_material_index, v.page_dependency_index,
        v.fallback_dependency_index, v.lod, v.terminal_fallback_dependency_index);
    Bounds(b, v.bounds); b.Fields(v.geometric_error, v.error_method_abi);
}
inline void Transform(CanonicalBytes &b, const CharacterLocalTransform &v)
{
    Numbers(b, v.translation); b.Scalar(v.translation_padding); Numbers(b, v.rotation);
    Numbers(b, v.scale); b.Scalar(v.scale_padding);
}
inline void Texture(CanonicalBytes &b, const render_data::TextureDescriptor &v)
{
    b.Fields(v.asset_id.value, v.bulk_index, v.description.width, v.description.height,
        v.description.array_size, v.description.mip_count, v.description.format);
    b.Sequence(v.dependencies, [](CanonicalBytes &out, AssetId id) { out.Scalar(id.value); });
    b.Sequence(v.subresources, [](CanonicalBytes &out, const auto &r) {
        out.Fields(r.offset, r.row_pitch, r.slice_pitch, r.byte_count);
        out.Blob(r.digest); out.Scalar(r.has_digest);
    });
    b.Fields(v.dimension, v.depth);
}
inline void Geometry(CanonicalBytes &b, const render_data::GeometryDescriptor &v)
{
    b.Fields(v.asset_id.value, v.bulk_index, v.flags, v.vertex_count, v.index_count,
        v.meshlet_count, v.vertex_ref_count, v.max_material_index, v.max_bone_index);
    Bounds(b, v.bounds);
    b.Sequence(v.chunks, [](CanonicalBytes &out, const auto &r) { out.Fields(r.offset, r.byte_count); });
    b.Sequence(v.dependencies, [](CanonicalBytes &out, AssetId id) { out.Scalar(id.value); });
    b.Sequence(v.submeshes, [](CanonicalBytes &out, const auto &r) {
        out.Fields(r.first_index, r.index_count, r.first_meshlet, r.meshlet_count, r.material_index);
    });
    b.Sequence(v.ground_support, [](CanonicalBytes &out, const auto &r) {
        Numbers(out, r.position); Numbers(out, r.normal); Numbers(out, r.bone_indices);
        Numbers(out, r.bone_weights); Numbers(out, r.uv); Numbers(out, r.tangent);
        out.Fields(r.material_index, r.reserved);
    });
    b.Scalar(v.payload_ready);
}
inline void Material(CanonicalBytes &b, const render_data::MaterialDescriptor &v)
{
    b.Fields(v.asset_id.value, v.bulk_index, v.kind, v.surface.capability_mask, v.surface.varying_mask);
    b.Blob(v.parameter_layout);
    b.Sequence(v.defaults, [](CanonicalBytes &out, const auto &r) { Numbers(out, r.words); });
    b.Sequence(v.programs, [](CanonicalBytes &out, const auto &r) {
        out.Fields(r.kind, r.bulk_index, r.asset_id.value); out.String(r.product_id);
    });
    b.Sequence(v.textures, [](CanonicalBytes &out, const auto &r) {
        out.Fields(r.slot, r.texture.value, r.dimension, r.semantic, r.shader_binding_abi);
        out.String(r.product_id);
    });
    b.Fields(v.metallic, v.roughness_scale, v.flags, v.domain,
        v.receivers.model_mask, v.receivers.required_surface_flags,
        v.receivers.forbidden_surface_flags, v.receivers.normal_filter);
}
inline void Character(CanonicalBytes &b, const render_data::CharacterDescriptor &v)
{
    b.Fields(v.asset_id.value, v.bulk_index, v.geometry_page.value); Metadata(b, v.geometry);
    b.Sequence(v.dependencies, [](CanonicalBytes &out, AssetId id) { out.Scalar(id.value); });
    b.Sequence(v.material_bindings, [](CanonicalBytes &out, const auto &r) {
        out.Fields(r.slot, r.dependency_index, r.hero_ordinal, r.domain);
    });
    b.Sequence(v.materials, [](CanonicalBytes &out, const auto &r) { out.Scalar(r.value); });
    b.Fields(v.bone_count, v.material_count, v.ground_offset);
    b.Sequence(v.clips, [](CanonicalBytes &out, const auto &r) {
        out.Fields(r.clip, r.looping, r.reserved, r.first_transform, r.frame_count, r.duration_seconds);
    });
    b.Sequence(v.parents, [](CanonicalBytes &out, auto r) { out.Scalar(r); });
    b.Sequence(v.inverse_bind_matrices, [](CanonicalBytes &out, const auto &r) { Numbers(out, r); });
    b.Sequence(v.upper_body_weights, [](CanonicalBytes &out, auto r) { out.Scalar(r); });
    Bulk(b, v.transforms); b.Fields(v.deformation_abi, v.deformation_flags); b.Blob(v.skeleton_binding);
    b.Fields(v.numerical_margin, v.bubble_amplitude);
    b.Sequence(v.lods, [](CanonicalBytes &out, const auto &r) { Metadata(out, r); });
    b.Sequence(v.interval_bounds, [](CanonicalBytes &out, const auto &r) {
        out.Fields(r.lod, r.clip, r.frame); Numbers(out, r.minimum); Numbers(out, r.maximum);
    });
    Bulk(b, v.final_matrices);
}
inline void Animation(CanonicalBytes &b, const render_data::AnimationDescriptor &v)
{
    b.Fields(v.asset_id.value, v.bulk_index, v.character.value, v.character_asset_id.value, v.sample_rate_hz);
    b.Sequence(v.bone_names, [](CanonicalBytes &out, std::string_view r) { out.String(r); });
    b.Sequence(v.parents, [](CanonicalBytes &out, auto r) { out.Scalar(r); });
    b.Sequence(v.inverse_bind_matrices, [](CanonicalBytes &out, const auto &r) { Numbers(out, r); });
    b.Sequence(v.clips, [](CanonicalBytes &out, const auto &r) {
        out.Scalar(r.id.value); out.String(r.name); out.Fields(r.first_transform, r.frame_count, r.duration_seconds, r.looping);
    });
    b.Sequence(v.sockets, [](CanonicalBytes &out, const auto &r) {
        out.String(r.name); out.Scalar(r.bone_index); Transform(out, r.local_transform);
    });
    Bulk(b, v.transforms);
}
inline void Atlas(CanonicalBytes &b, const render_data::VfxMeshAtlasDescriptor &v)
{
    b.Scalar(v.asset_id.value);
    b.Sequence(v.meshes, [](CanonicalBytes &out, const auto &r) {
        out.Fields(r.vertex_count, r.transverse_radius, r.minimum_z, r.widest_z, r.maximum_z);
    });
}
}
