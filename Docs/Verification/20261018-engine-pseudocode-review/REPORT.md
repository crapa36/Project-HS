# 공개 엔진 실제 코드와 Project HS 의사코드 비교

작성일: 2026-10-18. **분석만 수행했으며 게임 코드·그래픽 설정은 변경하지 않았다.**

## 범위와 판단 기준

최근 병목과 연결되는 재질/GBuffer, AO, geometry/LOD, 그림자, 프레임 그래프, 후처리를 비교했다. 게임플레이·물리·오디오 등 엔진 전체의 완전한 감사는 아니다. 의사코드는 실제 함수의 제어 흐름을 같은 추상화 수준으로 축약했고, 서로 다른 화질·데이터·실행 단위는 명시했다.

HS 기준은 `codex/cpu-gpu-utilization`, HEAD `bc7bd6024fe5a84456e785c81731a127ec339b6b` **위에 누적된 현재 미커밋 수정**이다. HEAD만 체크아웃하면 이 보고서의 코드와 다르다. 주요 파일 해시는 옆 `SOURCE_MANIFEST.json`에 있다. 아래 HS 경로는 이 작업트리 루트 기준이다.

공개 엔진은 공식 저장소의 고정 커밋 파일을 이번 조사에서 다시 읽었다. 최신 HEAD 또는 출시 버전이라고 주장하지 않는다.

| 엔진 | 고정 SHA | 비교 대상 |
|---|---|---|
| Godot | `81ab961f91066a63bcbf3ec340962bb1a1ada558` | 공유 깊이 준비, SSAO mip/적응 샘플 |
| Wicked Engine | `cb4a81c853cbdf3b792ba539888a3353512ce52c` | 지형 재질 갱신, AO 타일, LOD/그림자 배치, 최종 후처리 |
| O3DE Atom DX12 | `85f846e64e9d03f4114e371dc5eb255b6484e717` | 비용·큐·fence 기반 기록 그룹 |

## 먼저 고칠 문제와 아직 가설인 부분

| 우선순위 | 확인된 사실 | 문제 분석 / 가장 작은 다음 단계 |
|---|---|---|
| 1 | meadow ground LOD0/1/2가 각100,370삼각형으로 동일, meadow는 cluster refinement 제외 | 실제 감소하는 LOD를 만들어야 한다. 작은 자국의 과밀 메시 표현을 먼저 줄일 후보. 지면/수면 경계와 접촉 높이 보존 필수 |
| 2 | 지형은 활성층 최대4 × stochastic 좌표3마다 POM 평가 | 최대12탐색은 소스 상한. GBuffer6.332ms 전체를 지형 비용으로 단정하지 말고 terrain/활성층/POM 비중부터 분리 |
| 3 | Count와 Emit이 같은 hierarchy refinement를 반복하고, 비활성 뷰도 frustum plane 검사 | 출력 예약·overflow 원자성을 보존하며 비활성 뷰 검사부터 좁혀 검토. 선택 결과 저장은 추가 대역폭과 비교 필요 |
| 4 | CSM3개 cascade 매 프레임 raster + VSM dirty/cache 경로 공존 | VSM miss가 CSM을 소비하므로 단순 삭제 불가. cascade별 caster·triangle·fallback 사용률로 중복 범위를 확인 |
| 5 | AO 인접 guide는 공유하지만 horizon의 원거리 tap은 full-res guide 재로드 | GTAO 수식을 유지한 선형 깊이 공급의 총비용 비교. mip/샘플 감소는 별도 화질 정책 |
| 6 | CPU 기록 약420jobs, 제출 약247groups/프레임; graph 구성4.975ms·제출3.950ms | 캐시/병렬화는 이미 존재. key miss·경계 원인부터 확인하며 철회된 기록 병합을 무작정 복원하지 않음 |
| 7 | bloom 최종 합성·grade·tone map 사이 전체 화면 중간 출력 | 순서를 보존한 shader 결합 후보. 현재 관련 비용이 작아 우선순위는 낮음 |

이 표는 제거 가능한 시간 순위가 아니다. 2–7의 성능 절감량은 가설이며 실제 통제 측정 전에는 ms/FPS 개선치를 제시하지 않는다. 최신 기존 측정은 Playing653프레임 base28.978FPS/SDK57.965FPS, GPU critical31.146ms다. 지면 경계 수정 이전 캡처이며 같은 아트/코드의 전후 비교가 아니다. GPU 패스 합계는 큐 중첩 때문에 프레임 시간이 아니다. 자세한 표본·PDH partial 조건은 [측정 보고서](../20261018-pass-optimization/REPORT.md)를 따른다.

## 지면이 평평해 보이는데 10만 삼각형인 이유

외곽은 평면 slab이지만 중앙24×24m는 완전한 평면이 아니다. `tools/art/meadow/generate_surface.py:35–54,140–183`이 최대2.4cm 크기의 기본 굴곡과 작은 함몰을 실제 정점 높이에 적용한다. 발굽 주변50cm cell은32×32(간격1.5625cm), 발자국 주변은16×16(3.125cm), 다른 함몰 주변은4×4로 세분한다. 세부 cell은 중심점에서 둘레로 삼각형을 만들고, 이웃 해상도 경계에도 정점을 추가한다. 이 국소 고밀도가 LOD 하나당 약10만 삼각형을 만들고, 동일 LOD 복제는 거리별 감소를 막는다.

이 방식은 자국을 실제 기하로 재현하려는 구현이지만 현재 게임 카메라에서 필요한 화면 오차로 세분량을 정한 것은 아니다. **10만 삼각형이 필요한 것으로 검증되지 않았다.** 큰 굴곡·수면/웅덩이 윤곽은 기하로 유지하고, 작은 발자국·발굽 표면 디테일은 normal/height/decal로 옮기는 방향을 우선 검토한다. 사용자 요청의 자국을 없애는 것과 표현 방식을 가볍게 바꾸는 것은 다르다. CPU 접촉 높이와 렌더 지면의 허용 오차, 수면 들뜸, 가까운 카메라에서의 차이를 확인해야 한다. 이 보고서에서는 재생성하지 않았다.

## A. 재질과 AO

### A1. 지형 레이어 평가를 언제 수행하는가

HS: `Content/Shaders/material_terrain.hlsli` `TerrainParallaxUv` 51–73, `TerrainSample` 75–105, `DecodeTerrainHistogram` 114–127, `TerrainLayer` 129–159, `EnvTerrain` 236–268.

```text
HS / 지형 재질 평가
각 화면 지형 픽셀:
    4개 레이어 가중치와 공통 warp 계산
    양수 가중치 레이어마다:
        삼각 격자의 3개 확률적 텍스처 좌표마다:
            해당 좌표에서 높이맵 POM 탐색 + 세분화
            이동한 좌표의 색/표면/법선/선택적 디테일/높이 샘플
        3개 결과 혼합 후 색 히스토그램 복원
        샘플 높이로 레이어 가중치 보정
    전체 레이어 정규화 혼합

Wicked / 지형 재질 평가용 텍스처 갱신
갱신 dispatch의 각 4×4 출력 블록:
    16개 출력 texel마다:
        레이어를 뒤에서부터 순회:
            blendmap의 가중치 0이면 생략
            선택한 출력 종류의 재질맵 샘플
            front-to-back 누적; 완전 피복이면 중단
    결과를 BC1/BC5/BC3 블록으로 압축하여 저장
```

Wicked 근거: [`terrainVirtualTextureUpdateCS.hlsl` 51–89, 91–185](https://github.com/turanszkij/WickedEngine/blob/cb4a81c853cbdf3b792ba539888a3353512ce52c/WickedEngine/shaders/terrainVirtualTextureUpdateCS.hlsl#L51). SHA `cb4a81c853cbdf3b792ba539888a3353512ce52c`. 읽은 `surfaceHF.hlsli`의 [지형 재질 샘플 경로 716–763](https://github.com/turanszkij/WickedEngine/blob/cb4a81c853cbdf3b792ba539888a3353512ce52c/WickedEngine/shaders/surfaceHF.hlsli#L716)에서는 chunk material의 이미 만들어진 base/normal/surface/emissive 맵을 샘플한다. 이 구간은 다른 표면과의 terrain blending 경로이므로 이를 Wicked의 모든 terrain draw 경로라고 일반화하지 않는다. 이번에 CPU 갱신 스케줄러까지 조사하지 않았으므로 페이지 갱신 빈도도 단정하지 않는다.

**확인된 차이:** HS는 양수 레이어당 3개 POM 탐색을 화면 픽셀에서 수행한다. 4개 레이어가 모두 유효하면 최대 12개 탐색이다. 가까운 거리에서 확률적 변형당 최대 `steps + refinementSteps + 5` 텍스처 샘플(디테일 켜짐), 추가로 레이어당 히스토그램 3 sample + 4 load가 있다. 이는 소스상 상한이며 실제 실행 수·캐시 미스·GPU 시간 측정값이 아니다. POM은 55 m 이상에서 꺼지고 35–55 m 구간에서 변위가 줄어든다. Wicked 비교 경로는 레이어 합성을 별도 텍스처 갱신으로 분리한다.

**그대로 옮기면 안 되는 이유:** HS는 높이 기반 정규화 혼합이다. Wicked의 알파 피복 조기 종료를 적용하면 남은 레이어의 높이·색·법선 기여가 사라진다. POM의 위치는 시선에 의존하므로 최종 POM 결과를 정적 페이지로 굽는 것도 동등하지 않다. 공통 warp는 이미 루프 밖으로 이동했으므로 추가 개선 후보가 아니다.

**최소 변경 제안:** 먼저 terrain 전용 GPU 구간과 활성 레이어 수/POM 반복 수를 구분해 전체 GBuffer 중 실제 비중을 확인한다. 이후 원거리의 POM 비활성 구간에 한정해 시선 비의존 재질 결과 캐시를 검토한다. 전체 VT 도입보다 범위가 작지만, 현재 월드 좌표/seed/히스토그램/미분 기반 mip/경계 혼합을 보존해야 한다. 압축·해상도·경계 오차와 갱신 비용 때문에 이 역시 무조건 동일 화질·고속이라고 주장할 수 없다. 근거리 POM step 축소나 낮은 가중치 레이어 생략은 별도의 화질 정책 변경이다.

### A2. AO 반복 샘플의 깊이 표현과 품질 예산

HS: `ambient_occlusion.hlsl` `AoLoadGuide` 77–89, `AoGuideForSample` 90–111, `AoHorizonVisibility` 114–165; `adaptive_gtao.hlsl` 6–65.

```text
HS / AO 샘플 공급과 평가
각 8×8 그룹: guide + density를 각 lane이 한 번 준비하고 공유
각 2×2: 재질/깊이/법선/density 일치하면 anchor만 horizon 계산
horizon 계산:
    slice × step × 양쪽 방향마다:
        전체 해상도 깊이/재질로 위치 복원
        반경·유효성 검사 후 horizon 누적
resolve: anchor 호환이면 재사용, 아니면 자기 horizon 재계산

Godot / AO 샘플 공급과 평가
SSAO와 SSIL 요구를 함께 보고 선형 깊이 deinterleave/mip 준비
각 AO 평가 위치:
    화면상의 샘플 반경과 tap 위치로 depth mip 선택
    선택 mip에서 view-space Z를 읽고 위치 복원
    대칭 tap 두 개로 차폐 누적
    적응 품질 모드: importance와 전체 예산으로 추가 tap 수 제한
```

Godot SHA `81ab961f91066a63bcbf3ec340962bb1a1ada558`. 직접 확인한 근거:
- [`SSEffects::downsample_depth`, ss_effects.cpp 537–605](https://github.com/godotengine/godot/blob/81ab961f91066a63bcbf3ec340962bb1a1ada558/servers/rendering/renderer_rd/effects/ss_effects.cpp#L537): SSAO/SSIL 공유 선형 깊이, 두 옵션의 품질·half-size 조합으로 mip 모드 결정.
- [`SSAO_tap_inner`/`SSAOTap`, ssao.glsl 186–244](https://github.com/godotengine/godot/blob/81ab961f91066a63bcbf3ec340962bb1a1ada558/servers/rendering/renderer_rd/shaders/effects/ssao.glsl#L186): 선택 mip Z 샘플·위치 복원·대칭 tap.
- [같은 파일 357–407](https://github.com/godotengine/godot/blob/81ab961f91066a63bcbf3ec340962bb1a1ada558/servers/rendering/renderer_rd/shaders/effects/ssao.glsl#L357): 반경 기반 mip와 importance 기반 추가 샘플 예산.

**확인된 차이:** HS의 공유 메모리는 인접 2×2 분류용 guide 재사용이다. 반경 방향의 horizon 샘플까지 타일에서 재사용하는 것은 아니다. HS horizon에는 반경에 따른 깊이 mip 선택이 없다. Godot은 선형 깊이 준비를 SSAO/SSIL이 공유하고 먼 샘플에는 mip를 사용한다. 그러나 Godot의 이 셰이더는 SSAO/ASSAO 계열이며 HS GTAO와 같은 차폐 수식이 아니다. tap 수를 그대로 대응시켜 효율이나 품질 우열을 판단하면 안 된다.

**성능 가설·최소 변경 제안:** 우선 GTAO 수식을 유지하면서 반복 샘플의 view-space Z 준비 비용을 별도 선형 깊이 입력으로 바꾸는 후보를 평가한다. 추가 전처리 dispatch/메모리 쓰기 비용까지 포함해야 하며, 다른 소비자가 동일한 해상도·jitter·유효 깊이 계약을 요구할 때만 공유한다. 기존 culling HZB는 보수적 min/max 목적이 다르므로 그대로 AO 입력으로 재활용하지 않는다. 그다음 반경 기반 mip는 별도 화질 변경으로 검토한다. 얇은 풀·울타리·물체 접촉부에서 차폐 손실/후광/깜빡임 위험이 있다. 품질 예산은 현재 anchor 비율과 resolve fallback 비율을 먼저 알아야 효과를 판단할 수 있다.

### A3. 이미 적용된 타일 공유는 중복 개선으로 세지 않는다

```text
HS / 그룹 내 인접 정보 공유
각 lane: authored normal + material + depth guide와 density 저장
barrier
공유 guide로 2×2 일관성 판정; 영역 밖 lane도 barrier 참여

Wicked / 그룹 내 인접 정보 공유
각 lane이 분담하여 border 포함 depth tile에서 위치 복원·저장
barrier
가까운 이웃 깊이로 삼각형을 골라 중심의 평면 법선 복원
그 법선을 이용해 AO hemisphere 샘플
```

Wicked [`ssaoCS.hlsl` 11–78, 91–119](https://github.com/turanszkij/WickedEngine/blob/cb4a81c853cbdf3b792ba539888a3353512ce52c/WickedEngine/shaders/ssaoCS.hlsl#L11), 같은 고정 SHA. 이 소스도 직접 읽었다. HS와 Wicked 모두 인접 정보의 groupshared 재사용을 한다. 저장 대상과 사용 목적이 다르며, Wicked는 깊이로 평면 법선을 복원하고 HS는 재질·저장 법선을 유지한다. 이를 바꾸면 normal map과 재질 경계 반응이 바뀐다. 따라서 현 HS 공유 타일을 다시 신규 최적화라고 제안하지 않는다. HS 타일 변경의 기존 GPU oracle(192 cases/39,936 texel pairs, bit difference 0)은 정확성 근거이며 실행시간 절감의 증거가 아니다.

권고 순서: **지형/POM 실행 비중 분리 → GTAO 선형 깊이 입력의 총비용 검토 → 원거리 재질 캐시·깊이 mip처럼 화질/메모리 계약이 바뀌는 후보 검토**. 현재 숫자만으로 어느 후보도 ms 또는 FPS 개선량을 산정하지 않는다.
## B. 지오메트리와 그림자

### 고정 소스

Wicked Engine SHA `cb4a81c853cbdf3b792ba539888a3353512ce52c` from earlier RESEARCH.md was reopened via official raw.githubusercontent.com (Invoke-WebRequest). Browser open failed cache lookup; direct official source retrieval succeeded. No claim that this historical SHA is today's latest head.

- [wiRenderer.cpp DrawShadowmaps 6699–6833](https://github.com/turanszkij/WickedEngine/blob/cb4a81c853cbdf3b792ba539888a3353512ce52c/WickedEngine/wiRenderer.cpp#L6699)
- [wiRenderer.cpp RenderMeshes 3143–3492](https://github.com/turanszkij/WickedEngine/blob/cb4a81c853cbdf3b792ba539888a3353512ce52c/WickedEngine/wiRenderer.cpp#L3143)
- [wiScene.cpp ComputeObjectLODForView 9013–9056](https://github.com/turanszkij/WickedEngine/blob/cb4a81c853cbdf3b792ba539888a3353512ce52c/WickedEngine/wiScene.cpp#L9013)

### B1. 같은 단위: 객체의 여러 그림자 뷰 선택 → LOD 결정 → draw 배치

### Wicked 실제 코드의 의사코드

```text
여러 light/cascade를 한 view 묶음으로 준비
for 객체:
    layer/renderable/cast-shadow/거리 조건에서 제외하면 skip
    for light 묶음: 구 경계에서 제외하면 skip
        for 해당 light의 뷰:
            cascade 정책과 AABB/frustum을 통과하면 view bit 설정
            shadow LOD override가 켜졌으면 뷰별 LOD 중 가장 상세한 것 선택
    view bit가 있으면 객체+mesh+LOD+view mask를 queue에 추가
queue 정렬
동일 mesh/LOD를 묶고 켜진 view bit별 instance-pointer만 추가
선택된 LOD의 subset을 instanced draw/mesh dispatch
```

DrawShadowmaps의 shadow LOD override는 조건부다. 항상 shadow 전용 LOD를 선택한다고 해석하면 안 된다. RenderMeshes에서 camera_mask를 실제 인스턴스로 펼치므로 multi-view batching은 그림자 vertex/raster 작업을 한 번만 한다는 뜻이 아니다.

### HS 현재 코드의 같은 수준 의사코드

```text
for hierarchy-enabled GPU candidate (meadow 제외):
    camera+3cascade mask를 HLOD mask와 교집합
    Count: hierarchy를 순회
        group sphere의 4개 뷰 frustum + projected error 검사
        활성 뷰의 최대 error ratio로 공통 cut 결정
        child residency가 완전하면 refine, 아니면 resident ancestor 유지
        page demand와 선택 cluster 수 기록
Prefix: 각 candidate의 정확한 출력 범위를 확보; capacity fault면 게시하지 않음
Emit: 같은 hierarchy/refinement를 다시 평가
    draw record/history와 4개 뷰 command를 생성; 비활성 command는 zero
이전 HZB occlusion → 현재 depth에서 재검사
CSM pass: atlas clear 후 cascade 0..2 각각의 indirect draw 실행
```

근거: `Content/Shaders/cluster_refinement.hlsl` Projection:308, Refinement:352, CountCuts:373, EmitCluster:499, EmitCuts:550. `renderer_cluster_graph.cpp:189,259,298,387`가 실제 Count/Prefix/Emit/HZB 패스를 등록한다. `renderer_frame.cpp:5279–5330`가 CSM pass와 3개 cascade draw loop를 등록한다.

**공통점:** HS에도 GPU-driven visibility, HLOD, 오류 기반 LOD, demand streaming, temporal occlusion/retest가 이미 있다. Wicked처럼 가장 상세한 활성 뷰 요구를 만족하는 공통 LOD/cut를 선택하는 방향이다. HS에 culling/LOD/batching이 없다는 진단은 틀리다.

**확인된 반복 작업:** HS Count와 Emit은 각각 Refinement→Projection 및 residency 검사를 수행한다. 단순 실수가 아니라 count/prefix/정확한 출력 배치와 fault 원자성을 위한 현재 설계다. 두 패스 시간을 그대로 절감 예상치로 계산할 수 없다.

**작은 후보:** Projection의 frustum plane 루프는 candidate mask에서 이미 제외된 뷰도 검사한다(현재 315–322의 4뷰×6plane 무조건 loop; 뒤 333–335의 projected-error loop에는 활성 mask skip이 이미 있음). 유효하지 않은 frustum 상수의 fault 검증을 dispatch 준비/균일 검증에서 보존한 상태로, 비활성 뷰의 공간 검사를 생략하는 것이 상대적으로 작은 후보다. 현재는 해당 루프가 상수의 finite 검사도 하므로 무작정 continue하면 검증 범위가 달라진다. CPU가 이미 전체 상수 유효성을 보장하는지 먼저 확인해야 한다. branch/divergence로 실제 이득이 없을 수도 있다.

**중간 후보:** Count에서 선택 group ID+view mask를 scratch에 저장해 Emit이 hierarchy를 다시 순회하지 않게 한다. 다만 scratch 예약, overflow zero-submit, 이전 frame history, page version 및 immutable residency 계약을 유지해야 한다. 추가 메모리 write/read 비용과 전환 비용이 있어 작은 패치로 단정하지 않는다. 먼저 방문 node 수/선택 group 수/활성 뷰 수를 측정하고 비용을 비교한다.

**보류:** Wicked의 객체-level CPU queue를 HS GPU cluster hierarchy 대신 그대로 이식하지 않는다. 분할 수준·streaming·정확성 계약이 다르며 CPU 병목을 다시 만들 수 있다. view별 독립 cut도 far cascade의 과한 detail을 줄일 가능성은 있지만 순회·출력·history가 늘어난다.

### B2. 같은 단위: LOD 선택 입력 → 선택한 실제 mesh 데이터

### Wicked

```text
LOD가 하나면 0
AABB 8개 corner를 해당 view로 투영
w를 양수 최소값으로 제한하고 화면 extent를 유한 범위로 제한
화면상 최대 폭/높이와 lod_bias로 LOD index 결정
해당 index의 subset range를 draw
```

ComputeObjectLODForView 9013–9056, RenderMeshes 3236/3441의 실제 연결이다. 이것은 screen-coverage 방식이며 HS의 보수적 geometric-error 방식과 품질 보장이 동일하지 않다. Wicked 방식으로 단순 치환하면 HS의 error tolerance 계약이 약해질 수 있다.

### HS meadow의 확인된 자산 문제

`tools/art/meadow/generate_surface.py:187–190`은 같은 vertices를 LOD0/1/2 이름으로 세 번 쓴다. 실제 `meadow_surface_meshes.json`을 읽고 vertices JSON SHA256을 비교했다:

|자산|LOD0/1/2 triangle count|3개 vertices hash|
|---|---:|---|
|meadow_ground|각 100370|각 `39a2a28031e44a17c9f0d18b72d71260dbd320c13d88f6fa4b9d722a4819b606`|
|meadow_water|각 2621|각 `4f4fbb4986c957ae7479cec3c32cf78256110d4aeb47461c38cadf79906bf424`|

```text
HS producer:
    정밀 ground/water vertices 생성
    for lod in 0,1,2: 동일 vertices를 다른 이름으로 저장
HS loader:
    모든 LOD metadata와 fallback 연결 검증
    LOD0 payload는 startup에서 열지 않고 streaming 대상으로 둠
    LOD1/2 complete payload는 읽음
```

loader 근거 `renderer_environment_mesh.cpp:237–260`. 따라서 “시작부터 3개 GPU 복사본 모두 상주”는 부정확하다. 정확한 문제는 complete fallback LOD1/2가 둘 다 full-detail이고, 이 경로에서 거리로 LOD index를 바꿔도 mesh triangle 수가 감소하지 않는다는 것이다. **현재 meadow는 hierarchy refinement에서 명시적으로 제외된다.** `renderer_frame.cpp:798`의 유일한 cluster_active 대입이 ordinal<kMeadowGeometryBegin(28) 조건이며, 839의 whole-LOD visibility 해제 및 1122의 cluster 등록도 이 bool을 따른다. 3544에서는 meadow가 whole-LOD geometry demand로 들어간다. 다른 override 대입은 없다. 일반 환경의 cluster cut 기능을 meadow의 비용 감소 근거로 사용할 수 없다. 다만 static visibility/whole-mesh frustum·occlusion 및 raster clipping 등이 있으므로 항상 100370 triangle을 모든 view에 실제 raster한다고 단정하지 않는다. 물 geometry는 5083/5101에서 opaque/shadow draw를 제외하고 7434/7557의 투명 경로로 간다.

개선 우선순위: 먼저 실제 감소하는 meadow ground LOD1/2를 생성한다. 외곽 y=0/법선, 수면 경계, foot/hoof/rut의 silhouette·높이와 CPU contact 오차를 검증해야 한다. water와 지면을 독립 decimate하여 물 밑 지면이 솟거나 물이 뜨는 것은 금지한다. 얕은 자국은 멀리서 normal/detail 표현으로 넘길 수 있지만 그 전환은 별도 검증이 필요하다. water 2621 triangles는 ground보다 작아 후순위다. 공통 완전동일 LOD dedup는 byte 절약 후보이지만 page ownership/version/fallback identity 수정이 필요하므로 geometry 생성 개선보다 반드시 작은 변경은 아니다.

### B3. 그림자 cache 존재 여부와 4.576ms 해석

HS `renderer_virtual_shadow.cpp:514–532`는 immutable/skin/wind/animated-alpha로 static 여부를 나누고 generation/revision/cut/bounds 변경만 static invalidation chain에 넣는다. VSM static/dynamic bank와 dirty page 경로가 이미 있다. CSM은 `renderer_frame.cpp:5299–5320`에서 atlas clear+3cascade raster를 수행한다.

`project_hs.hlsl:1344–1347` ShadowVisibility는 virtual lookup 성공 시 해당 결과를 사용하고 실패 시 아래 CSM fallback으로 간다. 그러므로 VSM이 존재한다는 이유만으로 CSM pass를 제거하면 miss/out-of-range shading이 깨진다. 반대로 “그림자 cache가 전혀 없어 느리다”도 틀리다.

Wicked의 이 함수는 light/cascade들을 view batch로 묶고 선택된 view bit만 instance pointer에 쓴다. HS CSM의 3-pass viewport 상태 변경을 줄이는 multi-view 전송은 조사 후보지만, raster work는 여전히 cascade별로 필요하며 mesh frontend/alpha cutoff/atlas viewport indexing/indirect ABI 변경이 따른다. 가장 작은 우선 개선으로 단정하지 않는다.

실제 4.576ms를 줄일 후보 판정에는 CSM fallback 사용률, 뷰별 caster/triangle 수, wind foliage shadow 비중이 필요하다. 미사용 cascade/caster를 제거하려면 receiver coverage와 fallback 필요성을 증명해야 한다. 기존 cache와 AO/CSM overlap을 보존하고, 위 LOD 자산/중복 projection부터 통제 측정하는 편이 근거가 명확하다.

### 확정과 가설

- 확정: meadow 3 LOD source vertices 동일 및 현재 cluster cut 명시 제외; Count/Emit hierarchy 평가 반복; Projection 비활성 view 공간검사도 수행; CSM clear와 3cascade draw; VSM dirty/static cache 존재; GPU-driven/HLOD/HZB/retest 존재.
- 가설: 각 후보의 GPU ms 절감량, far-cascade 과잉 detail 비중, multi-view batching 이득, Count scratch replay 이득. 소스만으로 값이나 FPS를 제시하지 않는다.
- 미수행: 수정, build, play, shader DXIL 비교, 새로운 GPU 측정. 본 결과는 다음 변경을 고르기 위한 코드 비교이며 완료된 최적화 보고가 아니다.



## C. 프레임 그래프와 CPU 기록·제출 — HS 대 O3DE

근거: HS `render_graph.cpp:2276–2590,1473–1483,1597–1632,1722–1784,1885–1891`, `renderer_frame.cpp:4259,8190–8212,8274–8322`; O3DE [FrameGraphExecuter.cpp:55–179](https://github.com/o3de/o3de/blob/85f846e64e9d03f4114e371dc5eb255b6484e717/Gems/Atom/RHI/DX12/Code/Source/RHI/FrameGraphExecuter.cpp#L55).

```text
HS, 프레임 단위:
  clear graph declarations; register enabled passes/resources/dependencies
  validate imports/accesses; construct exact structural key
  if key != last successful key:
    compile schedule + transient aliases; cache successful plan
  claim transient resources for this frame
  form command batches, including transitions/external operations
  in workers: record one list for each eligible batch; join
  for consecutive same-queue lists allowed by Enhanced/fence boundaries:
    queue wait → ExecuteCommandLists(group) → signal completion
  resolve timestamps after relevant queue work

O3DE DX12, scope 단위:
  for scope in frame graph:
    cost = estimated_items * item_cost + attachments * attachment_cost
    if cost/swapchain/queue/device/fence boundary breaks current group:
      flush merged group
    if small scope: append to merged recording group
    else: split scope into bounded parallel command-list contexts
  execute each group's work request on its hardware queue
```

**확인된 차이:** HS는 기록 리스트의 크기/작업량에 따른 병합이 없고, 안전한 경우 이미 만들어진 리스트들을 한 제출 호출에 묶는다. O3DE는 기록 단위의 비용으로 작은 scope를 묶거나 큰 scope를 나눈다. 둘은 같은 최적화가 아니다. HS에도 worker 기록과 계획 캐시가 있으므로 “직렬 기록만 한다/매번 무조건 컴파일한다”는 진단은 틀리다.

기존 캡처의 Playing/config3 652프레임을 다시 집계했다:

| CPU 또는 제출 지표 | 평균 |
|---|---:|
| graph 구성 | 4.975ms |
| graph 제출 | 3.950ms |
| 병렬 기록 wall time | 1.516ms |
| Direct / Compute / Copy 제출 그룹 | 191.166 / 30.997 / 25.224 |
| 기록 작업 수 | 420.089 |
| cross-queue wait 카운터 | 27.227 |

이들은 draw call 수가 아니다. 제출 비용은 약4ms로 개선 후보지만 그 전체가 제거 가능한 비용은 아니다. 전체 CPU 점유율4.564%는20 logical CPU 전체를 분모로 한 값이므로, 단일 프레임 생성 경로의 비용이 작다는 뜻도 아니다.

**제안:** 먼저 원인별 graph key miss와 batch 생성 원인을 기록해, 매 프레임 변하지 않는 선언 구성/불필요한 경계를 좁혀 제거한다. `PlanCompilationCount/PlanReuseCount`는 이미 있으나 현재 performance.csv에는 없다. compile 단계0.713ms만으로 캐시 실패 비율을 역산하지 않는다. 새 범용 graph나 무조건 scope 병합을 바로 도입하지 않는다. 이전 기록 병합은 실제 native binding 오류/드라이버 충돌 후 철회했으며, 단순히 O3DE와 닮았다는 이유로 되살릴 수 없다.

## D. 옵션 결합과 실행 순서 — HS 대 Wicked Engine

근거: HS `render_features.cpp:296–315`, `renderer_post_subgraphs.cpp:25–50`, `renderer_frame.cpp:8045–8057`, `project_hs.hlsl:4002–4022`, `hdr_post.hlsl`의 `HdrPostGradeCS`; Wicked [wiRenderPath3D.cpp:2448–2490](https://github.com/turanszkij/WickedEngine/blob/cb4a81c853cbdf3b792ba539888a3353512ce52c/WickedEngine/wiRenderPath3D.cpp#L2448)와 [tonemapCS.hlsl:110–195](https://github.com/turanszkij/WickedEngine/blob/cb4a81c853cbdf3b792ba539888a3353512ce52c/WickedEngine/shaders/tonemapCS.hlsl#L110).

```text
HS:
  bloom = extract → downsample → upsample
  full_resolution_A = sharpen_once(scene) + bounded_bloom_halo
  full_resolution_B = scene_linear_LUT(A)       # grading enabled
  display_linear = tone_map(B)
  then outline/UI/display encoding as defined by the graph

Wicked (SDR branch; HDR branch differs):
  bloom_pyramid = ComputeBloom(scene)
  one output shader per pixel:
    c = sample_distorted_scene() * exposure
    c += sample_bloom_mips()
    c = tone_map(c); c = sRGB_encode(c)
    c = optional_LUT(c)
    c = dither_and_color_adjust(c)
    store(c)
```

**확인된 차이:** HS에는 bloom 최종 합성·grading의 별도 전체 화면 중간 출력이 있다. Wicked는 준비된 bloom/luminance/LUT를 마지막 shader에 입력한다. HS의 SSR spatial+compose, guide+CoC는 이미 합쳐져 있어 추가 후보로 다시 세지 않는다.

**중요한 비동등성:** Wicked의 SDR LUT는 tone map/sRGB 뒤에 있고 HS는 scene-linear LUT가 앞이다. 외부 shader를 그대로 복사하면 색·밝기가 바뀐다. 도입 대상은 “최종 픽셀 계산을 한 shader에서 처리하는 구조”이며, HS의 수식 순서와 UI 경계는 보존해야 한다.

**제안:** 저해상도 bloom 필터는 유지하고 terminal shader 안에서 `sharpen once → halo → scene-linear grade → tone map` 순서를 보존하여 두 중간 출력 왕복을 제거하는 후보를 만든다. fp16 중간 반올림, HDR/SDR, FXAA/outline, UI 밝기, 옵션 off 경로는 검증 대상이다. 다만 기존 bloom 그룹0.050ms·grade0.020ms는 작아서, 이 결합만으로144FPS에 도달한다는 주장은 성립하지 않는다.

## E. 동시에 처리할 수 있는 범위

```text
공통 원칙:
  input producer completes → independent consumers may overlap
  consumer output completes → dependent consumer starts
  a dispatch has no general whole-GPU barrier inside it

HS already:
  GBuffer → adaptive guide classification → AO [Compute]
                                      └→ CSM [Direct, overlap conditions satisfied]
  opaque HDR → SSR trace → temporal → spatial+compose
  nearest guides+optional CoC → foreground dilation → DoF

Wicked:
  depth prepass → compute effects
  async preparation → shadow work (explicit wait)
  compute effects → graphics lighting (explicit wait)
```

HS 근거: `renderer_frame.cpp:6745–6768,6815–6825`. Wicked 근거: [wiRenderPath3D.cpp:1025–1028,1283,1439](https://github.com/turanszkij/WickedEngine/blob/cb4a81c853cbdf3b792ba539888a3353512ce52c/WickedEngine/wiRenderPath3D.cpp#L1025). 조건부 그림자·효과의 전체 그래프를 생략한 의존성 발췌다.

**판정:** 공유 가능한 준비 계산, 최종 동일 픽셀 합성, 독립 GPU 작업 중첩을 구별한다. SSR이 읽는 opaque HDR나 DoF가 읽는 motion-blur 결과가 아직 없는데 동시에 실행시키면 잘못된 결과다. 현재 AO/CSM의 실제 중첩 이득·경쟁은 queue timeline으로 확인해야 하며, 점유율을 높이는 것 자체가 성능 목표는 아니다.

## 검증 범위와 적용 판단

이번에는 공식 원문·현재 호출 경로·생성 자산 및 기존 CSV를 읽고 비교했다. 기존 source snapshot과 현재 파일을 혼동하지 않도록 주요 HS 파일을 해시로 고정했다. 코드 변경·새 GPU 캡처·게임 실행·빌드는 수행하지 않았다. 지면 경계 수정 자체는 이전 작업에서 동일 카메라180프레임/resize·validation0으로 확인됐으며, 이 코드 비교를 위한 성능 재측정은 아니다.

적용 판단은 **실제로 줄어드는 지면 LOD → 상위 GPU 비용의 세부 비중 → 검증 가능한 중복 계산 제거 → 작은 후처리 결합** 순서가 타당하다. 다른 엔진의 구조가 있다는 사실만으로 더 빠르다고 판정하지 않는다. 특히 HS의 화면 색 공간, geometry page/residency, count-prefix-emit 원자성, history, fallback 및 barrier 계약은 유지해야 한다.
