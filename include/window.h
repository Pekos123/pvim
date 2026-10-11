#ifndef PVIM_WINDOW_H
#define PVIM_WINDOW_H

/*
 * Floating windows / popup overlay engine.
 *
 * A FloatWindow is a rectangular panel (optionally bordered) that is drawn on
 * top of the text grid. Windows live in a WindowLayer owned by the editor; the
 * renderer composites every active window after the base frame, lowest
 * elevation (z) first, so higher-z popups win where they overlap.
 *
 * The engine is deliberately terminal-agnostic: it never writes ANSI itself.
 * Instead it walks the window rectangle and drives a WinEmitter sink, which the
 * display layer implements on top of its output buffer. This keeps the bounds
 * checking, border drawing and z-ordering testable without a terminal.
 */

#define WIN_TITLE_MAX 128

/* Border styles. WIN_BORDER_NONE leaves the whole rectangle for content. */
typedef enum WinBorder {
    WIN_BORDER_NONE = 0,
    WIN_BORDER_SINGLE, /* ┌ ┐ └ ┘ ─ │ */
    WIN_BORDER_ROUND,  /* ╭ ╮ ╰ ╯ ─ │ */
    WIN_BORDER_DOUBLE, /* ╔ ╗ ╚ ╝ ═ ║ */
    WIN_BORDER_COUNT
} WinBorder;

/* Abstract content styles. The display layer maps each style (plus the
 * selected flag) to a concrete SGR using theme roles, so the engine stays
 * theme-agnostic. */
typedef enum WinStyle {
    WIN_STYLE_TEXT   = 0, /* normal candidate text */
    WIN_STYLE_MUTED  = 1, /* dimmed signature / detail */
    WIN_STYLE_ACCENT = 2, /* kind tag / source badge */
    WIN_STYLE_BORDER = 3,
    WIN_STYLE_TITLE  = 4,
    WIN_STYLE_COUNT
} WinStyle;

/* A styled run inside a content line: columns [col, col + len) use `style`.
 * Lines may carry several spans (e.g. a label, a dimmed signature and an
 * accent badge) so a single row can be multi-coloured. */
typedef struct WinSpan {
    int col;             /* start column, 0-based from the content left edge */
    int len;             /* number of display columns */
    int style;           /* WinStyle */
} WinSpan;

typedef struct FloatWindow {
    int active;              /* 0 once cleared / closed */
    int z;                   /* elevation: higher draws later (on top) */
    int x, y;                /* top-left cell (border included), 0-based */
    int w, h;                /* total size including the border */
    char title[WIN_TITLE_MAX];
    WinBorder border;
    char **lines;            /* content lines; entries may be NULL (blank) */
    WinSpan **spans;         /* optional styled runs, parallel to `lines` */
    int *span_counts;        /* number of spans per line */
    int line_count;
    int line_cap;
    int scroll;              /* first content line shown */
    int selected;            /* highlighted content line, or -1 (menus) */
    int cursor_x, cursor_y;  /* content-relative cursor, or -1 for none */
} FloatWindow;

typedef struct WindowLayer {
    FloatWindow *items;
    int count;
    int cap;
} WindowLayer;

/* Abstract output sink. Coordinates are 0-based. `sgr` is called with a
 * WinStyle and a selected flag (the row's background block); the sink is free
 * to combine them into one SGR sequence. */
typedef struct WinEmitter {
    void *ctx;
    void (*move)(void *ctx, int row, int col);
    void (*sgr)(void *ctx, int style, int selected);
    void (*text)(void *ctx, const char *utf8);
} WinEmitter;

void windows_init(WindowLayer *wl);
void windows_free(WindowLayer *wl);

/* Discard every window (frees their content); the layer stays usable. */
void windows_clear(WindowLayer *wl);

/* Add a window and return it. NOTE: the returned pointer is invalidated by a
 * later windows_add() (the backing array may be reallocated), so re-fetch if
 * you keep adding. Returns NULL on allocation failure. */
FloatWindow *windows_add(WindowLayer *wl, int x, int y, int w, int h,
                         const char *title, WinBorder border, int z);

/* Content helpers. Indices outside the current content grow the line list,
 * filling gaps with blanks. */
void window_clear_lines(FloatWindow *win);
void window_set_line(FloatWindow *win, int row, const char *text);
/* Like window_set_line, but also attaches styled runs. `spans` is copied, so
 * the caller keeps ownership; passing NULL/0 clears any spans on the line. */
void window_set_line_spans(FloatWindow *win, int row, const char *text,
                           const WinSpan *spans, int nspans);
void window_append_line(FloatWindow *win, const char *text);
void window_printf(FloatWindow *win, int row, const char *fmt, ...);

/* Content rectangle of a window, in screen coordinates (before clipping). */
void window_content_dims(const FloatWindow *win, int *cx, int *cy, int *cw, int *ch);

/* Fit a window on screen: shrink it to the available rows/cols if needed and
 * shift it so it sits fully inside [0, rows) x [0, cols). Returns 1 if any part
 * remains visible (w/h > 0), else 0. */
int window_fit(const FloatWindow *win, int rows, int cols,
               int *x, int *y, int *w, int *h);

/* Bounds test against the fitted, on-screen rectangle. */
int window_contains(const FloatWindow *win, int rows, int cols, int row, int col);

/* Draw every active window over the grid, lowest z first. */
void windows_draw(const WindowLayer *wl, int rows, int cols,
                  const WinEmitter *em);

#endif /* PVIM_WINDOW_H */
