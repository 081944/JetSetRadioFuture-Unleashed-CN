#ifndef JSRF_CHARACTER_SWITCH_H
#define JSRF_CHARACTER_SWITCH_H
#include <stdint.h>

enum {
    JSRF_CHARACTER_UNAVAILABLE, JSRF_CHARACTER_READY, JSRF_CHARACTER_LOADING,
    JSRF_CHARACTER_REBUILDING, JSRF_CHARACTER_DONE, JSRF_CHARACTER_UNSAFE,
    JSRF_CHARACTER_LOCKED, JSRF_CHARACTER_FAILED, JSRF_CHARACTER_RESTORED,
    JSRF_CHARACTER_SCENE_CHANGED
};
/* Snapshot: status, current ID, unlock bits, requested ID, can request, sequence. */
#define JSRF_CHARACTER_WORDS 6
typedef int (*JsrfCharacterCall)(uint32_t va, unsigned count,
                               const uint32_t *args, uint32_t *result);
#if defined(__ANDROID__) || defined(JSRF_CHARACTER_TEST)
int jsrf_character_request(int id);
void jsrf_character_snapshot(int out[JSRF_CHARACTER_WORDS]);
void jsrf_character_tick(uint8_t *ram, double now, int world_available, JsrfCharacterCall call);
void jsrf_character_destroyed(uint32_t manager);
int jsrf_character_hold_flow(uint32_t flow);
int jsrf_character_blocks_input(void);
#else
static inline void jsrf_character_destroyed(uint32_t manager) { (void)manager; }
static inline int jsrf_character_hold_flow(uint32_t flow) { (void)flow; return 0; }
static inline int jsrf_character_blocks_input(void) { return 0; }
#endif
#endif
