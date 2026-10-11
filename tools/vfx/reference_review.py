#!/usr/bin/env python3
"""Local-only reference ingestion. Extraction is NOT a visual review.

Python 3.10+, FFmpeg/ffprobe with -fps_mode support. No third-party Python
packages, network access, source modification, or runtime dependencies.
"""
from __future__ import annotations

import argparse
import hashlib
import html
import json
import math
from pathlib import Path
import re
import shutil
import subprocess
import sys
from typing import Any

MEDIA = {'.mp4', '.webm', '.mov', '.mkv', '.gif', '.png', '.jpg', '.jpeg'}
STILLS = {'.png', '.jpg', '.jpeg'}
PTS = re.compile(r'\bn:\s*(\d+)\s+pts:\s*-?\d+\s+pts_time:\s*([-+0-9.eE]+)')


def write_json(path: Path, value: Any) -> None:
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    temporary.replace(path)


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def number(value: Any, default: float = 0.0) -> float:
    try:
        result = float(value)
        return result if math.isfinite(result) else default
    except (TypeError, ValueError):
        return default


def positive(value: str) -> float:
    result = float(value)
    if not math.isfinite(result) or result <= 0:
        raise argparse.ArgumentTypeError('Expected a finite number greater than zero')
    return result


def make_output(output: Path, source: Path) -> Path:
    output, source = output.resolve(), source.resolve()
    if output == source or output.is_relative_to(source):
        raise ValueError('Output must be outside the input file/directory')
    output.mkdir(parents=True, exist_ok=False)
    # Review evidence can contain copyrighted reference footage and private paths.
    (output / '.gitignore').write_text('*\n', encoding='utf-8')
    return output


def check_tools() -> None:
    for name in ('ffmpeg', 'ffprobe'):
        if shutil.which(name) is None:
            raise RuntimeError(f'{name} is not available on PATH')


def run(command: list[str], log: Path, timeout: float) -> None:
    with log.open('wb') as stream:
        result = subprocess.run(command, stdin=subprocess.DEVNULL,
                                stdout=subprocess.DEVNULL, stderr=stream,
                                timeout=timeout, check=False)
    if result.returncode:
        raise RuntimeError(f'{command[0]} failed ({result.returncode}); see {log.name}')


def probe(path: Path, timeout: float) -> dict[str, Any]:
    result = subprocess.run(
        ['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries',
         'stream=codec_name,width,height,avg_frame_rate,r_frame_rate,duration:format=duration',
         '-of', 'json', str(path)], capture_output=True, timeout=timeout, check=False)
    if result.returncode:
        raise RuntimeError('ffprobe failed: ' + result.stderr.decode('utf-8', errors='replace')[-2000:])
    data = json.loads(result.stdout)
    if not data.get('streams'):
        raise ValueError('No decodable visual stream')
    stream = data['streams'][0]
    duration = number(stream.get('duration'), number(data.get('format', {}).get('duration')))
    return {'stream': stream, 'duration_seconds': duration}


def decode_all(path: Path, log: Path, timeout: float) -> None:
    run(['ffmpeg', '-nostdin', '-hide_banner', '-v', 'error', '-xerror',
         '-err_detect', 'explode', '-threads', '2', '-i', str(path),
         '-map', '0:v:0', '-an', '-f', 'null', '-'], log, timeout)


def parse_timestamps(log: Path) -> list[float]:
    found = []
    for line in log.read_text(encoding='utf-8', errors='replace').splitlines():
        match = PTS.search(line)
        if match:
            if int(match.group(1)) != len(found):
                raise ValueError('Non-sequential showinfo frame indices')
            timestamp = float(match.group(2))
            if not math.isfinite(timestamp):
                raise ValueError('Non-finite frame timestamp')
            found.append(timestamp)
    return found


def extract_frames(path: Path, output: Path, filters: str, timeout: float,
                   extension: str = 'jpg') -> list[dict[str, Any]]:
    output.mkdir(parents=True, exist_ok=False)
    log = output / 'ffmpeg.log'
    # Normalize once to first decoded source frame, then select/trim. showinfo is
    # deliberately before encoding. Never insert fps/-r/interpolation for detail.
    run(['ffmpeg', '-nostdin', '-hide_banner', '-v', 'info', '-xerror',
         '-threads', '2', '-i', str(path), '-map', '0:v:0', '-an',
         '-filter_threads', '1', '-vf', filters + ',showinfo',
         '-fps_mode', 'passthrough', '-threads', '1',
         str(output / ('%06d.' + extension))], log, timeout)
    timestamps = parse_timestamps(log)
    images = sorted(output.glob('*.' + extension))
    if not images or len(images) != len(timestamps):
        raise ValueError(f'Frame/timestamp mismatch: {len(images)} images, {len(timestamps)} timestamps')
    return [{'file': image.name, 'source_seconds': timestamp}
            for image, timestamp in zip(images, timestamps)]


def gallery(path: Path, title: str, source: Path, frames: list[dict[str, Any]],
            prefix: str, still: bool = False) -> None:
    e = html.escape
    player = (f'<img class="source" src="{e(source.as_uri(), quote=True)}">' if still else
              f'<video class="source" controls preload="metadata" src="{e(source.as_uri(), quote=True)}"></video>')
    cards = '\n'.join(
        f'<figure><a href="{e(prefix + item["file"], quote=True)}"><img loading="lazy" '
        f'src="{e(prefix + item["file"], quote=True)}"></a>'
        f'<figcaption>{item["source_seconds"]:.6f} s</figcaption></figure>'
        for item in frames)
    path.write_text('<!doctype html><meta charset="utf-8"><title>' + e(title) + '</title>'
        '<style>body{font:16px system-ui;margin:24px;background:#171a21;color:#eee}'
        '.source{max-width:100%;max-height:70vh}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(240px,1fr));gap:12px}'
        'figure{margin:0}figure img{width:100%}figcaption{padding:4px}a{color:#9ce}</style>'
        '<h1>' + e(title) + '</h1><p>Extraction only; NOT a completed visual review. '
        'Times are relative to the first decoded source frame. Open original for normal-speed motion.</p>'
        + player + '<div class="grid">' + cards + '</div>', encoding='utf-8')


def inventory(root: Path, output: Path, interval: float, timeout: float,
              expected_count: int | None = None) -> dict[str, Any]:
    check_tools()
    root = root.resolve()
    if not root.is_dir():
        raise ValueError('Input must be an existing directory')
    sources = sorted(p for p in root.rglob('*') if p.is_file() and p.suffix.lower() in MEDIA)
    if not sources:
        raise ValueError('No supported media files found')
    if expected_count is not None and len(sources) != expected_count:
        raise ValueError(f'Expected {expected_count} media files; found {len(sources)}')
    for source in sources:
        if not source.resolve().is_relative_to(root):
            raise ValueError('Input symlink escapes the reference directory')
    output = make_output(output, root)
    result: dict[str, Any] = {'schema_version': 1, 'source_root': str(root),
                             'visual_review_complete': False, 'files': []}
    links, review = [], []
    for index, source in enumerate(sources, 1):
        key = f'source_{index:03d}'
        folder = output / key
        folder.mkdir()
        item: dict[str, Any] = {'id': key, 'relative_path': source.relative_to(root).as_posix(),
                               'bytes': source.stat().st_size, 'sha256': digest(source),
                               'kind': 'still' if source.suffix.lower() in STILLS else 'motion',
                               'status': 'pending'}
        try:
            item.update(probe(source, timeout))
            if item['kind'] == 'motion' and item['duration_seconds'] <= 0:
                raise ValueError('Motion source has no valid duration')
            decode_all(source, folder / 'decode.log', timeout)
            # Periodic sampling is navigation, not complete action recognition.
            # Low-contrast or sub-frame flashes need detail review at native rate.
            filters = f"setpts=PTS-STARTPTS,select='isnan(prev_selected_t)+gte(t-prev_selected_t,{interval:.9f})',scale=480:-2"
            frames = extract_frames(source, folder / 'overview', filters, timeout)
            write_json(folder / 'overview_frames.json', frames)
            gallery(folder / 'index.html', source.name, source, frames, 'overview/', item['kind'] == 'still')
            item['status'] = 'decoded_and_sampled_not_reviewed'
            item['sampled_frames'] = len(frames)
            item['evidence_directory'] = key
            links.append(f'<li><a href="{key}/index.html">{html.escape(source.name)}</a></li>')
        except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
            item['status'], item['error'] = 'error', str(error)
            links.append('<li>ERROR: ' + html.escape(source.name) + '</li>')
        result['files'].append(item)
        review.append({'id': key, 'sha256': item['sha256'], 'reviewed': False,
                       'observed': [], 'implementation_hypotheses': [], 'coverage': []})
        write_json(output / 'inventory.json', result)
        print(f'{index}/{len(sources)} {item["status"]}: {source.name}', flush=True)
    write_json(output / 'review.template.json', {'schema_version': 1, 'files': review})
    (output / 'index.html').write_text('<!doctype html><meta charset="utf-8"><h1>VFX reference navigation</h1>'
        '<p>All entries are unreviewed until actual visual inspection. Thumbnail sampling is not complete temporal coverage.</p><ul>'
        + ''.join(links) + '</ul>', encoding='utf-8')
    return result


def detail(source: Path, output: Path, start: float, end: float, timeout: float) -> dict[str, Any]:
    check_tools()
    source = source.resolve()
    if not source.is_file() or source.suffix.lower() not in MEDIA:
        raise ValueError('Input must be a supported local media file')
    metadata = probe(source, timeout)
    if not math.isfinite(start) or not math.isfinite(end) or start < 0 or end <= start:
        raise ValueError('Require finite 0 <= start < end')
    if end > metadata['duration_seconds'] + 0.001:
        raise ValueError('Detail end exceeds source duration')
    if end - start > 30:
        raise ValueError('Limit each native-resolution detail interval to 30 seconds')
    output = make_output(output, source)
    frames = extract_frames(source, output / 'frames',
        f'setpts=PTS-STARTPTS,trim=start={start:.9f}:end={end:.9f}', timeout, 'png')
    record = {'schema_version': 1, 'sha256': digest(source), 'start_seconds': start,
              'end_seconds': end, 'time_origin': 'first_decoded_source_frame',
              'resampled': False, 'visual_review_complete': False, 'frames': frames}
    write_json(output / 'frames.json', record)
    gallery(output / 'index.html', source.name, source, frames, 'frames/')
    return record


def validate_review(inventory_data: dict[str, Any], review_data: dict[str, Any]) -> list[str]:
    """Check review declarations, NOT whether a person actually watched footage."""
    errors: list[str] = []
    sources = inventory_data.get('files', [])
    reviews = review_data.get('files', [])
    ids = [item.get('id') for item in reviews]
    expected = [item.get('id') for item in sources]
    if not sources or len(set(expected)) != len(expected):
        return ['Inventory is empty or contains duplicate ids']
    if len(ids) != len(set(ids)) or set(ids) != set(expected):
        return ['Review must contain exactly one entry for every inventory source']
    by_id = {item['id']: item for item in reviews}
    for source in sources:
        key = source['id']
        review = by_id[key]
        if source.get('status') != 'decoded_and_sampled_not_reviewed':
            errors.append(f'{key}: source decode/extraction not successful')
        if review.get('sha256') != source.get('sha256'):
            errors.append(f'{key}: source fingerprint mismatch')
        if review.get('reviewed') is not True:
            errors.append(f'{key}: visual review not declared complete')
        observations = review.get('observed', [])
        if not isinstance(observations, list) or not observations or any(not isinstance(x, str) or not x.strip() for x in observations):
            errors.append(f'{key}: concrete visual observations required')
        if source.get('kind') == 'still':
            continue
        coverage = review.get('coverage', [])
        if not isinstance(coverage, list) or not coverage:
            errors.append(f'{key}: motion coverage required')
            continue
        cursor = 0.0
        for interval in coverage:
            start = number(interval.get('start'), -1)
            end = number(interval.get('end'), -1)
            if abs(start - cursor) > 0.001 or end <= start:
                errors.append(f'{key}: coverage gap, overlap, or invalid interval')
            if interval.get('kind') not in ('detail', 'overview', 'excluded'):
                errors.append(f'{key}: invalid coverage kind')
            if not isinstance(interval.get('notes'), str) or not interval['notes'].strip():
                errors.append(f'{key}: every interval needs observations or an exclusion reason')
            cursor = end
        if abs(cursor - source.get('duration_seconds', 0)) > 0.001:
            errors.append(f'{key}: coverage does not end at source duration')
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    scan = commands.add_parser('inventory')
    scan.add_argument('root', type=Path)
    scan.add_argument('--out', type=Path, required=True)
    scan.add_argument('--interval', type=positive, default=2.0)
    scan.add_argument('--expected-count', type=int)
    scan.add_argument('--timeout', type=positive, default=1800.0)
    cut = commands.add_parser('detail')
    cut.add_argument('source', type=Path)
    cut.add_argument('--out', type=Path, required=True)
    cut.add_argument('--start', type=float, required=True)
    cut.add_argument('--end', type=float, required=True)
    cut.add_argument('--timeout', type=positive, default=1800.0)
    gate = commands.add_parser('check-review')
    gate.add_argument('inventory', type=Path)
    gate.add_argument('review', type=Path)
    args = parser.parse_args()
    try:
        if args.command == 'inventory':
            data = inventory(args.root, args.out, args.interval, args.timeout, args.expected_count)
            return 1 if any(x['status'] == 'error' for x in data['files']) else 0
        if args.command == 'detail':
            data = detail(args.source, args.out, args.start, args.end, args.timeout)
            print(f'Extracted {len(data["frames"])} native frames; visual review is still pending')
            return 0
        issues = validate_review(json.loads(args.inventory.read_text(encoding='utf-8')),
                                 json.loads(args.review.read_text(encoding='utf-8')))
        for issue in issues:
            print(issue, file=sys.stderr)
        if not issues:
            print('Review declarations are structurally complete; this is not automated visual acceptance')
        return 1 if issues else 0
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(str(error), file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
