# Measured Project HS tooling

Use the smallest tool that answers the current question. Installations and raw
evidence are isolated under ignored `Build/tooling`; none is a game dependency.

## C++ navigation: adopted

`hs_lsp` is registered in repo-local `.codex/config.toml` and starts
`node Tools/tooling/lsp-server.mjs`. The launcher discovers Visual Studio clangd
and uses `Build/msvc-core/compile_commands.json`. Run `Tools/verify.ps1` first if
that database is absent. A newly started Codex session loads this registration.

- Use `goto_definition` for symbol identity, `symbols` for a bounded outline,
  and `diagnostics` for an edited file. Use `rg` for literals and file discovery.
- MCP positions: line is 1-based; character is 0-based. Inspect rename scope before
  applying edits. No post-edit blocking hook is enabled.
- Core compilation data does not cover Runtime/renderer targets. For those tasks,
  use their actual compile database before trusting semantic diagnostics.
- Background indexing is enabled in the persistent server; wait for indexing
  before interpreting an empty cross-file reference result as absence.

Qualification: codex-lsp 0.2.0 at
`55836641be9b7dbf85772474a21b7a5a8ebeed35`, lsp-tools-mcp submodule
`e7c65b04d0cc549f0478d3b78b51714fc0f572b3`, installed clangd 19.1.5.
MCP initialize and actual goto-definition both succeeded. The result correctly
identified `SimulationWorld` at `simulation_world.hpp:523`. Measured fresh-server
lookup was 2.29-4.50 seconds; repeated lookup 1.6-1.8 ms; file symbols 4-5 ms.
An isolated undefined-name probe produced the expected compiler diagnostic.
These are navigation measurements, not a measured reduction in model tokens.
Evidence: `Build/tooling/lsp-measurements.json` and `.log`.

Restore installation using the pinned upstream checkout and submodule, then run
`npm.cmd ci --ignore-scripts --no-audit --no-fund` and `npm.cmd run build` inside
`Build/tooling/codex-lsp/packages/lsp-tools-mcp`. Do not install unrelated servers.
Upstream: https://github.com/code-yeongyu/codex-lsp

## Compilation cache: excluded from production workflow

sccache 0.17.0 Windows x64 official ZIP SHA-256:
`e94cfc5b58cbe439302f586c1d1bd7980c2cd371d47bdf385ade657411e6f3ac`.
Three direct `simulation_combat.cpp` compilations had median 2290.93 ms; three
warm cache calls had median 221.26 ms (90.3% less elapsed time). Cold/warm object
hashes matched. Statistics showed one miss and five hits: the second/third
nominal cold samples were already cache hits because unused macro changes did
not alter the preprocessed source. Do not call them three cold misses.

Actual isolated CMake/Ninja integration with `/Z7` passed six behavioral tests
but FAILED header dependency verification (`deps=0`). Include output encoding
did not match Ninja's prefix. The experimental preset was removed, its server
stopped, and the ordinary build's dependency check passed afterward.
Speed cannot justify restoring stale-object ABI risk. Do not enable sccache in
normal presets until a future version passes cold/hit dependency and header-touch
invalidation checks. This result applies to this tested Windows toolchain.

Evidence: `Build/tooling/sccache/measurements.json`, `stats.txt`, and
`Build/verification/verify-core-cached-20260905T084718305Z-12584.log`.
The isolated failed build tree is retained only as evidence.
Upstream: https://github.com/mozilla/sccache

## Repowise: optional structural lookup, not the semantic oracle

Qualified 0.48.0, commit `eb4010c34caf7379b1de608f1b5acb0b5e19e8b4`.
Offline mock-provider/mock-embedder indexing covered 140 files and 1,817 symbols;
seven oversized files were skipped. No model or embedding service was used.
Exact `EffectiveAttack` lookup returned a verified live 10-line body in 1.78 s;
the checked-in wrapper also succeeded. Broad `SimulationWorld` context took
2.37 s but selected a constructor rather than the class and included guessed
graph edges. Use clangd for exact C++ identity. These timings do not establish
an improvement over rg or a reduction in tokens.

Use only when a structural overview answers a concrete question:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/tooling/repowise.ps1 -Action symbol -Query 'Source/Gameplay/Private/simulation_combat.cpp::GameSimulation::SimulationWorld::EffectiveAttack'
```

The wrapper disables telemetry and workspace auto-discovery. No always-on MCP
or editor hook is registered. The isolated installation resolved 132 packages,
so do not make it a prerequisite for normal navigation or builds. Index freshness
must be checked against live files, including uncommitted changes, not HEAD alone.
For index initialization use the isolated CLI with `--provider=mock
--embedder=mock --no-prose --mode=fast --no-onboarding --no-editor-setup
--no-save-key --no-agents --no-claude-md --no-codex --no-distill-hook
--no-workspace --exclude=ContentSource/** --exclude=Content/**
--exclude=Build/** --exclude=Artifacts/** --yes --progress=json`.
Use `--exclude=PATTERN`, not separate wildcard arguments on Windows.

Evidence: `Build/tooling/repowise/index2.log`, `query.json`, `symbol.json`.
Upstream: https://github.com/repowise-dev/repowise

## AddressSanitizer: on-demand core investigations

From an MSVC x64 environment, use the isolated preset only for a concrete core
memory-safety investigation:

```powershell
cmake --preset msvc-asan-core
cmake --build --preset msvc-asan-core
ctest --preset msvc-asan-core
```

On this MSVC installation, import `VsDevCmd.bat` with x64 arguments before
running these commands and ensure the Windows SDK x64 `bin` directory is on
`PATH` so `rc.exe` and `mt.exe` are available. The ASan runtime DLL is supplied
by the MSVC toolchain and its directory must also be on `PATH` when running
tests. If configure has a
cached failed Taskflow atomic probe, rerun configure once with
`-U TF_ATOMIC_BUILTIN -U TF_ATOMIC_WITH_LATOMIC` and the pinned local source
override `-DFETCHCONTENT_SOURCE_DIR_TASKFLOW=Build/msvc-core/_deps/taskflow-src`.

The preset builds only `hs_simulation_tests` and runs only
`simulation.determinism-core`. It keeps Runtime and content tools disabled,
uses `/fsanitize=address` with `/INCREMENTAL:NO`, and writes to
`Build/tooling/msvc-asan-core`. It is not part of either default verification
workflow; use it as the narrow validation for a concrete core memory-safety
investigation.

## Usage Tracker: not adopted

Both 0.28.0 (GitHub tag) and 0.27.0 (PyPI) failed initial database creation on
Windows with WinError 32. `kernel/operational.py` opens a staging SQLite connection
in a context manager, then calls `os.replace` without closing that connection.
The context manager commits but does not close it; Windows retains the file lock.
This is not evidence of successfully collected usage or measured token savings.
Keep the bounded single-session `usage-query.py` harness for future qualification;
do not scan/upload all conversations or start its dashboard by default.
Current isolated venv is 0.27.0. Evidence:
`Build/tooling/usage/query-027-error.log`.
Upstream: https://github.com/douglasmonsky/codex-usage-tracker

## RenderDoc / rdc-cli: qualification incomplete, not adopted

RenderDoc 1.44 portable CLI help succeeded; rdc-cli 0.6.3 installed in an isolated
Python 3.13 environment. `rdc doctor` could not import the RenderDoc replay module:
the portable package did not provide matching `renderdoc.pyd` bindings. Its
renderdoccmd discovery also failed in that probe. A bounded qrenderdoc Python
startup probe timed out. Task-owned GUI processes were stopped afterward.
No `.rdc` capture or replay analysis succeeded, so no GPU diagnosis benefit has
been demonstrated. Do not replace existing GPU-validation/render-smoke gates.
Future qualification requires matching Python bindings, successful doctor replay
checks, an actual game capture, and a verified event/resource query. Vulkan
registration is not needed to qualify this D3D12 game.

Evidence: `Build/tooling/renderdoc/doctor.log`; portable package SHA-256:
`67a6cfabd5041c2d646b66b6dfE90f94272457c5410084b71f652f9f9cff8f96`.
Upstreams: https://renderdoc.org/builds and https://github.com/BANANASJIM/rdc-cli

## Build and experiment loop

Default: rg discovery -> persistent hs_lsp symbol lookup -> smallest edit ->
targeted MSVC build/test -> required full workflow after stabilization.
Use Repowise only for a bounded structural question not already answered by LSP.
No usage collection, cache server, or GPU capture runs automatically per edit.

Use `Tools/verify.ps1 -Workflow verify-core` or `-Workflow verify` as appropriate.
During iteration use paired `-Target` / `-Test` to build and test only the affected
area. Completed evidence remains reusable only while its inputs remain valid.
Before a balance simulation, use `hs_combat_sim --relic-balance-suite=FILE
--validate-only` and inspect its planned size. Do not rerun completed matrices.

## Build Insights: on demand

Use the installed `vcperf.exe` for a concrete MSVC compile-time investigation.
The qualified binary is VS toolset `14.44.35207/bin/Hostx64/x64/vcperf.exe`
(version 2.4.24101401); it was not on the ordinary PATH. In an MSVC x64
shell, bracket the relevant build with `/start /noadmin /nocpusampling /level3
<unique-session>` and `/stopnoanalyze <unique-session> <new-output.etl>`, then
use `/analyze <new-output.etl> /timetrace <new-output.json>`. Always stop the
session in a finally block and retain the original compile options.

The qualified Project-HS combat TU capture had 316 events and no dropped events;
2216.5 ms of 2314 ms compiler time was front-end work (95.8%). The JSON exposed
individual include costs without WPA interaction. This points to the combat/world
compilation seam; it does not prove an optimization or a whole-build speedup.
Use it to choose and verify a specific compile-cost change, not on every edit.
Evidence and exact invocation: `Build/tooling/qualification-20260906/debug/`
(`vcperf_capture.ps1`, `a9-analysis.json`, capture receipt and ETL/time trace).

## Microsoft Learn CLI: on demand

When ordinary web retrieval cannot provide a Microsoft API reference, use
`npx --yes --cache Build/tooling/npm-cache @microsoft/learn-cli@0.1.0 search "API question"`.
Use `fetch <official-page-url>` for a specific page. Send public API questions,
and check the exact API variant against the project's call site. No always-on
MCP registration is needed.

Four concrete DXGI/D3D12/XAudio2 questions were answered through the CLI when
ordinary direct web retrieval failed for all four pages. This establishes a
working alternative retrieval path, not a general speed advantage. Evidence:
`Build/tooling/qualification-20260906/docs-cli/comparison.md`.
