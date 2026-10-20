# 최고 옵션 전면 플레이 재측정 — 2026-10-16

144 FPS 목표는 미달성이다. 이전 구조 개선 턴은 실제 코드·검증 결과를 남긴 progress였으며, 이번 턴은 최신 실행 파일의 실제 foreground 측정으로 다음 병목 우선순위를 갱신했다.

## 조건과 원본

- RTX 4080, 모니터 현재 기본1920×1080, Windows 보고143Hz. Release, D3D12/GBV OFF.
- borderless fullscreen, 내부100%, 모든 quality Ultra, RT AO/GI/반사/그림자 활성, Bloom/DOF/Outline ON, MotionBlur High, DLSS FG2x. 원래 사용자 프로필 대신 별도 프로필 사용.
- 원본: `Build/play-captures/20261016-structure-ultra-01/`. 기존 human/capture05 기록은 변경하지 않았다.
- 실행 파일/Generation 해시는 `measurement.json`, 원본 `capture-session.json`에 기록했다.
- Computer Use로 시작·지면 클릭 입력을 수행했으나, playtest의 실제 이동 거리는 0m였다. 측정 범위는 초기 정지 전투이며 이동 플레이를 검증하지 않는다. 게임은 exit0으로 정상 종료됐으며, 입력 변경 감지 후 재관측 시 게임 창이 사라진 것을 확인했다. 이를 무단 강제 종료나 성공한 추가 공격 입력으로 기록하지 않는다.
- 게임·sampler가 종료된 후 Computer Use kernel/확인된 helper도 종료했다.

## 결과

|항목|측정|
|---|---:|
|전체 CPU/GPU 프레임 기록|379/379, 완전|
|Playing/foreground|186프레임, 모두 foreground=1|
|동일 상태·옵션 프레임 구간|21.963초,184구간|
|실제 렌더 FPS|8.378|
|SDK 실제 reported-present FPS|16.997 (366회/21.534초)|
|CPU render 평균|119.128ms|
|GPU critical path 평균|96.591ms|
|프레임 구간 median / p95 / p99|113.053 /126.396 /393.186ms|
|72 base FPS 예산13.889ms 이내 구간|0/184|
|프로세스 CPU|1.259코어,20논리코어 기준6.294%|
|대상 PID GPU busiest engine|69.427%,22.337/22.872초 관측|
|NVIDIA 장치 전체 GPU|79.818%,다른 앱 포함|
|장치 전력 평균/최대|179.050/204.530W|

SDK present 수치는 물리 디스플레이 scanout FPS가 아니다. 프레임 생성2x를 허용해도 현재 렌더링은 목표 기반72FPS보다 크게 느리다. 샘플링/캡처 오버헤드가 포함된다. 전면 여부는 프레임별 관측이며 관측 사이의 포커스 이력은 추정하지 않는다.

PDH41회 오류로 기존 analyzer는 partial을 유지한다. 원본을 재확인한 결과 대상 PID GPU1440행은 전부 status0이고, invalid600행은 다른 PID였다. 시작 관측 공백과 대상 PID 미관측 구간은 여전히 N/A다. 수집 오류를 숨기거나 partial을 complete로 바꾸지 않았다.

## 무거운 순서

Playing config3의185완료 GPU프레임 기준이다. 그룹은 공유 패스 비용이며 옵션 하나를 끌 때의 절약량이 아니다. 큐가 겹치므로 아래 값을 전체 프레임 시간으로 합산하지 않는다.

|순위|GPU 그룹|ms/완료프레임|
|---:|---|---:|
|1|RT GI|35.288|
|2|반사|20.666|
|3|RT 장면/가속구조|9.209|
|4|AO|8.429|
|5|그림자|6.953|
|6|Surface/GBuffer|5.163|
|7|Geometry|2.368|
|8|굴절|1.569|
|9|조명|1.434|
|10|DOF|1.052|
|11|볼류메트릭|0.958|

CPU는 graph_build50.307ms > graph_record18.881ms > graph_prepare18.274ms > post_submit8.121ms > graph_plan7.323ms > graph_submit6.366ms 순이다. graph_build 안의 ray_scene_register26.373ms, graph_prepare 안의 compile10.402/info_key5.206ms는 부모 시간에 이미 포함된다. GPU 단일 패스는 RTGITrace34.317ms, HybridReflectionTrace19.399ms, RTAOTrace8.018ms, dynamic BLAS6.587ms, GBuffer5.155ms 순이다.

전체56옵션은 `option-costs.json`의 revision3을 기준으로 본다. 원본 `REPORT.md`에 모든 revision의 순위와 `GPU_PASS_TIMES.csv`, `CPU_PASS_TIMES.csv`가 있다. revision4는 패배 이후 FG가 꺼진 상태이므로 Playing 성능으로 해석하지 않는다.

## 해석과 다음 변경 기준

GPU가 항상100%가 아닌 이유를 GPU 시간만으로 추정하지 않는다. 이번 캡처에서는 CPU프레임119ms, GPU critical96.6ms, wait0.023ms이며 CPU graph 생성·등록 비용이 확인됐다. CPU 전체6.294%는20논리코어 평균이고, 한두 코어에 집중된 준비 작업을 가리지 않는다.

이전 capture05의2.434/5.100FPS와 이번8.378/16.997FPS는 콘텐츠·장면 부하·시뮬레이션 pacing·여러 최적화가 달라 통제된 전후 성능 개선율로 사용하지 않는다. 정확한 다음 비교에는 이번 입력/설정/Generation과 일치하는 workload가 필요하다.

우선 CPU RT 등록의 남은 중복 작업과 GI/반사의 후보 hit 처리를 조사한다. [NVIDIA의 RT 성능 지침](https://developer.nvidia.com/blog/best-practices-using-nvidia-rtx-ray-tracing/)은 opaque 처리와 가속구조 구성을 주요 항목으로 설명한다. 그러나 현재 코드는 강제 non-opaque query에서 알파·앞뒷면·degenerate·identity/unsupported 조건을 검사한다. [Microsoft DXR 명세](https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html)의 opacity flag는 inline 제어 흐름을 바꾸므로 단순 FORCE_OPAQUE 전환은 현재 정확성 계약을 보존한다고 볼 수 없다. 최고 옵션 샘플 수나 해상도를 낮춰 목표 달성으로 처리하지 않는다.

## 측정 후 추가 CPU 변경

`renderer_ray_scene.cpp`의 동적 BLAS `used` 해시 삽입을 생략했다. 앞선 stable slot 중복 검사가 유효 입력의 slot 고유성을 보장하고, dynamic key는 slot/generation/geometry/frame slot을 포함한다. 각 키의 BLAS는 기존 exact-key 조회 또는 새 생성으로만 얻으므로 동적 항목끼리 pointer 중복이 생기지 않는다. Static 공유/compaction의 해시 검사, pending.used 순서, GPU lease·retirement 처리는 유지했다.

변경 전 소스/해시는 `Build/performance-actual/rt-used-dedup/source-receipt.json`에 보존했다. Debug/Release 재빌드와 기존8프레임 ray-entity-ultra/empty reset6/GBV 검증을 통과했다. Native exit0, execution_valid=true, validation_errors=0이다. `Build/performance-actual/rt-used-dedup/integration.json`에 새 빌드 해시와 검증 영수증을 기록했다. 새 set 미러 테스트나 불필요한 framework는 추가하지 않았다. 앞의8.378/16.997FPS는 변경 전 baseline이다. 아래 동일 입력 replay로 변경 후 수치를 확인했다.


## 동일 입력 재생 비교

`20261016-structure-ultra-replay-02`는 변경 후 Release `577498a5d5bd18a5235a618d3d7a81850559ffb25da52031c52d774147fd6724`로 원본 입력을 재생했다. 입력 파일 바이트/SHA-256, 56개 requested/effective 설정, 콘텐츠 Generation이 일치하며 전체 replay checksum 검증을 통과했다. Playing 192프레임 모두 foreground=1이다. 게임과 sampler는 exit0으로 종료했고 Computer Use는 창 활성화 직후 종료했다.

|항목|변경 전|변경 후|
|---|---:|---:|
|기본 렌더 FPS|8.378|8.627|
|SDK reported-present FPS|16.997|17.551|
|CPU render 평균 ms|119.128|115.733|
|GPU critical path 평균 ms|96.591|92.580|
|tick120–1200 RT 등록 평균 ms|22.928|20.722|
|같은 구간 graph build 평균 ms|46.594|44.085|

전후 한 쌍의 초기 정지 전투 비교다. GPU 코드가 같아도 GPU 시간이 변했으므로 전체 FPS 차이를 동적 BLAS 해시 제거만의 인과 효과로 단정하지 않는다. 새 sampler의 PDH42회 오류/partial 표시는 유지했다. 원본 capture-session의 초기 foreground_verified=false는 실제 프레임별 foreground=1과 별개이며 원본을 수정하지 않았다. `Build/performance-actual/rt-used-dedup/{foreground-comparison,replay-fidelity}.json`에 비교 범위와 근거를 보존했다. 144 FPS 목표는 여전히 미달성이다.


이후 초기화 범위와 RT 캐시 위치의 중복 계산 두 건을 추가 제거했다. 이 최종 빌드/Generation `b338b119...`는 CPU·shader·WARP oracle 및 RTX8프레임GBV를 통과했지만 FPS를 다시 측정하지 않았다. 따라서 위8.627/17.551FPS는 최종 두 변경 이전 수치다. 최종 검증은 `Docs/Verification/20261016-structure-review/REPORT.md`와 `Build/structure-review/followup/integration.json`을 참조한다.
