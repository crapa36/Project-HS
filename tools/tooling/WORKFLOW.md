# Measured Project HS tooling

Use the smallest tool that answers the current question. Installations and raw
evidence are isolated under ignored `Build/tooling`; none is a game dependency.

## Desloppify: on-demand cleanup candidates

Local Python 3.13 environment: `Build/tooling/desloppify/.venv`. Installed
`desloppify[full]` 1.0 from upstream commit
`3a7735d531a96b6a226bfbdc9fd662b14195f857`; the official Codex skill is
`.agents/skills/desloppify/SKILL.md`. No global PATH, CI hook, automatic review
runner, or game dependency was added.

```powershell
& ./Build/tooling/desloppify/.venv/Scripts/python.exe -X utf8 -m desloppify --lang cxx scan --path . --profile ci --no-badge
& ./Build/tooling/desloppify/.venv/Scripts/python.exe -X utf8 -m desloppify --lang cxx status
& ./Build/tooling/desloppify/.venv/Scripts/python.exe -X utf8 -m desloppify --lang cxx next
```

Run from the repository root. The initial scan used the existing validation
compile database copied to ignored `compile_commands.json`; refresh that copy
after relevant CMake changes. Local `.desloppify` state excludes build, external,
Git and agent metadata. Explicit test-zone overrides correct upstream's failure
to recognize `Tests/` and `_tests.cpp`; classify newly added tests with `zone set`.
The first scan covered 530 production and 89 test C/C++ files. `ci` skips duplicate
and subjective review phases; its score is not engine qualification.

The installed C++ extractor needed one local fix: remove bare `:` from its return
type separator character class (`[\s*&:]+` -> `[\s*&]+`). Qualified `::` tokens
remain supported. Otherwise `material_ir.cpp` case labels caused catastrophic
regex backtracking. Existing extractor/compile-database/dependency tests: 8 passed.
Full Source extraction completed in 1.74 s (529 files, 4,566 functions); the
original timed out, so whole-project old/new extraction equivalence is unproven.
Patch: `Build/tooling/desloppify/local-cxx-extractor.patch`. Reinstallation removes
this local fix. The AST grammar was downloaded into the host user's
tree-sitter-language-pack cache; a sandbox with a different AppData cache needs
its own accessible grammar.

Initial scan/status/next completed. Treat findings as candidates: CMake builds
translation units independently, so zero include importers does not mean dead
code. The numeric `/proc/self/statm` fscanf warning is a confirmed regex false
positive. The test log's "7546 production files" is a weighted potential, not a
file count or measured coverage. clang-tidy/cppcheck are unavailable; security
uses reduced regex coverage. No subjective score was supplied in that initial
scan. The later full scan and twenty independent blind dimension reviews are in
`Build/health-review-20261029/RESULTS.ko.md`; scores are analysis snapshots, not
performance/stability evidence. No score-driven tests/deletions were made.
The review module needed the same colon-regex correction as the extractor; its
original is preserved in the local tool folder and declaration/case-label checks
pass. Use `python -X utf8` to avoid Windows cp949 schema-read failures. Reinstalling
the tool removes the local fixes. Initial raw score table:
`Build/tooling/desloppify/RESULTS.md`.
Upstream: https://github.com/peteromallet/desloppify

## C++ navigation: adopted

`hs_lsp` is registered in repo-local `.codex/config.toml` with an explicit project
working directory and repository-relative launcher path. The launcher itself also
derives the repository root from `import.meta.url`, so it does not depend on a
user-specific checkout path.
discovers Visual Studio clangd and prefers `Build/msvc-validation/compile_commands.json`
for Runtime/renderer diagnostics, falling back to `Build/msvc-core/compile_commands.json`
for a core-only checkout. Run `Tools/verify.ps1` first if neither database exists.
A newly started Codex session loads the registration for the active worktree.

- Use `goto_definition` for symbol identity, `symbols` for a bounded outline,
  and `diagnostics` for an edited file. Use `rg` for literals and file discovery.
- MCP positions: line is 1-based; character is 0-based. Inspect rename scope before
  applying edits. No post-edit blocking hook is enabled.
- Core compilation data does not cover Runtime/renderer targets. For those tasks,
  create the validation database before trusting semantic diagnostics.
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

## Removed experiment harnesses

Repowise, Usage Tracker, and the LSP/sccache measurement harnesses are no longer
maintained repository entry points. No production build or test used them.
Use `rg` and the adopted LSP launcher for navigation. Previous qualification
artifacts under ignored `Build/tooling` are historical local evidence, not
prerequisites or proof of current performance.

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

## RenderDoc / rdc-cli: startup qualified, game capture blocked, not adopted

RenderDoc 1.46 portable CLI help and qrenderdoc's pre-UI `--python` startup
succeeded. Its built-in RenderDoc module uses Python 3.8.20; import, version,
capture-control and descriptor/buffer query API availability checks passed.
This path needs no separately built `renderdoc.pyd` or standalone Python wrapper.
rdc-cli 0.6.3 remains isolated and is not the qualified replay entry point.

The startup launcher temporarily sets `Analytics_TotalOptOut=true` to bypass the
first-run dialog, then restores the original UI.config bytes in a finally block.
Restoration was verified. Tools, scripts and artifacts stay in
`Build/tooling/renderdoc`; no global installation or Vulkan registration is
required. Capture and replay are separate, explicitly requested operations.

Actual Project-HS capture did not succeed. Injection returned success, but the
game exited with code 1 before the requested frame. Suspended launch with
redirected stdout/stderr exposed `Create DirectWrite UI surface failed:
0x80004002` (E_NOINTERFACE) during software Direct2D/WIC UI surface creation.
That shared error label covers bitmap, render-target and brush creation, so the
individual failing call is not established. No `.rdc` or actual GPU event/resource
query was obtained. API availability is not replay qualification; do not replace
existing GPU-validation/render-smoke gates. Do not upload raw captures.

Evidence: `Build/tooling/renderdoc/headless-qualification.json`,
`headless-process.json`, `capture-result-3.json` and `capture-result-3.target.log`.
Portable package SHA-256:
`9CA4D09ECABA2CC791168660D6FC2A7E3D70FE67146E87EC05C5F8DEA70772F7`.
Upstreams: https://renderdoc.org/builds and
https://github.com/baldurk/renderdoc/blob/v1.46/docs/python_api/faq.rst

## Build and experiment loop

For a cohesive implementation batch, name the exact native CMake targets and/or
Action IDs before invoking one partial build. Preview that same list first:

```powershell
Tools/hs_dev.ps1 -Command build -Target 'hs_renderer_d3d12,hs_render_test_runner,render.shader.runtime.geometry' -DryRun
Tools/hs_dev.ps1 -Command build -Target 'hs_renderer_d3d12,hs_render_test_runner,render.shader.runtime.geometry'
```

`build` requires a nonempty comma-separated `-Target` list and a configured Ninja
build tree. `-BuildRoot` selects another configured tree; the default is
`Build/msvc-validation`. `-DryRun -Json` returns the same plan as structured data.
Dry runs query metadata without building or bootstrapping CMake. The plan lists
every requested name, its resolved target, and the CMake argument array; execution
passes that array once to the pinned `Tools/cmake.ps1 --build ... --target ...`
wrapper. Use an MSVC developer environment for native Windows builds.

Native names must be configured CMake target aliases. Known Action IDs resolve
through their ActionResult namespace to `Actions/<namespace>/result.json`, then
the configured Ninja output graph validates the result. This works before a
Generation is composed and does not load its pointer. If an ActionResult does
not exist yet, use its exact native producer target for the first build; no
namespace is guessed from an ID. Duplicate resolved targets are built once.
Empty names, CLI flags, raw paths, and broad administrative targets such as
`all` or `clean` are rejected. There is no working-tree inference or default
all-target build. A partial build is not a verification receipt.

Named Debug `ProjectHS`/`hs_experiment` builds reuse the admitted generation rather
than recooking all content. After content edits, build `hs_content_generation`
before running. Default ALL builds still prepare content; Release keeps the
compiled shipping-image dependency.

`hs_dev` now exposes only `build` and `verify`. For exact tests, use
`Tools/hs_dev.ps1 verify -Workflow verify-core -Test simulation.determinism-core`
or call `Tools/verify.ps1` directly. Verification retains CTest preparation
dependencies, fixture selection, timeouts, and a small JSON result with selected
tests, exit status, and log path. CMake configures the selected preset with tests
enabled, then CTest supplies the selection including fixture setup/cleanup.
Native output and phase changes are streamed immediately and saved to the same
log. With `-Json`, progress goes to stderr and stdout remains one JSON result.
`-BuildRoot` applies only to `build`; verification selects its tree by workflow.
The duplicate `doctor`, `status`, `why`, `deps`, `rdeps`, `path`, `affected`, and
`gc` facade commands were removed. Use configured Ninja `-t query` for native
build dependencies and explicit `-Test` or `-Label` for test selection.
Ninja queries do not reconstruct the logical product graph. Generation
verification and explicit cleanup remain in `hs_generation`; its reader lease,
pin, and product validation contracts are unchanged.
Changed-file inference, verification receipts, and automatic corrupt-product
repair were removed. Verification reports producer/test failures directly;
repair a damaged local generation explicitly before retrying.

Default: rg discovery -> persistent hs_lsp symbol lookup -> smallest edit ->
targeted MSVC build/test -> required full workflow after stabilization.
No usage collection, cache server, or GPU capture runs automatically per edit.

Use `Tools/verify.ps1 -Workflow verify-core` or `-Workflow verify` as appropriate.
During iteration use `hs_dev build -Target` for explicit compilation and
`verify.ps1 -Test` for exact tests and their prerequisites.
Completed evidence remains reusable only while its inputs remain valid.
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

## Native stall stacks: on demand

The Microsoft-signed CDB is local to
`Build/tooling/dbgeng/windbg-amd64/cdb.exe`; no global installation is required.
For an owned probe process, use the matching executable/PDB directory and detach
after a short noninvasive stack snapshot:

```powershell
$taskSymbols = (Resolve-Path Build/msvc-debug).Path
& Build/tooling/dbgeng/windbg-amd64/cdb.exe -pv -sins -p $taskProcess.Id -y $taskSymbols -c '~* k 64; lm; .detach; q'
```

Verify that the process ID still belongs to the probe before attaching. Do not
relink its executable/PDB while sampling. `-pv` briefly suspends the target;
these snapshots are diagnosis, not FPS measurements. Keep symbol paths local.
Check for actual thread stacks and private engine symbols: exit code zero alone
did not prove a successful capture. This host rejects `-pd`; explicit `.detach`
works. Missing optional debugger extensions did not prevent stack capture.

The corrected owned-view GBV snapshots at20/40s put main in the recording-task
join and workers inside draw recording through SDK layers/NVIDIA modules.
An offline DbgHelp lookup of the retained return addresses against that matching
EXE/PDB resolves both to `SetPipelineState`, without another engine run. This is
consistent with GBV first-use patch compilation, not proof of a GPU-fence deadlock
or the separate teardown stall. Evidence:
`Build/gbv-stack-corrected-20261029/`; tool provenance and owned-helper attach
qualification: `Build/tooling/dbgeng/`.
