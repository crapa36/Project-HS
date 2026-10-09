# Project HS 엔진 최종 재구현 지시서 v7

대상: `crapa36/Project-HS` · 기준 커밋: `1354b1edb3939d480dab62c05ebb264d9d6e05d8` · 작성: 2026-09-27

## 0. 구현 목표와 범위

AAA급 스타일라이즈드 화면 품질, 실제 렌더 성능, 실행 중 그래픽 설정, 콘텐츠·재질·렌더 기능·하드웨어 확장성을 함께 만족하도록 현재 D3D12 렌더러와 관련 콘텐츠·설정 경로를 최종 구조로 교체한다. 이 문서는 구현 요구이며 현재 구현·벤치마크 결과가 아니다. v5·v6와 후속 합의를 대체하는 단일 설계 기준으로 사용한다. 아래 번호는 개발 단계가 아니라 구성요소 식별자다.

중간 렌더러·미연결 subsystem·이름만 등록된 옵션으로 범위를 축소하지 않는다. 기존 렌더러를 호환용으로 병행 유지하지 않는다. Indexed/Mesh Shader, VisibilityBuffer/CompactDeferred, raster/RT, atlas/virtual shadow는 완성된 엔진의 정식 실행 경로다. 전부 동시에 실행하는 것이 아니라 활성 profile이 필요한 경로만 선택한다. 구조 변경 비용을 이유로 최종 품질·성능을 제한하지 않는다.

| 구분 | 이번 완성 범위 |
|---|---|
| 장면·콘텐츠 | PersistentGpuScene, indexed/meshlet/LOD/HLOD 자산, 공유 SkinCache, texture/geometry/RT streaming, 실제 개선 재질·환경광·대표 장면 |
| 재질·확장 | MaterialDefinition→typed MaterialIR→shader variant, bindless materials, 공통 opacity/deformation/surface/BSDF, 기능별 subgraph 등록 |
| 실행 | FrameGraph DAG compiler, direct/compute/copy queue, 병렬 command recording, transient aliasing, 통합 residency, PSO cache |
| 가시성 | GPU instance/cluster culling, previous-HZB와 current-frame retest, VisibilityBuffer 및 CompactDeferred |
| 조명 | clustered lights, area lights, ProbeIBL, GTAO, SSR, cached virtual shadows, 공유 RT scene, RT shadow/RTAO/GI/reflection, BrixelizerGI |
| 표현·시간축 | 셀·젤리·피부·천·식생 재질, coat/anisotropy/sheen/subsurface/transmission, OIT·다층 hero refraction, particles, volumetrics, NativeTAA/InternalTAAU, DRS |
| 출력·사용성 | HDR 후처리, SDR/HDR10, output-resolution UI, FG·latency·pacing, 완전한 그래픽 메뉴·저장·복구 |
| SDK 통합 | NRD, DLSS SR/RR/FG, XeSS SR/FG·XeLL, FSR SR/FG/Ray Regeneration, DirectSR, BrixelizerGI |
| capability별 구현 | Mesh Shader, DXR, OMM/SER, raster VRS, Sampler Feedback, DirectStorage |

SDK와 capability별 경로도 실제 초기화·입력 생성·실행·해제 코드를 구현한다. `CompiledOut`은 명시적으로 SDK를 제외한 빌드, `Unsupported`는 기기·드라이버 제약, `Unavailable`은 배포물·초기화 문제다. 어느 것도 미구현을 완료로 표시하는 용도가 아니다. 사용하지 않는 provider는 불필요한 context/history/GPU 작업을 만들지 않는다.

ReSTIR, Work Graphs, Neural Texture Compression, Advanced Shader Delivery, 범용 노드 편집기, 다른 그래픽 API 백엔드는 이번 실행 기능에 추가하지 않는다. 확장은 아래의 실사용 계약으로 수용하며, 미래 기술명만 가진 placeholder나 범용 plugin framework를 만들지 않는다.

### 보존할 프로젝트 계약 [P]

Windows x64/C++23/HLSL, 60Hz Gameplay의 phase·입력·충돌·공격 판정·난수·체크섬·profile 의미를 유지한다. 그래픽 품질은 장식·표현 workload에만 영향을 준다. 1 unit=1m, 현재 캐릭터 비율·접지·애니메이션 타이밍, 슬라임의 불투명 눈/장식과 투명 몸체, 지형의 Gaussian/POM 의도, 필수 투사체·공격 예고를 이식한다. 비율·스타일 보존은 현재 재질·조명 품질을 상한으로 삼는다는 뜻이 아니다.

`simulation_rules.hsbin`과 `presentation_catalog.hsbin`의 역할 및 기존 오디오·키 바인딩을 보존한다. 원본은 `ContentSource/`, `Schemas/`, `Content/Shaders/`이며 cooked 결과를 원본 대신 수정하지 않는다. `HS_BUILD_RUNTIME=OFF`, `HS_BUILD_CONTENT_TOOLS=OFF`의 기존 저수준 빌드에 D3D12·SDK·FBX·DXC·font·새 도구 의존성을 추가하지 않는다.

## 1. 최종 모듈·소유권

기존 Core/Jobs/GameDomain/GameRules/Gameplay/Presentation/Runtime 구분을 유지한다. 공유 render asset·material 형식을 위해 SDK 독립 `hs_render_data`만 추가한다. 재질 컴파일러와 렌더러가 서로의 Private 구현을 참조하거나 특수 재질 구현을 Core에 넣지 않도록 하는 실제 공유 계층이다.

| 모듈 | 책임·허용 의존성 |
|---|---|
| Core | 기존 공통 기반, 단순 snapshot/settings/event/ID 계약. 상위 모듈 의존 없음 |
| RenderData **신규** | render package·MaterialIR·material schema·CPU asset validation. Core만 의존; D3D12/SDK/FBX/DXC/JSON parser 없음 |
| Presentation | Core+GameDomain. 도메인→논리 asset/pose/light, UI layout/interaction. GPU 자원·SDK를 모름 |
| RendererD3D12 | Core+RenderData. 내부 subsystem과 GPU/SDK 수명 소유 |
| Runtime | 기존 composition과 RenderData 사용. 저장·창·요청/응답·작업 실행 주입 |
| Apps/Content | 기존 의존성+RenderData. FBX/texture/material/scene/shader cook; 런타임 Private 헤더 사용 안 함 |

`hs_render_data`는 runtime/content가 필요한 구성에서만 빌드한다. Gameplay·GameRules·GameDomain·Jobs 의존성은 바꾸지 않는다. 새 edge를 CMake·`ARCHITECTURE.md`·`Tests/check_architecture.cmake`에 동일하게 반영한다. D3D12Renderer는 구체적인 composition root로 유지하며 `IRenderer`/backend factory는 추가하지 않는다.

### 내부 파일 배치

다음은 생성할 구현 파일의 기준 stem이다. `a,b`는 각각의 `.hpp/.cpp` 쌍이며 구현 없는 파일을 개수 맞추기로 만들지 않는다. 모든 경로는 `Source/RendererD3D12/Private/` 상대 경로다.

| 경로·파일 stem | 소유하는 상태와 주요 동작 |
|---|---|
| `Device/graphics_device, graphics_caps, command_context, pipeline_manager` | device/adapter/queue/fence, capability, worker별 list, shader/PSO cache |
| `Memory/gpu_allocator, descriptor_manager, upload_readback_ring, residency_manager` | allocation, handle generation, completion 기반 회수, 전체 budget |
| `Scene/render_scene_extractor, gpu_scene, render_assets, skin_cache` | snapshot 추출, static scene+dynamic instance, asset tables, current/previous deformation |
| `Materials/material_library, material_bindings` | cooked IR/variant/reflection, parameter/texture tables, material 변경 generation |
| `Graph/frame_graph, frame_graph_compiler, frame_graph_executor, render_feature_registry` | typed resources/signals, DAG·barrier·aliasing·queue 계획, feature 실제 등록 |
| `Visibility/gpu_culling, depth_pyramids, geometry_frontend, surface_resolve` | 후보·indirect commands·RasterDrawRecord, 두 frontend, surface/guide |
| `Lighting/light_system, probe_system, shadow_system, ao_system, gi_system, reflection_system, ray_tracing_scene` | light lists, probes, cache, signal producer, 공유 AS |
| `Temporal/view_history, temporal_inputs, exposure, dynamic_resolution` | View별 history/jitter/masks, current/next exposure, GPU 시간 기반 extent |
| `Reconstruction/reconstruction_system, internal_taau, provider_registry, guide_conversion` | 단일 reconstruction plan, 실제 TAAU, signal별 denoising, 외부 ABI 변환 |
| `Reconstruction/Providers/nrd, streamline, xess, fsr, directsr` | SR/RR/denoiser adapter. 같은 SDK의 전역 수명을 공유 |
| `Lighting/Providers/brixelizer_gi` | SDF scene 갱신, radiance cache, diffuse/specular 출력 |
| `Effects/transparency, particles, volumetrics, post_process` | 투명 layer, VFX state, froxel, HDR post |
| `Streaming/asset_io, streaming_manager` | page request/completion/cancel, file IO/DirectStorage, 공개·회수 |
| `Display/display_manager, frame_pacing, ui_renderer` | 창 결과·swapchain·HDR·UI·유일한 present/pacing 경로 |
| `Display/Providers/streamline_fg, xess_fg, fsr_fg` | FG별 swapchain·UI·latency·completion 계약; SDK 전역 객체는 위 adapter와 공유 |
| `Settings/graphics_profile, settings_apply_plan` | requested→resolved, compatibility, impact·capacity·commit/rollback |
| `Diagnostics/render_diagnostics, render_capture` | 동적 GPU/CPU timing, bounded counters, SDR/HDR capture·DRED |

상위 subsystem의 상호 호출을 무제한 허용하지 않는다. feature는 GPU Scene/Material/Temporal/Shadow의 공개된 **내부 서비스와 signal handle**을 소비한다. 한 feature가 다른 feature의 texture·PSO 멤버를 직접 변경하지 않는다. Graph/Device/Memory는 게임 객체나 특정 GI/SR 구현을 알지 못한다.

## 2. 공개 계약·CPU 실행

공유 값 형식은 Core, GPU 및 SDK 형식은 Renderer Private에 둔다. UI 지역화 문자열·layout은 Presentation 소유다. 다음 이름과 의미를 구현한다. 표의 자료형은 계약이며 padding·allocator 같은 내부 선택은 구현에 맡긴다.

| 형식 | 필수 필드·의미 |
|---|---|
| `RenderInstance` | `stable_id`, asset ID, material-set ID, transform/pose/material/content revision, transform, visibility/semantic flags. GPU slot과 게임 ID를 혼용하지 않음 |
| `RenderSceneRef` | scene asset ID, content generation, 선택적 불변 procedural-static instance 묶음의 lease. 정적 환경·probe·light·material 배치의 원본 |
| `RenderViewInput` | ViewId, camera/near/far, SceneOrigin/ViewOrigin, output extent, camera-cut flag, view role(main/preview/capture) |
| `RenderFrameInput` | snapshot ReadPair, views, presentation/VFX commands, UI model, sampled simulation tick, render delta, capture/debug request |
| `FrameIdentity` | simulation tick, RenderFrameId, ViewId, PresentationId, provider token. 각 증가 조건을 분리 |
| `GraphicsSettingsRequest` | 세대 번호, 변경 원인(user/system/recovery), 전체 불변 requested 설정 |
| `SettingsApplyResult` | generation, 상태, resolved/applied profile summary, fallback reasons, confirmation 필요 여부, error |
| `GraphicsCapabilitiesView` | GraphicsCaps/ProviderCaps/DisplayCaps 요약과 stable option ID·availability·사유 |
| `RenderTaskExecutor` | 좁은 task submit/join 서비스. Runtime이 `TaskSystem::Submit/Wait`를 감쌈; task capture는 join까지 유효 |
| `RendererDiagnostics` | frame/queue/pass별 값, rendered/displayed cadence, budget/usage, active providers·fallback |

`D3D12Renderer` public API는 `Initialize(RendererConfig)`, `RequestGraphicsSettings(request)`, `GetGraphicsSettingsStatus()`, `GetGraphicsCapabilities()`, `Render(RenderFrameInput)`, `Resize(OutputExtent)`, `GetDiagnostics()`, `Capture(CaptureRequest)`, `Shutdown()`로 정리한다. `RendererConfig`는 window handle, initial requested settings, validation, artifact path, task executor, SDK bootstrap 설정을 받는다. 고정 `kRenderPassCount`, 나열식 `RendererOptions`는 없앤다.

CPU 실행 소유권은 다음과 같다.

- Gameplay 스레드는 기존 fixed-step을 유지한다. Presentation은 모델을 투영할 뿐 렌더 설정으로 simulation 결정을 바꾸지 않는다.
- Render 스레드가 subsystem state·settings commit·graph submit을 소유한다. worker는 frame별 immutable inputs와 자기 command context로 record/prepare한다.
- UI/Runtime↔Render는 generation이 있는 불변 메시지로 교환한다. worker나 SDK callback에서 live UI/settings 객체를 수정하지 않는다. 상태 조회도 복사 또는 불변 snapshot이다.
- RenderSnapshot의 span을 소비하는 CPU 작업이 끝나기 전에 ReadPair lease를 반환하지 않는다. GPU에는 frame/GPU Scene 소유 메모리로 복사한다. static scene shared data도 generation별 lease로 보호한다.
- `PreviousRenderedState`는 GPU 제출에 성공한 실제 render frame에서만 갱신한다. skipped/aborted/FG-generated frame은 진행시키지 않고, 같은 simulation tick의 새 rendered frame은 진행시킨다. 부분 제출 뒤 중단된 frame은 사용 자원을 완료까지 유지하고 영향 history를 reset한다.
- 창 메시지·DPI·foreground 처리는 기존 window thread에 남긴다. 렌더러가 최종 카메라 pose/unjittered projection을 같은 generation의 view 결과로 공개하고 aiming과 화면 좌표 변환이 이를 사용한다. 렌더 jitter는 조준에 반영하지 않는다.

## 3. 좌표·색·surface 불변 계약

| 항목 | 정의 |
|---|---|
| 공간 | LH, Y-up, 1m. CPU DirectXMath row-vector, 공통 HLSL `row_major`와 `mul(vector,matrix)`. 기존 column-vector skin 행렬도 의미를 변환하여 통일 |
| depth | `R32_TYPELESS` 자원, `D32_FLOAT` DSV, `R32_FLOAT` SRV, reversed-Z, clear=0, GREATER_EQUAL, NDC z∈[0,1]. near/far/infinite 여부를 View에 명시 |
| position | depth를 생성한 jittered inverse VP로 camera-relative FP32 위치 복원. frame별 world origin은 별도 보존; 현재/이전 origin의 차이를 재투영에 반영 |
| motion | `RG16_FLOAT`, UV 좌상단 원점/+X 오른쪽/+Y 아래. `previous_unjittered_uv-current_unjittered_uv` |
| valid surface | visibility/depth 유효성으로 구분; clear pixel에서 position/normal을 정상 surface처럼 평가하지 않음 |
| normal | geometric와 shading normal 구분. compact는 oct `R16G16_SNORM`; SDK float3/packing은 별도 guide |
| roughness | 공통 material은 perceptual r, microfacet alpha=r². SDK의 `linear roughness` 등 명칭을 추정해서 그대로 전달하지 말고 해당 ABI 변환 적용 |
| 색 | scene-linear Rec.709, albedo/emissive 색 texture만 올바른 view로 한 번 sRGB decode. normal/ORM/height/Gaussian/LUT는 각 데이터 의미 유지 |
| exposure | `Cwork[n]=Cscene[n]×E[n]`; radiance history 재사용은 `E[n]/E[n-1]`. UI와 post bloom 제외 metering으로 다음 E 계산 |
| 화면 크기 | render/output/allocation-capacity/active-rect를 분리. dispatch·UV·UI·capture에서 한 `ScreenSize`를 공유하지 않음 |

```text
previous_jittered_uv = current_jittered_uv + motion
                    + previous_jitter_uv - current_jitter_uv
```

SceneOrigin은 GPU Scene/공유 TLAS의 공통 기준이며 View마다 바꾸지 않는다. ViewOrigin은 camera-relative 계산 기준이다. `P_scene=P_view_relative+(ViewOrigin-SceneOrigin)`으로 ray/lighting 조회 공간을 맞추고 previous frame의 두 origin도 보존한다. global position은 CPU의 double 또는 sector+local 좌표로 유지한다.

UV는 각 frame의 active render viewport에 대한 정규화 좌표다. texture allocation UV로 바꿀 때 각각의 active rect/extent를 적용한다. SDK motion-Z는 같은 surface의 이전/현재 deformation·view에서 `previous_linear_view_z-current_linear_view_z`로 생성한다. 이전 depth texture의 임의 샘플을 현재 surface의 이전 위치로 간주하지 않는다.

기존 Position.a의 Outline mask와 Normal.a의 재질 식별/roughness overload는 `MaterialId/SurfaceFlags/OutlineGroup`으로 옮긴다. RGBA32F world-position RT는 제거하되 FP16 absolute world position으로 되돌리지 않는다.

## 4. 자산·MaterialIR·shader 생성

### 4.1 공통 RenderData

생성 위치는 `Source/RenderData/Public/hs/render_data/{render_asset_format,material_ir,material_schema}.hpp`와 대응 `Private/` 구현이다. cooker와 renderer loader가 동일한 schema/version·range validation을 사용한다. CPU package 형식과 GPU struct 배치는 구분하며 GPU struct는 shared generated C++/HLSL layout과 reflection으로 연결한다.

`RenderPackageHeader`는 magic/version/endianness/asset ID/content hash, chunk directory 범위, dependency 목록을 가진다. chunk는 type/version/offset/압축 전후 크기/count/stride/hash/required-or-optional 정보를 갖는다. 미래 unknown optional chunk는 건너뛰고 unknown required chunk는 거부한다. count×stride, offset+size, decompress allocation을 checked arithmetic과 asset별 상한으로 검사한다. shader/material/mesh/texture dependency가 모두 준비된 generation만 공개한다.

| 자산 | 생산할 데이터 |
|---|---|
| mesh | indexed streams, submesh/material ranges, meshlets+primitive remap+bounds, authored LOD+cluster hierarchy+screen error, static HLOD, streaming page dependency |
| deformation | skeleton/skin, animation clip metadata, morph/procedural parameters, conservative animated bounds. 5개 기존 animation semantic과 grounding/upper-body blend 보존 |
| RT | 재구성 가능한 source/proxy geometry·LOD·material/alpha mapping. GPU·driver별 native BLAS를 범용 파일처럼 저장하지 않음 |
| texture | format, color/channel semantics, full mip chain, alpha coverage, min resident tail, page/tile info. BC7/BC5/BC4/HDR는 실제 채널 의미로 선택 |
| material | typed IR, parameter reflection/defaults, texture/sampler slots, shading model/closure mask, domain별 shader keys, opacity/deformation dependencies |
| scene/look | 정적 instance/material 배치, bounds, environment HDR, probe·light, decal/volume 배치, exposure·grade 설정 |

### 4.2 재질 컴파일 경로

`Source/Apps/Content/material_cooker.*`와 `Schemas/material.schema.json`을 추가한다. `ContentSource/Materials/*.json`의 `MaterialDefinition`은 `id,version,domain,shading_model,parameters,textures,nodes,outputs,render_flags`를 가진다. 노드는 `node_id,op,type,inputs,parameters`이며 참조가 있는 typed DAG다. supported type은 bool/uint/float/float2/3/4/texture-ref/sampler-ref와 surface closure다. cycle·type·stage 제약을 컴파일 시 거부한다.

실제 연산 범위는 constant/parameter, arithmetic/lerp/clamp, texture SampleGrad/SampleLevel, UV/world transform, tangent normal/RNM, Fresnel, procedural noise, POM, 기존 Gaussian terrain function, closure 조합이다. 검증되지 않은 임의 HLSL 문자열을 사용자 데이터로 실행하지 않는다. 엔진 소유 HLSL library function을 typed node로 등록해 확장한다. 런타임은 IR interpreter가 아니라 **생성된 특화 shader**를 실행한다.

컴파일은 constant folding·dead-code 제거·동일 expression 공유와 domain pruning을 수행한다. variant key=`IR hash + static switches + quality specialization + domain + geometry path + shader ABI/profile`. 연속 material 값은 GPU parameter table로 전달하며 값마다 PSO를 만들지 않는다. 실제 cook manifest가 사용하는 variant와 선택 profile의 shader만 생성·warmup한다.

공통 HLSL entry 계약:

```text
EvaluateDeformation(vertex, instance, current/previous time) -> DeformedVertex
EvaluateOpacity(MaterialContext) -> opacity, cutoff, coverage
EvaluateSurface(MaterialContext) -> SurfaceClosure
EvaluateBSDF / SampleBSDF / PdfBSDF(surface, light/view) -> matched evaluation
```

`MaterialContext`는 UV 및 dUVdx/dUVdy, position 및 dPdx/dPdy, normal/tangent, vertex color, stable world anchor, instance/material parameters, current/previous deformation 정보를 제공한다. ray-hit은 derivative 대신 ray footprint/cone로 texture LOD를 정한다. opacity와 vertex deformation은 visibility/depth/shadow/motion/ray-hit에서 같은 expression을 사용한다.

closure는 metallic-roughness GGX+diffuse를 바탕으로 coat, anisotropy, sheen/cloth, subsurface profile, foliage two-sided transmission, dielectric refraction/absorption, emissive/unlit를 실제 지원한다. 스타일 제어는 material별 ramp/rim/contrast/outline parameter로 표현하고 lighting 공통 코드의 mesh-ID 분기로 만들지 않는다. coat/base, reflection/transmission의 에너지를 조절하며 rough specular와 normal filtering으로 반짝임을 억제한다. 피부는 분리된 diffuse/subsurface profile, 천은 sheen·fuzz, 식생은 thin-sheet transmission, 젤리는 thickness/IOR/absorption+surface reflection을 사용한다. [S5]

공통 Surface는 논리 계약이다. 모든 closure 필드를 거대한 고정 GBuffer에 넣지 않는다. 기본 depth/normal/base/roughness/material ID와, 소비자가 요구한 tangent/coat/SSS/transmission/guide만 sidecar로 물리화한다. 새 closure는 MaterialIR/library·필요 surface 필드·cooker metadata를 확장하며 중앙 frame 실행을 수정하지 않는다.

### 4.3 기존 지형·자산의 실제 이식

`environment.hlsli`의 `TerrainSample/TerrainLayer/EnvTerrain/DecodeTerrainHistogram`, stochastic UV 회전·POM·mip별 inverse histogram·RNM·alpha LOD를 공통 재질 library로 옮긴다. 카메라 원점 이동에도 terrain tile/noise·바람 기준은 world에 고정한다. material별로 재배열한 compute lane 사이에서 ddx/ddy를 구하지 않고 **삼각형에서 복원한 명시적 gradient**를 전달한다. Gaussian texture를 일반 sRGB albedo로 바꾸지 않는다. POM은 shading relief이며 geometry silhouette/RT surface까지 변위한 것으로 취급하지 않는다.

기존 모델·animation source를 사용해 캐릭터·슬라임 3종·boss·나무·바위·잔디·지면을 모두 새 package/material 경로로 cook한다. 파일 형식만 바꾸지 말고 roughness/normal·texel density·alpha mip·근접 silhouette·LOD를 정비하고 environment HDR/probe/look를 제공한다. 모델의 접지·체형·전투 의미를 바꾸지 않는다. 대표 장면에는 실제 gameplay와 같은 자산·lighting 경로를 사용한다. 새 외부 자산을 쓸 경우 출처·재배포 조건은 asset manifest에 기록한다.

## 5. PersistentGpuScene·deformation·streaming

`Scene/render_scene_extractor`는 static scene asset과 동적 snapshot을 합쳐 create/update/destroy delta를 만든다. 동적 stable_id의 slot/generation과 transform/pose/material/content revision을 독립 추적한다. static 환경 배치는 매 tick 재생성하지 않는다. 기존 배치 알고리즘을 Presentation의 `environment_projector.*`로 분리해 seed·obstacle/layout revision 변경 때만 불변 static instance 묶음을 만들고 `RenderSceneRef`와 generation으로 전달한다. snapshot lease가 이 묶음의 수명도 보유한다. collision geometry는 시뮬레이션을 재작성하지 않고 기존 read-model 의미를 보존한다.

GPU table은 Instance/Transform/Mesh/Meshlet/ClusterLOD/Material/Light/Skeleton/Bounds/RTGeometry/Residency로 나눈다. CPU가 수정 구간을 병합해 DEFAULT heap에 업로드한다. 매 frame 전체 scene 재할당·전체 table 복사는 하지 않는다. in-flight frame이 읽는 version은 copy-on-write range 또는 queue dependency로 보호하고 GPU 사용이 끝나기 전 덮어쓰지 않는다.

`SkinCache`는 camera·shadow·RT 후보의 합집합을 대상으로 동일한 current/previous render pose를 평가한다. coarse candidate는 conservative animated bounds, 정밀 culling은 계산된 bounds를 사용한다. skeleton별 계층 평가/clip interpolation/upper-body blend/ground correction 후 vertex·normal·tangent를 생성한다. squash/stretch/shear normal은 inverse-transpose 의미를 유지한다. raster/MV/shadow/BLAS가 이 결과를 공유한다. clip frame 반올림으로 몬스터가 계단식으로 움직이지 않도록 연속 시간 보간을 구현하되 gameplay timing은 유지한다.

pose/asset/LOD/deformation key가 같은 결과는 공유하고 instance-dependent wind·morph는 구분한다. 이전 geometry가 없는 새 instance/LOD는 현재값을 초기 previous로 사용하고 history confidence=0으로 표시한다. previous buffer도 모든 consumer 완료 전 재사용하지 않는다. offscreen reflection/shadow에 필요한 변형을 main-view culling 결과만으로 제거하지 않는다.

`StreamingManager`의 상태는 Requested→Reading→Decoded→Uploading→Resident→Retiring이다. 각 요청에 asset/page ID·content generation·priority·completion을 둔다. 오래된 요청 completion은 폐기하며 upload fence 완료 후 GPU table과 descriptor를 같은 generation으로 공개한다. 요청에는 primary projected size뿐 아니라 shadow·GI·reflection·RT·필수 gameplay 표현의 수요를 합산한다.

항상 resident인 최저 mip와 완전한 geometry fallback을 제공한다. cluster hierarchy에서 부모·자식 중 resident한 일관된 cut을 선택하며 부모와 일부 자식이 중복/누락되지 않게 한다. texture 품질 감소는 sampler bias만 아니라 실제 resident finest mip/할당량을 낮춘다. 지원 기기는 Sampler Feedback와 tiled resources를 사용하고, 미지원은 software demand+실제 mip 범위 자원 교체로 동작한다. DirectStorage와 일반 async file IO는 같은 completion 계약을 사용한다. 압축 해제·staging·업로드도 budget에 포함한다.

## 6. GPU 가시성·두 geometry frontend

기하 실행 방식(Mesh Shader 또는 ExecuteIndirect+indexed)과 opaque frontend(VisibilityBuffer 또는 CompactDeferred)는 독립 축이다. 모든 조합은 동일 asset/material/surface 의미를 사용한다. 기본 profile은 GPU별 검증 자료가 없을 때 Indexed+CompactDeferred를 사용하고 두 첨단 경로의 구현 자체를 미루지 않는다. 검증 profile이 있으면 geometry+shading+guide+reconstruction 비용을 합산해 선택한다. CPU readback으로 visible draw 목록을 만들지 않는다. [S6]

### 가시성

GPU에서 instance frustum→LOD/cluster refinement→선택적 cone→occlusion→material/PSO bucket compaction→indirect argument를 생성한다. double-sided/변형으로 부정확한 normal cone은 cone cull하지 않는다. screen error는 output image 관점과 render scale을 함께 고려하고 LOD hysteresis/crossfade를 사용한다. count buffer와 append buffer overflow는 다음 frame capacity 조정과 안전한 보수적 대체 draw로 처리하며 조용히 필수 object를 버리지 않는다.

previous HZB로 보수적으로 cull한 뒤 살아남은 geometry를 그려 current HZB를 만든다. 이전 단계의 rejected candidates를 **current depth로 재검사**해 새로 보이는 geometry를 추가 raster하고 최종 depth hierarchy를 다시 export한다. previous HZB는 자신의 View/origin/extent로 투영한다. camera cut, near-plane 교차, 신규 object, 불확실한 bounds·빠른 occluder 변화는 visible로 처리한다.

reversed-Z occlusion pyramid는 영역의 가장 먼 depth를 나타내는 min-reduction이다. object의 conservative nearest reversed-Z가 영역 minimum보다 bias를 고려해 작을 때만 occluded다. background clear=0이 포함된 영역은 완전 가림으로 보지 않는다. HZB와 SSR/AO의 linear/minmax pyramid는 reduction semantic을 분리한다. alpha-blended object는 opaque HZB occluder로 사용하지 않는다.

### VisibilityBuffer

`R32G32_UINT`, invalid=`0xFFFFFFFF/0xFFFFFFFF`:

```text
x = RasterDrawRecordId
    -> InstanceSlot + InstanceGeneration
    -> MeshId + MeshLOD + GeometryGeneration
    -> index/vertex stream + material range + primitive remap + fade flags
y = 그 record 기준 primitive index
```

동일 객체의 두 LOD·submesh·meshlet이 동시에 보이더라도 삼각형 복원이 유일해야 한다. frame-local draw ID를 history identity로 사용하지 않는다. primitive correspondence가 보장되지 않는 LOD는 history를 국소 무효화한다. 두 geometry 실행 경로는 같은 primitive remap을 사용한다.

`SurfaceResolve`는 실제 raster sample 위치에서 triangle fetch·perspective-correct barycentric·analytic attribute gradients를 복원한다. near-clipped/degenerate triangle은 homogeneous geometry를 바르게 처리하고 임의 w clamp로 좌표를 왜곡하지 않는다. 재질별 tile/pixel classification과 indirect dispatch로 해당 variant를 평가한다. lane 재배열 뒤 implicit derivative는 금지한다. visibility/shadow의 alpha cutoff·two-sided·LOD fade는 동일 `EvaluateOpacity`를 사용한다.

### CompactDeferred와 guide

기본 저장은 base color, oct shading normal, roughness/metalness/occlusion/flags, material ID, depth, motion이다. emissive·tangent·SSS/coat·기하 normal 등은 필요할 때 sidecar를 만든다. VisibilityBuffer에도 동일 guide consumer를 제공한다. 실제 format/layout은 `SurfaceLayout`으로 버전 관리한다. material 평가를 각 lighting/SDK pass에서 반복하지 않고 필요한 결과만 materialize한다.

Visibility ID/depth/alpha/MV는 full-rate를 유지한다. raster VRS는 지원되는 후속 pixel shading에만 적용하며 compute material/lighting에는 효과가 있다고 표시하지 않는다. compute adaptive shading은 tile 분류·실제 sample density/해상도를 별도 제어하고 경계·얇은 geometry·고주파 재질에는 full rate를 사용한다.

## 7. FrameGraph·기능 확장

`FrameGraph`는 resource version 기반 compiler다. 코드 등록 순서를 실행 의미로 사용하지 않는다.

| 계약 | 내용 |
|---|---|
| `GraphResourceDesc` | type, format/stride, capacity extent, active rect, mip/array/plane/sample, usage, Managed/Imported, Transient/Persistent/History/Swapchain, initial/export access, release tokens |
| `ResourceUse` | handle+version, mip/slice/plane 또는 buffer range, pipeline stages, read/write/read-write, access type |
| `PassDesc` | stable PassId, allowed/preferred queues, uses, side effects/export, RecordCommand 또는 ExternalQueueOperation, execute callback |
| `CompletionSet` | direct/compute/copy fence 값과 SDK release tokens. 모든 조건 충족 시 재사용 |
| `SignalHandle` | FeatureId, semantic, view, version, extent/encoding/radiometric meaning와 실제 resource handle. 모든 신호를 texture로 강제하지 않음 |
| `FeatureRegistration` | FeatureId, settings/capability predicate, 요구·생산 signal, surface-guide mask, history 요구, subgraph 생성 함수 |

각 쓰기는 새 logical version을 만든다. compiler는 RAW/WAR/WAW, 초기화 여부, 범위 overlap을 분석해 DAG·culling·queue batch·barrier·allocation·timestamp 계획을 만든다. history/export/present/streaming/AS update 등 외부 효과가 있는 pass는 단순 color 미사용으로 제거하지 않는다. read-write load는 기존 version을 명시적으로 소비한다. 동일 signal의 producer 충돌, undefined read, cycle은 compile error다.

access는 ShaderRead/RT/DepthRead/DepthWrite/UAVRead/UAVWrite/CopyRead/CopyWrite/Indirect/ASBuildRead/ASBuildWrite/RayTracingRead/ShadingRateRead/Present를 구분한다. pixel/nonpixel shader 읽기, texture UAV, 같은 UAV 상태의 memory ordering, read-only DSV+SRV, AS build→trace를 표현한다. bindless read 자산은 immutable resource-set lease로 선언하고 write 자원은 개별 선언한다.

Enhanced와 Legacy barrier backend는 같은 access 모델을 변환한다. Enhanced는 sync/access/layout을 분리하며 queue 지원 범위를 지킨다. Legacy가 buffer range별 state를 제공하지 않으면 전체 buffer에 대한 보수적 state로 병합한다. aliased memory는 실제 필요한 memory ordering과 새 자원의 layout/discard/clear 초기화 후 사용한다. 같은 atlas의 사각형 영역은 독립 D3D12 subresource가 아님을 반영한다. [S2]

queue schedule은 dependency와 profile의 overlap 이득을 기준으로 결정한다. direct에서도 compute를 실행할 수 있다. queue 전환에는 양쪽의 signal/wait와 해당 자원의 access 전환을 넣는다. 서로 다른 queue의 시간이 겹친다는 이유로 lifetime이 끝났다고 보지 않는다. aliasing은 **전체 queue DAG와 in-flight frame/provider lease에서 이전 사용 완료가 다음 사용보다 앞선 경우**에만 허용한다.

worker×frame×queue별 allocator/list를 갖고 immutable batch를 병렬 record한다. submission을 소유한 thread가 graph 순서로 제출하며 shared list를 여러 worker가 기록하지 않는다. topology/format/provider 변경에만 해당 compiled plan을 다시 만든다. 일반 DRS는 capacity 내 active rect/dispatch만 변경한다. 최초 import/access와 frame별 외부 완료 상태는 재사용 plan에도 갱신한다.

### 외부 SDK

adapter는 요구 input state, output state, heap/root 변경, 허용 queue, 외부 submission, 사용 종료 token을 선언한다. command-list record형 SDK는 graph pass로, 자체 queue submission형 SDK는 **외부 queue operation**으로 등록한다. 선행 commands를 먼저 제출하고 SDK execute와 후행 fence/dependency를 연결한다. 반환 직후 자원을 폐기하거나 모든 SDK가 동일 list에 record한다고 가정하지 않는다. SDK state 변경 뒤 engine heap/root/state tracking을 다시 맞춘다. 필요한 legacy-state SDK 경계는 명시적으로 bridge한다.

새 feature는 자신이 소비·생산할 signal과 subgraph를 등록한다. 중앙 `Render()`는 feature별 if/switch를 계속 늘리지 않는다. 신호 배치가 필요 없는 단순 기능에 별도 가상 계층을 만들지 않는다. 런타임 동적 DLL plugin이 아니라 실제 구현된 내부 feature 등록 체계다.

## 8. Memory·descriptor·residency

D3D12MA를 유지하고 persistent DEFAULT heap과 transient placed resources를 관리한다. 정적 vertex/index/animation/material data는 DEFAULT로 옮긴다. UPLOAD는 frame/stream staging, READBACK은 지연된 diagnostics/capture에 사용한다. upload/readback ring range와 descriptor는 `CompletionSet`으로 회수한다.

CPU handle은 `(index,generation)`, GPU table은 유효한 heap index를 사용한다. shader-visible resource/sampler heap direct-indexing flags와 divergent index의 `NonUniformResourceIndex`를 적용한다. root ABI는 frame/view/scene table·feature parameters 중심으로 고정하고 material마다 늘리지 않는다. [S4] fallback white/black/flat normal/default material은 resident다. heap capacity 변경 시 기존 heap/index를 in-flight shader 앞에서 교체하지 않고 새 generation으로 전환한다.

`ResidencyManager`는 DXGI의 local/nonlocal budget·usage 및 변화 알림으로 texture/geometry/skin/RT-AS/shadow/GI/history/transient/SDK/staging을 함께 계산한다. 물리 VRAM 총량을 곧 사용 가능 budget으로 보지 않는다. 압박 시 저우선 streaming과 재생성 cache·높은 mip/LOD부터 줄이고, 진행 중 frame·SDK-pinned·최저 fallback·필수 시각 정보는 보호한다. [S3]

budget 예약은 category별 상한과 현재 critical working set을 반영한다. texture_budget는 사용자 상한 요청이며 실제값을 별도 표시한다. RT와 raster가 공유하는 page는 양쪽 완료를 기다린다. tiled mapping 변경은 read/write/AS 사용과 동기화한다. 반복적인 page 퇴출↔재요청을 막도록 hysteresis와 최소 유지 기간을 둔다.

설정 전환 peak는 `live old + candidate delta + in-flight/provider retained + staging`으로 계산한다. 공유 가능한 asset/context는 복제하지 않는다. 안전한 peak가 불가능하면 commit 전에 요청을 거부하고 현재 applied 상태를 유지한다. 설정 적용을 위해 기존 자원을 먼저 파괴하는 방식은 쓰지 않는다.

## 9. 조명·그림자·GI·RT

### 9.1 공통 lighting

광원은 stable LightId, type, transform, color, intensity/unit, range, shape, shadow flags, revision을 가진다. Directional/Point/Spot/Rectangle/Disk를 실제 평가한다. authoring 단위는 방향광 lux, point/spot luminous flux와 cone, area luminance를 명시하고 내부 radiometric 기준으로 일관되게 변환한다. 기존 조명은 look 보존용 변환 계수로 이식한다.

Point/Spot은 view-frustum clustered lists를 사용하고 overflow는 spill list로 처리한다. RT hit·probe·volumetric에는 카메라 cluster 밖의 world-position light query를 제공한다. area lights는 실제 면적·방향·발광 분포를 사용하고 diffuse/GGX는 LTC 기반 평가, 미지원 특수 closure는 동일 BSDF의 면적 sampling으로 평가한다. emissive는 surface emission과 GI 기여를 분리하며 무작위 과대광을 막도록 sampling/normalization을 한다.

논리 신호는 `DirectDiffuse,DirectSpecular,IndirectDiffuse,IndirectSpecular,Emission,Visibility,Transmittance`로 구분한다. 필요한 경우 confidence/variance/hit-distance를 추가한다. 최종 producer와 fallback을 profile에서 정하고 같은 specular/occlusion을 여러 기술에서 단순 가산하지 않는다. 물리 buffer 분리는 소비자와 pass fusion에 따라 결정한다. feature/BSDF별 sample의 PDF·hit 의미를 보존하여 새 sampling 기법이 연결될 수 있게 한다.

### 9.2 ProbeIBL·AO

HDR 환경광에서 diffuse irradiance/SH, roughness별 prefiltered specular, BRDF LUT를 cook한다. scene의 spatial probe는 influence bounds·visibility/occlusion·blend·box projection을 제공한다. 실제 환경 자산과 단위·색공간을 맞추며 analytic hemisphere만으로 IBL 구현을 대체하지 않는다. background/sky도 같은 environment radiance·orientation·exposure를 사용하고 별도 평면색으로 남기지 않는다. HDR의 sun과 명시적 방향광은 조명 기여가 중복되지 않게 분리/정규화한다.

`scene_cooker/probe_cooker`는 probe 위치·bounds·baseline 환경광을 생산한다. 정적 장면의 local diffuse와 visibility는 같은 재질을 사용하는 offscreen cube views 또는 지원되는 ray bake로 생성한다. procedural layout이 바뀌면 해당 probe generation을 무효화하고 예산 내에서 다시 갱신하며 준비 중에는 environment fallback을 사용한다. Probe 모드를 실시간 전체 GI라고 표시하지 않는다.

GTAO는 depth/normal 기반 horizon 검색, 품질별 half/full input, edge-aware resolve·temporal filter를 구현한다. RTAO는 짧은 visibility rays+맞는 denoiser로 구현한다. AO는 기본적으로 indirect visibility에 적용하고 RTGI/Brixelizer가 이미 계산한 occlusion과 중복 곱하지 않는다. contact 강조와 장거리 간접광 차폐는 별도 parameter다.

### 9.3 그림자

두 출력을 구분한다: `ShadowData`는 light별 matrix/clipmap/page/atlas/filter/RT 조회 정보, `OpaqueShadowVisibility`는 현재 opaque surface에 계산된 결과다. 투명 fragment·froxel·RT secondary hit는 자기 world position에서 ShadowData를 조회한다.

Directional은 texel-snapped clipmap과 page table을 가진 cached virtual shadow로 구현한다. physical bank의 page는 초기 inner 128×128 및 filter guard를 사용하고 budget으로 개수를 정한다. GPU receiver feedback→request dedup→resident/dirty 판정→page-caster cull→indirect render→page table publish를 수행한다. opaque뿐 아니라 visible transparent bounds·volumetric view ranges도 receiver 수요에 포함한다. 물리 atlas에는 page-local clip/projection과 guard를 적용해 인접 page로 raster/filter가 새지 않게 한다. bank write와 sampling은 graph에서 실제 resource 단위로 동기화한다.

정적 caster cache와 동적 caster 갱신을 분리한다. transform·LOD·skin/wind·alpha/material 변화와 **이전/현재 bounds 둘 다** dirty 영역에 반영한다. dynamic depth를 합성/조회할 때 static cache를 파괴하지 않는다. 새로운 page는 clear+render 완료 후 valid가 된다. page 부재·budget 제한·지원 profile에는 conventional cascaded/local atlas를 사용하며 근접 detail과 cascade/page 전환을 안정화한다. 기존 `base×5 × base×4` 고정 과대 할당은 제거한다.

local-light shadow는 중요도·screen contribution·movement에 따라 cached atlas/cube faces를 할당한다. RT shadow는 선별 light/surface의 contact/soft visibility를 제공한다. 동일 light의 raster/RT visibility를 대안·confidence blend로 결합한다. RT shadow denoising은 해당 light/signal 범위 안에서 수행한다. [S7]

### 9.4 공유 RayTracingScene

DXR master가 켜지고 필요한 consumer가 있을 때만 AS를 유지한다. static mesh/LOD BLAS는 build/compact, dynamic BLAS는 SkinCache current geometry로 update한다. topology/LOD 변경 또는 refit 품질 저하에서 rebuild한다. TLAS instance에는 material/geometry generation·mask·stable scene 식별을 연결한다. build scratch·instance buffers·compaction copy·trace use를 Graph에 선언한다.

RT 후보는 main camera culling과 별도로 trace 거리·shadow/reflection/GI consumer를 기준으로 정한다. 근거리 hero geometry와 deformation은 raster와 일치시키고, 원거리 proxy의 error를 명시한다. offscreen hit에 필요한 material/texture fallback도 resident하게 유지한다. ray-hit은 공통 BSDF/opacity와 ray footprint LOD를 사용한다.

OMM은 alpha-test의 bake classification/alpha cutoff/material generation에 적용한다. blended slime은 opacity micromap으로 불투명화하지 않고 transmission/absorption 의미로 처리한다. SER/OMM은 지원되는 별도 shader/profile variant에서만 사용하며 일반 DXR path를 유지한다. 기본 셰이더 최소 요구를 그 전용 profile까지 일괄 올리지 않는다. [S1]

### 9.5 GI·반사·radiance history

| 모드 | 실제 구현·출력 |
|---|---|
| Probe GI | cooked diffuse probes+환경광, depth/normal·probe visibility에 따른 누출 억제 |
| SoftwareHybrid GI | Brixelizer scene/SDF 갱신, 이전 lighting cache 입력, diffuse/specular 결과와 유효성. 변형 geometry·stale SDF의 한계는 proxy revision과 confidence로 처리 |
| RayTraced GI | visible surface에서 matched diffuse BSDF/PDF sampling, secondary hit의 direct/emissive+probe/cache radiance, noisy diffuse/hit distance/variance. trace 거리·표본·해상도 품질 제어 |
| SSR | depth hierarchy trace, roughness sampling, thickness tolerance·edge/offscreen confidence, temporal/spatial resolve |
| HybridRT reflection | SSR confidence가 부족한 표본을 DXR로 추적, hit에서 공통 재질/광원, miss는 environment/probe. SSR/RT/probe는 하나의 indirect specular로 결합 |

SSR은 같은 frame의 최종 SSR 색을 다시 읽지 않는다. `TraceRadiance`는 reflection 합성 전 direct+diffuse+emission 또는 의미가 정해진 history다. `CacheInjectionRadiance`는 DirectDiffuse/Emission과 의도적으로 재사용하는 clean diffuse indirect로 생성하고 UI/bloom/tone mapping/view-dependent specular를 제외한다. 이전 indirect로 multibounce를 구현할 때 exposure 보정·energy clamp·scene/light 변경 무효화를 적용한다. producer 없이 `PreviousRadiance`를 외부 입력처럼 남기지 않는다. [S8]

신호는 noisy/clean, demodulated/modulated, radiance/visibility와 hit distance 단위를 metadata로 구분한다. NRD/FSR용 packing과 material 재적용은 해당 adapter와 lighting composite 사이에 정확히 한 번만 수행한다. specular와 GI가 제공하는 같은 indirect 항을 중복 소비하지 않는다.

## 10. 투명체·subsurface·VFX·volumetrics

렌더 domain은 Opaque/Masked/Translucent/Additive/HeroRefractive/Decal로 분리한다. Masked는 공통 alpha와 coverage-preserving mips를 사용한다. 일반 Translucent는 weighted OIT, Additive는 HDR 가산층이다. 불투명 재질의 skin SSS는 diffuse 성분만 profile별 thickness/depth-aware diffusion하고 specular는 흐리지 않는다.

HeroRefractive는 bounded depth-layer 방식으로 구현한다. 픽셀별 nearest layer를 depth peel로 얻고 동일 instance의 front/back 또는 authored thickness로 광학 두께를 구한다. IOR/Fresnel·Beer–Lambert absorption·거친 표면 반사·저비용 내부 scattering을 사용한다. far→near로 배경을 합성하고 왜곡된 UV의 depth/coverage를 검사한다. offscreen/무효 screen sample은 probe 또는 지원되는 RT transmission으로 대체한다. layer 예산 초과는 peel된 앞 layer를 제외한 나머지 fragment만 OIT tail로 처리하며 초과 counter와 국소 temporal confidence를 제공한다. 임의 개수 굴절층의 정확한 해법이라고 간주하지 않는다.

기존 슬라임의 눈/장식은 opaque, 몸체는 material 품질에 따라 OIT 또는 HeroRefractive다. low path도 같은 base tint·표정·공격 charge·사망 의미를 유지한다. 투명 body를 OIT와 hero 양쪽에 중복 그리지 않는다. solid body thickness와 단순 alpha fade를 구분하고 단순 전체 alpha 감소로 고급 재질을 대신하지 않는다.

투명 depth/motion/coverage·refraction/animated-emissive classification을 opaque depth/MV와 별도 보존한다. 가장 앞선 layer 또는 opacity-weighted 대표 motion 선택은 SDK별 계약에 따른다. opaque guide를 무조건 덮어쓰지 않는다. hero·foliage·slime animation은 현재/이전 render deformation과 일치한다.

Particle update는 실제 시간으로 진행하며 FG로 다시 업데이트하지 않는다. spawn sequence/ID로 중복 명령을 제거하고 current/previous position·ribbon history를 motion에 사용하며 새 particle은 confidence=0으로 시작한다. 기존 alive/dead/counter/indirect 방식과 VFX 의미를 유지하고 renderer 종류·sprite·ribbon/mesh를 데이터화한다. essential/decorative priority를 분리해 예산·LOD·spawn drop은 decorative부터 적용한다. 폭발·화염·젤리·투사체가 같은 과도한 bloom으로 뭉개지지 않게 HDR energy/shape/soft-depth/lighting을 조절한다.

Volumetrics는 log-depth froxel에서 density·extinction/scattering·anisotropy·local/global volumes·light/shadow를 평가하고 깊이에 따른 누적 scattering L(z), transmittance T(z)를 만든다. opaque와 각 transparent layer는 자기 깊이 또는 layer 사이 구간을 사용해 합성한다. 이미 fogged color에 전체 fog를 다시 적용하지 않는다. reprojection은 camera/volume/light 변경과 depth 불일치를 거부하고 depth-aware upsample한다. froxel stride/Z-slices/거리·sample budget은 품질에 연결한다.

Decal은 bounds와 material IR을 가진 feature로 구현하고 공통 surface resolve 이후 조명 전에 필요한 base/normal/roughness/emissive를 수정한다. masked/normal blend와 receiver flags를 사용하고 RayTracingScene에는 중요한 decal의 재질 평가가 반영되거나 명시적으로 raster-only임을 표시한다. 효과 추가 때문에 중앙 geometry/lighting switch를 늘리지 않는다.

## 11. TemporalSystem·DRS·재구성

`ViewHistory`의 key는 ViewId/FeatureId/signal semantic/format/capacity/generation이다. TemporalSystem은 수명과 frame metadata, feature는 누적·필터 수식을 소유한다. provider 내부 history를 엔진 history와 이중 소유하지 않는다. main/preview/capture가 별도 ViewId를 가지며 GPU Scene/Assets는 공유한다.

camera cut·scene reload·provider/비호환 format 변경은 해당 history hard reset이다. teleport·spawn·material revision·LOD/geometry 교체는 해당 surface confidence를 낮춘다. pose/skin/foliage와 카메라 motion을 모두 포함하고 신규 surface에 오래된 slot history를 붙이지 않는다. DRS capacity 내 변경은 active rect·reprojection scale을 갱신하며 provider가 요구하는 reset만 수행한다.

NativeTAA/InternalTAAU는 centered jitter, unjittered MV reprojection, depth/normal/material-generation rejection, neighborhood clipping, history confidence/lock, responsive mask, anti-ghosting을 실제 구현한다. TAAU는 output extent에서 filter·history를 유지하고 native/downsample도 같은 경로에서 처리한다. sky는 카메라 회전 motion, 투명체는 별도 mask/motion 정책을 사용한다. 얇은 식생·LOD dither·움직이는 emissive를 정지 물체로 처리하지 않는다. 마지막 sharpening은 provider 자체 sharpening과 합쳐 한 번만 적용한다. InternalTAAU의 ProviderQuality 모드는 Native=100%, Quality=2/3, Balanced=1/1.7, Performance=1/2의 output 대비 선형 해상도로 정의한다. NativeTAA는 Native만 제공한다. Manual에서는 품질 비율을 다시 곱하지 않고 요청 scale을 사용한다.

DRS는 generated frame을 제외한 GPU render critical-path sample을 사용한다. smoothing·hysteresis·변화율 제한, CPU 병목/idle/present/limiter wait 제외, provider min/max·정렬·feature 최소크기·메모리 capacity의 교집합을 적용한다. 작은 extent 변경으로 매 frame texture/context를 생성하지 않는다. Manual scale과 provider 권장 scale을 중복 곱하지 않는다. 변경한 active extent를 모든 guide·SDK·post 입력에 동일하게 전달한다.

### reconstruction의 단일 출력

| 선택된 profile | 실행 경로 |
|---|---|
| Raster | opaque+transparency/volume SceneHDR → NativeTAA/InternalTAAU 또는 선택 SR → DisplayHDR |
| Standalone RT | noisy signals → signal별 NRD 또는 FSR Ray Regeneration → material 재적용/lighting 합성 → transparency/volume → 선택 SR/TAAU → DisplayHDR |
| Fused DLSS RR | noisy color+DLSS RR guides 및 transparency 계약 → DLSS RR → DisplayHDR. 같은 신호의 standalone denoise와 별도 DLSS SR/TAA를 대체 |

신호마다 denoiser가 하나, View마다 최종 reconstruction producer가 하나다. shadow는 ShadowSystem의 별도 signal-specific 처리이며 fused RR이 처리하지 않는 항까지 무조건 끄지 않는다. RR 입력으로 불완전한 noise/guide를 넣고 Available이라고 표시하지 않는다.

### Provider 계약과 실제 구현 [S7–S12]

공통 adapter는 `QuerySupport/DescribeInputs/ResolveModeAndExtent/CreateOrReconfigure/BuildPasses/CollectCompletion/Shutdown`의 의미를 제공한다. 반환 계약에는 required/optional guides, radiance/노출/roughness/normal/depth/MV encoding, render/output extents, active rect/DRS/reset, queue/외부 submission, retained resources, produces-display-resolution, 호환 SR/FG/latency를 넣는다. 기능에 맞지 않는 하나의 거대 가상 인터페이스 대신 SR·signal-denoiser·fused-RR·present provider 역할을 구분한다.

| Adapter | 필수 연결과 차이 |
|---|---|
| NRD | REBLUR/RELAX를 diffuse/specular, occlusion variant를 AO, SIGMA를 light shadow에 맞게 선택. view-Z/normal/roughness/MV/hit-distance 및 demodulation은 포함된 frontend/backend helper와 일치. source/clean semantic을 보존 |
| DLSS SR | Streamline의 frame token/constants, depth/MV/jitter/exposure, input/output extent, quality query, resource tag·evaluate·수명. 별도 engine TAA를 선행 실행하지 않음 |
| DLSS RR | diffuse/specular albedo, 정상 float normal/roughness, noisy color, depth/MV, specular-hit-distance 또는 specular MV, camera/exposure. RR가 SR output도 생산하므로 별도 DLSS SR 중복 금지 |
| XeSS SR | 실제 SDK quality·optimal input size·velocity convention·responsive mask·exposure·context/pipeline 준비를 연결. XeSS라는 이름만으로 모든 GPU의 같은 path를 가정하지 않음 |
| FSR SR | 선택한 API/runtime variant의 depth/MV/jitter·reactive/composition·exposure·frame-time·extent/reset을 연결. ML/non-ML 구현 availability는 runtime query 결과 사용 |
| FSR Ray Regeneration | **독립 signal denoiser**, SR 포함 fused provider 아님. signed linear view-Z, XY motion+previous-current view-Z delta, SDK oct-normal/roughness/material packing. albedo의 linear/sqrt encoding은 flag와 일치시킴 |
| DirectSR | variant GUID·권장 extent/jitter·input flags를 질의. 자체 queue상의 비동기 Execute를 ExternalQueueOperation으로 연결하고 후행 fence까지 자원 유지. direct vendor SR과 동시에 같은 작업 실행 안 함 |
| BrixelizerGI | static/dynamic geometry revision→SDF updates, previous lit input·history depth/normal·MV·environment, diffuse/specular 출력. camera cut/origin 변경·budget·생성/삭제를 scene과 동기화 |

위 표는 내부 의미 계약이다. 외부 ABI의 정확한 함수·enum·struct는 §17에서 잠근 release의 header/sample을 사용한다. 버전이 다른 ABI를 가짜 wrapper 함수로 추정하지 않는다. `GuideConversion`은 필요한 변환만 graph에 넣고, 두 consumer가 같은 변환 결과를 요구하면 공유한다. denoiser가 없으면 해당 noisy RT mode를 지원되는 clean fallback으로 resolve하며 raw noise를 완성 영상으로 노출하지 않는다.

## 12. HDR 후처리·UI·Display·FG

scene metering은 bloom/UI 이전에 별도 실행하여 E[next]를 export한다. 현재 프레임에는 E[current]만 사용한다. 재구성된 display-resolution HDR에 optional motion blur/DoF, bloom, scene grade, outline, display transform, UI 합성, output encoding을 적용한다. feature가 Off이면 그 pass와 고유 transient/history를 제거한다.

Bloom은 soft-knee prefilter→half-resolution부터 mip downsample→progressive upsample→HDR 합성이다. motion blur는 render-frame velocity와 shutter·depth rejection을 사용한다. DoF는 near/far CoC를 분리하며 기본 Off다. color grade는 HDR log shaper+3D LUT, outline은 output-pixel thickness와 reconstructed depth/normal/OutlineGroup을 사용한다. 이미 tone-mapped color를 scene-linear라고 취급하지 않는다. output-depth/MV/normal은 필요한 consumer에만 edge-aware resolve한다.

SDR은 display-linear Rec.709 tone mapping→linear premultiplied UI→sRGB encode 한 번이다. HDR10은 luminance 기준 mapping→paper-white UI→Rec.2020 gamut mapping/PQ encode, `R10G10B10A2_UNORM` 및 대응 DXGI color space를 사용한다. HDR metadata·지원 색공간·monitor 이동은 실제 output에서 갱신하고 미지원은 SDR로 resolve한다. peak/paper-white 보정은 항상 실제값으로 표시한다. [S13]

UI는 logical layout과 output extent/DPI/letterbox transform을 분리한다. CPU D2D/WIC 기반 기존 UI를 사용할 경우 dirty surface/tile만 다시 raster/upload하고 sRGB/premultiplied alpha를 정확히 linear premultiplied `RGBA16F UIColorAlpha`로 변환한다. font 원본·측정·clipping 의미를 보존한다. 설정 UI의 렌더와 hit-test는 같은 layout 결과를 사용한다. UI shader에서 새로 별도의 aspect 보정을 중복하지 않는다.

DisplayManager가 swapchain·present·FG proxy를 단일 소유한다. 창 모드는 Windowed/BorderlessFullscreen이다. borderless의 실제 output은 선택 monitor desktop/client 크기이며 내부 렌더 해상도와 구분한다. flip-discard, 지원 tearing flags·sync interval을 조합하고 tearing 지원을 실제 VRR 활성 상태로 표시하지 않는다.

| FG provider | 실제 완료해야 할 경로 |
|---|---|
| DLSS FG | Streamline FG 설정·frame token·HUD-less/UI/depth/MV tagging, Reflex 연동, supported Fixed/Dynamic multiplier, present 결과와 retained resource 해제 |
| XeSS FG | XeSS FG swapchain wrapper·resource tagging·constants, XeLL context/sleep/markers, supported multiplier·실제 결과, resize/HDR/context 수명 |
| FSR FG | SDK interpolation context·swapchain/present callbacks·dispatch, HUD-less/UI·MV/depth·pacing, 지원 multiplier 및 실제 수명 |
| Off | engine flip-discard present, 지원 waitable swapchain과 단일 engine limiter |

SDK 전역 상태는 SR adapter와 공유하되 활성 present owner는 하나다. Streamline/XeSS/FSR proxy를 임의로 중첩하지 않는다. native↔proxy 변환은 adapter 경계에 한정한다. SDK가 요구하는 HUD-less/UI format·alpha·색공간으로 변환하여 넘기고 UI를 generated color에 두 번 합성하지 않는다. [S9,S10,S11]

사용자의 `frame_limit_fps`는 displayed FPS, `target_render_fps`는 DRS의 실제 render 목표다. provider mode에 맞춰 limiter 단위를 변환하고 engine/provider 두 limiter를 동시에 돌리지 않는다. vendor latency sleep/markers는 실제 input sample·render submit·present와 연결한다. 독립 60Hz simulation을 FG cadence에 맞춰 가짜로 진행시키지 않는다. generated frame은 RenderFrameId·jitter·pose history를 증가시키지 않는다.

provider가 displayed frame 수/시간을 반환하지 않으면 render FPS×요청 배수를 측정 displayed FPS로 표시하지 않는다. latency도 소프트웨어 pipeline latency와 실제 hardware end-to-end 측정을 구분한다. 매우 낮은 base cadence에서는 Auto FG를 낮추거나 Off로 resolve하며 threshold는 profile 데이터다.

resize/HDR/FG 교체는 StopAccepting→FG stop/drain→관련 resource 완료→swapchain/context 교체→재개로 처리한다. 이는 구현 단계가 아니라 런타임 상태 머신이다. 최소화/occlusion 시 유효하지 않은 extent로 context를 만들지 않고 rendering을 정지/완화한다. device removal은 DRED 기록과 제어된 실패/재초기화 경로로 보내며 단순 옵션 거부와 혼동하지 않는다.

## 13. 그래픽 설정: 단일 정의와 실제 효과

`Source/Core/Public/hs/core/graphics_settings.def`를 설정 ID·필드·기본값·범위·group·label key의 단일 정의로 만든다. `.hpp/.cpp`의 타입·값 검증과 Runtime JSON·Presentation descriptor는 이를 참조한다. renderer만 필요한 동적 availability/impact/resource resolve는 같은 SettingId로 연결한다. Core에 지역화 문구·D3D12·SDK logic을 넣지 않는다. low-level build용 새 code generator 의존성을 만들지 않는다.

SettingsData schema=2는 `schema_version, graphics, 기존 master/bgm/sfx/ui volume, skill_virtual_keys`를 가진다. ID/provider/mode는 stable symbolic token으로 저장하고 registry의 배열 index로 저장하지 않는다. 요청 메시지는 전체 불변 값이며 serialized 원문에서 인식 못한 optional token도 손상시키지 않는다.

표에서 `Q=Low/Medium/High/Ultra`, `OQ=Off+Q`, `C=Constants`, `V=SceneVisibility`, `SC=ShadowCache`, `SB=StreamingBudget`, `G=GraphTopology`, `RC=ResourceCapacity`, `H=History`, `PC=ProviderContext`, `SW=Swapchain`이다. impact는 합집합이며 표는 최소 영향이다. quality parameter가 capacity/variant까지 바꾸면 추가 영향도 계산한다.

| 키 | 타입·초기값·허용 범위 | 실제 연결·impact |
|---|---|---|
| `preset` | Low/Medium/High/Ultra/UltraRT/Custom; Medium | §14 preset-owned 값 묶음 적용 |
| `display_mode` | Windowed/BorderlessFullscreen; BorderlessFullscreen | window/display 변경; SW |
| `display_output` | DisplayId; Auto | 실제 monitor 선택; SW/RC/H |
| `output_extent` | extent 또는 Auto; Auto | Windowed client size. Borderless는 desktop effective extent; SW/RC/H/PC |
| `vsync` | bool; true | 단일 present owner sync interval; C/PC |
| `frame_limit_fps` | uint; 0; 0 또는 1…1000 | displayed limiter; C/PC |
| `hdr_mode` | Off/On/Auto; Auto | output format/color space; SW/G/RC/PC |
| `hdr_paper_white_nits` | float; 200; 80…400 | display/UI white; C |
| `hdr_peak_nits` | Auto 또는 float; Auto; 80…10000 | tone/gamut mapping; effective≥paper white; C |
| `reconstruction_provider` | ProviderId; Auto | NativeTAA/InternalTAAU/DLSS/XeSS/FSR/DirectSR 중 Available; G/RC/PC/H |
| `reconstruction_quality` | provider ModeId; Quality | query-derived input extent/quality; RC/PC/H |
| `resolution_mode` | Manual/ProviderQuality; ProviderQuality | render extent 결정권; RC/PC/H |
| `manual_scale_percent` | float; 100; 50…200 | SDK 허용 범위와 교집합, Native=100. 초과/임의 scale은 InternalTAAU; RC/H |
| `dynamic_resolution` | bool; false | DRS 활성; C/RC/PC |
| `target_render_fps` | uint; 60; 30…240 | 실제 GPU render budget; C |
| `dynamic_min_percent` | float; 50; 25…100 | DRS 최소 extent; C/RC/PC |
| `dynamic_max_percent` | float; 100; 25…100 | DRS 최대 extent; min≤max, provider 제약; C/RC/PC |
| `sharpening` | float; 0.2; 0…1 | reconstruction 후 단일 sharpen; C |
| `frame_generation_provider` | ProviderId; Auto | Off/DLSS/XeSS/FSR availability; G/PC/SW/RC |
| `frame_generation_mode` | Off/Auto/Fixed/Dynamic; Off | 선택 provider supported mode; PC/SW/RC |
| `frame_generation_multiplier` | uint; 2 | 실제 supported multipliers에서 열거; PC/RC |
| `low_latency` | Off/On/Auto; Auto | 선택 FG의 필수 latency pair 우선; PC/C |
| `texture_quality` | Q; Medium | finest resident mip·detail; SB |
| `texture_budget_mib` | uint; 0=Auto | OS/global budget 내 texture 상한; SB |
| `anisotropic_filtering` | 1/2/4/8/16; 16 | 실제 sampler descriptor; C |
| `geometry_quality` | Q; Medium | cluster error·LOD/HLOD·stream 수요; V/SB/H |
| `foliage_quality` | Q; Medium | 장식 density/distance·LOD·shadow 후보; V/SB/SC/H |
| `material_quality` | Q; Medium | closure 특화·normal/detail/SSS sample; G/H/RC |
| `pom_quality` | OQ; Medium | POM steps/distance, Off는 normal/detail 유지; C/H |
| `lighting_quality` | Q; Medium | area sample·decorative local shadow 예산; C/SC/RC |
| `shadow_mode` | Raster/HybridRT; Raster | virtual/atlas와 선택 RT visibility; G/PC/SC/H |
| `shadow_quality` | Q; Medium | texel density·physical pages·filter; SC/RC/H |
| `shadow_distance_m` | float; 160; 20…500 | receiver/caster/RT 수요; V/SC/SB/H |
| `contact_shadows` | bool; true | screen/RT contact subgraph; G/H |
| `ao_mode` | Off/Auto/GTAO/RTAO; GTAO | indirect occlusion producer; G/PC/RC/H |
| `ao_quality` | Q; Medium | AO extent/samples/filter; C/RC/H |
| `gi_mode` | Off/Probe/SoftwareHybrid/RayTraced; Probe | indirect diffuse producer; G/PC/SB/RC/H |
| `gi_quality` | Q; Medium | probe/SDF/cache/ray/extent budget; C/RC/SB/H |
| `reflection_mode` | Off/Probe/ScreenSpace/HybridRT; ScreenSpace | indirect specular source; G/PC/RC/H |
| `reflection_quality` | Q; Medium | SSR/RT distribution·steps·filter; C/RC/H |
| `ray_tracing_enabled` | bool; false | RT consumer/AS의 master gate; G/V/SB/PC/H |
| `rt_trace_distance_m` | float; 60; 5…500 | RT 후보·geometry residency·AS/history; V/SB/H |
| `neural_ray_reconstruction` | Off/Auto/On; Auto | conventional/neural 선택 요청; G/PC/H |
| `ray_reconstruction_provider` | ProviderId; Auto | NRD/FSR Ray Regeneration/DLSS RR의 실제 availability; G/PC/RC/H |
| `transparency_quality` | Q; Medium | OIT/hero 대상·layer 해상도·material 비용; G/RC/H |
| `refraction_quality` | OQ; Medium | thickness/trace/layer budget; G/RC/H |
| `volumetric_quality` | OQ; Medium | froxel XYZ/거리/sample/history; G/RC/H |
| `particle_quality` | Q; Medium | decorative spawn/count/LOD/예산; V/RC |
| `bloom` | bool; true | bloom subgraph; G |
| `bloom_strength` | float; 1; 0…2 | HDR bloom 합성; C |
| `motion_blur` | Off/Low/High; Off | velocity gather/shutter; G/RC |
| `depth_of_field` | bool; false | camera focus/CoC subgraph; G/RC |
| `outline` | bool; true | output edge subgraph; G |
| `outline_width_px` | float; 1; 0…4 | output pixel 두께; C |
| `color_grading` | bool; true | scene look LUT; display transform은 유지; G |
| `adaptive_shading` | Off/Auto/On; Auto | 유효 raster VRS/compute adaptive 각각 resolve; G/C |

표의 provider/mode는 문자열만 표시하는 기능이 아니다. 실제 shader·resource·dispatch/present 변경과 연결한다. 존재하지 않는 옵션은 Available이 아니다. 모드별 `Off`는 **추가 효과 Off**로 정의한다: GI Off도 기본 environment ambient 유지, Reflection Off도 기본 material/environment response 유지, Refraction Off도 투명 body·전투 의미 유지, grading Off도 display 색 변환 유지.

읽기 전용 상태에는 applied settings, fallback 사유, 지원/비지원, monitor refresh/HDR, render/output extent, actual render/display FPS, effective FG multiplier, VRAM budget/usage/전환 peak를 표시한다. 하나의 Q가 모든 SDK에 동일한 sample 수를 뜻하지 않으며 provider별 실제 mode table로 변환한다.

## 14. 프리셋·적용·저장

### 품질 데이터

`ContentSource/Graphics/quality_profiles.json`과 schema를 추가해 아래 초기값을 둔다. 숫자는 **시작용 설계값이지 측정된 최적값·성능 보장값이 아니다**. renderer의 resource planning이 global budget과 provider 조건으로 실효값을 계산한다.

| parameter | Low | Medium | High | Ultra |
|---|---:|---:|---:|---:|
| geometry screen error(output px) | 4 | 2 | 1 | 0.5 |
| decorative foliage density | 0.35 | 0.6 | 0.8 | 1 |
| finest texture mip offset | 2 | 1 | 0 | 0 |
| decorative particle fraction | 0.5 | 0.75 | 1 | 1 |
| shadow cache 요청 MiB | 32 | 64 | 128 | 256 |
| shadow filter samples | 4 | 8 | 12 | 16 |
| AO 해상도 축 비율 | 0.5 | 0.5 | 1 | 1 |
| SSR 최대 steps | 16 | 32 | 48 | 64 |
| RT diffuse/specular samples per active pixel | 1 | 1 | 1 | 2 |
| froxel pixel stride / Z slices | 16/32 | 12/48 | 8/64 | 8/96 |
| hero refraction surface layers | 0 | 1 | 2 | 4 |
| POM 최대 steps(Off=0) | 8 | 12 | 24 | 32 |

density·particle 감소는 essential 범위에는 적용하지 않는다. refraction layer=0은 OIT body 경로다. 높은 layer·cache 품질은 transition peak까지 예약할 수 있을 때만 활성화한다. screen-error·material/ray 품질은 변경 가능한 데이터이며 code 상수와 중복 저장하지 않는다.

| preset | Q fields | AO | GI | Reflection | Shadow | Volumetric | RT master |
|---|---|---|---|---|---|---|---|
| Low | Low | GTAO | Probe | Probe | Raster | Off | Off |
| Medium | Medium | GTAO | Probe | ScreenSpace | Raster | Medium | Off |
| High | High | GTAO | Probe | ScreenSpace | Raster | High | Off |
| Ultra | Ultra | GTAO | SoftwareHybrid | ScreenSpace | Raster | Ultra | Off |
| UltraRT | Ultra | Auto | RayTraced | HybridRT | HybridRT | Ultra | On |

preset-owned는 texture/geometry/foliage/material/POM/lighting/shadow/AO/GI/reflection/transparency/refraction/volumetric/particle의 품질과 위 표의 mode·RT master다. POM/refraction은 해당 Q로 설정한다. display/HDR 보정/FPS/SR/DRS/FG/latency/sharpening과 bloom·blur·DoF·outline·grading의 개인 선호는 덮어쓰지 않는다. preset-owned 값을 직접 바꾸면 Custom이다. SDK fallback 때문에 requested preset을 Custom으로 바꾸지는 않는다.

### 상태·resolve

`pending`은 UI 편집값, `requested`는 적용/저장하려는 의도, `resolved`는 capability·호환성·예산을 반영한 실행계획, `applied`는 실제 사용 중인 값, `last_known_good`는 복구 가능한 확정 상태다. `ResolvedRenderProfile`에 frontend/geometry path, features/quality, material variants, guide layout, reconstruction/FG/latency pair, extent, memory reservation, graph topology hash, fallback 사유를 넣는다.

resolve는 값 검증→기기/monitor 지원→provider chain 호환→extent/quality→resource/transition peak→impact 순으로 수행한다. explicit provider 요청을 Auto 선호로 조용히 바꾸지 말고 effective 대체와 사유를 표시한다. Auto는 검증된 adapter profile을 우선하며 자료가 없으면 InternalTAAU·일반 raster·안전한 budget으로 결정한다. UI는 요청과 실효값을 함께 보여준다.

RT master Off/미지원은 RTAO→GTAO, RTGI→SoftwareHybrid 또는 Probe, RT reflection→SSR, RT shadow→Raster다. SoftwareGI 미지원→Probe, SR 미지원→InternalTAAU, neural RR 충돌→NRD+선택 compatible SR, FG 충돌→Off, HDR 미지원→SDR이다. required guide/denoiser가 없는 효과는 그에 맞는 clean fallback으로 함께 resolve한다. material/scene/profile의 중요한 의미를 보존하면서 실제 active feature를 보고한다.

### 적용 transaction

```text
Editing → Requested(generation) → Preparing → ReadyToCommit
        → Applied 또는 AwaitingDisplayConfirmation → Confirmed → Persist
실패/취소/미확인 → 이전 applied/last_known_good 복원
```

Render는 generation이 있는 요청을 합치고 superseded preparation을 취소한다. 준비한 CPU 데이터·PSO·delta resources를 safe frame boundary에서 교체한 뒤 이전 자원은 CompletionSet으로 retire한다. config 값을 먼저 바꾸거나 새 allocation 실패 전에 old targets를 해제하지 않는다. 영향을 받는 history만 reset한다. display/FG는 §12 상태 머신과 window-thread acknowledgment까지 완료해야 적용 성공이다.

화면 변경은 15초 확인 후 확정한다. timeout/cancel/실패는 원래 window/HDR/provider/profile을 함께 복원한다. pending presentation 설정 마커를 같은 저장 영역에 두어 실행 중 crash 후 다음 시작에서 미확정 화면 상태를 복원한다. 장치 제거는 별도 오류다. 저장 실패는 게임 종료가 아니라 적용됨/저장 실패 상태로 표시한다.

미지원 fallback은 requested를 덮어쓰지 않는다. 성공하고 필요한 display 확인이 끝난 **requested 의도**만 저장한다. Apply/Cancel/Defaults·category/scroll·availability 사유를 main/pause 메뉴 양쪽에 연결한다. 입력/렌더 layout이 같은 결과를 사용하고 apply pending 동안 오래된 응답이 UI를 되돌리지 않게 한다.

### migration

v1의 기존 16개 키를 읽는다. width/height/borderless/vsync/frame_cap→대응 display 값, render_scale 75/100→Manual+InternalTAAU, shadow 1024/2048→Medium/High, particle 50/100→Low/High, bloom/outline→동일 값이다. FG 초기 Off로 frame cap 의미를 유지한다. migrated 출력 모드가 client/desktop 조건과 다르면 requested 크기는 보존하고 effective 크기를 표시한다. audio와 skill keys는 원값을 보존한다. 변경된 preset-owned 조합이 프리셋과 일치하지 않으면 Custom이다.

기존 크기·유한수·범위 검증, backup·temporary write/flush/replace를 유지한다. 새 schema의 optional 누락은 default, 알 수 없는 미래 schema는 손상 파일과 구분하여 원본/backup을 덮어쓰지 않는다. duplicate/잘못된 타입·과도한 크기는 거부한다. CLI→saved→default 우선순위를 단일 resolve로 통합하고 `--safe-graphics`는 기본 SDR/FG-Off 안전 실행만 적용하며 명시적으로 저장하기 전 사용자 의도를 덮어쓰지 않는다.

## 15. 최종 frame 데이터 흐름

아래는 의존관계다. 실제 queue/batch는 Graph가 결정한다. 각 행은 필요한 내부 pass로 펼쳐지며 별도의 고정 pass-count 배열을 만들지 않는다.

| Subgraph | 입력 | 출력 |
|---|---|---|
| SceneUpload | SceneDelta, streaming completions | 현재 GpuScene/table generation |
| CoarseVisibility | GpuScene, View, PreviousHZB, light/trace ranges | view/shadow/RT candidates |
| SkinCache | candidates, current/previous pose | current/previous skin, deformed bounds |
| ParticleUpdate | spawn/events, previous particle state, render time | current particle/indirect state |
| Geometry | candidates, skin, GpuScene, View, PreviousHZB | depth, visibility/GBuffer, MV, rejected list |
| VisibilityRetest | current depth pyramid, rejected list, scene/skin | final depth/visibility/MV |
| DepthPyramids | final depth | occlusion HZB, linear/minmax effect pyramids |
| SurfaceResolve/Decals | final visibility/GBuffer, scene/material/skin, View | Surface, requested guides/classification |
| LightLists | Lights, View/depth bounds, world light index | view clusters + arbitrary-world-position query |
| RayTracingScene | RT candidates, current skin, scene/residency | shared BLAS/TLAS |
| ShadowSubgraph | receivers(surface/transparent/volume), caster/skin, Lights, cache, optional TLAS | ShadowData, OpaqueShadowVisibility |
| DirectLighting/SSS | Surface, light lists, ShadowData/opaque visibility | DirectDiffuse/Specular, Emission, SSS diffuse |
| Indirect | Surface/guides, trace radiance, PreviousCacheRadiance, probes, depth hierarchy, optional TLAS, histories | stable indirect 또는 noisy diffuse/specular/AO·ray guides |
| StandaloneDenoise | noisy signals+그 guide/metadata | clean signals |
| OpaqueComposite | direct+선택 indirect+AO+Surface | OpaqueHDR |
| TransparencyVolume | opaque/noisy opaque HDR, depth, ShadowData, scene/material, skin, particles, froxel/history | SceneHDR 또는 NoisySceneHDR, transparent guides/masks |
| SR/TAAU **또는** fused RR | 해당 color/guide/metadata/profile | 유일한 DisplayHDR |
| HistoryExports | final depth/HZB, Surface metadata, 정의된 cache radiance | 다음 render frame의 imported history |
| ExposureMeter | pre-post/pre-UI scene luminance, E[current] | E[next] |
| OutputGuides/Post | DisplayHDR, 필요한 output-guide resolve, profile/look | display-linear HUD-less color |
| UIRender | UiModel/layout, actual output/DPI, paper white | linear premultiplied UIColorAlpha |
| PresentationAdapter | HUD-less/UI, provider-required depth/MV, frame token, DisplayProfile, pacing | 실제 presented frames+completion |

StandaloneDenoise가 필요 없는 신호는 그 단계를 지나지 않는다. DLSS RR profile의 NoisyComposite는 clean으로 위장하지 않고 raw signal과 실제 noise 의미를 보존한다. 해당 profile에서는 별도 최종 SR/TAA가 없다. visibility guide·depth·motion을 post/FG에 전달할 때 provider가 요구하는 해상도/encoding으로 변환한다.

`CacheInjectionExport`는 `DirectDiffuse+Emission`과 **이미 clean인 선택적 indirect diffuse**만 저장한다. fused RR이 clean diffuse를 별도로 반환하지 않는 경우 DisplayHDR에서 diffuse를 추정하지 말고 direct/emissive-only cache를 사용한다. specular/transparency/post/UI는 이 cache에 주입하지 않는다. Brixelizer/RT cache의 bounce 의미가 이 정책과 일치해야 한다.

PreviousHZB/PreviousCacheRadiance/E[current]는 이전에 실제 제출된 render frame의 generation이다. frame을 abort하면 새 history를 publish하지 않는다. reset된 입력은 기본값·valid=false로 초기화한다. disabled feature의 producer와 consumer branch를 함께 제거하며 unresolved handle을 남기지 않는다.

## 16. 기존 파일별 교체·이식 지도

§1의 신규 subsystem에 실제 동작을 옮긴 뒤 호출이 사라진 구현은 제거한다. 아래는 기존 파일별 수정 위치이며 내부 algorithm은 앞 절을 따른다.

| 기존/신규 경로 | 최종 변경 |
|---|---|
| `Source/Core/Public/hs/core/settings.hpp` | schema2 GraphicsSettings 포함, audio/key 유지, 그래픽 명령은 SettingId+typed value 및 Apply/Cancel/Reset/Confirm로 변경 |
| **신규** `Source/Core/Public/hs/core/graphics_settings.hpp`, `.def`; `Source/Core/Private/graphics_settings.cpp` | §13 값·ID·기본값·범위·순수 validation/read-model. UI 문자열·SDK logic 제외 |
| **신규** `Source/Core/Public/hs/core/render_types.hpp` | asset/view/feature/scene 참조와 기존 CharacterAnimationClip semantic 등 공유 값. GPU slot은 포함하지 않음 |
| `Source/Core/Public/hs/core/render_snapshot.hpp` | asset/material/revision/typed light/View/scene ref, immutable static-scene lease, animation semantics; RenderMesh별 하드코딩 binding을 제거 |
| `Source/Core/Private/render_snapshot.cpp`, `snapshot_exchange.cpp`; `snapshot_exchange.hpp` | 확장된 snapshot 복사·capacity·lease 수명. 동적 snapshot+정적 공유 묶음을 안전하게 교환 |
| `Source/Core/Public/hs/core/cooked_format.hpp` | CookedHeader/공통 hash·path·payload API 유지. CharacterAnimationClip semantic은 render_types로, vertex/skeleton/render-file 구조는 RenderData로 분리 |
| `Source/Core/Private/cooked_format.cpp` | 기존 simulation/presentation 파일 의미 유지; 공통 읽기의 size/overflow 검사만 필요한 범위로 보강 |
| **신규** `Source/RenderData/Public/hs/render_data/*`, `Private/*` | §4 render package·MaterialIR/schema·CPU validation 구현. 기존 렌더용 binary schema 대체 |
| `Source/Presentation/Private/presentation_catalog.cpp`, public catalog header | 논리 asset/material/scene/light binding, 현재 preview/VFX catalog 의미 유지. GPU index를 catalog에 저장하지 않음 |
| `Source/Presentation/Private/snapshot_projector.cpp` | 기존 RenderMesh/mesh number→asset/material ID, revision, current animation state를 투영. UI 설정 rendering은 공통 layout 소비 |
| **신규** `Source/Presentation/Private/environment_projector.*` | seed/layout/obstacle revision별 정적 환경 생성·cache; 기존 tree/rock/grass placement 의미 보존 |
| **신규** `Source/Presentation/Private/graphics_settings_layout.*` | descriptor 기반 tabs/scroll/control/상태표시 layout. 위치계산을 hit-test와 공유 |
| `Source/Presentation/Private/ui_interaction.cpp`, `Public/hs/presentation/projector.hpp` | 고정 좌표 토글→설정 layout hit-test/typed 명령. main/pause 모두 적용 |
| `Source/Runtime/Private/application.cpp` | 중복 settings load/override 정리, generation 요청/응답·task executor·view publish·startup SDK bootstrap 연결. 기존 매 frame 무제한 log vector 제거, 120FPS clamp·10,000 particle 성공조건을 applied profile/capacity 기준으로 교체 |
| **신규** `Source/Runtime/Private/graphics_settings_controller.*` | UI pending/requested·confirm·persistence/recovery 상태; renderer resource commit은 소유하지 않음 |
| `Source/Runtime/Private/runtime_channels.hpp` | 설정/결과/capability/view의 불변 메시지, generation·queue pressure 처리. graphics 변경으로 simulation tick 수정 금지 |
| `Source/Runtime/Private/save_store.cpp`, `Public/hs/runtime/save_store.hpp` | §14 migration/roundtrip·intent 저장·display recovery marker·future schema 보존 |
| `Source/Runtime/Private/window.cpp`, `window.hpp` | mode/monitor/DPI/client extent, confirm rollback, foreground audio 동작 보존, renderer와 동일 aiming/UI transform |
| `Source/Runtime/Public/hs/runtime/application.hpp` | RenderConfig/CLI/profile/preview/display 연결 형식 갱신 |
| `Source/RendererD3D12/Public/hs/renderer/renderer.hpp` | §2 public API/입력·상태·capability·diagnostics. 고정 pass 배열·RendererOptions 제거 |
| `renderer.cpp`, `renderer_impl.hpp`, `renderer_frame.cpp` | subsystem 수명·profile·frame context 소유와 Scene→FeatureRegistry→Graph→Display 조립만 유지. mesh ID 분기/CPU draw ranges/하드코딩 post_a,b 제거 |
| `render_graph.hpp/.cpp` | §7 `Graph/`로 교체. 기존 Direct-only 순차 실행/논리 transient만 만드는 구현 제거 |
| `renderer_device.cpp` | Device/SDK bootstrap/capability/queue로 이식. FL만 확인하는 초기화 대신 실제 shader/binding 기능 검사 |
| `renderer_resources.cpp`, `d3d12_resources.hpp` | 영구 자산→Scene/Memory, 중간 화면→Graph, display→Display. 고정 descriptor 개수와 광범위 전체 target 재생성 제거 |
| `renderer_pipeline.cpp` | PipelineManager·shader manifest 기반 PSO/variant/cache. 하나의 source/entry 묶음 컴파일 제거 |
| `renderer_environment.cpp`, `renderer_environment_mesh.cpp` | RenderAssets/Streaming의 공통 load/upload로 교체. 고정 asset 배열·정점 확장 meshbin loader 제거 |
| `renderer_ui.cpp` | §12 ui_renderer로 이식. fixed 1920×1080 전체 surface 무조건 redraw/upload를 output/dirty 경로로 교체 |
| `renderer_capture.cpp`, `renderer_diagnostics.*` | 동적 pass/queue, bounded history, fence 완료 readback, PNG/HDR capture로 교체 |
| `Source/Apps/Content/character_asset_cooker.cpp` | indexed+meshlet/deform/RT package 생산; skin/animation/grounding·body/arrow material 처리 보존 |
| `Source/Apps/Content/environment_mesh_cooker.*` | indexed mesh/LOD/cluster/HLOD/streaming·RT geometry package |
| `Source/Apps/Content/environment_texture_cooker.*`, `environment_histogram_cooker.*` | 채널/색공간/mip/alpha/stream metadata. Gaussian transform+inverse LUT 결과 유지 |
| **신규** `Source/Apps/Content/material_cooker.*`, `probe_cooker.*`, `scene_cooker.*` | MaterialIR/variant, HDR/probe/look, scene/layout/render package manifest 생산 |
| `Source/Apps/Content/content_cooker.*`, `content_source_inventory.*` | source/hash/dependency/chunk/package inventory를 통합하고 stale 부분만 recook |
| `Source/Apps/Content/game_data_cooker.cpp`, `game_data_validation.cpp`, `Schemas/game.schema.json` | presentation asset/light binding 확장만 반영; simulation balance/hash의 입력 의미 보존 |
| `Source/Apps/ProjectHS/main.cpp` | 단일 설정 resolve 입력, 기존 smoke/preview/camera/capture CLI 유지, safe-graphics/profile/provider CLI 추가, SDK export/bootstrap 일치 |
| `Source/Apps/Experiment/main.cpp`, `Source/Apps/ContentTools/main.cpp` | 변경된 renderer/cook public API와 profile·capture 결과 소비. 기존 도구에서 끊긴 호출 제거 |
| `Content/Shaders/project_hs.hlsl`, `environment.hlsli` | §17 feature별 entry와 공통 library로 분해. ScenePS/DeferredPS/JellyLighting/Skin*/Particle*/후처리의 의미를 각각 새 구현으로 이식 |
| `ContentSource/Materials/`, `Scenes/`, `Lighting/`, `Graphics/`; `Schemas/` | material·대표 장면·HDR/probes/LUT·quality profile 원본과 해당 schema 추가 |
| `CMakeLists.txt`, `CMakePresets.json`; **신규** `cmake/RenderDependencies.cmake`, `RenderSDKs.lock.json` | hs_render_data, 새 source/shader/cooker·optional SDK·package graph, 저수준 격리 |
| `Tests/runtime_tests.cpp`, `gameplay_presentation_tests.cpp`, `foundation_tests.cpp` | 설정·snapshot·public 타입 변경 반영. 기존 deterministic gameplay 의미를 약화시키지 않음 |
| **신규** `Tests/rendering_contract_tests.cpp`, `graphics_settings_tests.cpp`, `render_asset_tests.cpp` | §18의 변경 경계에 한정한 자동 회귀 |
| `Tests/check_architecture.cmake`, `ARCHITECTURE.md` | 새 RenderData edge·내부 소유권·public 경계와 실제 build graph 일치 |

`RenderMesh` 및 기존 five-clip 숫자 binding은 presentation catalog에서 asset/animation semantic으로 변환한다. shader는 Archer=0, monster=10…15, environment=16…19 같은 번호로 재질·skin 경로를 결정하지 않는다. 기존 API를 쓰는 나머지 호출부는 새 public API에 직접 연결하며 장기 호환 wrapper를 남기지 않는다.

## 17. Shader·SDK·build/package

### Shader 배치와 빌드

`Content/Shaders/`에 다음 실제 source와 shader manifest를 둔다. 공통 CPU/GPU schema로 생성되는 include는 Build/Generated에 두며 source 파일처럼 수동 편집하지 않는다.

| 위치 | 생성할 entry/library |
|---|---|
| `Common/` | frame/view/scene structs, math/depth/MV, color/exposure, material context, sampling·BRDF |
| `Materials/` | closure/opacity/deformation library, terrain Gaussian/POM, foliage, slime, skin/cloth, generated material variants |
| `Scene/` | scene upload helpers, skeleton/skin/morph, deformed bounds |
| `Visibility/` | cull/LOD/HZB/retest, mesh/indexed visibility/CompactDeferred, draw records, surface classification/resolve |
| `Lighting/` | clustered lists, direct/area/probe, virtual shadow page feedback/render/filter, GTAO/SSR, GI/RT hit/miss·guide |
| `Temporal/` | MV/classification/guide conversion, exposure, NativeTAA/TAAU·DRS 관련 kernels |
| `Effects/` | OIT/hero peel·refraction, particles, decals, SSS, froxel integrate/resolve |
| `Post/` | bloom/blur/DoF/grade/outline, output-guide resolve, display map·encoding |
| `UI/` | linear UI conversion·composition |

manifest 항목은 `id,source,entry,profile,defines,include dependencies,root ABI,material layout,output,required runtime files`다. DXC depfile 또는 정확한 include dependency를 CMake에 연결하여 `environment.hlsli` 이외의 변경도 다시 컴파일한다. CPU와 shader의 struct stride/offset·root binding은 생성 schema와 reflection으로 검사한다. 일반 shader는 SM6.6, Mesh/RT/OMM/SER는 각각 필요한 profile·feature variant를 별도 컴파일한다.

PSO key는 DXIL hash/root ABI/render formats·state/geometry path/material variant/adapter·driver/SDK다. pipeline library는 재사용 실패 시 정상 재생성한다. profile 활성 전에 필요한 PSO를 준비하며 기본 gameplay draw 중 첫 사용 동기 컴파일을 하지 않는다. Debug shader/material hot reload는 새 package·PSO 준비→atomic generation 교체→영향 history 무효화로 연결한다. Release는 배포된 cooked shader/package를 사용한다.

### SDK 잠금과 초기화

확인된 기준은 Agility 1.619.6 (`D3D12SDKVersion=619`), Streamline 2.14.1, XeSS 3.0.2, FSR Ray Regeneration 1.2.0의 문서 계약이다. 이것이 모든 SDK 조합을 실행 검증했다는 뜻은 아니다. [S1,S9,S10,S11]

`RenderSDKs.lock.json`은 SDK별 release/정확한 commit·archive URI/SHA-256, DXC version, include/library/runtime DLL 목록, shader profile, 공식 ABI 문서 revision, 재배포 notice를 고정한다. 해시는 실제 받아 사용하는 bytes에서 산출한다. 임의의 예시 해시나 HEAD/tag만으로 고정됐다고 처리하지 않는다. NRD/Brixelizer 등도 같은 lock을 사용한다. FSR의 최신 SR/FG/RR와 Brixelizer가 서로 다른 배포 세대에 있으면 별도 imported target/adapter namespace로 격리한다.

`HS_WITH_STREAMLINE/XESS/FSR/NRD/DIRECTSR/BRIXELIZER/DIRECTSTORAGE` 빌드 선택을 제공한다. ON인데 필수 SDK가 없으면 구성 시 해당 원인을 표시하며 조용히 stub으로 대체하지 않는다. 제외 빌드는 의도적으로 CompiledOut을 보고한다. 실제 target GPU가 없는 상태와 SDK source 통합 여부도 구분한다.

장치 초기화는 필요한 SDK 사전 bootstrap→DXGI/adapter→D3D12/caps→SDK device 연결 순서다. 실행 중 설정에서 선택 가능한 provider의 device 이전 bootstrap은 시작 시 준비하고, 그 feature의 GPU context/history는 실제 활성화할 때 생성한다. Streamline의 `slInit`은 DXGI/D3D12 호출보다 앞, `slShutdown`은 device 파괴보다 앞이다. native/proxy 객체 소유권을 한 adapter에 모은다. 모든 기능을 켜기 위해 모든 plugin을 항상 로드하지 않는다. DirectSR은 선택한 Agility/runtime에 실제 factory/header/`directsr.dll`이 있는지 검사하고 같은 D3D12Core 배포 디렉터리 계약을 따른다. [S9,S12]

최소 runtime 조건은 FL12_0·SM6.6와 사용하는 bindless/root 기능이다. Mesh Shader/DXR/VRS/feedback/OMM/SER/native FP16/wave·format 지원은 별도 query다. SDK compiled/driver/input/combination/display 조건까지 충족해야 해당 기능이 Available이다. FL12_0만 통과했다고 shader 기능을 가정하지 않는다.

CMake는 Core 저수준 구성에서 runtime/content-only dependency를 fetch/발견하지 않는다. `hs_render_data`는 API 독립 library, renderer와 cooker는 각자 필요한 SDK를 PRIVATE link한다. `runtime=ON/content tools=OFF`는 유효한 precooked package를 입력으로 받으며 없으면 누락을 명시한다. runtime이 FBX 원본을 임의로 cook하지 않는다. runtime package는 cook manifest가 참조하는 모든 mesh/material/texture/scene/probe/shader/audio/font와 실제 선택 DLL·license를 설치/CPack에 포함한다. 예전 고정 환경·몬스터 파일 목록으로 누락시키지 않는다. 라이브러리 DLL은 신뢰한 package 경로에서 로드하고 자산 loader는 package root 밖 경로·손상 chunk·과도한 decompress 요청을 거부한다.

## 18. 최종 상태의 확인 조건

완료 기준은 아래 연결 경계다. 동일 위험을 검증하는 중복 framework나 모든 옵션의 무차별 조합 테스트를 추가하지 않는다.

| 경계 | 필요한 확인 |
|---|---|
| gameplay 보존 | 기존 deterministic gate와 같은 입력/replay에서 프리셋·render FPS·FG 차이가 simulation checksum/판정에 영향 없음. essential 경고·투사체 유지 |
| material/asset | 기존 모든 사용 자산이 새 package로 로드됨. opacity/LOD/deformation이 visibility·shadow·motion·RT에서 일치. gradient·Gaussian 색공간·mip와 MaterialIR validation 확인 |
| temporal/visibility | 동일 tick 다중 render, camera cut·origin/DRS 변경·신규/reused slot·LOD crossfade·skin/foliage motion에서 이전 frame 대응. HZB reject가 current frame에서 다시 나타남 |
| graph/lifetime | same-state UAV, depth+SRV, mip/array/buffer 범위, AS build→trace, cross-queue alias, in-flight/SDK lease, DirectSR 외부 submit의 순서 확인 |
| reconstruction/display | NRD/FSR standalone과 DLSS fused의 producer 중복 없음. HUD-less/UI/HDR/SDR encoding·alpha·DPI hit-test·실제 frame token 일치 |
| 설정 | v1 migration/v2 roundtrip, 사용자 의도 보존, pending/cancel/확인 timeout·OOM·저장 실패·late response·provider/monitor 전환 복구. 노출 옵션마다 실제 effect 변경 |
| 자원/성능 | streaming·descriptor가 bounded하고 leak 없이 회수. 비활성 기능의 지속 비용 제거. 값은 지원 hardware에서 실제 render critical path·CPU submit·VRAM peak로 확인 |
| 확장 | 새 MaterialIR 조합은 데이터/variant만으로 사용, 새 feature는 signal/subgraph 등록으로 연결. 새 provider의 guide는 adapter 요구로 생성되고 중앙 Render에 재질·SDK별 분기를 추가하지 않음 |

실행 가능한 자동 검사는 기존 CTest와 `verify-core`/`verify`에 연결한다. D3D12 debug/GPU validation은 실제 실행한 지원 경로에서 오류 0이어야 한다. 실행하지 못한 GPU/provider는 미검증으로 남기고 오류를 숨겨 통과시키지 않는다.

대표 capture/replay는 기존 camera zoom 근접/일반, 이동 식생·LOD, 300슬라임 중첩, 다수 VFX, material/light showcase를 새 renderer의 같은 코드 경로에서 사용한다. 정지 그림뿐 아니라 카메라·포즈 이동 중 잔상/깜빡임·접촉 그림자·굴절 중첩·재질 구분·전투 가독성을 확인한다. showcase만 별도 고품질 shader를 사용하고 실제 gameplay를 이전 표현으로 남기지 않는다.

Diagnostics는 queue/pass timestamp를 **완료된 fence 이후** 읽고 bounded ring/비동기 기록으로 유지한다. CPU_render_ms, GPU_critical_path_ms, pass/queue timeline, p50/p95/p99, actual render/display FPS, frame pacing·latency 출처, VRAM 사용/예산/전환 peak, stream misses, AS 비용, provider 메모리를 구분한다. async pass 시간 합계를 frame GPU 시간으로 표시하지 않는다. validation-on 정확성 측정과 validation-off 성능 측정을 분리한다. 특정 GPU가 지정되지 않았으므로 임의 FPS·개선율을 보장값으로 적지 않는다.

기능을 빈 인터페이스/상수 출력/항상 Unsupported로 남기거나 모든 고급 옵션을 fallback으로 보내는 것은 구현 완료가 아니다. 결과에는 실제 연결·실행된 기능과 외부 prerequisites 때문에 실행하지 못한 경로를 구분한다. 최종 산출물은 새 엔진·관련 cooker/자산/설정/UI·빌드/패키지·필요 회귀 검사가 연결된 코드다.

## 참조

[P]는 기준 커밋의 실제 코드, 아래 [S]는 외부 ABI·기술 계약 확인용이다. 본문의 나머지는 이 프로젝트에 적용할 **설계 결정**이다. 선행 대화·v5/v6를 다시 읽어야만 해석되는 요구는 남기지 않는다.

- [P] `https://github.com/crapa36/Project-HS/tree/1354b1edb3939d480dab62c05ebb264d9d6e05d8` — ARCHITECTURE, Core/settings·render_snapshot·cooked_format, RendererD3D12, Runtime, Presentation, Content shaders/cookers, CMake. main 재확인 2026-09-27.
- [S1] `https://devblogs.microsoft.com/directx/directx12agility/` — Agility 1.619.6/619, SM·DXR capability 경계.
- [S2] `https://microsoft.github.io/DirectX-Specs/d3d/D3D12EnhancedBarriers.html` — access/sync/layout·aliasing 계약.
- [S3] `https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiadapter3-queryvideomemoryinfo` — process memory budget.
- [S4] `https://learn.microsoft.com/en-us/windows/win32/direct3d12/resource-binding-in-hlsl` — binding ABI.
- [S5] `https://google.github.io/filament/main/filament.html` — PBR·coat·anisotropy·cloth/subsurface 참고. 프로젝트 MaterialIR 설계 자체를 제공하는 문서는 아님.
- [S6] `https://learn.microsoft.com/en-us/samples/microsoft/xbox-gdk-samples/visibilitybuffer/` — visibility/attribute 복원 및 frontend 성능 특성.
- [S7] `https://github.com/NVIDIA-RTX/NRD` — signal packing·NRD integration.
- [S8] `https://gpuopen.com/manuals/fidelityfx_sdk/techniques/brixelizer-gi/` — SDF/previous radiance와 GI 입력·출력.
- [S9] `https://github.com/NVIDIA-RTX/Streamline/tree/v2.14.1/docs` — ProgrammingGuide, DLSS, DLSS_RR, DLSS_G, Reflex의 bootstrap/resource/present 계약.
- [S10] `https://github.com/intel/xess/tree/v3.0.2/doc` — SR/FG/XeLL 통합. `https://github.com/intel/xess/releases`.
- [S11] `https://gpuopen.com/manuals/fsr_sdk/techniques/denoising/` — Ray Regeneration 1.2 독립 denoising·2.5D motion·packing. 같은 공식 SDK 문서의 upscaling/frame-generation 계약과 함께 해당 lock release를 사용.
- [S12] `https://microsoft.github.io/DirectX-Specs/DirectSR/DirectSR.html` — variant·runtime 배포·자체 queue execute/completion.
- [S13] `https://learn.microsoft.com/en-us/windows/win32/direct3darticles/high-dynamic-range` — Advanced Color/HDR output 계약.
