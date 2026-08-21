# Gameplay Agent Rules

Gameplay owns the deterministic fixed-step state-transition boundary and may
depend only on Core, GameDomain, and GameRules.

Keep `/fp:strict`, fixed-step semantics, explicit seeds, deterministic iteration,
phase order, and checksum behavior intact. Treat an unexpected checksum change
as a semantic regression; do not update or weaken an oracle for structural work.
Preserve public `GameSimulation` behavior and API.

`Private/simulation_pipeline.hpp` is the authoritative phase order. Use its
existing phase groups as extraction seams and prefer code movement, private
declarations, and file-local helpers.

Gameplay must not know about D3D12, DXGI, renderer, presentation, UI/window,
audio, runtime orchestration, or wall-clock-driven game decisions.

Do not introduce virtual systems, factories, registries, an ECS migration, or an
event-bus rewrite.
