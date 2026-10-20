# Project HS Architecture

This document describes the module graph enforced by the current build and `Tests/check_architecture.cmake`. It is not a future-system roadmap.

## Module DAG

```text
                         Core
              ┌──────────┼───────────────┐
              ▼          ▼               ▼
            Jobs     GameDomain       RenderData
                         │                │
                         │                ▼
                         │           RendererD3D12
                 ┌───────┴────────┐
                 ▼                ▼
             GameRules       Presentation
                 │
                 ▼
              Gameplay
```

Runtime composes Core, Jobs, Gameplay, Presentation, and RendererD3D12. Apps are executable entry points and composition targets.

| Module | Responsibility | Allowed Project HS dependencies |
|---|---|---|
| Core | Foundation types and shared low-level contracts | none |
| AssetManifest | Selected Generation manifest parsing and product path lookup | Core |
| RenderData | Versioned CPU render packages and material metadata | Core |
| Jobs | Task-system integration | Core |
| GameDomain | Stable domain/read-model contracts | Core |
| GameRules | Rules independent of mutable simulation state | Core, GameDomain |
| Gameplay | Deterministic mutable simulation | Core, GameDomain, GameRules |
| Presentation | Domain/read-model projection | Core, GameDomain |
| RendererD3D12 | Concrete Windows/D3D12 backend | Core, RenderData |
| Runtime | Concrete subsystem composition and orchestration | Core, Jobs, Gameplay, Presentation, AssetManifest, RendererD3D12 |
| Apps | Argument handling and entry-point composition | approved public libraries |

## Forbidden dependencies

- Core must not depend on any higher Project HS module.
- Jobs must not depend on game, presentation, renderer, or runtime modules.
- GameDomain must not depend on Jobs or any higher game/runtime module.
- GameRules must not depend on Jobs, Gameplay, Presentation, RendererD3D12, or Runtime.
- Gameplay must not depend on Jobs, Presentation, RendererD3D12, or Runtime.
- Presentation must not depend on Jobs, GameRules, Gameplay, RendererD3D12, or Runtime.
- AssetManifest must not depend on Jobs, game, presentation, RenderData, renderer, or runtime modules.
- RenderData must not depend on Jobs, game, presentation, renderer, or runtime modules.
- RendererD3D12 must not depend on Jobs, game, presentation, or runtime modules.
- Production modules must not include another module's `Private` headers.

Moving a higher-layer type into Core only to bypass these rules is also a dependency violation. Keep types at the lowest layer that owns their meaning.

## Deterministic simulation boundary

Gameplay owns fixed-step state transitions, movement, combat, collision, statuses, progression, cleanup, and deterministic checksums.

Gameplay must not contain D3D12/DXGI, renderer, presentation, UI/window, audio, filesystem/runtime orchestration, or wall-clock-driven game decisions.

Structural changes must preserve phase order, fixed-step behavior, seeds, `/fp:strict`, deterministic iteration, checksums, and public read-model semantics. `Source/Gameplay/Private/simulation_pipeline.hpp` is the authoritative phase order.

Checksum capture is optional observability, not a gameplay decision. Existing
simulation callers default to capture enabled; ordinary Runtime play disables
it while smoke, recording and replay retain capture. Disabled TickResult and
read-model checksums are zero. Explicit `ComputeChecksum()` still computes the
same canonical state hash on demand without enabling automatic capture.

## Presentation and rendering

Presentation projects stable GameDomain contracts. It does not execute rules or reach into Gameplay implementation state.

RendererD3D12 is the single concrete platform backend. D3D12 types stay inside that boundary and Runtime/Apps that compose it. A generic renderer interface, factory, or registry is unwarranted without a real second backend.

AO, GI, reconstruction and post processing connect through their concrete inputs.
The FrameGraph owns resource dependency ordering, versions and lifetimes; there
is no second semantic registration graph above it. The post compiler emits the
configured fixed pass chain. Native services validate actual resource formats
and extents at their boundaries.

Sequential views retain their own temporal and native reconstruction state.
There is no parallel semantic history-resource registry: resources and history
generation owned by each view are authoritative. Main renders first; auxiliary
views retain packet order and share canonical scene simulation. Shared descriptor
banks still require completion before view exchange.
Capture callers use `Capture(request)` with a view ID. Legacy format-specific
capture helpers are private, so callers cannot bypass submitted-frame ownership
checks. Shared geometry/visibility diagnostics require a retained submission
belonging to the requested view.

Material emitters validate a graph once before optimization. Deferred closure
emission derives sidecar families from that same graph; callers do not supply a
second mask. Authored graph operations and parameter-only cooking remain shared.

## Runtime and Apps

Runtime is the composition root, so it may know the concrete systems it runs. Cross-subsystem knowledge in Runtime is not by itself an architecture violation.

Runtime loads saved settings once and applies explicit command-line overrides
before creating the window. Apps parse arguments once and do not load profiles.
Smoke uses process graphics defaults and explicit overrides.

Apps parse arguments, select configuration, call a library/runtime entry point, and return process results. They must use public module APIs rather than another module's private implementation.

Jobs submission takes a task name and returns admission success. TaskGroup owns
cancellation and joining; unused per-task numeric handles and scheduling priority
are not exposed.

Experiment children execute their validated spec directly. The parent retains
process isolation and a watchdog; static timeline commands are not retransmitted
through a separate IPC protocol. Runtime holds its selected simulation rules for
the session. Rebuilding and restarting selects a new Generation.

## Public and Private boundaries

Each module's `Public` directory is its supported cross-module API. `Private` is implementation detail. Tests should prefer public behavior; direct private testing is acceptable only for an existing, legitimate private unit.

## Build isolation

Material graph validation/optimization/HLSL generation is the RenderData-owned
`hs_material_compiler` target, enabled only for content tools or RenderData tests.
Only the material shader producer, render package producer and material package
tests link it. `hs_render_data` contains runtime data formats without depending
on this compiler, so compiler edits do not relink unrelated producers or the game.

`hs_environment_layout` is an internal Presentation target that compiles the
existing environment builder once. `hs_presentation` exposes it transitively;
layout-only scene producers link it directly. `hs_generation_lock` similarly
compiles AssetManifest publication locking once and is exposed by
`hs_asset_manifest`. Action producers link this narrow locking target; consumers
that parse selected manifests still link `hs_asset_manifest`. These targets retain
their module ownership and ordinary compiler/Ninja dependency tracking, so unrelated
presentation or selected-manifest edits do not invalidate producer recipes.

Asset recipes identify the linked producer executable by SHA-256. CMake/Ninja
owns source/header rebuild dependencies; a second source/compiler/link-command
authentication graph is not maintained. Explicit asset and shader-tool inputs
still participate in each Action key. Equivalent but byte-different relinks can
recook assets; identical products still retain their ProductSet identity.
The Apps-owned `hs_asset_pipeline` static library compiles the shared Action
implementation once. Producers remain separate executables, so a producer-specific
change does not invalidate unrelated recipes. Each process hashes its own executable;
there is no intermediate recipe manifest or post-link preparation target.

The low-level configuration is:

```text
HS_BUILD_RUNTIME=OFF
HS_BUILD_CONTENT_TOOLS=OFF
```

It builds Core, Jobs, GameDomain, GameRules, Gameplay, Presentation, and their low-level tests without AssetManifest, RenderData, ImGui, Agility SDK, PIX, D3D12 Memory Allocator, runtime fonts, DXC, DirectXTex, or FBX SDK. Shader infrastructure exists only when Runtime or content tools need it. Runtime and content options default to `ON`.

## Portable verification

Runtime VFX cases share two test executables, grouped by synthetic versus cooked
inputs. Each case remains a separate CTest process and name. Only cooked cases
prepare a Generation; compiling one group never compiles the other group's cases.

`cmake --workflow --preset verify-core-linux` builds the platform-independent
libraries and runs their low-level tests with GCC/Clang strict floating-point
options. The Windows process APIs and MSVC `/fp:strict` path remain intact.
Linux verification does not stand in for MSVC, D3D12, or GPU execution.

`HS_BUILD_RENDER_DATA_TESTS=ON` explicitly opts into CPU RenderData, selected
Generation, character-interchange, and the private production FrameGraph range
compiler checks without enabling the runtime or
FBX/DXC content tooling. This option defaults to OFF, so the ordinary low-level
configuration retains its dependency isolation. The real asset-action scheduler
regression additionally uses Python when available with the Ninja generator.

`HS_BUILD_ASSET_PIPELINE_TESTS=ON` separately opts into the real portable Audio
and Generation executables plus publication fixtures. Core owns shared streaming
SHA-256; RenderData retains its public wrapper. Neither opt-in enables D3D12 or
content SDK dependencies. Both remain OFF by default.

## Mechanical enforcement

`Tests/check_architecture.cmake` scans production includes, Gameplay graphics API knowledge, cross-module `Private` access, and major `target_link_libraries` edges. It intentionally uses CMake text checks; no parser framework or second build graph is needed for the current rules.

Code, target links, and executable tests are the source of truth if this document becomes stale. Verification policy is defined by the applicable `AGENTS.md`.
