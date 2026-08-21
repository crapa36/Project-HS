# Project HS

Project-specific contracts only.

## Project Context

`ARCHITECTURE.md` is the canonical architecture map.

Read it when changing module ownership, dependencies, public APIs, build layering,
or runtime composition.

More specific rules exist under:

- `Source/Gameplay/AGENTS.md`
- `Source/RendererD3D12/AGENTS.md`

Production modules communicate through another module's `Public` API.

Do not include another module's `Private` implementation.

Do not move a higher-level type into Core merely to bypass a dependency rule.

`Tests/check_architecture.cmake` mechanically enforces the module graph.

## Build Isolation

The low-level configuration intentionally uses:

```text
HS_BUILD_RUNTIME=OFF
HS_BUILD_CONTENT_TOOLS=OFF
```

It must remain possible to build and verify the low-level modules without
runtime- or content-only prerequisites.

Do not introduce a low-level dependency that makes `verify-core` require the
D3D12 runtime, runtime fonts, DXC, DirectXTex, or FBX SDK.

## Deterministic Gameplay

Gameplay is deterministic fixed-step simulation.

Preserve:

* `/fp:strict`;
* explicit seeds;
* deterministic iteration where state or checksums are affected;
* fixed-step semantics;
* simulation phase ordering.

`Source/Gameplay/Private/simulation_pipeline.hpp` is the authoritative phase
order.

An unexpected gameplay checksum change is a regression until its semantic cause
is established.

Do not update an oracle merely to make structural or implementation-only work
pass.

The simulation clock and content cooker share the project's 60 Hz tick
convention. A deliberate tick-rate change must audit both sides and all affected
tick-based rules and tests.

## Content and Generated Output

Authoritative inputs live under:

```text
ContentSource/
Schemas/
Content/Shaders/
```

Cooked and generated build outputs are not authoritative sources.

Change their upstream source input, schema, cooker, shader source, or build rule
instead of editing generated output.

Simulation and presentation cooked data remain separate:

```text
simulation_rules.hsbin
presentation_catalog.hsbin
```

Do not recombine presentation-only data with simulation ownership for
convenience.

Full content builds require the repository's configured Autodesk FBX SDK
environment.

## Verification

For low-level module, architecture, or deterministic simulation changes:

```powershell
cmake --workflow --preset verify-core
```

`verify-core` uses the repository's current low-level build and test preset
without Runtime or content tools.

For changes requiring Runtime, renderer, content cooking, assets, or integration
behavior:

```powershell
cmake --workflow --preset verify
```

Use the existing GPU-validation configuration when investigating D3D12
synchronization, barrier, resource-state, resource-lifetime, or GPU-validation
failures.

Shader hot-reload validation requires the repository's Debug validation path.

If an external prerequisite is unavailable, report the unavailable prerequisite
rather than weakening the check.
