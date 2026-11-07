/* Run on Android against a development RAM capture, never ship that capture. */
#include "../../port/android/map_snapshot.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static float out[JSRF_MAP_FLOATS];
static char caption[JSRF_MAP_CAPTION_BYTES];
int main(int argc,char **argv)
{
    assert(argc==2);
    unsigned char *m=malloc(64u<<20); assert(m);
    FILE *f=fopen(argv[1],"rb"); assert(f);
    assert(fread(m,1,64u<<20,f)==(64u<<20)); fclose(f);
    uint32_t root,flow; memcpy(&root,m+0x22FCE0,4); memcpy(&flow,m+root+(38+9)*4,4);
    uint32_t text; memcpy(&text,m+flow+0x1058,4);
    /* This captured mission's English begins 49 bytes after CN. Use an
     * explicit record pointer, as the guest does, and a known caption. */
    uint32_t english=text+49,record=0x3FFF000;
    memcpy(m+record+24,&english,4);
    jsrf_map_set_objective(m,flow,record);
    jsrf_map_enable(1); jsrf_map_publish(m);
    int seq=jsrf_map_copy(out,JSRF_MAP_FLOATS);
    assert(seq>0 && out[0]==1 && out[3]>0 && out[3]<=JSRF_MAP_MARKERS);
    printf("active stage=%08X state=%.0f markers=%.0f player=(%.3f %.3f %.3f)\n",
        (unsigned)out[1],out[2],out[3],out[4],out[5],out[6]);
    assert(jsrf_map_copy(out,JSRF_MAP_FLOATS-1)==0);
    assert(jsrf_map_objective_copy(caption,sizeof caption));
    assert(!jsrf_map_objective_copy(caption,sizeof caption-1));
    assert(!strcmp(caption,"Cover the graffiti at Shibuya Terminal!"));
    printf("live objective: %s\n",caption);
    jsrf_map_enable(0); usleep(35000); jsrf_map_publish(m);
    assert(jsrf_map_copy(out,JSRF_MAP_FLOATS)==seq);
    uint32_t state=93; memcpy(m+flow+0x5C,&state,4);
    jsrf_map_enable(1); jsrf_map_publish(m); jsrf_map_copy(out,JSRF_MAP_FLOATS);
    assert(out[0]==0 && out[3]==0);
    assert(jsrf_map_objective_copy(caption,sizeof caption) && !caption[0]);
    usleep(35000); memset(m+0x22FCE0,0xff,4); jsrf_map_publish(m); jsrf_map_copy(out,JSRF_MAP_FLOATS);
    assert(out[0]==0 && out[3]==0);
    free(m); puts("PASS: snapshot, disabled state, bounded copy, loading transition, invalid root");
    return 0;
}
