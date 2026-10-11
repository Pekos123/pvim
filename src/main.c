#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>

#include "../include/terminal.h"
#include "../include/editor.h"
#include "../include/display.h"
#include "../include/highlight.h"
#include "../include/buffer.h"
#include "../include/lsp.h"

static char *buffer_text(EditorState *state, int *len) {
    return buffer_rows_to_string(&state->buffer, len);
}

static volatile sig_atomic_t g_resized = 0;
static void on_sigwinch(int sig) {
    (void)sig;
    g_resized = 1;
}

/* Helper to start/restart clangd for a document safely */
static void setup_lsp_for_file(LspClient *lsp, EditorState *state, char *doc_path, size_t doc_path_size, int *lsp_on) {
    lsp_stop(lsp);
    lsp_init(lsp);
    *lsp_on = 0;
    state->lsp_diags = NULL;
    state->lsp_completions = NULL;
    editor_completion_close(state);
    doc_path[0] = '\0';

    if (!state->buffer.filename || state->buffer.filename[0] == '\0') return;

    /* Record which document this setup was for, even when no server supports it,
     * so the main loop's "document changed" check does not re-run setup (and
     * cancel an open completion menu) on every keystroke. */
    snprintf(doc_path, doc_path_size, "%s", state->buffer.filename);

    char lang[16];
    char root[4096];
    if (lsp_language_for(state->buffer.filename, lang, sizeof(lang))) {
        lsp_root_from_path(state->buffer.filename, root, sizeof(root));
        if (lsp_start(lsp, root) == 0) {
            *lsp_on = 1;
            state->lsp_diags = &lsp->diags;
            state->lsp_completions = &lsp->completions;
            int len = 0;
            char *text = buffer_text(state, &len);
            lsp_did_open(lsp, doc_path, text);
            free(text);
        }
    }
}

int main(int argc, char *argv[]) {
    terminal_enable_raw_mode();
    terminal_enter_alternate_screen();
    /* Install SIGWINCH without SA_RESTART so a resize interrupts poll() and the
     * loop repaints immediately. With signal()'s BSD semantics poll() is
     * restarted and the resize is only noticed on the next keystroke, leaving a
     * stale (mis-sized) frame on screen. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigwinch;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGWINCH, &sa, NULL);

    EditorState state;
    editor_init(&state);
    highlight_load_syntax();

    const char *path = (argc >= 2) ? argv[1] : NULL;

    if (path) {
        buffer_open(&state.buffer, path);
        highlight_select_for_buffer(&state.buffer);
    } else {
        buffer_insert_row(&state.buffer, 0, "Hello, OpenCode!", 16);
        /* The starter line is not an unsaved edit: start clean so `:q` on a
         * freshly opened scratch buffer doesn't demand `:q!`. */
        state.buffer.dirty = 0;
    }

    LspClient lsp;
    /* Must be zeroed before anything touches it: setup_lsp_for_file calls
     * lsp_stop() which closes whatever in_fd/out_fd happen to hold, and on an
     * uninitialized struct that garbage can close fd 0 (stdin), making poll()
     * return POLLNVAL and the main loop repaint at full speed forever. */
    lsp_init(&lsp);
    int lsp_on = 0;
    char doc_path[4096] = "";

    // Safely configure LSP for initial document
    setup_lsp_for_file(&lsp, &state, doc_path, sizeof(doc_path), &lsp_on);

    long last_dirty = state.buffer.dirty;

    while (!state.quit) {
        if (g_resized) {
            g_resized = 0;
            editor_update_window_size(&state);
            editor_scroll(&state);
        }
        display_refresh_screen(&state);

        struct pollfd fds[2];
        nfds_t nfds = 0;
        fds[nfds].fd = STDIN_FILENO;
        fds[nfds].events = POLLIN;
        nfds++;

        int lsp_idx = -1;
        if (lsp_on) {
            int fd = lsp_fd(&lsp);
            if (fd >= 0) {
                lsp_idx = (int)nfds;
                fds[nfds].fd = fd;
                fds[nfds].events = POLLIN;
                nfds++;
            }
        }

        if (poll(fds, nfds, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (lsp_idx >= 0 && (fds[lsp_idx].revents & (POLLIN | POLLHUP)))
            lsp_poll(&lsp);

        if (lsp_on && !lsp.running) {
            state.lsp_diags = NULL;
            state.lsp_completions = NULL;
            lsp_on = 0;
        }

        /* A completion response may have landed during the poll: rebuild the
         * menu (the editor ignores it when the session is not live). */
        if (lsp_on && lsp.completions_ready) {
            lsp.completions_ready = 0;
            editor_completion_update(&state);
        }

        if (fds[0].revents & POLLIN) {
            editor_process_keypress(&state);

            // Re-point LSP if the active document changed
            if (state.buffer.filename && state.buffer.filename[0] &&
                strcmp(state.buffer.filename, doc_path) != 0) {
                
                setup_lsp_for_file(&lsp, &state, doc_path, sizeof(doc_path), &lsp_on);
                last_dirty = state.buffer.dirty;
            }

            if (lsp_on && state.buffer.dirty != last_dirty) {
                last_dirty = state.buffer.dirty;
                int len = 0;
                char *text = buffer_text(&state, &len);
                lsp_did_change(&lsp, doc_path, text);
                free(text);
            }

            /* Completion requested by the editor this keystroke. The didChange
             * above has already been sent, so the server sees the latest text. */
            if (state.completion_request) {
                state.completion_request = 0;
                if (lsp_on)
                    lsp_request_completion(&lsp, state.completion_req_line,
                                           state.completion_req_char);
            }
        }
    }

    lsp_stop(&lsp);
    terminal_leave_alternate_screen();
    terminal_disable_raw_mode();
    editor_free(&state);
    return 0;
}
