import json, tempfile, unittest, wave
from pathlib import Path
from unittest import mock

from tools.audio import bulk_generate
from tools.audio import synth_micro
from tools.audio.finalize import finalize


class BulkTests(unittest.TestCase):
    def test_versioned_production_plan_is_complete(self):
        plan = Path(__file__).parents[2] / "GeneratedAudio" / "full_generation_plan.json"
        cues = bulk_generate._entries(plan)
        seeds = [int(cue["seed"]) + offset
                 for cue in cues for offset in range(int(cue["count"]))]
        self.assertEqual((len(cues), sum(int(cue["count"]) for cue in cues)),
                         (107, 379))
        self.assertEqual(len(set(seeds)), 379)

    def test_micro_cue_specs_and_charged_loop_finalize(self):
        samples = synth_micro.synthesize_cue("audio.skill.charged.loop", duration=.25, seed=2, loop=True)
        self.assertEqual(len(samples), 12000)
        self.assertLess(abs(samples[0] - samples[-1]), .05)
        self.assertLess(sum(value * value for value in samples) / len(samples), .02)
        self.assertEqual(samples, synth_micro.synthesize_cue("audio.skill.charged.loop", duration=.25, seed=2, loop=True))
        with tempfile.TemporaryDirectory() as d:
            source, output = Path(d) / "source.wav", Path(d) / "final.wav"
            synth_micro.write_wav(source, synth_micro.synthesize_cue("audio.skill.charged.loop", duration=.25, seed=2, loop=True))
            finalize(source, output, kind="spatial-sfx", loop=True)
        with self.assertRaises(ValueError): synth_micro.synthesize_cue("audio.ui.unknown", duration=.08)

    def test_medium_loop_crossfade_is_exact_stereo_target(self):
        import math
        target, rate = 1.0, 48000
        source = [[math.sin(2 * math.pi * 4 * i / rate) for i in range(round(rate * 1.25))] for _ in range(2)]
        mixed = bulk_generate._loop_crossfade(source, rate, target)
        self.assertEqual([len(channel) for channel in mixed], [rate, rate])
        with tempfile.TemporaryDirectory() as d:
            raw, final = Path(d) / "raw.wav", Path(d) / "final.wav"
            with wave.open(str(raw), "wb") as wav:
                wav.setnchannels(2); wav.setsampwidth(2); wav.setframerate(rate)
                wav.writeframes(b"".join(int(max(-1, min(1, mixed[c][i])) * 32767).to_bytes(2, "little", signed=True) for i in range(rate) for c in range(2)))
            finalize(raw, final, kind="ambience", loop=True)
            with wave.open(str(final), "rb") as wav: self.assertEqual((wav.getframerate(), wav.getsampwidth(), wav.getnchannels(), wav.getnframes()), (48000, 3, 2, 48000))

    def test_production_micro_counts_finalize_to_unique_pcm(self):
        with tempfile.TemporaryDirectory() as d:
            plan = Path(d) / "plan.json"; root = Path(d) / "GeneratedAudio"
            plan.write_text(json.dumps({"execution_plan": [
                {"cue_id": "audio.ui.hover", "generator": "micro-dsp", "duration": .08, "count": 4, "seed": 10, "kind": "ui"},
                {"cue_id": "audio.ui.tab", "generator": "micro-dsp", "duration": .09, "count": 4, "seed": 20, "kind": "ui"},
                {"cue_id": "audio.ui.slider_tick", "generator": "micro-dsp", "duration": .055, "count": 5, "seed": 30, "kind": "ui"},
                {"cue_id": "audio.skill.charged.loop", "generator": "micro-dsp", "duration": .25, "count": 2, "seed": 40, "kind": "spatial-sfx", "loop": True},
            ]}))
            outputs = bulk_generate.run(plan, root)
            self.assertEqual(len(outputs), 15)
            for cue_id, count in (("audio.ui.hover", 4), ("audio.ui.tab", 4), ("audio.ui.slider_tick", 5), ("audio.skill.charged.loop", 2)):
                files = sorted((root / cue_id).glob("v*.wav"))
                self.assertEqual(len(files), count)
                self.assertEqual(len({p.read_bytes() for p in files}), count, cue_id)

    def test_channels_normalizes_stable_batch_channel_shape(self):
        class FakeTensor:
            def __init__(self, values): self.values = values; self.ndim = 3 if len(values) == 1 else 2
            def detach(self): return self
            def float(self): return self
            def cpu(self): return self
            def __getitem__(self, index): return FakeTensor(self.values[index])
            def tolist(self): return self.values
            def __iter__(self): return iter(self.values)
        self.assertEqual(bulk_generate._channels(FakeTensor([[[1.0, 2.0], [3.0, 4.0]]])), [[1.0, 2.0], [3.0, 4.0]])

    def test_stable_model_id_uses_render_revision_label(self):
        from tools.audio import render
        self.assertEqual(bulk_generate._model_id("stable-small"), render._model_label("small-sfx"))
        self.assertEqual(bulk_generate._model_id("stable-medium"), render._model_label("medium"))

    def test_micro_group_count_resume_and_safe_promote(self):
        with tempfile.TemporaryDirectory() as d:
            root, plan, promote = Path(d) / "out", Path(d) / "plan.json", Path(d) / "content"
            plan.write_text(json.dumps({"execution_plan": [{"cue_id": "audio.ui.hover", "generator": "micro-dsp", "duration": .08, "count": 3, "seed": 4, "kind": "ui"}]}))
            first = bulk_generate.run(plan, root, promote_root=promote)
            self.assertEqual(len(first), 3)
            before = [p.read_bytes() for p in first]
            second = bulk_generate.run(plan, root)
            self.assertEqual([p.read_bytes() for p in second], before)
            self.assertEqual(len(list(promote.glob("*.wav"))), 3)

    def test_stable_groups_load_once_and_short_duration_requests_two_seconds(self):
        class Model:
            sample_rate = 48_000
            def __init__(self): self.calls = []
            def generate(self, **kwargs):
                self.calls.append(kwargs)
                bias = (kwargs["seed"] % 5) * .03
                return [0.3 + bias if (i % 17) < 8 else -0.2 for i in range((kwargs["duration"] * self.sample_rate).__int__())]
        model = Model()
        with tempfile.TemporaryDirectory() as d:
            plan = Path(d) / "plan.json"
            plan.write_text(json.dumps({"cues": [{"cue_id": "a", "generator": "stable-small", "duration": .5, "count": 2, "seed": 8, "kind": "ui"}]}))
            fake = mock.Mock(return_value=model)
            def save(audio, path, _ta, rate):
                with wave.open(str(path), "wb") as wav:
                    wav.setnchannels(1); wav.setsampwidth(2); wav.setframerate(rate)
                    wav.writeframes(b"".join(int(v * 32767).to_bytes(2, "little", signed=True) for v in audio))
            with mock.patch("tools.audio.render._dependencies", return_value=(object(), object())), mock.patch("tools.audio.render._load_model", fake), mock.patch("tools.audio.render._save_audio", side_effect=save):
                out = bulk_generate.run(plan, Path(d) / "out")
            self.assertEqual(len(out), 2); self.assertEqual(fake.call_count, 1)
            self.assertEqual([x["duration"] for x in model.calls], [2.0, 2.0])
            with wave.open(str(out[0]), "rb") as wav: self.assertAlmostEqual(wav.getnframes() / wav.getframerate(), .5, delta=.01)
            self.assertEqual(len({p.read_bytes() for p in out}), 2)

    def test_sidecar_uses_actual_duration_and_bad_resume_is_not_accepted(self):
        with tempfile.TemporaryDirectory() as d:
            plan = Path(d) / "plan.json"; root = Path(d) / "out"
            plan.write_text(json.dumps([{"cue_id": "audio.ui.hover", "generator": "micro-dsp", "duration": .08, "count": 1, "seed": 1, "kind": "ui"}]))
            out = bulk_generate.run(plan, root)[0]
            metadata = json.loads(out.with_suffix(".json").read_text())
            with wave.open(str(out), "rb") as wav: actual = wav.getnframes() / wav.getframerate()
            self.assertEqual(metadata["duration"], actual)
            metadata["duration"] = 99
            out.with_suffix(".json").write_text(json.dumps(metadata))
            replacement = bulk_generate.run(plan, root)[0]
            self.assertEqual(replacement, root / "audio.ui.hover" / "v02.wav")

    def test_changed_plan_cannot_reuse_existing_candidates(self):
        with tempfile.TemporaryDirectory() as d:
            plan, root = Path(d) / "plan.json", Path(d) / "out"
            cue = {"cue_id": "audio.ui.hover", "generator": "micro-dsp",
                   "duration": .08, "count": 1, "seed": 1, "kind": "ui",
                   "prompt": "soft", "gain_db": -3.0}
            plan.write_text(json.dumps([cue]))
            bulk_generate.run(plan, root)
            cue["prompt"] = "sharp"
            cue["lowpass_hz"] = 3200.0
            plan.write_text(json.dumps([cue]))
            with self.assertRaisesRegex(ValueError, "different cue plan"):
                bulk_generate.run(plan, root)

    def test_wav_contract_rejects_overtrimmed_candidate(self):
        with tempfile.TemporaryDirectory() as d:
            raw, final = Path(d) / "raw.wav", Path(d) / "final.wav"
            synth_micro.write_wav(raw, [0.2] * 1920)
            finalize(raw, final, kind="spatial-sfx")
            with self.assertRaisesRegex(ValueError, "shorter than the cue contract"):
                bulk_generate._wav_contract(final, "spatial-sfx", False, .2)

    def test_retry_seed_is_resume_compatible_and_promotion_is_wav_only(self):
        with tempfile.TemporaryDirectory() as d:
            plan = Path(d) / "plan.json"; root = Path(d) / "out"; promote = Path(d) / "promote"
            plan.write_text(json.dumps([{"cue_id": "audio.ui.hover", "generator": "micro-dsp", "duration": .08, "count": 1, "seed": 1, "kind": "ui"}]))
            output = bulk_generate.run(plan, root, promote_root=promote)[0]
            sidecar = output.with_suffix(".json"); metadata = json.loads(sidecar.read_text()); metadata["seed"] = 1 + 1000003; sidecar.write_text(json.dumps(metadata))
            self.assertEqual(bulk_generate.run(plan, root), [output])
            promoted = next(promote.glob("*.wav")); self.assertFalse(promoted.with_suffix(".json").exists())
            bulk_generate.run(plan, root, promote_root=promote)
            promoted.write_bytes(b"different")
            with self.assertRaises(FileExistsError): bulk_generate.run(plan, root, promote_root=promote)

    def test_rejects_escape(self):
        with tempfile.TemporaryDirectory() as d:
            plan = Path(d) / "plan.json"; plan.write_text(json.dumps([{"cue_id": "../outside", "generator": "micro-dsp", "duration": .1}]))
            with self.assertRaises(ValueError): bulk_generate.run(plan, Path(d) / "out")


if __name__ == "__main__": unittest.main()
