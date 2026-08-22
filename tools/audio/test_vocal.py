from __future__ import annotations

import json
import tempfile
import unittest
import wave
from pathlib import Path
from unittest.mock import patch

import vocal


class FakeModel:
    sr = 24000

    def __init__(self, audio=None):
        self.calls = []
        self.audio = [0.1, -0.1, 0.0] if audio is None else audio

    def generate(self, event, **kwargs):
        self.calls.append((event, kwargs))
        return self.audio


class VocalTests(unittest.TestCase):
    def test_load_once_sequential_candidates_and_exact_sidecar(self):
        model = FakeModel()
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "audio.vocal.groan" / "v01.wav").parent.mkdir(parents=True)
            (root / "audio.vocal.groan" / "v01.wav").write_bytes(b"existing")
            cue_dir = root / "audio.vocal.groan"
            with patch.object(vocal, "_load_model", return_value=model) as load:
                paths = vocal.generate(event="groan", count=2, seed=41, output_dir=cue_dir)
            self.assertEqual(load.call_count, 1)
            self.assertEqual([p.name for p in paths], ["v02.wav", "v03.wav"])
            self.assertEqual([json.loads(p.with_suffix(".json").read_text())["seed"] for p in paths], [41, 42])
            self.assertEqual(len(json.loads(paths[0].with_suffix(".json").read_text())), 10)

    def test_reference_voice_passed_through(self):
        model = FakeModel()
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            reference = root / "voice.wav"
            reference.write_bytes(b"reference")
            with patch.object(vocal, "_load_model", return_value=model):
                vocal.generate(event="sigh", reference_voice=reference, output_dir=root / "cue")
        self.assertEqual(model.calls[0][1]["audio_prompt_path"], str(reference))

    def test_spoken_event_rejected_before_model_load(self):
        with self.assertRaises(ValueError):
            vocal.generate(event="say hello")

    def test_raw_candidate_uses_model_rate(self):
        model = FakeModel()
        with tempfile.TemporaryDirectory() as d:
            with patch.object(vocal, "_load_model", return_value=model):
                path = vocal.generate(output_dir=Path(d))[0]
            with wave.open(str(path), "rb") as wav:
                self.assertEqual((wav.getframerate(), wav.getnchannels()), (model.sr, 1))
                self.assertEqual(wav.getnframes(), 3)
            metadata = json.loads(path.with_suffix(".json").read_text())
            self.assertAlmostEqual(metadata["duration"], 3 / model.sr)

    def test_event_extraction_removes_silence_and_keeps_padding(self):
        values = [0.0] * 100 + [0.5] * 200 + [0.0] * 100
        extracted = vocal._extract_event(values, 24000)
        self.assertEqual(len(extracted), 400)
        self.assertLessEqual(len(extracted) / 24000, 0.3)

    def test_silent_candidate_is_rejected_without_output(self):
        model = FakeModel([0.0] * 100)
        with tempfile.TemporaryDirectory() as d:
            with patch.object(vocal, "_load_model", return_value=model):
                with self.assertRaisesRegex(RuntimeError, "silent"):
                    vocal.generate(output_dir=Path(d) / "cue")
            self.assertEqual(list((Path(d) / "cue").glob("*")), [])

    def test_long_candidate_is_rejected_without_output(self):
        model = FakeModel([0.2] * 24000 * 1)
        with tempfile.TemporaryDirectory() as d:
            with patch.object(vocal, "_load_model", return_value=model):
                with self.assertRaisesRegex(RuntimeError, "0.3"):
                    vocal.generate(output_dir=Path(d) / "cue")
            self.assertEqual(list((Path(d) / "cue").glob("*")), [])


if __name__ == "__main__":
    unittest.main()
