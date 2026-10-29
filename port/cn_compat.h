#ifndef JSRF_CN_COMPAT_H
#define JSRF_CN_COMPAT_H
#include <stddef.h>
/* Active only in a build with a locally generated CN profile. Header-only
 * recognition permits the normal ISO preflight; full checksum is checked
 * before memory mapping. No on-disk game file is rewritten. */
int jsrf_cn_header(const unsigned char *x, size_t n);
int jsrf_cn_prepare(void **data, size_t size, char *why, size_t whyn);
#endif
