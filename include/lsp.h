#ifndef PVIM_LSP_H
#define PVIM_LSP_H

#include <stddef.h>

/*
 * Minimal Language Server Protocol client.
 *
 * Speaks JSON-RPC 2.0 over the clangd child process's stdio pipes, using the
 * LSP base protocol framing (Content-Length header + body). The client is
 * non-blocking: the editor polls lsp_fd() and drains messages with lsp_poll(),
 * so clangd output never stalls the UI.
 *
 * The pieces needed for live diagnostics and completion are implemented:
 * initialize / initialized handshake, textDocument/didOpen, full-text
 * textDocument/didChange, textDocument/publishDiagnostics and
 * textDocument/completion. Unknown notifications and requests are ignored.
 */

/* Diagnostic severities, as defined by the LSP. */
enum {
    LSP_SEV_ERROR = 1,
    LSP_SEV_WARNING = 2,
    LSP_SEV_INFO = 3,
    LSP_SEV_HINT = 4
};

typedef struct LspDiag {
    int line;       /* 0-based start line */
    int severity;   /* LSP_SEV_* */
    char message[256];
} LspDiag;

typedef struct LspDiagnostics {
    LspDiag *items;
    int count;
    int cap;
} LspDiagnostics;

/* Completion item kinds, mirroring the LSP CompletionItemKind (1..25). */
enum {
    LSP_CK_TEXT = 1, LSP_CK_METHOD, LSP_CK_FUNCTION, LSP_CK_CONSTRUCTOR,
    LSP_CK_FIELD, LSP_CK_VARIABLE, LSP_CK_CLASS, LSP_CK_INTERFACE,
    LSP_CK_MODULE, LSP_CK_PROPERTY, LSP_CK_UNIT, LSP_CK_VALUE,
    LSP_CK_ENUM, LSP_CK_KEYWORD, LSP_CK_SNIPPET, LSP_CK_COLOR,
    LSP_CK_FILE, LSP_CK_REFERENCE, LSP_CK_FOLDER, LSP_CK_ENUM_MEMBER,
    LSP_CK_CONSTANT, LSP_CK_STRUCT, LSP_CK_EVENT, LSP_CK_OPERATOR,
    LSP_CK_TYPE_PARAM
};

/* Upper bound on the items kept from a single completion response. */
#define LSP_COMPLETION_MAX 200

/* Where a completion candidate came from; drives the menu's source badge. */
enum {
    LSP_SRC_LSP = 0,   /* a language server (clangd) */
    LSP_SRC_BUFFER = 1, /* a word already present in the buffer */
    LSP_SRC_HEADER = 2  /* a header found by the local include scan */
};

/* One completion candidate. `insert` is the text to insert (textEdit.newText
 * preferred over insertText, falling back to the label); `filter` is the text
 * matched against the typed prefix when the server supplies one. `label_detail`
 * holds clangd's parameter list (labelDetails.detail) and `label_desc` its
 * return type / include hint (labelDetails.description), used for the function
 * argument preview. */
typedef struct LspCompletion {
    char label[128];
    char insert[256];
    char filter[128];
    char detail[128];
    char label_detail[128];  /* labelDetails.detail: "(int, char *)" */
    char label_desc[96];     /* labelDetails.description: return type / header */
    int kind;                /* LSP_CK_* or 0 when unknown */
    int source;              /* LSP_SRC_* */
    int snippet;             /* inserted text is a snippet (insertTextFormat 2) */
} LspCompletion;

typedef struct LspCompletions {
    LspCompletion *items;
    int count;
    int cap;
} LspCompletions;

typedef struct LspClient {
    int in_fd;        /* read clangd's stdout (-1 when stopped) */
    int out_fd;       /* write clangd's stdin (-1 when stopped) */
    int pid;
    int running;
    int initialized;  /* initialize response received */
    int next_id;
    char *doc_uri;    /* URI of the opened document, or NULL */
    int doc_version;
    char language_id[16];
    char *pending_path;   /* didOpen queued until the handshake completes */
    char *pending_text;
    char *recv;           /* receive accumulator */
    size_t recv_len, recv_cap;
    LspDiagnostics diags;
    /* Latest textDocument/completion result. `completion_id` is the id of the
     * request still in flight (0 when none); `completions_ready` is set once a
     * matching response is parsed and cleared by the next request. */
    LspCompletions completions;
    int completion_id;
    int completions_ready;
} LspClient;

/* Zero a client before use. */
void lsp_init(LspClient *c);

/* Spawn clangd rooted at `root_dir`. Returns 0 on success, -1 when the child
 * could not be started (callers should degrade gracefully). */
int lsp_start(LspClient *c, const char *root_dir);

/* Shut the server down and release every buffer. Safe to call more than once. */
void lsp_stop(LspClient *c);

/* fd to poll for readability, or -1 when not running. */
int lsp_fd(const LspClient *c);

/* Drain all currently available server output without blocking. */
void lsp_poll(LspClient *c);

/* Open `path` with the given full text. Queued until the handshake finishes. */
void lsp_did_open(LspClient *c, const char *path, const char *text);

/* Replace the open document's text (full synchronisation). */
void lsp_did_change(LspClient *c, const char *path, const char *text);

/* Ask the server for completions at the given 0-based line and UTF-16
 * character offset. Returns 1 when the request was sent, 0 when the server is
 * unavailable (not started / not initialised / no open document). */
int lsp_request_completion(LspClient *c, int line, int character);

/* Drop / release the stored completion items. */
void lsp_completions_clear(LspCompletions *l);
void lsp_completions_free(LspCompletions *l);

/* Choose clangd's languageId for `path`. Returns 0 when unsupported. */
int lsp_language_for(const char *path, char *out, size_t n);

/* Build "file://<absolute path>" for `path` (percent-encoded as needed). */
void lsp_uri_from_path(const char *path, char *out, size_t n);

/* Directory clangd should be rooted at: the file's directory. */
void lsp_root_from_path(const char *path, char *out, size_t n);

/* Diagnostics helpers used by the renderer. */
void lsp_diags_clear(LspDiagnostics *d);
void lsp_diags_free(LspDiagnostics *d);

/* First (most severe) diagnostic starting on `line`, or NULL. Kept inline so
 * display-only translation units do not need to link the transport. */
static inline const LspDiag *lsp_first_diag_on_line(const LspDiagnostics *d, int line) {
    const LspDiag *best = NULL;
    if (!d) return NULL;
    for (int i = 0; i < d->count; i++) {
        if (d->items[i].line != line) continue;
        if (!best || d->items[i].severity < best->severity) best = &d->items[i];
    }
    return best;
}

#endif /* PVIM_LSP_H */
