# 최종 01 → 03 foreground A/B

**144 FPS에 미달하며, 기준 01 대비 명확한 성능 향상을 입증하지 못했다.** 단일 측정에서 기본 FPS는 +0.617%, GPU critical path 시간은 -0.575%였다. 반복 측정에 따른 분산과 신뢰구간은 없다. 02에서 악화됐던 GBuffer 시간은 terrain 준비 공유 철회 후 기준 수준으로 돌아왔다.

## 동일성과 비교 한계

- foreground Playing, tick 599–1380으로 필터링했다. 실제 샘플 범위는 01: tick 599–1380 / 378 frames, 03: tick 599–1379 / 380 frames다. 정확히 같은 tick마다 렌더한 일대일 대응 표본은 아니다.
- 52개 옵션의 requested와 native_effective는 모두 같다. 정책 effective의 FG 배율은 01에서 1, 03에서 2로 다르다. native_effective는 양쪽 모두 2다. 표시 차이의 정확한 기록 시점 원인은 이 CSV만으로 확정하지 않았다.
- 원본 replay 경로, 기록된 inputs SHA256, simulation metadata, 최종 checksum **14666118289919368357**이 같다.
- 실행 파일은 VSM CPU 변경 때문에 다르다. 01 SHA: `32B3DA0FE3AF87934A54B9A465E6145CC98016B058CE87465EA2721D454D5E09`. 03 SHA: `345D12EE6070D780F09F844FA0CD1A0337B44B462DCB114557A1AB3CFDACFCB7`.
- Generation은 01 `81b09f798e527c4e487a4b00366d3d99623131d74ce4bd99b36b2072c3ddf007`, 03 `dad74bbf3d350c55d400ac36767e72e232c02de1320a11c86b6797188f62dc91`이다. 제품 digest 변경은 AO 관련 3개 DXIL, cluster_emit, virtual_shadow_feedback, `render/lighting/local_probes.spkg`다. mesh/texture digest는 같다. Probe는 digest 변경만 확인했고 내부 payload 차이는 검증하지 않았다.
- 여러 변경을 함께 적용했으므로 개별 변경의 인과관계를 확정하지 않는다.

## 기록 완전성

원본 capture-session의 incomplete / foreground_verified:false는 수정하지 않았다. Runtime diagnostics는 complete이며 GPU 1378/1378 frames, drain 성공, invalid 0, drop 0이다. Matched 380프레임 모두 GPU와 critical path 기록이 있다. PDH는 오류 1건으로 부분 자료이며 유효한 구간만 사용했다. GPU oracle/GBV는 본체의 별도 검증 증거다.

|항목|01 matched|03 matched|
|---|---:|---:|
|기본 FPS|28.978|29.157|
|SDK present FPS|57.956|58.305|
|GPU critical path ms|31.652|31.470|
|CPU machine % (20 logical processors)|7.955|3.857|
|PID GPU busiest engine %|61.450|61.755|
|PDH coverage %|86.474|94.459|
|NVIDIA whole GPU %|94.462|95.385|

SDK 원시 valid observation의 actual present count는 01의 378건, 03의 380건 모두 2다. 전환 구간 1건씩을 제외하면 01은 377 queries / 754 presents / 13.009784초, 03은 379 queries / 758 presents / 13.000496초다. Invalid는 모두 0이다. 물리 모니터 표시 주기까지 검증한 값은 아니다.

양쪽 matched NVIDIA graphics/SM clock은 2805 MHz다. 온도는 61.154 → 61.615°C, 전력은 236.785 → 240.335 W다. 이 장치 관측은 게임 프로세스만의 수치가 아니며 PID busiest engine과도 다른 지표다. CPU 비율 감소를 FPS 향상으로 환산하지 않는다.

03 foreground 전체는 654 frames / tick 1–1381, 기본 29.044 FPS, SDK 58.146 FPS, GPU 31.233 ms, CPU 5.012%, PID GPU 60.677%, PDH coverage 94.570%다. 전체 범위와 matched 결과를 섞지 않는다.

## 개별 패스: 03 matched 무거운 순

|패스|01 ms|03 ms|03 측정 수|
|---|---:|---:|---:|
|GBuffer+Depth|7.0678|7.066|380|
|3-cascade Directional Shadow|4.3572|4.2375|380|
|AdaptiveGTAOHorizon|2.9513|3.0634|380|
|ClusterCutCount|1.498|1.4987|380|
|ClusterCutEmit|1.4675|1.4685|380|
|Deferred Cel Lighting|1.2428|1.2373|380|
|HeroLayerComposite|1.0769|1.0741|380|
|VirtualShadow.ResidentAndDirty|0.9106|0.9431|380|
|VirtualShadow.CountPageDraws|0.6926|0.7073|380|
|VirtualShadow.CullAndCommands|0.5769|0.5932|380|
|HdrPostForegroundDilation|0.5706|0.5769|380|
|SSRTrace|0.4772|0.4788|380|
|VirtualShadow.AdmitCompletePages|0.4571|0.4673|380|
|HdrPostDepthOfField|0.4532|0.4512|380|
|SSRTemporal|0.3821|0.4052|380|
|SSRSpatialCompose|0.4094|0.3984|380|
|Static Visibility|0.3254|0.3213|380|
|GTAOTemporal|0.3774|0.2841|380|
|VolumetricTemporal|0.1654|0.2356|380|
|Temporal Resolve|0.2384|0.2328|380|

전체 패스 목록은 JSON에 있다. 패스와 큐는 중첩될 수 있으므로 합계를 프레임 시간으로 사용하지 않는다.

## Feature group 작업량

|그룹|ms/frame|
|---|---:|
|shadows|7.3|
|surface|7.077|
|geometry|3.826|
|ao|3.678|
|refraction|1.731|
|reflections|1.282|
|lighting|1.237|
|depth_of_field|1.028|
|volumetrics|0.976|
|post|0.345|
|reconstruction|0.233|
|motion_blur|0.201|
|contact_shadows|0.19|
|adaptive|0.114|
|shared_post_guides|0.11|
|outline|0.097|
|transparency|0.08|
|frame_generation_guides|0.079|
|subsurface|0.073|
|bloom|0.056|
|color_grading|0.027|
|light_lists|0.017|
|particles|0.016|
|distortion|0.003|
|ui|0.002|
## 현재 52개 옵션: 연결된 작업량 내림차순

구 56개 옵션에서 삭제된 RT 4개는 복원하지 않았다. 아래 값은 옵션이 연결된 GPU group 작업량이며, 옵션을 끄면 절약되는 시간이나 독립적인 비용이 아니다. 여러 옵션이 같은 group을 중복 참조하므로 옵션 간 값을 더하지 않는다. N/A는 독립 계측 또는 대응 GPU group이 없다는 뜻이며 0 ms가 아니다.

|옵션|관련 작업량 ms|연결된 측정 group|
|---|---:|---|
|geometry_quality|18.204|geometry, surface, shadows|
|foliage_quality|18.204|geometry, surface, shadows|
|anisotropic_filtering|14.457|surface, transparency, shadows|
|adaptive_shading|12.153|adaptive, surface, ao, reflections|
|output_extent|8.892|surface, lighting, reconstruction, post|
|reconstruction_quality|8.548|reconstruction, surface, lighting|
|resolution_mode|8.548|surface, lighting, reconstruction|
|manual_scale_percent|8.548|surface, lighting, reconstruction|
|material_quality|8.387|surface, lighting, subsurface|
|shadow_mode|7.3|shadows|
|shadow_quality|7.3|shadows|
|shadow_distance_m|7.3|shadows|
|texture_quality|7.157|surface, transparency|
|pom_quality|7.077|surface|
|ao_mode|3.678|ao|
|ao_quality|3.678|ao|
|lighting_quality|2.231|lighting, volumetrics, light_lists|
|refraction_quality|1.731|refraction|
|reflection_mode|1.282|reflections|
|reflection_quality|1.282|reflections|
|depth_of_field|1.138|depth_of_field, shared_post_guides|
|particle_quality|1.075|particles, transparency, distortion, volumetrics|
|volumetric_quality|0.976|volumetrics|
|sharpening|0.577|reconstruction, post|
|hdr_mode|0.345|post|
|hdr_paper_white_nits|0.345|post|
|hdr_peak_nits|0.345|post|
|motion_blur|0.311|motion_blur, shared_post_guides|
|reconstruction_provider|0.233|reconstruction|
|contact_shadows|0.19|contact_shadows|
|outline|0.097|outline|
|outline_width_px|0.097|outline|
|transparency_quality|0.08|transparency|
|frame_generation_provider|0.079|frame_generation_guides|
|frame_generation_mode|0.079|frame_generation_guides|
|frame_generation_multiplier|0.079|frame_generation_guides|
|bloom|0.056|bloom|
|bloom_strength|0.056|bloom|
|color_grading|0.027|color_grading|
|preset|N/A|N/A|
|display_mode|N/A|N/A|
|display_output|N/A|N/A|
|vsync|N/A|N/A|
|frame_limit_fps|N/A|N/A|
|dynamic_resolution|N/A|N/A|
|target_render_fps|N/A|N/A|
|dynamic_min_percent|N/A|N/A|
|dynamic_max_percent|N/A|N/A|
|low_latency|N/A|N/A|
|texture_budget_mib|N/A|N/A|
|gi_mode|N/A|N/A|
|gi_quality|N/A|N/A|
Upload/readback 결측과 SDK FG 내부 전체 GPU 비용은 N/A다. GI는 software_hybrid 요청에 대해 실제 probe로 fallback했다. 원본 파일은 보존했고 분석 중 소스 변경이나 GPU 실행은 하지 않았다. 상세 필드, QPC 구간, SDK query 시간, 원본 SHA와 generation 제품 비교는 `final-foreground-audit.json`에 있다.