#include "input/controller_mapping.h"
#include <stdio.h>
#include <string.h>

int main(void)
{
    /* Destination channels read these physical sources in each layout. */
    static const unsigned source[4][8] = {
        {0, 1, 2, 3, 4, 5, 6, 7},
        {0, 1, 2, 3, 6, 7, 4, 5},
        {1, 0, 3, 2, 4, 5, 6, 7},
        {1, 0, 3, 2, 6, 7, 4, 5}
    };
    unsigned flags, pressure, i;
    for (flags = 0; flags < 4; flags++) {
        for (pressure = 0; pressure < 256; pressure++) {
            unsigned char physical[8], mapped[8];
            for (i = 0; i < 8; i++) physical[i] = (unsigned char)(pressure + i * 31);
            memcpy(mapped, physical, sizeof mapped);
            jsrf_controller_map(mapped, flags);
            for (i = 0; i < 8; i++) {
                if (mapped[i] != physical[source[flags][i]]) {
                    fprintf(stderr, "Mapping failed: flags=%u channel=%u pressure=%u\n", flags, i, pressure);
                    return 1;
                }
            }
            jsrf_controller_map(mapped, flags);
            if (memcmp(mapped, physical, sizeof mapped)) return 1;
        }
    }
    puts("All four controller layouts preserve button states and 0-255 pressure");
    return 0;
}
