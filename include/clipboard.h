#ifndef PVIM_CLIPBOARD_H
#define PVIM_CLIPBOARD_H

#include <stddef.h>

/*
 * System clipboard access.
 *
 * Copying tries the usual platform helpers in turn (wl-copy, xclip, xsel,
 * pbcopy) and falls back to the terminal's OSC 52 escape, which needs no
 * external tools. Pasting uses wl-paste / xclip / xsel / pbpaste.
 */

/* Put `len` bytes of `text` on the clipboard. Returns 0 on success, -1 when no
 * mechanism worked. */
int clipboard_copy(const char *text, size_t len);

/* Read the clipboard. On success returns a malloc'd NUL-terminated buffer and
 * stores its length in *out_len (caller frees). Returns NULL when nothing could
 * be read. An empty clipboard yields a non-NULL, zero-length result. */
char *clipboard_paste(size_t *out_len);

#endif /* PVIM_CLIPBOARD_H */
