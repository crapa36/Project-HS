# VFX·시각 UX 감사 대응

2026-10-11. 입력은 `Project-HS_VFX_Visual_UX_Audit.zip`이다. 첨부 문서의
`AI_DIRECTIVE.md`는 검토 자료로 취급했으며 별도 사용자 명령으로 실행하지 않았다.
첨부 manifest의 SHA-256 여섯 개가 모두 일치했다. 원 보고의 기준은
`b8e9dbb878c8bf1cb0ae609b6143b3ef8dc0ec2a`이며 GPU 실행 없이 만든 합성 probe는
이번 엔진 검증의 증거로 재사용하지 않았다.

## 구현과 판단

| ID | 판단 | 현재 처리와 근거 |
|---|---|---|
| 추가 결함 | GPU 검증에서 발견, 수정 | Volumetric Off에서 null이던 `b0/space24`를 VFX의 `HsFogEnabled`가 읽었다. 실제 flash 16프레임 모두 byte695의 CBV 범위 오류가 났다. frame upload의 기존 12288..13056 빈 영역을 0으로 초기화해 유효한 768B 기본 CBV를 연결하고, On feature가 실제 상수로 덮어쓰도록 했다. 새 GPU 할당·FrameConstants ABI 변경·검증 억제 없이 해결한다. |
| VF01 | 수정 | 현재 틱의 modulo만 검사하던 상태 입자 일정을 실제 현재 episode의 시작·종료·최대 입자 수명으로 제한해 복원한다. renderer가 이미 가진 마지막 성공 제출 checkpoint를 사용하여 준비 실패 재시도, 같은 틱, 보조 뷰에서 cursor 소비·중복을 막는다. 원래 birth tick·sequence·seed·GPU age를 유지하며 틱 역행은 공유 pool/schedule을 초기화한다. 과거 위치와 이미 끝나 snapshot에서 사라진 episode는 만들지 않는다. |
| VF02 | 수정 | 지속 상태 stamp에 owner의 previous/current 위치를 함께 전달하고 renderer의 기존 mesh 보간 인자를 사용한다. 일반 burst/contact는 기본적으로 월드 위치를 유지한다. 새 owner/도탄/skip 처리는 기존 ingress의 유효 endpoint를 따르며 이것을 이전 렌더 모션으로 오해하지 않는다. |
| VF03 | 현행 유지, 연출 정책 조건부 | 현재 불꽃의 운동·정규화 수명은 부모 장판 종료로 수렴한다. authored source에는 독립 child TTL이 없고 현재 부모가 살아 있는 동안 이미 만료된 child도 없다. 수명·alpha·발생률을 임의로 줄이거나 출력 동등하지 않은 culling을 추가하지 않았다. |
| VF04 | 수정 | 현재 HP×전체 경과 시간 대신 직전 관측 HP의 주파수로 tick delta를 적분한다. 같은 틱의 HP 변경·pause·중복 프레임은 위상을 움직이지 않는다. 부재·owner 변경·session reset·틱 역행은 새 위상으로 시작하며 진입 envelope는 9틱 동안 증가한다. HP snapshot은 지연하지 않는다. 카탈로그 교체와 실패 변환은 축적 상태를 보존한다. 미관측 HP 변화를 복원한다는 보장은 없다. |
| VF05 | 수치 조사, 품질 정책 조건부 | 내부 TAA는 일반 VFX의 색 차이, 외부 guide는 추가 OIT coverage를 사용한다. Hero transparency의 별도 depth/motion/history는 유지했다. 실제 GPU의 fixed-exposure VFX-on/off 동일 프레임을 비교하는 진단을 추가했다. 수치 누적률을 자동으로 품질 결함으로 간주하거나 전체 reactive/history/depth 정책을 바꾸지 않았다. |
| VF06 | 수치 조사, 측광 정책 조건부 | 합성 결과가 실제 histogram 입력이며 기존 percentile·적응 보호가 있다. 같은 효과의 fixed/auto exposure, 원본 histogram, 효과 밖 ROI를 수집하는 진단을 추가했다. 장식과 실제 월드 조명을 구별할 근거 없이 측광 입력을 opaque-only로 교체하지 않았다. |
| VF07 | 현행 유지, 가독성 조건부 | 실제 위험 geometry와 `[warning_started, execute_tick)` 수명이 Presentation owner와 typed ground로 연결되고 취소된 owner는 제거된다. Gameplay의 시계방향 degree와 Presentation의 반대 radians도 일치한다. reverse-Z depth 검사·fwidth AA·fog, OIT/distortion 뒤 additive 경계 순서를 유지했다. 현재 연결 결함은 확인되지 않았으며 foliage·발광·중첩의 가독성 합격을 주장하지 않는다. |
| UX01 | 수정 | 캐릭터 스킬창의 Q/W/E/R 상수를 HUD와 같은 실제 binding label로 교체했다. 재지정·빈 슬롯·기능 키·loadout 변경 표시를 회귀에 포함했다. |
| UX02 | 수정 | 기존 캐릭터 상세 패널·이전/다음 패턴으로 강화와 유물을 각각 4개씩 표시하고 본문·기여 영역을 넓혔다. 본문 font18과 전체 텍스트를 사용하며 기존 슬롯·탭·닫기 좌표를 유지한다. 추가로 실제 글자 폭은 맞지만 줄 끝 공백 때문에 불필요하게 축소·말줄임하는 공통 원인을 고쳤다. 단순 폭 확대는 원복했고 최대 피해 표시의 실제 높이 부족만 23→24로 수정했다. 새 UI framework나 전역 글자 배율은 추가하지 않았다. |
| UX03 | 현행 유지 | 모든 클릭 복귀는 현재 안내와 일치하며 진입 전 공격 입력으로 즉시 닫히는 결함은 재현되지 않았다. 실제 입력 경계 결함 없이 버튼처럼 보이기만 하는 장식이나 새로운 보류 정책을 추가하지 않았다. |
| UX04 | 현행 유지 | 요청/적용 상태, 후속 변경, 실패 복원, 디스플레이 15초 확인은 이미 존재한다. 적용 비용의 추가 증거 없이 draft/apply 체계로 재작성하지 않았다. |
| UX05 | 국소 수정 | 선택 가능한 다음 값이 없는 설정은 무시하고, 음량·overlay 강도의 끝단은 기존 disabled 표현과 no-op 입력을 일치시켰다. 전체 키보드 메뉴 탐색은 현재 미구현이며 이번 국소 수정으로 구현했다고 보고하지 않는다. |
| UX06 | 현행 유지, 접근성 정책 조건부 | overlay 강도 0 경로와 필수 HUD·월드 경고를 구별했다. 모든 장식 효과를 제어하거나 HDR/SDR 감광성 기준을 만족한다는 주장은 하지 않는다. |
| GR01 | 기존 수정 유지 | `Docs/Grass_History_Correction.md`의 per-view 성공 렌더 이력, 이전 변형 상태·원점·세대 lease 계약과 기존 네이티브 근거를 재사용한다. confidence 강제나 중복 수정은 하지 않았다. |

## 검증 범위

Release·validation의 `ProjectHS`와 렌더 실행기 빌드가 통과했다.
최초 CPU 검사 7개 통과 결과(`verify-20261010T174908530Z-21232`)와
최종 한글 텍스트·Presentation 검사 2개 통과 결과
(`verify-20261010T180512949Z-25400`)를 합쳐 중복 제외 8개 항목을 확인했다.
최초 텍스트 실패는 통과로 덮지 않고 원본 receipt를 보존했다.
validation의 runtime-full/test-render와 Release runtime-full 세대도 검증했다.
Release는 `HS_BUILD_TESTS=OFF`여서 test-render 세대·생성 타깃이 없으며,
잘못 요청했던 추가 세대 검사 실패 로그도 보존했다. GPU 검사는 validation 구성이다.

진단 `--vfx-composition-probe=fixed|auto|control|switch`는 내부 TAA·SDR·native extent의
RadialEnergy를 32개의 논리 60Hz 프레임으로 렌더한다. 8프레임 배경 이후 약한
효과, 위치 변경, 큰 효과, 제거를 순서대로 사용한다. fixed/control은 최초 view
초기화 이후 exposure 적응 delta를 0으로 고정하고 auto는 1/60초를 사용한다.
switch는 실제 설정 요청으로 frame13 Medium→frame21 Off를 적용하고 active 값을
확인한다. 이는 실시간 144FPS나 전체 VFX/외부 SDK 품질 검증이 아니다. bloom을 끈 것은
원인 분리를 위한 진단 설정이며 제품 기능을 제거한 것이 아니다.

수치 비교의 배경·고정 노출·자동 노출 실행은 각각 32프레임을 완료했고 validation
오류는 0이었다. 자동 노출은 GBV도 켰다. GBV의 cold pipeline 준비와 readback을
포함한 실행 시간은 게임 FPS로 환산하지 않는다. 최초 switch 실행은 native 옵션으로
초기화한 fixture에 설정 승인 상태가 있다고 잘못 가정해 첫 프레임에서 중단됐다.
생산 렌더 실패와 구분했다. 초기 승인 요청을 명시한 switch 재실행은 32프레임을
완료했고 **GBV/validation 오류 0**이다. frame1 generation0 Off,
frame13 generation1 Medium, frame21 generation2 Off가 실제 적용·요청값과 모두
일치했다. 모든 프레임에서 이를 검사하고 Off 이후 frame24/25/32도 캡처해 뱅크
재사용을 거쳤다. 따라서 최종 native 실행 4개는 각각 32프레임·오류0이며,
그중 auto와 switch가 GBV를 사용했다.
환경은 NVIDIA GeForce RTX 4080, Windows 드라이버32.0.16.1742,
640×360, indexed geometry, compact surface, enhanced barriers다.
현재 하드웨어 조회는 `gpu-environment.json`에 기록했다.

실제 raw GPU 결과의 VF05/06 관측은 다음과 같다.

- VF05: 동일 프레임·동일 opaque/depth/jitter/exposure의 VFX-on/off 차이를 비교했다.
  저대비 신규 효과의 안정된 opaque 위 표본 375개에서 현재 신호 비율의 p10/p50/p90은
  0.1290/0.2736/1.0001이었다. TAA 필터와 히스토리가 포함된 값이며 품질 합격 기준은
  아니다. profile의 시간 성격·대응 identity 없이 전체 reactive를 높이지 않았다.
- VF06: fixed exposure는 1.0371383으로 유지됐다. auto의 frame8은 1.0370313,
  큰 효과 제거 직후 frame25는 0.8133453(-21.5698%), frame32는 0.8722879(-15.8861%)였다.
  모든 관측 histogram의 population은 230400(640×360)이었다. 합성 효과가 측광에
  영향을 준다는 실제 근거이며, 배경만 측광하는 정책으로 바꿀 근거까지 제공하지 않는다.
- frame17·24의 큰 효과는 화면 230400픽셀 전체를 덮었다. 효과 밖 배경 ROI가 없어
  그 두 프레임의 배경 전용 밝기 비교는 **판정 불가**다. 최초 분석의 빈 ROI 실패도
  보존하고 결과에는 null과 이유를 기록한다. 판정 불가를 품질 통과로 취급하지 않는다.

검증 로그는 `Build/verification/vfx-ux-final-batch-v2.log`,
`vfx-ux-finalize-v2.log`, `vfx-ux-generations.log`, `vfx-switch-build.log`,
`vfx-switch-native.log`에 있다.
실행 인자·binary SHA-256·성공/실패 receipt는
`Build/reviews/vfx-visual-ux-20261011/native-runs-before-switch-repair.json`,
수정 switch의 별도 실행은 `native-switch-v2.json`, 최종 수치 결과는
`composition-results.json`에 있다. 비교 3개 실행기의 SHA-256은
`D56B4E593E08886C1FFE00A86E4188A16D31F33C1EC24002492C97EF71C6FE3E`,
수정 switch 실행기는
`0DBD052B8D6837ABA92D3C038356DD58A50A30A207D8A5173ABF094F3DCC3FCD`이다.
생산 코드는 둘 사이에 바뀌지 않았고 switch 초기화·진단 기록만 수정했다.
소스 24개·실행 파일 4개·선택 세대의 해시는 같은 디렉터리의
`source-provenance.json`, HEAD 기준 해당 tracked 경로의 diff는 `working-tree.patch`에 있다.
앞선 3개 비교 실행 시점의 해시는 `control-fixed-auto-source-provenance.json`에 별도 보존했다.
원본 raw 결과는 `Build/msvc-validation/Artifacts/vfx-ux-cbv-fixed-*`에 보존한다.
새 보고 문서는 tracked diff에 포함되지 않는 미커밋 파일로 별도 보존한다.

VF03의 authored motion 근거는 `ContentSource/VFX/production_spec.v4.json`의
`+Y 상승 후 안쪽으로 휘며`라는 flame 출력이다. 독립 TTL은 정의되지 않는다.

텍스트 CPU 검사는 배포 NotoSansKR와 DWrite로 실제 설명과 최대 기여 수치를
측정한다. logical 1080p/720p 변환 후 추가 축소·잘림 없이 공간에 맞는지 검사하며,
실제 픽셀의 가독성이나 200% 확대 지원을 인증하지 않는다. 실패했던 11문구는
원래 폭에서도 visible width/height 초과가 0이었다. 5.04px의 줄 끝 공백 때문에
예를 들어 실제 폭353.322가 trailing 포함358.362로 집계되었다.
[DirectWrite의 두 폭 정의](https://learn.microsoft.com/en-us/windows/win32/api/dwrite/ns-dwrite-dwrite_text_metrics)에
맞춰 renderer의 축소·말줄임과 CPU 검사 모두 공백을 제외한 `metrics.width`로
판단한다. 허용 폭·높이를 완화해 통과시키는 변경은 아니다.

화면을 새로 열어 검사하거나 이미지로 재검수하지 않았다. 원래 화면 검증 횟수
제한을 유지하며 기존 최종 캡처를 새 변경의 시각 검증으로 재사용하지 않는다.

## 참고 기준과 남은 한계

[AMD FSR guide](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-upscaler/)는
alpha blended 출력의 reactive/coverage와 exposure 계약을 설명한다. 이것을 내부
TAA의 획일적인 reactive 값으로 옮기거나 실행하지 않은 SDK 소비 경로의 증거로
취급하지 않는다. 텍스트·디지털 탐색·시각 자극의 검토 기준은 Microsoft의
[XAG101](https://learn.microsoft.com/en-us/xbox/accessibility/xbox-accessibility-guidelines/101),
[XAG112](https://learn.microsoft.com/en-us/xbox/accessibility/xbox-accessibility-guidelines/112),
[XAG117](https://learn.microsoft.com/en-us/xbox/accessibility/xbox-accessibility-guidelines/117)을 참고했다.

RTX4080 FHD 최고 설정 144FPS는 아직 달성하지 못했다. 기존 FHD GBV fixture의
단일 픽셀 불일치는 이전/이후 모두 남아 있으며 이번 결과로 통과 처리하지 않는다.
RT·외부 SDK 전용 경로와 모든 전투 상황의 가독성·감광성은 아직 검증되지 않았다.
원본 브랜치·커밋·index를 보존하고 main 작업 트리에 미커밋으로 남긴다.
