#ifndef PVIM_BUFFER_H
#define PVIM_BUFFER_H

#include <stddef.h>

#define PVIM_TAB_STOP 4

typedef struct erow {
    int idx;
    int size;
    int rsize;
    char *chars;
    char *render;
    unsigned char *hl;   /* 1:1 syntax-highlight mapping for `chars` */
    int hl_open_comment; /* nonzero while this row ends inside an mlcomment */
} erow;

typedef struct EditorBuffer {
    int num_rows;
    erow *rows;
    char *filename;
    int dirty;
} EditorBuffer;

void buffer_init(EditorBuffer *buf);
void buffer_free(EditorBuffer *buf);
void buffer_clear_rows(EditorBuffer *buf);
int buffer_open(EditorBuffer *buf, const char *filename);
int buffer_save(EditorBuffer *buf, const char *filename);
char *buffer_rows_to_string(EditorBuffer *buf, int *buflen);

void buffer_insert_row(EditorBuffer *buf, int at, const char *s, size_t len);
void buffer_del_row(EditorBuffer *buf, int at);
void buffer_row_insert_char(EditorBuffer *buf, erow *row, int at, int c);
void buffer_row_append_string(EditorBuffer *buf, erow *row, const char *s, size_t len);
void buffer_row_del_char(EditorBuffer *buf, erow *row, int at);
void buffer_update_row(erow *row);

int buffer_row_cx_to_rx(const erow *row, int cx);
int buffer_row_rx_to_cx(const erow *row, int rx);

#endif /* PVIM_BUFFER_H */
