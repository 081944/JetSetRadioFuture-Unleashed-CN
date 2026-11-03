/*
 * A backend that does nothing, so the engine can run on Vulkan before the
 * Vulkan backend can draw.
 *
 * The engine asks its backend for render targets, textures, programs,
 * uniforms, state, vertices, draws and a present, and everything above that
 * line -- the pushbuffer, surfaces, caches, the frame rules -- is backend
 * neutral (nv2a_backend.h). Metal proved the line was real. This is the same
 * move again: a table that answers every call and draws nothing, with the
 * window and the swapchain owned by port/vk_engine.c instead.
 *
 * It exists to answer one question at a time: does the engine boot with a
 * non-GL backend on Android, take the window with SDL_WINDOW_VULKAN, and run
 * its frame loop with Vulkan presenting? Drawing comes after.
 */
#if defined(__ANDROID__)

#include <stddef.h>
#include <stdint.h>

#include "../../src/nv2a_gl/nv2a_backend.h"

static int      stub_init(void) { return 1; }
/* The render target is real: nv2a_gl.c's surface_bind asks for one when the
 * title binds a surface, and its size and its pixels are what the frame
 * boundary hands back to the title (port/vk_engine.c). */
extern uint32_t nv2a_vk_target_new(uint32_t pw, uint32_t ph);
extern int      nv2a_vk_target_read(uint32_t rt, uint32_t x, uint32_t y,
                                    uint32_t w, uint32_t h, int bgra, void *out);
static uint32_t stub_target_new(uint32_t pw, uint32_t ph)
{ return nv2a_vk_target_new(pw, ph); }
static void     stub_target_free(uint32_t rt) { (void)rt; }
static void     stub_target_bind(uint32_t rt) { (void)rt; }
static uint32_t stub_target_texture(uint32_t rt) { (void)rt; return 1; }
static uint32_t stub_target_snapshot(uint32_t rt) { (void)rt; return 1; }
static int      stub_target_read(uint32_t rt, uint32_t x, uint32_t y, uint32_t w,
                                 uint32_t h, int bgra, void *out)
{ return nv2a_vk_target_read(rt, x, y, w, h, bgra, out); }
static int      stub_target_read_depth(uint32_t rt, uint32_t pw, uint32_t ph, float *out)
{ (void)rt; (void)pw; (void)ph; (void)out; return 0; }

static uint32_t stub_tex_new(uint32_t w, uint32_t h, const void *rgba, const Nv2aSampler *s)
{ (void)w; (void)h; (void)rgba; (void)s; return 1; }
static void     stub_tex_free(uint32_t tex) { (void)tex; }
static int      stub_tex_read(uint32_t tex, uint32_t w, uint32_t h, void *bgra)
{ (void)tex; (void)w; (void)h; (void)bgra; return 0; }

static uint32_t stub_prog_new(const char *vsrc, const char *fsrc, const char *what)
{ (void)vsrc; (void)fsrc; (void)what; return 1; }
static void     stub_prog_free(uint32_t prog) { (void)prog; }
static int      stub_prog_uses(uint32_t prog, int id) { (void)prog; (void)id; return 1; }
static void     stub_prog_use(uint32_t prog) { (void)prog; }

static void     stub_uniform_f(int id, const float *v, int n) { (void)id; (void)v; (void)n; }
static void     stub_uniform_i(int id, int v) { (void)id; (void)v; }
static void     stub_texture(int unit, uint32_t tex) { (void)unit; (void)tex; }
static void     stub_state(const Nv2aRenderState *rs) { (void)rs; }
static void     stub_state_invalidate(void) { }
static void     stub_vertices(const float *v, uint32_t n, uint32_t stride,
                              const int attr_at[16], uint32_t mask,
                              const float const_attr[16][4])
{ (void)v; (void)n; (void)stride; (void)attr_at; (void)mask; (void)const_attr; }
static void     stub_draw(int prim, uint32_t count) { (void)prim; (void)count; }
static void     stub_clear(unsigned mask, const float rgba[4], float depth,
                           uint32_t stencil, const uint32_t rect[4])
{ (void)mask; (void)rgba; (void)depth; (void)stencil; (void)rect; }

/* Presenting is not this table's job: nv2a_gl.c's present() hands the frame to
 * port/vk_engine.c when the Vulkan backend is active, so a call here would put
 * the same frame up twice. */
static void     stub_present(uint32_t rt, uint32_t pw, uint32_t ph)
{ (void)rt; (void)pw; (void)ph; }

static void     stub_report(void) { }

static const Nv2aGfx g_vk_stub = {
    "Vulkan (window and swapchain; drawing not yet)",
    NV2A_LANG_GLSL,
    1,
    stub_init,
    stub_target_new, stub_target_free, stub_target_bind,
    stub_target_texture, stub_target_snapshot, stub_target_read,
    stub_target_read_depth,
    stub_tex_new, stub_tex_free, stub_tex_read,
    stub_prog_new, stub_prog_free, stub_prog_uses, stub_prog_use,
    stub_uniform_f, stub_uniform_i,
    stub_texture, stub_state, stub_state_invalidate,
    stub_vertices, stub_draw,
    stub_clear,
    stub_present,
    stub_report
};

const Nv2aGfx *nv2a_vk_stub_gfx(void) { return &g_vk_stub; }

#else

const void *nv2a_vk_stub_gfx(void) { return 0; }

#endif