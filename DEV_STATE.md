# DEV_STATE

## Objective

Retune harsh combat audio, replace charged-shot and enemy cue character, add
per-pulse Arrow Rain audio, and restore skill-menu interaction in the Tab
character window.

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
- Fifty-nine combat WAV variations were regenerated with warmer prompts. The
  charged-shot loop now uses deterministic non-tonal wood/friction noise instead
  of an oscillator stack. Explicit per-cue gain and low-pass mastering are
  supported without changing the default finalization path.
- Every `ArrowRainPulse` projects both incoming-arrow and impact audio.
- Runtime publishes the latest simulation `SessionProbe` through a synchronized
  snapshot so Tab-window skill selection and loadout swaps use real skill levels
  and loadout state instead of a default-empty probe.

## Verification

- Asset audit: PASS, 107 cues and 379 catalog-matched WAV files; 379 unique
  SHA-256 digests; 374 mono and five stereo files.
- Audio tool unittest discovery: PASS, 51/51 tests.
- Source routing audit: PASS, every catalog cue ID is referenced by production
  source.
- Full MSVC Debug build (`msvc-debug`): PASS.
- Debug CTest: PASS, 21/21 tests, including content validation/cooking, render
  smoke, barrier validation, deterministic gameplay, runtime integration, and
  offscreen experiment.
- Retuned-cue analysis: all previously flagged sharp cues are below the audit's
  combined high-frequency/loudness threshold; enemy ranged release is balanced
  to approximately -28 dBFS RMS.
- Candidate resume now rejects changed prompt/mastering plans, and finalized
  non-loop candidates must retain at least half of their planned duration.

## Environment Notes

The local CUDA generation paths use isolated ignored Python environments.
Flash Attention and Triton are unavailable on native Windows, so Stable Audio
uses its supported fallback path. Model weights and generation intermediates
remain outside versioned content.

## Remaining Acceptance

Automated format, content, runtime, and integration checks are complete. Final
timbre, musical fit, loudness balance, and in-game listening approval remain a
user play-review decision.
