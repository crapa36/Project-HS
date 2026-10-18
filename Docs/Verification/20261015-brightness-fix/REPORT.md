# 화면 밝기·ImGui·잔디 바닥 색 수정

## 원인과 변경

- **ImGui:** 비 SDK 표시 경로에서 sRGB 색을 linear 화면에 직접 그린 뒤 다시 sRGB로 인코딩했다. 기존 PostA를 투명 DevTools 출력으로 재사용하고, 기존 premultiplied sRGB decode 후 최종 UI 합성에서 한 번만 인코딩하도록 수정했다. SDK 경로의 기존 처리와 색 공간을 맞췄으며 새 텍스처/PSO는 추가하지 않았다. 그래프의 이전 PostA 읽기 → DevTools 쓰기 → UI 읽기 의존성을 명시했다.
- **SDR 장면 밝기:** 기존 흙 텍스처, 노출, ACES+sRGB 계산에서 전역 이중 gamma 오류는 발견되지 않았다. 사용자가 지적한 과도한 밝기는 별도 미술 조정으로 처리했다. ACES 입력에 0.7231708(약 -0.468 EV)을 적용해 입력 회색 0.18이 출력 linear 0.18이 되도록 했다. UI에는 적용하지 않는다. HDR 톤매핑은 변경하지 않았다.
- **풀 바닥:** 기존 4층 혼합에서 흙/마른 흙/자갈이 섞여 잔디와 다른 회백색이었다. 혼합 완료 후 기존 길/풀 가중치로 풀 영역만 잔디 atlas의 linear 평균색에 맞춘다. atlas 두 장의 alpha >= 0.5 평균은 (0.099848532, 0.159184398, 0.020007386), 기존 grass-soil 평균 휘도는 0.146997588이다. 텍스처 명암 비례, 4층 샘플/높이 혼합, normal/roughness/AO를 유지한다. 순수 흙길, DirtPatch mesh20, 풀 가중치 0은 원래 색을 유지한다. 잔디 메시와 지면의 방향·조명·그림자 차이까지 제거하지는 않는다.
- 기존 녹색 texel만 보정하는 중간안은 전체 지면의 6.1%만 대상으로 삼아 부족했다. 최종안에서 완전히 제거하고 보정을 한 번만 적용한다.
- terrain 공통 include를 사용하는 BSDF preintegration 생성의 입력 hash와 CMake 의존성도 추가하여 셰이더 변경이 캐시에 누락되지 않도록 했다.

## 검증

- `Tests/check_shader_reconstruction.py`: 24/24 통과. UI 색 왕복/반투명 처리, SDR 회색/단조성, 흙 경로 보호/풀 색조/명암 비례와 공통 셰이더 의존성을 포함한다. 이 검사는 native 실행을 대신하지 않는다.
- Debug 및 Release 빌드 성공, runtime-full 각각 3,680 제품 검증 성공.
- RTX 4080, Debug, 1280×720, 동일 seed/장면/56개 옵션의 숨겨진 3프레임 실행. 일반 D3D12 validation 켬, GPU-based validation 끔. 최종 25.13초, exit 0, execution_valid/assertions_passed=true, D3D12 오류 0, 체크섬 12633401072068484404 일치. 진단 수집 완료.
- 최종 PNG 직접 확인: ImGui 제목 막대 원색 복원, 중앙 흙길 갈색 유지, 풀 바닥 녹색 반영.

| 같은 화면 위치 RGB8 | 수정 전 | 최종 |
|---|---|---|
| ImGui 제목 막대 (450,70) | 112,147,184 | 41,74,122 |
| 흙길 (580,500) | 173,153,137 | 150,129,114 |
| 풀 바닥 (800,400) | 190,182,165 | 138,166,97 |

위 값은 고정 위치 비교이며 화면 전체 평균이나 모든 조명의 결과가 아니다.

## 빌드 식별

- Runtime generation: `e67ef6c28fe1e619b3247e9e156d88c1df089233b713dd03280eb626c12d7f3a`
- Debug SHA256: `9B516DF484E0DAA7D9C402C96C9127514AC40C672AD52E95CA03E075C9738495`
- Release SHA256: `D0284AC4912F1870D1AF1F505847EE4C20FC37A2B0F908F12014E7FEEF7D5306`

## 증거와 한계

`Build/brightness-fix/` 아래:

- `before-debug/`: 수정 전 원본 캡처
- `after-debug-validation/`: UI/SDR와 중간 지형 보정의 일반 검증
- `final-debug/`: 최종 PNG, result/render/diagnostics, 설정·GPU 패스·CPU 기록, raw 표면, generation/binary/run 기록
- `final-comparison.json`, `final-shader-contracts.log`, `final-build.log`, `final-cook.log`, `final-msvc-{debug,release}.log`
- `gpu-validation-incomplete.json`: 전체 장면 GPU 기반 검증 120초, 축소 preview 90초 시간초과. 두 실행은 강제 종료했고 완료/오류 집계가 없어 통과로 간주하지 않는다. 원본 `after-debug/`, `imgui-gbv-preview/`를 보존한다. 시간초과 원인은 확정하지 않았다.

Computer Use를 재개하지 않았다. 최종 테스트 프로세스는 종료했다. HDR/SDK/최고 RT 옵션의 최종 시각 검증, 장시간 플레이, 완전한 GPU 기반 검증은 완료하지 않았다. 이 짧은 색상 검증으로 FPS 개선이나 최고 옵션 144 FPS 달성을 주장하지 않는다.
