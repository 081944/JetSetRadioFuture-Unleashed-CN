"""Compare actual geometry helpers with the pre-optimization scalar counters."""
from pathlib import Path
import argparse
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
current = (root / 'src/nv2a_gl/nv2a_gl.c').read_text(encoding='utf-8')
original = subprocess.check_output(['git', 'show', 'a68db81:src/nv2a_gl/nv2a_gl.c'], cwd=root).decode('utf-8')


def function(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end] + '\n'


source = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#define MAX_INDICES 64u
#define MAX_INLINE 96u
#define M_ARRAY_ELEMENT16 0x1800u
#define M_ARRAY_ELEMENT32 0x1808u
#define M_INLINE_ARRAY 0x1818u
static struct { uint32_t idx_count,idx[MAX_INDICES],inline_count,inline_buf[MAX_INLINE]; int const_attr_init; } S;
static struct { uint64_t geometry_packets,geometry_words; } g_prof;
static int enabled=1,g_failed,allowed=1,splits,split_reset;
static int gl_enabled(void){return enabled;}
static int nv2a_gl_enabled(void){return enabled;}
static int nv2a_gl_packet_batch_allowed(void){return allowed;}
static void split_batch_if_state_moved(void){splits++;if(split_reset){S.idx_count=S.inline_count=0;split_reset=0;}}
static void nv2a_gl_method(uint32_t method,uint32_t param){
    if(!enabled||g_failed)return;S.const_attr_init=1;split_batch_if_state_moved();
    if(method==M_ARRAY_ELEMENT16 && S.idx_count+2<=MAX_INDICES){S.idx[S.idx_count++]=param&0xffff;S.idx[S.idx_count++]=param>>16;}
    else if(method==M_ARRAY_ELEMENT32 && S.idx_count<MAX_INDICES)S.idx[S.idx_count++]=param;
}
'''
for name, signature in [('nv2a_gl_method_run', 'int nv2a_gl_method_run('), ('nv2a_gl_indices', 'void nv2a_gl_indices(')]:
    source += function(current, signature)
    source += function(original, signature).replace(name + '(', 'reference_' + name + '(', 1)
source += r'''
static uint32_t rng=987654321;
static uint32_t random32(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
static double seconds(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
__attribute__((noinline)) static int bench_old(const uint32_t *p){return reference_nv2a_gl_method_run(M_ARRAY_ELEMENT16,p,32);}
__attribute__((noinline)) static int bench_new(const uint32_t *p){return nv2a_gl_method_run(M_ARRAY_ELEMENT16,p,32);}
static volatile uint32_t sink;
int main(void){
    uint32_t data[160];for(unsigned i=0;i<160;i++)data[i]=random32();
    for(int test=0;test<10000;test++) {
        memset(&S,0,sizeof S);S.const_attr_init=1;S.idx_count=random32()%(MAX_INDICES+1);S.inline_count=random32()%(MAX_INLINE+1);
        for(unsigned i=0;i<MAX_INDICES;i++)S.idx[i]=random32();
        for(unsigned i=0;i<MAX_INLINE;i++)S.inline_buf[i]=random32();
        enabled=!!(test%5);g_failed=!(test%7);allowed=!!(test%11);S.const_attr_init=!!(test%13);
        const uint32_t methods[]={M_ARRAY_ELEMENT16,M_ARRAY_ELEMENT32,M_INLINE_ARRAY,0x200};
        uint32_t method=methods[test%4],count=random32()%160;
        __typeof__(S) before=S,expected;
        splits=0;split_reset=test&1;memset(&g_prof,0,sizeof g_prof);
        int wanted=reference_nv2a_gl_method_run(method,data,count);expected=S;
        __typeof__(g_prof) profile=g_prof;int expected_splits=splits;
        S=before;splits=0;split_reset=test&1;memset(&g_prof,0,sizeof g_prof);
        int actual=nv2a_gl_method_run(method,data,count);
        assert(actual==wanted && !memcmp(&S,&expected,sizeof S));
        assert(splits==expected_splits && !memcmp(&profile,&g_prof,sizeof g_prof));
        S=before;splits=0;split_reset=test&1;method=methods[test&1];count=count?count:1;
        reference_nv2a_gl_indices(method,data,count);expected=S;expected_splits=splits;
        S=before;splits=0;split_reset=test&1;nv2a_gl_indices(method,data,count);
        assert(!memcmp(&S,&expected,sizeof S) && splits==expected_splits);
    }
    puts("PASS: 10000 geometry packet/index runs, odd/full capacities, splitting, flags, disabled/failed paths match original");
    enabled=allowed=1;g_failed=split_reset=0;S.const_attr_init=1;
    const unsigned loops=400000;double a=seconds();
    for(unsigned i=0;i<loops;i++){S.idx_count=0;sink+=bench_old(data);}double b=seconds();
    for(unsigned i=0;i<loops;i++){S.idx_count=0;sink+=bench_new(data);}double c=seconds();
    printf("BENCH index16 32-word packets: original %.3f ms, local cursor %.3f ms\n",(b-a)*1000,(c-b)*1000);
}
'''
parser = argparse.ArgumentParser()
parser.add_argument('--emit-c', type=Path)
args = parser.parse_args()
if args.emit_c:
    args.emit_c.write_text(source, encoding='utf-8')
else:
    with tempfile.TemporaryDirectory(prefix='jsrf-geometry-') as folder:
        c = Path(folder) / 'test.c'
        exe = Path(folder) / 'test.exe'
        c.write_text(source, encoding='utf-8')
        subprocess.run(['gcc', '-std=gnu11', '-O2', str(c), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
