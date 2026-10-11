# 잔디 변형 히스토리 수정

## 수정 범위

첨부 보충 문서는 조사 자료로 대조했다. 실제 소스와 네이티브 출력에서 확인한 GSH-01/02 결함을 수정했으며, 기존 형태·밀도·바람·접촉 강도·그림자 품질은 유지한다.

- 공통 `BendGrass`를 현재/이전 시간과 bender 목록으로 평가한다. 일반 VS, mesh, visibility resolve가 같은 이전 정점 대응을 사용한다. 그림자·ray·probe capture는 현재 변형만 평가한다.
- 이전 32개 bender와 시간·개수·유효성은 뷰별 마지막 성공 렌더에 귀속된다. 성공한 Present/오프스크린 제출 후에만 게시하며 실패 시 무효화한다. GPU 대기나 새 히스토리 GPU 자원은 추가하지 않는다.
- 동적 잔디는 기존 이전 인스턴스 테이블과 슬롯 세대 검사를 사용한다. 정적 잔디는 동일한 강한 scene lease와 이전 geometry page를 요구한다. new-cut/fade/HLOD 및 TAA의 깊이·재질·노멀 탭별 거부는 유지한다.
- 정적 lease의 좌표는 이전 scene basis 그대로이고, CPU의 동적 이전 변환은 현재 basis로 변환되어 있다. 바람 위상은 각자의 이전 basis에서 평가한 뒤 이전 행렬이 받는 현재 basis로 환산한다.
- FrameConstants 기존 prefix offset은 그대로다. tail은 이전 bender @11728, 상태 @12240, 원점 차이 @12256, 총 12272B다. Exposure CB와 겹치지 않도록 instance upload 시작을 12KiB에서 13KiB로 옮겼다. 뷰별 CPU 상태는 528B, CB 증가분은 544B다.

## 수치 검증

화면은 추가로 열지 않았다. FHD 원시 readback의 잔디 표면을 분리해 연속 프레임을 비교했다. A 고정, B 바람, C 카메라, D 접촉·이동, E 거리 변화 진단이며, DRS OFF·sharpening 0 등 진단 설정을 정상 최고 설정과 구분한다.

| 관측 | 결과 |
|---|---|
| A–E, 11–16프레임의 현재 base/normal/depth/metadata | 수정 전후 120쌍 모두 바이트 일치 |
| 정상 잔디의 surface history 사용 비율 | 0% → 100% |
| A의 내부 표면 재투영 RGB 변동 평균 | 0.00201818 → 0.00021598, 약 89.3% 감소 |
| C의 같은 변동 평균 | 0.00196217 → 0.00041261, 약 79.0% 감소 |
| C의 카메라 모션 투영 오차 p95 | 약 0.00350픽셀 |
| CPU TAA oracle 대 GPU 출력 절대 오차 p99 | 약 0.00012 |
| 원점 0.25m 전환, 고정 인스턴스·접촉 목록 | 이전 셰이더 최대 52.85픽셀 거짓 모션으로 회귀 검사 실패; 수정 후 실제 바람 약 0.70픽셀로 통과 |

TAA footprint의 accepted weight와 depth/material/normal rejection은 저장된 GPU 입력으로 계산한 **CPU 유도값**이다. GPU 내부 카운터를 직접 읽은 값으로 취급하지 않는다. 실제 최종 GPU 색상과 oracle을 대조했다. 움직이는 바람의 정당한 변화를 flicker 감소율에 섞지 않도록 감소율은 A/C에서만 보고한다. 접촉 잔상 길이와 LOD density pop의 정량화는 완료 증거에 포함하지 않는다.

회귀 fixture는 첫 렌더·카메라 컷·resize 후 confidence 0, 정상 렌더 후 회복을 검사한다. 다른 시간으로 드물게 렌더하는 Preview의 이전 시간 10.1초가 Main의 시간으로 덮이지 않는지도 검사한다. 60/120/144 수치는 논리 시간 입력이며 실제 디스플레이 cadence를 뜻하지 않는다.

201개 워밍업 샘플의 진단 장면 GPU critical 중앙값은 6.752256 → 6.779904ms, CPU render 중앙값은 5.0370 → 5.1379ms였다. 한 쌍의 측정이며 GPU critical에는 큐 대기·유휴 구간이 포함된다. 이전 변형만의 단독 비용, 32-bender 최악 부하 또는 정상 게임플레이 FPS로 해석하지 않는다. 생성된 SceneVS DXIL에는 임시 `alloca`가 없다.

원시 수치 근거는 `Build/msvc-validation/Artifacts/grass-history-{before,after}-*`, `grass-history-warm-*`, `grass-history-negative-origin`, `grass-history-final-gbv-mesh-D144`에 있다. 요약은 `Build/verification/grass-history/current-surface-equivalence.json`, `warm-performance.json`이다. 원점 보정 전 연속 비교 Generation은 `bc7e8eb...`, 원점 보정 후 검증 Generation은 `7736f5b...`다. 원점 차이 0인 기존 연속 비교와 원점 전환 회귀를 구분한다.

## 남은 조건

Release와 검증용 ProjectHS를 빌드했다. 최신 runtime-full `eaa38c805f6112b43e496d7e53a678c9e5175ece2342cb95ed2c0e3c649f880f`를 Release에 export하고 3,680개 제품을 Verify했다. mesh+visibility D144의 다중 뷰·원점·resize 검사도 GPU validation 오류 0으로 통과했다(223.35초, `grass-history-final-gbv-mesh-D144`). D120 일반 네이티브 실행과 10개 셰이더 계약·2개 분석 자체 검사도 통과했다. 최종 선택 통합 검사 5/5도 통과했다(134.88초, `Build/verification/verify-20261010T161204504Z-19116.json`): grass-history, shader-reconstruction-contracts, barriers, local-probe-revisions, cluster-retest. 검증 빌드는 RelWithDebInfo이며 Debug hot-reload 재검사로 간주하지 않는다.

- 외부 SR SDK는 현재 Release/검증 빌드에서 모두 OFF다. confidence로 인한 전 잔디 responsive=1 경로는 소스에서 해소했지만 실제 제공자의 입력 소비는 실행 검증하지 않았다.
- 기존 atlas coverage 보존, dilation, normal variance roughness AA와 6개 foliage mip는 유지했다. 압축 DDS별 coverage·resident/requested mip와 원거리 잔여 증상은 별도 근거가 필요하다. mip 추가나 밀도·바람 감소를 적용하지 않았다.
- 단일 스크린샷으로 전 상황의 무깜빡임을 선언하지 않는다. 정상 최고 설정의 실제 144FPS 달성도 이 진단으로 입증되지 않는다.
