# Project HS Architecture

This document describes the module graph enforced by the current build and
`Tests/check_architecture.cmake`. It is not a future-system roadmap.

## Module DAG

```text
                         Core
              ┌──────────┼───────────────┐
              ▼          ▼               ▼
            Jobs     GameDomain      RendererD3D12
                         │
                 ┌───────┴────────┐
                 ▼                ▼
             GameRules       Presentation
                 │
                 ▼
              Gameplay

Runtime composes Core, Jobs, Gameplay, Presentation, and RendererD3D12.
Apps are executable entry points and composition targets.
```

| Module | Responsibility | Allowed Project HS dependencies |
|---|---|---|
| Core | Foundation types and shared low-level contracts | none |
| Jobs | Task-system integration | Core |
| GameDomain | Stable domain/read-model contracts | Core |
| GameRules | Rules independent of mutable simulation state | Core, GameDomain |
| Gameplay | Deterministic mutable simulation | Core, GameDomain, GameRules |
| Presentation | Domain/read-model projection | Core, GameDomain |
| RendererD3D12 | Concrete Windows/D3D12 backend | Core |
| Runtime | Concrete subsystem composition and orchestration | Core, Jobs, Gameplay, Presentation, RendererD3D12 |
| Apps | Argument handling and entry-point composition | approved public libraries |

## Forbidden dependencies

- Core must not depend on any higher Project HS module.
- Jobs must not depend on game, presentation, renderer, or runtime modules.
- GameDomain must not depend on Jobs or any higher game/runtime module.
- GameRules must not depend on Jobs, Gameplay, Presentation, RendererD3D12, or Runtime.
- Gameplay must not depend on Jobs, Presentation, RendererD3D12, or Runtime.
- Presentation must not depend on Jobs, GameRules, Gameplay, RendererD3D12, or Runtime.
- RendererD3D12 must not depend on Jobs, game, presentation, or runtime modules.
- Production modules must not include another module's `Private` headers.

Moving a higher-layer type into Core only to bypass these rules is also a
dependency violation. Keep types at the lowest layer that owns their meaning.

## Deterministic simulation boundary

Gameplay owns fixed-step state transitions, movement, combat, collision,
statuses, progression, cleanup, and deterministic checksums.

Gameplay must not contain D3D12/DXGI, renderer, presentation, UI/window, audio,
filesystem/runtime orchestration, or wall-clock-driven game decisions.

Structural changes must preserve phase order, fixed-step behavior, seeds,
`/fp:strict`, deterministic iteration, checksums, and public read-model semantics.
`Source/Gameplay/Private/simulation_pipeline.hpp` is the authoritative phase order.

## Presentation and rendering

Presentation projects stable GameDomain contracts. It does not execute rules or
reach into Gameplay implementation state.

RendererD3D12 is the single concrete platform backend. D3D12 types stay inside
that boundary and Runtime/Apps that compose it. A generic renderer interface,
factory, or registry is unwarranted without a real second backend.

## Runtime and Apps

Runtime is the composition root, so it may know the concrete systems it runs.
Cross-subsystem knowledge in Runtime is not by itself an architecture violation.

Apps parse arguments, select configuration, call a library/runtime entry point,
and return process results. They must use public module APIs rather than another
module's private implementation.

## Public and Private boundaries

Each module's `Public` directory is its supported cross-module API. `Private` is
implementation detail. Tests should prefer public behavior; direct private
testing is acceptable only for an existing, legitimate private unit.

## Build isolation

The low-level configuration is:

```text
HS_BUILD_RUNTIME=OFF
HS_BUILD_CONTENT_TOOLS=OFF
```

It builds Core, Jobs, GameDomain, GameRules, Gameplay, Presentation, and their
low-level tests without ImGui, Agility SDK, PIX, D3D12 Memory Allocator, runtime
fonts, DXC, DirectXTex, or FBX SDK. Shader infrastructure exists only when
Runtime or content tools need it. Runtime and content options default to `ON`.

## Mechanical enforcement

`Tests/check_architecture.cmake` scans production includes, Gameplay graphics API
knowledge, cross-module `Private` access, and major `target_link_libraries` edges.
It intentionally uses CMake text checks; no parser framework or second build graph
is needed for the current rules.

## Verification

Run the low-level path after Core, Jobs, GameDomain, GameRules, Gameplay,
Presentation, low-level CMake, or architecture changes:

```powershell
cmake --workflow --preset verify-core
```

Run the full integration path when runtime/content prerequisites are available:

```powershell
cmake --workflow --preset verify
```

Code, target links, and executable tests are the source of truth if this document
becomes stale.
