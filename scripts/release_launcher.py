#!/usr/bin/env python3
"""Check a release tag and publish a validated launcher package, never just a tag."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import zipfile

try:
    from .package_launcher import LAUNCHER_EXE, launcher_version, package_name
except ImportError:
    from package_launcher import LAUNCHER_EXE, launcher_version, package_name

ROOT = Path(__file__).resolve().parents[1]
# RGO releases are tagged rgo-v<version>, apart from upstream's v<version> tags.
TAG_PREFIX = 'rgo-v'


def command(*args: str, cwd: Path = ROOT) -> str:
    return subprocess.check_output(list(args), cwd=cwd, text=True).strip()


def release_metadata(root: Path, tag: str, ref_type: str) -> dict:
    if ref_type != 'tag':
        raise ValueError('Run this workflow on an existing version tag, not a branch')
    version = launcher_version(root)
    if tag != TAG_PREFIX + version:
        raise ValueError('Tag must match the launcher version: ' + TAG_PREFIX + version)
    cmake = (root / 'CMakeLists.txt').read_text(encoding='utf-8')
    match = re.search(r'project\(\s*jfg_recomp\s+VERSION\s+([0-9.]+)', cmake)
    if not match or match[1] != version.split('-', 1)[0]:
        raise ValueError('CMake and launcher versions disagree')
    commit = command('git', 'rev-parse', 'HEAD', cwd=root)
    tagged = command('git', 'rev-parse', 'refs/tags/' + tag + '^{commit}', cwd=root)
    if tagged != commit:
        raise ValueError('The release tag does not identify the checked-out commit')
    subprocess.run(['git', 'merge-base', '--is-ancestor', commit, 'origin/main'], cwd=root, check=True)
    return {'tag': tag, 'version': version, 'commit': commit, 'prerelease': '-' in version}


def verify_assets(output: Path, metadata: dict) -> list[Path]:
    inventory_path = output / 'package-inventory.json'
    inventory = json.loads(inventory_path.read_text(encoding='utf-8'))
    name = package_name(metadata['version'])
    if inventory['source_commit'] != metadata['commit'] or inventory['package'] != name:
        raise ValueError('Package inventory does not match this release tag and commit')
    archive = output / name
    sums = output / 'SHA256SUMS.txt'
    assets = [archive, sums, inventory_path]
    if any(not p.is_file() or p.is_symlink() for p in assets):
        raise ValueError('Missing or linked release asset')
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest != inventory['sha256']:
        raise ValueError('Release ZIP hash differs from the package inventory')
    with zipfile.ZipFile(archive) as z:
        if len(z.namelist()) != len(set(z.namelist())) or set(z.namelist()) != set(inventory['members']) or z.testzip():
            raise ValueError('Release ZIP membership or integrity check failed')
        required = {LAUNCHER_EXE, 'LICENSE', 'THIRD-PARTY.txt', 'licenses/font-manifest.json', 'licenses/provenance.json'}
        if not required <= set(z.namelist()):
            raise ValueError('Release ZIP lacks the launcher or required license records')
        notices = z.read('THIRD-PARTY.txt').decode('utf-8')
        references = set(re.findall(r'\blicenses/[A-Za-z0-9_.-]+\.(?:txt|json)\b', notices))
        if not references <= set(z.namelist()):
            raise ValueError('Release ZIP lacks a referenced license')
        for member, expected in inventory['members'].items():
            if hashlib.sha256(z.read(member)).hexdigest() != expected:
                raise ValueError('Release ZIP member hash mismatch: ' + member)
        executable_digest = hashlib.sha256(z.read(LAUNCHER_EXE)).hexdigest()
    expected_sums = digest + '  ' + name + '\n' + executable_digest + '  ' + LAUNCHER_EXE + '\n'
    if sums.read_text(encoding='ascii') != expected_sums:
        raise ValueError('Release checksum file does not match the ZIP and embedded launcher')
    return assets


def publish(root: Path, output: Path, metadata: dict, repo: str) -> str:
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repo):
        raise ValueError('Expected the GitHub owner/repository name')
    assets = verify_assets(output, metadata)
    endpoint = f'repos/{repo}/releases/tags/{metadata["tag"]}'
    existing = subprocess.run(['gh', 'api', endpoint], cwd=root, text=True, capture_output=True)
    if existing.returncode == 0:
        raise ValueError('A release or draft already exists; inspect it before retrying. Published assets are never overwritten.')
    try:
        missing = json.loads(existing.stdout).get('status') in (404, '404')
    except (ValueError, AttributeError):
        missing = False
    if not missing:
        raise ValueError('Cannot confirm that the release is absent; check GitHub authentication or API availability')
    notes = output / 'release-notes.md'
    notes.write_text(
        f'Windows x64 launcher for RGO {metadata["version"]}.\n\n'
        f'Download **{assets[0].name}** and extract the complete ZIP, including its license files. '
        'Supply your own supported North American ROM; the launcher downloads what it needs and builds the game locally. First-time setup takes about an hour.\n\n'
        f'[Setup guide](https://github.com/{repo}/blob/{metadata["tag"]}/docs/rgo/PLAYER_GUIDE.md) · '
        f'[Known issues](https://github.com/{repo}/blob/{metadata["tag"]}/docs/known-issues.md)\n\n'
        f'Source commit: `{metadata["commit"]}`. Checksums and the package inventory are attached.\n', encoding='utf-8')
    # Keep the release private until every requested asset has uploaded.
    command('gh', 'release', 'create', metadata['tag'], *map(str, assets), '--repo', repo,
            '--verify-tag', '--draft', '--title', 'RGO ' + metadata['tag'],
            '--notes-file', str(notes), cwd=root)
    prerelease = 'true' if metadata['prerelease'] else 'false'
    latest = 'false' if metadata['prerelease'] else 'true'
    command('gh', 'release', 'edit', metadata['tag'], '--repo', repo,
            '--draft=false', '--prerelease=' + prerelease, '--latest=' + latest, cwd=root)
    release = json.loads(command('gh', 'api', endpoint, cwd=root))
    if release['draft'] or release['prerelease'] != metadata['prerelease'] or release['tag_name'] != metadata['tag']:
        raise ValueError('Published release metadata did not match the requested state')
    if {p.name for p in assets} != {a['name'] for a in release['assets']}:
        raise ValueError('Published release does not contain the expected assets')
    if not metadata['prerelease']:
        current = json.loads(command('gh', 'api', f'repos/{repo}/releases/latest', cwd=root))
        if current['tag_name'] != metadata['tag']:
            raise ValueError('The stable release was published but is not GitHub Latest')
    return release['html_url']


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('check', 'publish'))
    args = parser.parse_args()
    metadata = release_metadata(ROOT, os.environ.get('RELEASE_TAG', ''), os.environ.get('RELEASE_REF_TYPE', ''))
    if args.action == 'check':
        print(json.dumps(metadata, sort_keys=True))
    else:
        print(publish(ROOT, ROOT / 'build/launcher', metadata, os.environ.get('GITHUB_REPOSITORY', '')))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
