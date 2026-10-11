#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../include/clipboard.h"

/* Copy/paste helpers. Missing tools fail fast (127) with stderr discarded, so
 * we can simply try each one in turn. The commands are fixed strings, so there
 * is no shell-injection surface; the payload only travels over the pipe. */
static const char *COPY_CMDS[] = {
    "wl-copy 2>/dev/null",
    "xclip -selection clipboard 2>/dev/null",
    "xsel --clipboard --input 2>/dev/null",
    "pbcopy 2>/dev/null",
    NULL
};

static const char *PASTE_CMDS[] = {
    "wl-paste --no-newline 2>/dev/null",
    "xclip -selection clipboard -o 2>/dev/null",
    "xsel --clipboard --output 2>/dev/null",
    "pbpaste 2>/dev/null",
    NULL
};

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* A helper that exits before reading everything would otherwise raise SIGPIPE
 * and kill the editor mid-write. */
static void ignore_sigpipe(void) {
    static int done = 0;
    if (!done) {
        signal(SIGPIPE, SIG_IGN);
        done = 1;
    }
}

/* OSC 52: ask the terminal itself to set the clipboard. Works over ssh and
 * needs no external tools; terminals that disallow it simply ignore us. */
static int copy_osc52(const char *text, size_t len) {
    size_t b64len = ((len + 2) / 3) * 4;
    char *b64 = malloc(b64len + 1);
    if (!b64) return -1;

    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        unsigned v = (unsigned char)text[i] << 16;
        if (i + 1 < len) v |= (unsigned char)text[i + 1] << 8;
        if (i + 2 < len) v |= (unsigned char)text[i + 2];
        b64[o++] = B64[(v >> 18) & 63];
        b64[o++] = B64[(v >> 12) & 63];
        b64[o++] = (i + 1 < len) ? B64[(v >> 6) & 63] : '=';
        b64[o++] = (i + 2 < len) ? B64[v & 63] : '=';
    }
    b64[o] = '\0';

    char *seq = malloc(o + 16);
    if (!seq) { free(b64); return -1; }
    int n = snprintf(seq, o + 16, "\x1b]52;c;%s\x07", b64);
    free(b64);
    if (n < 0) { free(seq); return -1; }

    ssize_t w = write(STDOUT_FILENO, seq, (size_t)n);
    free(seq);
    return (w == n) ? 0 : -1;
}

int clipboard_copy(const char *text, size_t len) {
    if (!text) return -1;
    ignore_sigpipe();

    for (int i = 0; COPY_CMDS[i]; i++) {
        FILE *p = popen(COPY_CMDS[i], "w");
        if (!p) continue;
        size_t written = len ? fwrite(text, 1, len, p) : 0;
        if (pclose(p) == 0 && written == len)
            return 0;
    }
    return copy_osc52(text, len);
}

char *clipboard_paste(size_t *out_len) {
    for (int i = 0; PASTE_CMDS[i]; i++) {
        FILE *p = popen(PASTE_CMDS[i], "r");
        if (!p) continue;

        size_t cap = 4096, len = 0;
        char *buf = malloc(cap);
        if (!buf) { pclose(p); continue; }

        char chunk[4096];
        size_t n;
        while ((n = fread(chunk, 1, sizeof(chunk), p)) > 0) {
            if (len + n + 1 > cap) {
                while (len + n + 1 > cap) cap *= 2;
                char *nb = realloc(buf, cap);
                if (!nb) { free(buf); buf = NULL; break; }
                buf = nb;
            }
            memcpy(buf + len, chunk, n);
            len += n;
        }
        int rc = pclose(p);
        if (!buf) continue;
        if (rc == 0) {
            buf[len] = '\0';
            if (out_len) *out_len = len;
            return buf;
        }
        free(buf);
    }
    return NULL;
}
