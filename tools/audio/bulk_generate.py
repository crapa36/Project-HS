#!/usr/bin/env python3
"""Minimal local batch generator for a canonical audio execution plan."""
from __future__ import annotations
import argparse, hashlib, json, math, shutil, tempfile, wave
from pathlib import Path
from typing import Any
try:
    from . import finalize as _finalize
    from . import synth_micro
except ImportError:
    import finalize as _finalize
    import synth_micro

SIDECAR_FIELDS = {"cue_id", "generator", "model", "prompt", "negative_prompt", "source_audio", "init_noise_level", "seed", "duration", "created_at"}

def _duration(value: Any) -> float:
    if isinstance(value, (int, float)): return float(value)
    return float(str(value).strip().lower().removesuffix("s").split("-", 1)[0])

def _entries(path: Path) -> list[dict[str, Any]]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if isinstance(data, list): return data
    for key in ("execution_plan", "cues", "plan"):
        if isinstance(data.get(key), list): return data[key]
    raise ValueError("plan must contain an execution_plan/cues list")

def _safe(root: Path, *parts: str) -> Path:
    root = root.resolve(); result = root.joinpath(*parts).resolve()
    if result != root and root not in result.parents: raise ValueError("path escapes output root")
    return result

def _generator(cue: dict[str, Any]) -> str:
    value = str(cue.get("generator", "micro-dsp")).lower()
    return {"micro": "micro-dsp", "vocal": "chatterbox", "chatterbox-turbo": "chatterbox", "medium": "stable-medium", "stable": "stable-small", "small-sfx": "stable-small"}.get(value, value)

def _model_id(generator: str) -> str | None:
    if generator == "micro-dsp":
        return None
    if generator.startswith("stable"):
        try:
            from . import render
        except ImportError:
            import render
        return render._model_label("medium" if generator == "stable-medium" else "small-sfx")
    return generator

def _wav_contract(path: Path, kind: str, loop: bool) -> tuple[float, str]:
    with wave.open(str(path), "rb") as wav:
        rate, width, channels, frames = wav.getframerate(), wav.getsampwidth(), wav.getnchannels(), wav.getnframes()
    if rate != 48000 or width != 3 or channels not in (1, 2): raise ValueError("final WAV format contract invalid")
    rate, channels, data = _finalize.read_wav(path); _finalize._validate(data, channels, kind, loop)
    return frames / rate, hashlib.sha256(path.read_bytes()).hexdigest()

def _valid(path: Path, cue: dict[str, Any], seeds: set[int], generator: str) -> tuple[bool, str | None, int | None]:
    side = path.with_suffix(".json")
    if not path.is_file() or not side.is_file(): return False, None, None
    try:
        data = json.loads(side.read_text(encoding="utf-8")); actual, digest = _wav_contract(path, cue.get("kind", "spatial-sfx"), bool(cue.get("loop")))
    except (OSError, ValueError, TypeError, wave.Error): return False, None, None
    valid = (set(data) == SIDECAR_FIELDS and data["cue_id"] == cue["cue_id"] and data["generator"] == ("stable-audio" if generator.startswith("stable") else generator) and data["model"] == _model_id(generator) and data["seed"] in seeds and abs(float(data["duration"]) - actual) < 1e-6)
    return valid, digest if valid else None, int(data["seed"]) if valid else None

def _next(folder: Path, cue: dict[str, Any], seeds: set[int], generator: str) -> tuple[Path, bool, str | None, int | None]:
    folder.mkdir(parents=True, exist_ok=True); index = 1
    while True:
        path = folder / f"v{index:02d}.wav"; valid, digest, actual_seed = _valid(path, cue, seeds, generator)
        if valid: return path, True, digest, actual_seed
        if not path.exists() and not path.with_suffix(".json").exists(): return path, False, None, None
        index += 1

def _write_sidecar(path: Path, cue: dict[str, Any], seed: int, generator: str, duration: float) -> None:
    data = {"cue_id": cue["cue_id"], "generator": "stable-audio" if generator.startswith("stable") else generator, "model": _model_id(generator), "prompt": cue.get("prompt"), "negative_prompt": cue.get("negative_prompt"), "source_audio": None, "init_noise_level": None, "seed": seed, "duration": duration, "created_at": "bulk-generate"}
    path.with_suffix(".json").write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")

def _channels(audio: Any) -> list[list[float]]:
    if hasattr(audio, "detach"):
        values = audio.detach().float().cpu()
        if getattr(values, "ndim", 1) == 3:
            values = values[0]
        if getattr(values, "ndim", 1) == 1:
            return [values.tolist()]
        return [row.tolist() if hasattr(row, "tolist") else list(row) for row in values]
    if hasattr(audio, "ndim") and audio.ndim > 1: return [list(row) for row in audio]
    values = list(audio)
    return [list(row) for row in values] if values and isinstance(values[0], (list, tuple)) else [values]

def _crop(audio: Any, rate: int, target: float) -> Any:
    channels = _channels(audio); length = len(channels[0]); flat = [value for channel in channels for value in channel]
    if not flat or any(not math.isfinite(value) for value in flat) or max(abs(value) for value in flat) <= 1e-8 or max(flat) - min(flat) <= 1e-8: raise ValueError("Stable Audio returned silent, constant, or non-finite samples")
    count = max(1, round(target * rate))
    if length <= count: return audio
    downmix = [sum(channel[i] for channel in channels) / len(channels) for i in range(length)]; block = max(1, round(rate * .02)); limit = length - count
    best = max(range(0, limit + 1, block), key=lambda start: sum(value * value for value in downmix[start:start + count]))
    return audio[..., best:best + count] if hasattr(audio, "shape") else ([channel[best:best + count] for channel in channels] if len(channels) > 1 else channels[0][best:best + count])


def _loop_crossfade(audio: Any, rate: int, target: float, overlap: float = .25) -> Any:
    """Make one medium loop with an overlap while preserving exact target length."""
    channels = _channels(audio); total = round(target * rate); length = len(channels[0]); overlap_count = round(overlap * rate)
    if length < total + overlap_count or overlap_count < 1: raise ValueError("loop source is shorter than target plus overlap")
    end = total + overlap_count; start = overlap_count; fade = [i / max(1, overlap_count - 1) for i in range(overlap_count)]
    mixed = []
    for channel in channels:
        middle = channel[start:length - overlap_count]
        tail, head = channel[length - overlap_count:length], channel[:overlap_count]
        mixed.append(middle + [a * (1 - t) + b * t for a, b, t in zip(tail, head, fade)])
    if len(mixed) == 1 and not (hasattr(audio, "shape") and getattr(audio, "ndim", 1) > 1): return mixed[0]
    if hasattr(audio, "shape"):
        import torch
        tensor = torch.tensor(mixed, dtype=audio.dtype, device=audio.device)
        return tensor.unsqueeze(0) if audio.ndim == 3 else tensor
    return mixed

def _finite_wav(path: Path) -> None:
    _, _, data = _finalize.read_wav(path); flat = [value for channel in data for value in channel]
    if not flat or any(not math.isfinite(value) for value in flat) or max(abs(value) for value in flat) <= 1e-8: raise ValueError("candidate is silent or non-finite")

def _stable(model: Any, cue: dict[str, Any], seed: int, raw: Path, loop_crossfade: bool = False) -> None:
    try: import tools.audio.render as render
    except ImportError: import render
    sample_rate = int(getattr(model, "sample_rate", 0) or getattr(getattr(model, "model", None), "sample_rate", 44100))
    target = _duration(cue["duration"]); overlap = .25 if loop_crossfade else 0.0; request = max(2.0, target + overlap) if target < 2 else target + overlap
    audio = model.generate(prompt=cue.get("prompt", ""), negative_prompt=cue.get("negative_prompt"), duration=request, seed=seed)
    audio = _crop(audio, sample_rate, target + overlap)
    if loop_crossfade: audio = _loop_crossfade(audio, sample_rate, target, overlap)
    render._save_audio(audio, raw, _Torchaudio(), sample_rate)

class _Torchaudio:
    def save(self, path, audio, rate, **kwargs):
        import torchaudio
        return torchaudio.save(path, audio, rate, **kwargs)

def _micro(cue: dict[str, Any], seed: int, raw: Path) -> None:
    samples = synth_micro.synthesize_cue(cue["cue_id"], duration=_duration(cue["duration"]), seed=seed, loop=bool(cue.get("loop")))
    synth_micro.write_wav(raw, samples)

def _event_tag(prompt: Any) -> str:
    text = str(prompt or "groan").strip(); return text if text.startswith("[") and text.endswith("]") else f"[{text.strip('[]')}]"

def run(plan: Path, output_root: Path, *, generator_filter: str | None = None, cue_filter: str | None = None, promote_root: Path | None = None) -> list[Path]:
    entries = [cue for cue in _entries(plan) if (not generator_filter or _generator(cue) == generator_filter) and (not cue_filter or cue.get("cue_id") == cue_filter)]; output_root = output_root.resolve(); output_root.mkdir(parents=True, exist_ok=True); models: dict[str, Any] = {}; results: list[Path] = []
    for cue in entries:
        if not cue.get("cue_id"): raise ValueError("cue_id is required")
        generator = _generator(cue); folder = _safe(output_root, str(cue["cue_id"])); count, base = int(cue.get("count", 1)), int(cue.get("seed", 0)); hashes: set[str] = set()
        if generator not in models and generator != "micro-dsp":
            if generator.startswith("stable"):
                try: from . import render
                except ImportError: import render
                models[generator] = render._load_model(render._dependencies()[0], "medium" if generator == "stable-medium" else "small-sfx")
            else:
                try: from . import vocal
                except ImportError: import vocal
                models[generator] = vocal._load_model()
        for offset in range(count):
            seed = base + offset; allowed_seeds = {seed + attempt * 1000003 for attempt in range(5)}; final, skipped, digest, _ = _next(folder, cue, allowed_seeds, generator)
            if skipped:
                if digest in hashes: raise ValueError(f"duplicate variation PCM: {final}")
                hashes.add(digest); results.append(final); continue
            attempts = 5 if generator in ("stable-small", "stable-medium", "chatterbox") else 1
            for attempt in range(attempts):
                actual_seed = seed + attempt * 1000003; handle = tempfile.NamedTemporaryFile(prefix="candidate.", suffix=".wav", dir=folder, delete=False); raw = Path(handle.name); handle.close()
                try:
                    if generator.startswith("stable"): _stable(models[generator], cue, actual_seed, raw, generator == "stable-medium" and bool(cue.get("loop")))
                    elif generator == "chatterbox":
                        try: from . import vocal
                        except ImportError: import vocal
                        vocal._seed(actual_seed); tag = _event_tag(cue.get("prompt")); audio = models[generator].generate(tag); rate = int(getattr(models[generator], "sr", 24000)); vocal._write_raw(raw, vocal._extract_event(vocal._audio_values(audio), rate), rate)
                    else: _micro(cue, actual_seed, raw)
                    _finite_wav(raw); _finalize.finalize(raw, final, kind=cue.get("kind", "spatial-sfx"), loop=bool(cue.get("loop"))); actual_duration, digest = _wav_contract(final, cue.get("kind", "spatial-sfx"), bool(cue.get("loop")))
                    if digest in hashes: raise ValueError(f"duplicate variation PCM: {final}")
                    _write_sidecar(final, cue, actual_seed, generator, actual_duration); hashes.add(digest); results.append(final); break
                except (ValueError, RuntimeError):
                    final.unlink(missing_ok=True); final.with_suffix(".json").unlink(missing_ok=True)
                    if attempt + 1 == attempts: raise
                finally: raw.unlink(missing_ok=True)
    if promote_root:
        for path in results:
            target = _safe(promote_root.resolve(), path.parent.name.replace(".", "_") + "_" + path.name)
            if target.exists():
                if target.read_bytes() == path.read_bytes(): continue
                raise FileExistsError(f"refusing to overwrite promoted asset: {target}")
            target.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(path, target)
    return results

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(); parser.add_argument("--plan", type=Path, required=True); parser.add_argument("--output-root", type=Path, required=True); parser.add_argument("--generator"); parser.add_argument("--cue"); parser.add_argument("--promote-root", type=Path); args = parser.parse_args(argv); run(args.plan, args.output_root, generator_filter=args.generator, cue_filter=args.cue, promote_root=args.promote_root); return 0

if __name__ == "__main__": raise SystemExit(main())
