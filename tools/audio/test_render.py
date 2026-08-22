import json
import tempfile
import unittest
import wave
import types
from pathlib import Path
from unittest import mock

from tools.audio import render


class _Waveform:
    shape = (1, 44_100)


class _FakeModel:
    sample_rate = 44_100

    def __init__(self):
        self.calls = []

    def generate(self, **kwargs):
        self.calls.append(kwargs)
        return object()


class _FakeStableAudio:
    def __init__(self, model):
        self.model = model
        self.from_pretrained_calls = []

    def from_pretrained(self, model_name):
        self.from_pretrained_calls.append(model_name)
        return self.model


class _FakeAudio:
    def __init__(self, waveform=None, sample_rate=44_100):
        self.waveform = waveform or _Waveform()
        self.sample_rate = sample_rate

    def load(self, _path):
        return self.waveform, self.sample_rate


class _FakeScalar:
    def __init__(self, value):
        self.value = value

    def item(self):
        return self.value

    def __sub__(self, other):
        return _FakeScalar(self.value - other.value)


class _FakeFinite:
    def __init__(self, value):
        self.value = value

    def all(self):
        return self

    def item(self):
        return self.value


class _FakeTorch:
    def __init__(self, finite, peak, low, high):
        self.finite, self.peak, self.low, self.high = finite, peak, low, high

    def isfinite(self, _audio):
        return _FakeFinite(self.finite)

    def abs(self, _audio):
        return _audio

    def max(self, _audio):
        return _FakeScalar(self.peak if _audio is not None else self.high)

    def min(self, _audio):
        return _FakeScalar(self.low)


def _write_wav(_audio, path, _torchaudio, _source_rate):
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(44_100)
        output.writeframes(b"\0\0")


class RenderTests(unittest.TestCase):
    def test_save_validation_rejects_nonfinite_and_constant_audio(self):
        with self.assertRaisesRegex(ValueError, "non-finite"):
            render._validate_audio_tensor(object(), _FakeTorch(False, 1.0, 0.0, 1.0))
        with self.assertRaisesRegex(ValueError, "constant"):
            render._validate_audio_tensor(object(), _FakeTorch(True, 0.25, 0.25, 0.25))

    def test_generate_loads_once_writes_variations_and_exact_sidecar_keys(self):
        with tempfile.TemporaryDirectory() as temp:
            output_dir = Path(temp) / "audio.test"
            model = _FakeModel()
            stable_audio = _FakeStableAudio(model)
            fake_audio = _FakeAudio()
            args = render._parser().parse_args([
                "generate", "--model", "small-sfx", "--prompt", "bowstring snap",
                "--negative-prompt", "wood click", "--duration", "1", "--count", "3",
                "--seed", "41", "--output-dir", str(output_dir),
            ])
            with mock.patch.object(render, "_dependencies", return_value=(stable_audio, fake_audio)) as deps, \
                 mock.patch.object(render, "_save_audio", side_effect=_write_wav):
                self.assertEqual(args.func(args), 0)

            deps.assert_called_once_with()
            self.assertEqual(stable_audio.from_pretrained_calls, ["small-sfx"])
            self.assertEqual([call["seed"] for call in model.calls], [41, 42, 43])
            for index in range(1, 4):
                wav = output_dir / f"v{index:02d}.wav"
                self.assertTrue(wav.is_file())
                self.assertTrue(wav.with_suffix(".json").is_file())
            metadata = json.loads((output_dir / "v01.json").read_text(encoding="utf-8"))
            self.assertEqual(set(metadata), {
                "cue_id", "generator", "model", "prompt", "negative_prompt",
                "seed", "source_audio", "init_noise_level", "duration", "created_at",
            })
            self.assertEqual(metadata["seed"], 41)
            self.assertEqual(metadata["model"], "small-sfx@ae12755283df9d62ca39a9b050a39a0b607b8c20")

    def test_refine_reorders_torchaudio_load_tuple_for_model(self):
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / "source.wav"
            source.write_bytes(b"source")
            model = _FakeModel()
            stable_audio = _FakeStableAudio(model)
            waveform = _Waveform()
            fake_audio = _FakeAudio(waveform, 22_050)
            args = render._parser().parse_args([
                "refine", "--input-wav", str(source), "--prompt", "heavier impact",
                "--duration", "1", "--output-dir", str(Path(temp) / "out"),
            ])
            with mock.patch.object(render, "_dependencies", return_value=(stable_audio, fake_audio)), \
                 mock.patch.object(render, "_save_audio", side_effect=_write_wav):
                args.func(args)
            self.assertEqual(stable_audio.from_pretrained_calls, ["medium"])
            self.assertEqual(model.calls[0]["init_audio"], (22_050, waveform))

    def test_model_label_contains_pinned_revision(self):
        self.assertEqual(
            render._model_label("medium"),
            "medium@27b5a21b791b1b033d193a9e1e3ce78493f102f9",
        )

    def test_real_loader_pins_snapshot_and_uses_temporary_config(self):
        snapshot = Path(tempfile.mkdtemp())
        (snapshot / "model.safetensors").write_bytes(b"checkpoint")
        (snapshot / "t5gemma-b-b-ul2").mkdir()
        config = {"model": {"conditioning": {"configs": [{"config": {"subfolder": "t5gemma-b-b-ul2"}}]}}}
        (snapshot / "model_config.json").write_text(json.dumps(config), encoding="utf-8")
        calls = []
        seen_config = []

        class _Model:
            __module__ = "stable_audio_3.model"

            @staticmethod
            def from_pretrained(name):
                calls.append(name)
                seen_config.append(fake_configs.all_models[name])
                return object()

        original = types.SimpleNamespace(repo_id="repo", config_path="model_config.json", ckpt_path="model.safetensors")
        fake_configs = types.SimpleNamespace(models={"small-sfx": original}, all_models={"small-sfx": original})
        fake_package = types.SimpleNamespace(model_configs=fake_configs)
        fake_hub = types.SimpleNamespace(snapshot_download=mock.Mock(return_value=str(snapshot)))
        with mock.patch.dict("sys.modules", {"stable_audio_3": fake_package}), \
             mock.patch.dict("sys.modules", {"huggingface_hub": fake_hub}):
            self.assertIsNotNone(render._load_model(_Model, "small-sfx"))
        fake_hub.snapshot_download.assert_called_once_with(
            "repo", revision="ae12755283df9d62ca39a9b050a39a0b607b8c20",
            allow_patterns=("model_config.json", "model.safetensors", "t5gemma-b-b-ul2/*"),
        )
        self.assertEqual(calls, ["small-sfx"])
        pinned = seen_config[0]
        resolved_config, resolved_checkpoint = pinned.resolve()
        self.assertEqual(resolved_checkpoint, str(snapshot / "model.safetensors"))
        self.assertFalse(Path(resolved_config).exists())

    def test_inpaint_rejects_invalid_mask_and_existing_output(self):
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / "source.wav"
            source.write_bytes(b"source")
            output = Path(temp) / "candidate.wav"
            output.write_bytes(b"existing")
            fake_audio = _FakeAudio()
            base = ["inpaint", "--input-wav", str(source), "--prompt", "replace impact"]

            invalid = render._parser().parse_args(base + [
                "--mask-start", "0.8", "--mask-end", "0.2", "--output-path", str(Path(temp) / "new.wav")
            ])
            with self.assertRaises(ValueError):
                with mock.patch.object(render, "_dependencies", return_value=(_FakeModel(), fake_audio)):
                    invalid.func(invalid)

            overwrite = render._parser().parse_args(base + [
                "--mask-start", "0.1", "--mask-end", "0.2", "--output-path", str(output)
            ])
            with self.assertRaises(FileExistsError):
                with mock.patch.object(render, "_dependencies", return_value=(_FakeModel(), fake_audio)):
                    overwrite.func(overwrite)


if __name__ == "__main__":
    unittest.main()
