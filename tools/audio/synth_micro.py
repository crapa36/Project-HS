#!/usr/bin/env python3
"""Small deterministic procedural cue generator (stdlib only)."""
from __future__ import annotations

import argparse
import json
import math
import random
import time
import wave
from pathlib import Path

SAMPLE_RATE = 48_000


def synthesize_cue(cue_id: str, *, duration: float, seed: int = 0, loop: bool = False) -> list[float]:
    """Deterministic, cue-specific micro designs; unknown IDs fail closed."""
    if cue_id == "audio.skill.charged.loop":
        if not loop or abs(duration - .25) > 1e-9:
            raise ValueError("charged loop requires loop=true and duration=0.25")
        count = round(.25 * SAMPLE_RATE); cents = (seed % 5 - 2) * 3
        f220 = 220.0 * 2 ** (cents / 1200.0)
        cycles = round(f220 * .25); shimmer = round((584.0 + (seed % 3 - 1) * 1.0) * .25)
        tension_mix = .10 + (seed % 5) * .006
        air_mix = .025 + (seed % 4) * .004
        values = []
        for i in range(count):
            phase = i / count
            value = .34 * math.sin(2 * math.pi * cycles * phase)
            value += tension_mix * math.sin(2 * math.pi * cycles * 2 * phase)
            value += .07 * math.sin(2 * math.pi * cycles * 3 * phase)
            value += air_mix * math.sin(2 * math.pi * shimmer * phase)
            values.append(value)
        return values
    designs = {
        "audio.ui.hover": (1568.0, 1975.0),
        "audio.ui.tab": (1175.0, 880.0),
        "audio.ui.slider_tick": (880.0, 1046.0),
    }
    if cue_id not in designs:
        raise ValueError(f"no micro design for cue: {cue_id}")
    first, second = designs[cue_id]; count = max(1, round(duration * SAMPLE_RATE)); variation = (seed % 5 - 2) * .006
    values = []
    for i in range(count):
        t = i / SAMPLE_RATE; u = i / max(1, count - 1)
        envelope = min(1.0, t / .002) * max(0.0, 1.0 - t / max(duration, .001)) ** 3
        if cue_id == "audio.ui.hover":
            tone = math.sin(2 * math.pi * (first * (1 + variation)) * t) * .28 + math.sin(2 * math.pi * second * t) * .10
        elif cue_id == "audio.ui.tab":
            tone = math.sin(2 * math.pi * first * (1 + variation) * t) * .20 + math.sin(2 * math.pi * second * (1 - variation) * t) * .13
        else:
            tone = math.sin(2 * math.pi * ((first + (second - first) * u) * (1 + variation)) * t) * .24
        values.append(tone * envelope)
    return values


def _wave_sample(kind: str, phase: float, rng: random.Random) -> float:
    if kind == "sine":
        return math.sin(2.0 * math.pi * phase)
    if kind == "triangle":
        return 4.0 * abs(phase - math.floor(phase + 0.5)) - 1.0
    if kind == "white-noise":
        return rng.uniform(-1.0, 1.0)
    if kind == "band-limited-noise":
        # A deterministic, inexpensive one-pole low-pass noise source.
        return rng.uniform(-1.0, 1.0)
    raise ValueError(f"unknown waveform: {kind}")


def synthesize(*, duration: float, waveform: str = "sine", frequency: float = 440.0,
               frequency_end: float | None = None, attack: float = 0.005,
               decay: float = 0.02, gain: float = 0.8, seed: int = 0,
               loop_seed: bool = False) -> list[float]:
    if duration <= 0 or frequency <= 0 or (frequency_end is not None and frequency_end <= 0):
        raise ValueError("duration and frequencies must be positive")
    count = max(1, round(duration * SAMPLE_RATE))
    rng = random.Random(seed)
    end = frequency if frequency_end is None else frequency_end
    phase = 0.0
    filtered = 0.0
    periodic = []
    if loop_seed and waveform in ("white-noise", "band-limited-noise"):
        harmonics = 24 if waveform == "band-limited-noise" else 48
        periodic = [(rng.uniform(-1.0, 1.0) / math.sqrt(h), rng.uniform(0.0, 1.0))
                    for h in range(1, harmonics + 1)]
        norm = math.sqrt(sum(a * a for a, _ in periodic)) or 1.0
        periodic = [(a / norm, p) for a, p in periodic]
    samples: list[float] = []
    for i in range(count):
        t = i / SAMPLE_RATE
        if loop_seed:
            # Exact periodic phase: a seeded cue always wraps at its duration.
            cycles = max(1, round(frequency * duration))
            phase = (i * cycles / count) % 1.0
            hz = cycles / duration
        else:
            u = i / max(1, count - 1)
            hz = frequency + (end - frequency) * u
            phase = (phase + hz / SAMPLE_RATE) % 1.0
        if loop_seed and periodic:
            value = sum(a * math.sin(2.0 * math.pi * h * phase + 2.0 * math.pi * p)
                        for h, (a, p) in enumerate(periodic, 1))
        else:
            value = _wave_sample(waveform, phase, rng)
        if waveform == "band-limited-noise" and not loop_seed:
            cutoff = min(1.0, max(0.01, hz / SAMPLE_RATE * 8.0))
            filtered += cutoff * (value - filtered)
            value = filtered
        # Attack and decay are deliberately simple and deterministic.
        # Loop-seed cues use a constant periodic envelope; applying a one-shot
        # fade here would create an amplitude discontinuity at the wrap.
        env = 1.0
        if not loop_seed:
            if attack > 0:
                env = min(env, t / attack)
            if decay > 0 and duration - t < decay:
                env = min(env, max(0.0, (duration - t) / decay))
        samples.append(max(-1.0, min(1.0, value * gain * env)))
    return samples


def write_wav(path: Path, samples: list[float], rate: int = SAMPLE_RATE, bits: int = 16) -> None:
    peak = (1 << (bits - 1)) - 1
    raw = bytearray()
    for sample in samples:
        raw += int(max(-1.0, min(1.0, sample)) * peak).to_bytes(bits // 8, "little", signed=True)
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as out:
        out.setnchannels(1); out.setsampwidth(bits // 8); out.setframerate(rate); out.writeframes(raw)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Generate a small deterministic mono WAV cue")
    parser.add_argument("--output", "-o", required=True)
    parser.add_argument("--duration", type=float, required=True)
    parser.add_argument("--mode", choices=("one-shot", "loop-seed"), default="one-shot")
    parser.add_argument("--waveform", choices=("sine", "triangle", "white-noise", "band-limited-noise"), default="sine")
    parser.add_argument("--frequency", type=float, default=440.0)
    parser.add_argument("--frequency-end", type=float)
    parser.add_argument("--attack", type=float, default=0.005)
    parser.add_argument("--decay", type=float, default=0.02)
    parser.add_argument("--gain", type=float, default=0.8)
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--cue-id")
    args = parser.parse_args(argv)
    output = Path(args.output)
    sidecar = output.with_suffix(".json")
    if output.exists():
        raise FileExistsError(f"refusing to overwrite existing output: {output}")
    if sidecar.exists():
        raise FileExistsError(f"refusing to overwrite existing sidecar: {sidecar}")
    samples = synthesize(duration=args.duration, waveform=args.waveform, frequency=args.frequency,
                         frequency_end=args.frequency_end, attack=args.attack, decay=args.decay,
                         gain=args.gain, seed=args.seed, loop_seed=args.mode == "loop-seed")
    write_wav(output, samples)
    cue_id = args.cue_id or output.parent.name
    sidecar.write_text(json.dumps({"cue_id": cue_id, "generator": "micro-dsp",
            "model": None, "prompt": None, "negative_prompt": None, "source_audio": None,
            "init_noise_level": None, "seed": args.seed, "duration": args.duration,
            "created_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
