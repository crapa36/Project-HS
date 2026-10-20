# Current work: whole-runtime performance and correctness review

Target: native 1920x1080, highest including RT, frame generation allowed, sustained 144 FPS. NOT achieved.

- Preserve dirty worktree and original profile/captures; leave uncommitted.
- No Computer Use after physical Escape cancellation without fresh authorization. Session is reset.
- Existing agents completed; no new delegation.
- Capture05: 2.434 base FPS / 5.100 SDK present FPS, but simulation advanced only 4 ticks/render. Not valid matched-load improvement evidence.
- Capture06: zero valid frames, interrupted; excluded with sidecar.
- Fixed native handoff catch-up and rapid disable/enable deadlock; native-input, graphics settings, architecture 3/3 passed.
- Fixed snapshot export temporary path overflow; 248-character destination fixture passed; Debug generation EE exported 3680 products.
- Release and Debug current source built with Streamline ON. Native selected integration5/5 passed; separate Debug8-frame smoke passed,0validation errors.
- Broad audit recorded in Docs/Verification/20261015-human-debug-play/REPORT.md. Remaining: graph key churn, collision repeated hit-time computation, hit bookkeeping, rules input bounds, audio first-use and streaming failure branches.
- Next performance work: measure graph cache reuse/key changes before changing cache; prioritize ~70ms prepare and RT-scene CPU cost. DOF ~4.566ms is the heaviest non-RT post effect in latest measured config.
- No quality reduction or unverified 144 FPS claim. Post-fix foreground performance remains unverified.

## Minimal-test follow-up
- Applied DOF bounded dilation/zero-radius gather, collision sort-time cache, cast/area indexes, live-area binary search, alias reachability memoization and resource-user dedup.
- CPU combat/determinism/graph passed. GPU fixture boundary emulation corrected; WARP+RTX Legacy/Enhanced production DOF output comparisons passed.
- Alias microbenchmark512/128:22.9192->0.5861ms, identical hash; no gameplay FPS claim.
- New Debug75A0/ReleaseB24F built; cooked generation62a68d2b exported/verified3680 products to both builds.
- No Computer Use and no long full gate rerun. Next: exact graph reuse/key evidence and dominant RT scene/trace work. Highest144FPS remains unverified.

## RT registration follow-up
- Cached native-buffer facts within Register only; per-geometry range/version checks remain.
- Hash membership for stable slots/used BLAS/imports/retirement; actual caches and output order retained.
- Removed duplicate native-byte summation and redundant live-dynamic tree; normalize current frame-slot key for membership.
- ray-entity-ultra alone passed8frames with GPU validation/empty reset (093249 receipt,0errors); Release528D/DebugA260 rebuilt.
- No shader change; generation62a68d2b remains. No current gameplay FPS claim.
- Next substantive GPU work: investigate cached world positions for repeated ray-hit grass deformation, preserving exact producer/BLAS/shader ABI and checks. Still not implemented.


## Ray position cache follow-up
- Applied exact evaluated-world cache per deformed job; local BLAS stride12 unchanged, extra12B/vertex.
- Producer uncached; consumers skip repeated BendGrass only, preserve other vertex/material evaluation.
- Added AO/shadow/transport graph reads and input validation, AO lifetime retention.
- Minimal checks: multi-queue GPU oracle43cases Legacy/Enhanced passed8.76208s (094535); native RT ultra8frames passed93.716s (094632), GPU validation ON0errors.
- ReleaseA7BAD4 /DebugD6D95C built; runtime generation20b19e5b exported to both builds,3680 products each.
- No Computer Use restart or game/play measurement.144FPS remains unverified; no measured speedup claim for this cache.
- Next: inspect graph key churn/reuse, dominant BLAS/trace workload and remaining input-boundary audit findings; do not weaken quality/validation.


## Graph compiler follow-up
Graph Compile coalesces full texture mip/array/plane ranges; partial holes remain.
Reader hazards use nearest overlapping previous/next writer on the physical-write chain.
1000 random cases test three-queue reachability for all original RAW/WAR and writer edges.
CPU range/native multi-queue checks passed (095640 receipt,0.28123s/7.85787s).
Single synthetic comparison: full texture58.6852->1.2638ms; partial28.9608->30.9043ms
(no improvement established); buffer version chain1.4807->0.9842ms. All3 execution
order/queue reachability hashes match. Evidence graph-range-* under Build/performance-actual.
Both builds updated; generation20b19e5b unchanged/verified3680 products. No gameplay FPS claim.
Next: graph key churn/reuse evidence and dominant native workload; Computer Use stays off and144FPS remains unverified.


## Buffer barrier recording follow-up
Graph recording now batches consecutive Enhanced buffer barriers in64-entry stack
storage. Flush before texture/global/alias operations, callbacks and final recording;
retain exact sync/access/range/order, Legacy and explicit-callback paths unchanged.
Native fixture adds137buffers/3queues/8768words and64-entry flush/tail coverage.
Multi-queue and ray-entity-ultra2/2 passed (100448 receipt); actual RT8frames0validation
errors. Release/Debug rebuilt; runtime20b19e5b unchanged/verified3680 products.
Old capture05 config6 CPU record BLAS67.27948ms/TLAS32.31344ms includes pre/post
barriers/reset/close; no attribution of the entire time to barriers or new FPS claim.
Next: separate AS callback/native call cost from barrier cost; graph reuse evidence remains pending. Computer Use off,144FPS unverified.


## Short diagnostic and immutable graph follow-up

Latest hidden actual-arena diagnostic: rt-post-optimizations-16ticks,16frames,exit0,
CPU/GPU16/16 complete, EDEB8314 Release before read-only compiler optimization.
Fixed-tick lockstep, no foreground/input, isolated FG/lowlatency off; not144FPS evidence.
Tail11..16: GPUcritical180.959ms; reflection61.322/GI51.668/BLAS22.419/AO11.746ms.
CPUrender474.453/build144.406(ray_scene97.519)/prepare71.390(compile38.648)/record48.983ms.
PDH15errors; tailGPUonly1sample1.290/2.854s, CPU2samples; no stable utilization claim.
TAIL-REPORT.md/tail-analysis.json and original56-option attribution preserve provenance.
Graph Compile now skips dependency-only allocations for initialized writer-free version0;
all access/queue/range/duplicate/final validations retained, scratch ranges reused.
ReadOnlyImports regression and1000random replay remain; CPU/native multi-queue2/2 passed
(102004978 receipt,10.46s). No broad gate or long play repeated.
Single component benchmark22057buffers4passes14.8184->1.7527ms; all4schedule/reach hashes match.
ReleaseFE8F8302C0E80F419DEC19E036CB1FEE46202CE24304033824BE7190C738B4D5;
Debug282ABAE0B7A9C9B319F9D84AFC3CBB693A710761D90B0542A045B0216A877359.
Both rebuilt; runtime20b19e5b verified3680 unchanged. No post-change foreground FPS claim.
Next: dominant reflection/GI workload and CPU RT scene registration. CU remains off;144FPS unachieved.


## Measured AS policy selection
Latest AS policy: TLAS PREFER_FAST_TRACE; dynamic BLAS remains FAST_BUILD|ALLOW_UPDATE.
Both-fast-trace experiment rejected: tail GPU186.139ms vs baseline180.959ms,
BLAS update35.268 vs22.419ms. TLAS-only tailGPU164.299ms, reflection51.501/GI48.078/
BLAS21.585/AO10.388/TLAS1.061ms. Same16ticks/settings/checksum; hidden fixed-lockstep,
FG/lowlatency off in isolated profiles. Baseline precedes read-only graph change.
CPUrender599.882ms vs474.453 baseline; no whole-frame/foreground FPS gain claim.
Evidence as-policy-comparison.json/AS-POLICY-REPORT.md and three rt-*-16ticks directories.
Finalframe16 six depth/GBuffer/HDR resources byte-identical; as-policy-output-parity.json.
TLAS-only PDH child stopped by sampler owner while refreshing; PID GPU coverage partial,
missing intervals N/A. No sampler/game/test process remains. Computer Use stays off.
Native ray-entity-ultra8frames passed (103026485 receipt,171.771s,0validation errors,
empty reset6); no full gate. Release228FB0B6AEE25D3A181429F3AEFFDA2ED4B94C71B31A877574B00B05944C7DB5;
Debug705C052FB8E07E10C6E0791B430529DCA9A5BEE2800AE25381B7A79230ECEAF5.
Both built, runtime20b19e5b3680products verified unchanged. 144FPS remains unachieved.
Next: reduce per-instance dynamic BLAS/resource/registration overhead, then ray candidate
material cost; preserve full geometry, alpha/sidedness and simulation invariants.


## CPU ray registration ownership

CPU RT registration optimized: pending dynamic index is sorted contiguous storage;
one shared BuildJob payload survives through graph recording/completion; build-input
collector no longer copies RaySceneGeometry; FlightLease retains each native input once.
Preserve persistent map, exact generation/frame-slot retire logic, graph uses and budgets.
Two one-run hidden16frame profiles (external utilization sampler off), tail11..16:
instances22058, dynamic/build jobs42191, imports84400 unchanged; register102.533->62.091ms,
select35.273->16.931, budget/reads22.122->5.888, pass construction21.166->14.530ms.
CPUrender481.776->444.347ms; GPU178.748->182.726ms (no GPU improvement claim).
Six finalframe16 depth/GBuffer/HDR resources byte-identical, settings/checksum equal.
Evidence ray-scene-ownership-comparison.json/RAY-SCENE-OWNERSHIP-REPORT.md and
rt-scene-cpu-{profile,optimized}-16ticks. Temporary profiler removed from final sources.
Native ray-entity-ultra and multi-queue2/2 passed (104159978 receipt); RT8frames emptyreset6,
0validation errors. Release2C36CC9588B1B89CBEED0D3203D0492B3B9E326D5DBE4920327458936704AB51;
Debug1B7EAB6F4342824A129B2E523560776FB5B5746E1C23C380AB29E6BDBF4FC29F.
Both built, runtime20b19e5b verified3680 unchanged. No foreground144FPS proof.
Next: reduce 84400 graph resource registrations/42191 BLAS jobs structurally (shared
native storage must preserve distinct scratch ranges, byte budgets and all GPU leases),
and ray candidate material work. CU remains off, target unachieved.


## Dynamic BLAS scratch pooling
Dynamic BLAS scratch is now one SceneFrame allocation with disjoint256-byte aligned
ranges per build/update; static scratch unchanged. Frame completion, callback strong
ownership, bounds validation and old+new replacement budget remain. High-water capacity
is retained per frame and counted in native allocation budget. Result AS resources still
remain per BLAS. Source-derived imports for baseline42191jobs:84400->42210 (not a newly
exported runtime counter). No memory-saving claim from acceleration_structure_bytes.
One hidden16frame after run reused ownership baseline; tail11..16 CPUrender444.347->329.472ms,
graph_prepare70.665->38.555, register62.091->52.670; GPUcritical182.726->140.755ms.
Reflection44.127/GI41.214/dynamicBLAS16.263/AO9.174ms remain dominant. Same56settings,
checksum and six finalframe16 output bytes. Baseline temporary CPU profiler differs;
single-run clock/scheduling variability not isolated. External utilization sampler off,
CPU/GPU utilization N/A, FG/lowlatency off. Not foreground or144FPS evidence.
Evidence scratch-pool-comparison.json/SCRATCH-POOL-REPORT.md and rt-scratch-pool-16ticks.
Native ray-entity-ultra125.167s and multi-queue7.89552s passed (104817174 receipt);
RT8frames emptyreset6,0GPUvalidation errors. Scratch layout regression covers alignment,
disjointness, zero/overflow/misaligned rejection and unchanged outputs on failure.
Release72A539F47A477152CBA866490A02364F9B1B9E8D70562DFC4DCDDCC7BB41B5C1;
Debug35551AD232EB3BCC54B4EAE1DCF33D49EC329CB7D132C098D6F3EBB684515C77.
Both built, runtime20b19e5b verified3680 unchanged. No foreground144FPS proof.
Next: evaluate result-AS storage pooling with correct address offsets/completion/budgets,
and dominant ray candidate material cost. CU remains off, target unachieved.


## Shared BLAS and Ponytail redundancy review
Dynamic BLAS scratch uses one frame-local bank of disjoint256-byte aligned ranges.
Ordinary and OMM TLAS now share one BLAS per geometry/instance: OMM BLAS prebuild/build
sets ALLOW_DISABLE_OMMS; ordinary instance sets DISABLE_OMMS only on linked geometry;
OMM view restores original flags. Variant loop, second BLAS pointer array and duplicate
TLAS BLAS-read loop removed. Same cache/compaction/update/lease/completion/budget checks.
No shader candidate/identity/coverage/sidedness logic changed.
One hidden16frame after run reused scratch-pool baseline; tail11..16 CPUrender329.472->242.056ms,
ray_scene_register52.670->26.022, graph_prepare38.555->20.587, record35.051->18.845;
GPUcritical140.755->131.055, dynamicBLAS16.263->6.162ms. Reflection45.188/GI40.641/AO9.359ms
remain dominant. Same56settings/checksum and six finalframe16 output bytes. Both no
profiler/sampler, utilization N/A, FG/lowlatency off; not foreground144FPS evidence.
Evidence shared-omm-comparison.json/SHARED-OMM-REPORT.md/compare-shared-omm.py and
rt-shared-omm-16ticks. Native ray-entity-ultra now adds one dynamic grass instance;
Ultra foliage keeps it admitted.8frames/reset6 passed76.8607s (110843820 receipt),0GPUerrors.
OMM88triangles applied4/5/7/8. Two earlier fixture assertions failed because default
foliage dropped the grass; receipts110206873/110520811 retained. No full gate repeated.
Native fixture coverage has0known cells: proves shared linkage/update/fallback, not a
known-transparent classification oracle. Actual arena output parity scope is six buffers.
ReleaseCD40CD199784F16D5D5DFEB06ED03CFCDF5FA28562D50269A52A3DA478C5D21C;
Debug57552FC92E9B19B4452282C25AACFCEFBC73543663EBE6A1F06F85FF6E5E4628.
Both built, runtime20b19e5b verified3680 unchanged. No foreground144FPS proof.
Next: inspect actual package known-cell distribution and omit zero-known OMM setup if
semantics/status stay exact; evaluate result-AS storage pooling and dominant ray material
cost. FORCE_NON_OPAQUE cannot simply be removed: shader validates facing/unsupported
interfaces and identity. CU remains off, target unachieved.


## Unknown-only OMM pruning complete
Dynamic BLAS scratch uses one frame-local bank of disjoint256-byte aligned ranges.
Ordinary/OMM TLAS share one BLAS; ALLOW_DISABLE_OMMS at build and DISABLE_OMMS on
ordinary instances preserve both consumers. Duplicate variants/arrays/read loop removed.
Validated unknown-only OMM packages now retain metadata only: no native input/array/
scratch/upload or graph job. Known-cell packages keep the native path. Full package,
identity, actual opacity words, subdivision, budget and completion checks remain.
BuildRayOmmRequest borrows cached word spans through synchronous Register; no per-request
word vectors/copies. std::map keeps entries stable; revision/generation invalidation precedes
request construction. Immutable product digest validation rejects stale page replacements.
Inventory current runtime1266OMM packages,839zero-known; grass4/leaf3allzero-known.
These are package counts, not runtime instance counts. Evidence omm-coverage-inventory.json.
One hidden16frame after run rt-unknown-omm-skipped-16ticks reused shared-OMM baseline;
tail11..16 CPUrender242.056->230.702ms, register26.022->24.171, record18.845->16.898;
GPUcritical131.055->125.895ms. Shared groups reflections43.255/GI41.446/ray_scene11.009/
AO8.656/shadows7.863ms. Single-run variability; dynamicBLAS6.162->6.971 did not improve.
Same56settings/checksum and six finalframe16 output bytes. CPU/GPU16/16 complete,0invalid.
No profiler/sampler; utilization N/A, FG/lowlatency off; not foreground144FPS evidence.
Evidence unknown-omm-comparison.json/UNKNOWN-OMM-REPORT.md/compare-unknown-omm.py.
Native ray-entity-ultra8frames/reset6 passed85.6714s (verify-20261015T111541842Z-29864.json),
execution_valid=true,0GPUerrors. Final grass requires no applied/native-ready OMM,0linked
triangles/allocated bytes and explicit unknown-only reason. Result preserved as
Build/performance-actual/unknown-omm-native-result.json. Known-transparent native oracle
was not added/repeated; prior shared-linkage result remains in report history.
Release43E79E3DC9A6C889B1EB4FEE04FAFA7C2344FBAF701CF0580FF1C3C8325C1F88;
Debug24231F35B55BAF4A045E66181EB48D4E2572EED658101F78991CC1D5038C78F6.
Both built, runtime20b19e5b verified3680 unchanged. Native test generationc2b877a0:
local-probes Action republished; only render/lighting/local_probes.spkg changed among3297
products from prior test generation32c706. Performance comparisons use unchanged runtime.
Next technical work: dominant ray candidate/material cost and per-object BLAS result
resource overhead. Retain FORCE_NON_OPAQUE, identity/range checks, GPU retirement and
old/candidate/in-flight budgets. No new configuration/dependencies/abstractions.
144FPS goal remains unachieved; CU stays off. No broad gate/long play repeated.


## Immutable OMM request reuse complete
BuildRayOmmRequest now returns a pointer into the stable std::map cache, including its
word span. Cache hits avoid product-string construction/lookup and whole request copies.
Register memoizes validation per immutable request pointer, retaining distinct-request
identity/word validation, per-instance bindings/statistics and actual budget/retirement rules.
No public API, shader/content/quality change. Duplicate same-variant grass added to fixture.
Single16frame after capture rt-omm-request-reuse-16ticks reused unknown-only-skip baseline;
tail11..16 OMM15.005->9.566ms, graphbuild59.096->52.836. CPUrender230.702->265.508ms,
wait109.021->146.621, GPUcritical125.895->167.115: no whole-frame/FPS improvement claim.
Same56settings/checksum, six finalframe16 buffers byte-identical; identical654 measured
pass id/name/queue rows across compared6frames. GPU slowdown cause remains unproven.
Post-capture GPU P3/1020MHz/46C/28%/64.11W is not in-capture telemetry or cause proof.
CPU/GPU16/16 complete,0invalid; external sampler off/utilization N/A,FG/lowlatency off.
Shared groups reflections60.541/GI53.002/ray_scene14.539/AO12.510/shadows9.775ms.
Evidence omm-request-reuse-comparison.json/OMM-REQUEST-REUSE-REPORT.md and capture.
Native rendering.ray-entity-ultra145.986s PASS,8frames/reset6,0GPUerrors, execution_valid;
receipt verify-20261015T112633286Z-36028.json, native copy omm-request-reuse-native-result.json.
Test generationefdc4948; runtime20b19e5b unchanged/3680products verified in both builds.
Release76C328730D5A5190585D093E8178B9D0103CD9906A85948D7C23CF3A2BA6E70C;
Debug1D6C5465C0DA5C334DD61A8107D2435495E2FA84F13C1A55F634BF1ECE9CD4CC.
Both built; targeted diff --check passed. No broad gate/long play/known-transparent oracle.
Next: repeated ray_scene.AllocatedBytes scans within the same state, dominant ray candidate/
material work and per-object BLAS result resources. Keep all real admission/lifetime checks.
144FPS goal not achieved; CU remains off. No foreground speedup claimed.


## Single admission scan and lockstep measurement attribution
BuildRayOmmRequest returns stable-cache request pointers; per-registration memo retains
per-instance statistics and validates each distinct request. No copied word/request arrays.
Ray scene AdmitNativeByteHeadroom now calculates live backing once per admission (previously
caller2+setter1 scans), preserving pending/zero/overflow and actual backing/retirement checks.
No persistent accounting cache. Extracted production method MSVC harness passes10boundary
cases/empty/pending/one-scan/unchanged-on-failure; artifacts ray-budget-admission-check.*.
One16frame diagnostic rt-budget-single-scan-16ticks, tail11..16 vs request-reuse baseline:
OMM9.566->5.690ms, graphbuild52.836->51.475; CPUrender265.508->268.874,wait146.621->147.252,
GPU167.115->168.117. No whole-frame improvement. Baseline sampler off/after on adds confounder.
Same56settings/checksum/six finalframe16 raw outputs. CPU/GPU16/16 complete,0invalid.
Playing utilization6.395s:1.686cores/8.429%, deviceGPU26.571%/2721.43MHz/100W (7NVIDIA samples).
Tail only1CPU+2NVIDIA samples:1.506cores/7.529%,deviceGPU45%,2820MHz,139.89W.
PID GPU/logical CPU N/A:PDH init8.210s,invalid sample1,requery unfinished at target exit;
owned worker stopped, missing intervals remain unknown. Device counters include other apps.
Important: smoke lockstep passes wait_for_streaming=true -> WaitForGpu before frame-slot wait,
and simulation yields for render. Prior wait109/146ms had only0.0012/0.0010ms slot wait.
lockstep-wait-attribution.json records scopes; remaining wait includes global drain+setup.
Do not use lockstep throughput/utilization as ordinary foreground FPS/pacing evidence.
Next measurement: ordinary realtime recorded replay; keep deterministic smoke contract intact.
Existing golden-short header/content hash matches current smoke content hash; loader requires
simulation_rules hash/checksums. Reuse a bounded copy of inputs, preserve original. Normal
replay has visible=true and60Hz FixedStepClock; only smoke hides the application automatically.
GPU dominant groups reflections60.924/GI52.478/ray_scene15.473/AO12.585/shadows10.765ms.
Evidence budget-single-scan-comparison.json/BUDGET-SINGLE-SCAN-REPORT.md and capture56options.
Native ray-entity-ultra142.688s PASS,8frames/reset6,0GPUerrors,execution_valid; receipt
verify-20261015T113837787Z-8308.json; native copy ray-budget-single-scan-native-result.json.
Release1D0992E64902BBDCFA72840F7FBDAAEC025AA0ADCC7A47278DAD40D0DD87982F;
Debug7C938138BA5348C19E0D36F92BC436607B43C5B65AE950AAB38545B60803E0BD.
Both built/runtime20b19e5b3680products verified. Test generation7d2ff97f. All owned runs exited.
Next: normal runtime measurements; reduce short-run PDH startup/re-enumeration, then dominant
ray candidate/material and per-object BLAS result overhead. Keep GPU lifetime/budget checks.
144FPS remains unachieved; CU remains off.


## Realtime replay and pruning review
Normal realtime replay checked without smoke/lockstep: rt-realtime-native-playback-240ticks.
Old golden-short is incompatible (temporary invulnerability; checksum mismatch tick1), excluded.
Current /fp:strict headless preflight passes computer-ultra-05 first421records through tick240
and the human-debug prefix. Original inputs/checksums preserved. Native replay exits0 at240,
checksum12428462166565086596;18CPU/GPU frames complete,14Playing,all foreground=0.
Same-phase/config Playing4.932baseFPS,5.311SDK-presentFPS (11/2.071s),CPU250.002ms,
graphbuild102.834,record37.256,wait17.482;GPU169.003ms. Startup/background,not speedup proof.
GPU dominant passes reflection58.837/GI51.507/AO11.805/dynamicBLAS10.196/GBuffer5.827ms.
All56options per revision retained in capture analysis. Utilization unavailable: temporary
harness waited for buffered CSV and started sampler0.335s before exit;0CPU/NVIDIA samples,
PDH target-exit failure. Analysis partial; capture-session.complete describes game only.
Use immediate sampler startup as existing tools/start-play-capture.ps1 already does; no new
readiness abstraction or production changes needed. Game/sampler/PDH all exited; CU stays off.
Foreground/native144FPS remains unverified/unachieved. No unchanged build/native gate rerun.
No production change: existing launcher already starts sampler immediately; do not add another readiness layer. See human-debug REPORT latest section.


## Deformation dispatch batching
 실제 C++ 루프를 추출한 MSVC 검사에서500개 혼합 사례, 빈 입력,65535개 행 분할, 큰 메시의 Z 연속 구간, 각 정점의 중복/누락 없는 처리를 확인했다. 동일 크기22058개 합성 작업은1Dispatch다(실제 장면의 호출 수로 주장하지 않는다). 저장된 ShaderContracts.test_ray_deformation_dispatch_axes_are_disjoint도 통과했다. 네이티브 rendering.ray-entity-ultra는150.986초/8프레임/validation_errors0/execution_valid=true로 통과했다. receipt verify-20261015T120306505Z-31904.json, 별도 결과 deform-batching-native-result.json.

첫 진단 rt-deform-batched-16ticks는 설정을 LOCALAPPDATA에 넣었지만 smoke는 artifact/Profile을 읽어 Medium/RT off로 실행됐다. 비교 assertion이56설정 불일치를 발견했고, 원본을 보존한 채 제외했다. 이후 rt-deform-batched-ultra-16ticks는 Profile/settings.json을 기준 설정과 hash 비교한 뒤 실행했으며, 실제 기록의56개 requested/effective 옵션이 기준과 모두 일치한다.

아래는 기준 rt-budget-single-scan-16ticks와 새 최고옵션16프레임 진단의 후반11..16 평균이다. 두 실행 모두 외부 sampler ON, FHD100%, UltraRT, FG/low-latency OFF의 숨김 lockstep 진단이다. foreground/144FPS 판정이나 일반 플레이 개선율로 쓰지 않는다.

|구간|이전 ms|이후 ms|
|---|---:|---:|
|GPU RT 변형|3.871|2.034|
|CPU RT 변형 명령 기록|3.181|1.972|
|CPU 전체 렌더|268.874|268.147|
|GPU critical path|168.117|168.795|
|반사 추적|59.082|60.661|
|GI 추적|51.309|50.626|
|동적 BLAS|10.121|11.370|

해당 변형 패스 GPU47.5%, CPU 명령 기록38.0% 감소를 관찰했다. 전체 프레임 개선은 확인되지 않았다. 실행 시점·클럭 차이가 있는 단일 비교다. simulation checksum/56설정 및 최종 depth/GBuffer3종/lighting HDR/composite HDR6개 raw 결과가 모두 동일하다. Runtime3680제품 중 ray_geometry_cs.dxil 및 local_probes.spkg의 digest가 달라졌으므로 전체 제품이 셰이더 하나만 바뀌었다고 주장하지 않는다.

Playing7.329초 구간의 CPU6표본/6.346초는1.709core,20논리CPU 대비8.543%. NVIDIA7표본 장치 전체 GPU56.857%,graphics clock2655MHz(1710..2820),108.424W이며 다른 앱도 포함한다. PID GPU는 PDH초기화7.173초/카운터오류2회/대상관측 누락으로 N/A다. 게임과 sampler exit0, CPU/GPU16/16complete/invalid0이지만 분석은 partial로 유지한다.

공유 그룹 무거운 순서: 반사62.347,GI51.976,ray_scene14.723,AO12.900,그림자9.494,surface5.773,geometry4.452,DOF1.553,lighting1.211,volumetrics1.201ms. 공유·겹치는 GPU작업이므로 합산값은 frame time/단독 옵션 절감량이 아니다. [패스·56옵션 전체 결과](../../../Build/performance-actual/rt-deform-batched-ultra-16ticks/REPORT.md), [비교표](../../../Build/performance-actual/DEFORM-BATCHING-REPORT.md).

Release SHA25639AE256FF66AF1F7753772BDDE8AFB93A2C20510E598EB78E1BC09E523192EE1,
Debug32F00C591DCA5A3DE59324AC2063151CC5925D61157775877AB87BDB9843AE0D.
Runtime generation fe8e660b279bcb32dfa6a472b82c280c4b292c942e7e7d2842a26f8e9d02dcd7의3680제품을 두 빌드에서 검증했다. Native test generation48484c1c. 모든 소유 게임/계측 프로세스가 종료됐다. 컴퓨터 유즈OFF, 최고RT/FG포함144FPS는 미달/미검증이다.
