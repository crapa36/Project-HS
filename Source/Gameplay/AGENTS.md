# Gameplay Agent Rules

Read the root `AGENTS.md` and `ARCHITECTURE.md` first.

Gameplay owns deterministic fixed-step simulation and may depend only on Core,
GameDomain, and GameRules.

Preserve:

- `/fp:strict`;
- fixed-step behavior and phase order;
- seeds and deterministic iteration;
- public `GameSimulation` behavior and API;
- checksum expectations unless simulation semantics explicitly change.

Do not introduce:

- wall-clock-driven gameplay decisions;
- D3D12, DXGI, window, UI, audio, renderer, presentation, or runtime knowledge;
- nondeterministic iteration in checksum-affecting behavior;
- virtual systems, factories, registries, an ECS migration, or an event-bus rewrite.

`Private/simulation_pipeline.hpp` is the authoritative phase order. Use its
existing phase groups as extraction seams and prefer code movement, private
declarations, and file-local helpers.

After non-trivial Gameplay changes run:

```powershell
cmake --workflow --preset verify-core
```

Treat any unexpected checksum change as a regression. Do not update an oracle
or weaken a check to make structural work pass.
