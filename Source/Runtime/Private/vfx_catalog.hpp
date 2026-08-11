#pragma once

#include <hs/core/cooked_particle_effects.hpp>
#include <hs/core/presentation_event.hpp>
#include <hs/core/result.hpp>
#include <hs/renderer/particle_spawn_command.hpp>

#include <filesystem>
#include <span>
#include <vector>

namespace hs
{

class VfxCatalog
{
  public:
    [[nodiscard]] static Result Load(const std::filesystem::path &path,
                                     VfxCatalog &catalog);
    [[nodiscard]] const CookedVfxDefinition *Find(EffectId id) const noexcept;
    [[nodiscard]] const CookedParticleSprite *FindSprite(AssetId id) const noexcept;
    [[nodiscard]] Result Expand(const PresentationEvent &event,
                                std::uint32_t quality_percent,
                                std::vector<ParticleSpawnCommand> &particles,
                                std::vector<EffectLineSpawnCommand> &lines) const;
    [[nodiscard]] std::uint64_t PayloadHash() const noexcept { return payload_hash_; }
    [[nodiscard]] std::uint32_t SpriteCount() const noexcept { return sprite_count_; }

  private:
    std::vector<CookedVfxDefinition> definitions_;
    std::vector<CookedParticleEmitter> emitters_;
    std::vector<CookedParticleSprite> sprites_;
    std::uint32_t sprite_count_{};
    std::uint64_t payload_hash_{};
};

} // namespace hs
