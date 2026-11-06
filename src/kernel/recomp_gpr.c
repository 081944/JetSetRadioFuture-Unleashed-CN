#include "recomp_gpr.h"
#include <stddef.h>

#if !defined(__ANDROID__) || !defined(__aarch64__)
#error Grouped guest register TLS requires Android AArch64 ELF.
#endif

__thread RecompGpr g_recomp_gpr;

/* Keep these offsets checked against the C layout: other runtime objects
 * still declare and access the original independent __thread symbols. */
#define CHECK_FIELD(field, offset) _Static_assert(offsetof(RecompGpr, field) == offset, "guest GPR TLS layout")
CHECK_FIELD(r_eax, 0); CHECK_FIELD(r_ecx, 4); CHECK_FIELD(r_edx, 8); CHECK_FIELD(r_esp, 12);
CHECK_FIELD(r_ebx, 16); CHECK_FIELD(r_esi, 20); CHECK_FIELD(r_edi, 24); CHECK_FIELD(r_ebp, 28);
_Static_assert(sizeof(RecompGpr) == 32, "guest GPR TLS size");
#undef CHECK_FIELD

#define TLS_ALIAS(name, offset) \
    ".global " #name "\n.type " #name ", %tls_object\n" \
    ".set " #name ", g_recomp_gpr + " #offset "\n.size " #name ", 4\n"
__asm__(TLS_ALIAS(g_eax, 0) TLS_ALIAS(g_ecx, 4) TLS_ALIAS(g_edx, 8)
        TLS_ALIAS(g_esp, 12) TLS_ALIAS(g_ebx, 16) TLS_ALIAS(g_esi, 20)
        TLS_ALIAS(g_edi, 24) TLS_ALIAS(g_ebp, 28));
#undef TLS_ALIAS
