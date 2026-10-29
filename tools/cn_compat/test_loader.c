/* Compile against port/cn_compat.c, xbe_check.c and ios/xiso.c. */
#include "cn_compat.h"
#include "xbe_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *load(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    void *p;
    if (!f || fseek(f, 0, SEEK_END)) exit(2);
    *n = (size_t)ftell(f);
    rewind(f);
    p = malloc(*n);
    if (!p || fread(p, 1, *n, f) != *n) exit(2);
    fclose(f);
    return p;
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)
int main(int argc, char **argv)
{
    void *cn, *us, *expected, *p;
    size_t n, un, en;
    char why[512];
    if (argc != 4 && argc != 6) return 2;
    cn = load(argv[1], &n); us = load(argv[2], &un); expected = load(argv[3], &en);
#ifdef JSRF_CN_PROFILE
    if (argc == 6) CHECK(jsrf_xbe_check_iso(argv[4], why, sizeof why) == JSRF_XBE_OK);
    CHECK(jsrf_cn_header(cn, 4096));
    CHECK(!jsrf_cn_header(cn, 4095));
    CHECK(!jsrf_cn_header(us, un));
    CHECK(jsrf_xbe_check(us, un, why, sizeof why) == JSRF_XBE_OTHER);
    CHECK(jsrf_xbe_check(cn, 4096, why, sizeof why) == JSRF_XBE_OK);
    CHECK(jsrf_xbe_check(expected, en, why, sizeof why) == JSRF_XBE_OK);
    p = malloc(n); CHECK(p); memcpy(p, cn, n);
    ((unsigned char *)p)[0x1000] ^= 1;
    CHECK(jsrf_cn_prepare(&p, n, why, sizeof why) == -1);
    CHECK(((unsigned char *)p)[0x1000] != ((unsigned char *)cn)[0x1000]);
    free(p);
    CHECK(jsrf_cn_prepare(&us, un, why, sizeof why) == -1);
    CHECK(jsrf_cn_prepare(&cn, n-1, why, sizeof why) == -1);
    CHECK(jsrf_cn_prepare(&cn, n, why, sizeof why) == 1);
    CHECK(n == en && !memcmp(cn, expected, n));
    p = cn;
    CHECK(jsrf_cn_prepare(&cn, n, why, sizeof why) == 1 && cn == p);
#else
    if (argc == 6) CHECK(jsrf_xbe_check_iso(argv[4], why, sizeof why) == JSRF_XBE_OTHER);
    CHECK(!jsrf_cn_header(cn, n));
    CHECK(jsrf_xbe_check(cn, n, why, sizeof why) == JSRF_XBE_OTHER);
    CHECK(jsrf_xbe_check(us, un, why, sizeof why) == JSRF_XBE_OK);
    p = cn;
    CHECK(jsrf_cn_prepare(&cn, n, why, sizeof why) == 0 && cn == p);
#endif
    if (argc == 6) CHECK(jsrf_xbe_check_iso(argv[5], why, sizeof why) ==
#ifdef JSRF_CN_PROFILE
                        JSRF_XBE_OTHER
#else
                        JSRF_XBE_OK
#endif
                        );
    free(cn); free(us); free(expected);
    puts("Loader checks passed");
    return 0;
}
