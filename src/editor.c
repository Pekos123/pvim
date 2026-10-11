#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "../include/editor.h"
#include "../include/terminal.h"
#include "../include/highlight.h"
#include "../include/config.h"
#include "../include/clipboard.h"
#include "../include/lsp.h"
#include "../include/headers.h"

/* Map a `linenumbers` value to a gutter mode. Anything unrecognised falls back
 * to absolute numbers. */
static LineNumberMode parse_linenumber_mode(const char *v) {
    if (v && (!strcmp(v, "off") || !strcmp(v, "none") || !strcmp(v, "no")))
        return LN_OFF;
    if (v && (!strcmp(v, "relative") || !strcmp(v, "rel")))
        return LN_RELATIVE;
    return LN_ABSOLUTE;   /* "absolute", "abs", or the default */
}

/* Map a `clipboard_mod` value to a modifier mask. Anything unrecognised (and
 * the default) is Super, matching the "Super+y / Super+p" shortcut. */
static int parse_clipboard_mod(const char *v) {
    /* strcmp(a, b) returns 0 if strings are indentical, thats why we use !strcmp()  */ 
    if (v) {
        if (!strcmp(v, "ctrl") || !strcmp(v, "control"))   return KEY_MOD_CTRL;
        
        if (!strcmp(v, "alt")  || !strcmp(v, "opt") ||
            !strcmp(v, "option") || !strcmp(v, "meta"))    return KEY_MOD_ALT;
        
        if (!strcmp(v, "super") || !strcmp(v, "cmd") ||
            !strcmp(v, "command") || !strcmp(v, "win"))    return KEY_MOD_SUPER;
        
        if (!strcmp(v, "shift"))                           return KEY_MOD_SHIFT;
        
        if (!strcmp(v, "none") || !strcmp(v, "off") ||
            !strcmp(v, "no"))                              return 0;
    }
    return KEY_MOD_SUPER;
}

/* Interpret an on/off-ish config value; `dflt` when unset or unrecognised. */
static int parse_onoff(const char *v, int dflt) {
    if (!v || !*v) return dflt;
    if (!strcmp(v, "on") || !strcmp(v, "yes") || !strcmp(v, "true") ||
        !strcmp(v, "1")) return 1;
    if (!strcmp(v, "off") || !strcmp(v, "no") || !strcmp(v, "false") ||
        !strcmp(v, "0")) return 0;
    return dflt;
}

/* Encode modifier + letter the same way terminal_read_key reports it. Ctrl is
 * special-cased to the legacy control byte so it matches everywhere. */
static int clipboard_key(int mods, int letter) {
    if (mods == KEY_MOD_CTRL && letter >= 'a' && letter <= 'z')
        return CTRL_KEY(letter);
    if (mods == 0) return letter;
    return terminal_combo(mods, letter);
}

/* --------------------------- [bindings] key maps ------------------------- */

/* Case-insensitive comparison of `len` bytes of `s` against literal `lit`. */
static int eq_ci(const char *s, size_t len, const char *lit) {
    if (strlen(lit) != len) return 0;
    for (size_t i = 0; i < len; i++)
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)lit[i]))
            return 0;
    return 1;
}

/* Match a vim-style modifier prefix ("C-", "S-", "A-", "M-", "D-"). */
static int mod_token(const char *s) {
    if (s[1] != '-') return 0;
    switch (tolower((unsigned char)s[0])) {
        case 'c': return KEY_MOD_CTRL;
        case 's': return KEY_MOD_SHIFT;
        case 'a': return KEY_MOD_ALT;
        case 'm': return KEY_MOD_ALT;    /* Meta (synonym for Alt) */
        case 'd': return KEY_MOD_SUPER;  /* Cmd / Win / Super */
    }
    return 0;
}

/* Parse the key part of a `[bindings]` map line: either a single bare
 * character (`g`) or a vim-style `<...>` spec such as `<C-s>`, `<C-S-p>`,
 * `<F2>`, `<Up>`, `<Space>`, `<D-y>`. Returns the same value
 * terminal_read_key would produce for that key, or -1 when the spec is not
 * understood. Encoding keeps parity with clipboard_key(): Ctrl+letter becomes
 * the legacy control byte, unmodified keys their plain byte, and
 * alt/super/shift combos the KEY_MOD_BASE-encoded value. Shift on a letter is
 * vim's behaviour of collapsing onto the uppercase byte (`<S-p>` == `P`). */
static int parse_bind_key(const char *name) {
    const char *p = name;
    if (!p || !*p) return -1;
    int mods = 0;
    int code = -1;
    if (*p == '<') {
        p++;
        int m;
        while ((m = mod_token(p)) != 0) {
            mods |= m;
            p += 2;
        }
        const char *end = strchr(p, '>');
        if (!end || end == p) return -1;
        size_t n = (size_t)(end - p);
        if (n == 1) {
            code = (unsigned char)p[0];
        } else if (eq_ci(p, n, "esc")) {
            code = KEY_ESC;
        } else if (eq_ci(p, n, "cr") || eq_ci(p, n, "enter")) {
            code = KEY_ENTER;
        } else if (eq_ci(p, n, "tab")) {
            code = KEY_TAB;
        } else if (eq_ci(p, n, "bs") || eq_ci(p, n, "backspace")) {
            code = BACKSPACE;
        } else if (eq_ci(p, n, "space")) {
            code = ' ';
        } else if (eq_ci(p, n, "up")) {
            code = ARROW_UP;
        } else if (eq_ci(p, n, "down")) {
            code = ARROW_DOWN;
        } else if (eq_ci(p, n, "left")) {
            code = ARROW_LEFT;
        } else if (eq_ci(p, n, "right")) {
            code = ARROW_RIGHT;
        } else if (eq_ci(p, n, "home")) {
            code = HOME_KEY;
        } else if (eq_ci(p, n, "end")) {
            code = END_KEY;
        } else if (eq_ci(p, n, "del") || eq_ci(p, n, "delete")) {
            code = DEL_KEY;
        } else if (eq_ci(p, n, "pageup") || eq_ci(p, n, "pgup")) {
            code = PAGE_UP;
        } else if (eq_ci(p, n, "pagedown") || eq_ci(p, n, "pgdn")) {
            code = PAGE_DOWN;
        } else if (tolower((unsigned char)p[0]) == 'f' &&
                   n == 2 && p[1] >= '1' && p[1] <= '9') {
            code = KEY_F1 + (p[1] - '1');
        } else if (tolower((unsigned char)p[0]) == 'f' &&
                   n == 3 && p[1] == '1' && p[2] >= '0' && p[2] <= '2') {
            code = KEY_F10 + (p[2] - '0');
        } else {
            return -1;
        }
        if (end[1] != '\0') return -1;
    } else {
        if (p[1] != '\0') return -1;
        code = (unsigned char)p[0];
    }
    /* Ctrl+letter collapses to the legacy control byte (even with Shift). */
    if ((mods & KEY_MOD_CTRL) && code >= 'a' && code <= 'z')
        return CTRL_KEY(code);
    if (mods & KEY_MOD_SHIFT) {
        if (code >= 'a' && code <= 'z') return code - 32; /* <S-p> -> 'P' */
        mods &= ~KEY_MOD_SHIFT;                           /* <S-Up> -> <Up> */
    }
    if (mods == 0) return code;
    return terminal_combo(mods, code);
}

/* Load the `[bindings]` section of pvim.conf. Every line is a vim-style map:
 *   map <C-s> :w<CR>
 *   noremap g :wq
 * Inline `#` comments and a trailing `<CR>`/`<Enter>` are stripped. Lines that
 * are not maps, keys that fail to parse, and empty commands are skipped. */
static void bindings_load(EditorState *state) {
    free(state->bindings);
    state->bindings = NULL;
    state->binding_count = 0;
    char *lines[64];
    int n = pvim_config_section("bindings", lines, 64);
    if (n <= 0) return;
    Binding *arr = malloc(sizeof(Binding) * (size_t)n);
    if (!arr) {
        for (int i = 0; i < n; i++) free(lines[i]);
        return;
    }
    int count = 0;
    for (int i = 0; i < n; i++) {
        char *l = lines[i];
        while (*l == ' ' || *l == '\t') l++;
        char *hash = strchr(l, '#');
        if (hash) *hash = '\0';
        char *e = l + strlen(l);
        while (e > l && (e[-1] == ' ' || e[-1] == '\t')) *--e = '\0';
        if (!*l) continue;
        const char *rest = NULL;
        if (!strncmp(l, "map", 3) && (l[3] == ' ' || l[3] == '\t' || !l[3]))
            rest = l + 3;
        else if (!strncmp(l, "noremap", 7) && (l[7] == ' ' || l[7] == '\t' || !l[7]))
            rest = l + 7;
        if (!rest) continue;
        while (*rest == ' ' || *rest == '\t') rest++;
        if (!*rest) continue;

        char keyspec[64];
        const char *cmd_src;
        if (*rest == '<') {
            const char *gt = strchr(rest, '>');
            if (!gt) continue;
            size_t klen = (size_t)(gt - rest + 1);
            if (klen >= sizeof(keyspec)) klen = sizeof(keyspec) - 1;
            memcpy(keyspec, rest, klen);
            keyspec[klen] = '\0';
            cmd_src = gt + 1;
        } else {
            keyspec[0] = *rest;
            keyspec[1] = '\0';
            cmd_src = rest + 1;
        }

        while (*cmd_src == ' ' || *cmd_src == '\t') cmd_src++;
        if (*cmd_src == ':') cmd_src++;

        char cmd[128];
        size_t cl = strlen(cmd_src);
        if (cl >= 4 && eq_ci(cmd_src + cl - 4, 4, "<CR>")) cl -= 4;
        else if (cl >= 7 && eq_ci(cmd_src + cl - 7, 7, "<Enter>")) cl -= 7;
        while (cl > 0 && (cmd_src[cl - 1] == ' ' || cmd_src[cl - 1] == '\t')) cl--;
        if (cl >= sizeof(cmd)) cl = sizeof(cmd) - 1;

        int key = parse_bind_key(keyspec);
        if (key < 0 || cl == 0) continue;
        memcpy(cmd, cmd_src, cl);
        cmd[cl] = '\0';
        arr[count].key = key;
        snprintf(arr[count].cmd, sizeof(arr[count].cmd), "%s", cmd);
        count++;
    }
    for (int i = 0; i < n; i++) free(lines[i]);
    state->bindings = arr;
    state->binding_count = count;
}

/* Apply the editor-level pvim.conf settings: the line-number gutter, the
 * clipboard modifier and the `[bindings]` maps. Shared by editor_init and the
 * `:reload` command so a reload re-reads them exactly like startup does. The
 * environment overrides (PVIM_LINENUMBERS / PVIM_CLIPBOARD_MOD) still win. */
static void editor_apply_config(EditorState *state) {
    /* `linenumbers = absolute|relative|off`; absolute when unset. */
    char *cfg = pvim_config_get("linenumbers");
    const char *setting = getenv("PVIM_LINENUMBERS");
    if (!setting || !*setting) setting = cfg;
    state->line_numbers = parse_linenumber_mode(setting);
    free(cfg);

    /* Clipboard shortcuts: `clipboard_mod = ctrl|alt|super` (default super)
     * selects the modifier combined with `y` (copy) and `p` (paste). */
    char *cmod = pvim_config_get("clipboard_mod");
    const char *modsetting = getenv("PVIM_CLIPBOARD_MOD");
    if (!modsetting || !*modsetting) modsetting = cmod;
    int cbits = parse_clipboard_mod(modsetting);
    state->clipboard_copy_key = clipboard_key(cbits, 'y');
    state->clipboard_paste_key = clipboard_key(cbits, 'p');
    free(cmod);

    /* Custom key bindings from the `[bindings]` config section. */
    bindings_load(state);

    /* Completion settings. `completion` and `completion_auto` toggle the menu
     * on/off; `completion_trigger` is the key that opens it (default <C-Space>,
     * which legacy terminals also send as NUL); `completion_accept` is a
     * whitespace/comma-separated list of keys that accept the selection
     * (default <CR> <Tab>). `parse_bind_key` handles the same `<...>` notation
     * as the `[bindings]` section. */
    char *cen = pvim_config_get("completion");
    state->completion_enabled = parse_onoff(cen, 1);
    free(cen);

    char *cauto = pvim_config_get("completion_auto");
    state->completion_auto = parse_onoff(cauto, 1);
    free(cauto);

    char *trig = pvim_config_get("completion_trigger");
    int tkey = trig ? parse_bind_key(trig) : -1;
    if (tkey < 0) tkey = parse_bind_key("<C-Space>");
    state->completion_trigger_key = tkey;
    free(trig);

    char *acc = pvim_config_get("completion_accept");
    state->completion_accept_count = 0;
    if (acc && *acc) {
        char *save = NULL;
        char *tok = strtok_r(acc, " \t,", &save);
        while (tok && state->completion_accept_count < 4) {
            int k = parse_bind_key(tok);
            if (k >= 0) state->completion_accept_keys[state->completion_accept_count++] = k;
            tok = strtok_r(NULL, " \t,", &save);
        }
    }
    free(acc);
    if (state->completion_accept_count == 0) {
        state->completion_accept_keys[0] = KEY_ENTER;
        state->completion_accept_keys[1] = KEY_TAB;
        state->completion_accept_count = 2;
    }
}

void editor_init(EditorState *state) {
    // collumn x = cx | column y = cy | row = rx
    state->cx = 0;
    state->cy = 0;
    state->rx = 0;
    
    state->v_start_x = 0;
    state->v_start_y = 0;
    
    state->row_offset = 0;
    state->col_offset = 0;
    
    state->mode = MODE_NORMAL;
    state->quit = 0; // ??
    state->status_msg[0] = '\0';
    state->status_msg_time = 0;
    state->search_buf[0] = '\0';
    state->search_len = 0;
    state->active_search[0] = '\0';
    state->yank_buf = NULL;
    state->yank_len = 0;
    state->pending_op = 0;
    state->undo.items = NULL;
    state->undo.count = 0;
    state->undo.cap = 0;
    state->redo.items = NULL;
    state->redo.count = 0;
    state->redo.cap = 0;
    state->undo_active = 0;
    state->bindings = NULL;
    state->binding_count = 0;

    /* Completion menu runtime state (config-derived keys are set below by
     * editor_apply_config). */
    state->lsp_completions = NULL;
    state->completion_request = 0;
    state->completion_inflight = 0;
    state->completion_active = 0;
    state->completion_manual = 0;
    state->completion_sel = 0;
    state->completion_items = NULL;
    state->completion_item_count = 0;
    state->completion_item_cap = 0;
    state->completion_prefix[0] = '\0';
    state->completion_fragment[0] = '\0';
    state->completion_in_include = 0;
    state->completion_include_delim = 0;
    state->completion_req_line = 0;
    state->completion_req_char = 0;

    /* Line numbers, clipboard modifier and `[bindings]` maps from pvim.conf.
     * The same routine backs the `:reload` command. */
    editor_apply_config(state);

    /* Diagnostics are supplied by main once clangd is connected. */
    state->lsp_diags = NULL;

    windows_init(&state->windows);
    explorer_init(&state->explorer);

    buffer_init(&state->buffer);
    editor_update_window_size(state);
}

/* Number of screen columns the gutter needs: the digits of the largest line
 * number plus one separator space. Columns are 1-based per line, so an empty
 * buffer sizes for "1". */
int editor_gutter_width(const EditorState *state) {
    if (state->line_numbers == LN_OFF) return 0;
    int rows = state->buffer.num_rows;
    if (rows < 1) rows = 1;
    int digits = 1;
    while (rows >= 10) {
        rows /= 10;
        digits++;
    }
    return digits + 1;
}

static void snapshot_free(Snapshot *s) {
    for (int i = 0; i < s->num_rows; i++)
        free(s->lines[i]);
    free(s->lines);
    s->lines = NULL;
    s->num_rows = 0;
}

static void stack_clear(UndoStack *st) {
    while (st->count > 0)
        snapshot_free(&st->items[--st->count]);
    free(st->items);
    st->items = NULL;
    st->count = 0;
    st->cap = 0;
}

void editor_free(EditorState *state) {
    buffer_free(&state->buffer);
    windows_free(&state->windows);
    explorer_free(&state->explorer);
    free(state->yank_buf);
    state->yank_buf = NULL;
    state->yank_len = 0;
    free(state->bindings);
    state->bindings = NULL;
    state->binding_count = 0;
    free(state->completion_items);
    state->completion_items = NULL;
    state->completion_item_count = 0;
    state->completion_item_cap = 0;
    stack_clear(&state->undo);
    stack_clear(&state->redo);
}

static Snapshot snapshot_capture(EditorState *state) {
    Snapshot s;
    s.num_rows = state->buffer.num_rows;
    s.cy = state->cy;
    s.cx = state->cx;
    s.lines = malloc(sizeof(char *) * (s.num_rows > 0 ? (size_t)s.num_rows : 1));
    for (int i = 0; i < s.num_rows; i++)
        s.lines[i] = strdup(state->buffer.rows[i].chars);
    return s;
}

static void stack_push(UndoStack *st, Snapshot s) {
    if (st->count == st->cap) {
        st->cap = st->cap ? st->cap * 2 : 16;
        st->items = realloc(st->items, sizeof(Snapshot) * (size_t)st->cap);
    }
    st->items[st->count++] = s;
}

static Snapshot stack_pop(UndoStack *st) {
    return st->items[--st->count];
}

static void snapshot_restore(EditorState *state, Snapshot *s) {
    buffer_clear_rows(&state->buffer);
    for (int i = 0; i < s->num_rows; i++)
        buffer_insert_row(&state->buffer, i, s->lines[i], strlen(s->lines[i]));

    state->cy = s->cy;
    state->cx = s->cx;
    if (state->cy >= state->buffer.num_rows)
        state->cy = state->buffer.num_rows > 0 ? state->buffer.num_rows - 1 : 0;
    if (state->cy < 0) state->cy = 0;
    int rowlen = (state->cy < state->buffer.num_rows) ? state->buffer.rows[state->cy].size : 0;
    if (state->cx > rowlen) state->cx = rowlen;
    if (state->cx < 0) state->cx = 0;
    state->rx = (state->cy < state->buffer.num_rows)
        ? buffer_row_cx_to_rx(&state->buffer.rows[state->cy], state->cx) : 0;
}

void editor_begin_change(EditorState *state) {
    if (state->undo_active) return;
    Snapshot s = snapshot_capture(state);
    stack_push(&state->undo, s);
    /* A new change invalidates the redo history. */
    while (state->redo.count > 0)
        snapshot_free(&state->redo.items[--state->redo.count]);
    state->undo_active = 1;
}

void editor_end_change(EditorState *state) {
    state->undo_active = 0;
}

void editor_undo(EditorState *state) {
    editor_end_change(state);
    if (state->undo.count == 0) {
        editor_set_status_message(state, "Already at oldest change");
        return;
    }
    Snapshot current = snapshot_capture(state);
    stack_push(&state->redo, current);
    Snapshot s = stack_pop(&state->undo);
    snapshot_restore(state, &s);
    snapshot_free(&s);
    editor_set_status_message(state, "Undo");
}

void editor_redo(EditorState *state) {
    editor_end_change(state);
    if (state->redo.count == 0) {
        editor_set_status_message(state, "Already at newest change");
        return;
    }
    Snapshot current = snapshot_capture(state);
    stack_push(&state->undo, current);
    Snapshot s = stack_pop(&state->redo);
    snapshot_restore(state, &s);
    snapshot_free(&s);
    editor_set_status_message(state, "Redo");
}

void editor_update_window_size(EditorState *state) {
    if (terminal_get_window_size(&state->screen_rows, &state->screen_cols) == -1) {
        state->screen_rows = 24;
        state->screen_cols = 80;
    }
    /* The bottom row is the status line; everything above is the text area.
     * (Reserving two rows used to leave a permanently blank row below the
     * status bar, which became obvious after a terminal resize.) */
    state->screen_rows -= 1;
    if (state->screen_rows < 1) state->screen_rows = 1;
}

void editor_set_status_message(EditorState *state, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(state->status_msg, sizeof(state->status_msg), fmt, ap);
    va_end(ap);
    state->status_msg_time = time(NULL);
}

void editor_close_windows(EditorState *state) {
    if (!state) return;
    state->diag_panel_open = 0;
    windows_clear(&state->windows);
}

static void sanitize_ctl_line(char *s);

/* Open a floating overlay listing the current language-server diagnostics.
 * Demonstrates the popup engine (window.c); the same primitive is intended for
 * completion menus and command palettes. */
void editor_show_diagnostics_window(EditorState *state) {
    if (!state) return;
    state->diag_panel_open = 1;
    WindowLayer *wl = &state->windows;
    windows_clear(wl);

    const LspDiagnostics *d = state->lsp_diags;
    int cols = state->screen_cols;
    int rows = state->screen_rows;

    int w = 64;
    if (cols > 0 && w > cols - 2) w = cols - 2;
    if (w < 8) w = (cols > 0) ? cols : 8;

    int count = d ? d->count : 0;

    if (count == 0) {
        int h = 3;
        if (rows > 0 && h > rows) h = rows;
        FloatWindow *win = windows_add(wl, 2, 1, w, h, "Diagnostics",
                                       WIN_BORDER_ROUND, 10);
        if (win) {
            window_set_line(win, 0, d ? " No diagnostics."
                                      : " Language server not running.");
        }
        editor_set_status_message(state, d ? "No diagnostics"
                                           : "No language server");
        return;
    }

    int h = count + 2; /* one row per diagnostic plus top/bottom borders */
    if (rows > 0 && h > rows) h = rows;
    int content = h - 2;

    FloatWindow *win = windows_add(wl, 2, 1, w, h, "Diagnostics",
                                   WIN_BORDER_ROUND, 10);
    if (!win) {
        editor_set_status_message(state, "Out of memory");
        return;
    }
    for (int i = 0; i < count && i < content; i++) {
        const char *sev = "Hint";
        switch (d->items[i].severity) {
            case LSP_SEV_ERROR:   sev = "Error";   break;
            case LSP_SEV_WARNING: sev = "Warning"; break;
            case LSP_SEV_INFO:    sev = "Info";    break;
            default:              sev = "Hint";    break;
        }
        char linebuf[340];
        snprintf(linebuf, sizeof linebuf, " %-7s L%d  %s",
                 sev, d->items[i].line + 1, d->items[i].message);
        sanitize_ctl_line(linebuf);
        window_set_line(win, i, linebuf);
    }
    int extra = count - content;
    if (extra > 0 && content > 0)
        window_printf(win, content - 1, " ... %d more", extra);

    editor_set_status_message(state, "%d diagnostic(s)", count);
}

/* ----------------------------- completion menu --------------------------- */

/* Short tag for a CompletionItemKind, shown next to the label in the menu. */
static const char *completion_kind_tag(int kind) {
    switch (kind) {
        case LSP_CK_METHOD:
        case LSP_CK_FUNCTION:
        case LSP_CK_CONSTRUCTOR: return "fn";
        case LSP_CK_FIELD:       return "field";
        case LSP_CK_VARIABLE:
        case LSP_CK_PROPERTY:    return "var";
        case LSP_CK_CLASS:
        case LSP_CK_STRUCT:      return "class";
        case LSP_CK_INTERFACE:   return "iface";
        case LSP_CK_MODULE:      return "mod";
        case LSP_CK_FILE:        return "hdr";
        case LSP_CK_FOLDER:      return "dir";
        case LSP_CK_KEYWORD:     return "kwd";
        case LSP_CK_SNIPPET:     return "snip";
        case LSP_CK_ENUM:
        case LSP_CK_ENUM_MEMBER: return "enum";
        case LSP_CK_CONSTANT:    return "const";
        case LSP_CK_VALUE:       return "val";
        case LSP_CK_TYPE_PARAM:  return "type";
        case LSP_CK_OPERATOR:    return "op";
        default:                 return "";
    }
}

static int is_ident_char(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* Chars after which a completion refresh makes sense: identifier characters
 * plus the member-access / path punctuation servers complete after. */
static int completion_continue_char(int c) {
    return is_ident_char(c) || c == '.' || c == '>' || c == ':' ||
           c == '/' || c == '"' || c == '<';
}

/* Grow the editor-owned candidate snapshot and hand back the next slot. */
static LspCompletion *completion_new_item(EditorState *state) {
    if (state->completion_item_count == state->completion_item_cap) {
        int ncap = state->completion_item_cap ? state->completion_item_cap * 2 : 16;
        LspCompletion *ni = realloc(state->completion_items,
                                    sizeof(LspCompletion) * (size_t)ncap);
        if (!ni) return NULL;
        state->completion_items = ni;
        state->completion_item_cap = ncap;
    }
    return &state->completion_items[state->completion_item_count++];
}

/* Copy the identifier immediately left of the cursor into `out` (bounded). */
static int editor_completion_prefix(EditorState *state, char *out, size_t n) {
    out[0] = '\0';
    if (state->cy >= state->buffer.num_rows) return 0;
    const erow *row = &state->buffer.rows[state->cy];
    int end = state->cx;
    if (end > row->size) end = row->size;
    int start = end;
    while (start > 0 && is_ident_char((unsigned char)row->chars[start - 1])) start--;
    int len = end - start;
    if (len <= 0) return 0;
    if ((size_t)len >= n) len = (int)n - 1;
    memcpy(out, row->chars + start, (size_t)len);
    out[len] = '\0';
    return len;
}

/* Case-insensitive prefix test; an empty prefix matches everything. */
static int completion_matches(const char *text, const char *prefix) {
    size_t n = strlen(prefix);
    if (n == 0) return 1;
    return strncasecmp(text, prefix, n) == 0;
}

/* Last path component of a slash path (the part after the final '/'). */
static const char *completion_basename(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* The delimiter that closes an include opened with `open` ('<' -> '>'). */
static char completion_include_close(char open) {
    return open == '<' ? '>' : '"';
}

/* Detect the cursor sitting inside an `#include` directive's path. On success
 * `*frag_start`/`*frag_end` bound the typed path (from just after the opening
 * delimiter up to the cursor) and `*delim` is '<' or '"'. Leading whitespace
 * before the '#' is allowed; a cursor past the closing delimiter, or in
 * whitespace, is not an include context. */
static int editor_completion_include_context(const EditorState *state,
                                             int *frag_start, int *frag_end,
                                             char *delim) {
    if (state->cy < 0 || state->cy >= state->buffer.num_rows) return 0;
    const erow *row = &state->buffer.rows[state->cy];
    int cx = state->cx;
    if (cx > row->size) cx = row->size;
    if (cx < 0) cx = 0;

    int i = 0;
    while (i < cx && (row->chars[i] == ' ' || row->chars[i] == '\t')) i++;
    if (i >= cx || row->chars[i] != '#') return 0;
    i++;
    while (i < cx && (row->chars[i] == ' ' || row->chars[i] == '\t')) i++;
    static const char kw[] = "include";
    for (int k = 0; kw[k]; k++) {
        if (i >= cx || row->chars[i] != kw[k]) return 0;
        i++;
    }
    while (i < cx && (row->chars[i] == ' ' || row->chars[i] == '\t')) i++;
    if (i >= cx) return 0;
    char d = row->chars[i];
    if (d != '<' && d != '"') return 0;

    int start = i + 1;
    char close = completion_include_close(d);
    for (int j = start; j < cx; j++) {
        char ch = row->chars[j];
        if (ch == d || ch == close || ch == ' ' || ch == '\t' || ch == '\r') return 0;
    }
    *frag_start = start;
    *frag_end = cx;
    *delim = d;
    return 1;
}

/* Match a clangd include item against the typed path fragment. The server has
 * already resolved the directory, so only the final component is compared. */
static int completion_component_matches(const char *cand, const char *component) {
    size_t clen = strlen(component);
    if (clen == 0) return 1;
    return strncasecmp(completion_basename(cand), component, clen) == 0;
}

/* Match a locally scanned header path against the fragment: the stored relative
 * path is exactly the text that belongs in the directive (each include root is
 * indexed separately), so a plain prefix test is right. Matching only the
 * basename would offer `sub/foo.h` for `foo`, which would insert a wrong
 * `#include "foo.h"`. */
static int completion_header_matches(const char *path, const char *fragment) {
    size_t n = strlen(fragment);
    if (n == 0) return 1;
    return strncasecmp(path, fragment, n) == 0;
}

/* Append unique words from the buffer that start with `prefix` (a non-LSP
 * fallback so completions work without a language server). */
static void editor_completion_collect_words(EditorState *state, const char *prefix) {
    size_t plen = strlen(prefix);
    /* The identifier run the cursor sits in on its own row: never offer it back
     * to itself. Without this, typing the first letter of "one" would make the
     * token "Xone" and the menu would immediately suggest "Xone". */
    int cur_row = state->cy;
    int cur_s = -1, cur_e = -1;
    if (cur_row >= 0 && cur_row < state->buffer.num_rows) {
        const erow *cr = &state->buffer.rows[cur_row];
        int p = state->cx;
        if (p > cr->size) p = cr->size;
        cur_s = p;
        while (cur_s > 0 && is_ident_char((unsigned char)cr->chars[cur_s - 1])) cur_s--;
        cur_e = p;
        while (cur_e < cr->size && is_ident_char((unsigned char)cr->chars[cur_e])) cur_e++;
    }
    for (int y = 0; y < state->buffer.num_rows; y++) {
        const erow *row = &state->buffer.rows[y];
        int i = 0;
        while (i < row->size) {
            if (!is_ident_char((unsigned char)row->chars[i])) { i++; continue; }
            int s = i;
            while (i < row->size && is_ident_char((unsigned char)row->chars[i])) i++;
            int len = i - s;
            if (len <= 0 || len >= 128) continue;
            if (y == cur_row && s == cur_s && i == cur_e) continue;   /* the token under the cursor */
            if ((size_t)len < plen) continue;
            if (plen && strncasecmp(row->chars + s, prefix, plen) != 0) continue;
            /* Skip the word currently being typed. */
            if ((size_t)len == plen && plen && strncmp(row->chars + s, prefix, plen) == 0)
                continue;
            int dup = 0;
            for (int k = 0; k < state->completion_item_count; k++) {
                if (strlen(state->completion_items[k].label) == (size_t)len &&
                    strncmp(state->completion_items[k].label, row->chars + s, (size_t)len) == 0) {
                    dup = 1;
                    break;
                }
            }
            if (dup) continue;
            if (state->completion_item_count >= LSP_COMPLETION_MAX) return;
            LspCompletion *it = completion_new_item(state);
            if (!it) return;
            memset(it, 0, sizeof *it);
            memcpy(it->label, row->chars + s, (size_t)len);
            it->label[len] = '\0';
            snprintf(it->insert, sizeof(it->insert), "%s", it->label);
            snprintf(it->filter, sizeof(it->filter), "%s", it->label);
            it->source = LSP_SRC_BUFFER;
        }
    }
}

/* One column of a completion menu line. `role` distinguishes the signature
 * ('S'), the kind tag ('K') and the source badge ('B') so the caller can drop
 * the signature first when the terminal is too narrow. */
typedef struct { char text[288]; int style; char role; } CompPart;
#define COMP_MAX_PARTS 4

/* Build the right-hand column of a candidate: the return type + name +
 * parameter list (functions), the kind tag and the source badge, e.g.
 * "void LookAt(Transform t, Vector3 up) fn [LSP]". */
static int completion_right_column(const LspCompletion *it,
                                   CompPart parts[COMP_MAX_PARTS]) {
    int n = 0;
    int is_fn = (it->kind == LSP_CK_FUNCTION || it->kind == LSP_CK_METHOD ||
                 it->kind == LSP_CK_CONSTRUCTOR);
    const char *ret = it->label_desc[0] ? it->label_desc : it->detail;

    if (is_fn && (it->label_detail[0] || ret[0])) {
        if (it->label_detail[0] && ret[0])
            snprintf(parts[n].text, sizeof parts[n].text, "%s %s%s",
                     ret, it->label, it->label_detail);
        else if (it->label_detail[0])
            snprintf(parts[n].text, sizeof parts[n].text, "%s%s",
                     it->label, it->label_detail);
        else
            snprintf(parts[n].text, sizeof parts[n].text, "%s %s", ret, it->label);
        parts[n].style = WIN_STYLE_MUTED;
        parts[n].role = 'S';
        n++;
    } else if (it->detail[0]) {
        snprintf(parts[n].text, sizeof parts[n].text, "%s", it->detail);
        parts[n].style = WIN_STYLE_MUTED;
        parts[n].role = 'S';
        n++;
    }

    const char *tag = completion_kind_tag(it->kind);
    if (tag[0]) {
        snprintf(parts[n].text, sizeof parts[n].text, "%s", tag);
        parts[n].style = WIN_STYLE_MUTED;
        parts[n].role = 'K';
        n++;
    }

    snprintf(parts[n].text, sizeof parts[n].text, "%s",
             it->source == LSP_SRC_BUFFER ? "[Buffer]"
             : it->source == LSP_SRC_HEADER ? "[Hdr]"
             : it->snippet                 ? "[Snip]"
                                           : "[LSP]");
    parts[n].style = WIN_STYLE_ACCENT;
    parts[n].role = 'B';
    n++;
    return n;
}

/* Join the selected parts into `out`, recording one styled span per part (the
 * separating space belongs to the following part). Returns the joined length. */
static int completion_join_parts(const CompPart *parts, int n,
                                 int keep_sig, int keep_tag,
                                 char *out, size_t outsz,
                                 WinSpan *spans, int *span_n) {
    int len = 0, sn = 0;
    for (int k = 0; k < n; k++) {
        if (parts[k].role == 'S' && !keep_sig) continue;
        if (parts[k].role == 'K' && !keep_tag) continue;
        int tl = (int)strlen(parts[k].text);
        int sep = (sn > 0) ? 1 : 0;
        if ((size_t)len + (size_t)sep + (size_t)tl + 1 > outsz) break;
        int start = len;
        if (sep) out[len++] = ' ';
        memcpy(out + len, parts[k].text, (size_t)tl);
        len += tl;
        if (spans && sn < COMP_MAX_PARTS) {
            spans[sn].col = start;
            spans[sn].len = len - start;
            spans[sn].style = parts[k].style;
        }
        sn++;
    }
    out[len] = '\0';
    if (span_n) *span_n = sn;
    return len;
}

/* Draw (or redraw) the popup from the current snapshot as two aligned columns:
 * the clean label on the left and the signature + kind tag + source badge,
 * right-aligned, on the right. The menu is anchored just below the cursor and
 * fitted by the window engine so it can never cover the status line. */
static void editor_completion_draw(EditorState *state) {
    windows_clear(&state->windows);
    state->diag_panel_open = 0;

    int n = state->completion_item_count;
    if (n <= 0) return;

    int sel = state->completion_sel;
    if (sel < 0) sel = 0;
    if (sel >= n) sel = n - 1;

    int max_rows = 10;
    if (state->screen_rows > 2 && max_rows > state->screen_rows - 2)
        max_rows = state->screen_rows - 2;
    if (max_rows < 1) max_rows = 1;
    int shown = n < max_rows ? n : max_rows;

    int scroll = (sel >= max_rows) ? sel - max_rows + 1 : 0;
    if (scroll > n - shown) scroll = n - shown;
    if (scroll < 0) scroll = 0;

    /* Pre-compute every visible row so the popup can be sized to the widest. */
    CompPart parts[10][COMP_MAX_PARTS];
    int part_n[10];
    int max_label = 0, max_right = 0;
    for (int i = 0; i < shown; i++) {
        const LspCompletion *it = &state->completion_items[scroll + i];
        int lw = (int)strlen(it->label);
        if (lw > max_label) max_label = lw;
        part_n[i] = completion_right_column(it, parts[i]);
        char tmp[320];
        WinSpan sp[COMP_MAX_PARTS];
        int sn = 0;
        int rw = completion_join_parts(parts[i], part_n[i], 1, 1,
                                       tmp, sizeof tmp, sp, &sn);
        if (rw > max_right) max_right = rw;
    }

    /* width = border(2) + left pad(1) + label + gap(2) + right + right pad(1) */
    int content = 1 + max_label + 2 + max_right + 1;
    int width = content + 2;
    if (width < 12) width = 12;
    int maxw = state->screen_cols;
    if (maxw < 4) maxw = state->screen_cols;
    if (width > maxw) width = maxw;
    if (width > 400) width = 400;

    int cw = width - 2;                 /* inner content width */
    if (cw < 1) cw = 1;
    int compact = content > cw;         /* too narrow for the signature column */

    int h = shown + 2;                  /* top and bottom border */
    if (state->screen_rows > 0 && h > state->screen_rows) h = state->screen_rows;

    int gutter = editor_gutter_width(state);
    int x = gutter + (state->rx - state->col_offset);
    int y = (state->cy - state->row_offset) + 1;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    FloatWindow *win = windows_add(&state->windows, x, y, width, h, NULL,
                                   WIN_BORDER_ROUND, 20);
    if (!win) return;
    win->scroll = scroll;
    win->selected = sel;

    char line[420];
    for (int i = 0; i < shown; i++) {
        const LspCompletion *it = &state->completion_items[scroll + i];
        int llen = (int)strlen(it->label);
        if (llen > cw - 3) llen = (cw - 3 > 0) ? cw - 3 : 0;

        /* Keep the badge visible: drop the signature, then the kind tag. */
        int keep_sig = !compact, keep_tag = 1;
        char right[320];
        WinSpan rspans[COMP_MAX_PARTS];
        int rsn = 0;
        int rl = completion_join_parts(parts[i], part_n[i], keep_sig, keep_tag,
                                       right, sizeof right, rspans, &rsn);
        int avail = cw - llen - 3;      /* columns left for the right column */
        if (avail < 0) avail = 0;
        if (rl > avail && keep_sig) {
            keep_sig = 0;
            rl = completion_join_parts(parts[i], part_n[i], keep_sig, keep_tag,
                                       right, sizeof right, rspans, &rsn);
        }
        if (rl > avail && keep_tag) {
            keep_tag = 0;
            rl = completion_join_parts(parts[i], part_n[i], keep_sig, keep_tag,
                                       right, sizeof right, rspans, &rsn);
        }
        if (rl > avail) { rl = avail; right[rl] = '\0'; rsn = 0; }

        int right_start = cw - 1 - rl;  /* right-aligned, one trailing space */
        if (right_start < llen + 2) right_start = llen + 2;
        if (right_start > cw) right_start = cw;

        memset(line, ' ', (size_t)cw);
        line[cw] = '\0';
        memcpy(line + 1, it->label, (size_t)llen);

        WinSpan spans[COMP_MAX_PARTS + 1];
        int span_n = 0;
        spans[span_n].col = 1;
        spans[span_n].len = llen;
        spans[span_n].style = WIN_STYLE_TEXT;
        span_n++;

        if (rl > 0 && right_start + rl <= cw) {
            memcpy(line + right_start, right, (size_t)rl);
            for (int k = 0; k < rsn && span_n < COMP_MAX_PARTS + 1; k++) {
                spans[span_n].col = right_start + rspans[k].col;
                spans[span_n].len = rspans[k].len;
                spans[span_n].style = rspans[k].style;
                span_n++;
            }
        }

        sanitize_ctl_line(line);
        window_set_line_spans(win, i, line, spans, span_n);
    }
}

/* Public: end the completion session (accept, cancel, mode change, ...). */
void editor_completion_close(EditorState *state) {
    if (!state) return;
    state->completion_active = 0;
    state->completion_manual = 0;
    state->completion_inflight = 0;
    state->completion_request = 0;
    state->completion_sel = 0;
    state->completion_item_count = 0;
    state->completion_prefix[0] = '\0';
    state->completion_fragment[0] = '\0';
    state->completion_in_include = 0;
    state->completion_include_delim = 0;
    windows_clear(&state->windows);
    state->diag_panel_open = 0;
}

/* Directory the open file lives in; "." when the buffer is unnamed. Backs the
 * quoted-include scan (`#include "foo.h"` resolves relative to the file). */
static void editor_buffer_dir(const EditorState *state, char *out, size_t n) {
    const char *fn = state->buffer.filename;
    out[0] = '\0';
    if (!fn || !fn[0]) { snprintf(out, n, "."); return; }
    const char *slash = strrchr(fn, '/');
    if (!slash) { snprintf(out, n, "."); return; }
    size_t len = (size_t)(slash - fn);
    if (len == 0) len = 1;                 /* "/file" -> "/" */
    if (len >= n) len = n - 1;
    memcpy(out, fn, len);
    out[len] = '\0';
}

/* Fallback source for `#include`: header files found on disk by the local
 * index. Only consulted when clangd returned nothing to show. */
static void editor_completion_add_headers(EditorState *state) {
    char dir[4096];
    editor_buffer_dir(state, dir, sizeof dir);
    const HeaderIndex *ix = headers_for(dir, state->completion_include_delim);
    if (!ix) return;
    for (int i = 0; i < ix->count && state->completion_item_count < LSP_COMPLETION_MAX; i++) {
        const char *path = ix->paths[i];
        if (!completion_header_matches(path, state->completion_fragment)) continue;
        LspCompletion *it = completion_new_item(state);
        if (!it) return;
        memset(it, 0, sizeof *it);
        snprintf(it->label, sizeof(it->label), "%s", path);
        snprintf(it->insert, sizeof(it->insert), "%s", completion_basename(path));
        snprintf(it->filter, sizeof(it->filter), "%s", path);
        it->kind = LSP_CK_FILE;
        it->source = LSP_SRC_HEADER;
    }
}

/* Rebuild the snapshot from the last LSP result plus buffer words, filtered by
 * the prefix at the cursor, and draw it. An empty result leaves the session
 * live (a server reply may still arrive) but with no popup. */
static void editor_completion_open(EditorState *state) {
    int fs = 0, fe = 0;
    char delim = 0;
    state->completion_in_include =
        editor_completion_include_context(state, &fs, &fe, &delim);
    state->completion_include_delim = delim;

    if (state->completion_in_include) {
        /* The replacement prefix is the path component after the last '/', the
         * same slice clangd's textEdit range replaces; the full fragment is
         * kept for matching the local header index. */
        const erow *row = &state->buffer.rows[state->cy];
        int cs = fs;
        for (int i = fs; i < fe; i++)
            if (row->chars[i] == '/') cs = i + 1;
        int clen = fe - cs;
        if (clen < 0) clen = 0;
        if ((size_t)clen >= sizeof(state->completion_prefix))
            clen = (int)sizeof(state->completion_prefix) - 1;
        memcpy(state->completion_prefix, row->chars + cs, (size_t)clen);
        state->completion_prefix[clen] = '\0';

        int flen = fe - fs;
        if (flen < 0) flen = 0;
        if ((size_t)flen >= sizeof(state->completion_fragment))
            flen = (int)sizeof(state->completion_fragment) - 1;
        memcpy(state->completion_fragment, row->chars + fs, (size_t)flen);
        state->completion_fragment[flen] = '\0';
    } else {
        state->completion_fragment[0] = '\0';
        editor_completion_prefix(state, state->completion_prefix,
                                 sizeof(state->completion_prefix));
    }

    state->completion_item_count = 0;

    if (state->lsp_completions) {
        const LspCompletions *lc = state->lsp_completions;
        for (int i = 0; i < lc->count && state->completion_item_count < LSP_COMPLETION_MAX; i++) {
            const LspCompletion *src = &lc->items[i];
            const char *key = src->filter[0] ? src->filter : src->label;
            int match = state->completion_in_include
                ? completion_component_matches(key, state->completion_prefix)
                : completion_matches(key, state->completion_prefix);
            if (!match) continue;
            LspCompletion *it = completion_new_item(state);
            if (!it) break;
            *it = *src;
        }
    }

    if (state->completion_in_include) {
        if (state->completion_item_count == 0)
            editor_completion_add_headers(state);
    } else {
        editor_completion_collect_words(state, state->completion_prefix);
    }

    if (state->completion_item_count == 0) {
        windows_clear(&state->windows);
        state->diag_panel_open = 0;
        state->completion_sel = 0;
        return;
    }

    if (state->completion_sel < 0 || state->completion_sel >= state->completion_item_count)
        state->completion_sel = 0;
    editor_completion_draw(state);
}

/* Record that main should ask the server for completions at the cursor. */
static void editor_completion_note_request(EditorState *state) {
    if (!state->lsp_completions) return;
    state->completion_request = 1;
    state->completion_inflight = 1;
    state->completion_req_line = state->cy;
    state->completion_req_char = editor_cursor_utf16(state);
}

/* Open the menu in response to the trigger key or auto-completion. */
static void editor_completion_trigger(EditorState *state) {
    state->completion_active = 1;
    state->completion_sel = 0;
    editor_completion_open(state);

    if (state->lsp_completions) {
        editor_completion_note_request(state);
    } else if (state->completion_item_count == 0) {
        state->completion_active = 0;
        editor_set_status_message(state, "No completions");
    }
}

/* Re-filter after an edit while the menu is open, and refresh the server's
 * view when auto-completion is on. */
static void editor_completion_refresh(EditorState *state) {
    editor_completion_open(state);
    if (state->completion_item_count == 0 && !state->completion_inflight) {
        editor_completion_close(state);
        return;
    }
    if (state->completion_auto)
        editor_completion_note_request(state);
}

/* Insert the selected candidate, replacing the typed prefix. */
static void editor_completion_accept(EditorState *state) {
    int n = state->completion_item_count;
    int sel = state->completion_sel;
    if (n <= 0 || sel < 0 || sel >= n) { editor_completion_close(state); return; }

    char insert[256];
    snprintf(insert, sizeof(insert), "%s", state->completion_items[sel].insert);
    int prefix_len = (int)strlen(state->completion_prefix);
    if (prefix_len > state->cx) prefix_len = state->cx;
    int in_include = state->completion_in_include;
    char delim = state->completion_include_delim;

    editor_completion_close(state);
    editor_begin_change(state);
    int ilen = (int)strlen(insert);
    if (state->cy < state->buffer.num_rows) {
        for (int i = 0; i < prefix_len && state->cx > 0; i++) {
            buffer_row_del_char(&state->buffer, &state->buffer.rows[state->cy], state->cx - 1);
            state->cx--;
        }
        /* In an include directive, settle the closing delimiter: never double
         * it, and add it when neither the item nor the line already has it. */
        if (in_include && delim) {
            char close = completion_include_close(delim);
            const erow *row = &state->buffer.rows[state->cy];
            int ends = ilen > 0 && insert[ilen - 1] == close;
            int has = state->cx < row->size && row->chars[state->cx] == close;
            if (ends && has)
                insert[--ilen] = '\0';
            else if (!ends && !has && ilen < (int)sizeof(insert) - 1)
                insert[ilen++] = close, insert[ilen] = '\0';
        }
    }
    editor_insert_text(state, insert, ilen);
    editor_end_change(state);
    state->rx = (state->cy < state->buffer.num_rows)
        ? buffer_row_cx_to_rx(&state->buffer.rows[state->cy], state->cx) : 0;
    editor_set_status_message(state, "");
}

/* Handle a key while the menu is open. Returns 1 when the key was consumed,
 * 0 when it is an editing key (apply, then refresh) and 2 when the menu was
 * closed and the key should be processed normally. */
static int editor_completion_menu_key(EditorState *state, int c) {
    for (int i = 0; i < state->completion_accept_count; i++) {
        if (c == state->completion_accept_keys[i]) {
            editor_completion_accept(state);
            return 1;
        }
    }
    if (c == KEY_ESC) {
        editor_completion_close(state);
        editor_set_status_message(state, "");
        return 1;
    }
    if (c == CTRL_KEY('n') || c == ARROW_DOWN) {
        int n = state->completion_item_count;
        if (n > 0) state->completion_sel = (state->completion_sel + 1) % n;
        editor_completion_draw(state);
        return 1;
    }
    if (c == CTRL_KEY('p') || c == ARROW_UP) {
        int n = state->completion_item_count;
        if (n > 0) state->completion_sel = (state->completion_sel - 1 + n) % n;
        editor_completion_draw(state);
        return 1;
    }
    if (c == BACKSPACE || (c >= 32 && c <= 126))
        return 0;
    editor_completion_close(state);
    return 2;
}

/* Public: main calls this once a textDocument/completion response has landed. */
void editor_completion_update(EditorState *state) {
    if (!state) return;
    state->completion_inflight = 0;
    if (!state->completion_active) return;
    editor_completion_open(state);
    if (state->completion_item_count == 0) {
        editor_completion_close(state);
        editor_set_status_message(state, "No completions");
    }
}

/* True when `c` should open the menu: the configured trigger key, plus a raw
 * NUL, which is what legacy terminals send for Ctrl-Space (the default). */
static int completion_trigger_pressed(const EditorState *state, int c) {
    if (c == state->completion_trigger_key) return 1;
    if (c == 0 && state->completion_trigger_key == terminal_combo(KEY_MOD_CTRL, ' '))
        return 1;
    return 0;
}

int editor_cursor_utf16(const EditorState *state) {
    if (!state || state->cy < 0 || state->cy >= state->buffer.num_rows) return 0;
    const erow *row = &state->buffer.rows[state->cy];
    int cx = state->cx;
    if (cx > row->size) cx = row->size;
    int u16 = 0;
    for (int i = 0; i < cx;) {
        unsigned char ch = (unsigned char)row->chars[i];
        int len;
        unsigned cp;
        if (ch < 0x80)             { len = 1; cp = ch; }
        else if ((ch & 0xE0) == 0xC0) { len = 2; cp = ch & 0x1F; }
        else if ((ch & 0xF0) == 0xE0) { len = 3; cp = ch & 0x0F; }
        else if ((ch & 0xF8) == 0xF0) { len = 4; cp = ch & 0x07; }
        else                       { len = 1; cp = ch; }
        for (int k = 1; k < len && i + k < cx; k++)
            cp = (cp << 6) | ((unsigned char)row->chars[i + k] & 0x3F);
        if (i + len > cx) len = cx - i;   /* truncated sequence at the cursor */
        if (len < 1) len = 1;
        u16 += (cp > 0xFFFF) ? 2 : 1;
        i += len;
    }
    return u16;
}

/* Replace control bytes (newlines, ESC, ...) with spaces so untrusted message
 * text can never move the terminal cursor and corrupt the frame. UTF-8 bytes
 * (>= 0x80) pass through untouched. */
static void sanitize_ctl_line(char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c == 0x7f) *s = ' ';
    }
}

void editor_load_file(EditorState *state, const char *path) {
    buffer_clear_rows(&state->buffer);
    if (buffer_open(&state->buffer, path) != 0) {
        editor_set_status_message(state, "Cannot open \"%s\"", path);
        return;
    }
    highlight_select_for_buffer(&state->buffer);
    state->cx = 0;
    state->cy = 0;
    state->rx = 0;
    state->row_offset = 0;
    state->col_offset = 0;
    state->pending_op = 0;
    stack_clear(&state->undo);
    stack_clear(&state->redo);
    state->undo_active = 0;
    explorer_close(state);
    editor_completion_close(state);
    state->mode = MODE_NORMAL;
    editor_set_status_message(state, "\"%s\"", path);
}

static void editor_move_cursor(EditorState *state, int key) {
    switch (key) {
        case ARROW_LEFT:
            if (state->cx != 0) state->cx--;
            break;
        case ARROW_RIGHT:
            if (state->cy < state->buffer.num_rows) {
                if (state->cx < state->buffer.rows[state->cy].size) state->cx++;
            }
            break;
        case ARROW_UP:
            if (state->cy != 0) state->cy--;
            break;
        case ARROW_DOWN:
            if (state->cy < state->buffer.num_rows - 1) state->cy++;
            break;
    }
    
    int rowlen = (state->cy < state->buffer.num_rows) ? state->buffer.rows[state->cy].size : 0;
    if (state->cx > rowlen) state->cx = rowlen;
    if (state->cy < state->buffer.num_rows)
        state->rx = buffer_row_cx_to_rx(&state->buffer.rows[state->cy], state->cx);
}

/* Keep the cursor on screen by adjusting the row/column offsets. Scrolling
 * only crosses the visible window edge and stops at the first/last line of
 * text (and at column 0 / the end of the longest row). */
void editor_scroll(EditorState *state) {
    if (state->cy < state->row_offset)
        state->row_offset = state->cy;
    if (state->cy >= state->row_offset + state->screen_rows)
        state->row_offset = state->cy - state->screen_rows + 1;

    /* The gutter eats `gutter` columns; horizontal scrolling only concerns the
     * text viewport that remains. */
    int text_cols = state->screen_cols - editor_gutter_width(state);
    if (text_cols < 1) text_cols = 1;

    if (state->rx < state->col_offset)
        state->col_offset = state->rx;
    if (state->rx >= state->col_offset + text_cols)
        state->col_offset = state->rx - text_cols + 1;

    /* Clamp so we never scroll past the start or end of the text. */
    int max_ro = (state->buffer.num_rows > state->screen_rows)
                 ? state->buffer.num_rows - state->screen_rows : 0;
    if (state->row_offset > max_ro) state->row_offset = max_ro;
    if (state->row_offset < 0) state->row_offset = 0;

    int max_col = 0;
    for (int i = 0; i < state->buffer.num_rows; i++)
        if (state->buffer.rows[i].rsize > max_col)
            max_col = state->buffer.rows[i].rsize;
    int max_co = (max_col > text_cols) ? max_col - text_cols : 0;
    if (state->col_offset > max_co) state->col_offset = max_co;
    if (state->col_offset < 0) state->col_offset = 0;
}

/* Normalize the visual selection so that (sy,sx) <= (ey,ex). Inclusive.
 * Endpoints are clamped to valid buffer positions so callers never compute
 * negative or out-of-range copy lengths. `ex` may be -1 for an empty end row. */
void editor_get_selection(EditorState *state, int *sy, int *sx, int *ey, int *ex) {
    int n = state->buffer.num_rows;
    if (n <= 0) { *sy = 0; *sx = 0; *ey = 0; *ex = -1; return; }

    int ay = state->cy, ax = state->cx;
    int by = state->v_start_y, bx = state->v_start_x;
    if (ay > by || (ay == by && ax > bx)) {
        *sy = by; *sx = bx; *ey = ay; *ex = ax;
    } else {
        *sy = ay; *sx = ax; *ey = by; *ex = bx;
    }

    if (*sy < 0) *sy = 0;
    if (*sy > n - 1) *sy = n - 1;
    if (*ey < 0) *ey = 0;
    if (*ey > n - 1) *ey = n - 1;
    if (*ey < *sy) *ey = *sy;

    int sylen = state->buffer.rows[*sy].size;
    int eylen = state->buffer.rows[*ey].size;

    /* Start may legally point one past the last char. */
    if (*sx < 0) *sx = 0;
    if (*sx > sylen) *sx = sylen;

    /* End is inclusive, so clamp to the last character index; empty -> -1. */
    if (*ex > eylen - 1) *ex = eylen - 1;
    if (*ex < -1) *ex = -1;
}

static void editor_yank_text(EditorState *state, const char *text, int len) {
    free(state->yank_buf);
    state->yank_buf = malloc((size_t)len + 1);
    memcpy(state->yank_buf, text, (size_t)len);
    state->yank_buf[len] = '\0';
    state->yank_len = len;
}

/* Copy row `y` into the yank register, including its newline, so a later paste
 * restores a whole line. Shared by `yy` and by the line-deleting operators. */
static void editor_yank_row(EditorState *state, int y) {
    if (y < 0 || y >= state->buffer.num_rows) return;
    int len = state->buffer.rows[y].size;
    char *text = malloc((size_t)len + 2);
    if (!text) return;
    memcpy(text, state->buffer.rows[y].chars, (size_t)len);
    text[len] = '\n';
    text[len + 1] = '\0';
    editor_yank_text(state, text, len + 1);
    free(text);
}

static void editor_delete_row(EditorState *state, int y) {
    if (y < 0 || y >= state->buffer.num_rows) return;
    /* Line-wise yank: include the trailing newline so paste restores a row. */
    editor_yank_row(state, y);
    buffer_del_row(&state->buffer, y);
    if (state->cy >= state->buffer.num_rows) state->cy = state->buffer.num_rows - 1;
    if (state->cy < 0) state->cy = 0;
}

/* Delete one unit in the direction of an arrow key while the `d` operator is
 * pending (no separate mode). Up/Down remove the whole row in that direction;
 * Left/Right remove a single character in that direction. Removed text is
 * yanked so it can be pasted back. */
static void editor_delete_direction(EditorState *state, int key) {
    int deleted = 0;
    editor_begin_change(state);

    switch(key)
    {
        case ARROW_UP:
            if (state->cy > 0) {
                editor_delete_row(state, state->cy - 1);
                deleted = 1;
            }
            break;
       case ARROW_DOWN:
            if (state->cy < state->buffer.num_rows - 1) {
                editor_delete_row(state, state->cy + 1);
                deleted = 1;
            }
            break;
        case ARROW_LEFT:
            if (state->cy < state->buffer.num_rows && state->cx > 0) {
                erow *row = &state->buffer.rows[state->cy];
                char ch = row->chars[state->cx - 1];
                editor_yank_text(state, &ch, 1);
                buffer_row_del_char(&state->buffer, row, state->cx - 1);
                state->cx--;
                deleted = 1;
            }
            break;
        case ARROW_RIGHT:
            if (state->cy < state->buffer.num_rows) {
                erow *row = &state->buffer.rows[state->cy];
                if (state->cx < row->size) {
                    char ch = row->chars[state->cx];
                    editor_yank_text(state, &ch, 1);
                    buffer_row_del_char(&state->buffer, row, state->cx);
                    deleted = 1;
                }
            }            
            break; 
    }
    editor_end_change(state);

    int rowlen = (state->cy < state->buffer.num_rows) ? state->buffer.rows[state->cy].size : 0;
    if (state->cx > rowlen) state->cx = rowlen;
    if (state->cx < 0) state->cx = 0;
    state->rx = (state->cy < state->buffer.num_rows)
        ? buffer_row_cx_to_rx(&state->buffer.rows[state->cy], state->cx) : 0;

    /* Report the result and drop out of the pending `d` operator. */
    state->pending_op = 0;
    if (deleted)
        editor_set_status_message(state, key == ARROW_UP || key == ARROW_DOWN
                                        ? "Deleted line" : "Deleted char");
    else
        editor_set_status_message(state, "");
}

void editor_visual_delete(EditorState *state);

static char *editor_selection_to_string(EditorState *state, int *out_len) {
    int sy, sx, ey, ex;
    editor_get_selection(state, &sy, &sx, &ey, &ex);

    /* Empty selection (e.g. cursor past end of an empty row). */
    if (sy == ey && ex < sx) {
        char *empty = malloc(1);
        empty[0] = '\0';
        if (out_len) *out_len = 0;
        return empty;
    }

    size_t cap = 0;
    if (sy == ey) {
        cap = (size_t)(ex - sx + 1);
    } else {
        cap = (size_t)(state->buffer.rows[sy].size - sx) + 1;
        for (int r = sy + 1; r < ey; r++) cap += (size_t)state->buffer.rows[r].size + 1;
        cap += (size_t)ex + 1;
    }

    char *out = malloc(cap + 1);
    size_t o = 0;
    if (sy == ey) {
        memcpy(out, state->buffer.rows[sy].chars + sx, (size_t)(ex - sx + 1));
        o = (size_t)(ex - sx + 1);
    } else {
        memcpy(out + o, state->buffer.rows[sy].chars + sx, (size_t)(state->buffer.rows[sy].size - sx));
        o += (size_t)(state->buffer.rows[sy].size - sx);
        for (int r = sy + 1; r < ey; r++) {
            out[o++] = '\n';
            memcpy(out + o, state->buffer.rows[r].chars, (size_t)state->buffer.rows[r].size);
            o += (size_t)state->buffer.rows[r].size;
        }
        out[o++] = '\n';
        memcpy(out + o, state->buffer.rows[ey].chars, (size_t)ex + 1);
        o += (size_t)ex + 1;
    }
    out[o] = '\0';
    if (out_len) *out_len = (int)o;
    return out;
}

void editor_yank(EditorState *state) {
    int len;
    char *text = editor_selection_to_string(state, &len);
    editor_yank_text(state, text, len);
    free(text);
    editor_set_status_message(state, "Yanked %d char%s", len, len == 1 ? "" : "s");
}

void editor_visual_delete(EditorState *state) {
    if (state->buffer.num_rows == 0) return;
    int sy, sx, ey, ex;
    editor_get_selection(state, &sy, &sx, &ey, &ex);

    EditorBuffer *buf = &state->buffer;
    if (sy == ey) {
        for (int i = sx; i <= ex; i++)
            buffer_row_del_char(buf, &buf->rows[sy], sx);
    } else {
        int suffix_len = buf->rows[ey].size - (ex + 1);
        if (suffix_len < 0) suffix_len = 0;
        char *suffix = malloc((size_t)suffix_len + 1);
        if (suffix_len > 0)
            memcpy(suffix, buf->rows[ey].chars + ex + 1, (size_t)suffix_len);
        suffix[suffix_len] = '\0';

        if (sx > buf->rows[sy].size) sx = buf->rows[sy].size;
        buf->rows[sy].chars[sx] = '\0';
        buf->rows[sy].size = sx;
        buffer_update_row(&buf->rows[sy]);
        highlight_update_buffer(buf, sy);

        for (int r = ey; r > sy; r--)
            buffer_del_row(buf, r);

        if (suffix_len > 0)
            buffer_row_append_string(buf, &buf->rows[sy], suffix, (size_t)suffix_len);
        free(suffix);
    }

    state->cy = sy;
    state->cx = sx;
    int rowlen = (state->cy < buf->num_rows) ? buf->rows[state->cy].size : 0;
    if (state->cx > rowlen) state->cx = rowlen;
    state->rx = (state->cy < buf->num_rows)
        ? buffer_row_cx_to_rx(&buf->rows[state->cy], state->cx) : 0;
    editor_set_status_message(state, "Deleted selection");
}

void editor_insert_text(EditorState *state, const char *text, int len) {
    if (!text || len <= 0) return;

    /* A trailing newline means the text holds whole lines (e.g. from `dd` or a
     * line-wise clipboard copy), so it inserts new rows rather than splitting
     * the current line. */
    int linewise = (text[len - 1] == '\n');

    if (state->buffer.num_rows == 0) {
        if (!linewise) buffer_insert_row(&state->buffer, 0, "", 0);
    }

    if (linewise) {
        int at = (state->buffer.num_rows == 0) ? 0 : state->cy + 1;
        int first = at;
        int start = 0;
        for (int i = 0; i < len; i++) {
            if (text[i] == '\n') {
                buffer_insert_row(&state->buffer, at, text + start, (size_t)(i - start));
                at++;
                start = i + 1;
            }
        }
        if (at > first) {
            state->cy = first;
            state->cx = 0;
        }
    } else {
        for (int i = 0; i < len; i++) {
            char ch = text[i];
            if (ch == '\n') {
                erow *row = &state->buffer.rows[state->cy];
                if (state->cx < row->size) {
                    char *after = strdup(row->chars + state->cx);
                    row->chars[state->cx] = '\0';
                    row->size = state->cx;
                    buffer_update_row(row);
                    highlight_update_buffer(&state->buffer, (int)(row - state->buffer.rows));
                    buffer_insert_row(&state->buffer, state->cy + 1, after, strlen(after));
                    free(after);
                } else {
                    buffer_insert_row(&state->buffer, state->cy + 1, "", 0);
                }
                state->cy++;
                state->cx = 0;
            } else {
                buffer_row_insert_char(&state->buffer, &state->buffer.rows[state->cy], state->cx, ch);
                state->cx++;
            }
        }
    }
    state->rx = (state->cy < state->buffer.num_rows)
        ? buffer_row_cx_to_rx(&state->buffer.rows[state->cy], state->cx) : 0;
}

void editor_paste(EditorState *state) {
    if (!state->yank_buf || state->yank_len == 0) {
        editor_set_status_message(state, "Nothing to paste");
        return;
    }
    /* In visual mode the register replaces the selection (like vim). */
    if (state->mode == MODE_VISUAL) {
        editor_visual_delete(state);
        state->mode = MODE_NORMAL;
    }
    editor_insert_text(state, state->yank_buf, state->yank_len);
    editor_set_status_message(state, "Pasted %d char%s", state->yank_len, state->yank_len == 1 ? "" : "s");
}

/* Copy either the visual selection or (in normal mode) the current line to the
 * system clipboard. */
static void editor_clipboard_copy(EditorState *state) {
    if (state->mode == MODE_VISUAL) {
        int len = 0;
        char *text = editor_selection_to_string(state, &len);
        int rc = clipboard_copy(text, (size_t)len);
        if (rc == 0)
            editor_set_status_message(state, "Copied %d char%s to clipboard",
                                      len, len == 1 ? "" : "s");
        else
            editor_set_status_message(state, "Clipboard copy failed");
        free(text);
        state->mode = MODE_NORMAL;
        return;
    }

    if (state->buffer.num_rows == 0) {
        editor_set_status_message(state, "Nothing to copy");
        return;
    }
    /* Line-wise: include the trailing newline so it round-trips as a row. */
    size_t len = (size_t)state->buffer.rows[state->cy].size;
    char *text = malloc(len + 2);
    if (!text) { editor_set_status_message(state, "Out of memory"); return; }
    memcpy(text, state->buffer.rows[state->cy].chars, len);
    text[len] = '\n';
    text[len + 1] = '\0';

    if (clipboard_copy(text, len + 1) == 0)
        editor_set_status_message(state, "Copied line to clipboard");
    else
        editor_set_status_message(state, "Clipboard copy failed");
    free(text);
}

/* Paste the system clipboard at the cursor, replacing the visual selection. */
static void editor_clipboard_paste(EditorState *state) {
    size_t len = 0;
    char *text = clipboard_paste(&len);
    if (!text) {
        editor_set_status_message(state, "Clipboard unavailable");
        return;
    }
    if (len == 0) {
        free(text);
        editor_set_status_message(state, "Clipboard empty");
        return;
    }

    editor_begin_change(state);
    if (state->mode == MODE_VISUAL) {
        editor_visual_delete(state);
        state->mode = MODE_NORMAL;
    }
    editor_insert_text(state, text, (int)len);
    editor_end_change(state);
    editor_set_status_message(state, "Pasted %zu char%s from clipboard",
                              len, len == 1 ? "" : "s");
    free(text);
}

/* Execute a command as typed after `:` or fired by a `[bindings]` map line.
 * Shared by both entry points so a binding and `:cmd` mean exactly the same
 * thing. Supports the write/quit family, search-off, the diagnostics panel,
 * window-close and the explorer, plus a space-separated argument
 * (`w filename`). May set state->quit or switch to MODE_EXPLORER. */
static void editor_run_cmd(EditorState *state, const char *cmdline) {
    char cmd[128];
    char arg[128];
    arg[0] = '\0';
    snprintf(cmd, sizeof(cmd), "%s", cmdline);

    /* Split "w filename" style commands. */
    char *sp = strchr(cmd, ' ');
    if (sp) {
        *sp = '\0';
        snprintf(arg, sizeof(arg), "%s", sp + 1);
    }

    if (strcmp(cmd, "q") == 0) {
        /* Refuse to throw away unsaved work: `:q!` is the explicit override. */
        if (state->buffer.dirty)
            editor_set_status_message(state,
                "No write since last change (add ! to override)");
        else
            state->quit = 1;
    } else if (strcmp(cmd, "q!") == 0) {
        state->quit = 1;
    } else if (strcmp(cmd, "w") == 0 || strcmp(cmd, "wq") == 0 || strcmp(cmd, "x") == 0) {
        const char *target = arg[0] ? arg : state->buffer.filename;
        int saved = 0;
        if (target) {
            if (buffer_save(&state->buffer, target) == 0) {
                if (!state->buffer.filename) state->buffer.filename = strdup(target);
                editor_set_status_message(state, "\"%s\" written", target);
                saved = 1;
            } else {
                editor_set_status_message(state, "Error writing \"%s\"", target);
            }
        } else {
            editor_set_status_message(state, "No file name");
        }
        /* `w` never quits; `wq`/`x` only quit once the write succeeded, so a
         * failed save can't discard the buffer. */
        if (saved && strcmp(cmd, "w") != 0) state->quit = 1;
    } else if (strcmp(cmd, "noh") == 0 ||
               strcmp(cmd, "nohighlight") == 0 ||
               strcmp(cmd, "nohlsearch") == 0) {
        state->active_search[0] = '\0';
        editor_set_status_message(state, "No highlights");
    } else if (strcmp(cmd, "diag") == 0 ||
               strcmp(cmd, "diags") == 0 ||
               strcmp(cmd, "diagnostics") == 0 ||
               strcmp(cmd, "problems") == 0 ||
               strcmp(cmd, "errors") == 0) {
        editor_show_diagnostics_window(state);
    } else if (strcmp(cmd, "close") == 0 ||
               strcmp(cmd, "c") == 0 ||
               strcmp(cmd, "popup") == 0) {
        editor_close_windows(state);
        editor_set_status_message(state, "");
    } else if (strcmp(cmd, "reload") == 0 ||
               strcmp(cmd, "reloadconfig") == 0 ||
               strcmp(cmd, "source") == 0) {
        highlight_reload();          /* syntax files, theme, config cache */
        headers_invalidate();        /* a new -I dir may add headers */
        editor_apply_config(state);  /* linenumbers, clipboard_mod, [bindings] */
        highlight_select_for_buffer(&state->buffer);  /* re-pick + rescan */
        editor_set_status_message(state, "Config reloaded");
    } else if (strcmp(cmd, "explore") == 0 ||
               strcmp(cmd, "ex") == 0 ||
               strcmp(cmd, "e") == 0 ||
               strcmp(cmd, "files") == 0 ||
               strcmp(cmd, "oil") == 0) {
        explorer_open(state);
    } else {
        editor_set_status_message(state, "Unknown command: %s", cmd);
    }
}

void editor_process_keypress(EditorState *state) {
    int c = terminal_read_key();

    /* User key maps from the `[bindings]` config section. They are consulted
     * first (so e.g. `map <D-y> :q!` can rebind a clipboard shortcut) and only
     * in the editing key-modes: typing (INSERT, SEARCH) and `:`-typed commands
     * are never intercepted. A binding is a command, so VISUAL is dropped. */
    if (state->mode == MODE_NORMAL || state->mode == MODE_VISUAL) {
        for (int i = 0; i < state->binding_count; i++) {
            if (c == state->bindings[i].key) {
                state->pending_op = 0;
                state->mode = MODE_NORMAL;
                editor_run_cmd(state, state->bindings[i].cmd);
                editor_scroll(state);
                return;
            }
        }
    }

    /* Clipboard shortcuts (default Super+y / Super+p). They use modifier-encoded
     * key values, so they never collide with plain typing, and are handled
     * everywhere the user is editing rather than entering a command or search. */
    if (state->mode != MODE_COMMAND && state->mode != MODE_SEARCH) {
        if (c && c == state->clipboard_copy_key) {
            state->pending_op = 0;
            editor_clipboard_copy(state);
            editor_scroll(state);
            return;
        }
        if (c && c == state->clipboard_paste_key) {
            state->pending_op = 0;
            editor_clipboard_paste(state);
            editor_scroll(state);
            return;
        }
    }

    switch (state->mode) {
        case MODE_NORMAL:
            switch (c) {
                case ':': 
                    state->mode = MODE_COMMAND; 
                    state->command_len = 0;
                    state->command_buf[0] = '\0';
                    state->pending_op = 0;
                    editor_set_status_message(state, ":");
                    break;
                case 'i': 
                    state->mode = MODE_INSERT; 
                    editor_end_change(state);
                    state->pending_op = 0;
                    editor_set_status_message(state, "-- INSERT --"); 
                    break;
                case 'v':
                    state->mode = MODE_VISUAL;
                    state->v_start_x = state->cx;
                    state->v_start_y = state->cy;
                    state->pending_op = 0;
                    editor_set_status_message(state, "-- VISUAL --");
                    break;
                case '/':
                    /* Incremental search: type in the status bar, matches are
                     * highlighted live, Enter confirms, ESC cancels. */
                    state->mode = MODE_SEARCH;
                    state->search_len = 0;
                    state->search_buf[0] = '\0';
                    state->pending_op = 0;
                    editor_set_status_message(state, "/");
                    break;
                case '-':
                    /* Open the floating file explorer (netrw-style). */
                    state->pending_op = 0;
                    explorer_open(state);
                    break;
                case KEY_ESC:
                    /* Cancel a pending `d`, and close any open popup first. */
                    state->pending_op = 0;
                    if (state->windows.count > 0) {
                        editor_close_windows(state);
                        editor_set_status_message(state, "");
                    } else {
                        editor_set_status_message(state, "");
                    }
                    break;
                case 'u':
                    editor_undo(state);
                    state->pending_op = 0;
                    break;
                case 'p':
                    if (state->yank_buf && state->yank_len > 0) {
                        editor_begin_change(state);
                        editor_paste(state);
                        editor_end_change(state);
                    } else {
                        editor_set_status_message(state, "Nothing in register to paste");
                    }
                    state->pending_op = 0;
                    break;
                case 'd':
                    if (state->pending_op == 'd') {
                        /* dd */
                        editor_begin_change(state);
                        if (state->buffer.num_rows > 0) {
                            editor_delete_row(state, state->cy);
                            editor_set_status_message(state, "Deleted line");
                        }
                        editor_end_change(state);
                        state->pending_op = 0;
                    } else {
                        state->pending_op = 'd';
                        editor_set_status_message(state, "d");
                    }
                    break;
                case 'y':
                    if (state->pending_op == 'y') {
                        /* yy: yank the current line into the register. */
                        editor_yank_row(state, state->cy);
                        editor_set_status_message(state, "Yanked line");
                        state->pending_op = 0;
                    } else {
                        state->pending_op = 'y';
                        editor_set_status_message(state, "y");
                    }
                    break;
                case CTRL_KEY('r'):
                    editor_redo(state);
                    state->pending_op = 0;
                    break;
                case ARROW_UP:
                case ARROW_DOWN:
                case ARROW_LEFT:
                case ARROW_RIGHT:
                    if (state->pending_op == 'd') {
                        editor_delete_direction(state, c);
                    } else {
                        editor_move_cursor(state, c);
                    }
                    break;
                default:
                    /* Any other key cancels a pending `d`. */
                    state->pending_op = 0;
                    break;
            }
            break;
        case MODE_VISUAL:
            switch (c) {
                case KEY_ESC:
                case 'v':
                    state->mode = MODE_NORMAL;
                    editor_set_status_message(state, "");
                    break;
                case 'y':
                    editor_yank(state);
                    state->mode = MODE_NORMAL;
                    break;
                case 'd':
                case 'x':
                    editor_begin_change(state);
                    editor_yank(state);
                    editor_visual_delete(state);
                    editor_end_change(state);
                    state->mode = MODE_NORMAL;
                    break;
                case 'p':
                    /* Paste the register over the selection. */
                    if (state->yank_buf && state->yank_len > 0) {
                        editor_begin_change(state);
                        editor_paste(state);
                        editor_end_change(state);
                    } else {
                        editor_set_status_message(state, "Nothing in register to paste");
                    }
                    break;
                case ARROW_UP:
                case ARROW_DOWN:
                case ARROW_LEFT:
                case ARROW_RIGHT:
                    editor_move_cursor(state, c);
                    break;
            }
            break;
        case MODE_INSERT: {
            /* The completion menu gets first crack at the key while it is open.
             * `handled` means the key was consumed (navigation / accept /
             * cancel); otherwise it falls through to the normal insert logic. */
            int handled = 0;
            int was_active = state->completion_active;

            if (was_active) {
                int r = editor_completion_menu_key(state, c);
                if (r == 1) handled = 1;
                else if (r == 2) was_active = 0;
            } else if (completion_trigger_pressed(state, c)) {
                state->completion_manual = 1;
                editor_completion_trigger(state);
                handled = 1;
            }
            if (handled) break;

            if (c == KEY_ESC) {
                state->mode = MODE_NORMAL;
                editor_end_change(state);
                editor_set_status_message(state, "");
            } else if (c == KEY_ENTER) {
                editor_begin_change(state);
                if (state->cx < state->buffer.rows[state->cy].size) {
                    char *after = strdup(state->buffer.rows[state->cy].chars + state->cx);
                    state->buffer.rows[state->cy].chars[state->cx] = '\0';
                    state->buffer.rows[state->cy].size = state->cx;
                    buffer_update_row(&state->buffer.rows[state->cy]);
                    buffer_insert_row(&state->buffer, state->cy + 1, after, strlen(after));
                    free(after);
                } else {
                    buffer_insert_row(&state->buffer, state->cy + 1, "", 0);
                }
                state->cy++;
                state->cx = 0;
                state->rx = 0;
            } else if (c == ARROW_UP || c == ARROW_DOWN || c == ARROW_LEFT || c == ARROW_RIGHT) {
                editor_move_cursor(state, c);
            }
            else if (c == KEY_TAB) {
                editor_begin_change(state);
                for (int i = 0; i < PVIM_TAB_STOP; i++) {
                    buffer_row_insert_char(&state->buffer, &state->buffer.rows[state->cy], state->cx, ' ');
                    state->cx++;
                }
                state->rx = buffer_row_cx_to_rx(&state->buffer.rows[state->cy], state->cx);
            }
             else if (c == BACKSPACE) {
                if (state->cx > 0) {
                    editor_begin_change(state);
                    buffer_row_del_char(&state->buffer, &state->buffer.rows[state->cy], state->cx - 1);
                    state->cx--;
                    state->rx = buffer_row_cx_to_rx(&state->buffer.rows[state->cy], state->cx);
                } else if (state->cy > 0) {
                    editor_begin_change(state);
                    state->cx = state->buffer.rows[state->cy - 1].size;
                    buffer_row_append_string(&state->buffer, &state->buffer.rows[state->cy - 1], state->buffer.rows[state->cy].chars, state->buffer.rows[state->cy].size);
                    buffer_del_row(&state->buffer, state->cy);
                    state->cy--;
                    state->rx = buffer_row_cx_to_rx(&state->buffer.rows[state->cy], state->cx);
                }
            } else if (c >= 32 && c <= 126) {
                editor_begin_change(state);
                if (state->buffer.num_rows == 0) {
                    buffer_insert_row(&state->buffer, 0, "", 0);
                }
                buffer_row_insert_char(&state->buffer, &state->buffer.rows[state->cy], state->cx, c);
                state->cx++;
                state->rx = buffer_row_cx_to_rx(&state->buffer.rows[state->cy], state->cx);
            }

            /* Keep the menu in step with the edit: re-filter/refresh after an
             * identifier key, drop it after unrelated punctuation, and auto-open
             * once a char that can start a completion is typed. */
            if (was_active && c == BACKSPACE) {
                editor_completion_refresh(state);
            } else if (was_active && c >= 32 && c <= 126) {
                if (completion_continue_char(c))
                    editor_completion_refresh(state);
                else
                    editor_completion_close(state);
            } else if (!was_active && c >= 32 && c <= 126 &&
                       state->completion_enabled && state->completion_auto &&
                       completion_continue_char(c)) {
                state->completion_manual = 0;
                editor_completion_trigger(state);
            }
            break;
        }
        case MODE_EXPLORER:
            explorer_handle_key(state, c);
            break;
        case MODE_SEARCH:
            if (c == KEY_ESC) {
                /* Cancel: discard what was being typed; a previously
                 * confirmed search stays highlighted. */
                state->mode = MODE_NORMAL;
                state->search_len = 0;
                state->search_buf[0] = '\0';
                editor_set_status_message(state, "");
            } else if (c == KEY_ENTER) {
                /* Confirm: keep the search highlighted until `:noh`. */
                snprintf(state->active_search, sizeof(state->active_search),
                         "%s", state->search_buf);
                state->mode = MODE_NORMAL;
                if (state->active_search[0])
                    editor_set_status_message(state, "Search: %s", state->active_search);
                else
                    editor_set_status_message(state, "");
            } else if (c == BACKSPACE) {
                if (state->search_len > 0)
                    state->search_buf[--state->search_len] = '\0';
            } else if (c >= 32 && c <= 126 && state->search_len < 127) {
                state->search_buf[state->search_len++] = (char)c;
                state->search_buf[state->search_len] = '\0';
            }
            if (state->mode == MODE_SEARCH)
                editor_set_status_message(state, "/%s", state->search_buf);
            break;
        case MODE_COMMAND:
            if (c == KEY_ENTER) {
                editor_run_cmd(state, state->command_buf);
                /* `:explore` switches to MODE_EXPLORER; don't clobber it. */
                if (state->mode == MODE_COMMAND) state->mode = MODE_NORMAL;
            } else if (c == KEY_ESC) {
                state->mode = MODE_NORMAL;
                editor_set_status_message(state, "");
            } else if (c == BACKSPACE && state->command_len > 0) {
                state->command_buf[--state->command_len] = '\0';
            } else if (c >= 32 && c <= 126 && state->command_len < 127) {
                state->command_buf[state->command_len++] = (char)c;
                state->command_buf[state->command_len] = '\0';
            }
            if (state->mode == MODE_COMMAND) editor_set_status_message(state, ":%s", state->command_buf);
            break;
    }
    /* Recompute the visible window after any cursor/state change. */
    editor_scroll(state);
}
