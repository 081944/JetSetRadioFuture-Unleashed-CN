"""Check std140 bytes and cache invalidation using the actual Vulkan setters."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
text = (root / "port/vk_game.inc").read_text(encoding="utf-8")


def extract(signature):
    start = text.index(signature)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


source = r'''
#include "nv2a_backend.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define VG_PROG 2
static struct { uint8_t *uniforms; uint32_t revision; } vg_prog[VG_PROG];
static uint32_t vg_current_prog=1,vg_uniform_offset[NV2A_U_COUNT],vg_uniform_stride[NV2A_U_COUNT];
static uint32_t vg_uniform_bytes,vg_attr_offset;
static int vg_uniform_dedup;
static uint64_t vg_uniform_calls,vg_uniform_unchanged;
'''
for name in ["static void vg_uniform_layout(void)", "static void vg_uniform_f(",
             "static void vg_uniform_i(", "static int vg_uniform_f_part("]:
    source += extract(name) + "\n"
source += r'''
int main(void) {
    vg_uniform_layout();
    uint8_t *u=calloc(1,vg_uniform_bytes+16); vg_prog[1].uniforms=u;
    float data[1024]; uint32_t *bits=(uint32_t *)data;
    for(int i=0;i<1024;i++) bits[i]=0x3f000000u+i; /* includes exact bit tests below */
    bits[0]=0x7fc00123u;bits[1]=0x80000000u;
    for(int mode=0;mode<2;mode++) {
        vg_uniform_dedup=mode;
        for(int id=0;id<NV2A_U_COUNT;id++) {
            const Nv2aUniformDesc *d=&nv2a_uniform_desc[id];
            if(d->type==NV2A_UT_SAMPLER) continue;
            memset(u,0x5a,vg_uniform_bytes+16);
            uint32_t revision=vg_prog[1].revision;
            if(d->type==NV2A_UT_INT) vg_uniform_i(id,0x12345678);
            else vg_uniform_f(id,data,d->count);
            assert(vg_prog[1].revision==revision+1);
            static const int sizes[]={1,2,3,4,1,4};
            for(int i=0;i<d->count;i++) {
                uint8_t *dst=u+vg_uniform_offset[id]+i*vg_uniform_stride[id];
                if(d->type==NV2A_UT_INT) {assert(!memcmp(dst,"\x78\x56\x34\x12",4));break;}
                const float *v=data+i*sizes[d->type];
                if(d->type==NV2A_UT_MAT2) {
                    assert(!memcmp(dst,v,8));assert(!memcmp(dst+16,v+2,8));
                    assert(dst[8]==0x5a && dst[24]==0x5a);
                } else assert(!memcmp(dst,v,sizes[d->type]*4));
            }
            revision=vg_prog[1].revision;
            if(d->type==NV2A_UT_INT) vg_uniform_i(id,0x12345678);
            else vg_uniform_f(id,data,d->count);
            assert(vg_prog[1].revision==revision+(mode?0:1));
            assert(u[vg_uniform_bytes]==0x5a);
        }
    }
    memset(u,0,vg_uniform_bytes);vg_uniform_dedup=1;
    assert(vg_uniform_f_part(NV2A_U_C,3,data,2));
    assert(!memcmp(u+vg_uniform_offset[NV2A_U_C]+48,data,32));
    assert(u[vg_uniform_offset[NV2A_U_C]+47]==0 && u[vg_uniform_offset[NV2A_U_C]+80]==0);
    uint32_t revision=vg_prog[1].revision;
    assert(vg_uniform_f_part(NV2A_U_C,3,data,2));assert(vg_prog[1].revision==revision);
    bits[0]^=1;vg_uniform_f_part(NV2A_U_C,3,data,2);assert(vg_prog[1].revision==revision+1);
    free(u);puts("PASS: all uniform descriptors, std140 padding, exact NaN/negative-zero bytes, partial updates and cache invalidation");
}
'''
with tempfile.TemporaryDirectory(prefix="jsrf-vk-uniform-") as folder:
    c = Path(folder) / "test.c"
    exe = Path(folder) / "test.exe"
    c.write_text(source, encoding="utf-8")
    subprocess.run(["gcc", "-std=c11", "-O2", "-I", str(root / "src/nv2a_gl"),
                    str(c), str(root / "src/nv2a_gl/nv2a_uniforms.c"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
