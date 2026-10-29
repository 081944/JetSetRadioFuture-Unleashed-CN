"""Generate C for the measured CN prototype. Does not compile/package an APK."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def generate(profile, gen):
    profile, gen = Path(profile).resolve(), Path(gen).resolve()
    xbe = profile/'default.xbe'
    expected = json.loads((profile/'manifest.json').read_text(encoding='utf-8'))['target_sha256']
    if hashlib.sha256(xbe.read_bytes()).hexdigest() != expected:
        raise ValueError('Prototype XBE does not match its profile')
    gen.mkdir(parents=True, exist_ok=True)
    previous = {p.name: (hashlib.sha256(p.read_bytes()).digest(), p.stat().st_atime_ns,
                         p.stat().st_mtime_ns)
                for p in gen.iterdir() if p.suffix in ('.c', '.h')}
    stamp = gen/'cn_codegen.sha256'
    stamp.unlink(missing_ok=True)
    commands = [
        ['tools.xbe_parser', str(xbe), '--json', str(profile/'default_analysis.json')],
        ['tools.disasm', str(xbe), '-v', '--extra-sections', '.cnfont',
         '--seed-functions', str(profile/'seeds.json'), '-o', str(profile/'disasm')],
        ['tools.func_id', str(xbe), '--functions', str(profile/'disasm/functions.json'),
         '--strings', str(profile/'disasm/strings.json'), '--xrefs', str(profile/'disasm/xrefs.json'),
         '-o', str(profile/'func_id')],
        ['tools.abi_analysis', str(xbe), '--disasm-dir', str(profile/'disasm'),
         '--func-id-dir', str(profile/'func_id'), '--output-dir', str(profile/'abi')],
        ['tools.recomp', str(xbe), '-v', '--all', '--split', '1000', '--back-edge-yield',
         '-o', str(profile/'recomp'),
         '--exclude-manual', str(ROOT/'port/recomp_manual.c'),
         '--disasm-dir', str(profile/'disasm'), '--func-id-dir', str(profile/'func_id'),
         '--abi-dir', str(profile/'abi'), '--gen-dir', str(gen)],
    ]
    for index, command in enumerate(commands):
        log = profile/f'generate-{index}.log'
        print(f'{command[0]} -> {log}', flush=True)
        with log.open('w', encoding='utf-8') as output:
            subprocess.run([sys.executable, '-u', '-m', *command], cwd=ROOT,
                           stdout=output, stderr=subprocess.STDOUT, check=True)
    # Stamp only after successful generation, checked again by CMake.
    stats = json.loads((profile/'recomp/summary.json').read_text(encoding='utf-8'))
    if stats.get('failed'):
        raise RuntimeError(f"Incomplete translation: {stats['failed']} functions failed")
    if hashlib.sha256(xbe.read_bytes()).hexdigest() != expected:
        raise RuntimeError('Prototype changed during code generation; no build stamp written')
    # A data-only localization change can generate identical C. Keep matching
    # files' timestamps, after regeneration, so Ninja need not rebuild them.
    for name, (digest, atime, mtime) in previous.items():
        output = gen/name
        if output.is_file() and hashlib.sha256(output.read_bytes()).digest() == digest:
            os.utime(output, ns=(atime, mtime))
    stamp.write_text(expected+'\n', encoding='utf-8')
    print(f'Generated CN prototype C in {gen}', flush=True)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--profile', required=True)
    p.add_argument('--gen', required=True)
    a = p.parse_args()
    generate(a.profile, a.gen)
