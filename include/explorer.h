#ifndef PVIM_EXPLORER_H
#define PVIM_EXPLORER_H

/*
 * Floating 3-column file manager (Miller columns / Ranger style).
 *
 * The explorer owns a WindowLayer and lays three bordered floating windows
 * across the text area:
 *
 *   [ Parent directory ] [ Current directory ] [ Preview ]
 *
 * The middle column is the working directory (arrow / h-j-k-l navigation,
 * selection highlight, `▸`/`/` directory icons); the left column mirrors the
 * parent so you can see where you are; the right column previews the selected
 * file (first lines) or lists a hovered directory. File creation, renaming and
 * deletion are done inline through a prompt shown in the status line.
 *
 * Rendering reuses the generic overlay engine in window.c: the explorer only
 * populates its FloatWindows and lets the display compositor paint them.
 */

#include <limits.h>
#include "window.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Forward declaration: the key handler mutates the editor but explorer.h must
 * not depend on editor.h (editor.h embeds Explorer by value). */
struct EditorState;

typedef enum ExplorerMode {
    EXPLORER_BROWSE = 0,
    EXPLORER_CREATE,   /* typing a new file/directory name */
    EXPLORER_RENAME,   /* typing a new name for the selection */
    EXPLORER_DELETE    /* confirming a deletion (y/N) */
} ExplorerMode;

typedef struct ExplorerEntry {
    char name[256];
    int is_dir;
} ExplorerEntry;

typedef struct Explorer {
    int active;
    int dirty;          /* set when the layer must be rebuilt */

    char cwd[PATH_MAX];

    ExplorerEntry *entries;   /* current directory contents */
    int count, cap;
    int selected, scroll;

    ExplorerEntry *pentries;  /* parent directory contents */
    int pcount, pcap;
    int pselected, pscroll;

    char **preview;           /* preview pane lines */
    int preview_count, preview_cap;
    char preview_title[PATH_MAX];

    ExplorerMode mode;
    char prompt[32];          /* e.g. "New: " */
    char input[PATH_MAX];     /* in-progress create/rename text */
    int input_len;
    char target[256];         /* entry being renamed / deleted */
    char pending_select[256]; /* name to re-select after a refresh */

    WindowLayer layer;        /* the three floating columns */

    int built_rows, built_cols;
} Explorer;

void explorer_init(Explorer *ex);
void explorer_free(Explorer *ex);

/* Open the explorer at the current file's directory (or ".") and switch the
 * editor into MODE_EXPLORER. */
void explorer_open(struct EditorState *state);

/* Close the explorer and clear its layer (returns the editor to normal mode). */
void explorer_close(struct EditorState *state);

/* Handle one key while in MODE_EXPLORER. */
void explorer_handle_key(struct EditorState *state, int key);

/* (Re)build the floating columns if the explorer changed or the terminal was
 * resized. Safe to call every frame; a no-op when nothing changed. */
void explorer_render(struct EditorState *state);

#endif /* PVIM_EXPLORER_H */
