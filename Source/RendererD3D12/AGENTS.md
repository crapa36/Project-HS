# RendererD3D12 Agent Rules

RendererD3D12 is the concrete Windows/Direct3D 12 backend. Keep D3D12 and DXGI types out of GameDomain, GameRules, and Gameplay.

There is one renderer backend. Do not add `IRenderer`, a renderer factory, registry, plugin system, or generic backend layer without a real second backend.

Do not hide or suppress D3D12 validation, GPU validation, resource-state, resource-lifetime, or shader errors. Validation error counts must remain zero.

Actual renderer changes use the existing render smoke/integration validation when the D3D12 runtime and GPU environment are available.

Use the existing GPU-validation configuration when investigating synchronization, barriers, resource state, or resource lifetime. Shader hot-reload validation uses the Debug validation path.

Report unavailable prerequisites exactly; do not claim an unexecuted check.
