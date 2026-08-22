"""Minimal Stable Audio 3 renderer for Project-HS authoring."""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import os
import secrets
import tempfile
from pathlib import Path
from typing import Any

TARGET_SAMPLE_RATE = 44_100
MAX_DURATION = {"small-sfx": 120.0, "medium": 380.0}
MODEL_REVISIONS = {
    "small-sfx": "ae12755283df9d62ca39a9b050a39a0b607b8c20",
    "medium": "27b5a21b791b1b033d193a9e1e3ce78493f102f9",
}


def _positive(value: str) -> float:
    result = float(value)
    if result <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return result


def _count(value: str) -> int:
    result = int(value)
    if result < 1:
        raise argparse.ArgumentTypeError("must be at least 1")
    return result


def _noise_level(value: str) -> float:
    result = float(value)
    if not 0.4 <= result <= 0.8:
        raise argparse.ArgumentTypeError("init-noise-level must be between 0.4 and 0.8")
    return result


def _base_seed(seed: int | None) -> int:
    return seed if seed is not None else secrets.randbits(32)


def _validate_duration(duration: float, model: str) -> None:
    if duration > MAX_DURATION[model]:
        raise ValueError(f"{model} duration cannot exceed {MAX_DURATION[model]:g} seconds")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Generate, refine, or inpaint Project-HS audio with Stable Audio 3. "
        "negative prompts are passed through but may have limited effect on post-trained checkpoints."
    )
    sub = parser.add_subparsers(dest="command", required=True)

    def common(command: argparse.ArgumentParser) -> None:
        command.add_argument("--prompt", required=True)
        command.add_argument("--negative-prompt", default=None)
        command.add_argument("--seed", type=int, default=None)
        command.add_argument("--cue-id", default=None)

    generate = sub.add_parser("generate", help="text-to-audio generation")
    generate.add_argument("--model", choices=("small-sfx", "medium"), required=True)
    common(generate)
    generate.add_argument("--duration", type=_positive, required=True)
    generate.add_argument("--count", type=_count, default=1)
    generate.add_argument("--output-dir", type=Path, required=True)
    generate.set_defaults(func=_generate)

    refine = sub.add_parser("refine", help="audio-to-audio variation; always uses medium")
    common(refine)
    refine.add_argument("--input-wav", type=Path, required=True)
    refine.add_argument("--duration", type=_positive, required=True)
    refine.add_argument("--count", type=_count, default=1)
    refine.add_argument("--init-noise-level", type=_noise_level, default=0.55)
    refine.add_argument("--output-dir", type=Path, required=True)
    refine.set_defaults(func=_refine)

    inpaint = sub.add_parser("inpaint", help="replace one region; always uses medium")
    common(inpaint)
    inpaint.add_argument("--input-wav", type=Path, required=True)
    inpaint.add_argument("--mask-start-seconds", "--mask-start", type=float, required=True)
    inpaint.add_argument("--mask-end-seconds", "--mask-end", type=float, required=True)
    inpaint.add_argument("--output-path", type=Path, required=True)
    inpaint.set_defaults(func=_inpaint)
    return parser


def _dependencies() -> tuple[Any, Any]:
    try:
        from stable_audio_3 import StableAudioModel
        import torch  # noqa: F401 - Stable Audio 3 requires torch at runtime.
        import torchaudio
    except ImportError as exc:
        raise RuntimeError("Stable Audio 3 dependencies are unavailable; install stable-audio-3, torch, and torchaudio.") from exc
    return StableAudioModel, torchaudio


def _load_model(StableAudioModel: Any, model_name: str) -> Any:
    """Load through the official API while pinning model and T5 assets."""
    revision = MODEL_REVISIONS[model_name]
    if getattr(StableAudioModel, "__module__", "").startswith("stable_audio_3"):
        from huggingface_hub import snapshot_download
        from stable_audio_3 import model_configs

        config = model_configs.models[model_name]
        snapshot = Path(snapshot_download(
            config.repo_id,
            revision=revision,
            allow_patterns=(config.config_path, config.ckpt_path, "t5gemma-b-b-ul2/*"),
        ))
        source_config = json.loads((snapshot / config.config_path).read_text(encoding="utf-8"))
        source_config["model"]["conditioning"]["configs"][0]["config"]["model_path"] = str(snapshot)
        temporary = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False, encoding="utf-8")
        original_config = model_configs.all_models.get(model_name)
        try:
            json.dump(source_config, temporary)
            temporary.close()
            config_path = Path(temporary.name)
            ckpt_path = snapshot / config.ckpt_path

            class _PinnedConfig:
                def resolve(self) -> tuple[str, str]:
                    return str(config_path), str(ckpt_path)

            model_configs.all_models[model_name] = _PinnedConfig()
            return StableAudioModel.from_pretrained(model_name)
        finally:
            if original_config is not None:
                model_configs.all_models[model_name] = original_config
            temporary.close()
            Path(temporary.name).unlink(missing_ok=True)
    return StableAudioModel.from_pretrained(model_name)


def _model_label(model_name: str) -> str:
    return f"{model_name}@{MODEL_REVISIONS[model_name]}"


def _load_audio(path: Path, torchaudio: Any) -> tuple[int, Any, float]:
    waveform, sample_rate = torchaudio.load(str(path))
    duration = waveform.shape[-1] / sample_rate
    return sample_rate, waveform, duration


def _next_output(directory: Path) -> tuple[Path, int]:
    directory.mkdir(parents=True, exist_ok=True)
    index = 1
    while True:
        wav = directory / f"v{index:02d}.wav"
        sidecar = wav.with_suffix(".json")
        if not wav.exists() and not sidecar.exists():
            return wav, index
        index += 1


def _save_audio(audio: Any, path: Path, torchaudio: Any, source_rate: int) -> None:
    import torch

    audio = audio.detach() if hasattr(audio, "detach") else audio
    if getattr(audio, "ndim", 0) == 3:
        audio = audio[0]
    if audio.ndim == 1:
        audio = audio.unsqueeze(0)
    audio = audio.float().cpu()
    _validate_audio_tensor(audio, torch)
    if source_rate != TARGET_SAMPLE_RATE:
        audio = torchaudio.functional.resample(audio, source_rate, TARGET_SAMPLE_RATE)
        _validate_audio_tensor(audio, torch)
    path.parent.mkdir(parents=True, exist_ok=True)
    torchaudio.save(str(path), audio.clamp(-1, 1), TARGET_SAMPLE_RATE, encoding="PCM_S", bits_per_sample=16)


def _validate_audio_tensor(audio: Any, torch_module: Any) -> None:
    if not bool(torch_module.isfinite(audio).all().item()):
        raise ValueError("Stable Audio returned non-finite samples; refusing to write WAV")
    peak = float(torch_module.max(torch_module.abs(audio)).item())
    span = float((torch_module.max(audio) - torch_module.min(audio)).item())
    if peak <= 1e-8 or span <= 1e-8:
        raise ValueError("Stable Audio returned silent or constant samples; refusing to write WAV")


def _metadata(args: argparse.Namespace, model: str, path: Path, seed: int | None, source: Path | None, duration: float | None) -> None:
    cue_id = args.cue_id or (path.parent.name if args.command != "inpaint" else path.stem)
    data = {
        "cue_id": cue_id,
        "generator": "stable-audio",
        "model": model,
        "prompt": args.prompt,
        "negative_prompt": args.negative_prompt,
        "seed": seed,
        "source_audio": str(source) if source else None,
        "init_noise_level": getattr(args, "init_noise_level", None),
        "duration": duration,
        "created_at": _dt.datetime.now(_dt.timezone.utc).isoformat(),
    }
    path.with_suffix(".json").write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


def _generate(args: argparse.Namespace) -> int:
    _validate_duration(args.duration, args.model)
    StableAudioModel, torchaudio = _dependencies()
    model = _load_model(StableAudioModel, args.model)
    base_seed = _base_seed(args.seed)
    for offset in range(args.count):
        seed = base_seed + offset
        kwargs = {"prompt": args.prompt, "negative_prompt": args.negative_prompt, "duration": args.duration}
        kwargs["seed"] = seed
        audio = model.generate(**kwargs)
        path, _ = _next_output(args.output_dir)
        _save_audio(audio, path, torchaudio, getattr(model, "sample_rate", TARGET_SAMPLE_RATE))
        _metadata(args, _model_label(args.model), path, seed, None, args.duration)
        print(path)
    return 0


def _refine(args: argparse.Namespace) -> int:
    _validate_duration(args.duration, "medium")
    StableAudioModel, torchaudio = _dependencies()
    sample_rate, waveform, _ = _load_audio(args.input_wav, torchaudio)
    model = _load_model(StableAudioModel, "medium")
    init_audio = (sample_rate, waveform)
    base_seed = _base_seed(args.seed)
    for offset in range(args.count):
        seed = base_seed + offset
        kwargs = {"init_audio": init_audio, "init_noise_level": args.init_noise_level, "prompt": args.prompt,
                  "negative_prompt": args.negative_prompt, "duration": args.duration}
        kwargs["seed"] = seed
        audio = model.generate(**kwargs)
        path, _ = _next_output(args.output_dir)
        _save_audio(audio, path, torchaudio, getattr(model, "sample_rate", TARGET_SAMPLE_RATE))
        _metadata(args, _model_label("medium"), path, seed, args.input_wav, args.duration)
        print(path)
    return 0


def _inpaint(args: argparse.Namespace) -> int:
    if args.mask_start_seconds < 0 or args.mask_end_seconds <= args.mask_start_seconds:
        raise ValueError("mask end must be greater than mask start, and start cannot be negative")
    StableAudioModel, torchaudio = _dependencies()
    sample_rate, waveform, duration = _load_audio(args.input_wav, torchaudio)
    if duration > MAX_DURATION["medium"]:
        raise ValueError("medium duration cannot exceed 380 seconds")
    if args.mask_end_seconds > duration:
        raise ValueError("mask end cannot exceed input WAV duration")
    if args.output_path.exists() or args.output_path.with_suffix(".json").exists():
        raise FileExistsError(f"refusing to overwrite existing output: {args.output_path}")
    model = _load_model(StableAudioModel, "medium")
    seed = _base_seed(args.seed)
    kwargs = {"inpaint_audio": (sample_rate, waveform), "inpaint_mask_start_seconds": args.mask_start_seconds,
              "inpaint_mask_end_seconds": args.mask_end_seconds, "prompt": args.prompt,
              "negative_prompt": args.negative_prompt, "duration": duration}
    kwargs["seed"] = seed
    audio = model.generate(**kwargs)
    _save_audio(audio, args.output_path, torchaudio, getattr(model, "sample_rate", TARGET_SAMPLE_RATE))
    _metadata(args, _model_label("medium"), args.output_path, seed, args.input_wav, duration)
    print(args.output_path)
    return 0


def _self_test() -> None:
    parser = _parser()
    args = parser.parse_args(["generate", "--model", "small-sfx", "--prompt", "test", "--duration", "1", "--output-dir", "out"])
    assert args.model == "small-sfx" and args.count == 1
    assert _noise_level("0.55") == 0.55
    _validate_duration(120.0, "small-sfx")
    _validate_duration(380.0, "medium")
    try:
        _validate_duration(121.0, "small-sfx")
    except ValueError:
        pass
    else:
        raise AssertionError("small-sfx duration limit was not enforced")
    assert 0 <= _base_seed(None) <= 0xFFFFFFFF
    assert _base_seed(7) == 7


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    if os.environ.get("PROJECT_HS_AUDIO_SELF_TEST"):
        _self_test()
    else:
        raise SystemExit(main())
