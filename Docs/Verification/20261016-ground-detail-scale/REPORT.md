# 지면 배치물 크기 수정

사용자가 지적한 캐릭터 대비 작은 울타리·꽃·나뭇가지를 공통 배치 함수에서 확대했다. 원본 mesh, LOD, UV, 지면 피벗은 유지하고 균등 배율만 바꿨다. 캐릭터의 실제 mesh 높이는 이번 조사로 확정하지 않았으므로 아래 수치를 캐릭터 키 대비 비율로 해석하지 않는다.

| 대상 | 기존 배치 크기 | 변경 후 배치 크기 | 추가 배율 |
|---|---|---|---:|
| 울타리 높이 | 0.675–0.788m | 1.013–1.181m | 1.5 |
| 데이지 높이 | 0.277–0.398m | 0.415–0.597m | 1.5 |
| 미나리아재비 높이 | 0.288–0.414m | 0.432–0.621m | 1.5 |
| 제비꽃 높이 | 0.199–0.287m | 0.399–0.573m | 2.0 |
| 나뭇가지 묶음 길이 | 0.485–0.667m | 0.728–1.001m | 1.5 |
| 클로버·낙엽 | 기존 크기 | 유지 | 1.0 |

수치는 원본 LOD0 bbox와 허용 배율 범위 기준이다. 배치 난수/여유거리 필터 때문에 실제 씬이 양 끝값을 정확히 포함하는 것은 아니다. 울타리 폭 역시 균등하게 2.43–2.835m로 증가한다.

확대 후 최종 배율을 통로·경계·장애물 여유거리 계산에도 사용한다. 겹치는 후보는 기존처럼 배치에서 제외되므로 개수가 감소할 수 있다. 서로 다른 detail patch 간의 겹침 검사는 기존에도 없으며 이번에 새로 추가하지 않았다.

## 검증

- simulation.presentation-contract, content.scene-hlod-cooker 2/2 통과(6.47초).
- 테스트는 종류별 배율 범위, 확대된 반경을 포함한 통로 이격, 균등 배율, 모든 종류 존재, 안정 ID/배치 예산, runtime/cook digest 일치를 확인한다.
- Receipt: Build/verification/verify-20261016T020358002Z-20708.json.
- 최초 테스트 실행은 CLI 배열 인자와 테스트명 지정 오류로 실행되지 않았다. 올바른 이름으로 위 검증을 완료했으며 실패 receipt도 보존했다.
- 빌드·콘텐츠 배포·native smoke 결과는 Build/ground-detail-scale/에 기록한다.

## 별도 진행한 VSM 변경

VSM은 기존 요청 비트셋을 순회해 불필요한 page×caster dispatch를 줄였다. 새로운 버퍼/ABI 없이 count/scatter dispatch Y=entries에서 words로 바꾸고 admission의 오름차순을 유지한다. 관련 native multi-queue test와 production CullCS DXC 컴파일은 통과했다.

성능 비교는 채택 근거로 사용할 수 없다. before01은 검증과 겹쳐 제외, before02는 Playing 729프레임 중727프레임이 background였다. after01은669프레임 모두 foreground, 기본29.662/SDK59.279 FPS, GPU30.474ms다. 조건이 다르므로 전후 FPS와 패스 차이를 최적화 효과로 단정하지 않는다. 원본은 그대로 보존했고 Build/vsm-request-walk/comparison.json에 한계를 기록했다. 크기 수정은 이 두 측정 이후 적용했다.

144 FPS 목표는 미달이며 Brixelizer OFF에 따른 GI fallback, 실제 production VSM의 GPU-validation 및 동일 foreground 조건의 성능 검증이 남아 있다.

최종 결과: Debug/Release 빌드와 양쪽 export/verify 성공. runtime-full generation `d9ac1dbc3f8373da3cbb567bd93ef514eb5191681382f5fe0faf6fb3d851e4a6`, 2699 products. 180프레임 네이티브 렌더·리사이즈·종료 smoke 1/1 통과(22.53초), execution_valid/assertions_passed=true. 확대된 울타리와 꽃이 포함된 smoke PNG를 확인했다. 이 offscreen smoke는 실제 플레이 성능이나 모든 배치물의 시각 품질 검증을 대신하지 않는다.
