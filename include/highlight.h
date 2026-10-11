#ifndef PVIM_HIGHLIGHT_H
#define PVIM_HIGHLIGHT_H

#include "buffer.h"

/* Highlight types. Each maps 1:1 with a character in `erow->chars` via the
 * `erow->hl` array, so a colored renderer never has to re-parse text. */
enum editorHighlight {
    HL_NORMAL = 0,
    HL_COMMENT,     /* single-line comments */
    HL_MLCOMMENT,   /* multi-line comments (every char in the region) */
    HL_KEYWORD1,    /* control-flow / declaration keywords */
    HL_KEYWORD2,    /* type keywords and struct/class/enum names */
    HL_STRING,      /* "..." and '...' literals */
    HL_NUMBER,      /* numeric literals */
    HL_MACRO,       /* #-directive lines and ALL_CAPS identifiers */
    HL_VARIABLE,    /* plain identifiers */
    HL_FUNCTION,    /* identifiers immediately followed by `(` */
    HL_MATCH,       /* search match (drawn as a reverse-video block) */
    HL_SELECTION,   /* visual selection (drawn as a reverse-video block) */
    HL_ERROR,       /* error diagnostic */
    HL_WARNING,     /* warning diagnostic */
    HL_INFO,        /* info diagnostic */
    HL_HINT,        /* hint diagnostic */
    HL_MENU,        /* completion menu: candidate label */
    HL_MENU_DETAIL, /* completion menu: dimmed signature / detail */
    HL_MENU_SOURCE, /* completion menu: kind tag / source badge */
    HL_MENU_SELECT, /* completion menu: selected-row background */
    HL_MENU_BG,     /* completion menu: popup background */
    HL_MENU_BORDER  /* completion menu: popup border */
};

/* Colour (XTerm-256 index) for an LSP diagnostic severity (1=Error...4=Hint),
 * honouring the `error`/`warning`/`info`/`hint` theme roles. */
int highlight_diag_color(int severity);

/*
 * Incremental update after a character-level edit: rescan `buf->rows[start]`
 * and continue only while the multi-line-comment state keeps changing. Assumes
 * every row is already scanned, so once a row's comment state is stable no
 * later row can be affected.
 */
void highlight_update_buffer(EditorBuffer *buf, int start);

/*
 * Full rescan of `buf->rows[start..num_rows-1]`. Used after structural changes
 * (row insert/delete) and after selecting a scheme, where the incoming state of
 * later rows may change without the first row's own end state changing.
 */
void highlight_rescan_buffer(EditorBuffer *buf, int start);

/* Pick a highlight scheme from the buffer's filename extension or a `#!`
 * shebang on the first line. Passing an unmatched buffer disables highlighting.
 * Re-scans the whole buffer so colours are (re)applied immediately. This lazily
 * loads the syntax rule files on first use. */
void highlight_select_for_buffer(EditorBuffer *buf);

/*
 * Load `*.syntax` rule files from (in precedence order):
 *   - ./syntax/                       (bundled defaults, cwd)
 *   - <executable_dir>/syntax and <executable_dir>/../syntax (bundled,
 *     cwd-independent: covers an installed binary and the repo's bin/pvim)
 *   - $XDG_CONFIG_HOME/pvim/syntax or ~/.config/pvim/syntax (user rules)
 *   - the directory named by $PVIM_SYNTAX_DIR                     (override)
 * Files with the same `filetype` replace the earlier one, so user files can
 * override the bundled defaults. Idempotent; returns non-zero when at least
 * one rule file was loaded.
 */
int highlight_load_syntax(void);

/* Free every loaded syntax rule. Safe to call when nothing is loaded. */
void highlight_free_syntax(void);

/* Re-read syntax rules, the selected theme (and its pvim.conf colour
 * overrides) and the config cache from disk. Used by `:reload` so edits to
 * pvim.conf / themes / syntax files take effect without a restart. The caller
 * should re-run highlight_select_for_buffer() afterwards to re-apply the
 * scheme. */
void highlight_reload(void);

/*
 * Load and apply the active theme. The theme name is taken from $PVIM_THEME,
 * else the `theme =` key in pvim.conf, else "default". The theme file is
 * `themes/<name>.theme` searched in the user config dir, next to the
 * executable, and ./themes. Any colour overrides in pvim.conf win over the
 * theme. Called automatically by highlight_load_syntax().
 */
void highlight_load_theme(void);

/* Map a highlight type to an XTerm-256 foreground index, or -1 for the
 * terminal default colour. */
int highlight_to_color(unsigned char hl);

/* Brightness (bold) flags for the reverse-video selection and search-match
 * blocks, set from the `selection_bold` / `search_bold` keys. */
int highlight_selection_bold(void);
int highlight_search_bold(void);

/* Name of the currently selected highlight scheme ("c", "python", ...) or
 * "none" when no scheme matches the buffer. */
const char *highlight_syntax_name(void);

/* Name of the active theme, for the status bar. */
const char *highlight_theme_name(void);

#endif /* PVIM_HIGHLIGHT_H */