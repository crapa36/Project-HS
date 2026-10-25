#include <hs/render_data/character_animation_extension.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace hs::render_data
{
namespace
{
constexpr std::size_t kMaxTransforms = 800'000;
constexpr std::size_t kMaxNamesBytes = 128 * 1024;

void Require(bool value, std::string_view message)
{
    if (!value) throw std::runtime_error("character animation: " + std::string(message));
}

void Put(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value,
         std::size_t width = 4)
{
    Require(offset <= bytes.size() && width <= bytes.size() - offset, "write range");
    for (std::size_t i = 0; i < width; ++i)
        bytes[offset + i] = std::byte((value >> (8 * i)) & 0xff);
}

std::uint64_t Get(std::span<const std::byte> bytes, std::size_t offset,
                  std::size_t width = 4)
{
    Require(offset <= bytes.size() && width <= bytes.size() - offset, "read range");
    std::uint64_t value{};
    for (std::size_t i = 0; i < width; ++i)
        value |= std::uint64_t(std::to_integer<unsigned>(bytes[offset + i])) << (8 * i);
    return value;
}

void PutFloat(std::span<std::byte> bytes, std::size_t offset, float value)
{
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
    Put(bytes, offset, std::bit_cast<std::uint32_t>(value));
}

float GetFloat(std::span<const std::byte> bytes, std::size_t offset)
{
    return std::bit_cast<float>(static_cast<std::uint32_t>(Get(bytes, offset)));
}

// Keep exact FBX labels, including namespaces and non-ASCII labels, while
// rejecting malformed UTF-8, embedded NULs and control characters.
void ValidateName(std::string_view name, std::size_t maximum)
{
    Require(!name.empty() && name.size() <= maximum, "name length");
    for (std::size_t i = 0; i < name.size();)
    {
        const auto lead = static_cast<unsigned char>(name[i]);
        if (lead < 0x80)
        {
            Require(lead >= 0x20 && lead != 0x7f, "name control character");
            ++i;
            continue;
        }
        const std::size_t count = lead >= 0xc2 && lead <= 0xdf ? 2 :
                                  lead >= 0xe0 && lead <= 0xef ? 3 :
                                  lead >= 0xf0 && lead <= 0xf4 ? 4 : 0;
        Require(count && count <= name.size() - i, "name UTF-8");
        for (std::size_t j = 1; j < count; ++j)
            Require((static_cast<unsigned char>(name[i + j]) & 0xc0) == 0x80,
                    "name UTF-8 continuation");
        const auto second = static_cast<unsigned char>(name[i + 1]);
        Require(!(lead == 0xe0 && second < 0xa0) &&
                !(lead == 0xed && second >= 0xa0) &&
                !(lead == 0xf0 && second < 0x90) &&
                !(lead == 0xf4 && second >= 0x90), "name UTF-8 code point");
        i += count;
    }
}

void ValidateTransform(const CharacterLocalTransform &transform)
{
    const auto finite = [](float value) { return std::isfinite(value); };
    Require(std::ranges::all_of(transform.translation, finite) &&
            std::ranges::all_of(transform.rotation, finite) &&
            std::ranges::all_of(transform.scale, finite), "nonfinite transform");
    float norm{};
    for (const auto value : transform.rotation) norm += value * value;
    norm = std::sqrt(norm);
    Require(std::isfinite(norm) && norm >= 0.999f && norm <= 1.001f, "quaternion normalization");
    Require(std::ranges::all_of(transform.scale, [](float value) {
        return std::abs(value) > 0.0001f;
    }), "singular transform scale");
}

void Validate(const CharacterAnimationExtensionData &animation)
{
    const auto bones = animation.bone_names.size();
    Require(animation.character_asset_id.value != 0 && bones > 0 && bones <= kMaxCharacterBones,
            "character identity or bone count");
    Require(animation.sample_rate_hz == 0 || animation.sample_rate_hz == 60,
            "unsupported nominal sample rate");
    Require(animation.parents.size() == bones && animation.inverse_bind_matrices.size() == bones,
            "skeleton array counts");
    std::unordered_set<std::string> bone_names;
    std::size_t roots{};
    for (std::size_t i = 0; i < bones; ++i)
    {
        ValidateName(animation.bone_names[i], 256);
        Require(bone_names.insert(animation.bone_names[i]).second, "duplicate bone name");
        if (animation.parents[i] == std::numeric_limits<std::uint16_t>::max()) ++roots;
        else Require(animation.parents[i] < i, "parent must precede child");
        Require(std::ranges::all_of(animation.inverse_bind_matrices[i], [](float value) {
            return std::isfinite(value);
        }), "nonfinite inverse bind");
    }
    Require(roots == 1, "skeleton must have one root");
    Require(!animation.clips.empty() && animation.clips.size() <= 128 &&
            !animation.transforms.empty() && animation.transforms.size() <= kMaxTransforms &&
            animation.sockets.size() <= 128, "animation capacity");
    std::unordered_set<std::uint64_t> clip_ids;
    std::unordered_set<std::string> clip_names;
    std::size_t next{};
    for (const auto &clip : animation.clips)
    {
        ValidateName(clip.name, 128);
        Require(clip.id.value != 0 && clip_ids.insert(clip.id.value).second &&
                clip_names.insert(clip.name).second, "duplicate clip identity");
        Require(clip.first_transform == next && clip.frame_count >= 2 &&
                std::isfinite(clip.duration_seconds) && clip.duration_seconds > 0.0f,
                "clip range or duration");
        Require(next <= animation.transforms.size() &&
                clip.frame_count <= (animation.transforms.size() - next) / bones,
                "clip transform range");
        next += static_cast<std::size_t>(clip.frame_count) * bones;
    }
    Require(next == animation.transforms.size(), "unreferenced transforms");
    for (const auto &transform : animation.transforms) ValidateTransform(transform);
    std::unordered_set<std::string> socket_names;
    for (const auto &socket : animation.sockets)
    {
        ValidateName(socket.name, 128);
        Require(socket_names.insert(socket.name).second && socket.bone_index < bones,
                "duplicate socket or invalid bone");
        ValidateTransform(socket.local_transform);
    }
}

void EncodeTransform(std::span<std::byte> bytes, std::size_t offset,
                     const CharacterLocalTransform &transform)
{
    for (const auto value : transform.translation) { PutFloat(bytes, offset, value); offset += 4; }
    for (const auto value : transform.rotation) { PutFloat(bytes, offset, value); offset += 4; }
    for (const auto value : transform.scale) { PutFloat(bytes, offset, value); offset += 4; }
}

CharacterLocalTransform DecodeTransform(std::span<const std::byte> bytes, std::size_t offset)
{
    CharacterLocalTransform transform{};
    for (auto &value : transform.translation) { value = GetFloat(bytes, offset); offset += 4; }
    for (auto &value : transform.rotation) { value = GetFloat(bytes, offset); offset += 4; }
    for (auto &value : transform.scale) { value = GetFloat(bytes, offset); offset += 4; }
    return transform;
}

const ChunkView &Find(const PackageView &package, ChunkType type)
{
    const auto found = std::ranges::find(package.chunks, type, &ChunkView::type);
    Require(found != package.chunks.end() && found->required, "missing required chunk");
    return *found;
}
} // namespace

std::vector<std::byte> BuildCharacterAnimationExtension(
    AssetId asset_id, std::span<const std::string> dependencies,
    const CharacterAnimationExtensionData &animation)
{
    Validate(animation);
    const auto bones = animation.bone_names.size();
    std::vector<std::byte> metadata(kCharacterAnimationMetadataStride);
    Put(metadata, 0, animation.character_asset_id.value, 8);
    Put(metadata, 8, bones);
    Put(metadata, 12, animation.clips.size());
    Put(metadata, 16, animation.transforms.size());
    Put(metadata, 20, animation.sockets.size());
    Put(metadata, 24, animation.sample_rate_hz);
    std::vector<std::byte> parents(bones * 2), inverse(bones * 64);
    std::vector<std::byte> names(bones * 8);
    const auto append_name = [&](std::string_view name, std::span<std::byte> record,
                                 std::size_t offset) {
        Require(names.size() <= kMaxNamesBytes && name.size() <= kMaxNamesBytes - names.size(),
                "names capacity");
        Put(record, offset, names.size());
        Put(record, offset + 4, name.size());
        for (const auto byte : name) names.push_back(std::byte(static_cast<unsigned char>(byte)));
    };
    // names may reallocate while appending, so write its bone directory after
    // the append rather than retaining a span into the vector across growth.
    for (std::size_t i = 0; i < bones; ++i)
    {
        const auto offset = names.size();
        Require(animation.bone_names[i].size() <= kMaxNamesBytes - offset, "names capacity");
        for (const auto byte : animation.bone_names[i])
            names.push_back(std::byte(static_cast<unsigned char>(byte)));
        Put(names, i * 8, offset);
        Put(names, i * 8 + 4, animation.bone_names[i].size());
        Put(parents, i * 2, animation.parents[i], 2);
        for (std::size_t j = 0; j < 16; ++j)
            PutFloat(inverse, i * 64 + j * 4, animation.inverse_bind_matrices[i][j]);
    }
    std::vector<std::byte> clips(animation.clips.size() * kCharacterNamedClipStride);
    for (std::size_t i = 0; i < animation.clips.size(); ++i)
    {
        const auto &clip = animation.clips[i];
        const auto offset = i * kCharacterNamedClipStride;
        Put(clips, offset, clip.id.value, 8);
        append_name(clip.name, clips, offset + 8);
        Put(clips, offset + 16, clip.first_transform);
        Put(clips, offset + 20, clip.frame_count);
        PutFloat(clips, offset + 24, clip.duration_seconds);
        Put(clips, offset + 28, clip.looping ? 1 : 0);
    }
    std::vector<std::byte> transforms(animation.transforms.size() * 40);
    for (std::size_t i = 0; i < animation.transforms.size(); ++i)
        EncodeTransform(transforms, i * 40, animation.transforms[i]);
    std::vector<std::byte> sockets(animation.sockets.size() * kCharacterAnimationSocketStride);
    for (std::size_t i = 0; i < animation.sockets.size(); ++i)
    {
        const auto &socket = animation.sockets[i];
        const auto offset = i * kCharacterAnimationSocketStride;
        append_name(socket.name, sockets, offset);
        Put(sockets, offset + 8, socket.bone_index);
        EncodeTransform(sockets, offset + 12, socket.local_transform);
    }
    const auto chunk = [](ChunkType type, std::span<const std::byte> bytes,
                          std::uint64_t count, std::uint32_t stride) {
        return ChunkSource{static_cast<std::uint32_t>(type), 1, true, bytes, count, stride};
    };
    std::vector<ChunkSource> chunks{
        chunk(ChunkType::CharacterAnimationMetadata, metadata, 1, kCharacterAnimationMetadataStride),
        chunk(ChunkType::CharacterParents, parents, bones, 2),
        chunk(ChunkType::CharacterInverseBind, inverse, bones, 64),
        chunk(ChunkType::CharacterNamedClips, clips, animation.clips.size(), kCharacterNamedClipStride),
        chunk(ChunkType::CharacterLocalTransforms, transforms, animation.transforms.size(), 40),
        chunk(ChunkType::CharacterAnimationNames, names, names.size(), 1)};
    if (!sockets.empty())
        chunks.push_back(chunk(ChunkType::CharacterAnimationSockets, sockets,
                               animation.sockets.size(), kCharacterAnimationSocketStride));
    return BuildPackage(asset_id, dependencies, chunks);
}

CharacterAnimationExtensionData DecodeCharacterAnimationExtension(
    std::span<const std::byte> bytes, AssetId expected_asset_id)
{
    const auto package = ReadPackage(bytes, expected_asset_id);
    const auto &metadata = Find(package, ChunkType::CharacterAnimationMetadata);
    CharacterAnimationExtensionData animation;
    animation.character_asset_id = AssetId{Get(metadata.bytes, 0, 8)};
    const auto bones = static_cast<std::uint32_t>(Get(metadata.bytes, 8));
    const auto clip_count = static_cast<std::uint32_t>(Get(metadata.bytes, 12));
    const auto transform_count = static_cast<std::uint32_t>(Get(metadata.bytes, 16));
    const auto socket_count = static_cast<std::uint32_t>(Get(metadata.bytes, 20));
    animation.sample_rate_hz = static_cast<std::uint32_t>(Get(metadata.bytes, 24));
    Require(Get(metadata.bytes, 28) == 0, "metadata reserved field");
    Require(bones > 0 && bones <= kMaxCharacterBones && clip_count > 0 && clip_count <= 128 &&
            transform_count > 0 && transform_count <= kMaxTransforms && socket_count <= 128,
            "metadata capacity");
    Require(package.chunks.size() == (socket_count ? 7 : 6), "unexpected chunk count");
    const auto &parents = Find(package, ChunkType::CharacterParents);
    const auto &inverse = Find(package, ChunkType::CharacterInverseBind);
    const auto &clips = Find(package, ChunkType::CharacterNamedClips);
    const auto &transforms = Find(package, ChunkType::CharacterLocalTransforms);
    const auto &names = Find(package, ChunkType::CharacterAnimationNames);
    Require(parents.count == bones && inverse.count == bones && clips.count == clip_count &&
            transforms.count == transform_count && names.bytes.size() >= bones * 8,
            "metadata and chunk counts disagree");
    std::size_t next_name = bones * 8;
    const auto read_name = [&](std::span<const std::byte> record, std::size_t offset) {
        const auto start = static_cast<std::size_t>(Get(record, offset));
        const auto length = static_cast<std::size_t>(Get(record, offset + 4));
        Require(start == next_name && start <= names.bytes.size() &&
                length <= names.bytes.size() - start, "noncanonical name range");
        next_name += length;
        return std::string(reinterpret_cast<const char *>(names.bytes.data() + start), length);
    };
    animation.bone_names.reserve(bones);
    animation.parents.resize(bones);
    animation.inverse_bind_matrices.resize(bones);
    for (std::size_t i = 0; i < bones; ++i)
    {
        animation.bone_names.push_back(read_name(names.bytes, i * 8));
        animation.parents[i] = static_cast<std::uint16_t>(Get(parents.bytes, i * 2, 2));
        for (std::size_t j = 0; j < 16; ++j)
            animation.inverse_bind_matrices[i][j] = GetFloat(inverse.bytes, i * 64 + j * 4);
    }
    animation.clips.reserve(clip_count);
    for (std::size_t i = 0; i < clip_count; ++i)
    {
        const auto offset = i * kCharacterNamedClipStride;
        const auto flags = Get(clips.bytes, offset + 28);
        Require(flags <= 1, "clip flags");
        animation.clips.push_back({AssetId{Get(clips.bytes, offset, 8)}, read_name(clips.bytes, offset + 8),
            static_cast<std::uint32_t>(Get(clips.bytes, offset + 16)),
            static_cast<std::uint32_t>(Get(clips.bytes, offset + 20)),
            GetFloat(clips.bytes, offset + 24), flags != 0});
    }
    animation.transforms.reserve(transform_count);
    for (std::size_t i = 0; i < transform_count; ++i)
        animation.transforms.push_back(DecodeTransform(transforms.bytes, i * 40));
    if (socket_count)
    {
        const auto &sockets = Find(package, ChunkType::CharacterAnimationSockets);
        Require(sockets.count == socket_count, "socket count mismatch");
        animation.sockets.reserve(socket_count);
        for (std::size_t i = 0; i < socket_count; ++i)
        {
            const auto offset = i * kCharacterAnimationSocketStride;
            animation.sockets.push_back({read_name(sockets.bytes, offset),
                static_cast<std::uint32_t>(Get(sockets.bytes, offset + 8)),
                DecodeTransform(sockets.bytes, offset + 12)});
        }
    }
    Require(next_name == names.bytes.size(), "unreferenced names");
    Validate(animation);
    return animation;
}

namespace
{
template <typename Character>
void ValidateBinding(const CharacterAnimationExtensionData &animation,
    const Character &character, AssetId expected_character_asset_id)
{
    Validate(animation);
    Require(expected_character_asset_id.value != 0 &&
            animation.character_asset_id.value == expected_character_asset_id.value,
            "character asset identity mismatch");
    Require(animation.bone_names.size() == character.bone_count &&
            animation.parents == character.parents &&
            animation.inverse_bind_matrices == character.inverse_bind_matrices,
            "ordered skeleton binding mismatch");
}
} // namespace

void ValidateCharacterAnimationBinding(
    const CharacterAnimationExtensionData &animation,
    const CharacterMetadataPackageData &character, AssetId expected_character_asset_id)
{
    ValidateBinding(animation, character, expected_character_asset_id);
}

void ValidateCharacterAnimationBinding(
    const CharacterAnimationExtensionData &animation,
    const CharacterPackageData &character, AssetId expected_character_asset_id)
{
    ValidateBinding(animation, character, expected_character_asset_id);
}
} // namespace hs::render_data
