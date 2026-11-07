/* Synthetic game-thread harness. Runs the production state machine; guest
 * calls and the next frame's destructor/spawner are deliberately controlled. */
#include "../../port/android/character_switch.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *ram;
enum { ROOT=0x10000, MANAGER=0x30000, PLAYER=0x40000, CAMERA=0x50000,
       FLOW=0x60000, RES=0x70000, SPAWNER=0x80000, MODEL=0x90000, DATA=0xA0000 };
static unsigned loads,deletes,spawns,pending_delete,fail_spawn,defer_destructor,sweeps,fail_sweep;
enum { RECEIVER=0xD0000 };
static const unsigned receiver_offsets[]={0x2D0,0xF00,0x3C8,0x2D4,0x101C};
static void run_spawner(void);
static double now;
static uint32_t u(uint32_t at) { uint32_t v; memcpy(&v,ram+at,4); return v; }
static void w(uint32_t at,uint32_t v) { memcpy(ram+at,&v,4); }
static uint32_t object(unsigned id) { return ROOT+0x98+id*4; }
static uint32_t variable(unsigned id) { return ROOT+0x7868+id*4; }
static void resource(unsigned id)
{
    uint32_t rec=0xB0000+id*0x660;
    w(rec+8,id); w(rec+0x640,MODEL); w(rec+0x644,DATA);
    w(rec+0x65C,u(RES+0x1E4)); w(RES+0x1E4,rec);
}
static void make_player(unsigned id)
{
    w(object(12),MANAGER); w(MANAGER,0x1CD570); w(MANAGER+4,0);
    w(MANAGER+0x8D0,SPAWNER+0x544);
    w(MANAGER+0x8D4,0x2C); w(MANAGER+0x8D8,0x4C);
    w(object(0x2C),PLAYER); w(PLAYER,0x1CCFF8); w(PLAYER+4,0);
    w(PLAYER+0xFBC,id);
    /* The original receiver allocator only initializes 0x34 and 0x38.
     * Simulate recycled geometry spanning an unrelated scene volume. */
    for (unsigned i=0;i<5;i++) {
        uint32_t r=RECEIVER+i*0x100;
        w(PLAYER+receiver_offsets[i],r);
        memset(ram+r,0xA5,0x68);
        w(r+0x30,0x123456); w(r+0x34,0x654321);
        w(r+0x38,0); w(r+0x40,0xCAFEBABE);
    }
    w(PLAYER+0x2A4,MODEL); w(PLAYER+0xC90,MODEL); w(PLAYER+0x58,DATA);
    memcpy(ram+PLAYER+0xCA4,ram+SPAWNER+0x544+12,12);
    memcpy(ram+PLAYER+0xCB0,ram+PLAYER+0xCA4,12);
    memcpy(ram+PLAYER+0xCBC,ram+SPAWNER+0x544+24,12);
    w(object(0x4C),CAMERA); w(CAMERA,0x1CD518); w(CAMERA+4,0); w(CAMERA+0x24,PLAYER);
}
static int call(uint32_t va,unsigned count,const uint32_t *args,uint32_t *result)
{
    if (va==0x256A0) {
        assert(count==2 && args[0]==2 && args[1]<24 && !u(RES+0x44));
        loads++;
        /* Existing records return immediately, otherwise async work follows. */
        uint32_t rec=0xB0000+args[1]*0x660;
        w(RES+0x44,u(rec+0x640) ? 0 : 1);
        *result=1;
    } else if (va==0x13FC0) {
        assert(count==1 && args[0]==12);
        assert(u(object(12))==MANAGER && !u(RES+0x44));
        deletes++; pending_delete=1;
        w(MANAGER+4,0x80000000); w(PLAYER+4,0x80000000); w(CAMERA+4,0x80000000);
        w(object(12),0); w(object(0x2C),0); w(object(0x4C),0);
        *result=0;
    } else if (va==0x11B90) {
        assert(count==2 && args[0]==MANAGER && args[1]==u(MANAGER+0x28));
        *result=0;
    } else if (va==0xA6AF0) {
        assert(count==2 && args[0]==MANAGER && args[1]==1 && pending_delete);
        if (!defer_destructor) { pending_delete=0; jsrf_character_destroyed(MANAGER); }
        *result=0;
    } else if (va==0x6F890) {
        assert(count==4);
        unsigned slot=(args[1]-RECEIVER)/0x100;
        assert(slot<3);
        const uint32_t owners[]={0x20CC58,0x1EBABC,0x1EC050};
        assert(args[0]==owners[slot] && args[1]==RECEIVER+slot*0x100);
        assert(args[2]==PLAYER+0xCB0 && args[3]==PLAYER+0xCA4);
        if (fail_sweep) { fail_sweep--; return 0; }
        memcpy(ram+RECEIVER,ram+args[2],12);
        memcpy(ram+RECEIVER+0x20,ram+args[3],12);
        /* Real sub_14B910 normalizes a zero segment, yielding NaN. */
        w(args[1]+0x10,0x7FC00000); w(args[1]+0x14,0x7FC00000); w(args[1]+0x18,0x7FC00000);
        w(args[1]+0x38,0); sweeps++; *result=0;
    } else {
        assert(va==0xA8370 && count==1 && args[0]==SPAWNER);
        assert(!pending_delete); run_spawner(); *result=0;
    }
    return 1;
}
static void tick(void) { jsrf_character_tick(ram,now,1,call); now+=0.1; }
static int status(void) { int s[6]; jsrf_character_snapshot(s); return s[0]; }
static void run_spawner(void)
{
    uint32_t cfg=u(variable(0x6D));
    if (cfg) {
        assert(cfg==SPAWNER+0x544);
        assert(!u(object(12)));
        if (fail_spawn) fail_spawn--;
        else make_player(u(variable(0x2D)));
        spawns++; w(variable(0x6D),0);
    }
}
static void next_frame(void)
{
    if (pending_delete) { pending_delete=0; jsrf_character_destroyed(MANAGER); }
    run_spawner(); tick();
}
static void init(void)
{
    ram=calloc(1,64u<<20); assert(ram);
    w(0x22FCE0,ROOT); w(0x1EFFC4,0x46);
    w(object(9),FLOW); w(FLOW,0x1CAAB0); w(FLOW+0x5C,15);
    w(FLOW+0x58,0x10009); w(FLOW+0x104C,0x10000);
    w(object(7),RES); w(RES,0x1C4F68);
    w(object(11),SPAWNER); w(SPAWNER,0x1CD5C8);
    w(SPAWNER+0x544+12,0x42C80000); /* 100.0 */
    w(SPAWNER+0x544+16,0x43480000); /* 200.0 */
    w(SPAWNER+0x544+20,0x43960000); /* 300.0 */
    w(SPAWNER+0x544+28,12345);
    w(variable(0xD),6); w(variable(0x2D),6); w(variable(0xAD),4);
    resource(6); make_player(6); tick();
}
static void complete_load(unsigned id) { resource(id); w(RES+0x44,0); }
static void switch_to(unsigned id)
{
    assert(jsrf_character_request((int)id)); tick();
    if (u(RES+0x44)) complete_load(id);
    tick(); assert(deletes && jsrf_character_hold_flow(FLOW));
    assert(jsrf_character_blocks_input());
    /* The replacement exists before another ordinary world update. */
    assert(u(object(12))==MANAGER && u(PLAYER+0xFBC)==id && !u(variable(0x6D)));
    /* Before any world frame can test scene volumes, both endpoints must be
     * the new player's actual position, without corrupting the list. */
    for (unsigned i=0;i<5;i++) {
        uint32_t r=RECEIVER+i*0x100;
        assert(!memcmp(ram+r,ram+PLAYER+0xCA4,12));
        assert(!memcmp(ram+r+0x20,ram+PLAYER+0xCA4,12));
        assert(!u(r+0x10) && !u(r+0x14) && !u(r+0x18));
        assert(u(r+0x30)==0x123456 && u(r+0x34)==0x654321);
        if(i<3) assert(!u(r+0x38));
        assert(u(r+0x40)==0xCAFEBABE);
    }
    next_frame(); assert(status()==JSRF_CHARACTER_DONE);
    assert(!jsrf_character_hold_flow(FLOW));
    assert(!jsrf_character_blocks_input());
    assert(u(PLAYER+0xFBC)==id && u(CAMERA+0x24)==PLAYER);
    assert(u(variable(0xD))==id && u(variable(0x2D))==id);
    assert(u(PLAYER+0xCA4)==0x42C80000 && u(PLAYER+0xCC0)==12345);
}
int main(int argc,char **argv)
{
    assert(argc==2 || argc==3); init();
    if (argc==3) {
        assert(!strcmp(argv[1],"capture"));
        FILE *f=fopen(argv[2],"rb"); assert(f);
        assert(fread(ram,1,64u<<20,f)==(64u<<20)); fclose(f);
        uint32_t root=u(0x22FCE0),flow=u(root+0x98+9*4);
        /* Test fixture only: the capture was taken with the map paused.
         * Changing its copied state is not a live resume or guest execution. */
        w(flow+0x5C,15); tick();
        int state[6]; jsrf_character_snapshot(state);
        assert(state[4] && state[1]==6 && state[2]==0x46);
        jsrf_character_request(6); tick();
        assert(status()==JSRF_CHARACTER_DONE && !loads && !deletes && !spawns);
        free(ram); puts("PASS: captured CN object layout and current-character no-op"); return 0;
    }
    if (!strcmp(argv[1],"roundtrip")) {
        switch_to(1); switch_to(6); switch_to(2); switch_to(1); switch_to(6);
        assert(spawns==5 && loads==5 && deletes==5 && sweeps==15);
    } else if (!strcmp(argv[1],"sweep_failure")) {
        jsrf_character_request(1); tick(); complete_load(1); fail_sweep=1; tick();
        next_frame(); assert(status()!=JSRF_CHARACTER_DONE);
        now=10; tick(); next_frame(); next_frame();
        assert(status()==JSRF_CHARACTER_RESTORED && u(PLAYER+0xFBC)==6);
    } else if (!strcmp(argv[1],"locked")) {
        assert(!jsrf_character_request(-1) && !jsrf_character_request(24));
        jsrf_character_request(0); tick(); assert(status()==JSRF_CHARACTER_LOCKED && !loads && !deletes);
    } else if (!strcmp(argv[1],"unsafe")) {
        w(FLOW+0x5C,110); jsrf_character_request(1); tick();
        assert(status()==JSRF_CHARACTER_UNSAFE && !loads && !deletes);
    } else if (!strcmp(argv[1],"load_timeout")) {
        jsrf_character_request(1); tick(); tick(); assert(loads==1 && !deletes);
        now=31; tick(); assert(status()==JSRF_CHARACTER_FAILED && u(PLAYER+0xFBC)==6);
    } else if (!strcmp(argv[1],"scene_change")) {
        jsrf_character_request(1); tick(); w(FLOW+0x104C,0x20000); tick();
        assert(status()==JSRF_CHARACTER_SCENE_CHANGED && !deletes && !jsrf_character_hold_flow(FLOW));
    } else if (!strcmp(argv[1],"restore")) {
        jsrf_character_request(1); tick(); complete_load(1); fail_spawn=1; tick();
        assert(u(variable(0x2D))==6 && u(object(12))==MANAGER && u(PLAYER+0xFBC)==6);
        next_frame(); assert(status()==JSRF_CHARACTER_RESTORED && u(PLAYER+0xFBC)==6);
    } else if (!strcmp(argv[1],"latest")) {
        jsrf_character_request(1); jsrf_character_request(2); tick();
        assert(loads==1); complete_load(2); tick(); next_frame(); next_frame();
        assert(status()==JSRF_CHARACTER_DONE && u(PLAYER+0xFBC)==2);
    } else if (!strcmp(argv[1],"incomplete")) {
        jsrf_character_request(1); tick(); w(RES+0x44,0); tick();
        assert(status()==JSRF_CHARACTER_FAILED && loads==1 && !deletes);
    } else if (!strcmp(argv[1],"same")) {
        jsrf_character_request(6); tick(); assert(status()==JSRF_CHARACTER_DONE && !loads && !deletes);
    } else if (!strcmp(argv[1],"pending_after_commit")) {
        jsrf_character_request(1); tick(); complete_load(1); tick();
        jsrf_character_request(6); next_frame();
        assert(u(PLAYER+0xFBC)==1); tick(); tick(); next_frame();
        assert(status()==JSRF_CHARACTER_DONE && u(PLAYER+0xFBC)==6);
    } else if (!strcmp(argv[1],"delayed_destructor")) {
        jsrf_character_request(1); tick(); complete_load(1); defer_destructor=1; tick();
        jsrf_character_destroyed(0xDEAD); tick(); assert(!spawns && !u(variable(0x6D)));
        now=10; tick();
        assert(status()==JSRF_CHARACTER_FAILED && jsrf_character_hold_flow(FLOW));
        assert(!u(variable(0x6D)));
        next_frame(); next_frame(); assert(status()==JSRF_CHARACTER_DONE);
    } else if (!strcmp(argv[1],"wrong_bindings")) {
        jsrf_character_request(1); tick(); complete_load(1); tick();
        /* Emulate construction with the right ID but an old/wrong model. */
        w(PLAYER+0x2A4,MODEL+4); tick();
        assert(status()!=JSRF_CHARACTER_DONE);
        now=10; tick(); /* delete this incomplete replacement, then restore */
        next_frame(); next_frame(); assert(status()==JSRF_CHARACTER_RESTORED);
    } else assert(!"unknown test");
    free(ram); printf("PASS %s\n",argv[1]); return 0;
}
