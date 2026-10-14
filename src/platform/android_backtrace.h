/*
 * android_backtrace.h -- <execinfo.h>'s backtrace() for Android.
 *
 * Bionic has backtrace() and backtrace_symbols_fd() only from API 33; this
 * build targets API 29. The same two calls, over the unwinder every NDK
 * library already links (_Unwind_Backtrace) and dladdr for the names. Only
 * the crash report and a debugging watch use them.
 */
#ifndef ANDROID_BACKTRACE_H
#define ANDROID_BACKTRACE_H

#include <unwind.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

struct jsrf_bt_state { void **buf; int n, max; };

static _Unwind_Reason_Code jsrf_bt_step(struct _Unwind_Context *ctx, void *arg)
{
    struct jsrf_bt_state *st = (struct jsrf_bt_state *)arg;
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc) {
        if (st->n >= st->max) return _URC_END_OF_STACK;
        st->buf[st->n++] = (void *)pc;
    }
    return _URC_NO_REASON;
}

static inline int jsrf_backtrace(void **buf, int max)
{
    struct jsrf_bt_state st;
    st.buf = buf; st.n = 0; st.max = max;
    _Unwind_Backtrace(jsrf_bt_step, &st);
    return st.n;
}

static inline void jsrf_backtrace_symbols_fd(void *const *buf, int n, int fd)
{
    int i;
    for (i = 0; i < n; i++) {
        Dl_info di;
        char line[512];
        int len;
        if (dladdr(buf[i], &di) && di.dli_sname)
            len = snprintf(line, sizeof line, "  #%02d %p %s+0x%lx (%s)\n", i, buf[i],
                           di.dli_sname, (unsigned long)((uintptr_t)buf[i] - (uintptr_t)di.dli_saddr),
                           di.dli_fname ? di.dli_fname : "?");
        else if (dladdr(buf[i], &di) && di.dli_fname)
            len = snprintf(line, sizeof line, "  #%02d %p (%s+0x%lx)\n", i, buf[i], di.dli_fname,
                           (unsigned long)((uintptr_t)buf[i] - (uintptr_t)di.dli_fbase));
        else
            len = snprintf(line, sizeof line, "  #%02d %p\n", i, buf[i]);
        if (len > 0) {
            ssize_t w = write(fd, line, (size_t)(len < (int)sizeof line ? len : (int)sizeof line - 1));
            (void)w;
        }
    }
}

#define backtrace(buf, n)              jsrf_backtrace((buf), (n))
#define backtrace_symbols_fd(b, n, fd) jsrf_backtrace_symbols_fd((b), (n), (fd))

#endif /* ANDROID_BACKTRACE_H */
