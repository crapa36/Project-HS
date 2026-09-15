---
name: project-hs-audio
description: Author or modify Project-HS game audio assets: SFX, UI sounds, loops, BGM, ambience, and nonverbal character vocals. Do not use for XAudio2/runtime audio code debugging.
---

# Project-HS audio authoring

Resolve the target cue in `ContentSource/GameData/audio_cues.json` and read only the relevant authoring contract. Use nearby cues only when needed to infer a noncritical convention.

If a missing value would materially change the asset or its acceptance and cannot be inferred safely, ask for that value. Otherwise choose a reasonable authoring detail and continue.

## Route

- UI SFX below 100 ms: `tools/audio/synth_micro.py`
- Loop below 1 second: create a loop-safe seed with `tools/audio/synth_micro.py`; use Stable Audio medium audio-to-audio only when AI texture is needed.
- Ordinary one-shot SFX and UI SFX at or above 100 ms:
  `tools\audio\.venv-stable\Scripts\python.exe tools/audio/render.py generate --model small-sfx`
- High-quality SFX, BGM, ambience, or variation of an existing candidate: use `render.py refine` with the medium model.
- Local repair: use `render.py inpaint` with the medium model.
- Nonverbal character vocal: use `tools\audio\.venv-vocal\Scripts\python.exe tools/audio/vocal.py generate`.

Read `references/RUNTIME.md` only when model/environment requirements, vocal constraints, or environment recreation details are needed.

## Authoring

Build the generation prompt from the cue and request using only relevant characteristics such as event, material, attack/body, texture, pitch movement, frequency character, tail, duration, and realistic/fantasy character.

Do not combine independent effects into one candidate.

For bow release, center the sound on `bowstring snap` with a short high-frequency arrow/air whoosh; wood click must not become the main transient.

Fantasy effects may use shimmer, harmonic ping, filtered noise, or resonance, but not gun, laser, or missile character.

Pass forbidden timbres as `negative_prompt` where supported, but do not treat a negative prompt as validation.

## Candidates and finalization

Write generated candidates under:

```text
GeneratedAudio/<cue-id>/vNN.wav
```

with a same-stem JSON sidecar.

Never overwrite or delete existing candidates, and never replace a game asset automatically.

Pass every generated candidate through `tools/audio/finalize.py`.

Finalization requirements:

- one-shot: trim only while preserving attack and tail
- `spatial-sfx`, `vocal-sfx`, and UI: mono
- BGM and ambience: stereo
- 48 kHz
- 24-bit PCM WAV
- attenuate only when peak exceeds the applicable limit
- validate decode, finite samples, non-silence, sample rate, bit depth, channels, peak, and loop boundary where applicable

Do not add default EQ, compression, saturation, reverb, widening, automatic crossfade, or automatic quality scoring.

Approved files use:

```text
<cue-id with dots replaced by underscores>_vNN.wav
```

Final timbre and usability are accepted by listening, not by an automatic quality score.

Report candidate paths and decisive validation results.
