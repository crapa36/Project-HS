# 환경 메쉬 중복 정점 제거 — 2026-10-18

LFS는 사용하지 않았다. 큰 파일은 FBX 자체가 아니라 삼각형마다 13성분 정점을 반복 저장한 JSON이었다. 같은 정점을 인덱스로 공유하도록 변경했다.

|실제 입력|저장 정점 전→후|파일 바이트 전→후|
|---|---:|---:|
|environment_meshes.json, 84 meshes|682512 → 267061|145007856 → 59680125|
|meadow_surface_meshes.json, 6 meshes|1064919 → 180428|108392164 → 24467775|
|MeadowProps/runtime_records.json, 위 소품의 authoring copy|208122 → 102704|44599516 → 22805246|

실제 런타임 원본 90메쉬의 중복 저장 정점 1,299,942개를 제거했다. 좌표뿐 아니라 normal/UV/tangent/material을 포함한 13성분 binary64 값이 같을 때만 합쳤다. Signed zero와 원본 정밀도를 보존했고, 각 mesh의 인덱스 확장 결과와 삼각형 순서가 원본과 동일하다. 남은 저장 정점은 모두 참조되며 같은 전체 레코드가 중복되지 않는다.

쿠커는 기존 v1 triangle list와 v1 vertices+indices를 모두 읽는다. 인덱스 정수성·범위·개수, 정점과 재질, 기존 degenerate/normal/tangent/foliage UV 검사를 유지한다. 중간 meshbin ABI는 그대로다. 5개 authoring writer를 동일 helper에 연결해 재생성 때 중복이 되살아나지 않게 했다.

원래 production producer와 변경 후 producer로 실제 90개 meshbin을 각각 생성했다. 모두 전체 바이트 SHA256과 크기가 같다. Legacy/indexed/mixed 형식 및 malformed 입력 C++ 회귀가 통과했다. Meadow의 물 경계·LOD·면적·높이 및 helper signed-zero/정밀도 회귀도 통과했다. 이전 FBX export proof는 전체 원본값 동일성 근거로 재사용했고, 새 Blender export를 실행했다고 주장하지 않는다.

과거 원본 스냅샷 100408352바이트는 현재 런타임 입력이 아니다. 바이트를 그대로 보존하여 `Build/asset-authoring-backups/meadow-props/`로 옮겼고, 등록 manifest와 향후 authoring checkpoint 위치를 맞췄다. 원본 및 이번 변경 전 입력은 로컬 Build에 보존한다.

GPU geometry cooker는 기존에도 76바이트 전체 정점을 weld했다. 이번 변경은 소스 저장량·파싱·저장소 크기 개선이다. 삼각형 수나 렌더 품질은 바꾸지 않았고 FPS 향상으로 주장하지 않는다. 실제 지형의 원거리 topology 단순화는 별도 작업이며 이번에 수행하지 않았다. 동일한 런타임 meshbin 결과에 대해서는 기존 native 결과를 재사용하고 GPU 측정을 반복하지 않았다.

근거: `source-indexing.json`, `cooked-identity.json`; 추가 로컬 검증은 `Build/20261018-mesh-dedup/consumer-tests.log`, `meadow-source-check.log`에 있다.