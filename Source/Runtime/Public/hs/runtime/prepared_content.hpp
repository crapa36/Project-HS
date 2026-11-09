#pragma once
#include <hs/runtime/shipping_content.hpp>
#include <hs/runtime/audio_image_io.hpp>
#include <hs/runtime/vfx_image_io.hpp>
#include <hs/game_rules/simulation_rules.hpp>
#include <hs/presentation/presentation_catalog.hpp>
#include <hs/render_data/renderer_catalog.hpp>
#include <memory>

namespace hs { struct SelectedGeneration; }
namespace hs::audio_detail { struct PreparedAudio; }
namespace hs::shipping
{
struct PreparedContent
{
    PreparedContent();
    ~PreparedContent();
    PreparedContent(PreparedContent &&) noexcept;
    PreparedContent &operator=(PreparedContent &&) noexcept;
    PreparedContent(const PreparedContent &) = delete;
    PreparedContent &operator=(const PreparedContent &) = delete;

    std::shared_ptr<ShippedContentOwner> owner;
    std::shared_ptr<const SelectedGeneration> generation;
    std::uint64_t content_hash{};
    std::unique_ptr<SimulationRules> simulation;
    PresentationCatalog presentation;
    AudioImageData audio;
    std::shared_ptr<audio_detail::PreparedAudio> prepared_audio;
    VfxImageData vfx;
    std::shared_ptr<const render_data::RendererCatalogData> renderer;
};
// Admit all immutable tables once; every borrowed row keeps this owner alive.
[[nodiscard]] Result LoadPreparedContent(const std::filesystem::path &directory,
                                         const ShippingImageInfo &, PreparedContent &);
[[nodiscard]] Result LoadPreparedContent(SelectedGeneration generation, PreparedContent &);
[[nodiscard]] Result LoadPreparedVfx(const SelectedGeneration &, VfxImageData &);
}
