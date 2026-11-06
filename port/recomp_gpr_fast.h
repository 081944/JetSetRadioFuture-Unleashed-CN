#ifndef RECOMP_GPR_FAST_H
#define RECOMP_GPR_FAST_H

/* Forced before generated sources, after enabling their normal type header.
 * Native runtime sources keep the original TLS declarations and aliases. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#if defined(JSRF_GPR_GENERATED) && !defined(RECOMP_GENERATED_CODE)
#define RECOMP_GENERATED_CODE
#endif
#if defined(JSRF_GROUPED_GPR_TLS)
/* Build-local copy installs aliases after the declarations but before any
 * helper bodies, so all C accesses agree about the underlying object. */
#include "recomp_gpr_types.h"
#else
#include "recomp_types.h"
#endif

#endif
