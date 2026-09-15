# Project-HS

C++23 / DirectX 12 기반 아레나형 생존 액션 게임 개인 프로젝트입니다. AI 코딩 에이전트를 개발에 활용하되, 생성된 변경을 그대로 신뢰하지 않고 저장소 규칙·자동 테스트·반복 실행 도구로 구조와 게임 규칙을 검증하는 개발 방식을 실험하고 있습니다.

- 개발 형태: AI 코딩 에이전트 활용 개인 프로젝트
- 기간: 2026.07–진행 중
- 기술: C++23, DirectX 12, HLSL, CMake, CTest, GitHub Actions, flecs, Taskflow
- Portfolio: https://app.notion.com/p/3c1a57a3469281199f70d64ded6e4eaf

## Project Focus

### 1. Architecture Rules as Executable Checks

기능 추가와 리팩터링이 반복되면서 모듈 책임이 섞이는 문제를 프롬프트 규칙만으로 통제하지 않고 저장소와 CI에서 검사하도록 구성했습니다.

- `ARCHITECTURE.md`에 모듈 DAG와 허용/금지 의존성 기록
- 루트와 주요 모듈의 `AGENTS.md`에 변경 범위와 검증 경로 기록
- CMake 검사에서 금지된 Include와 Target Link 관계 검출
- Presentation → Gameplay implementation / Simulation Rules 의존 금지

Representative commit:
- `1a2b1f7` — test: 구조 검증 자동화

### 2. Deterministic Gameplay Validation

컴파일 성공만으로 게임 규칙이 유지됐다고 판단하지 않도록 fixed-step Simulation과 Checksum 기반 검증을 사용합니다.

- Explicit Seed
- Fixed-step Simulation
- Deterministic Iteration / Phase Order
- 동일 입력 반복 실행 Checksum 비교
- Simulation Rules Hash 변화 검증
- GitHub Actions / CTest에서 Determinism Test 실행

### 3. Repeatable Experiment / Failure Capture

동일한 전투 상황을 반복 실행하고 중단 원인을 추적하기 위한 별도 실행 경로를 구성했습니다.

- JSON Experiment Spec
- Timeline Action
- Parent / Child Process 실행
- Named Pipe 기반 Tick 명령 전달
- Heartbeat 기반 정지 감지
- 중단 시 Minidump 및 결과 파일 기록
- Simulation-only 실행 지원

### 4. Simulation / Presentation Data Separation

게임 규칙과 표현 데이터를 별도 Cooked 데이터로 분리하고 Hash / Reload 검사를 수행합니다.

- `simulation_rules.hsbin`
- `presentation_catalog.hsbin`
- Schema Hash / Source Hash
- Cook 직후 Reload Self-check

### 5. DirectX 12 Character Rendering

- GPU Skinning
- Bone Index / Weight 기반 Vertex Skinning
- `Texture2DArray` Diffuse / Normal Material Sampling
- Tangent Space Normal → World Normal 변환
- Skinning 결과를 Shadow Pass에도 적용

## Verification Philosophy

일반 PowerShell에서도 `powershell -NoProfile -File Tools/verify.ps1`로 저수준
검증을 실행할 수 있습니다. Runtime/콘텐츠 변경은 `-Workflow verify`를 사용합니다.
스크립트는 설치된 MSVC 환경을 불러오고 기존 CMake workflow를 실행하며,
전체 로그와 실행 결과 JSON을 `Build/verification`에 보관합니다.
이미 구성된 빌드에서 관련 검사만 실행하려면 예를 들어
`Tools/verify.ps1 -Workflow verify -Target hs_combat_sim -Test '^tools\.combat-suite-input$'`
를 사용합니다. 전체 workflow를 대체하는 완료 판정이 아니라 반복 수정용 명령입니다.

AI가 만든 변경을 그대로 반영하지 않고 다음 경로로 확인합니다.

`Requirements → Implementation → Build → Architecture / Rule Tests → Determinism Tests → Runtime Review`

자동 검사가 통과하더라도 게임플레이 감각과 VFX 품질은 실제 실행 결과를 직접 확인합니다.

## Portfolio

구조와 검증 방식, 반복 실행 도구에 대한 상세 설명은 Notion 포트폴리오에 정리했습니다.

https://app.notion.com/p/3c1a57a3469281199f70d64ded6e4eaf
