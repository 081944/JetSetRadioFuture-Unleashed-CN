"""Differential check of the manual KickOff against the lifted Xbox code.

Compile the actual two function bodies with a small guest-memory fixture.
The reference flush is acknowledged on its polling back edge. Compare all
fixture memory and guest registers, including the optional idle-wait branch.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(path, name):
    source = path.read_text(encoding="utf-8")
    start = source.index(f"void {name}(void)\n{{")
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


REFERENCE = function(ROOT / "port/recomp/gen/recomp_0008.c", "sub_001912A0_gen")
MANUAL = function(ROOT / "port/recomp_manual.c", "sub_001912A0")
HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t mem[0x200000 / 4], regs[0x110000 / 4];
static uint32_t expected_mem[0x200000 / 4], expected_regs[0x110000 / 4];
static uint32_t g_eax, g_ecx, g_edx, g_esp;
static unsigned idle_calls;
static int mode;
static uint32_t *ptr(uint32_t a) {
    if (a >= 0xFD000000u && a < 0xFD110000u) return regs + (a-0xFD000000u)/4;
    if (a < sizeof mem) return mem + a/4;
    fprintf(stderr,"bad fixture address: %08x\n",a); abort();
}
#define XBOX_PTR(a) ptr(a)
#define MEM32(a) (*ptr((uint32_t)(a)))
#define MEMW32(a,v) (MEM32(a)=(v))
#define PUSH32(sp,v) do { uint32_t pv=(v); (sp)-=4; MEM32(sp)=pv; } while(0)
#define POP32(sp,v) do { (v)=MEM32(sp); (sp)+=4; } while(0)
#define HI8(v) (((v)>>8)&255)
#define LO8(v) ((v)&255)
#define TEST_NZ(a,b) (((a)&(b))!=0)
#define TEST_Z(a,b) (((a)&(b))==0)
#define CMP_EQ(a,b) ((a)==(b))
#define CMP_NE(a,b) ((a)!=(b))
#define RECOMP_BACK_EDGE(va) do { if ((va)==0x1912EA) MEM32(0xFD100410u)&=~0x10000u; } while(0)
#define RECOMP_ABI_CALL(va,fn) fn()
static const char *jsrf_getenv(const char *name) { (void)name; return mode?"0":"1"; }
static void xbox_nv2a_kick(void) {}
static void xbox_nv2a_hurry(int on) { (void)on; }
static void xbox_nv2a_flush_wait(uint32_t base) { MEM32(base+0x100410u)&=~0x10000u; }
static void sub_0018E120(void) {
    if (MEM32(g_esp)!=0x191316u) abort();
    g_esp+=4; g_ecx=0x8765; g_edx=0x4321;
    g_eax=++idle_calls<2;
}
#define eax g_eax
#define ecx g_ecx
#define edx g_edx
#define esp g_esp
'''
MAIN = r'''
static void setup(unsigned flags, unsigned idle) {
    memset(mem,0,sizeof mem); memset(regs,0,sizeof regs);
    g_eax=0x111; g_ecx=0x10000; g_edx=0x333; g_esp=0x1F0000; idle_calls=0;
    MEM32(0x19DCE0)=0x10000; MEM32(0x10008)=flags;
    MEM32(0x10000)=0x81234567; MEM32(0x1035C)=0x82345678;
    MEM32(0x12268)=0xFD000000; MEM32(0x12264)=0x30000;
    MEM32(0x19DED0)=idle; MEM32(0xFD100410u)=0x520;
    MEM32(0x10030)=20; MEM32(0x10034)=0x40000;
    MEM32(0x12ABC)=0x9988; MEM32(0x10040)=0x7766;
    MEM32(g_esp)=0xBEEF;
}
int main(int argc,char **argv) {
    mode=argc>1 && !strcmp(argv[1],"legacy");
    for(unsigned flags=0; flags<4; flags++) for(unsigned idle=0; idle<2; idle++) {
        unsigned f=(flags&1?4:0)|(flags&2?0x2000:0);
        setup(f,idle); sub_001912A0_gen();
        uint32_t a=g_eax,c=g_ecx,d=g_edx,s=g_esp; unsigned calls=idle_calls;
        memcpy(expected_mem,mem,sizeof mem); memcpy(expected_regs,regs,sizeof regs);
        setup(f,idle); sub_001912A0();
        if(a!=g_eax || c!=g_ecx || d!=g_edx || s!=g_esp || calls!=idle_calls ||
           memcmp(expected_mem,mem,sizeof mem) || memcmp(expected_regs,regs,sizeof regs)) {
            fprintf(stderr,"mismatch flags=%x idle=%u\n",f,idle); return 1;
        }
    }
    printf("PASS: 8 KickOff cases (%s), guest registers/stack/memory/idle calls match\n",
           mode?"legacy switch":"blocking + software fallback");
}
'''

with tempfile.TemporaryDirectory(prefix="jsrf-kickoff-") as folder:
    source = Path(folder) / "test.c"
    exe = Path(folder) / "test.exe"
    source.write_text(HARNESS + REFERENCE + "\n" + MANUAL + MAIN, encoding="utf-8")
    subprocess.run(["gcc", "-std=c11", "-O2", str(source), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    subprocess.run([str(exe), "legacy"], check=True)

    # Exercise the actual condition-variable implementation too: unlike the
    # register fixture above this catches lost acknowledgements / wakeups.
    kernel = ROOT / "src/kernel/xbox_memory_layout.c"
    kernel_source = kernel.read_text(encoding="utf-8")
    def kernel_function(signature):
        begin = kernel_source.index(signature)
        end = kernel_source.index("{", begin) + 1
        depth = 1
        while depth:
            depth += (kernel_source[end] == "{") - (kernel_source[end] == "}")
            end += 1
        return kernel_source[begin:end]
    concurrent = r'''
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <stdio.h>
#include <sched.h>
static uint32_t mmio[0x110000/4];
static ptrdiff_t g_memory_offset;
static pthread_mutex_t s_flush_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_flush_cv=PTHREAD_COND_INITIALIZER;
static pthread_mutex_t kick_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t kick_cv=PTHREAD_COND_INITIALIZER;
static int pending, stop, dropped, retaken;
int xbox_guest_lock_drop(void) { dropped++; return 1; }
void xbox_guest_lock_retake(int held) { retaken+=held; }
void xbox_nv2a_kick(void) {
    pthread_mutex_lock(&kick_mu); pending=1;
    pthread_cond_signal(&kick_cv); pthread_mutex_unlock(&kick_mu);
}
'''
    concurrent += kernel_function("void xbox_nv2a_flush_wait(uint32_t register_base)")
    concurrent += kernel_function("static void nv2a_flush_ack(uint32_t *r)")
    concurrent += r'''
static void *ack(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&kick_mu);
        while(!pending && !stop) pthread_cond_wait(&kick_cv,&kick_mu);
        int done=stop; pending=0; pthread_mutex_unlock(&kick_mu);
        if(done) return NULL;
        nv2a_flush_ack(mmio+0x100410/4);
    }
}
int main(void) {
    pthread_t worker;
    g_memory_offset=(ptrdiff_t)(uintptr_t)mmio-(ptrdiff_t)0xFD000000u;
    pthread_create(&worker,NULL,ack,NULL);
    for(int i=0;i<20000;i++) {
        xbox_nv2a_flush_wait(0xFD000000u);
        if(__atomic_load_n(mmio+0x100410/4,__ATOMIC_ACQUIRE)&0x10000u) return 1;
        if(i%7==0) sched_yield();
    }
    pthread_mutex_lock(&kick_mu); stop=1; pthread_cond_signal(&kick_cv);
    pthread_mutex_unlock(&kick_mu); pthread_join(worker,NULL);
    if(dropped!=20000 || retaken!=20000) return 2;
    puts("PASS: 20000 threaded flush acknowledgements, no lost wakeup, guest lock paired");
}
'''
    source.write_text(concurrent, encoding="utf-8")
    subprocess.run(["gcc", "-std=c11", "-O2", "-pthread", str(source), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=30)
