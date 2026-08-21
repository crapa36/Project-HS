# Project HS

Project-specific rules only. Do not duplicate general engineering guidance here.

## Canonical Project Context

Read `ARCHITECTURE.md` before changing module boundaries, ownership, public APIs, or build structure.

More specific rules override this file:

* `Source/Gameplay/AGENTS.md`
* `Source/RendererD3D12/AGENTS.md`

`Tests/check_architecture.cmake` mechanically enforces the module graph.

## Build Contract

Current project toolchain:

* Windows
* MSVC
* Ninja
* CMake 3.30+
* C++23
* Direct3D 12 runtime

The main build switches are:

```text
HS_BUILD_RUNTIME
HS_BUILD_CONTENT_TOOLS
HS_BUILD_TESTS
HS_ENABLE_VALIDATION
HS_ENABLE_GPU_VALIDATION
```

Runtime and content tools default to enabled.

The low-level configuration intentionally disables both:

```text
HS_BUILD_RUNTIME=OFF
HS_BUILD_CONTENT_TOOLS=OFF
```

This path must remain able to build and test Core, Jobs, GameDomain, GameRules, Gameplay, and Presentation without requiring the D3D12 runtime or content toolchain.

Do not introduce a low-level dependency that makes `verify-core` require runtime/content-only prerequisites.

## Architecture

The canonical module DAG is defined in `ARCHITECTURE.md`.

Cross-module production code must use another module's `Public` API. Do not include another module's `Private` implementation.

Do not move a higher-level type into Core merely to bypass a dependency restriction.

The architecture test is:

```text
architecture.layers
```

Any module-boundary change must keep it passing.

## Deterministic Simulation

Deterministic fixed-step simulation is a project contract.

Gameplay uses MSVC `/fp:strict`, deterministic phase ordering, explicit seeds, and checksum verification.

`Source/Gameplay/Private/simulation_pipeline.hpp` is the authoritative simulation phase order.

An unexpected gameplay checksum change is a regression until the semantic cause is established.

Do not update a checksum oracle merely to accept a structural or implementation-only change.

The simulation clock and content cooker share the 60 Hz tick convention. A tick-rate change must update the clock, content time-to-tick conversion, tick-based rules, and determinism tests together.

Gameplay-specific implementation rules live in `Source/Gameplay/AGENTS.md`.

## Content Pipeline

Authoritative content inputs are under:

```text
ContentSource/
Schemas/
Content/Shaders/
```

The content pipeline is:

```text
ContentSource + Schemas + shader/assets
                ↓
             hs_content
                ↓
        Build/<preset>/Cooked
```

Cooked and generated build outputs are not source files. Change their source input, schema, cooker, shader source, or build rule instead.

Simulation and presentation cooked data are deliberately separate:

```text
simulation_rules.hsbin
presentation_catalog.hsbin
```

Do not recombine simulation ownership with presentation-only data for convenience.

Full content builds require Autodesk FBX SDK 2020.3.7 for VS2022. `HS_FBX_SDK_ROOT` may override its default location.

## Verification

For Core, Jobs, GameDomain, GameRules, Gameplay, Presentation, architecture, or low-level build changes:

```powershell
cmake --workflow --preset verify-core
```

`verify-core` uses `msvc-core`, builds the low-level test targets, and runs the foundation, architecture, rules, and determinism-core checks without Runtime or content tools.

For changes that require Runtime, renderer, content cooking, assets, or integration behavior:

```powershell
cmake --workflow --preset verify
```

Use `msvc-gpu-validation` when investigating D3D12 synchronization, barrier, resource-state, lifetime, or GPU-validation failures.

Shader hot-reload validation requires a Debug configuration.

Renderer-specific requirements live in `Source/RendererD3D12/AGENTS.md`.
