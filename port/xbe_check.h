/*
 * JSRF Unleashed - is this the game the engine is made from?
 *
 * The engine runs one build of Jet Set Radio Future: the USA release's
 * default.xbe. See xbe_check.c.
 */
#ifndef JSRF_XBE_CHECK_H
#define JSRF_XBE_CHECK_H

#include <stddef.h>

#define JSRF_XBE_OK         0   /* the USA release's program (or a patched copy of it) */
#define JSRF_XBE_MISSING    2   /* no default.xbe, or it cannot be read */
#define JSRF_XBE_OTHER      3   /* another program: another release of the game, or not the game */

/* x, n: the start of default.xbe -- its header and section table, the first
 * 4 KB or the whole file. Returns one of the codes above; for anything but
 * JSRF_XBE_OK, why gets a sentence or two saying what the file is. */
int jsrf_xbe_check(const unsigned char *x, size_t n, char *why, size_t whyn);

/* The same for <game folder>/default.xbe, read from disk. */
int jsrf_xbe_check_folder(const char *game_dir, char *why, size_t whyn);

/* After a JSRF_XBE_OTHER: how that file's section table differs from the USA
 * build's, for a log (the player's message leaves it out). */
const char *jsrf_xbe_detail(void);

/* What a player is told when the code is JSRF_XBE_OTHER, after "why". */
#define JSRF_XBE_OTHER_ADVICE \
    "JSRF Unleashed is made from the USA release of Jet Set Radio Future (NTSC, 2002), " \
    "address for address, and runs only that release's files: the European and Japanese " \
    "releases are built differently, and the game crashes a few seconds in. " \
    "Use the files of a USA copy."

#endif
