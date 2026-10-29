#include "cn_compat.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef JSRF_CN_PROFILE
#include "cn_profile.h"
static uint64_t fingerprint(const unsigned char *p, size_t n)
{
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    while (n--) h = (h ^ *p++) * UINT64_C(0x100000001b3);
    return h;
}
#endif

int jsrf_cn_header(const unsigned char *x, size_t n)
{
#ifdef JSRF_CN_PROFILE
    return n >= sizeof cn_source_head &&
        (!memcmp(x, cn_source_head, sizeof cn_source_head) ||
         !memcmp(x, cn_target_head, sizeof cn_target_head));
#else
    (void)x; (void)n;
    return 0;
#endif
}

int jsrf_cn_prepare(void **data, size_t size, char *why, size_t whyn)
{
#ifdef JSRF_CN_PROFILE
    unsigned char *out;
    uint64_t h;
    size_t i, pos = 0;
    /* This experimental build's code was generated from the normalized XBE.
     * Reject US/unknown files too, rather than run patched code without its data. */
    if (size != JSRF_CN_SIZE || !jsrf_cn_header(*data, size)) {
        snprintf(why, whyn, "This experimental engine needs the measured Chinese XBE.");
        return -1;
    }
    h = fingerprint(*data, size);
    if (h == JSRF_CN_TARGET_FNV) return 1;
    if (h != JSRF_CN_SOURCE_FNV) {
        snprintf(why, whyn, "Chinese XBE checksum mismatch; import the original measured image.");
        return -1;
    }
    out = malloc(size);
    if (!out) {
        snprintf(why, whyn, "Not enough memory to prepare Chinese game data.");
        return -1;
    }
    memcpy(out, *data, size);
    for (i = 0; i < sizeof cn_patch_runs / sizeof cn_patch_runs[0]; i++) {
        size_t off = cn_patch_runs[i].off, len = cn_patch_runs[i].size;
        if (off > size || len > size-off || pos > sizeof cn_patch_bytes ||
            len > sizeof cn_patch_bytes-pos) {
            free(out);
            snprintf(why, whyn, "Invalid compiled Chinese compatibility profile.");
            return -1;
        }
        memcpy(out+off, cn_patch_bytes+pos, len);
        pos += len;
    }
    if (fingerprint(out, size) != JSRF_CN_TARGET_FNV) {
        free(out);
        snprintf(why, whyn, "Chinese compatibility data verification failed.");
        return -1;
    }
    free(*data);
    *data = out;
    return 1;
#else
    (void)data; (void)size; (void)why; (void)whyn;
    return 0;
#endif
}
