# Project HS Agent Guide

Project HS is a C++23 Windows game using Direct3D 12.
It builds with CMake 3.30+, Ninja, and MSVC.

Read `ARCHITECTURE.md` for module responsibilities and dependency rules.

## Working rules

1. Trace the complete code path before editing it.
2. Make the smallest correct change.
3. Reuse existing helpers, targets, tests, types, and conventions.
4. Fix root causes at shared boundaries; do not suppress errors downstream.
5. Preserve public APIs unless the task explicitly changes them.
6. Preserve behavior, formats, CLI semantics, and test expectations during structural work.
7. Do not weaken validation, error handling, determinism, or architecture checks.
8. Do not add external dependencies unless explicitly required.
9. Do not perform unrelated cleanup.

## Dependency direction

```text
Core
├── Jobs
├── GameDomain
│   ├── GameRules
│   │   └── Gameplay
│   └── Presentation
└── RendererD3D12

Runtime composes concrete subsystems.
Apps are entry points and composition targets.
```

- Core must not depend on higher Project HS modules.
- Jobs may depend on Core.
- GameDomain may depend on Core.
- GameRules may depend on Core and GameDomain.
- Gameplay may depend on Core, GameDomain, and GameRules.
- Presentation may depend on Core and GameDomain.
- RendererD3D12 may depend on Core and native D3D12 dependencies.
- Runtime may compose Core, Jobs, Gameplay, Presentation, and RendererD3D12.
- Production modules must not include another module's `Private` headers.

## Deterministic Gameplay

Gameplay is deterministic fixed-step simulation code.

Do not introduce into Gameplay:

- D3D12 or DXGI knowledge;
- windowing, audio, presentation, or renderer dependencies;
- runtime/filesystem orchestration;
- wall-clock-driven simulation decisions;
- nondeterministic iteration that affects state or checksums.

Preserve `/fp:strict`, phase ordering, seeds, and checksum expectations.
An unexpected checksum change is a regression until its cause is proven.

See `Source/Gameplay/AGENTS.md` for local rules.

## Renderer

RendererD3D12 is the concrete Windows/D3D12 backend.
Do not add an interface, factory, registry, or generic backend layer without a real second backend.
Do not leak D3D12 types into GameDomain, GameRules, or Gameplay.

See `Source/RendererD3D12/AGENTS.md` for local rules.

## Verification

Fast low-level verification:

```powershell
cmake --workflow --preset verify-core
```

Full integration verification:

```powershell
cmake --workflow --preset verify
```

The core workflow must not require the D3D12 runtime, DXC, font download,
DirectXTex, or FBX SDK. Full verification requires the provisioned MSVC/Ninja,
DXC, FBX SDK, D3D12 runtime, content, and GPU environment used by integration tests.

If a prerequisite is unavailable, report it exactly. Never disable a check.

## Ponytail rule

Before writing code, stop at the first option that works:

1. Do we need it?
2. Does Project HS already provide it?
3. Does the standard library provide it?
4. Does the native platform provide it?
5. Does an installed dependency provide it?
6. Can existing code be simplified instead?
7. Only then write the minimum new code.

Prefer deletion to addition, movement to rewriting, and mechanical checks to prose.
Do not create speculative interfaces, factories, registries, service containers,
plugin systems, dependency injection, ECS migrations, or generic backends.

For a deliberate simplification with a real ceiling, use:

```cpp
// ponytail: <known ceiling>; upgrade when <specific trigger>
```

Do not add `ponytail:` comments to ordinary implementation choices.
