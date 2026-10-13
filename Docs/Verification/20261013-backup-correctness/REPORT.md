# 백업 브랜치 1차 정확성 수정 — 2026-10-13

기준: `codex/engine-v7-backup-20261009` / `a0d98d477099509fe7eb0db379de537d2cbf8ac9`.
깨끗한 Linux 체크아웃에서 작업했다. 사용자 PC의 worktree는 터널 연결 실패로 읽거나 변경하지 않았다.
첨부 종합계획의 첫 구현 묶음을 적용했으며, 전체 계획이나 native 검증 완료 보고서가 아니다.

| 작업 | 구현 | 검증과 남은 게이트 |
|---|---|---|
| B00/G01 | 기존 Linux core 게이트와 MSVC core 대상에 실제 회귀 연결, Linux CI 추가 | 원본 4개 PASS, 후보 통합 16개 PASS. CI 실행 결과는 이 로컬 증거와 별개 |
| C01 | base 픽셀의 정수 축소, 공통 HLSL/C++ 주소 helper, 실제 생성 extent/level/element metadata, NaN·overflow·용량 불확실성에서 visible | NPOT/tiny/DRS/가장자리 및 구체 반례 CPU PASS. production shader 7개 DXIL 컴파일 PASS. GPU readback/장면/GBV 미실행 |
| C02A | admission 전 할당, 예약/완료 guard, 잠금 밖 stop callback, 고정 크기 첫 실패, worker 이름 비할당, ParallelFor overflow 방지 | 원본 engine/reentry timeout 재현. 수정 후 0/1/4 worker, allocation sweep, nested/cancel/기존 jobs PASS |
| C02B | pin 유지, source hash 검증된 Taskflow patch. 미게시 node/topology 및 실패한 queue resize 회수 | 실제 executor callable/name/node/외부 queue 성장 실패 후 Wait/destructor PASS. LSan 환경 제한 아래 참조 |
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

1. 연결된 Windows 환경에서 `verify-core`, `verify-portable`, Runtime/renderer full `verify`와 변경된 UI/diagnostics native 회귀 실행.
2. HZB production helper GPU readback harness와 실제 장면·DRS·cluster 비교. 현재 GPU readback 테스트를 구현/실행했다고 주장하지 않는다.
3. native close/disk failure 및 MSVC allocator 실패 주입. LSan 가능한 환경에서 메모리 회수 재검증.
4. 기존 full-FHD GBV DeviceHung 기준선은 미해결 유지. 성능/FPS·DXIL product Generation·GPU/driver/SDK 실측 없음.
5. 전체계획 C05와 S/P/D 구조·성능 작업은 아직 미착수. 첫 묶음 native 검증 전에 대형 렌더 구조 변경을 섞지 않는다.

Rollback: 이 변경 commit을 revert하면 기준선으로 돌아간다. 원본 기준 commit은 ancestry로 보존한다.

## 첫 원격 CI 후 수정

커밋 `3bd953d`의 [Foundation 실행](https://github.com/crapa36/Project-HS/actions/runs/38357551495): Linux 성공, Windows는 Taskflow patch postimage SHA256 불일치로 configure 실패, portable 단계 미실행.
`core.autocrlf=true`가 `git apply` 결과를 CRLF로 바꾸는 것을 Linux 임시 repo에서도 재현했다.
후속 수정은 patch 명령의 LF 출력을 고정하고, 이미 CRLF로 바뀐 정확한 원본/수정본만 LF로 복구한다.
임의 편집 파일은 여전히 거절한다. 원본 LF·원본 CRLF·수정본 CRLF 복구와 변조 거절을 검증했다.
근거: `ci-initial.json`, `patch-eol-repro.json`, `patch-eol-verification.json`.
후속 Windows CI 결과는 별도 실행을 확인해야 하며, 첫 실패를 PASS로 대체하지 않는다.
