"""Summarize a human play capture without launching the game or assuming replay ticks."""
import argparse
from collections import Counter, defaultdict
import csv
import importlib.util
import heapq
from itertools import groupby
import json
import math
from pathlib import Path
import tempfile

_spec = importlib.util.spec_from_file_location("hs_graphics_options", Path(__file__).with_name("analyze-graphics-options.py"))
graphics = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(graphics)
STAGES = ("adapter", "render", "wait", "streaming", "scene_prepare", "ui", "constants_upload",
          "graph_build", "graph_prepare", "descriptors", "graph_plan", "graph_record", "graph_submit",
          "post_submit", "present", "diagnostics", "wrapper")
# Legacy subsets are read only when their columns are present in historical captures.
SUBSTAGES = {"ray_geometry_register": "graph_build", "ray_omm_prepare_register": "graph_build",
             "ray_scene_register": "graph_build", "graph_prepare_validation": "graph_prepare",
             "graph_prepare_info_key": "graph_prepare", "graph_prepare_compile": "graph_prepare",
             "graph_prepare_transient": "graph_prepare"}
PHASES = ("main_menu", "playing", "paused", "card_selection", "stat_allocation", "relic_selection", "victory", "defeat", "quit_requested")


def number(value):
    try:
        value = float(value)
        return value if math.isfinite(value) else None
    except (ValueError, TypeError): return None


class Stats:
    def __init__(self):
        self.count = self.rejected = 0
        self.total = self.weight = self.weighted = 0.0
        self.minimum = self.maximum = None
    def add(self, value, weight=1.0):
        if value is None or not math.isfinite(value) or value < 0 or not math.isfinite(weight) or weight <= 0:
            self.rejected += 1; return
        self.count += 1; self.total += value; self.weight += weight; self.weighted += value * weight
        self.minimum = value if self.minimum is None else min(self.minimum, value)
        self.maximum = value if self.maximum is None else max(self.maximum, value)
    def result(self):
        return {"available": bool(self.count), "count": self.count, "rejected": self.rejected,
                "mean": self.total / self.count if self.count else None,
                "weighted_mean": self.weighted / self.weight if self.weight else None,
                "min": self.minimum, "max": self.maximum}


def reject_json_constant(value): raise ValueError(f"Nonfinite JSON constant: {value}")


def json_read(path):
    value = json.loads(path.read_text(encoding="utf-8-sig"), parse_constant=reject_json_constant)
    if not isinstance(value, dict): raise ValueError(f"Expected JSON object: {path.name}")
    return value


def csv_rows(path):
    with path.open(encoding="utf-8-sig", newline="") as stream:
        for row in csv.DictReader(stream):
            if None in row or any(v is None for v in row.values()): raise ValueError(f"Malformed CSV: {path.name}")
            yield row


def scopes(row):
    revision = int(row["config_revision"])
    phase = int(row["session_phase"])
    result = ["overall", f"config:{revision}", f"phase:{phase}"]
    if phase == 1: result += ["playing", f"playing_config:{revision}"]
    if row.get("foreground") in ("0", "1"):
        foreground = row["foreground"]
        result += [f"foreground:{foreground}", f"phase_foreground:{phase}:{foreground}", f"config_foreground:{revision}:{foreground}"]
    return result


def frame_scope():
    return {"frames": 0, "intervals": Stats(), "raw_intervals": Stats(), "transitions_excluded": 0,
            "cpu_stages": {key: Stats() for key in STAGES},
            "cpu_substages": {key: Stats() for key in SUBSTAGES},
            "cpu_unattributed": {parent: Stats() for parent in dict.fromkeys(SUBSTAGES.values())},
            "streamline_present": {"observations": 0, "frames": 0, "seconds": 0.0, "excluded": 0, "invalid": 0},
            "slot_wait": Stats(), "phases": Counter(),
            "cached_fields": defaultdict(Stats), "diagnostic_identity_invalid": 0, "tick_min": None, "tick_max": None}


def qpc_seconds(row):
    qpc, frequency = number(row.get("frame_qpc")), number(row.get("qpc_frequency"))
    if qpc is None or frequency is None or qpc < 0 or frequency <= 0: raise ValueError("Invalid frame QPC")
    return qpc / frequency


def interval_result(stats):
    return {"available": bool(stats.count), "frame_intervals": stats.count,
            "wall_seconds": stats.total, "fps": stats.count / stats.total if stats.total > 0 else None,
            "interval_ms": {key: (value * 1000 if value is not None else None) for key, value in stats.result().items()
                            if key in ("mean", "min", "max")}}


def presentation_observation(row):
    if row.get("streamline_actual_present_count_valid") != "1": return None
    count, begin, end, frequency = (number(row.get(key)) for key in (
        "streamline_actual_present_count", "streamline_actual_present_begin_qpc",
        "streamline_actual_present_end_qpc", "streamline_actual_present_qpc_frequency"))
    if (count is None or count < 0 or not count.is_integer() or frequency is None or frequency <= 0
        or begin is None or begin <= 0 or end is None or end <= begin
        or frequency != number(row.get("qpc_frequency"))):
        raise ValueError("Invalid Streamline actual-present counter window")
    return int(count), begin / frequency, end / frequency


def presentation_report(data):
    return {"available": bool(data["observations"]), "observations": data["observations"],
            "sdk_reported_present_count": data["frames"], "observed_query_interval_seconds": data["seconds"],
            "sdk_reported_present_fps": data["frames"] / data["seconds"] if data["seconds"] else None,
            "transition_or_window_observations_excluded": data["excluded"], "invalid_observations": data["invalid"],
            "physical_display_cadence_verified": False,
            "source": "Streamline numFramesActuallyPresented since preceding native GetState; excludes FSR generated/XeSS queued counts"}


def frame_report(data):
    return {"frames": data["frames"], "phase_frames": dict(data["phases"]), "simulation_tick_range": [data["tick_min"], data["tick_max"]],
            "fps_contiguous_same_phase_config": interval_result(data["intervals"]),
            "raw_adjacent_frame_throughput": interval_result(data["raw_intervals"]),
            "transition_intervals_excluded": data["transitions_excluded"],
            "cpu_stage_wall_ms": {key: value.result() for key, value in data["cpu_stages"].items()},
            "cpu_stage_subsets_wall_ms": {key: {"parent_stage": SUBSTAGES[key], **value.result()}
                                          for key, value in data["cpu_substages"].items() if value.count or value.rejected},
            "cpu_stage_unattributed_wall_ms": {key: value.result() for key, value in data["cpu_unattributed"].items() if value.count or value.rejected},
            "streamline_reported_present": presentation_report(data["streamline_present"]),
            "frame_slot_wait_ms": data["slot_wait"].result(),
            "cached_frame_metrics": {key: value.result() for key, value in data["cached_fields"].items()},
            "diagnostic_identity_invalid": data["diagnostic_identity_invalid"]}


class CpuFrameCursor:
    def __init__(self, path): self.rows, self.current = iter(csv_rows(path)), None
    def find(self, identity):
        while self.current is None or int(self.current["frame"]) < identity:
            self.current = next(self.rows, None)
            if self.current is None: return None
        return self.current if int(self.current["frame"]) == identity else None


def elapsed_ms(row, prefix):
    if row.get(prefix + "_valid") != "1": return None
    frequency = number(row.get("cpu_qpc_frequency"))
    begin, end = number(row.get(prefix + "_begin_qpc")), number(row.get(prefix + "_end_qpc"))
    if frequency is None or frequency <= 0 or begin is None or end is None or begin < 0 or end < begin: return None
    return (end - begin) * 1000 / frequency


def pass_stats(): return {"scheduled": 0, "gpu": Stats(), "record": Stats(), "provider": Stats()}


def group_stats(): return {"scheduled": 0, "incomplete": 0, "work": Stats(), "passes": set()}


def group_result(data, total_frames):
    work = data["work"]; count = total_frames - data["incomplete"]
    measured = work.count > 0 and count > 0
    result = {"count": count, "mean": work.total / count,
              "min": 0 if data["scheduled"] < total_frames else work.minimum, "max": work.maximum} if measured else None
    return {"gpu_work_ms_per_completed_frame": result, "scheduled_frames": data["scheduled"],
            "fully_measured_scheduled_frames": work.count,
            "passes": [{"id": p, "name": name} for p, name in sorted(data["passes"])]}


def bound_windows(bounds):
    if bounds is None: return []
    if len(bounds) == 2 and isinstance(bounds[0], (int, float)): return [bounds]
    return bounds


def within_bounds(bounds, start, end):
    return bounds is None or any(begin <= start and end <= finish for begin, finish in bound_windows(bounds))


def append_window(windows, start, end):
    if end <= start: return
    if windows and math.isclose(windows[-1][1], start, rel_tol=0, abs_tol=1e-9): windows[-1][1] = end
    else: windows.append([start, end])


def counter_intervals(rows, bounds):
    previous = None
    for identity, iterator in groupby(rows, lambda row: int(row["sample"])):
        group = list(iterator)
        qpc, frequency = number(group[0].get("qpc")), number(group[0].get("qpc_frequency"))
        if qpc is None or frequency is None or frequency <= 0:
            previous = None; continue
        timestamp = qpc / frequency
        eligible = (previous is not None and identity == previous[0] + 1 and frequency == previous[2]
                    and timestamp > previous[1] and previous[3] and group[0].get("warmup") == "0"
                    and within_bounds(bounds, previous[1], timestamp))
        if eligible: yield group, timestamp - previous[1]
        previous = (identity, timestamp, frequency, group[0].get("warmup") == "0")


def utilization(directory, bounds, issues):
    output = {"available": False}
    try: metadata = json_read(directory / "sampling.json")
    except (OSError, ValueError, KeyError, TypeError) as error:
        issues.append(f"Utilization metadata unavailable: {error}"); return output
    output["sampling_metadata"] = metadata
    if metadata.get("complete") is False or metadata.get("error"):
        issues.append("Utilization sampler reports incomplete/error; valid samples remain usable")
    pid = int(metadata.get("process_id", 0))
    samples, cpu_seconds, wall_seconds, logical_counts = {}, 0.0, 0.0, set()
    process, system, memories = Stats(), Stats(), defaultdict(Stats)
    try:
        for row in csv_rows(directory / "cpu-process.csv"):
            wall, cpu, frequency, qpc = (number(row.get(key)) for key in ("wall_seconds", "cpu_seconds", "qpc_frequency", "qpc"))
            logical = number(row.get("logical_processors"))
            if row.get("warmup") != "0" or wall is None or wall <= 0 or cpu is None or cpu < 0 or frequency is None or frequency <= 0 or qpc is None or logical is None or logical <= 0: continue
            timestamp = qpc / frequency
            if not within_bounds(bounds, timestamp - wall, timestamp): continue
            samples[int(row["sample"])] = wall
            cpu_seconds += cpu; wall_seconds += wall; logical_counts.add(int(logical))
            process.add(100 * cpu / wall / logical, wall)
            system.add(number(row.get("system_cpu_percent")), wall)
            for key in ("working_set_bytes", "private_memory_bytes", "handle_count", "system_busy_core_equivalents"):
                memories[key].add(number(row.get(key)), wall)
    except (OSError, ValueError, KeyError, TypeError) as error: issues.append(f"Process CPU samples partial/unavailable: {error}")
    output.update(available=bool(samples), process_cpu={"samples": len(samples), "sampled_wall_seconds": wall_seconds,
                  "process_cpu_seconds": cpu_seconds, "weighted_core_equivalents": cpu_seconds / wall_seconds if wall_seconds else None,
                  "machine_percent": process.result(), "logical_processors": sorted(logical_counts)},
                  system_cpu_percent=system.result(), process_and_system_metrics={key: value.result() for key, value in memories.items()})
    cores, node_totals = defaultdict(Stats), defaultdict(Stats)
    try:
        for rows, wall in counter_intervals(csv_rows(directory / "cpu-system.csv"), bounds):
            for row in rows:
                if row.get("status") not in ("0", "1"): continue
                instance = row["instance"]; target = node_totals if instance.endswith("_total") else cores
                target[instance].add(number(row.get("utilization_percent")), wall)
    except (OSError, ValueError, KeyError, TypeError) as error: issues.append(f"Per-core CPU samples unavailable: {error}")
    if not samples: issues.append("No valid aligned process CPU utilization samples")
    if not any(data.count for data in cores.values()): issues.append("No valid aligned logical CPU samples")
    output["logical_cpu_percent"] = {key: data.result() for key, data in sorted(cores.items())}
    output["cpu_total_and_group_percent"] = {key: data.result() for key, data in sorted(node_totals.items())}
    output["system_cpu_percent"] = node_totals["_total"].result()
    output["system_cpu_summary_source"] = "Independent PDH sample QPC intervals; cached CPU-row summary percentages are not repeated"
    threads = defaultdict(float)
    try:
        for row in csv_rows(directory / "cpu-threads.csv"):
            value = number(row.get("cpu_seconds"))
            if int(row["sample"]) in samples and value is not None and value >= 0: threads[int(row["thread_id"])] += value
    except (OSError, ValueError, KeyError, TypeError) as error: issues.append(f"Per-thread CPU samples unavailable: {error}")
    output["busiest_threads"] = [{"thread_id": identity, "cpu_seconds": value,
                                    "core_equivalents": value / wall_seconds if wall_seconds else None}
                                   for identity, value in sorted(threads.items(), key=lambda item: item[1], reverse=True)[:20]]
    engines, busiest = defaultdict(Stats), Stats()
    source_paths = [directory] + sorted(path for path in directory.parent.glob("utilization-counter-refresh*") if path.is_dir())
    sources = []
    for path in source_paths:
        source = {"directory": str(path.resolve()), "supplemental": path != directory,
                  "sampled_wall_seconds": 0.0, "accepted_intervals": 0, "overlapping_intervals_excluded": 0, "metadata": {}}
        for filename in ("pdh-worker.json", "sampling.json", "auxiliary-worker.json"):
            if (path / filename).exists():
                try:
                    source["metadata"][filename] = json_read(path / filename)
                    status = source["metadata"][filename]
                    if filename == "pdh-worker.json" and (status.get("complete") is False or status.get("error") or status.get("counter_errors_total", 0)):
                        issues.append(f"GPU segment {path.name} worker partial/counter errors: complete={status.get('complete')}, reason={status.get('reason')}, counter_errors={status.get('counter_errors_total', 0)}; invalid/warmup intervals excluded")
                except (OSError, ValueError, KeyError, TypeError) as error: issues.append(f"GPU segment {path.name} metadata unavailable: {error}")
        sources.append(source)
    def target_intervals(path, source_index):
        try:
            for rows, wall in counter_intervals(csv_rows(path / "gpu-engines.csv"), bounds):
                target = [row for row in rows if int(row["pid"]) == pid and row.get("target_process") == "1"
                          and row.get("status") in ("0", "1") and number(row.get("utilization_percent")) is not None
                          and number(row["utilization_percent"]) >= 0]
                if target:
                    end = number(target[0]["qpc"]) / number(target[0]["qpc_frequency"])
                    yield end, source_index, end - wall, target, wall
        except (OSError, ValueError, KeyError, TypeError) as error:
            issues.append(f"GPU segment {path.name} samples partial/unavailable: {error}")
    previous_end = None
    first_target_observation = None
    for end, source_index, start, rows, wall in heapq.merge(*(target_intervals(path, index) for index, path in enumerate(source_paths))):
        source = sources[source_index]
        # Segments have independent sample IDs. Conservative whole-interval exclusion avoids double weighting overlaps.
        if previous_end is not None and start < previous_end - 1e-9:
            source["overlapping_intervals_excluded"] += 1; continue
        previous_end = end
        if first_target_observation is None: first_target_observation = start
        per_engine = {}
        for row in rows:
            key = (row["luid"], row["phys"], row["engine"], row["engine_type"])
            value = number(row["utilization_percent"])
            per_engine[key] = max(per_engine.get(key, value), value)
        for key, value in per_engine.items(): engines[key].add(value, wall)
        busiest.add(max(per_engine.values()), wall)
        source["sampled_wall_seconds"] += wall; source["accepted_intervals"] += 1
    if not busiest.count: issues.append("No valid aligned target-PID GPU engine samples; GPU utilization is N/A")
    windows = bound_windows(bounds)
    window = sum(end - begin for begin, end in windows) if bounds is not None else None
    gap = max(0, first_target_observation - windows[0][0]) if first_target_observation is not None and windows else None
    if any(source["supplemental"] and source["accepted_intervals"] for source in sources):
        issues.append("Supplemental GPU PID enumeration used; initial target-PID observation gap remains N/A, original System/CPU/NVIDIA data retained")
    output["process_gpu"] = {"target_pid": pid, "busiest_engine_percent": busiest.result(),
        "sampled_wall_seconds": busiest.weight, "frame_window_seconds": window,
        "initial_target_observation_gap_seconds": gap,
        "window_coverage_fraction": busiest.weight / window if window else None, "sources": sources,
        "engines": [{"luid": key[0], "physical_adapter": key[1], "engine": key[2], "engine_type": key[3], "percent": value.result()}
                    for key, value in engines.items()],
        "note": "Original plus optional supplemental GPU-only segments use independent PDH QPC intervals. Intervals before and after every query warmup, gaps and overlaps are N/A; original System/CPU/NVIDIA are not duplicated. Target PID only, maximum engine, no engine sum."}
    devices = defaultdict(lambda: defaultdict(Stats))
    try:
        for row in csv_rows(directory / "nvidia-whole-gpu.csv"):
            frequency, qpc = number(row.get("qpc_frequency")), number(row.get("qpc"))
            if frequency is None or frequency <= 0 or qpc is None: continue
            if not within_bounds(bounds, qpc / frequency, qpc / frequency): continue
            key = (row.get("uuid", ""), row.get("pci_bus_id", ""), row.get("name", ""))
            for field in ("gpu_percent", "memory_controller_percent", "graphics_clock_mhz", "sm_clock_mhz", "memory_clock_mhz",
                          "memory_used_mib", "memory_total_mib", "memory_free_mib", "temperature_c", "power_watts", "power_limit_watts"):
                devices[key][field].add(number(row.get(field)))
    except (OSError, ValueError, KeyError, TypeError) as error: issues.append(f"NVIDIA device samples unavailable: {error}")
    if not devices: issues.append("No valid aligned NVIDIA device samples; device context is N/A")
    output["nvidia_device_context"] = [{"uuid": key[0], "pci_bus_id": key[1], "name": key[2],
        "metrics": {field: data.result() for field, data in values.items()},
        "scope": "Whole physical device, includes other processes; no Windows LUID mapping is assumed from NVIDIA index."}
        for key, values in devices.items()]
    nvidia = metadata.get("nvidia_worker")
    if isinstance(nvidia, dict) and (nvidia.get("complete") is False or nvidia.get("error")):
        issues.append("NVIDIA worker reports incomplete/error; device context is partial")
    return output


def capture_build_limit(provenance):
    session = provenance.get("capture-session.json", {})
    build = session.get("build_type") or "unknown"
    validation = session.get("validation_enabled")
    if build == "Debug" and validation is True:
        return "Debug unoptimized code, D3D12 validation, capture formatting/I/O and external sampling overhead are included. This is captured Debug behavior, not Release FPS or general option savings."
    def state(value): return "enabled" if value is True else "disabled" if value is False else "unknown"
    return (f"Build type: {build}; D3D12 validation: {state(validation)}; GPU validation: {state(session.get('gpu_validation_enabled'))}. "
            "Capture formatting/I/O and external sampling overhead are included. This is instrumented capture behavior, not an uninstrumented FPS result or general option savings.")


def analyze(directory):
    issues, diagnostics, manifests = [], {}, defaultdict(list)
    cpu_analysis_valid = gpu_analysis_valid = settings_valid = True
    try: diagnostics = json_read(directory / "diagnostics.json")
    except (OSError, ValueError, KeyError, TypeError) as error: issues.append(f"Diagnostic status unavailable: {error}")
    if not diagnostics.get("complete") or diagnostics.get("io_failed"): issues.append("Runtime diagnostic capture is incomplete or has writer failures")
    try:
        for row in csv_rows(directory / "graphics_settings.csv"):
            if not {"config_revision", "setting_id", "key", "requested", "effective", "requested_available"}.issubset(row):
                raise ValueError("Missing graphics receipt columns")
            if not 1 <= int(row["setting_id"]) <= 56: raise ValueError("Invalid graphics setting ID")
            manifests[int(row["config_revision"])].append(row)
    except (OSError, ValueError, KeyError, TypeError) as error:
        settings_valid = False; issues.append(f"Graphics settings receipts partial/unavailable: {error}")
    if not manifests: settings_valid = False; issues.append("No graphics settings receipts")
    settings = {}
    for revision, rows in manifests.items():
        unique = {row["key"]: row for row in rows}
        if len(rows) != len(unique) or set(unique) not in (graphics.CURRENT_OPTION_KEYS, set(graphics.LEGACY_OPTIONS)) or any(graphics.SETTING_IDS.get(row["key"]) != int(row["setting_id"]) for row in rows):
            settings_valid = False; issues.append(f"Configuration {revision} lacks a complete current or legacy settings catalog with matching IDs")
        settings[revision] = unique
    frame_data, seen_configs = defaultdict(frame_scope), set()
    previous, first_qpc, end_qpc, count, last_id = None, None, None, 0, 0
    cpu_gaps = invalid_intervals = 0
    last_streamline_query_end = 0.0
    playing_windows = []
    seen_phases, seen_foregrounds = set(), set()
    try:
        for row in csv_rows(directory / "performance.csv"):
            identity, revision = int(row["frame"]), int(row["config_revision"])
            timestamp = qpc_seconds(row)
            if identity <= last_id: raise ValueError("CPU frame identities are not unique/increasing")
            cpu_gaps += max(0, identity - last_id - 1); last_id = identity; count += 1; seen_configs.add(revision)
            if revision not in settings:
                settings_valid = False; issues.append(f"CPU frame refers to missing config revision {revision}")
            if first_qpc is None: first_qpc = timestamp
            end_qpc = timestamp + max(number(row.get("render_us")) or 0, 0) / 1e6
            current_scopes = scopes(row)
            seen_phases.add(int(row["session_phase"]))
            if row.get("foreground") in ("0", "1"): seen_foregrounds.add(int(row["foreground"]))
            for scope in current_scopes:
                target = frame_data[scope]; target["frames"] += 1
                tick = number(row.get("tick"))
                if tick is not None:
                    target["tick_min"] = int(tick) if target["tick_min"] is None else min(target["tick_min"], int(tick))
                    target["tick_max"] = int(tick) if target["tick_max"] is None else max(target["tick_max"], int(tick))
                phase = int(row["session_phase"]); target["phases"][PHASES[phase] if 0 <= phase < len(PHASES) else f"unknown:{phase}"] += 1
                for stage in STAGES:
                    value = number(row.get(stage + "_us")); target["cpu_stages"][stage].add(value / 1000 if value is not None else None)
                for stage in SUBSTAGES:
                    if stage + "_us" in row:
                        value = number(row[stage + "_us"]); target["cpu_substages"][stage].add(value / 1000 if value is not None else None)
                for parent in target["cpu_unattributed"]:
                    children = [key + "_us" for key, owner in SUBSTAGES.items() if owner == parent]
                    if all(key in row for key in children):
                        values = [number(row.get(key)) for key in [parent + "_us", *children]]
                        residual = values[0] - sum(values[1:]) if all(value is not None and value >= 0 for value in values) else None
                        target["cpu_unattributed"][parent].add(residual / 1000 if residual is not None else None)
                begin, end, frequency = (number(row.get(key)) for key in ("frame_slot_wait_begin_qpc", "frame_slot_wait_end_qpc", "qpc_frequency"))
                target["slot_wait"].add((end - begin) * 1000 / frequency if begin is not None and begin > 0 and end is not None and end >= begin and frequency and frequency > 0 else None)
                if row.get("renderer_diagnostics_valid") == "1":
                    for field in ("local_usage_bytes", "tracked_live_bytes", "tracked_retiring_bytes", "texture_physical_bytes", "acceleration_structure_bytes", "host_staging_bytes", "direct_batches", "compute_batches", "copy_batches", "cross_queue_waits", "record_jobs", "output_width", "output_height", "render_active_width", "render_active_height", "render_capacity_width", "render_capacity_height"):
                        if field in row: target["cached_fields"][field].add(number(row[field]))
                    if row.get("memory_budget_available") == "1": target["cached_fields"]["local_budget_bytes"].add(number(row.get("local_budget_bytes")))
                elif "renderer_diagnostics_valid" in row: target["diagnostic_identity_invalid"] += 1
            try:
                observation = presentation_observation(row)
                if observation:
                    presented_count, query_begin, query_end = observation
                    if query_begin < last_streamline_query_end or query_end > end_qpc + 1e-6:
                        raise ValueError("Overlapping/out-of-frame Streamline actual-present counter window")
                    last_streamline_query_end = query_end
                    same_owner_window = (previous is not None and identity == int(previous["frame"]) + 1
                        and row["qpc_frequency"] == previous["qpc_frequency"]
                        and row["session_phase"] == previous["session_phase"]
                        and row["config_revision"] == previous["config_revision"]
                        and query_begin >= qpc_seconds(previous))
                    prior_scopes = set(scopes(previous)) if previous is not None else set()
                    for scope in current_scopes:
                        target = frame_data[scope]["streamline_present"]
                        if same_owner_window and scope in prior_scopes:
                            target["observations"] += 1; target["frames"] += presented_count
                            target["seconds"] += query_end - query_begin
                        else: target["excluded"] += 1
            except (ValueError, KeyError, TypeError):
                for scope in current_scopes: frame_data[scope]["streamline_present"]["invalid"] += 1
            if previous is not None:
                interval = timestamp - qpc_seconds(previous)
                adjacent = identity == int(previous["frame"]) + 1 and row["qpc_frequency"] == previous["qpc_frequency"]
                matching = adjacent and row["session_phase"] == previous["session_phase"] and row["config_revision"] == previous["config_revision"]
                if adjacent and interval > 0 and row["session_phase"] == previous["session_phase"] == "1":
                    append_window(playing_windows, qpc_seconds(previous), timestamp)
                for scope in set(current_scopes).intersection(scopes(previous)):
                    if adjacent and interval > 0: frame_data[scope]["raw_intervals"].add(interval)
                    if matching and interval > 0: frame_data[scope]["intervals"].add(interval)
                    elif adjacent: frame_data[scope]["transitions_excluded"] += 1
                if interval <= 0 or row["qpc_frequency"] != previous["qpc_frequency"]: invalid_intervals += 1
            previous = row
    except (OSError, ValueError, KeyError, TypeError) as error:
        cpu_analysis_valid = False; issues.append(f"CPU frame analysis partial/unavailable: {error}")
    if not count: issues.append("No valid CPU frame samples")
    if frame_data["overall"]["streamline_present"]["invalid"]:
        issues.append("Streamline actual-present observations contain invalid/overlapping windows; affected observations excluded")
    unavailable_stages = [name for name, data in frame_data["overall"]["cpu_stages"].items() if data.rejected]
    if unavailable_stages: issues.append("CPU stages contain missing/invalid samples: " + ", ".join(unavailable_stages))
    invalid_substages = [name for name, data in frame_data["overall"]["cpu_substages"].items() if data.rejected]
    invalid_residuals = [name for name, data in frame_data["overall"]["cpu_unattributed"].items() if data.rejected]
    if invalid_substages: issues.append("Optional CPU subsets contain invalid samples: " + ", ".join(invalid_substages))
    if invalid_residuals: issues.append("CPU subset residuals are invalid/negative; affected residuals remain N/A: " + ", ".join(invalid_residuals))
    if frame_data["overall"]["diagnostic_identity_invalid"]:
        issues.append("Cached renderer diagnostics have stale/mismatched frame identity; affected cached metrics excluded")
    if cpu_gaps or invalid_intervals: issues.append(f"CPU samples have {cpu_gaps} missing frame IDs and {invalid_intervals} nonpositive intervals")
    passes, groups, gpu_counts, critical = defaultdict(pass_stats), defaultdict(group_stats), Counter(), defaultdict(Stats)
    gpu_rows = gpu_frames = missing_cpu = incomplete = invalid_queries = 0
    last_gpu = 0
    try:
        cursor = CpuFrameCursor(directory / "performance.csv")
        for identity, iterator in groupby(csv_rows(directory / "gpu_live.csv"), lambda row: int(row["completed_frame"])):
            if identity <= last_gpu: raise ValueError("Completed GPU frame groups are not unique/increasing")
            last_gpu = identity; rows = list(iterator); gpu_rows += len(rows)
            owner = cursor.find(identity)
            if owner is None: missing_cpu += 1; continue
            owner_scopes = scopes(owner)
            if any(row.get("fence_completed") != "1" for row in rows): incomplete += 1; continue
            gpu_frames += 1; seen = set(); frame_groups = {}
            if rows[0].get("gpu_timings_valid") == "0": invalid_queries += 1
            for scope in owner_scopes:
                gpu_counts[scope] += 1
                value = number(rows[0].get("critical_path_ms")) if rows[0].get("critical_path_valid") == "1" else None
                critical[scope].add(value)
            for row in rows:
                pass_id = int(row["pass_id"])
                if pass_id in seen: raise ValueError(f"Duplicate GPU pass {pass_id} in completed frame {identity}")
                seen.add(pass_id)
                if (row["pass_name"].startswith("RenderCriticalPath") or row["pass_name"] == "SourceRenderOutputReady"): continue
                frequency, ns = number(row.get("timestamp_frequency")), number(row.get("nanoseconds"))
                valid = row.get("measured") == "1" and frequency is not None and frequency > 0 and ns is not None and ns >= 0
                duration = ns / 1e6 if valid else None
                record, provider = elapsed_ms(row, "cpu_record"), elapsed_ms(row, "cpu_provider_submit")
                group = graphics.pass_group(row)
                for scope in owner_scopes:
                    aggregate = passes[(scope, pass_id, row["pass_name"], int(row["queue"]))]
                    aggregate["scheduled"] += 1; aggregate["gpu"].add(duration); aggregate["record"].add(record); aggregate["provider"].add(provider)
                if group is not None:
                    bucket = frame_groups.setdefault(group, {"work": 0.0, "incomplete": False, "passes": set()})
                    bucket["passes"].add((pass_id, row["pass_name"]))
                    if valid: bucket["work"] += duration
                    else: bucket["incomplete"] = True
            for scope in owner_scopes:
                for name, bucket in frame_groups.items():
                    aggregate = groups[(scope, name)]; aggregate["scheduled"] += 1; aggregate["passes"].update(bucket["passes"])
                    if bucket["incomplete"]: aggregate["incomplete"] += 1
                    else: aggregate["work"].add(bucket["work"])
    except (OSError, ValueError, KeyError, TypeError) as error:
        gpu_analysis_valid = False; issues.append(f"GPU frame analysis partial/unavailable: {error}")
    if invalid_queries: issues.append(f"{invalid_queries} completed GPU frames contain invalid query samples; independent per-pass validity retained")
    gpu_coverage = cpu_analysis_valid and gpu_analysis_valid and bool(count) and not cpu_gaps and gpu_frames == count and last_gpu == last_id and not missing_cpu and not incomplete
    if not gpu_coverage: issues.append(f"GPU tail/frame coverage incomplete: {gpu_frames}/{count} own CPU frames, last GPU {last_gpu}, last CPU {last_id}, unmatched {missing_cpu}, incomplete {incomplete}")
    option_report = {"schema": 2, "scope": "Playing frames only; running mean/min/max, no percentile claim", "configurations": [],
                     "settings_receipts": {str(revision): rows for revision, rows in manifests.items()}, "errors": []}
    if not frame_data["playing"]["frames"]: option_report["errors"].append("No Playing CPU frames; no option cost can be reported")
    for revision, manifest in sorted(settings.items()):
        scope = f"playing_config:{revision}"; total = gpu_counts[scope]
        if set(manifest) not in (graphics.CURRENT_OPTION_KEYS, set(graphics.LEGACY_OPTIONS)): continue
        values = {name: group_result(data, total) for (group_scope, name), data in groups.items() if group_scope == scope}
        options = []
        catalog = graphics.LEGACY_OPTIONS if graphics.LEGACY_RT_KEYS & manifest.keys() else graphics.OPTIONS
        for key, row in sorted(manifest.items(), key=lambda item: int(item[1]["setting_id"])):
            kind, links, note = catalog[key]; status = graphics.option_status(row, manifest)
            measured = [name for name in links if values.get(name, {}).get("gpu_work_ms_per_completed_frame")]
            disabled = status in ("off", "effective_unavailable", "requested_unavailable_effective_off")
            if status == "active": status = "measured_group" if kind == "feature" and measured else "no_measured_dedicated_pass" if kind == "feature" else kind
            adaptive = values.get("adaptive", {})
            actual = {"native_effective": row.get("native_effective"), "scheduled_frames": adaptive.get("scheduled_frames", 0)} if key == "adaptive_shading" else None
            if status == "policy": status = "policy_with_stage_evidence" if actual["scheduled_frames"] else "policy_no_stage_evidence"
            options.append({**row, "kind": kind, "cost_status": status, "feature_groups": links,
                "measured_groups": [] if disabled else measured, "actual_stage_availability": actual,
                "feature_group_gpu_work_ms": {name: values[name]["gpu_work_ms_per_completed_frame"] for name in measured} if kind == "feature" and not disabled else {},
                "isolated_option_ms": None, "note": note})
        ranked = sorted(({"group": name, **data, "option_keys": [key for key, (_, links, _) in catalog.items() if key in manifest and name in links]}
                         for name, data in values.items() if data["gpu_work_ms_per_completed_frame"]), key=lambda row: row["gpu_work_ms_per_completed_frame"]["mean"], reverse=True)
        option_report["configurations"].append({"config_revision": revision, "completed_frames": total, "options": options,
            "gpu_critical_path_ms": critical[scope].result(), "ranked_feature_groups": ranked,
            "copy_transfer_groups": [{"group": name, **data} for name, data in values.items() if name.endswith("_copy_transfer")],
            "gpu_work_partial": any(data["incomplete"] for (group_scope, _), data in groups.items() if group_scope == scope),
            "available": total > 0, "unavailable_reason": None if total else "No completed Playing GPU frames in this configuration"})
    if option_report["errors"]: issues.extend(option_report["errors"])
    bounds = (first_qpc, end_qpc) if first_qpc is not None and end_qpc is not None else None
    usage = utilization(directory / "utilization", bounds, issues)
    playing_issues = []
    playing_usage = utilization(directory / "utilization", playing_windows, playing_issues) if playing_windows else {"available": False}
    playing_usage["scope"] = "Only intervals fully contained in contiguous Playing CPU-frame QPC windows; no interpolation across menus or transitions. NVIDIA samples use their own timestamps."
    playing_usage["windows_qpc_seconds"] = playing_windows
    playing_usage["window_seconds"] = sum(end - begin for begin, end in playing_windows)
    playing_usage["issues"] = list(dict.fromkeys(playing_issues))
    issues.extend("Playing utilization: " + issue for issue in playing_issues)
    provenance = {}
    for name in ("capture-session.json", "invocation.json", "render.json", "result.json"):
        if (directory / name).exists():
            try: provenance[name] = json_read(directory / name)
            except (OSError, ValueError, KeyError, TypeError) as error: issues.append(f"Capture provenance {name} unavailable: {error}")
    capture_complete = diagnostics.get("complete") is True and not diagnostics.get("io_failed") and gpu_coverage and settings_valid and not invalid_intervals
    summary = {"schema": 1, "source": str(directory.resolve()), "capture_complete": capture_complete,
        "analysis_status": "complete" if capture_complete and not issues else "partial", "issues": list(dict.fromkeys(issues)),
        "diagnostics": diagnostics, "capture_provenance": provenance, "cpu_frame_count": count, "gpu_frame_count": gpu_frames, "gpu_pass_rows": gpu_rows,
        "gpu_frame_coverage_complete": gpu_coverage, "gpu_frames_missing_cpu_identity": missing_cpu,
        "gpu_incomplete_frames": incomplete, "gpu_invalid_query_frames": invalid_queries, "cpu_missing_frame_ids": cpu_gaps,
        "overall": frame_report(frame_data["overall"]), "playing": frame_report(frame_data["playing"]),
        "phases": [{"phase_id": phase, "phase": PHASES[phase] if 0 <= phase < len(PHASES) else f"unknown:{phase}",
            **frame_report(frame_data[f"phase:{phase}"]), "gpu_critical_path_ms": critical[f"phase:{phase}"].result(),
            "foreground": [{"foreground": bool(foreground), **frame_report(frame_data[f"phase_foreground:{phase}:{foreground}"]),
                "gpu_critical_path_ms": critical[f"phase_foreground:{phase}:{foreground}"].result()}
                for foreground in sorted(seen_foregrounds) if frame_data[f"phase_foreground:{phase}:{foreground}"]["frames"]]}
            for phase in sorted(seen_phases)],
        "foreground": [{"foreground": bool(foreground), **frame_report(frame_data[f"foreground:{foreground}"]),
            "gpu_critical_path_ms": critical[f"foreground:{foreground}"].result()} for foreground in sorted(seen_foregrounds)],
        "configurations": [{"config_revision": revision, **frame_report(frame_data[f"config:{revision}"]),
            "playing": frame_report(frame_data[f"playing_config:{revision}"]),
            "gpu_completed_frames": gpu_counts[f"config:{revision}"], "gpu_critical_path_ms": critical[f"config:{revision}"].result()}
            for revision in sorted(seen_configs)], "gpu_critical_path_ms": critical["overall"].result(),
        "playing_gpu_critical_path_ms": critical["playing"].result(), "utilization": usage, "playing_utilization": playing_usage,
        "limits": [capture_build_limit(provenance),
                   "Foreground is sampled at each CPU frame; only consecutive frames with the same foreground state contribute to its split FPS. No focus history is inferred between samples.",
                   "FPS uses adjacent CPU frame QPC intervals, never GPU-duration inversion. Same-phase/config FPS excludes transition intervals; raw throughput includes them.",
                   "Streamline reported-present FPS sums actual SDK counts over valid same-phase/config GetState QPC windows. It is separate from base FPS and does not verify physical display cadence. No nominal multiplier is applied; FSR generated/XeSS queued counts are excluded.",
                   "CPU stage and per-pass recording/provider times are wall time. Parallel tasks overlap; their sum is not whole-frame CPU busy time.",
                   "Optional CPU subsets are already included in graph_build/graph_prepare. Unattributed residuals exclude the named subsets and may include cleanup after detailed scopes. Older 66-column captures leave these fields N/A.",
                   "GPU feature work sums pass durations; async queues overlap. Critical path, GPU busy percentage and marginal option savings are distinct.",
                   "Unsupported/unmeasured Copy and unavailable native counters remain N/A. A zero is emitted only for a measured zero or no scheduled group work.",
                   "Per-pass/group summaries use running count/mean/min/max; no p95/p99 estimate is fabricated.",
                   "NVIDIA counters are device-wide. Tracked renderer memory is partial accounting; DXGI local usage/budget is a separate observation."]}
    gpu_csv, cpu_csv = [], []
    for (scope, identity, name, queue), data in sorted(passes.items(), key=lambda item: (item[0][0], -(item[1]["gpu"].total / max(item[1]["gpu"].count, 1)))):
        denominator = gpu_counts[scope]; stat = data["gpu"].result()
        base = dict(scope=scope, pass_id=identity, pass_name=name, queue=queue, completed_frames=denominator,
                    scheduled_frames=data["scheduled"], scheduled_coverage=data["scheduled"] / denominator if denominator else None)
        gpu_csv.append({**base, "measured_frames": stat["count"], "unavailable_frames": data["scheduled"] - stat["count"],
                        "mean_ms": stat["mean"], "min_ms": stat["min"], "max_ms": stat["max"]})
        record, provider = data["record"].result(), data["provider"].result()
        cpu_csv.append({**base, "record_measured_frames": record["count"], "record_mean_ms": record["mean"], "record_min_ms": record["min"], "record_max_ms": record["max"],
                        "provider_measured_frames": provider["count"], "provider_mean_ms": provider["mean"], "provider_min_ms": provider["min"], "provider_max_ms": provider["max"]})
    return summary, option_report, gpu_csv, cpu_csv


def write_outputs(directory, result):
    summary, options, gpu, cpu = result
    for name, data in (("summary.json", summary), ("option-costs.json", options)):
        (directory / name).write_text(json.dumps(data, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    for name, rows in (("GPU_PASS_TIMES.csv", gpu), ("CPU_PASS_TIMES.csv", cpu)):
        with (directory / name).open("w", encoding="utf-8", newline="") as stream:
            fields = list(rows[0]) if rows else ["scope", "pass_id", "pass_name", "queue", "completed_frames", "scheduled_frames"]
            writer = csv.DictWriter(stream, fieldnames=fields); writer.writeheader(); writer.writerows(rows)
    def fmt(value): return "N/A" if value is None else f"{value:.3f}"
    overall, playing = summary["overall"], summary["playing"]
    report = [f"Capture analysis: {summary['analysis_status']}", "", f"Runtime/own-frame GPU coverage complete: {summary['capture_complete']}. CPU frames {summary['cpu_frame_count']}; GPU completed frames {summary['gpu_frame_count']}.", "",
              f"Playing contiguous same-phase/config FPS: {fmt(playing['fps_contiguous_same_phase_config']['fps'])}; Playing frames: {playing['frames']}.",
              f"Overall contiguous same-phase/config FPS: {fmt(overall['fps_contiguous_same_phase_config']['fps'])}; raw adjacent QPC throughput: {fmt(overall['raw_adjacent_frame_throughput']['fps'])}.", "",
              f"Playing GPU critical path mean: {fmt(summary['playing_gpu_critical_path_ms']['mean'])} ms; overall {fmt(summary['gpu_critical_path_ms']['mean'])} ms. This is not GPU busy percentage or an FPS estimate.", "",
              "CPU stage wall time (Playing only)", "", "| Stage | Count | Mean ms | Min ms | Max ms |", "|---|---:|---:|---:|---:|"]
    for name, data in sorted(playing["cpu_stage_wall_ms"].items(), key=lambda item: item[1]["mean"] or -1, reverse=True):
        report.append(f"| {name} | {data['count']} | {fmt(data['mean'])} | {fmt(data['min'])} | {fmt(data['max'])} |")
    report += ["", "Streamline SDK-reported presentation throughput (separate from base FPS)", "",
        "Counts come from numFramesActuallyPresented over their native GetState QPC windows. They are not physical display cadence, nominal multiplication, FSR generated frames or XeSS queued frames. Transition, overlapping and out-of-frame windows are excluded; unavailable data remains N/A.", "",
        "| Scope | Base QPC FPS | SDK-reported present FPS | Actual SDK count | Observed query seconds | Observations | Excluded / invalid |", "|---|---:|---:|---:|---:|---:|---:|"]
    for scope_name, scope in [("Playing", playing), ("overall", overall)] + [
            (f"Playing config {config['config_revision']}", config["playing"]) for config in summary["configurations"]]:
        data = scope["streamline_reported_present"]
        report.append(f"| {scope_name} | {fmt(scope['fps_contiguous_same_phase_config']['fps'])} | {fmt(data['sdk_reported_present_fps'])} | {data['sdk_reported_present_count'] if data['available'] else 'N/A'} | {fmt(data['observed_query_interval_seconds']) if data['available'] else 'N/A'} | {data['observations']} | {data['transition_or_window_observations_excluded']} / {data['invalid_observations']} |")
    for scope_name, scope in (("Playing only", playing), ("overall", overall)):
        report += ["", f"CPU graph subsets ({scope_name}; included in parent stage)", "",
            "These values are already inside graph_build/graph_prepare and must not be added to their totals. Unattributed is the per-frame parent minus all named subsets; it may include cleanup after the detailed scopes. Missing older columns remain N/A.", "",
            "| Parent stage | Subset | Count | Mean ms | Min ms | Max ms |", "|---|---|---:|---:|---:|---:|"]
        for name, data in scope["cpu_stage_subsets_wall_ms"].items():
            report.append(f"| {data['parent_stage']} | {name} | {data['count']} | {fmt(data['mean'])} | {fmt(data['min'])} | {fmt(data['max'])} |")
        for parent, data in sorted(scope["cpu_stage_unattributed_wall_ms"].items()):
            report.append(f"| {parent} | unattributed residual | {data['count']} | {fmt(data['mean'])} | {fmt(data['min'])} | {fmt(data['max'])} |")
    report += ["", "Session phase / foreground breakdown", "", "| Phase | Foreground | Frames | QPC interval seconds | FPS | GPU critical mean ms |", "|---|---|---:|---:|---:|---:|"]
    for phase in summary["phases"]:
        for row in [{**phase, "foreground": "all"}] + phase["foreground"]:
            fps = row["fps_contiguous_same_phase_config"]
            report.append(f"| {phase['phase']} | {row['foreground']} | {row['frames']} | {fmt(fps['wall_seconds'])} | {fmt(fps['fps'])} | {fmt(row['gpu_critical_path_ms']['mean'])} |")
    report += ["", "CPU stage wall time (overall; dominated by menu time)", "", "| Stage | Count | Mean ms | Min ms | Max ms |", "|---|---:|---:|---:|---:|"]
    for name, data in sorted(overall["cpu_stage_wall_ms"].items(), key=lambda item: item[1]["mean"] or -1, reverse=True):
        report.append(f"| {name} | {data['count']} | {fmt(data['mean'])} | {fmt(data['min'])} | {fmt(data['max'])} |")
    def label(value): return str(value).replace("|", "\\|").replace("\n", " ")
    def amortized(row, prefix=""):
        mean = row.get(prefix + "mean_ms")
        measured = row.get(prefix + "measured_frames", row.get("measured_frames", 0))
        return mean * measured / row["completed_frames"] if mean is not None and row["completed_frames"] and measured == row["scheduled_frames"] else None
    def periodic(row): return row["pass_name"].startswith("LocalProbeStatic")
    playing_gpu = [row for row in gpu if row["scope"] == "playing" and row["mean_ms"] is not None]
    report += ["", "Heaviest measured GPU passes (Playing only; ordered by amortized work)", "",
        "Scheduled mean is per invocation. Amortized work includes no-pass frames as zero only when every scheduled instance was measured. GPU queues overlap; values are not isolated option savings.", "",
        "| ID | Pass | Scheduled mean ms | Amortized ms / completed frame | Scheduled / completed |", "|---:|---|---:|---:|---:|"]
    for row in sorted((row for row in playing_gpu if not periodic(row)), key=lambda row: amortized(row) or 0, reverse=True)[:20]:
        report.append(f"| {row['pass_id']} | {label(row['pass_name'])} | {fmt(row['mean_ms'])} | {fmt(amortized(row))} | {row['scheduled_frames']} / {row['completed_frames']} |")
    report += ["", "Periodic local-probe GPU work (Playing only)", "", "| ID | Pass | Scheduled mean ms | Amortized ms / completed frame | Scheduled / completed |", "|---:|---|---:|---:|---:|"]
    for row in sorted((row for row in playing_gpu if periodic(row)), key=lambda row: amortized(row) or 0, reverse=True):
        report.append(f"| {row['pass_id']} | {label(row['pass_name'])} | {fmt(row['mean_ms'])} | {fmt(amortized(row))} | {row['scheduled_frames']} / {row['completed_frames']} |")
    playing_cpu = [row for row in cpu if row["scope"] == "playing" and row["record_mean_ms"] is not None]
    for is_periodic in (False, True):
        report += ["", ("Periodic local-probe" if is_periodic else "Heaviest") + " CPU pass recording wall time (Playing only)", "",
            "Parallel pass recordings overlap. Amortized wall values cannot be summed into frame CPU time.", "",
            "| ID | Pass | Scheduled record mean ms | Amortized record wall ms / completed frame | Record max ms | Scheduled / completed |", "|---:|---|---:|---:|---:|---:|"]
        rows = sorted((row for row in playing_cpu if periodic(row) == is_periodic), key=lambda row: amortized(row, "record_") or 0, reverse=True)
        for row in rows if is_periodic else rows[:15]:
            report.append(f"| {row['pass_id']} | {label(row['pass_name'])} | {fmt(row['record_mean_ms'])} | {fmt(amortized(row, 'record_'))} | {fmt(row['record_max_ms'])} | {row['scheduled_frames']} / {row['completed_frames']} |")
    report += ["", "Heaviest measured GPU passes (overall; unavailable passes remain in GPU_PASS_TIMES.csv)", "", "| ID | Pass | Mean ms | Scheduled / completed |", "|---:|---|---:|---:|"]
    heavy = sorted((row for row in gpu if row["scope"] == "overall" and row["mean_ms"] is not None), key=lambda row: row["mean_ms"], reverse=True)
    for row in heavy[:20]:
        pass_label = label(row["pass_name"])
        report.append(f"| {row['pass_id']} | {pass_label} | {fmt(row['mean_ms'])} | {row['scheduled_frames']} / {row['completed_frames']} |")
    report += ["", "Per-configuration Playing FPS", "", "| Config revision | Playing frames | QPC intervals | Playing FPS | Completed GPU frames |", "|---:|---:|---:|---:|---:|"]
    config_frames = {config["config_revision"]: config for config in summary["configurations"]}
    for config in options["configurations"]:
        playing_data = config_frames.get(config["config_revision"], {}).get("playing", {})
        fps = playing_data.get("fps_contiguous_same_phase_config", {})
        report.append(f"| {config['config_revision']} | {playing_data.get('frames', 0)} | {fps.get('frame_intervals', 0)} | {fmt(fps.get('fps'))} | {config['completed_frames']} |")
    for config in options["configurations"]:
        report += ["", f"Config {config['config_revision']}: heaviest Playing feature groups (shared work, not isolated option cost)", ""]
        if not config["available"]:
            report.append(config["unavailable_reason"]); continue
        report += ["| Group | Mean ms / completed frame | Min ms | Max ms | Scheduled / completed | Linked options |", "|---|---:|---:|---:|---:|---|"]
        for group in config["ranked_feature_groups"]:
            data = group["gpu_work_ms_per_completed_frame"]
            report.append(f"| {group['group']} | {fmt(data['mean'])} | {fmt(data['min'])} | {fmt(data['max'])} | {group['scheduled_frames']} / {config['completed_frames']} | {', '.join(group['option_keys'])} |")
        unknown_copy = [group["group"] for group in config["copy_transfer_groups"] if group["gpu_work_ms_per_completed_frame"] is None]
        if unknown_copy: report += ["", "Unmeasured Copy transfers (N/A): " + ", ".join(unknown_copy) + "."]
    for config in options["configurations"]:
        report += ["", f"Config {config['config_revision']}: all {len(config['options'])} graphics options (Playing)", "",
            "Linked shared group timings are also shown for shared settings and policies. They repeat the same group work, are never summed, and do not measure option savings. Ordering uses the heaviest linked measured group; isolated cost is always N/A. Off and unobserved groups remain N/A.", "",
            "| Option key | Requested | Effective | Cost status | Linked shared group work ms / completed frame | Isolated option cost ms |", "|---|---|---|---|---|---|"]
        linked_costs = {group["group"]: group["gpu_work_ms_per_completed_frame"] for group in config["ranked_feature_groups"]} if config["available"] else {}
        def option_costs(option):
            return {group: linked_costs[group] for group in option["measured_groups"] if group in linked_costs}
        def option_weight(option):
            return max((cost["mean"] for cost in option_costs(option).values()), default=-1)
        for option in sorted(config["options"], key=lambda option: (-option_weight(option), int(option["setting_id"]))):
            costs = option_costs(option)
            groups_text = "; ".join(f"{group}: {fmt(cost['mean'])}" for group, cost in sorted(costs.items(), key=lambda item: item[1]["mean"], reverse=True))
            if not groups_text: groups_text = "N/A" + (" (" + ", ".join(option["feature_groups"]) + ")" if option["feature_groups"] else "")
            report.append(f"| {label(option['key'])} | {label(option['requested'])} | {label(option['effective'])} | {label(option['cost_status'])} | {label(groups_text)} | N/A |")
    usage = summary["utilization"]
    if usage.get("available"):
        process = usage["process_cpu"]; gpu_usage = usage["process_gpu"]["busiest_engine_percent"]
        report += ["", f"Process CPU: {fmt(process['weighted_core_equivalents'])} cores; machine-normalized weighted usage {fmt(process['machine_percent']['weighted_mean'])}%. Logical processors: {process['logical_processors']}.",
                   f"Process GPU busiest-engine weighted mean: {fmt(gpu_usage['weighted_mean'])}%; only the target PID, maximum engine, no engine sum.",
                   f"System CPU weighted mean: {fmt(usage['system_cpu_percent']['weighted_mean'])}%; full per-core samples are summarized in summary.json.",
                   f"Target-PID GPU observed interval coverage: {fmt(usage['process_gpu']['sampled_wall_seconds'])} / {fmt(usage['process_gpu']['frame_window_seconds'])} seconds; initial observation gap {fmt(usage['process_gpu']['initial_target_observation_gap_seconds'])} seconds is N/A, never zero GPU load."]
    playing_usage = summary["playing_utilization"]
    report += ["", "Playing-only utilization", "", f"Contiguous Playing QPC window seconds: {fmt(playing_usage['window_seconds'])}. Only fully contained CPU/PDH intervals and in-window NVIDIA timestamps are included."]
    if "process_cpu" in playing_usage:
        cpu_usage = playing_usage["process_cpu"]; gpu_usage = playing_usage["process_gpu"]
        report += [f"Process CPU: {fmt(cpu_usage['weighted_core_equivalents'])} cores; machine usage {fmt(cpu_usage['machine_percent']['weighted_mean'])}%; sampled {fmt(cpu_usage['sampled_wall_seconds'])} seconds.",
                   f"System CPU: {fmt(playing_usage['system_cpu_percent']['weighted_mean'])}%.",
                   f"Target-PID GPU busiest engine: {fmt(gpu_usage['busiest_engine_percent']['weighted_mean'])}%; sampled {fmt(gpu_usage['sampled_wall_seconds'])} / {fmt(gpu_usage['frame_window_seconds'])} seconds. Unobserved intervals remain N/A."]
    else: report.append("No contiguous Playing observation window; utilization is N/A.")
    for device in playing_usage.get("nvidia_device_context", []):
        metrics = device["metrics"]
        def playing_metric(name, field="mean"): return fmt(metrics.get(name, {}).get(field))
        report.append(f"Playing NVIDIA device {device['uuid']} / {device['pci_bus_id']}: GPU {playing_metric('gpu_percent')}%; VRAM {playing_metric('memory_used_mib')} MiB (max {playing_metric('memory_used_mib','max')}); power {playing_metric('power_watts')} W (max {playing_metric('power_watts','max')}). Includes other applications.")
    report += ["", "NVIDIA whole-device context (includes other applications)", "", "| Device UUID / PCI | GPU mean % | VRAM mean / max MiB | Total MiB | Clock mean MHz | Temperature max C | Power mean / max W |", "|---|---:|---:|---:|---:|---:|---:|"]
    for device in usage.get("nvidia_device_context", []):
        metrics = device["metrics"]
        def metric(name, field="mean"): return fmt(metrics.get(name, {}).get(field))
        report.append(f"| {device['uuid']} / {device['pci_bus_id']} | {metric('gpu_percent')} | {metric('memory_used_mib')} / {metric('memory_used_mib','max')} | {metric('memory_total_mib')} | {metric('graphics_clock_mhz')} | {metric('temperature_c','max')} | {metric('power_watts')} / {metric('power_watts','max')} |")
    if not usage.get("nvidia_device_context"): report.append("| N/A | N/A | N/A | N/A | N/A | N/A | N/A |")
    report += ["", "Issues / missing data", ""] + (["- " + issue for issue in summary["issues"]] or ["- None recorded."])
    report += ["", "Interpretation limits", ""] + ["- " + limit for limit in summary["limits"]]
    report += ["", "Files: summary.json, option-costs.json, GPU_PASS_TIMES.csv, CPU_PASS_TIMES.csv. Raw capture CSV/NDJSON/settings receipts remain unchanged.", ""]
    (directory / "REPORT.md").write_text("\n".join(report), encoding="utf-8")


def self_test():
    # Shared classifier must include native GI preparation as well as SDK passes.
    # Each GPU pass has one group; linked options repeat it, never add savings.
    native_gi_passes = ((142, "SceneGeometryDeformation"), (143, "SceneGeometryUpload"),
                       (259, "BrixelizerUploadPinnedNoise"), (260, "BrixelizerGuides"),
                       (261, "BrixelizerSdfUpdate"), (262, "BrixelizerNativeGi"),
                       (263, "BrixelizerMaterialResolve"), (264, "BrixelizerCacheInjectionExport"),
                       (265, "BrixelizerRetainHistories"), (266, "BrixelizerCacheDirectDiffuseEmission"),
                       (267, "BrixelizerComposeHDR"))
    for identity, name in native_gi_passes:
        row = {"pass_id": identity, "pass_name": name, "queue": 0}
        assert graphics.pass_group(row) == "gi"
        assert graphics.pass_group({**row, "queue": 2}) == "gi_copy_transfer"
    assert "gi" in graphics.OPTIONS["gi_mode"][1]
    assert "gi" in graphics.OPTIONS["gi_quality"][1]
    assert graphics.pass_group({"pass_id": 4, "pass_name": "GBuffer+Depth", "queue": 0}) == "surface"
    with tempfile.TemporaryDirectory() as temp:
        directory = Path(temp)
        def write(name, rows):
            with (directory / name).open("w", encoding="utf-8", newline="") as stream:
                writer = csv.DictWriter(stream, fieldnames=list(rows[0])); writer.writeheader(); writer.writerows(rows)
        (directory / "diagnostics.json").write_text('{"complete":true,"io_failed":false}', encoding="utf-8")
        debug_metadata = '{"build_type":"Debug","validation_enabled":true,"gpu_validation_enabled":false}'
        (directory / "capture-session.json").write_text(debug_metadata, encoding="utf-8")
        manifest = [dict(config_revision=revision, setting_id=index, key=key, requested="true", effective="true", requested_available=-1, native_effective="true", native_reason="")
                    for revision in (1, 2) for index, key in enumerate(graphics.LEGACY_OPTIONS, 1)]
        write("graphics_settings.csv", manifest)
        old_columns = (
            "frame,tick,frame_qpc,qpc_frequency,interval_us,adapter_us,render_us,wait_us,streaming_us,scene_prepare_us,ui_us,"
            "constants_upload_us,graph_build_us,graph_prepare_us,descriptors_us,graph_plan_us,graph_record_us,graph_submit_us,post_submit_us,present_us,diagnostics_us,wrapper_us,"
            "gpu_frame,gpu_valid,gpu_critical_path_ms,gpu_critical_path_valid,foreground,dynamic_instances,static_instances,session_phase,frame_cap,config_revision,settings_generation,"
            "pipeline_revision,view_id,output_width,output_height,render_active_width,render_active_height,render_capacity_width,render_capacity_height,diagnostic_frame,renderer_diagnostics_valid,submit_qpc,"
            "present_begin_qpc,present_end_qpc,renderer_qpc_frequency,memory_budget_available,local_usage_bytes,local_budget_bytes,tracked_live_bytes,tracked_retiring_bytes,texture_physical_bytes,acceleration_structure_bytes,host_staging_bytes,"
            "direct_batches,compute_batches,copy_batches,cross_queue_waits,record_jobs,transient_bytes_without_aliasing,transient_backing_bytes,validation_enabled,gpu_validation_enabled,frame_slot_wait_begin_qpc,frame_slot_wait_end_qpc"
        ).split(",")
        assert len(old_columns) == 66
        frames = [{**dict.fromkeys(old_columns, 0), **{stage + "_us": 1000 for stage in STAGES}, "frame": i, "config_revision": 1 if i <= 2 else 2,
                   "session_phase": 1, "foreground": 1 if i <= 2 else 0, "frame_qpc": 1000000 + (i - 1) * 20000, "qpc_frequency": 1000000,
                   "render_us": 10000, "graph_record_us": 2000, "renderer_diagnostics_valid": 1} for i in (1, 2, 3, 4)]
        assert len(frames[0]) == 66
        write("performance.csv", frames)
        gpu = []
        for identity in (1, 2, 3, 4):
            for pass_id, name, queue, measured, frequency in ((4, "GBuffer+Depth", 0, 1, 1000000000), (147, "VirtualShadow.SnapshotUpload", 2, 0, 0)):
                gpu.append(dict(completed_frame=identity, observed_frame=99, fence_completed=1, pass_id=pass_id,
                    pass_name=name, queue=queue, measured=measured, timestamp_frequency=frequency, nanoseconds=1000000 if measured else 0,
                    critical_path_valid=1, critical_path_ms=1, cpu_qpc_frequency=1000000,
                    cpu_record_begin_qpc=1000, cpu_record_end_qpc=3000, cpu_record_valid=1,
                    cpu_provider_submit_begin_qpc=4000, cpu_provider_submit_end_qpc=4500, cpu_provider_submit_valid=1))
        write("gpu_live.csv", gpu)
        usage = directory / "utilization"; usage.mkdir()
        (usage / "sampling.json").write_text('{"complete":true,"process_id":7,"error":null,"logical_processors":20}', encoding="utf-8")
        write("utilization/cpu-process.csv", [dict(sample=1, warmup=0, wall_seconds=.02, cpu_seconds=.02,
              qpc=1020000, qpc_frequency=1000000, logical_processors=20, system_cpu_percent=25,
              working_set_bytes=1000, private_memory_bytes=2000, handle_count=10, system_busy_core_equivalents=5)])
        write("utilization/cpu-system.csv", [dict(sample=sample,warmup=warmup,qpc=qpc,qpc_frequency=1000000,
              instance=name,utilization_percent=value,status=0)
              for sample,warmup,qpc in ((76,1,980000),(77,0,1000000),(78,0,1020000))
              for name,value in (("_total",25),("0,0",80),("0,_total",25))])
        write("utilization/cpu-threads.csv", [dict(sample=1,thread_id=2,cpu_seconds=.02)])
        gpu_counters = [dict(sample=sample,warmup=warmup,qpc=qpc,qpc_frequency=1000000,pid=pid,target_process=target,status=0,
              luid="L",phys=0,engine=engine,engine_type="3d",utilization_percent=value)
              for sample,warmup,qpc in ((76,1,980000),(77,0,1000000),(78,0,1020000))
              for pid,target,engine,value in ((7,1,0,20),(7,1,1,30),(99,0,0,100))]
        write("utilization/gpu-engines.csv", gpu_counters)
        write("utilization/nvidia-whole-gpu.csv", [dict(qpc=1020000,qpc_frequency=1000000,uuid="GPU-A",pci_bus_id="PCI-A",name="fixture",
              graphics_clock_mhz=1000,memory_used_mib=200,memory_total_mib=1000,temperature_c=50,power_watts=100)])
        summary, options, gpu_times, cpu_times = analyze(directory)
        assert summary["capture_complete"] and summary["gpu_frame_count"] == 4
        assert summary["analysis_status"] == "complete"
        current_manifest = [row for row in manifest if row["key"] in graphics.CURRENT_OPTION_KEYS]
        write("graphics_settings.csv", current_manifest)
        current = analyze(directory)
        assert current[0]["analysis_status"] == "complete"
        assert all(len(c["options"]) == 52 for c in current[1]["configurations"])
        write_outputs(directory, current)
        assert "all 52 graphics options" in (directory / "REPORT.md").read_text(encoding="utf-8")
        assert not current[0]["playing"]["cpu_stage_subsets_wall_ms"]
        assert "ray_geometry_register" not in (directory / "REPORT.md").read_text(encoding="utf-8")
        assert all(not ({"ray_scene", "shared_ray_compose", "denoising"} & set(o["feature_groups"])) for c in current[1]["configurations"] for o in c["options"])
        write("graphics_settings.csv", current_manifest[:-1])
        assert analyze(directory)[0]["analysis_status"] == "partial"
        write("graphics_settings.csv", manifest)
        assert all(not data["available"] and data["rejected"] == 0 for data in summary["playing"]["cpu_stage_subsets_wall_ms"].values())
        assert all(not data["available"] for data in summary["playing"]["cpu_stage_unattributed_wall_ms"].values())
        extended_frames = [{**row, **{name + "_us": 100 for name in SUBSTAGES}} for row in frames]
        assert len(extended_frames[0]) == 73
        write("performance.csv", extended_frames)
        detailed = analyze(directory)
        assert detailed[0]["capture_complete"] and detailed[0]["analysis_status"] == "complete"
        assert detailed[0]["playing"]["cpu_stage_wall_ms"] == summary["playing"]["cpu_stage_wall_ms"]
        assert detailed[0]["playing"]["fps_contiguous_same_phase_config"] == summary["playing"]["fps_contiguous_same_phase_config"]
        assert detailed[1:] == (options, gpu_times, cpu_times)
        subsets = detailed[0]["playing"]["cpu_stage_subsets_wall_ms"]
        assert all(data["count"] == 4 and data["mean"] == .1 and data["parent_stage"] == SUBSTAGES[name] for name, data in subsets.items())
        residuals = detailed[0]["playing"]["cpu_stage_unattributed_wall_ms"]
        assert residuals["graph_build"]["mean"] == .7 and residuals["graph_prepare"]["mean"] == .6
        assert all(config["playing"]["cpu_stage_subsets_wall_ms"]["ray_geometry_register"]["count"] == 2 for config in detailed[0]["configurations"])
        write_outputs(directory, detailed)
        detailed_report = (directory / "REPORT.md").read_text(encoding="utf-8")
        assert "CPU graph subsets (Playing only; included in parent stage)" in detailed_report
        assert "graph_prepare | unattributed residual | 4 | 0.600" in detailed_report
        invalid_subsets = [{**row, "ray_scene_register_us": "invalid"} for row in extended_frames]
        write("performance.csv", invalid_subsets)
        invalid_detail = analyze(directory)[0]
        assert invalid_detail["playing"]["cpu_stage_subsets_wall_ms"]["ray_scene_register"]["mean"] is None
        assert invalid_detail["playing"]["cpu_stage_unattributed_wall_ms"]["graph_build"]["mean"] is None
        assert invalid_detail["analysis_status"] == "partial"
        present_frames = [{**row, "streamline_actual_present_count": 2,
                           "streamline_actual_present_count_valid": int(i > 0),
                           "streamline_actual_present_begin_qpc": 1005000 + (i - 1) * 20000,
                           "streamline_actual_present_end_qpc": 1005000 + i * 20000,
                           "streamline_actual_present_qpc_frequency": 1000000}
                          for i, row in enumerate(extended_frames)]
        assert len(present_frames[0]) == 78
        write("performance.csv", present_frames)
        presentation = analyze(directory)
        actual = presentation[0]["playing"]["streamline_reported_present"]
        assert actual["observations"] == 2 and actual["sdk_reported_present_count"] == 4
        assert math.isclose(actual["sdk_reported_present_fps"], 100)
        assert math.isclose(presentation[0]["playing"]["fps_contiguous_same_phase_config"]["fps"], 50)
        assert actual["transition_or_window_observations_excluded"] == 1 and not actual["physical_display_cadence_verified"]
        assert all(config["playing"]["streamline_reported_present"]["observations"] == 1 for config in presentation[0]["configurations"])
        assert presentation[1:] == (options, gpu_times, cpu_times)
        write_outputs(directory, presentation)
        assert "Playing | 50.000 | 100.000 | 4" in (directory / "REPORT.md").read_text(encoding="utf-8")
        assert not summary["playing"]["streamline_reported_present"]["available"]
        # A capability query can shorten the SDK counter window without filling its gap.
        present_frames[-1].update(streamline_actual_present_count=1, streamline_actual_present_begin_qpc=1055000)
        write("performance.csv", present_frames)
        shortened = analyze(directory)[0]["playing"]["streamline_reported_present"]
        assert shortened["sdk_reported_present_count"] == 3 and math.isclose(shortened["observed_query_interval_seconds"], .03)
        assert math.isclose(shortened["sdk_reported_present_fps"], 100)
        present_frames[-1].update(streamline_actual_present_count=0)
        write("performance.csv", present_frames)
        zero = analyze(directory)[0]["playing"]["streamline_reported_present"]
        assert zero["observations"] == 2 and zero["sdk_reported_present_count"] == 2
        present_frames[-1].update(streamline_actual_present_begin_qpc=1005000)
        write("performance.csv", present_frames)
        bad_present = analyze(directory)[0]
        assert bad_present["analysis_status"] == "partial" and bad_present["playing"]["streamline_reported_present"]["invalid_observations"] == 1
        write("performance.csv", frames)
        assert summary["limits"][0] == "Debug unoptimized code, D3D12 validation, capture formatting/I/O and external sampling overhead are included. This is captured Debug behavior, not Release FPS or general option savings."
        (directory / "capture-session.json").write_text('{"build_type":"Release","validation_enabled":false,"gpu_validation_enabled":false}', encoding="utf-8")
        release_result = analyze(directory)
        assert "Build type: Release; D3D12 validation: disabled; GPU validation: disabled" in release_result[0]["limits"][0]
        assert "Debug unoptimized" not in release_result[0]["limits"][0]
        assert release_result[0]["overall"] == summary["overall"] and release_result[0]["playing"] == summary["playing"]
        assert release_result[1:] == (options, gpu_times, cpu_times)
        (directory / "capture-session.json").unlink()
        unknown_result = analyze(directory)[0]
        assert "Build type: unknown; D3D12 validation: unknown; GPU validation: unknown" in unknown_result["limits"][0]
        assert "Debug unoptimized" not in unknown_result["limits"][0]
        assert unknown_result["playing"] == summary["playing"]
        (directory / "capture-session.json").write_text(debug_metadata, encoding="utf-8")
        usage = summary["utilization"]
        assert usage["process_cpu"]["weighted_core_equivalents"] == 1
        assert usage["process_cpu"]["machine_percent"]["weighted_mean"] == 5
        assert usage["process_gpu"]["busiest_engine_percent"]["weighted_mean"] == 30
        assert math.isclose(usage["process_gpu"]["sampled_wall_seconds"], .02)
        assert usage["system_cpu_percent"]["weighted_mean"] == 25
        assert usage["logical_cpu_percent"]["0,0"]["mean"] == 80
        assert usage["nvidia_device_context"][0]["metrics"]["memory_used_mib"]["mean"] == 200
        assert math.isclose(summary["playing"]["fps_contiguous_same_phase_config"]["fps"], 50)
        assert summary["playing"]["fps_contiguous_same_phase_config"]["frame_intervals"] == 2
        assert [row["frames"] for row in summary["foreground"]] == [2,2]
        assert all(math.isclose(row["fps_contiguous_same_phase_config"]["fps"], 50) for row in summary["foreground"])
        assert [c["completed_frames"] for c in options["configurations"]] == [2, 2]
        assert all(len(c["options"]) == 56 for c in options["configurations"])
        copy = next(row for row in gpu_times if row["scope"] == "overall" and row["pass_id"] == 147)
        assert copy["mean_ms"] is None and copy["measured_frames"] == 0
        record = next(row for row in cpu_times if row["scope"] == "overall" and row["pass_id"] == 4)
        assert record["record_mean_ms"] == 2 and record["provider_mean_ms"] == .5
        weighted = [{**gpu_counters[3], "sample":sample, "warmup":warmup, "qpc":qpc, "utilization_percent":value}
                    for sample,warmup,qpc,value in ((0,1,990000,0),(1,0,1000000,0),(2,0,1010000,20),(3,0,1060000,40))]
        write("utilization/gpu-engines.csv", weighted)
        weighted_gpu = analyze(directory)[0]["utilization"]["process_gpu"]
        assert math.isclose(weighted_gpu["sampled_wall_seconds"], .06)
        assert math.isclose(weighted_gpu["busiest_engine_percent"]["weighted_mean"], 110 / 3)
        # Matching integer IDs must never attach late PDH initialization to an early CPU sample.
        write("utilization/gpu-engines.csv", [{**row,"sample":row["sample"]-76,
              "qpc":row["qpc"]+30000000} for row in gpu_counters])
        late = analyze(directory)[0]
        assert late["utilization"]["process_cpu"]["weighted_core_equivalents"] == 1
        assert not late["utilization"]["process_gpu"]["busiest_engine_percent"]["available"]
        assert late["utilization"]["process_gpu"]["sampled_wall_seconds"] == 0
        write("utilization/gpu-engines.csv", gpu_counters)
        supplement = directory / "utilization-counter-refresh"; supplement.mkdir()
        (supplement / "pdh-worker.json").write_text('{"complete":true,"reason":"process_exit"}', encoding="utf-8")
        (supplement / "auxiliary-worker.json").write_text('{"worker_process_id":35672,"target_pid":7}', encoding="utf-8")
        write("utilization/gpu-engines.csv", [row for row in gpu_counters if row["pid"] == 99])
        supplementary = [{**row,"sample":row["sample"]-75,"qpc":{76:1020000,77:1040000,78:1060000}[row["sample"]]}
                         for row in gpu_counters if row["pid"] == 7]
        write("utilization-counter-refresh/gpu-engines.csv", supplementary)
        write("utilization-counter-refresh/cpu-system.csv", [dict(sample=sample,warmup=warmup,qpc=qpc,qpc_frequency=1000000,
              instance="_total",utilization_percent=100,status=0)
              for sample,warmup,qpc in ((1,1,1020000),(2,0,1040000),(3,0,1060000))])
        refreshed = analyze(directory)[0]["utilization"]
        assert refreshed["process_gpu"]["busiest_engine_percent"]["weighted_mean"] == 30
        assert math.isclose(refreshed["process_gpu"]["sampled_wall_seconds"], .02)
        assert math.isclose(refreshed["process_gpu"]["initial_target_observation_gap_seconds"], .04)
        assert refreshed["process_gpu"]["window_coverage_fraction"] < .3
        assert refreshed["system_cpu_percent"]["weighted_mean"] == 25
        assert refreshed["process_gpu"]["sources"][0]["accepted_intervals"] == 0
        assert refreshed["process_gpu"]["sources"][1]["metadata"]["pdh-worker.json"]["complete"]
        assert refreshed["process_gpu"]["sources"][1]["metadata"]["auxiliary-worker.json"]["worker_process_id"] == 35672
        write("utilization/gpu-engines.csv", gpu_counters)
        # An overlapping supplemental interval is not counted twice.
        write("utilization-counter-refresh/gpu-engines.csv", gpu_counters)
        overlap = analyze(directory)[0]["utilization"]["process_gpu"]
        assert math.isclose(overlap["sampled_wall_seconds"], .02)
        assert overlap["sources"][1]["overlapping_intervals_excluded"] == 1
        refresh_intervals = list(counter_intervals(iter([dict(sample=sample,warmup=str(warmup),qpc=qpc,qpc_frequency=1000000)
              for sample,warmup,qpc in ((1,0,1000000),(2,0,1010000),(3,1,1020000),(4,0,1030000),(5,0,1040000))]), None))
        assert len(refresh_intervals) == 2
        assert [int(rows[0]["sample"]) for rows, wall in refresh_intervals] == [2,5]
        assert all(math.isclose(wall, .01) for rows, wall in refresh_intervals)
        # A CPU interval crossing menu->Playing is excluded; counter IDs do not join CPU IDs.
        window_frames = [{**row, "session_phase": 1 if int(row["frame"]) in (2,3) else 0} for row in frames]
        write("performance.csv", window_frames)
        write("utilization/cpu-process.csv", [dict(sample=sample, warmup=0, wall_seconds=.02, cpu_seconds=cpu,
              qpc=qpc, qpc_frequency=1000000, logical_processors=20)
              for sample,qpc,cpu in ((1,1020000,.02),(2,1040000,.04))])
        write("utilization/nvidia-whole-gpu.csv", [dict(qpc=qpc,qpc_frequency=1000000,uuid="GPU-A",pci_bus_id="PCI-A",name="fixture",gpu_percent=value)
              for qpc,value in ((1010000,90),(1030000,10))])
        windowed = analyze(directory)[0]
        assert math.isclose(windowed["playing_utilization"]["window_seconds"], .02)
        assert windowed["playing_utilization"]["process_cpu"]["weighted_core_equivalents"] == 2
        assert math.isclose(windowed["playing_utilization"]["process_cpu"]["sampled_wall_seconds"], .02)
        assert not windowed["playing_utilization"]["process_gpu"]["busiest_engine_percent"]["available"]
        assert windowed["playing_utilization"]["nvidia_device_context"][0]["metrics"]["gpu_percent"]["mean"] == 10
        assert windowed["overall"]["phase_frames"] == {"main_menu":2,"playing":2}
        assert windowed["playing"]["fps_contiguous_same_phase_config"]["fps"] is None
        write("performance.csv", frames)
        write("gpu_live.csv", gpu[:-2])
        partial = analyze(directory)
        assert not partial[0]["capture_complete"] and partial[0]["analysis_status"] == "partial"
        write_outputs(directory, partial)
        assert (directory / "REPORT.md").exists() and not json_read(directory / "summary.json")["capture_complete"]
        text = (directory / "REPORT.md").read_text(encoding="utf-8")
        assert "Per-configuration Playing FPS" in text and "shared work, not isolated option cost" in text
        assert "NVIDIA whole-device context" in text and "GPU-A" in text
        assert "all 56 graphics options" in text and "Amortized ms / completed frame" in text
        assert text.index("CPU stage wall time (Playing only)") < text.index("CPU stage wall time (overall")
        for revision in (1,2):
            section = text.split(f"Config {revision}: all 56 graphics options (Playing)",1)[1]
            table = section.split("|---|---|---|---|---|---|",1)[1].strip().splitlines()[:56]
            assert len(table) == 56 and {line.split("|")[1].strip() for line in table} == set(graphics.LEGACY_OPTIONS)
            assert all(line.split("|")[-2].strip() == "N/A" for line in table)
            geometry_row = next(line for line in table if line.startswith("| geometry_quality |"))
            reconstruction_row = next(line for line in table if line.startswith("| reconstruction_quality |"))
            if revision == 1: assert "surface: 1.000" in geometry_row and "surface: 1.000" in reconstruction_row
            assert all(option["isolated_option_ms"] is None for config in options["configurations"] for option in config["options"])
        write("performance.csv", [frames[0], frames[1], frames[3]])
        write("gpu_live.csv", [row for row in gpu if row["completed_frame"] != 3])
        sparse = analyze(directory)[0]
        assert sparse["cpu_missing_frame_ids"] == 1
        assert math.isclose(sparse["overall"]["fps_contiguous_same_phase_config"]["fps"], 50)
        write("performance.csv", [{**row, "session_phase": 0} for row in frames])
        no_play = analyze(directory)[1]
        assert no_play["errors"] and all(not c["available"] for c in no_play["configurations"])
        assert sum(len(rows) for rows in no_play["settings_receipts"].values()) == 112
        invalid = Stats(); invalid.add(float("nan")); invalid.add(-1)
        assert invalid.result()["mean"] is None
        json.dumps(sparse, allow_nan=False)
        (directory / "diagnostics.json").write_text('{"complete":false,"invalid":NaN}', encoding="utf-8")
        write("performance.csv", [{"frame":1,"unexpected_column":0}])
        malformed = analyze(directory)
        assert not malformed[0]["capture_complete"] and malformed[0]["issues"]
        write_outputs(directory, malformed)
        assert json_read(directory / "summary.json")["analysis_status"] == "partial"
    print("PASS sparse/QPC FPS, config owner join, per-pass CPU timing, unknown Copy N/A, supplemental GPU provenance/overlap/refresh warmup, Playing windows/foreground, missing tail partial outputs, no Playing receipts, actual Debug/Release/unknown build limits, optional CPU graph subsets/residuals, independent Streamline actual-present QPC counts and finite JSON")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, nargs="?")
    parser.add_argument("--self-test", action="store_true")
    arguments = parser.parse_args()
    if arguments.self_test: self_test()
    else:
        if arguments.directory is None or not arguments.directory.is_dir(): parser.error("an existing capture directory is required")
        result = analyze(arguments.directory)
        write_outputs(arguments.directory, result)
        print(f"{result[0]['analysis_status']}: {arguments.directory / 'REPORT.md'}")
        if result[0]["analysis_status"] != "complete": raise SystemExit(2)
