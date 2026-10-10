# Project HS

Windows / C++23 / D3D12 게임 프로젝트입니다. 모듈 경계는 [ARCHITECTURE.md](ARCHITECTURE.md), 작업 규칙은 [AGENTS.md](AGENTS.md)를 따릅니다.

개발 환경은 Visual Studio 2022 C++ x64 도구와 Windows SDK, Ninja, Python 3입니다. 저장소의 `Tools/cmake.ps1`이 해시로 고정한 CMake 4.4.3을 준비합니다. 최초 구성에는 외부 의존성 다운로드를 위한 네트워크가 필요합니다.

엔진의 CPU 기반 검증은 일반 PowerShell에서 실행합니다.

```powershell
./Tools/verify.ps1 -Workflow verify-core
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

콘텐츠 원본은 `ContentSource/`, `Schemas/`, `Content/Shaders/`입니다. `Build/`의 cooked 파일을 직접 수정하지 마세요. 실행 파일은 빌드가 게시한 Selected Generation을 사용합니다. 개발 도구·검증 상세는 [Tools/tooling/WORKFLOW.md](Tools/tooling/WORKFLOW.md)를 참고하세요.
