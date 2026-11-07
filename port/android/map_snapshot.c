/* Read-only companion map telemetry. Addresses are verified against the CN
 * generated code (the US executable has different globals). No guest calls,
 * input writes, pause flags, or render state changes are made here. */
#include "map_snapshot.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

static atomic_int enabled;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static float snapshot[JSRF_MAP_FLOATS];
static char objective[JSRF_MAP_CAPTION_BYTES],pending_objective[JSRF_MAP_CAPTION_BYTES];
static uint32_t objective_flow,objective_source;
static unsigned sequence;
#ifdef JSRF_CN_PROFILE
#define MAP_ROOT 0x22FCE0u
#define MAP_GRAFFITI 0x2314B0u
#define MAP_PICKUPS 0x20C750u
#define MAP_ROTATION 0x21BEB8u
#else
#define MAP_ROOT 0x22FCC0u
#define MAP_GRAFFITI 0x231490u
#define MAP_PICKUPS 0x20C748u
#define MAP_ROTATION 0x21BE98u
#endif
static int valid(uint32_t p, unsigned bytes)
{ return p >= 0x10000u && p < 0x04000000u && bytes <= 0x04000000u - p; }
static uint32_t word(const uint8_t *m, uint32_t p)
{ uint32_t n; memcpy(&n, m + p, 4); return n; }
static float real(const uint8_t *m, uint32_t p)
{ float n; memcpy(&n, m + p, 4); return n; }
void jsrf_map_set_objective(const uint8_t *m,uint32_t flow,uint32_t record)
{
    /* sub_4CA20 receives the five explicit language pointers. Their strings
     * have variable lengths; adjacent text must never be treated as slots. */
    objective_flow=flow; objective_source=0; pending_objective[0]=0;
    if (!m || !valid(flow,0x105C) || !valid(record,40)) return;
    objective_source=word(m,flow+0x1058);
    uint32_t english=word(m,record+24);
    if (!english) return;
    for (unsigned i=0;i<sizeof pending_objective-1;++i) {
        if (!valid(english+i,1)) { pending_objective[0]=0; return; }
        unsigned char c=m[english+i];
        if (!c) { pending_objective[i]=0; return; }
        if (c<32 || c>126) { pending_objective[0]=0; return; }
        pending_objective[i]=(char)c;
    }
    pending_objective[sizeof pending_objective-1]=0;
}
static void marker(float *s, int kind, float x, float y, float z, float yaw)
{
    unsigned n = (unsigned)s[3];
    if (n >= JSRF_MAP_MARKERS || !isfinite(x) || !isfinite(y) || !isfinite(z)
        || fabsf(x) > 10000 || fabsf(y) > 10000 || fabsf(z) > 10000) return;
    float *p = s + 16 + n * 6;
    p[0] = (float)kind; p[1] = x; p[2] = y; p[3] = z;
    p[4] = yaw; p[5] = 1; s[3] = (float)(n + 1);
}
void jsrf_map_enable(int on) { atomic_store_explicit(&enabled, !!on, memory_order_relaxed); }
int jsrf_map_copy(float *out, unsigned capacity)
{
    if (!out || capacity < JSRF_MAP_FLOATS) return 0;
    pthread_mutex_lock(&lock);
    memcpy(out, snapshot, sizeof snapshot);
    int seq = (int)sequence;
    pthread_mutex_unlock(&lock);
    return seq;
}
int jsrf_map_objective_copy(char *out,unsigned capacity)
{
    if (!out || capacity<sizeof objective) return 0;
    pthread_mutex_lock(&lock);
    memcpy(out,objective,sizeof objective);
    pthread_mutex_unlock(&lock);
    return 1;
}
void jsrf_map_publish(const uint8_t *m)
{
    static struct timespec last;
    static float work[JSRF_MAP_FLOATS]; /* one producer, guest frame thread */
    char caption[JSRF_MAP_CAPTION_BYTES]={0};
    struct timespec now;
    if (!atomic_load_explicit(&enabled, memory_order_relaxed) || !m) return;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if ((now.tv_sec - last.tv_sec) * 1000000000LL + now.tv_nsec - last.tv_nsec < 32000000) return;
    last = now;
    memset(work, 0, 16 * sizeof(float));
    uint32_t root = word(m, MAP_ROOT);
    if (!valid(root, (38 + 6582) * 4)) goto done;
    uint32_t flow = word(m, root + (38 + 9) * 4);
    uint32_t cam = word(m, root + (38 + 0x4C) * 4);
    if (!valid(flow, 0x1064) || !valid(cam, 0x230)) goto done;
    uint32_t stage = word(m, flow + 0x104C), state = word(m, flow + 0x5C);
    uint32_t who = word(m, cam + 0x24);
    /* 93/94 transfer ownership to a newly loaded stage. Never expose its
     * predecessor's player or marks as the new map. Pause/map states stay live. */
    if (word(m, flow + 0x58) == 0x5A || state < 15 || state == 93 || state == 94
        || (stage >> 16) > 5 || (stage & 0xffff) > 9 || !valid(who, 0xD00)) goto done;
    if (word(m, who) == 0) goto done;
    work[0] = 1; work[1] = (float)stage; work[2] = (float)state;
    if (flow==objective_flow && word(m,flow+0x1058)==objective_source)
        memcpy(caption,pending_objective,sizeof caption);
    for (unsigned k = 0; k < 3; ++k) work[4 + k] = real(m, who + 0xCA4 + k * 4) * .01f;
    work[7] = (float)(int16_t)word(m, who + 0xCC0) * (360.0f / 65536.0f);
    unsigned district = stage >> 16, area = stage & 0xffff, index;
    switch (district) {
    case 1: index=area; break;
    case 2: index=area+4; break;
    case 3: index=area+9; break;
    case 4: index=area<3 ? area+14 : 16; break;
    case 5: index=area+17; break;
    default: index=area+22; break;
    }
    for (unsigned k=0; k<3; ++k) {
        float angle = index<32 ? real(m,MAP_ROTATION+index*12+k*4) : 0;
        work[8+k] = isfinite(angle) && fabsf(angle)<=360 ? angle : 0;
    }
    marker(work, 3, work[4], work[5], work[6], work[7]);
    /* Stage exits and names, same 16-byte records used by sub_00112150. */
    uint32_t n = word(m, flow + 0x105C), p = word(m, flow + 0x1060);
    if (n <= 256 && valid(p, n * 16)) for (unsigned i = 0; i < n; ++i, p += 16) {
        uint32_t kind = word(m, p);
        if (kind == 1 || kind == 2 || (kind >= 12 && kind <= 34))
            marker(work, (int)kind, real(m,p+4)*.01f, real(m,p+8)*.01f, real(m,p+12)*.01f, 0);
    }
    /* Incomplete graffiti: preserve the original three-bit completion test. */
    p = word(m, MAP_GRAFFITI + 514 * 4);
    for (unsigned guard = 0; p && guard < 512 && valid(p, 272); ++guard) {
        uint32_t model = word(m,p), parts = word(m,p+152), mask = word(m,p+164);
        int incomplete = 0;
        for (unsigned i = 0; i < parts && i < 10; ++i)
            if ((mask & (7u << (i*3))) == (7u << (i*3))) incomplete = 1;
        if (word(m,p+160) && !word(m,p+196) && incomplete && valid(model,156))
            marker(work,0,real(m,model+144)*.01f,real(m,model+148)*.01f,real(m,model+152)*.01f,0);
        uint32_t next = word(m,p+268); if (next == p) break; p = next;
    }
    /* Can pickups: the original map's sub_00111A00 uses each entity's matrix. */
    p = word(m, MAP_PICKUPS + 4);
    for (unsigned guard = 0; p && guard < 256 && valid(p,400); ++guard) {
        n = word(m,p+4); uint32_t items = word(m,p);
        if (n <= 1024 && valid(items,n*72)) for (unsigned i = 0; i < n; ++i) {
            uint32_t q = items+i*72+56;
            marker(work,7,real(m,q)*.01f,real(m,q+4)*.01f,real(m,q+8)*.01f,0);
        }
        uint32_t next = word(m,p+396); if (next == p) break; p = next;
    }
#ifdef JSRF_CN_PROFILE
    /* Secret collectibles use their own list and availability/collected bit
     * banks (sub_113E90 / sub_39EF0), not the can or graffiti lists. */
    p = word(m, 0x1F9138u);
    for (unsigned guard=0; p && guard<256 && valid(p,60); ++guard) {
        unsigned id=word(m,p+12);
        if (id<256) {
            uint32_t bit=1u<<(id&31);
            uint32_t available=word(m,0x1EFFB0u+54*4+(id>>5)*4);
            uint32_t collected=word(m,0x1EFFB0u+62*4+(id>>5)*4);
            if (available & ~collected & bit)
                marker(work,5,real(m,p)*.01f,real(m,p+4)*.01f,real(m,p+8)*.01f,0);
        }
        uint32_t next=word(m,p+56); if (next==p) break; p=next;
    }
    uint32_t map=word(m,root+(38+4455)*4);
    if (valid(map,1108)) {
        /* Original map's static targets and other characters. */
        n=word(m,map+1104); p=word(m,map+1100);
        if (n<=256 && valid(p,n*16)) for (unsigned i=0;i<n;++i,p+=16) {
            unsigned type=word(m,p);
            int kind=type==1 || type==4 || type==5 || type==7 || (type>=10 && type<=14)
                || (type>=18 && type<=22) ? 10 : 11;
            marker(work,kind,real(m,p+4)*.01f,real(m,p+8)*.01f,real(m,p+12)*.01f,0);
        }
        n=word(m,map+1096);
        uint32_t playerId=valid(root+(7706+13)*4,4) ? word(m,root+(7706+13)*4) : UINT32_MAX;
        if (n<=32 && valid(map+456,n*20)) for (unsigned i=0;i<n;++i) {
            p=map+456+i*20;
            if (word(m,p)!=playerId)
                marker(work,7,real(m,p+4)*.01f,real(m,p+8)*.01f,real(m,p+12)*.01f,0);
        }
        uint32_t mission=word(m,map+452);
        if (valid(mission,4)) {
            unsigned type=word(m,mission);
            if (type==7 && word(m,0x1EB938u+8)!=2)
                marker(work,6,real(m,0x1EB938u+12)*.01f,real(m,0x1EB938u+16)*.01f,real(m,0x1EB938u+20)*.01f,0);
            if (type==4 || type==12) {
                uint32_t task=word(m,root+(38+6581)*4);
                if (valid(task,84)) {
                    unsigned at=word(m,task+68), total=word(m,task+72);
                    uint32_t points=word(m,task+80);
                    if (at<total && total<=1024 && valid(points,total*92)) {
                        p=points+at*92;
                        marker(work,5,real(m,p)*.01f,real(m,p+4)*.01f,real(m,p+8)*.01f,0);
                    }
                }
            }
        }
    }
#endif
done:
    pthread_mutex_lock(&lock);
    ++sequence;
    memcpy(objective,caption,sizeof objective);
    memcpy(snapshot, work, (16 + (unsigned)work[3] * 6) * sizeof(float));
    pthread_mutex_unlock(&lock);
}
