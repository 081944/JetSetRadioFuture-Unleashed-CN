#ifndef RECOMP_GPR_H
#define RECOMP_GPR_H

#include <stdint.h>

/* One dynamic TLS address for the generated code's eight integer registers.
 * The old individual ELF TLS symbols alias these fields (recomp_gpr.c), so
 * native bridges, callbacks, and register snapshots keep their existing ABI. */
typedef struct {
    uint32_t r_eax, r_ecx, r_edx, r_esp, r_ebx, r_esi, r_edi, r_ebp;
} RecompGpr;

extern __thread RecompGpr g_recomp_gpr __attribute__((visibility("hidden")));

#endif
