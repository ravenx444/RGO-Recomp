from __future__ import annotations
import copy
import hashlib
import json
import tempfile
import unittest
from pathlib import Path
from scripts import native_generation_metadata as metadata


class NativeMetadataTests(unittest.TestCase):
    def fixture(self):
        functions = {'section': [{'name': '.synthetic', 'rom': 0, 'vram': 0, 'size': 4,
                                  'functions': [{'name': 'synthetic', 'vram': 0, 'size': 4}]}]}
        data = {'section': [{'name': '', 'vram': 0, 'size': 0, 'symbols': []}]}
        bindings = {'schema_version': 1, 'rom_sha1': metadata.ROM_SHA1,
                    'layout': dict.fromkeys(metadata.LAYOUT_FIELDS, 0),
                    'covered_aliases': [], 'manual_sizes': [], 'sdk_identified': [],
                    'entrypoint_vram': '0x00000000',
                    'audio': {'rom_start': 0, 'size': 4, 'configuration': {
                        'text_offset': 0, 'text_size': 4, 'text_address': 0,
                        'extra_indirect_branch_targets': [], 'overlay_slots': []}}}
        return functions, data, bindings

    def test_factual_synthetic_shape_is_admitted(self):
        metadata.validate_facts(*self.fixture())

    def test_executable_payload_fields_are_rejected(self):
        for index, key in ((0, 'instructions'), (1, 'bytes'), (2, 'source_code')):
            args = self.fixture()
            args[index][key] = 'synthetic payload'
            with self.subTest(key=key), self.assertRaises(ValueError):
                metadata.validate_facts(*args)

    def test_function_body_and_nonfactual_name_are_rejected(self):
        for change in ({'code': 'synthetic'}, {'name': 'void synthetic() {}'}):
            args = self.fixture()
            args[0]['section'][0]['functions'][0].update(change)
            with self.subTest(change=change), self.assertRaises(ValueError):
                metadata.validate_facts(*args)

    def test_boolean_and_negative_addresses_are_rejected(self):
        for value in (True, -1, 2**32):
            args = self.fixture()
            args[0]['section'][0]['vram'] = value
            with self.subTest(value=value), self.assertRaises(ValueError):
                metadata.validate_facts(*args)

    def test_audio_extents_cannot_read_beyond_rom(self):
        args = self.fixture(); args[2]['audio']['rom_start'] = 32*1024*1024
        with self.assertRaises(ValueError): metadata.validate_facts(*args)

    def test_audio_overlay_extents_cannot_read_beyond_program(self):
        args = self.fixture()
        args[2]['audio']['configuration']['overlay_slots'] = [{'text_address': 0,
            'overlays': [{'offset': 3, 'size': 2}]}]
        with self.assertRaises(ValueError): metadata.validate_facts(*args)

    def test_external_manifest_digest_is_mandatory(self):
        for value in ('', '0'*63, 'g'*64):
            with self.subTest(value=value), self.assertRaises(ValueError):
                metadata.load_bundle(Path('unused'), value)

    def test_self_hashing_does_not_establish_manifest_trust(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            payload = json.dumps({'schema_version': 1}).encode()
            (root/'manifest.json').write_bytes(payload)
            with self.assertRaisesRegex(ValueError, 'trusted digest'):
                metadata.load_bundle(root, '0'*64)

    def test_native_transform_excludes_legacy_elf_inputs(self):
        from unittest import mock
        from scripts import transform_private_n64recomp_inputs as transform
        import sys
        base = ['transform', '--rom', 'synthetic.z64', '--context', 'functions.toml',
                '--data-context', 'data.toml', '--layout', 'layout.json', '--output-dir', 'ignored']
        with mock.patch.object(sys, 'argv', base + ['--native-metadata', 'facts',
                               '--metadata-manifest-sha256', '0'*64]):
            args = transform.parse_args()
            self.assertIsNone(args.elf)
            self.assertIsNone(args.readelf)
        with mock.patch.object(sys, 'argv', base + ['--native-metadata', 'facts',
                               '--metadata-manifest-sha256', '0'*64, '--elf', 'synthetic.elf']):
            with self.assertRaises(SystemExit): transform.parse_args()

    def test_windows_generator_uses_same_audio_numeric_configuration(self):
        import tomllib
        from scripts import generate_from_rom_windows as native
        record = self.fixture()[2]['audio']
        config = tomllib.loads(native.audio_config(record))
        for key, value in record['configuration'].items():
            self.assertEqual(config[key], value)
        self.assertEqual(config['rom_file_path'], 'program.bin')
        self.assertEqual(config['output_file_path'], './jfg-audio-probe.cpp')

    def test_native_stage_accepts_parent_maximum_jobs(self):
        import sys
        from unittest import mock
        from scripts import generate_from_rom_windows as native
        with tempfile.TemporaryDirectory() as directory:
            arguments = ['native-stage', '--rom', 'synthetic.z64', '--workspace', directory,
                         '--dependency-root', 'unused', '--jobs', str(native.MAX_JOBS)]
            with mock.patch.object(sys, 'argv', arguments), mock.patch.object(sys, 'platform', 'win32'), \
                 mock.patch.object(native, 'validate_rom', side_effect=RuntimeError('fixture stopped at ROM validation')):
                with self.assertRaisesRegex(RuntimeError, 'fixture stopped'):
                    native.main()

    def test_native_workspace_accepts_fresh_dependency_setup_logs(self):
        from scripts import generate_from_rom_windows as native
        import build_from_rom as build
        names = {key:value for key,value in build.NAMES.items() if key!='jfg-decomp'}
        lock = {'repositories': [{'id':key} for key in names]}
        with tempfile.TemporaryDirectory() as directory:
            workspace = Path(directory)
            # Exercise actual parent step names from an empty dependency cache.
            steps = build.plan_steps(workspace/'deps', lock, True, True, False, names=names)
            for step in steps:
                if step not in ('configure', 'compile'):
                    (workspace/(step+'.log')).write_text('fixture diagnostic')
            (workspace/'status.json').write_text('{}')
            (workspace/'build.lock').touch()
            native.verify_new_workspace(workspace)
            unknown = workspace/'arbitrary.log'
            unknown.write_text('not an admitted parent diagnostic')
            with self.assertRaisesRegex(ValueError, 'only build bookkeeping'):
                native.verify_new_workspace(workspace)
            unknown.unlink()
            (workspace/'normalized').mkdir()
            with self.assertRaisesRegex(ValueError, 'only build bookkeeping'):
                native.verify_new_workspace(workspace)

    def test_native_workspace_rejects_directory_using_log_name(self):
        from scripts import generate_from_rom_windows as native
        with tempfile.TemporaryDirectory() as directory:
            workspace = Path(directory)
            (workspace/'clone-rt64.log').mkdir()
            with self.assertRaisesRegex(ValueError, 'only build bookkeeping'):
                native.verify_new_workspace(workspace)

    def test_native_build_plan_excludes_decomp_download(self):
        from scripts import generate_from_rom_windows
        import build_from_rom as build
        names = {key:value for key,value in build.NAMES.items() if key!='jfg-decomp'}
        lock = {'repositories': [{'id':key} for key in build.NAMES]}
        with tempfile.TemporaryDirectory() as directory:
            steps = build.plan_steps(Path(directory), lock, True, True, False, names=names)
        self.assertNotIn('clone-jfg-decomp', steps)
        self.assertNotIn('submodules-jfg-decomp', steps)
        self.assertIn('clone-n64recomp', steps)
        self.assertIn('generate', steps)

    def test_default_candidate_has_pinned_source_provenance(self):
        root = Path(__file__).resolve().parents[1]
        directory, digest = metadata.default_bundle(root)
        functions, data, bindings = metadata.load_bundle(directory, digest)
        self.assertEqual(len(functions['section']), 156)
        self.assertEqual(len(data['section']), 324)
        self.assertEqual(bindings['rom_sha1'], metadata.ROM_SHA1)

    def test_default_pin_rejects_changed_payload_and_replaced_manifest(self):
        import shutil
        root = Path(__file__).resolve().parents[1]
        directory, digest = metadata.default_bundle(root)
        with tempfile.TemporaryDirectory() as temporary:
            candidate = Path(temporary)/'facts'
            shutil.copytree(directory, candidate)
            payload = candidate/'bindings.json'
            values = json.loads(payload.read_text())
            values['audio']['rom_start'] += 4
            payload.write_text(json.dumps(values))
            with self.assertRaisesRegex(ValueError, 'payload'):
                metadata.load_bundle(candidate, digest)
            manifest = candidate/'manifest.json'
            values = json.loads(manifest.read_text())
            values['files']['bindings.json']['size'] = payload.stat().st_size
            values['files']['bindings.json']['sha256'] = hashlib.sha256(payload.read_bytes()).hexdigest()
            manifest.write_text(json.dumps(values))
            with self.assertRaisesRegex(ValueError, 'trusted digest'):
                metadata.load_bundle(candidate, digest)

    def test_nonobject_section_is_rejected_with_validation_error(self):
        for value in (True, 'synthetic', 123):
            args = self.fixture();args[1]['section'] = [value]
            with self.subTest(value=value), self.assertRaises(ValueError):
                metadata.validate_facts(*args)

    def test_duplicate_json_fields_are_rejected(self):
        with self.assertRaises(ValueError): metadata._json(b'{"size":1,"size":2}')

    def test_pinned_manifest_still_rejects_extra_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = {'schema_version': 1, 'rom_sha1': metadata.ROM_SHA1,
                'source_decomp_commit': '1'*40, 'source_recompiler_commit': '2'*40,
                'files': dict.fromkeys(metadata.FILES | {'game.z64'}, {}), 'scope': 'synthetic'}
            payload = json.dumps(manifest).encode(); (root/'manifest.json').write_bytes(payload)
            with self.assertRaises(ValueError):
                metadata.load_bundle(root, hashlib.sha256(payload).hexdigest())


if __name__ == '__main__': unittest.main()
