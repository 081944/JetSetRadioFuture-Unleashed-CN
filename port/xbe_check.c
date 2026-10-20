/*
 * JSRF Unleashed - is this the game the engine is made from?
 *
 * The engine is one build of Jet Set Radio Future translated ahead of time:
 * the USA release's default.xbe (NTSC, built 28 January 2002). That build's
 * addresses are written all through the translated code -- every function a
 * table points at, every piece of data, the kernel imports at 0x001C3F60.
 * Another build of the game, the European or the Japanese release, has its
 * code and data at other addresses. The engine would still load it, then run
 * the USA build's code against it, follow pointers into the wrong bytes and
 * fall over a few seconds in. A player's 0.4.6 log on a Mac shows exactly
 * that: one call to 0x0028448C (a pointer read from where the USA build keeps
 * something else), then a crash reading 0xFFFFF000 -- the value his
 * default.xbe holds at 0x001C4048, the USA build's import of LaunchDataPage.
 *
 * So default.xbe's section table is compared with the USA build's before
 * anything runs, and the player is told what the files are instead.
 *
 * Only the layout is compared, not the bytes: where each section is loaded,
 * how big it is and where it sits in the file. A USA default.xbe with a patch
 * applied (the region, a widescreen hack) has the same layout and still runs:
 * the engine never executes the file's code anyway, only its own translation.
 */
#include "xbe_check.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const struct {
    const char *name;
    uint32_t va, vsize, raw, rsize;
} k_usa[] = {
    { ".text",     0x00011000u, 0x0017BB30u, 0x00001000u, 0x0017BB30u },
    { "D3D",       0x0018CB40u, 0x000117F8u, 0x0017D000u, 0x0000E6B0u },
    { "DSOUND",    0x0019E340u, 0x0001C55Cu, 0x0018C000u, 0x0001C32Cu },
    { "MMATRIX",   0x001BA8A0u, 0x00001210u, 0x001A9000u, 0x00001210u },
    { "XGRPH",     0x001BBAC0u, 0x00000CFCu, 0x001AB000u, 0x00000CFCu },
    { "XPP",       0x001BC7C0u, 0x00007798u, 0x001AC000u, 0x00007798u },
    { ".rdata",    0x001C3F60u, 0x00027800u, 0x001B4000u, 0x000277F0u },
    { ".data",     0x001EB760u, 0x00092914u, 0x001DC000u, 0x00044574u },
    { "DOLBY",     0x0027E080u, 0x00006D98u, 0x00221000u, 0x00006D84u },
    { "$$XTIMAGE", 0x00284E20u, 0x00002800u, 0x00228000u, 0x00002800u },
    { "$$XSIMAGE", 0x00287620u, 0x00001000u, 0x0022B000u, 0x00001000u },
};
#define USA_SECTIONS   (sizeof k_usa / sizeof k_usa[0])
#define USA_BASE       0x00010000u
#define USA_TIMEDATE   0x3C558EA7u     /* 2002-01-28 */
#define USA_TITLE_ID   0x5345000Au     /* SE-010, Jet Set Radio Future */

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* A Unix time as YYYY-MM-DD (no gmtime: the same on every platform). */
static void day_of(uint32_t t, char *out, size_t n)
{
    long z = (long)(t / 86400u) + 719468;
    long era = z / 146097, doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1;
    long m = mp < 10 ? mp + 3 : mp - 9;
    snprintf(out, n, "%04ld-%02ld-%02ld", y + (m <= 2), m, d);
}

static void region_of(uint32_t r, char *out, size_t n)
{
    out[0] = 0;
    if (r & 1u) snprintf(out + strlen(out), n - strlen(out), "%sNorth America", out[0] ? " + " : "");
    if (r & 2u) snprintf(out + strlen(out), n - strlen(out), "%sJapan", out[0] ? " + " : "");
    if (r & 4u) snprintf(out + strlen(out), n - strlen(out), "%sEurope and the rest of the world (PAL)",
                         out[0] ? " + " : "");
    if (!out[0]) snprintf(out, n, "none given (%08X)", (unsigned)r);
}

/* How the last default.xbe that was not the USA build's differs from it, in
 * the terms of its section table -- for the log, not for the player. */
static char s_detail[200];
const char *jsrf_xbe_detail(void) { return s_detail; }

int jsrf_xbe_check(const unsigned char *x, size_t n, char *why, size_t whyn)
{
    uint32_t base, td, cert, nsec, secs, title = 0, region = 0, version = 0;
    char name[41] = "", when[24], usa_when[24], reg[96], where[160] = "";
    size_t i, c;
    int same = 1;

    if (why && whyn) why[0] = 0;
    if (n < 0x178 || memcmp(x, "XBEH", 4)) {
        if (why) snprintf(why, whyn, "default.xbe is not an Xbox program (it does not start with XBEH).");
        snprintf(s_detail, sizeof s_detail, "default.xbe: %u bytes, no XBEH header", (unsigned)n);
        return JSRF_XBE_OTHER;
    }
    base = rd32(x + 0x104);
    td   = rd32(x + 0x114);
    cert = rd32(x + 0x118);
    nsec = rd32(x + 0x11C);
    secs = rd32(x + 0x120);

    if (cert >= base && (size_t)(cert - base) + 0xB0 <= n) {
        const unsigned char *ct = x + (cert - base);
        title   = rd32(ct + 0x08);
        region  = rd32(ct + 0xA0);
        version = rd32(ct + 0xAC);
        for (c = 0; c < 40; c++) {
            unsigned ch = (unsigned)ct[0x0C + 2 * c] | (unsigned)ct[0x0D + 2 * c] << 8;
            if (!ch) break;
            name[c] = ch >= 0x20 && ch < 0x7F ? (char)ch : '?';
        }
        name[c] = 0;
    }

    if (base != USA_BASE || nsec != USA_SECTIONS || secs < base) {
        same = 0;
        snprintf(where, sizeof where, "%u sections, loaded at 0x%08X", (unsigned)nsec, (unsigned)base);
    }
    for (i = 0; same && i < USA_SECTIONS; i++) {
        size_t o = (size_t)(secs - base) + i * 56;
        if (o + 56 > n) {
            same = 0;
            snprintf(where, sizeof where, "a section table that does not fit in its header");
            break;
        }
        if (rd32(x + o + 4) != k_usa[i].va || rd32(x + o + 8) != k_usa[i].vsize ||
            rd32(x + o + 12) != k_usa[i].raw || rd32(x + o + 16) != k_usa[i].rsize) {
            same = 0;
            snprintf(where, sizeof where,
                     "its section %u at 0x%08X, %u bytes, where the USA release has %s at 0x%08X, %u bytes",
                     (unsigned)(i + 1), (unsigned)rd32(x + o + 4), (unsigned)rd32(x + o + 8),
                     k_usa[i].name, (unsigned)k_usa[i].va, (unsigned)k_usa[i].vsize);
        }
    }
    if (same) return JSRF_XBE_OK;
    snprintf(s_detail, sizeof s_detail, "default.xbe's layout: %s", where);

    if (why && whyn) {
        day_of(td, when, sizeof when);
        day_of(USA_TIMEDATE, usa_when, sizeof usa_when);
        region_of(region, reg, sizeof reg);
        if (title != USA_TITLE_ID)
            snprintf(why, whyn,
                     "This default.xbe is not Jet Set Radio Future's: it is \"%s\" (title ID %08X, "
                     "region %s, built %s).", name[0] ? name : "?", (unsigned)title, reg, when);
        else
            snprintf(why, whyn,
                     "These files are another release of Jet Set Radio Future than the USA one: their "
                     "default.xbe is for region %s, version %u, built %s (the USA release's was built "
                     "%s).", reg, (unsigned)version, when, usa_when);
    }
    return JSRF_XBE_OTHER;
}

int jsrf_xbe_check_folder(const char *game_dir, char *why, size_t whyn)
{
    char path[4096];
    unsigned char head[16384];
    size_t got;
    FILE *f;

    snprintf(path, sizeof path, "%s/default.xbe", game_dir);
    f = fopen(path, "rb");
    if (!f) {
        if (why) snprintf(why, whyn, "There is no default.xbe in %s.", game_dir);
        return JSRF_XBE_MISSING;
    }
    got = fread(head, 1, sizeof head, f);
    fclose(f);
    return jsrf_xbe_check(head, got, why, whyn);
}
