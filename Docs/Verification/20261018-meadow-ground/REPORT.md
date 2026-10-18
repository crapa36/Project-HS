# 지면 표현과 세분화 개선 — 2026-10-18

현재: 1차 흙색/도로 수정은 Validation·Release·Debug 빌드와 native180프레임 검증 완료. 2차 지형·배치의 빌드 및 기본/근접 native 검증 완료. 참조 이미지 품질 달성은 미완료.

## 표현 수정

- soil coverage에만 갈색 reflectance gain(.58,.43,.30)을 적용한다. 기존 텍스처 대비와 잔디 팔레트, 노출/UI를 유지한다. 외부 게임에서 추출한 수치가 아니라 HS 후보값이다.
- 수레의 기존 곡선과 같은 중심선/지선 좌표로 1.15–1.65m 반폭의 마모된 길을 연결한다. 바퀴 중심 ±.82m는 재사용 warp의 최대 경계 변동에서도 길 안에 있다.
- 기존 실제 바퀴 홈/발자국/발굽/웅덩이 높이와 물고임을 유지하며 바퀴 마모 색을 추가로 눌렀다. 실제 패임을 단순 색 줄로 대체하지 않았다.
- macrocell 경계와 수면 geometry를 그대로 보존하면서 빈 영역 세분화만 줄였다. 재질의 작은 입자/POM과 실제 기하의 큰 패임을 분리한다.

## 메시 결과

| ground LOD | 이전 | 후보 | 원본 표본 대비 최대 높이 차이 |
|---|---:|---:|---:|
| 0 | 100370 | 68342 | .00370mm |
| 1 | 100370 | 66798 | .01465mm |
| 2 | 100370 | 66654 | .02545mm |

LOD0도31.91%감소한다. 원본 정점/삼각형 중심161340점을 각LOD에 대조한 수치이며 연속 최대오차의 수학적 증명은 아니다. 젖은 보수영역118cell/40592triangles와 수면2621triangles의 원본 속성은 그대로다. 높이 함수/기록/CPU·HLSL테이블은 바뀌지 않았다. 수면LOD3개는 독립 선택시 수면접합을 보존하기 위해 동일하게 유지했다.

Tests/check_meadow_surface.py: 원본은 감소조건에서 예상 실패, 후보는 면적576m²/내부변2·외곽변1/경계192점 위치·법선/물 깊이·방향·종류 검증 통과. Tests/check_shader_reconstruction.py 35개 통과.

## 성능 근거의 한계

Build/play-captures/20261018-meadow-lod-before: Playing596모두foreground, base26.464FPS/SDK52.960FPS, 게임/샘플러exit0, timing1288/1288완료. PDH분석partial이며 생성 작업이 마지막약20초와 겹쳤다. 따라서 통제된 성능 기준으로 쓰지 않는다. 원본 기록은 보존했다. 또한 사용자 후속 요청으로 재질/길 표현까지 바뀌므로 이후 캡처는 메시 단독 A/B가 아니다. 31.9%는 삼각형 감소율이며 FPS 향상률이 아니다.

참고 제작 자료와 접근 제한은 [REFERENCES.md](REFERENCES.md). AAA 품질 달성이나144FPS 달성을 주장하지 않는다.

## 2차: 사용자 이미지와 공개 엔진 지형 비교 반영

- 중앙 1.15–1.65m / 외곽 2.8–5.8m의 서로 다른 반폭 계산을 제거했다. 생성된 C++/HLSL MeadowRoadCoverage가 전체 길을 정의한다. 샛길 시작도 x1.4–2.2m로 옮겨 본길과 겹쳐 합류한다.
- 소품·풀 제외에 남아 있던 5.8/4.8m 규칙을 공통 도로 정의로 교체했다. 씨드 결정성, 충돌 여유, 인스턴스 상한을 유지하며 군집/소품 주변 배치를 수정했다.
- shader reconstruction36개 통과. presentation-contract와 scene-hlod-cooker 2/2 통과: Build/verification/verify-20261018T022824684Z-29084.json.
- 새 홈/융기 형상은 생성 및 검증 진행 중. 위 표와 원본 동일성 수치는 1차 빈영역 최적화 증거이며 새 형상의 수치로 사용하지 않는다.
- 1차 native 캡처: 7a7965d29ca40b16087ee378ec27afd1c2f546d8e243b431a9b97dd072260ba5,180frames,validation_errors0. 2차 입력을 검증한 결과는 아니다.
- 실제 공개 코드 비교: [TERRAIN_ENGINE_COMPARISON.md](TERRAIN_ENGINE_COMPARISON.md). Blender MCP 연결은 실패했으며 이번 지형은 기존 생성기로 제작한다.

## 2차 최종 형상 검증

홈 반폭19–22cm/깊이6.1–7.8cm, 양측융기약2–2.5cm. 동일 중심곡선과 1.64m 바퀴 간격을 유지한다. 물은 새 형상과 공통 평면 수위로 다시 클립했다.

|LOD|최종 ground triangles|미세 원본 후보 대비 최대 높이 차이|
|---|---:|---:|
|0|122944|3.70µm|
|1|108422|14.70µm|
|2|106078|25.55µm|

각LOD310790표본으로 기존100µm 형상 보존 기준 통과. 새 형상과 동일한 미세 후보를 비교한 값이며, 1차 납작한 홈과 동일하다는 뜻은 아니다. 분석 높이 함수와 실제 LOD0 삼각형 중심 접지 오차는 별도로 최대8.98mm다. 상세: Build/meadow-rut-shaping/README.md.

수면5843삼각형. float32 변환 후 퇴화하는 클리핑 조각3개를 생성기에서 제외해 콘텐츠 쿠커와 같은 기준을 적용했다. 실제 hs_environment --meshes 재실행 통과. 외곽192점/면적576m²/내부접합/물 경계/모든LOD 수면 동일성 검사 통과.

최고LOD는 1차 최적화68342 대비79.9% 증가, 최초100370 대비22.5% 증가했다. 추가 형상 품질의 비용이며 FPS 향상으로 보고하지 않는다. 195636삼각형의 중간 후보는 채택하지 않았다. GPU 테셀레이션은 도입하지 않았으며 오프라인 세분화 방식이다.

## 최종 통합 결과

Validation/Release/Debug ProjectHS 빌드 완료. Generation81b09f798e527c4e487a4b00366d3d99623131d74ce4bd99b36b2072c3ddf007.

- 기본180프레임/resize: 실행·assertions 통과,validation_errors0. Build/msvc-validation/Artifacts/render-baseline/6f2b10bed3eba849759b4387a43d31a89586eeb86f349e1ff339f3957e7246a3/.
- 근접40% zoom,16프레임: 실행·assertions 통과,validation_errors0. Build/pass-optimization-20261018/meadow-close-native/.
- 최종 C++ 변경 포함 presentation-contract/scene-hlod-cooker2/2 통과(5.23초), receipt verify-20261018T024509581Z-26056.json.
- 두 실제 캡처를 직접 확인: 길 폭의 급변과 샛길 단절이 없어졌고, 가까운 화면에서 바퀴 홈·융기가 읽힌다. 기본 플레이 카메라는 변경하지 않았다.

남은 시각 차이: 참조 대비 식생 부피/군집 연결이 약하고, 물고임은 밝은 띠처럼 읽힌다. 첨부 이미지 품질 도달로 판정하지 않는다. 근접 캡처도 PBR/재질·자산 전체 품질을 인증하지 않는다. 이번 변경의 GPU/FPS 성능 재측정은 하지 않았으며 앞서 CPU작업이 겹친 기준 캡처로 향상률을 만들지 않는다.

Release/Debug 최종 Generation snapshot 내보내기와 무결성 검증 모두 통과. 컴퓨터 유즈는 이번 통합 검증에서 사용하지 않았고 테스트 게임은 종료됐다.
