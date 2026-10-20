# 실제 Debug 플레이 분석 및 후속 최적화

측정 원본: `Build/play-captures/20261015T0435-human-debug`.
메뉴 63분과 실제 플레이를 분리했다. 아래 수치는 foreground Playing 243프레임, 30.054초만 대상으로 한다.
원본 CPU/GPU 스트림 34,258프레임과 GPU 패스 2,877,947행은 전부 저장되었으며, 외부 사용률 카운터 일부만 누락되었다.

## 실제 플레이 성능

| 지표 | 결과 |
|---|---:|
| 평균 FPS | 8.052 |
| 평균 프레임 간격 | 124.191 ms |
| 최대 프레임 간격 | 1,085.342 ms |
| 프로세스 CPU 사용량 | 1.725 코어 |
| 전체 20 논리 CPU 대비 사용률 | 8.627% |
| 프로세스 GPU busiest-engine 사용률 | 21.165% |
| 프로세스 GPU 카운터 유효 구간 | 23.604초 / 30.054초 (78.54%) |
| NVIDIA 장치 전체 GPU 사용률 | 26.867% |
| CPU 렌더 구간 평균 | 122.760 ms |
| GPU critical path 평균 | 32.268 ms |
| GPU VRAM 평균 / 최대 | 3,950 / 4,564 MiB |

CPU 사용률은 27개 표본/28.924초, NVIDIA 장치 사용률은 30개 표본이다.
PID GPU 사용률은 엔진별 최대값이며, NVIDIA 장치 전체 사용률과 측정 범위가 다르다.
누락 구간은 0%로 채우지 않았다. GPU critical path는 점유율이나 FPS 환산값이 아니다.

현재 설정은 custom, 주로 Medium, 1280×720 출력/960×540 내부 해상도(75%), 내부 TAAU, VSync Off, FPS 제한 0, RT/FG Off다.
이 기록을 최고 품질 설정의 성능으로 취급하지 않는다.

CPU의 그래프 구성·준비가 긴 동안 GPU에 새 작업이 충분히 공급되지 않는 것이 낮은 FPS와 낮은 사용률이 함께 나타난 주된 설명이다.
CPU 렌더 122.760ms 중 fence 대기 평균은 0.017ms다. 총 CPU 사용률이 낮아도 일부 직렬 작업이 프레임을 제한할 수 있으며, 이 실행에는 Debug/디버그 CRT와 검증 비용이 포함된다.

## CPU 구간: 무거운 순

| 구간 | 평균 ms |
|---|---:|
| graph_build | 40.684 |
| graph_record | 18.611 |
| graph_prepare | 17.079 |
| streaming | 15.901 |
| scene_prepare | 9.685 |
| constants_upload | 7.002 |
| graph_submit | 4.619 |
| graph_plan | 3.960 |
| descriptors | 1.890 |
| wrapper | 1.043 |
| UI | 0.811 |
| diagnostics | 0.409 |
| post_submit | 0.234 |
| adapter | 0.207 |
| present | 0.195 |
| wait | 0.017 |

프로브 갱신은 4/243프레임에서 발생했다. 큐브 면별 CPU callback은 실행 시 평균 569–608ms, 프로브 그림자 callback은 267.442ms였다.
면별 callback은 병렬 실행되므로 여섯 값을 더해 프레임 CPU 시간으로 계산하지 않는다.

## GPU 패스: 무거운 순

아래 상시 패스는 243/243프레임에서 측정되었다.

| 패스 | 평균 ms |
|---|---:|
| 3-cascade Directional Shadow | 9.294 |
| GBuffer+Depth | 3.288 |
| VirtualShadow.ResidentAndDirty | 2.571 |
| VirtualShadow.DynamicBank | 1.616 |
| ClusterCutCount | 1.310 |
| VirtualShadow.AdmitCompletePages | 1.173 |
| Deferred Cel Lighting | 1.172 |
| ClusterCutEmit | 1.052 |
| HeroLayerComposite | 0.926 |
| Static Visibility | 0.798 |
| SSRTemporal | 0.744 |
| VirtualShadow.CountPageDraws | 0.605 |
| VirtualShadow.CullAndCommands | 0.601 |
| Temporal Resolve | 0.558 |
| Skin Cache | 0.529 |

주기적 프로브 패스의 실행 시 시간과 전체 프레임당 상각 시간은 별도로 기록했다.
전체 패스의 CPU/GPU 평균·최소·최대·호출 횟수·미측정 상태는 아래 원본 분석 링크에 있다.

## 그래픽 옵션 연산 그룹: 무거운 순

각 그룹의 GPU 시간을 완료 프레임 수로 나눈 값이다. 공유 패스를 여러 옵션이 사용하므로, 이것은 옵션을 껐을 때 절약되는 시간과 같지 않다.
여러 GPU 큐가 겹쳐 실행되므로 그룹 시간을 더해 프레임 지연으로 계산하지 않는다.

| 순위 | 연산 그룹 | ms / 프레임 | 관련 옵션 |
|---:|---|---:|---|
| 1 | 그림자 | 16.785 | shadow mode/quality/distance, geometry, foliage, AF |
| 2 | 지오메트리 | 4.616 | geometry, foliage |
| 3 | 표면/GBuffer | 3.296 | material, POM, texture, AF, geometry, 해상도, adaptive |
| 4 | 굴절 | 1.483 | refraction |
| 5 | GI·반사 공유 프로브 갱신 | 1.367 | probe GI, reflection |
| 6 | 조명 | 1.172 | lighting, material, 해상도 |
| 7 | 화면 공간 반사 | 1.146 | SSR reflection, adaptive |
| 8 | AO | 0.857 | GTAO, adaptive |
| 9 | 볼류메트릭 | 0.561 | volumetric, lighting, particle |
| 10 | 재구성/TAAU | 0.558 | reconstruction, 해상도, sharpening |
| 11 | 후처리 | 0.404 | display, 해상도, sharpening |
| 12 | UI | 0.194 | UI 렌더 |
| 13 | 투명 | 0.133 | transparency, texture, particle |
| 14 | 접촉 그림자 | 0.113 | contact shadows |
| 15 | 색 보정 | 0.112 | color grading |
| 16 | adaptive 처리 | 0.100 | adaptive shading |
| 17 | 표면하 산란 | 0.093 | material |
| 18 | 조명 목록 | 0.064 | lighting |
| 19 | 파티클 | 0.033 | particle |
| 20 | 왜곡 | 0.008 | particle |

공유 프로브는 주변 정적 장면을 큐브맵으로 갱신해 GI와 반사에 함께 제공하는 데이터다. 갱신은 4프레임에서만 발생했으며, 1.367ms는 그 비용을 전체 243프레임으로 나눈 값이다.

## 전체 기록

- [56개 옵션의 requested/effective 및 공유 비용, 전체 분석](../../../Build/play-captures/20261015T0435-human-debug/REPORT.md)
- [CPU 패스 시간 CSV](../../../Build/play-captures/20261015T0435-human-debug/CPU_PASS_TIMES.csv)
- [GPU 패스 시간 CSV](../../../Build/play-captures/20261015T0435-human-debug/GPU_PASS_TIMES.csv)
- [기계 판독용 요약](../../../Build/play-captures/20261015T0435-human-debug/summary.json)
- [옵션 매핑](../../../Build/play-captures/20261015T0435-human-debug/option-costs.json)

## 후속 변경과 검증

실제 기록의 requested/effective 설정은 GTAO·프로브 GI·SSR·래스터 그림자·TAAU 경로와 일치한다. 꺼진 RT·Bloom·Outline·Motion Blur·DOF·프레임 생성의 전용 패스는 없고, SDR 색 보정·노출·Tone Map 등 공유 후처리는 정상적으로 남는다. 수동75%는 내부960×540에 해당한다. 단일 설정 기록으로 옵션을 껐을 때의 절감량이나 품질 단계별 성능 배율을 확정할 수는 없다. 실제 mip/LOD/POM 단계 소비량은 이 기록에 없어 품질 예산 코드와 같은 범위까지만 확인했다.

추가 최적화는 Debug caster 멤버십 저장소 재사용, 프로브 면별 보수적 culling/동일 메시 연속 instancing, 명시적 프로브 source-base 수정, VSM 요청 word별 순회, render-graph batch 리소스 멤버십의 반복 선형 검색 제거다. 기존 GPU/CPU 계약과 원본 캡처는 보존했다. batch 멤버십의 선택 검사8/8 및 RTX 그래프·multi-queue 검증은 통과했다. 신규7개 CPU 하위 계측과 RT opaque 후보 빠른 판정은 추가 native 검증 중이다.

## 최고 RT·144FPS 목표의 별도 측정

원본 인간 Debug 기록과 다른 설정인 FHD 최고 RT·Release capture02는32개 Playing 프레임/22.430초에서1.382 baseFPS였다. CPUrender716.596ms(계획184.275ms), GPUcritical197.697ms다. GPU 패스는 RT반사58.218ms, RTGI48.940ms, 동적BLAS25.907ms, RTAO12.000ms 순이다. CPU는1.062코어/전체5.310%, PID GPU busiest-engine22.497%(21.038초 관측), 장치 전체NVIDIA29%였다. 실제 effective 설정과 공유 비용은 해당 raw report에 별도로 남겼다.

후속 capture03은 정상 종료 Finish·최종 diagnostics가 없고 CPU/GPU/input 버퍼 끝이 잘렸다. 제어 중 인간 입력도 감지되어 엄격한 성능 비교에서 제외했다. 원본은 수정하지 않았고 provenance-correction.json으로 제외 사유만 추가했다.

목표는 모니터 기본1920×1080, RT 포함 최고, 프레임 생성 포함144FPS이며 아직 달성하지 않았다. capture02/04의 실행 파일은 Streamline SDK OFF였으며 후속 capture05는 고정 Streamline2.14.1 ON이다. base 렌더FPS와 실제 표시 프레임 수는 구분하여 후속 측정한다.


## 추가 실제 플레이 capture04와 현재 검증

capture04는 FHD100%·UltraRT·FG OFF의 foreground Playing45프레임/22.385초에서 **1.966 baseFPS**다. Bloom/Outline/DOF/MotionBlur는 꺼져 있어 전체 옵션을 모두 켠 최고 설정 인증은 아니다. Runtime264 CPU/GPU프레임 기록은 완전하며, 외부 PDH91회 오류로 점유율 관측은 부분적이다.

CPUrender504.796ms, 전체CPU5.604%(1.121코어), PID GPU33.471%(21.064/22.385초), 장치 전체NVIDIA39.318%, GPUcritical202.182ms다. CPUgraph build222.409ms 중 ray scene169.074ms와 OMM31.812ms가 큰 비중이다. graph prepare70.364ms, record77.422ms, plan28.106ms다. 하위 시간은 부모 시간에 이미 포함된다.

| GPU 연산 그룹, 무거운 순 | ms/프레임 |
|---|---:|
| RT 반사 |61.681|
| RT GI |54.739|
| RT 장면/가속 구조 |28.406|
| AO |12.909|
| 그림자 |8.123|
| 표면/GBuffer |5.400|
| 지오메트리 |4.485|

공유·병렬 실행 때문에 합계는 프레임 시간이나 옵션 제거 절감량이 아니다. capture02와 플레이 입력 및 시작 구간이 같지 않아 엄격한 개선율은 주장하지 않는다. [전체 capture04 분석](../../../Build/play-captures/20261015-computer-ultra-04/REPORT.md)에 나머지 패스와 옵션 매핑을 보존했다.

이후 OMM 반복 메모리 합산/작업 검색을 줄였고, Playing에만 FG를 허용하며 SDK가 보고한 실제 프레임 수를 별도 계측하도록 수정했다. Streamline ON Release와 실제 FG 동작은 후속 capture05에서 확인했다(아래 제한 참조). RTX/WARP39개 opaque 비교 사례와 셰이더 계약은 통과했다. 이 과정에서 퇴화 삼각형의 FMA 잔차를 막는 precise 면적 판정을 추가했으며, 해당 셰이더는 generation ee632ca6으로 재쿠킹·내보내기를 완료했다. 검증 범위는 아래에 구분한다. 목표144FPS는 미달성이다.

## RT 외 로직 검토와 capture05 정정 (2026-10-15)

목표에 비추어 현재 모든 로직이 최선이라고 판단할 수 없다. 실행 비용이 큰 경로와 데이터·동기화 경계를 중심으로 검토했다. 전체 소스의 모든 분기, 장시간 부하, 모든 GPU 조합을 검사한 것은 아니다.

capture05는 1920×1080·100%·Ultra RT, Bloom/Outline/DOF ON, Motion Blur High, DLSS FG 2 설정이다. Runtime CPU/GPU 205프레임 기록은 완전하다. Playing 136프레임의 동일 설정·상태 구간은 53.830초, **base 2.434 FPS**, SDK 실제 보고 262회/51.370초로 **5.100 present FPS**다. 후자는 물리 디스플레이 표시율이 아니다. CPU는 1.080코어/전체 5.400%, PID GPU busiest engine 37.967%(51.106/59.869초 관측), 장치 전체 NVIDIA 평균 45.633%다. PDH 45회 오류로 점유율은 부분 관측이며 FPS와 집계 구간도 다르다.

중요한 정확성 문제: 이 기록은 Playing 136프레임에 시뮬레이션 544틱만 진행했다. 저지연 핸드오프가 렌더 요청당 한 번의 최대4틱 배치만 완료해 게임 시간이 느려졌다. 따라서 capture04와의 FPS 차이를 동일 게임 부하에서의 개선율로 사용할 수 없다. 패스 비용과 FG 동작의 증거로만 사용한다. Playing→Pause→Playing의 FG 허용 전환은 관측됐다.

### 우선순위별 검토 결과

| 우선순위 | 경로 | 근거와 판단 | 처리 상태 |
|---|---|---|---|
| P1 | 입력→시뮬레이션 핸드오프 | 낮은 FPS에서 프레임당4틱으로 60Hz 진행을 제한. 측정 부하와 게임 속도까지 왜곡 | 같은 요청 안에서 남은 clock debt를 처리하도록 수정. 24/60틱 지연·0틱·취소 검사 통과 |
| P1 | 저지연 모드 전환 | 빠른 disable/enable 시 실제 대기 중인 워커의 parked 플래그를 지워 영구 대기 가능. 회귀 검사에서 10초 timeout 재현 | 실제 워커가 재개할 때만 플래그 해제. 128회 빠른 전환 포함 검사 통과 |
| P2 | 공용 렌더 그래프 준비 | CPU 평균70.276ms: 검증6.713, 정보/키20.590, 컴파일39.807, transient1.122ms. 기존 캐시는 직전 성공 계획 하나 | 키가 실제로 달라지는 원인과 재사용 횟수 계측이 먼저 필요. 검증 제거·무조건 다중 캐시 적용은 보류 |
| P2 | 충돌 후보 정렬 | simulation_combat.cpp의 정렬 비교마다 MovingDiscHitTime 재계산, 후보 처리에서도 반복 | 구간별 1회 계산 후 시간/ID 정렬 후보. 반사·방향 전환 시 재계산과 결정성 동등성 검사 필요. 실시간 비용 미분리 |
| P2 | 명중 기록/정리 | IncrementCastHit/IncrementAreaHit 선형 검색, area_hits 정리에서 영역 목록 반복 검색 | 다수 투사체·영역에서 비용 증가 후보. 원래 순서/체크섬을 보존하는 보조 색인 검토. 실제 병목으로 확정하지 않음 |
| P2 | 자산 snapshot 내보내기 | 긴 목적 파일명에 임시 접미사를 덧붙여 Windows 경로 한도 초과; 실제 export 실패 재현 | 같은 디렉터리의 짧고 고유한 임시 이름으로 수정. 248자 목적 경로·반복 export·무결성·repair 검사 통과 |
| 추가 확인 | 게임 규칙 입력 경계 | LoadSimulationRules는 형식/크기/버전 확인 후 복사. Gameplay의 ArenaObstacles는 count로 span 생성 | 생성기의 검증과 별개로 공개 API에 잘못된 count를 전달하는 경우의 거부 범위 추가 확인 필요. 현재 정상 자산 오류로 주장하지 않음 |

### 측정된 CPU 비용 순위

RT를 포함한 공용 렌더 경로의 벽시계 시간이다. 하위 항목을 부모에 다시 더하지 않는다.

| 순서 | CPU 단계 | 평균 ms |
|---|---|---:|
| 1 | 그래프 구성 |169.765|
| 2 | 명령 기록 |92.134|
| 3 | 그래프 준비 |70.276|
| 4 | 제출 이후 처리 |46.899|
| 5 | 실행 계획 |29.083|
| 6 | 큐 제출 |11.243|
| 7 | 프레임 어댑터 |7.989|
| 8 | 진단 기록 |4.654|
| 9 | 대기 |2.394|
| 10 | 상수 업로드 |2.149|
| 11 | Present |1.768|
| 12 | 장면 준비 |1.129|
| 13 | UI |0.694|
| 14 | 스트리밍 |0.307|

그래프 구성 안에 ray scene125.294ms, OMM23.762ms가 포함된다. CPU 전체 점유율을 높이는 것 자체가 목표가 아니라, 이 직렬 경로를 줄여 GPU 작업 사이의 공백을 줄이는 것이 우선이다.

### 최신 설정 구간 GPU 비용 순위

capture05 config6의 Playing63프레임 기준. 옵션의 독립 절감량이 아닌 공유 그룹 비용이다. 그림자/AO/장면은 RT와도 연결된다.

| 순서 | 그룹 | 평균 ms |
|---|---|---:|
| 1 | 반사 |66.301|
| 2 | GI |59.888|
| 3 | RT 장면 |25.316|
| 4 | AO |11.133|
| 5 | 그림자 |6.958|
| 6 | Surface/GBuffer |5.009|
| 7 | DOF |4.566|
| 8 | 지오메트리 |4.107|
| 9 | 조명 |1.371|
| 10 | 볼류메트릭 |0.984|
| 11 | 접촉 그림자 |0.229|
| 12 | RT 합성 |0.228|
| 13 | 재구성 |0.219|
| 14 | 굴절 |0.214|
| 15 | 모션 블러 |0.198|
| 16 | 공통 후처리 |0.180|
| 17 | 적응형 셰이딩 처리 |0.133|
| 18 | 표면하 산란 |0.130|
| 19 | 외곽선 |0.097|
| 20 | 공유 후처리 가이드 |0.093|
| 21 | 프레임 생성 가이드 |0.063|
| 22 | 투명 |0.058|
| 23 | Bloom |0.057|
| 24 | 색 보정 |0.020|
| 25 | 조명 목록 |0.019|
| 26 | 파티클 |0.007|

프레임 생성 가이드 시간은 외부 SDK 프레임 생성 전체 비용이 아니다. 전체 56개 옵션의 requested/effective와 공유 비용 연결은 [capture05 분석](../../../Build/play-captures/20261015-computer-ultra-05/REPORT.md)에 있다. 단일 최고 설정 측정만으로 각 옵션의 단계별 효율이나 비활성화 절감량을 판정할 수 없다. DOF4.566ms는 비RT 후처리 중 우선 분석할 대상이다. 전체 Playing 기준 하위 패스는 전경 반경 확장4.024ms, 실제 DOF gather0.490ms로 대부분이 반경 확장이다. hdr_post.hlsl은 기본 반경12에서 픽셀마다25×25 이웃을 검사한다. 공유 메모리 타일은 이미 사용하므로, 다음 후보는 타일의 실제 최대 전경 반경으로 검사 범위를 좁히고 전경이 없는 타일을 건너뛰는 정확성 보존 최적화다. 단순 수평/수직 최대 필터로 바꾸면 현재 원형 커버리지 의미가 달라질 수 있다.

### 유지할 구조와 검토 한계

- Core/Gameplay: 60Hz 결정적 시뮬레이션, 명시적 phase 순서, 공간 격자는 유지한다. 게임 속도 문제는 Runtime 핸드오프에서 수정했다.
- Jobs/Runtime: 기존 작업 풀의 worker 대기 협업, bounded SPSC 전달 및 스냅샷 구조는 유지한다. 새 작업 시스템/ECS 도입 근거는 없다.
- GameDomain/Presentation: read-model 저장소의 capacity 재사용, 정적 환경 캐시는 유지한다. 모든 시각 효과 경로를 개별 벤치마크하지는 않았다.
- RendererD3D12: 그래프 검증과 정확한 캐시 키를 유지한다. RT 외에도 그래프 준비·명령 기록·DOF가 검토 대상이다.
- AssetManifest/RenderData: 경로 소유권·의존성 검증, 크기/오버플로/청크 제한은 유지한다. 모든 형식의 fuzz 검증을 새로 실행하지 않았다.
- 저장/오디오: 저장 파일의 flush/교체/백업 구조는 유지한다. 오디오 초기 payload 로딩 및 모든 장치 오류 경로의 실측은 미완료다.
- Apps/빌드: export 임시 경로 오류를 수정했다. 저수준 RuntimeOFF/ContentToolsOFF 경로에 의존성을 추가하지 않았다.

capture06은 물리 Escape로 제어가 중단되고 CPU/GPU 유효 프레임이0이라 제외했다. 원본은 그대로 두고 provenance sidecar에 제외 이유를 기록했다. 컴퓨터 유즈는 종료 상태이며 수정 후 실제 플레이 FPS/60Hz 검증과 144FPS 목표 달성은 아직 확인하지 않았다.

### 현재 수정의 검증 증거

- 입력 동기화·그래픽 설정·아키텍처 3/3: `verify-20261015T090002898Z-17416.json`.
- Runtime 통합·입력 동기화·렌더 스모크·배리어·RT Ultra GPU 검증 5/5: `verify-20261015T090758136Z-21360.json`. RT fixture8프레임, validation errors0, test-render generation `ce566e4156421b187a7e5466a5505bc2b5babb8b37eaf28d347a2a11d9299f66`.
- 내보내기 긴 경로 회귀: `Build/performance-actual/export-long-path-test.log`. 현재 CTest 구성에는 portable asset 검사가 등록되지 않아 기존 Python sibling fixture를 직접 실행했다.
- Streamline ON Release/Debug 빌드 성공. Release SHA256 `F1BEA3D8A7CC5D90E34E9593D7B4C03695D8DB01EE775549371AEBB0ACDCBCE9`, Debug `EB416D9A710AABEBA0A0108C11BB0517B12BE01BF739EE11BB5F20A16FF7D666`.
- 두 빌드의 runtime-full generation은 `ee632ca61e6412cd263ea70ca0e534648781cb93663ea9f9cdb8b01b10d82ac0`이며 3680개 제품을 검증했다. 빌드·fixture 성공을 실제 플레이 FPS 개선으로 대체하지 않는다.

Debug isolated-profile smoke: exit0,8 rendered frames, execution_valid/assertions_passed true, complete diagnostics, validation_errors0. Artifacts: Build/performance-actual/audit-debug-smoke. No game/test-runner process remains. Computer Use was not restarted.

## 테스트 최소화 후속 최적화 (2026-10-15)

확인된 경로를 묶어 수정했고, 이미 통과한 장시간 전체 검증은 반복하지 않았다.

| 적용한 개선 | 보존한 조건 |
|---|---|
| DOF 전경 반경 확장을 타일의 실제 최대 반경 이내로 제한 | 기존 원형 커버리지·경계 픽셀·부분 스레드 그룹·최대32픽셀 반경 |
| DOF gather 반경0에서 32회 샘플링 생략 | 유한값 처리된 원래 중심색 출력 |
| 충돌 후보의 중복 제거 후 정렬용 hit time을 후보당1회 계산 | 시간/적ID 순서. 명중 처리 중 적 이동을 반영하는 실제 처리 직전 재계산 유지 |
| cast/area 명중 기록에 보조 색인 추가 | 원본 vector 순서·count 동작·체크섬 그대로. reset 시 삭제, 정리 후 위치 변경 시 재구축 |
| 종료된 영역 정리를 정렬된 생존ID 이진 검색으로 변경 | 원래 영역·명중 기록 순서 유지 |
| 임시 GPU 메모리 alias 계획에서 시작 패스별 도달 가능성 재사용 | 동일 queue/fence 경로와 공유 가능 조건. 검증 생략 없음 |
| graph resource-user 목록의 중복 검사 단순화 | pass 순서로 수집되므로 마지막 항목만 비교 |

단일 실행의 동일 합성 그래프 비교에서 alias planner는 아래와 같았다. 최적화 전 참조는 현 소스에서 이번 도달 가능성/중복 검사 변경만 되돌린 별도 파일로 만들었고 실제 작업 소스는 되돌리지 않았다.

| 리소스 / 패스 | 이전 ms | 이후 ms | 결과 |
|---|---:|---:|---|
|128 /128|1.4282|0.0754|공유 그룹·메모리량·결과 해시 동일|
|512 /128|22.9192|0.5861|공유 그룹·메모리량·결과 해시 동일|

단일 마이크로벤치마크이며 게임 프레임 시간이나 그래프 전체70ms의 개선율을 뜻하지 않는다. 원본은 `Build/performance-actual/alias-before.csv`, `alias-after.csv`, `alias-reference.cpp`, `alias-benchmark.cpp`다.

최소 검증:
- `simulation.combat`, `simulation.determinism`, `rendering.graph-range-validation` 통과: `verify-20261015T092106113Z-35360.json`의 해당3개 결과. 이 실행의 GPU fixture는 실패했으므로 전체4/4 통과로 표기하지 않는다.
- GPU fixture의 buffer 입력 모사에서 텍스처의 경계 밖 Load와 다른 주소 접근이 검출됐다. 테스트 입력 주소를 clamp하되 원래0출력 조건은 유지해 수정했다. 오류를 숨기거나 검증을 끄지 않았다.
- 수정 후 `rendering.multi-queue-graph` 통과(약4초): `verify-20261015T092303605Z-3844.json`. 기존 VSM/그래프 검사에 실제 DOF dilation 함수 본문을 연결했고,6종 입력의 모든 출력 비트를 독립 CPU 원형 커버리지 계산과 비교했다. 버퍼 Load/Store로 바인딩하므로 최종 화면 합성의 시각 검증을 대신하지는 않는다.
- RTX에서도 Legacy/Enhanced 모두 통과: `Build/performance-actual/dof-alias-rtx.log`. GPU validation 활성 상태.
- 실제 HDR post7개 셰이더 DXC 재쿠킹 성공. runtime-full generation `62a68d2b7eac931e9ae234370009932accd87c7fef03352e70e39498cf77c565`.
- Debug/Release 빌드 완료. 이번 source의 Release SHA256 `B24F7FE8BEB493D7FF01815B7397C8848AA06FC10F3FC7044C5373C60F00F99C`, Debug `75A0F12959E463013CC177CCC1DF0FE69933855969B3E059EDD22A856CFE2123`.

남은 주요 항목은 실제 그래프 key 변경 원인/재사용률, RT 장면 등록과 grass BLAS/광선 비용이다. 옵션별 독립 절감량, 수정 후 실제 FPS와144FPS 유지는 아직 증명하지 않았다. 기존 테스트 결과로 모든 gameplay 분기의 이전 버전 체크섬 동등성을 증명했다고 주장하지 않는다. 원본 캡처와 사용자 프로필을 보존했고 컴퓨터 유즈는 재개하지 않았다.

Debug/Release 양쪽의 generation62a68d2b snapshot 내보내기 완료: 각각3680개 제품 검증 및 runtime-full 포인터 일치 확인.

## RT 장면 등록/제출 CPU 후속 개선 (2026-10-15)

- 같은 native buffer의 descriptor·GPU 주소·장치 소유권을 Register 호출 안에서1회 조회한다. geometry별 범위·정체성·그래프 버전·OMM 검사는 그대로 수행한다. 프레임을 넘겨 보관하지 않아 해제된 포인터/새 generation의 검증을 재사용하지 않는다.
- stable-slot/used-BLAS/import membership에 hash lookup을 사용하고 geometry 검증 여부는 index flag로 저장한다. 출력·등록 순서는 기존 입력 순회 그대로다. 정적/동적 BLAS의 실제 소유 캐시는 기존 ordered map을 유지했다.
- native 메모리 예산 확인에서 동일 live-byte 합산을 한 번만 수행한다.
- 제출 완료 시 별도 live-dynamic 트리를 다시 만들지 않는다. 기존 pending 키의 frame-slot만 현재 슬롯으로 바꿔 조회하며, 다른 프레임 슬롯의 생존 BLAS와 현재 슬롯의 replacement를 구별한다.
- retired BLAS 중복 확인을 hash membership으로 바꾼다. 실제 보존 목록의 추가 순서와 success/failure에서 유지해야 하는 자원은 동일하다.

검증은 변경 경로를 사용하는 `rendering.ray-entity-ultra` 한 개로 한정했다. `verify-20261015T093249776Z-23796.json`: pass,76.5505초. 실제 GPU validation을 켠8프레임, empty-reset frame6, execution_valid=true, validation_errors=0. test-render generation `ca409a7af9944b82cf890ace6b3f3c14bc3bfa5a1caab1dc34ed54a6c4d56e80`. 검사 실행시간 차이를 렌더 성능 개선으로 해석하지 않는다.

Debug/Release 빌드 성공. Release SHA256 `528D0D614978A765C3A81C763944AF2B7D8716A895F0C1ED8477BBC38EAE6965`, Debug `A260069E2FD17EA73FB73CD263D1B7EABDE3BE2C7FCC69F45DECA70244B5B74B`. shader 변경이 없어 runtime-full generation62a68d2b를 유지한다. 실제 플레이 FPS/등록 시간의 전후 비교는 아직 없으며, 컴퓨터 유즈는 재개하지 않았다.

위 CPU 단계 다음으로 수행한 ray world-position 재사용 결과는 아래 절에 기록했다. BLAS stride는12B를 유지한다.


## RT 풀 변형 좌표 재사용 (2026-10-15)

- RayGeometryCS가 이미 계산한 정확한 world position을 저장하고, RT hit 정점 평가에서 이를 재사용한다. 풀 삼각형 후보/확정 hit마다 반복하던 BendGrass의 최대32개 bender 순회와 바람 계산을 생략한다. 정적 좌표, 법선, UV, 재질, 알파 평가와 광선 수/품질은 유지한다.
- job별 `[N local float3][N world float3]` layout이다. BLAS 입력은 기존12B stride 그대로이고, 추가 메모리는 변형 정점당12B다. world 좌표를 local에서 역변환하지 않아 그 왕복 오차도 추가하지 않는다. producer는 uncached 평가를 강제한다.
- RT 그림자/RTAO/GI/반사 graph에 position-cache read를 등록했고, 변형 geometry를 사용할 때 native resource/version을 확인한다. RTAO의 제출 수명 보관에도 해당 buffer를 포함했다.
- 기존 GPU oracle에 실제 shader 저장/로드 helper를 연결했다.43사례(기존39+변형4), Legacy/Enhanced 통과. cached world override 사례로 캐시 소비 여부와 stable coordinates 보존도 검사한다. 재질 opacity/vertex 평가의 일부는 fixture이므로 실제 풀 전체 화면의 시각 동등성 증명으로 확대하지 않는다.

이번에는 테스트2개만 실행했다.

|검사|결과|실행시간|증거|
|---|---|---:|---|
|rendering.multi-queue-graph|PASS|8.76208초|verify-20261015T094535817Z-7896.json|
|rendering.ray-entity-ultra|PASS,8프레임, empty-reset6, GPU validation ON,0errors|93.716초|verify-20261015T094632066Z-4256.json|

실제 RT 검사 test-render generation은 `32c706a808fc243492aa29bc1e08ad71aafda6af27147ed078250e85aff8c5ea`다. 검사 실행시간은 FPS 또는 최적화 전후 비교 자료가 아니다.

Release SHA256 `A7BAD4A31ACF87B437B3E1B8C5FC20C117F88C3379779EB64915B54BC0865282`. Debug SHA256 `D6D95CF8F142F9D8EC64E8B0B2CF1009E8D0ECDF30E521911AD4DF45FF407F46`. 두 빌드 컴파일 성공. runtime-full generation `20b19e5b8ef489eec8fa00a825b9172f7c9f473f8f7935204219088d821e65e2` 생성 완료. Debug/Release 각각3680제품 snapshot export 완료. 로그: `Build/performance-actual/ray-position-export-debug.log`, `ray-position-export-release.log`.

추가 메모리·읽기 대역폭과 연산 감소의 실제 손익은 아직 플레이에서 측정하지 않았다. 수정 후144FPS 유지/화면 비교는 미검증이다. 컴퓨터 유즈와 게임을 재개하지 않았다. 남은 주요 성능 항목은 graph key 변화/재사용률과 RT 장면·BLAS/trace 비용이다.


## 렌더 그래프 범위/의존성 컴파일 개선 (2026-10-15)

- 전체 mip chain과 연속 array/plane 범위를 단일 구간으로 정규화한다. 슬라이스마다 만들던 범위 vector의 할당·복사·교차 검사를 줄였다. 일부 mip/array만 지정한 경우 실제 빈 구간은 유지한다.
- 물리 쓰기는 이미 버전 순으로 연결되어 있으므로 reader를 가장 가까운 이전/다음 overlapping writer에 연결한다. 더 먼 writer와의 순서는 기존 write chain이 보장한다.
- 기존1000개 randomized version replay를3개 queue로 확장했다. 실제 queue order+cross-queue wait로 도달 가능성을 구성하고 원래 모든 겹치는 RAW/WAR 관계와 물리 write chain을 독립적으로 확인한다. 일부 mip/array/plane hole도 검사한다.

검사2개만 실행했다. `verify-20261015T095640413Z-22112.json`: rendering.graph-range-validation PASS0.28123초, rendering.multi-queue-graph PASS7.85787초. 후자는 Legacy/Enhanced 실제 GPU 큐 실행과 출력 비교를 포함한다. 장시간 RT gate는 재실행하지 않았다.

동일 합성 입력을 각 버전에서 한 번씩 실행했다.64리소스/128패스, texture는8mip×256array×2plane이다. 참조 소스는 이번 두 변경 직전 복사이며 이전 최적화를 포함한다.

|입력|이전 ms|이후 ms|동등성|
|---|---:|---:|---|
|전체 텍스처 배열 읽기|58.6852|1.2638|실행 순서·큐 도달 가능성 해시 동일|
|부분 mip/array 읽기|28.9608|30.9043|해시 동일, 개선 미확인|
|buffer32쓰기 버전과 읽기|1.4807|0.9842|해시 동일|

단일 측정이며 부분 범위 증가 원인을 확정하지 않는다. 전체 그래프 준비70ms 또는 게임 FPS 개선율로 환산하지 않는다. 증거: `Build/performance-actual/graph-range-before.csv`, `graph-range-after.csv`, `graph-range-reference.cpp`, `graph-range-benchmark.cpp`, `run-graph-range-benchmark.ps1`.

Release SHA256 `0603E81E5BFB89FC156E3F2CB9845406EE537F499E93E0472619BB6D8215C5FB`. Debug SHA256 `02D1DFC0ADE09E9B141A537078B86AF3F0601F7EE24318843E13223FB065582D`. 두 빌드 성공; runtime generation20b19e5b는 변경 없고 각각3680제품 검증 성공. 수정 후 foreground FPS/144FPS·최고 옵션 화면 비교는 미검증이며 컴퓨터 유즈는 재개하지 않았다.


## 명령 기록: 연속 버퍼 배리어 호출 통합 (2026-10-15)

기존 capture05의 config6,63프레임 CPU_PASS_TIMES에서 RaySceneDynamicBLASBuildUpdate 평균67.27948ms, RaySceneTLASBuild32.31344ms, RayGeometryDeformation3.05037ms였다. 이 값은 callback만이 아니라 allocator/list reset, 전후 배리어와 close를 포함한 기록 구간이다. 병렬 기록이므로 패스 시간을 더해 프레임 시간으로 사용하지 않는다. 같은 scope의 graph_record 평균은67.792635ms, static render instances22057개였다. 원본 캡처의 simulation4ticks/render 제한과 전후 비교 부적합 판정은 유지한다.

- Enhanced 경로의 연속된 buffer barrier를 고정64개 배열에 모아 한 번에 발행한다. 별도 heap 할당은 추가하지 않는다.
- texture/global/alias barrier를 만나거나 패스 본문/마지막 후처리에 들어가기 전에 flush한다. 모든 기존 Sync/Access/resource/범위 값과 배열 순서를 유지한다.
- 명시적 callback 내부 barrier와 Legacy 경로는 기존 즉시 호출을 유지한다. 동기화 제거, wait 생략, quality 변경은 없다.
- 순서 계약은 Microsoft [Enhanced Barriers, Barrier Ordering](https://microsoft.github.io/DirectX-Specs/d3d/D3D12EnhancedBarriers.html#barrier-ordering)을 확인했다. 별칭과 texture layout/discard 경계를 넘겨 묶지 않는다.
- 기존 native fixture에137개 buffer,3개 queue,8768개 word 비교를 추가했다.64개 flush 경계를 두 번 넘기고 나머지9개 tail을 다룬다. Legacy/Enhanced 출력 일치와 GPU validation 검사 통과.

이번 검사는 multi-queue-graph와 ray-entity-ultra2개만 실행했다. 증거: `verify-20261015T100448322Z-37616.json`. 실제 RT8프레임, empty reset6, execution_valid=true, validation_errors=0. 전체 gate나 긴 플레이 테스트는 재실행하지 않았다.

- rendering.ray-entity-ultra: passed, 100.307초.
- rendering.multi-queue-graph: passed, 7.9745초.

Release SHA256 `EDEB83143F53A81BB53274ADB2B13861B3A8F8DF64C6E92822E81B2819D7BD7C`. Debug SHA256 `85D966B9F222ACB315453B8B7D75AC488E5799F7AB67B60FDCBE4C48E32198AB`. 빌드 완료; runtime generation20b19e5b는 동일하고 양쪽3680제품 검증 성공.

이 변경의 실제 기록시간/FPS 절감량은 미측정이다. 기존67/32ms 전체를 배리어 원인으로 단정하지 않으며, 드라이버의 AS build 호출 자체 비용도 남은 후보다. 컴퓨터 유즈는 재개하지 않았고144FPS 유지는 아직 증명하지 않았다.


## 짧은 실제 아레나 진단과 읽기 전용 그래프 최적화 (2026-10-15)

컴퓨터 유즈 없이 숨김 smoke/lockstep 모드로 실제 아레나를16프레임만 실행했다. 입력/foreground 플레이가 아니며 고정 틱, streaming 대기와 마지막 readback이 포함된다. FHD100%, Ultra RT, trace60m/shadow160m 설정을 유지하되 이 진단의 격리 프로필에서만 FG/low-latency를 껐다. 사용자144FPS 목표 조건을 바꾼 것은 아니다. 입력 원본과 사용자 설정은 보존했다.

캡처 `Build/performance-actual/rt-post-optimizations-16ticks`: exit0, execution_valid/assertions_passed=true, CPU/GPU16/16, invalid GPU timings0, diagnostics complete. 실행파일은 EDEB8314..., runtime20b19e5b였다. 이번 아래 읽기 전용 최적화보다 **이전** 기록이다. GPU 점유율은 PDH15오류로 partial이며 잘못된/warmup 구간을 제외했다. sampler 경로를 analyzer 예상 utilization/로 복사한 후 기존 데이터만 다시 분석했으며 게임/샘플링을 재실행하지 않았다.

초기10프레임을 제외한11..16의6프레임 요약이며 안정 상태/백분위 보장이 아니다. GPU는 CPU행의 지연된gpu_frame 값 대신 실제 completed_frame11..16을 사용했다.

|GPU 패스, 무거운 순|평균 ms|
|---|---:|
|HybridReflectionTrace|61.322|
|RTGITrace|51.668|
|RaySceneDynamicBLASBuildUpdate|22.419|
|RTAOTrace|11.746|
|GBuffer+Depth|5.456|
|3cascadeDirectionalShadow|4.086|
|RayGeometryDeformation|3.418|

GPU critical path180.959ms. CPU render474.453ms 중 graph_build144.406ms(ray_scene_register97.519ms 포함), wait134.846ms, graph_prepare71.390ms(compile38.648ms 포함), record48.983ms다. 중첩 scope/병렬 패스 시간을 더하지 않는다. 이후 짧은 구간 CPU2표본은1.707core equivalents/8.533% machine; PID GPU1표본은28.877%,1.290/2.854초만 유효하다. 이 표본은 지속 점유율을 입증하지 못한다.

공유 옵션 그룹 GPU work 순위: reflection62.948ms, GI52.895ms, ray_scene27.065ms, AO12.242ms, shadows7.536ms. 전체56개 옵션 requested/effective 및 공유 그룹 연결은 option-costs.json에 보존했다. 단독 옵션 증분 시간으로 해석하지 않는다. `TAIL-REPORT.md`와 `tail-analysis.json`에 상세 패스/그룹 순위, 중첩 CPU 비용, 표본 범위를 기록했다. 숨김 진단의0.850FPS를 foreground 성능이나 capture05 대비 개선율로 사용하지 않는다.

### 이번 개선과 최소 검증

초기화되어 있고 그래프 어디에도 writer가 없는 version0 자원은 의존성용 reader/version/range 배열을 만들지 않는다. 핸들, ordinal, 접근/큐, 중복 사용, 범위, 최종 export 검증은 그대로 실행한다. 다른 자원의 정규화 임시 배열도 재사용한다. 새 회귀 검사는 잘못된 ordinal/범위/미초기화/export를 거부하고, 뒤에 선언된 writer가 있을 때 모든 이전 read hazard를 유지하는지 확인한다.

한 번씩 실행한 합성 컴파일 비교(게임 프레임 측정 아님):

|입력|전 ms|후 ms|
|---|---:|---:|
|전체 texture 읽기|0.9577|0.1478|
|부분 mip/array 읽기|31.3812|0.1308|
|32쓰기 buffer version chain|1.0908|0.8875|
|22057 읽기 전용 buffer,4패스|14.8184|1.7527|

모든4사례 실행순서/큐 reachability 해시가 같다. 증거 `graph-readonly-*`, `run-graph-readonly-benchmark.ps1`. 절감률을 실제 graph_prepare나 FPS에 대입하지 않는다.

검사2개만 실행: rendering.graph-range-validation, rendering.multi-queue-graph 모두 통과. 영수증 `verify-20261015T102004978Z-8536.json`, 총10.46초. native fixture의 debug/GPU validation을 유지했다. 처음 축약 test명 지정은 선택 단계에서 거부됐고, 정확한 기존 이름으로 정정했다. 전체 gate/긴 플레이/RT fixture 반복은 하지 않았다.

Release SHA256 `FE8F8302C0E80F419DEC19E036CB1FEE46202CE24304033824BE7190C738B4D5`.
Debug SHA256 `282ABAE0B7A9C9B319F9D84AFC3CBB693A710761D90B0542A045B0216A877359`.
두 빌드 성공, runtime generation20b19e5b 및3680제품 검증 성공. git diff --check 통과. 현재 추가 변경의 foreground FPS 개선은 미측정이며144FPS 목표는 미달/미검증이다. 남은 핵심은 반사/GI 광선 비용, dynamic BLAS, CPU RT 장면 등록이다. 컴퓨터 유즈는 계속 꺼져 있다.


## AS 생성 정책의 실제 장면 비교 (2026-10-15)

가속 구조 정책을 짧은 숨김 실제 아레나 실행으로 확인했다. 기준 기록은 이전 `rt-post-optimizations-16ticks`를 재사용했다. 이번에는 양쪽 추적 우선, TLAS만 추적 우선 두 후보를 각각16프레임 한 번씩 실행했다. 앞10프레임은 준비/streaming 영향으로 분리하고 completed GPU frame11..16을 비교했다. 세 기록의56개 requested/effective 설정, input settings 해시, 시뮬레이션 체크섬4930774313638502297이 일치한다. 모두 FHD100%, Ultra RT이며 원본은 보존했다.

|GPU 평균 ms|기준 생성 우선|BLAS/TLAS 추적 우선(기각)|TLAS만 추적 우선(유지)|
|---|---:|---:|---:|
|Critical path|180.959|186.139|164.299|
|반사 추적|61.322|56.540|51.501|
|GI 추적|51.668|49.493|48.078|
|동적 BLAS 갱신|22.419|35.268|21.585|
|RTAO 추적|11.746|10.884|10.388|
|TLAS 생성|0.986|1.043|1.061|
|GBuffer+Depth|5.456|5.903|5.436|

동적 BLAS까지 바꾼 정책은 갱신 비용 증가가 추적 절감을 초과해 되돌렸다. 최종 소스 변경은 `renderer_ray_scene.cpp`의 TlasInputs에서 FAST_BUILD를 FAST_TRACE로 바꾸는 한 줄이다. static BLAS FAST_TRACE/COMPACTION과 dynamic FAST_BUILD/ALLOW_UPDATE는 유지한다. prebuild와 실제 build가 같은 TlasInputs를 사용한다. 광선 수, 해상도, 거리, geometry, alpha/sidedness 검증은 줄이지 않았다.

정책 선택은 NVIDIA의 [AS build preference 안내](https://developer.nvidia.com/blog/best-practices-using-nvidia-rtx-ray-tracing/)를 참고하되 이 PC 측정으로 판단했다. TLAS 추적 우선은 권장 출발점이며 동적 BLAS는 생성/추적 합계 비용에 따라 선택한다. 플래그 조합은 [Microsoft D3D12 계약](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_raytracing_acceleration_structure_build_flags)을 확인했다.

단일 짧은 비교의 GPU critical path는 약9.2% 감소했지만 CPU render는474.453→599.882ms로 증가했다. CPUbuild/compile/record 모두 변동했고 기준 실행은 직전 read-only graph 최적화 이전이므로 AS 정책만의 CPU 효과로 분리할 수 없다. GPU timestamp 감소를 실제 FPS 개선으로 바꾸어 주장하지 않는다. FG/low-latency를 끈 숨김 고정틱 진단이며 foreground144FPS 증거가 아니다.

최종frame16의 depth, base/metadata/normal GBuffer, lighting HDR, composite HDR 여섯 자원은 기준과 바이트 단위 동일하다. `as-policy-output-parity.json`에 해시를 보존했다. 다른 장면/전체시간의 동등성을 뜻하지 않는다.

모든 캡처는 exit0, CPU/GPU16/16 complete, invalid timings0. TLAS-only의 PDH worker는 갱신 대기 중 sampler owner가 종료해 부분 기록이다. 전체 CPU창21.369초 중 유효 PID GPU구간2.584초뿐이며 평균25.930%는 그 유효구간에만 해당한다. 누락 구간은N/A로 유지한다. NVIDIA whole-device/CPU/패스 기록은 각 capture의 analyzer 결과에 별도로 남겼다. GPU 옵션별 단독 토글 측정은 반복하지 않았고 전체56개 설정과 공유 그룹 귀속만 기록했다.

기존 `rendering.ray-entity-ultra` 검사 하나만 실행해 통과했다: `verify-20261015T103026485Z-22560.json`,171.771초,8프레임(empty reset6),execution_valid=true,validation_errors=0. 긴 전체 gate와 다른 변경 없는 검사는 반복하지 않았다. Debug/Release 빌드와3680제품 generation 검증 성공.

- Release SHA256 `228FB0B6AEE25D3A181429F3AEFFDA2ED4B94C71B31A877574B00B05944C7DB5`
- Debug SHA256 `705C052FB8E07E10C6E0791B430529DCA9A5BEE2800AE25381B7A79230ECEAF5`
- runtime generation `20b19e5b8ef489eec8fa00a825b9172f7c9f473f8f7935204219088d821e65e2`

상세 재현 자료: `Build/performance-actual/AS-POLICY-REPORT.md`, `as-policy-comparison.json`, `compare-as-policy.py`, `rt-fast-trace-16ticks`, `rt-tlas-fast-trace-16ticks`. 게임·검사·sampler 프로세스는 종료됐으며 컴퓨터 유즈는 재개하지 않았다. 다음 병목은 객체별 동적BLAS/자원 관리·CPU장면등록과 반사/GI 후보 재질 계산이다.144FPS 목표는 여전히 미달/미검증이다.


## CPU RT 장면 자료 소유권·할당 개선 (2026-10-15)

큰 CPU 비용을 구분하기 위해 `RayTracingSceneSystem::Register`에7개 임시 wall-clock 구간을 넣고, 같은 숨김 아레나를 전후16프레임씩 한 번 실행했다. 외부 CPU/GPU 점유율 sampler는 이 두 실행에서 끄고, renderer의 CPU/GPU timestamp와 설정·진단 기록만 수집했다. 최종 소스와 Debug/Release에서는 임시 계측을 제거했다.

매 후반 프레임의 작업은 동일했다: scene instances22058개, dynamic BLAS/build jobs42191개, graph imports84400개. 일반/OMM BLAS 변형을 각각 유지하는 비용이 포함된다. 아래는 CPU/GPU completed frame11..16의6프레임 평균이며 안정 상태나 foreground FPS를 증명하지 않는다.

|CPU 등록 구간|전 ms|후 ms|
|---|---:|---:|
|입력 검증|2.536|2.518|
|완료 자원 정리·프레임 자원|0.466|0.038|
|BLAS 선택·임시 인덱스 구성|35.273|16.931|
|예산·공유 build 입력 정리|22.122|5.888|
|인스턴스 업로드|2.457|2.559|
|그래프 자원 import|14.161|15.290|
|패스 구성|21.166|14.530|
|전체 RT Register(정리 포함)|102.533|62.091|
|전체 CPU graph_build(상위 scope)|149.413|108.513|
|전체 CPU render(상위 scope)|481.776|444.347|

- pending dynamic cache는 키로 정렬한 연속 배열로 구성하고 completion에서 binary search한다. 기존 persistent map과 프레임 슬롯/세대별 retirement 판정은 유지한다.
- 큰 BuildJob 배열은 pending 완료 처리와 graph callback이 하나의 소유권을 공유한다. Register에서 버전을 모두 기록한 뒤 callback이 읽으며, callback별 대량 복사는 제거했다.
- build 입력 검증은 같은 공통 루틴을 사용하면서 RaySceneGeometry의 임시 복사를 없앴다. 동일 버퍼의 충돌 version/access 검사는 유지한다.
- 같은 native vertex/index 입력은 FlightLease에서 한 번만 강하게 보유한다. 정확한 completion과 BLAS의 transitive lease는 그대로 유지한다.

설정 파일과 시뮬레이션 체크섬이 같고, 마지막frame16의 depth, base/metadata/normal GBuffer, lighting HDR, composite HDR6개 자원은 SHA256/크기 동일이다. 장면·광선·sample 수를 줄인 변화가 아니다. GPUcritical은178.748→182.726ms로 개선되지 않았으며, 이번 변경은 CPU 자료 처리 개선이다. 실제 foreground FPS 개선이나144FPS 달성으로 해석하지 않는다.

기존 검사 두 개만 실행했다. `verify-20261015T104159978Z-6612.json`: 모두 통과, native RT8프레임(empty reset6), execution_valid=true, validation_errors=0. 공유 OMM 입력 충돌/수명, Legacy/Enhanced native multi-queue 검사도 포함된다. 전체 gate나 변경 없는 다른 검사는 반복하지 않았다.

rendering.ray-entity-ultra: passed (137.808s); rendering.multi-queue-graph: passed (9.84204s)

Release SHA256 `2C36CC9588B1B89CBEED0D3203D0492B3B9E326D5DBE4920327458936704AB51`.
Debug SHA256 `1B7EAB6F4342824A129B2E523560776FB5B5746E1C23C380AB29E6BDBF4FC29F`.
두 빌드 성공, runtime generation20b19e5b 및3680제품 검증 성공. 계측 빌드 해시는 각 capture-session.json에 별도로 있으며 최종 실행파일과 혼동하지 않는다.

증거: `rt-scene-cpu-profile-16ticks`, `rt-scene-cpu-optimized-16ticks`, `ray-scene-ownership-comparison.json`, `RAY-SCENE-OWNERSHIP-REPORT.md`; 임시 계측 전후 소스도 ignored Build에 보존했다. 아직84400개 graph imports와42191개 BLAS 작업이 남아 있으므로, 다음은 native 자원/그래프 등록 수 자체를 줄이는 저장 구조와 반사/GI 후보 계산이다. 컴퓨터 유즈는 재개하지 않았고144FPS는 미달/미검증이다.


## 동적 BLAS scratch 공유 버퍼 (2026-10-15)

동적 BLAS마다 따로 만들던 scratch 자원을 프레임별 공유 버퍼로 통합했다. 각 build/update는 서로 겹치지 않는 256-byte 정렬 구간을 사용한다. 프레임 재사용 completion, callback의 강한 소유권, 각 구간 범위 검증과 기존/새 버퍼를 함께 포함하는 교체 시점 예산 검사를 유지한다. static BLAS scratch와 광선/geometry/품질 설정은 변경하지 않았다. 버퍼는 프레임별 최대 필요 용량을 유지하며 실제 native 메모리 예산에 포함된다.

직전 동일 장면 기록을 기준으로 재사용하고 변경 후 숨김 실행을16프레임 한 번만 수행했다. 두 기록의56개 설정과 simulation checksum이 같으며, completed frame11..16의6프레임을 비교했다. 기준에는 임시 CPU stage 출력 계측이 있고 이번 최종 빌드에는 없으므로 변화 전체를 공유 버퍼만의 효과로 단정하지 않는다. 단일 실행의 GPU clock/scheduling 변동도 분리하지 않았다.

|평균 구간|전 ms|후 ms|
|---|---:|---:|
|CPU render|444.347|329.472|
|CPU graph_build|108.513|96.469|
|CPU RT scene register (build에 포함)|62.091|52.670|
|CPU graph_prepare|70.665|38.555|
|CPU graph compile (prepare에 포함)|37.722|22.024|
|CPU graph record|47.819|35.051|
|GPU critical path|182.726|140.755|
|GPU 반사 추적|59.628|44.127|
|GPU GI 추적|51.810|41.214|
|GPU 동적 BLAS|23.755|16.263|
|GPU RTAO|12.660|9.174|
|GPU GBuffer+Depth|5.906|4.922|

후반 공유 옵션 그룹 비용 순서: 반사45.489ms, GI42.285ms, ray scene20.532ms, AO9.587ms, 그림자7.603ms, surface4.926ms, geometry4.075ms, lighting1.164ms, volumetrics1.156ms, DOF1.120ms. 전체 그룹과 패스는 `Build/performance-actual/SCRATCH-POOL-REPORT.md`, `scratch-pool-comparison.json`에 내림차순 기록했다. 공유 그룹은 개별 옵션의 단독 증분 비용이 아니며, 중첩 CPU scope나 겹치는 GPU 패스를 합산하지 않는다.

기준42191개 동적 작업에 현재 등록 경로를 적용하면 graph imports는84400→42210으로 줄어든다. 이는 소스에서 도출한 수이며 새 runtime counter 실측값은 아니다. acceleration_structure_bytes가 같다는 이유로 메모리 절감량을 주장하지 않는다.

마지막frame16의 depth/GBuffer3종/lighting HDR/composite HDR6개 출력은 바이트 단위 동일하다. 진단 CPU/GPU16/16 complete, invalid timings0. 이번 실행에서는 외부 점유율 sampler를 사용하지 않아 CPU/GPU 점유율은 N/A다. FG/low-latency를 끈 숨김 lockstep 진단이므로 foreground FPS나144FPS 달성 증거로 사용하지 않는다.

최소 검증: `verify-20261015T104817174Z-22616.json`에서 rendering.ray-entity-ultra125.167초, rendering.multi-queue-graph7.89552초 모두 통과했다. RT8프레임(empty reset6), execution_valid=true, GPU validation_errors=0. 새 범위 회귀 검사는 정렬·비중첩·zero/overflow/미정렬 거부 및 실패 시 출력 불변을 확인한다. 전체 gate와 긴 플레이는 반복하지 않았다.

- Release SHA256 `72A539F47A477152CBA866490A02364F9B1B9E8D70562DFC4DCDDCC7BB41B5C1`
- Debug SHA256 `35551AD232EB3BCC54B4EAE1DCF33D49EC329CB7D132C098D6F3EBB684515C77`
- runtime generation `20b19e5b8ef489eec8fa00a825b9172f7c9f473f8f7935204219088d821e65e2`,3680제품 검증 성공.

남은 핵심은 반사/GI의 ray candidate 계산과 여전히 객체별인 BLAS 결과 자원 관리다. scratch 풀은 완료됐지만 결과 자원 풀링은 아직 구현하지 않았다.144FPS 목표는 미달/미검증이며 컴퓨터 유즈는 계속 꺼져 있다.


## 일반/OMM BLAS 중복 제거와 불필요한 로직 검토 (2026-10-15)

같은 형상에 일반용·OMM용 BLAS를 각각 만들던 variant 루프를 제거했다. OMM이 있는 BLAS는 `ALLOW_DISABLE_OMMS`로 한 번 생성하고, 일반 TLAS의 해당 instance에만 `DISABLE_OMMS`를 지정한다. OMM TLAS는 같은 GPU 주소를 사용하되 원래 instance flags를 유지한다. 일반/OMM 두 소비 경로, AO·그림자와 반사·GI 셰이더 판정은 유지한다. 별도 OMM BLAS 포인터 배열과 중복 build/cache 등록·두 번째 BLAS graph read 루프가 없어졌다. prebuild와 실제 build/update는 같은 플래그 함수를 사용한다.

이는 [Microsoft DXR의 instance별 OMM 비활성화 계약](https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html#d3d12_raytracing_instance_flags)에 따른 공유다. FORCE_NON_OPAQUE는 제거하지 않았다. 현재 셰이더는 앞뒷면·identity·지원되지 않는 재질을 직접 판정하므로 단순 제거는 동등성을 보장하지 못한다. OMM의 투명 미세삼각형 제거는 이 플래그보다 먼저 적용된다.

### 단일 짧은 비교

기준 `rt-scratch-pool-16ticks`를 재사용하고 `rt-shared-omm-16ticks`를16프레임 한 번 실행했다. FHD100%/최고 RT 설정, 동일56개 requested/effective 값과 checksum4930774313638502297이다. 두 빌드 모두 임시 profiler가 없으며 외부 점유율 sampler는 껐다. completed frame11..16 평균:

|구간|전 ms|후 ms|
|---|---:|---:|
|CPU render|329.472|242.056|
|CPU graph_build|96.469|62.801|
|CPU ray_scene_register (build에 포함)|52.670|26.022|
|CPU graph_prepare|38.555|20.587|
|CPU graph compile (prepare에 포함)|22.024|12.097|
|CPU graph record|35.051|18.845|
|GPU critical path|140.755|131.055|
|GPU dynamic BLAS|16.263|6.162|
|GPU 반사 추적|44.127|45.188|
|GPU GI 추적|41.214|40.641|
|GPU RTAO|9.174|9.359|

CPU 중첩 scope와 GPU 병렬 구간은 합산하지 않는다. 단일 실행의 clock/scheduling 변동이 남으며 반사·AO 자체의 개선은 확인되지 않았다. 마지막frame16의 depth/GBuffer3종/lighting HDR/composite HDR6개 raw 자원은 크기와 SHA256이 같다. CPU/GPU16/16 complete, invalid timings0, exit0. 점유율은 N/A이고 analyzer의 partial 사유는 의도적으로 생략한 utilization metadata다. FG/low-latency를 끈 숨김 고정틱 진단으로 실제 foreground FPS/144FPS를 입증하지 않는다.

무거운 공유 옵션 그룹: 반사46.625ms, GI41.617ms, ray scene10.568ms, AO9.773ms, 그림자6.994ms, surface4.793ms, geometry3.894ms, volumetrics1.197ms, lighting1.194ms, DOF1.182ms. 단독 옵션 토글 비용이 아니며 전체 그룹·패스와56개 설정은 `SHARED-OMM-REPORT.md`, `shared-omm-comparison.json`, 각 capture의 `option-costs.json`에 있다. 비교 스크립트는 `Build/performance-actual/compare-shared-omm.py`다.

### 최소 네이티브 검증과 범위

기존 rendering.ray-entity-ultra에 동적 잔디 하나를 추가하고 마지막 populated frame에서 실제 OMM 연결을 요구했다. 초기 두 시도는 assertion 실패였다: 첫 프레임 검사, 이어 마지막 프레임 검사 모두 OMM이 연결되지 않았다. native D3D12 오류가 아니라 기본 식생 밀도에서 해당 잔디가 제외된 fixture 조건이었다. 식생 품질을 Ultra로 고정한 최종 검사 `verify-20261015T110843820Z-35144.json`은76.8607초에 통과했다.8프레임, empty reset6, execution_valid=true, validation_errors=0. OMM88삼각형이frame4/5/7/8에 실제 연결됐고 일반 AO/그림자 및 OMM GI/반사 패스가 실행됐다. 최초 실패 기록110206873/110520811은 보존했다. 전체 gate와 무관한 검사는 반복하지 않았다.

이 fixture의 OMM은 known_cells=0인 unknown coverage다. 따라서 연결·공유·갱신·reset과 shader fallback을 검증하지만, known-transparent microtriangle의 별도 동등성 oracle을 대신하지 않는다. 실제 아레나 출력 비교는 위6개 최종 자원 범위에 한정된다.

- Release SHA256 `CD40CD199784F16D5D5DFEB06ED03CFCDF5FA28562D50269A52A3DA478C5D21C`
- Debug SHA256 `57552FC92E9B19B4452282C25AACFCEFBC73543663EBE6A1F06F85FF6E5E4628`
- 두 빌드 성공, runtime generation20b19e5b3680제품 검증 성공. 변경 파일 diff --check 통과.

### 다음 삭제·단순화 후보

- OMM known_cells=0인 패키지는 모든 후보를 다시 셰이더에서 판정한다. 이 fixture에서 확인한 불필요한 준비/연결 후보이며, 실제 아레나 패키지별 분포를 확인한 뒤 zero-known OMM 생성 생략을 검토한다. 아직 변경하지 않았다.
- 남은 객체별 BLAS 결과 자원의 등록·barrier 비용은 공유 저장으로 줄일 후보지만 주소·수명·budget 계약을 먼저 보존해야 한다. 아직 구현하지 않았다.
- identity/범위/지원 재질 검사, GPU 완료 대기, 교체 전후 메모리 예산 검사는 필요한 로직이므로 삭제하지 않았다.

최고 옵션144FPS 목표는 미달/미검증이다. 컴퓨터 유즈는 계속 꺼져 있고 원본 사용자 캡처·설정은 보존했다.


## 포니테일 검토: 효과 없는 OMM 준비와 반복 복사 제거 (2026-10-15)

현재 runtime generation의 OMM1266패키지를 manifest SHA256과 대조하고 분석했다.839개는 known_cells=0이며 잔디4종·잎3종도 모두 해당한다. 패키지 수이며 실제 활성 instance 수가 아니다. 모든 미세삼각형을 셰이더에서 다시 판정해야 하는 이 경우, 검증한 메타데이터만 캐시하고 native input/array/scratch/upload 및 graph job을 생략했다. 알려진 coverage가 있는 패키지는 기존 native 경로를 유지한다.

BuildRayOmmRequest의 출력용 words 인자와 매프레임 중첩 vector를 제거하고 기존 std::map 캐시의 words를 span으로 참조한다. 모든 호출처와 fixture를 갱신했다. Register가 동기적으로 소비하며 material/pipeline/generation 변경은 요청 구성 전에 캐시를 무효화한다. 패키지·geometry·재질 identity, 실제 opacity words, subdivision, 메모리 예산 및 GPU 완료 검증은 유지했다. 새 설정·의존성·추상화는 추가하지 않았다.

### 짧은 측정 한 번

`rt-shared-omm-16ticks`를 기준으로 새 `rt-unknown-omm-skipped-16ticks`를16프레임 한 번 실행했다. 완료 frame11..16 평균:

|구간|전 ms|후 ms|
|---|---:|---:|
|CPU render|242.056|230.702|
|CPU graph build|62.801|59.096|
|CPU ray scene register (build 포함)|26.022|24.171|
|CPU graph prepare|20.587|18.762|
|CPU record|18.845|16.898|
|GPU critical path|131.055|125.895|
|GPU 반사 추적|45.188|41.743|
|GPU GI 추적|40.641|40.462|
|GPU 동적 BLAS|6.162|6.971|
|GPU RTAO|9.359|8.241|
|GPU GBuffer+Depth|4.788|4.453|

단일 실행의 scheduling/clock 변동이 있어 모든 차이를 변경의 효과로 단정하지 않는다. 동적 BLAS 시간은 오히려 증가했다. 동일56설정/checksum, 마지막frame의 depth/GBuffer3종/lighting HDR/composite HDR6개 raw 자원은 바이트 단위 동일하다. CPU/GPU16/16 complete, invalid0, 정상 종료. 외부 점유율 sampler를 생략했으므로 점유율 N/A이며 analyzer partial 사유도 이것뿐이다. FHD100%/최고 RT, FG·low latency OFF의 숨김 고정틱 진단이며 실제 foreground FPS나144FPS 증거가 아니다.

무거운 공유 옵션 그룹 순서: **반사43.255ms → GI41.446ms → ray scene11.009ms → AO8.656ms → 그림자7.863ms → surface4.457ms → geometry3.934ms → DOF1.177ms → lighting1.168ms → volumetrics1.029ms**. 공유 그룹은 옵션 단독 증분 비용이 아니다. 전체 패스·그룹은 `Build/performance-actual/UNKNOWN-OMM-REPORT.md` 및 `unknown-omm-comparison.json`,56설정별 비용은 새 capture의 `option-costs.json`에 보존했다.

### 최소 검증

- `verify-20261015T111541842Z-29864.json`: rendering.ray-entity-ultra85.6714초 통과,8frames/reset6, execution_valid=true, validation_errors=0. 잔디의 unknown-only OMM은 마지막frame에 applied/ready=false,linked0,allocated0 및 명시적 이유를 요구한다. 결과 사본은 `unknown-omm-native-result.json`.
- Native test generation은c2b877a000f80dd227b11f9e236d618d147d18d8f9ddd3f14832fa5704ab24c2다. 빌드가 기존 local-probes Action을 재발행했으며 이전32c706 generation의3297제품 중 `render/lighting/local_probes.spkg`만 달라졌다. 이는 고정 runtime20b를 사용한 성능 비교와 구분한다.
- Release SHA256 `43E79E3DC9A6C889B1EB4FEE04FAFA7C2344FBAF701CF0580FF1C3C8325C1F88`
- Debug SHA256 `24231F35B55BAF4A045E66181EB48D4E2572EED658101F78991CC1D5038C78F6`
- 두 빌드 성공. runtime20b19e5b3680제품 검증 성공, diff --check 통과. 테스트 게임과 runner 종료 확인.

전체 gate·장시간 플레이·known-transparent native oracle은 추가 반복하지 않았다. 현재 fixture는 unknown-only 생략과 fallback을 검증한다. 남은 병목은 반사/GI 후보 판정·재질 계산과 객체별 BLAS 결과 자원 관리다. FORCE_NON_OPAQUE와 identity/범위/완료대기/실제 예산 검사는 필요한 로직이므로 유지한다.144FPS는 미달/미검증이며 컴퓨터 유즈는 계속 꺼져 있다.


## 동일 OMM 요청의 복사·반복 검증 제거 (2026-10-15)

`BuildRayOmmRequest`는 안정적인 std::map 캐시의 불변 request 포인터를 돌려준다. cache hit에서는 제품 경로 문자열 생성·조회, identity/소유권/문자열 복사가 사라진다. `Register`는 동일 포인터의 request를 한 번만 검증한다. 다른 request는 같은 coverage key여도 별도로 identity/actual words를 확인하며, instance별 binding·linked/known/unknown/unavailable 통계는 유지한다. actual old/candidate/in-flight 예산 검사와 완료 대기, GPU callback의 소유권 보존은 그대로다. 캐시 무효화는 material/pipeline/generation 변경 시 요청 수집 전에 발생한다.

모든 private 호출부·선언과 fixture를 함께 바꿨다. 네이티브 fixture에는 같은 environment_variant의 잔디 instance를 하나 더 넣어 중복 request 경로를 사용한다. public API·셰이더·콘텐츠·그래픽 설정은 바꾸지 않았다.

### 단일 비교: CPU 준비 감소, 전체 프레임 개선은 미확인

기준 `rt-unknown-omm-skipped-16ticks`와 새 `rt-omm-request-reuse-16ticks`의 completed frame11..16 평균:

|구간|전 ms|후 ms|
|---|---:|---:|
|CPU OMM prepare/register|15.005|9.566|
|CPU graph build (OMM 포함)|59.096|52.836|
|CPU ray scene register|24.171|24.580|
|CPU graph prepare|18.762|19.108|
|CPU graph record|16.898|18.280|
|CPU wait|109.021|146.621|
|CPU render 전체|230.702|265.508|
|GPU critical path|125.895|167.115|
|GPU 반사 추적|41.743|58.757|
|GPU GI 추적|40.462|51.631|
|GPU 동적 BLAS|6.971|9.261|
|GPU RTAO|8.241|11.940|
|GPU GBuffer+Depth|4.453|6.235|

**OMM 준비 구간은5.439ms 감소했으나 GPU와 전체 render 시간은 증가했다. FPS 개선을 확인했다고 주장하지 않는다.** GPU 패스별 비용이 전반적으로 증가했고, 비교한6프레임의 측정 pass id/name/queue654행은 완전히 일치했다. 동일56설정/checksum, 마지막frame depth/GBuffer3종/lighting HDR/composite HDR6개 raw 자원도 바이트 단위 동일하다. 원인 확인을 위한 추가 플레이 측정은 반복하지 않았다.

측정 뒤 조회한 GPU는 P3/1020MHz/46C/28%/64.11W였다. 이는 실행 중 클럭·점유율이 아니며 시간 증가의 원인을 확정하지 못한다. 실행 중 외부 sampler는 꺼져 있어 평균 CPU/GPU 점유율은 N/A다. CPU/GPU16/16 complete, invalid0, exit0. FHD100%/최고 RT/FG·low latency OFF 숨김 lockstep 진단으로 foreground 플레이나144FPS 검증을 대신하지 않는다.

이번 측정의 공유 그룹 무거운 순서: 반사60.541ms, GI53.002ms, ray scene14.539ms, AO12.510ms, 그림자9.775ms, surface6.241ms, geometry4.598ms, DOF1.354ms, lighting1.212ms, volumetrics1.114ms. 각 옵션의 단독 증분 비용이 아니다. 전체 패스·그룹·56설정 기록은 `Build/performance-actual/OMM-REQUEST-REUSE-REPORT.md`, `omm-request-reuse-comparison.json`, 새 capture의 `option-costs.json`에 있다.

### 검증·남은 작업

- `verify-20261015T112633286Z-36028.json`: rendering.ray-entity-ultra145.986초 통과.8frames/reset6, execution_valid=true, validation_errors=0. 추가 broad gate 없음. 결과 사본 `omm-request-reuse-native-result.json`.
- Release SHA256 `76C328730D5A5190585D093E8178B9D0103CD9906A85948D7C23CF3A2BA6E70C`
- Debug SHA256 `1D6C5465C0DA5C334DD61A8107D2435495E2FA84F13C1A55F634BF1ECE9CD4CC`
- 두 빌드 성공, runtime generation20b19e5b3680제품 검증 성공. 네이티브 fixture는 별도 test-render generationefdc4948deb6c2dbdaf9dd0512c8b6e3165d479beee11ae2f119a0a27322a9c9 사용. diff --check 통과. 게임·runner 종료 확인.

남은 CPU 후보는 한 프레임 내 반복하는 `ray_scene.AllocatedBytes()` 목록 순회다. 예산 검사를 삭제하지 않고 동일 상태의 중복 집계만 줄이는 방향으로 검토한다. GPU는 여전히 반사/GI가 우선이며 이번 전체 GPU 시간 증가 원인은 확정되지 않았다.144FPS 목표 미달/미검증, 컴퓨터 유즈 OFF 유지.


## Native RT 예산 집계 중복 제거와 계측 경로 확인 (2026-10-15)

frame의 두 `AllocatedBytes()` 호출과 setter의 재집계를 `AdmitNativeByteHeadroom(headroom)`으로 합쳤다. 예산 승인 한 번당 같은 live BLAS 목록 순회가3회에서1회로 줄었다. persistent cache나 무효화 상태는 추가하지 않았다. 진행 중 graph 거부·0예산 거부·uint64 overflow 거부 및 old/candidate/in-flight backing을 포함하는 기존 실제 집계와 Register의 실제 예산 검사는 유지한다.

실제 메서드 본문을 추출해 최소 MSVC harness에서 실행했다. empty/pending 상태,10개 경계값, 정확히1회 집계, 실패 시 예산 불변 모두 통과했다. `ray-budget-admission-check.cpp/.log`와 method SHA256이 증거이며 D3D12 allocator 자체의 테스트를 대신하지 않는다. 기존 `rendering.ray-entity-ultra`도142.688초 통과:8frames/reset6, execution_valid=true, validation_errors=0 (`verify-20261015T113837787Z-8308.json`, 결과 사본 `ray-budget-single-scan-native-result.json`).

### 한 번의 비교와 사용률 기록

`rt-budget-single-scan-16ticks` 한 번, 동일FHD100%/최고RT/56설정/checksum. 기준은 이전 request-reuse 실행이며 completed frame11..16 평균이다. 기준 sampler OFF, 이번 기존 CPU/GPU/NVIDIA sampler ON이므로 계측 오버헤드 차이가 있다.

|구간|전 ms|후 ms|
|---|---:|---:|
|CPU OMM prepare/register (scene budget 승인 포함)|9.566|5.690|
|CPU graph build|52.836|51.475|
|CPU ray scene register|24.580|26.236|
|CPU graph prepare|19.108|20.070|
|CPU graph record|18.280|18.957|
|CPU wait|146.621|147.252|
|CPU render 전체|265.508|268.874|
|GPU critical path|167.115|168.117|
|GPU 반사 추적|58.757|59.082|
|GPU GI 추적|51.631|51.309|
|GPU 동적 BLAS|9.261|10.121|

OMM 준비 scope는3.876ms 감소했지만 전체 프레임/FPS 개선은 확인되지 않았다. 마지막frame의 depth/GBuffer3종/lighting HDR/composite HDR6개 raw 자원은 바이트 단위 동일하다. CPU/GPU16/16complete, invalid0, game/sampler exit0.

Playing window7.313초에 완전히 포함된 CPU6표본/6.395초 기준 process CPU1.686core,20논리CPU 대비8.429%. NVIDIA7표본 기준 장치 전체 GPU26.571%, graphics clock평균2721.43MHz(2130..2820),전력100W이다. 비교 후반6프레임에는 CPU1표본/1.027초(1.506core,7.529%)와 NVIDIA2표본만 포함된다: GPU45%(14..76),clock2820MHz2회,전력139.89W. NVIDIA는 다른 앱을 포함한 장치 전체 수치다. 이전 실행 중 클럭 기록이 없으므로 이전 GPU 시간 증가의 원인을 확정하지 못한다.

PID별 GPU와 논리CPU PDH는 N/A. 초기화8.210초, invalid sample1회, target engine을 얻지 못한 상태에서 재조회하다 게임 종료 후 소유 worker가 정리됐다. incomplete worker snapshot/누락을 숨기거나0%로 채우지 않았다. 게임36952·sampler28156·PDH29268 종료 확인. 다음 계측 개선 대상은 짧은 실행에서 PDH 초기화/재열거 비용을 줄이는 것이다.

공유 그룹 비용 순서: 반사60.924ms,GI52.478ms,ray scene15.473ms,AO12.585ms,그림자10.765ms,surface5.756ms,geometry4.429ms,volumetrics1.482ms,DOF1.394ms,lighting1.340ms. 단독 옵션 증분 비용이 아니다. `BUDGET-SINGLE-SCAN-REPORT.md`, `budget-single-scan-comparison.json`, capture의 `option-costs.json`/전체패스 CSV에 기록했다.

### 고정틱 진단의 강제 대기: 실제 플레이 FPS와 분리

Runtime은 `smoke_render_lockstep`일 때 `RenderFrameInput.wait_for_streaming=true`를 넘긴다. Renderer는 frame slot 대기 전에 `WaitForGpu()`로 전체 제출을 기다린다. simulation도 렌더 완료까지 yield한다. 따라서 이 진단의 CPU사용률·wait·전체 frame time은 일반 플레이의 병렬 진행/프레임 페이싱을 대표하지 않는다. 결정적 streaming/출력 비교 계약이므로 이 대기를 제거하거나144FPS 목표의 해결책으로 삼지 않았다.

기존 QPC로 분리한 frame slot 대기는 unknown-only skip기준0.0012ms,request reuse기준0.0010ms에 불과하다. 각 wait_us109.021/146.621ms의 대부분은 그 밖의 구간이다. 이번 frame slot 대기도0.001167ms이고 나머지는147.251333ms다. 나머지에는 강제 WaitForGpu와 사전 준비가 포함되므로 이를 순수 GPU-drain 단독 타이머라고 부르지 않는다. 증거 `lockstep-wait-attribution.json` 및 Runtime application.cpp2328/Renderer renderer_frame.cpp103..137. 다음 성능 측정은 일반 realtime replay 경로로 구분해야 한다. 현재 기록은 foreground144FPS 증거가 아니다.

Release SHA256 `1D0992E64902BBDCFA72840F7FBDAAEC025AA0ADCC7A47278DAD40D0DD87982F`, Debug `7C938138BA5348C19E0D36F92BC436607B43C5B65AE950AAB38545B60803E0BD`. 두 빌드/runtime20b19e5b3680제품 검증 성공. Native test generation7d2ff97fc9b3284cef207b9295c72c8db5e2c6e0c6f8037ae35a579b6d5e9c05. 컴퓨터 유즈OFF,원본 캡처·dirty 작업 보존. 목표 미달/미검증.


## 포니테일 재검토와 일반 재생 경로 확인 (2026-10-15)

검토 결과, 중복 BLAS/OMM 준비, 요청 복사, 반복 메모리 예산 집계를 없앤 기존 최적화를 유지한다. RayScene의 실제 자원/graph 일치 검사, GPU 완료 확인, 동적 scratch 범위 및 예산 검사는 제거하지 않는다. 이번에는 production 소스와 그래픽 품질을 추가 변경하지 않았다. 정식 tools/start-play-capture.ps1은 이미 게임 시작 직후 sampler를 실행하므로 별도 readiness 계층을 추가할 필요가 없다.

기존 golden-short는 임시 무적 빌드에서 만든 입력으로 현재 실행 시 tick1 체크섬이 다르다. 첫 실행 rt-realtime-replay-240ticks는 프레임을 기록하기 전에 exit1로 종료했고 성능 표본에서 제외했다. 체크섬을 덮어쓰거나 비교를 끄지 않았다. 현재 Release Gameplay 라이브러리를 연결한 /fp:strict 사전 검사에서 computer-ultra-05 원본의 첫421개 입력(게임 tick240까지)과 human-debug 원본의 해당 prefix가 모두 통과했다. 원본은 수정하지 않았다.

이후 rt-realtime-native-playback-240ticks는 smoke/lockstep 없이 정상60Hz replay로 실행했다. FHD1920x1080, 최고 RT, DLSS FG2x 설정을 사용했고 tick240/checksum12428462166565086596, exit0, CPU/GPU18프레임 완결을 확인했다. 실행 전체 약14.414초, Playing14프레임이다. 단 모든 프레임 foreground=0이므로 foreground 플레이 검증이 아니다. 이번 Release 실행에서는 D3D12/GPU validation을 켜지 않았으며 이전 별도 native 검증과 구분한다.

|측정 항목|결과|
|---|---:|
|Playing 동일 phase/config QPC 기반 FPS|4.932|
|SDK 실제 presentation count 기반 FPS|5.311 (11회/2.071초)|
|CPU render 평균|250.002ms|
|CPU graph build 평균|102.834ms|
|CPU graph record 평균|37.256ms|
|CPU wait 평균|17.482ms|
|GPU critical path 평균|169.003ms|

SDK count는 물리 모니터 표시 cadence가 아니며 nominal2배를 곱하지 않았다. 초기 구축과 streaming이 포함된 짧은 백그라운드 기록으로, 다른 장면/lockstep/foreground 기록과의 FPS 개선율을 계산하지 않는다.

무거운 GPU 패스 순서는 반사 추적58.837ms, GI 추적51.507ms, AO11.805ms, 동적 BLAS10.196ms, GBuffer+Depth5.827ms, geometry deformation4.325ms, 방향광 cascade shadow3.888ms다. 모두 Playing14프레임 평균이다. 공유 패스와 여러 큐가 겹치므로 단순 합계나 각 옵션을 끌 때의 절감량으로 해석하지 않는다. 전체 패스와 설정 revision별56옵션은 [이번 분석](../../../Build/performance-actual/rt-realtime-native-playback-240ticks/REPORT.md)에 보존했다.

점유율 계측은 불완전하다. 임시 replay 실행 스크립트가 버퍼링되는 performance.csv 크기를 기다려 sampler를 게임 종료0.335초 전에 실행했다. CPU/NVIDIA 표본은0개, PDH worker는 대상 프로세스가 이미 종료돼 실패했다. 따라서 이번 CPU/GPU 점유율은 N/A다. capture-session.complete=true는 게임 diagnostics만을 가리키며 sampler_exit_code=1 및 분석 partial을 함께 확인해야 한다. 정식 launcher는 이 대기를 사용하지 않는다. 다음에 짧은 replay를 실행할 때도 정식 launcher처럼 프로세스 시작 직후 sampler를 시작해야 한다.

게임36688, sampler29008, PDH26696의 종료를 확인했다. 컴퓨터 유즈는 재시작하지 않았다. 최소 테스트 요청에 따라 변경 없는 빌드와 native gate는 반복하지 않았다. 현재144FPS 목표는 달성되지 않았고 실제 foreground 재검증도 남아 있다.


## RT 변형 Dispatch 배치 처리 (2026-10-15)

RayGeometryDeformation이 변형 물체마다 호출하던 Dispatch를, 연속된 같은 작업 그룹 수의 물체끼리 묶었다. X축은 정점, Y축은 물체, Z축은65535그룹보다 큰 메시의 정점 연속 구간이다.65535개 물체마다 분할하며 static/cached-skin 항목을 건너뛴다. 정점/형상/변형 계산, 버퍼 배치, 품질, 자원 수명 및 graph 계약은 그대로다. 새 작업 버퍼나 설정을 추가하지 않았다. renderer_ray_geometry.cpp와 ray_geometry.hlsl의 좌표 계약을 함께 변경했다.

검증: 실제 C++ 루프를 추출한 MSVC 검사에서500개 혼합 사례, 빈 입력,65535개 행 분할, 큰 메시의 Z 연속 구간, 각 정점의 중복/누락 없는 처리를 확인했다. 동일 크기22058개 합성 작업은1Dispatch다(실제 장면의 호출 수로 주장하지 않는다). 저장된 ShaderContracts.test_ray_deformation_dispatch_axes_are_disjoint도 통과했다. 네이티브 rendering.ray-entity-ultra는150.986초/8프레임/validation_errors0/execution_valid=true로 통과했다. receipt verify-20261015T120306505Z-31904.json, 별도 결과 deform-batching-native-result.json.

첫 진단 rt-deform-batched-16ticks는 설정을 LOCALAPPDATA에 넣었지만 smoke는 artifact/Profile을 읽어 Medium/RT off로 실행됐다. 비교 assertion이56설정 불일치를 발견했고, 원본을 보존한 채 제외했다. 이후 rt-deform-batched-ultra-16ticks는 Profile/settings.json을 기준 설정과 hash 비교한 뒤 실행했으며, 실제 기록의56개 requested/effective 옵션이 기준과 모두 일치한다.

아래는 기준 rt-budget-single-scan-16ticks와 새 최고옵션16프레임 진단의 후반11..16 평균이다. 두 실행 모두 외부 sampler ON, FHD100%, UltraRT, FG/low-latency OFF의 숨김 lockstep 진단이다. foreground/144FPS 판정이나 일반 플레이 개선율로 쓰지 않는다.

|구간|이전 ms|이후 ms|
|---|---:|---:|
|GPU RT 변형|3.871|2.034|
|CPU RT 변형 명령 기록|3.181|1.972|
|CPU 전체 렌더|268.874|268.147|
|GPU critical path|168.117|168.795|
|반사 추적|59.082|60.661|
|GI 추적|51.309|50.626|
|동적 BLAS|10.121|11.370|

해당 변형 패스 GPU47.5%, CPU 명령 기록38.0% 감소를 관찰했다. 전체 프레임 개선은 확인되지 않았다. 실행 시점·클럭 차이가 있는 단일 비교다. simulation checksum/56설정 및 최종 depth/GBuffer3종/lighting HDR/composite HDR6개 raw 결과가 모두 동일하다. Runtime3680제품 중 ray_geometry_cs.dxil 및 local_probes.spkg의 digest가 달라졌으므로 전체 제품이 셰이더 하나만 바뀌었다고 주장하지 않는다.

Playing7.329초 구간의 CPU6표본/6.346초는1.709core,20논리CPU 대비8.543%. NVIDIA7표본 장치 전체 GPU56.857%,graphics clock2655MHz(1710..2820),108.424W이며 다른 앱도 포함한다. PID GPU는 PDH초기화7.173초/카운터오류2회/대상관측 누락으로 N/A다. 게임과 sampler exit0, CPU/GPU16/16complete/invalid0이지만 분석은 partial로 유지한다.

공유 그룹 무거운 순서: 반사62.347,GI51.976,ray_scene14.723,AO12.900,그림자9.494,surface5.773,geometry4.452,DOF1.553,lighting1.211,volumetrics1.201ms. 공유·겹치는 GPU작업이므로 합산값은 frame time/단독 옵션 절감량이 아니다. [패스·56옵션 전체 결과](../../../Build/performance-actual/rt-deform-batched-ultra-16ticks/REPORT.md), [비교표](../../../Build/performance-actual/DEFORM-BATCHING-REPORT.md).

Release SHA25639AE256FF66AF1F7753772BDDE8AFB93A2C20510E598EB78E1BC09E523192EE1,
Debug32F00C591DCA5A3DE59324AC2063151CC5925D61157775877AB87BDB9843AE0D.
Runtime generation fe8e660b279bcb32dfa6a472b82c280c4b292c942e7e7d2842a26f8e9d02dcd7의3680제품을 두 빌드에서 검증했다. Native test generation48484c1c. 모든 소유 게임/계측 프로세스가 종료됐다. 컴퓨터 유즈OFF, 최고RT/FG포함144FPS는 미달/미검증이다.


## 단일 광원의 중복 RT 추적 제거와 최종 검증 (2026-10-15)

`RtHitDirectLighting`은 광원이 하나이고 방향광·점광·스폿광이면 같은 입력의 그림자 ray를 직접광 sample 수만큼 반복했다. Ultra의4개 sample에서 첫 평가를 재사용하고 원래 누적식을 반복한다. 광원 선택·분모·누적 횟수, opacity/identity/unsupported confidence 검사는 유지한다. 면광원과 다중 광원의 확률 표본 경로는 바꾸지 않았다. 별도 캐시·리소스·품질 옵션·의존성은 추가하지 않았다.

최종 원본 누적식 버전의16프레임 진단을1회 실행했다. 비교 기준은 기존 `rt-deform-batched-ultra-16ticks`, 최종은 `rt-single-light-accumulate-16ticks`다. 마지막6프레임(11–16),1920×1080 내부100%, 최고RT,FG/low latency Off,56개 설정과 simulation checksum이 일치한다.

|구간|기준 ms|최종 ms|
|---|---:|---:|
|GPU critical path|168.795|117.817|
|RT 반사 추적|60.661|38.647|
|RT GI 추적|50.626|34.393|
|CPU render(대기 포함)|268.147|228.992|
|CPU wait|149.686|97.384|
|CPU graph_build|49.988|57.228|

GPU critical path 관측값은30.2% 감소했다. 그러나 변경하지 않은 AO/BLAS 등도 빨라졌고 CPU graph_build는 느려졌으므로 전체 감소를 이 변경만의 순수 효과로 단정하지 않는다. 강제 lockstep·백그라운드·외부 계측 조건이며 정상 foreground FPS나144FPS 증거가 아니다.

### 출력 비교의 실제 결과

Depth/GBuffer base/metadata/normal/lighting HDR의5개 raw는 byte-identical이다. 최종 composite HDR은2073600픽셀 중28개 성분/28개 픽셀에서 최대1 FP16 ULP(절대0.000244140625) 차이가 난다. 두 출력 모두 비정상 수치(NaN/Inf)가 없다. 앞선 임시 contribution 버전은12개 성분/12개 픽셀에서1ULP였다. 원래 누적식으로 수정해도 byte-identical은 아니므로 반올림 원인을 확정하거나 완전 동등하다고 보고하지 않는다.

분석 광원의 동일 입력 재사용이라는 의미 검토와 정상 native 실행, 관측된 작은 수치 차이를 근거로 최종 버전을 유지했다. 장면 전체·시간축의 시각적 동일성은 미검증이다. exact 비교를 통과로 바꾸거나 실패를 숨기지 않았다. 수치 근거: `Build/performance-actual/single-light-accumulate-hdr-difference.json`; 첫 후보 원본도 보존했다.

### 무거운 순서: 최종 GPU 기능 그룹

|순위|기능 그룹|ms/frame|
|---:|---|---:|
|1|RT 반사|40.083|
|2|RT GI|35.481|
|3|RTAO|9.627|
|4|RT 장면·변형·BLAS/TLAS 공통|9.537|
|5|그림자|7.834|
|6|GBuffer·Surface|4.919|
|7|가시성·Geometry|4.177|
|8|피사계 심도|1.259|
|9|직접 조명|1.241|
|10|볼류메트릭|1.074|

공유 기능·비동기 중첩이 있으므로 합계는 frame time도 각 옵션을 끌 때의 절감량도 아니다. 전체28그룹·56옵션·패스 원자료는 [최종 분석](../../../Build/performance-actual/rt-single-light-accumulate-16ticks/REPORT.md)과 [비교표](../../../Build/performance-actual/SINGLE-LIGHT-ACCUMULATE-REPORT.md)에 있다. 비용 순서는 설명되지만 절대 성능이 목표에 적합하다는 뜻은 아니다. RT 반사/GI와 CPU graph_build/준비가 남은 우선 병목이다.

최종 Playing CPU는6표본/6.471초,2.721코어(20논리CPU 대비13.606%)다. NVIDIA 장치 전체는7표본 평균12.714%,2457.9MHz,87.85W다. PID GPU 사용률은유효 표본0으로 N/A다. 첫 카운터는 시작 약30초 뒤 도착했고 warmup이었다. 이를0%로 기록하거나 장치 전체 수치로 대체하지 않았다.

### 일반 페이싱 재생과 GPU 카운터 준비 시점

이 RT 셰이더 수정 전 `rt-realtime-gpu-ready-480ticks`는 원본 실제 입력의480tick 구간을 일반 runtime 페이싱으로 재생했다.50렌더/40Playing프레임, foreground=0이다. Playing base6.469FPS, SDK 실제 present count34/5.037초=6.750FPS이며 물리 디스플레이 주사율 측정이 아니다. CPU1.088코어(5.441%); PID GPU 유효2표본의27.958%는Playing구간29.238%만 덮으므로 전체 평균으로 사용하지 않는다. 이 재생의GPU critical108.880ms는 장면/페이싱이 다른 위 진단과 직접 비교하지 않는다.

GPU 생성 전에 PDH query를 열어 대상 PID를 놓치는 경우를 줄이도록 기존 sampler에 `-GpuReadyFile`을 연결했다. launcher의 고유 capture 폴더에 이미 생성되는 `renderer_features.jsonl`을 이용한다. PDH worker만 파일의 nonempty 상태를 기다리고 CPU/스레드/NVIDIA 수집은 즉시 시작한다. 프로세스 identity·종료·owner stop·기존 시간제한을 유지한다. 별도 runtime marker나 대기 연장을 추가하지 않았다. 짧은 실행에서 PDH 초기화/warmup 지연 자체를 해결하지는 못하며 누락은 계속 N/A다.

### 최종 검증·빌드

- 원본 누적식 버전: 단일 ShaderContracts 회귀 PASS, native `rendering.ray-entity-ultra`85.6805초/8프레임/validation_errors0/execution_valid=true PASS. Receipt `Build/verification/verify-20261015T123424663Z-11072.json`.
- 첫 contribution 후보 native114.12초 PASS도 별도 보존했으며 최종 검증과 혼합하지 않는다.
- sampler/launcher 자체검사와 PowerShell AST PASS: `Build/performance-actual/tools-gpu-ready-selfcheck-20261015T122829361Z/`.
- Debug/Release runtime generation `4a114a17f3abbf7054161c51896ed79de527e5d6821212686d892e3522a14fbe`,각3680제품 검증 완료. Native test generation `22be090c0d02dc8c451dbb25ae7531751bfd9f81dee70c7cfaf282cf03382cfb`.
- Release SHA256 `39AE256FF66AF1F7753772BDDE8AFB93A2C20510E598EB78E1BC09E523192EE1`, Debug `32F00C591DCA5A3DE59324AC2063151CC5925D61157775877AB87BDB9843AE0D`.
- 최종 game29680/sampler37364/PDH6032 종료 확인. 컴퓨터 유즈OFF 유지.

변경하지 않은 전체 gate·장시간 반복 플레이는 생략했다. 최고RT/FG포함144FPS는 달성하지 못했고 최신 foreground FPS는 미검증이다. 위 변경은 필요한 검사·오류처리를 보존하며 실제 중복 작업만 줄인 범위다.
