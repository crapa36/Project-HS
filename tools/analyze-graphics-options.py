"""Attribute one completed GPU capture to graphics feature groups; never infer toggle savings."""
import argparse
from collections import defaultdict
import csv
from itertools import groupby
import json
import math
from pathlib import Path
import re
import tempfile

# Original 56-option capture schema; read-only compatibility for recordings made before RT removal.
LEGACY_OPTIONS = {
    "preset": ("configuration", (), "Quality preset selects several controls; no separate GPU pass."),
    "display_mode": ("presentation", (), "Window/presentation policy; no isolated graph pass."),
    "display_output": ("presentation", (), "Monitor selection; no isolated graph pass."),
    "output_extent": ("shared", ("surface", "lighting", "reconstruction", "post"), "Output size affects several stages."),
    "vsync": ("presentation", (), "Presentation pacing; compare QPC intervals and Present CPU wall time."),
    "frame_limit_fps": ("presentation", (), "CPU pacing limit; no GPU work pass."),
    "hdr_mode": ("shared", ("post",), "HDR output shares color conversion and presentation."),
    "hdr_paper_white_nits": ("shared", ("post",), "Color parameter inside shared output shaders."),
    "hdr_peak_nits": ("shared", ("post",), "Color parameter inside shared output shaders."),
    "reconstruction_provider": ("shared_policy", ("reconstruction",), "Active provider's reconstruction group; mutually exclusive providers are not compared."),
    "reconstruction_quality": ("shared_policy", ("reconstruction", "surface", "lighting"), "Provider quality also changes render resolution."),
    "resolution_mode": ("shared_policy", ("surface", "lighting", "reconstruction"), "Resolution policy affects most image stages."),
    "manual_scale_percent": ("shared_policy", ("surface", "lighting", "reconstruction"), "Resolution affects most image stages."),
    "dynamic_resolution": ("configuration", (), "Resolution controller; no separate GPU pass."),
    "target_render_fps": ("configuration", (), "Dynamic resolution controller target."),
    "dynamic_min_percent": ("configuration", (), "Dynamic resolution controller bound."),
    "dynamic_max_percent": ("configuration", (), "Dynamic resolution controller bound."),
    "sharpening": ("shared", ("reconstruction", "post"), "Parameter inside reconstruction/output shader."),
    "frame_generation_provider": ("external", ("frame_generation_guides",), "Graph timestamps cover prepare/guides only; SDK-generated frames outside graph are unmeasured."),
    "frame_generation_mode": ("external", ("frame_generation_guides",), "Generated presentation rate is separate from base render FPS."),
    "frame_generation_multiplier": ("external", ("frame_generation_guides",), "No total SDK generation GPU duration from current graph timestamps."),
    "low_latency": ("presentation", (), "Native scheduling/input latency control; no isolated graph pass."),
    "texture_quality": ("shared", ("texture_streaming", "surface", "transparency"), "Texture sampling is shared with surface/material shading; uploads are transient."),
    "texture_budget_mib": ("resource", ("texture_streaming",), "Residency budget; upload cost is not the total texture shading cost."),
    "anisotropic_filtering": ("shared", ("surface", "transparency", "shadows"), "Sampler setting inside shared draw passes."),
    "geometry_quality": ("shared", ("geometry", "surface", "shadows", "ray_scene"), "LOD/geometry affects several stages; geometry group is preparation only."),
    "foliage_quality": ("shared", ("geometry", "surface", "shadows"), "Density and draw cost share scene passes."),
    "material_quality": ("shared", ("surface", "lighting", "subsurface"), "Material choices share surface and lighting shaders."),
    "pom_quality": ("shared", ("surface",), "Parallax work inside surface shader; no exclusive timing."),
    "lighting_quality": ("shared", ("lighting", "volumetrics", "light_lists"), "Several light/shading quality controls share groups."),
    "shadow_mode": ("feature", ("shadows",), "Directional/local/virtual/ray shadow work; shared shadow controls reference this same group."),
    "shadow_quality": ("feature", ("shadows",), "Same shadow group as mode/distance, not an independent additive cost."),
    "shadow_distance_m": ("feature", ("shadows",), "Same shadow group; no marginal distance comparison in a single capture."),
    "contact_shadows": ("feature", ("contact_shadows",), "Dedicated contact shadow pass."),
    "ao_mode": ("feature", ("ao",), "Active GTAO/RTAO group."),
    "ao_quality": ("feature", ("ao",), "Same AO group as mode, not an additive cost."),
    "gi_mode": ("feature", ("gi", "shared_probes", "shared_ray_compose"), "Active probe/software/RT GI work; ray-scene/denoising can be shared."),
    "gi_quality": ("feature", ("gi", "shared_probes", "shared_ray_compose"), "Same GI group; shared ray-scene/denoising reported separately."),
    "reflection_mode": ("feature", ("reflections", "shared_probes", "shared_ray_compose"), "Active probe/SSR/RT reflection work; shared probes/denoising reported separately."),
    "reflection_quality": ("feature", ("reflections", "shared_probes", "shared_ray_compose"), "Same reflection group, not an independent additive cost."),
    "ray_tracing_enabled": ("shared", ("ray_scene", "gi", "reflections", "ao", "shadows", "denoising"), "Master switch controls multiple consumers; TLAS/BLAS are shared."),
    "rt_trace_distance_m": ("shared", ("gi", "reflections", "ao", "shadows"), "Distance parameter across multiple ray consumers."),
    "neural_ray_reconstruction": ("feature", ("denoising",), "Active denoiser group can serve multiple ray signals."),
    "ray_reconstruction_provider": ("feature", ("denoising",), "Same shared radiance/occlusion denoiser group; provider alternatives are untested."),
    "transparency_quality": ("feature", ("transparency",), "OIT/transparent drawing; refraction may share composition."),
    "refraction_quality": ("feature", ("refraction",), "Hero peel/composite group; not all transparent shading."),
    "volumetric_quality": ("feature", ("volumetrics",), "Dedicated froxel density/light/filter/resolve passes."),
    "particle_quality": ("shared", ("particles", "transparency", "distortion", "volumetrics"), "Simulation group excludes shared particle rendering/light/volume cost."),
    "bloom": ("feature", ("bloom",), "Dedicated bloom chain."),
    "bloom_strength": ("shared", ("bloom",), "Strength parameter inside same bloom chain; no exclusive cost."),
    "motion_blur": ("feature", ("motion_blur", "shared_post_guides"), "Dedicated motion blur; nearest guides shared with DOF."),
    "depth_of_field": ("feature", ("depth_of_field", "shared_post_guides"), "Dilation/DOF plus shared nearest guides (including fused CoC); legacy captures may have a separate CoC pass. Shared cost is not additive."),
    "outline": ("feature", ("outline",), "Dedicated screen-space outline pass."),
    "outline_width_px": ("shared", ("outline",), "Parameter inside outline pass; no exclusive cost."),
    "color_grading": ("feature", ("color_grading",), "Color grade plus transient LUT initialization."),
    "adaptive_shading": ("shared_policy", ("adaptive", "surface", "ao", "reflections"), "Classification/rate generation is overhead; savings in affected shared passes need a comparison."),
}
# Retain legacy RT mappings only to read original captures made before removal.
LEGACY_RT_KEYS = frozenset(("ray_tracing_enabled", "rt_trace_distance_m", "neural_ray_reconstruction", "ray_reconstruction_provider"))
SETTING_IDS = {key: index for index, key in enumerate(LEGACY_OPTIONS, 1)}
OPTIONS = {key: value for key, value in LEGACY_OPTIONS.items() if key not in LEGACY_RT_KEYS}
OPTIONS.update({
    "geometry_quality": ("shared", ("geometry", "surface", "shadows"), "LOD/geometry affects several stages; geometry group is preparation only."),
    "shadow_mode": ("feature", ("shadows",), "Directional/local/virtual shadow work; shared shadow controls reference this same group."),
    "ao_mode": ("feature", ("ao",), "Active GTAO group."),
    "gi_mode": ("feature", ("gi", "shared_probes"), "Active probe/software GI work; probe updates can be shared with reflections."),
    "gi_quality": ("feature", ("gi", "shared_probes"), "Same GI group; shared probe updates are reported separately."),
    "reflection_mode": ("feature", ("reflections", "shared_probes"), "Active probe/SSR reflection work; shared probe updates are reported separately."),
    "reflection_quality": ("feature", ("reflections", "shared_probes"), "Same reflection group, not an independent additive cost."),
})
CURRENT_OPTION_KEYS = frozenset(OPTIONS)
DEPENDENCIES = {
    "ao_quality": "ao_mode", "gi_quality": "gi_mode", "reflection_quality": "reflection_mode",
    "bloom_strength": "bloom", "outline_width_px": "outline", "frame_generation_multiplier": "frame_generation_mode",
    "ray_reconstruction_provider": "neural_ray_reconstruction", "target_render_fps": "dynamic_resolution",
    "dynamic_min_percent": "dynamic_resolution", "dynamic_max_percent": "dynamic_resolution",
    "frame_generation_provider": "frame_generation_mode", "rt_trace_distance_m": "ray_tracing_enabled",
    "hdr_paper_white_nits": "hdr_mode", "hdr_peak_nits": "hdr_mode",
}


def pass_feature_group(row):
    name, identity = row["pass_name"], int(row["pass_id"])
    if (name.startswith("RenderCriticalPath") or name == "SourceRenderOutputReady"): return None
    if identity == 169 or name == "RayTransportCompose": return "shared_ray_compose"
    if identity in (2, 177) or name.startswith(("VirtualShadow.", "LocalShadow.", "RayShadow.")): return "shadows"
    if name.startswith(("GTAO", "AdaptiveGTAO", "RTAO")): return "ao"
    if name == "ContactShadows": return "contact_shadows"
    if name.startswith(("Brixelizer", "RTGI", "RRDiffuse", "GINRD")) or name == "LightingProbeUpload": return "gi"
    if name.startswith(("SSR", "HybridReflection", "RRSpecular", "RRReflection", "ReflectionNRD")): return "reflections"
    if name.startswith(("LocalProbe",)): return "shared_probes"
    if name.startswith(("FSR RR", "DLSS RR")) or identity >= 3000: return "denoising"
    # Current SceneGeometry preparation is consumed only by SoftwareHybrid GI.
    if name.startswith("SceneGeometry"): return "gi"
    if name.startswith(("RayScene", "RayGeometry", "OpacityMicromap")): return "ray_scene"
    if name.startswith(("HdrPostSignedCoC", "HdrPostForegroundDilation", "HdrPostDepthOfField")): return "depth_of_field"
    if name == "HdrPostMotionBlur": return "motion_blur"
    if name in ("HdrPostColorGrade", "HdrPostLutInitialize"): return "color_grading"
    if name == "HdrPostNearestGuides": return "shared_post_guides"
    if name.startswith("Hero"): return "refraction"
    if name.startswith("Volumetric"): return "volumetrics"
    if name.startswith("Subsurface"): return "subsurface"
    if name.startswith("Bloom"): return "bloom"
    if identity == 15: return "outline"
    if name.startswith("Adaptive."): return "adaptive"
    if name.startswith(("LightList", "LtcLut")): return "light_lists"
    if "Frame Generation" in name or name in ("SDK Display Guides", "SDK Undilated Guides"): return "frame_generation_guides"
    if identity in (9, 49, 51) or "Reconstruction" in name: return "reconstruction"
    if name.startswith(("Texture mip", "Clear actual material feedback", "Decode actual material feedback")): return "texture_streaming"
    if name.startswith(("Cluster", "Hlod")) or identity in (3, 18, 19, 21, 22): return "geometry"
    if identity in (4, 20, 25, 26, 27, 28, 29, 30, 31, 295): return "surface"
    if identity in (7, 8, 290): return "transparency"
    if identity == 1: return "particles"
    if identity == 6: return "distortion"
    if identity == 5: return "lighting"
    if identity in (14, 16, 32, 33, 34, 35, 36, 903): return "post"
    if identity in (17, 23) or name == "SDK DevTools UI": return "ui"
    if identity in (0, 24): return "scene_upload_initialization"
    return "unclassified"


def pass_group(row):
    group = pass_feature_group(row)
    # Copy timestamps can be unsupported. Keep unknown transfers out of measured Direct/Compute feature work.
    return group + "_copy_transfer" if group is not None and int(row.get("queue", -1)) == 2 else group


def read_csv(path):
    with path.open(encoding="utf-8-sig", newline="") as stream:
        for row in csv.DictReader(stream):
            if None in row or any(v is None for v in row.values()): raise ValueError(f"Malformed CSV: {path}")
            yield row


def stats(values):
    values = sorted(values)
    if not values: return None
    if any(not math.isfinite(v) or v < 0 for v in values): raise ValueError("Invalid timing sample")
    def percentile(q): return values[max(0, math.ceil(len(values) * q) - 1)]
    return {"count": len(values), "mean": sum(values) / len(values), "p50": percentile(.5),
            "p95": percentile(.95), "p99": percentile(.99), "max": values[-1]}


def option_status(option, manifest):
    effective = option["effective"]
    dependency = DEPENDENCIES.get(option["key"])
    if not effective: return "effective_unavailable"
    disabled = effective in ("false", "off") or (dependency and manifest[dependency]["effective"] in ("false", "off"))
    if option["requested_available"] == "0":
        return "requested_unavailable_effective_off" if disabled else "requested_unavailable_effective_fallback"
    if disabled: return "off"
    if option["key"] == "adaptive_shading": return "policy"
    return "active"


def analyze(directory, warmup=10, tick_start=0, tick_end=None):
    diagnostics = json.loads((directory / "diagnostics.json").read_text(encoding="utf-8-sig"))
    if not diagnostics.get("complete") or diagnostics.get("io_failed"):
        raise ValueError("Capture diagnostics are incomplete or contain a writer failure")
    manifests = defaultdict(dict)
    for row in read_csv(directory / "graphics_settings.csv"):
        revision, key = int(row["config_revision"]), row["key"]
        if key not in LEGACY_OPTIONS: raise ValueError(f"Unclassified setting: {key}")
        if key in manifests[revision]: raise ValueError("Duplicate setting in configuration revision")
        manifests[revision][key] = row
    if not manifests or any(set(m) not in (CURRENT_OPTION_KEYS, set(LEGACY_OPTIONS)) for m in manifests.values()):
        raise ValueError("Each configuration revision must contain all 52 current or 56 legacy settings")
    frames, selected = {}, set()
    origin = None
    for row in read_csv(directory / "performance.csv"):
        frame, revision = int(row["frame"]), int(row["config_revision"])
        if frame in frames or revision not in manifests: raise ValueError("Duplicate frame or missing configuration revision")
        frequency, qpc = float(row["qpc_frequency"]), float(row["frame_qpc"])
        if frequency <= 0 or not math.isfinite(qpc): raise ValueError("Invalid QPC identity")
        if origin is None: origin = qpc / frequency
        frames[frame] = revision
        if qpc / frequency - origin >= warmup and row["session_phase"] == "1" and int(row["tick"]) >= tick_start and (tick_end is None or int(row["tick"]) < tick_end):
            selected.add(frame)
    costs, critical, counts, names, coverage = defaultdict(list), defaultdict(list), defaultdict(int), defaultdict(set), defaultdict(lambda: [0, 0])
    rejected, missing, last_frame = 0, 0, 0
    for frame, rows in groupby(read_csv(directory / "gpu_live.csv"), lambda r: int(r["completed_frame"])):
        if frame <= last_frame: raise ValueError("GPU frame groups must be unique and increasing")
        last_frame = frame
        rows = list(rows)
        if frame not in frames: missing += 1; continue
        if frame not in selected: continue
        if any(r["fence_completed"] != "1" for r in rows): rejected += 1; continue
        revision = frames[frame]  # Never observed_frame: it may already use a different configuration.
        totals, scheduled, incomplete, identities = defaultdict(float), set(), set(), set()
        for row in rows:
            identity = int(row["pass_id"])
            if identity in identities: raise ValueError("Duplicate pass in completed frame")
            identities.add(identity)
            group = pass_group(row)
            if group is None: continue
            scheduled.add(group)
            names[(revision, group)].add((identity, row["pass_name"]))
            duration = float(row["nanoseconds"]) / 1e6
            frequency = float(row["timestamp_frequency"])
            if row["measured"] != "1" or not math.isfinite(frequency) or frequency <= 0 or duration < 0 or not math.isfinite(duration): incomplete.add(group)
            else: totals[group] += duration
        counts[revision] += 1
        # Keep sparse frame records until groups are known, allowing cache-hit frames with no scheduled work to count as zero.
        costs[revision].append((totals, incomplete))
        for group in scheduled:
            coverage[(revision, group)][0] += 1
            coverage[(revision, group)][1] += group not in incomplete
        if rows[0]["critical_path_valid"] == "1":
            value = float(rows[0]["critical_path_ms"])
            if math.isfinite(value) and value >= 0: critical[revision].append(value)
    configurations = []
    for revision, count in sorted(counts.items()):
        groups = {}
        for rev, group in names:
            if rev != revision: continue
            scheduled, measured = coverage[(revision, group)]
            values = [total.get(group, 0) for total, incomplete in costs[revision] if group not in incomplete]
            groups[group] = {"gpu_work_ms_per_completed_frame": stats(values) if measured else None,
                             "scheduled_frames": scheduled, "fully_measured_scheduled_frames": measured,
                             "measurement_scope": "copy_transfer" if group.endswith("_copy_transfer") else "direct_compute_passes",
                             "excludes_copy_transfer": (revision, group + "_copy_transfer") in names,
                             "passes": [{"id": i, "name": n} for i, n in sorted(names[(revision, group)])]}
        options = []
        manifest = manifests[revision]
        catalog = LEGACY_OPTIONS if LEGACY_RT_KEYS & manifest.keys() else OPTIONS
        for key, row in sorted(manifest.items(), key=lambda item: int(item[1]["setting_id"])):
            kind, linked, note = catalog[key]
            status = option_status(row, manifest)
            measured = [g for g in linked if groups.get(g, {}).get("gpu_work_ms_per_completed_frame")]
            cost_status = status
            if status == "active":
                if kind in ("shared", "shared_policy"): cost_status = kind
                elif kind == "external": cost_status = "external_partial"
                elif kind == "feature": cost_status = "measured_group" if measured else "no_measured_dedicated_pass"
                else: cost_status = kind
            actual_stage = None
            if key == "adaptive_shading":
                adaptive_group = groups.get("adaptive", {})
                actual_stage = {"native_effective": row["native_effective"],
                                "scheduled_frames": adaptive_group.get("scheduled_frames", 0),
                                "measured_gpu_work_available": bool(adaptive_group.get("gpu_work_ms_per_completed_frame"))}
                if status == "policy":
                    cost_status = "policy_with_stage_evidence" if actual_stage["scheduled_frames"] else "policy_no_stage_evidence"
            options.append({**row, "kind": kind, "cost_status": cost_status, "feature_groups": linked,
                            "actual_stage_availability": actual_stage,
                            "measured_groups": [] if status in ("off", "requested_unavailable_effective_off", "effective_unavailable") else measured,
                            "feature_group_gpu_work_ms": {g: groups[g]["gpu_work_ms_per_completed_frame"] for g in measured}
                                if kind == "feature" and status not in ("off", "requested_unavailable_effective_off", "effective_unavailable") else {},
                            "isolated_option_ms": None, "note": note})
        ranked = sorted(({"group": g, **data, "option_keys": [key for key, (_, linked, _) in catalog.items() if key in manifest and g in linked]}
                         for g, data in groups.items() if data["gpu_work_ms_per_completed_frame"]),
                        key=lambda row: row["gpu_work_ms_per_completed_frame"]["mean"], reverse=True)
        copy_transfers = [{"group": group, **data} for group, data in groups.items() if group.endswith("_copy_transfer")]
        configurations.append({"config_revision": revision, "completed_frames": count,
                               "gpu_critical_path_ms": stats(critical[revision]), "options": options,
                               "gpu_work_partial": any(data["fully_measured_scheduled_frames"] < data["scheduled_frames"] for data in groups.values()),
                               "copy_transfer_groups": copy_transfers,
                               "ranked_feature_groups": ranked})
    primary = max(configurations, key=lambda c: c["completed_frames"], default=None)
    return {"schema": 1, "source": str(directory.resolve()), "selection": {"warmup_seconds": warmup, "playing_only": True, "tick_start": tick_start, "tick_end_exclusive": tick_end},
            "primary_config_revision": primary["config_revision"] if primary else None,
            "cpu_frames_selected": len(selected), "gpu_frames_missing_cpu_identity": missing,
            "gpu_frames_incomplete_rejected": rejected, "configurations": configurations,
            "limits": ["One configuration run measures active feature work, not marginal option savings or unselected providers.",
                       "GPU work sums pass durations inside a feature group. Async queue overlap means these sums are not elapsed frame time or GPU busy percentage.",
                       "Feature groups are shared by multiple controls; do not add the same group once per option.",
                       "Measured Direct/Compute feature work excludes separate Copy transfers. Unsupported/unmeasured Copy work remains N/A, so overall GPU work is partial.",
                       "Canonical reconstruction/adaptive settings describe policy. Native status and scheduled stage evidence are separate from policy and do not prove marginal savings.",
                       "Critical path is reported independently. Off/unavailable/unscheduled work has no claimed isolated timing.",
                       "Frame generation prepare/guides exclude SDK-generated work and generated output FPS.",
                       "Use the separate QPC frame report and per-process GPU sampler for base FPS and utilization."]}


def self_test():
    assert len(OPTIONS) == 52 and len(LEGACY_OPTIONS) == 56
    registry = Path(__file__).resolve().parents[1] / "Source/Core/Public/hs/core/graphics_settings.def"
    keys = set(re.findall(r"HS_GRAPHICS_SETTING\(\d+,\s*\w+,\s*(\w+),", registry.read_text(encoding="utf-8")))
    assert len(CURRENT_OPTION_KEYS) == 52
    assert keys == CURRENT_OPTION_KEYS, keys ^ CURRENT_OPTION_KEYS
    assert stats([0, 1, 2, 9])["p95"] == 9
    assert stats([]) is None
    try: stats([float("nan")]); assert False
    except ValueError: pass
    assert pass_group({"pass_name": "GBuffer+Depth", "pass_id": "4"}) == "surface"
    assert pass_group({"pass_name": "RenderCriticalPathEnd", "pass_id": "38"}) is None
    assert pass_group({"pass_name": "SourceRenderOutputReady", "pass_id": "38"}) is None
    assert pass_group({"pass_name": "Texture mip map/upload", "pass_id": "1000"}) == "texture_streaming"
    assert pass_group({"pass_name": "REBLUR temporal accumulation", "pass_id": "3000"}) == "denoising"
    assert pass_group({"pass_name": "RayTransportCompose", "pass_id": "169"}) == "shared_ray_compose"
    assert "shared_ray_compose" in LEGACY_OPTIONS["gi_mode"][1] and "shared_ray_compose" in LEGACY_OPTIONS["reflection_mode"][1]
    assert "shared_post_guides" in OPTIONS["motion_blur"][1] and "shared_post_guides" in OPTIONS["depth_of_field"][1]
    for key, dependency in (("frame_generation_provider", "frame_generation_mode"),
                            ("rt_trace_distance_m", "ray_tracing_enabled"),
                            ("hdr_paper_white_nits", "hdr_mode"), ("hdr_peak_nits", "hdr_mode")):
        assert option_status({"key": key, "effective": "auto", "requested_available": "1"},
                             {dependency: {"effective": "off"}}) == "off"
    for token in ("auto", "on"):
        assert option_status({"key": "adaptive_shading", "effective": token, "requested_available": "1"}, {}) == "policy"
    for key in ("reconstruction_provider", "reconstruction_quality", "resolution_mode", "manual_scale_percent"):
        assert OPTIONS[key][0] == "shared_policy"
    assert option_status({"key": "ao_mode", "effective": "off", "requested_available": "0"}, {}) == "requested_unavailable_effective_off"
    with tempfile.TemporaryDirectory() as temp:
        directory = Path(temp)
        (directory / "diagnostics.json").write_text('{"complete":true,"io_failed":false}', encoding="utf-8")
        def write(name, rows):
            with (directory / name).open("w", encoding="utf-8", newline="") as stream:
                writer = csv.DictWriter(stream, fieldnames=rows[0].keys()); writer.writeheader(); writer.writerows(rows)
        manifest = []
        for revision in (1, 2):
            for identity, key in enumerate(LEGACY_OPTIONS, 1):
                token = "off" if key in ("ao_mode", "dynamic_resolution", "frame_generation_mode", "ray_tracing_enabled", "hdr_mode") else "auto" if key == "adaptive_shading" else "true"
                manifest.append(dict(config_revision=revision, settings_generation=revision, pipeline_revision=1,
                                     setting_id=identity, key=key, requested=token, effective=token,
                                     requested_available=-1, native_effective=token, native_reason='native, "quoted"'))
        write("graphics_settings.csv", manifest)
        write("performance.csv", [dict(frame=1, config_revision=1, qpc_frequency=1000, frame_qpc=1000, session_phase=1, tick=1),
                                  dict(frame=2, config_revision=2, qpc_frequency=1000, frame_qpc=2000, session_phase=1, tick=2)])
        gpu = []
        for frame in (1, 2):
            for identity, name in ((10, "Bloom Half Extract"), (11, "Bloom Quarter Downsample")):
                gpu.append(dict(completed_frame=frame, observed_frame=2, fence_completed=1, pass_id=identity,
                                pass_name=name, queue=0, timestamp_frequency=1000000, measured=1, nanoseconds=1000000, critical_path_valid=1, critical_path_ms=1))
            for identity, name, queue, measured, frequency, ns in (
                    (2, "3-cascade Directional Shadow", 0, 1, 1000000, 1000000),
                    (147, "VirtualShadow.SnapshotUpload", 2, 0, 0, 0),
                    (40, "ClusterCutCount", 1, 1, 1000000, 200000),
                    (50, "ClusterDemandStatusReadback", 2, 0, 0, 0),
                    (23, "UI Upload", 2, 1, 0, 0),
                    (169, "RayTransportCompose", 0, 1, 1000000, 250000),
                    (110, "HdrPostNearestGuides", 0, 1, 1000000, 125000)):
                gpu.append(dict(completed_frame=frame, observed_frame=2, fence_completed=1, pass_id=identity,
                                pass_name=name, queue=queue, timestamp_frequency=frequency, measured=measured,
                                nanoseconds=ns, critical_path_valid=1, critical_path_ms=1))
            if frame == 2:
                gpu.append(dict(completed_frame=frame, observed_frame=2, fence_completed=1, pass_id=280,
                                pass_name="Adaptive.ClassifyActualGuides", queue=0, timestamp_frequency=1000000,
                                measured=1, nanoseconds=10000, critical_path_valid=1, critical_path_ms=1))
        write("gpu_live.csv", gpu)
        report = analyze(directory, warmup=0)
        assert [c["config_revision"] for c in report["configurations"]] == [1, 2]
        for config in report["configurations"]:
            assert config["gpu_critical_path_ms"]["mean"] == 1
            assert config["ranked_feature_groups"][0]["gpu_work_ms_per_completed_frame"]["mean"] == 2  # Overlap must not replace critical path.
            assert len(config["options"]) == 56
            assert next(o for o in config["options"] if o["key"] == "ao_quality")["cost_status"] == "off"
            assert next(o for o in config["options"] if o["key"] == "pom_quality")["cost_status"] == "shared"
            adaptive = next(o for o in config["options"] if o["key"] == "adaptive_shading")
            assert adaptive["cost_status"] == ("policy_no_stage_evidence" if config["config_revision"] == 1 else "policy_with_stage_evidence")
            assert adaptive["actual_stage_availability"]["measured_gpu_work_available"] == (config["config_revision"] == 2)
            assert next(o for o in config["options"] if o["key"] == "frame_generation_provider")["cost_status"] == "off"
            assert config["gpu_work_partial"]
            measured_groups = {group["group"]: group for group in config["ranked_feature_groups"]}
            assert measured_groups["shadows"]["gpu_work_ms_per_completed_frame"]["mean"] == 1
            assert measured_groups["shadows"]["excludes_copy_transfer"]
            assert measured_groups["geometry"]["gpu_work_ms_per_completed_frame"]["mean"] == .2
            assert measured_groups["shared_ray_compose"]["gpu_work_ms_per_completed_frame"]["mean"] == .25
            assert measured_groups["shared_post_guides"]["gpu_work_ms_per_completed_frame"]["mean"] == .125
            assert all(group["gpu_work_ms_per_completed_frame"] is None for group in config["copy_transfer_groups"])
            assert {group["group"] for group in config["copy_transfer_groups"]} == {"shadows_copy_transfer", "geometry_copy_transfer", "ui_copy_transfer"}
        current_manifest = [row for row in manifest if row["key"] in CURRENT_OPTION_KEYS]
        write("graphics_settings.csv", current_manifest)
        current_report = analyze(directory, warmup=0)
        assert all(len(c["options"]) == 52 for c in current_report["configurations"])
        assert all(not ({"ray_scene", "shared_ray_compose", "denoising"} & set(o["feature_groups"])) for c in current_report["configurations"] for o in c["options"])
        assert all(not (set(g["option_keys"]) & LEGACY_RT_KEYS) for c in current_report["configurations"] for g in c["ranked_feature_groups"])
        write("graphics_settings.csv", current_manifest[:-1])
        try: analyze(directory, warmup=0); assert False
        except ValueError: pass
        write("graphics_settings.csv", manifest)
        gpu[-1]["fence_completed"] = 0; write("gpu_live.csv", gpu)
        assert analyze(directory, warmup=0)["gpu_frames_incomplete_rejected"] == 1
        (directory / "diagnostics.json").write_text('{"complete":false}', encoding="utf-8")
        try: analyze(directory, warmup=0); assert False
        except ValueError: pass
    print("PASS registry/revision identity, shared compose/guides, option policies/dependencies, unknown Copy isolation, incomplete fences and overlapping GPU work")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, nargs="?")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--warmup-seconds", type=float, default=10)
    parser.add_argument("--tick-start", type=int, default=0)
    parser.add_argument("--tick-end", type=int)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test: self_test()
    else:
        if args.directory is None: parser.error("capture directory is required")
        if args.warmup_seconds < 0: parser.error("warmup cannot be negative")
        report = analyze(args.directory, args.warmup_seconds, args.tick_start, args.tick_end)
        text = json.dumps(report, ensure_ascii=False, indent=2)
        if args.output: args.output.write_text(text + "\n", encoding="utf-8")
        else: print(text)
        if not report["configurations"]: raise SystemExit("No completed GPU frames in the selected gameplay range")
