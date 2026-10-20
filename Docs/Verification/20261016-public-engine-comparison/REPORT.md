# 공개 엔진 비교와 AO 개선 — 2026-10-16

## 범위와 출처

공식 저장소의 최신 공개 개발 HEAD를 조회하고 아래 커밋의 소스를 고정하여 비교했다. 안정판 전체 엔진의 전수 리뷰나 엔진 간 성능 벤치마크는 아니다. 최근 Project HS 측정에서 비중이 컸던 AO, 그림자, GBuffer, CPU 명령 기록에 집중했다. 원본은 `Build/engine-comparison/`에 보존했다.

| 엔진 | 확인한 개발 커밋 | 커밋 날짜 |
|---|---|---|
| Godot | `4d10419cacd6d3009976e8f9fd3abd970fd58018` | 2026-10-15 |
| Wicked Engine | `54518f2ca9a4c37716f6e2fe5b96a3b276944b0a` | 2026-10-15 |
| O3DE | `74006f539d581ea2ae8d21d4ef629d7a610a3e01` | 2026-10-14 |

## 차이와 판단

| 영역 | 공개 엔진 코드 | Project HS와의 차이 | 처리 |
|---|---|---|---|
| AO 위치 복원 | [Godot SSAO](https://github.com/godotengine/godot/blob/4d10419cacd6d3009976e8f9fd3abd970fd58018/servers/rendering/renderer_rd/shaders/effects/ssao.glsl#L132), [Wicked SSAO](https://github.com/turanszkij/WickedEngine/blob/54518f2ca9a4c37716f6e2fe5b96a3b276944b0a/WickedEngine/shaders/ssaoCS.hlsl#L24)는 뷰 공간을 직접 복원 | HS는 샘플마다 카메라 상대 월드 공간을 복원한 뒤 다시 뷰 공간으로 회전 | **직접 역투영 적용**. 같은 AO 알고리즘이라는 뜻은 아니며 좌표 복원 경로만 비교 |
| AO 깊이 접근 | Godot은 depth mip와 tap 거리별 LOD, Wicked는 depth mip와 공유 메모리 타일 사용 | HS의 반복 깊이 접근 최적화 후보 | 보류. 얇은 물체/경계 AO와 추가 준비 패스 비용을 함께 검증해야 함 |
| 그림자 갱신 | [Godot shadow atlas](https://github.com/godotengine/godot/blob/4d10419cacd6d3009976e8f9fd3abd970fd58018/servers/rendering/renderer_rd/storage_rd/light_storage.cpp#L2622)는 버전으로 재그리기 판단 | HS도 이미 epoch, 변경 caster, 정적/동적 페이지를 구분 | 캐시를 중복 도입하지 않음. 기존 요청 비트셋 최적화의 실제 성능 검증이 먼저 |
| 깊이 선행 패스 | [Wicked RenderPath3D](https://github.com/turanszkij/WickedEngine/blob/54518f2ca9a4c37716f6e2fe5b96a3b276944b0a/WickedEngine/wiRenderPath3D.cpp#L938)는 opaque depth prepass 사용 | HS GBuffer는 깊이와 6개 MRT를 함께 기록하며 조건부 material sidecar 존재 | 전면 도입 보류. 선택적 opaque prepass 또는 앞→뒤 정렬 후보. 추가 정점/드로 비용과 alpha/POM/모션 정합성을 먼저 확인 |
| 작은 패스 명령 기록 | [O3DE DX12 executor](https://github.com/o3de/o3de/blob/74006f539d581ea2ae8d21d4ef629d7a610a3e01/Gems/Atom/RHI/DX12/Code/Source/RHI/FrameGraphExecuter.cpp#L91), [merged group](https://github.com/o3de/o3de/blob/74006f539d581ea2ae8d21d4ef629d7a610a3e01/Gems/Atom/RHI/DX12/Code/Source/RHI/FrameGraphExecuteGroupMerged.cpp#L77)는 비용과 경계에 따라 한 명령 리스트에 여러 scope 기록 | HS Enhanced 경로는 제출을 합치지만 기록은 batch별 allocator/list 사용 | 다음 CPU 후보. 큐/fence/owner callback 경계, 패스별 timestamp, Legacy 경로를 보존해야 함 |
| 기록 완료 즉시 제출 | [O3DE 공통 executor](https://github.com/o3de/o3de/blob/74006f539d581ea2ae8d21d4ef629d7a610a3e01/Gems/Atom/RHI/Code/Source/RHI/FrameGraphExecuter.cpp#L72)는 준비된 그룹을 제출 | HS는 모든 기록 결과를 확인한 후 제출하며 기록 실패 시 제출 0을 보장 | 이번 변경에서 배제. 단순 이식하면 실패 시 자원 수명/회수 계약을 깨뜨림 |

HS 근거: `renderer_pipeline.cpp`의 GBuffer PSO, `renderer_virtual_shadow.cpp`의 epoch 유지 및 `virtual_shadow.hlsl`의 changed-caster 경로, `render_graph.cpp`의 batch별 기록·전체 join·Enhanced 제출 병합. 이미 있는 schedule/alias-plan 캐시를 새 최적화로 제안하지 않았다.

## 적용한 변경

- `Content/Shaders/ambient_occlusion.hlsl`: depth→camera-relative world→view 대신 depth→view 직접 복원.
- `renderer_ambient_occlusion.hpp`: 첫 행렬을 `inverse_projection`으로 변경. 상수 버퍼 368바이트와 오프셋 유지.
- `renderer_frame.cpp`: 현재 jitter가 포함된 projection의 역행렬 전달. 법선/광원용 world-to-view 유지.
- `Tests/check_shader_reconstruction.py`: 투영 방식, 깊이 방향, jitter, yaw/pitch, eye 이동을 조합한 384개 위치의 기존 수식/직접 수식 동등성 확인.

AO 샘플 수·해상도·필터·히스토리 거부 조건은 유지한다. 외부 엔진 의존성이나 새로운 GPU 리소스는 추가하지 않았다.

## 컴파일 결과

동일 DXC `cs_6_0 -O3 -Ges -WX`로 변경 전후 6개 entrypoint를 컴파일했다. 근거: `Build/engine-comparison/ao-view/comparison.json` 및 `before/after`의 소스·DXIL·disassembly.

| entrypoint | 정적 SSA 명령 전→후 | FMad 전→후 |
|---|---:|---:|
| GTAOHorizonCS | 1301→1154 | 104→62 |
| GTAOTemporalCS | 1421→1316 | 70→40 |
| GTAOBilateralCS | 252→231 | 14→8 |
| ContactShadowCS | 423→381 | 46→34 |
| AdaptiveGtaoHorizonCS | 4859→4187 | 454→262 |
| AdaptiveGtaoResolveCS | 8984→7682 | 874→502 |

Adaptive horizon 정적 명령은 약 13.8% 감소했다. 이것은 최적화된 DXIL의 정적 집계이며 GPU ISA, 동적 실행 횟수, GPU 시간 또는 FPS 개선율이 아니다. 메모리 대기와 다른 패스 비용은 그대로 남는다.

## 검증

- 기존 `rendering.shader-reconstruction-contracts`: 통과, 내부 31개 테스트. CPU 수치/소스 계약이며 GPU float 결과 자체를 증명하지는 않는다.
- 6개 shader entrypoint 변경 전후 컴파일 통과.
- 통합 빌드/네이티브 결과는 아래 완료 기록 참조.

이번에는 실제 플레이 FPS를 재측정하지 않았다. 장시간 플레이, 전체 엔진 전수 검토, 생산 VSM GPU-based validation, Brixelizer OFF 상태의 software-GI 실행은 검증 범위 밖이다. 144 FPS 목표 달성은 확인되지 않았다.

### 통합 완료 기록

- validation content/ProjectHS, Release ProjectHS, Debug ProjectHS 빌드 성공.
- Debug/Release runtime-full snapshot export 및 verify 성공: `b1f5d33dd42e1bcee3d7b908e6dd461d66d9785efed5b11ec0a62c2ef5f46a06`, 2699 products.
- `integration.render-smoke` 1/1 통과, 22.26초. offscreen 180 frames/ticks, resize/exit 경로. `execution_valid=true`, `assertions_passed=true`.
- 네이티브 산출물: `Build/msvc-validation/Artifacts/render-baseline/8d63afca7ac0172c69127f55d88f1de153d7cad062a2d7405a8bd3c0f215f01b/`.
- 빌드·검증 로그: `Build/engine-comparison/`. `git diff --check` 통과.

네이티브 smoke는 실행 경로의 회귀 검증이며, 전후 GPU 이미지의 수치적 동일성이나 실제 플레이 성능 측정은 아니다.
