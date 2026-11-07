"""Compile the actual guest bridge with clobbering callees to check its ABI."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/logs/character-switch'
text = (ROOT / 'port/recomp_manual.c').read_text(encoding='utf-8')
start = text.index('static int character_guest_call(')
bridge = text[start:text.index('\n#endif', start)]
OUT.mkdir(parents=True, exist_ok=True)
source = OUT / 'test-character-guest-call.c'
source.write_text(r'''
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "recomp_types.h"
ptrdiff_t g_xbox_mem_offset;
uint32_t g_xbox_code_lo,g_xbox_code_hi;
uint32_t recomp_mmio_read(uint32_t address,unsigned size) {
    (void)address;(void)size;assert(!"unexpected MMIO read in guest bridge test");return 0;
}
RECOMP_TLS uint32_t g_eax,g_ecx,g_edx,g_ebx,g_esp,g_ebp,g_esi,g_edi,g_seh_ebp;
RECOMP_TLS uint32_t g_eflags,g_mxcsr;
RECOMP_TLS int g_fp_top,g_df,g_fp_cmp;
RECOMP_TLS uint16_t g_fp_control_word,g_fp_cc;
RECOMP_TLS double g_fp_stack[8];
RECOMP_TLS RecompXmm g_xmm0,g_xmm1,g_xmm2,g_xmm3,g_xmm4,g_xmm5,g_xmm6,g_xmm7;
static int bad_cleanup,calls;
static void clobber(unsigned n,int thiscall) {
    assert(MEM32(g_esp)==0);
    if (thiscall) assert(g_ecx==12);
    if (n) assert(MEM32(g_esp+4)==(thiscall ? 99 : 12));
    if (!thiscall && n==2) assert(MEM32(g_esp+8)==99);
    calls++;
    g_eax=12345; g_ecx=g_edx=g_ebx=g_ebp=g_esi=g_edi=g_seh_ebp=0;
    g_eflags=g_mxcsr=0; g_fp_top=g_df=g_fp_cmp=0;
    g_fp_control_word=g_fp_cc=0; memset(g_fp_stack,0,sizeof g_fp_stack);
    memset(&g_xmm0,0,sizeof g_xmm0); memset(&g_xmm7,0,sizeof g_xmm7);
    g_esp+=(bad_cleanup ? 8 : 4)+(thiscall ? n*4 : 0);
}
void sub_000256A0(void) { clobber(2,0); }
void sub_00013FC0(void) { clobber(1,0); }
void sub_00011B90(void) { clobber(1,1); }
void sub_000A6AF0(void) { clobber(1,1); }
void sub_000A8370(void) { clobber(0,1); }
void sub_0006F890(void) {
    assert(g_ecx==12 && MEM32(g_esp+4)==99 && MEM32(g_esp+8)==100 && MEM32(g_esp+12)==101);
    clobber(3,1);
}
''' + bridge + r'''
static void prepare(void) {
    g_eax=1;g_ecx=2;g_edx=3;g_ebx=4;g_esp=0x20000;g_ebp=6;g_esi=7;g_edi=8;g_seh_ebp=9;
    g_eflags=0x202;g_mxcsr=0x1F80;g_fp_top=3;g_df=1;g_fp_cmp=2;
    g_fp_control_word=0x27F;g_fp_cc=0x4000;
    for (int i=0;i<8;i++) g_fp_stack[i]=i+0.5;
    memset(&g_xmm0,0x17,sizeof g_xmm0);memset(&g_xmm7,0x71,sizeof g_xmm7);
}
static void restored(void) {
    assert(g_eax==1 && g_ecx==2 && g_edx==3 && g_ebx==4 && g_esp==0x20000);
    assert(g_ebp==6 && g_esi==7 && g_edi==8 && g_seh_ebp==9);
    assert(g_eflags==0x202 && g_mxcsr==0x1F80 && g_fp_top==3 && g_df==1 && g_fp_cmp==2);
    assert(g_fp_control_word==0x27F && g_fp_cc==0x4000);
    for (int i=0;i<8;i++) assert(g_fp_stack[i]==i+0.5);
    for (unsigned i=0;i<sizeof g_xmm0;i++) assert(((unsigned char *)&g_xmm0)[i]==0x17);
    for (unsigned i=0;i<sizeof g_xmm7;i++) assert(((unsigned char *)&g_xmm7)[i]==0x71);
}
int main(void) {
    void *memory=calloc(1,0x40000);assert(memory);g_xbox_mem_offset=(ptrdiff_t)memory;
    uint32_t args[]={12,99,100,101},result=0;
    prepare();assert(character_guest_call(0x256A0,2,args,&result));assert(result==12345);restored();
    prepare();assert(character_guest_call(0x13FC0,1,args,&result));restored();
    prepare();assert(character_guest_call(0x11B90,2,args,&result));restored();
    prepare();assert(character_guest_call(0xA6AF0,2,args,&result));restored();
    prepare();assert(character_guest_call(0xA8370,1,args,&result));restored();
    prepare();assert(character_guest_call(0x6F890,4,args,&result));restored();
    bad_cleanup=1;
    prepare();assert(!character_guest_call(0x13FC0,1,args,&result));restored();
    assert(!character_guest_call(0xDEAD,1,args,&result) && calls==7);
    assert(!character_guest_call(0x256A0,3,args,&result) && calls==7);
    free(memory);puts("PASS: guest registers, FP/SSE, cdecl stack, bad cleanup and unknown entries");
}
''', encoding='utf-8')
binary = OUT / 'test-character-guest-call.exe'
subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-I', str(ROOT / 'port/recomp/gen'), str(source), '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True)
