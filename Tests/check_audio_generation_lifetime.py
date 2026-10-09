"""Confirm the real AudioEngine owns and releases its selected Generation."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

from check_asset_pipeline_portable import sources, wave


def run(*args):
    p = subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=30)
    assert p.returncode == 0, (args, p.stdout, p.stderr)


def ready(process, marker):
    deadline = time.monotonic() + 10
    while not marker.exists():
        assert process.poll() is None, process.communicate()
        assert time.monotonic() < deadline, f'timeout: {marker}'
        time.sleep(.01)


def main():
    parser = argparse.ArgumentParser()
    for name in ('generation', 'audio', 'audio-probe'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix='hs-audio-generation-lifetime-') as temporary:
        base = Path(temporary)
        root, source = base / 'installed', base / 'source'
        audio, catalog_path = sources(source)
        catalog = json.loads(catalog_path.read_text())
        for entry in catalog['entries']:
            entry.update(asset_id=entry['id'], submix='SFX', priority='Other',
                         spatial=False, preload=False, loop=False,
                         max_simultaneous=1, minimum_retrigger_ms=0)
        catalog_path.write_text(json.dumps(catalog))
        producer = [args.audio, '--produce', '--source-root', source, '--build-root', root]
        def compose():
            run(args.generation, '--compose', '--build-root', root, '--scope', 'runtime-full',
                '--result', root / 'Actions/audio/result.json')
        def gc():
            run(args.generation, '--gc', '--build-root', root, '--grace-seconds', 0)
        run(*producer); compose()
        pointer = root / 'GenerationState/runtime-full.json'
        old = json.loads(pointer.read_text())['generation_id']
        executable = root / 'audio_lifetime_probe.exe'
        shutil.copy2(args.audio_probe, executable)
        ready_file, release, exit_file, retry = [base / x for x in ('ready', 'release', 'exit', 'retry')]
        p = subprocess.Popen([str(executable), '--hold', str(ready_file), str(release),
                              str(exit_file), str(retry)], stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True)
        try:
            ready(p, ready_file)
            (audio / 'wave_000.wav').write_bytes(wave(1))
            run(*producer); compose(); gc()
            assert (root / 'Generations' / old).exists(), 'AudioEngine failed to retain old snapshot'
            current = pointer.read_bytes()
            pointer.write_text('{}')
            retry.touch()
            ready(p, Path(str(retry) + '.checked'))
            pointer.write_bytes(current)
            gc()
            assert (root / 'Generations' / old).exists(), 'failed reselect lost prior lease'
            release.touch()
            ready(p, Path(str(ready_file) + '.shutdown'))
            gc()
            assert not (root / 'Generations' / old).exists(), 'AudioEngine Shutdown leaked lease'
            exit_file.touch()
            stdout, stderr = p.communicate(timeout=10)
            assert p.returncode == 0, (stdout, stderr)
            print(f'PASS AudioEngine retain/reselect/Shutdown GC elapsed={time.monotonic()-started:.3f}s')
        finally:
            if p.poll() is None:
                p.kill(); p.communicate(timeout=5)


if __name__ == '__main__':
    main()
