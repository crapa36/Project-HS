# 백업 브랜치 1차 정확성 수정 — 2026-10-13

기준: `codex/engine-v7-backup-20261009` / `a0d98d477099509fe7eb0db379de537d2cbf8ac9`.
깨끗한 Linux 체크아웃에서 작업했다. 사용자 PC의 worktree는 터널 연결 실패로 읽거나 변경하지 않았다.
첨부 종합계획의 첫 구현 묶음을 적용했으며, 전체 계획이나 native 검증 완료 보고서가 아니다.

| 작업 | 구현 | 검증과 남은 게이트 |
|---|---|---|
| B00/G01 | 기존 Linux core 게이트와 MSVC core 대상에 실제 회귀 연결, Linux CI 추가 | 원본 4개 PASS, Linux CI 16개 PASS, MSVC Debug core 17개·portable 31개 PASS. Runtime compile/WARP 결과는 아래 별도 기록 |
| C01 | base 픽셀의 정수 축소, 공통 HLSL/C++ 주소 helper, 실제 생성 extent/level/element metadata, NaN·overflow·용량 불확실성에서 visible | NPOT/tiny/DRS/가장자리 및 구체 반례 CPU PASS. production shader 7개 DXIL 컴파일 PASS. 컴파일된 helper readback harness 구현, WARP CI PASS (Debug layer·GBV ON). 실제 GPU 장면·DRS·cluster 검증 미실행 |
| C02A | admission 전 할당, 예약/완료 guard, 잠금 밖 stop callback, 고정 크기 첫 실패, worker 이름 비할당, ParallelFor overflow 방지 | 원본 engine/reentry timeout 재현. 수정 후 GCC/MSVC Debug 0/1/4 worker, allocation sweep, nested/cancel/기존 jobs PASS |
| C02B | pin 유지, source hash 검증된 Taskflow patch. 미게시 node/topology 및 실패한 queue resize 회수 | GCC/MSVC Debug에서 실제 executor callable/이름 유무/node/외부 queue 성장 실패 후 Wait/destructor PASS. LSan 환경 제한 아래 참조 |
| C03 | 실제 enqueue 승인 후 관련 UI commit/cue, 거절 시 원상복구·invalid cue·재입력 안내, 자동 재전송 없음 | 64개 포화 후 65번째 거절, 생산 ResolveUiInteraction의 설정 뒤로가기 rollback, 순수 탐색/새 입력/1회 소비 PASS. Windows UI 통합 미실행 |
| C04 | 로그당 4096행·2MiB 문자열 capacity 상한, 64KiB 초과 행 유실 표시, 64KiB chunk 동기 capture, flush/close/status 실패 보고 | 10만 건 retention 상한·원래 header·전체 capture·writer/flush 오류 PASS. 실제 Windows disk/종료 통합 미실행 |

## 변경 계약

- HZB min reduction, clear=0, 비교 bias=.0005와 클러스터의 기존 padding/깊이 정책을 유지했다. 루트 시그니처는 이미 64 DWORD이므로 늘리지 않았다. 기존 32-byte range의 마지막에 같은 크기의 생성-layout metadata 레코드를 붙인다. 동적 HZB 비활성 정책 유지.
- Taskflow 수정 범위는 Jobs가 사용하는 executor `silent_async` 입장과 single-node scheduling/queue resize다. 다른 async API 전체의 예외 안전성을 승인한 것이 아니다. configure가 정확한 원본/수정 hash만 수용한다. source override도 같은 검사를 받는다.
- 작업 내부 실패 진단은 511자까지이며 첫 실패를 유지한다. Submit 준비 할당은 admission 전에 throw할 수 있다. Wait의 Result 문자열 할당은 작업 종료 뒤 throw할 수 있다. 현재 작업/동기 stop callback에서 자기 그룹 Wait는 미지원이다.
- `--capture-diagnostics`는 전체 CSV/NDJSON 기록을 켠다. smoke·record/replay는 기존 분석기 계약 때문에 전체 기록을 자동 유지한다. 일반 실행은 최근 기록 모드다. `diagnostics.json`에 mode/complete/io_failed/records/dropped를 기록한다. 전체 capture의 느린 디스크는 render thread에 동기 backpressure를 준다. 새 writer thread/무제한 queue는 없다.

## 실행 증거

`manifest.json`은 수정 소스와 생성 DXIL/실행파일 SHA256을 기록한다.

```sh
cmake --preset linux-core
cmake --build --preset linux-core --target hs_tests hs_simulation_tests
ctest --test-dir Build/linux-core -R '^(foundation.core|foundation.task-system|architecture.layers|simulation.determinism-core)$' --output-on-failure
cmake --workflow --preset verify-core-linux
python3 Tests/check_shader_reconstruction.py
```

마지막 source-string 보조 검사 14개 PASS는 실제 GPU 검증을 대신하지 않는다.
통합16개 이후 HZB 구체 oracle와 실제 UI resolver 회귀를 강화해 해당 두 테스트를 다시 통과했다.
DXC 1.9.2609.5 Linux release를 별도 도구 경로에 두고 SHA256 검증 후 `-WX -O3 -Ges -T cs_6_0`로 7 entry를 컴파일했다. Windows SDK/DXC pin이나 제품 Generation은 변경하지 않았다.

원본 timeout은 `Tests/task_system_failure_tests.cpp`를 원본 Jobs library와 고정 원본 Taskflow에 연결한 5초 watchdog subprocess에서 관측했다. 독립 상태 모델을 원본 엔진 결과로 바꿔 부른 것이 아니다.

추가 ASan/UBSan 실행은 LeakSanitizer가 `/proc/.../task`를 읽지 못해 모두 exit1이다. `jobs-sanitized.json`에 정확한 오류를 보존했다. sanitizer PASS나 leak-free로 인정하지 않으며, 검사를 꺼서 PASS로 바꾸지 않았다.

## 남은 작업

1. Windows core·portable은 CI에서 통과. Runtime/renderer full `verify`와 변경된 UI/diagnostics 실제 실행 회귀는 남아 있다.
2. HZB production helper readback harness는 구현·DXIL 컴파일 완료, WARP CI PASS (Debug layer·GBV ON). 실제 하드웨어 장면·DRS·cluster 비교는 미실행.
3. MSVC allocator 실패 주입은 통과. native close/disk failure 통합과 LSan 가능한 환경의 메모리 회수 재검증은 남아 있다.
4. 기존 full-FHD GBV DeviceHung 기준선은 미해결 유지. 성능/FPS·DXIL product Generation·GPU/driver/SDK 실측 없음.
5. 전체계획 C05와 S/P/D 구조·성능 작업은 아직 미착수. 첫 묶음 native 검증 전에 대형 렌더 구조 변경을 섞지 않는다.

Rollback: 기준선 이후 이번 작업 commit들을 역순으로 revert하는 방식으로 되돌릴 수 있다. 원본 `a0d98d4`는 ancestry로 보존한다.

## 첫 원격 CI 후 수정

커밋 `3bd953d`의 [Foundation 실행](https://github.com/crapa36/Project-HS/actions/runs/38357551495): Linux 성공, Windows는 Taskflow patch postimage SHA256 불일치로 configure 실패, portable 단계 미실행.
`core.autocrlf=true`가 `git apply` 결과를 CRLF로 바꾸는 것을 Linux 임시 repo에서도 재현했다.
후속 수정은 patch 명령의 LF 출력을 고정하고, 이미 CRLF로 바뀐 정확한 원본/수정본만 LF로 복구한다.
임의 편집 파일은 여전히 거절한다. 원본 LF·원본 CRLF·수정본 CRLF 복구와 변조 거절을 검증했다.
근거: `ci-initial.json`, `patch-eol-repro.json`, `patch-eol-verification.json`.
후속 Windows CI 결과는 아래 별도로 기록한다. 이 첫 실패를 PASS로 대체하지 않는다.

후속 `e099ef9` CI(run `38358018633`)는 Linux 성공, Windows configure/build 성공과 15/17 테스트 성공. Jobs allocation sweep 및 실제 backend 실패 주입 두 테스트는 30초 timeout이다. 당시 portable 단계는 미실행이었다. Windows 결함을 다시 열어 stderr 단계 추적과 종료 스택으로 조사했고, 다음 절의 수정과 MSVC 재검증으로 해결했다. Linux PASS로 Windows 결과를 대체하지 않았다.

## Windows 예외 경로와 생산 코드 컴파일

`8cca50e`에서 종료 스택을 확보했다. MSVC Debug의 문자열 iterator-proxy 할당이
`noexcept` 문자열 move/default 생성자에 들어가 `terminate`를 호출했고, 기존에는
CRT 대화상자가 CI에서 기다리며 timeout으로 보였다. `7026854`는 엔진 이름을
`string_view`에서 직접 생성하고 Taskflow 빈 이름을 예외 전파 가능한 포인터 생성자로
초기화한다. 이름 유무 모두 allocation sweep하며 큐 성장 주입 위치는 실제 node 할당
횟수로 측정한다. 실패 시 worker 해제를 보장하고, fatal handler는 실패 종료한다.
실패 주입·assertion·제한 시간·Debug iterator 검사를 끄거나 완화하지 않았다.

`5507e90`의 Windows core17 및 portable31은 성공했다. 추가 생산 코드 컴파일은
`hs_renderer_d3d12.lib`까지 성공한 뒤 UI 상태의 `SimulationPorts` 참조 누락을 발견했다.
`8ace5cd`에서 기존 atomic을 읽기 전용 포트에 연결했다. 후속 `1ac2d71`에서 `hs_runtime.lib`와 `hs_renderer_d3d12.lib` 모두 빌드에 성공했다.
근거: `ci-windows-noexcept.json`, `patch-three-file-verification.json`.

새 HZB probe는 실제 `hzb_region.hlsli`를 DXC로 컴파일해 CPU oracle가 검사한 모든
입력과 readback을 비교한다. 17×17/1919×1079/1920×1080, tiny/odd/DRS, 부족한
capacity/level, overflow, sphere 반례가 같은 실행에 포함된다. 두 CTest는 분리했다:
`rendering.hzb-region-gpu`는 하드웨어를 요구하며 소프트웨어로 자동 대체하지 않는다.
`rendering.hzb-region-warp`만 명시적으로 WARP를 선택한다. 둘 다 Debug layer 및
GBV를 요구하고 오류·corruption을 검사한다. WARP 통과를 실제 GPU 장면 통과로
해석하지 않는다. Linux DXC probe 컴파일 및 변경 CPU 회귀는 통과했다.

## 최종 CI 결과

코드 기준 `1ac2d71abdd4539aafcad4666063a711c025d483`,
[Foundation 실행 38362247340](https://github.com/crapa36/Project-HS/actions/runs/38362247340)의
세 작업 모두 성공했다. Linux core16, Windows core17 및 portable31, HZB WARP1을
통과했다. WARP CTest 시간은 3.84초이며 성능 benchmark 수치가 아니다.
MSVC 19.44.35229.0으로 validation-enabled RelWithDebInfo의 생산 Runtime/renderer
라이브러리를 컴파일했다. FBX content tools를 끈 별도 compile gate이므로 전체
`verify`·content generation·게임 실행 완료로 해석하지 않는다.
정확한 job/step 결과와 발췌 로그는 `ci-final.json`에 저장했다.

소스는 백업 브랜치에 저장했다. 뒤따르는 기록 전용 commit은 검증 코드와 동일한
source hash를 유지하며, 위 CI 결과를 근거로 삼는다. 사용자 PC 터널 미접속으로
실제 하드웨어 및 full native 검증은 실행하지 못했다.
