"""Real resolver/producer/composer/GC races, without GPU or repository assets."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

from check_asset_pipeline_portable import assert_generation_diagnostic, sources, wave


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


def pending_publication(root, expected_id, manifest_bytes, **changes):
    marker = dict(schema_version=1, kind='project-hs-generation-publication',
        generation_id=expected_id,
        manifest_digest='sha256:' + hashlib.sha256(manifest_bytes).hexdigest(),
        manifest_size=len(manifest_bytes))
    marker.update(changes)
    path = root / 'Generations' / f'.pending-{expected_id}.json'
    path.write_text(json.dumps(marker, indent=2) + '\n')
    return path


def legacy(root):
    locator = f'Products/legacy/{"1" * 64}/product.bin'
    product_path = root / locator
    product_path.parent.mkdir(parents=True)
    product_path.write_bytes(b'L')
    product = dict(action_id='legacy', namespace_path='legacy', product_set_id='1' * 64,
        logical_id='legacy/product', locator=locator,
        digest='sha256:' + hashlib.sha256(b'L').hexdigest(), size=1)
    semantics = dict(schema_version=2, scope='legacy', required_ids=['legacy/product'],
        products=[product], upstream_product_dependencies=[])
    gid = hashlib.sha256(json.dumps(semantics, separators=(',', ':')).encode()).hexdigest()
    manifest = dict(schema_version=2, generation_id=gid, scope='legacy',
        required_ids=['legacy/product'], products=[product], upstream_product_dependencies=[])
    manifest_path = root / f'Generations/{gid}/generation.json'
    manifest_path.parent.mkdir(parents=True)
    manifest_path.write_bytes((json.dumps(manifest, indent=2) + '\n').encode())
    pointer = root / 'GenerationState/legacy.json'
    pointer.parent.mkdir(exist_ok=True)
    pointer.write_text(json.dumps(dict(schema_version=1, scope='legacy', action_ids=['legacy'],
        generation_id=gid, manifest_locator=f'Generations/{gid}/generation.json', action_id='legacy')))
    return pointer, manifest_path, product_path


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
            gc(); assert not old_dir.exists(), 'released schema3 snapshot leaked'
            print('PASS schema3 retained copy / failed selection / release GC')

            reader, _ = hold(pointer)
            killed_dir = root / 'Generations' / json.loads(pointer.read_text())['generation_id']
            publish(root, 'c' * 64, b'C'); compose(); gc()
            assert killed_dir.exists()
            reader.kill(); reader.communicate(timeout=5)
            gc(); assert not killed_dir.exists(), 'abnormal exit left a stale lease'
            print('PASS abnormal exit kernel lease release')

            legacy_pointer, legacy_manifest, legacy_product = legacy(root)
            reader, release = hold(legacy_pointer)
            legacy_pointer.unlink()
            gc()
            assert legacy_manifest.exists() and legacy_product.exists(), 'schema2 ProductSet deleted under reader'
            release.touch(); finish(reader); gc()
            assert not legacy_manifest.exists() and not legacy_product.exists()
            print('PASS schema2 active snapshot and original ProductSet')

            current = json.loads(pointer.read_text())
            manifest = json.loads((root / current['manifest_locator']).read_text())
            snapshot = root / manifest['products'][0]['locator']
            bytes_before = snapshot.read_bytes()
            reader, release = hold(pointer)
            pointer_before = pointer.read_bytes()
            snapshot.write_bytes(b'X')
            assert_generation_diagnostic(compose(success=False), 'generation-in-use', '-',
                                         'cannot repair active Generation: ' + current['generation_id'])
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

            # A legacy reader may select a healthy sibling of a corrupt Audio product.
            result = json.loads(audio_result.read_text())
            catalog = next(p for p in result['products'] if p['logical_id'] == 'audio/audio_cues.json')
            namespace = 'AUDIO' if os.name == 'nt' else 'audio'
            selected = dict(action_id='audio', namespace_path=namespace,
                product_set_id=result['product_set_id'], logical_id=catalog['logical_id'],
                locator=catalog['locator'].replace('Products/audio/', f'Products/{namespace}/'),
                digest=catalog['digest'], size=catalog['size'])
            required = [selected['logical_id']]
            semantics = dict(schema_version=2, scope='legacy-audio', required_ids=required,
                products=[selected], upstream_product_dependencies=[])
            legacy_id = hashlib.sha256(json.dumps(semantics, separators=(',', ':')).encode()).hexdigest()
            legacy_manifest = dict(schema_version=2, generation_id=legacy_id,
                scope='legacy-audio', required_ids=required, products=[selected],
                upstream_product_dependencies=[])
            legacy_path = root / f'Generations/{legacy_id}/generation.json'
            legacy_path.parent.mkdir()
            legacy_path.write_bytes((json.dumps(legacy_manifest, indent=2) + '\n').encode('utf-8'))
            legacy_pointer = root / 'GenerationState/legacy-audio.json'
            legacy_pointer.write_text(json.dumps(dict(schema_version=1, scope='legacy-audio',
                action_ids=['audio'], generation_id=legacy_id,
                manifest_locator=f'Generations/{legacy_id}/generation.json', action_id='audio')))
            run(args.generation, '--verify', '--build-root', root, '--scope', 'legacy-audio')
            legacy_pointer_before = legacy_pointer.read_bytes()
            reader, release = hold(legacy_pointer)
            catalog_path = root / catalog['locator']
            catalog_identity = catalog_path.stat().st_ino
            catalog_bytes = catalog_path.read_bytes()
            result_before = audio_result.read_bytes()
            sibling = next(p for p in result['products'] if p['logical_id'].endswith('.wav'))
            sibling_path = root / sibling['locator']
            sibling_bytes = sibling_path.read_bytes()
            sibling_path.write_bytes(b'X' * len(sibling_bytes))
            refusal = run(*producer_command, success=False)
            preserved = {
                'catalog_bytes': catalog_path.read_bytes() == catalog_bytes,
                'catalog_file_identity': catalog_path.stat().st_ino == catalog_identity,
                'action_result': audio_result.read_bytes() == result_before,
                'selected_pointer': legacy_pointer.read_bytes() == legacy_pointer_before,
                'no_quarantine': not (root / 'CorruptProducts').exists(),
            }
            assert all(preserved.values()), {'preserved': preserved, 'producer_output': refusal}
            assert 'generation-in-use' in refusal, {'preserved': preserved, 'producer_output': refusal}
            release.touch(); finish(reader)
            malformed = root / f'Generations/{"e" * 64}/generation.json'
            malformed.parent.mkdir()
            malformed.write_text(json.dumps(dict(schema_version=2, products={})))
            assert 'invalid Generation products' in run(*producer_command, success=False)
            assert catalog_path.stat().st_ino == catalog_identity
            assert audio_result.read_bytes() == result_before
            malformed.unlink(); malformed.parent.rmdir()
            run(*producer_command)
            assert sibling_path.read_bytes() == sibling_bytes
            assert catalog_path.read_bytes() == catalog_bytes
            print('PASS schema2 healthy sibling preserved during refused producer repair')

            # Process death before manifest commit owns only a marked partial tree.
            publishing = base / 'interrupted-publication'
            publication_result = publish(publishing, '7' * 64, b'P')
            publication_command = [args.generation, '--compose', '--build-root', publishing,
                '--scope', 'publication', '--result', publication_result]
            run(*publication_command)
            publication_pointer = publishing / 'GenerationState/publication.json'
            old_pointer = publication_pointer.read_bytes()
            selected_tree = publishing / 'Generations' / json.loads(old_pointer)['generation_id']
            reader, release = hold(publication_pointer)
            publish(publishing, '8' * 64, b'Q')
            run(*publication_command)
            committed_pointer = publication_pointer.read_bytes()
            publication_id = json.loads(committed_pointer)['generation_id']
            publication_tree = publishing / 'Generations' / publication_id
            publication_manifest = publication_tree / 'generation.json'
            manifest_bytes = publication_manifest.read_bytes()
            publication_product = publishing / json.loads(manifest_bytes)['products'][0]['locator']
            publication_manifest.unlink()
            publication_product.write_bytes(b'partial')
            marker = pending_publication(publishing, publication_id, manifest_bytes)
            publication_pointer.write_bytes(old_pointer)
            run(*publication_command)
            assert publication_pointer.read_bytes() == committed_pointer
            assert publication_manifest.read_bytes() == manifest_bytes
            assert publication_product.read_bytes() == b'Q' and not marker.exists()
            assert selected_tree.exists(), 'selected reader lost during interrupted fresh publish recovery'

            # A committed manifest is authoritative even if the pointer is still old.
            publication_pointer.write_bytes(old_pointer)
            marker = pending_publication(publishing, publication_id, manifest_bytes,
                manifest_digest='sha256:' + '0' * 64)
            manifest_identity = publication_manifest.stat().st_ino
            run(*publication_command)
            assert publication_pointer.read_bytes() == committed_pointer
            assert publication_manifest.stat().st_ino == manifest_identity
            assert publication_manifest.read_bytes() == manifest_bytes and not marker.exists()
            selected, selected_release = hold(publication_pointer)
            marker = pending_publication(publishing, publication_id, manifest_bytes,
                manifest_digest='sha256:' + '0' * 64)
            publish(publishing, '9' * 64, b'R'); run(*publication_command)
            before_dry_run = snapshot_tree(publishing)
            report = gc_dry_run(publishing)
            metadata_candidates = report['candidates']['metadata_cleanup']
            assert any(entry['locator'] ==
                       f'Generations/.pending-{publication_id}.json' and
                       entry['estimated_bytes'] > 0 for entry in metadata_candidates)
            assert report['totals']['metadata_cleanup_count'] == len(metadata_candidates)
            assert report['totals']['metadata_cleanup_bytes'] == sum(
                entry['estimated_bytes'] for entry in metadata_candidates)
            assert snapshot_tree(publishing) == before_dry_run
            run(args.generation, '--gc', '--build-root', publishing, '--grace-seconds', 0)
            assert selected_tree.exists() and publication_tree.exists(), 'fresh publish bypassed reader leases'
            assert not marker.exists(), 'committed stale marker retained under reader lease'
            release.touch(); finish(reader)
            selected_release.touch(); finish(selected)
            run(args.generation, '--gc', '--build-root', publishing, '--grace-seconds', 0)
            assert not selected_tree.exists() and not publication_tree.exists()
            print('PASS pending fresh publish / manifest commit before pointer / selected reader leases')

            # GC never interprets an unpublished tree as a semantic Generation.
            collecting = base / 'pending-gc'
            collection_result = publish(collecting, '2' * 64, b'S')
            run(args.generation, '--compose', '--build-root', collecting,
                '--scope', 'pending', '--result', collection_result)
            collection_pointer = collecting / 'GenerationState/pending.json'
            collection_pointer_bytes = collection_pointer.read_bytes()
            collection_id = json.loads(collection_pointer_bytes)['generation_id']
            collection_tree = collecting / 'Generations' / collection_id
            collection_manifest = collection_tree / 'generation.json'
            collection_manifest_bytes = collection_manifest.read_bytes()
            marker = pending_publication(collecting, collection_id, collection_manifest_bytes)
            collection_manifest.unlink(); collection_pointer.unlink()
            collection_gc = [args.generation, '--gc', '--build-root', collecting]
            run(*collection_gc, '--grace-seconds', 3600)
            assert collection_tree.exists() and marker.exists(), 'pending publication ignored grace period'
            pending_report = gc_dry_run(collecting, 3600)
            pending_retained = {entry['identity']: set(entry['reasons'])
                                for entry in pending_report['retained']}
            assert collection_id in pending_retained
            assert {'pending publication', 'grace period'} <= pending_retained[collection_id]
            run(*collection_gc, '--grace-seconds', 0, '--pin', collection_id)
            assert collection_tree.exists() and marker.exists(), 'pending publication ignored pin'
            collection_pointer.write_bytes(collection_pointer_bytes)
            run(*collection_gc, '--grace-seconds', 0)
            assert collection_tree.exists() and marker.exists(), 'pending publication ignored current pointer'
            collection_pointer.unlink()
            run(*collection_gc, '--grace-seconds', 0)
            assert not collection_tree.exists() and not marker.exists()
            # Crash after marker publication, before directory creation.
            marker = pending_publication(collecting, collection_id, collection_manifest_bytes)
            run(*collection_gc, '--grace-seconds', 0)
            assert not marker.exists()
            print('PASS unpublished pending GC grace / pins / current retention / orphan marker')

            # Ownership/identity errors preserve the old pointer and partial evidence.
            refusing = base / 'unowned-publication'
            refused_result = publish(refusing, '3' * 64, b'T')
            refused_command = [args.generation, '--compose', '--build-root', refusing,
                '--scope', 'refusal', '--result', refused_result]
            run(*refused_command)
            refused_pointer = refusing / 'GenerationState/refusal.json'
            pointer_before = refused_pointer.read_bytes()
            publish(refusing, '4' * 64, b'U'); run(*refused_command)
            refused_id = json.loads(refused_pointer.read_bytes())['generation_id']
            refused_tree = refusing / 'Generations' / refused_id
            refused_manifest = refused_tree / 'generation.json'
            refused_bytes = refused_manifest.read_bytes()
            refused_manifest.unlink(); refused_pointer.write_bytes(pointer_before)
            assert 'generation-conflict' in run(*refused_command, success=False)
            assert refused_pointer.read_bytes() == pointer_before and refused_tree.exists()
            assert 'missing-manifest' in run(args.generation, '--gc', '--build-root', refusing,
                '--grace-seconds', 0, success=False)
            for changes in (dict(unexpected=True), dict(generation_id='5' * 64), dict(manifest_size=0)):
                marker = pending_publication(refusing, refused_id, refused_bytes, **changes)
                run(*refused_command, success=False)
                run(args.generation, '--gc', '--build-root', refusing, '--grace-seconds', 0, success=False)
                assert refused_pointer.read_bytes() == pointer_before
                assert refused_tree.exists() and marker.exists()
                marker.unlink()
            marker = pending_publication(refusing, refused_id, refused_bytes,
                manifest_digest='sha256:' + '0' * 64)
            assert 'generation-conflict' in run(*refused_command, success=False)
            assert refused_pointer.read_bytes() == pointer_before and refused_tree.exists() and marker.exists()
            marker.unlink()
            pending_publication(refusing, refused_id, refused_bytes)
            run(*refused_command)
            run(args.generation, '--verify', '--build-root', refusing, '--scope', 'refusal')
            print('PASS unmarked / malformed / mismatched pending publication refusal preserves pointer')

            # Reproduce process death after manifest retirement, before tree removal.
            interrupted = base / 'interrupted'
            retired_result = publish(interrupted, 'd' * 64, b'D')
            retired_command = [args.generation, '--compose', '--build-root', interrupted,
                '--scope', 'retired', '--result', retired_result]
            run(*retired_command)
            retired_pointer = interrupted / 'GenerationState/retired.json'
            retired_id = json.loads(retired_pointer.read_text())['generation_id']
            retired_tree = interrupted / 'Generations' / retired_id
            tombstone = interrupted / 'Generations' / f'.gc-{retired_id}.tmp-fixture.json'
            (retired_tree / 'generation.json').rename(tombstone)
            retired_pointer.unlink(); retired_result.unlink()
            run(args.generation, '--gc', '--build-root', interrupted, '--grace-seconds', 0)
            assert not retired_tree.exists() and not tombstone.exists()
            assert not (interrupted / f'Products/fixture/{"d" * 64}').exists()

            retired_result = publish(interrupted, 'e' * 64, b'E')
            run(*retired_command)
            retired_id = json.loads(retired_pointer.read_text())['generation_id']
            retired_tree = interrupted / 'Generations' / retired_id
            tombstone = interrupted / 'Generations' / f'.gc-{retired_id}.tmp-fixture.json'
            (retired_tree / 'generation.json').rename(tombstone)
            run(*retired_command)  # Same identity can be republished after interrupted GC.
            run(args.generation, '--gc', '--build-root', interrupted, '--grace-seconds', 0)
            run(args.generation, '--verify', '--build-root', interrupted, '--scope', 'retired')
            assert retired_tree.exists() and not tombstone.exists()
            print('PASS interrupted GC namespace sweep / same-ID republish recovery')

            # Both rename boundaries in repair must recover without losing evidence.
            for moved_tree in (False, True):
                repairing = base / f'repair-{int(moved_tree)}'
                repair_result = publish(repairing, 'f' * 64, b'F')
                repair_command = [args.generation, '--compose', '--build-root', repairing,
                    '--scope', 'repair', '--result', repair_result]
                run(*repair_command)
                repair_pointer = repairing / 'GenerationState/repair.json'
                pointer_before = repair_pointer.read_bytes()
                repair_id = json.loads(pointer_before)['generation_id']
                repair_tree = repairing / 'Generations' / repair_id
                repair_manifest = repair_tree / 'generation.json'
                product = repairing / json.loads(repair_manifest.read_text())['products'][0]['locator']
                product.write_bytes(b'X')
                quarantine = repairing / 'CorruptGenerations'
                quarantine.mkdir()
                destination = quarantine / f'{repair_id}.tmp-fixture'
                repair_manifest.rename(Path(str(destination) + '.json'))
                if moved_tree: repair_tree.rename(destination)
                run(*repair_command)
                assert repair_pointer.read_bytes() == pointer_before
                assert product.read_bytes() == b'F'
                retired = list(quarantine.iterdir())
                assert len(retired) == 1 and retired[0].is_dir()
                assert (retired[0] / 'generation.json').is_file()
                assert next((retired[0] / 'products').iterdir()).read_bytes() == b'X'
                run(args.generation, '--gc', '--build-root', repairing, '--grace-seconds', 0)
                run(args.generation, '--verify', '--build-root', repairing, '--scope', 'repair')
            print('PASS interrupted repair manifest/tree retirement recovery')
        finally:
            for p in processes:
                if p.poll() is None: p.kill(); p.communicate(timeout=5)
    print(f'PASS content.generation-lifetime elapsed={time.monotonic() - started:.3f}s')


if __name__ == '__main__':
    main()
