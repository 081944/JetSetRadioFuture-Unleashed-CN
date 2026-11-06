"""Emit an Android test of the actual pacing helpers and a 60 Hz wait model."""
import argparse
from pathlib import Path

root = Path(__file__).resolve().parents[2]
gl = (root / 'src/nv2a_gl/nv2a_gl.c').read_text(encoding='utf-8')
stats = gl[gl.index('static struct {\n    uint64_t frames, late, restarts'):]
stats = stats[:stats.index('} g_pace_st;') + len('} g_pace_st;')]
helpers = gl[gl.index('#if defined(__linux__)\n#include <errno.h>\n#define PACE_ABSOLUTE 1'):gl.index('static void frame_pace(void)')]
source = r'''
#define _GNU_SOURCE 1
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <sys/time.h>
static const char *test_spin;
static const char *jsrf_getenv(const char *name){assert(!strcmp(name,"RECOMP_PACE_SPIN_US"));return test_spin;}
static double mono_now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
'''
source += stats + '\n' + helpers
source += r'''
static void reset(const char *fixed){test_spin=fixed;g_pace_margin=-1;g_pace_margin_min=0;g_pace_margin_fixed=0;memset(&g_pace_st,0,sizeof g_pace_st);}
static int near(double a,double b){return a>b-1e-9 && a<b+1e-9;}
static volatile sig_atomic_t signals;
static void interrupted(int signo){(void)signo;signals++;}
static double cpu_now(void){struct timespec t;clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);return t.tv_sec+t.tv_nsec*1e-9;}
static int compare_double(const void *a,const void *b){double x=*(const double *)a,y=*(const double *)b;return (x>y)-(x<y);}
static void correctness(void){
    reset(NULL);assert(near(pace_margin(),.0003));pace_margin_learn(.00025);assert(near(pace_margin(),.00045));
    for(int i=0;i<100;i++)pace_margin_learn(0);assert(near(pace_margin(),.0003));
    pace_margin_learn(.005);assert(near(pace_margin(),.0012));
    reset("1200");assert(near(pace_margin(),.0012));pace_margin_learn(.020);assert(near(pace_margin(),.0012));
    reset("0");assert(near(pace_margin(),0));pace_margin_learn(.020);assert(near(pace_margin(),0));
    reset("-500");assert(near(pace_margin(),0));
    reset("99999");assert(near(pace_margin(),.005));
    struct sigaction action={0};action.sa_handler=interrupted;sigemptyset(&action.sa_mask);assert(!sigaction(SIGALRM,&action,NULL));
    struct itimerval timer={.it_value={.tv_sec=0,.tv_usec=500}};assert(!setitimer(ITIMER_REAL,&timer,NULL));
    double until=mono_now()+.004;pace_sleep_until(until);assert(mono_now()>=until && signals>0);
    until=mono_now()-.001;pace_sleep_until(until);assert(mono_now()>=until);
    puts("PASS: adaptive growth/decay, fixed/zero/clamped margins, monotonic absolute sleeps, EINTR retry");
}
static void cadence(int legacy){
    enum {FRAMES=180};double intervals[FRAMES],lateness[FRAMES];unsigned late=0;
    reset(legacy?"1200":NULL);double deadline=mono_now(),previous=deadline,begin=deadline,cpu=cpu_now();
    for(int i=0;i<FRAMES;i++){
        deadline+=1.0/60.0;double now=mono_now(),spin=deadline-pace_margin();
        if(now<spin){
            if(legacy){double wait=spin-now;struct timespec ts={(time_t)wait,(long)((wait-(time_t)wait)*1e9)};nanosleep(&ts,NULL);}
            else pace_sleep_until(spin);
            now=mono_now();if(!legacy)pace_margin_learn(now-spin);
        }
        while((now=mono_now())<deadline){}
        assert(now>=deadline);intervals[i]=(now-previous)*1000;previous=now;
        lateness[i]=(now-deadline)*1000;if(lateness[i]>.5)late++;
    }
    double spent=(cpu_now()-cpu)*1000/FRAMES,elapsed=mono_now()-begin;
    qsort(intervals,FRAMES,sizeof(double),compare_double);qsort(lateness,FRAMES,sizeof(double),compare_double);
    printf("PACE %s: %.3f fps, CPU %.3f ms/frame, interval p95 %.3f p99 %.3f ms, lateness p99 %.3f ms, late>0.5ms %u/%u, final spin %.3f ms\n",
        legacy?"legacy-1.2ms":"adaptive",FRAMES/elapsed,spent,intervals[FRAMES*95/100],intervals[FRAMES*99/100],lateness[FRAMES*99/100],late,FRAMES,pace_margin()*1000);
}
int main(void){correctness();cadence(1);cadence(0);return 0;}
'''
parser = argparse.ArgumentParser()
parser.add_argument('--emit-c', required=True, type=Path)
args = parser.parse_args()
args.emit_c.write_text(source, encoding='utf-8')
