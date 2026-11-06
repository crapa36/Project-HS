#include <hs/runtime/prepared_content.hpp>
#include <hs/core/shipped_content_owner.hpp>
#include <hs/game_rules/simulation_rules_io.hpp>
#include <hs/presentation/presentation_catalog_io.hpp>
#include <hs/runtime/vfx_image_io.hpp>
#include <hs/runtime/audio_engine.hpp>
#include <utility>
#ifdef HS_DEVELOPMENT_TOOLS
#include <hs/asset_manifest/selected_generation.hpp>
#endif

namespace hs::shipping
{
PreparedContent::PreparedContent() = default;
PreparedContent::~PreparedContent() = default;
PreparedContent::PreparedContent(PreparedContent &&) noexcept = default;
PreparedContent &PreparedContent::operator=(PreparedContent &&) noexcept = default;

Result LoadPreparedContent(const std::filesystem::path &directory,
                           const ShippingImageInfo &info, PreparedContent &output)
{
    PreparedContent candidate;
    candidate.owner = std::make_shared<ShippedContentOwner>();
    if (auto result = OpenImage(directory, info, *candidate.owner); !result) return result;
    candidate.simulation = std::make_unique<SimulationRules>();
    std::vector<std::byte> bytes;
    const auto read = [&](ShippingTable table) {
        return ReadRange(*candidate.owner, info.tables[static_cast<std::size_t>(table)], bytes);
    };
    if (auto result = read(ShippingTable::Simulation); !result) return result;
    if (auto result = DecodeSimulationRules(bytes, *candidate.simulation); !result) return result;
    if (auto result = read(ShippingTable::Presentation); !result) return result;
    if (auto result = DecodePresentationCatalog(bytes, candidate.presentation); !result) return result;
    if (auto result = read(ShippingTable::Audio); !result) return result;
    if (auto result = DecodeAudioImage(bytes, candidate.owner->Size(), candidate.audio); !result) return result;
    if (auto result = PrepareAudio(candidate.audio.View(), *candidate.owner, candidate.prepared_audio); !result) return result;
    if (auto result = read(ShippingTable::Renderer); !result) return result;
    if (auto result = render_data::DecodeRendererCatalog(bytes, candidate.owner->Size(), candidate.renderer); !result) return result;
    std::array<std::vector<std::byte>, 2> vfx;
    for (std::size_t i = 0; i < vfx.size(); ++i)
        if (auto result = ReadRange(*candidate.owner, info.vfx[i], vfx[i]); !result) return result;
    if (auto result = DecodeVfxImage(vfx[0], vfx[1], candidate.vfx); !result) return result;
    if (!candidate.vfx.has_required_sprite_bindings)
        return Result::Failure(ErrorCode::InvalidArgument, "ShippingImage", "VFX omits required sprite bindings.");
    candidate.content_hash = info.content_hash;
    output = std::move(candidate);
    return Result::Success();
}

#ifdef HS_DEVELOPMENT_TOOLS
Result LoadPreparedVfx(const SelectedGeneration &generation, VfxImageData &output)
{
    std::filesystem::path path, program;
    if (auto result = generation.Require("vfx/particle_effects.hsbin", path); !result) return result;
    if (auto result = generation.Require("vfx/vfx_program.hsbin", program); !result) return result;
    return LoadVfxImage(path, program, output);
}

Result LoadPreparedContent(SelectedGeneration generation, PreparedContent &output)
{
    PreparedContent candidate;
    candidate.generation = std::make_shared<const SelectedGeneration>(std::move(generation));
    candidate.simulation = std::make_unique<SimulationRules>();
    std::filesystem::path path;
    if (auto result = candidate.generation->Require("game-data/simulation-rules", path); !result) return result;
    if (auto result = LoadSimulationRules(path, *candidate.simulation, &candidate.content_hash); !result) return result;
    if (auto result = candidate.generation->Require("game-data/presentation-catalog", path); !result) return result;
    if (auto result = LoadPresentationCatalog(path, candidate.presentation, &candidate.content_hash); !result) return result;
    if (auto result = LoadPreparedVfx(*candidate.generation, candidate.vfx); !result) return result;
    if (auto result = PrepareAudio(candidate.generation->product_paths, candidate.prepared_audio); !result) return result;
    output = std::move(candidate);
    return Result::Success();
}
#endif
}
