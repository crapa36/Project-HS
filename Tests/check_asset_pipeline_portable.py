"""Exercise real Audio producer, shared publication code, composer and Ninja."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import time


def run(*arguments, success=True):
    result = subprocess.run([str(arg) for arg in arguments], capture_output=True, text=True)
    if (result.returncode == 0) != success:
        raise AssertionError(f"Command returned {result.returncode}: {arguments}\n{result.stdout}{result.stderr}")
    return result.stdout + result.stderr


def assert_generation_diagnostic(output, error_code, action_id, reason):
    diagnostic = json.loads(output)
    assert diagnostic['schema_version'] == 1
    assert diagnostic['producer_id'] == 'hs_generation'
    assert diagnostic['error_code'] == error_code
    assert diagnostic['action_id'] == action_id
    assert diagnostic['reason'] == reason, (diagnostic['reason'], reason)
    assert diagnostic['build_root'] and isinstance(diagnostic['dependency'], dict)
    assert diagnostic['remediation']


def wave(sample=0):
    data = bytes([sample, 0, 0, 0, 0, 0])
    return struct.pack('<4sI4s4sIHHIIHH4sI', b'RIFF', 36 + len(data), b'WAVE',
                       b'fmt ', 16, 1, 1, 48000, 144000, 3, 24, b'data', len(data)) + data


def reject_bad_manifest(generation, root, scope, path, legacy=False):
    original = path.read_bytes()
    try:
        for corrupt, code in ((original + b'\n', 'invalid-manifest'), (b'{"schema_version":', 'invalid-schema')):
            path.write_bytes(corrupt)
            commands = [('--gc', '--grace-seconds', '0')]
            if not legacy:
                commands.append(('--verify', '--scope', scope))
            for command in commands:
                output = run(generation, *command, '--build-root', root, success=False)
                assert json.loads(output)['error_code'] == code
    finally:
        path.write_bytes(original)


def sources(root):
    audio = root / 'ContentSource/Audio'
    audio.mkdir(parents=True)
    catalog = root / 'ContentSource/GameData/audio_cues.json'
    catalog.parent.mkdir(parents=True)
    entries = []
    index = 0
    for cue in range(107):
        names = []
        for _ in range(4 if cue < 58 else 3):
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


def publication_copy_fixture(generation, root):
    result = root / 'Actions/copy/result.json'
    result.parent.mkdir(parents=True)

    def publish(set_id, payloads):
        products = []
        for name, data in payloads.items():
            locator = f'Products/copy/{set_id}/{name}.bin'
            path = root / locator
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            products.append(dict(logical_id=f'copy/{name}', locator=locator,
                                 digest='sha256:' + hashlib.sha256(data).hexdigest(), size=len(data)))
        result.write_text(json.dumps(dict(schema_version=1, action_id='copy', namespace_path='copy',
            action_key='sha256:' + set_id, product_set_id=set_id, products=products,
            upstream_product_dependencies=[])))
        return products

    command = [generation, '--compose', '--build-root', root, '--scope', 'copy', '--result', result]
    publish('1' * 64, {'old': b'old'})
    run(*command)
    pointer = root / 'GenerationState/copy.json'
    previous_pointer = pointer.read_bytes()
    previous_generations = {path.name for path in (root / 'Generations').iterdir()}
    payloads = {'a-empty': b'', 'b-large': bytes(range(256)) * 513, 'c-last': b'last'}
    products = publish('2' * 64, payloads)
    last = root / products[-1]['locator']
    # Fail after earlier products have been copied; no partial snapshot may be committed.
    for damage, code, reason in (
        (None, 'missing-product', f'copy/c-last: {last}'),
        (b'short', 'corrupt-product', 'copy/c-last: size mismatch'),
        (b'xxxx', 'corrupt-product', 'copy/c-last: digest/size mismatch'),
    ):
        if damage is None:
            last.unlink()
        else:
            last.write_bytes(damage)
        output = run(*command, success=False)
        if damage is None:
            reason = json.loads(output)['reason']
            assert reason.startswith('copy/c-last: ') and Path(reason.removeprefix('copy/c-last: ')) == last
        assert_generation_diagnostic(output, code, '-', reason)
        assert pointer.read_bytes() == previous_pointer
        assert {path.name for path in (root / 'Generations').iterdir()} == previous_generations
        run(generation, '--verify', '--build-root', root, '--scope', 'copy')
        last.write_bytes(payloads['c-last'])
    run(*command)
    run(generation, '--verify', '--build-root', root, '--scope', 'copy')
    manifest = json.loads((root / json.loads(pointer.read_text())['manifest_locator']).read_text())
    for product in manifest['products']:
        snapshot = root / product['locator']
        name = product['logical_id'].split('/')[1]
        assert snapshot.read_bytes() == payloads[name]
        original = root / f'Products/copy/{"2" * 64}/{name}.bin'
        assert not os.path.samefile(original, snapshot), 'snapshot must own an independent copy'
    assert not list((root / 'Generations').glob('.pending-*'))


def sibling_fixture(generation, root):
    result = root / 'Actions/fixture/result.json'
    result.parent.mkdir(parents=True)
    pointer_path = root / 'GenerationState/test-sibling.json'
    def publish(set_id, x, y, dependencies=()):
        products = []
        for name, data in [('x', x), ('y', y)]:
            locator = f'Products/fixture/{set_id}/{name}.bin'
            path = root / locator
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            products.append(dict(logical_id=f'fixture/{name}', locator=locator,
                                 digest='sha256:' + hashlib.sha256(data).hexdigest(), size=len(data)))
        products[0]['dependencies'] = list(dependencies)
        result.write_text(json.dumps(dict(schema_version=1, action_id='fixture', namespace_path='fixture',
                                         action_key='sha256:' + set_id, product_set_id=set_id,
                                         products=products, upstream_product_dependencies=[])))
    def compose(*required, success=True):
        arguments = [generation, '--compose', '--build-root', root, '--scope', 'test-sibling', '--result', result]
        for logical_id in required:
            arguments += ['--require', logical_id]
        return run(*arguments, success=success)
    publish('a' * 64, b'x', b'y1')
    compose('fixture/x')
    first_pointer = pointer_path.read_bytes()
    first = json.loads(first_pointer)
    manifest_path = root / first['manifest_locator']
    manifest_bytes = manifest_path.read_bytes()
    manifest = json.loads(manifest_bytes)
    reject_bad_manifest(generation, root, 'test-sibling', manifest_path)
    snapshot = root / manifest['products'][0]['locator']
    # A valid Windows destination must not overflow MAX_PATH only for its temporary copy.
    with tempfile.TemporaryDirectory(prefix='hse-') as export_temp:
        relative = manifest['products'][0]['locator']
        padding = 248 - len(str(Path(export_temp))) - len(relative) - 2
        assert padding > 0
        destination = Path(export_temp) / ('e' * padding)
        destination.mkdir()
        assert len(str(destination / relative)) == 248
        for _ in range(2):
            run(generation, '--export-snapshot', '--build-root', root,
                '--scope', 'test-sibling', '--destination', destination)
        run(generation, '--verify', '--build-root', destination, '--scope', 'test-sibling')
        assert (destination / relative).read_bytes() == snapshot.read_bytes()
        assert not list(destination.rglob('*.tmp-*'))
    snapshot_stamp = snapshot.stat().st_mtime_ns
    pointer_stamp = pointer_path.stat().st_mtime_ns
    assert manifest['schema_version'] == 3 and 'product_set_id' not in manifest['products'][0]
    publish('b' * 64, b'x', b'y2')
    compose('fixture/x')
    assert pointer_path.read_bytes() == first_pointer, 'unrelated sibling changed scoped identity'
    assert pointer_path.stat().st_mtime_ns == pointer_stamp
    assert manifest_path.read_bytes() == manifest_bytes
    assert snapshot.stat().st_mtime_ns == snapshot_stamp
    receipts = [json.loads(p.read_text()) for p in (root / 'GenerationReceipts/test-sibling.json').glob('*.json')]
    assert {r['products'][0]['product_set_id'] for r in receipts} == {'a' * 64, 'b' * 64}
    assert {r['actions'][0]['action_key'] for r in receipts} == {'sha256:' + 'a' * 64, 'sha256:' + 'b' * 64}
    # Separate files are critical: producer corruption must not mutate a pinned snapshot.
    (root / f'Products/fixture/{"b" * 64}/x.bin').write_bytes(b'z')
    run(generation, '--verify', '--build-root', root, '--scope', 'test-sibling')
    assert_generation_diagnostic(compose('fixture/x', success=False), 'corrupt-product', '-',
                                 'fixture/x: digest/size mismatch')
    publish('b' * 64, b'x', b'y2')
    snapshot.write_bytes(b'q')
    assert_generation_diagnostic(run(generation, '--verify', '--build-root', root,
                                     '--scope', 'test-sibling', success=False),
                                 'corrupt-product', '-', 'fixture/x: digest/size mismatch')
    compose('fixture/x')
    assert snapshot.read_bytes() == b'x' and pointer_path.read_bytes() == first_pointer
    assert len(list((root / 'CorruptGenerations').iterdir())) == 1
    publish('c' * 64, b'X', b'y3')
    compose('fixture/x')
    second = json.loads(pointer_path.read_text())
    assert second['generation_id'] != first['generation_id'], 'selected byte change retained identity'
    # A product dependency must be explicitly included and is part of identity.
    publish('d' * 64, b'X', b'y3', ['fixture/y'])
    assert_generation_diagnostic(compose('fixture/x', success=False),
                                 'missing-product-dependency', 'fixture', 'fixture/x -> fixture/y')
    assert json.loads(pointer_path.read_text()) == second
    compose('fixture/x', 'fixture/y')
    closure = json.loads(pointer_path.read_text())
    assert closure['generation_id'] != second['generation_id']
    # GC must not mutate anything when a retained snapshot fails verification.
    snapshot.write_bytes(b'q')
    assert_generation_diagnostic(run(generation, '--gc', '--build-root', root,
                                     '--pin', first['generation_id'], '--grace-seconds', 0,
                                     success=False),
                                 'corrupt-product', '-', 'fixture/x: digest/size mismatch')
    assert (root / f'Products/fixture/{"a" * 64}').exists()
    assert (root / 'Generations' / second['generation_id']).exists()
    snapshot.write_bytes(b'x')
    # Current ActionResults retain their sets; old pinned snapshots own their copies.
    run(generation, '--gc', '--build-root', root, '--pin', first['generation_id'], '--grace-seconds', 0)
    assert snapshot.read_bytes() == b'x'
    assert not (root / f'Products/fixture/{"a" * 64}').exists()
    assert not (root / f'Products/fixture/{"b" * 64}').exists()
    assert not (root / 'Generations' / second['generation_id']).exists()
    assert (root / f'Products/fixture/{"d" * 64}').is_dir()
    pointer_path.write_bytes(first_pointer)
    run(generation, '--verify', '--build-root', root, '--scope', 'test-sibling')


def upstream_fixture(generation, root):
    root.mkdir()
    paths = []
    for action, data in [('upstream', b'u'), ('downstream', b'd')]:
        set_id = hashlib.sha256(data).hexdigest()
        locator = f'Products/{action}/{set_id}/product.bin'
        path = root / locator
        path.parent.mkdir(parents=True)
        path.write_bytes(data)
        result = root / f'Actions/{action}/result.json'
        result.parent.mkdir(parents=True)
        edges = [] if action == 'upstream' else [dict(logical_id='upstream/product',
                   digest='sha256:' + hashlib.sha256(b'u').hexdigest(), size=1)]
        result.write_text(json.dumps(dict(schema_version=1, action_id=action, namespace_path=action,
            action_key='sha256:' + set_id, product_set_id=set_id, upstream_product_dependencies=edges,
            products=[dict(logical_id=f'{action}/product', locator=locator,
                           digest='sha256:' + set_id, size=1)])))
        paths += ['--result', result]
    command = [generation, '--compose', '--build-root', root, '--scope', 'upstream-test', *paths]
    assert_generation_diagnostic(run(*command, '--require', 'downstream/product', success=False),
                                 'missing-upstream-product', 'downstream',
                                 'missing selected upstream product: upstream/product')
    run(*command)
    pointer = root / 'GenerationState/upstream-test.json'
    baseline = pointer.read_bytes()
    document = json.loads((root / 'Actions/downstream/result.json').read_text())
    document['upstream_product_dependencies'][0]['digest'] = 'sha256:' + '0' * 64
    (root / 'Actions/downstream/result.json').write_text(json.dumps(document))
    assert_generation_diagnostic(run(*command, success=False), 'upstream-product-mismatch',
                                 'downstream', 'selected upstream product identity differs: upstream/product')
    assert pointer.read_bytes() == baseline


def legacy_fixture(generation, root):
    root.mkdir()
    # Canonical pre-schema-3 hashes/bytes remain valid; schema 1 is GC-only.
    pointers = []
    for version in (1, 2):
        action = f'legacy{version}'
        set_id = str(version) * 64
        locator = f'Products/{action}/{set_id}/product.bin'
        source = root / locator
        source.parent.mkdir(parents=True)
        source.write_bytes(b'L')
        product = dict(action_id=action, namespace_path=action, product_set_id=set_id,
                       logical_id=f'{action}/product', locator=locator,
                       digest='sha256:' + hashlib.sha256(b'L').hexdigest(), size=1)
        scope = f'legacy-{version}'
        semantics = dict(schema_version=version, scope=scope,
                         required_ids=[product['logical_id']], products=[product])
        if version == 2:
            semantics['upstream_product_dependencies'] = []
        generation_id = hashlib.sha256(json.dumps(semantics, separators=(',', ':')).encode()).hexdigest()
        manifest = dict(schema_version=version, generation_id=generation_id, scope=scope,
                        required_ids=[product['logical_id']], products=[product])
        if version == 2:
            manifest['upstream_product_dependencies'] = []
        manifest_locator = f'Generations/{generation_id}/generation.json'
        path = root / manifest_locator
        path.parent.mkdir(parents=True)
        path.write_bytes((json.dumps(manifest, indent=2) + '\n').encode('utf-8'))
        pointer = root / f'GenerationState/{scope}.json'
        pointer.parent.mkdir(exist_ok=True)
        pointer.write_text(json.dumps(dict(schema_version=1, scope=scope, action_ids=[action],
                            generation_id=generation_id, manifest_locator=manifest_locator, action_id=action)))
        if version == 2:
            run(generation, '--verify', '--build-root', root, '--scope', scope)
        else:
            assert_generation_diagnostic(run(generation, '--verify', '--build-root', root,
                                             '--scope', scope, success=False),
                                         'invalid-schema', '-', 'expected Generation schema_version=2 or 3')
        reject_bad_manifest(generation, root, scope, path, legacy=version == 1)
        pointers.append((pointer, path, source))
    run(generation, '--gc', '--build-root', root, '--grace-seconds', 0)
    assert all(source.exists() and manifest.exists() for _, manifest, source in pointers)
    for pointer, _, _ in pointers:
        pointer.unlink()
    run(generation, '--gc', '--build-root', root, '--grace-seconds', 0)
    assert all(not source.exists() and not manifest.exists() for _, manifest, source in pointers)


def retired_action_fixture(generation, root):
    root.mkdir()
    set_id = 'a' * 64
    locator = f'Products/fixture/{set_id}/x.bin'
    product = root / locator
    product.parent.mkdir(parents=True)
    product.write_bytes(b'x')
    action = root / 'Actions/fixture/result.json'
    action.parent.mkdir(parents=True)
    action.write_text(json.dumps(dict(schema_version=1, action_id='fixture',
        namespace_path='fixture', action_key='sha256:' + set_id, product_set_id=set_id,
        upstream_product_dependencies=[], products=[dict(logical_id='fixture/x', locator=locator,
            digest='sha256:' + hashlib.sha256(b'x').hexdigest(), size=1)])))
    run(generation, '--compose', '--build-root', root, '--scope', 'retired', '--result', action)
    pointer = root / 'GenerationState/retired.json'
    generation_id = json.loads(pointer.read_text())['generation_id']
    pointer.unlink()
    action.unlink()
    run(generation, '--gc', '--build-root', root, '--grace-seconds', 0)
    assert not (root / 'Generations' / generation_id).exists()
    assert not product.parent.exists(), 'retired action ProductSet leaked after Generation GC'


def deep_dependency_fixture(generation, root):
    action = root / 'Actions/chain/result.json'
    action.parent.mkdir(parents=True)
    count = 20000
    ids = [f'chain-{100000 + index}' for index in range(count)]
    set_id = '0' * 64
    products = [dict(logical_id=logical_id, locator=f'Products/chain/{set_id}/missing.bin',
                     digest='sha256:' + hashlib.sha256(b'x').hexdigest(), size=1,
                     dependencies=[ids[(index + 1) % count]])
                for index, logical_id in enumerate(ids)]
    receipt = dict(schema_version=1, action_id='chain', namespace_path='chain',
                   action_key='sha256:' + set_id, product_set_id=set_id,
                   products=products, upstream_product_dependencies=[])
    command = [generation, '--compose', '--build-root', root,
               '--scope', 'test-chain', '--result', action]
    action.write_text(json.dumps(receipt))
    output = run(*command, success=False)
    assert_generation_diagnostic(output, 'cyclic-product-dependency', 'chain', ids[0])
    # With no closing cycle, the complete graph must reach payload validation.
    # The missing payload keeps this test bounded without copying 20k snapshot files.
    products[-1]['dependencies'] = []
    action.write_text(json.dumps(receipt))
    output = json.loads(run(*command, success=False))
    assert output['error_code'] == 'missing-product' and output['reason'].startswith(ids[0] + ': ')
    products[-1]['dependencies'] = ['absent']
    action.write_text(json.dumps(receipt))
    assert_generation_diagnostic(run(*command, success=False), 'missing-product-dependency',
                                 'chain', ids[-1] + ' -> absent')
    assert not (root / 'GenerationState/test-chain.json').exists()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('audio', 'generation', 'cmake', 'ninja'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix='hs native asset ') as temporary:
        root = Path(temporary)
        source = root / 'source'
        audio, catalog = sources(source)
        project = root / 'project'
        project.mkdir()
        tools = root / 'tools'
        tools.mkdir()
        fixture_audio = Path(shutil.copy2(args.audio, tools))
        build = root / 'build'
        trigger = project / 'composition-trigger'
        trigger.write_text('unchanged semantics')
        quote = lambda path: json.dumps(str(path).replace('\\', '/'), ensure_ascii=False)
        (project / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 4.4)
project(NativeAssetFixture NONE)
include({quote(repo / 'cmake/AssetActions.cmake')})
add_executable(audio_fixture IMPORTED GLOBAL)
set_target_properties(audio_fixture PROPERTIES IMPORTED_LOCATION {quote(fixture_audio)})
add_executable(generation_fixture IMPORTED GLOBAL)
set_target_properties(generation_fixture PROPERTIES IMPORTED_LOCATION {quote(args.generation)})
hs_add_asset_action(NAME audio_action BUILDER audio_fixture
    RESULT "${{CMAKE_CURRENT_BINARY_DIR}}/Actions/audio/result.json"
    DEPFILE "${{CMAKE_CURRENT_BINARY_DIR}}/Actions/audio/result.d"
    STATIC_INPUTS {quote(catalog)}
    ARGS --produce --source-root {quote(source)} --build-root "${{CMAKE_CURRENT_BINARY_DIR}}")
add_custom_command(OUTPUT "${{CMAKE_CURRENT_BINARY_DIR}}/GenerationState/test-audio.json"
    COMMAND "$<TARGET_FILE:generation_fixture>" --compose --build-root "${{CMAKE_CURRENT_BINARY_DIR}}"
        --scope test-audio --result "${{CMAKE_CURRENT_BINARY_DIR}}/Actions/audio/result.json"
    DEPENDS generation_fixture "${{CMAKE_CURRENT_BINARY_DIR}}/Actions/audio/result.json"
        "${{CMAKE_CURRENT_SOURCE_DIR}}/composition-trigger" VERBATIM)
add_custom_command(OUTPUT "${{CMAKE_CURRENT_BINARY_DIR}}/consumer.stamp"
    COMMAND {quote(args.cmake)} -E touch "${{CMAKE_CURRENT_BINARY_DIR}}/consumer.stamp"
    DEPENDS "${{CMAKE_CURRENT_BINARY_DIR}}/GenerationState/test-audio.json" VERBATIM)
add_custom_target(fixture ALL DEPENDS "${{CMAKE_CURRENT_BINARY_DIR}}/consumer.stamp")
''')
        run(args.cmake, '-S', project, '-B', build, '-G', 'Ninja',
            '-DCMAKE_MAKE_PROGRAM=' + args.ninja)
        build_now = lambda: run(args.cmake, '--build', build)
        verify = lambda success=True: run(args.generation, '--verify', '--build-root', build,
                                          '--scope', 'test-audio', success=success)
        receipt_count = lambda: len(list((build / 'ActionReceipts/audio').glob('*.json')))
        action_path = build / 'Actions/audio/result.json'
        pointer_path = build / 'GenerationState/test-audio.json'
        consumer = build / 'consumer.stamp'
        build_now()
        verify()
        action = json.loads(action_path.read_text())
        pinned_pointer = pointer_path.read_bytes()
        pinned_generation = json.loads(pinned_pointer)
        pinned_manifest_path = build / pinned_generation['manifest_locator']
        pinned_manifest = pinned_manifest_path.read_bytes()
        pinned_products = json.loads(pinned_manifest)['products']
        pinned_bytes = {item['logical_id']: (build / item['locator']).read_bytes()
                        for item in pinned_products}
        assert action['recipe_identity'] == 'sha256:' + hashlib.sha256(fixture_audio.read_bytes()).hexdigest()
        expected_count = len({name for entry in json.loads(catalog.read_text())['entries']
                              for name in entry['files']}) + 4
        assert len(action['products']) == expected_count
        for item in action['products']:
            data = (build / item['locator']).read_bytes()
            assert item['size'] == len(data)
            assert item['digest'] == 'sha256:' + hashlib.sha256(data).hexdigest()
        producer = [fixture_audio, '--produce', '--source-root', source, '--build-root', build]
        published = {item['logical_id']: (build / item['locator']).read_bytes() for item in action['products']}
        result_bytes, result_stamp = action_path.read_bytes(), action_path.stat().st_mtime_ns
        actions = action_path.parent

        def without_preparation(success=True):
            # A stage create/delete would change this directory, even if it was cleaned before exit.
            os.utime(actions, ns=(1_600_000_000_000_000_000, 1_600_000_000_000_000_000))
            stamp = actions.stat().st_mtime_ns
            output = run(*producer, success=success)
            assert 'shipping-audio PCM' not in output, output
            assert actions.stat().st_mtime_ns == stamp, 'forced hit or rejected cache created a stage'
            assert not list(actions.glob('.stage-*'))
            return output

        assert f'audio current files={expected_count}' in without_preparation()
        assert action_path.read_bytes() == result_bytes and action_path.stat().st_mtime_ns == result_stamp
        assert all((build / item['locator']).read_bytes() == published[item['logical_id']]
                   for item in action['products'])
        # Corrupt current products are repaired as one quarantined ProductSet; the pinned
        # generation remains an independent immutable snapshot throughout publication.
        for logical_id in ('audio/release-cpp', 'audio/release-pcm', 'audio/release-identity'):
            item = next(item for item in action['products'] if item['logical_id'] == logical_id)
            path = build / item['locator']
            damaged = bytearray(published[logical_id])
            damaged[-1] ^= 1
            path.write_bytes(damaged)
            try:
                output = run(*producer)
                assert 'shipping-audio PCM' in output, output
                assert action_path.read_bytes() == result_bytes
                assert all((build / item['locator']).read_bytes() == published[item['logical_id']]
                           for item in action['products']), 'repair exposed a partial ProductSet'
                quarantine_root = build / 'CorruptProducts/audio'
                quarantined = [p for p in quarantine_root.iterdir()
                               if p.name.startswith(action['product_set_id'] + '-')]
                quarantine_set = next((p for p in quarantined
                    if (p / path.name).is_file() and (p / path.name).read_bytes() == damaged), None)
                assert quarantine_set is not None, 'corrupt ProductSet was not quarantined before repair'
                assert all((quarantine_set / item['locator'].split('/')[-1]).read_bytes() ==
                           (damaged if item['logical_id'] == logical_id else published[item['logical_id']])
                           for item in action['products']), 'quarantine did not preserve the complete prior set'
                assert pointer_path.read_bytes() == pinned_pointer
                assert pinned_manifest_path.read_bytes() == pinned_manifest
                assert all((build / item['locator']).read_bytes() == pinned_bytes[item['logical_id']]
                           for item in pinned_products), 'repair mutated the pinned generation snapshot'
                verify()  # A producer-side fault must not alter the independently composed snapshot.
            finally:
                path.write_bytes(published[logical_id])
        # A changed schema fails before source discovery/copy and preserves the published set.
        original_catalog = catalog.read_bytes()
        invalid = json.loads(original_catalog)
        invalid['schema_version'] = 2
        catalog.write_text(json.dumps(invalid))
        try:
            assert 'audio catalog schema or maximum voices changed' in without_preparation(False)
            assert action_path.read_bytes() == result_bytes
        finally:
            catalog.write_bytes(original_catalog)
        # A verified product hit cannot hide missing native completion metadata.
        (actions / 'result.d').unlink()
        assert 'shipping-audio PCM' in run(*producer)
        assert (actions / 'result.d').is_file() and action_path.read_bytes() == result_bytes
        build_now()  # Settle Ninja's discovered input timestamps after the direct invocations.
        baseline = (action_path.read_bytes(), action_path.stat().st_mtime_ns,
                    pointer_path.stat().st_mtime_ns, consumer.stat().st_mtime_ns, receipt_count())
        build_now()
        assert receipt_count() == baseline[4]
        time.sleep(1.1)
        (audio / 'wave_000.wav').touch()
        build_now()
        assert action_path.read_bytes() == baseline[0] and action_path.stat().st_mtime_ns == baseline[1]
        assert pointer_path.stat().st_mtime_ns == baseline[2] and consumer.stat().st_mtime_ns == baseline[3]
        assert receipt_count() == baseline[4] + 1
        build_now()
        assert receipt_count() == baseline[4] + 1
        time.sleep(1.1)
        trigger.touch()
        build_now()
        assert pointer_path.stat().st_mtime_ns == baseline[2], 'unchanged generation pointer was touched'
        assert consumer.stat().st_mtime_ns == baseline[3], 'unchanged generation reran downstream consumer'
        assert 'no work to do' in build_now().lower()
        (audio / 'unreferenced.wav').write_bytes(wave(2))
        build_now()
        assert receipt_count() == baseline[4] + 1
        # A producer-byte change invalidates its action even when products are identical.
        time.sleep(1.1)
        with fixture_audio.open('ab') as binary:
            binary.write(b'hs-recipe-identity-fixture')
        before_producer_change = receipt_count()
        build_now()
        changed_action = json.loads(action_path.read_text())
        assert changed_action['recipe_identity'] == 'sha256:' + hashlib.sha256(fixture_audio.read_bytes()).hexdigest()
        assert changed_action['recipe_identity'] != action['recipe_identity']
        assert changed_action['action_key'] != action['action_key']
        assert changed_action['product_set_id'] == action['product_set_id']
        assert receipt_count() == before_producer_change + 1
        assert 'no work to do' in build_now().lower()
        assert receipt_count() == before_producer_change + 1
        time.sleep(1.1)
        (audio / 'wave_000.wav').write_bytes(wave(1))
        changed_output = build_now()
        assert 'audio current' not in changed_output and 'shipping-audio PCM' in changed_output
        verify()
        assert json.loads(action_path.read_text())['product_set_id'] != action['product_set_id']
        previous_result = action_path.read_bytes()
        previous_pointer = pointer_path.read_bytes()
        (audio / 'wave_000.wav').write_bytes(b'truncated WAV')
        failure = run(args.cmake, '--build', build, success=False)
        assert 'release WAV must use canonical PCM chunks' in failure
        assert action_path.read_bytes() == previous_result
        assert pointer_path.read_bytes() == previous_pointer
        verify()
        (audio / 'wave_000.wav').write_bytes(wave(1))
        build_now()
        # Change a discovered reference; the old file must cease to be a dependency.
        authored = json.loads(catalog.read_text())
        authored['entries'][0]['files'][0] = 'replacement.wav'
        (audio / 'replacement.wav').write_bytes(wave(3))
        catalog.write_text(json.dumps(authored))
        build_now()
        count = receipt_count()
        time.sleep(1.1)
        (audio / 'wave_000.wav').touch()
        build_now()
        assert receipt_count() == count
        (audio / 'replacement.wav').touch()
        build_now()
        assert receipt_count() == count + 1
        assert 'no work to do' in build_now().lower()
        # Repair through Ninja, exercising existing corrupt-ProductSet quarantine.
        for mode in ('missing', 'corrupt'):
            action = json.loads(action_path.read_text())
            item = next(p for p in action['products'] if p['logical_id'] == 'audio/replacement.wav')
            product = build / item['locator']
            if mode == 'missing':
                product.unlink()
            else:
                data = bytearray(product.read_bytes())
                data[-1] ^= 1
                product.write_bytes(data)
            expected_code = 'missing-product' if mode == 'missing' else 'corrupt-product'
            verify()  # Generation owns an independent immutable copy
            previous_pointer = pointer_path.read_bytes()
            failure = run(args.generation, '--compose', '--build-root', build, '--scope', 'test-audio',
                          '--result', action_path, success=False)
            expected_reason = ('audio/replacement.wav: ' + str(build) + os.sep + item['locator']
                               if mode == 'missing' else 'audio/replacement.wav: digest/size mismatch')
            assert_generation_diagnostic(failure, expected_code, '-', expected_reason)
            assert pointer_path.read_bytes() == previous_pointer
            selected = action_path.read_bytes()
            damaged = product.read_bytes() if product.exists() else None
            failure = without_preparation(False)
            assert 'cannot inspect path' in failure or 'upstream product is missing or corrupt' in failure or 'missing regular file' in failure, failure
            assert action_path.read_bytes() == selected and pointer_path.read_bytes() == previous_pointer
            assert (product.read_bytes() if product.exists() else None) == damaged
            action_path.unlink()
            build_now()
            verify()
        assert len(list((build / 'CorruptProducts/audio').iterdir())) == 2
        # Missing and same-size corrupt snapshots are repaired even with healthy producer bytes.
        for mode in ('missing', 'corrupt'):
            manifest = json.loads((build / json.loads(pointer_path.read_text())['manifest_locator']).read_text())
            item = next(p for p in manifest['products'] if p['logical_id'] == 'audio/replacement.wav')
            product = build / item['locator']
            if mode == 'missing':
                product.unlink()
            else:
                data = bytearray(product.read_bytes())
                data[-1] ^= 1
                product.write_bytes(data)
            expected_code = 'missing-product' if mode == 'missing' else 'corrupt-product'
            expected_reason = ('audio/replacement.wav: ' + str(build) + os.sep + item['locator']
                               if mode == 'missing' else 'audio/replacement.wav: digest/size mismatch')
            assert_generation_diagnostic(verify(False), expected_code, '-', expected_reason)
            stamp = pointer_path.stat().st_mtime_ns
            run(args.generation, '--compose', '--build-root', build, '--scope', 'test-audio', '--result', action_path)
            verify()
            assert pointer_path.stat().st_mtime_ns == stamp
        assert len(list((build / 'CorruptGenerations').iterdir())) == 2
        if os.name != 'nt':
            manifest = json.loads((build / json.loads(pointer_path.read_text())['manifest_locator']).read_text())
            product = build / manifest['products'][0]['locator']
            data = product.read_bytes()
            outside = root / 'outside'
            outside.write_bytes(data)
            product.unlink()
            product.symlink_to(outside)
            assert_generation_diagnostic(verify(False), 'path-escape', '-', 'symbolic link: ' + str(product))
            product.unlink()
            product.write_bytes(data)
            verify()
        # The same producer binary and inputs preserve identities across source/build trees.
        second_source = root / 'other-source'
        shutil.copytree(source, second_source)
        second_build = root / 'other-build'
        run(fixture_audio, '--produce', '--source-root', second_source, '--build-root', second_build)
        assert json.loads(action_path.read_text()) == json.loads((second_build / 'Actions/audio/result.json').read_text())
        run(args.generation, '--compose', '--build-root', second_build, '--scope', 'test-audio',
            '--result', second_build / 'Actions/audio/result.json')
        assert pointer_path.read_bytes() == (second_build / 'GenerationState/test-audio.json').read_bytes()
        manifest_locator = json.loads(pointer_path.read_text())['manifest_locator']
        assert (build / manifest_locator).read_bytes() == (second_build / manifest_locator).read_bytes()
        # Concurrent identical producer publications must converge without partial files.
        concurrent_build = root / 'concurrent-build'
        command = [str(fixture_audio), '--produce', '--source-root', str(source), '--build-root', str(concurrent_build)]
        processes = [subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) for _ in range(3)]
        for process in processes:
            output, error = process.communicate()
            assert process.returncode == 0, output + error
        compose_command = [args.generation, '--compose', '--build-root', str(concurrent_build),
                           '--scope', 'test-audio', '--result', str(concurrent_build / 'Actions/audio/result.json')]
        composers = [subprocess.Popen(compose_command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) for _ in range(3)]
        for process in composers:
            output, error = process.communicate()
            assert process.returncode == 0, output + error
        run(args.generation, '--verify', '--build-root', concurrent_build, '--scope', 'test-audio')
        publication_copy_fixture(args.generation, root / 'copy')
        sibling_fixture(args.generation, root / 'sibling')
        upstream_fixture(args.generation, root / 'upstream')
        legacy_fixture(args.generation, root / 'legacy')
        retired_action_fixture(args.generation, root / 'retired')
        deep_dependency_fixture(args.generation, root / 'deep-chain')
    print('PASS: native Audio, depfiles/restat, generation, integrity/repair, symlinks, worktrees and concurrency')


if __name__ == '__main__':
    main()
