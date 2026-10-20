# Elin interchange v1 compatibility checkpoint

`hs_character_interchange character.interchange.json <new-output-directory>`
validates a Blender export and writes `archer.rpkg`, `source_metadata.json`, and
`import_report.json`. This is a portable CPU cooker using the real RenderData
writer and reader. It does **not** publish an asset Generation, cook textures,
replace the game player, or implement the complete Elin delivery contract.
The generated package is the existing five-slot runtime projection. The source
JSON metadata is preserved verbatim as JSON values; the original source binary
must remain alongside the original manifest for all 21 animation payloads.
`source_metadata.json` is a report copy, not a self-contained re-export.

## Exact source schema

Manifest location: `ContentSource/Characters/Archer/Export/character.interchange.json`.
All binary numeric scalars are little endian. Required root members:

- `schema_version`: integer 1
- `id`: `character.archer.elin`
- `coordinate_system`: `left_handed_y_up_z_forward_meters` (the old ambiguous
  `y_up_z_forward_meters` tag is rejected)
- `binary`: `{path: "character.bin", byte_length: integer, sha256: lowercase 64 hex}`
  Path is relative to the manifest directory. Absolute paths, dot segments,
  backslashes, drive names, NUL, and symlink escapes are rejected
- `buffers`: the five named descriptors below, each with `offset`, `count`,
  `stride`, and `type`. Their ranges partition the entire binary with no gaps,
  overlap, or trailing bytes. Byte offsets need no alignment padding
- `bones`: ordered array `{id: string, parent: integer}`. Parent indices precede
  children; exactly one root uses 65535. Root, all required ancestors, and
  socket/IK reference bones must be exported even when unweighted
- `materials`: ordered array `{id: string, ...authoring metadata}`
- `meshes`: ordered array `{id, first_vertex, vertex_count, material: material_id}`
  covering every expanded vertex contiguously, with triangle-aligned boundaries
- `masks`: `{upper_body: {required_bones: [bone_id, ...]}}`. Export the actual
  authored graded weights; the importer requires both zero and positive regions
- `clips`: the 21 source clips listed below, in payload order; each is
  `{id, first_transform, frame_count, duration_seconds, looping: boolean,
  markers: [{id, time_seconds}], curves: {curve_id: [float per frame]}}`
  Transform ranges partition all transforms. Markers must lie inside duration
- `runtime_clips`: `{idle: "idle", run: "run_loop", draw: "draw_charge",
  recoil: "shoot", death: "death"}`. Values are source clip IDs; all five must
  resolve. This mapping is temporary compatibility, not full action semantics
- `sockets`: array `{id, bone: bone_id, translation: [x,y,z],
  rotation: [x,y,z,w], scale: [x,y,z]}`. Required IDs: `weapon_grip`,
  `arrow_nock`, `projectile_visual_origin`, `quiver_socket`, `hat_socket`
- `ground_offset`: finite number; normally zero for grounded authored geometry

Identifiers contain ASCII letters, digits, `_`, `-`, `.`; length 1–128.
Unknown top-level/material metadata is retained, never treated as implemented.

## Binary buffers

| Name | Type | Stride | Layout |
|---|---|---:|---|
| vertices | expanded_vertex_v1 | 76 | position 3f@0, normal 3f@12, joints 4u16@24, weights 4f@32, UV 2f@48, tangent 4f@56, material u16@72, reserved u16=0@74 |
| parents | u16 | 2 | same values as JSON bones |
| inverse_bind_matrices | f32x16 | 64 | column-vector affine matrix flattened row-major |
| upper_body_weights | f32 | 4 | one weight per bone |
| local_transforms | trs_f32 | 40 | translation 3f@0, quaternion xyzw 4f@12, scale 3f@28 |

Transforms are frame-major, bone-major. Quaternion norms must be one. Scales
must be nonzero. Four skin weights sum to one; even zero-weight joint indices
must be valid. Triangles are expanded, with positive dot of geometric normal
and summed authored normals. UV uses top-left origin: flip Blender V. Transform
Blender positions with `C(x,y,z) = (-x,z,-y)`, determinant -1. This changes
Blender's RH basis to the runtime LH camera convention while retaining up+Y
and forward+Z. Source anatomical left is +X (facing -Y); it becomes runtime-X.
Source right/quiver -X becomes runtime+X. Do not rename anatomical bones.

Conjugate local/global/bind/socket matrices by `C * M * inverse(C)`. Transform
normals by inverse transpose (for orthogonal C, simply C). Reverse each source
triangle from `(a,b,c)` to `(a,c,b)` after this reflection. The resulting
geometric/authored-normal dot remains positive, and a front-view LH camera
projects it clockwise, as required by `FrontCounterClockwise=FALSE`. Archer's
current no-culling PSO can hide a winding mistake; it cannot validate the export.

For positive-determinant source object transforms, spatial reflection and UV V
flip each change tangent handedness: `h_out = (-1) * (-1) * h_in = h_in`.
Do not blindly negate tangent.w once. Prefer recomputing tangent and sign from
final runtime positions, normals, and flipped UV, including any evaluated object
transform.

`Contracts/materials.json` declares canonical maps as **NormalDX**, linear,
image top-left, authored UV V-up, green=-Y, in final-triangulation MikkTSpace.
Keep these canonical maps unchanged. A Blender +Y Normal Map node must decode
NormalDX green (`1-G`) in its preview node graph, without rewriting the image.
The runtime exported UV is V-down; its derived bitangent is the negative of the
spatially transformed authored bitangent. Thus existing NormalDX pixels already
have the correct sign for the current Archer shader and require **zero additional
green flips**. A newly baked OpenGL/+Y map requires exactly one conversion to
NormalDX at the declared export/cook boundary. Record source and output normal
conventions; do not infer them from mesh ID or add a mesh-ID shader branch.
The canonical source map is never overwritten by this conversion. Any runtime
BC5 decoder must additionally reconstruct Z and normalize as the materials
contract requires; the current compatibility shader still reads RGB normals.
A non-flat normal regression distinguishes no conversion, one conversion, and
an erroneous double conversion. Never apply a coordinate/normal conversion twice.

### Labeled handedness fixture

Match a Blender RH front camera at `(0,-5,0)` (up+Z) to runtime LH preview
camera yaw180/pitch0 at `(0,0,5)` (up+Y), both FOV60/aspect1. No jitter is used.
The LH camera's screen-right is `cross(up,forward) = -X`. Reversed-Z depth in
production does not change projected X/Y.

| Label | Blender point | Runtime point | Blender/runtime NDC X |
|---|---|---|---:|
| anatomical left / weapon_grip | (0.6,-0.25,1.2) | (-0.6,1.2,0.25) | +0.218785 |
| anatomical right / quiver_socket | (-0.35,0.2,1.3) | (0.35,1.3,-0.2) | -0.116580 |
| front-facing normal | (0,-1,0) | (0,0,1) | 0 |

The old rotation-only `(x,z,-y)` produces opposite NDC X for both markers.
A front triangle `(-1,0,0),(1,0,0),(0,0,1)` has source normal-Y. C transforms
its vertices to `(1,0,0),(-1,0,0),(0,1,0)` and normal+Z. Swap vertices1/2:
its cross product becomes +Z, and its front-view image winding is clockwise.
The CPU regression tests check these labels, sockets, screen coordinates,
normal direction, winding, and both tangent handedness flips. This is a
mathematical/source-code check, not a claimed D3D12 capture.

Code references: renderer_frame.cpp camera direction/eye and
XMMatrixLookAtLH/XMMatrixPerspectiveFovLH; renderer_pipeline.cpp rasterizer;
project_hs.hlsl InstanceWorldPosition and tangent basis; legacy
character_asset_cooker.cpp DirectX.ConvertScene and per-triangle winding repair.
DirectXMath's [actual camera implementation](https://github.com/microsoft/DirectXMath/blob/main/Inc/DirectXMathMatrix.inl)
uses cross(up,forward) for its LH screen-right axis.

Current projection limits: 128 bones, four influences, eight materials,
3,000,000 expanded vertices, 800,000 source transforms, 256 MiB payload,
32 MiB JSON. These are checkpoint constraints, not the requested final capacity.

Source clip IDs: idle, run_start, run_loop, run_stop, pivot_90_l, pivot_90_r,
pivot_180_l, pivot_180_r, turn_in_place_l, turn_in_place_r, shoot, multishot,
draw_charge, release_charge, arrow_rain_cast, roll_forward, retreat_shot, hit,
death, revive, victory.

## Deliberate remaining work

The current HSRPKG character decoder requires exactly seven chunks and five
ordered clips. Curves, markers, socket transforms, morph/correctives, multiple
LODs, secondary guides and 8-weight skinning are not part of that ABI. Full
runtime evolution must version named clip tables and extension chunks, preserve
monster compatibility, and update CPU/HLSL constant-buffer layouts together.
Do not extend Core's enum alone: HLSL still declares `MonsterClipMeta[6][5]`.

AssetPipeline action keys hash recipe version and content inputs/upstream
products, not executable implementation. Wiring this recipe requires a recipe
version bump for semantic changes, depfile entries for every consumed source,
and selected-Generation products/dependencies. Visual sources must not enter
simulation-rule hashing. Existing Archer texture packaging hardcodes five
materials, 2048 RGBA8/one mip and a 1-pixel first normal, so it also needs a real
manifest-driven update before game integration.

The tests exercise an asymmetric axis/winding fixture, all 21 source IDs,
five-slot projection, source metadata retention, actual package reader
roundtrip, and malformed inputs. Real Blender roundtrip and Windows/D3D12
validation remain distinct requirements.
