#include <assert.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

static uint32_t (*legacy_step)(uint32_t),(*grouped_step)(uint32_t);
static uint32_t (*legacy_bridge_roundtrip)(uint32_t),(*grouped_bridge_roundtrip)(uint32_t);
static void (*legacy_reset)(uint32_t),(*grouped_reset)(uint32_t);
static uint32_t *(*legacy_address)(unsigned),*(*grouped_address)(unsigned);
static pthread_barrier_t barrier;

static void *worker(void *arg)
{
    uintptr_t id=(uintptr_t)arg;
    pthread_barrier_wait(&barrier);
    for(unsigned n=0;n<2000;n++) {
        uint32_t seed=(uint32_t)(id*1000000+n),expected[8];
        legacy_reset(seed);uint32_t wanted=legacy_step(n);
        for(unsigned i=0;i<8;i++)expected[i]=*legacy_address(i);
        grouped_reset(seed);uint32_t actual=grouped_step(n);assert(wanted==actual);
        sched_yield();
        for(unsigned i=0;i<8;i++) {
            assert(legacy_address(i)==grouped_address(i));
            assert(*legacy_address(i)==expected[i]);
        }
        /* Alternate both access models against the same storage. */
        legacy_reset(seed);legacy_step(n);uint32_t second=grouped_step(n+1);
        grouped_reset(seed);grouped_step(n);assert(legacy_step(n+1)==second);
        assert(legacy_bridge_roundtrip(seed)==seed+1);
        assert(grouped_bridge_roundtrip(seed)==seed+1);
    }
    return NULL;
}

static double seconds(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
static volatile uint32_t sink;
int main(int argc,char **argv)
{
    assert(argc==2);void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
    if(!lib){fprintf(stderr,"dlopen: %s\n",dlerror());return 1;}
#define GET(name) do { *(void **)(&name)=dlsym(lib,#name);assert(name); } while(0)
    GET(legacy_step);GET(grouped_step);GET(legacy_reset);GET(grouped_reset);
    GET(legacy_address);GET(grouped_address);
    GET(legacy_bridge_roundtrip);GET(grouped_bridge_roundtrip);
    pthread_t threads[8];assert(!pthread_barrier_init(&barrier,NULL,8));
    for(uintptr_t i=0;i<8;i++)assert(!pthread_create(&threads[i],NULL,worker,(void *)i));
    for(unsigned i=0;i<8;i++)assert(!pthread_join(threads[i],NULL));
    pthread_barrier_destroy(&barrier);
    puts("PASS: dlopen, all 8 TLS aliases, cross-module read/write, 8 concurrent guest contexts, repeated yields");
    const unsigned loops=500000;
    legacy_reset(123);double a=seconds();
    for(unsigned i=0;i<loops;i++)sink=legacy_step(i);
    double b=seconds();uint32_t wanted=sink;
    grouped_reset(123);double c=seconds();
    for(unsigned i=0;i<loops;i++)sink=grouped_step(i);
    double d=seconds();assert(wanted==sink);
    printf("BENCH GPR TLS %u calls: separate %.3f ms, grouped %.3f ms\n",loops,(b-a)*1000,(d-c)*1000);
    dlclose(lib);return 0;
}
