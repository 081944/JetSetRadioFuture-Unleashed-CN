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
TEXT_SUFFIXES = {'.c', '.h', '.inc', '.java', '.py', '.ps1', '.md', '.json', '.xml', '.txt',
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
        # Exclude actual inherited upstream objects as promised, including
        # upstream merges after the initial CN audit baseline. Only exclude
        # the shared ancestor, never unmerged upstream or local CN changes.
        excludes = [base]
        try:
            inherited = git('merge-base', 'HEAD', 'refs/remotes/upstream/main').decode().strip()
            if inherited:
                excludes.append(inherited)
        except subprocess.CalledProcessError:
            pass  # A checkout without the upstream remote still audits fully.
        objects = [line.partition(' ') for line in
                   git('rev-list', '--objects', 'HEAD', '--not', *excludes).decode('utf-8').splitlines()]
        # A single batch avoids spawning Git twice per object on Windows.
        request = ''.join(oid+'\n' for oid, _, name in objects).encode()
        info = subprocess.check_output(['git', 'cat-file', '--batch-check'], input=request).splitlines()
        wanted = []
        for (oid, _, name), header in zip(objects, info):
            _, kind, size = header.split()
            if kind == b'blob':
                if int(size) > 2_000_000:
                    raise ValueError(f'Oversized new blob: {name}')
                wanted.append((oid, name))
        if wanted:
            payload = subprocess.check_output(['git', 'cat-file', '--batch'],
                input=''.join(oid+'\n' for oid, name in wanted).encode())
            cursor = 0
            for oid, name in wanted:
                end = payload.index(b'\n', cursor)
                actual, kind, size = payload[cursor:end].split()
                if actual.decode() != oid or kind != b'blob':
                    raise ValueError(f'Unexpected batch object: {name}')
                cursor = end+1
                size = int(size)
                blobs.append((name, payload[cursor:cursor+size]))
                cursor += size+1
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
