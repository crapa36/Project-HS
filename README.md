# Project HS

Windows / C++23 / D3D12 게임 프로젝트입니다. 모듈 경계는 [ARCHITECTURE.md](ARCHITECTURE.md), 작업 규칙은 [AGENTS.md](AGENTS.md)를 따릅니다.

개발 환경은 Visual Studio 2022 C++ x64 도구와 Windows SDK, Ninja, Python 3입니다. 저장소의 `Tools/cmake.ps1`이 해시로 고정한 CMake 4.4.3을 준비합니다. 최초 구성에는 외부 의존성 다운로드를 위한 네트워크가 필요합니다.

엔진의 CPU 기반 검증은 일반 PowerShell에서 실행합니다.

```powershell
./Tools/verify.ps1 -Workflow verify-core
```

반복 작업에서는 필요한 검사만 선택합니다. CTest가 해당 검사의 빌드 의존성과
fixture를 준비하며, 별도 변경 영향 추론이나 검증 영수증은 사용하지 않습니다.

```powershell
./Tools/verify.ps1 -Workflow verify-core -Test simulation.determinism-core
./Tools/verify.ps1 -Workflow verify -Test integration.render-smoke
```

RenderData와 에셋 게시 파이프라인까지 검사하려면 MSVC 개발자 PowerShell에서 다음을 실행합니다. Runtime과 콘텐츠 도구는 끈 상태이며 D3D12 장치, DXC, FBX SDK가 필요하지 않습니다. CI에서도 두 CPU 검증을 실행합니다.

```powershell
./Tools/cmake.ps1 --workflow --preset verify-portable
```

전체 콘텐츠·렌더러 검증에는 D3D12 GPU, DXC와 동반 DLL, Autodesk FBX SDK 2020.3.7 VS2022가 필요합니다. SDK 경로는 `HS_FBX_SDK_ROOT`로 지정할 수 있습니다. 선택 기능의 SDK는 `cmake/RenderSDKs.cmake`와 `Tools/bootstrap_render_sdks.ps1`을 참고하세요.

```powershell
./Tools/verify.ps1 -Workflow verify
```

Release는 MSVC 개발자 PowerShell에서 구성한 뒤 정식 빌드 진입점으로 빌드합니다.

```powershell
./Tools/cmake.ps1 --preset msvc-release
./Tools/hs_dev.ps1 -Command build -BuildRoot ./Build/msvc-release -Target ProjectHS
./Build/msvc-release/ProjectHS.exe
```

개발 빌드의 시각 비교 룸은 다음 명령으로 엽니다. 캐릭터와 각 애니메이션, 프롭·아이템·투사체·VFX를 공간별로 동시에 재생하며 게임 출력음은 실행 중에만 음소거합니다.
기존 게임의 지형지물 배치와 분리된 빈 테스트 맵이며, 조명·하늘·노출·재질과 렌더링 경로는 게임 설정을 재사용합니다. 다른 맵의 정적 GI를 강제로 적용하지 않습니다.

```powershell
./Build/msvc-debug/ProjectHS.exe --test-room --artifacts=Build/msvc-debug/Artifacts/runtime
```

오른쪽 마우스로 일반 게임처럼 이동하고 마우스 휠로 카메라를 조절합니다. Space는 일시정지·재개, R은 카메라 초기화, F12는 전체 화면을 지정한 기본 빌드의 산출물 폴더에 저장하며 Escape는 종료합니다. 이 룸은 Release에서 제외됩니다.

특정 공간의 이미지를 자동 캡처하려면 1부터 시작하는 공간 번호와 재생 시간(초, 0~30)을 지정합니다. 결과 폴더는 기본 빌드 안에서 재사용합니다.
번호·이름·위치는 `test-room-catalog.csv`, 실제 캡처 시간과 콘텐츠 세대는 `test-room-capture.json`에 기록됩니다. 전체 화면 피격·저체력 효과는 해당 전시물 가까이에서 보거나 그 번호를 캡처할 때만 적용해 다른 전시물의 색을 바꾸지 않습니다.

```powershell
./Build/msvc-debug/ProjectHS.exe --test-room-capture=1 --test-room-time=0.5 --artifacts=Build/msvc-debug/Artifacts/runtime
```

표면·HDR 원인 분석이 필요할 때만 기존 `--capture-diagnostics`를 함께 지정합니다. 사용이 끝난 대형 원본 캡처는 즉시 삭제합니다.

콘텐츠 원본은 `ContentSource/`, `Schemas/`, `Content/Shaders/`입니다. `Build/`의 cooked 파일을 직접 수정하지 마세요. 실행 파일은 빌드가 게시한 Selected Generation을 사용합니다. 개발 도구·검증 상세는 [Tools/tooling/WORKFLOW.md](Tools/tooling/WORKFLOW.md)를 참고하세요.

콘텐츠 편집은 원본 JSON에서 하고 해당 제작기 타깃을 빌드합니다. 별도 `hs_content_tools` 실행 파일은 제거했습니다. 전체 콘텐츠는 `hs_content_generation`, 게임 실행 파일은 `ProjectHS` 타깃으로 빌드합니다. 게임 데이터만 검사할 때는 빌드된 `hs_game_data.exe --validate-only --source-root <저장소 절대경로> --build-root <빌드 절대경로>`를 사용합니다.

시뮬레이션 규칙 변경은 콘텐츠를 다시 빌드한 뒤 게임을 재시작해 적용합니다. 실행 중인 Generation의 규칙 파일을 감시·교체하지 않습니다. AI 실험은 검증된 spec의 timeline을 자식 프로세스에서 직접 실행하며, 부모는 watchdog과 결과 수집을 담당합니다.

녹화·smoke·리플레이·명시적 capture가 없는 플레이는 상세 진단 로그를 수집하지 않습니다.
Debug도 자동 녹화하지 않습니다. `--record-playtest`로 녹화를 요청하고 `--no-playtest-recording`으로 끌 수 있습니다.
AI smoke·녹화·리플레이에서는 기존 결과 파일을 만들고, 성능 조사는 `--capture-diagnostics`로 요청합니다.
기능별 유지·삭제 판단과 이번 변경은 [엔진 경량화 조사](Docs/ENGINE_LEAN_AUDIT.md)에 정리했습니다.
