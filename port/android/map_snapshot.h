#ifndef JSRF_MAP_SNAPSHOT_H
#define JSRF_MAP_SNAPSHOT_H
#include <stdint.h>
#define JSRF_MAP_MARKERS 1024
#define JSRF_MAP_FLOATS (16 + JSRF_MAP_MARKERS * 6)
#define JSRF_MAP_CAPTION_BYTES 257
#ifdef __ANDROID__
/* Publish only on the guest's frame thread; Java never reads guest RAM. */
void jsrf_map_publish(const uint8_t *ram);
void jsrf_map_enable(int enabled);
int jsrf_map_copy(float *out, unsigned capacity);
int jsrf_map_objective_copy(char *out, unsigned capacity);
void jsrf_map_set_objective(const uint8_t *ram,uint32_t flow,uint32_t record);
#else
static inline void jsrf_map_publish(const uint8_t *ram) { (void)ram; }
static inline void jsrf_map_set_objective(const uint8_t *ram,uint32_t flow,uint32_t record)
{ (void)ram; (void)flow; (void)record; }
#endif
#endif
