#!/usr/bin/env python3
"""Generate nonverbal vocal SFX with the local Chatterbox Turbo model."""
from __future__ import annotations

import argparse
import datetime as _dt
import json
import random
import wave
from pathlib import Path

MODEL_REVISION = "749d1c1a46eb10492095d68fbcf55691ccf137cd"
MODEL_NAME = f"ResembleAI/chatterbox-turbo@{MODEL_REVISION}"
EVENTS = ("clear throat", "sigh", "shush", "cough", "groan", "sniff", "gasp", "chuckle", "laugh")


def _seed(seed: int) -> None:
    random.seed(seed)
    try:
        import numpy as np
        np.random.seed(seed & 0xFFFFFFFF)
    except ImportError:
        pass
    try:
        import torch
        torch.manual_seed(seed)
        if torch.cuda.is_available():
            torch.cuda.manual_seed_all(seed)
    except ImportError:
        pass


def _next_candidate(folder: Path) -> Path:
    folder.mkdir(parents=True, exist_ok=True)
    index = 1
    while True:
        path = folder / f"v{index:02d}.wav"
        if not path.exists() and not path.with_suffix(".json").exists():
            return path
        index += 1


def _audio_values(audio) -> list[float]:
    try:
        values = audio.detach().float().cpu().reshape(-1).tolist()
    except AttributeError:
        try:
            values = audio.reshape(-1).tolist()
        except AttributeError:
            values = list(audio)
    return [max(-1.0, min(1.0, float(v))) for v in values]


def _extract_event(values: list[float], sample_rate: int) -> list[float]:
    peak = max((abs(v) for v in values), default=0.0)
    threshold = max(peak * 0.10, 1e-4)
    active = [i for i, value in enumerate(values) if abs(value) >= threshold]
    if not active:
        raise RuntimeError("Chatterbox candidate is silent; reject it and generate another candidate")
    padding = round(sample_rate * 0.010)
    first = max(0, active[0] - padding)
    last = min(len(values), active[-1] + padding + 1)
    extracted = values[first:last]
    if len(extracted) / sample_rate > 0.3:
        raise RuntimeError("Chatterbox vocal event exceeds 0.3 seconds; reject it instead of cropping or stretching")
    return extracted


def _write_raw(path: Path, values: list[float], sample_rate: int) -> int:
    """Write extracted mono values as PCM16 without requiring torchaudio."""
    raw = b"".join(int(round(v * 32767)).to_bytes(2, "little", signed=True) for v in values)
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(int(sample_rate))
        wav.writeframes(raw)
    return len(values)


def _load_model():
    try:
        from chatterbox.tts_turbo import ChatterboxTurboTTS
        import torch
        from huggingface_hub import snapshot_download
    except ImportError as exc:
        raise RuntimeError("Chatterbox, torch, and huggingface_hub are required in .venv-vocal") from exc
    snapshot = snapshot_download(
        repo_id="ResembleAI/chatterbox-turbo",
        revision=MODEL_REVISION,
    )
    device = "cuda" if torch.cuda.is_available() else "cpu"
    return ChatterboxTurboTTS.from_local(snapshot, device=device)


def generate(*, event: str = "groan", count: int = 1, seed: int = 0,
             reference_voice: Path | None = None, output_dir: Path = Path("GeneratedAudio")) -> list[Path]:
    if event not in EVENTS:
        raise ValueError(f"event must be one of: {', '.join(EVENTS)}; spoken text is not allowed")
    if count < 1:
        raise ValueError("count must be positive")
    if reference_voice is not None:
        reference_voice = Path(reference_voice)
        if not reference_voice.is_file():
            raise FileNotFoundError(f"reference voice does not exist: {reference_voice}")
    try:
        model = _load_model()
    except AssertionError as exc:
        raise RuntimeError("Chatterbox Turbo requires a reference voice longer than 5 seconds for this path") from exc
    sample_rate = int(getattr(model, "sr", 24_000))
    results: list[Path] = []
    event_tag = f"[{event}]"
    folder = Path(output_dir)
    cue_id = folder.name
    for offset in range(count):
        candidate_seed = seed + offset
        _seed(candidate_seed)
        kwargs = {}
        if reference_voice is not None:
            kwargs["audio_prompt_path"] = str(reference_voice)
        try:
            audio = model.generate(event_tag, **kwargs)
        except AssertionError as exc:
            raise RuntimeError("Chatterbox Turbo requires a reference voice longer than 5 seconds for this path") from exc
        path = _next_candidate(folder)
        sample_count = _write_raw(path, _extract_event(_audio_values(audio), sample_rate), sample_rate)
        metadata = {
            "cue_id": cue_id,
            "generator": "chatterbox",
            "model": MODEL_NAME,
            "prompt": event_tag,
            "negative_prompt": None,
            "seed": candidate_seed,
            "source_audio": str(reference_voice) if reference_voice else None,
            "init_noise_level": None,
            "duration": sample_count / sample_rate,
            "created_at": _dt.datetime.now(_dt.timezone.utc).isoformat(),
        }
        path.with_suffix(".json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        results.append(path)
    return results


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Generate nonverbal vocal SFX with Chatterbox Turbo")
    sub = parser.add_subparsers(dest="command", required=True)
    gen = sub.add_parser("generate")
    gen.add_argument("--event", default="groan", choices=EVENTS)
    gen.add_argument("--count", type=int, default=1)
    gen.add_argument("--seed", type=int, default=0)
    gen.add_argument("--reference-voice", type=Path)
    gen.add_argument("--output-dir", type=Path, default=Path("GeneratedAudio"))
    args = parser.parse_args(argv)
    if args.command == "generate":
        generate(event=args.event, count=args.count, seed=args.seed,
                 reference_voice=args.reference_voice, output_dir=args.output_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
