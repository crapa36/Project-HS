# DEV_STATE

## Objective

Generate every cue in the Project-HS AI audio guide locally, promote the final
WAV files into authoritative content, and play them through the existing
gameplay, presentation, content-cooking, and runtime paths.

## Implemented State

- `ContentSource/Audio` contains all 379 required WAV variations for the 107
  catalog cues. The catalog and generated file sets match exactly.
- All assets are 48 kHz, 24-bit PCM. Spatial, UI, and vocal assets are mono;
  the two ambience variations and three BGM tracks are stereo.
- Stable Audio small generated 354 SFX variations, deterministic micro DSP
  generated 15 UI/short-loop variations, Chatterbox generated five nonverbal
  vocal variations, and Stable Audio medium generated five ambience/BGM files.
- The pinned model revisions remain Stable Audio small
  `ae12755283df9d62ca39a9b050a39a0b607b8c20`, Stable Audio medium
  `27b5a21b791b1b033d193a9e1e3ce78493f102f9`, and Chatterbox Turbo
  `749d1c1a46eb10492095d68fbcf55691ccf137cd`.
- The local `project-hs-audio` skill and `tools/audio` pipeline generate,
  finalize, validate, resume, and safely promote cue batches. RIFF output now
  writes the required pad byte for odd-sized data chunks. The complete 107-cue
  generation plan is versioned; model caches and intermediates remain ignored.
- The content schema validates the catalog. The cooker validates and copies
  every referenced WAV into `Cooked/Audio`, includes them in the manifest and
  content hash, and keeps them out of the deterministic gameplay hash.
- Gameplay emits semantic audio domain signals. Presentation maps those and UI
  interactions to catalog cue IDs. Runtime routes the resulting audio events
  through XAudio2 with catalog-defined variations, buses, priorities,
  retrigger/concurrency limits, XP pickup aggregation, arrow-impact rate
  limiting, looping, 35 m/90 m spatial attenuation, pause/resume, lazy payload
  loading, and voice reclamation.
- Main-menu BGM starts with the runtime. Timed bosses emit a 1.5-second warning
  before spawning, and final-boss context switches playback to final-boss BGM.
- Runtime initializes audio from `Cooked/Audio`; packaging installs the cooked
  catalog and WAV files.

## Verification

- Asset audit: PASS, 107 cues and 379 catalog-matched WAV files; 379 unique
  SHA-256 digests; 374 mono and five stereo files.
- Audio tool unittest discovery: PASS, 46/46 tests.
- Source routing audit: PASS, every catalog cue ID is referenced by production
  source.
- `cmake --workflow --preset verify-core`: PASS, 5/5 tests.
- `cmake --workflow --preset verify`: PASS, 20/20 tests, including content
  validation/cooking, render smoke, barrier validation, deterministic gameplay,
  runtime integration, and offscreen experiment.
- `git diff --check`: PASS before final state documentation update.

## Environment Notes

The local CUDA generation paths use isolated ignored Python environments.
Flash Attention and Triton are unavailable on native Windows, so Stable Audio
uses its supported fallback path. Model weights and generation intermediates
remain outside versioned content.

## Remaining Acceptance

Automated format, content, runtime, and integration checks are complete. Final
timbre, musical fit, loudness balance, and in-game listening approval remain a
user play-review decision.
