# 패스·옵션 비용 재검토 (2026-10-16)

기존 20261016-nonrt-ultra-01 Release 기록을 최신 분석기로 재분석했다. 새 실행/측정은 하지 않았다. 이 기록은 최종 RT 잔재 정리 전 빌드이며 현재 빌드 성능을 증명하지 않는다.

1920×1080, 내부 해상도 100%, Ultra, TAAU, DLSS FG 2배. Playing 742프레임 중 안정 설정 config 3은 741프레임이며 전환 config 2가 1프레임 포함된다. 패스 표는 Playing 전체, 옵션 표는 config 3 기준이다. 이동 없는 초기 전투 재생으로 장시간/이동 스트리밍 성능을 대표하지 않는다.

기본 32.943 FPS / SDK 보고 present 65.859 FPS. GPU critical path 27.101 ms, CPU render wall 28.784 ms. SDK 수치는 물리 디스플레이 표시 간격이 아니다. CPU 프로세스 4.203%(20논리코어 중 0.841코어), 프로세스 GPU busiest engine 68.709%, 장치 전체 GPU 92.091%(다른 앱 포함). PDH 오류 2건 때문에 전체 분석 상태 partial; 런타임 자체 GPU 프레임 수집은 완전하다.

시간은 ms. GPU 큐/CPU 병렬 기록은 겹칠 수 있어 합계가 프레임 시간이나 제거 시 절감량이 아니다. 미측정 Copy/외부 SDK 내부 비용은 N/A다. Present 12.285 ms는 순수 CPU 계산이 아닌 호출 벽시계 시간이며 대기 원인별 분해는 없다.

## GPU 패스 전체 — 프레임당 비용 내림차순

| ID | 패스 | 큐 | 실행시 평균 | 프레임당 비용 | 실행/전체 |
|---:|---|---|---:|---:|---:|
| 4 | GBuffer+Depth | 0 | 4.985 | 4.985 | 742/742 |
| 45 | AdaptiveGTAOHorizon | 0 | 3.894 | 3.894 | 742/742 |
| 2 | 3-cascade Directional Shadow | 0 | 3.576 | 3.576 | 742/742 |
| 150 | VirtualShadow.ResidentAndDirty | 1 | 1.513 | 1.513 | 742/742 |
| 224 | VirtualShadow.CountPageDraws | 1 | 1.465 | 1.465 | 742/742 |
| 5 | Deferred Cel Lighting | 0 | 1.247 | 1.247 | 742/742 |
| 40 | ClusterCutCount | 1 | 0.764 | 0.764 | 742/742 |
| 151 | VirtualShadow.CullAndCommands | 1 | 0.730 | 0.730 | 742/742 |
| 44 | ClusterCutEmit | 1 | 0.682 | 0.682 | 742/742 |
| 108 | HeroLayerComposite | 0 | 0.669 | 0.669 | 742/742 |
| 70 | SSRTrace | 0 | 0.631 | 0.631 | 742/742 |
| 113 | HdrPostForegroundDilation | 0 | 0.604 | 0.604 | 742/742 |
| 225 | VirtualShadow.AdmitCompletePages | 1 | 0.582 | 0.582 | 742/742 |
| 153 | VirtualShadow.DynamicBank | 0 | 0.492 | 0.492 | 742/742 |
| 114 | HdrPostDepthOfField | 0 | 0.449 | 0.449 | 742/742 |
| 72 | SSRSpatial | 0 | 0.447 | 0.447 | 742/742 |
| 71 | SSRTemporal | 0 | 0.431 | 0.431 | 742/742 |
| 3 | Static Visibility | 1 | 0.386 | 0.386 | 742/742 |
| 46 | GTAOTemporal | 0 | 0.264 | 0.264 | 742/742 |
| 171 | VolumetricLighting | 0 | 0.236 | 0.236 | 742/742 |
| 48 | ContactShadows | 0 | 0.229 | 0.229 | 742/742 |
| 172 | VolumetricTemporal | 0 | 0.226 | 0.226 | 742/742 |
| 9 | Temporal Resolve | 0 | 0.226 | 0.226 | 742/742 |
| 111 | HdrPostMotionBlur | 0 | 0.214 | 0.214 | 742/742 |
| 174 | VolumetricIntegrate | 0 | 0.178 | 0.178 | 742/742 |
| 47 | GTAOBilateral | 0 | 0.170 | 0.170 | 742/742 |
| 173 | VolumetricSpatial | 0 | 0.157 | 0.157 | 742/742 |
| 22 | Skin Cache | 1 | 0.151 | 0.151 | 742/742 |
| 282 | AdaptiveGTAOCurrentGuideResolve | 0 | 0.140 | 0.140 | 742/742 |
| 73 | SSRCompose | 0 | 0.116 | 0.116 | 742/742 |
| 149 | VirtualShadow.ReceiverFeedback | 1 | 0.112 | 0.112 | 742/742 |
| 106 | HeroLayerComposite | 0 | 0.110 | 0.110 | 742/742 |
| 280 | Adaptive.ClassifyActualGuides | 0 | 0.102 | 0.102 | 742/742 |
| 15 | Screen-space Outline | 0 | 0.098 | 0.098 | 742/742 |
| 110 | HdrPostNearestGuides | 0 | 0.094 | 0.094 | 742/742 |
| 170 | VolumetricDensity | 0 | 0.090 | 0.090 | 742/742 |
| 35 | ExposureHistogramBuild | 1 | 0.086 | 0.086 | 742/742 |
| 36 | ExposureReduce | 1 | 0.083 | 0.083 | 742/742 |
| 175 | VolumetricResolve | 0 | 0.069 | 0.069 | 742/742 |
| 8 | OIT Composite | 0 | 0.063 | 0.063 | 742/742 |
| 104 | HeroLayerComposite | 0 | 0.061 | 0.061 | 742/742 |
| 192 | HlodSelect | 1 | 0.055 | 0.055 | 742/742 |
| 18 | Depth Pyramid | 1 | 0.050 | 0.050 | 742/742 |
| 21 | Final Depth Pyramid | 1 | 0.042 | 0.042 | 742/742 |
| 61 | SubsurfaceVertical | 0 | 0.042 | 0.042 | 742/742 |
| 900 | SDK Display Guides | 0 | 0.037 | 0.037 | 741/742 |
| 152 | VirtualShadow.StaticBank | 0 | 0.036 | 0.036 | 742/742 |
| 60 | SubsurfaceHorizontal | 0 | 0.033 | 0.033 | 742/742 |
| 43 | ClusterCutPrefix2 | 1 | 0.030 | 0.030 | 742/742 |
| 901 | SDK Undilated Guides | 0 | 0.027 | 0.027 | 741/742 |
| 100 | HeroNearestFrontGuides | 0 | 0.026 | 0.026 | 742/742 |
| 112 | HdrPostSignedCoC | 0 | 0.026 | 0.026 | 742/742 |
| 52 | ClusterPreviousHzbOcclusion | 1 | 0.025 | 0.025 | 742/742 |
| 103 | HeroCopyBackground | 0 | 0.023 | 0.023 | 742/742 |
| 33 | PreExposure | 0 | 0.021 | 0.021 | 742/742 |
| 116 | HdrPostColorGrade | 0 | 0.021 | 0.021 | 742/742 |
| 10 | Bloom Half Extract | 0 | 0.021 | 0.021 | 742/742 |
| 14 | ToneMap | 0 | 0.021 | 0.021 | 742/742 |
| 16 | FXAA | 0 | 0.020 | 0.020 | 742/742 |
| 90 | HeroPeelFront | 0 | 0.020 | 0.020 | 742/742 |
| 101 | HeroCopyBackground | 0 | 0.018 | 0.018 | 742/742 |
| 105 | HeroCopyBackground | 0 | 0.018 | 0.018 | 742/742 |
| 13 | Bloom | 0 | 0.018 | 0.018 | 742/742 |
| 102 | HeroLayerComposite | 0 | 0.017 | 0.017 | 742/742 |
| 99 | HeroOitTail | 0 | 0.015 | 0.015 | 742/742 |
| 92 | HeroPeelFront | 0 | 0.015 | 0.015 | 742/742 |
| 53 | ClusterCurrentDepthRetest | 1 | 0.015 | 0.015 | 742/742 |
| 96 | HeroPeelFront | 0 | 0.015 | 0.015 | 742/742 |
| 107 | HeroCopyBackground | 0 | 0.015 | 0.015 | 742/742 |
| 94 | HeroPeelFront | 0 | 0.015 | 0.015 | 742/742 |
| 903 | SDK Backbuffer Copy | 0 | 0.015 | 0.015 | 741/742 |
| 91 | HeroPeelExit | 0 | 0.014 | 0.014 | 742/742 |
| 93 | HeroPeelExit | 0 | 0.014 | 0.014 | 742/742 |
| 95 | HeroPeelExit | 0 | 0.014 | 0.014 | 742/742 |
| 177 | LocalProbeStaticDirectionalShadow | 0 | 2.535 | 0.014 | 4/742 |
| 97 | HeroPeelExit | 0 | 0.012 | 0.012 | 742/742 |
| 12 | Bloom Half Upsample | 0 | 0.011 | 0.011 | 742/742 |
| 42 | ClusterCutPrefix1 | 1 | 0.010 | 0.010 | 742/742 |
| 41 | ClusterCutPrefix0 | 1 | 0.010 | 0.010 | 742/742 |
| 1 | GPU Particle Spawn/Update | 1 | 0.009 | 0.009 | 742/742 |
| 7 | Forward Transparent/OIT | 0 | 0.008 | 0.008 | 742/742 |
| 148 | VirtualShadow.InitializeFeedback | 1 | 0.008 | 0.008 | 742/742 |
| 131 | LightListClusters | 0 | 0.008 | 0.008 | 742/742 |
| 193 | HlodEmit | 1 | 0.008 | 0.008 | 742/742 |
| 191 | HlodClear | 1 | 0.007 | 0.007 | 742/742 |
| 194 | HlodPublish | 1 | 0.005 | 0.005 | 742/742 |
| 20 | GBuffer Retest | 0 | 0.005 | 0.005 | 742/742 |
| 281 | Adaptive.ReduceNativeVrsTiles | 0 | 0.005 | 0.005 | 742/742 |
| 34 | ExposureHistogramClear | 1 | 0.005 | 0.005 | 742/742 |
| 180 | LocalProbeStaticCubeCapture | 0 | 0.830 | 0.004 | 4/742 |
| 195 | HlodHistory | 1 | 0.004 | 0.004 | 742/742 |
| 19 | Visibility Retest | 1 | 0.004 | 0.004 | 742/742 |
| 185 | LocalProbeStaticCubeCapture | 0 | 0.772 | 0.004 | 4/742 |
| 11 | Bloom Quarter Downsample | 0 | 0.004 | 0.004 | 742/742 |
| 290 | VfxScreenOverlayAlpha | 0 | 0.025 | 0.004 | 114/742 |
| 181 | LocalProbeStaticCubeCapture | 0 | 0.707 | 0.004 | 4/742 |
| 184 | LocalProbeStaticCubeCapture | 0 | 0.679 | 0.004 | 4/742 |
| 154 | VirtualShadow.Publish | 1 | 0.003 | 0.003 | 742/742 |
| 6 | VFX Distortion Vectors | 0 | 0.002 | 0.002 | 742/742 |
| 98 | HeroClearOverflow | 0 | 0.002 | 0.002 | 742/742 |
| 902 | SDK DevTools UI | 0 | 0.002 | 0.002 | 741/742 |
| 130 | LightListUpload | 0 | 0.001 | 0.001 | 742/742 |
| 183 | LocalProbeStaticCubeCapture | 0 | 0.050 | 0.000 | 4/742 |
| 187 | LocalProbeCosineShIntegration | 0 | 0.044 | 0.000 | 4/742 |
| 188 | LocalProbeGgxPrefilter | 0 | 0.016 | 0.000 | 4/742 |
| 17 | Game UI | 0 | 0.000 | 0.000 | 742/742 |
| 182 | LocalProbeStaticCubeCapture | 0 | 0.004 | 0.000 | 4/742 |
| 223 | LocalShadow.ShadowDataReady | 0 | 0.000 | 0.000 | 742/742 |
| 904 | SDK Frame Generation Prepare | 0 | 0.000 | 0.000 | 741/742 |
| 155 | VirtualShadow.ShadowDataReady | 0 | 0.000 | 0.000 | 742/742 |
| 221 | LocalShadow.StaticBank | 0 | 0.000 | 0.000 | 742/742 |
| 222 | LocalShadow.DynamicBank | 0 | 0.000 | 0.000 | 742/742 |
| 0 | Scene Buffer Upload | 2 | N/A | N/A | 742/742 |
| 23 | UI Upload | 2 | N/A | N/A | 147/742 |
| 39 | ClusterPageUpload | 2 | N/A | N/A | 742/742 |
| 147 | VirtualShadow.SnapshotUpload | 2 | N/A | N/A | 742/742 |
| 190 | HlodUpload | 2 | N/A | N/A | 742/742 |
| 50 | ClusterDemandStatusReadback | 2 | N/A | N/A | 742/742 |
| 196 | HlodDemandStatusReadback | 2 | N/A | N/A | 742/742 |
| 220 | LocalShadow.UploadCommands | 2 | N/A | N/A | 742/742 |

## CPU 패스 기록 전체 — 프레임당 벽시계 비용 내림차순

| ID | 패스 | 실행시 평균 | 프레임당 비용 | 최대 | 실행/전체 |
|---:|---|---:|---:|---:|---:|
| 40 | ClusterCutCount | 0.396 | 0.396 | 0.905 | 742/742 |
| 2 | 3-cascade Directional Shadow | 0.258 | 0.258 | 0.648 | 742/742 |
| 4 | GBuffer+Depth | 0.135 | 0.135 | 1.073 | 742/742 |
| 20 | GBuffer Retest | 0.121 | 0.121 | 2.400 | 742/742 |
| 1 | GPU Particle Spawn/Update | 0.090 | 0.090 | 9.751 | 742/742 |
| 22 | Skin Cache | 0.090 | 0.090 | 10.093 | 742/742 |
| 0 | Scene Buffer Upload | 0.072 | 0.072 | 9.790 | 742/742 |
| 44 | ClusterCutEmit | 0.068 | 0.068 | 0.392 | 742/742 |
| 191 | HlodClear | 0.066 | 0.066 | 10.121 | 742/742 |
| 902 | SDK DevTools UI | 0.059 | 0.059 | 9.670 | 741/742 |
| 190 | HlodUpload | 0.059 | 0.059 | 9.800 | 742/742 |
| 7 | Forward Transparent/OIT | 0.058 | 0.058 | 0.847 | 742/742 |
| 192 | HlodSelect | 0.057 | 0.057 | 9.783 | 742/742 |
| 152 | VirtualShadow.StaticBank | 0.057 | 0.057 | 1.336 | 742/742 |
| 153 | VirtualShadow.DynamicBank | 0.054 | 0.054 | 0.395 | 742/742 |
| 41 | ClusterCutPrefix0 | 0.053 | 0.053 | 0.709 | 742/742 |
| 193 | HlodEmit | 0.052 | 0.052 | 9.832 | 742/742 |
| 52 | ClusterPreviousHzbOcclusion | 0.051 | 0.051 | 0.350 | 742/742 |
| 5 | Deferred Cel Lighting | 0.051 | 0.051 | 1.178 | 742/742 |
| 43 | ClusterCutPrefix2 | 0.050 | 0.050 | 1.136 | 742/742 |
| 50 | ClusterDemandStatusReadback | 0.049 | 0.049 | 1.109 | 742/742 |
| 904 | SDK Frame Generation Prepare | 0.049 | 0.049 | 1.133 | 741/742 |
| 42 | ClusterCutPrefix1 | 0.047 | 0.047 | 0.298 | 742/742 |
| 70 | SSRTrace | 0.047 | 0.047 | 1.202 | 742/742 |
| 90 | HeroPeelFront | 0.046 | 0.046 | 0.564 | 742/742 |
| 53 | ClusterCurrentDepthRetest | 0.045 | 0.045 | 0.292 | 742/742 |
| 108 | HeroLayerComposite | 0.042 | 0.042 | 0.770 | 742/742 |
| 100 | HeroNearestFrontGuides | 0.040 | 0.040 | 1.072 | 742/742 |
| 8 | OIT Composite | 0.039 | 0.039 | 0.940 | 742/742 |
| 18 | Depth Pyramid | 0.038 | 0.038 | 0.277 | 742/742 |
| 148 | VirtualShadow.InitializeFeedback | 0.037 | 0.037 | 9.873 | 742/742 |
| 9 | Temporal Resolve | 0.037 | 0.037 | 0.337 | 742/742 |
| 92 | HeroPeelFront | 0.036 | 0.036 | 0.944 | 742/742 |
| 21 | Final Depth Pyramid | 0.036 | 0.036 | 0.287 | 742/742 |
| 91 | HeroPeelExit | 0.035 | 0.035 | 0.370 | 742/742 |
| 194 | HlodPublish | 0.033 | 0.033 | 1.114 | 742/742 |
| 3 | Static Visibility | 0.033 | 0.033 | 0.243 | 742/742 |
| 94 | HeroPeelFront | 0.031 | 0.031 | 0.292 | 742/742 |
| 102 | HeroLayerComposite | 0.031 | 0.031 | 0.810 | 742/742 |
| 6 | VFX Distortion Vectors | 0.031 | 0.031 | 0.383 | 742/742 |
| 13 | Bloom | 0.031 | 0.031 | 1.022 | 742/742 |
| 101 | HeroCopyBackground | 0.030 | 0.030 | 0.285 | 742/742 |
| 900 | SDK Display Guides | 0.030 | 0.030 | 0.794 | 741/742 |
| 175 | VolumetricResolve | 0.030 | 0.030 | 0.955 | 742/742 |
| 99 | HeroOitTail | 0.030 | 0.030 | 0.820 | 742/742 |
| 93 | HeroPeelExit | 0.030 | 0.030 | 0.386 | 742/742 |
| 903 | SDK Backbuffer Copy | 0.029 | 0.029 | 0.519 | 741/742 |
| 195 | HlodHistory | 0.029 | 0.029 | 0.453 | 742/742 |
| 104 | HeroLayerComposite | 0.028 | 0.028 | 0.459 | 742/742 |
| 96 | HeroPeelFront | 0.028 | 0.028 | 0.132 | 742/742 |
| 11 | Bloom Quarter Downsample | 0.028 | 0.028 | 0.987 | 742/742 |
| 97 | HeroPeelExit | 0.028 | 0.028 | 0.420 | 742/742 |
| 106 | HeroLayerComposite | 0.027 | 0.027 | 0.456 | 742/742 |
| 19 | Visibility Retest | 0.027 | 0.027 | 0.286 | 742/742 |
| 95 | HeroPeelExit | 0.027 | 0.027 | 0.128 | 742/742 |
| 33 | PreExposure | 0.027 | 0.027 | 0.278 | 742/742 |
| 110 | HdrPostNearestGuides | 0.027 | 0.027 | 1.048 | 742/742 |
| 12 | Bloom Half Upsample | 0.026 | 0.026 | 0.237 | 742/742 |
| 16 | FXAA | 0.026 | 0.026 | 1.097 | 742/742 |
| 10 | Bloom Half Extract | 0.026 | 0.026 | 0.248 | 742/742 |
| 103 | HeroCopyBackground | 0.026 | 0.026 | 0.145 | 742/742 |
| 35 | ExposureHistogramBuild | 0.026 | 0.026 | 0.506 | 742/742 |
| 14 | ToneMap | 0.025 | 0.025 | 0.468 | 742/742 |
| 171 | VolumetricLighting | 0.025 | 0.025 | 0.766 | 742/742 |
| 174 | VolumetricIntegrate | 0.025 | 0.025 | 1.081 | 742/742 |
| 105 | HeroCopyBackground | 0.025 | 0.025 | 0.290 | 742/742 |
| 98 | HeroClearOverflow | 0.025 | 0.025 | 0.622 | 742/742 |
| 173 | VolumetricSpatial | 0.025 | 0.025 | 1.071 | 742/742 |
| 150 | VirtualShadow.ResidentAndDirty | 0.025 | 0.025 | 0.644 | 742/742 |
| 15 | Screen-space Outline | 0.025 | 0.025 | 0.410 | 742/742 |
| 111 | HdrPostMotionBlur | 0.024 | 0.024 | 0.622 | 742/742 |
| 116 | HdrPostColorGrade | 0.024 | 0.024 | 0.492 | 742/742 |
| 46 | GTAOTemporal | 0.024 | 0.024 | 0.419 | 742/742 |
| 901 | SDK Undilated Guides | 0.024 | 0.024 | 0.311 | 741/742 |
| 45 | AdaptiveGTAOHorizon | 0.024 | 0.024 | 0.917 | 742/742 |
| 114 | HdrPostDepthOfField | 0.024 | 0.024 | 0.485 | 742/742 |
| 34 | ExposureHistogramClear | 0.024 | 0.024 | 0.767 | 742/742 |
| 36 | ExposureReduce | 0.024 | 0.024 | 0.138 | 742/742 |
| 107 | HeroCopyBackground | 0.024 | 0.024 | 0.314 | 742/742 |
| 131 | LightListClusters | 0.023 | 0.023 | 0.182 | 742/742 |
| 170 | VolumetricDensity | 0.023 | 0.023 | 0.884 | 742/742 |
| 112 | HdrPostSignedCoC | 0.023 | 0.023 | 0.408 | 742/742 |
| 60 | SubsurfaceHorizontal | 0.023 | 0.023 | 0.169 | 742/742 |
| 280 | Adaptive.ClassifyActualGuides | 0.023 | 0.023 | 0.085 | 742/742 |
| 281 | Adaptive.ReduceNativeVrsTiles | 0.022 | 0.022 | 0.746 | 742/742 |
| 149 | VirtualShadow.ReceiverFeedback | 0.022 | 0.022 | 0.232 | 742/742 |
| 113 | HdrPostForegroundDilation | 0.022 | 0.022 | 0.428 | 742/742 |
| 282 | AdaptiveGTAOCurrentGuideResolve | 0.022 | 0.022 | 0.485 | 742/742 |
| 72 | SSRSpatial | 0.022 | 0.022 | 0.761 | 742/742 |
| 151 | VirtualShadow.CullAndCommands | 0.022 | 0.022 | 0.303 | 742/742 |
| 172 | VolumetricTemporal | 0.022 | 0.022 | 0.167 | 742/742 |
| 73 | SSRCompose | 0.022 | 0.022 | 0.467 | 742/742 |
| 47 | GTAOBilateral | 0.022 | 0.022 | 0.157 | 742/742 |
| 61 | SubsurfaceVertical | 0.021 | 0.021 | 0.239 | 742/742 |
| 71 | SSRTemporal | 0.021 | 0.021 | 0.118 | 742/742 |
| 196 | HlodDemandStatusReadback | 0.021 | 0.021 | 0.198 | 742/742 |
| 220 | LocalShadow.UploadCommands | 0.021 | 0.021 | 0.078 | 742/742 |
| 130 | LightListUpload | 0.021 | 0.021 | 0.103 | 742/742 |
| 48 | ContactShadows | 0.021 | 0.021 | 0.123 | 742/742 |
| 154 | VirtualShadow.Publish | 0.021 | 0.021 | 0.159 | 742/742 |
| 221 | LocalShadow.StaticBank | 0.020 | 0.020 | 0.121 | 742/742 |
| 224 | VirtualShadow.CountPageDraws | 0.020 | 0.020 | 0.175 | 742/742 |
| 147 | VirtualShadow.SnapshotUpload | 0.020 | 0.020 | 0.167 | 742/742 |
| 17 | Game UI | 0.020 | 0.020 | 0.507 | 742/742 |
| 39 | ClusterPageUpload | 0.019 | 0.019 | 0.370 | 742/742 |
| 225 | VirtualShadow.AdmitCompletePages | 0.019 | 0.019 | 0.105 | 742/742 |
| 222 | LocalShadow.DynamicBank | 0.019 | 0.019 | 0.138 | 742/742 |
| 155 | VirtualShadow.ShadowDataReady | 0.019 | 0.019 | 0.226 | 742/742 |
| 223 | LocalShadow.ShadowDataReady | 0.018 | 0.018 | 0.214 | 742/742 |
| 177 | LocalProbeStaticDirectionalShadow | 2.736 | 0.015 | 3.233 | 4/742 |
| 23 | UI Upload | 0.064 | 0.013 | 0.246 | 147/742 |
| 181 | LocalProbeStaticCubeCapture | 1.046 | 0.006 | 1.916 | 4/742 |
| 185 | LocalProbeStaticCubeCapture | 0.976 | 0.005 | 1.871 | 4/742 |
| 184 | LocalProbeStaticCubeCapture | 0.966 | 0.005 | 1.301 | 4/742 |
| 180 | LocalProbeStaticCubeCapture | 0.937 | 0.005 | 1.567 | 4/742 |
| 290 | VfxScreenOverlayAlpha | 0.031 | 0.005 | 0.161 | 114/742 |
| 183 | LocalProbeStaticCubeCapture | 0.275 | 0.001 | 0.405 | 4/742 |
| 182 | LocalProbeStaticCubeCapture | 0.163 | 0.001 | 0.180 | 4/742 |
| 187 | LocalProbeCosineShIntegration | 0.033 | 0.000 | 0.054 | 4/742 |
| 188 | LocalProbeGgxPrefilter | 0.030 | 0.000 | 0.047 | 4/742 |

## CPU 단계 전체

하위 단계와 render 전체를 더하지 않는다. constants_upload는 단순 memcpy만이 아니라 주변 준비 작업을 포함하는 계측 범위다.

| 단계 | 평균 | 최대 |
|---|---:|---:|
| render | 28.784 | 113.301 |
| present | 12.285 | 41.363 |
| graph_build | 4.807 | 24.133 |
| graph_submit | 3.568 | 5.989 |
| constants_upload | 2.284 | 5.724 |
| adapter | 2.072 | 413.630 |
| scene_prepare | 1.457 | 5.917 |
| graph_record | 1.321 | 11.133 |
| graph_prepare | 0.982 | 1.598 |
| graph_plan | 0.571 | 78.130 |
| streaming | 0.407 | 2.425 |
| ui | 0.316 | 8.559 |
| wrapper | 0.289 | 29.998 |
| wait | 0.247 | 11.679 |
| post_submit | 0.053 | 0.237 |
| diagnostics | 0.053 | 0.248 |
| descriptors | 0.052 | 0.178 |

## 기능 묶음 비용 — 옵션 간 공유

| 기능 | 프레임당 GPU 작업 | 관련 옵션 |
|---|---:|---|
| shadows | 8.526 |anisotropic_filtering, geometry_quality, foliage_quality, shadow_mode, shadow_quality, shadow_distance_m |
| surface | 4.991 |output_extent, reconstruction_quality, resolution_mode, manual_scale_percent, texture_quality, anisotropic_filtering, geometry_quality, foliage_quality, material_quality, pom_quality, adaptive_shading |
| ao | 4.468 |ao_mode, ao_quality, adaptive_shading |
| geometry | 2.249 |geometry_quality, foliage_quality |
| reflections | 1.625 |reflection_mode, reflection_quality, adaptive_shading |
| lighting | 1.247 |output_extent, reconstruction_quality, resolution_mode, manual_scale_percent, material_quality, lighting_quality |
| refraction | 1.095 |refraction_quality |
| depth_of_field | 1.079 |depth_of_field |
| volumetrics | 0.955 |lighting_quality, volumetric_quality, particle_quality |
| post | 0.251 |output_extent, hdr_mode, hdr_paper_white_nits, hdr_peak_nits, sharpening |
| contact_shadows | 0.229 |contact_shadows |
| reconstruction | 0.226 |output_extent, reconstruction_provider, reconstruction_quality, resolution_mode, manual_scale_percent, sharpening |
| motion_blur | 0.214 |motion_blur |
| adaptive | 0.106 |adaptive_shading |
| outline | 0.098 |outline, outline_width_px |
| shared_post_guides | 0.094 |motion_blur, depth_of_field |
| transparency | 0.075 |texture_quality, anisotropic_filtering, transparency_quality, particle_quality |
| subsurface | 0.075 |material_quality |
| frame_generation_guides | 0.063 |frame_generation_provider, frame_generation_mode, frame_generation_multiplier |
| bloom | 0.054 |bloom, bloom_strength |
| color_grading | 0.021 |color_grading |
| shared_probes | 0.013 |gi_mode, gi_quality, reflection_mode, reflection_quality |
| light_lists | 0.009 |lighting_quality |
| particles | 0.009 |particle_quality |
| distortion | 0.002 |particle_quality |
| ui | 0.002 | |

## 그래픽 옵션 전체 52개

가장 무거운 연결 기능 기준으로 정렬한다. 동일 비용이 여러 옵션에 반복되므로 더하지 않는다. 옵션 단독 증분 비용은 전부 N/A: ON/OFF·등급별 비교 측정이 없다. off/미관측은 0 ms로 취급하지 않는다. FG guides는 프레임 생성 전체 비용이 아니다.

| 옵션 | 요청 → 적용 | 상태 | 연결 기능 비용 ms | 단독 증분 |
|---|---|---|---|---|
| anisotropic_filtering | 16 → 16 | shared | shadows: 8.526; surface: 4.991; transparency: 0.075 | N/A |
| geometry_quality | ultra → ultra | shared | shadows: 8.526; surface: 4.991; geometry: 2.249 | N/A |
| foliage_quality | ultra → ultra | shared | shadows: 8.526; surface: 4.991; geometry: 2.249 | N/A |
| shadow_mode | raster → raster | measured_group | shadows: 8.526 | N/A |
| shadow_quality | ultra → ultra | measured_group | shadows: 8.526 | N/A |
| shadow_distance_m | 160 → 160 | measured_group | shadows: 8.526 | N/A |
| output_extent | 1920x1080 → 1920x1080 | shared | surface: 4.991; lighting: 1.247; post: 0.251; reconstruction: 0.226 | N/A |
| reconstruction_quality | quality → manual | shared_policy | surface: 4.991; lighting: 1.247; reconstruction: 0.226 | N/A |
| resolution_mode | manual → manual | shared_policy | surface: 4.991; lighting: 1.247; reconstruction: 0.226 | N/A |
| manual_scale_percent | 100 → 100 | shared_policy | surface: 4.991; lighting: 1.247; reconstruction: 0.226 | N/A |
| texture_quality | ultra → ultra | shared | surface: 4.991; transparency: 0.075 | N/A |
| material_quality | ultra → ultra | shared | surface: 4.991; lighting: 1.247; subsurface: 0.075 | N/A |
| pom_quality | ultra → ultra | shared | surface: 4.991 | N/A |
| adaptive_shading | auto → auto | policy_with_stage_evidence | surface: 4.991; ao: 4.468; reflections: 1.625; adaptive: 0.106 | N/A |
| ao_mode | gtao → gtao | measured_group | ao: 4.468 | N/A |
| ao_quality | ultra → ultra | measured_group | ao: 4.468 | N/A |
| reflection_mode | screen_space → screen_space | measured_group | reflections: 1.625; shared_probes: 0.013 | N/A |
| reflection_quality | ultra → ultra | measured_group | reflections: 1.625; shared_probes: 0.013 | N/A |
| lighting_quality | ultra → ultra | shared | lighting: 1.247; volumetrics: 0.955; light_lists: 0.009 | N/A |
| refraction_quality | ultra → ultra | measured_group | refraction: 1.095 | N/A |
| depth_of_field | true → true | measured_group | depth_of_field: 1.079; shared_post_guides: 0.094 | N/A |
| volumetric_quality | ultra → ultra | measured_group | volumetrics: 0.955 | N/A |
| particle_quality | ultra → ultra | shared | volumetrics: 0.955; transparency: 0.075; particles: 0.009; distortion: 0.002 | N/A |
| sharpening | 0.200000003 → 0.200000003 | shared | post: 0.251; reconstruction: 0.226 | N/A |
| contact_shadows | true → true | measured_group | contact_shadows: 0.229 | N/A |
| reconstruction_provider | internal_taau → internal_taau | shared_policy | reconstruction: 0.226 | N/A |
| motion_blur | high → high | measured_group | motion_blur: 0.214; shared_post_guides: 0.094 | N/A |
| outline | true → true | measured_group | outline: 0.098 | N/A |
| outline_width_px | 1 → 1 | shared | outline: 0.098 | N/A |
| transparency_quality | ultra → ultra | measured_group | transparency: 0.075 | N/A |
| frame_generation_provider | dlss → dlss | external | frame_generation_guides: 0.063 | N/A |
| frame_generation_mode | fixed → fixed | external | frame_generation_guides: 0.063 | N/A |
| frame_generation_multiplier | 2 → 2 | external | frame_generation_guides: 0.063 | N/A |
| bloom | true → true | measured_group | bloom: 0.054 | N/A |
| bloom_strength | 1 → 1 | shared | bloom: 0.054 | N/A |
| color_grading | true → true | measured_group | color_grading: 0.021 | N/A |
| gi_mode | software_hybrid → probe | requested_unavailable_effective_fallback | shared_probes: 0.013 | N/A |
| gi_quality | ultra → ultra | measured_group | shared_probes: 0.013 | N/A |
| preset | ultra → ultra | configuration | N/A | N/A |
| display_mode | borderless_fullscreen → borderless_fullscreen | presentation | N/A | N/A |
| display_output | auto → dxgi:000000000000f494:\\.\DISPLAY1 | presentation | N/A | N/A |
| vsync | false → false | off | N/A | N/A |
| frame_limit_fps | 0 → 0 | presentation | N/A | N/A |
| hdr_mode | auto → off | off | N/A | N/A |
| hdr_paper_white_nits | 200 → 200 | off | N/A | N/A |
| hdr_peak_nits | auto → 1000 | off | N/A | N/A |
| dynamic_resolution | false → false | off | N/A | N/A |
| target_render_fps | 144 → 144 | off | N/A | N/A |
| dynamic_min_percent | 50 → 50 | off | N/A | N/A |
| dynamic_max_percent | 100 → 100 | off | N/A | N/A |
| low_latency | on → on | presentation | N/A | N/A |
| texture_budget_mib | 0 → 0 | resource | N/A | N/A |

## 기대 성능과의 대조

- 그림자 8.526 ms, surface 4.991 ms, AO 4.468 ms가 우선 조사 대상이다. 보편적인 정상 ms 기준은 없으므로 이 기록만으로 구현 결함이나 타 엔진 대비 열세를 확정할 수 없다.
- software_hybrid GI 요청은 실제 probe로 fallback했다(consumer_unavailable). 소프트웨어 GI 최고 옵션의 성능 측정으로 간주할 수 없다.
- probe 0.013 ms는 741프레임에 분산한 값이다. 드물게 실행되는 프레임의 비용을 숨기지 않도록 패스 실행시 평균도 함께 기록했다.
- geometry/foliage/anisotropic은 여러 패스를 공유한다. 예를 들어 그림자 8.526 ms를 이방성 필터링 자체 비용으로 읽으면 안 된다.
- Bloom 0.054 ms, outline 0.098 ms보다 우선순위가 높은 비용이 명확하다. 저비용 효과를 먼저 제거하는 방식은 이번 자료로 정당화되지 않는다.
- 144 FPS 목표는 미달이다. FG 2배여도 실제 렌더 약72 FPS가 필요하지만 표시 지연과 FG 비용 때문에 이를 그대로 보장식으로 사용할 수 없다.

## 개선 조사 결과 — 구현 전 우선순위

아래는 현재 코드에서 확인한 개선 후보이며 절감 시간 예측이나 구현 완료가 아니다. 품질을 낮추는 옵션 변경은 최적화와 별도로 판단한다.

| 우선순위 | 대상 / 기록 비용 | 코드에서 확인한 내용과 최소 개선 방향 | 위험 및 좁은 검증 |
|---:|---|---|---|
| 1 | VSM Count 1.465 + Cull 0.730 ms | [Source/RendererD3D12/Private/renderer_virtual_shadow.cpp:642](C:/Users/crapa/.codex/worktrees/cpu-gpu-utilization/Project-HS/Source/RendererD3D12/Private/renderer_virtual_shadow.cpp:642) 전체 page × caster dispatch. 정렬된 활성 페이지 목록으로 범위 압축을 먼저 비교한다. | 목록 생성비 포함 비교. zero caster/overflow/sparse/dense에서 count·command·완전 페이지 수용 계약 일치. |
| 2 | GTAO Horizon 3.894 ms | [Content/Shaders/ambient_occlusion.hlsl:140](C:/Users/crapa/.codex/worktrees/cpu-gpu-utilization/Project-HS/Content/Shaders/ambient_occlusion.hlsl:140) 방향×step×양쪽마다 full-res depth와 view position 복원. view-space depth 전처리와 거리별 mip 샘플링 검토. | 기존 HZB의 reduction 의미를 확인해야 한다. AO 전체 비용과 얇은 물체/이동 경계 품질 비교. |
| 3 | GBuffer+Depth 4.985 ms | [Content/Shaders/project_hs.hlsl:814](C:/Users/crapa/.codex/worktrees/cpu-gpu-utilization/Project-HS/Content/Shaders/project_hs.hlsl:814) 기본 6개 출력과 조건부 sidecar. [Source/RendererD3D12/Private/renderer_pipeline.cpp:760](C:/Users/crapa/.codex/worktrees/cpu-gpu-utilization/Project-HS/Source/RendererD3D12/Private/renderer_pipeline.cpp:760)에서 실제 MRT 구성. material/terrain/foliage별 비용, 픽셀 shader·bandwidth·overdraw 원인을 먼저 분리한다. | 타이밍만으로 bandwidth 병목 확정 불가. sidecar는 이미 조건부이므로 무조건 제거하지 않는다. opaque front-to-back/선별 depth prepass는 추가 draw비까지 포함 비교. foliage alpha·motion/history·재질 데이터 보존. |
| 4 | VSM ResidentAndDirty 1.513 ms | [Content/Shaders/virtual_shadow.hlsl:225](C:/Users/crapa/.codex/worktrees/cpu-gpu-utilization/Project-HS/Content/Shaders/virtual_shadow.hlsl:225) phase 1은 단일 GPU thread allocator. 두 dispatch를 분리 계측한 뒤 병렬화 가치 판단. 이미 변경 static caster만 순회하므로 그 최적화를 다시 제안하지 않는다. | ascending allocation·resident 비축출·physical ownership·UAV 순서 보존. cold/warm/budget 부족/epoch 변경에서 mapping oracle 비교. |
| 5 | VSM Admit 0.582 ms | [Content/Shaders/virtual_shadow.hlsl:286](C:/Users/crapa/.codex/worktrees/cpu-gpu-utilization/Project-HS/Content/Shaders/virtual_shadow.hlsl:286) segment마다 전체 entries 순회. 1번의 정렬된 활성 목록 공유를 먼저 검토. | 무순서 압축은 수용 결과를 바꾼다. oversized 첫 페이지 거부 후 작은 페이지 수용 동작을 보존. |
| 6 | CSM 3.576 ms | [Source/RendererD3D12/Private/renderer_frame.cpp:5313](C:/Users/crapa/.codex/worktrees/cpu-gpu-utilization/Project-HS/Source/RendererD3D12/Private/renderer_frame.cpp:5313) cascade별 draw 비용 분리 후 caster LOD/culling 검토. [Content/Shaders/project_hs.hlsl:1338](C:/Users/crapa/.codex/worktrees/cpu-gpu-utilization/Project-HS/Content/Shaders/project_hs.hlsl:1338)의 VSM fallback이라 삭제 불가. | volumetrics도 fallback 사용. camera-only culling 금지. cache cold·clipmap 경계·VSM budget 부족에서 그림자 누락 확인. 해상도 감소는 품질 변경으로 별도 비교. |
| 7 | CPU graph build 4.807 / submit 3.568 ms | [Source/RendererD3D12/Private/render_graph.cpp:1728](C:/Users/crapa/.codex/worktrees/cpu-gpu-utilization/Project-HS/Source/RendererD3D12/Private/render_graph.cpp:1728) Enhanced command list 묶음 제출은 이미 구현됨. 호출 수/Wait/Signal/retention 비용을 나눠 남는 과도한 경계의 원인을 확인. | Legacy decay 및 외부 provider fence 경계 보존. 막연한 묶음 제출 재구현·fence 삭제 금지. 동기화 변경 시 native GPU validation 필수. |
| 8 | CPU constants_upload 2.284 / Present 12.285 ms | [Source/RendererD3D12/Private/renderer_frame.cpp:4256](C:/Users/crapa/.codex/worktrees/cpu-gpu-utilization/Project-HS/Source/RendererD3D12/Private/renderer_frame.cpp:4256) 측정 범위가 upload만을 의미하지 않는다. upload bytes·준비 단계, Present의 FG/pacing 대기를 먼저 분리. | Present 시간은 제거 가능한 CPU 계산 시간으로 볼 수 없다. FG SDK 내부 비용 및 Copy 시간 미측정 한계를 유지. |

공식 자료와의 대조:

- Intel XeGTAO는 view-space depth 변환과 depth mip chain 전처리를 분리한다. 현재 GTAO의 반복 복원/원거리 depth 접근을 개선할 비교 설계로 적합하다. 프로젝트는 보관 상태이므로 신규 의존성으로 바로 추가하기보다 알고리즘을 참고한다. 샘플의 다른 GPU/장면 타이밍을 이 프로젝트의 보장값으로 사용하지 않았다. [XeGTAO 구현 설명](https://github.com/GameTechDev/XeGTAO#implementation-and-integration-overview)
- NVIDIA는 Early-Z 보존과 shader의 불필요한 작업 감소를 권고한다. GBuffer의 선택적 prepass 후보는 이 원칙에서 도출했지만, 실제 overdraw 원인은 아직 미확정이다. [NVIDIA shader 성능 지침](https://developer.nvidia.com/blog/advanced-api-performance-shaders/)
- Microsoft는 여러 command list를 같은 호출에 제출하는 경우와 별도 호출 사이의 실행 보장을 구분한다. 현 구현의 barrier/Wait/Signal 경계를 보존하면서 묶음 가능성을 판단해야 한다. [ExecuteCommandLists 계약](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-executecommandlists)

## 검증 범위

최신 분석기로 원본 재분석, GPU/CPU 각각 120개 패스와 52개 옵션 수 확인, 원본 57개 파일 SHA-256 전후 일치 확인. `source-integrity.json`에 보존 증거를 저장했다. 전체 상세 데이터는 `ANALYZER_REPORT.md`, `GPU_PASS_TIMES.csv`, `CPU_PASS_TIMES.csv`, `summary.json`, `option-costs.json`이다.

코드 변경·게임 실행·새 측정·빌드/네이티브 검증은 하지 않았다. 현재 빌드의 실제 개선량과 144 FPS 달성 여부는 이 과거 기록만으로 확인할 수 없다.
