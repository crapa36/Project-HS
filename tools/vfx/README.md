# Local VFX reference review

This is analysis preparation, not a completed review and not a production VFX change.
It uses Python 3.10+ and FFmpeg/ffprobe supporting `-fps_mode passthrough`.
No Python packages need installing. No renderer or content build dependency is added.

## Private local inputs and outputs

Keep downloaded references outside the repository. No Drive URLs, credentials,
reference footage, extracted third-party art, or media evidence belongs in Git.
The tool writes a `.gitignore` containing `*` in every newly created output root.
It never overwrites an existing output root or modifies source media.
This guard is not a substitute for checking `git status` before committing.

## 1. Decode every source and build navigation

```powershell
python tools/vfx/reference_review.py inventory "D:/VFX_References" --out "D:/VFX_Review/run01" --expected-count 32 --interval 2
```

Also works with POSIX paths on Linux/macOS. Input discovery is recursive and
supports MP4, WebM, MOV, MKV, GIF, PNG, JPG and JPEG. `--expected-count 32`
checks the agreed input count; it does not validate the identity of those files.
An inventory records names, bytes, SHA-256, codec, dimensions, average/rational
frame rate, and duration. Full visual-stream decoding checks the entire file,
not merely its beginning/middle/end. Audio is not validated by this VFX tool.
Errors remain per-source errors; they do not become successful reviews.

Open `index.html` locally. Each source page includes the original video or image,
plus navigation thumbnails labelled with actual selected frame timestamps.
The first decoded source frame defines time zero. A 2-second thumbnail interval
is ONLY coarse navigation. It can miss an entire quick skill; it does not
constitute watching the video, motion analysis, or automatic VFX recognition.
Use the original player at normal speed to locate actions and classify intros,
menus, repeated demonstrations, and useful effects before selecting detail spans.
The HTML player requires the original files to remain at their local paths.

## 2. Extract actual native frames around a selected action

```powershell
python tools/vfx/reference_review.py detail "D:/VFX_References/example.mp4" --start 12.3 --end 13.5 --out "D:/VFX_Review/detail01"
```

The timestamps above are an invocation example, NOT measurements of a reference.
The tool decodes from the beginning to avoid inaccurate keyframe-only seeking,
normalizes source time once, and trims on decoded frame timestamps. Every selected
frame is saved as native-resolution PNG. There is no `fps` filter, forced 60fps,
interpolation, or optical-flow reconstruction. GIF variable frame delays and
variable-rate video intervals are represented by their decoded timestamps rather
than being treated as an invented fixed frame rate.

`frames.json` records the source SHA-256, interval, exact extracted frame times,
and `resampled: false`. The interval is half-open: start <= t < end.
Each request is limited to 30 seconds to bound full-resolution evidence storage.
Do not infer exposure/blend mode, mesh topology, or a particular shader solely
from frames; corroborate implementation hypotheses with creator breakdowns.

## 3. Record observations separately from implementation hypotheses

Copy `review.template.json` to `review.json` outside Git and populate each source:

- `reviewed`: true only after actual visual inspection;
- `observed`: concrete things seen, with referenced interval/frame evidence;
- `implementation_hypotheses`: plausible mesh/texture/shader construction, not facts;
- `coverage`: ordered, continuous source intervals with `start`, `end`, `kind`
  (`overview`, `detail`, or `excluded`) and nonempty `notes`.

Record normal-speed movement as well as native detail frames. Include an exclusion
reason for non-VFX sections; do not silently omit sources or inconvenient intervals.
Coverage runs from zero to the recorded duration without gaps or overlaps. Stills
need observations, not a fabricated motion timeline. Do not use timing or material
claims from coarse thumbnails as if they came from native frame inspection.

```powershell
python tools/vfx/reference_review.py check-review "D:/VFX_Review/run01/inventory.json" "D:/VFX_Review/run01/review.json"
```

The checker validates review declarations and fingerprints against the inventory.
It does NOT prove that a reviewer watched anything, verify external evidence files,
re-hash sources, or judge visual quality. A passing declaration is not permission
to claim automatic visual acceptance. Regenerate the inventory if inputs change.

## Tests

```powershell
python -m unittest discover -s tools/vfx -p "test_*.py" -v
```

The dedicated CI workflow uses synthetic MP4/GIF/PNG/VFR media only. It neither
fetches the user's Drive data nor publishes reference media. Unit/integration
results establish tool behavior, not completion of the 32-source visual review.

## Implementation reference

- FFmpeg timestamp and passthrough semantics: https://ffmpeg.org/ffmpeg.html
- FFmpeg trim/showinfo filters: https://ffmpeg.org/ffmpeg-filters.html
- ffprobe structured metadata: https://ffmpeg.org/ffprobe.html
