# Ground detail integration — 2026-10-16

## Result

Added daisy, buttercup, violet, clover, curled fallen leaves, broken twig clusters and weathered short fences. Seven archetypes each have three LODs; petals/leaves use closed silhouette geometry rather than alpha rectangles. Small stones and short grass reuse existing assets. Terrain adds sparse wet/dried mud marks, paired wheel ruts, alternating footprints, moss and shallow normal relief while preserving the preceding grass-color correction, 12/9 m mapping and original terrain maps.

Assets: `ContentSource/Models/Environment/GroundDetail/ground_detail_v001.blend`, `ground_detail_v001.glb`, `FBX/`, `preview/mcp_final.png`. Authoring and topology evidence: `BUILD_NOTES.md` and `verification/` there. Reproducible authoring: `tools/art/create_ground_detail_blender.py`.

## Runtime contracts

- Existing 42 mesh values match bc7bd60 exactly; 21 new meshes append to the source catalog.
- Geometry ordinals14–20 share authored material15; terrain stays14. Original mesh/asset IDs stay stable; RenderMesh GroundDetail appends at21.
- One opaque material and three2048-square PBR maps (BC7 sRGB base, BC5 roughness/AO and normal,12mips). Atlas cells have24px gutters. Runtime/Blender normal-map green-channel conventions are verified separately.
- Deterministic cached clusters avoid the central path and obstacle clearances. Maximum320 new patches plus64 pebbles and64 short-grass instances. Existing instances retain their order and IDs. Fences are visual props outside the open corridor.
- LOD, visibility, shadows, scene HLOD, probe capture, RT/OMM and texture feedback are connected. Geometry21 is distinct from material16; probe pages22 include terrain. Feedback bindings expanded90→96, visibility variants23→24, hero metadata IDs unchanged.
- HLOD eligible metadata is sorted by stable ID before hashing, matching the canonical content validator without reordering the rendered instance array.

## Validation

- All21 new meshes: finite positions/UVs, unit orthogonal normal/tangent basis, valid material/layer, no degenerate or duplicate triangle/polygon geometry; LOD triangle counts strictly decrease.
- Seven FBX files re-import with all21 LOD meshes, matching triangles/UVs/materials/dimensions; GLB re-import has7LOD0 meshes and3embedded maps. Packed blend reopened and rendered through Blender MCP. Parent independently verified all18 source-receipt hashes.
- Selected CPU gate7/7 passed: architecture, instance order, local-probe draws, package format, scene HLOD, shader reconstruction and OMM contracts (`verify-20261015T143715866Z-27240.json`). Final shader contracts30/30 passed after visibility-boundary correction.
- Presentation contract passed (`verify-20261015T144007305Z-6172.json`). Final environment asset package check passed1.32s (`verify-20261015T151152931Z-3984.json`).
- HLOD digest regression reproduced before the fix and passed after it, including real-layout canonical digest parity and a tiny mixed detail/pebble cook; `Build/ground-detail/digest-before-test.log` and `digest-after-test.log` (5.13s).
- RuntimeOFF/ContentToolsOFF configuration and Core/RenderData/EnvironmentLayout build passed. Debug and Release ProjectHS builds and runtime-full snapshot verification passed,4222 products each.
- Generation: `99a3978b877f8522f2f0dfde46de00d3052b336523efd46a1fda393e3b798517`.
- Debug SHA256: `798CEA03042C98A6C2E8FA95AA067C220F262E3AA95BF1883EC8C69500F0A95B`.
- Release SHA256: `AC12C2848612FC0BCA8170B85EEE2D1F166DF3E34F6BC83043A3E16606340172`.

| Native validation | Raster | RT |
|---|---:|---:|
| Rendered frames |16|16|
| Execution/assertions |pass|pass|
| D3D12 validation errors |0|0|
| Shared-detail material pixels at captured frame |161|54|
| Process wall seconds, including startup |84.640|70.717|

Both native captures are1280×720,100% internal scale, InternalTAAU, frame generation off, isolated profiles and identical simulation checksum4930774313638502297. Raster uses Medium effects/geometry with Ultra textures; RT uses Ultra quality fields with actual `hybrid_rt` shadows/reflections, `ray_traced` GI and `rtao`. Other post toggles retain the isolated profile (DOF and motion blur off); this is not an all-options gameplay benchmark.56 requested/effective/native settings were recorded. HDR buffers are finite, and new material15 is present in both G-buffers. Images were inspected. SDK diagnostic warnings are retained in stderr; zero errors refers specifically to D3D12 validation.

Captures and receipts: `Build/ground-detail/native/`, `native-rt/`, `native-analysis.json`, `cook.log`, `msvc-debug.log`, `msvc-release.log`, `assets-check.log`. These16-frame smoke results do not measure steady-state FPS or isolate added GPU cost. Startup wall time must not be converted to FPS.

## Corrections and limits

The first new test compile used unsupported AssetId equality and was corrected to compare its value. Final asset validation found detached twig fibres, LOD height drift and overlapping violet root caps; source geometry was corrected and re-exported, without weakening checks. One stale-mesh cook was intentionally stopped. The following full cook exposed insertion-order versus canonical-order HLOD hashing; the regression fix was proven before the successful final cook. All failure logs remain under Build/ground-detail.

Mud/puddle/footprint/rut features are static material marks and shallow normal relief, not dynamic water, tracked footsteps or collision deformation. Props have no newly authored collision or wind animation. Native camera-motion/LOD-transition play, completed GBV, HDR-display testing and144FPS performance measurement were not run. Visual evidence is concrete asset/game rendering, not a blanket AAA-quality certification. No Computer Use session was restarted. Task-owned Blender and native-game processes exited; unrelated processes and original assets were preserved. Changes remain uncommitted.
