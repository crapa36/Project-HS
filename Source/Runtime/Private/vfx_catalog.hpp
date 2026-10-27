#pragma once

#include <hs/core/cooked_particle_effects.hpp>
#include <hs/core/presentation_event.hpp>
#include <hs/core/result.hpp>
#include <hs/render_data/vfx_spawn_command.hpp>
#include <hs/runtime/vfx_image.hpp>
#include <memory>

#include <filesystem>
#include <span>
#include <vector>

namespace hs
{
struct VfxImageData;

class VfxCatalog
{
  public:
    VfxCatalog() noexcept;
    ~VfxCatalog();
    VfxCatalog(VfxCatalog &&other) noexcept;
    VfxCatalog &operator=(VfxCatalog &&other) noexcept;
    VfxCatalog(const VfxCatalog &) = delete;
    VfxCatalog &operator=(const VfxCatalog &) = delete;
    // External immutable backing must outlive the catalog and its users.
    // Never bind a shallow copy of this catalog's owned image; direct self-bind is a no-op.
    void Bind(const VfxImage &image) noexcept;
    [[nodiscard]] const VfxImage &Image() const noexcept { return image_; }
    [[nodiscard]] static Result Load(const std::filesystem::path &path,
                                     VfxCatalog &catalog);
    [[nodiscard]] static Result Load(const std::filesystem::path &path,
                                     const std::filesystem::path &program_path,
                                     VfxCatalog &catalog);
    [[nodiscard]] const CookedVfxDefinition *FindEffect(EffectId id) const noexcept;
    [[nodiscard]] const CookedParticleSprite *FindSprite(AssetId id) const noexcept;
    [[nodiscard]] Result ExpandEvent(const PresentationEvent &event,
                                float decorative_fraction,
                                std::vector<ParticleSpawnCommand> &particles,
                                std::vector<VfxLineSpawnCommand> &lines,
                                std::span<const Sequence> replaced_particle_sequences = {}) const;
    [[nodiscard]] std::uint64_t PayloadHash() const noexcept { return image_.payload_hash; }
    [[nodiscard]] std::uint32_t SpriteCount() const noexcept { return image_.sprite_count; }
    [[nodiscard]] const VfxProgramView &Program() const noexcept { return image_.program; }
    void Swap(VfxCatalog &prepared) noexcept;

  private:
    VfxImage image_{};
    std::unique_ptr<VfxImageData> owned_;
};

} // namespace hs
