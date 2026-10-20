# Project HS 경량화 조사와 적용

기준: 2026-10-20 KST, `codex/renderer-meadow-backup-20261018`, 원격 최신 `6e30b9f55386389b1e282b5ad9d30294fbd1f689`.
사용자 요청에 따라 현재 작업 폴더에서 수정했다. 기존 HLOD 소스·테스트 및 Debug 기본 설정 변경은 보존했다. 커밋·푸시는 하지 않는다.
입력 자료: 바탕화면의 `Project-HS-Engine-Audit-6e30b9f.md`, `Project-HS-critical-review.html`. 보고서의 권고는 지시로 실행하지 않고 실제 호출·빌드·콘텐츠와 대조했다.

## 인게임 추가 최적화 (2026-10-21)

- 전투 `FindEnemy`는 적 배열 전체 검색을 증가 ID 기반 이진 검색으로 변경했다. 생성·spawn barrier는 ID 순서로 append하고 cleanup은 생존자 순서를 유지한다. 별도 인덱스나 동기화 상태를 추가하지 않았다. 늦게 등록되는 cast runtime에는 이 전제가 성립하지 않아 적용하지 않았다.
- 경로장 `RebuildFlow`는 이동 비용 10/14에 맞는 15개 순환 버킷으로 변경했다. 매번 heap을 구성하는 대신 버킷 capacity를 재사용한다. predecessor를 저장하지 않으므로 동률 처리 순서가 달라도 최종 거리 배열과 waypoint 선택은 동일하다.
- 대상 캐시·공간 인덱스를 최근접 적 검색에 무조건 재사용하지 않았다. spawn/이동/cleanup 단계별 유효성이 달라 별도 동기화 비용과 결과 변경 위험이 있다. 피해 판정 후 위치가 달라질 수 있는 hit time 재계산도 유지했다.

검증: core 18/18 (`verify-core-20261020T155517272Z-6904.log`), 콘텐츠 기반 전투·진행·결정론·Experiment·실제 렌더 smoke 등 관련 검사 11/11 (`verify-20261020T155631444Z-9760.log`). 기존 결정론 oracle은 변경하지 않았다. Debug 경로 검사에서는 이전 heap을 기준으로 차단 지도·좁은 격자·외부 목표·무작위 장애물·긴 미로를 포함한 72개 전체 거리 배열을 비교했다. 전투 회귀는 중간 ID 제거·재생성·session reset을 검사한다.

경로장 단독 측정(`/O2 /fp:strict`, 121×121, 각 40회 순서 교대, 모든 거리 배열 동일):

| 지도 | 기존 heap 평균 | 순환 버킷 평균 | 변화 |
|---|---:|---:|---:|
| 빈 지도 | 0.673 ms | 0.249 ms | -62.9% |
| 장애물 25% | 0.728 ms | 0.498 ms | -31.5% |
| 장애물 45% | 0.0464 ms | 0.0438 ms | -5.8% |
| 긴 단일 통로 | 0.112 ms | 0.130 ms | +16.1% |

단일 통로는 heap 후보가 거의 하나라 버킷의 빈 비용 순회가 더 비싸다(+0.018 ms). 일반 지도에서의 이득을 근거로 버킷을 유지한다. 측정 최대 잔류 capacity는 grid당 10,536 bytes이며 vector 객체 360 bytes는 별도다. 실제 `NavigationState`에는 enemy/boss 두 grid가 있다. 기록: `Build/engine-lean-audit/navigation-flow-benchmark.log`.

별도로 변경 전 라이브러리·실행 파일을 보존하고, 초기 적 600개/120 tick마다 60개 보충/이동·기본 공격·4개 스킬 시나리오 1,800 ticks를 전후 각 6회 실행했다. 기본 지도 TickFixed 중앙값은 947.22→960.87 ms(+1.44%), 원형 장애물 4개 지도는 1,041.35→1,057.09 ms(+1.51%)다. 반복 간 변동이 크며 **전체 tick 성능 개선은 입증하지 못했다**. 모든 실행에서 매 tick canonical checksum과 read-model/domain-signal 요약 trace는 일치한다. 전체 원시 상태 바이트 또는 전체 게임 FPS의 동등성/개선 주장이 아니다. 입력과 원자료: `Build/engine-lean-audit/gameplay-benchmark-scenario.json`, `gameplay-benchmark-comparison.json`, `gameplay-paired-*`.

적 조회 단독 측정은 실제 728-byte `EnemyActor` 배열로 4회 순서를 교대하고 반환 ID 합계를 비교했다. 균등 조회에서 600개는 74.10→37.06 ns, 1,500개는 193.37→46.14 ns로 감소했다. 반대로 8개는 5.58→11.50 ns, 64개는 16.92→25.54 ns이며 첫 적만 찾는 경우도 1.5→2~4.3 ns로 느려졌다. 다수 적에서의 검색 확장성과 별도 상태가 없다는 점을 근거로 이진 검색을 유지한다. 작은 배열 전용 임계값·이중 경로는 추가하지 않았다. 기록: `Build/engine-lean-audit/enemy-lookup-benchmark.log`. 이 단독 이득이 전체 tick 이득을 입증하는 것은 아니다.

후속 상세 계측에서 위 벤치의 이동 목표 입력이 기본 공격 유지로 차단되는 조건임을 확인했다. 따라서 기존 결과는 적 이동·전투 조건의 측정이며, 플레이어가 계속 이동하며 경로장을 갱신하는 부하의 근거가 아니다. 실제 이동 조건과 14개 단계·렌더 패스 상세 분석은 `Docs/PERFORMANCE_BREAKDOWN_20261021.md`에 별도로 기록한다.

## 판단

이 프로젝트에는 게임 외에 별도 빌드 인증·검증 관리 도구가 지나치게 커져 있었다. 사용자 추가 지시인 “빌드 정확성 검사 같은 과도한 기능도 대폭 축소”를 반영해 이 계층을 실제로 걷어냈다. 게임 로직과 렌더링 기능은 이름이 고급스럽다는 이유만으로 제거하지 않았다. 실제 재질과 화면, 결과창에서 사용하는 기능이 많다.

개인용 단일 게임·로컬 개발을 기준으로 한다. 다른 컴퓨터·다른 링크 결과까지 같은 제작기임을 인증하거나 자동으로 검증 범위를 추론하는 요구는 버렸다. 현재 필요한 부분만 정확하게 빌드하는 CMake/Ninja 그래프, 게임 실행, 명시적으로 선택한 테스트는 남겼다.

## 1차 적용 내용

1. `tools/hs_dev.ps1`: build·verify만 남겼다. doctor/status/why/deps/rdeps/path/affected/gc 중복 진입점과 제품 그래프 해석을 제거했다. native 타깃·기존 Action ID 선택, dry-run, 실패 종료 코드는 유지한다.
2. `tools/verify.ps1`, `verification_metadata.ps1`: 변경 영향 추론, 자체 준비 그래프 해석, 자동 Generation 복구, 상세 타이밍·검증 영수증을 제거했다. CMake 구성 → CTest 선택 → `test_prep/<test>` → CTest 실행으로 축소했다. fixture 자동 선택과 실제 실패는 CTest가 처리한다.
3. `cmake/AssetRecipeIdentity*.cmake`, `AssetPipeline/action.cpp`: 소스·헤더·컴파일 DB·링크 명령·SDK closure를 다시 인증하던 체계를 제거했다. 실제 연결된 producer 실행 파일의 SHA-256이 recipe identity다. 소스/헤더 재빌드는 compiler/Ninja가 맡는다. DXC/DLL 등 제작 시 직접 사용하는 도구 입력은 Action의 명시 의존성으로 유지한다.
4. `build.msvc-header-dependencies`와 `check_msvc_dependencies.cmake` 제거. 한국어 MSVC 출력을 Ninja 헤더 의존성으로 연결하는 `MsvcIncludeProjection.cmake`는 실제 빌드 기능이므로 유지한다.
5. 구형 `hs_legacy_content_oracle` 타깃과 전용 main/orchestration, 사용하지 않는 전체 원본 로더 제거. 분리된 실제 cooker와 원본 검증은 유지한다.
6. `Generated/Shaders`로 다시 복사하던 구형 shader compatibility 경로와 `--project-legacy` 제거. `hs_shaders`는 기존 shader Action 결과를 직접 요구한다. 개별 shader producer 타깃도 유지한다.
7. 녹화하지 않는 일반 실행의 events/timeline/GPU 로그, 상세 JSON·renderer feature 기록과 진단 완전성 의무를 해제했다. 문자열 포맷과 매 frame working-set 조회도 진단 수집 시에만 한다. smoke·Experiment·녹화·리플레이·명시적 capture의 결과 파일과 오류 처리는 유지한다. 1차에서는 Debug 자동 녹화가 예외였으며 후속 작업에서 기본 녹화도 제거했다. `render.json`을 검사하는 shader hot reload 테스트도 `--capture-diagnostics`를 명시한다.
8. 생산 코드에서 호출되지 않는 `performance_log.hpp`와 전용 테스트, 미채택 Repowise/usage/LSP/sccache 실험 스크립트 4개 제거. 실제 성능 캡처 도구와 LSP 서버는 유지한다.
9. PCF·sparse effect·SSR composition 검사에서 코드 공백/변수/본문 배치를 고정한 문자열 검사를 제거했다. 실제 tap table 및 수학·경계 검사는 남겼다. 이 CPU 검사가 production shader 실행을 증명한다고 주장하지 않는다.
10. `PopulateRenderInstanceRevisions`: identity 생성 후 이미 채워진 비애니메이션 revision의 반복 해시를 생략한다. caller revision과 named-animation 시간에 따른 pose 갱신은 유지한다.
11. `msvc-core`에 테스트 ON을 명시했다. 기존 사용자의 일반/Debug 테스트 OFF 설정을 유지하면서 기본 AI 테스트 명령이 깨지지 않도록 한다.
12. 통합 빌드를 막던 기존 HLOD 변경의 연결 오류 두 곳만 수정했다. `scene_hlod_cooker.cpp`는 저장된 오차 예산을 작업 람다에 값으로 전달하고, `geometry_error.cpp`는 헤더에 이미 추가된 예산 인자를 받아 양방향 계산에 전달한다. 기존 convex bound·프로파일링·기하 입력·테스트 변경은 그대로 유지했다.

규모: 기존 사용자 변경과 문서를 제외하고 새 테스트를 포함한 코드·설정·도구의 순감소는 약 3,270줄이다. 이는 유지보수 대상 감소이며 실행 속도 측정치는 아니다.

## 실제 실행 구조

원본/Schema/Shader → 개별 producer → Action 제품 → Selected Generation → Runtime.
Runtime의 메인 스레드는 창·입력·UI·오디오·저장을 처리하고, 시뮬레이션은 60 Hz Gameplay를 진행한다. Presentation이 읽기 모델과 이벤트를 화면 스냅샷으로 변환한다. 렌더 스레드는 D3D12 자원·가시성·그림자·재질·조명·VFX·후처리를 실행한다.

이 분리는 CPU 단독 테스트와 부분 빌드에 이점이 있다. 작은 모듈을 합치거나 새 ECS/렌더러 인터페이스를 만드는 것은 경량화가 아니다. 반대로 빌드 그래프 위에서 같은 의존성을 다시 탐색·인증하던 관리 코드는 제거할 수 있었다.

## 전체 기능군 판정

아래 ID는 입력 Markdown 보고서의 77개 기능군과 대응한다. **적용**은 실제 수정, **유지**는 현재 필수/실사용, **후속**은 추가 변경의 구체적 조건이 있는 항목이다. 전 기능군을 분류했으며 모든 함수·수식·입력 조합을 전수 검증했다는 뜻은 아니다.

### A. 게임과 기반

| ID | 기능 | 판정과 근거 |
|---|---|---|
| A01 | 시간·입력·Result·queue·snapshot | 유지. 멀티스레드 순서·소유권 계약이다. `Source/Core` |
| A02 | GameDomain/GameRules/Gameplay/Presentation | 유지. CPU 단독 빌드와 게임/표현 분리를 지킨다. `ARCHITECTURE.md` |
| A03 | 전투·AI·내비·충돌·상태·스킬·유물·진행 | 유지. 실제 인게임 기능. `Gameplay/Private/simulation_*` |
| A04 | 매 tick checksum | 유지. 저장된 checksum을 반환하는 API와 replay·결정론 계약. 무계산 모드 도입은 의미 변경이다. |
| A05 | BalanceObserver | 기능 유지, **후속 적용:** 누적 cast 선형 검색을 archetype별 연속 저장·이진 검색으로 교체. 결과/상세 UI 통계는 보존. |
| A06 | Taskflow/Jobs | backend 유지. **후속 적용:** 미사용 priority/숫자 handle/TaskDesc 제거, 작업명+승인 bool로 축소. admission·cancel·join 보호 유지. |
| A07 | main/simulation/render 스레드 | 유지. 큐·입력·종료·GPU 수명과 연결. 스레드 축소는 별도 성능 설계다. |
| A08 | 저장·설정 migration | 유지. 원자 저장과 미래 버전 덮어쓰기 방지는 사용자 데이터 보호다. |
| A09 | 메뉴/HUD/설정/DirectWrite | 유지. 실제 게임 UI이며 같은 모델 재래스터화 생략도 이미 있다. |
| A10 | XAudio2/X3DAudio/BGM/SFX | 유지. voice 제한·음원 수명은 런타임 기능. 제작 AI 패키지와 구분. |
| A11 | snapshot/event/projection | **적용:** 완성된 revision 중복 계산 생략. 이벤트 순서와 표현 결과는 보존. |
| A12 | typed VFX/persistent/ribbon | 유지. owner·seed·TTL·rebind·보스 예고는 실제 동작. 파일 수만으로 범용 VM에 합치지 않는다. |

### B. AI 테스트와 관측

| ID | 기능 | 판정과 근거 |
|---|---|---|
| B01 | ProjectHS smoke | 유지. seed/tick 제한·실제 화면·종료 코드가 AI 기본 환경이다. |
| B02 | CombatSim/밸런스 행렬 | 기본 실행 유지. 대형 행렬은 명시 실행. 게임 구현을 복제하지 않는다. |
| B03 | Experiment spec/probe/assertion | 유지. 재현 가능한 실행 제어. 모든 상세 보고 체계를 기본 플레이에 요구하지 않는다. |
| B04 | Named Pipe timeline 왕복 | **후속 적용:** 중복 전송·응답·동일성 검사·pipe 구현 삭제. 검증된 spec 직접 실행, 같은 tick의 sequence 순서와 watchdog 유지. |
| B05 | watchdog/timeout/process 관리 | 유지. 멈춘 실험 프로세스를 정리하는 최소 안전장치. |
| B06 | record/replay/compare | 유지. 입력·seed·콘텐츠 identity로 재현. |
| B07 | 일반 진단 로그 | **적용:** ordinary 상세 수집·포맷·필수 IO 제거. 요청한 capture는 실패를 숨기지 않는다. |
| B08 | 성능 CSV/GPU/PDH 분석 | 명시적 조사만 유지. **적용:** production 미호출 writer 제거. |
| B09 | 캐릭터/보스/스킬/environment preview | 유지. AI가 실제 장면을 재현하는 용도. |
| B10 | RenderTestRunner/GPU fixture | 유지. CPU 테스트는 GPU 수명·이미지 결과의 대체물이 아니다. |
| B11 | 소스 문자열 고정 검사 | **적용:** 3개 파일의 구현 형태 검사 축소. 수학·table 데이터 검사 유지. 다른 ABI/resource 관련 검사는 개별 결과 검사 확보 후 정리. |
| B12 | 이미지 baseline/barrier 비교 | 해당 렌더 변경에만 사용. 실패를 없애기 위해 baseline을 갱신하지 않는다. |
| B13 | architecture 검사 | 유지. 작은 검사로 모듈 경계와 OFF/OFF 빌드를 보호. 별도 parser 도입 없음. |
| B14 | 빌드/게시 정확성 검사 | **적용:** 별도 MSVC 의존성 감사 제거. 실제 변경/무변경/실패 게시 동작 검사는 남긴다. |
| B15 | verify/metadata/receipts/Changed | **적용:** 영향 추론·영수증·자동 복구 제거, 명시 선택+CTest로 대폭 축소. |
| B16 | VFX 단위 검사 | 유지. 세션/owner/reset은 실제 게임 회귀. 반복 fixture 정리만 후속. |
| B17 | Windows/Linux CI | 유지. 플랫폼 차이를 검출한다. runtime compile은 GPU 전체 검사가 아니다. 중복 job 조정은 후속. |
| B18 | ASan/GPU validation/ImGui | 필요할 때만 유지. Release 경로의 guard를 삭제하거나 GPU 오류를 감추지 않는다. |

### C. 빌드와 콘텐츠

| ID | 기능 | 판정과 근거 |
|---|---|---|
| C01 | native targets/AssetActions/DEPFILE | 유지. 필요한 부분만 빌드하는 핵심 기능. |
| C02 | hs_dev build/Action alias/dry-run | **적용:** 작은 명시 build 진입점 유지. 최초 Action 결과가 없으면 native 타깃 사용. |
| C03 | why/deps/rdeps/path/affected/status/doctor | **적용:** 중복 facade 삭제. 필요 시 Ninja query 직접 사용. |
| C04 | source/compiler/header recipe 인증 | **적용:** 연결된 실행 파일 SHA-256으로 교체. 자체 closure·compile DB·링크 명령 인증 제거. |
| C05 | ActionKey/ProductSet/input-output hash | 유지. 제작기 변경 무시·제품 혼합 방지. source identity 재탐색은 제거했다. |
| C06 | Selected Generation/원자 게시 | 유지. 게임이 반쯤 제작된 제품을 읽지 않게 한다. |
| C07 | GC/repair/quarantine/pin/receipt | **일부 적용:** hs_dev GC facade·verify 자동 repair 제거. native 관리 기능은 후속 축소. online GC를 유지하며 reader lock만 없애면 위험하다. |
| C08 | 전체 Generation hot reload/rebind | 후속 축소 후보. 재시작 workflow를 채택하면 큰 삭제 가능. 현재 VFX·resident 후보·old generation 수명과 얽혀 있어 동작 보존 삭제로 취급하지 않는다. |
| C09 | export/install/CPack | 유지. 배포·재현 가능한 실행에 필요. |
| C10 | 개별 producer 실행 파일 | 유지. 거대 cooker로 합치면 작은 수정이 전체 제작을 무효화한다. |
| C11 | legacy oracle/shader projection | **적용:** 실행 파일·전용 orchestration·shader 중복 복사 삭제. 실제 cooker 공유 코드는 보존. |
| C12 | ContentTools CLI | **후속 적용:** 실행 파일 전체 제거. 중복 build/launch wrapper와 범용 숫자 편집기 대신 원본 JSON 편집·실제 제작기 검증·native 빌드 사용. |
| C13 | runtime와 제작 SDK | 후속 분리. FBX는 content-only지만 DirectXTex는 runtime capture에도 쓰이므로 단순 전부 제거 불가. |
| C14 | 큰 CMakeLists/presets | 기존 target/graph 유지. **적용:** 구형 타깃/검사 삭제, core test ON 명시. 파일 쪼개기만으로 성능 개선이라 부르지 않는다. |
| C15 | offline geometry Debug /O2 | 유지. 기준 커밋에 이미 적용됨. 기존 사용자의 HLOD 수정도 보존. |
| C16 | Schema/binary bounds/input 검사 | 유지. 손상 패키지·NaN·범위 초과는 실제 데이터 경계 문제. |

### D. 렌더링

| ID | 기능 | 판정과 근거 |
|---|---|---|
| D01 | D3D12 device/swapchain/fence/D3D12MA | 유지. 단일 concrete backend와 실제 GPU 수명 처리. |
| D02 | FrameGraph/barrier/alias/multi-queue | 유지. exact plan cache가 이미 있음. 임시 준비 작업만 추가 측정 후보. |
| D03 | FeatureRegistration/SignalMetadata | **후속 적용:** 중복 semantic graph 삭제. AO/GI/reconstruction 직접 연결, 고정 post chain. 실제 자원 검사는 native 서비스·FrameGraph에 유지. |
| D04 | worker recording/queue submit | 측정 후 묶음 조정. 병합이 GPU overlap을 악화시킬 수 있다. |
| D05 | 다중 view/history | 후속. Runtime main view 하나, RenderTestRunner 다중 view 사용. probe/shadow view와 구분. |
| D06 | GPU scene delta/HZB/visibility/indirect | 유지. 실제 적·초목 처리와 연결. count/prefix/scatter도 이미 적용됨. |
| D07 | cluster/HLOD/LOD | 측정 후 축소. fallback·streaming·가시성 계약. 사용자 진행 중 변경도 보호. |
| D08 | texture streaming/residency/IO | 유지. resident-only는 VRAM·시작 지연 tradeoff를 먼저 측정. |
| D09 | DirectStorage | 기본 OFF, 후속 지원 제거 후보. 기본 게임의 FPS 비용이라고 주장하지 않는다. |
| D10 | native SR/FXAA/XeSS/FSR/Streamline/DirectSR | native 유지. 외부 지원은 실제 사용자 preset 선택 후 축소. 기본 OFF와 미사용은 같지 않다. |
| D11 | frame generation/latency/input token | 후속 지원 축소. 제거는 프레임 표시 기능 변경이다. |
| D12 | Brixelizer GI | 기본 OFF, 후속 지원 제거 후보. Probe 기본 경로는 유지. |
| D13 | Probe IBL/local capture | 유지. arena 4 probe, revision/cache/required 조건이 이미 있음. 매 frame 무조건 rebake 아님. |
| D14 | CSM/virtual/local/contact shadow | 유지. CSM은 virtual shadow fallback이므로 단순 중복 아님. |
| D15 | GTAO/adaptive/VRS | 기본 AO 유지. adaptive 고정 해제는 실제 GPU 비용 비교 필요. |
| D16 | SSR/refraction/OIT | 유지. 기본 설정·투명 slime 등에 실사용. |
| D17 | material graph/IR/BSDF | 실사용 연산 유지. 제한된 authoring 체계로 바꾸는 작업은 기능 축소이며 별도 범위. |
| D18 | terrain stochastic/histogram/POM/meadow | 유지. 실제 terrain graph. 샘플 감소는 시각적 동등 최적화 아님. |
| D19 | SSS/LTC/volume | 피부 SSS·실제 atmosphere/VFX 유지. LTC 일반 게임 광원 사용 여부는 추가 추적 후 판단. |
| D20 | skin cache/animation/grass | 유지. 여러 pass의 중복 작업을 막고 motion history를 공유한다. |
| D21 | HDR/dynamic resolution | 후속 지원 축소. HDR Auto 기본, dynamic resolution OFF. 삭제는 지원 기기/설정 변경. |
| D22 | bloom/tonemap/outline/grade/blur/DoF | 기본 화면 효과 유지. blur/DoF 기본 OFF이며 후속 지원 축소 후보. |
| D23 | graphics descriptor/request/effective/UI | 유지. 실제 적용 상태와 저장 ID를 공유한다. 삭제 시 ID 재사용 금지. |
| D24 | renderer capture/diagnostics | 기본 screenshot/오류 유지. 일반 상세 로그는 Runtime에서 축소. 시작 시 adapter/DRED 등 best-effort 진단은 유지. |

### E. 제작과 기록

| ID | 기능 | 판정과 근거 |
|---|---|---|
| E01 | Blender/FBX/texture authoring | 제작 시만 사용. 원본·재생성 경로 보존. |
| E02 | AI 음원 Python 패키지 | 엔진 기본 환경에서 제외된 선택 제작 도구로 유지. runtime audio와 구분. |
| E03 | LSP/compile_commands | 유지. 선택 코드 탐색 도구이며 게임/테스트의 필수 prerequisite 아님. |
| E04 | repowise/usage/measure-lsp/sccache harness | **적용:** 네 파일 제거. 측정 이력은 현재 기능으로 취급하지 않는다. |
| E05 | Docs/Plans/DEV_STATE | **적용:** 현재 지침·현황 갱신. 과거 설계 문서는 현재 인증 계약이 아니라는 주석 추가. |
| E06 | Archive/GeneratedAudio/제작 verification 원본 | 보존. 이름만으로 미사용 판단해 사용자 원본을 삭제하지 않는다. |
| E07 | ThirdParty/pin/license | 실사용 의존성 유지. 새 dependency 추가 없음. |

## 보고서에서 바로 채택하지 않은 주장

- 상세 기여도는 `game_simulation.cpp`의 summary에서 `snapshot_projector.cpp`의 결과/캐릭터 UI로 연결된다. 전부 opt-in으로 만들면 게임 표시를 잃는다.
- `archer_skin`의 SSS, `archer_cloth`의 sheen/anisotropy, grass/leaf transmission, monster emission, slime transmission/coat가 실제 authored material에 있다. “고급 BSDF 제거”는 현재 화면을 바꾸는 제안이다.
- FrameGraph plan cache, UI model equality skip, render instance count/prefix/scatter는 이미 존재한다. 새 최적화로 중복 계산하지 않는다.
- local probe는 revision·cache·capture-required 조건을 사용한다. 전체 probe 제거의 이득을 매 frame rebake 비용으로 과장하지 않는다.
- OFF SDK 제거는 관리 범위를 줄이지만 현재 기본 실행의 FPS 상승 근거가 아니다.

## 남은 큰 축소 후보의 순서

1. **정적 Experiment timeline의 pipe 왕복 제거.** AI spec/timeout/probe/capture는 유지하고 이미 가진 명령을 다시 직렬화·송수신하는 부분만 직접 전달로 바꾼다.
2. **전체 asset hot reload 대신 재시작.** shader/parameter reload만 유지하면 Generation 후보·VFX rebind·online GC 범위를 함께 줄일 수 있다. 개발 workflow는 달라진다.
3. **선택 렌더 SDK 지원 범위 고정.** 실제 사용하는 외부 provider만 남기고 DirectSR/Brixelizer/DirectStorage 등 비사용 지원을 소스·설정·배포까지 제거한다. 현재 사용자 save 설정은 이번 조사에서 읽지 않았다.
4. **고정 feature 연결.** RenderFeature composition을 실제 조합으로 제한한다. 모든 GPU resource validation을 무조건 없애는 작업으로 확대하지 않는다.
5. **HLOD/streaming/VSM/material IR 범위 축소.** 현재 장면 크기·VRAM·화질·최악 frame을 비교한 뒤 결정한다. 코드 줄 수만으로 성능 개선을 주장하지 않는다.

## 추가 검토: 통합·축소·로직 변경·개선

아래는 1차 경량화가 반영된 작업 트리를 다시 조사한 제안이다. 이후 1–6, 정적 Experiment 전송·규칙 감시·고정 렌더 연결·시작 설정 통합·신규 Generation 복사/해시·Jobs API 축소를 적용했다. 구체적인 최종 변경·검증은 후속 절을 따른다. 나머지는 미적용이다. 위 표의 “유지”는 현재 기능이 사용된다는 뜻이며, 현재 구현 구조가 최선이라는 뜻은 아니다.

### 먼저 정정할 현재 동작

검토 당시 `Source/Apps/ProjectHS/command_line.cpp:19`는 Debug에서 `record_playtest=true`를 기본으로 설정했고, `Source/Runtime/Private/application.cpp:517`은 녹화를 상세 진단 조건에 포함했다. 따라서 1차 변경의 일반 실행 진단 비활성화는 **녹화하지 않는 실행에 한정**됐고 Debug 자동 녹화는 예외였다. 이후 병목 해결 작업에서 이 기본값을 제거하고 기본·명시적 옵션·옵션 순서 검사를 추가했다. 현재 Debug도 `--record-playtest` 등으로 요청할 때만 녹화한다.

### 우선 적용을 권하는 작은 변경

| 순서 | 현재 → 변경안과 코드 근거 | 기대 효과·제약 | 필요한 확인 |
|---|---|---|---|
| 1 | **Debug 자동 녹화 제거.** `command_line.cpp:19,226`에서 기본 녹화만 끄고 명시적 record/output/replay 옵션은 유지 | Debug 평상시 tick 기록과 상세 진단을 줄인다. 사후 문제의 자동 녹화 자료는 잃는다 | Debug/Release 기본값·옵션 순서, 명시적 녹화→리플레이 |
| 2 | **recipe 중간 JSON 제거.** `cmake/AssetRecipeIdentityBuild.cmake:8`은 EXE 해시를 기록하지만 `Source/Apps/AssetPipeline/action.cpp:754`는 버전만 읽고 EXE를 다시 해시한다. 실제 EXE 의존성과 프로세스당 1회 해시로 통합 | 중복 해시·JSON·post-link 준비 타깃 제거. 실행 파일 변경 감지는 유지. recipe manifest 조회 인터페이스는 사라진다 | cold 단일 target 빌드, EXE 변경 감지, no-op/restat, fixture 인자 정리 |
| 3 | **Action 공통 구현을 라이브러리로 통합.** `CMakeLists.txt:298,810,879,950` 등 19개 producer가 같은 `action.cpp`를 각각 컴파일한다. 2번의 producer별 매크로를 제거한 뒤 공통 static library로 링크 | 중복 컴파일 감소. producer 실행 파일과 개별 Action은 유지해 부분 빌드 범위를 보존한다 | audio 단독 빌드, shader 전용 변경이 audio cook을 유발하지 않음, 공통 Action 변경 시 필요한 재링크 |
| 4 | **장면 delta 복사 제거.** `Source/RendererD3D12/Private/renderer_frame.cpp:575–625`의 연속 delta 경로에서 vector 두 개를 복사한다. 기존 `RenderSceneDeltaView`로 snapshot span을 빌리고 fallback만 owning delta를 생성 | 연속 경로의 복사·할당 감소. 마지막 소비까지 snapshot 수명 유지 필요. publisher와 성공한 GPU submit의 이력은 따로 유지 | generation 건너뛰기·abort·reset·revision mask, 생성/삭제 smoke, submit 후 Commit 보존 |
| 5 | **전투 통계 중복 판정 개선.** `Source/Gameplay/Private/simulation_combat.cpp:3650,3680`의 누적 cast vector 선형 검색을 집합 삽입 결과로 변경 | 긴 세션에서 검색 비용 감소 후보. 통계/UI는 그대로 유지. `unordered_set`은 노드 메모리·할당이 늘 수 있으므로 무조건 더 가볍다고 단정하지 않는다 | 다중 대상·지속 피해·중복 cast·reset·결정론 비교, 짧은/긴 세션 CPU·메모리 측정. 집합 순서를 시뮬레이션에 사용하지 않음 |
| 6 | **manifest를 한 번 읽고 검증.** `Source/Apps/AssetGeneration/main.cpp:1278,1299`의 parse와 canonical 비교를 동일 bytes로 처리. `:1314`의 GC가 다시 LoadGeneration으로 진입하는 중복도 통합 | 파일 재읽기·재파싱 감소. 서로 다른 시점의 파일을 비교하는 문제도 줄인다. schema/identity/path 검증은 유지 | schema 1 GC, 2·3 로드, 비정규 bytes·잘린 JSON·잘못된 ID 거부 |

### 실행 구조를 바꾸되 필수 기능은 보존하는 변경

| 후보 | 현재 → 변경안과 코드 근거 | 줄이는 책임·보존할 계약 | 검증 및 위험 |
|---|---|---|---|
| **정적 Experiment 전송 통합** | `Source/Apps/Experiment/main.cpp:122,219,528`: 자식이 spec을 읽은 뒤 부모가 동일 timeline을 pipe로 다시 전송한다 → 검증된 spec을 자식이 바로 Runtime 명령으로 변환 | 초기 명령의 직렬화·전송·동일성 검사 제거. 부모 watchdog/프로세스 격리, sequence·tick·payload 검증과 결과는 보존 | 같은 seed/timeline의 checksum·health·이벤트 순서, 잘못된 입력과 timeout 검사. 기존 pipe 검사는 timeline 동작 검사로 바뀌며 별도 실제 pipe 사용자가 없을 때만 transport 제거 |
| **시작 설정 적용 통합** | `Source/Apps/ProjectHS/main.cpp:27–38`의 설정 읽기·CLI 재파싱과 `application.cpp:592–598`의 설정 읽기·적용 → Runtime에서 저장값에 명시적 CLI override를 한 번 적용 | 설정 파일 IO·두 단계 해석 감소. 저장값 < CLI, smoke 독립 기본값 유지 | `application.cpp:763` 창 생성과 `startup_graphics_settings.hpp:30`까지 함께 바꿔야 함. 저장된 해상도/borderless/bloom=false/outline=false, CLI, 손상 파일 검사. main의 로드만 삭제하는 수정은 잘못됨 |
| **불변 Generation 규칙과 충돌하는 감시 축소** | `application.cpp:3376–3398`은 시작 때 선택된 rules 파일을 250ms마다 감시해 메뉴에서 교체한다 → 규칙 변경은 새 Generation 게시 후 재시작 | `simulation_rules_updates` 생산/소비 큐와 감시 분기 제거. renderer/shader/VFX 교체 경로는 유지 | rules 파일을 제자리 수정하는 메뉴 hot reload를 잃음. 새 Generation 재시작 smoke와 기존 renderer reload 검사. 다른 호출자가 있는 Gameplay API까지 제거하지 않음 |
| **신규 Generation 복사·해시 통합** | `AssetGeneration/main.cpp:1034,1147,1150`: source 전체 해시→복사→destination 전체 해시 → 복사하면서 source digest 계산 | 제품 데이터 읽기 3회→2회 후보. `AssetPipeline/action.cpp:689`의 copy-and-hash 방식 재사용. 최종 destination 검사·원자적 manifest 게시 유지 | 손상 원본·복사 실패·재시도 검사. source 오류 감지 시점이 staging 중으로 이동. 다른 기존 Generation 재사용 경로와 분리해 설계 |
| **고정 렌더 기능 연결** | `Source/RendererD3D12/Private/render_features.cpp:38–153`의 registration/map/인접 행렬/위상 정렬 → 기존 `renderer_post_subgraphs.hpp` 같은 typed 입력·출력으로 AO/GI/reconstruction을 명시적으로 연결 | 범용 semantic registry와 매 frame 임시 container 축소. extent/encoding/history 검사는 연결 경계로 옮기고 FrameGraph 자원·queue·alias 검사는 보존 | 임의 기능 등록·자동 정렬 능력은 잃음. 실제 provider/fallback·GI·bloom/outline 조합의 pass read/write, metadata 오류 검사, GPU validation 필요 |
| **테스트 실행 파일 통합** | `CMakeLists.txt:2399–2535`의 VFX별 별도 main/링크 → synthetic 입력 검사끼리, cooked fixture 검사끼리 소수 실행 파일로 통합하고 case 인자로 CTest 이름 유지 | 링크 산출물·반복 등록 감소. 작은 테스트에 runtime-full 콘텐츠 준비를 강요하지 않는다 | 한 테스트 수정 시 통합 바이너리 재링크가 넓어지는 절충. fixture와 비fixture를 합치지 말고 기존 실패 exit/선택 실행/개별 이름 유지. 새 테스트 프레임워크 도입은 불필요 |
| **작업 제출 API 축소** | `Source/Jobs/Public/hs/jobs/task_system.hpp:15–35`, `Private/task_system.cpp:105,152`: Normal만 있는 priority와 조회 API가 없는 숫자 handle → priority 제거, 제출 승인 bool 반환 | 사용되지 않는 정책/ID와 제출당 atomic ID 증가 제거. 작업 이름은 오류 설명에 사용하므로 유지 | 이득은 작아 후순위. 취소·join·sealed admission·예외 완료 보장은 유지. failure injection/reentry 검사 및 모든 호출자 갱신 필요 |

### 기능 지원 범위를 줄이는 큰 변경

1. **renderer당 presentation camera 하나로 제한.** Runtime은 `application.cpp:2286`에서 main view 하나를 보내지만 `Source/RendererD3D12/Private/renderer_views.cpp:1014–1194`는 view 교체·provider 상태·rollback을 지원한다. 테스트 preview는 독립 실행으로 옮기면 view 상태 교환을 줄일 수 있다. 한 renderer에서 같은 particle 상태를 공유하는 다중 카메라 기능은 잃으며, 별도 실행의 시작 비용·GPU 메모리는 늘 수 있다. probe cubemap과 shadow cascade까지 없애는 제안은 아니다. camera cut/resize/history, preview, SDK token 수명을 확인해야 한다.
2. **범용 material graph를 실제 재질 계열 recipe로 전환.** `Source/RenderData/Private/material_ir.cpp:70–89,186–265,1695–1848`의 언어·형 검사·최적화·코드 생성을 skin/cloth/foliage/slime/terrain/decal 계열의 고정 shader와 binding/parameter로 좁힌다. 실제 SSS·sheen·anisotropy·transmission·coat·terrain 연산은 유지할 수 있다. 임의 노드 조합 능력은 잃고 새 계열은 코드 수정이 필요해진다. 모든 authored material의 binding/layout/opacity/closure, parameter-only 부분 빌드, 대표 화면 비교가 이관 완료 조건이다. GPU 성능 개선과는 별개의 관리 범위 축소다.

### 권하지 않는 통합과 적용 순서

- **producer 실행 파일 일괄 통합:** 한 제작기 변경이 모든 recipe 해시를 바꿔 불필요한 cook을 넓힌다. 공통 구현만 공유한다.
- **Gameplay·Presentation·GameRules의 단일 모듈화:** GameRules는 콘텐츠 제작기도 사용한다(`CMakeLists.txt:818,1454,1576`). 합치면 콘텐츠 제작기에 mutable simulation 의존성까지 퍼진다. CPU 단독 테스트·부분 빌드에 유리한 경계는 유지한다.
- **두 scene 이력의 무조건 통합:** 발표한 snapshot과 실제 GPU 제출에 성공한 snapshot은 다를 수 있다. delta 데이터 복사만 줄인다.
- **실사용 재질 효과 삭제를 동일 동작 개선으로 표시:** 지원 범위 축소와 화면 변경을 구분한다. 고정 recipe로 이관하는 것과 SSS/transmission 자체를 지우는 것은 다른 작업이다.

권장 순서: **Debug 녹화 기본값 → recipe JSON 제거+Action 공통 라이브러리 → delta view → 중복 manifest IO → 전투 통계 알고리즘 → Experiment/설정 통합 → 고정 렌더 연결**. 테스트 바이너리/Jobs API는 작은 관리 개선이므로 후순위다. 다중 카메라·material 언어 축소는 지원 범위를 정한 뒤 별도 이관으로 다룬다.

추가 검토 자체는 소스와 호출·의존성을 대조한 결과다. 이후 적용·측정한 내용만 다음 절에 기록하며, 나머지 제안의 효과나 검증을 완료했다고 주장하지 않는다.

## 후속 병목 조사와 해결

| 관측한 병목 | 근거 | 대응 |
|---|---|---|
| 동일 Action 구현 중복 컴파일 | validation Ninja 로그에 `action.cpp.obj` 18개. 각 최신 명령의 wall duration 합계 246.666초 | 별도 producer EXE는 유지하고 공통 static library로 통합. 활성 구성은 한 번 컴파일(이번 6.602초). 19번째 producer는 선택 Brixelizer 타깃. 병렬 작업 시간 합계를 전체 빌드 시간 절감으로 해석하지 않음 |
| 중복 recipe 해시/준비 파일 | EXE 해시 JSON을 만들지만 Action은 그 값을 쓰지 않고 실행 EXE를 다시 해시 | JSON·CMake 준비 타깃 제거. 실제 EXE가 Ninja/depfile 의존성이며 각 producer 프로세스에서 한 번 해시. cold/no-op/변경 감지/게시·복구·동시 실행 fixture 통과 |
| 기본 Debug의 자동 녹화 | `_DEBUG` 기본값이 tick 기록과 상세 진단을 활성화 | 기본 녹화 제거. 명시적 녹화·출력·replay·compare와 옵션 순서 보존. Debug core 18/18 통과 |
| 검증 종료 전까지 진행을 볼 수 없음 | wrapper가 모든 native 출력을 파일에만 redirect. 빈 세션 조회와 별도 로그 확인 반복 | 기존 출력·단계를 즉시 표시하고 같은 로그에 저장. JSON stdout과 진행 stderr 분리. 종료 전 flush, 오류 코드 23 보존, JSON 파싱 fixture 통과 |
| GPU 검증 첫 프레임 지연 | 전체 CTest 1,595.31초 중 dynamic LOD 342.76초·streaming 478.72초·material reload 401.11초 | 별도 namespace의 영속 cache를 실측했지만 첫 프레임 지연을 해결하지 못해 변경을 되돌림. 검증 강도·시간 제한 유지 |

`verify`의 전체 build 다음 test-preparation은 기존 로그에서도 `ninja: no work to do.`였다. 이것을 중복 컴파일 원인으로 취급하지 않는다. core preset의 명시적 타깃 범위를 불필요하게 넓히지 않도록 CMake/CTest 준비 구조는 유지했다.

기존 GPU 결과의 첫 프레임 CPU는 각각 331.258/465.416/345.122초였지만 첫 프레임 GPU는 188.75/262.79/109.41ms였다. 초기화는 4.15/4.79/6.48초이며 이후 frame CPU는 약 0.09–1.63초였다. 이는 실제 게임 GPU 연산량보다 first-use GBV shader patching/driver 준비가 주된 비용이라는 강한 근거다. 특정 API 내부 시간을 분리 측정한 확정은 아니다. Microsoft의 [GBV PSO patching 설명](https://learn.microsoft.com/en-us/windows/win32/api/d3d12sdklayers/ne-d3d12sdklayers-d3d12_gpu_based_validation_pipeline_state_create_flags)은 기본 patching 시점이 첫 command-list 사용임을 설명한다.

기존 GBV는 일반 PSO cache와 disk identity가 같다는 이유로 persistence가 꺼져 있었다. 검증 설정은 그대로 두고 `pso-cache-gbv` 별도 namespace로 cold/warm 재사용 실험을 수행했다. [Pipeline library serialization](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12pipelinelibrary-serialize)은 다른 프로세스의 재사용을 지원하지만 patched shader까지 보존한다는 보장은 확인하지 못했다.

| 동일 dynamic LOD GBV 검사 | Cold | Warm |
|---|---:|---:|
| 전체 시간 | 459.63초 | 412.65초 |
| 초기화 | 63.16초 | 23.59초 |
| 첫 frame CPU | 390.24초 | 382.01초 |
| 첫 frame GPU | 195.65ms | 203.60ms |
| native PSO 생성 | 154 | 0 |
| cache hits | 24 | 178 |
| 검증 오류 | 0 | 0 |

두 실행 모두 8 frame, exit 0, execution_valid=true이며 실행 파일·Generation SHA256, fixture, 옵션, 동적 LOD 검사 결과가 같았다. warm에서 7,175,492바이트를 정상 로드했다. **native PSO 생성이 없어져도 첫 frame CPU 382초가 남으므로 이 캐시는 주된 병목의 해결책이 아니다.** 단일 cold/warm 쌍의 전체 시간 차이를 캐시 효과로 확정하거나, 다른 시점의 기존 342.76초와 통제된 비교로 취급하지 않는다. 효과가 불충분한 renderer 변경은 되돌렸고 원본 측정은 `Build/verification/gbv-cache-20261020T231026675/comparison.json`과 `cold/`, `warm/`에 보존했다.

남은 GPU 검증 지연은 first-use patching/driver 준비가 강한 가설이지만 개별 API stack별 CPU 시간을 분리 측정하지 않았다. 시간을 초기화로 옮기는 front-load 옵션이나 검사 장면·검증 강도를 줄이는 방식은 동일 검증의 성능 개선으로 채택하지 않았다. 변경되지 않은 검사 결과를 재사용하고 명시적으로 필요한 테스트만 선택하는 현재 작업 경로를 유지한다.

추가 관측: HLOD cook 196.265초, test-render Generation 준비 80.081초, BSDF shader 63.657초도 이전 Ninja 로그에 있다. 이번 작업에서 사용자 HLOD 품질·오차 예산을 더 낮추거나 GPU 검증·timeout 기준을 완화하지 않았다.

추가 검증: core 18/18, 관련 콘텐츠·런타임 통합 6/6 통과. 제작기 두 타깃과 전체 콘텐츠 Generation을 각각 다시 요청해 모두 `ninja: no work to do.`를 확인했다. 삭제된 recipe 타깃을 호출하던 Linux CI 명령도 갱신했다. Linux 실행은 이 Windows 환경에서 수행하지 않았다.

GPU 캐시 실험을 되돌린 최종 코드로 ProjectHS와 render runner를 재빌드하고 렌더 smoke 1/1을 다시 통과했다: `Build/verification/verify-20261020T142619056Z-7324.log`.

증거: `Build/engine-lean-audit/action-compiles-before.json`, `action-compiles-after.json`, `portable-shared-action.log`, `partial-build-noop.log`, `content-build-noop.log`, `output-fixture/`; `Build/verification/verify-core-20261020T140219821Z-32844.log`, `verify-20261020T140325359Z-35476.log`.

## 추가 경량화: 다른 엔진과 비교한 구조 개선

참조한 원칙과 이 엔진에서 내린 판단을 구분한다.

- [Filament FrameGraph](https://google.github.io/filament/notes/framegraph.html)는 pass/resource의 실제 의존성과 자원 수명을 표현한다. Project HS에서는 이 역할이 기존 FrameGraph에 이미 있는데 그 위에서 별도 semantic graph를 구성했다. 실제 AO/GI/reconstruction batch에는 기능 하나씩만 등록되므로 위상 정렬의 선택지가 없었다. 따라서 상위 graph를 제거하는 것이 타당하다고 판단했다.
- [Godot 직접 Server API](https://docs.godotengine.org/en/4.6/tutorials/performance/using_servers.html)와 [최적화 지침](https://docs.godotengine.org/en/4.6/tutorials/performance/general_optimization.html)은 상위 계층의 비용, 연속 데이터 접근, 알고리즘과 측정을 다룬다. 이를 참고해 구조체·스냅샷을 재포장하는 작업과 누적 선형 검색을 줄였다. Godot의 Server나 ECS를 새로 도입한 것은 아니다.
- [Unreal Automation](https://dev.epicgames.com/documentation/en-us/unreal-engine/automation-test-framework-in-unreal-engine)은 빠른 smoke와 content stress를 구분한다. 이 프로젝트도 짧은 CPU/AI 테스트와 무거운 GPU 검사를 명시적으로 선택한다. Unreal은 smoke를 1초 이내로 정의하지만 기존 HS `integration.render-smoke`는 GPU 초기화를 포함하므로 같은 속도 계약이라고 볼 수 없다. 테스트 이름만으로 가볍다고 가정하지 않는다.

### 실제 변경

| 대상 | 제거·변경 | 남긴 동작 |
|---|---|---|
| 렌더 기능 연결 | `FeatureRegistration`, `SignalMetadata`, semantic lookup 두 map, 인접 행렬, 위상 정렬, 임시 callback/vector 전달을 제거. 기존 typed AO/GI/reconstruction 입력을 바로 Register에 전달. Post pass 9–17을 고정 체인으로 생성 | bloom/outline/native·external reconstruction, transparent guide 7개, exposure, GI backing 접근, history write, UI 합성. native format/extent와 FrameGraph barrier/alias/lifetime 검사는 유지 |
| 장면 delta | 연속 published delta의 vector 두 개 복사 제거. snapshot lease 내 span을 사용하고 generation gap 때만 owning fallback 생성 | submit 성공 후 Commit, abort·reset·누락 generation 이력 |
| 전투 통계 | 전체 cast 이력 선형 검색을 증가 ID append + 나머지 lower_bound로 교체. enemy는 6 archetype별 vector | 지연 타격·중복 cast·zero cast·reset·기존 56비트 enemy identity와 UI 통계 |
| Experiment | 자식이 이미 읽은 spec을 부모가 Named Pipe로 다시 보내고 비교하는 protocol/API/queue 제거 | child process, watchdog, spec 경계 검사, 동일 tick sequence 정렬, assertion/result. `experiment.pipe`는 `experiment.timeline`으로 변경 |
| 규칙 hot reload | 250ms 파일 timestamp 조회, 메뉴 상태 재검사, rules 전달 큐·매 tick 소비 삭제 | 선택된 불변 Generation을 실행 동안 사용. 새 규칙은 빌드·재시작으로 적용. 별도 shader/renderer reload는 유지 |
| Generation 로더 | GC/일반 로더 통합. schema 2·3 GC의 읽기 3회→1회, 일반 로드 2회→1회. 같은 bytes를 parse와 canonical 비교에 사용 | schema 1 GC 전용, schema 2·3 identity/path/dependency 검증과 삭제 전 계획 수립 |
| SceneProbe Action | C++/HLSL 원본 17개 수동 해시와 renderer cpp의 CMake 의존성 제거. cache hit마다 encode→decode→전체 bytes 비교하던 경로 제거 | authored JSON/schema, producer EXE, 실제 upstream closure. cache hit는 기존 Action key·제품 digest 검증. 실제 native light/layout/material/pipeline identity는 실행 시 계산 |

SceneProbe 문제는 추측으로 추가하지 않았다. delta 복사만 바꾼 빌드에서도 `local_probes.spkg` digest가 바뀌었고 renderer source hash가 제품 revision 안에 들어가는 것을 확인했다. 이 제품은 native GPU bake 결과가 아닌 layout이다. 실제 native capture는 `renderer_local_probe_runtime.cpp`에서 instance·LightView·material·pipeline 상태를 별도로 추적한다. 코드 원본을 콘텐츠 identity에 섞는 경로를 걷어내 무관한 수정의 cook 전파를 차단했다.

렌더 feature 관련 5개 파일은 테스트를 포함해 약 810줄 순감소했다. 이는 유지할 로직의 감소이며 FPS 측정치가 아니다. 제거한 상위 선언 일치 검사는 같은 호출부가 만든 metadata끼리 비교하던 검사였다. 실제 입력 자원과 외부 JSON·제품 바이트의 검증은 유지했다.

### 측정과 검증 범위

독립 MSVC `/O2` cast 중복 판정 합성 측정:

| 고유 cast / 이벤트 | 입력 순서 | 기존 → 변경 |
|---|---|---|
| 8,192 / 73,472 | 증가 ID와 최대 256 지연 | 25.32 → 0.147 ms |
| 동일 | 무작위 | 25.23 → 2.88 ms |
| 65,536 / 589,568 | 증가 ID와 최대 256 지연 | 1,722.9 → 1.41 ms |
| 동일 | 무작위 | 2,287.0 → 309.85 ms |

같은 고유 ID 수의 vector capacity는 같았으며 작은 256개 무작위 입력은 0.0362ms로 동률이었다. archetype별 vector 객체의 고정 오버헤드는 늘어난다. 보관량은 여전히 고유 cast 수에 비례하고 순서가 뒤집힌 신규 ID 삽입은 O(n)이다. 노드별 heap 할당 없이 실제 증가 ID 경로를 빠르게 만든 절충이며 전체 시뮬레이션/FPS 개선을 의미하지 않는다.

확인한 결과: Core 18/18, 관련 runtime/Experiment/delta 5/5, native portable Action 검사 통과. portable 검사에는 schema 1/2/3 잘린 JSON·비정규 manifest 거부를 추가했다. 최종 렌더·SceneProbe·AI 통합 8/8도 통과했다(67.88초): post topology, transient graph, render smoke, local-probe revisions, scene-probe Action, Runtime, Experiment simulation/timeline. 실제 probe 실행의 `execution_valid=true`, `validation_errors=0`을 확인했다.

증거: `Build/engine-lean-audit/combat-hit-benchmark.cpp`, `combat-hit-benchmark.log`, `lean-second-build.log`, `lean-second-render-build.log`, `lean-second-runtime-tests.log`, `lean-second-portable.log`; Core 로그 `Build/verification/verify-core-20261020T143512118Z-29020.log`, 최종 통합 `Build/verification/verify-20261020T144136418Z-23492.log`.

마지막으로 `hs_content_generation`, `ProjectHS`, `hs_experiment`를 다시 빌드해 `ninja: no work to do.`를 확인했다(`Build/engine-lean-audit/lean-second-noop.log`). 제거한 semantic graph·pipe·rules queue의 잔여 production/test 호출자는 없으며 `git diff --check`도 통과했다.

선택 SDK Brixelizer/FSR/Streamline/XeSS는 현재 OFF다. 해당 native 등록 경로는 코드로 대조했지만 SDK 활성 실행을 검증한 것은 아니다. GBV 첫 프레임 장시간 지연은 앞 절의 미해결 상태이며 이번 경량화로 해결됐다고 주장하지 않는다.

## 2026-10-21: 시작 설정·게시 IO·Jobs·도구 통합

작은 문법 변경보다 책임과 데이터 흐름을 줄이는 기준으로 다음 후보를 적용했다.

| 대상 | 실제 문제와 변경 | 계약·비용 |
|---|---|---|
| 시작 설정 | ProjectHS main이 설정을 읽고 CLI를 재파싱한 뒤 Runtime이 다시 읽었다. main의 파일 IO·두 번째 파싱 제거. Runtime의 최종 설정으로 창도 생성 | 저장값 < 명시적 CLI, 저장 bloom/outline=false, borderless/해상도, 첫 실행 기본값 유지. smoke는 process 기본값/명시값을 사용. 손상·미래 버전 파일 처리는 기존 SaveStore 그대로 |
| 신규 Generation | source 해시 → 별도 copy_file → destination 해시의 3회 데이터 읽기를 source 복사 중 해시 → destination 검증의 2회로 통합 | 새 파일 덮어쓰기 금지, 쓰기/close 오류, 게시 전 destination 전체 검사·pending 정리·원자 게시 유지. 기존 Generation reuse/repair의 source 선검사는 유지 |
| Jobs | Normal 하나인 priority, 조회 기능 없는 숫자 handle, 이름만 남는 TaskDesc 제거. Submit은 작업명과 승인 bool을 사용 | 제출마다 atomic ID 증가 제거. 그룹 소유권·sealed admission·cancel/join·실패 기록 유지. Taskflow의 복사 가능한 callback 보관에 필요한 공유 작업 소유권까지 무조건 삭제하지 않음 |
| ContentTools | 저장소 내 호출자가 없는 별도 실행 파일 제거. build/launch wrapper와 범용 JSON 숫자 편집·rollback 경로를 폐기 | 원본 JSON 직접 편집, 실제 GameData validator, native producer/Generation 빌드와 게임 직접 실행으로 통일. `hs_build_config.json`의 불필요한 CMake 경로와 미사용 Cooked 경로 API도 제거 |

`hs_content_tools list/set/preview/cook/status/hot-reload/shader-reload`는 더 이상 제공하지 않는다. 새 wrapper로 대체하지 않았으며 README에 실제 제작기 검증과 native 타깃을 안내했다. 현재 validation 빌드의 낡은 ContentTools exe/ilk/pdb도 제거했다. 원본 콘텐츠는 수정하지 않았다.

TaskSystem을 자체 스케줄러로 교체하거나 cancellation·OOM 처리를 지우면 코드가 줄어도 실패/종료 계약이 깨진다. 반대로 priority와 handle은 실제 기능이 없었으므로 제거했다. 시작 설정의 `*_explicit` 표시는 저장값과 명시적 false를 구분하는 실제 의미가 있어 유지했다.

신규 게시 회귀에는 빈 파일, 64KiB 버퍼 여러 개를 넘는 제품, 뒤쪽 제품의 누락/크기/해시 오류, 이전 pointer·Generation 보존, 실패 트리 정리와 재시도, source와 snapshot의 독립 소유를 포함했다. 데이터 손상 검사와 삭제 전 검증을 성능 목적으로 무효화하지 않았다.

검증은 Core 18/18, 최종 관련 통합 6/6을 통과했다. 통합 범위는 시작 인자, 저장 그래픽 설정, Runtime, render smoke, Experiment timeline, audio Generation lifetime이다. native portable Action 전체 검사와 실제 GameData 13개 문서 검사도 통과했다. 새 fixture의 Windows 경로 구분자 기대값 오류는 경로 동등 비교로 수정한 뒤 전체 portable 검사를 다시 통과했다. ProjectHS·render runner·Experiment·Generation 빌드 후 `hs_content_generation`, `ProjectHS`, `hs_experiment` 재빌드는 `ninja: no work to do.`였다.

128 MiB 신규 게시를 구/신 실행 파일로 순서를 교대하며 각각 3회 측정했다. 중앙값은 0.388716 → 0.379143초(-2.46%)였으나 범위가 겹친다(구 0.365257–0.478840, 신 0.363509–0.405218초). 따라서 안정적인 시간 단축의 증거로 보지 않는다. 빌드 부하 없이 측정했지만 warm cache이며 물리 디스크 IO를 측정한 것은 아니다. 코드상 전체 데이터 읽기 3회→2회와 6개 결과의 동일 Generation ID·manifest·pointer·제품 바이트, 독립 `--verify` 통과를 확인했다.

증거: `Build/verification/verify-core-20261020T151344495Z-34200.log`, `verify-20261020T151927229Z-7548.log`; `Build/engine-lean-audit/lean-third-portable.log`, `lean-third-game-data-validation.log`, `lean-third-noop.log`, `io-20261021T001830/result.json`. 전체 FPS, 선택 SDK 활성 조합과 Linux 실행은 이번 검사 범위에 포함하지 않았다.

남은 큰 후보는 다중 view/SDK 상태 교환, material 언어의 고정 계열화, 테스트 실행 파일 통합이다. 앞의 두 항목은 실제 지원 기능·품질 조합을 바꾸며 이번 작업에서 삭제하지 않았다. 테스트 파일 통합은 링크 수를 줄여도 수정 때 재링크 범위를 넓히므로 파일 수만으로 개선이라고 판단하지 않는다.

## 2026-10-21: view 상태·material 분석·VFX 링크 통합

| 대상 | 적용 | 실제 비용과 유지한 계약 |
|---|---|---|
| 다중 view | 소비자가 없는 history key vector/resource map과 타입·등록 함수 삭제. 실제 사용하던 main output extent만 유지. preflight ID vector와 실행 순서 pointer vector 제거, 결과 vector는 한 번 reserve | history 등록 할당은 생성/교체/camera cut 시 발생했으며 매 프레임 비용이 아니었다. 매 프레임 임시 vector 두 개는 제거했다. Main 우선·보조 view 입력 순서, view별 temporal/history·공유 scene simulation 유지 |
| 캡처 API | format별 공개 capture 함수 4개를 private helper로 제한하고 외부 호출을 Capture(request)로 통합. 미사용 CaptureViewPng wrapper 삭제 | 다중 view 검사에서 발견한 실제 진단 오류 수정. runner가 소유권 검사 API를 우회해 Main texture와 마지막 보조 view의 공유 geometry buffer를 섞었다. 최종 진단은 실제 마지막 제출 view를 명시. stale Main geometry 요청 거부 회귀 추가 |
| Material | domain 조건과 전체 graph 검증 분리로 emitter/layer 검사에서 전체 검증 2회→1회. deferred emitter가 reflection 결과로 sidecar mask를 직접 결정 | 외부 mask 옵션과 호출부의 중복 AnalyzeClosure/Optimize 삭제. JSON 입력 검증·실제 패키지 경계 검증 유지. 기존 authored closure·opacity·binding·parameter-only cook 유지 |
| Material 빌드 경계 | material_ir.cpp를 공용 hs_render_data에서 RenderData 소유 hs_material_compiler로 분리. 실제 제작기 2개와 package test만 링크 | 최초 변경 빌드에서 compiler 수정이 무관한 제작기와 HLOD까지 재생성하는 병목을 확인했다. Ninja 직접 실행 파일 의존성은 기존 공용 archive 32개→compiler archive 3개. 런타임·무관한 제작기의 재링크 원인 제거. 실제 material 제품 변경에 따른 하류 cook은 유지 |
| VFX 테스트 | 11개 실행 파일을 synthetic 5개/cooked 6개의 두 실행 파일로 통합. 기존 TU는 각각 컴파일하고 case dispatcher만 추가 | Runtime 변경 시 링크 대상 11→2. 기존 CTest 이름·별도 프로세스·실패 코드·선택 준비 유지. synthetic은 Generation 준비 없음. 최초 개별 case 빌드는 그룹 전체 TU가 필요하며 TU 수 자체는 11→13 |

VFX 실행 파일 합계는 현재 MSVC RelWithDebInfo 산출물 기준 24,392,704→7,425,024 bytes다. PDB와 정적 라이브러리를 포함한 전체 빌드 디스크 사용량이나 링크 시간 측정치는 아니다. Ninja의 `test_prep/runtime.vfx-typed-decal`은 synthetic 실행 파일만, `test_prep/runtime.vfx-typed-flash`는 cooked 실행 파일과 test-vfx Generation을 준비한다. 무인자·알 수 없는 case는 두 실행 파일 모두 exit 2를 반환한다.

새 compiler target은 콘텐츠 도구 또는 RenderData 테스트가 켜졌을 때만 생성한다. 소스와 공개 API는 RenderData에 유지하며 별도 엔진 모듈을 만들지 않았다. 파일 이동 없이 링크 경계만 바꿨다. 기존 architecture 검사에 runtime/data→compiler 역의존 금지만 추가했으며, 제작기별 허용 목록이나 새 검증 프레임워크는 만들지 않았다. 최초 분리 때 공용 archive가 바뀌어 한 차례 재링크·recook이 발생하는 비용은 있다.

최초 owned-view 실행은 9개 view 제출·history·capture를 통과한 뒤 최종 Visibility 진단에서 `primitive 638 outside geometry [960,1008)`로 실패했다(`Build/engine-lean-audit/owned-view-fourth/result.json`). 기존 main-first 실행 순서는 바뀌지 않았으며 원인은 기존 runner의 legacy 직접 호출이었다. 수정은 실제 `Capture`의 view 소유권·geometry 범위 검사를 유지한다. 진단 실패를 무시하거나 검사를 건너뛰는 방식으로 처리하지 않았다.

소유권 수정 후 재실행에서는 별개의 CPU 진단 매핑 오류가 드러났다. raw raster 65921은 geometry record 379(page slot 42, flags 13, mesh 21)를 참조했다. geometry ordinal 14는 GroundDetail이며 실제 material ordinal은 15인데, CPU가 `page/3`을 material 번호로 간주해 variant 15를 기대했다. 셰이더가 기록한 16이 맞다. CPU 진단도 기존 `EnvironmentGeometryMaterialOrdinal`과 LOD 상수를 사용하도록 수정했다. geometry/material 간 다대일 매핑을 중복 구현하지 않도록 통합한 것이며 범위 검사는 유지한다.

source6 cluster 경로를 대조하니 실제 `cluster_refinement.hlsl`도 `CatalogIndex`를 material binding/variant로 기록하고 있었다. CPU가 이미 공용 매핑을 적용해 준비한 `MaterialPartition`을 사용하도록 두 곳을 변경했다. 진단만 맞춘 것이 아니라 실제 잘못된 재질 선택도 수정했다. authored cluster partition은 기존 catalog에서 0으로 제한되고, GPU 테이블은 material base를 더하므로 프로젝트·visibility 셰이더가 읽는 binding과 일치한다. 이 셰이더 수정은 material compiler 최적화의 출력 동등성 측정과 구분한다.

다중 view 자체는 동일 canonical deformation/particle 상태에서 AI preview와 capture를 검사하는 실사용 기능이다. 공유 descriptor bank를 재사용하기 전 GPU 완료 대기는 삭제하지 않았다. FSR는 이미 view별 context를 직접 선택한다. Streamline의 활성 상태 교환을 바꾸는 일은 SDK 조건부 코드·presentation/Reflex token 수명까지 건드리며, 현재 SDK 비활성 빌드만으로 안전성을 입증할 수 없어 적용하지 않았다. 단순히 별도 renderer/process로 옮기면 초기화와 메모리를 중복 부담한다.

참조한 [FSR 공식 문서](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-upscaler/)도 context 생성 시 GPU 메모리를 할당한다고 설명한다. [Filament material 문서](https://google.github.io/filament/main/materials.html)의 material/instance 분리처럼, 이 프로젝트도 구조적 shader와 변경 가능한 parameter의 분리를 유지하는 쪽이 적합하다고 판단했다. 고정 재질 계열로 전환해 현재 skin/cloth/foliage/slime/terrain 연산을 다시 구현하기보다, 실제 중복 분석을 제거했다. 다른 엔진의 성능 수치를 이 엔진의 개선 근거로 사용하지 않았다.

검증 결과:

- Core 18/18: `Build/verification/verify-core-20261020T153349264Z-5512.log`.
- 최초 관련 17/17: VFX 11개, material format/parameter-only cook, DRS, architecture, smoke, grass history. `Build/verification/verify-20261020T152812294Z-3880.log`.
- compiler 분리 후 material/VFX/architecture 5/5: `Build/engine-lean-audit/lean-fourth-isolation-tests.log`.
- 캡처·실제 cluster binding 수정 후 GPU 3/3(smoke·cluster retest·grass history): `lean-fourth-final-gpu.log`. runner 두 결과 모두 `execution_valid=true`, `validation_errors=0`.
- 최종 owned-view probe: 3패키지·9제출, Main 첫/중간/끝 입력 순서, history/camera cut, 중복/비정상 입력 거부, stale Main 진단 거부와 실제 capture view 진단 성공. `owned-view-fourth-final/result.json`의 `execution_valid=true`, `validation_errors=0`.
- compiler 처리 통합·분리만 적용했을 때 68개 shader/parameter Action, 340개 제품 digest가 동일했다. 실제 cluster binding 수정까지 적용한 최종 상태에서는 의도한 `render/shader/cluster_emit_cs.dxil` 하나만 달라지고 339개는 동일하다. `material-products-comparison.json`, `final-shader-comparison.json`.
- `hs_content_generation`, `ProjectHS`, `hs_render_test_runner` 최종 재빌드에 추가 작업 없음: `lean-fourth-noop.log`. 최종 Runtime Generation은 `276c695e6f26fd3ff15808ec2751fbf261b5f4c90cb91a8941c9b1748271d573`이다.

비용 근거는 `material-link-isolation.json`(32→3 직접 링크 의존), `vfx-links-before.json`/`vfx-links-after.json`(11→2 실행 파일)이다. 실패한 두 owned-view 결과도 위 artifact에 보존했다. 최종 `git diff --check` 통과. 현재 validation 폴더의 폐기된 VFX exe/ilk/pdb 11세트는 제거했으며 원본 콘텐츠와 기존 사용자 변경은 보존했다. 커밋하지 않았다. 전체 FPS·VRAM·링크 시간, 활성 선택 SDK와 Linux 실행은 측정하지 않았다.

## 1차 적용의 검증과 한계

아래는 이번 수정 상태에서 실행한 결과다. 기존 보고서의 과거 성공 기록을 재사용하지 않았다.

- 최신 원격 SHA와 현재 HEAD 일치 확인.
- 수정된 verify-core로 18/18 통과: 기본 AI 테스트 경로, 결정론, snapshot revision, 비활성 진단/명시적 실패 검사를 포함한다.
- 변경한 런타임·runtime shader producer·game data producer 타깃 빌드 통과.
- PCF 3,888 조합, sparse-effect 2개, SSR composition 3개 CPU 검사 통과. GPU 셰이더 동등성 증거로 간주하지 않는다.
- `check_hs_dev.ps1`은 PowerShell 7/Windows PowerShell 5.1에서 통과했다. native/Action 선택, 중복 제거, 잘못된 입력, JSON 출력과 실패 코드 전달을 검사했다.
- `check_verification_selection.ps1`은 실제 CMake/Ninja로 정확한 이름·label·fixture 자동 확장, 필요한 빌드만 준비, 성공 3/3과 실패 반환을 확인했다.
- 새 recipe manifest의 최초 생성·무변경·동일 바이트 touch·실행 파일 변경 fixture 통과.
- 수정된 `check_asset_pipeline_portable.py` 전체 통과. 실제 Audio 제작기로 실행 파일 변경 감지, 동일 ProductSet 보존, no-op, 다른 build 경로, 원자적 게시·복구·symlink·동시 실행을 확인했다. 로그: `Build/engine-lean-audit/recipe-action-test.log`.
- 전체 빌드 완료 후 통합 검사 108/109 통과. 마지막 shader hot reload 검사는 일반 실행에서 생성하지 않는 `render.json`을 요구해 실패했다. 테스트 실행 인자에 `--capture-diagnostics`를 추가하고 동일 검사를 재실행해 1/1 통과했다. 검증 조건은 유지했다. 이 테스트 호출부 외 실행 코드는 재실행 전에 변경하지 않았으며, 나머지 108개 결과를 재사용했다.
- 전체 실행 로그: `Build/verification/verify-20261020T131639452Z-17792.log`. 수정 후 재실행 로그: `Build/verification/verify-20261020T135146635Z-7044.log`. core 로그: `Build/verification/verify-core-20261020T130500124Z-33736.log`.
- `git diff --check` 통과. 기존 사용자 변경의 보호 해시를 다시 확인했고, 명시한 프리셋 및 HLOD 연결 수정 외 원본 변경 파일은 동일하다. 커밋·푸시는 하지 않았다.

범위 한계: 전체 77개 기능군·모듈 관계를 조사했지만 소스 전 줄·모든 설정 조합·바이너리 아트 품질의 전수 검증은 아니다. 일반/특수 설정의 전후 FPS·VRAM 측정은 하지 않았다. 변경의 주된 이득은 관리 코드와 진단 작업 제거이며 정량 FPS 개선을 주장하지 않는다.
