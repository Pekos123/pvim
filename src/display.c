#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include "../include/display.h"
#include "../include/highlight.h"
#include "../include/buffer.h"
#include "../include/lsp.h"
#include "../include/window.h"

/* A growable output buffer. */
typedef struct {
    char *b;
    size_t len;
    size_t cap;
} abuf;

static void ab_append(abuf *ab, const char *s, size_t len) {
    if (ab->len + len + 1 > ab->cap) {
        size_t newcap = ab->cap ? ab->cap : 256;
        while (ab->len + len + 1 > newcap) newcap *= 2;
        char *nb = realloc(ab->b, newcap);
        if (!nb) return;
        ab->b = nb;
        ab->cap = newcap;
    }
    if (len > 0) memcpy(ab->b + ab->len, s, len);
    ab->len += len;
    ab->b[ab->len] = '\0';
}

static void ab_puts(abuf *ab, const char *s) {
    ab_append(ab, s, strlen(s));
}

/* Append text with control bytes (newline, ESC, ...) turned into spaces, so
 * untrusted strings (diagnostics, file names, search text) can never move the
 * terminal cursor mid-frame and corrupt the layout. UTF-8 bytes pass through. */
static void ab_puts_c(abuf *ab, const char *s) {
    for (const char *p = s; *p;) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f) {
            ab_append(ab, " ", 1);
            p++;
            continue;
        }
        size_t len;
        if (c < 0x80) len = 1;
        else if ((c & 0xE0) == 0xC0) len = (p[1] ? 2 : 1);
        else if ((c & 0xF0) == 0xE0) len = (p[1] && p[2] ? 3 : 1);
        else if ((c & 0xF8) == 0xF0) len = (p[1] && p[2] && p[3] ? 4 : 1);
        else len = 1;
        ab_append(ab, p, len);
        p += len;
    }
}

/* Append sanitized text cropped to at most `max_cols` display columns. Used for
 * diagnostic messages: control bytes are turned into spaces, and text past the
 * limit is dropped instead of wrapping onto the next row. UTF-8 bytes count as
 * one column each and are never split. */
static void ab_puts_crop(abuf *ab, const char *s, int max_cols) {
    int cols = 0;
    for (const char *p = s; *p && cols < max_cols;) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f) {
            ab_append(ab, " ", 1);
            p++;
            cols++;
            continue;
        }
        size_t len;
        if (c < 0x80) len = 1;
        else if ((c & 0xE0) == 0xC0) len = (p[1] ? 2 : 1);
        else if ((c & 0xF0) == 0xE0) len = (p[1] && p[2] ? 3 : 1);
        else if ((c & 0xF8) == 0xF0) len = (p[1] && p[2] && p[3] ? 4 : 1);
        else len = 1;
        ab_append(ab, p, len);
        p += len;
        cols++;
    }
}

/* Emit the SGR attribute sequence for a foreground colour, an optional
 * reverse-video `mark` (visual selection / search match) and an optional
 * `bold` (brightness). `color` is an XTerm-256 palette index, or -1 for the
 * terminal default colour; with `mark` set the colour becomes the block's
 * background via reverse video (like a normal terminal selection). */
static void ab_set_attrs(abuf *ab, int color, int mark, int bold) {
    char seq[64];
    size_t n = 0;
    int first = 1;
    n += (size_t)snprintf(seq + n, sizeof(seq) - n, "\x1b[");
    if (bold) {
        n += (size_t)snprintf(seq + n, sizeof(seq) - n, "1");
        first = 0;
    }
    if (color >= 0) {
        n += (size_t)snprintf(seq + n, sizeof(seq) - n, "%s38;5;%d",
                              first ? "" : ";", color);
        first = 0;
    }
    if (mark) {
        n += (size_t)snprintf(seq + n, sizeof(seq) - n, "%s7", first ? "" : ";");
        first = 0;
    }
    n += (size_t)snprintf(seq + n, sizeof(seq) - n, "%sm", first ? "0" : "");
    ab_puts(ab, seq);
}

/* Print formatted text into the abuf. */
static void ab_printf(abuf *ab, const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof(tmp)) {
        ab_append(ab, tmp, (size_t)n);
    } else {
        char *big = malloc((size_t)n + 1);
        if (!big) return;
        va_start(ap, fmt);
        vsnprintf(big, (size_t)n + 1, fmt, ap);
        va_end(ap);
        ab_append(ab, big, (size_t)n);
        free(big);
    }
}

/* Clipboard sanitizer: replace control bytes with spaces. */
static void sanitize_ctl_line(char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c == 0x7f) *s = ' ';
    }
}

/* WinEmitter sink used to composite floating windows over the frame. It writes
 * cursor moves, colour attributes and text into the same output buffer as the
 * rest of the screen, so popups are painted during the normal refresh pass. */
static void win_em_move(void *ctx, int row, int col) {
    ab_printf((abuf *)ctx, "\x1b[%d;%dH", row + 1, col + 1);
}

/* Map an abstract window style (plus the selected flag) to a concrete SGR
 * sequence, using the completion-menu theme roles so popups follow the active
 * theme. A selected row paints the full-width background block; the row's
 * foreground colours are preserved on top of it. */
static void win_em_sgr(void *ctx, int style, int selected) {
    int fg;
    switch (style) {
        case WIN_STYLE_MUTED:  fg = highlight_to_color(HL_MENU_DETAIL); break;
        case WIN_STYLE_ACCENT: fg = highlight_to_color(HL_MENU_SOURCE); break;
        case WIN_STYLE_BORDER: fg = highlight_to_color(HL_MENU_BORDER); break;
        case WIN_STYLE_TITLE:  fg = highlight_to_color(HL_MENU_SOURCE); break;
        case WIN_STYLE_TEXT:
        default:               fg = highlight_to_color(HL_MENU);        break;
    }
    int bg = selected ? highlight_to_color(HL_MENU_SELECT)
                      : highlight_to_color(HL_MENU_BG);

    char seq[64];
    size_t len = 0;
    int w = snprintf(seq, sizeof seq, "\x1b[0");
    if (w > 0) len = (size_t)w;
    if (fg >= 0) { w = snprintf(seq + len, sizeof seq - len, ";38;5;%d", fg); if (w > 0) len += (size_t)w; }
    if (bg >= 0) { w = snprintf(seq + len, sizeof seq - len, ";48;5;%d", bg); if (w > 0) len += (size_t)w; }
    if (len + 1 < sizeof seq) { seq[len++] = 'm'; seq[len] = '\0'; }
    ab_puts((abuf *)ctx, seq);
}

static void win_em_text(void *ctx, const char *utf8) {
    ab_puts((abuf *)ctx, utf8);
}

/* Build a colour map for a row's render columns. */
static void display_build_color_map(const erow *row, int start, int end,
                                    unsigned char *out) {
    int ci = 0;
    int rp = 0;
    while (rp < end && ci < row->size) {
        unsigned char h = row->hl[ci];
        if (row->chars[ci] == '\t') {
            int width = PVIM_TAB_STOP - (rp % PVIM_TAB_STOP);
            for (int k = 0; k < width; k++) {
                if (rp >= start && rp < end) out[rp - start] = h;
                rp++;
                if (rp >= end) break;
            }
        } else {
            if (rp >= start && rp < end) out[rp - start] = h;
            ci++;
            rp++;
        }
    }
}

/* Mark the columns of the render window [start, end) that fall inside a
 * case-insensitive match of the search text. */
static void display_search_overlay(EditorState *state, const erow *row,
                                   int start, int end, unsigned char *out) {
    if (end <= start) return;
    int n = end - start;
    memset(out, 0, (size_t)n);
    const char *needle = (state->mode == MODE_SEARCH)
        ? state->search_buf : state->active_search;
    int nlen = (int)strlen(needle);
    if (nlen == 0) return;
    int scan = start - nlen + 1;
    if (scan < 0) scan = 0;
    for (int p = scan; p + nlen <= end && p + nlen <= row->rsize; p++) {
        int eq = 1;
        for (int k = 0; k < nlen; k++) {
            if (tolower((unsigned char)row->render[p + k]) !=
                tolower((unsigned char)needle[k])) {
                eq = 0;
                break;
            }
        }
        if (eq) {
            for (int k = p; k < p + nlen; k++)
                if (k >= start) out[k - start] = 1;
        }
    }
}

/* Render the line-number gutter for `file_row`. In relative mode every line
 * shows its distance from the cursor; the cursor line keeps its absolute
 * number. */
static void display_append_gutter(abuf *ab, EditorState *state, int file_row) {
    int w = editor_gutter_width(state);
    if (w <= 0) return;

    int num;
    if (state->line_numbers == LN_RELATIVE && file_row != state->cy)
        num = abs(file_row - state->cy);
    else
        num = file_row + 1;

    /* A diagnostic on this line takes over the trailing gutter column: the
     * number is coloured by severity and the separator becomes E/W/I/H. */
    const LspDiag *diag = state->lsp_diags
        ? lsp_first_diag_on_line(state->lsp_diags, file_row) : NULL;
    if (diag) {
        int col_idx = highlight_diag_color(diag->severity);
        char mark;
        switch (diag->severity) {
            case LSP_SEV_ERROR:   mark = 'E'; break;
            case LSP_SEV_WARNING: mark = 'W'; break;
            case LSP_SEV_INFO:    mark = 'I'; break;
            default:              mark = 'H'; break;
        }
        if (col_idx >= 0)
            ab_printf(ab, "\x1b[38;5;%dm", col_idx);
        ab_printf(ab, "%*d%c", w - 1, num, mark);
        ab_puts(ab, "\x1b[0m");
        return;
    }

    if (file_row == state->cy)
        ab_puts(ab, "\x1b[1m");          /* cursor line: bold */
    else
        ab_puts(ab, "\x1b[38;5;240m");   /* others: dim grey   */
    ab_printf(ab, "%*d ", w - 1, num);
    ab_puts(ab, "\x1b[0m");
}

/* Append one file row to the frame, colouring syntax highlighted characters. */
static void display_append_row(abuf *ab, EditorState *state, int file_row) {
    erow *row = &state->buffer.rows[file_row];
    int screen_cols = state->screen_cols - editor_gutter_width(state);
    if (screen_cols < 1) screen_cols = 1;

    /* Horizontal window of render columns to draw. */
    int start = state->col_offset;
    int end = row->rsize;
    if (end > start + screen_cols) end = start + screen_cols;
    int n = end - start;

    /* Selection range in render columns (exclusive end) for this row, plus
     * which shape is active. `sel_end <= sel_start` means nothing is selected
     * on this row. */
    int sel_start = 0;
    int sel_end = -1;
    int line_sel = 0;
    int block_sel = 0;
    int newline_sel = 0;   /* char-wise: the row's newline is selected */
    if (state->mode == MODE_VISUAL) {
        if (state->visual_type == VISUAL_LINE) {
            /* Whole rows between the anchor and the cursor. */
            int r0 = state->v_start_y, r1 = state->cy;
            if (r0 > r1) { int t = r0; r0 = r1; r1 = t; }
            if (file_row >= r0 && file_row <= r1) {
                line_sel = 1;
                sel_start = 0;
                sel_end = row->rsize;
            }
        } else if (state->visual_type == VISUAL_BLOCK) {
            /* Rectangular block: the screen columns between the anchor
             * and the cursor, on every row of the range. */
            int r0 = state->v_start_y, r1 = state->cy;
            if (r0 > r1) { int t = r0; r0 = r1; r1 = t; }
            if (file_row >= r0 && file_row <= r1) {
                int arx = 0;
                if (state->v_start_y >= 0 &&
                    state->v_start_y < state->buffer.num_rows)
                    arx = buffer_row_cx_to_rx(
                        &state->buffer.rows[state->v_start_y], state->v_start_x);
                int crx = state->rx;
                block_sel = 1;
                sel_start = (arx < crx) ? arx : crx;
                sel_end = ((arx > crx) ? arx : crx) + 1;
            }
        } else {
            int sy, sx, ey, ex;
            editor_get_selection(state, &sy, &sx, &ey, &ex);
            if (file_row >= sy && file_row <= ey) {
                int startc = (file_row == sy) ? ((sx < 0) ? 0 : sx) : 0;
                int endc = (file_row == ey) ? ex + 1 : row->size;
                if (startc > row->size) startc = row->size;
                if (endc > row->size) endc = row->size;
                if (endc < startc) endc = startc;
                sel_start = buffer_row_cx_to_rx(row, startc);
                sel_end = buffer_row_cx_to_rx(row, endc);
                /* Char-wise selection also covers the newline of every row it
                 * runs past, and of an empty end row (whose only cell is its
                 * newline). That is how a blank line becomes visible in `v`. */
                if (file_row < ey || (file_row == ey && ex == -1))
                    newline_sel = 1;
            }
        }
    }

    int sel_col = highlight_to_color(HL_SELECTION);
    int srch_col = highlight_to_color(HL_MATCH);
    int sel_bold = highlight_selection_bold();
    int srch_bold = highlight_search_bold();
    int used = 0;   /* text-area columns painted on this row */

    if (n > 0) {
        unsigned char colhl[4096];
        unsigned char match[4096];
        if ((size_t)n > sizeof(colhl)) n = (int)sizeof(colhl);
        display_build_color_map(row, start, end, colhl);
        display_search_overlay(state, row, start, end, match);

        int prev_color = -1;
        int prev_mark = 0;
        int prev_bold = 0;
        for (int p = 0; p < n; p++) {
            int want_sel = (start + p >= sel_start && start + p < sel_end);
            int color, mark, bold;
            if (want_sel) {
                color = (sel_col >= 0) ? sel_col : highlight_to_color(colhl[p]);
                mark = 1;
                bold = sel_bold;
            } else if (match[p] && srch_col >= 0) {
                color = srch_col;
                mark = 1;
                bold = srch_bold;
            } else {
                color = highlight_to_color(colhl[p]);
                mark = 0;
                bold = 0;
            }
            if (color != prev_color || mark != prev_mark || bold != prev_bold) {
                if (prev_color != -1 || prev_mark || prev_bold)
                    ab_puts(ab, "\x1b[0m");
                ab_set_attrs(ab, color, mark, bold);
                prev_color = color;
                prev_mark = mark;
                prev_bold = bold;
            }
            ab_append(ab, &row->render[start + p], 1);
        }
        if (prev_color != -1 || prev_mark || prev_bold)
            ab_puts(ab, "\x1b[0m");
        used = n;
    }

    /* Line-wise selection paints a full-width band. A blank or short line has
     * no characters to reverse-video, so fill the rest of the text area with
     * selected spaces; the selection stays clearly visible. */
    if (line_sel) {
        int pad = screen_cols - used;
        if (pad > 0) {
            ab_set_attrs(ab, sel_col, 1, sel_bold);
            for (int i = 0; i < pad; i++) ab_append(ab, " ", 1);
            ab_puts(ab, "\x1b[0m");
            used = screen_cols;
        }
    }

    /* Block-wise selection keeps its rectangle on short/blank rows by drawing
     * the missing columns as selected (virtual) spaces. */
    if (block_sel && sel_end > sel_start) {
        int from = sel_start - start;
        if (from < used) from = used;
        if (from < 0) from = 0;
        int to = sel_end - start;
        if (to > screen_cols) to = screen_cols;
        if (from < to) {
            for (int c = used; c < from; c++) ab_append(ab, " ", 1);
            ab_set_attrs(ab, sel_col, 1, sel_bold);
            for (int c = from; c < to; c++) ab_append(ab, " ", 1);
            ab_puts(ab, "\x1b[0m");
            used = to;
        }
    }

    /* Character-wise selection has no band; it marks the newline cell, the
     * selection's line break, one column past the text. A blank line has no
     * characters at all, so that cell (at column 0) is what makes it visible. */
    if (newline_sel) {
        int screen_col = row->rsize - start;    /* newline position on screen */
        if (screen_col >= used && screen_col < screen_cols) {
            for (int c = used; c < screen_col; c++) ab_append(ab, " ", 1);
            ab_set_attrs(ab, sel_col, 1, sel_bold);
            ab_append(ab, " ", 1);
            ab_puts(ab, "\x1b[0m");
            used = screen_col + 1;
        }
    }

    /* Append the diagnostic message inline at the end of the code text, after
     * one column of padding, coloured by severity and cropped so the whole row
     * still fits the text area (overflow would make the terminal wrap and
     * corrupt the frame). It is written after the row's characters and is never
     * part of the buffer, so it cannot be edited. */
    if (state->lsp_diags) {
        const LspDiag *d = lsp_first_diag_on_line(state->lsp_diags, file_row);
        int room = screen_cols - used - 1; /* leave 1 col padding */
        if (d && d->message[0] && room >= 4) {
            const char *label = "Hint";
            switch (d->severity) {
                case LSP_SEV_ERROR:   label = "Error";   break;
                case LSP_SEV_WARNING: label = "Warning"; break;
                case LSP_SEV_INFO:    label = "Info";    break;
                default:              label = "Hint";    break;
            }
            char text[256];
            snprintf(text, sizeof text, "%s: %.200s", label, d->message);
            sanitize_ctl_line(text);
            int color = highlight_diag_color(d->severity);
            ab_set_attrs(ab, color, 0, 0);
            ab_append(ab, " ", 1);
            ab_puts_crop(ab, text, room);
            ab_puts(ab, "\x1b[0m");
        }
    }
}

/* Display refresh screen. */
void display_refresh_screen(EditorState *state) {
    abuf ab = {0};

    /* Hide cursor while drawing */
    ab_puts(&ab, "\x1b[?25l");
    ab_puts(&ab, "\x1b[H");

    /* Draw buffer rows */
    for (int i = 0; i < state->screen_rows; i++) {
        int file_row = i + state->row_offset;
        if (file_row >= state->buffer.num_rows) {
            ab_puts(&ab, "~\x1b[K\r\n");
        } else {
            display_append_gutter(&ab, state, file_row);
            display_append_row(&ab, state, file_row);
            ab_puts(&ab, "\x1b[K\r\n");
        }
    }

    /* Status line */
    ab_puts(&ab, "\x1b[7m");
    size_t status_start = ab.len;
    const char *mode_name = "NORMAL";
    if (state->mode == MODE_INSERT) mode_name = "INSERT";
    else if (state->mode == MODE_BLOCK_INSERT) mode_name = "V-BLOCK-INSERT";
    else if (state->mode == MODE_VISUAL) {
        if (state->visual_type == VISUAL_LINE) mode_name = "V-LINE";
        else if (state->visual_type == VISUAL_BLOCK) mode_name = "V-BLOCK";
        else mode_name = "VISUAL";
    }
    else if (state->mode == MODE_COMMAND) mode_name = "COMMAND";
    else if (state->mode == MODE_SEARCH) mode_name = "SEARCH";
    else if (state->mode == MODE_EXPLORER) mode_name = "EXPLORER";
    if (state->status_msg[0] != '\0') {
        ab_puts_c(&ab, state->status_msg);
    } else if (state->mode == MODE_SEARCH) {
        /* Show what the user is typing after `/`. */
        ab_puts(&ab, "Search: /");
        ab_puts_c(&ab, state->search_buf);
    } else {
        ab_printf(&ab, "MODE: %s | Lines: %d", mode_name, state->buffer.num_rows);
    }
    /* Diagnostics come before the static context so that, on a narrow terminal,
     * the clip below drops Syntax/Theme/Ln rather than the active error. */
    if (state->lsp_diags) {
        int errs = 0, warns = 0;
        for (int i = 0; i < state->lsp_diags->count; i++) {
            if (state->lsp_diags->items[i].severity == LSP_SEV_ERROR) errs++;
            else if (state->lsp_diags->items[i].severity == LSP_SEV_WARNING) warns++;
        }
        if (state->lsp_diags->count > 0)
            ab_printf(&ab, " | E%d W%d", errs, warns);
        else
            ab_puts(&ab, " | clangd");
    }
    ab_printf(&ab, " | Syntax: %s | Theme: %s",
              highlight_syntax_name(), highlight_theme_name());
    ab_printf(&ab, " | Ln: %s",
              state->line_numbers == LN_RELATIVE ? "rel" :
              state->line_numbers == LN_OFF     ? "off" : "abs");
    if (state->mode != MODE_SEARCH && state->active_search[0] != '\0') {
        ab_puts(&ab, " | /");
        ab_puts_c(&ab, state->active_search);
    }
    /* Keep the status line on one row: clip it to the terminal width so a long
     * message cannot wrap and corrupt the frame below. */
    if (state->screen_cols > 0 && ab.len - status_start > (size_t)state->screen_cols) {
        ab.len = status_start + (size_t)state->screen_cols;
        /* Never cut inside a UTF-8 sequence: back up to a sequence start. */
        while (ab.len > 0 && (unsigned char)ab.b[ab.len - 1] >= 0x80 &&
               (unsigned char)ab.b[ab.len - 1] < 0xC0)
            ab.len--;
        ab.b[ab.len] = '\0';
    }
    /* The status line lives on the terminal's bottom row. Erase to end of line
     * but do NOT emit a newline: that would scroll the screen by one and push
     * a stray line below the status bar. */
    ab_puts(&ab, "\x1b[K\x1b[m");

    /* Composite floating windows (popups) over the text grid. They are fitted
     * to the text area, which ends above the status line on the last row. */
    WinEmitter em = { &ab, win_em_move, win_em_sgr, win_em_text };
    int any_popup = 0;
    if (state->windows.count > 0) {
        windows_draw(&state->windows, state->screen_rows, state->screen_cols, &em);
        any_popup = 1;
    }
    /* The file explorer is a full-screen overlay built from the same engine. */
    if (state->explorer.active) {
        explorer_render(state);
        if (state->explorer.layer.count > 0) {
            windows_draw(&state->explorer.layer, state->screen_rows,
                         state->screen_cols, &em);
            any_popup = 1;
        }
    }
    if (any_popup) ab_puts(&ab, "\x1b[0m");

    /* Cursor placement. While the explorer is open the buffer cursor is hidden;
     * during a create/rename prompt it rides at the end of the status line. */
    if (state->explorer.active) {
        if (state->explorer.mode != EXPLORER_BROWSE) {
            size_t plen = strlen(state->status_msg);
            int col = (int)plen + 1;
            if (state->screen_cols > 0 && col > state->screen_cols)
                col = state->screen_cols;
            if (col < 1) col = 1;
            ab_printf(&ab, "\x1b[%d;%dH", state->screen_rows + 1, col);
            ab_puts(&ab, "\x1b[?25h");
        } else {
            ab_puts(&ab, "\x1b[?25l");
        }
    } else {
        /* Cursor position (the gutter shifts the text area right by its width) */
        ab_printf(&ab, "\x1b[%d;%dH",
                  (state->cy - state->row_offset) + 1,
                  editor_gutter_width(state) + (state->rx - state->col_offset) + 1);
        ab_puts(&ab, "\x1b[?25h");
    }

    if (ab.b) {
        write(STDOUT_FILENO, ab.b, ab.len);
        free(ab.b);
    }
}