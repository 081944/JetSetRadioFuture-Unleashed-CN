/* Compatibility entry point for the shared NV2A front end. The renderer
 * lives with its Vulkan device in vk_engine.c / vk_game.inc. */
#if defined(__ANDROID__)
#include "../src/nv2a_gl/nv2a_backend.h"
extern const Nv2aGfx *nv2a_vk_game_gfx(void);
const Nv2aGfx *nv2a_vk_stub_gfx(void) { return nv2a_vk_game_gfx(); }
#else
const void *nv2a_vk_stub_gfx(void) { return 0; }
#endif
