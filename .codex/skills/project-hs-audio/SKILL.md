---
name: project-hs-audio
description: Use for Project-HS SFX, BGM, ambience, UI, loop, nonverbal character vocal, grunt, exertion, and local vocal authoring; routes each request to the smallest verified local generator and finalizer.
---

# Project-HS audio authoring

Use this skill for: 새 SFX 생성, 기존 SFX variation 생성, 기존 SFX 수정, BGM 생성, ambience 생성, UI sound 생성, loop sound 생성, nonverbal character vocal 생성, and Project-HS용 게임 사운드 제작.

1. Find exactly one matching entry in `ContentSource/GameData/audio_cues.json`. Read only that cue's authoring requirements. If target length, variation count, loop, pitch/frequency, layers, envelope, timbre, forbidden timbres, or mono/stereo channel is not defined, ask the user for those values; do not guess.
2. Route the request:
   - 100ms 미만 UI SFX: `tools/audio/synth_micro.py` (AI is not used).
   - 1초 미만 loop: `tools/audio/synth_micro.py`로 loop-safe seed를 만든다. If AI texture is explicitly needed, use Stable Audio 3 medium audio-to-audio on that seed, then finalize and check the boundary.
   - ordinary one-shot SFX and 100ms 이상 UI SFX: `tools\audio\.venv-stable\Scripts\python.exe tools/audio/render.py generate --model small-sfx`.
   - high-quality, BGM, ambience, or existing-candidate variation: use the same interpreter with `render.py refine` and medium.
   - local repair: use the same interpreter with `render.py inpaint` and medium.
   - nonverbal vocal only: use `tools\audio\.venv-vocal\Scripts\python.exe tools/audio/vocal.py generate`. This is local Chatterbox Turbo only: allowed tags are `[clear throat]`, `[sigh]`, `[shush]`, `[cough]`, `[groan]`, `[sniff]`, `[gasp]`, `[chuckle]`, and `[laugh]`; default is `[groan]`. Generate raw candidates, then run `tools\audio\.venv-vocal\Scripts\python.exe tools/audio/finalize.py --kind vocal-sfx` and listen.
3. Compose a concise English prompt from the cue and user request: event, material, attack, body, texture, pitch movement, frequency character, tail, duration character, and fantasy/realistic character. Do not combine independent effects. For bow release, center `bowstring snap` plus a short high-frequency arrow/air whoosh; avoid wood click as the main transient. Fantasy effects may use shimmer, harmonic ping, filtered noise, or resonance, but not gun/laser/missile character. Pass forbidden timbres as `negative_prompt`; Stable Audio 3 post-trained checkpoints may not reliably honor negative prompts.
4. Keep environments separate: `tools\audio\.venv-stable` for Stable Audio 3 and `tools\audio\.venv-vocal` for Chatterbox Turbo. Recreate them from `requirements-stable.in` and `requirements-vocal.in`. Stable Audio uses only `HF_TOKEN`; `small-sfx` can run on CPU, while `medium` requires CUDA + Flash Attention 2 and Linux/WSL because native Windows is unsupported. Stable model revisions are recorded in `requirements-stable.in` and require accepted model terms. Chatterbox Turbo is pinned to revision `749d1c1a46eb10492095d68fbcf55691ccf137cd`. Load only one model per process and never keep both model families in GPU memory.
5. Render candidates, then pass every result through `tools/audio/finalize.py`. Generated candidates live under `GeneratedAudio/<cue-id>/vNN.wav` with a same-stem JSON sidecar. Unused sidecar fields are null. Never overwrite existing candidates, delete candidates, or replace game assets automatically.
6. Finalize rules: one-shot trim only; preserve attack and tail. Convert channels by cue (`spatial-sfx`, `vocal-sfx`, and UI mono; BGM and ambience stereo), resample to 48kHz, save 24-bit PCM WAV, attenuate only when peak exceeds the applicable limit, and validate decode, finite samples, non-silence, sample rate, bit depth, channels, peak, and loop boundary. Do not add default EQ, compression, saturation, reverb, widening, automatic crossfade, or automatic quality scoring.
7. Report paths and validation. Final timbre is accepted by user listening, not an automatic score. Approved files use `<cue-id with dots replaced by underscores>_vNN.wav`; do not overwrite existing assets.

Example local nonverbal vocal call (write a raw WAV candidate, then finalize):

```powershell
tools\audio\.venv-vocal\Scripts\python.exe tools/audio/vocal.py generate --event groan --count 3 --seed 41 --output-dir GeneratedAudio\audio.vocal.groan
tools\audio\.venv-vocal\Scripts\python.exe tools/audio/finalize.py GeneratedAudio\audio.vocal.groan\v01.wav GeneratedAudio\audio.vocal.groan\v01.wav --kind vocal-sfx
```

The pinned Chatterbox Turbo snapshot includes built-in conditions, so a
reference is optional. If supplied, `--reference-voice` must point to an
existing clip longer than five seconds. Do not clone, synthesize, or silently
invent a reference voice. Every candidate gets a same-stem sidecar; review
candidates by listening before approving one.
