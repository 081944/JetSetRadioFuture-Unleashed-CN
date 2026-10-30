/* Debug switches that can be reached on Android.
 *
 * The backend reads its diagnostics from the environment -- RECOMP_GL_PROFILE,
 * RECOMP_PACE, RECOMP_GL_TEXCHECK_KB and a hundred more -- which is how they
 * are set on a desktop. An Android app inherits zygote's environment, so on a
 * phone there is no way to set one without rebuilding.
 *
 * Every RECOMP_* switch is therefore also readable as a system property:
 *
 *     adb shell setprop debug.jsrf.recomp_gl_profile 1
 *     adb shell am force-stop com.jumatt.jsrfunleashed   # switches are latched
 *
 * The lower-cased name after `debug.jsrf.` is the switch. The environment wins
 * when both are set, so a desktop run is unaffected.
 *
 * Lookups are cheap enough for the once-per-process callers here (each caches
 * the answer in a static), and jsrf_getenv() returns a pointer into a small
 * ring of buffers, so do not hold the result across another lookup.
 */
#ifndef JSRF_DEBUG_ENV_H
#define JSRF_DEBUG_ENV_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __ANDROID__
#include <android/log.h>
#include <sys/system_properties.h>

#define JSRF_PROP_PREFIX "debug.jsrf."
#define JSRF_PROP_RING 4

static inline const char *jsrf_getenv(const char *name)
{
    const char *value = getenv(name);
    if (value) return value;
    {
        static __thread char ring[JSRF_PROP_RING][PROP_VALUE_MAX];
        static __thread int slot;
        char key[PROP_VALUE_MAX];
        char *out = ring[slot];
        size_t n = 0, i;

        slot = (slot + 1) % JSRF_PROP_RING;
        for (i = 0; JSRF_PROP_PREFIX[i] && n + 1 < sizeof key; i++)
            key[n++] = JSRF_PROP_PREFIX[i];
        /* RECOMP_GL_TEXCHECK_KB -> recomp_gl_texcheck_kb */
        for (i = 0; name[i] && n + 1 < sizeof key; i++) {
            char c = name[i];
            key[n++] = (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
        }
        key[n] = '\0';
        if (__system_property_get(key, out) > 0) return out;
    }
    return NULL;
}

#else

static inline const char *jsrf_getenv(const char *name)
{
    return getenv(name);
}

#endif

/* Diagnostics go to the log a phone can actually be read from. A release build
 * has stderr pointed nowhere, so the environment switches above would work and
 * still print into the void. */
static inline void jsrf_dbg_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
#ifdef __ANDROID__
    __android_log_vprint(ANDROID_LOG_INFO, "JSRF-GLPROF", fmt, ap);
#else
    vfprintf(stderr, fmt, ap);
#endif
    va_end(ap);
}

#endif /* JSRF_DEBUG_ENV_H */
