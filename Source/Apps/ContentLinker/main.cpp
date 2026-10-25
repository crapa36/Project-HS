#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../AssetPipeline/action.hpp"
#include "../Content/character_products.hpp"
#include "../Content/release_vfx_codegen.hpp"
#include <hs/runtime/vfx_image_io.hpp>

#include <charconv>
#include <cstdint>
#include <iostream>
#include <map>
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

void Require(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error("hs_content_linker: " + std::string(message));
}

std::span<const std::byte> Bytes(std::string_view text)
{
    return std::as_bytes(std::span(text.data(), text.size()));
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
    put(8, 1, 4);  // Format version.
    put(12, 7, 4); // Typed tables plus a bounded packed font range.
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
          std::span<const std::string> binding_results)
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
    spec.recipe_version = 8;
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
    const auto action_key = ap::ActionKey(spec);
    Require(action_key.starts_with("sha256:") && action_key.size() == 71,
            "invalid shipping action key");
    const auto build_id = action_key.substr(7);
    const auto font_offset = 64 + pcm.size();
    const auto header = Header(build_id, font_offset + font.size());
    auto pack = std::move(pcm);
    pack += font;
    pack.insert(0, header);
    source += ImageSource(header, content_hash, font_offset, font.size(), font_digest);

    const auto pack_path = build_root / HS_SHIPPING_PACK_RELATIVE_PATH;
    ap::MakeDirectory(pack_path.parent_path());
    const auto shipping = build_root / "Shipping";
    ap::MakeDirectory(shipping);
    // Atomic replacement preserves previously opened handles. The pair is not a
    // transaction: an interrupted publish is rejected by exact header matching.
    ap::AtomicWriteIfDifferent(pack_path, pack);
    ap::AtomicWriteIfDifferent(shipping / "content.generated.cpp", source);
    std::cout << "shipping-image published " << build_id << '\n';
}
}

int wmain(int argc, wchar_t **argv)
{
    fs::path build_root;
    try
    {
        Require(argc >= 5 && std::wstring_view(argv[1]) == L"--build-root" && std::wstring_view(argv[3]) == L"--font",
                "usage: hs_content_linker --build-root <absolute> --font <pinned-ttf> --character <result> <product> [--binding-result <result> ...]");
        build_root = fs::path(argv[2]).lexically_normal();
        std::vector<std::pair<std::string, std::string>> characters;
        std::vector<std::string> binding_results;
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
            else Require(false, "invalid or incomplete character binding argument");
        }
        Require(!characters.empty(), "shipping image requires character products");
        Link(build_root, fs::absolute(argv[4]).lexically_normal(), characters, binding_results);
        return 0;
    }
    catch (const std::exception &error)
    {
        ap::ReportFailure("hs_content_linker", error.what(),
                          {{"build_root", ap::DiagnosticPath(build_root)}});
        return 1;
    }
}
