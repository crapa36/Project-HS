# 레이트레이싱 제거 및 비RT 재검증

최신 잔재 정밀 검토·제거와 재빌드 결과는 [REMNANTS.md](REMNANTS.md)를 따른다. 아래 성능 측정은 추가 잔재 정리 전 기록이며 반복하지 않았다.

## 변경 범위

- DXR AO·GI·반사·그림자, BLAS/TLAS, OMM, SER, NRD, DLSS/FSR Ray Reconstruction 구현·UI·능력 조회·진단·셰이더 생산·빌드 의존성을 제거했다.
- 래스터/가상 그림자, GTAO, SSR, 프로브, 소프트웨어 GI, 일반 SR, 프레임 생성, Reflex는 유지했다.
- 소프트웨어 GI가 사용하던 공용 변형 계산은 `renderer_scene_geometry.*`와 `scene_geometry.hlsl`로 분리했다.
- 현재 그래픽 옵션은52개다. 기존 설정 ID1..40,45..56은 유지하고41..44를 재사용하지 않는다.
- 저장 파일을 읽을 때 구RT 모드와 이전 `dlss_rr` SR 선택을 비RT 모드로 변환한다. 다른 그래픽·오디오·키·알 수 없는 확장 데이터는 유지하며 읽기만으로 원본을 덮어쓰지 않는다.
- QualityProfiles는 version2/140B로 변경했고 구version1/176B를 거부한다. AutoFG cadence는 version1/12B를 유지한다.
- 현재 CPU 기록74열, 옵션52개. 분석기는 원본을 바꾸지 않고 과거56개 옵션 기록도 지원한다.
- 추가 정밀 검토에서 Decal의 기존 RasterOnly 정책 필드도 제거하고 metadata v2/28B로 재쿠킹했다. 오래된 캡처·Generation은 보존하고 RT 전용 SDK/빌드 잔재는 정리했다.

## 빌드 및 검증

- Debug/Release ProjectHS 빌드 성공. Streamline SR/FG가 켜진 구성이다.
- 최초 제거 검증 당시 runtime-full Generation: `db3d307553e8cde6480fbb52771ced35d246160c7319919520c636d94484f223`,2712개 제품. Debug/Release export 및 verify 성공.
- 이전4219개 제품에서 RT 전용 제품을 제거했으며 실행 폴더의 `sl.dlss_d.dll`, `nvngx_dlssd.dll` 총4개도 제거했다. SR/FG DLL은 유지했다.
- OFF/OFF 저수준 gate19/19 통과: `Build/verification/verify-core-20261015T171101225Z-22876.json`.
- 선택 회귀6/6 통과: feature-signals, shader-reconstruction-contracts(29개 내부 검사), graph-range-validation, render-package-format, multi-queue-graph, graphics-settings-store. `Build/rt-removal/targeted-tests.xml`.
- 기존 실제 게임 렌더 smoke180프레임/리사이즈/종료 통과(25.19s): `Build/rt-removal/native-smoke.xml`.
- 중단된 최초 native/배포 시도는 성공으로 취급하지 않았다. 실제 프로세스 종료를 확인한 뒤 재실행했다.
- 처음 컴파일에서 발견한 공용 digest 타입과 Brixelizer 선언 누락을 수정했다. 최종 빌드는 통과했으며 첫 실패 로그도 보존했다.

GPU validation은 surface-motion-resize fixture 8프레임, execution_valid=true, validation_errors=0, 종료 코드0으로 통과했다. 근거: `Build/rt-removal/native-gbv/result.json`, `run.json`. 초기 GBV 셰이더 컴파일을 포함한443.23초 실행은 성능 측정이 아니다.

## 비RT 짧은 전경 리플레이 측정

기록: `Build/play-captures/20261016-nonrt-ultra-01`. Release, 검증 레이어 OFF, 출력/내부1920×1080, 남은 품질 Ultra, 내부TAAU100%, DLSS FG2×. 기존 입력과 재기록 입력의 SHA256이 같고 최종 시뮬레이션 checksum14666118289919368357/tick1382가 일치한다. Playing742프레임 전부 전경이며, 같은 phase/config740개 간격22.463초를 사용한다. 정지 상태 초기 전투이며 이동/보스전/장시간 플레이를 대표하지 않는다.

| 항목 | Playing 결과 |
|---|---:|
| 기본 렌더 FPS |32.943|
| Streamline SDK 실제 present 수 기반 FPS |65.859|
| GPU critical path 평균 |27.101ms|
| CPU render 구간 경과시간 평균 |28.784ms|
| 프로세스 CPU (20논리CPU 전체 기준) |4.203%, 0.841코어|
| 대상PID GPU busiest engine |68.709%|
| 장치 전체 GPU (다른 앱 포함) |92.091%|
| 장치 VRAM 평균 / 최대 |5773.318 /5883.000MiB|

SDK1474회 present/22.381초이며 실제 모니터 표시 간격의 독립 검증은 아니다. PID GPU 관측은21.666/22.960초이고 누락 구간을0으로 채우지 않았다. 프로세스 CPU는21.135초 관측했다. CPU 구간은 대기·겹침을 포함한 wall time이며 CPU 사용률과 다르다.

### 무거운 GPU 패스 순서

| 패스 | Playing 프레임당 평균 |
|---|---:|
| GBuffer+Depth |4.985ms|
| AdaptiveGTAOHorizon |3.894ms|
| Directional Shadow (3 cascades) |3.576ms|
| VirtualShadow.ResidentAndDirty |1.513ms|
| VirtualShadow.CountPageDraws |1.465ms|
| Deferred Cel Lighting |1.247ms|
| ClusterCutCount |0.764ms|
| VirtualShadow.CullAndCommands |0.730ms|
| ClusterCutEmit |0.682ms|
| HeroLayerComposite |0.669ms|
| SSRTrace |0.631ms|

CPU 측에서는 Present12.285ms, graph_build4.807ms, graph_submit3.568ms, constants_upload2.284ms가 크다. Present는 GPU/표시 대기 가능성을 포함하므로 계산 부하로 단정하지 않는다. 패스 시간 합계는 비동기 큐 겹침 때문에 전체 프레임 시간이나 옵션 OFF 시 절약량과 같지 않다. 전체 패스·52개 옵션별 매핑은 원본 기록의 `REPORT.md`, `option-costs.json`, `GPU_PASS_TIMES.csv`, `CPU_PASS_TIMES.csv`에 있다.

이전 RT replay03의8.964/18.182FPS 대비 상승했으나 RT 기능을 제거한 조건 차이가 있으므로 같은 화질의 미세 최적화 효과로 해석하지 않는다. 144FPS 목표는 미달이며, 남은 주요 후보는 GBuffer, GTAO, 그림자와 CPU 그래프 준비/제출이다.

### 측정 한계 및 데이터 상태

- 소프트웨어 GI 구현은 보존했으나 현재 빌드는 Brixelizer OFF다. 요청한 software_hybrid가 consumer_unavailable로 probe로 대체되므로 모든 남은 최고 기능을 검증한 결과가 아니다.
- CPU/GPU 프레임1571개와 진단 기록은 완전하고 유효하지 않은 GPU 프레임은0이다. 게임/샘플러는 종료 코드0이다.
- PDH 카운터 오류2건으로 분석은 partial, 최종 capture-session은 incomplete다. 오류 문구만으로 대상PID 오류인지 다른 프로세스 오류인지 확정할 수 없다. 원본 상태를 성공으로 바꾸지 않았다.
- summary의 capture_provenance는 최종화 전 스냅샷이다. 세션 상태는 최종 capture-session.json을 우선한다. 해당 파일의 foreground_verified=false는 하네스 초기값이며 실제 전경 근거는 프레임CSV다. 별도 `measurement-audit.json`에 재현 가능한 파생 근거를 보관했다.
- 게임·샘플러·컴퓨터 유즈는 종료했다. 추가 측정은 반복하지 않았다.

## 검증 범위

원본 기록·기존 지형/캐릭터 변경과60Hz 시뮬레이션 계약을 보존했다. 짧은 기록에서144FPS 목표는 미달이다. 전체 장시간 플레이 및 비활성 SDK의 모든 기기 조합 검증을 이번 삭제 작업의 통과 근거로 주장하지 않는다.
