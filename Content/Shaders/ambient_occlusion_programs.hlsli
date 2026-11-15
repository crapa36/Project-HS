#ifndef HS_AMBIENT_OCCLUSION_PROGRAMS_HLSLI
#define HS_AMBIENT_OCCLUSION_PROGRAMS_HLSLI

// Versioned products prevent pairing the old matrix constant layout with native coefficients.
// Order matches the native pipeline roles; Main and Adaptive are separate cook families.
#define HS_AO_PROGRAMS(X) \
 X(Horizon,Main,"gtao_horizon_v2_cs.dxil",L"GTAOHorizonCS",L"cs_6_0") \
 X(Temporal,Main,"gtao_temporal_v2_cs.dxil",L"GTAOTemporalCS",L"cs_6_0") \
 X(Bilateral,Main,"gtao_bilateral_v2_cs.dxil",L"GTAOBilateralCS",L"cs_6_0") \
 X(Contact,Main,"contact_shadow_v2_cs.dxil",L"ContactShadowCS",L"cs_6_0") \
 X(Adaptive,Adaptive,"adaptive_gtao_v2_cs.dxil",L"AdaptiveGtaoCS",L"cs_6_0") \
 X(Prepare,Main,"gtao_prepare_inverse_w_v2_cs.dxil",L"AoPrepareInverseWCS",L"cs_6_0")

#endif
