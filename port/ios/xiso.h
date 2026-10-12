/*
 * Unpacking an Xbox disc image (XDVDFS, "XISO") into a folder.
 *
 * The Mac and Linux launchers hand an .iso to extract-xiso, which is
 * somebody else's program and not something an iPhone can run. The format is
 * small enough to read here: a volume descriptor 64 KB into the game
 * partition, and directories that are binary trees of entries pointing at
 * sectors. Both kinds of image people have are handled -- an XISO (the game
 * partition alone, what extract-xiso and most tools write) and a full disc
 * image with the video partition in front of it.
 */
#ifndef JSRF_XISO_H
#define JSRF_XISO_H

#include <stddef.h>
#include <stdint.h>

/* Called now and then while files are written: bytes so far, bytes in all.
 * Return non-zero to stop (the call then fails with "cancelled"). */
typedef int (*xiso_progress_fn)(void *ctx, uint64_t done, uint64_t total);

/* Unpack every file of the image at iso_path into dest_dir (created if
 * needed). 0 on success; -1 on failure, with a sentence for a person in err.
 * A failed or cancelled run leaves what it wrote: the caller unpacks into a
 * folder of its own and removes it. */
int xiso_extract(const char *iso_path, const char *dest_dir,
                 xiso_progress_fn progress, void *ctx,
                 char *err, size_t errlen);

/* 1 if the file looks like an Xbox disc image (either kind), else 0. */
int xiso_probe(const char *iso_path);

#endif
