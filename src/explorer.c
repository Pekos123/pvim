#define _XOPEN_SOURCE 700
#include "../include/explorer.h"
#include "../include/editor.h"
#include "../include/terminal.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* ------------------------------------------------------------------ paths */

static void path_join(char *out, size_t n, const char *dir, const char *name) {
    if (strcmp(dir, "/") == 0)
        snprintf(out, n, "/%s", name);
    else
        snprintf(out, n, "%s/%s", dir, name);
}

/* Parent of `dir` (absolute or relative). "/" stays "/". */
static void parent_of(const char *dir, char *out, size_t n) {
    if (strcmp(dir, "/") == 0) {
        snprintf(out, n, "/");
        return;
    }
    snprintf(out, n, "%s", dir);
    char *slash = strrchr(out, '/');
    if (!slash) {
        snprintf(out, n, ".");
    } else if (slash == out) {
        out[1] = '\0'; /* "/foo" -> "/" */
    } else {
        *slash = '\0';
    }
}

static const char *base_of(const char *dir) {
    if (strcmp(dir, "/") == 0) return "/";
    const char *s = strrchr(dir, '/');
    return s ? s + 1 : dir;
}

/* Replace control characters (and tabs) so names preview cleanly. */
static void sanitize(char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\t') *s = ' ';
        else if (c < 0x20 || c == 0x7f) *s = ' ';
    }
}

/* --------------------------------------------------------------- entries */

static int entries_add(ExplorerEntry **arr, int *count, int *cap,
                       const char *name, int is_dir) {
    if (*count == *cap) {
        int nc = *cap ? *cap * 2 : 16;
        ExplorerEntry *na = realloc(*arr, (size_t)nc * sizeof(ExplorerEntry));
        if (!na) return -1;
        *arr = na;
        *cap = nc;
    }
    ExplorerEntry *e = &(*arr)[*count];
    snprintf(e->name, sizeof e->name, "%s", name);
    e->is_dir = is_dir;
    (*count)++;
    return 0;
}

static void entries_reset(ExplorerEntry **arr, int *count, int *cap) {
    free(*arr);
    *arr = NULL;
    *count = 0;
    *cap = 0;
}

/* Directories first, then case-insensitive by name. */
static int entry_cmp(const void *a, const void *b) {
    const ExplorerEntry *x = (const ExplorerEntry *)a;
    const ExplorerEntry *y = (const ExplorerEntry *)b;
    if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

/* Read `dir` into a freshly allocated array. Returns -1 on failure. */
static int read_dir(const char *dir, ExplorerEntry **out, int *out_n) {
    *out = NULL;
    *out_n = 0;
    DIR *d = opendir(dir);
    if (!d) return -1;
    int cap = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        char full[PATH_MAX];
        path_join(full, sizeof full, dir, de->d_name);
        struct stat st;
        int is_dir = (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) ? 1 : 0;
        if (entries_add(out, out_n, &cap, de->d_name, is_dir) != 0) break;
    }
    closedir(d);
    if (*out_n > 0)
        qsort(*out, (size_t)*out_n, sizeof(ExplorerEntry), entry_cmp);
    return 0;
}

/* --------------------------------------------------------------- preview */

static void preview_reset(Explorer *ex) {
    for (int i = 0; i < ex->preview_count; i++) free(ex->preview[i]);
    free(ex->preview);
    ex->preview = NULL;
    ex->preview_count = 0;
    ex->preview_cap = 0;
}

static int preview_add(Explorer *ex, const char *line) {
    if (ex->preview_count == ex->preview_cap) {
        int nc = ex->preview_cap ? ex->preview_cap * 2 : 32;
        char **na = realloc(ex->preview, (size_t)nc * sizeof(char *));
        if (!na) return -1;
        ex->preview = na;
        ex->preview_cap = nc;
    }
    char *copy = strdup(line);
    if (!copy) return -1;
    ex->preview[ex->preview_count++] = copy;
    return 0;
}

static int file_is_binary(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char buf[4096];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    for (size_t i = 0; i < n; i++)
        if (buf[i] == 0) return 1;
    return 0;
}

#define PREVIEW_MAX_LINES 400

static void load_preview(Explorer *ex) {
    preview_reset(ex);
    ex->preview_title[0] = '\0';
    if (ex->count == 0) return;

    ExplorerEntry *e = &ex->entries[ex->selected];
    char full[PATH_MAX];
    path_join(full, sizeof full, ex->cwd, e->name);

    if (e->is_dir) {
        snprintf(ex->preview_title, sizeof ex->preview_title, "%s/", e->name);
        ExplorerEntry *arr = NULL;
        int n = 0;
        if (read_dir(full, &arr, &n) == 0) {
            for (int i = 0; i < n; i++) {
                char buf[300];
                snprintf(buf, sizeof buf, "%s%s%s",
                         arr[i].is_dir ? "\xe2\x96\xb8 " : "  ",
                         arr[i].name, arr[i].is_dir ? "/" : "");
                preview_add(ex, buf);
            }
            free(arr);
        }
    } else {
        snprintf(ex->preview_title, sizeof ex->preview_title, "%s", e->name);
        if (file_is_binary(full)) {
            preview_add(ex, "<binary file>");
        } else {
            FILE *f = fopen(full, "r");
            if (!f) {
                preview_add(ex, "<cannot open>");
            } else {
                char line[512];
                int lines = 0;
                while (lines < PREVIEW_MAX_LINES && fgets(line, sizeof line, f)) {
                    size_t l = strlen(line);
                    while (l && (line[l - 1] == '\n' || line[l - 1] == '\r'))
                        line[--l] = '\0';
                    sanitize(line);
                    preview_add(ex, line);
                    lines++;
                }
                fclose(f);
            }
        }
    }
}

/* ------------------------------------------------------------- reloading */

static void load_current(Explorer *ex) {
    entries_reset(&ex->entries, &ex->count, &ex->cap);
    ExplorerEntry *arr = NULL;
    int n = 0;
    if (read_dir(ex->cwd, &arr, &n) == 0) {
        ex->entries = arr;
        ex->count = n;
        ex->cap = n;
    }
    if (ex->selected >= ex->count) ex->selected = ex->count - 1;
    if (ex->selected < 0) ex->selected = 0;
    if (ex->scroll < 0) ex->scroll = 0;
}

static void apply_pending(Explorer *ex) {
    if (!ex->pending_select[0]) return;
    for (int i = 0; i < ex->count; i++) {
        if (strcmp(ex->entries[i].name, ex->pending_select) == 0) {
            ex->selected = i;
            ex->scroll = 0;
            break;
        }
    }
    ex->pending_select[0] = '\0';
}

static void load_parent(Explorer *ex) {
    entries_reset(&ex->pentries, &ex->pcount, &ex->pcap);
    ex->pselected = -1;
    if (strcmp(ex->cwd, "/") == 0) return; /* root: parent disabled/empty */

    char parent[PATH_MAX];
    parent_of(ex->cwd, parent, sizeof parent);
    ExplorerEntry *arr = NULL;
    int n = 0;
    if (read_dir(parent, &arr, &n) != 0) return;
    ex->pentries = arr;
    ex->pcount = n;
    ex->pcap = n;

    const char *base = base_of(ex->cwd);
    for (int i = 0; i < n; i++) {
        if (strcmp(ex->pentries[i].name, base) == 0) {
            ex->pselected = i;
            break;
        }
    }
}

static void explorer_refresh(Explorer *ex) {
    load_current(ex);
    apply_pending(ex);
    load_parent(ex);
    load_preview(ex);
    ex->dirty = 1;
}

/* -------------------------------------------------------- window drawing */

static void fill_entries(FloatWindow *win, ExplorerEntry *e, int n, int sel,
                         int *scrollp) {
    int cx, cy, cw, ch;
    window_content_dims(win, &cx, &cy, &cw, &ch);
    (void)cx;
    (void)cy;

    int scroll = *scrollp;
    if (sel < 0) sel = 0;
    if (ch > 0) {
        if (sel < scroll) scroll = sel;
        if (sel >= scroll + ch) scroll = sel - ch + 1;
        if (scroll > n - ch) scroll = n - ch;
        if (scroll < 0) scroll = 0;
    }
    *scrollp = scroll;

    for (int r = 0; r < ch; r++) {
        int i = scroll + r;
        if (i >= 0 && i < n) {
            char buf[300];
            snprintf(buf, sizeof buf, "%s%s%s",
                     e[i].is_dir ? "\xe2\x96\xb8 " : "  ",
                     e[i].name, e[i].is_dir ? "/" : "");
            window_set_line(win, r, buf);
        } else {
            window_set_line(win, r, "");
        }
    }
    win->selected = (n > 0 && sel >= scroll && sel - scroll < ch) ? sel - scroll : -1;
}

static void fill_preview(FloatWindow *win, Explorer *ex) {
    int cx, cy, cw, ch;
    window_content_dims(win, &cx, &cy, &cw, &ch);
    (void)cx;
    (void)cy;
    for (int r = 0; r < ch; r++) {
        if (r < ex->preview_count)
            window_set_line(win, r, ex->preview[r]);
        else
            window_set_line(win, r, "");
    }
    win->selected = -1;
}

void explorer_render(EditorState *state) {
    Explorer *ex = &state->explorer;
    if (!ex->active) return;

    int rows = state->screen_rows;
    int cols = state->screen_cols;
    if (!ex->dirty && rows == ex->built_rows && cols == ex->built_cols) return;
    ex->built_rows = rows;
    ex->built_cols = cols;
    ex->dirty = 0;

    windows_clear(&ex->layer);
    if (rows < 4 || cols < 16) return; /* too small: leave the grid visible */

    /* Split the width into three columns: parent / current / preview. */
    int w1 = cols * 25 / 100;
    int w2 = cols * 30 / 100;
    if (w1 < 12) w1 = 12;
    if (w2 < 14) w2 = 14;
    int w3 = cols - w1 - w2;
    if (w3 < 14) {
        w3 = 14;
        w2 = cols - w1 - w3;
        if (w2 < 10) {
            w2 = 10;
            w1 = cols - w2 - w3;
        }
    }
    if (w1 < 6) w1 = 6;
    int xM = w1;
    int xR = w1 + w2;

    char ptitle[PATH_MAX];
    if (strcmp(ex->cwd, "/") == 0)
        snprintf(ptitle, sizeof ptitle, "Parent (root)");
    else
        parent_of(ex->cwd, ptitle, sizeof ptitle);

    windows_add(&ex->layer, 0, 0, w1, rows, ptitle, WIN_BORDER_ROUND, 1);
    windows_add(&ex->layer, xM, 0, w2, rows, ex->cwd, WIN_BORDER_ROUND, 2);
    windows_add(&ex->layer, xR, 0, w3, rows,
                ex->preview_title[0] ? ex->preview_title : "Preview",
                WIN_BORDER_ROUND, 3);
    if (ex->layer.count != 3) return;

    /* Re-fetch after the adds (the backing array may have moved). */
    FloatWindow *L = &ex->layer.items[0];
    FloatWindow *M = &ex->layer.items[1];
    FloatWindow *R = &ex->layer.items[2];

    fill_entries(L, ex->pentries, ex->pcount, ex->pselected, &ex->pscroll);
    fill_entries(M, ex->entries, ex->count, ex->selected, &ex->scroll);
    fill_preview(R, ex);
}

/* ---------------------------------------------------------------- actions */

static void set_browse_status(EditorState *state) {
    Explorer *ex = &state->explorer;
    if (ex->count == 0) {
        editor_set_status_message(state, "%s  (empty)", ex->cwd);
        return;
    }
    char full[PATH_MAX];
    path_join(full, sizeof full, ex->cwd, ex->entries[ex->selected].name);
    editor_set_status_message(state, "%s", full);
}

static void enter_dir(EditorState *state, const char *dir) {
    Explorer *ex = &state->explorer;
    char resolved[PATH_MAX];
    /* Keep paths tidy without requiring the directory to exist. */
    if (realpath(dir, resolved)) {
        snprintf(ex->cwd, sizeof ex->cwd, "%s", resolved);
    } else {
        snprintf(ex->cwd, sizeof ex->cwd, "%s", dir);
    }
    ex->selected = 0;
    ex->scroll = 0;
    explorer_refresh(ex);
    set_browse_status(state);
}

static void go_parent(EditorState *state) {
    Explorer *ex = &state->explorer;
    if (strcmp(ex->cwd, "/") == 0) {
        editor_set_status_message(state, "Already at root");
        return;
    }
    char parent[PATH_MAX];
    parent_of(ex->cwd, parent, sizeof parent);
    /* Re-select the directory we came from. */
    snprintf(ex->pending_select, sizeof ex->pending_select, "%s",
             base_of(ex->cwd));
    enter_dir(state, parent);
}

static void activate(EditorState *state) {
    Explorer *ex = &state->explorer;
    if (ex->count == 0) return;
    ExplorerEntry *e = &ex->entries[ex->selected];
    char full[PATH_MAX];
    path_join(full, sizeof full, ex->cwd, e->name);
    if (e->is_dir) {
        enter_dir(state, full);
    } else {
        editor_load_file(state, full);
    }
}

/* Commit the create/rename prompt. */
static void commit_prompt(EditorState *state) {
    Explorer *ex = &state->explorer;
    if (ex->input_len == 0) {
        ex->mode = EXPLORER_BROWSE;
        editor_set_status_message(state, "");
        return;
    }

    char full[PATH_MAX];
    path_join(full, sizeof full, ex->cwd, ex->input);

    if (ex->mode == EXPLORER_CREATE) {
        int is_dir = (ex->input[ex->input_len - 1] == '/');
        int rc;
        if (is_dir) {
            rc = mkdir(full, 0755);
        } else {
            int fd = open(full, O_WRONLY | O_CREAT, 0644);
            rc = (fd >= 0) ? 0 : -1;
            if (fd >= 0) close(fd);
        }
        if (rc != 0) {
            editor_set_status_message(state, "Create failed: %s", strerror(errno));
        } else {
            editor_set_status_message(state, "Created %s", ex->input);
            /* Re-select the created entry; strip any trailing '/' the user
             * typed so it matches the entry name (readdir never includes it). */
            char clean[PATH_MAX];
            snprintf(clean, sizeof clean, "%s", ex->input);
            size_t cl = strlen(clean);
            while (cl > 0 && clean[cl - 1] == '/') clean[--cl] = '\0';
            snprintf(ex->pending_select, sizeof ex->pending_select, "%.*s",
                     (int)sizeof(ex->pending_select) - 1, clean);
        }
    } else if (ex->mode == EXPLORER_RENAME) {
        char from[PATH_MAX];
        path_join(from, sizeof from, ex->cwd, ex->target);
        if (rename(from, full) != 0)
            editor_set_status_message(state, "Rename failed: %s", strerror(errno));
        else {
            editor_set_status_message(state, "Renamed to %s", ex->input);
            snprintf(ex->pending_select, sizeof ex->pending_select, "%.*s",
                     (int)sizeof(ex->pending_select) - 1, ex->input);
        }
    }

    ex->mode = EXPLORER_BROWSE;
    explorer_refresh(ex);
}

static void commit_delete(EditorState *state) {
    Explorer *ex = &state->explorer;
    char full[PATH_MAX];
    path_join(full, sizeof full, ex->cwd, ex->target);

    struct stat st;
    int rc;
    if (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
        rc = rmdir(full); /* safe: refuses a non-empty directory */
    else
        rc = unlink(full);

    if (rc != 0)
        editor_set_status_message(state, "Delete failed: %s", strerror(errno));
    else
        editor_set_status_message(state, "Deleted %s", ex->target);

    ex->mode = EXPLORER_BROWSE;
    explorer_refresh(ex);
}

static void begin_prompt(EditorState *state, ExplorerMode mode,
                         const char *prompt, const char *seed) {
    Explorer *ex = &state->explorer;
    ex->mode = mode;
    snprintf(ex->prompt, sizeof ex->prompt, "%s", prompt);
    snprintf(ex->input, sizeof ex->input, "%s", seed ? seed : "");
    ex->input_len = (int)strlen(ex->input);
    if (mode == EXPLORER_RENAME && ex->count > 0)
        snprintf(ex->target, sizeof ex->target, "%s",
                 ex->entries[ex->selected].name);
    editor_set_status_message(state, "%s%s", ex->prompt, ex->input);
}

static void update_prompt(EditorState *state) {
    Explorer *ex = &state->explorer;
    editor_set_status_message(state, "%s%s", ex->prompt, ex->input);
}

/* ------------------------------------------------------------------- API */

void explorer_init(Explorer *ex) {
    memset(ex, 0, sizeof *ex);
    windows_init(&ex->layer);
    ex->mode = EXPLORER_BROWSE;
    ex->built_rows = -1;
    ex->built_cols = -1;
}

void explorer_free(Explorer *ex) {
    entries_reset(&ex->entries, &ex->count, &ex->cap);
    entries_reset(&ex->pentries, &ex->pcount, &ex->pcap);
    preview_reset(ex);
    windows_free(&ex->layer);
}

void explorer_open(EditorState *state) {
    Explorer *ex = &state->explorer;
    if (ex->active) return;

    char dir[PATH_MAX] = ".";
    if (state->buffer.filename && state->buffer.filename[0]) {
        snprintf(dir, sizeof dir, "%s", state->buffer.filename);
        char *slash = strrchr(dir, '/');
        if (slash) {
            if (slash == dir) dir[1] = '\0';
            else *slash = '\0';
        } else {
            snprintf(dir, sizeof dir, ".");
        }
    }

    char resolved[PATH_MAX];
    if (!realpath(dir, resolved)) snprintf(resolved, sizeof resolved, "%s", dir);

    snprintf(ex->cwd, sizeof ex->cwd, "%s", resolved);
    ex->selected = 0;
    ex->scroll = 0;
    ex->pscroll = 0;
    ex->pending_select[0] = '\0';
    ex->mode = EXPLORER_BROWSE;
    ex->active = 1;
    ex->built_rows = -1;
    ex->built_cols = -1;
    explorer_refresh(ex);

    state->mode = MODE_EXPLORER;
    set_browse_status(state);
}

void explorer_close(EditorState *state) {
    Explorer *ex = &state->explorer;
    if (!ex->active) return;
    ex->active = 0;
    ex->dirty = 1;
    windows_clear(&ex->layer);
    if (state->mode == MODE_EXPLORER) state->mode = MODE_NORMAL;
    editor_set_status_message(state, "");
}

void explorer_handle_key(EditorState *state, int key) {
    Explorer *ex = &state->explorer;
    if (!ex->active) return;

    /* --- text prompts --- */
    if (ex->mode == EXPLORER_CREATE || ex->mode == EXPLORER_RENAME) {
        if (key == KEY_ESC) {
            ex->mode = EXPLORER_BROWSE;
            editor_set_status_message(state, "Cancelled");
        } else if (key == KEY_ENTER) {
            commit_prompt(state);
        } else if (key == BACKSPACE) {
            if (ex->input_len > 0) ex->input[--ex->input_len] = '\0';
            update_prompt(state);
        } else if (key >= 32 && key <= 126 &&
                   ex->input_len < (int)sizeof(ex->input) - 1) {
            ex->input[ex->input_len++] = (char)key;
            ex->input[ex->input_len] = '\0';
            update_prompt(state);
        }
        return;
    }

    /* --- delete confirmation --- */
    if (ex->mode == EXPLORER_DELETE) {
        if (key == 'y' || key == 'Y') {
            commit_delete(state);
        } else {
            ex->mode = EXPLORER_BROWSE;
            editor_set_status_message(state, "Cancelled");
        }
        return;
    }

    /* --- browse --- */
    switch (key) {
        case ARROW_UP:
        case 'k':
            if (ex->selected > 0) {
                ex->selected--;
                load_preview(ex);
                ex->dirty = 1;
                set_browse_status(state);
            }
            break;
        case ARROW_DOWN:
        case 'j':
            if (ex->selected < ex->count - 1) {
                ex->selected++;
                load_preview(ex);
                ex->dirty = 1;
                set_browse_status(state);
            }
            break;
        case ARROW_LEFT:
        case 'h':
            go_parent(state);
            break;
        case ARROW_RIGHT:
        case 'l':
        case KEY_ENTER:
            activate(state);
            break;
        case KEY_ESC:
            explorer_close(state);
            break;
        case 'a':
            begin_prompt(state, EXPLORER_CREATE, "New: ", "");
            break;
        case 'r':
            if (ex->count > 0)
                begin_prompt(state, EXPLORER_RENAME, "Rename: ",
                             ex->entries[ex->selected].name);
            break;
        case 'd':
            if (ex->count > 0) {
                snprintf(ex->target, sizeof ex->target, "%s",
                         ex->entries[ex->selected].name);
                ex->mode = EXPLORER_DELETE;
                editor_set_status_message(state, "Delete '%s'? (y/N)",
                                          ex->target);
            }
            break;
        default:
            break;
    }
}
