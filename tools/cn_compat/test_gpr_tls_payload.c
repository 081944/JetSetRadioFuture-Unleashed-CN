#if defined(GPR_LEGACY)
#include "recomp_types.h"
#define STEP legacy_step
#define RESET legacy_reset
#define ADDRESS legacy_address
#define BRIDGE legacy_bridge_roundtrip
#define OTHER grouped_step
#else
#include "recomp_gpr_fast.h"
#define STEP grouped_step
#define RESET grouped_reset
#define ADDRESS grouped_address
#define BRIDGE grouped_bridge_roundtrip
#define OTHER legacy_step
#endif

uint32_t *ADDRESS(unsigned i)
{
    uint32_t *p[] = {&g_eax,&g_ecx,&g_edx,&g_esp,&g_ebx,&g_esi,&g_edi,&g_ebp};
    return p[i];
}

void RESET(uint32_t seed)
{
    g_eax=seed;g_ecx=seed+1;g_edx=seed+2;g_esp=seed+3;
    g_ebx=seed+4;g_esi=seed+5;g_edi=seed+6;g_ebp=seed+7;
}

/* Separate translation units, like generated code and the native bridge.
 * No inlining or link-time optimization may erase the actual TLS accesses. */
__attribute__((noinline)) uint32_t STEP(uint32_t value)
{
    g_eax+=value;g_ecx^=g_eax;g_edx+=g_ecx*3u;g_esp^=g_edx;
    g_ebx+=g_esp;g_esi^=g_ebx;g_edi+=g_esi*7u;g_ebp^=g_edi;
    return g_eax+g_ecx+g_edx+g_esp+g_ebx+g_esi+g_edi+g_ebp;
}

extern uint32_t OTHER(uint32_t);
uint32_t BRIDGE(uint32_t value)
{
    g_eax=value;
    OTHER(1);
    return g_eax; /* Caller must observe the other TU's aliased write. */
}
