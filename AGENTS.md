# Project HS

Project-wide contracts only.

## Architecture

`ARCHITECTURE.md` is the canonical module map. Read it when changing module ownership, dependencies, public APIs, build layering, or runtime composition.

Scoped rules:

- `Source/Gameplay/AGENTS.md`
- `Source/RendererD3D12/AGENTS.md`

Production modules communicate through another module's `Public` API. Do not include another module's `Private` implementation or move higher-level types into Core to bypass dependency rules.

`Tests/check_architecture.cmake` mechanically enforces the module graph.

## Cross-cutting invariants

The low-level build must remain valid with:

```text
HS_BUILD_RUNTIME=OFF
HS_BUILD_CONTENT_TOOLS=OFF
```

Do not make that path depend on runtime/content-only prerequisites such as the D3D12 runtime, runtime fonts, DXC, DirectXTex, or the FBX SDK.

Gameplay simulation and the content cooker share the project's 60 Hz tick convention. A deliberate tick-rate change must update both sides and affected tick-based rules and tests.

Authoritative content inputs live under:

```text
ContentSource/
Schemas/
Content/Shaders/
```

Do not edit cooked or generated build output as source.

Keep simulation and presentation cooked data separate:

```text
simulation_rules.hsbin
presentation_catalog.hsbin
```

## Verification

Use the narrowest existing check that answers the risk introduced by the change.

For localized changes, prefer the relevant build/test target before broader workflows.

Use:

```powershell
cmake --workflow --preset verify-core
```

when low-level integration, architecture, or deterministic simulation requires the repository's low-level gate.

Use:

```powershell
cmake --workflow --preset verify
```

when Runtime, renderer, content cooking, assets, or integration behavior requires the full gate.

Documentation-only changes do not require build/test execution unless they change an executable contract or validation input.

Do not repeat a broader gate when an unchanged valid result still covers the relevant inputs. If an external prerequisite is unavailable, report it rather than weakening the check.
