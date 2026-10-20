# Local probe producer and native integration

`local_probes.json` is an authored spatial layout for the independent SPB1
product `render/lighting/local_probes.spkg`. Global
`render/lighting/default_probe.rpkg` retains its strict LPB1 v1 format.

The source producer parses the scene-probe schema and captures an immutable
snapshot of actual selected environment, static geometry/material/texture,
static light and procedural layout inputs. Hash those inputs into nonzero
`SceneProbeRevisions` values and keep their canonical sorted unique dependency
IDs. The four description dependency indices must identify the environment,
material catalog, static light definition and static layout identities. They
must be resolved inputs, including their relevant dependency closure; do not
insert placeholder revisions or refer to products that were never produced.
Call `BuildSceneProbePackage` with the derived description and authored records.
Empty bake spans intentionally produce a layout-only package for native baking.

The producer and Generation validation share `DeriveSceneProbeRevision` from
RenderData; do not duplicate a JSON hash or truncate another hash differently.
The exact fold is documented in its public header. Supply sorted unique actual
logical IDs and lowercase SHA256 digests. Per-domain input sets are:

- Environment: `render/lighting/default_probe.rpkg` and its declared product
  dependency closure.
- Material: `render/material/terrain.rpkg`, its parameter product, all 14
  `render/{rock_0..3,trunk_0..2,leaf_0..2,grass_0..3}.rpkg` and corresponding
  `render/material/<name>/parameters.rpkg`, with every declared product
  dependency recursively included. This includes actual material, texture,
  geometry and shader products.
- Light: `game-data/simulation-rules` and its product closure, plus exact source
  digests for `Source/Presentation/Private/snapshot_projector.cpp`,
  `Source/RendererD3D12/Private/renderer_frame.cpp` and
  `Source/Core/Public/hs/core/render_snapshot.hpp`.
- StaticLayout: `game-data/simulation-rules` and its product closure, plus exact
  source digests for `ContentSource/GameData/level.json`,
  `Source/Presentation/Private/environment_layout.cpp`,
  `Source/Presentation/Public/hs/presentation/environment_layout.hpp`,
  `Source/Presentation/Private/environment_projector.cpp` and the immutable
  authored `ContentSource/Lighting/local_probes.json` snapshot.

The current project has no separate cooked static-light/layout products.
Generation validation must retain these exact source input identities from the
Action metadata if it needs to recompute the four authored package revisions;
selected product digests alone cannot reproduce those two domains. Retain the
validated baseline with the immutable static scene. Qualify a native capture
with actual LightView and static-instance/material state as well; changed lights
or procedural placement require a new capture even within a content Generation.
Never label an instance/LightView hash as the producer's source-input revision.

An offline producer may supply actual common-material static-scene cube renders
to the same API. Supply all four complete arrays: scene-linear Rec709 RGBA32F
raw radiance, normalized GGX full mip chain, signed cosine-convolved world SH9,
and nonnegative first-hit radial world distance. Raw/distance order is probe,
D3D face, row, column; GGX adds mip inside each face. Miss distance equals that
probe's finite `capture_distance`. The local capture includes global environment
through sky once and static direct/emissive response; it excludes dynamic
characters, dynamic VFX, UI, post, exposure and recursive local probe lighting.
The portable SH/GGX producer must use the same conventions as
`local_probe_bake.hlsl`. CPU readback is an explicit offline export operation,
never part of normal frame registration or native bake publication.

SPB1 uses magic `HSSPROBE`, version 1, little-endian marker 1, a 192-byte header
and 128-byte records. Header byte offsets: identity/total 24/32, payload kind
40, probe count/edge/mips/sample count/blend 44/48/52/56/60, four uint64 revisions
64..95, dependency count 96, four uint32 dependency indices 100..115, reserved
116, dependency/raw/GGX/SH/distance byte counts 120/128/136/144/152, and SHA256
of header bytes 0..159 followed by all bytes after the header at 160..191.
Data follows contiguously: uint16
length-prefixed dependencies, records, raw, GGX, SH, distance. Layout-only has
zero counts for all four bake payloads. Readers check bounds, count/stride
products, exact layout, digest, all dependency references, finite fields,
ordered unique probe identities and complete baked arrays.

Serialized record offsets: stable uint64 ID 0, position/weight 8/20,
influence minimum/fade 24/36, influence maximum/visibility bias 40/52,
projection minimum/visibility fade 56/68, projection maximum/capture distance
72/84, reserved zero 88..127. Influence contains probe position; projection
contains influence; capture distance covers the projection diagonal.

The concrete renderer service has Prepare/Commit/Register/CompleteSubmission/
ResetAfterCompletion. Prepare allocates without submitting. Register retains
`shared_ptr<const StaticRenderScene>` and receives explicit graph reads for
every actual geometry/material/light/shadow/environment resource used by the
root-owned draw callback. At most six 90 degree faces are captured per graph;
each finished probe is SH/GGX convolved before the next probe completes.
Capture uses absolute world-space, unjittered LH row-vector cameras. D3D face
directions are +X,-X,+Y,-Y,+Z,-Z with up vectors +Y,+Y,-Z,+Z,+Y,+Y; near is .05 m.
Use dedicated per-face FrameConstants storage so the main view is preserved.
The callback receives RGBA32F raw-radiance RTV, R32F radial-distance RTV and
D32 depth DSV and uses the same eligible static material/opacity path. Render
global sky at depth far with the same environment orientation, without exposure.
The native producer uses raster rendering.

Pass IDs are 180..185 for cube views, 186 for immutable package/record upload,
187 for cosine SH, and 188 for GGX. Environment upload retains pass 80. Shader
products are `render/shader/local_probe_sh_cs.dxil` (CSSh) and
`render/shader/local_probe_ggx_cs.dxil` (CSGgx), both cs_6_0.

Append five space23 SRVs to native descriptor table32, with no new root DWORDs:

| Register | Native view | Contents |
| --- | --- | --- |
| t0 | RGBA32F TextureCubeArray | raw scene-linear captured radiance |
| t1 | RGBA32F TextureCubeArray | complete GGX mip chain |
| t2 | StructuredBuffer float4, stride16 | probe-major cosine SH9 |
| t3 | R32F Texture2DArray | six exact first-hit distance faces per probe |
| t4 | StructuredBuffer, stride128 | eight float4 GPU records |

The GPU record follows `HsLocalProbeRecord`, not the serialized source record.
`parameters` stores max GGX mip, cube index, enabled and active probe count.
Disabled exports are valid typed null cube/SH/distance descriptors plus a real
zeroed metadata buffer. Its active count is zero, so sampling returns global
environment without touching capture resources. Consumers declare reads of
every nonempty returned ResourceVersion. Every attempted graph calls
CompleteSubmission with current scene/revisions, including failure. The service
publishes only after full successful bake GPU completion and matching scene and
revisions, and retains stale scene/resource leases until safe replacement/reset.

Use world-position-aware `HsLocalProbeIrradiance`,
`HsLocalProbePrefiltered`, and `HsLocalProbeRadiance` for surface indirect
lighting; global sky continues to call global probe helpers. Local influences
fade in world metres, first-hit visibility attenuates receivers behind static
occluders, reflection/raw queries project from the receiver to the probe box.
Weights are normalized to a coverage in [0,1]; environment weight is exactly
one minus coverage because captured local radiance already contains sky.

`forest_slope_1k.hdr` is unchanged upstream Poly Haven CC0 RGBE source.
Its source JSON records official URLs, MD5 and SHA256. The global default probe
input selects it with explicit `authored_rec709_if_unspecified` policy: zero
chromaticities are treated as unspecified and interpreted as Rec709 linear by
that authored policy. Valid nonzero declared primaries retain strict Rec709
validation. The raw file is preserved; the default source and policy are Action
inputs while LPB1 remains version 1.
