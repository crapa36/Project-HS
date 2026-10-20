# 지형 매핑·색·선명도 수정

## 확인한 원인

1. 원본과 cooked/runtime 지형 맵은 실제 **2048×2048, 4층, 12밉**이다. 색용 Gaussian은 BC7 linear, surface/normal/detail은 BC5, height는 BC4다. histogram은 색 복원 LUT이므로 256×16·1밉인 것이 정상이다.
2. 원본에는 흙 알갱이·작은 잎 디테일이 있지만, 6m(자갈4.5m) 단위로 배치되어 플레이 화면에서 작게 축소된다. 이때 정상적인 밉 필터링과 TAA가 작은 무늬를 평균화한다. 2K 원본이 화면에서 항상 원본 픽셀 단위로 보인다는 뜻은 아니다.
3. 이전 풀 보정이 RGB를 휘도 하나로 줄인 뒤 녹색 하나를 곱해, 원본 흙·잎의 색 대비를 제거했다. 이번 수정에서 폐기했다.
4. Gaussian 샘플은 실제 resident mip로 제한되는데 inverse histogram LUT는 ideal gradient mip만 사용했다. 스트리밍 초기·품질 제한에서 둘이 달라져 색/대비가 잘못 복원될 수 있다. 이것은 항상 밝아지는 오류는 아니다. 실제 grass-soil LUT 중앙값은 mip0=(.1384,.1242,.0387), mip4=(.1615,.1462,.0461)로 방향이 조건에 따라 다르다.
5. 바닥 반사색만 낮추면 자동 노출이 다시 끌어올린다. 수정 중 최고 옵션 캡처에서는 노출1.125가 목표1.250을 향해 상승 중이었다. 장면 노출 목표까지 바꿔 재밝아짐을 해결했다.

## 적용

- 지형 반복 크기: 흙/풀/마른 흙 **6→12m**, 자갈 **4.5→9m**. 텍스처 무늬가 화면에서 약2배 커진다. software mip demand의 최소 tile도9m로 맞췄다. POM/normal/detail도 같은 좌표·gradient 계약을 사용한다.
- 풀 색은 원본 RGB 각각에 보정 비율을 곱한다. 잔디 atlas 평균색을 기준으로 하되 흙·잎 색 차이를 보존한다. 순수 길/mesh20은 풀 색 보정에서 제외한다.
- 지형 albedo에0.6을 적용해 주간 조명에서 씻긴 듯한 밝기를 낮췄다. 4층/높이 혼합·normal·roughness·AO는 유지했다.
- 장면 노출 목표 **0.18→0.12**. 자동 노출 적응 후에도 낮춘 밝기를 유지한다. SDR/HDR 장면의 pre-exposure 목표에 적용되며 UI에는 적용되지 않는다. 기존 SDR 톤매핑 교정은 유지한다.
- inverse LUT mip는 Gaussian의 resident minimum보다 낮아지지 않도록 수정했다. anisotropic 하드웨어의 정확한 유효 LOD와 수동 추정이 완전히 같다고 주장하지 않는다.
- 기존 surface.json 진단에 terrain_texture_mips를 추가했다. slot별 논리 해상도/밉 수/물리 첫 밉/resident 최소 밉을 캡처 시점에 기록한다. 매 프레임 기록 비용은 추가하지 않았다.

## 검증

- source/산술 회귀 **27/27 통과**. RGB 색 구분/흙 보호/명암 비례, CPU↔shader tile 계약, resident mip 복원 및 노출 목표를 확인했다.
- Debug·Release 빌드 및 각각 runtime-full **3,680 제품 검증** 성공.
- RTX4080,1280×720/native100%,텍스처Ultra,AF16,InternalTAAU,일반 D3D12 validation. 최종16프레임25.77초,exit0,execution_valid/assertions_passed=true,오류0,진단완료.
- 최종 지형5종 모두 logical2048²,12mips,physical_first_mip=resident_min_mip=0. LUT는256×16,mip0.
- 노출 조정 전후 Ultra16프레임의56설정·시뮬레이션 checksum4930774313638502297 일치, GBuffer base raw byte-exact. 따라서 마지막 노출 조정이 재질 디테일을 변경하지 않음도 확인했다.
- 같은16프레임에서 Medium(mip1상주)과Ultra(mip0상주)를 비교한 흙ROI(580,400)-(680,560)의 GBuffer RGB 차이는0이었다. 이 거리·영역에서는 원본 해상도 부족이 주원인이 아니다. 전체 장면/거리로 일반화하지 않는다.
- 최종 PNG 직접 확인: 흙 무늬 확대, 풀 원본 색 차이 복원, 전체 지면의 과밝음 감소. ImGui 제목막대(41,74,122) 유지.

| 같은 위치 RGB8 | 이전 완료본(3프레임 Medium) | 이번 최종(16프레임 Ultra) |
|---|---|---|
| 흙길(580,500) |150,129,114|125,104,92|
| 풀 바닥(800,400)|138,166,97|102,113,83|
| ImGui(450,70)|41,74,122|41,74,122|

처음과 마지막은 밉 품질·틱·UV·노출이 함께 달라진 시각 비교이며, 개별 변경의 기여율 측정이 아니다.
최종 captured histogram으로 계산한 노출 목표는0.8333,현재0.9557이므로 같은 장면에서는 추가 적응 시 더 어두워지는 방향이다. 장시간 플레이 대신 동일 히스토그램의 수렴점을 계산했다.

## TAA와 검증 범위

이전3틱 캡처에서는 흙ROI의 TAA 이력 혼합이 현재 프레임 de-jitter 이후에도 고주파 대비를 약40%,풀ROI는약14% 줄였다. 밝기 평균은약1% 이내 변화였다. 이는 해당 캡처의 필터링 측정이며 모든 대비 감소를 결함으로 단정하지 않는다. FXAA는TAA활성경로에서 center를반환하고,DOF/모션블러는꺼져있으며,공간upscale도없었다. TAA와AF16을 유지하고 무늬 크기를 키워 필터링 후에도 읽히게 했다. RT노이즈·움직임 부작용을 확인하지 않은 TAA 전역 약화는 하지 않았다.

처음 준비한 테스트용 설정은 필수 음량/키 필드가 없어 런타임이 격리했고 Medium으로실행됐다. `final-debug`는Medium 검증만유효하며,최고옵션근거에서제외했다. 완전한 격리 문서를 사용한 `ultra-debug`와`exposure-final`에서 실제Ultra/mip0를확인했다. 사용자 실제설정은변경하지않았다.

Computer Use 미사용, 소유테스트프로세스모두종료. 장시간/최고RT전체설정/실제HDR화면/완료된GPU-based validation과144FPS는이번검증범위가아니다. 이전GBV시간초과는별도보고서에남아있다.

## 산출물

`Build/terrain-detail-fix/`:

- `exposure-final/`: 최종PNG,표면raw,밉메타데이터,설정/CPU/GPU패스기록,result/render/diagnostics,binary/generation/run
- `final-verification.json`, `texture-quality-comparison.json`, `exposure-adaptation-check.json`
- `final-shader-contracts.log`, `exposure-build.log`, `exposure-msvc-{debug,release}.log`
- `initial-profile-rejection.json`, `final-debug/`, `ultra-debug/`: 중간실행및제외사유보존

Runtime generation: `976c8ecfbb67fee641589d8bcd0b51789bd4156350cab80606b74c34cf4836e6`

Debug SHA256: `0DE782B5F468828328BD7BC3AC5623A98360975530DE953767F5ED02CDB0CFF2`

Release SHA256: `FE344E8A18D8329203C873AD2D5F7B2A44C9C56BF222C348F8EB61CB43B24F83`
