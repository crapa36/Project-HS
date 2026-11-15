"""Real resolver/producer/composer/GC races, without GPU or repository assets."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import struct
import tempfile
import time

def wave(sample=0):
    data = bytes([sample, 0, 0, 0, 0, 0])
    return struct.pack('<4sI4s4sIHHIIHH4sI', b'RIFF', 36 + len(data), b'WAVE',
                       b'fmt ', 16, 1, 1, 48000, 144000, 3, 24, b'data', len(data)) + data


def sources(root):
    audio = root / 'ContentSource/Audio'
    audio.mkdir(parents=True)
    catalog = root / 'ContentSource/GameData/audio_cues.json'
    catalog.parent.mkdir(parents=True)
    entries = []
    index = 0
    for cue in range(2):
        names = []
        for _ in range(2 if cue == 0 else 1):
            name = f'wave_{index:03}.wav'
            (audio / name).write_bytes(wave())
            names.append(name)
            index += 1
        entries.append(dict(id=f'audio.test.{cue}', asset_id=f'audio.test.{cue}', submix='SFX',
                            priority='Other', spatial=False, preload=False, loop=False,
                            max_simultaneous=1, minimum_retrigger_ms=0,
                            encoding='PCM', streaming=False, files=names))
    catalog.write_text(json.dumps(dict(schema_version=1, category='audio_cues',
                                      maximum_source_voices=64, entries=entries)))
    return audio, catalog


def run(*args, success=True):
    p = subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=30)
    assert (p.returncode == 0) == success, (args, p.returncode, p.stdout, p.stderr)
    return p.stdout + p.stderr


def wait_ready(process, ready):
    deadline = time.monotonic() + 10
    while not ready.exists():
        assert process.poll() is None, process.communicate()
        assert time.monotonic() < deadline, 'probe readiness timeout'
        time.sleep(.01)


def finish(process):
    stdout, stderr = process.communicate(timeout=30)
    assert process.returncode == 0, (process.returncode, stdout, stderr)


def snapshot_tree(root):
    return {
        p.relative_to(root).as_posix(): p.read_bytes()
        for p in root.rglob('*') if p.is_file()
    }


def publish(root, set_id, data):
    locator = f'Products/fixture/{set_id}/product.bin'
    path = root / locator
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    result = root / 'Actions/fixture/result.json'
    result.parent.mkdir(parents=True, exist_ok=True)
    result.write_text(json.dumps(dict(schema_version=1, action_id='fixture', namespace_path='fixture',
        action_key='sha256:' + set_id, product_set_id=set_id, upstream_product_dependencies=[],
        products=[dict(logical_id='fixture/product', locator=locator,
            digest='sha256:' + hashlib.sha256(data).hexdigest(), size=len(data))])))
    return result


def main():
    parser = argparse.ArgumentParser()
    for name in ('generation', 'probe', 'audio'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    processes = []
    started = time.monotonic()
    # Quarantined paths contain two SHA256 names plus a native temporary suffix.
    # Keep fixture names compact so ordinary Windows I/O stays below MAX_PATH.
    with tempfile.TemporaryDirectory(prefix='hs-gen-') as temporary:
        base = Path(temporary)
        root = base / 'build'
        def compose(scope='test', result=None, success=True):
            return run(args.generation, '--compose', '--build-root', root, '--scope', scope,
                '--result', result or root / 'Actions/fixture/result.json', success=success)
        def gc():
            return run(args.generation, '--gc', '--build-root', root, '--grace-seconds', 0)
        def gc_dry_run(target_root=root, grace=0):
            output = run(args.generation, '--gc', '--dry-run', '--json',
                '--build-root', target_root, '--grace-seconds', grace)
            return json.loads(output)
        def hold(path, mode='hold', bad=None):
            serial = len(processes)
            ready, release = base / f'ready{serial}', base / f'release{serial}'
            command = [args.probe, mode, str(path), str(ready), str(release)]
            if bad: command.append(str(bad))
            p = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            processes.append(p)
            wait_ready(p, ready)
            return p, release
        try:
            publish(root, 'a' * 64, b'A')
            compose()
            pointer = root / 'GenerationState/test.json'
            old = json.loads(pointer.read_text())
            old_dir = root / 'Generations' / old['generation_id']
            bad = base / 'bad.json'
            bad.write_text('{}')
            reader, release = hold(pointer, bad=bad)
            publish(root, 'b' * 64, b'B')
            compose()
            before_dry_run = snapshot_tree(root)
            report = gc_dry_run()
            assert report['schema_version'] == 1 and report['command'] == 'gc'
            assert report['dry_run'] is True and report['grace_seconds'] == 0
            retained = {entry['identity']: set(entry['reasons'])
                        for entry in report['retained']}
            assert old['generation_id'] in retained
            assert any('reader lease' in reason for reason in retained[old['generation_id']])
            current_id = json.loads(pointer.read_text())['generation_id']
            assert current_id in retained
            assert any('current pointer:' in reason for reason in retained[current_id])
            generation_candidates = report['candidates']['generations']
            assert isinstance(generation_candidates, list)
            assert report['totals']['generation_count'] == len(generation_candidates)
            product_candidates = report['candidates']['product_sets']
            assert any(entry['locator'] == f'Products/fixture/{"a" * 64}' and
                       entry['estimated_bytes'] > 0 for entry in product_candidates)
            assert report['totals']['product_set_count'] == len(product_candidates)
            assert snapshot_tree(root) == before_dry_run, 'GC dry-run mutated the fixture root'
            gc()
            assert old_dir.exists(), 'schema3 active snapshot deleted'
            assert not (root / f'Products/fixture/{"a" * 64}').exists(), 'schema3 reader retained source ProductSet'
            release.touch(); finish(reader)
            alias = root / 'GenerationState/retained-test.json'
            alias.write_text(json.dumps(old))
            gc(); assert old_dir.exists(), 'valid pointer alias lost its generation'
            alias.write_text(json.dumps({**old, 'scope': 'other'}))
            before_bad_alias = snapshot_tree(root)
            run(args.generation, '--gc', '--build-root', root, '--grace-seconds', 0, success=False)
            assert snapshot_tree(root) == before_bad_alias, 'mismatched pointer scope changed published data'
            alias.unlink()
            gc(); assert not old_dir.exists(), 'released schema3 snapshot leaked'
            print('PASS schema3 retained copy / failed selection / release GC')

            current = json.loads(pointer.read_text())
            manifest = json.loads((root / current['manifest_locator']).read_text())
            snapshot = root / manifest['products'][0]['locator']
            bytes_before = snapshot.read_bytes()
            reader, release = hold(pointer)
            pointer_before = pointer.read_bytes()
            snapshot.write_bytes(b'X')
            report = gc_dry_run()
            assert any(entry['identity'] == current['generation_id'] for entry in report['retained'])
            run(args.generation, '--verify', '--build-root', root, '--scope', 'test', success=False)
            assert snapshot.read_bytes() == b'X', 'GC or verification repaired active payload bytes'
            compose(success=False)
            assert pointer.read_bytes() == pointer_before and snapshot.exists()
            assert not (root / 'CorruptGenerations').exists(), 'active snapshot quarantined'
            snapshot.write_bytes(bytes_before)
            release.touch(); finish(reader)
            snapshot.write_bytes(b'X'); compose()
            assert snapshot.read_bytes() == bytes_before
            print('PASS repair refuses active reader / succeeds after release')

            # Reader selection requires no writable file or writable directory.
            installed = base / 'installed'
            import shutil
            shutil.copytree(root / 'Generations', installed / 'Generations')
            (installed / 'GenerationState').mkdir()
            installed_pointer = installed / 'GenerationState/test.json'
            installed_pointer.write_bytes(pointer.read_bytes())
            paths = list(installed.rglob('*')) + [installed]
            try:
                for path in paths: path.chmod(0o555 if path.is_dir() else 0o444)
                reader, release = hold(installed_pointer)
                assert not (installed / '.generation-mutation.lock').exists()
                release.touch(); finish(reader)
            finally:
                for path in paths: path.chmod(0o755 if path.is_dir() else 0o644)
            print('PASS read-only installed snapshot without Actions/Products/lease writes')

            # Real producer publication, compose and GC share the same mutex.
            source = base / 'source'
            audio, _ = sources(source)
            producer_command = [args.audio, '--produce', '--source-root', source, '--build-root', root]
            run(*producer_command)
            audio_result = root / 'Actions/audio/result.json'
            compose('test-audio', audio_result)
            (audio / 'wave_000.wav').write_bytes(wave(1))
            guard, release = hold(root, mode='mutation')
            commands = [producer_command,
                [args.generation, '--compose', '--build-root', root, '--scope', 'test-audio', '--result', audio_result],
                [args.generation, '--gc', '--build-root', root, '--grace-seconds', '0']]
            contenders = []
            for command in commands:
                p = subprocess.Popen(list(map(str, command)), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                contenders.append(p); processes.append(p)
            time.sleep(.4)
            assert all(p.poll() is None for p in contenders), 'mutator bypassed root lock'
            release.touch(); finish(guard)
            for p in contenders: finish(p)
            compose('test-audio', audio_result)
            gc()
            run(args.generation, '--verify', '--build-root', root, '--scope', 'test-audio')
            print('PASS real producer / publication / GC contention')

            if os.name == 'nt':
                import _winapi
                guarded = base / 'junction-gc'
                guarded_result = publish(guarded, 'a' * 64, b'old')
                guarded_compose = [args.generation, '--compose', '--build-root', guarded,
                    '--scope', 'guarded', '--result', guarded_result]
                run(*guarded_compose)
                guarded_pointer = guarded / 'GenerationState/guarded.json'
                old_id = json.loads(guarded_pointer.read_bytes())['generation_id']
                old_tree = guarded / 'Generations' / old_id
                publish(guarded, 'b' * 64, b'current'); run(*guarded_compose)
                selected = guarded_pointer.read_bytes()
                current_id = json.loads(selected)['generation_id']
                current_tree = guarded / 'Generations' / current_id
                current_bytes = snapshot_tree(current_tree)
                outside = base / 'outside-gc'
                outside.mkdir(); (outside / 'keep').write_bytes(b'untouched')
                junction = old_tree / 'nested' / 'link'
                junction.parent.mkdir()
                _winapi.CreateJunction(str(outside), str(junction))
                guarded_gc = [args.generation, '--gc', '--build-root', guarded,
                    '--grace-seconds', 0]
                try:
                    for mode in (['--dry-run', '--json'], []):
                        assert 'path-escape' in run(*guarded_gc, *mode, success=False)
                        assert guarded_pointer.read_bytes() == selected
                        assert snapshot_tree(current_tree) == current_bytes
                        assert old_tree.exists() and (old_tree / 'generation.json').exists()
                        assert snapshot_tree(outside) == {'keep': b'untouched'}
                finally:
                    if junction.exists(): os.rmdir(junction)
                run(*guarded_gc)
                assert not old_tree.exists() and snapshot_tree(current_tree) == current_bytes
                assert snapshot_tree(outside) == {'keep': b'untouched'}
                print('PASS per-candidate GC junction refusal preserves current and external trees')

            # A retained ActionResult snapshot must pin its ProductSet across receipt replacement and GC.
            action_root = base / 'action-reader'
            action_args = (action_root, 'fixture', 'fixture.action', 'fixture/product', 'product.bin')
            run(args.probe, 'publish-action', *action_args[:3], '1', *action_args[3:], 'A')
            old_result = json.loads((action_root / 'Actions/fixture/result.json').read_text())
            old_product_set = action_root / 'Products/fixture' / old_result['product_set_id']
            old_product = old_product_set / 'product.bin'
            assert old_product.read_bytes() == b'A'
            ready, release = base / 'action-ready', base / 'action-release'
            action_reader = subprocess.Popen(
                [str(args.probe), 'action-hold', str(action_root), 'fixture',
                 'fixture/product', str(ready), str(release)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            processes.append(action_reader)
            wait_ready(action_reader, ready)
            run(args.probe, 'publish-action', *action_args[:3], '2', *action_args[3:], 'B')
            run(args.generation, '--gc', '--build-root', action_root, '--grace-seconds', '0')
            assert old_product.exists() and old_product.read_bytes() == b'A', \
                'GC removed a ProductSet held by an ActionResult snapshot'
            release.touch(); finish(action_reader)
            run(args.generation, '--gc', '--build-root', action_root, '--grace-seconds', '0')
            assert not old_product_set.exists(), 'released ActionResult ProductSet was not collected'
            print('PASS ActionResult ProductSet lease across replacement and GC')
        finally:
            for p in processes:
                if p.poll() is None: p.kill(); p.communicate(timeout=5)
    print(f'PASS content.generation-lifetime elapsed={time.monotonic() - started:.3f}s')


if __name__ == '__main__':
    main()
