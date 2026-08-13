#pragma once

#include <hs/core/presentation_event.hpp>
#include <hs/core/render_snapshot.hpp>
#include <hs/core/settings.hpp>
#include <hs/game_domain/domain_signal.hpp>
#include <hs/game_domain/game_read_model.hpp>
#include <hs/game_rules/simulation_rules.hpp>

namespace hs
{

[[nodiscard]] PresentationEvent ProjectPresentation(const DomainSignal &signal) noexcept;
[[nodiscard]] bool ProjectRenderSnapshot(const GameReadModel &model,
                                         const SimulationRules &rules,
                                         const PresentationCatalog &presentation,
                                         const SettingsData &settings,
                                         RenderSnapshotStorage &snapshot);

} // namespace hs
