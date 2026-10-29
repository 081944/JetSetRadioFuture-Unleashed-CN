"""Audit newly introduced Git blobs; inherited upstream content is unchanged.

This is an accidental-file/secret guard, not a copyright or licence verdict.
Both staged content and every new commit can be checked, including deleted
files that remain reachable through history. No working-directory glob upload.
"""
import argparse
from pathlib import PurePosixPath
import re
import subprocess

BASE = '414b1037a4503a167c5440a258ae2ff904d408f5'
TEXT_SUFFIXES = {'.c', '.h', '.java', '.py', '.ps1', '.md', '.json', '.xml', '.txt',
                 '.yml', '.yaml', '.sh', '.cmake'}


def git(*args):
    return subprocess.check_output(['git', *args])


def inspect(name, data):
    path = PurePosixPath(name)
    parts = [part.lower() for part in path.parts]
    if (any(p in {'game_files', 'analysis', 'media'} or p.startswith('build') for p in parts[:-1])
            or parts[:2] == ['port', 'recomp']
            or path.name.lower() in {'cn_profile.h', 'cn_codegen.sha256'}):
        raise ValueError(f'Local/generated path: {name}')
    if path.suffix.lower() not in TEXT_SUFFIXES and path.name not in {'.gitignore', 'LICENSE', 'NOTICE'}:
        raise ValueError(f'Non-source extension requires review: {name}')
    if len(data) > 2_000_000 or b'\0' in data:
        raise ValueError(f'Binary/oversized new blob: {name}')
    data.decode('utf-8-sig')
    if re.search(rb'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----', data):
        raise ValueError(f'Private key: {name}')
    if re.search(rb'gh[pousr]_[A-Za-z0-9]{30,}', data):
        raise ValueError(f'GitHub credential: {name}')


def audit(staged, base):
    blobs = []
    if staged:
        paths = git('diff', '--cached', '--name-only', '--diff-filter=ACMR', '-z').decode('utf-8').split('\0')
        for path in filter(None, paths):
            blobs.append((path, git('show', ':'+path)))
    else:
        # rev-list excludes every object already present in the reviewed base.
        for line in git('rev-list', '--objects', 'HEAD', '--not', base).decode('utf-8').splitlines():
            oid, _, name = line.partition(' ')
            if git('cat-file', '-t', oid).strip() == b'blob':
                blobs.append((name, git('cat-file', 'blob', oid)))
    for name, data in blobs:
        inspect(name, data)
        print(f'OK {len(data):7d} {name}')
    print(f'PASS: {len(blobs)} new source blobs audited; inherited upstream excluded')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--staged', action='store_true')
    parser.add_argument('--base', default=BASE)
    args = parser.parse_args()
    audit(args.staged, args.base)
