# 패스 병목 최종 재측정 — 2026-10-18

현재 결과는 기본 29.16 FPS, DLSS 2배 프레임 생성의 SDK present 관측 58.31 FPS다. 144 FPS 미달이다. 기준 대비 +0.617%의 단일 측정으로 명확한 성능 향상을 입증하지 못했다. 물리 디스플레이의 실제 표시 간격은 측정하지 않았다.

## 적용 및 철회

- AO의 동일한 반경·감쇠 계산을 반복문 밖으로 이동했다. RTX 4080 GBV 비교 192개 경우 / 39,936개 texel 쌍에서 비트 차이 0이다. 이것은 정확성 증거이며 이번 실제 플레이에서 AO 시간 개선은 입증하지 못했다.
- Cluster Emit은 기존 admission/fault guard 이후 예상 출력이 정확히 0/0인 노드의 작업을 생략한다. 기존 count/highword·offset 검사를 유지했다. `rendering.cluster-retest`가 통과했으며 손상된 GPU count를 직접 주입한 검사는 아니다.
- Local probe는 자체 static CSM을 사용한다. 실제 선택된 프로그램을 포함한 58개 DXIL에 VSM binding이 없음을 확인했다. Probe bounds는 기존 Z 구간 계산에 남기고 VSM 페이지 요청만 생략한다. 공유 메모리 barrier는 모든 lane이 통과한다. Water와 gel의 실제 수신 범위는 보수적인 union AABB로 추가했다. GPU payload 32B, receiver 제한 256, capture reads·roots·queues·barriers는 유지했다. 전체 FPS 또는 VSM 시간 감소를 입증하지 못했다.
- Terrain 레이어 좌표 준비 공유 후보는 실제 GBuffer가 7.068→8.442ms로 악화되어 철회했다. 원본 셰이더 SHA256 `1AC5BB26C387EC5874461E7DE194DA36E72BC8E3F3636B78B476D611CFD90E4E` 복원 후 최종 GBuffer 7.066ms로 기준 수준에 돌아왔다.

## 같은 플레이 구간 비교

1920×1080, 남은 비 RT 옵션 최고, native frame generation 2배. Foreground Playing의 tick 599–1380 필터를 사용했다. 기준 378프레임, 최종 380프레임이며 렌더 프레임마다 tick이 일대일 대응하는 표본은 아니다. 기록 입력과 simulation checksum `14666118289919368357`은 같다.

|항목|기준 01|최종 03|
|---|---:|---:|
|기본 FPS|28.978|29.157|
|SDK present FPS|57.956|58.305|
|GPU critical path|31.652ms|31.470ms|
|CPU 전체 20 logical processor 기준|7.955%|3.857%|
|PID GPU busiest engine|61.450%|61.755%|
|NVIDIA 전체 장치 GPU|94.462%|95.385%|
|유효 PDH interval coverage|86.474%|94.459%|

CPU 전체 비율은 단일 코어의 포화도를 뜻하지 않는다. PID의 busiest engine과 전체 장치 GPU는 다른 지표다. 최종 PDH 수집 오류 1건으로 사용률 데이터는 partial이다. 원본 capture-session의 incomplete/foreground_verified:false 표시는 수정하지 않았다. 별도 감사에서 유효 foreground 구간을 확인했다. 실제 게임 기록은 complete, GPU 1378/1378, invalid/drop 0, drain 성공이다.

52개 requested/native_effective 설정은 같지만 policy effective의 FG multiplier 표시는 1→2로 달랐다. 실제 SDK 관측의 native multiplier는 양쪽 모두 2다. CPU 변경으로 실행파일 SHA가 달라졌고 Generation의 AO 3개 DXIL, cluster Emit, VSM feedback, local_probes.spkg digest가 달라졌다. Probe payload의 의미상 동일성까지 검증하지 않았으므로 순수 단일 셰이더 A/B로 주장하지 않는다.

## 남은 비용 — 최종 평균, 무거운 순

|개별 패스|ms/frame|
|---|---:|
|GBuffer+Depth|7.066|
|3-cascade Directional Shadow|4.238|
|AdaptiveGTAOHorizon|3.063|
|ClusterCutCount|1.499|
|ClusterCutEmit|1.468|
|Deferred Cel Lighting|1.237|
|HeroLayerComposite|1.074|
|VirtualShadow.ResidentAndDirty|0.943|
|VirtualShadow.CountPageDraws|0.707|
|VirtualShadow.CullAndCommands|0.593|

중복 없는 feature group 작업량은 shadows 7.300ms, surface 7.077ms, geometry 3.826ms, AO 3.678ms 순이다. Queue가 겹치므로 합계를 critical path 또는 프레임 시간으로 대체하지 않는다. 모든 패스와 52개 옵션 연결 작업량은 아래 상세 감사에 있다. 옵션 연결 작업량은 해당 옵션의 단독 증분 비용이 아니며 여러 옵션이 같은 패스를 참조한다. 이번 기록만으로 각 옵션 자체의 정상·비정상 비용을 확정할 수 없다.

## 지형 테셀레이션 질문에 대한 판단

현재 지형은 24×24m이고 `tools/art/meadow/generate_surface.py`가 0.5m 기본 셀 및 자국 주변의 adaptive mesh를 오프라인 생성한다. GPU hull/domain tessellation은 없다. LOD 삼각형은 122944 / 108422 / 106078로 원거리 감소가 작다.

큰 평면을 여러 사각 패치로 나누고 가까운 곳의 화면상 크기와 높이 오차에 따라 세분화하는 GPU tessellation은 가능한 대안이다. 한 패치의 균일 세분화만 늘려서는 국소 밀도와 culling을 잘 제어하기 어렵다. Microsoft 문서의 factor 상한 64에서 24m 단일 패치의 균일 간격은 37.5cm이므로 미세 자국에는 부족하다. 높이 변위·공유 경계·그림자·이전 프레임·물 경계와 접지 계약도 같이 유지해야 한다. 현재 GBuffer 픽셀 셰이딩 비용의 자동 감소를 보장하지 않는다. 이번에는 GPU tessellation 전환을 구현하거나 성능 측정하지 않았다.

근거: [Microsoft tessellator factor 범위](https://learn.microsoft.com/en-us/windows/win32/direct3d11/direct3d-11-advanced-stages-tessellation), [NVIDIA 지형 패치·화면 기반 LOD](https://developer.nvidia.com/sites/default/files/akamai/gamedev/files/sdk/11/TerrainTessellation_WhitePaper.pdf).

## 검증 및 근거

- Python shader 계약 검사 38/38 통과.
- Validation / Release / Debug ProjectHS 빌드 성공. 최종 runtime-full Generation `dad74bbf3d350c55d400ac36767e72e232c02de1320a11c86b6797188f62dc91`; Release / Debug snapshot export 및 2892개 제품 무결성 검사 성공.
- 기존 runtime 180프레임 / Legacy / resize 검사 통과, 오류 0. Artifacts `Build/msvc-validation/Artifacts/render-baseline/4935b8608f3ffe5c8146982e28a5afcdc46d6c34bf80379541e43324d58d84c9`.
- 실제 world-transport 7프레임 / Enhanced / GBV 통과, 오류 0, required passes complete. VSM feedback의 완료 프레임 1·5·6·7 확인. 검증 실행의 render 255.609초는 성능 결과에서 제외했다.
- 실제 플레이 03은 game/sampler exit 0. Computer Use는 전면 창 전환 후 즉시 reset, 게임·빌드·GPU test 프로세스 종료 확인.
- `Build/20261018-next-bottleneck/FINAL-AB-report.md`, `final-foreground-audit.json`, `final-world-gbv/result.json`, `ao-hoist-gpu-run.log`, `vsm-probe-receivers-source.json`, `vsm-csm-fallback-review.md`가 상세 근거다. 원본 `Build/play-captures/20261018-meadow-current-01/02/03` 보존.

SoftwareHybrid GI 요청은 실제 Probe fallback이며 Brixelizer는 아직 OFF다. 고정 SDK bootstrap의 865개 파일 무결성만 준비했으며 ON 빌드·native GI·최고 옵션 실제 실행은 검증하지 않았다. Hardware RT를 복구하지 않았다. 전체 verify workflow와 장시간 테스트는 반복하지 않았으며 위의 해당 검사만 실행했다. AAA reference 품질과 144 FPS 모두 완료로 주장하지 않는다.