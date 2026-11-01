# DEV_STATE

AI implementation continuity only. Original main, HEAD
ffef538a760228341ed244307c338ab5e0a6af86 plus uncommitted changes; no commit/push.
The prior directive goal remains incomplete. Current item dispositions and exact
check/source/PE receipts: Build/structure-slim-20261101/disposition.md and
verification-phase1.json through verification-phase17.json.

## Contracts

Windows only. Public module layering/OFF-OFF,60Hz,strictFP/RNG,Debug/Od/asserts/
checked iterators,renderer_frame/Ob2,1080pUltra100%200,translucent slimes,four Hero
layers and authored sampling quality remain. Preserve binding/range/alias/queue
completion,accepted-prefix/old-reader leases and separate CPU activation/native
mutation/Main Present receipts. Release excludes developer diagnostics; DRS/FG
control timing remains. No generic manager/registry,per-frame verification hash,
new runner or repeated broad gate. Root owns one Ninja/one GPU workload; no edits
while Ninja runs. Agents Luna/high or Terra/high,no Sol.

## Current phase17 checkpoint

Receipts:verification-phase17.json;phase17/research-and-decision.md,
owner-analysis.json,foreground-analysis.json,latest-foreground-profile.md andCSV.
FinalC++renderer_frame337836a...;hs_experiment SHA256b2fe6d44.../FNV2c563c3e8eef429e.
Originalmain+prior dirty preserved;no commit/push. No ownedNinja/GPU workload.

Static GI leafindices borrow the existingimmutable section;onlycurrenttail owns
indices,withprevious prefix/tail order andlategraphversions. Oneframe owner keeps
generation/scene andComPtr<D3D12MA::Allocation> foractualdeduplicatedbackings. It
replacesper-allocationcontrolblocks andN-elementowner-vectorcopies. Nativebuffer/
slot consumers alreadyretainedthewholeownerset;collectiveretention remains.
ResolveQueryData mergesonlyactualconsecutiveselectedpairs onsamequeue;holes/base,
separateheaps/readbacks andlast-producerdependency remain. Realcapturedorder
implies118->25calls(analytical,notGPUtimeproof). ExistingGBVmultiqueuecasepasses
consecutive1/2,gap4,base4 andindependentCompute/Copy;CPUsceneinputpasses.

CurrentDebugproductionlinksPASS;ownerGI21normalvalidationPASS/errors0. Initialfull
GI-GBVrunwasabortedinNVIDIAshadercompilation;notapass. ExistingmultiqueueGBVvalid.
SelectedMSVC14.44/Od unchanged;PDB C1902 inrestrictedexecution requiredsamecommand
withnormalWindowsauthority,notcompilerflags/PDBdeletion. Restoredshader'soldmtime
wasrefreshedbeforeofficialcook/bake,closingmissedNinjadependency.

Offscreenlockstep300same02ddproducts:graph_buildmeans5.201/5.122->5.005ms,
p956.332/6.155->6.072;record2.305/2.324->2.173ms. Scopedobservations,nofullFPSgain.
Trackedlivemax925300096B/retiring0 unchanged,notprocessRAM. Events/render/particle
facts match,27/36opaque/GBuffer/AO/etcrawhashesmatch;scene_work/OIT/exposure/
temporal/composite9differences remainunresolvedandunattributed.

LatestactualPresent/nonlockstep2400tick:1326/1326foreground,1080Ultra100%200,
translucency4layers,timingON/validationOFF;GPUcoverage/draincomplete/invalid0.
Warmtick301..2100 has1067frames:35.54FPS,1%low26.80;intervalmean28.134ms,
p9532.430,p9934.729. GPUtopgroupmeansHeroComposite4layers3.810,SurfaceResolve3.442,
NativeGI2.365,GTAO1.490;CPUgraph_build5.232,record2.384,submit2.075,plan2.013ms.
CPUelapsed/nested/wait andGPUqueueoverlaparenotadditive. GPUcriticalpathtotalhas
0validsamples,threadCPUbusy unmeasured. Gameplaytick1.099ms/60Hz tick. Thislive
workload cannotbeA/Bcomparedwithoffscreenlockstep300.

POMinterleavingcandidateimprovedtargetmean/p95intwoA/Bpairs,butwholeworkload/tail
anddownstreamidentityunqualified;allnewshadercodeexactlyremoved. Smallpatch/data
retained,rejected1b1242Generationremovedunderexclusiveleases. Finalofficial8e76b984...
contains3639productdigestsexactlyequalto02dd;default9750unchanged. EarlierAO
prepared-inverseWconsumer andlight-query-first changesremainwiththeir24GBV/6HDR
receipts;rawfallback/producerchecksremain. No quality/samplecut/publicAPI/runner.
NativeGIactual-consumer/history/finalHDR variability and60FPS/fullgoal remainopen.
Next:Hero/Peel/SurfaceResolve/GI/GTAO andCPUgraphbuild/record/submit/compiletails;
submitelapsed canincludedriverwait,sodonotlabelitpureCPUcompute.

## Current implementation

- Render normalizes frame/time/UI once,then passes explicit frame/view inputs.
  Hidden frame pointers/raw HLOD re-Prepare removed. Auxiliary-only first views
  consume births once. Immutable UiDrawContent is the sole internal UI input;
  layouts survive raster-target changes,bitmap/Present/click contracts remain.
- Sealed SceneGeometry has one resolver. GI uses typed immutable sections,ordered
  current runs and a consuming preparation token. Warm planning skips immutable
  revalidation; registration refreshes late-bound geometry graph versions.
- Static SDF reads packed12-byte positions appended once to each page allocation:
  the SDK encoded old76-byte Vertex stride in6bits and incorrectly read12. Raster
  ABI,skin36/deformed12 and native owners remain. Residency includes added bytes;
  SDK admission rejects unrepresentable strides.
- GI defaults8Mi references/16Ki bricks,triangle storage64MiB. Actual SDK scratch,
  adapter available+live budget and completion-based retirement remain. Disabled
  hero volumes no longer request unused World shadow coverage.
- GI camera cuts invalidate outer view history before RenderView. Reset
  tests actual internal GI history as well,so that invalidation cannot skip SDK
  reset. Same-extent drained reset retains screen allocations/descriptors/graph
  state,recreates SDK GI,retains SDF,and releases completed refs/scratch once
  before admission. Reset rejects prepared frames/pending graph/incomplete uses.
  Existing SoftwareGI fixture cuts on frame8;requested21/actual20frames passed
  GPU-validation configuration,execution_valid=true,validation_errors0. A single
  optional history-revision accessor now avoids copying Status/reason each warm
  frame. That final value-access change compiled;long GPU check not repeated.
- Static GI Rows now hold only admitted included jobs plus checked excluded
  counts and unique page slots. Warm frames borrow them directly;removed static
  per-job page/source suffix arrays and per-job ResourceVersion copies. Requested
  page identities still detect fallback/residency changes;native imports use the
  actual resolved slots. Late slot versions use one bounded array. Wind/skin,
  late-bound reads,canonical leaf order and strong GPU allocation owners remain.
- Corrected existing CPU scopes: software-scene preparation excludes the parallel
  shadow join;shadow preparation includes setup/join wait. Worker task timing is
  separate and overlaps. CSV columns unchanged;runtime metadata states meaning.
- Runtime owns snapshot/status VFX births before one frozen native batch. Shared
  PairFacts masks/previous lookup replace repeated scene copies/coverage scans.
  Fixed Fresnel/mesh recipes are sealed borrowed RenderData views; dynamic command
  data stays separate. Actual live resource/payload checks remain. Distinct-time
  event birth/display grouping and activation/native/Present ACKs remain.
- Cosmetic links moved from Gameplay to Runtime presentation retention,with stable
  DomainSignal.sequence,8/11/9tick lifetime,audio/checksum/session semantics.
  Five eager Gameplay states are values beneath its unchanged heap pimpl;
  TickFixed unused delta removed,scheduled-action facts counted once,and no-op UI
  restores prior checksum dirtiness only after normal return.
- Shared post Add-or-Bind/topology comparison and admitted native guide contracts
  replace duplicate setup. Loose/packed asset sources are disjoint. Feedback
  reuses per-bank view snapshots and SSE2 reduces valid row bytes with scalar tail.
- Settings normalize edits once; save failure retains applied graphics/unsaved
  status. Real out-of-line settings/generation leafs fix header leakage/COFF
  overflow without /bigobj. Full-only clock storage compiles out of Release.

## Verification scope and unresolved work

Phases1-3 linked Debug/Release production and relevant existing CPU leaves;
settings,VFX,gameplay,graph,pack,architecture checks passed. Later changes linked
Debug only;earlier Release receipts do not cover them. Phase17 links current ProjectHS/hs_experiment/RenderTestRunner and existing
hardware single/multiqueue tests;current CPU/native/replay checks are scoped above.
Current200/fixed-Hero/native checks are scoped above. Earlier phase10 CPU/CLI receipts remain
for unchanged inputs only. Latestforegroundandper-pass evidenceis phase17above.
No fullworkflow,unchangedCPU suite,Release orLinux rerun. DirectStorage remains disabled by the pinned build.

Corrected-input200enemy1800frame validationON/timingOFF qualification returned all
source-valid SDF records below provisioned caps:static refs7347546/bricks10604,
dynamic refs3285426/bricks590,allocation peak1306471936B,scratch242246144B. Last3
physical visits pending;not all-source/other-content proof. Temporary counters
removed. See sdf-after-fix/analysis and its source receipts.

Official full producer closure candidate5b5f5bc9dace0e1e1b564a7390b37059643b961024ccf9f8eb7090e9898271e5
combines terrain/shared BSDF/Resolve:85DXIL+local-probe SPB1 changed. Existing GPU
BSDF prepared/raw7,coatedIBL72,Fresnel14 passes. Probe numeric difference<=2.98e-8.
Static DXIL size/instruction counts are not frame-time evidence. Default9750a33dc4e29081ffb464b2dce7a37ea9776018e9947ee092815ccece73fe05
still selected;simulation content85a0f257edd1ce70. No fabricated old action result,
overlay/manual manifest or tolerance relaxation. Raw GI/consumed prior/internal
cache/SDF variability remains unresolved;do not attribute it to harmless randomness
or call shader candidate quality accepted.

Next:continue measured graphics/GI preparation and graph compile-miss tails.
Do not add another shader/bake Action solely from coarse compile logs:probe-shadow
16products and static-shadow574upstreams/outputs match prior5b5f;producer executable
identity is intentionally in ActionKey. Preserve that rebuild defense.
Hero fixture exposure input is now fixed;do not generalize its exact final PNG
comparison to unresolved native GI/whole workload quality.
Continue constants/graphics work from actual counters.
Archer palette lookup repeats were reviewed,but248us scope includes sampling and
actual key count is unobserved;do not add another map on that evidence alone.
Keep distinct baked coarse VSM/full-resolution snapped CSM and separate full-rate
GI seed vs Tier2VRS deferred evaluation unless actual equal-quality benefit is
proved. Host-cap streaming/UI/frozen no-op reviews already found old defects fixed;
do not recreate those tasks or add duplicate validation/manager layers.
