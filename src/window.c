#define _POSIX_C_SOURCE 200809L
#include "../include/window.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Box-drawing glyphs for each border style. The index order matches enum
 * WinBorder; the NONE entry is never dereferenced. */
typedef struct BorderGlyphs {
    const char *tl, *tr, *bl, *br, *h, *v;
} BorderGlyphs;

static const BorderGlyphs BORDERS[WIN_BORDER_COUNT] = {
    [WIN_BORDER_NONE]   = { "",   "",   "",   "",   "",   "" },
    [WIN_BORDER_SINGLE] = { "\xe2\x94\x8c", "\xe2\x94\x90", "\xe2\x94\x94",
                            "\xe2\x94\x98", "\xe2\x94\x80", "\xe2\x94\x82" },
    [WIN_BORDER_ROUND]  = { "\xe2\x95\xad", "\xe2\x95\xae", "\xe2\x95\xb0",
                            "\xe2\x95\xaf", "\xe2\x94\x80", "\xe2\x94\x82" },
    [WIN_BORDER_DOUBLE] = { "\xe2\x95\x94", "\xe2\x95\x97", "\xe2\x95\x9a",
                            "\xe2\x95\x9d", "\xe2\x95\x90", "\xe2\x95\x91" }
};

void windows_init(WindowLayer *wl) {
    if (!wl) return;
    wl->items = NULL;
    wl->count = 0;
    wl->cap = 0;
}

/* Release a window's line storage (text + styled spans). */
static void free_line_storage(FloatWindow *win) {
    if (!win) return;
    if (win->lines)
        for (int j = 0; j < win->line_count; j++) free(win->lines[j]);
    if (win->spans)
        for (int j = 0; j < win->line_count; j++) free(win->spans[j]);
    free(win->lines);
    free(win->spans);
    free(win->span_counts);
    win->lines = NULL;
    win->spans = NULL;
    win->span_counts = NULL;
    win->line_count = 0;
    win->line_cap = 0;
}

void windows_free(WindowLayer *wl) {
    if (!wl) return;
    for (int i = 0; i < wl->count; i++)
        free_line_storage(&wl->items[i]);
    free(wl->items);
    wl->items = NULL;
    wl->count = 0;
    wl->cap = 0;
}

void windows_clear(WindowLayer *wl) {
    if (!wl) return;
    for (int i = 0; i < wl->count; i++)
        free_line_storage(&wl->items[i]);
    wl->count = 0;
}

FloatWindow *windows_add(WindowLayer *wl, int x, int y, int w, int h,
                         const char *title, WinBorder border, int z) {
    if (!wl || w <= 0 || h <= 0) return NULL;
    if (wl->count == wl->cap) {
        int cap = wl->cap ? wl->cap * 2 : 4;
        FloatWindow *ni = realloc(wl->items, (size_t)cap * sizeof(FloatWindow));
        if (!ni) return NULL;
        wl->items = ni;
        wl->cap = cap;
    }
    FloatWindow *win = &wl->items[wl->count++];
    memset(win, 0, sizeof *win);
    win->active = 1;
    win->z = z;
    win->x = x;
    win->y = y;
    win->w = w;
    win->h = h;
    win->border = border;
    win->scroll = 0;
    win->selected = -1;
    win->cursor_x = -1;
    win->cursor_y = -1;
    if (title) snprintf(win->title, sizeof win->title, "%s", title);
    return win;
}

static int ensure_lines(FloatWindow *win, int n) {
    if (n <= win->line_cap) return 1;
    int cap = win->line_cap ? win->line_cap : 8;
    while (cap < n) cap *= 2;
    char **nl = realloc(win->lines, (size_t)cap * sizeof(char *));
    if (!nl) return 0;
    win->lines = nl;
    WinSpan **ns = realloc(win->spans, (size_t)cap * sizeof(WinSpan *));
    if (!ns) return 0;
    win->spans = ns;
    int *nc = realloc(win->span_counts, (size_t)cap * sizeof(int));
    if (!nc) return 0;
    win->span_counts = nc;
    for (int i = win->line_cap; i < cap; i++) {
        win->lines[i] = NULL;
        win->spans[i] = NULL;
        win->span_counts[i] = 0;
    }
    win->line_cap = cap;
    return 1;
}

void window_set_line_spans(FloatWindow *win, int row, const char *text,
                           const WinSpan *spans, int nspans) {
    if (!win || row < 0) return;
    if (!ensure_lines(win, row + 1)) return;
    char *copy = NULL;
    if (text) {
        copy = strdup(text);
        if (!copy) return;
    }
    WinSpan *scopy = NULL;
    if (nspans > 0 && spans) {
        scopy = malloc(sizeof(WinSpan) * (size_t)nspans);
        if (!scopy) { free(copy); return; }
        memcpy(scopy, spans, sizeof(WinSpan) * (size_t)nspans);
    }
    free(win->lines[row]);
    free(win->spans[row]);
    win->lines[row] = copy;
    win->spans[row] = scopy;
    win->span_counts[row] = scopy ? nspans : 0;
    if (row >= win->line_count) win->line_count = row + 1;
}

void window_set_line(FloatWindow *win, int row, const char *text) {
    window_set_line_spans(win, row, text, NULL, 0);
}

void window_append_line(FloatWindow *win, const char *text) {
    if (!win) return;
    window_set_line(win, win->line_count, text);
}

void window_printf(FloatWindow *win, int row, const char *fmt, ...) {
    if (!win || row < 0) return;
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) {
        va_end(ap2);
        return;
    }
    char *buf = malloc((size_t)need + 1);
    if (!buf) {
        va_end(ap2);
        return;
    }
    vsnprintf(buf, (size_t)need + 1, fmt, ap2);
    va_end(ap2);
    window_set_line(win, row, buf);
    free(buf);
}

void window_clear_lines(FloatWindow *win) {
    if (!win) return;
    for (int i = 0; i < win->line_count; i++) {
        free(win->lines[i]);
        if (win->spans) free(win->spans[i]);
        if (win->span_counts) win->span_counts[i] = 0;
        if (win->lines) win->lines[i] = NULL;
        if (win->spans) win->spans[i] = NULL;
    }
    win->line_count = 0;
    win->scroll = 0;
}

static void content_dims_rect(int x, int y, int w, int h, WinBorder border,
                              int *cx, int *cy, int *cw, int *ch) {
    if (border == WIN_BORDER_NONE) {
        *cx = x;
        *cy = y;
        *cw = w;
        *ch = h;
    } else {
        *cx = x + 1;
        *cy = y + 1;
        *cw = w - 2;
        *ch = h - 2;
    }
    if (*cw < 0) *cw = 0;
    if (*ch < 0) *ch = 0;
}

void window_content_dims(const FloatWindow *win, int *cx, int *cy, int *cw, int *ch) {
    if (!win) return;
    content_dims_rect(win->x, win->y, win->w, win->h, win->border, cx, cy, cw, ch);
}

int window_fit(const FloatWindow *win, int rows, int cols,
               int *x, int *y, int *w, int *h) {
    if (!win || rows <= 0 || cols <= 0) return 0;
    int ww = win->w;
    int hh = win->h;
    if (ww > cols) ww = cols;
    if (hh > rows) hh = rows;
    if (ww <= 0 || hh <= 0) return 0;
    int xx = win->x;
    int yy = win->y;
    if (xx + ww > cols) xx = cols - ww;
    if (yy + hh > rows) yy = rows - hh;
    if (xx < 0) xx = 0;
    if (yy < 0) yy = 0;
    if (x) *x = xx;
    if (y) *y = yy;
    if (w) *w = ww;
    if (h) *h = hh;
    return 1;
}

int window_contains(const FloatWindow *win, int rows, int cols, int row, int col) {
    if (!win || !win->active) return 0;
    int x, y, w, h;
    if (!window_fit(win, rows, cols, &x, &y, &w, &h)) return 0;
    return row >= y && row < y + h && col >= x && col < x + w;
}

/* Return the byte length of the UTF-8 sequence starting at `c`. */
static int utf8_seq_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

/* Emit one content line clipped to `cols` display columns. Each column is
 * coloured by the span that covers it (falling back to WIN_STYLE_TEXT), and
 * the row is padded to the full width so the popup stays opaque. `selected`
 * asks the sink for the highlighted background block. Each UTF-8 sequence
 * counts as one column (wide CJK glyphs are not double-counted). */
static void emit_line_styled(const WinEmitter *em, const char *s,
                             const WinSpan *spans, int nspans,
                             int selected, int cols) {
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    int cur = -1;
    for (int col = 0; col < cols; col++) {
        int style = WIN_STYLE_TEXT;
        for (int k = 0; k < nspans; k++) {
            if (col >= spans[k].col && col < spans[k].col + spans[k].len) {
                style = spans[k].style;
                break;
            }
        }
        if (style != cur) {
            em->sgr(em->ctx, style, selected);
            cur = style;
        }
        if (*p) {
            int len = utf8_seq_len(*p);
            if (len < 1 || len > 4) len = 1;
            char buf[8];
            memcpy(buf, p, (size_t)len);
            buf[len] = '\0';
            em->text(em->ctx, buf);
            p += len;
        } else {
            em->text(em->ctx, " ");
        }
    }
}

static void draw_window(const FloatWindow *win, int rows, int cols,
                        const WinEmitter *em) {
    int x, y, w, h;
    if (!window_fit(win, rows, cols, &x, &y, &w, &h)) return;

    int cx, cy, cw, ch;
    content_dims_rect(x, y, w, h, win->border, &cx, &cy, &cw, &ch);
    const BorderGlyphs *g = &BORDERS[win->border];

    if (win->border != WIN_BORDER_NONE && w >= 2 && h >= 2) {
        em->move(em->ctx, y, x);
        em->sgr(em->ctx, WIN_STYLE_BORDER, 0);
        em->text(em->ctx, g->tl);
        for (int i = 0; i < w - 2; i++) em->text(em->ctx, g->h);
        em->text(em->ctx, g->tr);

        if (win->title[0] && w >= 4) {
            char tbuf[WIN_TITLE_MAX + 2];
            snprintf(tbuf, sizeof tbuf, " %s ", win->title);
            em->move(em->ctx, y, x + 1);
            em->sgr(em->ctx, WIN_STYLE_TITLE, 0);
            emit_line_styled(em, tbuf, NULL, 0, 0, w - 2);
        }

        em->move(em->ctx, y + h - 1, x);
        em->sgr(em->ctx, WIN_STYLE_BORDER, 0);
        em->text(em->ctx, g->bl);
        for (int i = 0; i < w - 2; i++) em->text(em->ctx, g->h);
        em->text(em->ctx, g->br);

        for (int r = 0; r < h - 2; r++) {
            em->move(em->ctx, y + 1 + r, x);
            em->sgr(em->ctx, WIN_STYLE_BORDER, 0);
            em->text(em->ctx, g->v);
            em->move(em->ctx, y + 1 + r, x + w - 1);
            em->text(em->ctx, g->v);
        }
    }

    for (int r = 0; r < ch; r++) {
        int li = win->scroll + r;
        int has = (li >= 0 && li < win->line_count);
        const char *line = (has && win->lines && win->lines[li])
                               ? win->lines[li] : "";
        const WinSpan *spans = (has && win->spans) ? win->spans[li] : NULL;
        int nspans = (has && win->span_counts) ? win->span_counts[li] : 0;
        int selected = (li >= 0 && li == win->selected);
        em->move(em->ctx, cy + r, cx);
        emit_line_styled(em, line, spans, nspans, selected, cw);
    }
}

void windows_draw(const WindowLayer *wl, int rows, int cols,
                  const WinEmitter *em) {
    if (!wl || !em || wl->count <= 0) return;

    char *done = calloc((size_t)wl->count, 1);
    if (!done) {
        /* Out of memory: fall back to insertion order (best effort). */
        for (int i = 0; i < wl->count; i++)
            if (wl->items[i].active) draw_window(&wl->items[i], rows, cols, em);
        return;
    }

    int remaining = 0;
    for (int i = 0; i < wl->count; i++)
        if (wl->items[i].active) remaining++;

    /* Paint in ascending elevation so higher-z windows land on top. */
    while (remaining > 0) {
        int best = -1;
        for (int i = 0; i < wl->count; i++) {
            if (!wl->items[i].active) continue;
            if (done[i]) continue;
            if (best == -1 || wl->items[i].z < wl->items[best].z) best = i;
        }
        if (best == -1) break;
        done[best] = 1;
        remaining--;
        draw_window(&wl->items[best], rows, cols, em);
    }
    free(done);
}
