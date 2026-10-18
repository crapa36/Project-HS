# 그림자·GBuffer 최적화 및 최고 옵션 단일 캡처

작업 브랜치: `codex/cpu-gpu-utilization`, 기준 `bc7bd6024fe5a84456e785c81731a127ec339b6b`.
작업 경로: `C:/Users/crapa/.codex/worktrees/cpu-gpu-utilization/Project-HS`.
원래 main의 변경 파일은 건드리지 않았으며, 이 변경은 커밋하지 않았다.

## 확인된 개선

동일한 60초 입력, FHD 100%, 무제한 FPS에서 기본 설정 플레이는 **55.665 → 58.265 FPS (+4.67%)**였다.
p95 프레임 간격은 **21.437 → 20.301ms**였다. 두 실행은 tick3511과 checksum14939530550461817357이 일치한다.
전체 FPS는 QPC 간격으로 계산했으며, GPU 패스 시간의 역수를 FPS로 사용하지 않았다.
프로세스 CPU는 각각1.777/1.770코어, PID의 가장 바쁜 GPU 엔진은59.86/58.36%였다.
이 비교는 같은 background 창 상태이며 다른 앱의 GPU 부하가 존재한다. 반복 통계나 일반 벤치마크를 주장하지 않는다.

품질을 유지하며 적용한 GPU 변경:

- CSM·cluster·mesh 그림자에서 이전 Grass/Skin 변형 계산을 제거했다. 메인 Surface의 모션/히스토리는 유지한다.
- Grass의 영향이 확실히0인 먼 bender를 보수적으로 건너뛴다.
- VSM 피드백의 중복 요청을 group 공유 비트셋으로 합친다. 부분 그룹 barrier, overflow 요청과 기존 비트는 보존한다.
- 이미 픽셀 셰이더가 버리던 Hero 후면은 GBuffer/그림자 PSO에서 미리 컬링한다.

## 최고 옵션 한 회에서 발견한 실제 병목

56개 설정을 기록했고 품질 등급14개는 전부 Ultra였다. RTAO·RTGI·Hybrid RT 반사/그림자가 실제 활성화됐다.
FHD 100%, 거리500m, AF16, 무제한 FPS, 자동 메모리 예산과 중립 미술 파라미터를 사용했다.
미지원 외부 SDK 모드를0ms로 취급하지 않았다. 모든 값·상태·이유는 [전체 옵션 평가](OPTION_COSTS.md)에 있다.

두 초기 실행은 공유 STBN 및 OMM 배열의 중복 선언으로 유효 GPU 측정을 남기지 못했다.
검증을 끄지 않고 두 문제를 수정한 뒤 **정상 종료한 최고 옵션 캡처는 한 회**다.
그 후 최고 옵션 성능 캡처는 반복하지 않았다. 10초 prefix 재현과 native fixture는 정확성 검사다.

정상 캡처의 CPU 렌더 프레임은8개에 그쳤다. Warm 구간 QPC FPS는0.07252였다.
CPU 렌더 평균13.501초 중 graph prepare9.469초와 build3.034초가 **92.61%**를 차지했다.
프로세스는0.988코어(20 logical CPU의4.94%), 가장 바쁜 스레드는0.935코어였다.
PID GPU 엔진은2.70%였다. 이는 직렬 CPU 작업이 GPU 제출을 막는 상황과 일치한다.

이 근거로 추가 수정했다:

- Canonical COM/import identity 중복 검사를 해시 집합으로 바꿨다.
- 패스별 리소스 중복 검사를 stamp로 바꾸고 첫 중복의 진단을 유지했다.
- 버전 표는 실제 사용한 ordinal까지만 할당·순회한다.
- BLAS 작업마다 전체 작업 목록을 재검색하던 갱신을 한 번의 순회로 바꿨다.
- 공유 OMM·STBN 읽기는 하나의 정확한 버전으로 선언하며 충돌은 계속 거부한다.

실제 컴파일러의 CPU 검사(50,000개 리소스,128패스,각3회 중앙값)는
**1866.233 → 14.396ms (129.6배, 99.23% 감소)**였다.
이것은 그래프 컴파일 구성요소 수치이며 최종 FPS나 GPU 사용률 개선폭이 아니다.

## 관측된 GPU 비용: 무거운 순서

완료 GPU frame4/5의 **2개 표본**만 사용했다. CPU frame4/5의 설정 revision과 연결했다.
단위는 기능 그룹 안의 측정된 Direct/Compute 패스 작업 ms이며, 프레임 경과시간과 다르다.
비동기 겹침·공통 기능 때문에 표를 합하거나 동일 비용을 여러 UI 옵션에 더하면 안 된다.
Copy 비용은 미측정 N/A로 분리했다. 전체28개 그룹과56개 옵션 연결은 [전체 평가](OPTION_COSTS.md)에 있다.

| 순위 | 기능 | 평균 ms |
|---:|---|---:|
|1|RT 장면·BLAS/TLAS·변형(공통)|139.389|
|2|RT 반사|67.845|
|3|GI/반사 공유 프로브 갱신|59.416|
|4|RT GI|55.315|
|5|그림자|39.192|
|6|RTAO|13.883|
|7|GBuffer·Surface|9.657|
|8|볼류메트릭|9.111|
|9|가시성·Geometry·Skin|8.225|
|10|피사계 심도|4.998|

프로브는2개 중 한 프레임에서만118.831ms 갱신이 발생했으므로 상시 비용으로 해석하지 않는다.
GPU critical path는320.850/622.564ms였지만 CPU 공백 중 장치 클록도 크게 변했다.
최고 옵션의 정상 지속 FPS, 일반적인 절대 성능, 개별 옵션을 끄면 얻을 개선폭은 이 표본으로 검증할 수 없다.

## 기대 연산량과 판단

Ultra RT GI/반사는 픽셀당2개 radiance ray와 hit당4개 direct-light sample을 사용한다.
`rt_diffuse_bounces=4`는 현재 구현에서 직접광 sample 예산이며4회 bounce loop라는 뜻이 아니다.
RTAO는8ray/pixel이다. 많은 변형 BLAS·긴 추적 거리·다중 ray가 상위 비용을 차지하는 구조는 설명된다.
DOF·볼류메트릭·POM도 최대 품질의 연산 예산을 적용한다. 작은 옵션이나 미지원 기능에 큰 전용 비용을 잘못 배정하지 않았다.

정상적으로 기대할 수 없는 낭비로 확인된 것은 리소스 수의 제곱에 비례한 CPU 처리였다. 이를 제거했다.
GPU 절대 성능의 합격을 선언하거나 화질을 낮춰 숫자를 맞추지는 않았다.
잎/잔디 재질 컨텍스트 재사용 후보는 실제10개 DXIL 제품 모두 샘플 수가2개로 유지되어 제거했다.
효과가 확인되지 않은 코드나 해당 후보의 FPS 향상 주장은 남기지 않았다.

## 검증과 산출물

- 기본 GPU 변경: native14/14, RTX4080 Legacy/Enhanced GBV, owned-view0오류.
- 공유 STBN·OMM 및 선형 그래프: 오류 우선순위·충돌·ordinal holes·초기화·final-version 회귀를 포함한다.
- 최종 CPU 수정: native8/8 및 Runtime OFF / Content Tools OFF CPU CTest2/2 통과.
- 실제 populated EntitySurface와 Ultra RT4모드의 현재 완료 GPU 패스를 강제하는 `rendering.ray-entity-ultra`를 추가했다.
- 후보 제거 후 셰이더 계약·렌더 스모크·최고 RT 캐릭터 검사3/3 통과.
- 최종 콘텐츠 세대는 측정 전 `f240b960...`와 정확히 일치한다. 정상 체력 Release를 재빌드했고3프레임 스모크도 정상 종료했다.

계측은 `--capture-diagnostics`에서 `performance.csv`, 이름이 있는 `gpu_live.csv`,
56개 요청/실효값의 `graphics_settings.csv`를 남긴다.
재사용 분석기: `tools/analyze-graphics-options.py --self-test` 및 `<capture> --output <report.json>`.
설정은 CPU frame 소유 revision으로 결합하며 observed frame으로 혼합하지 않는다.

원자료: `Build/performance-actual/shadow-before`, `shadow-after`, `options-highest-capture`.
CPU 검사: `graph-compile-before.csv`, `graph-compile-after.csv`.
전체 옵션 결과: [OPTION_COSTS.md](OPTION_COSTS.md), [option-costs.json](option-costs.json).

최고 옵션 수정 후 FPS/점유율은 요청한 단일 측정 제한 때문에 재측정하지 않았다.

최종 검증 기록:

- `Build/verification/verify-20261015T024850002Z-34440.json` — native8/8.
- `Build/verification/verify-20261015T025336095Z-29616.json` — 최종 source3/3.
- `Build/performance-actual/core-low-tests.log` — low configuration2/2.
- `Build/performance-actual/temporary-invulnerability.json` — 소스 복원·최종 Release SHA256.

이전 full native93개 검증은 이전 상태의 기록이다. 변경 위험을 답하는 최신 좁은 검사를 위와 같이 수행했다.

최종 정상 Release SHA256: `7CB94DCF14F023A2431E66ECEF409FF9418A367F9AB68A58262D7F4E745A0864`.
실행 파일: `Build/msvc-release/ProjectHS.exe`; 임시 무적 없음.
Release 실행 증거: `Build/performance-actual/final-release-smoke/result.json`, execution_valid=true, rendered_frames=3.
