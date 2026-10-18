# Representative ground and static water implementation

Scope: bounded 24 m square centred at the world origin; enabled by Presentation for arenas at least 14 m in half extent. Existing 2D simulation remains unchanged. Outside this region the existing terrain remains.

## Source and runtime contract

- `ContentSource/Models/Environment/meadow_surface.json`: authoritative static depression records, seed 41703, dimensions in metres, partial-fill fractions, constant water levels; roe-deer cloven-hoof assumption recorded. Human records alternate mirrored arch and tapered heel/toe outline.
- `Tools/art/meadow/generate_surface.py`: reads this source; generates geometry, the Presentation public contact-height sampler, and matching shader masks. Running again replaces only its owned outputs. No Blender or external service was used for this terrain.
- `Source/Presentation/Public/hs/presentation/meadow_surface.hpp`: `MeadowGroundHeight`, `MeadowContains`, `MeadowWaterLevel` (NaN outside water), `MeadowWetness`, `MeadowCartWear`, `MeadowWalkWear`, `MeadowPressed`. Source formulas are shared with generated geometry. Sin/cos of record orientation are precomputed; distance rejection precedes local shape evaluation.
- `Content/Shaders/meadow_surface.hlsli`: matching bounded material masks. Water does not scan the records per pixel: optical depth is baked into its UV.x.
- `ContentSource/Models/Environment/meadow_surface_meshes.json`: separate source supplement consumed with the existing environment mesh source. It contains `meadow_ground` and `meadow_water`, each with the existing three-LOD file contract. It does not overwrite the prop source.
- `RenderMesh::MeadowSurface`: variant 0 ground, variant 1 water. IDs `render/meadow_ground.rpkg`, `render/meadow_water.rpkg`. Both are origin/unit-scale authored meshes. Ground uses terrain material. Neither participates in scene HLOD. Other agent integrates four outside slabs and contact heights.

## Real geometry, not painted depressions

Ground has 100,370 triangles, adaptive shared edge sampling, y=0 boundary, actual shallow shoe, deer-hoof, wheel and basin depressions. Water has 2,621 triangles in 13 authored partial-fill records. Each record's connected surface is a constant horizontal level; triangles are clipped to actual piecewise-linear ground height. Water UV.x stores vertical optical depth, UV.y stores 100+basin index. Raw vertex material selector remains zero for valid packaged texture-layer contracts; shared vertex evaluation identifies water from its dedicated mesh and basin lane.

Both dry and partially filled examples of wheel, human and animal traces exist. Two separate hoof lobes remain visible. Larger wet/dry patches and a cart spur to the supply site at (11,3) accompany the tracks. Central cart grass is retained through the two-wheel wear mask; pedestrian wear has its own lateral route.

## Rendering

Ground follows existing terrain textures/POM, with the representative region's shared route and wetness masks replacing the former full-width soil strip. Damp soil retains roughness; a limited high-wetness fraction receives smoother thin-film response.

Water follows the existing layered Hero transmission pipeline with a water-specific closure: IOR 1.333, roughness 0.09, dielectric non-metallic surface, per-metre absorption, baked local depth and horizontal normal. It samples existing opaque radiance/depth through that pipeline, so the mud floor is optically distinct. The same closure is used by weighted-OIT fallback when layered refraction is disabled. Static geometry dependencies, draw buffers and scene references are declared to the render graph. Normal opaque drawing and all shadow pixel shaders reject water; static caster bookkeeping remains allocated to preserve its existing node-cache contract.

Water GPU cost is SHARED with the existing Hero transparency/refraction passes, not separately isolated by current timestamps. No independent water milliseconds or FPS gain is claimed.

## Checks completed by ground implementation agent

- `Tests/check_meadow_surface.py`: finite vertices, upward triangle winding, ground boundary continuity, per-basin horizontal water plane, bounded baked optical depth, nonempty actual water thickness, all three trace species with dry and partial-fill cases. Passed.
- Generic `ScenePS` and `ShadowPS`: DXC ps_6_0, strict validation and warnings-as-errors. Passed.
- `hs_renderer_d3d12` and `hs_presentation`, msvc-validation: compilation passed; log `Build/meadow-surface/renderer-compile.log`.
- Production generated Hero transmission shader compilation: pending result in `Build/meadow-surface/transport-compile.log`; parent must inspect final receipt.

## Still required

Parent integration must cook the updated environment plus prop sources, verify Debug/Release native render and GPU validation, inspect same-camera actual ground/water screenshots, and measure foreground performance. No native image inspection or new FPS measurement was performed by this agent. Three geometry LOD source files currently preserve the same detailed geometry to avoid shoreline drift; this is not a measured optimal distance budget. Static water refraction uses existing screen-space transport limitations; no new fluid simulation, rain ripple system, runtime footprint generation or terrain destruction was introduced.
