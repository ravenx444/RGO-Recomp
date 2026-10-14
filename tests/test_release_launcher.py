import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock
import zipfile

from scripts import package_launcher as package
from scripts import release_launcher as release


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'launcher/windows').mkdir(parents=True)
        self.version('1.0.0')
        (self.root / 'CMakeLists.txt').write_text('project( jfg_recomp VERSION 1.0.0 LANGUAGES C CXX)', encoding='utf-8')
        self.metadata = {'tag': 'rgo-v1.0.0', 'version': '1.0.0', 'commit': 'a' * 40, 'prerelease': False}

    def version(self, version):
        (self.root / 'launcher/windows/Launcher.cs').write_text(
            f'[assembly: AssemblyInformationalVersion("{version}")]', encoding='utf-8')

    def assets(self):
        name = package.package_name(self.metadata['version'])
        members = {'RGO-Launcher.exe': b'MZ synthetic test fixture', 'LICENSE': b'fixture',
                   'THIRD-PARTY.txt': b'licenses/fixture.txt', 'licenses/fixture.txt': b'fixture notice',
                   'licenses/font-manifest.json': b'{}', 'licenses/provenance.json': b'{}'}
        with zipfile.ZipFile(self.root / name, 'w') as z:
            for path, data in members.items():
                z.writestr(path, data)
        digest = hashlib.sha256((self.root / name).read_bytes()).hexdigest()
        inventory = {'source_commit': self.metadata['commit'], 'package': name, 'sha256': digest,
                     'members': {path: hashlib.sha256(data).hexdigest() for path, data in members.items()}}
        (self.root / 'package-inventory.json').write_text(json.dumps(inventory), encoding='utf-8')
        (self.root / 'SHA256SUMS.txt').write_text(digest + '  ' + name + '\n' + inventory['members']['RGO-Launcher.exe'] + '  RGO-Launcher.exe\n', encoding='ascii')
        return inventory

    def test_package_inputs_must_belong_to_the_embedded_commit(self):
        subprocess.run(['git', 'init', '-q'], cwd=self.root, check=True)
        subprocess.run(['git', 'add', 'launcher/windows/Launcher.cs', 'CMakeLists.txt'],
                       cwd=self.root, check=True)
        subprocess.run(['git', '-c', 'user.name=Fixture',
                        '-c', 'user.email=fixture@example.invalid',
                        'commit', '-qm', 'Synthetic release inputs'], cwd=self.root, check=True)
        inputs = {'launcher/windows/Launcher.cs', 'CMakeLists.txt'}
        package.validate_tracked_inputs(self.root, inputs)
        uncommitted = self.root / 'launcher' / 'ui' / 'uncommitted.rml'
        uncommitted.parent.mkdir()
        uncommitted.write_text('synthetic asset')
        with self.assertRaises(SystemExit):
            package.validate_tracked_inputs(self.root, inputs | {'launcher/ui/uncommitted.rml'})
        subprocess.run(['git', 'add', 'launcher/ui/uncommitted.rml'], cwd=self.root, check=True)
        with self.assertRaises(SystemExit):
            package.validate_tracked_inputs(self.root, inputs | {'launcher/ui/uncommitted.rml'})

    def test_package_version_follows_source(self):
        self.version('1.2.3-rc.1')
        self.assertEqual(package.package_name(package.launcher_version(self.root)), 'RGO-Launcher-1.2.3-rc.1-windows-x64.zip')

    def test_invalid_versions_rejected(self):
        for version in ('01.0.0', '1.0', '1.0.0/evil', '1.0.0-rc.01', ''):
            with self.subTest(version=version):
                self.version(version)
                with self.assertRaises(ValueError):
                    package.launcher_version(self.root)

    def test_stable_tag_on_main(self):
        with mock.patch.object(release, 'command', side_effect=['a' * 40, 'a' * 40]), mock.patch.object(release.subprocess, 'run') as git:
            self.assertEqual(release.release_metadata(self.root, 'rgo-v1.0.0', 'tag'), self.metadata)
            self.assertIn('origin/main', git.call_args.args[0])

    def test_preview_tag_classified_separately(self):
        self.version('1.0.0-rc.1')
        with mock.patch.object(release, 'command', side_effect=['a' * 40, 'a' * 40]), mock.patch.object(release.subprocess, 'run'):
            self.assertTrue(release.release_metadata(self.root, 'rgo-v1.0.0-rc.1', 'tag')['prerelease'])

    def test_branches_and_mismatched_tags_rejected(self):
        for tag, ref in [('main', 'branch'), ('rgo-v0.4.0-preview.4', 'tag'), ('rgo-v1.0', 'tag'), ('v1.0.0', 'tag')]:
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                release.release_metadata(self.root, tag, ref)

    def test_mismatched_cmake_version_rejected(self):
        (self.root / 'CMakeLists.txt').write_text('project(jfg_recomp VERSION 2.0.0)', encoding='utf-8')
        with self.assertRaises(ValueError):
            release.release_metadata(self.root, 'rgo-v1.0.0', 'tag')

    def test_moved_tag_rejected(self):
        with mock.patch.object(release, 'command', side_effect=['a' * 40, 'b' * 40]), self.assertRaises(ValueError):
            release.release_metadata(self.root, 'rgo-v1.0.0', 'tag')

    def test_commit_outside_main_rejected(self):
        with mock.patch.object(release, 'command', side_effect=['a' * 40, 'a' * 40]), mock.patch.object(release.subprocess, 'run', side_effect=subprocess.CalledProcessError(1, ['git'])), self.assertRaises(subprocess.CalledProcessError):
            release.release_metadata(self.root, 'rgo-v1.0.0', 'tag')

    def test_written_package_keeps_archive_name_in_inventory_and_checksums(self):
        self.assets()
        archive_name = package.package_name(self.metadata['version'])
        with zipfile.ZipFile(self.root / archive_name) as z:
            payloads = {name: z.read(name) for name in z.namelist()}
        destination = self.root / 'packaged'
        destination.mkdir()
        package.write_package(destination, archive_name, payloads, self.metadata['commit'])
        inventory = json.loads((destination / 'package-inventory.json').read_text())
        self.assertEqual(inventory['package'], archive_name)
        self.assertTrue((destination / 'SHA256SUMS.txt').read_text().splitlines()[0].endswith('  ' + archive_name))
        self.assertEqual(release.verify_assets(destination, self.metadata)[0].name, archive_name)
        with self.assertRaises(SystemExit):
            package.write_package(destination, archive_name, payloads, self.metadata['commit'])

    def test_release_assets_verified(self):
        self.assets()
        assets = release.verify_assets(self.root, self.metadata)
        self.assertEqual(len(assets), 3)
        self.assertFalse(any(p.suffix == '.exe' for p in assets))

    def test_stale_package_rejected(self):
        self.assets()
        with self.assertRaises(ValueError):
            release.verify_assets(self.root, {**self.metadata, 'commit': 'b' * 40})

    def test_corrupted_package_rejected(self):
        self.assets()
        with (self.root / package.package_name('1.0.0')).open('ab') as f:
            f.write(b'changed')
        with self.assertRaises(ValueError):
            release.verify_assets(self.root, self.metadata)

    def test_wrong_checksums_rejected(self):
        self.assets()
        (self.root / 'SHA256SUMS.txt').write_text('bad', encoding='ascii')
        with self.assertRaises(ValueError):
            release.verify_assets(self.root, self.metadata)

    def test_stable_and_preview_publication_flags(self):
        for preview in (False, True):
            with self.subTest(preview=preview):
                metadata = {**self.metadata, 'prerelease': preview}
                self.assets()
                calls = []
                def gh(*args, **kwargs):
                    calls.append(args)
                    if args[:2] == ('gh', 'api'):
                        return json.dumps({'draft': False, 'prerelease': preview, 'tag_name': metadata['tag'],
                                           'assets': [{'name': p.name} for p in release.verify_assets(self.root, metadata)], 'html_url': 'https://example.invalid/release'})
                    return ''
                with mock.patch.object(release.subprocess, 'run', return_value=subprocess.CompletedProcess([], 1, '{"status":"404"}', '')), mock.patch.object(release, 'command', side_effect=gh):
                    release.publish(self.root, self.root, metadata, 'owner/repo')
                create = next(c for c in calls if c[:3] == ('gh', 'release', 'create'))
                edit = next(c for c in calls if c[:3] == ('gh', 'release', 'edit'))
                self.assertIn('--draft', create)
                self.assertIn('--verify-tag', create)
                self.assertIn('--draft=false', edit)
                self.assertIn('--latest=' + ('false' if preview else 'true'), edit)
                self.assertIn('--prerelease=' + ('true' if preview else 'false'), edit)
                self.assertEqual(any(c[-1].endswith('/latest') for c in calls), not preview)

    def test_existing_releases_and_api_errors_do_not_publish(self):
        self.assets()
        for code, response in [(0, '{}'), (1, '{"status":"403"}'), (1, 'network error')]:
            with self.subTest(response=response), mock.patch.object(release.subprocess, 'run', return_value=subprocess.CompletedProcess([], code, response, '')), mock.patch.object(release, 'command') as gh:
                with self.assertRaises(ValueError):
                    release.publish(self.root, self.root, self.metadata, 'owner/repo')
                gh.assert_not_called()

    def test_failed_upload_does_not_publish_draft(self):
        self.assets()
        with mock.patch.object(release.subprocess, 'run', return_value=subprocess.CompletedProcess([], 1, '{"status":"404"}', '')), mock.patch.object(release, 'command', side_effect=subprocess.CalledProcessError(1, ['gh'])) as gh:
            with self.assertRaises(subprocess.CalledProcessError):
                release.publish(self.root, self.root, self.metadata, 'owner/repo')
            self.assertEqual(gh.call_count, 1)
            self.assertEqual(gh.call_args.args[:3], ('gh', 'release', 'create'))


if __name__ == '__main__':
    unittest.main()
