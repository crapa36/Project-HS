# Project HS development tools

## Build and verification

Use the pinned CMake in an x64 MSVC developer shell to configure a preset.
The build wrapper loads the configured MSVC toolset for an existing build tree.

```powershell
./tools/cmake.ps1 --preset msvc-debug
./tools/cmake.ps1 --build --preset msvc-debug
./tools/cmake.ps1 --build Build/msvc-debug --target hs_renderer_d3d12
./tools/verify.ps1 -Workflow verify -Test rendering.owned-view
```

The Debug and Release game presets disable test targets. The `verify` configure
preset enables them in the existing Debug tree. Use CMake target names for partial builds. To build a single configured output,
pass its build-relative path to the same native build command. CTest selects
fixtures and their build prerequisites. Its latest test output is in
`Testing/Temporary/LastTest.log` inside the configuration directory.

`ProjectHS` links a matched content pack. Only build `hs_content_generation`
when an offline tool or development experiment needs a loose selection.
Use `--artifacts` for an explicit diagnostic destination. The default for a
requested runtime capture is `bin/Artifacts/runtime` in the selected build tree.

## Optional tools

For C++ symbol navigation the LSP launcher uses the Debug compilation database,
falling back to the core configuration. To restore the local server, check out
`https://github.com/code-yeongyu/codex-lsp` at
`55836641be9b7dbf85772474a21b7a5a8ebeed35` under `Build/tooling/codex-lsp`,
initialize its submodules, then run `npm.cmd ci --ignore-scripts --no-audit --no-fund`
and `npm.cmd run build` in `packages/lsp-tools-mcp`.

For a muted Debug play recording run `tools/start-play-capture.ps1`. Its default
destination is `Artifacts/play-captures` in the selected build tree.
