"""Differential tests and a device-runnable benchmark of actual renderer helpers."""
import argparse
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
gl = (root / "src/nv2a_gl/nv2a_gl.c").read_text(encoding="utf-8")
vk = (root / "port/vk_game.inc").read_text(encoding="utf-8")
original = subprocess.check_output(
    ["git", "show", "a68db81:src/nv2a_gl/nv2a_gl.c"], cwd=root
).decode("utf-8")


def function(text, signature):
    start = text.index(signature)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end] + "\n"


source = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "nv2a_backend.h"
typedef uint64_t VkPipeline;
#define VK_NULL_HANDLE 0
#define VG_PIPES 8192
#define VG_PIPE_BUCKETS 4096
#define VG_PROG 64
static int full_hash,enabled_calls,g_on=-1;
static const char *enabled_value;
static struct { uint32_t prog[NV2A_VSH_MAX_INSNS*4]; } S;
static uint32_t g_vsh_gen=1;
static const char *jsrf_getenv(const char *name) {
    if(!strcmp(name,"RECOMP_GL")){enabled_calls++;return enabled_value;}
    return full_hash?"1":NULL;
}
'''
end = vk.index("} VgPipeline;") + len("} VgPipeline;")
source += vk[vk.rfind("typedef struct {", 0, end):end] + "\n"
source += r'''
static VgPipeline vg_pipes[VG_PIPES];
static uint32_t vg_pipe_head[VG_PIPE_BUCKETS],vg_pipe_next[VG_PIPES],vg_npipe,vg_current_prog;
static struct { void *uniforms; int vs,fs; } vg_prog[VG_PROG];
static int w_dev,freed;
static void vg_drain(void){}
static void p_pipe_free(int dev,VkPipeline pipe,void *unused){(void)dev;(void)unused;assert(pipe);freed++;}
static void p_shader_free(int dev,int shader,void *unused){(void)dev;(void)shader;(void)unused;}
'''
for signature in ["static uint32_t vg_pipeline_bucket(", "static VkPipeline vg_pipeline_find(",
                  "static void vg_pipeline_remember(", "static void vg_prog_free("]:
    source += function(vk, signature)
source += function(gl, "static inline int gl_enabled(")
source += function(gl, "static inline void vsh_word_store(")
source += function(gl, "int nv2a_gl_enabled(")
source += function(gl, "static uint32_t tex_sample(")
source += function(original, "static uint32_t tex_sample(").replace("tex_sample(", "tex_sample_reference(", 1)
source += r'''
static uint32_t rng=1234567;
static uint32_t random32(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
static VgPipeline keys[VG_PIPES];
static void reset(void){
    memset(vg_pipes,0,sizeof vg_pipes);memset(vg_pipe_head,0,sizeof vg_pipe_head);
    memset(vg_pipe_next,0,sizeof vg_pipe_next);vg_npipe=0;
}
static VgPipeline key_for(uint32_t i){
    VgPipeline k;memset(&k,0,sizeof k);
    for(size_t n=0;n<offsetof(VgPipeline,pipeline);n++)((uint8_t *)&k)[n]=(uint8_t)random32();
    k.prog=i%31+1;k.prim=i%7+1;k.stride=16;k.attr[0]=i;k.pipeline=i+1;return k;
}
__attribute__((noinline)) static VkPipeline linear(const VgPipeline *k){
    for(uint32_t i=0;i<vg_npipe;i++)if(vg_pipes[i].pipeline && !memcmp(&vg_pipes[i],k,offsetof(VgPipeline,pipeline)))return vg_pipes[i].pipeline;
    return 0;
}
__attribute__((noinline)) static VkPipeline indexed(const VgPipeline *k){return vg_pipeline_find(k,vg_pipeline_bucket(k));}
static double seconds(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
static volatile uint64_t sink;
static void pipeline_test(void){
    reset();
    for(uint32_t i=0;i<VG_PIPES;i++){keys[i]=key_for(i);vg_pipeline_remember(&keys[i],vg_pipeline_bucket(&keys[i]));}
    for(uint32_t i=0;i<VG_PIPES;i++)assert(indexed(&keys[i])==keys[i].pipeline);
    /* Real program destruction invalidates every matching handle, including
       chains with collisions. Reuse the same program ID afterwards. */
    vg_prog[7].uniforms=malloc(1);vg_current_prog=7;vg_prog_free(7);
    assert(!vg_current_prog && freed>0);
    for(uint32_t i=0;i<VG_PIPES;i++)assert(indexed(&keys[i])==linear(&keys[i]));
    reset();
    for(uint32_t i=0;i<32;i++){keys[i]=key_for(i);vg_pipeline_remember(&keys[i],vg_pipeline_bucket(&keys[i]));}
    vg_prog[7].uniforms=malloc(1);vg_current_prog=7;vg_prog_free(7);
    VgPipeline reused=keys[6];reused.pipeline=99999;
    vg_pipeline_remember(&reused,vg_pipeline_bucket(&reused));assert(indexed(&reused)==99999);
    /* Alter every key byte independently: collisions must still compare the
       whole original key, including float bit patterns and padding. */
    for(size_t n=0;n<offsetof(VgPipeline,pipeline);n++){
        VgPipeline changed=reused;((uint8_t *)&changed)[n]^=1;
        assert(indexed(&changed)==linear(&changed));
    }
    reset();assert(!indexed(&reused));
    puts("PASS: 8192 pipeline keys, collisions, deletion, program-ID reuse, exact key comparison, reset");
}
static void texture_test(void){
    const uint32_t sizes[]={0,1,2,3,63,64,65,127,128,129,255,256,257,4095,4096,4097,8191,8192,8193,65535,65536,1048577};
    uint8_t *memory=malloc(1048584);assert(memory);
    for(uint32_t i=0;i<1048584;i++)memory[i]=(uint8_t)random32();
    for(int offset=0;offset<4;offset++)for(unsigned j=0;j<sizeof sizes/sizeof *sizes;j++){
        uint8_t *p=memory+offset;uint32_t size=sizes[j];
        for(int bias=0;bias<2;bias++){
            uint32_t color=random32();
            assert(tex_sample(color,p,size,bias)==tex_sample_reference(color,p,size,bias));
            if(size){p[size-1]^=0xff;assert(tex_sample(color,p,size,bias)==tex_sample_reference(color,p,size,bias));p[size-1]^=0xff;}
        }
    }
    for(int i=0;i<1000;i++){
        uint32_t size=random32()%10000,color=random32();int bias=i&1;
        assert(tex_sample(color,memory,size,bias)==tex_sample_reference(color,memory,size,bias));
    }
    free(memory);puts("PASS: texture hashes match original, boundaries/unaligned data/edits/bias/full-content mode");
}
static void benchmark(void){
    reset();for(uint32_t i=0;i<285;i++){keys[i]=key_for(i);vg_pipeline_remember(&keys[i],vg_pipeline_bucket(&keys[i]));}
    const uint32_t loops=400000;double a=seconds();
    for(uint32_t i=0;i<loops;i++)sink+=linear(&keys[(i*37u)%285]);double b=seconds();
    for(uint32_t i=0;i<loops;i++)sink+=indexed(&keys[(i*37u)%285]);double c=seconds();
    printf("BENCH pipeline 285 keys: linear %.3f ms, indexed %.3f ms\n",(b-a)*1000,(c-b)*1000);
    uint8_t *p=malloc(1048576);for(uint32_t i=0;i<1048576;i++)p[i]=(uint8_t)i;
    a=seconds();for(uint32_t i=0;i<loops;i++)sink+=tex_sample_reference(i,p+(i&3),65536,0);b=seconds();
    for(uint32_t i=0;i<loops;i++)sink+=tex_sample(i,p+(i&3),65536,0);c=seconds();
    printf("BENCH texture: original %.3f ms, grouped %.3f ms\n",(b-a)*1000,(c-b)*1000);free(p);
}
int main(int argc,char **argv){
    full_hash=argc>1&&!strcmp(argv[1],"full");pipeline_test();texture_test();
    assert(gl_enabled()==0 && enabled_calls==1);enabled_value="0";
    assert(nv2a_gl_enabled()==0 && enabled_calls==1);g_on=-1;
    assert(gl_enabled()==1 && nv2a_gl_enabled()==1 && enabled_calls==2);
    puts("PASS: enabled-state cache and public/internal entry points preserve existing semantics");
    for(uint32_t i=0;i<NV2A_VSH_MAX_INSNS*4;i++){
        uint32_t before=g_vsh_gen;vsh_word_store(i,0);assert(g_vsh_gen==before);
        uint32_t value=random32()|1;vsh_word_store(i,value);assert(S.prog[i]==value && g_vsh_gen==before+1);
        vsh_word_store(i,value);assert(g_vsh_gen==before+1);
        vsh_word_store(i,value^1);assert(S.prog[i]==(value^1) && g_vsh_gen==before+2);
    }
    puts("PASS: identical vertex microcode preserves the memo, every changed word invalidates it");
    if(argc>1&&!strcmp(argv[1],"bench"))benchmark();return 0;
}
'''
parser = argparse.ArgumentParser()
parser.add_argument("--emit-c", type=Path)
args = parser.parse_args()
if args.emit_c:
    args.emit_c.write_text(source, encoding="utf-8")
else:
    with tempfile.TemporaryDirectory(prefix="jsrf-hotpaths-") as folder:
        c = Path(folder) / "test.c"
        exe = Path(folder) / "test.exe"
        c.write_text(source, encoding="utf-8")
        subprocess.run(["gcc", "-std=c11", "-O2", "-I", str(root / "src/nv2a_gl"), str(c), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
        subprocess.run([str(exe), "full"], check=True)
