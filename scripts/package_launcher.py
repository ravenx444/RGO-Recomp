#!/usr/bin/env python3
"""Package only the independently compiled launcher, setup guides and license."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / 'build' / 'launcher'
LAUNCHER_EXE = 'RGO-Launcher.exe'
VERSION = re.compile(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?')


def launcher_version(root: Path) -> str:
    source = (root / 'launcher/windows/Launcher.cs').read_text(encoding='utf-8')
    versions = re.findall(r'AssemblyInformationalVersion\("([^"\n]*)"\)', source)
    if len(versions) != 1 or not VERSION.fullmatch(versions[0]):
        raise ValueError('Launcher informational version must be a semantic version')
    version = versions[0]
    suffix = VERSION.fullmatch(version)[4]
    if suffix and any(part.isdigit() and len(part) > 1 and part.startswith('0') for part in suffix.split('.')):
        raise ValueError('Numeric prerelease identifiers must not have leading zeros')
    return version


def package_name(version: str) -> str:
    return 'RGO-Launcher-' + version + '-windows-x64.zip'


def package_members(root: Path, output: Path) -> dict[str, Path]:
    members = {
        LAUNCHER_EXE: output / LAUNCHER_EXE,
        'README.md': root / 'docs/rgo/PLAYER_GUIDE.md',
        'LICENSE': root / 'LICENSE',
        'THIRD_PARTY_NOTICES.md': root / 'THIRD_PARTY_NOTICES.md',
        'CREDITS.md': root / 'CREDITS.md',
    }
    for folder in ('licenses', 'fonts'):
        for path in (root / 'launcher/ui' / folder).glob('*.txt'):
            members['licenses/' + path.name] = path
    members['THIRD-PARTY.txt'] = root / 'launcher/ui/THIRD-PARTY.txt'
    members['licenses/font-manifest.json'] = root / 'launcher/ui/fonts/manifest.json'
    members['licenses/provenance.json'] = root / 'launcher/ui/licenses/provenance.json'
    for name, path in members.items():
        if not path.is_file() or path.is_symlink():
            raise SystemExit('Missing or linked package input: ' + name)
    notices = members['THIRD-PARTY.txt'].read_text(encoding='utf-8')
    references = set(re.findall(r'\blicenses/[A-Za-z0-9_.-]+\.(?:txt|json)\b', notices))
    missing = references - members.keys()
    if missing:
        raise SystemExit('Referenced license missing from package: ' + ', '.join(sorted(missing)))
    return members


def write_package(output: Path, archive_name: str, payloads: dict[str, bytes], revision: str) -> None:
    archive = output / archive_name
    if archive.exists():
        raise SystemExit('Package already exists; preserve it and use a new reviewed version')
    with zipfile.ZipFile(archive, 'x', compression=zipfile.ZIP_DEFLATED) as package:
        for name, payload in payloads.items():
            info = zipfile.ZipInfo(name, date_time=(2026, 10, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            package.writestr(info, payload)
    with zipfile.ZipFile(archive) as package:
        if set(package.namelist()) != set(payloads) or package.testzip() is not None:
            raise SystemExit('Archive verification failed')
        for name, payload in payloads.items():
            if package.read(name) != payload:
                raise SystemExit('Archive member changed')
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    (output / 'SHA256SUMS.txt').write_text(digest + '  ' + archive_name + '\n' + hashlib.sha256(payloads[LAUNCHER_EXE]).hexdigest() + '  ' + LAUNCHER_EXE + '\n', encoding='ascii')
    (output / 'package-inventory.json').write_text(json.dumps({
        'package': archive_name,
        'source_commit': revision,
        'sha256': digest,
        'members': {name: hashlib.sha256(payload).hexdigest() for name, payload in payloads.items()},
        'scope': 'Launcher only; no game runtime or ROM-derived inputs',
    }, indent=2) + '\n', encoding='utf-8')
    print('Verified launcher-only archive: ' + archive_name)


def validate_tracked_inputs(root: Path, inputs: set[str]) -> None:
    tracked = set(subprocess.check_output(
        ['git', 'ls-tree', '-r', '--name-only', '-z', 'HEAD'], cwd=root
    ).decode('utf-8').split('\0'))
    uncommitted = inputs - tracked
    if uncommitted:
        raise SystemExit('Launcher build includes uncommitted inputs: ' +
                         ', '.join(sorted(uncommitted)))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=OUTPUT,
                        help="Directory containing the tested launcher and build receipt")
    output = parser.parse_args(argv).output.resolve()
    name = package_name(launcher_version(ROOT))
    members = package_members(ROOT, output)
    binary = members[LAUNCHER_EXE].read_bytes()
    if not binary.startswith(b'MZ') or len(binary) > 33_554_432:
        raise SystemExit('Unexpected launcher executable')
    receipt = json.loads((output / 'launcher-build.json').read_text(encoding='utf-8-sig'))
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    if subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=no'], cwd=ROOT, text=True).strip():
        raise SystemExit('Commit the reviewed source changes before packaging the pinned launcher')
    expected_inputs = {'launcher/windows/Launcher.cs', 'launcher/windows/FrontendBridge.cs', 'src/app/frontend_win32.cpp', 'launcher/native/CMakeLists.txt', 'launcher/windows/FirstRun.cs',
                       'launcher/windows/Setup.ps1', 'launcher/windows/Support.cs', 'launcher/windows/Diagnostics.cs', 'launcher/windows/Controller.cs', 'launcher/windows/NavigationMap.cs', 'launcher/windows/Audio.cs', 'launcher/windows/MapLayers.cs', 'launcher/windows/NavigationRoute.cs', 'launcher/windows/NavigationExplorer.cs', 'launcher/windows/NavigationCollision.cs', 'launcher/windows/BoxJump.cs', 'launcher/windows/ChestRoute.cs', 'launcher/windows/NavigationRunner.cs', 'launcher/windows/AutonomousExplorer.cs', 'launcher/windows/InventoryWindow.cs', 'launcher/windows/InventoryImages.cs', 'launcher/windows/InventoryModel.cs', 'launcher/windows/NativeLiveTools.cs', 'scripts/build_launcher.ps1'}
    expected_inputs.update(path.relative_to(ROOT).as_posix() for path in (ROOT / 'launcher/ui').rglob('*') if path.is_file())
    validate_tracked_inputs(ROOT, expected_inputs)
    if (receipt.get('source_commit') != revision or set(receipt.get('inputs', {})) != expected_inputs
            or receipt.get('executable_sha256') != hashlib.sha256(binary).hexdigest()
            or any(hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != digest
                   for name, digest in receipt['inputs'].items())):
        raise SystemExit('Launcher build is stale; compile and test this revision before packaging')
    # Compiler debug paths are not needed in the distributable launcher.
    for data in (binary.lower(), binary.decode('utf-16le', errors='ignore').lower().encode('utf-8')):
        if b':\\users\\' in data or b'/home/' in data or b'.pdb' in data:
            raise SystemExit('Executable contains a debug or personal path')
    write_package(output, name, {name: path.read_bytes() for name, path in members.items()}, revision)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
