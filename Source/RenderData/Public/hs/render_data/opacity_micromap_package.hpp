#pragma once
#include <hs/render_data/render_asset_format.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace hs::render_data
{
using OpacityMicromapDigest=std::array<std::byte,32>;
inline constexpr std::uint32_t kOpacityMicromapBakerAbi=1;
inline constexpr std::uint16_t kOpacityMicromapFourStateFormat=2;
// Reserved package registry IDs; the common package reader also recognizes them.
inline constexpr std::uint32_t kOmmMetadataChunk=37,kOmmRecipeChunk=38,kOmmStatesChunk=39,
    kOmmDescriptorsChunk=40,kOmmIndicesChunk=41,kOmmHistogramChunk=42;
struct OpacityMicromapIdentity
{
    AssetId geometry_asset{};
    OpacityMicromapDigest geometry_digest{},topology_digest{},material_digest{},coverage_key{};
    std::uint32_t triangle_count{};
};
struct OpacityMicromapMaterialWords
{
    AssetId material_asset{};
    OpacityMicromapDigest program_layout_digest{}; // semantic IR + parameter layout
    std::span<const std::uint32_t> parameter_words; // FULL actual live words
    std::uint32_t cutoff_bits{0x3f000000u};
};
// OPW1/materialCount; records sorted by AssetId: idLow/idHigh/digest8words/
// parameterWordCount/fullWords/cutoffBits. Producer and runtime share this exact
// packing. Empty or repeated material identities are rejected.
[[nodiscard]] std::vector<std::uint32_t> BuildOpacityMicromapWords(
    std::span<const OpacityMicromapMaterialWords>);
// These fields are explicitly little-endian encoded, never memcpy'd object bytes.
struct OpacityMicromapDescriptor
{
    std::uint32_t byte_offset{};
    std::uint16_t subdivision_level{},format{kOpacityMicromapFourStateFormat};
};
struct OpacityMicromapHistogram
{
    std::uint32_t count{},subdivision_level{},format{kOpacityMicromapFourStateFormat};
};
struct OpacityMicromapData
{
    std::vector<std::byte> states;
    std::vector<OpacityMicromapDescriptor> descriptors;
    std::vector<std::uint32_t> indices;
    std::vector<OpacityMicromapHistogram> histogram;
    // Counts include each referenced triangle, even when identical OMMs are shared.
    std::uint64_t known_cells{},unknown_cells{};
};
struct OpacityMicromapPackageView
{
    OpacityMicromapIdentity identity;
    std::vector<std::string> dependencies;
    std::span<const std::byte> recipe,opacity_word_bytes,states,descriptor_bytes,index_bytes;
    std::vector<OpacityMicromapDescriptor> descriptors;
    std::vector<std::uint32_t> indices;
    std::vector<OpacityMicromapHistogram> histogram;
    std::uint64_t known_cells{},unknown_cells{};
    std::uint32_t maximum_subdivision{};
};
// recipe is the producer's explicit immutable coverage contract: canonical UV/
// topology/submesh/material IR + actual parameter words, selected compressed
// alpha product digests/mips/layer, UV transform, sampler/addressing/filter/
// anisotropy/clamp domain and baker ABI. coverage_key must equal Sha256(recipe).
// Prefix OMR1/u32 count/u32 actual opacity words in canonical binding order.
// Words include actual mutable parameter words and cutoff bits, never defaults.
[[nodiscard]] std::vector<std::byte> BuildOpacityMicromapRecipe(
    std::span<const std::uint32_t> actual_opacity_words,std::span<const std::byte> immutable_proof_inputs);
[[nodiscard]] std::vector<std::byte> BuildOpacityMicromapPackage(AssetId,
    std::span<const std::string>,const OpacityMicromapIdentity &,
    std::span<const std::byte> recipe,const OpacityMicromapData &);
// Spans retain caller-owned package bytes. Invalid identity, counts, native
// ranges, state histograms or coverage hash throw just like ReadTexturePackage.
[[nodiscard]] OpacityMicromapPackageView ReadOpacityMicromapPackage(
    std::span<const std::byte>,AssetId expected_asset_id);
}
