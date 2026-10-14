#!/usr/bin/env python3
"""Windows-only private generation from pinned factual metadata.

The default bundle contains factual metadata only. All generated game files remain local.
The maintainer ELF pipeline remains available separately for qualification.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tomllib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'scripts'))
from native_generation_metadata import load_bundle, default_bundle, ROM_SHA1
from probe_n64recomp_cpu import verify_private_work_directory
from build_from_rom import find_cmake, validate_rom
import original_timing_generation as timing
import identify_libultra as sdk
from rgo_build_cache import EXPECTED_GENERATION_FILES, MAX_JOBS


def audio_config(record: dict) -> str:
    def toml(value):
        if isinstance(value, dict):
            return '{'+', '.join(k+' = '+toml(v) for k,v in value.items())+'}'
        if isinstance(value, list): return '['+', '.join(map(toml,value))+']'
        return json.dumps(value)
    values = dict(record['configuration'], rom_file_path='program.bin',
                  output_file_path='./jfg-audio-probe.cpp', output_function_name='jfg_audio_probe')
    return '\n'.join(k+' = '+toml(v) for k,v in values.items())+'\n'


def verify_new_workspace(workspace: Path) -> None:
    # The parent owns dependency setup and writes these diagnostic logs before
    # generation starts. Admit only its fixed filenames, never game output.
    bookkeeping = {'status.json', 'build.lock', 'generate.log'}
    bookkeeping.update(f'{stage}-{dependency}.log'
                       for stage in ('clone', 'pin', 'submodules')
                       for dependency in ('n64recomp', 'n64modernruntime', 'rt64'))
    if (not workspace.is_dir() or any(
            p.name not in bookkeeping or p.is_symlink() or not p.is_file()
            for p in workspace.iterdir())):
        raise ValueError('Native generation requires a new private workspace containing only build bookkeeping')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rom', type=Path, required=True)
    parser.add_argument('--workspace', type=Path, required=True)
    parser.add_argument('--dependency-root', type=Path, required=True)
    parser.add_argument('--native-metadata', type=Path)
    parser.add_argument('--metadata-manifest-sha256')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if sys.platform != 'win32': parser.error('Run this stage on Windows')
    if not 1 <= args.jobs <= MAX_JOBS: parser.error(f'--jobs must be between 1 and {MAX_JOBS}')
    if bool(args.native_metadata) != bool(args.metadata_manifest_sha256):
        parser.error('Custom native metadata requires both its path and trusted manifest SHA256')
    if not args.native_metadata:
        args.native_metadata, args.metadata_manifest_sha256 = default_bundle(ROOT)
    workspace = args.workspace.resolve()
    verify_private_work_directory(workspace)
    verify_new_workspace(workspace)
    validate_rom(args.rom)
    facts = args.native_metadata.resolve()
    _, _, bindings = load_bundle(facts, args.metadata_manifest_sha256)
    cmake = find_cmake()
    source = args.dependency_root.resolve()/'N64Recomp'
    lock = json.loads((ROOT/'dependencies.lock.json').read_text())
    pin = next(r['commit'] for r in lock['repositories'] if r['id']=='n64recomp')
    head = subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD'],text=True).strip()
    if head != pin: raise ValueError('Recompiler dependency revision mismatch')
    def run(name, command, cwd=ROOT):
        print(name, flush=True)
        with (workspace/(name+'.log')).open('w', encoding='utf-8') as log:
            subprocess.run([str(x) for x in command],cwd=cwd,stdout=log,stderr=subprocess.STDOUT,check=True)
    recomp = workspace/'N64Recomp'
    run('clone-recompiler',['git','clone','--no-hardlinks','--config','core.autocrlf=false',source,recomp])
    run('recompiler-submodules',['git','submodule','update','--init','--recursive'],recomp)
    run('patch-recompiler',[sys.executable,ROOT/'scripts/apply_n64recomp_patchset.py','--target',recomp,'--project-root',ROOT])
    toolbuild = workspace/'generators'
    run('configure-recompiler',[cmake,'-S',recomp,'-B',toolbuild,'-G','Visual Studio 17 2022','-A','x64','-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded'])
    run('build-recompiler',[cmake,'--build',toolbuild,'--config','Release','--target','N64RecompCLI','RSPRecomp','--parallel',min(args.jobs, 32)])
    cpu, rsp = toolbuild/'Release/N64Recomp.exe', toolbuild/'Release/RSPRecomp.exe'
    (workspace/'layout.json').write_text(json.dumps(bindings['layout']),encoding='utf-8')
    # Normalization admits only private paths. Copy the qualified factual
    # contexts into this private workspace instead of weakening that boundary.
    context = workspace/'context'
    context.mkdir()
    shutil.copy2(facts/'functions.toml', context/'dump.toml')
    shutil.copy2(facts/'data-symbols.toml', context/'data_dump.toml')
    transform = workspace/'transform'
    run('transform',[sys.executable,ROOT/'scripts/transform_private_n64recomp_inputs.py',
        '--rom',args.rom.resolve(),'--native-metadata',facts,'--metadata-manifest-sha256',args.metadata_manifest_sha256,
        '--layout',workspace/'layout.json','--context',context/'dump.toml',
        '--data-context',context/'data_dump.toml','--output-dir',transform])
    sites = timing.configure(transform)
    run('generate-cpu',[cpu,'recompile-private.toml'],transform)
    paired = timing.prepare_raw(transform/'generated')
    run('normalize',[sys.executable,ROOT/'scripts/build_private_generated_root.py',
        '--raw-generated',transform/'generated','--symbols',transform/'symbols-private.toml',
        '--original-context',context/'dump.toml','--runtime-manifest',transform/'runtime-link-private.json',
        '--recomp-header',recomp/'include/recomp.h','--output',workspace/'normalized'])
    timing.finish(workspace/'normalized',sites,paired)
    names = {r['libultra']:int(r['vram'],16) for r in bindings['sdk_identified']}
    names['entrypoint'] = int(bindings['entrypoint_vram'],16)
    detail = sdk.identify(names,sdk._parse_generated_symbols(transform/'symbols-private.toml'))['detail']
    (workspace/'libultra.json').write_text(json.dumps(detail,indent=2,sort_keys=True)+'\n',encoding='utf-8',newline='\n')
    audio = workspace/'audio'; audio.mkdir()
    rom = args.rom.read_bytes(); record = bindings['audio']; start = record['rom_start']
    (audio/'program.bin').write_bytes(rom[start:start+record['size']])
    (audio/'input.toml').write_text(audio_config(record),encoding='utf-8',newline='\n')
    run('generate-audio',[rsp,'input.toml'],audio)
    generated = audio/'jfg-audio-probe.cpp'; text = generated.read_text(encoding='utf-8')
    marker = '        slots[slot] = overlay;'
    if text.count(marker)!=1: raise ValueError('Unexpected RSP generator overlay dispatcher')
    text = text.replace(marker,'        jfg_audio_probe_overlay_swaps++;\n        jfg_audio_probe_overlay_variant_one += overlay == 1;\n'+marker)
    generated.write_text('#include <cstdint>\nuint32_t jfg_audio_probe_overlay_swaps = 0;\nuint32_t jfg_audio_probe_overlay_variant_one = 0;\n'+text,encoding='utf-8',newline='\n')
    for name in ('brokered-rsp.hpp','production-audio-adapter.cpp'):
        shutil.copy2(ROOT/'src/bootstrap'/name,audio/name)
    (audio/'brokered-jfg-audio-probe.cpp').write_text('#include "brokered-rsp.hpp"\n#include "jfg-audio-probe.cpp"\n',encoding='utf-8',newline='\n')
    manifest = {'complete':True,'rom_sha1':ROM_SHA1,
        'files':{name:hashlib.sha256((workspace/name).read_bytes()).hexdigest() for name in sorted(EXPECTED_GENERATION_FILES)},
        'fresh_matching_elf':False,'fresh_cpu':True,'fresh_audio':True,'tool_cache_reused':False,
        'metadata_backend':'pinned-factual-metadata-v1','metadata_manifest_sha256':args.metadata_manifest_sha256,
        'generator_host':'windows','generator_source_commit':pin,
        'generator_sha256':{name:hashlib.sha256(path.read_bytes()).hexdigest() for name,path in (('cpu',cpu),('rsp',rsp))}}
    (workspace/'generation.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8',newline='\n')
    print('Fresh Windows CPU and audio generation complete; no fresh ELF claimed.',flush=True)
    return 0


def generation_error_message(error: Exception) -> str:
    if isinstance(error, OSError) and getattr(error, 'winerror', None) == 4551:
        return ('Windows Application Control blocked a locally built setup tool. '
                'Setup stopped. Ask your administrator or security provider to review '
                'the blocked tool and signing policy before retrying. '
                'Original diagnostic: ' + str(error))
    return str(error)


if __name__=='__main__':
    try: raise SystemExit(main())
    except (OSError,ValueError,subprocess.CalledProcessError) as error:
        print(generation_error_message(error),file=sys.stderr); raise SystemExit(1)
