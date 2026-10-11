#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "../include/headers.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Bounds so a scan of a large toolchain stays quick and bounded. /usr/include
 * alone can hold tens of thousands of files, so the cap has to be generous: a
 * low one would silently drop common headers like stdio.h. */
#define HDR_MAX       200000
#define HDR_MAX_DEPTH 12
#define HDR_MAX_DIRS  65536
#define HDR_MAX_ROOTS 64

static HeaderIndex g_index;        /* lazily built, owned here */
static int g_built;
static char g_key[PATH_MAX + 2];   /* delim + file_dir of the cached index */
static int g_key_valid;

static void index_clear(HeaderIndex *ix) {
    for (int i = 0; i < ix->count; i++) free(ix->paths[i]);
    free(ix->paths);
    ix->paths = NULL;
    ix->count = 0;
    ix->cap = 0;
}

void headers_invalidate(void) {
    index_clear(&g_index);
    g_built = 0;
    g_key_valid = 0;
}

static int index_add(HeaderIndex *ix, const char *rel) {
    if (ix->count >= HDR_MAX) return 0;
    if (ix->count == ix->cap) {
        int ncap = ix->cap ? ix->cap * 2 : 256;
        if (ncap > HDR_MAX) ncap = HDR_MAX;
        char **np = realloc(ix->paths, sizeof(char *) * (size_t)ncap);
        if (!np) return 0;
        ix->paths = np;
        ix->cap = ncap;
    }
    char *copy = strdup(rel);
    if (!copy) return 0;
    ix->paths[ix->count++] = copy;
    return 1;
}

static int has_header_ext(const char *name) {
    static const char *exts[] = {
        ".h", ".hh", ".hpp", ".hxx", ".h++", ".inc",
        ".ipp", ".tcc", ".inl", ".tpl", NULL
    };
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) return 0;
    for (int i = 0; exts[i]; i++)
        if (strcasecmp(dot, exts[i]) == 0) return 1;
    return 0;
}

/* Any file with a header extension, plus the extensionless regular files that
 * make up the C++ standard library (e.g. "vector", "iostream"). */
static int is_header_file(const char *name) {
    if (has_header_ext(name)) return 1;
    return strchr(name, '.') == NULL;
}

typedef struct { dev_t dev; ino_t ino; } DevIno;

/* Depth-first walk of `full`, collecting header files. `rel` is the path
 * relative to the root the walk started from. A per-root (dev, ino) set stops
 * symlink loops. Regular files are indexed before the walk descends into
 * subdirectories, so the shallow, commonly included headers are always present
 * even if the cap is reached. */
static void walk_dir(HeaderIndex *ix, const char *full, const char *rel,
                     int depth, DevIno *seen, int *seen_n) {
    if (depth > HDR_MAX_DEPTH || ix->count >= HDR_MAX) return;
    struct stat st;
    if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) return;
    if (seen) {
        for (int i = 0; i < *seen_n; i++)
            if (seen[i].dev == st.st_dev && seen[i].ino == st.st_ino) return;
        if (*seen_n < HDR_MAX_DIRS) {
            seen[*seen_n].dev = st.st_dev;
            seen[*seen_n].ino = st.st_ino;
            (*seen_n)++;
        }
    }

    DIR *d = opendir(full);
    if (!d) return;
    struct dirent *e;
    char cfull[PATH_MAX];
    char crel[PATH_MAX];

    /* Pass 1: the header files directly in this directory. */
    while ((e = readdir(d)) != NULL) {
        if (ix->count >= HDR_MAX) { closedir(d); return; }
        const char *n = e->d_name;
        if (n[0] == '.') continue;             /* ".", "..", and hidden files */
        if (snprintf(cfull, sizeof cfull, "%s/%s", full, n) >= (int)sizeof cfull)
            continue;
        struct stat cst;
        if (stat(cfull, &cst) != 0 || !S_ISREG(cst.st_mode) || !is_header_file(n))
            continue;
        if (rel[0]) snprintf(crel, sizeof crel, "%s/%s", rel, n);
        else        snprintf(crel, sizeof crel, "%s", n);
        index_add(ix, crel);
    }

    /* Pass 2: descend into the subdirectories. */
    rewinddir(d);
    while ((e = readdir(d)) != NULL) {
        if (ix->count >= HDR_MAX) break;
        const char *n = e->d_name;
        if (n[0] == '.') continue;
        if (snprintf(cfull, sizeof cfull, "%s/%s", full, n) >= (int)sizeof cfull)
            continue;
        struct stat cst;
        if (stat(cfull, &cst) != 0 || !S_ISDIR(cst.st_mode)) continue;
        if (rel[0]) snprintf(crel, sizeof crel, "%s/%s", rel, n);
        else        snprintf(crel, sizeof crel, "%s", n);
        walk_dir(ix, cfull, crel, depth + 1, seen, seen_n);
    }
    closedir(d);
}

typedef struct { char *item[HDR_MAX_ROOTS]; int n; } RootList;

static void roots_free(RootList *r) {
    for (int i = 0; i < r->n; i++) free(r->item[i]);
    r->n = 0;
}

static void root_add(RootList *r, const char *p) {
    if (!p || !p[0] || r->n >= HDR_MAX_ROOTS) return;
    struct stat st;
    if (stat(p, &st) != 0 || !S_ISDIR(st.st_mode)) return;   /* nonexistent: skip */
    for (int i = 0; i < r->n; i++)
        if (strcmp(r->item[i], p) == 0) return;
    r->item[r->n] = strdup(p);
    if (r->item[r->n]) r->n++;
}

/* Append every immediate subdirectory of `parent`, optionally only those whose
 * name contains `match` (used for the multiarch and C++ version directories). */
static void root_add_subdirs(RootList *r, const char *parent, const char *match) {
    DIR *d = opendir(parent);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && r->n < HDR_MAX_ROOTS) {
        const char *n = e->d_name;
        if (n[0] == '.') continue;
        if (match && !strstr(n, match)) continue;
        char full[PATH_MAX];
        snprintf(full, sizeof full, "%s/%s", parent, n);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) root_add(r, full);
    }
    closedir(d);
}

static void system_roots(RootList *r) {
    static const char *envs[] = {"CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", NULL};
    for (int i = 0; envs[i]; i++) {
        const char *v = getenv(envs[i]);
        if (!v) continue;
        char buf[PATH_MAX];
        snprintf(buf, sizeof buf, "%s", v);
        char *save = NULL;
        for (char *tok = strtok_r(buf, ":", &save); tok; tok = strtok_r(NULL, ":", &save))
            root_add(r, tok);
    }
    root_add(r, "/usr/include");
    root_add(r, "/usr/local/include");
    root_add_subdirs(r, "/usr/include/c++", NULL);          /* libstdc++ */
    root_add_subdirs(r, "/usr/include", "-linux-gnu");      /* multiarch bits/ */
}

static void quoted_roots(RootList *r, const char *file_dir) {
    root_add(r, file_dir);
    char buf[PATH_MAX];
    snprintf(buf, sizeof buf, "%s", file_dir);
    for (;;) {
        char *slash = strrchr(buf, '/');
        if (!slash) break;
        if (slash == buf) buf[1] = '\0';
        else *slash = '\0';
        char inc[PATH_MAX];
        if (snprintf(inc, sizeof inc, "%s/include", buf) < (int)sizeof inc)
            root_add(r, inc);
        if (strcmp(buf, "/") == 0 || buf[0] == '\0') break;
    }
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

const HeaderIndex *headers_for(const char *file_dir, char delim) {
    if (!file_dir || !file_dir[0]) file_dir = ".";

    char key[PATH_MAX + 2];
    snprintf(key, sizeof key, "%c%s", delim, file_dir);
    if (g_built && g_key_valid && strcmp(key, g_key) == 0) return &g_index;

    index_clear(&g_index);
    g_built = 1;
    snprintf(g_key, sizeof g_key, "%s", key);
    g_key_valid = 1;

    RootList roots;
    roots.n = 0;
    if (delim == '"') quoted_roots(&roots, file_dir);
    else              system_roots(&roots);

    DevIno *seen = calloc(HDR_MAX_DIRS, sizeof(DevIno));
    int seen_n = 0;
    /* A fresh loop-detection set per root: subdirectories such as /usr/include/
     * c++/13 and the multiarch dir are also roots of their own, and walking them
     * again is what yields the short, correct include strings ("vector",
     * "bits/types.h") instead of their long path from /usr/include. */
    for (int i = 0; i < roots.n && g_index.count < HDR_MAX; i++) {
        seen_n = 0;
        walk_dir(&g_index, roots.item[i], "", 0, seen, &seen_n);
    }
    free(seen);
    roots_free(&roots);

    if (g_index.count > 1) {
        qsort(g_index.paths, (size_t)g_index.count, sizeof(char *), cmp_str);
        int w = 1;
        for (int i = 1; i < g_index.count; i++) {
            if (strcmp(g_index.paths[i], g_index.paths[w - 1]) == 0)
                free(g_index.paths[i]);
            else
                g_index.paths[w++] = g_index.paths[i];
        }
        g_index.count = w;
    }
    return &g_index;
}
