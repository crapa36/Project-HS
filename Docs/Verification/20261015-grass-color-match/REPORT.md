# 잔디와 풀 지면 색 맞춤

이전 보정은 텍스처 평균색만 맞췄다. 실제 주간 장면에서는 위를 향한 지면의 직사광과 하늘 반사가 잔디 카드보다 강해 밝고 회색으로 보였다. 이번에는 풀 coverage에만 RGB 보정, 간접광 차폐(AO), 무광 roughness 보정을 함께 적용했다. 전체 조명/노출/UI/잔디 메시는 변경하지 않았다.

`TerrainGrassPalette`의 canopy gain은 (0.5,0.72,0.08), 풀 coverage에서 AO 배율은 0.1, roughness는 원본에서 1.0 방향으로 80% 혼합한다. 원본 색 대비, 법선, 4층 높이 혼합, POM, 2K 텍스처/밉, 12m/9m 매핑은 유지한다. 순수 흙길/mesh20은 보정 제외이며 경계는 연속 혼합한다. 재질의 주간 외관 조정이며 새 감마 오류를 수정한 것은 아니다.

## 실제 캡처 비교

같은 16틱/1280×720/56설정, ROI (900,160)-(1240,600). GBuffer ID로 잔디 25,047픽셀과 지면 124,553픽셀을 분리했다. 표는 노출·톤매핑 전 composite HDR 평균이다.

| 대상 | 이전 RGB | 최종 RGB |
|---|---|---|
| 잔디 |0.05033, 0.06873, 0.01997|0.05026, 0.06863, 0.01994|
| 풀 지면 |0.15165, 0.18939, 0.10549|0.04982, 0.06622, 0.02077|

지면/잔디 평균 휘도비 **2.860 → 0.971**. 지면의 과밝음과 회청색을 크게 줄였다. RGB 성분 차이는 최종 평균 약 0.9%/3.5%/4.2%다. 모든 픽셀이 동일한 색이라는 의미는 아니며 그림자·잎 방향·원본 무늬는 남는다. 최종 PNG 직접 확인 완료.

## 검증

- shader/source 회귀 27/27 통과(원래 1,000 표본 및 4층 혼합 보존 조건 유지).
- Debug/Release 빌드 및 runtime-full 3,680제품씩 검증 성공.
- 최종 숨김 native 16프레임, 27.82초, exit0, execution_valid/assertions_passed, D3D12 validation 오류0.
- 동일 56설정 및 checksum 4930774313638502297; 전체 GBuffer normal/metadata byte-exact.
- 흙길 중앙 (640,500) base/material byte-exact. 최초 (580,500) 검사는 AO 241→240으로 실패했다. 이 위치는 혼합 경계이므로 그대로 기록하고 중앙 표본과 순수 길 coverage 산술 계약을 별도로 확인했다.
- ImGui 제목막대 (41,74,122) 유지. 지형 5맵 2048²/12밉, physical/resident mip0 확인.
- 검증프로세스 종료. Computer Use 재개 없음.

이번 검증은 TextureUltra/native100%/AF16/InternalTAAU 및 기존 Medium/raster 프로필이다. 최고 RT 전체·HDR 화면·다른 조명·장시간 플레이·144FPS 검증은 반복하지 않았다. 현 주간 조명에 대한 외관 보정이므로 다른 조명에서 같은 평균색을 보장하지 않는다.

## 근거

- `Build/grass-match-fix/comparison.json`, `compare.py`: 원본/중간/최종 정량 비교와 보존 검사.
- `Build/grass-match-fix/matched/final-debug/`: 최종 PNG/raw/설정/result/render/run/generation 기록.
- `Build/grass-match-fix/matched/`: 최종 빌드/회귀 로그. 이전 후보들은 삭제하지 않았다.

Runtime generation: `ae3db3a1e2322cd3fb3c4b9b962132429fc955c598c109997871f3e01a80f6ca`

Debug SHA256: `0DE782B5F468828328BD7BC3AC5623A98360975530DE953767F5ED02CDB0CFF2`

Release SHA256: `FE344E8A18D8329203C873AD2D5F7B2A44C9C56BF222C348F8EB61CB43B24F83`
