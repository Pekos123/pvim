#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include "../include/buffer.h"
#include "../include/highlight.h"


void buffer_init(EditorBuffer *buf) {
    buf->num_rows = 0;
    buf->rows = NULL;
    buf->filename = NULL;
    buf->dirty = 0;
}

void buffer_free(EditorBuffer *buf) {
    for (int i = 0; i < buf->num_rows; i++) {
        free(buf->rows[i].chars);
        free(buf->rows[i].render);
        free(buf->rows[i].hl);
    }
    free(buf->rows);
    free(buf->filename);
}

/* Free all rows but keep the filename and dirty flag intact. */
void buffer_clear_rows(EditorBuffer *buf) {
    for (int i = 0; i < buf->num_rows; i++) {
        free(buf->rows[i].chars);
        free(buf->rows[i].render);
        free(buf->rows[i].hl);
    }
    free(buf->rows);
    buf->rows = NULL;
    buf->num_rows = 0;
}

int buffer_open(EditorBuffer *buf, const char *filename) {
    free(buf->filename);
    buf->filename = strdup(filename);

    FILE *fp = fopen(filename, "r");
    if (!fp) {
        if (errno != ENOENT) return -1;
        buffer_insert_row(buf, 0, "", 0);
        return 0;
    }

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
            linelen--;
        buffer_insert_row(buf, buf->num_rows, line, linelen);
    }
    free(line);
    fclose(fp);
    buf->dirty = 0;
    return 0;
}

int buffer_save(EditorBuffer *buf, const char *filename) {
    FILE *fp = fopen(filename, "w");
    if (!fp) return -1;

    for (int i = 0; i < buf->num_rows; i++) {
        fprintf(fp, "%s\n", buf->rows[i].chars);
    }
    fclose(fp);
    buf->dirty = 0;
    return 0;
}

/* Join every row into one document string, one '\n' per row (including the
 * last). Used to hand full text to the language server. */
char *buffer_rows_to_string(EditorBuffer *buf, int *buflen) {
    size_t total = 0;
    for (int i = 0; i < buf->num_rows; i++)
        total += (size_t)buf->rows[i].size + 1;

    char *out = malloc(total + 1);
    if (!out) {
        if (buflen) *buflen = 0;
        return NULL;
    }

    size_t o = 0;
    for (int i = 0; i < buf->num_rows; i++) {
        memcpy(out + o, buf->rows[i].chars, (size_t)buf->rows[i].size);
        o += (size_t)buf->rows[i].size;
        out[o++] = '\n';
    }
    out[o] = '\0';
    if (buflen) *buflen = (int)o;
    return out;
}

void buffer_update_row(erow *row) {
    int tabs = 0;
    for (int j = 0; j < row->size; j++)
        if (row->chars[j] == '\t') tabs++;

    free(row->render);
    row->render = malloc(row->size + tabs*(PVIM_TAB_STOP - 1) + 1);

    int idx = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % PVIM_TAB_STOP != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

void buffer_insert_row(EditorBuffer *buf, int at, const char *s, size_t len) {
    if (at < 0 || at > buf->num_rows) return;

    buf->rows = realloc(buf->rows, sizeof(erow) * (buf->num_rows + 1));
    memmove(&buf->rows[at + 1], &buf->rows[at], sizeof(erow) * (buf->num_rows - at));

    buf->rows[at].idx = at;
    buf->rows[at].size = len;
    buf->rows[at].chars = malloc(len + 1);
    memcpy(buf->rows[at].chars, s, len);
    buf->rows[at].chars[len] = '\0';

    buf->rows[at].rsize = 0;
    buf->rows[at].render = NULL;
    buf->rows[at].hl = NULL;
    buf->rows[at].hl_open_comment = 0;
    buffer_update_row(&buf->rows[at]);

    buf->num_rows++;
    buf->dirty++;

    /* Keep the row indices consistent after the shift. */
    for (int i = at; i < buf->num_rows; i++)
        buf->rows[i].idx = i;

    /* Rescan the new row and every later row, whose incoming comment state may
     * have changed even if the new row itself ends in a stable state. */
    highlight_rescan_buffer(buf, at);
}

void buffer_row_insert_char(EditorBuffer *buf, erow *row, int at, int c) {
    if (at < 0 || at > row->size) at = row->size;
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    buffer_update_row(row);
    buf->dirty++;
    highlight_update_buffer(buf, (int)(row - buf->rows));
}

int buffer_row_cx_to_rx(const erow *row, int cx) {
    int rx = 0;
    for (int j = 0; j < cx; j++) {
        if (row->chars[j] == '\t')
            rx += (PVIM_TAB_STOP - 1) - (rx % PVIM_TAB_STOP);
        rx++;
    }
    return rx;
}

void buffer_row_del_char(EditorBuffer *buf, erow *row, int at) {
    if (at < 0 || at >= row->size) return;
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    buffer_update_row(row);
    buf->dirty++;
    highlight_update_buffer(buf, (int)(row - buf->rows));
}

void buffer_row_append_string(EditorBuffer *buf, erow *row, const char *s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    buffer_update_row(row);
    buf->dirty++;
    highlight_update_buffer(buf, (int)(row - buf->rows));
}

void buffer_del_row(EditorBuffer *buf, int at) {
    if (at < 0 || at >= buf->num_rows) return;
    free(buf->rows[at].chars);
    free(buf->rows[at].render);
    free(buf->rows[at].hl);
    memmove(&buf->rows[at], &buf->rows[at + 1], sizeof(erow) * (buf->num_rows - at - 1));
    buf->num_rows--;
    buf->dirty++;

    /* Keep the row indices consistent after the shift. */
    for (int i = at; i < buf->num_rows; i++)
        buf->rows[i].idx = i;

    /* The row now at `at` has a new previous row; rescan the whole tail. */
    highlight_rescan_buffer(buf, at);
}


