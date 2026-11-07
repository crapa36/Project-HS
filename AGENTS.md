# Project HS

Project-wide contracts only.

## Architecture

`ARCHITECTURE.md` is the canonical module map. Read it when changing module ownership, dependencies, public APIs, build layering, or runtime composition.

Scoped rules:

- `Source/Gameplay/AGENTS.md`
- `Source/RendererD3D12/AGENTS.md`

Production modules communicate through another module's `Public` API. Do not include another module's `Private` implementation or move higher-level types into Core to bypass dependency rules.

`Tests/check_architecture.cmake` mechanically enforces the module graph.

## Cross-cutting invariants

The low-level build must remain valid with:

```text
HS_BUILD_RUNTIME=OFF
HS_BUILD_CONTENT_TOOLS=OFF
```

Do not make that path depend on runtime/content-only prerequisites such as the D3D12 runtime, runtime fonts, DXC, DirectXTex, or the FBX SDK.

Gameplay simulation and the content cooker share the project's 60 Hz tick convention. A deliberate tick-rate change must update both sides and affected tick-based rules and tests.

Authoritative content inputs live under:

```text
ContentSource/
Schemas/
Content/Shaders/
```

Do not edit cooked or generated build output as source.

Keep simulation and presentation cooked data separate:

```text
simulation_rules.hsbin
presentation_catalog.hsbin
```

## Verification

Use the narrowest existing check that answers the risk introduced by the change.

구조 간소화와 최적화를 우선하고, 관련 수정을 모은 뒤 빌드와 필요한 기존 검사를 묶어서 실행한다.
기본 `Build/msvc-debug`·`Build/msvc-release`를 덮어쓰며 사용하고 별도 날짜별·실험별 빌드 트리는 만들지 않는다.
불가피한 임시 빌드는 고정 임시 경로를 재사용하고 사용 후 즉시 삭제한다. 완료된 대용량 중간 생성물도 즉시 정리한다.
변경 위험을 직접 잡지 못하는 중복 테스트·상태 검사·진단 코드는 추가하지 말고 정리한다.
승인된 불변 계약은 생성·입력 경계에서 확정하고 내부 소비 경로에서 반복 검증하지 않는다.
렌더 화질은 같은 조건의 실제 화면과 움직임 안정성을 보고 판단한다.
원본과의 수치 차이는 진단 자료이며, 원본을 자동으로 더 좋은 결과로 간주하지 않는다.

자동 테스트·플레이 테스트·성능 측정은 게임의 모든 출력음을 음소거하여 실행한다.
기존 실행별 음소거를 사용하고 사용자 저장 오디오 설정은 변경하지 않는다.
오디오 처리·재생 경로는 실행하되 master 출력만 0으로 설정한다.

성능 비교의 각 실행 직전에 CPU·GPU 외부 부하를 짧게 확인하고 진행 여부를 판단한다.
외부 게임 등 큰 부하가 있으면 성능 비교를 보류하고, 가능한 구현·기능 검사는 계속한다.
실행 도중 외부 부하가 커진 결과도 오염된 측정으로 구분하여 성능 채택 근거로 사용하지 않는다.
사용자의 다른 프로세스를 임의로 종료하지 않는다.

For localized changes, prefer the relevant build/test target before broader workflows.

Keep only tests that detect a distinct product contract. Extend an existing case
table before adding a new test file or executable; separate runners require a real
isolation or dependency boundary. Do not add source-spelling assertions, disconnected
algorithm copies, or permanent before/after tools for a completed optimization.

Use:

```powershell
cmake --workflow --preset verify-core
```

when low-level integration, architecture, or deterministic simulation requires the repository's low-level gate.

Use:

```powershell
cmake --workflow --preset verify
```

when Runtime, renderer, content cooking, assets, or integration behavior requires the full gate.

Documentation-only changes do not require build/test execution unless they change an executable contract or validation input.

Do not repeat a broader gate when an unchanged valid result still covers the relevant inputs. If an external prerequisite is unavailable, report it rather than weakening the check.

## Development state records

`DEV_STATE.md` is AI implementation continuity only. Record current technical state, contracts, verified checks, unresolved technical issues, and next implementation steps. Do not add user/manual acceptance, playtest, listening, visual inspection, or target-hardware checklists. Replace stale entries; do not accumulate history.

불필요한 생성물들 주기적으로 정리.
