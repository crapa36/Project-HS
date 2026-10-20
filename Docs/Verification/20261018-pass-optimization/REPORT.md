# 패스 통합 최적화 — 2026-10-18

AO·terrain 공통 계산, HDR guide+CoC, SSR spatial+compose를 적용하고 아래 범위의 네이티브 검증 및 foreground 측정을 마쳤다. **144FPS 목표는 미달성이다. 최신 측정은 현재 진행 중인 지면 경계(seam) 수정 이전 버전이며 해당 수정 결과를 검증하지 않는다.**

## 최종 적용 상태

| 변경 | 제거한 작업 | 보존한 계약 |
|---|---|---|
| HDR nearest guides + CoC | pass112, guide 전체 화면 재읽기, 별도 shader/PSO 단계 | 동일 thin-lens 수식, DoF off에서 CoC 쓰기 없음, MB·dilation·DoF 의존성 |
| SSR spatial + compose | pass73, 후반 RGBA32F 중간 write/read | Trace raw·history·filter·rejection, 기존 opaque/environment와 alpha |
| AO guide 공유 | 동일 2×2 guide/density 반복 로드·복원 | 8×8 LDS3072bytes, barrier1회, 부분 타일·jitter·fallback |
| terrain 공통 계산 | 층별 동일 warp noise, meadow 내부에서 덮어쓰던 legacy path noise | 좌표·seed·색/표면 수식·POM·텍스처 품질 |

SSR 왕복 제거량은 FHD 논리 트래픽 약66.36MB/frame이며 실제 DRAM 또는 FPS 개선율이 아니다. terrain의 184개 SSA 계산은 실제 DXIL CFG에서 layer loop 밖으로 이동했다. RESEARCH.md 참조.

**신규 record-list 병합은 전부 철회**했다. 기존 Enhanced 제출 묶기, task executor, 패스별 timing은 유지한다. 처음의 좁은 multi-queue 통과를 전체 엔진 정상 근거로 사용하지 않는다.

## 네이티브 검증·실패 기록

Build/pass-optimization-20261018/native-v4-{combined,motion-only,dof-only}/의 invocation.json과 render.json을 대조했다. 각 640×360/16ticks, Enhanced, exit0·validation_errors0. combined는 GPU-based validation까지 사용했고 MB-only/DoF-only는 D3D12 validation을 사용했다. 제한된 smoke이며 장시간 플레이나 전체 픽셀 동등성 증명은 아니다.

- Release/Debug 빌드·snapshot export/verify 및 모듈 경계 검사 통과. Legacy180frames/resize: verify-20261018T011923421Z-12896 PASS,67.71초.
- AO 실제 원본/후보 GPU 비교: RTX4080+GBV,192cases/39936texel쌍 bitdiff0. 비백색 AO·incompatible raw fallback 포함. ao-guide-sharing/gpu-test/verified-run.json.
- SSR CPU/source4개와 CTest rendering.ssr-spatial-composition-contracts 통과. GPU 픽셀 동등성 검사는 아니다.
- terrain 최초 binding 누락 및 128-page capacity 부족은 최초 사용 추적과 공통256-page 계약으로 수정. 필수141슬롯 근거: ../20261018-geometry-capacity.md.
- 실패 원본 보존: native-combined id1003 134건; root 무효화 보완 후 native-v2 및 빈 캐시 native-v3에서 nvwgf2umx.dll 0xc0000005/offset0x6dbab9 충돌. record-list 병합 철회 후 native-v4 세 조합 정상 종료. 캐시만의 문제로 단정하지 않는다.

## 실제 foreground 측정

원본: Build/play-captures/20261018-pass-fusion-01/{summary.json,option-costs.json,performance.csv,gpu_live.csv}. Release, 원본 플레이 replay, 무적 비활성,1920×1080 출력/내부 해상도. Playing653프레임 모두 foreground=1을 raw CSV에서 확인했다. capture-session의 시작 시 foreground_verified=false와 구분한다.

| 지표 | 결과 | 범위 |
|---|---:|---|
| base FPS | 28.978 | Playing, 동일 phase/config 연속651구간 |
| SDK 보고 present FPS | 57.965 | Streamline1296개/22.358초; 물리 디스플레이 cadence 미검증 |
| GPU critical path | 31.146ms | Playing653 완료 프레임 평균 |
| 프로세스 CPU | 4.564% | 머신 전체20 logical CPU 기준 시간 가중 평균;0.913core-equivalents |
| 프로세스 GPU | 60.384% | PID11752의 가장 바쁜 engine 시간 가중 평균; engine 합계 아님 |
| GPU 점유율 시간 coverage | 97.08% | 22.312/22.983초; 빈 구간을0으로 채우지 않음 |

런타임 프레임 기록은 전체1404/1404, GPU invalid0. PDH counter 오류42건으로 점유율 분석은 **partial**이며 유효 구간만 사용했다. GPU critical 측정 완결과 모든 개별 copy 패스의 timestamp 제공 여부는 다르다. GI 요청 software_hybrid는 실제 probe fallback(consumer_unavailable)이므로 최고 GI 경로 실행 증거가 아니다.

아트·geometry·장면이 이전 캡처와 달라 유효한 통제 전후 비교가 없다. 개선율/회귀율을 계산하지 않는다. 20261016-ao-direct-view-01은 Playing 전체 background여서 제외했다.

## GPU 패스: 무거운 순

설정 전환1프레임(config2)을 제외한 **Playing+config3,652프레임**. raw completed_frame을 performance.frame에 연결했다. 아래는 전체652프레임당 평균 작업량 상위20개로 모두652회 측정됐다. queue 중첩 때문에 합계는 frame time이 아니다. timestamp 미제공 copy/upload/readback 패스는0ms로 간주하지 않고 제외했다.

| ID | 패스 | ms/완료 프레임 | 측정 횟수 |
|---:|---|---:|---:|
| 4 | GBuffer+Depth | 6.332 | 652/652 |
| 2 | 3-cascade Directional Shadow | 4.576 | 652/652 |
| 45 | AdaptiveGTAOHorizon | 3.275 | 652/652 |
| 40 | ClusterCutCount | 1.522 | 652/652 |
| 44 | ClusterCutEmit | 1.475 | 652/652 |
| 150 | VirtualShadow.ResidentAndDirty | 1.332 | 652/652 |
| 5 | Deferred Cel Lighting | 1.230 | 652/652 |
| 224 | VirtualShadow.CountPageDraws | 0.928 | 652/652 |
| 149 | VirtualShadow.ReceiverFeedback | 0.815 | 652/652 |
| 113 | HdrPostForegroundDilation | 0.797 | 652/652 |
| 151 | VirtualShadow.CullAndCommands | 0.709 | 652/652 |
| 108 | HeroLayerComposite | 0.653 | 652/652 |
| 70 | SSRTrace | 0.589 | 652/652 |
| 225 | VirtualShadow.AdmitCompletePages | 0.475 | 652/652 |
| 114 | HdrPostDepthOfField | 0.437 | 652/652 |
| 71 | SSRTemporal | 0.432 | 652/652 |
| 3 | Static Visibility | 0.393 | 652/652 |
| 72 | SSRSpatialCompose | 0.389 | 652/652 |
| 46 | GTAOTemporal | 0.341 | 652/652 |
| 153 | VirtualShadow.DynamicBank | 0.287 | 652/652 |

## 그래픽 옵션 관련 공유 기능 비용: 무거운 순

option-costs.json의 **Playing config3/652프레임** ranked_feature_groups다. 옵션 off 차분이 아닌 관련 패스 작업량이다. geometry·foliage·해상도 옵션은 여러 그룹에 영향을 주므로 그룹을 옵션마다 다시 더하지 않는다. shared_post_guides는 MB/DoF 공유 guide와 통합 CoC이며 양쪽에 중복 배분하지 않았다. 미실행 기능은0ms가 아니라 미측정이다. 그룹명은 원본 JSON 식별자를 유지한다.

| 공유 기능 그룹 | ms/완료 프레임 |
|---|---:|
| shadows | 9.177 |
| surface | 6.339 |
| ao | 3.942 |
| geometry | 3.854 |
| reflections | 1.409 |
| depth_of_field | 1.235 |
| lighting | 1.230 |
| refraction | 1.172 |
| volumetrics | 0.964 |
| post | 0.326 |
| reconstruction | 0.225 |
| contact_shadows | 0.213 |
| motion_blur | 0.199 |
| shared_post_guides | 0.107 |
| adaptive | 0.106 |
| outline | 0.100 |
| transparency | 0.075 |
| subsurface | 0.066 |
| frame_generation_guides | 0.065 |
| bloom | 0.053 |
| color_grading | 0.023 |
| particles | 0.015 |
| shared_probes | 0.015 |
| light_lists | 0.010 |
| distortion | 0.003 |
| ui | 0.002 |

frame_generation_guides는 SDK 전체 비용이 아니다. shared_probes는 주기 갱신을 전체 프레임으로 나눈 비용이며 software GI 계산 시간이 아니다. config3 GPU critical 평균31.136ms는 전체Playing653 평균31.146ms와 표본 범위가 다르다.

CPU Playing 평균: graph_build5.004ms, graph_submit3.952ms, constants_upload2.325ms, graph_record1.519ms, graph_prepare1.303ms, scene_prepare1.278ms. present14.667ms는 대기·SDK를 포함한 wall time으로 CPU 연산량과 다르다. render32.809ms 안의 부분 단계를 다시 합산하지 않는다.

## 남은 작업

상위 병목은 GBuffer, directional shadow, AO, cluster count/emit 및 VSM이다. 지면 경계 수정과 해당 동일 카메라 검증은 완료했다. 이후 비교는 같은 아트·옵션·경로의 foreground 조건을 맞춰야 한다. terminal bloom/grading/tonemap 결합은 조사 후보로 남았다. 모델·장면의 전체 시각 검증, 최고 GI 실제 실행 및144FPS 유지 검증도 완료되지 않았다.

### 지면 경계 후속 상태

수정 및 네이티브 확인 완료: 외곽 slab 상면 +5mm→0, MeadowSurface seed 통일, path/wet/traces1.5m 경계 blend. warp 공통 계산은 유지했다. presentation-contract와 shader reconstruction33개 검사 통과. Validation/Release/Debug 빌드를 완료했다. 동일 카메라 Legacy 180프레임/resize 검증은 execution_valid/assertions_passed=true, validation_errors=0이다. 새 capture_tick_180.png에서 이전의 직선 사각 재질 경계가 사라진 것을 확인했다. 증거: Build/msvc-validation/Artifacts/render-baseline/3dc4a67a411f460c21ba63c01ff598191a5e3a4b6397c45209112fc18ea337a9/. 위 측정 수치는 이 수정 이전 결과다.


원본 capture-session.json은 보존했고 별도 analysis-audit.json에 raw CSV 기반 Playing653/foreground653, 프레임 timing1404/1404와 PDH partial의 차이를 기록했다. copy/upload/readback의 개별 미측정 행도 별도로 남겼다.
