# Findings

Preserved close capture shows broad flat grass-texture areas, isolated low leafy islands and puddles that read as pale strips. Existing report explicitly lists weak vegetation volume and overly bright water as unresolved. Road-width and real rut geometry already integrated; retain them. DEV_STATE no longer tracks art detail; recover from Docs/Verification/20261018-meadow-ground.

Reference priority: final realistic meadow photos for soil/plant structure; preserve bright fantasy mood and overhead game readability. No village creation.

Found material-level transmission fallback: disabled/rejected refraction used sky instead of the opaque/layer source at the original pixel. This can make shallow puddles pale. Preserve the valid pixel background with raw HDR+actual opaque view depth for interval fog, or already-composited layer HDR for layered transport. No water reflectance/exposure gain change.

Stage2: bake static wet/dry in existing ground UV.xy, preserve mesh22 in both contexts and forward ctx.uv to terrain intrinsic; dry cracked soil2K texture already exists and uses layer2 rather than new procedural loops/textures. Reuse meso noise for bounded road edges and color.4 grass graphs consume instance tint; regular grass now neutral and dry packing retains warm tint.

Wicked fixed code downloaded and read: splinefactor drives both height and baked blendmap (1130-1148); LOD internal/border indices separate. Terrain3D fixed vertex185-246 morphs XZ and interpolates paired heights. Adopt shared authored height/control and preserved boundaries here, not full infinite clipmap. Mud-puddle interface candidate roughness .18 and blue-weighted sediment absorption(3,8,16)/m replace clear-water blue tint; art constants, not measured natural-water coefficients.

2026-10-18 follow-up: latest terrain 123934/122750/122290 triangles. Fine wet macro-cells keep 32-way subdivision even where the actual surface is locally linear; rectangle_without_center unconditionally blocks all protected cells before checking position/normal/tangent/control error. Evaluate narrowly removing that redundant blanket for triangles whose existing attribute bounds prove equivalence, without changing height oracle. POM currently repeated for three stochastic samples in each active layer; delegate screen-space subpixel skip. Read commands hit invalid optional ground.json/SourceContentCooker and misplaced rg -g; no source mutation from those failures.
