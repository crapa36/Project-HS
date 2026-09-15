# Local audio runtime

## Stable Audio

Use `tools\audio\.venv-stable` and recreate it from `requirements-stable.in`.

Model revisions are pinned there and require the applicable model terms to have been accepted.

- `small-sfx` may run on CPU.
- `medium` requires CUDA, Flash Attention 2, and Linux/WSL.
- Native Windows is not supported for the medium path.
- Stable Audio uses only `HF_TOKEN`.
- Load one model per process; do not keep Stable Audio and the vocal model family resident in GPU memory together.
- Negative prompts are advisory and must not replace listening/validation.

## Nonverbal vocal

Use:

```text
tools\audio\.venv-vocal\Scripts\python.exe tools/audio/vocal.py generate
```

Recreate the environment from `requirements-vocal.in`.

Chatterbox Turbo is pinned to revision:

```text
749d1c1a46eb10492095d68fbcf55691ccf137cd
```

Allowed events:

```text
[clear throat]
[sigh]
[shush]
[cough]
[groan]
[sniff]
[gasp]
[chuckle]
[laugh]
```

Default: `[groan]`.

Example:

```powershell
tools\audio\.venv-vocal\Scripts\python.exe tools/audio/vocal.py generate --event groan --count 3 --seed 41 --output-dir GeneratedAudio\audio.vocal.groan
tools\audio\.venv-vocal\Scripts\python.exe tools/audio/finalize.py GeneratedAudio\audio.vocal.groan\v01.wav GeneratedAudio\audio.vocal.groan\v01.wav --kind vocal-sfx
```

A reference voice is optional because the pinned snapshot contains built-in conditions.

If `--reference-voice` is used, it must point to an existing clip longer than five seconds. Do not synthesize, clone, or invent a reference voice when one was not supplied.
