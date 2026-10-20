# 코드 구조 검토 및 개선 — 2026-10-16

상태: 초기 구조 개선9건과 측정 후 중복 작업 제거3건, 총12건 반영. Debug/Release 빌드, CPU 회귀, 일반 렌더링·RT·가시성·GPU 오라클 검증 통과.

## 범위와 기준

활성 작업 트리 `cpu-gpu-utilization/Project-HS`의 기존 수정본을 기준으로 시작했다. `Build/structure-review/baseline/`과 `inventory.json`에 변경 전 코드 669개 파일(219,017줄)과 해시를 보존했다. 기존 그래픽 최적화·지면 자산·사용자 변경은 유지하며 커밋하지 않는다.

전체 파일 목록과 구조 패턴을 조사하고 모듈 경계, 시작/종료, 입력 상태 전이, 콘텐츠 검증·게시, CPU/GPU 바인딩 계약을 집중 추적했다. 모든 줄을 동일 깊이로 읽었다는 의미는 아니다. 상세 깊이와 제외 영역은 `Build/structure-review/{renderer,runtime,content}/`의 coverage/report 및 `tooling-coverage.json`에 기록한다. Build, 외부 SDK와 third-party 구현은 자체 코드 감사 범위에서 제외했다.

## 발견과 변경

|우선순위|문제와 재현 조건|개선 구조|확인 근거|
|---|---|---|---|
|P1|시작 오류 후 mutex를 보유한 채 Render thread를 join하여 상호 대기|오류 상태 복사 후 잠금을 해제하고 미시작 simulation의 완료 신호를 설정한 뒤 join|동일 오류 주입: 기존 108.08초 미종료 → 수정본 18.34초 후 예상 오류 exit 1, 교착 없음|
|P1|기록의 마지막 flush/close 실패를 성공으로 보고|raw/metadata/report 종료 결과를 검사하고 완료 실패를 실행 결과까지 전달|실패 주입 회귀 검사|
|P2|동일 입력 배치의 Quit 뒤 Resume가 종료 상태를 취소|Paused→Playing 전이만 허용|기존 회귀 실패, 수정 후 통과; 결정론 checksum 유지|
|P2|긴 콘텐츠 의존성의 재귀 호출이 프로세스 스택을 소진|명시적인 작업 스택으로 동일 순서의 의존성 검사|20,000개 순환 입력에서 기존 exit 0xC00000FD, 진단 없음|
|P2|CPU와 HLSL에 재질·가시성·피드백 개수가 수동 복제|공개 카탈로그에서 공통 DXC 정의를 계산하고 컴파일·의존성 검색·Action identity에 동일 적용|남은 DXIL 335개 모두 기존과 동일한 해시; 실제 3종 producer와 DXC 통과|
|P2|실제 draw에서 쓰지 않는 일반 환경 PSO와 shader 제품 3개|생성·읽기·mesh pair·교체/복구와 빌드 항목 제거|재질별 PSO 및 실제 사용 중인 opaque shadow/entry 함수 보존|
|P2|각 geometry page 검사마다 같은 JSON을 재파싱·선형 검색|호출 범위의 ActionResultSnapshot/인덱스 재사용|제품 크기·해시·경로 유지, 동일 크기 페이지 손상 복구를 통합 CTest로 확인|
|P2|검증 직후 동일 Generation 전체를 재해시|분기별 전체 검증을 유지하고 마지막 중복 검사 제거|기존 4,222제품·2,251,078,021바이트의 중복 읽기 1회 제거|
|P2|제작 도구 4개가 옛 절대 checkout 경로를 사용|스크립트 위치 기준 경로와 표준 runpy 실행 사용|현재/이동된 공백·한글 경로 모두 통과; 자산은 재생성하지 않음|

## 조사 근거와 선택

- 잠금 범위를 짧게 유지하고 중복 정의를 제거하는 방향은 [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines)의 CP.43, ES.3과 맞는다. 실제 수정 대상은 프로젝트에서 재현하거나 호출 관계로 확인한 부분으로 한정했다.
- [Microsoft의 PSO 설명](https://learn.microsoft.com/en-us/windows/win32/direct3d12/managing-graphics-pipeline-state-in-direct3d-12)에 따라 PSO 생성은 상태 전처리를 포함한다. 사용하지 않는 PSO를 줄이는 것은 초기화 작업 감소이며, 이 사실만으로 FPS 향상을 주장하지 않는다.
- [ofstream close 계약](https://learn.microsoft.com/en-us/cpp/standard-library/basic-ofstream-class?view=msvc-170#close)에 맞게 최종 종료 후 스트림 상태까지 확인한다.
- [Microsoft 재귀 함수 설명](https://learn.microsoft.com/en-us/cpp/c-language/recursive-functions?view=msvc-170)은 호출 수가 스택 크기에 제한됨을 명시한다. 이 프로젝트에서는 실제 overflow를 확인하고 기존 reader와 같은 반복 구조로 변경했다.
- [nlohmann JSON parse](https://json.nlohmann.me/api/basic_json/parse/)의 입력 파싱 비용을 페이지마다 반복하지 않고 기존 snapshot 인덱스를 재사용한다. 전역 캐시는 추가하지 않는다.
- [Python runpy.run_path](https://docs.python.org/3/library/runpy.html)는 실행할 파일의 __file__을 설정한다. 중첩 exec 대신 사용해 경로 기준을 유지한다.
- [CMake custom command 계약](https://cmake.org/cmake/help/latest/command/add_custom_command.html)에 맞는 기존 DEPENDS/DEPFILE/단일 출력 소유 구조는 유지한다.

## 유지한 구조

Public API 모듈 DAG, 단일 D3D12 backend, 60Hz 및 deterministic phase order, immutable Action/Generation 게시·복구·read lease, 기존 측정의 shared-pass 의미를 유지한다. 근거 없는 엔진 교체, 범용 renderer interface, 새 의존성/프레임워크, 대형 파일의 기계적 분할은 추가하지 않는다.

## 검증

아래는 이번 변경에서 실제 실행한 검사다. 경로는 저장소 기준이다.

|검사|결과|근거|
|---|---|---|
|OFF/OFF verify-core|19/19 통과|`Build/verification/verify-core-20261015T154349816Z-29012.json`|
|Runtime integration/diagnostics|2/2 통과; 기록 실패 주입·상태 전이 회귀 포함|`Build/verification/verify-20261015T154350777Z-3432.json`|
|콘텐츠 CPU fixture|7그룹 통과; 20k 그래프·정확한 upstream·복구·게시·GC·reader|`Build/structure-review/content/cpu-checks.json`|
|렌더러/계약 CPU CTest|6/6 통과; package format·instance order·probe draw·shader contracts·OMM·architecture|`Build/structure-review/render-cpu-tests.xml`|
|Material parameter action 통합|1/1 통과, 17.00초; 실제 shader/package producer와 페이지 손상 복구|`Build/structure-review/material-action-tests-fixed.json`|
|전체 runtime-full 콘텐츠 제작|통과; 4,219제품, DXIL 338→335, 남은 335개 해시 동일|`Build/structure-review/validation-build.log`, `shader-product-comparison.json`|
|Debug/Release 실행 파일|모두 링크 성공; 새 Generation export/verify 성공|`Build/structure-review/msvc-{debug,release}.log`|
|시작 실패 회귀|예상 IO 오류 exit 1, 18.34초, 교착 없음|`Build/structure-review/startup-failure-{before,after}/run.json`|
|일반 렌더링 Debug smoke|16프레임, exit 0, assertions/execution valid, D3D12 오류 0|`Build/structure-review/native/{result,render,run}.json`|
|최고 품질 RT Debug smoke|16프레임, exit 0, assertions/execution valid, D3D12 오류 0; RT AO/GI/반사/그림자 활성, fallback 없음|`Build/structure-review/native-rt/{result,render,run}.json`, `renderer_features.jsonl`|
|가시성/persistent-scene Debug runner|16프레임, execution valid, D3D12 오류 0; HZB 재검사·장면 변경 회귀 통과|`Build/structure-review/native-visibility/{result,run}.json`, `d3d12_validation.log`|
|네이티브 multi-queue GPU 오라클|1/1 통과, 8.47초; 실제 production opaque coverage 43 GPU cases 포함|`Build/structure-review/multi-queue-tests.xml`|

두 게임 smoke의 결정론 checksum은 모두 `4930774313638502297`로 기존과 같다. 각 smoke는 1280×720의 짧은 정확성 검사이며 플레이 FPS 벤치마크가 아니다. 새 Generation은 `8c5527e9b37e6eb4d6e3c71dd04110e02437022569bd7cc00f08d44c4ec193d8`이다.

추가 회귀 테스트의 첫 통합 실행에서 helper에 package 대신 결과 묶음을 전달한 오류를 발견해 수정했고, 이후 실제 통합 CTest를 통과했다. Release 검증 스크립트의 단일 문자열 splat도 배열로 수정한 뒤 실제 Release 링크를 완료했다. 실패 기록은 삭제하지 않았다.

MSVC 경고는 남아 있다: 기존 지역 이름 가림과 새 공통 정의의 카탈로그 `.size()`→uint32_t 변환 경고다. 현재 고정 카탈로그 값은 16/7/21이며 동일 DXIL 검증을 통과했다. 경고를 숨기거나 검증 조건을 완화하지 않았다.

전체 `verify` gate를 모두 다시 실행한 것은 아니다. 이번 변경 위험에 맞는 위 검사와 기존 유효한 체크를 사용했다. GPU-based validation(GBV)·장시간 플레이·shader hot reload·Blender bake·Linux CI는 이번에 실행하지 않았다.

## 남겨 둔 후보

재질 제품 경로를 정하는 legacy/terrain/GroundDetail/hero 분기가 renderer의 세 위치에 반복된다. 현재 결과는 모두 일치한다. 공개 RenderData helper로 합치는 것은 다음 카탈로그 변경과 함께 고려할 수 있지만, 이번에는 새 API 범위를 늘리지 않았다. 대형 파일의 분리만으로 성능이나 안정성이 좋아진다고 판단하지 않았다.

## 한계

전역 구조 조사와 핵심 경로의 정밀 검토이며 모든 코드의 무결성을 증명하지 않는다. 외부 SDK 소스, 모든 셰이더 수학·전투 밸런스 공식·오디오 DSP·저장 데이터 migration 분기의 전수 의미 검증은 포함하지 않았다. 새 FPS/144 FPS 달성 측정과 Blender 재베이크는 이번 구조 변경 검증에 포함하지 않는다.


## 측정에 따른 후속 구조 개선

동일 입력 초기 정지 전투 replay에서 동적 BLAS 중복 해시 제거 후 기본 FPS8.378→8.627, SDK reported-present16.997→17.551을 관측했다. 입력/56개 설정/Generation/체크섬 일치를 검증했지만 한 쌍의 비교이므로 전체 차이를 한 줄 변경의 단독 효과로 단정하지 않는다. 자세한 원본과 순위는 `Docs/Verification/20261016-foreground-ultra/REPORT.md`에 있다. 144 FPS는 미달성이다.

|문제|변경|보존한 계약|
|---|---|---|
|고유 slot/key의 동적 BLAS까지 매번 used hash 삽입|동적 항목은 순서대로 직접 보존, 공유 static만 중복 검사|stable slot 검증, static compaction, GPU lease/retirement|
|이미 전체 초기화된 imported resource의 범위를 매 프레임 다시 할당·합집합 계산|초기화가 필요한 resource에만 coverage 계산|최신 버전, 부분 범위, RAW/WAR/WAW, undefined read 검사|
|RT 불투명 후보에서 정확한 위치 캐시를 읽고도 공통 정점 처리를 수행|cached coverage 위치 직접 Load3, uncached는 기존 평가|identity/front/index/degenerate/alpha 및 committed shading|

프레임 그래프의 GetDesc는 이미 논리 import당 한 번이고 중복 physical import는 거부한다. 여기에 추가 캐시를 넣지 않았다. 큐 batch 병합은 외부 SDK/동시 reader/자원 수명 계약에 걸쳐 있으므로 이번 최소 변경에 섞지 않았다.

[Microsoft DXR 명세](https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html#opacity-micromaps-and-tracerayinline)에서 OMM known-transparent 제거는 FORCE_NON_OPAQUE에서도 유지되지만 known-opaque 자동 commit은 제한된다. 현재 소프트웨어의 앞뒷면/identity/퇴화 삼각형 검사와 같다는 증거 없이 flag를 제거하지 않았다. 최적화 전 production GI DXIL에도 cached 위치 읽기 이후 canonical vertex 읽기가 남아 있음을 `Build/performance-actual/rt-cached-opaque-position/before-evidence.json`에 보존했다.

후속 CPU graph 회귀 1/1, shader 계약31/31, WARP multi-queue oracle1/1(실제 coverage helper49 GPU 사례,12.77초) 통과. Debug/Release 실행 파일 빌드 통과. 새 shader 콘텐츠 및 RTX 네이티브 검증도 완료했다. 이전 FPS는 이 마지막 두 변경 이전 바이너리/콘텐츠의 수치다.


최종 검증: 새 Generation `b338b11995a7c973dcea7ab4f1817da47595303f71b7e0bd2a6f9e52a47c71af`를 Debug/Release 양쪽에 export/verify했다(4,219제품). RTX ray-entity-ultra 8프레임/reset6/GBV는 exit0, execution_valid=true, validation_errors=0으로 통과했다. 근거와 최종 바이너리/소스 해시는 `Build/structure-review/followup/integration.json` 및 `native/`에 있다.

새 production GI DXIL의 첫 radiance query cached opaque 경로에서 canonical vertex raw-load12회가0회로 줄고 캐시 위치 Load3 3회만 남는다. uncached/committed 경로는 유지한다. `Build/performance-actual/rt-cached-opaque-position/after-evidence.json`이 분기/제품 해시 근거다. 드라이버 기계어 비용이나 전체 프레임 시간 개선율을 이 수치로 추정하지 않는다.

후속 작업에서는 전체 gate/장시간 플레이/새 FPS 측정을 반복하지 않았다. 앞의 초기9건 검증과 이번 좁은 CPU·shader·WARP·RTX 검증을 결합한 결과이며 전수 의미 검증은 아니다. 게임 검증 프로세스는 정상 종료했고 Computer Use는 다시 활성화하지 않았다.
