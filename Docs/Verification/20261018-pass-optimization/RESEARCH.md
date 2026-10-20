# 패스 공유·결합 추가 조사 — 2026-10-18

초기 조사 후 후보1·2, AO 및 terrain 공통 계산을 적용했다. 최종 네이티브·실측 상태는 REPORT.md 참조. 최신 캡처는 지면 경계 수정 이전이며 통제 전후 비교가 아니다.

## 근거와 우선순위

`20261016-pass-option-review/ANALYZER_REPORT.md`의 foreground Playing 742프레임은 base 32.943 FPS, SDK 보고 65.859 FPS, GPU critical path 27.101ms다. GBuffer 4.985ms, Adaptive GTAO horizon 3.894ms, directional shadow 3.576ms가 컸다. 작은 패스 결합만으로 144 FPS를 입증할 수 없다. 해당 값은 옵션을 개별 끈 차이가 아니며, 큐 중첩 때문에 패스 시간을 더해서 프레임 시간이나 예상 절약량으로 쓰면 안 된다. 이전 56옵션 비용표 역시 공유 비용을 옵션별 독립 비용으로 해석하지 않는다.

기존 구현과 이번 턴의 동시 작업을 구분했다:
- record-list 최대4-scope 병합 실험은 좁은 검증을 통과했으나 전체 native에서 binding 오류와 driver 충돌을 재현하여 전부 철회했다. 기존 Enhanced 제출 묶기는 유지한다. 초기 line1525–1558은 실험 당시 소스이며 현재 적용 상태가 아니다.
- `renderer_frame.cpp:6754–6768`: adaptive classification 선행 순서와 AO/CSM async overlap이 이미 있다. 이것은 shader 내부의 AoGuideForSample 반복 호출을 제거했다는 뜻은 아니다.
- `hdr_post.hlsl:202` 이하: DoF foreground dilation groupshared tile 최적화가 이미 있다.
- SSR 깊이는 완성된 opaque의 reversed-Z MAX이고 culling HZB와 시점/의미가 다르다(`renderer_reflections.cpp:422`). 이름이 depth pyramid라고 그대로 공유하면 안 된다.

## 1. HDR nearest guide + signed CoC 계산 결합 — 가장 작은 변경

현재 `renderer_hdr_post.cpp:385–416`, `hdr_post.hlsl:78,189`: guide dispatch가 이미 복원한 distance를 출력하고 별도 SignedCoC dispatch가 다시 읽어 thin-lens radius를 계산한다. CoC는 HDR 색상이나 motion-blurred 결과에 의존하지 않는다.

변경: DoF가 켜졌을 때 guide dispatch에서 같은 distance로 CoC UAV도 기록. 별도 pass 112와 dispatch 제거. MB는 guide/normal을, dilation은 CoC를 읽고, DoF는 MB 결과 및 guide/CoC/dilation을 읽는 기존 순서 유지. DoF off이면 CoC write/binding이 없어야 한다.

제거 작업: 전체 화면 guide read, 별도 dispatch/상태 설정/graph node. guide 출력 및 CoC 출력 자체는 후속 소비자가 있으므로 유지한다. producer→dilation UAV ordering은 graph가 선언된 write/read로 처리해야 한다. dispatch 내부 global barrier로 dilation까지 합치지 않는다.

검증: MB off/on × DoF off/on, sky/transparent nearest-front, 활성 해상도 변경, lens 극값, 기존 CoC 수치와 비교. graph pass 112 미등록과 정확한 producer version 확인, DXC 및 native validation. 구현 완료. native-v4 combined/MB-only/DoF-only가 exit0/errors0이며 combined는 GBV 포함. 모든 이미지 조건의 수치 동등성을 입증한 것은 아니다.

## 2. SSR spatial resolve + environment replacement 결합

현재 `screen_reflections.hlsl:421–459`, `renderer_reflections.cpp:466–478`: spatial이 완성한 각 픽셀의 reflection을 raw에 저장하고 Compose가 같은 픽셀을 읽어 `opaque - environment + lerp(environment, reflection.rgb, confidence)`를 수행한다. `renderer_frame.cpp:7177–7185`의 실제 호출자는 composed만 소비하며 reflections export는 사용하지 않는다.

변경: spatial dispatch 마지막에 동일 replacement 수식을 실행하여 composed로 바로 기록. pass 73 및 후반 raw write→SRV read 제거. **raw 자원은 Trace→Temporal에 필요하므로 자원 전체를 삭제하면 안 된다.** 미사용 reflections export를 제거할 경우 private output 계약 및 모든 호출자를 함께 갱신한다.

절약 대상: raw는 R32G32B32A32_FLOAT(`renderer_reflections.cpp:192,376`)다. 1920×1080에서 후반 write+read 논리 트래픽 32 bytes/pixel = 약 66.36MB/frame와 dispatch 하나 제거 후보. 캐시/압축/실제 DRAM 트래픽 및 ms 절약과 동일하지 않다. fp16 중간 양자화는 여기에는 없다.

동기화: temporal history/current validation의 final access와 completion 유지. spatial에 opaque_hdr/environment read와 composed write를 명시. Trace의 radiance는 replacement 이전 snapshot을 계속 읽어 피드백과 환경 반사 중복 가산을 막는다. Spatial의 filter 계산과 역사 거부는 변경하지 않는다.

검증: SSR off/각 quality, roughness/sky/miss/disocclusion/environment-only, moving camera 및 얇은 면. 독립 CPU 수식 oracle + 기존/결합 native output 비교, native GBV, timestamp 73 미등록 및 72 통합 비용 이름 갱신. 기존 과거 72 비용 0.447ms 전체가 제거되는 것은 아니다.

## 3. Bloom 최종 합성 + grading + tone map 결합 — 후속 후보

현재 `project_hs.hlsl:4002–4024`, `renderer_frame.cpp:7888–7909,8048–8055`, `renderer_hdr_post.cpp:446`: Bloom full-resolution 합성, LUT grading, tone map이 별도 full-screen 출력/읽기를 한다. 반면 저해상도 bloom pyramid/필터는 여전히 이웃 의존성이 있어 유지해야 한다.

후속 변경: terminal ToneMap에서 sharpen-once → bloom halo → scene-linear LUT → display-linear tone map의 현재 순서를 유지하며 하나의 pixel 계산으로 합친다. Bloom composition과 Grade의 중간 full-resolution target round trip을 없앨 수 있다. 먼저 1·2를 적용하고 측정한 뒤 진행하는 편이 범위 대비 효율적이다.

주의: 단순히 color grading을 tone map 뒤로 옮기면 화면 색이 바뀐다. 현재 bloom intermediate의 fp16 rounding, LUT sample shaping, sharpening을 정확히 한 번 적용하는 조건, HDR/SDR, ImGui와 최종 display encoding 경계를 유지해야 한다. graph post version plan, descriptors/root signature, shader catalog, output ownership 및 `render_feature_tests.cpp`의 topology assertions도 함께 변경해야 한다. copy/alias lifetime를 무시한 표면적 shader 병합은 안 된다.

## 공개 엔진 최신 소스 재확인

GitHub 웹 검색 캐시는 9월 결과를 보여 최신 근거로 쓰지 않았다. 2026-10-18 직접 GitHub 공식 REST branch endpoint를 읽어 SHA를 얻고, 각 SHA의 raw source를 조회했다(읽기만 수행).

| 엔진 | 현재 조회한 개발 SHA | committer UTC 날짜 | 확인 위치 |
|---|---|---|---|
| Godot | `81ab961f91066a63bcbf3ec340962bb1a1ada558` | 2026-10-16 16:29:07 | [ss_effects.cpp](https://github.com/godotengine/godot/blob/81ab961f91066a63bcbf3ec340962bb1a1ada558/servers/rendering/renderer_rd/effects/ss_effects.cpp#L537) |
| Wicked Engine | `cb4a81c853cbdf3b792ba539888a3353512ce52c` | 2026-10-16 12:47:36 | [wiRenderPath3D.cpp](https://github.com/turanszkij/WickedEngine/blob/cb4a81c853cbdf3b792ba539888a3353512ce52c/WickedEngine/wiRenderPath3D.cpp#L2448) |
| O3DE | `85f846e64e9d03f4114e371dc5eb255b6484e717` | 2026-10-17 15:48:24 | [FrameGraphExecuter.cpp](https://github.com/o3de/o3de/blob/85f846e64e9d03f4114e371dc5eb255b6484e717/Gems/Atom/RHI/DX12/Code/Source/RHI/FrameGraphExecuter.cpp#L109) |

Godot: downsample_depth가 SSAO와 SSIL의 quality를 함께 보고 mip/half-size 모드를 선택한다(537–658). 공통 준비를 공유하지만 입력/해상도 요구가 일치해야 한다는 사례다. HS의 culling HZB를 SSR에 무조건 연결하는 근거가 아니다.

Wicked: bloom와 luminance를 준비하고 최종 Tonemap 호출에 bloom/컬러그레이딩/노출 입력을 함께 전달한다(2448–2490). HS 후보3과 직접 관련된다. 또한 compute effects는 prepass를 기다리고(1025–1028), graphics lighting은 compute effects를 기다린다(1439). 독립 작업만 overlap하는 명시적 dependency 구조이며 모든 효과를 compute queue로 옮기는 방식이 아니다.

O3DE: 비용, swapchain 수, hardware queue, fence 경계, device 조건으로 scope group을 합친다(109–145). HS의 최대4-scope record 병합 실험은 전체 native 실패 후 철회했다. 기존 제출 그룹은 유지한다. 추가 범용 executor 재설계보다 위의 실제 중간 texture 작업 제거가 새 이득 후보다.

## Async compute 판단

현재 확실한 새 후보보다 기존 AO/CSM overlap의 실제 queue timeline 검증이 우선이다. SSR은 현재 opaque HDR를 읽고 후속 volumetric/composite는 SSR 결과를 읽기 때문에 queue label만 바꾸면 대기/전환만 늘 수 있다. DoF/MB도 색상 의존 순서가 있으므로 전체 동시 실행은 올바르지 않다. 독립 CoC를 guide에 함께 계산하는 결합과, 소비 완료를 기다리는 effect 자체의 병렬화는 구분한다. 계산량을 제거하는 1·2가 우선이다.

이후 native-v4 세 조합 검증과 foreground Playing653프레임 측정을 완료했다(REPORT.md). base28.978/SDK57.965FPS로144FPS 미달이다. 장면 변경 때문에 전후 개선율은 미검증이고 최신 캡처는 지면 경계 수정 결과를 포함하지 않는다.

## SSR 구현 완료 기록

부모의 추가 구현 승인 후 후보2를 적용했다. pass72 `SSRSpatialCompose`가 최종 HDR까지 기록하고 pass73은 제거했다. Trace→Temporal용 raw R32 texture는 유지하며, 사용하지 않던 resolved SRV t13 및 reflections 출력/별도 export descriptor/compose shader product를 제거했다. 변경은 `screen_reflections.hlsl`, `renderer_reflections.cpp/.hpp`, `RenderMaterialShader/main.cpp`의 SSR catalog 두 행에 한정했다. Frame의 실제 호출자는 composed만 사용하므로 수정하지 않았다.

`Tests/check_ssr_spatial_composition.py` 4개 검사(환경 유지/교체, confidence/HDR 범위, graph 입력·출력 및 Trace 유지) 통과. 이후 CMake rendering.ssr-spatial-composition-contracts에 등록되어 통과했다. `git diff --check` 통과. 이 CPU/source 검사는 GPU floating-point 동일성이나 barrier 정확성을 증명하지 않는다. 통합 content/DXC와 native-v4 combined GBV가 이후 통과했고 foreground 측정도 완료했다. 통제 전후 비교와 SSR 전체 픽셀 동등성은 여전히 미검증이다.
## GBuffer 후속: terrain 반복 계산 제거

현재 소스 대조에서 중앙 지면의 중복 draw는 발견되지 않았다. `environment_layout.cpp:104–126`은 외곽 4개 slab와 중앙 meadow를 분리하며, `renderer_frame.cpp:5083,5101`은 meadow water를 opaque/shadow draw에서 제외한다. 실제 GPU overdraw 수치는 아직 측정하지 않았다.

`material_terrain.hlsli`의 주요 비용 구조는 활성 terrain layer 최대 4개 × stochastic tile 3개 × 각 POM/표면 샘플이다. POM·표면 품질을 낮추는 대신 아래 불필요한 ALU 작업을 제거했다:

1. EnvTerrain의 meadow 내부 경로: legacy edgeNoise와 path를 먼저 계산한 뒤 MeadowPressed로 덮어쓰던 코드를 if/else로 바꿨다. 내부는 MeadowPressed만, 외부는 원래 legacy 수식을 실행한다. 경계 조건 `all(abs(world.xz)<=12)` 그대로다.
2. TerrainLayer의 두 warp noise는 world.xz와 seed만 읽고 layer에 의존하지 않는다. EnvTerrain에서 한 번 계산해 전달하도록 이동했다. seed+413/827, 좌표 world.xz/7, 이후 tile별 scaling, texture derivatives/POM/normal/레이어 결합 순서는 유지한다. layer0의 양수 최소 weight 때문에 원본도 반드시 한 번 이상 이 noise를 실행하므로 불필요한 0-layer 평가가 새로 생기지 않는다.

부모 승인 후 측정 버전의 `Content/Shaders/material_terrain.hlsli`에 두 변경을 적용했다. 후속 경계 수정과 구분하는 당시 SHA256 `74213279E86435E017AB64BFF8E82771A02BCE52BBC36090B1E0F4CBCACADA0A`.

### 컴파일 근거와 한계

`Build/pass-optimization-20261018/terrain-sharing/`에 원본(before), warp-only 후보(warp), 최종 후보(after), 실제 helper를 호출하는 3-MRT pixel shader harness와 DXIL을 보존했다. DXC ps_6_0 -O3 -Ges -WX 통과. RenderData 현행 정의(geometry page256/buffers4, authored16/hero7/environment30/LOD3/texture slots6/ground begin14/material15)를 사용했다. 첫 슬롯16 시도와 중간 계약 추가에 따른 실패 후 현행 정의로 모두 다시 컴파일했으며 최종 비교는 수정된 결과다.

정적 SSA 합계는 before3192, warp3192, after3205다. **개수만 보면 이 최적화를 놓친다.** 실제 DXIL CFG에서 seed+413/827 계산은 before의 cyclic block805 안에 있었고 warp의 block773, after의 block782에서는 루프 밖이다. warp-only 비교의 184개 SSA 계산 덩어리가 반복 layer body에서 preheader로 이동했다. 컴파일러가 기존에 이미 제거했던 일을 재작성한 것이 아니다. `check_compiled_hoist.py`는 실제 DXIL branch 그래프의 cycle을 따라 두 계산의 루프 소속이 바뀌었음을 검사하며 통과했다. 최대 4개 활성층이면 동일 noise 평가 최대4회→1회지만, 이를 GPU 시간이나 FPS 향상률로 환산하지 않는다. 최종 branch 때문에 정적 명령은 13개 늘며 meadow 내부에서는 덮어쓰던 noise 계산 경로를 실행하지 않는다.

전체 generated GBuffer shader·네이티브 출력 동등성은 이 standalone helper 컴파일이 증명하지 않는다. 이후 main shader compile 및 native-v4 combined GBV가 통과했다. 다만 영역별 픽셀 동일성이나 후속 경계 수정 검증까지 대신하지 않는다. 최소 이미지 비교 영역은 meadow 내부/외부와 x/z=±12 경계, mesh20 dirt 경로, 다층 지면, POM off/on, 가까운/먼 거리다. 색·normal·AO·roughness의 수식은 유지했지만 GPU 코드 이동에 따른 float 차이는 native 결과로 확인한다.

### 공개 엔진과 비교 및 보류

[Wicked Engine 최신 고정 소스](https://github.com/turanszkij/WickedEngine/blob/cb4a81c853cbdf3b792ba539888a3353512ce52c/WickedEngine/wiRenderPath3D.cpp#L938)의 main camera depth prepass는 shaded overdraw를 줄이는 구조다. HS에 전면 도입하면 추가 geometry/alpha draw와 depth 정합성 비용이 있어 이번 작은 변경에 포함하지 않았다. Godot의 공통 depth preparation처럼 공유 가능한 입력 계산을 앞에서 한 번 수행하는 접근을 terrain의 순수 layer-invariant warp에 국한했다. 알고리즘 이식이나 엔진 전체 속도 비교가 아니다.

MeadowWetness의 33개 레코드 중 wet 레코드만 순회하도록 generator를 바꾸는 후보는 source-of-truth/generated CPU·HLSL·asset 재생성 범위가 커 이번에는 보류했다. terrain POM 샘플 수와 텍스처 디테일, geometry, draw ordering 및 그래픽 품질 옵션은 변경하지 않았다.


## 후속 지면 경계 수정 상태

부모 통합 보고: 외곽 slab 상면 +5mm→0, MeadowSurface seed를 static/dynamic/software capture/canonical vertex에서 통일, path/wet/traces에 높이와 같은1.5m 경계 blend를 적용했다. warp hoist는 유지했다. presentation-contract 및 shader reconstruction33개 검사 통과. scene package 재빌드 후 네이티브 시각 검증은 대기 중이다. 최신 pass-fusion-01 캡처와 native-v4 결과는 이 수정 이전 것이므로 시각 문제가 해결됐다고 판정하지 않는다.
