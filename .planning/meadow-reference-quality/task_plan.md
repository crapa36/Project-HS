# Meadow reference quality continuation

Goal: improve existing in-game meadow toward user-provided reference photographs: readable dark damp soil/ruts, localized puddles, vegetation volume and natural coherent patches. Preserve default gameplay camera, combat readability, road widths/contact, 60Hz, current non-hardware-RT runtime and dirty optimizations. Leave uncommitted.

- [complete] Transmission fallback and stage1 visual integration; native smoke/environment assets passed.
- [complete] Stage2 macro relief/banks/water, static wet/dry controls and geometry oracle; native smoke/environment/HLOD passed.
- [complete] Improve bounded grass/detail and contextual roadside prop composition (placement agent).
- [complete] Refine flowers/clover with Blender, round-trip and register v002; task Blender process closed.
- [complete] Latest scope: add five LOD props with shared atlas layer1, bounded dense short/long/dry grass, flowers and road-centre weeds; preserve authored16/ABI2.
- [complete] Integrate catalog/assets/placement, seeded-order fix, narrow CPU/standard native checks and final FHD capture; export selected Generation to Debug/Release.
- [complete] Inspect output and record remaining gaps without AAA/FPS completion claim.
- [unresolved] Additional GBV teardown stalls, both attempts stopped; standard native checks pass. No further long tests in this art pass.

Root owns material shaders, integration and this plan; workers own disjoint assigned files. No long benchmark, global renderer redesign or unrelated content cleanup.

Current follow-up: use perceptual approximations rather than adding geometry. Root: remove only redundant sampled centres with existing precision limits and preserve water/support/LOD contracts; worker: subpixel POM skip with numeric guard. Reuse existing maps and shared heights; no new tessellation path. Narrow CPU/oracle/shader/native check then record results.

- [complete] Perceptual follow-up: conservative subpixel POM, redundant centre removal without relaxed attribute/oracle limits; final content exported, narrow source/shader/asset and oneFHD16 standard native check passed. No FPS/GBV claim.
