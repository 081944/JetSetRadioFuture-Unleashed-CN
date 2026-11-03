/*
 * GLSL to SPIR-V, at run time, on the phone.
 *
 * The renderer does not have shaders written out anywhere: it builds GLSL from
 * the title's register state as it goes (nv2a_psh.c, nv2a_vsh.c) and hands the
 * source to a backend. OpenGL eats that directly. Vulkan does not -- it takes
 * SPIR-V -- so one of two things has to happen: the source is compiled at run
 * time, or the set of sources is enumerated ahead of time and precompiled.
 *
 * Runtime, with glslang linked in, is the one that fits what is already there:
 * there is a shader cache keyed by state (nv2a_shader_cache.h), so a program
 * compiles once and is reused, exactly as the cached GL programs are. The cost
 * is binary size (glslang is a C++ library; see prebuilt/README.md) and the
 * compile itself, on the frame that first needs the program -- the same moment
 * OpenGL drove its own compiler.
 *
 * This file is the bridge only: source in, SPIR-V words out. Nothing calls it
 * yet; RECOMP_VK_SPIRV_TEST=1 compiles a small pair on start and says how many
 * words came out, which is what proves the bridge on a driver before any of
 * the renderer depends on it.
 */
#if defined(__ANDROID__)

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Include/glslang_c_shader_types.h>
#include <glslang/Public/resource_limits_c.h>

#include "../src/nv2a_gl/jsrf_debug_env.h"

static void sh_say(const char *fmt, ...)
{
    char line[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    jsrf_dbg_log("[VKS] %s\n", line);
}

static int sh_init;

/* GLSL source of `stage` (0 vertex, 1 fragment), as this build generates it,
 * compiled to SPIR-V. On success `*words` is malloc'ed and the caller frees
 * it. 0 and a logged reason on failure. */
int nv2a_vk_glsl_to_spirv(const char *glsl, int stage, uint32_t **words,
                          size_t *nwords)
{
    glslang_input_t in;
    glslang_shader_t *shader;
    glslang_program_t *program;
    unsigned int *spv;
    size_t n;

    if (!sh_init) { glslang_initialize_process(); sh_init = 1; }
    memset(&in, 0, sizeof in);
    in.language = GLSLANG_SOURCE_GLSL;
    in.stage = stage ? GLSLANG_STAGE_FRAGMENT : GLSLANG_STAGE_VERTEX;
    in.client = GLSLANG_CLIENT_VULKAN;
    in.client_version = GLSLANG_TARGET_VULKAN_1_3;
    in.target_language = GLSLANG_TARGET_SPV;
    in.target_language_version = GLSLANG_TARGET_SPV_1_6;
    in.code = glsl;
    in.default_version = 100;
    in.default_profile = GLSLANG_NO_PROFILE;
    in.messages = GLSLANG_MSG_DEFAULT_BIT;
    in.resource = glslang_default_resource();

    shader = glslang_shader_create(&in);
    if (!shader) { sh_say("shader_create failed"); return 0; }
    if (!glslang_shader_preprocess(shader, &in)) {
        sh_say("preprocess: %s", glslang_shader_get_info_log(shader));
        glslang_shader_delete(shader);
        return 0;
    }
    if (!glslang_shader_parse(shader, &in)) {
        sh_say("parse: %s", glslang_shader_get_info_log(shader));
        glslang_shader_delete(shader);
        return 0;
    }
    program = glslang_program_create();
    if (!program) { glslang_shader_delete(shader); sh_say("program_create failed"); return 0; }
    glslang_program_add_shader(program, shader);
    if (!glslang_program_link(program, GLSLANG_MSG_VULKAN_RULES_BIT)) {
        sh_say("link: %s", glslang_program_SPIRV_get_messages(program));
        glslang_program_delete(program);
        glslang_shader_delete(shader);
        return 0;
    }
    glslang_program_SPIRV_generate(program, in.stage);
    n = glslang_program_SPIRV_get_size(program);
    spv = glslang_program_SPIRV_get_ptr(program);
    if (!n || !spv) {
        sh_say("no SPIR-V out");
        glslang_program_delete(program);
        glslang_shader_delete(shader);
        return 0;
    }
    *nwords = n;
    *words = (uint32_t *)malloc(n * sizeof *words);
    if (*words) memcpy(*words, spv, n * sizeof *words);
    glslang_program_delete(program);
    glslang_shader_delete(shader);
    return *words != NULL;
}

/* A pair small enough to read, and the sort of thing the renderer generates:
 * a position through a model-view-projection matrix, and a colour out. */
int nv2a_vk_spirv_test(void)
{
    static const char *vs =
        "#version 450\n"
        "layout(location = 0) in vec3 a_pos;\n"
        "layout(set = 0, binding = 0) uniform U { mat4 mvp; } u;\n"
        "void main() { gl_Position = u.mvp * vec4(a_pos, 1.0); }\n";
    static const char *fs =
        "#version 450\n"
        "layout(location = 0) out vec4 o;\n"
        "void main() { o = vec4(1.0, 0.5, 0.25, 1.0); }\n";
    uint32_t *w = NULL;
    size_t n = 0;
    int ok = 0;

    if (nv2a_vk_glsl_to_spirv(vs, 0, &w, &n)) {
        sh_say("vertex: %u words of SPIR-V", (unsigned)n);
        free(w);
        w = NULL;
        ok++;
    }
    if (nv2a_vk_glsl_to_spirv(fs, 1, &w, &n)) {
        sh_say("fragment: %u words of SPIR-V", (unsigned)n);
        free(w);
        ok++;
    }
    sh_say(ok == 2 ? "OK: glslang compiles this build's GLSL to SPIR-V at run time"
                   : "glslang could not compile the test pair");
    return ok == 2 ? 0 : 1;
}

#else

int  nv2a_vk_glsl_to_spirv(const char *g, int s, uint32_t **w, size_t *n)
{ (void)g; (void)s; (void)w; (void)n; return 0; }
int  nv2a_vk_spirv_test(void) { return 0; }

#endif