Capture analysis: partial

Runtime/own-frame GPU coverage complete: True. CPU frames 1571; GPU completed frames 1571.

Playing contiguous same-phase/config FPS: 32.943; Playing frames: 742.
Overall contiguous same-phase/config FPS: 32.371; raw adjacent QPC throughput: 31.826.

Playing GPU critical path mean: 27.101 ms; overall 27.965 ms. This is not GPU busy percentage or an FPS estimate.

CPU stage wall time (Playing only)

| Stage | Count | Mean ms | Min ms | Max ms |
|---|---:|---:|---:|---:|
| render | 742 | 28.784 | 14.252 | 113.301 |
| present | 742 | 12.285 | 0.081 | 41.363 |
| graph_build | 742 | 4.807 | 4.199 | 24.133 |
| graph_submit | 742 | 3.568 | 3.120 | 5.989 |
| constants_upload | 742 | 2.284 | 1.833 | 5.724 |
| adapter | 742 | 2.072 | 0.148 | 413.630 |
| scene_prepare | 742 | 1.457 | 0.981 | 5.917 |
| graph_record | 742 | 1.321 | 1.078 | 11.133 |
| graph_prepare | 742 | 0.982 | 0.875 | 1.598 |
| graph_plan | 742 | 0.571 | 0.302 | 78.130 |
| streaming | 742 | 0.407 | 0.244 | 2.425 |
| ui | 742 | 0.316 | 0.000 | 8.559 |
| wrapper | 742 | 0.289 | 0.132 | 29.998 |
| wait | 742 | 0.247 | 0.004 | 11.679 |
| post_submit | 742 | 0.053 | 0.043 | 0.237 |
| diagnostics | 742 | 0.053 | 0.038 | 0.248 |
| descriptors | 742 | 0.052 | 0.043 | 0.178 |

Streamline SDK-reported presentation throughput (separate from base FPS)

Counts come from numFramesActuallyPresented over their native GetState QPC windows. They are not physical display cadence, nominal multiplication, FSR generated frames or XeSS queued frames. Transition, overlapping and out-of-frame windows are excluded; unavailable data remains N/A.

| Scope | Base QPC FPS | SDK-reported present FPS | Actual SDK count | Observed query seconds | Observations | Excluded / invalid |
|---|---:|---:|---:|---:|---:|---:|
| Playing | 32.943 | 65.859 | 1474 | 22.381 | 740 | 0 / 0 |
| overall | 32.371 | 65.859 | 1474 | 22.381 | 740 | 0 / 0 |
| Playing config 1 | N/A | N/A | N/A | N/A | 0 | 0 / 0 |
| Playing config 2 | N/A | N/A | N/A | N/A | 0 | 0 / 0 |
| Playing config 3 | 32.943 | 65.859 | 1474 | 22.381 | 740 | 0 / 0 |
| Playing config 4 | N/A | N/A | N/A | N/A | 0 | 0 / 0 |

CPU graph subsets (Playing only; included in parent stage)

These values are already inside graph_build/graph_prepare and must not be added to their totals. Unattributed is the per-frame parent minus all named subsets; it may include cleanup after the detailed scopes. Missing older columns remain N/A.

| Parent stage | Subset | Count | Mean ms | Min ms | Max ms |
|---|---|---:|---:|---:|---:|
| graph_prepare | graph_prepare_validation | 742 | 0.077 | 0.064 | 0.194 |
| graph_prepare | graph_prepare_info_key | 742 | 0.270 | 0.231 | 0.563 |
| graph_prepare | graph_prepare_compile | 742 | 0.598 | 0.534 | 1.212 |
| graph_prepare | graph_prepare_transient | 742 | 0.018 | 0.011 | 0.207 |
| graph_prepare | unattributed residual | 742 | 0.018 | 0.014 | 0.105 |

CPU graph subsets (overall; included in parent stage)

These values are already inside graph_build/graph_prepare and must not be added to their totals. Unattributed is the per-frame parent minus all named subsets; it may include cleanup after the detailed scopes. Missing older columns remain N/A.

| Parent stage | Subset | Count | Mean ms | Min ms | Max ms |
|---|---|---:|---:|---:|---:|
| graph_prepare | graph_prepare_validation | 1571 | 0.081 | 0.064 | 0.279 |
| graph_prepare | graph_prepare_info_key | 1571 | 0.277 | 0.229 | 0.806 |
| graph_prepare | graph_prepare_compile | 1571 | 0.591 | 0.515 | 1.263 |
| graph_prepare | graph_prepare_transient | 1571 | 0.020 | 0.011 | 2.219 |
| graph_prepare | unattributed residual | 1571 | 0.018 | 0.013 | 0.204 |

Session phase / foreground breakdown

| Phase | Foreground | Frames | QPC interval seconds | FPS | GPU critical mean ms |
|---|---|---:|---:|---:|---:|
| main_menu | all | 722 | 22.134 | 32.484 | 28.488 |
| main_menu | False | 506 | 15.813 | 31.873 | 28.596 |
| main_menu | True | 216 | 6.292 | 34.014 | 28.236 |
| playing | all | 742 | 22.463 | 32.943 | 27.101 |
| playing | True | 742 | 22.463 | 32.943 | 27.101 |
| defeat | all | 107 | 3.748 | 28.280 | 30.425 |
| defeat | True | 107 | 3.748 | 28.280 | 30.425 |

CPU stage wall time (overall; dominated by menu time)

| Stage | Count | Mean ms | Min ms | Max ms |
|---|---:|---:|---:|---:|
| render | 1571 | 28.247 | 14.252 | 695.914 |
| present | 1571 | 5.965 | 0.081 | 41.363 |
| graph_build | 1571 | 4.861 | 4.178 | 37.069 |
| wait | 1571 | 4.850 | 0.002 | 20.525 |
| graph_submit | 1571 | 3.882 | 3.110 | 46.985 |
| adapter | 1571 | 3.023 | 0.115 | 413.630 |
| constants_upload | 1571 | 2.522 | 1.713 | 216.262 |
| graph_record | 1571 | 1.362 | 1.049 | 11.133 |
| scene_prepare | 1571 | 1.352 | 0.981 | 37.916 |
| graph_plan | 1571 | 1.132 | 0.296 | 352.576 |
| graph_prepare | 1571 | 0.987 | 0.842 | 3.577 |
| streaming | 1571 | 0.507 | 0.230 | 61.143 |
| wrapper | 1571 | 0.233 | 0.132 | 29.998 |
| ui | 1571 | 0.175 | 0.000 | 12.362 |
| post_submit | 1571 | 0.059 | 0.043 | 1.563 |
| descriptors | 1571 | 0.055 | 0.043 | 0.407 |
| diagnostics | 1571 | 0.049 | 0.032 | 0.248 |

Heaviest measured GPU passes (Playing only; ordered by amortized work)

Scheduled mean is per invocation. Amortized work includes no-pass frames as zero only when every scheduled instance was measured. GPU queues overlap; values are not isolated option savings.

| ID | Pass | Scheduled mean ms | Amortized ms / completed frame | Scheduled / completed |
|---:|---|---:|---:|---:|
| 4 | GBuffer+Depth | 4.985 | 4.985 | 742 / 742 |
| 45 | AdaptiveGTAOHorizon | 3.894 | 3.894 | 742 / 742 |
| 2 | 3-cascade Directional Shadow | 3.576 | 3.576 | 742 / 742 |
| 150 | VirtualShadow.ResidentAndDirty | 1.513 | 1.513 | 742 / 742 |
| 224 | VirtualShadow.CountPageDraws | 1.465 | 1.465 | 742 / 742 |
| 5 | Deferred Cel Lighting | 1.247 | 1.247 | 742 / 742 |
| 40 | ClusterCutCount | 0.764 | 0.764 | 742 / 742 |
| 151 | VirtualShadow.CullAndCommands | 0.730 | 0.730 | 742 / 742 |
| 44 | ClusterCutEmit | 0.682 | 0.682 | 742 / 742 |
| 108 | HeroLayerComposite | 0.669 | 0.669 | 742 / 742 |
| 70 | SSRTrace | 0.631 | 0.631 | 742 / 742 |
| 113 | HdrPostForegroundDilation | 0.604 | 0.604 | 742 / 742 |
| 225 | VirtualShadow.AdmitCompletePages | 0.582 | 0.582 | 742 / 742 |
| 153 | VirtualShadow.DynamicBank | 0.492 | 0.492 | 742 / 742 |
| 114 | HdrPostDepthOfField | 0.449 | 0.449 | 742 / 742 |
| 72 | SSRSpatial | 0.447 | 0.447 | 742 / 742 |
| 71 | SSRTemporal | 0.431 | 0.431 | 742 / 742 |
| 3 | Static Visibility | 0.386 | 0.386 | 742 / 742 |
| 46 | GTAOTemporal | 0.264 | 0.264 | 742 / 742 |
| 171 | VolumetricLighting | 0.236 | 0.236 | 742 / 742 |

Periodic local-probe GPU work (Playing only)

| ID | Pass | Scheduled mean ms | Amortized ms / completed frame | Scheduled / completed |
|---:|---|---:|---:|---:|
| 177 | LocalProbeStaticDirectionalShadow | 2.535 | 0.014 | 4 / 742 |
| 180 | LocalProbeStaticCubeCapture | 0.830 | 0.004 | 4 / 742 |
| 185 | LocalProbeStaticCubeCapture | 0.772 | 0.004 | 4 / 742 |
| 181 | LocalProbeStaticCubeCapture | 0.707 | 0.004 | 4 / 742 |
| 184 | LocalProbeStaticCubeCapture | 0.679 | 0.004 | 4 / 742 |
| 183 | LocalProbeStaticCubeCapture | 0.050 | 0.000 | 4 / 742 |
| 182 | LocalProbeStaticCubeCapture | 0.004 | 0.000 | 4 / 742 |

Heaviest CPU pass recording wall time (Playing only)

Parallel pass recordings overlap. Amortized wall values cannot be summed into frame CPU time.

| ID | Pass | Scheduled record mean ms | Amortized record wall ms / completed frame | Record max ms | Scheduled / completed |
|---:|---|---:|---:|---:|---:|
| 40 | ClusterCutCount | 0.396 | 0.396 | 0.905 | 742 / 742 |
| 2 | 3-cascade Directional Shadow | 0.258 | 0.258 | 0.648 | 742 / 742 |
| 4 | GBuffer+Depth | 0.135 | 0.135 | 1.073 | 742 / 742 |
| 20 | GBuffer Retest | 0.121 | 0.121 | 2.400 | 742 / 742 |
| 1 | GPU Particle Spawn/Update | 0.090 | 0.090 | 9.751 | 742 / 742 |
| 22 | Skin Cache | 0.090 | 0.090 | 10.093 | 742 / 742 |
| 0 | Scene Buffer Upload | 0.072 | 0.072 | 9.790 | 742 / 742 |
| 44 | ClusterCutEmit | 0.068 | 0.068 | 0.392 | 742 / 742 |
| 191 | HlodClear | 0.066 | 0.066 | 10.121 | 742 / 742 |
| 902 | SDK DevTools UI | 0.059 | 0.059 | 9.670 | 741 / 742 |
| 190 | HlodUpload | 0.059 | 0.059 | 9.800 | 742 / 742 |
| 7 | Forward Transparent/OIT | 0.058 | 0.058 | 0.847 | 742 / 742 |
| 192 | HlodSelect | 0.057 | 0.057 | 9.783 | 742 / 742 |
| 152 | VirtualShadow.StaticBank | 0.057 | 0.057 | 1.336 | 742 / 742 |
| 153 | VirtualShadow.DynamicBank | 0.054 | 0.054 | 0.395 | 742 / 742 |

Periodic local-probe CPU pass recording wall time (Playing only)

Parallel pass recordings overlap. Amortized wall values cannot be summed into frame CPU time.

| ID | Pass | Scheduled record mean ms | Amortized record wall ms / completed frame | Record max ms | Scheduled / completed |
|---:|---|---:|---:|---:|---:|
| 177 | LocalProbeStaticDirectionalShadow | 2.736 | 0.015 | 3.233 | 4 / 742 |
| 181 | LocalProbeStaticCubeCapture | 1.046 | 0.006 | 1.916 | 4 / 742 |
| 185 | LocalProbeStaticCubeCapture | 0.976 | 0.005 | 1.871 | 4 / 742 |
| 184 | LocalProbeStaticCubeCapture | 0.966 | 0.005 | 1.301 | 4 / 742 |
| 180 | LocalProbeStaticCubeCapture | 0.937 | 0.005 | 1.567 | 4 / 742 |
| 183 | LocalProbeStaticCubeCapture | 0.275 | 0.001 | 0.405 | 4 / 742 |
| 182 | LocalProbeStaticCubeCapture | 0.163 | 0.001 | 0.180 | 4 / 742 |

Heaviest measured GPU passes (overall; unavailable passes remain in GPU_PASS_TIMES.csv)

| ID | Pass | Mean ms | Scheduled / completed |
|---:|---|---:|---:|
| 4 | GBuffer+Depth | 5.168 | 1571 / 1571 |
| 177 | LocalProbeStaticDirectionalShadow | 4.438 | 11 / 1571 |
| 45 | AdaptiveGTAOHorizon | 4.094 | 1571 / 1571 |
| 2 | 3-cascade Directional Shadow | 3.746 | 1571 / 1571 |
| 224 | VirtualShadow.CountPageDraws | 1.543 | 1571 / 1571 |
| 150 | VirtualShadow.ResidentAndDirty | 1.428 | 1571 / 1571 |
| 184 | LocalProbeStaticCubeCapture | 1.406 | 11 / 1571 |
| 5 | Deferred Cel Lighting | 1.257 | 1571 / 1571 |
| 180 | LocalProbeStaticCubeCapture | 1.088 | 11 / 1571 |
| 181 | LocalProbeStaticCubeCapture | 0.990 | 11 / 1571 |
| 185 | LocalProbeStaticCubeCapture | 0.972 | 11 / 1571 |
| 151 | VirtualShadow.CullAndCommands | 0.873 | 1571 / 1571 |
| 40 | ClusterCutCount | 0.804 | 1571 / 1571 |
| 44 | ClusterCutEmit | 0.713 | 1571 / 1571 |
| 70 | SSRTrace | 0.667 | 1571 / 1571 |
| 113 | HdrPostForegroundDilation | 0.632 | 1571 / 1571 |
| 108 | HeroLayerComposite | 0.559 | 1571 / 1571 |
| 225 | VirtualShadow.AdmitCompletePages | 0.552 | 1571 / 1571 |
| 153 | VirtualShadow.DynamicBank | 0.518 | 1571 / 1571 |
| 114 | HdrPostDepthOfField | 0.470 | 1571 / 1571 |

Per-configuration Playing FPS

| Config revision | Playing frames | QPC intervals | Playing FPS | Completed GPU frames |
|---:|---:|---:|---:|---:|
| 1 | 0 | 0 | N/A | 0 |
| 2 | 1 | 0 | N/A | 1 |
| 3 | 741 | 740 | 32.943 | 741 |
| 4 | 0 | 0 | N/A | 0 |

Config 1: heaviest Playing feature groups (shared work, not isolated option cost)

No completed Playing GPU frames in this configuration

Config 2: heaviest Playing feature groups (shared work, not isolated option cost)

| Group | Mean ms / completed frame | Min ms | Max ms | Scheduled / completed | Linked options |
|---|---:|---:|---:|---:|---|
| shadows | 12.686 | 12.686 | 12.686 | 1 / 1 | anisotropic_filtering, geometry_quality, foliage_quality, shadow_mode, shadow_quality, shadow_distance_m |
| ao | 4.660 | 4.660 | 4.660 | 1 / 1 | ao_mode, ao_quality, adaptive_shading |
| surface | 4.620 | 4.620 | 4.620 | 1 / 1 | output_extent, reconstruction_quality, resolution_mode, manual_scale_percent, texture_quality, anisotropic_filtering, geometry_quality, foliage_quality, material_quality, pom_quality, adaptive_shading |
| shared_probes | 2.862 | 2.862 | 2.862 | 1 / 1 | gi_mode, gi_quality, reflection_mode, reflection_quality |
| geometry | 2.054 | 2.054 | 2.054 | 1 / 1 | geometry_quality, foliage_quality |
| reflections | 1.257 | 1.257 | 1.257 | 1 / 1 | reflection_mode, reflection_quality, adaptive_shading |
| depth_of_field | 1.062 | 1.062 | 1.062 | 1 / 1 | depth_of_field |
| volumetrics | 0.945 | 0.945 | 0.945 | 1 / 1 | lighting_quality, volumetric_quality, particle_quality |
| lighting | 0.845 | 0.845 | 0.845 | 1 / 1 | output_extent, reconstruction_quality, resolution_mode, manual_scale_percent, material_quality, lighting_quality |
| contact_shadows | 0.230 | 0.230 | 0.230 | 1 / 1 | contact_shadows |
| motion_blur | 0.191 | 0.191 | 0.191 | 1 / 1 | motion_blur |
| post | 0.173 | 0.173 | 0.173 | 1 / 1 | output_extent, hdr_mode, hdr_paper_white_nits, hdr_peak_nits, sharpening |
| reconstruction | 0.127 | 0.127 | 0.127 | 1 / 1 | output_extent, reconstruction_provider, reconstruction_quality, resolution_mode, manual_scale_percent, sharpening |
| refraction | 0.110 | 0.110 | 0.110 | 1 / 1 | refraction_quality |
| adaptive | 0.106 | 0.106 | 0.106 | 1 / 1 | adaptive_shading |
| outline | 0.097 | 0.097 | 0.097 | 1 / 1 | outline, outline_width_px |
| shared_post_guides | 0.082 | 0.082 | 0.082 | 1 / 1 | motion_blur, depth_of_field |
| transparency | 0.072 | 0.072 | 0.072 | 1 / 1 | texture_quality, anisotropic_filtering, transparency_quality, particle_quality |
| subsurface | 0.072 | 0.072 | 0.072 | 1 / 1 | material_quality |
| bloom | 0.053 | 0.053 | 0.053 | 1 / 1 | bloom, bloom_strength |
| particles | 0.038 | 0.038 | 0.038 | 1 / 1 | particle_quality |
| ui | 0.022 | 0.022 | 0.022 | 1 / 1 |  |
| color_grading | 0.019 | 0.019 | 0.019 | 1 / 1 | color_grading |
| light_lists | 0.016 | 0.016 | 0.016 | 1 / 1 | lighting_quality |
| distortion | 0.003 | 0.003 | 0.003 | 1 / 1 | particle_quality |

Unmeasured Copy transfers (N/A): scene_upload_initialization_copy_transfer, ui_copy_transfer, geometry_copy_transfer, shadows_copy_transfer.

Config 3: heaviest Playing feature groups (shared work, not isolated option cost)

| Group | Mean ms / completed frame | Min ms | Max ms | Scheduled / completed | Linked options |
|---|---:|---:|---:|---:|---|
| shadows | 8.526 | 6.118 | 11.557 | 741 / 741 | anisotropic_filtering, geometry_quality, foliage_quality, shadow_mode, shadow_quality, shadow_distance_m |
| surface | 4.991 | 4.478 | 6.214 | 741 / 741 | output_extent, reconstruction_quality, resolution_mode, manual_scale_percent, texture_quality, anisotropic_filtering, geometry_quality, foliage_quality, material_quality, pom_quality, adaptive_shading |
| ao | 4.468 | 3.097 | 6.282 | 741 / 741 | ao_mode, ao_quality, adaptive_shading |
| geometry | 2.249 | 2.103 | 2.864 | 741 / 741 | geometry_quality, foliage_quality |
| reflections | 1.625 | 1.491 | 2.342 | 741 / 741 | reflection_mode, reflection_quality, adaptive_shading |
| lighting | 1.247 | 0.848 | 1.359 | 741 / 741 | output_extent, reconstruction_quality, resolution_mode, manual_scale_percent, material_quality, lighting_quality |
| refraction | 1.095 | 0.095 | 4.474 | 741 / 741 | refraction_quality |
| depth_of_field | 1.079 | 0.940 | 1.761 | 741 / 741 | depth_of_field |
| volumetrics | 0.955 | 0.858 | 1.054 | 741 / 741 | lighting_quality, volumetric_quality, particle_quality |
| post | 0.251 | 0.142 | 1.021 | 741 / 741 | output_extent, hdr_mode, hdr_paper_white_nits, hdr_peak_nits, sharpening |
| contact_shadows | 0.229 | 0.227 | 0.244 | 741 / 741 | contact_shadows |
| reconstruction | 0.226 | 0.188 | 0.553 | 741 / 741 | output_extent, reconstruction_provider, reconstruction_quality, resolution_mode, manual_scale_percent, sharpening |
| motion_blur | 0.214 | 0.189 | 1.267 | 741 / 741 | motion_blur |
| adaptive | 0.106 | 0.104 | 0.146 | 741 / 741 | adaptive_shading |
| outline | 0.098 | 0.095 | 0.125 | 741 / 741 | outline, outline_width_px |
| shared_post_guides | 0.094 | 0.062 | 0.500 | 741 / 741 | motion_blur, depth_of_field |
| transparency | 0.075 | 0.057 | 0.114 | 741 / 741 | texture_quality, anisotropic_filtering, transparency_quality, particle_quality |
| subsurface | 0.075 | 0.058 | 0.102 | 741 / 741 | material_quality |
| frame_generation_guides | 0.063 | 0.056 | 0.097 | 741 / 741 | frame_generation_provider, frame_generation_mode, frame_generation_multiplier |
| bloom | 0.054 | 0.051 | 0.061 | 741 / 741 | bloom, bloom_strength |
| color_grading | 0.021 | 0.018 | 0.041 | 741 / 741 | color_grading |
| shared_probes | 0.013 | 0.000 | 3.348 | 3 / 741 | gi_mode, gi_quality, reflection_mode, reflection_quality |
| light_lists | 0.009 | 0.007 | 0.040 | 741 / 741 | lighting_quality |
| particles | 0.009 | 0.005 | 0.028 | 741 / 741 | particle_quality |
| distortion | 0.002 | 0.001 | 0.009 | 741 / 741 | particle_quality |
| ui | 0.002 | 0.001 | 0.004 | 741 / 741 |  |

Unmeasured Copy transfers (N/A): scene_upload_initialization_copy_transfer, ui_copy_transfer, geometry_copy_transfer, shadows_copy_transfer.

Config 4: heaviest Playing feature groups (shared work, not isolated option cost)

No completed Playing GPU frames in this configuration

Config 1: all 52 graphics options (Playing)

Linked shared group timings are also shown for shared settings and policies. They repeat the same group work, are never summed, and do not measure option savings. Ordering uses the heaviest linked measured group; isolated cost is always N/A. Off and unobserved groups remain N/A.

| Option key | Requested | Effective | Cost status | Linked shared group work ms / completed frame | Isolated option cost ms |
|---|---|---|---|---|---|
| preset | ultra | ultra | configuration | N/A | N/A |
| display_mode | borderless_fullscreen | borderless_fullscreen | presentation | N/A | N/A |
| display_output | auto | dxgi:000000000000f494:\\.\DISPLAY1 | presentation | N/A | N/A |
| output_extent | 1920x1080 | 1920x1080 | shared | N/A (surface, lighting, reconstruction, post) | N/A |
| vsync | false | false | off | N/A | N/A |
| frame_limit_fps | 0 | 0 | presentation | N/A | N/A |
| hdr_mode | auto | off | off | N/A (post) | N/A |
| hdr_paper_white_nits | 200 | 200 | off | N/A (post) | N/A |
| hdr_peak_nits | auto | 1000 | off | N/A (post) | N/A |
| reconstruction_provider | internal_taau | internal_taau | shared_policy | N/A (reconstruction) | N/A |
| reconstruction_quality | quality | manual | shared_policy | N/A (reconstruction, surface, lighting) | N/A |
| resolution_mode | manual | manual | shared_policy | N/A (surface, lighting, reconstruction) | N/A |
| manual_scale_percent | 100 | 100 | shared_policy | N/A (surface, lighting, reconstruction) | N/A |
| dynamic_resolution | false | false | off | N/A | N/A |
| target_render_fps | 144 | 144 | off | N/A | N/A |
| dynamic_min_percent | 50 | 50 | off | N/A | N/A |
| dynamic_max_percent | 100 | 100 | off | N/A | N/A |
| sharpening | 0.200000003 | 0.200000003 | shared | N/A (reconstruction, post) | N/A |
| frame_generation_provider | dlss | off | off | N/A (frame_generation_guides) | N/A |
| frame_generation_mode | fixed | off | off | N/A (frame_generation_guides) | N/A |
| frame_generation_multiplier | 2 | 2 | off | N/A (frame_generation_guides) | N/A |
| low_latency | on | on | presentation | N/A | N/A |
| texture_quality | ultra | ultra | shared | N/A (texture_streaming, surface, transparency) | N/A |
| texture_budget_mib | 0 | 0 | resource | N/A (texture_streaming) | N/A |
| anisotropic_filtering | 16 | 16 | shared | N/A (surface, transparency, shadows) | N/A |
| geometry_quality | ultra | ultra | shared | N/A (geometry, surface, shadows) | N/A |
| foliage_quality | ultra | ultra | shared | N/A (geometry, surface, shadows) | N/A |
| material_quality | ultra | ultra | shared | N/A (surface, lighting, subsurface) | N/A |
| pom_quality | ultra | ultra | shared | N/A (surface) | N/A |
| lighting_quality | ultra | ultra | shared | N/A (lighting, volumetrics, light_lists) | N/A |
| shadow_mode | raster | raster | no_measured_dedicated_pass | N/A (shadows) | N/A |
| shadow_quality | ultra | ultra | no_measured_dedicated_pass | N/A (shadows) | N/A |
| shadow_distance_m | 160 | 160 | no_measured_dedicated_pass | N/A (shadows) | N/A |
| contact_shadows | true | true | no_measured_dedicated_pass | N/A (contact_shadows) | N/A |
| ao_mode | gtao | gtao | no_measured_dedicated_pass | N/A (ao) | N/A |
| ao_quality | ultra | ultra | no_measured_dedicated_pass | N/A (ao) | N/A |
| gi_mode | software_hybrid | probe | requested_unavailable_effective_fallback | N/A (gi, shared_probes) | N/A |
| gi_quality | ultra | ultra | no_measured_dedicated_pass | N/A (gi, shared_probes) | N/A |
| reflection_mode | screen_space | screen_space | no_measured_dedicated_pass | N/A (reflections, shared_probes) | N/A |
| reflection_quality | ultra | ultra | no_measured_dedicated_pass | N/A (reflections, shared_probes) | N/A |
| transparency_quality | ultra | ultra | no_measured_dedicated_pass | N/A (transparency) | N/A |
| refraction_quality | ultra | ultra | no_measured_dedicated_pass | N/A (refraction) | N/A |
| volumetric_quality | ultra | ultra | no_measured_dedicated_pass | N/A (volumetrics) | N/A |
| particle_quality | ultra | ultra | shared | N/A (particles, transparency, distortion, volumetrics) | N/A |
| bloom | true | true | no_measured_dedicated_pass | N/A (bloom) | N/A |
| bloom_strength | 1 | 1 | shared | N/A (bloom) | N/A |
| motion_blur | high | high | no_measured_dedicated_pass | N/A (motion_blur, shared_post_guides) | N/A |
| depth_of_field | true | true | no_measured_dedicated_pass | N/A (depth_of_field, shared_post_guides) | N/A |
| outline | true | true | no_measured_dedicated_pass | N/A (outline) | N/A |
| outline_width_px | 1 | 1 | shared | N/A (outline) | N/A |
| color_grading | true | true | no_measured_dedicated_pass | N/A (color_grading) | N/A |
| adaptive_shading | auto | auto | policy_no_stage_evidence | N/A (adaptive, surface, ao, reflections) | N/A |

Config 2: all 52 graphics options (Playing)

Linked shared group timings are also shown for shared settings and policies. They repeat the same group work, are never summed, and do not measure option savings. Ordering uses the heaviest linked measured group; isolated cost is always N/A. Off and unobserved groups remain N/A.

| Option key | Requested | Effective | Cost status | Linked shared group work ms / completed frame | Isolated option cost ms |
|---|---|---|---|---|---|
| anisotropic_filtering | 16 | 16 | shared | shadows: 12.686; surface: 4.620; transparency: 0.072 | N/A |
| geometry_quality | ultra | ultra | shared | shadows: 12.686; surface: 4.620; geometry: 2.054 | N/A |
| foliage_quality | ultra | ultra | shared | shadows: 12.686; surface: 4.620; geometry: 2.054 | N/A |
| shadow_mode | raster | raster | measured_group | shadows: 12.686 | N/A |
| shadow_quality | ultra | ultra | measured_group | shadows: 12.686 | N/A |
| shadow_distance_m | 160 | 160 | measured_group | shadows: 12.686 | N/A |
| ao_mode | gtao | gtao | measured_group | ao: 4.660 | N/A |
| ao_quality | ultra | ultra | measured_group | ao: 4.660 | N/A |
| adaptive_shading | auto | auto | policy_with_stage_evidence | ao: 4.660; surface: 4.620; reflections: 1.257; adaptive: 0.106 | N/A |
| output_extent | 1920x1080 | 1920x1080 | shared | surface: 4.620; lighting: 0.845; post: 0.173; reconstruction: 0.127 | N/A |
| reconstruction_quality | quality | manual | shared_policy | surface: 4.620; lighting: 0.845; reconstruction: 0.127 | N/A |
| resolution_mode | manual | manual | shared_policy | surface: 4.620; lighting: 0.845; reconstruction: 0.127 | N/A |
| manual_scale_percent | 100 | 100 | shared_policy | surface: 4.620; lighting: 0.845; reconstruction: 0.127 | N/A |
| texture_quality | ultra | ultra | shared | surface: 4.620; transparency: 0.072 | N/A |
| material_quality | ultra | ultra | shared | surface: 4.620; lighting: 0.845; subsurface: 0.072 | N/A |
| pom_quality | ultra | ultra | shared | surface: 4.620 | N/A |
| gi_mode | software_hybrid | probe | requested_unavailable_effective_fallback | shared_probes: 2.862 | N/A |
| gi_quality | ultra | ultra | measured_group | shared_probes: 2.862 | N/A |
| reflection_mode | screen_space | screen_space | measured_group | shared_probes: 2.862; reflections: 1.257 | N/A |
| reflection_quality | ultra | ultra | measured_group | shared_probes: 2.862; reflections: 1.257 | N/A |
| depth_of_field | true | true | measured_group | depth_of_field: 1.062; shared_post_guides: 0.082 | N/A |
| lighting_quality | ultra | ultra | shared | volumetrics: 0.945; lighting: 0.845; light_lists: 0.016 | N/A |
| volumetric_quality | ultra | ultra | measured_group | volumetrics: 0.945 | N/A |
| particle_quality | ultra | ultra | shared | volumetrics: 0.945; transparency: 0.072; particles: 0.038; distortion: 0.003 | N/A |
| contact_shadows | true | true | measured_group | contact_shadows: 0.230 | N/A |
| motion_blur | high | high | measured_group | motion_blur: 0.191; shared_post_guides: 0.082 | N/A |
| sharpening | 0.200000003 | 0.200000003 | shared | post: 0.173; reconstruction: 0.127 | N/A |
| reconstruction_provider | internal_taau | internal_taau | shared_policy | reconstruction: 0.127 | N/A |
| refraction_quality | ultra | ultra | measured_group | refraction: 0.110 | N/A |
| outline | true | true | measured_group | outline: 0.097 | N/A |
| outline_width_px | 1 | 1 | shared | outline: 0.097 | N/A |
| transparency_quality | ultra | ultra | measured_group | transparency: 0.072 | N/A |
| bloom | true | true | measured_group | bloom: 0.053 | N/A |
| bloom_strength | 1 | 1 | shared | bloom: 0.053 | N/A |
| color_grading | true | true | measured_group | color_grading: 0.019 | N/A |
| preset | ultra | ultra | configuration | N/A | N/A |
| display_mode | borderless_fullscreen | borderless_fullscreen | presentation | N/A | N/A |
| display_output | auto | dxgi:000000000000f494:\\.\DISPLAY1 | presentation | N/A | N/A |
| vsync | false | false | off | N/A | N/A |
| frame_limit_fps | 0 | 0 | presentation | N/A | N/A |
| hdr_mode | auto | off | off | N/A (post) | N/A |
| hdr_paper_white_nits | 200 | 200 | off | N/A (post) | N/A |
| hdr_peak_nits | auto | 1000 | off | N/A (post) | N/A |
| dynamic_resolution | false | false | off | N/A | N/A |
| target_render_fps | 144 | 144 | off | N/A | N/A |
| dynamic_min_percent | 50 | 50 | off | N/A | N/A |
| dynamic_max_percent | 100 | 100 | off | N/A | N/A |
| frame_generation_provider | dlss | off | off | N/A (frame_generation_guides) | N/A |
| frame_generation_mode | fixed | off | off | N/A (frame_generation_guides) | N/A |
| frame_generation_multiplier | 2 | 2 | off | N/A (frame_generation_guides) | N/A |
| low_latency | on | on | presentation | N/A | N/A |
| texture_budget_mib | 0 | 0 | resource | N/A (texture_streaming) | N/A |

Config 3: all 52 graphics options (Playing)

Linked shared group timings are also shown for shared settings and policies. They repeat the same group work, are never summed, and do not measure option savings. Ordering uses the heaviest linked measured group; isolated cost is always N/A. Off and unobserved groups remain N/A.

| Option key | Requested | Effective | Cost status | Linked shared group work ms / completed frame | Isolated option cost ms |
|---|---|---|---|---|---|
| anisotropic_filtering | 16 | 16 | shared | shadows: 8.526; surface: 4.991; transparency: 0.075 | N/A |
| geometry_quality | ultra | ultra | shared | shadows: 8.526; surface: 4.991; geometry: 2.249 | N/A |
| foliage_quality | ultra | ultra | shared | shadows: 8.526; surface: 4.991; geometry: 2.249 | N/A |
| shadow_mode | raster | raster | measured_group | shadows: 8.526 | N/A |
| shadow_quality | ultra | ultra | measured_group | shadows: 8.526 | N/A |
| shadow_distance_m | 160 | 160 | measured_group | shadows: 8.526 | N/A |
| output_extent | 1920x1080 | 1920x1080 | shared | surface: 4.991; lighting: 1.247; post: 0.251; reconstruction: 0.226 | N/A |
| reconstruction_quality | quality | manual | shared_policy | surface: 4.991; lighting: 1.247; reconstruction: 0.226 | N/A |
| resolution_mode | manual | manual | shared_policy | surface: 4.991; lighting: 1.247; reconstruction: 0.226 | N/A |
| manual_scale_percent | 100 | 100 | shared_policy | surface: 4.991; lighting: 1.247; reconstruction: 0.226 | N/A |
| texture_quality | ultra | ultra | shared | surface: 4.991; transparency: 0.075 | N/A |
| material_quality | ultra | ultra | shared | surface: 4.991; lighting: 1.247; subsurface: 0.075 | N/A |
| pom_quality | ultra | ultra | shared | surface: 4.991 | N/A |
| adaptive_shading | auto | auto | policy_with_stage_evidence | surface: 4.991; ao: 4.468; reflections: 1.625; adaptive: 0.106 | N/A |
| ao_mode | gtao | gtao | measured_group | ao: 4.468 | N/A |
| ao_quality | ultra | ultra | measured_group | ao: 4.468 | N/A |
| reflection_mode | screen_space | screen_space | measured_group | reflections: 1.625; shared_probes: 0.013 | N/A |
| reflection_quality | ultra | ultra | measured_group | reflections: 1.625; shared_probes: 0.013 | N/A |
| lighting_quality | ultra | ultra | shared | lighting: 1.247; volumetrics: 0.955; light_lists: 0.009 | N/A |
| refraction_quality | ultra | ultra | measured_group | refraction: 1.095 | N/A |
| depth_of_field | true | true | measured_group | depth_of_field: 1.079; shared_post_guides: 0.094 | N/A |
| volumetric_quality | ultra | ultra | measured_group | volumetrics: 0.955 | N/A |
| particle_quality | ultra | ultra | shared | volumetrics: 0.955; transparency: 0.075; particles: 0.009; distortion: 0.002 | N/A |
| sharpening | 0.200000003 | 0.200000003 | shared | post: 0.251; reconstruction: 0.226 | N/A |
| contact_shadows | true | true | measured_group | contact_shadows: 0.229 | N/A |
| reconstruction_provider | internal_taau | internal_taau | shared_policy | reconstruction: 0.226 | N/A |
| motion_blur | high | high | measured_group | motion_blur: 0.214; shared_post_guides: 0.094 | N/A |
| outline | true | true | measured_group | outline: 0.098 | N/A |
| outline_width_px | 1 | 1 | shared | outline: 0.098 | N/A |
| transparency_quality | ultra | ultra | measured_group | transparency: 0.075 | N/A |
| frame_generation_provider | dlss | dlss | external | frame_generation_guides: 0.063 | N/A |
| frame_generation_mode | fixed | fixed | external | frame_generation_guides: 0.063 | N/A |
| frame_generation_multiplier | 2 | 2 | external | frame_generation_guides: 0.063 | N/A |
| bloom | true | true | measured_group | bloom: 0.054 | N/A |
| bloom_strength | 1 | 1 | shared | bloom: 0.054 | N/A |
| color_grading | true | true | measured_group | color_grading: 0.021 | N/A |
| gi_mode | software_hybrid | probe | requested_unavailable_effective_fallback | shared_probes: 0.013 | N/A |
| gi_quality | ultra | ultra | measured_group | shared_probes: 0.013 | N/A |
| preset | ultra | ultra | configuration | N/A | N/A |
| display_mode | borderless_fullscreen | borderless_fullscreen | presentation | N/A | N/A |
| display_output | auto | dxgi:000000000000f494:\\.\DISPLAY1 | presentation | N/A | N/A |
| vsync | false | false | off | N/A | N/A |
| frame_limit_fps | 0 | 0 | presentation | N/A | N/A |
| hdr_mode | auto | off | off | N/A (post) | N/A |
| hdr_paper_white_nits | 200 | 200 | off | N/A (post) | N/A |
| hdr_peak_nits | auto | 1000 | off | N/A (post) | N/A |
| dynamic_resolution | false | false | off | N/A | N/A |
| target_render_fps | 144 | 144 | off | N/A | N/A |
| dynamic_min_percent | 50 | 50 | off | N/A | N/A |
| dynamic_max_percent | 100 | 100 | off | N/A | N/A |
| low_latency | on | on | presentation | N/A | N/A |
| texture_budget_mib | 0 | 0 | resource | N/A (texture_streaming) | N/A |

Config 4: all 52 graphics options (Playing)

Linked shared group timings are also shown for shared settings and policies. They repeat the same group work, are never summed, and do not measure option savings. Ordering uses the heaviest linked measured group; isolated cost is always N/A. Off and unobserved groups remain N/A.

| Option key | Requested | Effective | Cost status | Linked shared group work ms / completed frame | Isolated option cost ms |
|---|---|---|---|---|---|
| preset | ultra | ultra | configuration | N/A | N/A |
| display_mode | borderless_fullscreen | borderless_fullscreen | presentation | N/A | N/A |
| display_output | auto | dxgi:000000000000f494:\\.\DISPLAY1 | presentation | N/A | N/A |
| output_extent | 1920x1080 | 1920x1080 | shared | N/A (surface, lighting, reconstruction, post) | N/A |
| vsync | false | false | off | N/A | N/A |
| frame_limit_fps | 0 | 0 | presentation | N/A | N/A |
| hdr_mode | auto | off | off | N/A (post) | N/A |
| hdr_paper_white_nits | 200 | 200 | off | N/A (post) | N/A |
| hdr_peak_nits | auto | 1000 | off | N/A (post) | N/A |
| reconstruction_provider | internal_taau | internal_taau | shared_policy | N/A (reconstruction) | N/A |
| reconstruction_quality | quality | manual | shared_policy | N/A (reconstruction, surface, lighting) | N/A |
| resolution_mode | manual | manual | shared_policy | N/A (surface, lighting, reconstruction) | N/A |
| manual_scale_percent | 100 | 100 | shared_policy | N/A (surface, lighting, reconstruction) | N/A |
| dynamic_resolution | false | false | off | N/A | N/A |
| target_render_fps | 144 | 144 | off | N/A | N/A |
| dynamic_min_percent | 50 | 50 | off | N/A | N/A |
| dynamic_max_percent | 100 | 100 | off | N/A | N/A |
| sharpening | 0.200000003 | 0.200000003 | shared | N/A (reconstruction, post) | N/A |
| frame_generation_provider | dlss | off | off | N/A (frame_generation_guides) | N/A |
| frame_generation_mode | fixed | off | off | N/A (frame_generation_guides) | N/A |
| frame_generation_multiplier | 2 | 2 | off | N/A (frame_generation_guides) | N/A |
| low_latency | on | on | presentation | N/A | N/A |
| texture_quality | ultra | ultra | shared | N/A (texture_streaming, surface, transparency) | N/A |
| texture_budget_mib | 0 | 0 | resource | N/A (texture_streaming) | N/A |
| anisotropic_filtering | 16 | 16 | shared | N/A (surface, transparency, shadows) | N/A |
| geometry_quality | ultra | ultra | shared | N/A (geometry, surface, shadows) | N/A |
| foliage_quality | ultra | ultra | shared | N/A (geometry, surface, shadows) | N/A |
| material_quality | ultra | ultra | shared | N/A (surface, lighting, subsurface) | N/A |
| pom_quality | ultra | ultra | shared | N/A (surface) | N/A |
| lighting_quality | ultra | ultra | shared | N/A (lighting, volumetrics, light_lists) | N/A |
| shadow_mode | raster | raster | no_measured_dedicated_pass | N/A (shadows) | N/A |
| shadow_quality | ultra | ultra | no_measured_dedicated_pass | N/A (shadows) | N/A |
| shadow_distance_m | 160 | 160 | no_measured_dedicated_pass | N/A (shadows) | N/A |
| contact_shadows | true | true | no_measured_dedicated_pass | N/A (contact_shadows) | N/A |
| ao_mode | gtao | gtao | no_measured_dedicated_pass | N/A (ao) | N/A |
| ao_quality | ultra | ultra | no_measured_dedicated_pass | N/A (ao) | N/A |
| gi_mode | software_hybrid | probe | requested_unavailable_effective_fallback | N/A (gi, shared_probes) | N/A |
| gi_quality | ultra | ultra | no_measured_dedicated_pass | N/A (gi, shared_probes) | N/A |
| reflection_mode | screen_space | screen_space | no_measured_dedicated_pass | N/A (reflections, shared_probes) | N/A |
| reflection_quality | ultra | ultra | no_measured_dedicated_pass | N/A (reflections, shared_probes) | N/A |
| transparency_quality | ultra | ultra | no_measured_dedicated_pass | N/A (transparency) | N/A |
| refraction_quality | ultra | ultra | no_measured_dedicated_pass | N/A (refraction) | N/A |
| volumetric_quality | ultra | ultra | no_measured_dedicated_pass | N/A (volumetrics) | N/A |
| particle_quality | ultra | ultra | shared | N/A (particles, transparency, distortion, volumetrics) | N/A |
| bloom | true | true | no_measured_dedicated_pass | N/A (bloom) | N/A |
| bloom_strength | 1 | 1 | shared | N/A (bloom) | N/A |
| motion_blur | high | high | no_measured_dedicated_pass | N/A (motion_blur, shared_post_guides) | N/A |
| depth_of_field | true | true | no_measured_dedicated_pass | N/A (depth_of_field, shared_post_guides) | N/A |
| outline | true | true | no_measured_dedicated_pass | N/A (outline) | N/A |
| outline_width_px | 1 | 1 | shared | N/A (outline) | N/A |
| color_grading | true | true | no_measured_dedicated_pass | N/A (color_grading) | N/A |
| adaptive_shading | auto | auto | policy_no_stage_evidence | N/A (adaptive, surface, ao, reflections) | N/A |

Process CPU: 0.970 cores; machine-normalized weighted usage 4.849%. Logical processors: [20].
Process GPU busiest-engine weighted mean: 64.627%; only the target PID, maximum engine, no engine sum.
System CPU weighted mean: 12.923%; full per-core samples are summarized in summary.json.
Target-PID GPU observed interval coverage: 41.175 / 49.346 seconds; initial observation gap 7.514 seconds is N/A, never zero GPU load.

Playing-only utilization

Contiguous Playing QPC window seconds: 22.960. Only fully contained CPU/PDH intervals and in-window NVIDIA timestamps are included.
Process CPU: 0.841 cores; machine usage 4.203%; sampled 21.135 seconds.
System CPU: 11.323%.
Target-PID GPU busiest engine: 68.709%; sampled 21.666 / 22.960 seconds. Unobserved intervals remain N/A.
Playing NVIDIA device  GPU-bc269b7d-8628-6f57-6a33-88e466eeb257 /  00000000:01:00.0: GPU 92.091%; VRAM 5773.318 MiB (max 5883.000); power 254.532 W (max 262.650). Includes other applications.

NVIDIA whole-device context (includes other applications)

| Device UUID / PCI | GPU mean % | VRAM mean / max MiB | Total MiB | Clock mean MHz | Temperature max C | Power mean / max W |
|---|---:|---:|---:|---:|---:|---:|
|  GPU-bc269b7d-8628-6f57-6a33-88e466eeb257 /  00000000:01:00.0 | 90.458 | 5436.958 / 5883.000 | 16376.000 | 2738.125 | 64.000 | 243.294 / 264.150 |

Issues / missing data

- GPU segment utilization worker partial/counter errors: complete=True, reason=process_exit, counter_errors=2; invalid/warmup intervals excluded
- Playing utilization: GPU segment utilization worker partial/counter errors: complete=True, reason=process_exit, counter_errors=2; invalid/warmup intervals excluded

Interpretation limits

- Build type: Release; D3D12 validation: disabled; GPU validation: disabled. Capture formatting/I/O and external sampling overhead are included. This is instrumented capture behavior, not an uninstrumented FPS result or general option savings.
- Foreground is sampled at each CPU frame; only consecutive frames with the same foreground state contribute to its split FPS. No focus history is inferred between samples.
- FPS uses adjacent CPU frame QPC intervals, never GPU-duration inversion. Same-phase/config FPS excludes transition intervals; raw throughput includes them.
- Streamline reported-present FPS sums actual SDK counts over valid same-phase/config GetState QPC windows. It is separate from base FPS and does not verify physical display cadence. No nominal multiplier is applied; FSR generated/XeSS queued counts are excluded.
- CPU stage and per-pass recording/provider times are wall time. Parallel tasks overlap; their sum is not whole-frame CPU busy time.
- Optional CPU subsets are already included in graph_build/graph_prepare. Unattributed residuals exclude the named subsets and may include cleanup after detailed scopes. Older 66-column captures leave these fields N/A.
- GPU feature work sums pass durations; async queues overlap. Critical path, GPU busy percentage and marginal option savings are distinct.
- Unsupported/unmeasured Copy and unavailable native counters remain N/A. A zero is emitted only for a measured zero or no scheduled group work.
- Per-pass/group summaries use running count/mean/min/max; no p95/p99 estimate is fabricated.
- NVIDIA counters are device-wide. Tracked renderer memory is partial accounting; DXGI local usage/budget is a separate observation.

Files: summary.json, option-costs.json, GPU_PASS_TIMES.csv, CPU_PASS_TIMES.csv. Raw capture CSV/NDJSON/settings receipts remain unchanged.
