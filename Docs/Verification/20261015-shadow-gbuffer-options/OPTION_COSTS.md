**단일 최고 그래픽 옵션 캡처 평가 — CPU 직렬 그래프 작업이 렌더링을 지배합니다.**

RTX 4080에서 모든 품질 등급 14개는 Ultra로 실제 적용됐고, native RTAO/RT GI/hybrid RT 반사·그림자가 활성화됐습니다. 그러나 60초 입력 재생 동안 CPU 렌더 프레임은 8개뿐입니다. 10초 warmup 이후 관측된 base FPS는 **0.07252 FPS**이며, 렌더 wall 평균 **13.501초** 중 graph prepare **9.469초(70.14%)**, graph build **3.034초(22.47%)**가 차지합니다. CPU 병목 수정이 우선이며, 이 캡처로 수정 후 FPS나 기능을 끄면 얻을 개선폭을 계산할 수 없습니다.

**측정 범위와 증거**

- 실행은 `invocation.json`의 strict `--replay=.../golden-short`이며 `--replay-compare`, smoke, lockstep 인자는 없습니다. 기존 result/spec의 `runtime-smoke`/`offscreen_render` 문자열은 실제 실행 모드를 설명하는 근거로 쓰지 않았습니다.
- FHD 1920×1080, render scale 100%, VSync 꺼짐, cap 0, seed 1, 사용자 승인 임시 HP 고정. authored artistic scalar는 중립값 유지, texture budget은 자동입니다. 이 보고서는 재실행 없이 기존 파일만 분석했습니다.
- strict 실행 `execution_valid=true`, `assertions_passed=true`, 최종 tick 3511, checksum `14939530550461817357`, 종료코드 0. 진단 기록은 complete=true/io_failed=false이고 기록된 validation_errors=0입니다. 성능 캡처 자체의 validation 활성 여부와 correctness preflight는 별도 증거입니다.
- 3600 입력 프레임 중 실제 growth tick 3511(58.517초), kill 38, 피해량 380, melee spawn 52, peak enemies 20, 최소/최종 HP 100. ranged/late wave/boss 장면은 포함되지 않습니다.
- QPC 10MHz. CPU 8프레임 중 warm Playing 5개(frame 4–8), 이들 사이 4개 실제 QPC 간격 총55.159초로 FPS를 계산했습니다. CPU stage 역수로 FPS를 만들지 않았습니다.
- GPU 완료 데이터는 frame 1–5만 기록됐습니다. warmup 이후 자기 CPU 프레임으로 결합 가능한 GPU는 **frame 4(tick617,10.283초), frame 5(tick1474,24.567초)의 2개**뿐입니다. 이들은 observed frame7/8에서 늦게 읽혔습니다. tick1800 이후 GPU 표본은 없으므로 30–60초 GPU 통계는 제시하지 않습니다.
- 완료 GPU frame4/5는 같은 config revision2/settings generation0/pipeline revision1입니다. 마지막 GPU frame6–8은 다음 Render 관측 전에 실행이 끝나 이 로그에 없습니다. 표본 2개의 p95/p99는 단순 최대값이라 신뢰할 tail 통계로 해석하지 않습니다.

**CPU와 GPU를 분리한 병목 판단**

| CPU wall stage | warm 5프레임 평균 ms | 범위 ms | 렌더 평균 대비 |
|---|---:|---:|---:|
| graph_prepare | 9469.281 | 9397.590–9523.614 | 70.14% |
| graph_build | 3034.181 | 2557.452–3499.779 | 22.47% |
| graph_record | 653.344 | 99.025–904.393 | 4.84% |
| graph_plan | 187.060 | 185.467–190.485 | 1.39% |
| post_submit | 112.777 | 23.807–446.370 | 0.84% |
| graph_submit | 14.300 | 11.189–17.590 | 0.11% |
| constants_upload | 10.095 | 4.539–14.280 | 0.07% |
| streaming | 10.155 | 2.576–20.210 | 0.08% |
| present | 0.337 | 0.300–0.391 | 0.00% |
| wait | 0.015 | 0.004–0.057 | 0.00% |

prepare+build 합계는 렌더 wall의 **92.61%**입니다. 초기 frame1–3에도 prepare1.895–2.015초가 있었고, frame4 이후9.398–9.524초로 증가합니다. stage wall은 CPU busy 시간과 동일하지 않지만, processCPU와 thread 자료가 직렬 CPU 작업이라는 해석을 지지합니다. 어떤 함수의 quadratic 경로인지는 이 stage 계측만으로 확정하지 않으며 별도 소스 감사 결과와 구분합니다.

유효 warm utilization 61표본/실제66.404초에서 process CPU는 **0.988 core**, 20 logical CPU 기준 **4.94%**입니다. 가장 바쁜 thread 16548가 **0.935 core**를 사용했습니다. renderer worker_count8이라는 설정과 다르게 이 캡처의 실행 시간은 거의 한 코어에 집중됩니다.

게임 PID29360의 가장 바쁜 GPU engine 사용률은 유효61표본의 가중평균 **2.70%**, p50 0%, 최대43.87%입니다. 다른 PID/장치 전체/여러 engine 합산은 포함하지 않았습니다. GetCounter invalid-sample 경고66회가 있었고 분석기는 상태0/1의 target PID만 사용했습니다. NVIDIA 장치 전체 보조 로그101표본의 graphics clock은210–2835MHz이며77개가1000MHz 미만입니다. 이는 CPU가 오래 준비하는 동안 GPU가 공회전/낮은 클록에 머문 상황과 일치하나, 개별 패스의 낮은 클록 원인을 단독 확정하는 자료는 아닙니다.

같은 GPU 소유 frame4/5에 대응하는 CPU render wall 평균은 **14233.442ms**, GPU critical path는 **320.850/622.564ms(평균471.707ms)**입니다. CPU wall이 약30.17배 큽니다. GPU critical path는 큐 대기/겹침을 포함한 그래프 경과시간이고 GPU busy%가 아닙니다. 이2개 GPU 프레임도60FPS 예산16.667ms보다 큽니다. CPU 알고리즘 수정만으로 최고 옵션60FPS를 보장할 수 없으며, 그 수정 뒤의 정상 부하 성능을 이번 자료로 예측하지 않습니다.

**무거운 기능 그룹 순위 — 완료 GPU 2프레임의 측정 Direct/Compute 작업 ms**

같은 기능의 여러 패스 duration을 더한 작업량입니다. 비동기 큐 중첩과 공유 작업 때문에 이 열을 모두 더해 frame cost로 쓰거나 UI 옵션마다 다시 더하면 안 됩니다. 모드/품질/거리 같은 여러 컨트롤은 같은 기능 그룹을 참조합니다. 미측정 Copy transfer는 이 값에서 제외되어 전체GPU 작업량은 partial입니다.

| 순위 | 기능 그룹 | 평균 ms/완료프레임 | 두 프레임 범위 ms | 실제 scheduled 프레임 |
|---:|---|---:|---:|---:|
| 1 | RT 장면/BLAS·TLAS·변형 (`ray_scene`) | 139.389 | 71.095–207.682 | 2/2 |
| 2 | Hybrid RT 반사 (`reflections`) | 67.845 | 64.607–71.083 | 2/2 |
| 3 | GI·반사 공유 local probe 갱신 (`shared_probes`) | 59.416 | 0.000–118.831 | 1/2 |
| 4 | RT GI (`gi`) | 55.315 | 51.435–59.195 | 2/2 |
| 5 | 방향/가상/로컬 그림자 (`shadows`) | 39.192 | 29.775–48.608 | 2/2 |
| 6 | RTAO (`ao`) | 13.883 | 13.624–14.142 | 2/2 |
| 7 | GBuffer·surface (`surface`) | 9.657 | 5.034–14.280 | 2/2 |
| 8 | 볼류메트릭 (`volumetrics`) | 9.111 | 1.296–16.926 | 2/2 |
| 9 | 가시성·cluster·HLOD·skin (`geometry`) | 8.225 | 7.102–9.348 | 2/2 |
| 10 | 피사계 심도 (`depth_of_field`) | 4.998 | 4.199–5.796 | 2/2 |
| 11 | deferred 조명 (`lighting`) | 1.030 | 0.929–1.130 | 2/2 |
| 12 | adaptive 분류/VRS 생성 (`adaptive`) | 0.713 | 0.106–1.319 | 2/2 |
| 13 | 굴절/hero peel (`refraction`) | 0.676 | 0.388–0.964 | 2/2 |
| 14 | 접촉 그림자 (`contact_shadows`) | 0.374 | 0.227–0.521 | 2/2 |
| 15 | native temporal resolve (`reconstruction`) | 0.218 | 0.215–0.220 | 2/2 |
| 16 | 모션 블러 (`motion_blur`) | 0.218 | 0.201–0.234 | 2/2 |
| 17 | GI·반사 공유 합성 (`shared_ray_compose`) | 0.211 | 0.211–0.211 | 2/2 |
| 18 | 공유 노출·tone map·FXAA (`post`) | 0.193 | 0.193–0.193 | 2/2 |
| 19 | subsurface (`subsurface`) | 0.133 | 0.131–0.135 | 2/2 |
| 20 | 외곽선 (`outline`) | 0.108 | 0.099–0.117 | 2/2 |
| 21 | light list (`light_lists`) | 0.098 | 0.077–0.119 | 2/2 |
| 22 | 투명/OIT (`transparency`) | 0.096 | 0.086–0.105 | 2/2 |
| 23 | 모션 블러·DOF 공유 guides (`shared_post_guides`) | 0.080 | 0.076–0.084 | 2/2 |
| 24 | GPU 파티클 업데이트 (`particles`) | 0.068 | 0.048–0.088 | 2/2 |
| 25 | Bloom (`bloom`) | 0.057 | 0.054–0.060 | 2/2 |
| 26 | 색 보정 (`color_grading`) | 0.023 | 0.022–0.024 | 2/2 |
| 27 | UI (`ui`) | 0.020 | 0.019–0.022 | 2/2 |
| 28 | 왜곡 벡터 (`distortion`) | 0.010 | 0.007–0.013 | 2/2 |

`shared_probes`는 한 프레임에서만118.831ms의 cube capture/integration/prefilter가 발생했고 다른 프레임은 미스케줄(0 work)이므로 평균59.416ms입니다. shadows에는 이 probe 갱신의 directional shadow25.443ms도 한 번 포함됩니다. 정상 steady state의 지속 비용으로 일반화하지 않습니다. RT shadow mode 자체는 active지만 이2프레임에는 `RayShadow.TraceFiniteEmitter` 패스가 없고 초기화/ready만 있으므로 실제 finite-emitter RT shadow tracing 비용은 측정되지 않았습니다.

미측정 Copy 그룹: `scene_upload_initialization_copy_transfer`(pass 0), `ui_copy_transfer`(pass 23), `geometry_copy_transfer`(pass 39,50,190,196), `shadows_copy_transfer`(pass 147,220). 값은N/A로 유지하며0ms로 간주하지 않습니다.

**개별 패스 상위20개 — 실행하지 않은 프레임은0 work, Copy 제외**

| pass | 이름 | 큐 | GPU frame4 ms | GPU frame5 ms | 두 완료프레임당 평균 ms | scheduled n |
|---:|---|---|---:|---:|---:|---:|
| 123 | RaySceneDynamicBLASBuildUpdate | Direct | 61.572 | 193.096 | 127.334 | 2 |
| 160 | HybridReflectionTrace | Direct | 69.704 | 63.414 | 66.559 | 2 |
| 156 | RTGITrace | Direct | 58.344 | 50.255 | 54.300 | 2 |
| 2 | 3-cascade Directional Shadow | Direct | 21.137 | 20.390 | 20.764 | 2 |
| 180 | LocalProbeStaticCubeCapture | Direct | 0.000 | 28.052 | 14.026 | 1 |
| 144 | RTAOTrace | Direct | 13.180 | 13.739 | 13.459 | 2 |
| 177 | LocalProbeStaticDirectionalShadow | Direct | 0.000 | 25.443 | 12.722 | 1 |
| 184 | LocalProbeStaticCubeCapture | Direct | 0.000 | 25.134 | 12.567 | 1 |
| 185 | LocalProbeStaticCubeCapture | Direct | 0.000 | 21.700 | 10.850 | 1 |
| 183 | LocalProbeStaticCubeCapture | Direct | 0.000 | 20.848 | 10.424 | 1 |
| 181 | LocalProbeStaticCubeCapture | Direct | 0.000 | 20.618 | 10.309 | 1 |
| 142 | RayGeometryDeformation | Direct | 7.758 | 12.161 | 9.959 | 2 |
| 4 | GBuffer+Depth | Direct | 14.268 | 5.029 | 9.649 | 2 |
| 113 | HdrPostForegroundDilation | Direct | 5.256 | 3.718 | 4.487 | 2 |
| 40 | ClusterCutCount | Compute | 2.529 | 4.732 | 3.631 | 2 |
| 174 | VolumetricIntegrate | Direct | 5.359 | 0.181 | 2.770 | 2 |
| 172 | VolumetricTemporal | Direct | 4.077 | 0.139 | 2.108 | 2 |
| 150 | VirtualShadow.ResidentAndDirty | Compute | 2.902 | 0.670 | 1.786 | 2 |
| 171 | VolumetricLighting | Direct | 3.225 | 0.218 | 1.721 | 2 |
| 44 | ClusterCutEmit | Compute | 1.011 | 2.119 | 1.565 | 2 |

이 표의 cache 패스 평균은 두 완료프레임 기준입니다. `analysis.json` per_pass의 평균은 scheduled 표본만 대상으로 하므로 한 번 실행된 cache 패스 값과 정의가 다릅니다. RT dynamic BLAS61.572→193.096ms, GBuffer14.268→5.029ms처럼 표본 간 변동이 커서 이 순위를 안정적인 장기 순위로 단정하지 않습니다.

**최고 품질 적용과 native 비가용 범위**

14개 품질 항목 모두 요청·실효값Ultra: `texture_quality`, `geometry_quality`, `foliage_quality`, `material_quality`, `pom_quality`, `lighting_quality`, `shadow_quality`, `ao_quality`, `gi_quality`, `reflection_quality`, `transparency_quality`, `refraction_quality`, `volumetric_quality`, `particle_quality`. Motion blur는High, DOF/Bloom/Outline/Color grading/Contact shadows는켜짐, AF16, shadow/RT 거리500m입니다. `preset=custom`은CLI 제어값을 명시하면서 변경된 표시이며, 개별 품질이Medium으로 낮아졌다는 뜻이 아닙니다.

- DLSS/Streamline(SR/RR/FG/Reflex), FSR(SR/RR/FG), XeSS SR/FG/XeLL, DirectSR, NRD(REBLUR/RELAX/SIGMA), Brixelizer는 이 Release 구성의 해당 HS_WITH 플래그가OFF입니다. 설치된 SDK 폴더 경로와 build-in/native 활성화를 혼동하지 않았습니다. 이 단일 캡처는 이 provider들의 성능 비교를 포함하지 않습니다.
- Frame generation 요청auto→실효off. mode와multiplier의 requested_available=0; canonical multiplier2는 보존값이고 native_effective0입니다. 생성된 출력프레임/FGGPU비용은없어N/A입니다. Low latency auto→off입니다.
- Neural RR/provider auto→off. 내부 RTAO/RTGI/reflection temporal/spatial 패스는 존재하므로 외부 neural denoiser가off인 것과 내부 필터가없는 것을 혼동하지 않습니다. 외부 RR/NRD 성능은N/A입니다.
- HDR auto→off/SDR. 이번native출력이SDR이라는 사실만확인되며 모니터의HDR하드웨어지원자체는이자료로단정하지않습니다. HDR brightness는비활성정책입니다.
- Adaptive shading 요청on/canonical on이지만native상태는**raster 켜짐/compute 꺼짐**입니다. classify/VRS 생성0.713ms는실제overhead측정이며 compute adaptive적용이나순성능개선의증거가아닙니다.
- 내부TAAU+manual100%/1920×1080은실제적용됐습니다. reconstruction quality native→manual은명시적해상도정책의표현이며해상도fallback으로화질을낮췄다는뜻이아닙니다. 서로배타적인provider/모드는한실행에서동시에최고로켤수없습니다.

**56개 옵션 전체 실효값 감사**

최고라는 비교가 성립하지 않는 화면·pacing·자동예산·예술적scalar는 고정/자동/중립 정책으로 분류합니다. 아래값은 config revision2의 실제캡처manifest입니다.

| ID | 옵션 | 요청 | 실효 | 감사 결과 |
|---:|---|---|---|---|
| 1 | `preset` | `custom` | `custom` | 개별 최고값 유지; CLI로 custom 표시 |
| 2 | `display_mode` | `windowed` | `windowed` | FHD 측정 화면 정책 |
| 3 | `display_output` | `auto` | `dxgi:000000000000f3c1:\\.\DISPLAY1` | 현재 모니터 자동선택 |
| 4 | `output_extent` | `1920x1080` | `1920x1080` | FHD 고정 |
| 5 | `vsync` | `false` | `false` | 측정용 off |
| 6 | `frame_limit_fps` | `0` | `0` | 무제한 |
| 7 | `hdr_mode` | `auto` | `off` | SDR 자동결정; HDR 미활성 |
| 8 | `hdr_paper_white_nits` | `200` | `200` | HDR off에 종속; 중립값 |
| 9 | `hdr_peak_nits` | `auto` | `1000` | HDR off에 종속; 자동값 |
| 10 | `reconstruction_provider` | `internal_taau` | `internal_taau` | native TAAU 정책; 대체 SDK 미측정 |
| 11 | `reconstruction_quality` | `native` | `manual` | manual100% 정책 표현 |
| 12 | `resolution_mode` | `manual` | `manual` | FHD/100% 고정 정책 |
| 13 | `manual_scale_percent` | `100` | `100` | FHD native100%; supersampling200%는 제외 |
| 14 | `dynamic_resolution` | `false` | `false` | 자동 하향조절 off |
| 15 | `target_render_fps` | `60` | `60` | dynamic off; 비활성 controller 목표 |
| 16 | `dynamic_min_percent` | `50` | `50` | dynamic off; 비활성 bound |
| 17 | `dynamic_max_percent` | `100` | `100` | dynamic off; 비활성 bound |
| 18 | `sharpening` | `0.200000003` | `0.200000003` | 예술적 중립 scalar |
| 19 | `frame_generation_provider` | `auto` | `off` | FG off; 생성비용 N/A |
| 20 | `frame_generation_mode` | `auto` | `off` | native 미준비/미가용 fallback off |
| 21 | `frame_generation_multiplier` | `2` | `2` | FG off; canonical2/native0, 비용 N/A |
| 22 | `low_latency` | `auto` | `off` | native auto→off |
| 23 | `texture_quality` | `ultra` | `ultra` | 최고Ultra 적용 |
| 24 | `texture_budget_mib` | `0` | `0` | 자동 budget; 최대정수는 품질등급 아님 |
| 25 | `anisotropic_filtering` | `16` | `16` | 최대16 적용 |
| 26 | `geometry_quality` | `ultra` | `ultra` | 최고Ultra 적용 |
| 27 | `foliage_quality` | `ultra` | `ultra` | 최고Ultra 적용 |
| 28 | `material_quality` | `ultra` | `ultra` | 최고Ultra 적용 |
| 29 | `pom_quality` | `ultra` | `ultra` | 최고Ultra 적용 |
| 30 | `lighting_quality` | `ultra` | `ultra` | 최고Ultra 적용 |
| 31 | `shadow_mode` | `hybrid_rt` | `hybrid_rt` | native RT 최고모드 활성 |
| 32 | `shadow_quality` | `ultra` | `ultra` | 최고Ultra 적용 |
| 33 | `shadow_distance_m` | `500` | `500` | 허용최대500m |
| 34 | `contact_shadows` | `true` | `true` | 켜짐 적용 |
| 35 | `ao_mode` | `rtao` | `rtao` | native RT 최고모드 활성 |
| 36 | `ao_quality` | `ultra` | `ultra` | 최고Ultra 적용 |
| 37 | `gi_mode` | `ray_traced` | `ray_traced` | native RT 최고모드 활성 |
| 38 | `gi_quality` | `ultra` | `ultra` | 최고Ultra 적용 |
| 39 | `reflection_mode` | `hybrid_rt` | `hybrid_rt` | native RT 최고모드 활성 |
| 40 | `reflection_quality` | `ultra` | `ultra` | 최고Ultra 적용 |
| 41 | `ray_tracing_enabled` | `true` | `true` | native RT 최고모드 활성 |
| 42 | `rt_trace_distance_m` | `500` | `500` | 허용최대500m |
| 43 | `neural_ray_reconstruction` | `auto` | `off` | 외부 neural 기능 off; N/A |
| 44 | `ray_reconstruction_provider` | `auto` | `off` | 외부 provider off; 내부 필터와 구분 |
| 45 | `transparency_quality` | `ultra` | `ultra` | 최고Ultra 적용 |
| 46 | `refraction_quality` | `ultra` | `ultra` | 최고Ultra 적용 |
| 47 | `volumetric_quality` | `ultra` | `ultra` | 최고Ultra 적용 |
| 48 | `particle_quality` | `ultra` | `ultra` | 최고Ultra 적용 |
| 49 | `bloom` | `true` | `true` | 켜짐 적용 |
| 50 | `bloom_strength` | `1` | `1` | 예술적 중립1 |
| 51 | `motion_blur` | `high` | `high` | 최고High 적용 |
| 52 | `depth_of_field` | `true` | `true` | 켜짐 적용 |
| 53 | `outline` | `true` | `true` | 켜짐 적용 |
| 54 | `outline_width_px` | `1` | `1` | 예술적 중립1px |
| 55 | `color_grading` | `true` | `true` | 켜짐 적용 |
| 56 | `adaptive_shading` | `on` | `on` | raster on/compute off; 부분적용 정책 |

**연산량의 합리성과 추가 최적화 우선순위**

선택된Ultra프로필은RT axis ratio1, diffuse/specular2 samples, diffuse bounce budget4, RTAO8 samples, froxel stride8/slices96, shadow2048/filter16, hero refraction4 layers, POM32 steps를요청합니다. FHD의유효픽셀전부가참여한다고가정한상한예시는RTAO약1659만ray proposals, GI약415만primary path proposals에최대4bounce예산, reflection약415만proposals입니다. 실제ray수는depth/material검사·무효proposal·screen hit·조기종료에따라작아지므로실행counter로측정한수치가아닙니다. Froxel그리드는정책상240×135×96≈311만cell입니다. 22,057개static instance와500m거리, 동적deformation/BLAS, probe갱신이겹친환경에서RT장면과trace가GPU상위를차지하는방향은합리적입니다. 반면CPUgraph prepare9.5초/build3초가필수적인화질비용이라고해석하면안됩니다.

1. **CPU graph prepare/build의반복검색·쌍비교를먼저제거**: CPUwall92.61%와0.94core단일thread집중의직접증거가있습니다. 관련수정의정확성/복잡도검증과성능예측을구분합니다.
2. **RT dynamic BLAS/deformation의갱신대상과재사용검토**: dynamic BLAS평균127.334ms, deformation9.959ms로GPU상위입니다. 미변형/정적geometry까지불필요하게갱신하는지소스감사가필요하며, 이표본만으로범인을단정하지않습니다.
3. **full-resolution RT reflection/GI와RTAO예산검토**: trace각66.559/54.300/13.459ms. quality를바꾼A/B가없으므로저해상도·sample수감축으로얻을ms/FPS는추정하지않습니다. 현재요청은최고품질유지이므로무조건화질을낮추는변경과구분합니다.
4. **local probe/cached shadow의불필요한재캡처·dirty전파검토**: 단일probe갱신118.831ms+관련shadow25.443ms가발생했습니다. 한프레임에만관측되므로평균59.416ms를상시비용으로보지않습니다.
5. **이후GPU후순위검토**: shadow Direct/Compute39.192ms, surface9.657ms, volumetric9.111ms, geometry8.225ms, DOF4.998ms. shared/COPY/cache가분리된부분비용이며, 다른모드와비교하지않았습니다.

이한번의캡처는최고설정에서병목의위치와실제활성기능을확인하는데유효합니다. 정상steady-state 최고옵션FPS, 장기tail, 기능별marginal cost, 수정후개선율의검증자료로는부족합니다. 사용자요청대로동일성능캡처는추가반복하지않았고production코드변경도하지않았습니다.

증거파일: `invocation.json`, `performance.csv`, `gpu_live.csv`, `graphics_settings.csv`, `diagnostics.json`, `result.json`, `analysis.json`, `option-costs.json`, `renderer_features.jsonl`, `utilization/*`; 정책: `../max-profile-provenance.json`; 코드/품질예산참조: `ContentSource/Graphics/quality_profiles.json`, `renderer_ray_transport.cpp`, `ray_gi.hlsl`, `ray_reflections.hlsl`, `ray_ambient_occlusion.hlsl`.
