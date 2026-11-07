/* Only the guest frame thread reads/writes RAM or invokes guest functions.
 * Android threads exchange a latest-request mailbox and a small copied status. */
#include "character_switch.h"
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

static atomic_int request = ATOMIC_VAR_INIT(-1);
static atomic_int interlock;
int jsrf_character_blocks_input(void) { return atomic_load_explicit(&interlock,memory_order_acquire); }
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int published[JSRF_CHARACTER_WORDS] = {0, -1, 0, -1, 0, 0};

int jsrf_character_request(int id)
{
    if (id < 0 || id >= 24) return 0;
    atomic_store_explicit(&request, id, memory_order_release);
    return 1;
}
void jsrf_character_snapshot(int out[JSRF_CHARACTER_WORDS])
{
    pthread_mutex_lock(&lock);
    memcpy(out, published, sizeof published);
    pthread_mutex_unlock(&lock);
}

#ifdef JSRF_CN_PROFILE
enum { IDLE, LOAD, DELETE, SPAWN };
static struct {
    int phase, status, target, original, destroyed, rollback, load_started, sweep_ready;
    uint32_t root, flow, spawner, manager, mission, stage;
    uint32_t config[13];
    double deadline;
} tx;
static int valid(uint32_t p, unsigned size)
{ return p >= 0x10000u && p < 0x4000000u && size <= 0x4000000u-p; }
static uint32_t word(const uint8_t *m, uint32_t p)
{ uint32_t v; memcpy(&v,m+p,4); return v; }
static void put(uint8_t *m,uint32_t p,uint32_t v) { memcpy(m+p,&v,4); }
static uint32_t obj(const uint8_t *m,uint32_t root,unsigned id)
{ return word(m,root+0x98+id*4); }
static uint32_t var(const uint8_t *m,uint32_t root,unsigned id)
{ return word(m,root+0x7868+id*4); }
static void setvar(uint8_t *m,uint32_t root,unsigned id,uint32_t value)
{ put(m,root+0x7868+id*4,value); }
static int object(const uint8_t *m,uint32_t p,unsigned size,uint32_t vtable)
{ return valid(p,size) && word(m,p)==vtable && !(word(m,p+4)&0x80000000u); }
/* sub_14B910's sweep payload. A new actor has no movement history; publishing
 * its current point avoids a sweep from recycled storage. Do not normalize a
 * zero-length segment (the original helper produces NaNs), or touch links and
 * receiver-specific metadata at +0x30 and beyond. */
static void initial_sweep(uint8_t *m,uint32_t receiver,uint32_t position)
{
    memset(m+receiver,0,0x30);
    memcpy(m+receiver,m+position,12);
    memcpy(m+receiver+0x20,m+position,12);
}
static uint32_t player(const uint8_t *m,uint32_t root)
{
    uint32_t cam=obj(m,root,0x4C);
    if (!object(m,cam,0x240,0x1CD518)) return 0;
    uint32_t p=word(m,cam+0x24);
    return object(m,p,0x1178,0x1CCFF8) ? p : 0;
}
/* Use the same exact-ID and shared fallback rules as sub_00032610. A low-byte
 * alias alone does not satisfy command 2's exact completion predicate. */
static int resident(const uint8_t *m,uint32_t res,int id,uint32_t *bindings)
{
    uint32_t p=word(m,res+0x1E4), found=0;
    for (unsigned n=0;p && n<256;n++) {
        if (!valid(p,0x660)) return 0;
        if (word(m,p+8)==(unsigned)id) { found=p; break; }
        p=word(m,p+0x65C);
    }
    if (!found) return 0;
    uint32_t model=word(m,found+0x640), data=word(m,found+0x644);
    uint32_t key=word(m,found+0x10);
    p=word(m,res+0x1F0);
    for (unsigned n=0;p && n<256 && (!model || !data);n++) {
        if (!valid(p,0x550)) return 0;
        if (word(m,p)==key) {
            if (!model) model=word(m,p+0x530);
            if (!data) data=word(m,p+0x534);
            break;
        }
        p=word(m,p+0x54C);
    }
    if (bindings) { bindings[0]=model; bindings[1]=data; }
    return valid(model,0x164C) && valid(data,0x904);
}
static void finish(int status)
{
    fprintf(stderr,"[CHARACTER] result=%d target=%d original=%d rollback=%d\n",
            status,tx.target,tx.original,tx.rollback);
    tx.phase=IDLE; tx.status=status;
    atomic_store_explicit(&interlock,0,memory_order_release);
}
void jsrf_character_destroyed(uint32_t manager)
{ if (tx.phase==DELETE && tx.manager==manager) tx.destroyed=1; }
int jsrf_character_hold_flow(uint32_t flow)
{ return (tx.phase==DELETE || tx.phase==SPAWN) && tx.flow==flow; }

static int begin_delete(uint8_t *m,double now,JsrfCharacterCall call,uint32_t manager)
{
    uint32_t args[]={12}, result=0;
    tx.manager=manager; tx.destroyed=0; tx.phase=DELETE;
    atomic_store_explicit(&interlock,1,memory_order_release);
    tx.deadline=now+5; tx.status=JSRF_CHARACTER_REBUILDING;
    if (!call(0x13FC0,1,args,&result)) { tx.status=JSRF_CHARACTER_FAILED; return 0; }
    /* Same child-first destruction as sub_00011D00's marked-node branch,
     * scoped to this manager. No task walk is active at this frame boundary.
     * Do not run the whole walk here: that would also publish its siblings. */
    uint32_t children[]={manager,word(m,manager+0x28)};
    uint32_t destroy[]={manager,1};
    if (!call(0x11B90,2,children,&result) || !call(0xA6AF0,2,destroy,&result)) {
        tx.status=JSRF_CHARACTER_FAILED; return 0;
    }
    return 1;
}
static void spawn(uint8_t *m,double now,JsrfCharacterCall call)
{
    /* The original spawner owns this persistent 52-byte slot-0 config. It
     * copies normal configurations here itself; source==destination is valid.
     * No host pointer or transient guest stack address escapes into RAM. */
    tx.sweep_ready=0;
    uint32_t config=tx.spawner+0x544;
    memcpy(m+config,tx.config,sizeof tx.config);
    setvar(m,tx.root,0xD,(uint32_t)tx.target);
    setvar(m,tx.root,0x2D,(uint32_t)tx.target);
    setvar(m,tx.root,0x8D,0);
    setvar(m,tx.root,0x6D,config);
    tx.phase=SPAWN; tx.deadline=now+5;
    /* Complete the normal handover before another world update can run with
     * no player. This is the same persistent spawner's ordinary entry point. */
    uint32_t args[]={tx.spawner},result=0;
    if (!call(0xA8370,1,args,&result)) tx.status=JSRF_CHARACTER_FAILED;
    /* sub_00024AB0 registers the new scene-volume receiver but leaves its
     * sweep geometry uninitialized until the player's first update. The
     * volume task can run first and interpret recycled heap data as a sweep
     * through a launch/respawn volume. Publish initial sweeps and clear event
     * flags before any world task runs (including moving-platform receivers).
     * Keep the receiver's intrusive-list links (0x30/0x34) untouched. */
    uint32_t replacement=player(m,tx.root);
    if (replacement) {
        /* The moving-platform receiver is consumed BEFORE its first update:
         * sub_80340 adds +0x3C's displacement directly to player/camera. It and
         * both solid-collision sweeps were missed by the original fix. */
        const unsigned offsets[]={0x2D0,0xF00,0x3C8,0x2D4,0x101C};
        const unsigned sizes[]={0x48,0x60,0x68,0x44,0x58};
        const uint32_t owners[]={0x20CC58,0x1EBABC,0x1EC050};
        tx.sweep_ready=1;
        for (unsigned i=0;i<5;i++) {
            uint32_t receiver=word(m,replacement+offsets[i]);
            if (!valid(receiver,sizes[i])) { tx.sweep_ready=0; break; }
            if (i<3) {
                uint32_t sweep[]={owners[i],receiver,replacement+0xCB0,replacement+0xCA4};
                if (!call(0x6F890,4,sweep,&result)) { tx.sweep_ready=0; break; }
            }
            initial_sweep(m,receiver,replacement+0xCA4);
        }
        if (!tx.sweep_ready) tx.status=JSRF_CHARACTER_FAILED;
    }
    if (!obj(m,tx.root,12) && !var(m,tx.root,0x6D) && !tx.rollback) {
        tx.target=tx.original; tx.rollback=1;
        spawn(m,now,call);
    }
}
void jsrf_character_tick(uint8_t *m,double now,int world_available,JsrfCharacterCall call)
{
    uint32_t root=word(m,0x22FCE0), flow=0,res=0,spawner=0,p=0;
    uint32_t unlocked=word(m,0x1EFFC4);
    int safe=0,current=-1;
    if (valid(root,0x8800)) {
        flow=obj(m,root,9); res=obj(m,root,7); spawner=obj(m,root,11);
        p=player(m,root);
        if (p) current=(int)word(m,p+0xFBC);
        uint32_t manager=obj(m,root,12);
        safe=world_available && object(m,flow,0x1064,0x1CAAB0)
            && word(m,flow+0x5C)==15 && word(m,flow+0x58)!=0x5A
            && object(m,res,0x278,0x1C4F68)
            && object(m,spawner,0x12C4,0x1CD5C8)
            && object(m,manager,0x8E8,0x1CD570) && p
            && word(m,manager+0x8D4)==0x2C && obj(m,root,0x2C)==p
            && word(m,manager+0x8D8)==0x4C && word(m,manager+0x7C)==0
            && current>=0 && current<24
            && var(m,root,0xD)==(unsigned)current && var(m,root,0x2D)==(unsigned)current
            && !var(m,root,0x6D) && !obj(m,root,13) && !obj(m,root,14) && !obj(m,root,15);
    }
    /* Never recover an old scene's actor into a new scene, even if addresses
     * were recycled. Object identities, mission and stage must still match. */
    if (tx.phase && (root!=tx.root || flow!=tx.flow || spawner!=tx.spawner
        || !valid(flow,0x1064) || word(m,flow+0x58)!=tx.mission
        || word(m,flow+0x104C)!=tx.stage || !object(m,res,0x278,0x1C4F68)))
        finish(JSRF_CHARACTER_SCENE_CHANGED);

    if (tx.phase==IDLE || tx.phase==LOAD) {
        int requested=atomic_exchange_explicit(&request,-1,memory_order_acq_rel);
        if (requested>=0) {
            if (tx.phase==LOAD) tx.phase=IDLE; /* old async load remains loader-owned */
            tx.target=requested; tx.rollback=0;
            if (!safe) finish(JSRF_CHARACTER_UNSAFE);
            else if (!(unlocked&(1u<<requested))) finish(JSRF_CHARACTER_LOCKED);
            else if (requested==current) finish(JSRF_CHARACTER_DONE);
            else {
                tx.root=root; tx.flow=flow; tx.spawner=spawner;
                tx.mission=word(m,flow+0x58); tx.stage=word(m,flow+0x104C);
                tx.original=current; tx.phase=LOAD; tx.status=JSRF_CHARACTER_LOADING;
                tx.load_started=0;
                tx.deadline=now+30;
                fprintf(stderr,"[CHARACTER] request %d -> %d\n",current,requested);
            }
        }
    }
    if (tx.phase==LOAD) {
        if (!safe || current!=tx.original) finish(JSRF_CHARACTER_UNSAFE);
        else if (now>tx.deadline) finish(JSRF_CHARACTER_FAILED);
        else if (!word(m,res+0x44)) {
            if (!tx.load_started) {
                uint32_t args[]={2,(uint32_t)tx.target}, result=0;
                if (!call(0x256A0,2,args,&result) || !result) finish(JSRF_CHARACTER_FAILED);
                /* One start per request. Existing but invalid/incomplete records
                 * must fail rather than repeatedly starting command 2 forever. */
                else tx.load_started=1;
            } else if (!resident(m,res,tx.target,NULL)) finish(JSRF_CHARACTER_FAILED);
            else {
                uint32_t manager=obj(m,root,12), cfg=word(m,manager+0x8D0);
                if (!valid(cfg,sizeof tx.config) || word(m,cfg)!=0) finish(JSRF_CHARACTER_UNSAFE);
                else {
                    memcpy(tx.config,m+cfg,sizeof tx.config);
                    memcpy(tx.config+3,m+p+0xCA4,12);
                    memcpy(tx.config+6,m+p+0xCBC,12);
                    int finite=1;
                    for (unsigned k=3;k<6;k++) {
                        float value; memcpy(&value,&tx.config[k],4);
                        if (!isfinite(value) || fabsf(value)>1000000) finite=0;
                    }
                    if (!finite || !resident(m,res,tx.original,NULL)) finish(JSRF_CHARACTER_FAILED);
                    else begin_delete(m,now,call,manager);
                }
            }
        }
    }
    if (tx.phase==DELETE) {
        if (tx.destroyed && !obj(m,root,12) && !obj(m,root,0x2C) && !obj(m,root,0x4C)) spawn(m,now,call);
        else if (tx.deadline>0 && now>tx.deadline) {
            /* A missing table entry does not prove the destructor has run.
             * Keep the flow held and never overwrite live storage on timeout. */
            tx.deadline=0; tx.status=JSRF_CHARACTER_FAILED;
            fprintf(stderr,"[CHARACTER] waiting for old manager destructor\n");
        }
    } else if (tx.phase==SPAWN) {
        uint32_t manager=obj(m,root,12);
        uint32_t bindings[2]={0,0};
        if (tx.sweep_ready && p && object(m,manager,0x8E8,0x1CD570) && obj(m,root,0x2C)==p
            && current==tx.target && word(m,manager+0x8D8)==0x4C
            && resident(m,res,tx.target,bindings) && word(m,p+0x2A4)==bindings[0]
            && word(m,p+0xC90)==bindings[0] && word(m,p+0x58)==bindings[1])
            finish(tx.rollback ? JSRF_CHARACTER_RESTORED : JSRF_CHARACTER_DONE);
        else if ((!var(m,root,0x6D) && !manager) || now>tx.deadline) {
            if (tx.rollback) finish(JSRF_CHARACTER_FAILED);
            else {
                tx.target=tx.original; tx.rollback=1;
                if (manager && object(m,manager,0x8E8,0x1CD570)) {
                    begin_delete(m,now,call,manager);
                    if (tx.destroyed && !obj(m,root,12) && !obj(m,root,0x2C) && !obj(m,root,0x4C)) spawn(m,now,call);
                }
                else if (!manager) spawn(m,now,call);
                else finish(JSRF_CHARACTER_FAILED);
            }
        }
    }
    int status=tx.status;
    if (!tx.phase && !status) status=safe ? JSRF_CHARACTER_READY : JSRF_CHARACTER_UNAVAILABLE;
    int next[JSRF_CHARACTER_WORDS]={status,current,(int)unlocked,tx.target,safe && !tx.phase,0};
    pthread_mutex_lock(&lock);
    next[5]=published[5]+(memcmp(next,published,5*sizeof(int))!=0);
    memcpy(published,next,sizeof next);
    pthread_mutex_unlock(&lock);
}
#else
void jsrf_character_destroyed(uint32_t manager) { (void)manager; }
int jsrf_character_hold_flow(uint32_t flow) { (void)flow; return 0; }
void jsrf_character_tick(uint8_t *ram,double now,int world,JsrfCharacterCall call)
{ (void)ram; (void)now; (void)world; (void)call; atomic_store(&request,-1); }
#endif
