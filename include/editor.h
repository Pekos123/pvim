#ifndef PVIM_EDITOR_H
#define PVIM_EDITOR_H

#include "buffer.h"
#include "window.h"
#include "explorer.h"
#include "lsp.h"
#include <time.h>

/* Diagnostics published by the language server; owned by the caller (main). */
struct LspDiagnostics;

typedef enum EditorMode {
    MODE_NORMAL,
    MODE_INSERT,
    MODE_COMMAND,
    MODE_VISUAL,
    MODE_SEARCH,
    MODE_EXPLORER
} EditorMode;

/* Line-number gutter modes, set from the `linenumbers` config key. */
typedef enum LineNumberMode {
    LN_OFF = 0,
    LN_ABSOLUTE = 1,
    LN_RELATIVE = 2
} LineNumberMode;

/* A full copy of the buffer contents at a point in time. */
typedef struct Snapshot {
    int num_rows;
    char **lines;
    int cy;
    int cx;
} Snapshot;

typedef struct UndoStack {
    Snapshot *items;
    int count;
    int cap;
} UndoStack;

/* A user-defined `[bindings]` map entry (`map <C-s> :w<CR>`). `key` uses the
 * same encoding as terminal_read_key (plain byte, legacy CTRL byte, or a
 * KEY_MOD_BASE modifier combo); `cmd` is executed via editor_run_cmd. */
typedef struct Binding {
    int key;
    char cmd[128];
} Binding;

typedef struct EditorState {
    EditorBuffer buffer;
    EditorMode mode;
    int cx, cy;
    int rx;
    int v_start_x, v_start_y; // Visual selection start point
    int row_offset, col_offset;
    int screen_rows;
    int screen_cols;
    LineNumberMode line_numbers;
    char status_msg[128];
    time_t status_msg_time;
    char command_buf[128];
    int command_len;
    char pending_key;
    char pending_op; // Pending operator in normal mode (e.g. 'd')
    /* Incremental search: `search_buf` is what the user is typing in search
     * mode (shown live in the status bar and highlighted while typing).
     * `active_search` is the last confirmed search (`/` + Enter), highlighted
     * until cleared with `:noh`. Empty string means "nothing highlighted". */
    char search_buf[128];
    int search_len;
    char active_search[128];
    /* Encoded key values for the clipboard shortcuts, built from the
     * `clipboard_mod` config key (modifier + `y` to copy, + `p` to paste). */
    int clipboard_copy_key;
    int clipboard_paste_key;
    /* Language-server diagnostics to render, or NULL when LSP is inactive.
     * Owned by main; the editor only reads it. */
    struct LspDiagnostics *lsp_diags;
    /* Floating windows / popups composited over the text grid (see window.h).
     * `diag_panel_open` remembers that the explicit `:diag` list is showing, so
     * the transient per-line diagnostics toast stays out of its way. */
    WindowLayer windows;
    int diag_panel_open;
    /* Floating file manager (see explorer.h). */
    Explorer explorer;
    char *yank_buf;
    int yank_len;
    UndoStack undo;
    UndoStack redo;
    int undo_active; // Non-zero while coalescing edits into one undo group
    /* Custom key bindings from the `[bindings]` config section (NORMAL + VISUAL
     * only). Owned by the editor; loaded in editor_init, freed in editor_free. */
    Binding *bindings;
    int binding_count;
    /* Language-server completion menu (INSERT mode). `lsp_completions` is a
     * read-only view owned by main, like `lsp_diags`. `completion_items` is the
     * editor-owned, prefix-filtered snapshot the popup draws and `completion_sel`
     * indexes it. */
    LspCompletions *lsp_completions;
    int completion_request;      /* one-shot: main should query the server */
    int completion_inflight;     /* a request is out, awaiting a response */
    int completion_active;       /* completion session is live */
    int completion_manual;       /* opened with the trigger key (not auto) */
    int completion_sel;          /* selected row in completion_items */
    LspCompletion *completion_items;
    int completion_item_count;
    int completion_item_cap;
    char completion_prefix[128]; /* identifier being completed */
    char completion_fragment[512]; /* full `#include <...>` path fragment */
    int completion_in_include;   /* cursor sits in an #include directive */
    char completion_include_delim; /* '<' or '"' for the active include */
    int completion_req_line;     /* cursor line for the queued request */
    int completion_req_char;     /* UTF-16 column for the queued request */
    int completion_trigger_key;  /* config: `completion_trigger` */
    int completion_accept_keys[4]; /* config: `completion_accept` */
    int completion_accept_count;
    int completion_enabled;      /* config: `completion = on|off` */
    int completion_auto;         /* config: `completion_auto = on|off` */
    int quit;
} EditorState;

/* Visual-mode selection helpers (0-based, inclusive on both ends). */
void editor_get_selection(EditorState *state, int *sy, int *sx, int *ey, int *ex);
void editor_visual_delete(EditorState *state);
void editor_yank(EditorState *state);
void editor_paste(EditorState *state);
/* Insert arbitrary text at the cursor, honouring newlines: a trailing newline
 * means the text is whole lines and is inserted as new rows. */
void editor_insert_text(EditorState *state, const char *text, int len);

/* Undo / redo. */
void editor_begin_change(EditorState *state);
void editor_end_change(EditorState *state);
void editor_undo(EditorState *state);
void editor_redo(EditorState *state);

void editor_init(EditorState *state);
void editor_free(EditorState *state);
void editor_set_status_message(EditorState *state, const char *fmt, ...);
void editor_process_keypress(EditorState *state);
void editor_update_window_size(EditorState *state);
void editor_scroll(EditorState *state);

/* Floating-window popups (composited by display_refresh_screen). */
void editor_show_diagnostics_window(EditorState *state);
void editor_close_windows(EditorState *state);

/* Completion menu: main calls editor_completion_update() after a
 * textDocument/completion response lands in state->lsp_completions, and
 * editor_completion_close() to cancel the session. */
void editor_completion_update(EditorState *state);
void editor_completion_close(EditorState *state);

/* UTF-16 character offset of the cursor within its (0-based) line, as the LSP
 * position encoding requires. */
int editor_cursor_utf16(const EditorState *state);

/* Replace the buffer with the file at `path` (used by the file explorer):
 * clears undo/redo, resets the cursor and closes any open explorer. */
void editor_load_file(EditorState *state, const char *path);

/* Columns the line-number gutter occupies (0 when line numbers are off). The
 * gutter includes the trailing separator space; the text viewport is
 * `screen_cols` minus this value. */
int editor_gutter_width(const EditorState *state);
int editor_diag_lines_in_range(const EditorState *state, int lo, int hi);

#endif /* PVIM_EDITOR_H */
