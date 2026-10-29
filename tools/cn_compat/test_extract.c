/* Integration driver for the same ISO extractor called by Android JNI. */
#include "ios/xiso.h"
#include "xbe_check.h"
#include <stdio.h>
int main(int argc, char **argv)
{
    char why[512];
    if (argc != 3) return 2;
    if (jsrf_xbe_check_iso(argv[1], why, sizeof why) != JSRF_XBE_OK ||
        xiso_extract(argv[1], argv[2], NULL, NULL, why, sizeof why)) {
        fprintf(stderr, "%s\n", why);
        return 1;
    }
    puts("ISO preflight and native extraction passed");
    return 0;
}
