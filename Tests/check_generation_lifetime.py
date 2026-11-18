"""Essential ProductSet reader, publisher, and collector lifetime races."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time


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


def main():
    parser = argparse.ArgumentParser()
    for name in ('generation', 'probe'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    processes = []
    with tempfile.TemporaryDirectory(prefix='hs-gen-') as temporary:
        base = Path(temporary)
        root = base / 'build'

        def publish(namespace, version, data):
            run(args.probe, 'publish-action', root, namespace, namespace, version,
                'fixture/product', 'product.bin', data)
            return root / 'Actions' / namespace / 'result.json'

        def compose(scope, result):
            run(args.generation, '--compose', '--build-root', root, '--scope', scope,
                '--result', result)

        def gc(target=root):
            return run(args.generation, '--gc', '--build-root', target)

        def hold(manifest_path, bad=None, mode='hold'):
            serial = len(processes)
            ready, release = base / f'ready{serial}', base / f'release{serial}'
            command = [args.probe, mode, str(manifest_path), str(ready), str(release)]
            if bad:
                command.append(str(bad))
            process = subprocess.Popen(command, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True)
            processes.append(process)
            wait_ready(process, ready)
            return process, release

        def selected_products(scope):
            manifest_path = root / 'Generations/current' / f'{scope}.json'
            manifest = json.loads(manifest_path.read_text())
            return manifest_path, manifest

        try:
            # A reader's copied selection keeps A's ProductSet after fixed manifest replacement.
            result_a = publish('fixture', '1', 'A')
            compose('test', result_a)
            manifest_path, manifest_a = selected_products('test')
            product_a = root / manifest_a['products'][0]['locator']
            assert product_a.read_bytes() == b'A'
            bad = base / 'bad.json'
            bad.write_text('{}')
            reader, release = hold(manifest_path, bad=bad)

            result_b = publish('fixture', '2', 'B')
            compose('test', result_b)
            _, manifest_b = selected_products('test')
            product_b = root / manifest_b['products'][0]['locator']
            assert product_b != product_a and product_b.read_bytes() == b'B'
            gc()
            assert reader.poll() is None and product_a.read_bytes() == b'A'
            release.touch()
            finish(reader)
            gc()
            assert not product_a.parent.exists() and product_b.read_bytes() == b'B'
            run(args.generation, '--verify', '--build-root', root, '--scope', 'test')
            print('PASS ProductSet reader lease across compose and GC')

            # Separate scopes may select different action Products with the same logical ID.
            result_other = publish('fixture_other', '1', 'C')
            compose('other', result_other)
            _, other_manifest = selected_products('other')
            other_product = root / other_manifest['products'][0]['locator']
            _, current_manifest = selected_products('test')
            current_product = root / current_manifest['products'][0]['locator']
            assert other_manifest['products'][0]['logical_id'] == current_manifest['products'][0]['logical_id']
            assert other_product != current_product and other_product.read_bytes() == b'C'
            run(args.generation, '--verify', '--build-root', root, '--scope', 'other')

            # Producer, compose, and GC must all wait for the root mutation lock.
            producer = [args.probe, 'publish-action', root, 'locked', 'locked', '1',
                        'fixture/product', 'product.bin', 'D']
            run(*producer)
            producer_result = root / 'Actions/locked/result.json'
            compose('locked', producer_result)
            guard, guard_release = hold(root, mode='mutation')
            contenders = []
            commands = [producer,
                [args.generation, '--compose', '--build-root', root, '--scope', 'locked',
                 '--result', producer_result],
                [args.generation, '--gc', '--build-root', root]]
            for command in commands:
                process = subprocess.Popen(list(map(str, command)), stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE, text=True)
                contenders.append(process)
                processes.append(process)
            time.sleep(.4)
            assert all(process.poll() is None for process in contenders), 'mutator bypassed root lock'
            guard_release.touch()
            finish(guard)
            for process in contenders:
                finish(process)
            compose('locked', producer_result)
            run(args.generation, '--verify', '--build-root', root, '--scope', 'locked')
            print('PASS producer, compose, and GC root-lock contention')

            # Repairing a matching corrupted ProductSet is blocked only while a selected reader holds it.
            producer_manifest_path, producer_manifest = selected_products('locked')
            producer_product = root / producer_manifest['products'][0]['locator']
            expected = producer_product.read_bytes()
            corrupted = bytes([expected[0] ^ 0xFF]) + expected[1:]
            producer_product.write_bytes(corrupted)
            reader, release = hold(producer_manifest_path)
            repair_error = run(*producer, success=False)
            assert 'product-set-in-use' in repair_error and producer_product.read_bytes() == corrupted
            release.touch()
            finish(reader)
            run(*producer)
            run(args.generation, '--verify', '--build-root', root, '--scope', 'locked')
            assert producer_product.read_bytes() == expected
            print('PASS ProductSet repair waits for reader lease')

            # A junction in an unreferenced ProductSet must abort GC without touching its target.
            if os.name == 'nt':
                import _winapi
                guarded = base / 'guarded'
                run(args.probe, 'publish-action', guarded, 'fixture', 'fixture', '3',
                    'fixture/product', 'product.bin', 'old')
                old_result = guarded / 'Actions/fixture/result.json'
                old_set = json.loads(old_result.read_text())['product_set_id']
                old_product_set = guarded / 'Products/fixture' / old_set
                compose_args = [args.generation, '--compose', '--build-root', guarded,
                                '--scope', 'guarded', '--result', old_result]
                run(*compose_args)
                run(args.probe, 'publish-action', guarded, 'fixture', 'fixture', '4',
                    'fixture/product', 'product.bin', 'new')
                run(*compose_args)
                selected_manifest = guarded / 'Generations/current/guarded.json'
                manifest_bytes = selected_manifest.read_bytes()
                current_manifest = json.loads(manifest_bytes)
                current_product = guarded / current_manifest['products'][0]['locator']
                current_bytes = current_product.read_bytes()
                outside = base / 'outside'
                outside.mkdir()
                (outside / 'keep').write_bytes(b'untouched')
                junction = old_product_set / 'nested/link'
                junction.parent.mkdir()
                _winapi.CreateJunction(str(outside), str(junction))
                try:
                    run(args.generation, '--gc', '--build-root', guarded, success=False)
                    assert selected_manifest.read_bytes() == manifest_bytes
                    assert current_product.read_bytes() == current_bytes
                    assert (outside / 'keep').read_bytes() == b'untouched'
                finally:
                    if junction.exists():
                        os.rmdir(junction)
                        os.rmdir(junction.parent)
                gc(guarded)
                assert not old_product_set.exists()
                assert current_product.read_bytes() == current_bytes
                assert (outside / 'keep').read_bytes() == b'untouched'
                print('PASS GC refuses unsafe ProductSet, collects after repair')
        finally:
            for process in processes:
                if process.poll() is None:
                    process.kill()
                    process.communicate(timeout=5)


if __name__ == '__main__':
    main()
