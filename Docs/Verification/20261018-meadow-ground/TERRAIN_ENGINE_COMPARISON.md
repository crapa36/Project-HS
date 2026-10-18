# 실제 공개 엔진 지형 생성과 HS 비교

2026-10-18 GitHub API에서 확인한 HEAD를 고정하여 공식 raw 코드를 읽었다. Terrain3D는 Godot 본체가 아닌 TokisanGames의 Godot 지형 확장이다. 생산 코드 수정 없음.

## 직접 확인한 함수·소스

**Terrain3D — `59bacc5e61aede7d0fd96ae9cd263f4cc0f09858`**

- [`main.glsl::vertex()` 185–246](https://github.com/TokisanGames/Terrain3D/blob/59bacc5e61aede7d0fd96ae9cd263f4cc0f09858/src/shaders/main.glsl#L185): 평면 clipmap 좌표를 월드 좌표로 변환한다. LOD 경계에서 XZ 정점을 coarse grid 쪽으로 이동시키며 두 높이 샘플도 같은 morph 인자로 혼합한다. 단순히 삼각형 수만 줄이는 LOD가 아니다.
- [`Terrain3DData::get_surface_height()` 722–740, `get_modified_height()` 746 이후, `get_normal()` 807–826](https://github.com/TokisanGames/Terrain3D/blob/59bacc5e61aede7d0fd96ae9cd263f4cc0f09858/src/terrain_3d_data.cpp#L722): 정점 가까이는 원본 높이, 그 외에는 주변 네 높이를 bilinear 보간한다. 수정 높이는 hole/background도 고려한다. 법선 역시 이 높이 질의를 이용한다.
- [`terrain_3d_instancer.cpp` 516–540, 700](https://github.com/TokisanGames/Terrain3D/blob/59bacc5e61aede7d0fd96ae9cd263f4cc0f09858/src/terrain_3d_instancer.cpp#L516): 인스턴스 배치에서 surface height를 질의하고 선택적 raycast 결과를 적용한다. 법선도 data의 get_normal 또는 raycast를 사용한다.
- [공식 shader design](https://github.com/TokisanGames/Terrain3D/blob/59bacc5e61aede7d0fd96ae9cd263f4cc0f09858/doc/docs/shader_design.md): height/control/color map 분리, control의 base/overlay texture와 blend, fragment에서 높이 기반 법선 계산. 이번에 읽은 핵심 함수에는 도로 spline 생성 자체가 없으므로 Terrain3D가 도로를 특정 방식으로 자동 생성한다고 주장하지 않는다.

**Wicked Engine — `cb4a81c853cbdf3b792ba539888a3353512ce52c`**

- [`wiTerrain.cpp::Terrain::Generation_Update()` 내 chunk 생성 1105–1224](https://github.com/turanszkij/WickedEngine/blob/cb4a81c853cbdf3b792ba539888a3353512ce52c/WickedEngine/wiTerrain.cpp#L1105): 이웃을 포함한 높이 격자를 먼저 만들고 world-position modifier들을 적용한다. 이후 spline의 가장 가까운 위치·거리로 높이를 변형하고 **같은 splinefactor에서 재질 blendmap을 저장**한다. 최종 높이 이웃으로 법선을 만들고 경사/고도 레이어를 계산한다. 식생 밀도에도 저장된 spline blendmap이 들어간다.
- [같은 파일 1253–1256, 1358–1398](https://github.com/turanszkij/WickedEngine/blob/cb4a81c853cbdf3b792ba539888a3353512ce52c/WickedEngine/wiTerrain.cpp#L1358): grass는 생성 mesh를 참조하고 props는 실제 LOD0 삼각형의 barycentric 좌표로 접지한다.
- [같은 파일 48–192](https://github.com/turanszkij/WickedEngine/blob/cb4a81c853cbdf3b792ba539888a3353512ce52c/WickedEngine/wiTerrain.cpp#L48): LOD별 index 범위를 생성하며 coarse 내부와 별도로 네 border를 생성한다. 이번 조사에서 border 각 index의 수학적 watertightness까지 증명한 것은 아니다.

## 같은 단계로 본 차이

|계약|Terrain3D|Wicked|현재 HS|
|---|---|---|---|
|높이 원천|region heightmap와 modifier를 CPU/GPU가 질의|world modifier → spline 보정 → chunk 높이 격자|Python 높이 함수가 mesh를 만들고 같은 record 표를 C++/HLSL로 출력|
|LOD 경계|XZ+높이를 함께 geomorph|LOD별 내부/index border 생성|24×24m patch의 adaptive mesh, macrocell 경계 샘플 보존, wet support 보존|
|법선|동일 높이 기반 질의·fragment 재구성|최종 높이 격자의 인접 샘플|generator의 height 미분; 재질 normal/POM은 별도 미세 표면|
|레이어/길|control map의 base/overlay/blend; 이번 도로 생성 미확인|같은 splinefactor가 높이와 재질 마스크를 함께 구동|EnvTerrain의 path와 MeadowPressed, CPU grass 제외/배치가 각 경로에서 사용됨|
|식생 접지|surface height/normal API|실제 지형 mesh·삼각형 보간|environment_layout.cpp의 append가 MeadowGroundHeight를 더함; 배치 제외는 MeadowPressed/WaterLevel|

HS 직접 확인 위치: `tools/art/meadow/generate_surface.py` height/vertex/optimized_ground 및 생성 C++·HLSL 문자열; `Content/Shaders/material_terrain.hlsli` EnvTerrain; `Source/Presentation/Private/environment_layout.cpp` 81–82, 277–293. 동시 변경 중이므로 줄 번호는 이동할 수 있다.

## 바로 적용할 문제와 작은 수정 방향

1. **Wicked에서 직접 가져올 원칙은 도로의 단일 공간 계약이다.** 도로 중심선·폭·전이 마스크를 하나의 정의에서 내보내고 흙색, 수레자국 범위, 풀 제외, 높이 변형이 이를 같이 사용해야 한다. HS의 기존 MeadowPressed는 수레 바퀴의 좁은 띠와 보행 띠의 max라서 도로 전체 폭과 같은 의미가 아니다. 이를 도로 mask로 그대로 쓰면 바퀴 사이가 초지이거나 별도 길 셰이더와 경계가 달라질 수 있다. 도로 면적 mask와 눌린 세부 흔적 mask를 구분하되 동일 중심선·폭에서 도출한다. 부모가 진행 중인 roadwidth 일원화와 맞는 수정이다.
2. **접지 함수와 실제 삼각형의 차이를 제한한다.** HS는 같은 analytic height 원천을 쓰지만 mesh 내부는 선형 삼각형, CPU 높이는 analytic 함수이므로 동일 원천만으로 모든 위치의 높이가 정확히 같은 것은 아니다. 강한 rut/융기를 추가하면 식생 떠오름 위험이 커진다. 변형 구역 tessellation 유지와 임의 위치의 mesh-vs-contact 오차 oracle로 먼저 제한한다. 오차가 클 때만 로컬 triangle interpolation 질의를 도입한다. Terrain3D도 bilinear를 쓰므로 '타 엔진은 언제나 렌더 삼각형과 완전 동일'이라고 일반화하지 않는다.
3. **물·법선은 최종 높이에서 다시 계산한다.** Wicked가 spline 이후 높이로 법선을 만드는 것처럼, rut/융기 수정 후 Python·C++·HLSL 높이 계약과 clipped water/depth를 함께 재생성한다. 기존 perimeter=0, wet support, 공유 edge·면적 테스트를 유지한다. LOD가 더 적은 삼각형을 쓴다는 사실만으로 접지·물 경계가 보존된 것은 아니다.

## 지금 불필요한 개편

576m² patch 때문에 카메라 중심의 무한 clipmap, 대규모 terrain streaming, sparse virtual texture와 편집기 전체를 도입할 근거는 없다. 현재 adaptive mesh의 경계·접지·도로 마스크를 맞추는 것이 우선이다. Terrain3D의 per-fragment 높이 법선이나 Wicked의 VT도 추가 샘플/메모리/갱신 비용이 있어 무조건 더 빠르지 않다. 이번 조사는 구조 근거이며 새 GPU 성능 또는 AAA 시각 완성 검증은 아니다.