#!/usr/bin/env python3
"""Conservative PCM WAV finalizer and validator; intentionally stdlib-only."""
from __future__ import annotations

import argparse
import math
import os
import tempfile
import wave
from fractions import Fraction
from functools import lru_cache
from pathlib import Path

TARGET_RATE = 48_000
SILENCE_THRESHOLD = 10 ** (-60 / 20)
LOOP_MAX_SEAM = 0.05
LOOP_MAX_RMS_DELTA = 0.10
TRIM_PADDING = round(TARGET_RATE * 0.005)
ITU_TRUE_PEAK_ROWS = (
    (.001708984375,-.0291748046875,-.0189208984375,-.00830078125),
    (.010986328125,.029296875,.0330810546875,.014892578125),
    (-.0196533203125,-.0517578125,-.0582275390625,-.026611328125),
    (.033203125,.089111328125,.1015625,.047607421875),
    (-.0594482421875,-.16650390625,-.2003173828125,-.102294921875),
    (.1373291015625,.465087890625,.77978515625,.97216796875),
    (.97216796875,.77978515625,.465087890625,.1373291015625),
    (-.102294921875,-.2003173828125,-.16650390625,-.0594482421875),
    (.047607421875,.1015625,.089111328125,.033203125),
    (-.026611328125,-.0582275390625,-.0517578125,-.0196533203125),
    (.014892578125,.0330810546875,.029296875,.010986328125),
    (-.00830078125,-.0189208984375,-.0291748046875,.001708984375),
)


def read_wav(path: Path) -> tuple[int, int, list[list[float]]]:
    with wave.open(str(path), "rb") as wav:
        channels, width, rate, frames = wav.getnchannels(), wav.getsampwidth(), wav.getframerate(), wav.getnframes()
        if wav.getcomptype() != "NONE" or width not in (1, 2, 3, 4):
            raise ValueError("only uncompressed PCM WAV 8/16/24/32-bit is supported")
        raw = wav.readframes(frames)
    step = width * channels
    result = [[] for _ in range(channels)]
    for pos in range(0, len(raw), step):
        for channel in range(channels):
            b = raw[pos + channel * width:pos + (channel + 1) * width]
            if width == 1:
                value = (b[0] - 128) / 128.0
            else:
                if width == 3:
                    signed = int.from_bytes(b, "little", signed=False)
                    if signed & 0x800000:
                        signed -= 0x1000000
                else:
                    signed = int.from_bytes(b, "little", signed=True)
                value = signed / float(1 << (width * 8 - 1))
            result[channel].append(value)
    return rate, channels, result


@lru_cache(maxsize=8)
def _resample_kernels(source_rate: int) -> tuple[int, tuple[tuple[float, ...], ...]]:
    ratio = Fraction(source_rate, TARGET_RATE)
    phases = ratio.denominator
    cutoff = min(1.0, TARGET_RATE / source_rate)
    taps_each_side = 16
    kernels = []
    for phase in range(phases):
        fraction = phase / phases
        weights = []
        for offset in range(-taps_each_side + 1, taps_each_side + 1):
            x = offset - fraction
            sinc = cutoff if x == 0 else math.sin(math.pi * cutoff * x) / (math.pi * x)
            window = 0.5 + 0.5 * math.cos(math.pi * (offset - fraction) / taps_each_side)
            weights.append(sinc * window)
        total = sum(weights)
        kernels.append(tuple(w / total for w in weights))
    return phases, tuple(kernels)


def _resample(channels: list[list[float]], source_rate: int) -> list[list[float]]:
    if source_rate == TARGET_RATE:
        return channels
    old = len(channels[0]) if channels else 0
    new = max(1, round(old * TARGET_RATE / source_rate))
    phases, kernels = _resample_kernels(source_rate)
    start = -15
    out = [[] for _ in channels]
    for i in range(new):
        numerator = i * source_rate
        left, remainder = divmod(numerator, TARGET_RATE)
        left = min(old - 1, left)
        phase = (remainder * phases) // TARGET_RATE
        kernel = kernels[phase]
        for c, values in enumerate(channels):
            total = 0.0
            for k, weight in enumerate(kernel):
                index = min(old - 1, max(0, left + start + k))
                total += values[index] * weight
            out[c].append(total)
    return out


def _channel_contract(data: list[list[float]], kind: str) -> list[list[float]]:
    if kind in ("spatial-sfx", "vocal-sfx", "ui") and len(data) == 2:
        return [[(a + b) * 0.5 for a, b in zip(data[0], data[1])]]
    if kind in ("bgm", "ambience") and len(data) == 1:
        return [list(data[0]), list(data[0])]
    return data


def _rms(values: list[float]) -> float:
    return math.sqrt(sum(v * v for v in values) / max(1, len(values)))


def _trim(channels: list[list[float]]) -> list[list[float]]:
    if not channels:
        return channels
    length = len(channels[0]); active = [max(abs(ch[i]) for ch in channels) for i in range(length)]
    first = next((i for i, v in enumerate(active) if v > SILENCE_THRESHOLD), 0)
    last = next((i for i in range(length - 1, -1, -1) if active[i] > SILENCE_THRESHOLD), length - 1)
    first = max(0, first - TRIM_PADDING)
    last = min(length - 1, last + TRIM_PADDING)
    return [ch[first:last + 1] for ch in channels]


def _true_peak(channels: list[list[float]]) -> float:
    """ITU-R BS.1770 Annex 2, 48-order, four-phase true-peak estimate."""
    peak = max(abs(v) for ch in channels for v in ch)
    for values in channels:
        for i in range(len(values)):
            for phase in range(4):
                total = 0.0
                for row, coeffs in enumerate(ITU_TRUE_PEAK_ROWS):
                    index = min(len(values) - 1, max(0, i - 5 + row))
                    total += values[index] * coeffs[phase]
                peak = max(peak, abs(total))
    return peak


def _validate(data: list[list[float]], channels: int, kind: str, loop: bool) -> None:
    if len(data) != channels or not data or not data[0] or any(len(ch) != len(data[0]) for ch in data):
        raise ValueError("final WAV channel contract or length invalid")
    if any(not math.isfinite(v) for ch in data for v in ch) or max(abs(v) for ch in data for v in ch) == 0:
        raise ValueError("final WAV is silent or non-finite")
    limit = 10 ** ((-3 if kind in ("spatial-sfx", "vocal-sfx", "ui") else -1) / 20)
    peak = max(abs(v) for ch in data for v in ch) if kind in ("spatial-sfx", "vocal-sfx", "ui") else _true_peak(data)
    if peak > limit + 0.002:
        raise ValueError(f"final peak exceeds limit ({peak:.4f} > {limit:.4f})")
    if loop:
        n = min(round(TARGET_RATE * 0.01), len(data[0]) // 2)
        seam = max(abs(ch[0] - ch[-1]) for ch in data)
        rms_delta = max(abs(_rms(ch[:n]) - _rms(ch[-n:])) for ch in data) if n else float("inf")
        if seam > LOOP_MAX_SEAM or rms_delta > LOOP_MAX_RMS_DELTA:
            raise ValueError(f"loop discontinuity (seam={seam:.4f}, rms_delta={rms_delta:.4f})")


def _pad_riff(path: Path) -> None:
    raw = bytearray(path.read_bytes())
    if len(raw) & 1:
        raw.append(0)
        raw[4:8] = (len(raw) - 8).to_bytes(4, "little")
        path.write_bytes(raw)


def finalize(input_wav: Path, output_wav: Path, *, kind: str = "spatial-sfx", loop: bool = False) -> None:
    if kind not in ("spatial-sfx", "vocal-sfx", "ui", "bgm", "ambience"):
        raise ValueError(f"unknown audio kind: {kind}")
    input_path = input_wav.resolve()
    output_path = output_wav.resolve()
    if output_path.exists() and output_path != input_path:
        raise FileExistsError(f"refusing to overwrite existing output: {output_wav}")
    rate, channels, data = read_wav(input_wav)
    if not data or not data[0] or channels not in (1, 2):
        raise ValueError("WAV must contain mono or stereo samples")
    for ch in data:
        if any(not math.isfinite(v) for v in ch):
            raise ValueError("audio must be finite")
    if max(abs(v) for ch in data for v in ch) == 0:
        raise ValueError("audio must be non-silent")
    data = _channel_contract(_resample(data, rate), kind)
    if not loop and kind in ("spatial-sfx", "vocal-sfx", "ui"):
        data = _trim(data)
    peak = max(abs(v) for ch in data for v in ch) if kind in ("spatial-sfx", "vocal-sfx", "ui") else _true_peak(data)
    limit = 10 ** ((-3 if kind in ("spatial-sfx", "vocal-sfx", "ui") else -1) / 20)
    if peak > limit:
        scale = limit / peak
        data = [[v * scale for v in ch] for ch in data]
    _validate(data, len(data), kind, loop)
    raw = bytearray()
    for i in range(len(data[0])):
        for ch in data:
            value = max(-1.0, min(1.0, ch[i]))
            integer = round(value * 8388607)
            raw += int(integer).to_bytes(3, "little", signed=True)
    output_wav.parent.mkdir(parents=True, exist_ok=True)
    fd, temp_name = tempfile.mkstemp(prefix=output_wav.stem + ".", suffix=".tmp", dir=output_wav.parent)
    os.close(fd)
    try:
        with wave.open(temp_name, "wb") as wav:
            wav.setnchannels(len(data)); wav.setsampwidth(3); wav.setframerate(TARGET_RATE); wav.writeframes(raw)
        _pad_riff(Path(temp_name))
        final_rate, final_channels, final_data = read_wav(Path(temp_name))
        if final_rate != TARGET_RATE or final_channels != len(data):
            raise ValueError("final WAV header mismatch")
        _validate(final_data, len(data), kind, loop)
        os.replace(temp_name, output_wav)
    finally:
        if os.path.exists(temp_name): os.unlink(temp_name)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Finalize and validate a PCM WAV")
    parser.add_argument("input_wav", nargs="?"); parser.add_argument("output_wav", nargs="?")
    parser.add_argument("kind", nargs="?", choices=("spatial-sfx", "vocal-sfx", "ui", "bgm", "ambience"), default="spatial-sfx")
    parser.add_argument("--input-wav", dest="input_flag"); parser.add_argument("--output-wav", dest="output_flag")
    parser.add_argument("--kind", dest="kind_flag", choices=("spatial-sfx", "vocal-sfx", "ui", "bgm", "ambience")); parser.add_argument("--loop", action="store_true")
    args = parser.parse_args(argv)
    source, target = args.input_flag or args.input_wav, args.output_flag or args.output_wav
    if not source or not target: parser.error("input_wav and output_wav are required")
    finalize(Path(source), Path(target), kind=args.kind_flag or args.kind, loop=args.loop)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
