"""Prepare an experimental US-layout Chinese font port from two owned XBEs.

Outputs are local recompiler inputs, NOT a runnable disc or a release package.
Only the two measured inputs are supported. No game bytes live in this script.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.xiso.xdvdfs import Xiso

US_SHA = 'bb2410618c35ccab1ab8ad989194bbd50619eeb648a03b21483efca57f36547d'
CN_SHA = '9ff97d993c826ad47f94d540bb0cbe1205f390eb35e6184c370b8cd8cdfb97e9'
BASE = 0x10000


def read_xbe(path):
    path = Path(path)
    with path.open('rb') as f:
        if f.read(4) == b'XBEH':
            return path.read_bytes()
    with Xiso(str(path)) as iso:
        entry = iso.find('default.xbe')
        if entry is None:
            raise ValueError('No default.xbe in image')
        return iso.read(entry)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def rd(data, off):
    return struct.unpack_from('<I', data, off)[0]


def wr(data, off, value):
    struct.pack_into('<I', data, off, value)


def offset(va):
    if 0x11000 <= va < 0x18CB30:
        return va - 0x10000
    if 0x1C3F60 <= va < 0x1EB750:
        return va - 0x1C3F60 + 0x1B4000
    if 0x1EB760 <= va < 0x22FCB4:
        return va - 0x1EB760 + 0x1DC000
    if 0x289000 <= va < 0x28A000:
        return va - 0x289000 + 0x22D000
    raise ValueError(f'Unmapped prototype address {va:#x}')


def localized_slots(us, cn):
    """Audited same-address literals, not JP pointer tables or global data.

    This address-only allowlist is tied to the two hashes checked by hybrid.
    Preserve raw custom-glyph encoding and never expand a US string slot.
    """
    addresses = json.loads((Path(__file__).with_name('localization_slots.json'))
                           .read_text(encoding='utf-8'))
    previous_end = 0
    for address in addresses:
        va = int(address, 16)
        start = offset(va)
        old_end, new_end = us.index(0, start), cn.index(0, start)
        end = (old_end + 4) & ~3
        if start < previous_end or new_end >= end:
            raise ValueError(f'Overlapping or oversized localized string at {va:#x}')
        if us[start-1] or cn[start-1] or any(us[old_end:end]):
            raise ValueError(f'Unexpected string boundary at {va:#x}')
        # Both builds refer to the same literal VA, despite shifted tables.
        pointer = struct.pack('<I', va)
        if pointer not in us or pointer not in cn:
            raise ValueError(f'Missing string reference at {va:#x}')
        us[start:old_end].decode('cp932')
        cn[start:new_end].decode('cp932')
        yield start, end, cn[start:new_end] + bytes(end-new_end)
        previous_end = end


def hybrid(us, cn):
    if sha(us) != US_SHA or sha(cn) != CN_SHA:
        raise ValueError('Unsupported XBE: expected the measured US and Chinese versions')
    out = bytearray(us + cn[0x22D000:0x22E000])
    # Five measured patch sites. Preserve all other US machine code/data.
    for va, length in [(0x2C01A, 7), (0x2C0E8, 6), (0x2C249, 10),
                       (0x3C830, 6), (0x3CA35, 32)]:
        o = offset(va)
        out[o:o+length] = cn[o:o+length]
    # The CN thunk uses the JP resource manager. Only these two immediates
    # need rebasing when transplanted onto the US layout.
    for va in [0x289705, 0x28980C]:
        o = offset(va)
        if rd(out, o) != 0x252778:
            raise ValueError(f'Unexpected resource manager operand at {va:#x}')
        wr(out, o, 0x252790)
    # The Xbox patch jumps back into a JAE that consumes flags set in this
    # thunk. The C lifter stores flags per function, so that tail edge would
    # lose the comparison. Finish the small getter locally instead. Same x86
    # results/stack effect, and no dependency on flags across C functions.
    getter_tail = bytes.fromhex('83f806730a8b848198150000c20c0031c0c20c00')
    o = offset(0x289791)
    out[o:o+len(getter_tail)] = getter_tail
    # Original lookup table was also localized, not just the extra mapping.
    lo, hi = offset(0x1F7088), offset(0x1F8B8C)
    out[lo:hi] = cn[lo:hi]
    wr(out, offset(0x1FC724), 0)  # GAME resource language, not XC_LANGUAGE
    for start, end, literal in localized_slots(us, cn):
        out[start:end] = literal

    # US header's spare space is too small for another in-place descriptor:
    # relocate the complete table into measured zero padding instead.
    if any(us[0xA00:0x1000]):
        raise ValueError('Expected empty US header padding')
    old = rd(us, 0x120) - BASE
    out[0xA00:0xA00 + 11*56] = us[old:old + 11*56]
    cn_table = rd(cn, 0x120) - BASE
    desc = bytearray(cn[cn_table+11*56:cn_table+12*56])
    wr(desc, 20, BASE + 0xCE0)
    out[0xA00+11*56:0xA00+12*56] = desc
    out[0xCE0:0xCE8] = b'.cnfont\0'
    wr(out, 0x108, 0x1000)
    wr(out, 0x10C, 0x28A000 - BASE)
    wr(out, 0x11C, 12)
    wr(out, 0x120, BASE + 0xA00)
    # XBE section digests describe the new local recompiler input correctly.
    for i in range(12):
        s = 0xA00 + i*56
        raw, size = rd(out, s+12), rd(out, s+16)
        out[s+36:s+56] = hashlib.sha1(out[raw:raw+size]).digest()
    return bytes(out)


def fnv(data):
    value = 0xCBF29CE484222325
    for b in data:
        value = ((value ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def delta(source, target):
    if len(source) != len(target):
        raise ValueError('Profile expects equal-length buffers')
    runs = []
    start = end = None
    for i, (a, b) in enumerate(zip(source, target)):
        if a == b:
            continue
        if start is not None and i - end > 8:
            runs.append((start, target[start:end]))
            start = None
        if start is None:
            start = i
        end = i + 1
    if start is not None:
        runs.append((start, target[start:end]))
    return runs


def array(name, data):
    body = '\n'.join('    '+','.join(f'0x{x:02x}' for x in data[i:i+24])+','
                     for i in range(0, len(data), 24))
    return f'static const unsigned char {name}[] = {{\n{body}\n}};\n'


def prepare(us, cn, dest):
    target = hybrid(us, cn)
    dest = Path(dest)
    dest.mkdir(parents=True, exist_ok=True)
    runs = delta(cn, target)
    blob = b''.join(data for _, data in runs)
    hdr = '/* GENERATED FROM OWNED GAME FILES. Keep local; do not commit. */\n'
    hdr += f'#define JSRF_CN_SIZE {len(cn)}u\n'
    hdr += f'#define JSRF_CN_SOURCE_FNV UINT64_C(0x{fnv(cn):016x})\n'
    hdr += f'#define JSRF_CN_TARGET_FNV UINT64_C(0x{fnv(target):016x})\n'
    hdr += array('cn_source_head', cn[:0x1000])
    hdr += array('cn_target_head', target[:0x1000])
    hdr += array('cn_patch_bytes', blob)
    hdr += 'static const struct { uint32_t off, size; } cn_patch_runs[] = {\n'
    hdr += ''.join(f'    {{{off}u, {len(data)}u}},\n' for off, data in runs)
    hdr += '};\n'
    (dest/'cn_profile.h').write_text(hdr, encoding='utf-8')
    (dest/'default.xbe').write_bytes(target)
    seeds = [0x2C010, 0x2C040, 0x2C1D0, 0x3C830, 0x3CA20, 0x289000]
    (dest/'seeds.json').write_text(json.dumps([{'start':f'0x{x:08X}'} for x in seeds]), encoding='utf-8')
    manifest = dict(source_sha256=sha(cn), target_sha256=sha(target),
                    patch_runs=len(runs), patch_bytes=len(blob),
                    localized_strings=409,
                    stage='experimental-font-and-text-port; device validation pending')
    (dest/'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--us', required=True)
    parser.add_argument('--cn', required=True)
    parser.add_argument('--out', required=True)
    args = parser.parse_args()
    output_xbe = (Path(args.out)/'default.xbe').resolve()
    if output_xbe in {Path(args.us).resolve(), Path(args.cn).resolve()}:
        parser.error('Output must not overwrite either input XBE')
    print(json.dumps(prepare(read_xbe(args.us), read_xbe(args.cn), args.out), indent=2))
