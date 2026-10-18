# RT 잔재 정밀 재검토

## 범위와 추가 제거

현재 작업트리의 소스·셰이더·스키마·제작 데이터·빌드·분석 도구·현재 설계 문서를 검사했다. 삭제된 파일명만 찾지 않고 API 호출, producer/consumer, CPU/HLSL stride, descriptor 범위, 직렬화 버전, 설정 저장 및 SDK 배포 연결을 대조했다. 소스/테스트 검색665파일, 분류된 검색 결과26건, 미분류 및 하드웨어 RT API 결과0건. 파일 해시와 검색식은 `Build/rt-removal/remnants/source-audit.json`에 있다.

- 데칼의 `DecalRayPolicy`, `ray_policy`, 제작 JSON/schema의 `rt_policy`를 제거했다. 메타데이터는 v2/28B로 재정의하며 v1은 명시적으로 거부한다. 새 Generation 재쿠킹·배포가 필요하다.
- RT에서만 쓰던 월드 좌표 복제 출력을 제거했다. 소프트웨어 GI용 변형 정점 저장은24→12B, 기하 레코드는80→56B다. C++14개 uint32와 HLSL3개 uint4+uint2, allocator/Store3/Brixelizer offset·stride가 일치한다. 리소스 소유권과 완료 대기는 유지했다.
- 사용되지 않는 transport/denoising feature·signal enum8개, 중복 canonical_geometry_enabled와 사용되지 않는 출력 필드·SRV 분기를 제거했다.
- 현재52옵션 분석을 과거56옵션 분석과 분리했다. 현재 기록에는 제거된 RT 패스·CPU 하위 시간·AS 메모리 행을 표시하지 않는다.
- 현재 렌더러 계획에서 RT 요구·옵션·SDK·기능 표를 제거했고 사용하지 않는 NRD 배포 고지를 정리했다.
- RT 전용 DXC69 SDK/아카이브, 잔존 RR 헤더·문서·DLL, 오래된 RT 오브젝트·생성기 등90개 항목을 경계·reparse point 검사 후 삭제했다. 기록: `Build/rt-removal/remnants/removed-build-sdk-files.json`.
- 활성 Debug/Release/validation 캐시에서 폐기된 NRD/DXC69 키를 제거하고 재구성했다. 활성 Ninja 빌드 그래프에 RT 생산자/라이브러리/배포 연결이 없다.

## 남긴 문자열의 이유

이전 사용자 설정의 RT 토큰을 비RT 설정으로 옮기고 폐기 키의 재저장을 막는 읽기 호환 처리, 과거 측정 기록의 분석 및 회귀 검사만 프로젝트 소스에 남겼다. 이들은 현재 옵션이나 실행 기능을 등록하지 않는다.

원본 측정 기록·검증 이력·이전 불변 Generation·복구 백업은 증거로 보존했다. 과거 실행계획은 이력임을 표시했다. 공용 meshoptimizer/플랫폼 SDK의 원본 선언·참고 문헌과 해시 검증용 원본 Streamline 아카이브는 공급자 자료다. meshoptimizer OMM 구현은 빌드하지 않으며 Streamline은 잠근 SR/FG/Reflex 구성만 설치·배포한다. SSR, POM, 소프트웨어 SDF GI 등의 실제 사용하는 ray-marching 계산은 유지했다.

## 검증

- Debug/Release/validation 컴파일 성공. 두 분석기 자체 검사 통과.
- 집중 회귀6/6 통과(3.85s): feature-signals, shader-reconstruction-contracts, graph-range-validation, render-package-format, multi-queue-graph, graphics-settings-store.
- 새 기하 ABI 정적 계약, 데칼 v2 roundtrip/구버전·reserved-field 오염·삭제된 제작 옵션 거부 검사 포함.
- 기존 native render smoke180프레임/리사이즈/종료1/1 통과(22.52s). `Build/rt-removal/remnants/native-smoke.xml`.
- 최신 runtime-full Generation `8c5ff7f80b358bd9227d03539409b18051dfe493f23f208b49b2530a9c27d9d1`,2712제품. Debug/Release export 및 verify 성공.
- 세 빌드의 선택된 두 데칼 바이너리를 실제 읽어 chunk43/version2/28B와 manifest SHA256을 확인했다. 선택된 manifest의 RT 전용 제품0개, 폐기된 CMake 캐시 키0개다.
- 빌드 후 검색 대상665파일 해시 불변을 확인했다. 근거와 실행 파일 SHA256은 `Build/rt-removal/remnants/integration.json`.

OFF/OFF 저수준 gate와 장시간/GBV 검사는 반복하지 않았다. 이번 변경은 저수준 실행 논리나 queue 동기화 계약을 바꾸지 않는다.

현재 구성은 Brixelizer OFF다. 정적 ABI 대조와 shader compile이 소프트웨어 GI SDK 활성 GPU 실행까지 입증하지는 않는다. 이번 잔재 정리에서는 FPS 측정을 반복하지 않는다.
