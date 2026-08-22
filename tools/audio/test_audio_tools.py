from __future__ import annotations

import tempfile
import unittest
import wave
import json
import subprocess
import sys
import math
from pathlib import Path

from finalize import finalize
from synth_micro import SAMPLE_RATE, synthesize, write_wav


class AudioToolsTests(unittest.TestCase):
    def test_ui_finalize(self):
        with tempfile.TemporaryDirectory() as d:
            src, dst = Path(d) / "in.wav", Path(d) / "out.wav"
            write_wav(src, synthesize(duration=.055, waveform="sine", attack=.002, decay=.01))
            finalize(src, dst, kind="ui")
            with wave.open(str(dst), "rb") as f:
                self.assertEqual((f.getframerate(), f.getsampwidth(), f.getnchannels()), (48000, 3, 1))
                self.assertAlmostEqual(f.getnframes(), 2640, delta=8)

    def test_finalize_pads_odd_riff_data(self):
        with tempfile.TemporaryDirectory() as d:
            src, dst = Path(d) / "in.wav", Path(d) / "out.wav"
            write_wav(src, [0.2] * 481)
            finalize(src, dst, kind="ui")
            raw = dst.read_bytes()
            self.assertEqual(len(raw) % 2, 0)
            self.assertEqual(int.from_bytes(raw[4:8], "little"), len(raw) - 8)
            self.assertEqual(int.from_bytes(raw[40:44], "little"), 481 * 3)

    def test_vocal_sfx_finalize_is_mono_trimmed_and_limited(self):
        with tempfile.TemporaryDirectory() as d:
            src, dst = Path(d) / "in.wav", Path(d) / "out.wav"
            write_wav(src, [0.0] * 100 + [0.95] * 200 + [0.0] * 100)
            finalize(src, dst, kind="vocal-sfx")
            rate, channels, data = __import__("finalize").read_wav(dst)
            self.assertEqual((rate, channels), (48000, 1))
            self.assertLessEqual(max(map(abs, data[0])), 10 ** (-3 / 20) + .002)
            self.assertEqual(len(data[0]), 400)

    def test_vocal_sfx_validation_uses_sample_peak_minus_three_db(self):
        from finalize import _validate
        with self.assertRaises(ValueError):
            _validate([[0.9]], 1, "vocal-sfx", False)

    def test_loop_seed(self):
        with tempfile.TemporaryDirectory() as d:
            src, dst = Path(d) / "in.wav", Path(d) / "out.wav"
            write_wav(src, synthesize(duration=.25, frequency=8, attack=0, decay=0, loop_seed=True))
            finalize(src, dst, kind="ambience", loop=True)

    def test_peak_attenuation_only(self):
        with tempfile.TemporaryDirectory() as d:
            src, dst = Path(d) / "in.wav", Path(d) / "out.wav"
            write_wav(src, [0.95] * 480)
            finalize(src, dst, kind="ui")
            from finalize import read_wav
            rate, channels, data = read_wav(dst)
            self.assertEqual((rate, channels), (SAMPLE_RATE, 1))
            self.assertAlmostEqual(max(map(abs, data[0])), 10 ** (-3 / 20), delta=.002)
            self.assertEqual(len(data[0]), 480)

    def test_low_peak_gain_is_unchanged(self):
        with tempfile.TemporaryDirectory() as d:
            src, dst = Path(d) / "in.wav", Path(d) / "out.wav"
            write_wav(src, [0.2] * 480)
            finalize(src, dst, kind="ui")
            from finalize import read_wav
            _, _, data = read_wav(dst)
            self.assertAlmostEqual(max(map(abs, data[0])), 0.2, delta=.002)

    def test_bgm_true_peak_overshoot_is_limited(self):
        with tempfile.TemporaryDirectory() as d:
            src, dst = Path(d) / "in.wav", Path(d) / "out.wav"
            write_wav(src, [0.0, 1.0, -1.0, 0.0] * 200)
            finalize(src, dst, kind="bgm")
            from finalize import _true_peak, read_wav
            _, _, data = read_wav(dst)
            self.assertLessEqual(_true_peak(data), 10 ** (-1 / 20) + .003)

    def test_itu_true_peak_recovers_12khz_sine_peak(self):
        from finalize import _true_peak
        values = [math.sin(math.pi / 2 * n + math.pi / 4) for n in range(128)]
        self.assertAlmostEqual(max(map(abs, values)), math.sqrt(.5), places=6)
        self.assertAlmostEqual(_true_peak([values]), 1.0, delta=.06)

    def test_resample_preserves_dc_and_filters_near_nyquist(self):
        from finalize import _resample
        constant = _resample([[.25] * 4410], 44100)[0]
        self.assertAlmostEqual(sum(constant) / len(constant), .25, delta=.002)
        source = [math.sin(2 * math.pi * 20500 * n / 44100) for n in range(4410)]
        converted = _resample([source], 44100)[0]
        linear = [source[min(len(source)-1, int(i * 44100 / 48000))] for i in range(len(converted))]
        self.assertGreater(sum(abs(a-b) for a,b in zip(converted, linear)) / len(converted), .01)

    def test_resample_rational_phases_and_boundaries(self):
        from finalize import _resample, _resample_kernels
        phases, _ = _resample_kernels(44100)
        self.assertEqual(phases, 160)
        phase_sequence = {((i * 44100) % 48000) * phases // 48000 for i in range(160)}
        self.assertEqual(phase_sequence, set(range(160)))
        impulse = [0.0] * 4410
        impulse[2205] = 1.0
        converted = _resample([impulse], 44100)[0]
        self.assertLess(max(abs(converted[i] - converted[i-1]) for i in range(1, len(converted))), 1.1)
        tone = [math.sin(2 * math.pi * 440 * n / 44100) for n in range(4410)]
        tone_out = _resample([tone], 44100)[0]
        self.assertLess(max(abs(tone_out[i] - tone_out[i-1]) for i in range(1, len(tone_out))), .2)

    def test_downsample_stopband_rejects_30_and_40khz(self):
        from finalize import _resample
        for frequency in (30000, 40000):
            source = [math.sin(2 * math.pi * frequency * n / 96000) for n in range(9600)]
            converted = _resample([source], 96000)[0]
            rms = math.sqrt(sum(v * v for v in converted[500:-500]) / len(converted[500:-500]))
            self.assertLess(rms, .05, f"{frequency} Hz stopband RMS={rms}")

    def test_existing_output_is_preserved(self):
        with tempfile.TemporaryDirectory() as d:
            src, dst = Path(d) / "in.wav", Path(d) / "out.wav"
            write_wav(src, [0.2] * 480)
            dst.write_bytes(b"keep this candidate")
            with self.assertRaises(FileExistsError):
                finalize(src, dst, kind="ui")
            self.assertEqual(dst.read_bytes(), b"keep this candidate")

    def test_in_place_finalize_is_allowed(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "candidate.wav"
            write_wav(path, [0.2] * 480)
            finalize(path, path, kind="ui")
            with wave.open(str(path), "rb") as f:
                self.assertEqual((f.getframerate(), f.getsampwidth(), f.getnchannels()), (48000, 3, 1))

    def test_stereo_bgm(self):
        with tempfile.TemporaryDirectory() as d:
            src, dst = Path(d) / "in.wav", Path(d) / "out.wav"
            write_wav(src, [0.2] * 4410)
            with wave.open(str(src), "rb") as f:
                raw = f.readframes(f.getnframes())
            with wave.open(str(src), "wb") as f:
                f.setnchannels(2); f.setsampwidth(2); f.setframerate(44100)
                f.writeframes(b"".join(raw[i:i+2] * 2 for i in range(0, len(raw), 2)))
            finalize(src, dst, kind="bgm")
            with wave.open(str(dst), "rb") as f:
                self.assertEqual((f.getframerate(), f.getsampwidth(), f.getnchannels()), (48000, 3, 2))

    def test_stereo_sfx_to_mono(self):
        with tempfile.TemporaryDirectory() as d:
            src, dst = Path(d) / "in.wav", Path(d) / "out.wav"
            write_wav(src, [0.2] * 100)
            with wave.open(str(src), "rb") as f:
                raw = f.readframes(f.getnframes())
            with wave.open(str(src), "wb") as f:
                f.setnchannels(2); f.setsampwidth(2); f.setframerate(48000)
                f.writeframes(b"".join(raw[i:i+2] * 2 for i in range(0, len(raw), 2)))
            finalize(src, dst, kind="ui")
            with wave.open(str(dst), "rb") as f:
                self.assertEqual(f.getnchannels(), 1)

    def test_mono_ambience_to_stereo(self):
        with tempfile.TemporaryDirectory() as d:
            src, dst = Path(d) / "in.wav", Path(d) / "out.wav"
            write_wav(src, [0.2] * 1000)
            finalize(src, dst, kind="ambience")
            with wave.open(str(dst), "rb") as f:
                self.assertEqual(f.getnchannels(), 2)

    def test_discontinuous_loop_rejected_without_output(self):
        with tempfile.TemporaryDirectory() as d:
            src, dst = Path(d) / "in.wav", Path(d) / "out.wav"
            write_wav(src, [0.8] * 479 + [-0.8])
            with self.assertRaises(ValueError):
                finalize(src, dst, kind="spatial-sfx", loop=True)
            self.assertFalse(dst.exists())

    def test_sidecar_fields(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "cue.wav"
            subprocess.run([sys.executable, str(Path(__file__).with_name("synth_micro.py")),
                            "--output", str(out), "--duration", ".055", "--cue-id", "ui.click",
                            "--seed", "17"], check=True)
            metadata = json.loads(out.with_suffix(".json").read_text(encoding="utf-8"))
            for key in ("generator", "model", "prompt", "negative_prompt", "source_audio", "init_noise_level", "seed", "duration", "created_at"):
                self.assertIn(key, metadata)
            self.assertEqual((metadata["generator"], metadata["seed"]), ("micro-dsp", 17))

    def test_sidecar_is_always_written_without_cue_id(self):
        with tempfile.TemporaryDirectory() as d:
            from synth_micro import main
            out = Path(d) / "cue.wav"
            main(["--output", str(out), "--duration", ".01"])
            metadata = json.loads(out.with_suffix(".json").read_text(encoding="utf-8"))
            self.assertEqual(metadata["cue_id"], Path(d).name)

    def test_synth_cli_preserves_existing_wav(self):
        with tempfile.TemporaryDirectory() as d:
            from synth_micro import main
            out = Path(d) / "cue.wav"
            out.write_bytes(b"existing wav")
            with self.assertRaises(FileExistsError):
                main(["--output", str(out), "--duration", ".055"])
            self.assertEqual(out.read_bytes(), b"existing wav")

    def test_synth_cli_preserves_existing_sidecar(self):
        with tempfile.TemporaryDirectory() as d:
            from synth_micro import main
            out = Path(d) / "cue.wav"
            sidecar = out.with_suffix(".json")
            sidecar.write_bytes(b"existing metadata")
            with self.assertRaises(FileExistsError):
                main(["--output", str(out), "--duration", ".055", "--cue-id", "ui.click"])
            self.assertEqual(sidecar.read_bytes(), b"existing metadata")


if __name__ == "__main__":
    unittest.main()
