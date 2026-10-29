#ifndef JSRF_CONTROLLER_MAPPING_H
#define JSRF_CONTROLLER_MAPPING_H

enum {
    JSRF_PAD_SWAP_TRIGGERS = 1u,
    JSRF_PAD_NINTENDO = 2u
};

/* Physical SDL pad only, before merging keyboard/touch. Preserve analog
 * trigger pressure when exchanging Black/White (LB/RB) with LT/RT. */
static inline void jsrf_controller_map(unsigned char analog[8], unsigned flags)
{
    unsigned char tmp;
    if (flags & JSRF_PAD_SWAP_TRIGGERS) {
        tmp = analog[4]; analog[4] = analog[6]; analog[6] = tmp;
        tmp = analog[5]; analog[5] = analog[7]; analog[7] = tmp;
    }
    if (flags & JSRF_PAD_NINTENDO) {
        tmp = analog[0]; analog[0] = analog[1]; analog[1] = tmp;
        tmp = analog[2]; analog[2] = analog[3]; analog[3] = tmp;
    }
}

#ifdef __ANDROID__
void xbox_InputSetControllerMapping(unsigned flags);
#endif
#endif
