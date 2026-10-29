"""Compile the actual generated glyph C and compare it with original CN x86.

Local integration test: requires GCC and Unicorn, no game/window/device.
"""
import argparse
from pathlib import Path
import struct
import subprocess
import re

from tools.cn_compat.prepare import ROOT, offset
from tools.recomp import config
from tools.recomp.translator import FunctionTranslator
from tools.cn_compat.test_local import Prototype


def verify(profile, generated_dir=None):
    profile = Path(profile).resolve()
    xbe = profile/'default.xbe'
    config.configure_from_xbe(str(xbe))
    bounds = [(0x289000, 0x289052), (0x2C010, 0x2C032), (0x289780, 0x2897A5),
              (0x289700, 0x28971F), (0x289800, 0x28982F)]
    db = {s:dict(_addr=s, start=hex(s), end=e, size=e-s,
                 section='.cnfont' if s>0x280000 else '.text') for s,e in bounds}
    translator = FunctionTranslator(xbe.read_bytes(), db)
    code = '\n'.join(translator.translate_function(s, info) for s,info in db.items())
    if generated_dir:
        # Exercise exactly the function bodies that go into libmain.so,
        # including ABI analysis and cooperative scheduling emissions.
        found = {}
        for chunk in Path(generated_dir).glob('recomp_*.c'):
            text = chunk.read_text(encoding='utf-8')
            for address in db:
                pattern = rf'^void sub_{address:08X}\(void\)\n\{{.*?^\}}'
                match = re.search(pattern, text, re.M | re.S)
                if match:
                    found[address] = match.group(0)
        if set(found) != set(db):
            raise RuntimeError(f'Generated patch bodies missing: {set(db)-set(found)}')
        code = '\n'.join(found[s] for s in db)
    if not code or 'TODO' in code:
        raise RuntimeError('Glyph translation incomplete')
    harness = r'''
#define RECOMP_GENERATED_CODE 1
#include "recomp_types.h"
#include <stdio.h>
#include <stdlib.h>
ptrdiff_t g_xbox_mem_offset;
RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
RECOMP_TLS uint32_t g_ebp, g_seh_ebp;
int32_t g_xbox_guest_cpu_preempt;
void xbox_GuestCpuPreempt(uint32_t address) { (void)address; abort(); }
void sub_00289780(void);
static int resource_error;
void sub_0002C0EE(void) {}
void sub_0002C253(void) {}
void sub_0006E1C0(void) {
    if(g_ecx!=0x252790 || MEM32(g_esp+4)!=0x470000) resource_error=1;
    g_eax=0x467890; g_ecx=123; g_edx=456; g_esp+=8;
}
void sub_0006E7B0(void) {
    if(g_ecx!=0x252790 || MEM32(g_esp+4)!=0x467890) resource_error=1;
    g_eax=789; g_ecx=123; g_edx=456; g_esp+=8;
}
uint32_t recomp_mmio_read(uint32_t address, unsigned size) {
    (void)address; (void)size; abort(); /* glyph lookup must never touch MMIO */
}
void recomp_mmio_write(uint32_t address, uint32_t value, unsigned size) {
    (void)address; (void)value; (void)size; abort();
}
'''+code+r'''
int main(int argc, char **argv) {
    unsigned char *mem = calloc(1, 0x700000);
    unsigned char header[4096];
    FILE *in, *out;
    uint32_t table, count, i, codepoint;
    if (argc != 3 || !mem) return 2;
    in = fopen(argv[1], "rb"); out = fopen(argv[2], "wb");
    if (!in || !out || fread(header, 1, 4096, in) != 4096) return 2;
    memcpy(&count, header+0x11c, 4); memcpy(&table, header+0x120, 4);
    table -= 0x10000;
    for (i=0;i<count;i++) {
        uint32_t va, raw, size;
        memcpy(&va, header+table+i*56+4, 4);
        memcpy(&raw, header+table+i*56+12, 4);
        memcpy(&size, header+table+i*56+16, 4);
        if (fseek(in,raw,SEEK_SET) || fread(mem+va,1,size,in)!=size) return 2;
    }
    g_xbox_mem_offset = (ptrdiff_t)mem;
    for (codepoint=0;codepoint<65536;codepoint++) {
        g_esp = 0x600000; MEM32(g_esp+4) = codepoint;
        sub_00289000();
        if (g_esp != 0x600008 || fwrite(&g_eax,4,1,out)!=1) return 3;
    }
    MEM32(0x4515bc) = 1; MEM32(0x289f00) = 0x467890;
    for(i=0;i<6;i++) MEM32(0x451598+i*4) = 100+i;
    for(i=0;i<8;i++) {
        uint32_t want = i<6?100+i:i==6?0x467890:0;
        g_ecx=0x450000; g_esp=0x600000; MEM32(g_esp+4)=i;
        sub_0002C010();
        if(g_eax!=want || g_esp!=0x600010) return 4;
    }
    MEM32(0x4515bc)=0; g_ecx=0x450000; g_esp=0x600000;
    MEM32(g_esp+4)=6; sub_0002C010();
    if(g_eax || g_esp!=0x600010) return 5;
    MEM32(0x460000)=0x470000; MEM32(0x4515b8)=0x490000;
    g_edi=0x460000; g_esi=0x450000; g_esp=0x600000;
    g_eax=111; g_ecx=222; g_edx=333; g_ebx=444;
    g_seh_ebp=0x12345678;
    sub_00289700();
    if(resource_error || MEM32(0x289f00)!=0x467890 || g_esp!=0x600000 ||
       g_eax!=111 || g_ecx!=222 || g_edx!=0x490000 || g_ebx!=444 ||
       g_seh_ebp!=0x12345678) return 6;
    sub_00289800();
    if(resource_error || MEM32(0x289f00) || MEM32(0x4515bc) ||
       g_esp!=0x600000 || g_eax!=111 || g_ecx!=222 || g_edx!=0x490000 ||
       g_ebx!=444 || g_seh_ebp!=0x12345678) return 7;
    fclose(in); fclose(out); free(mem); return 0;
}
'''
    (profile/'test_codegen.c').write_text(harness, encoding='utf-8')
    exe = profile/'test_codegen.exe'
    subprocess.run(['gcc', '-std=c11', '-O2', '-I'+str(ROOT/'templates/runtime'),
                    str(profile/'test_codegen.c'), '-o', str(exe)], check=True)
    results = profile/'glyph_results.bin'
    subprocess.run([str(exe), str(xbe), str(results)], check=True)
    actual = struct.unpack('<65536I', results.read_bytes())
    Prototype.setUpClass()
    test = Prototype()
    original = test.machine(test.cn)
    codes = set()
    for i in range(0x486):
        codes.add(struct.unpack_from('<H', test.cn, offset(0x1F7088)+i*4)[0])
    for i in range(0x183):
        codes.add(struct.unpack_from('<H', test.cn, offset(0x289080)+i*4)[0])
    for code in codes | {0xFFFF, 0x1234}:
        expected = test.call(original, 0x3C830, code)
        if actual[code] != expected:
            raise AssertionError(f'C/x86 mismatch for {code:#x}: {actual[code]} != {expected}')
    for code in set(range(65536))-codes:
        if actual[code] != 0xFFFFFFFF:
            raise AssertionError(f'C falsely recognized {code:#x}')
    print(f'Generated native C agrees with CN x86 for {len(codes)} mapped codes; '
          'all 65536 inputs, texture indices 0-7, resource load/free, readiness and stack cleanup checked.')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--profile', required=True)
    p.add_argument('--generated-dir')
    a = p.parse_args()
    verify(a.profile, a.generated_dir)
