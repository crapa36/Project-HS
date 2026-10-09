#ifndef HS_RAY_NATIVE_TRACE_HLSLI
#define HS_RAY_NATIVE_TRACE_HLSLI
#ifndef HS_RAY_OPACITY_MICROMAPS
#define HS_RAY_OPACITY_MICROMAPS 0
#endif
#ifndef HS_RAY_SER
#define HS_RAY_SER 0
#endif
#if HS_RAY_OPACITY_MICROMAPS
#define HS_RAY_QUERY_OMM_FLAGS , RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS
#else
#define HS_RAY_QUERY_OMM_FLAGS
#endif
// This expansion is used only in the raygeneration variant, after the exact
// candidate/closest/unsupported-interface decision and before secondary shading.
// HitObject remains local; no memory, wave-lane or payload assumption crosses it.
#if HS_RAY_SER
#define HS_RAY_REORDER(query) { dx::HitObject hsReorderHit=dx::HitObject::FromRayQuery(query); dx::MaybeReorderThread(hsReorderHit); }
#else
#define HS_RAY_REORDER(query)
#endif
#endif
