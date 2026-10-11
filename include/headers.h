#ifndef PVIM_HEADERS_H
#define PVIM_HEADERS_H

/*
 * Lazy, cached index of header files on disk.
 *
 * clangd already completes `#include` directives, but this is a local fallback
 * so header completion still works when no server is running (or when clangd's
 * include search paths do not cover a directory). Paths are stored relative to
 * the root they were found under, e.g. "sys/types.h" or "inc/foo.h".
 */

typedef struct HeaderIndex {
    char **paths;   /* relative header paths, sorted and de-duplicated */
    int count;
    int cap;
} HeaderIndex;

/* Return the shared index for an including file in `file_dir` using `delim`
 * (`<` for angle includes, `"` for quoted ones). The root set differs: angle
 * includes scan the system include directories, quoted ones scan the file's own
 * directory and any ancestor `include/` directory. The result is cached and
 * owned by the module; do not free it. Returns NULL on allocation failure. */
const HeaderIndex *headers_for(const char *file_dir, char delim);

/* Drop the cache so the next headers_for() rescans the filesystem. Backs the
 * `:reload` command (a new -I directory should be picked up). */
void headers_invalidate(void);

#endif /* PVIM_HEADERS_H */
