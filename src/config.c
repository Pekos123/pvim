#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../include/config.h"

/* --------------------------- small helpers ------------------------------ */

static char *trim_ws(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = '\0';
    return s;
}

static char *aprintf(const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return NULL; }
    char *s = malloc((size_t)n + 1);
    if (s) vsnprintf(s, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return s;
}

/* --------------------- executable directory resolution ------------------ */

static char g_exe_dir[PATH_MAX] = "";
static char g_exe_parent[PATH_MAX] = "";
static int g_exe_resolved = 0;

static void resolve_exe_dirs(void) {
    if (g_exe_resolved) return;
    g_exe_resolved = 1;
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0 || n >= (ssize_t)sizeof(exe)) return;
    exe[n] = '\0';
    char *slash = strrchr(exe, '/');
    if (!slash) return;
    *slash = '\0';
    g_exe_dir[0] = '\0';
    if (strlen(exe) < sizeof(g_exe_dir))
        snprintf(g_exe_dir, sizeof(g_exe_dir), "%s", exe);
    char *up = strrchr(exe, '/');
    if (up) {
        *up = '\0';
        g_exe_parent[0] = '\0';
        if (strlen(exe) < sizeof(g_exe_parent))
            snprintf(g_exe_parent, sizeof(g_exe_parent), "%s", exe);
    }
}

const char *pvim_exe_dir(void) {
    resolve_exe_dirs();
    return g_exe_dir;
}

const char *pvim_exe_parent_dir(void) {
    resolve_exe_dirs();
    return g_exe_parent;
}

/* -------------------------- config discovery ---------------------------- */

static char *user_config_dir(void) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return aprintf("%s/pvim", xdg);
    const char *home = getenv("HOME");
    if (home && *home) return aprintf("%s/.config/pvim", home);
    return NULL;
}

char *pvim_user_config_dir(void) {
    return user_config_dir();
}

static char *g_config_path = NULL;
static int g_freed = 0;

static void config_exit_free(void) {
    pvim_config_free();
}

const char *pvim_config_path(void) {
    if (!g_freed) { g_freed = 1; atexit(config_exit_free); }
    if (g_config_path) return g_config_path;

    const char *env = getenv("PVIM_CONFIG");
    char *user_conf = NULL;
    char *ud = user_config_dir();
    if (ud) {
        user_conf = aprintf("%s/pvim.conf", ud);
        free(ud);
    }

    resolve_exe_dirs();
    char *exe_conf = NULL;
    char *parent_conf = NULL;
    if (g_exe_dir[0]) exe_conf = aprintf("%s/pvim.conf", g_exe_dir);
    if (g_exe_parent[0]) parent_conf = aprintf("%s/pvim.conf", g_exe_parent);

    const char *cands[5];
    int n = 0;
    if (env && *env) cands[n++] = env;
    if (user_conf) cands[n++] = user_conf;
    cands[n++] = "./pvim.conf";
    if (exe_conf) cands[n++] = exe_conf;
    if (parent_conf) cands[n++] = parent_conf;

    for (int i = 0; i < n; i++) {
        FILE *f = fopen(cands[i], "r");
        if (f) { fclose(f); g_config_path = strdup(cands[i]); break; }
    }
    free(user_conf);
    free(exe_conf);
    free(parent_conf);
    return g_config_path;
}

char *pvim_config_get(const char *key) {
    const char *path = pvim_config_path();
    if (!path || !key) return NULL;
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char line[1024];
    char *value = NULL;
    while (fgets(line, sizeof(line), f)) {
        char *l = trim_ws(line);
        if (!*l || *l == '#') continue;
        char *eq = strchr(l, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = trim_ws(l);
        char *v = trim_ws(eq + 1);
        char *hash = strchr(v, '#');
        if (hash) *hash = '\0';
        v = trim_ws(v);
        if (!strcmp(k, key) && *v) { value = strdup(v); break; }
    }
    fclose(f);
    return value;
}

/* Collect every line inside a `[section]` block. The region keeps the vim-ish
 * `map <C-s> :w<CR>` lines verbatim (they have no `=`), so the caller does the
 * interpretation; comments and blank lines are dropped, and the region ends at
 * the next section header. The section header line is `[name]` (spaces
 * trimmed). */
int pvim_config_section(const char *section, char **lines, int max) {
    const char *path = pvim_config_path();
    if (!path || !section || !lines || max <= 0) return -1;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char want[256];
    snprintf(want, sizeof(want), "[%s]", section);
    char line[1024];
    int in_section = 0;
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        char *l = trim_ws(line);
        if (!*l || *l == '#') continue;
        if (*l == '[') {
            char *endc = strchr(l, ']');
            if (!endc) continue;
            *endc = '\0';
            if (in_section) break;              /* region ends at next header */
            if (!strcmp(trim_ws(l + 1), section)) in_section = 1;
            continue;
        }
        if (in_section && n < max) lines[n++] = strdup(l);
    }
    fclose(f);
    return in_section ? n : -1;
}

void pvim_config_free(void) {
    free(g_config_path);
    g_config_path = NULL;
    g_exe_dir[0] = '\0';
    g_exe_parent[0] = '\0';
    g_exe_resolved = 0;
}
