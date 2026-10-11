#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../include/lsp.h"

/* ------------------------------ string buffer --------------------------- */

typedef struct {
    char *b;
    size_t len;
    size_t cap;
} StrBuf;

static void sb_reserve(StrBuf *sb, size_t extra) {
    if (sb->len + extra + 1 <= sb->cap) return;
    size_t ncap = sb->cap ? sb->cap : 256;
    while (sb->len + extra + 1 > ncap) ncap *= 2;
    char *nb = realloc(sb->b, ncap);
    if (!nb) return;
    sb->b = nb;
    sb->cap = ncap;
}

static void sb_append(StrBuf *sb, const char *s, size_t n) {
    sb_reserve(sb, n);
    if (!sb->b) return;
    memcpy(sb->b + sb->len, s, n);
    sb->len += n;
    sb->b[sb->len] = '\0';
}

static void sb_puts(StrBuf *sb, const char *s) { sb_append(sb, s, strlen(s)); }
static void sb_putc(StrBuf *sb, char c) { sb_append(sb, &c, 1); }

static void sb_printf(StrBuf *sb, const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof(tmp)) {
        sb_append(sb, tmp, (size_t)n);
    } else {
        char *big = malloc((size_t)n + 1);
        if (!big) return;
        va_start(ap, fmt);
        vsnprintf(big, (size_t)n + 1, fmt, ap);
        va_end(ap);
        sb_append(sb, big, (size_t)n);
        free(big);
    }
}

static char *sb_take(StrBuf *sb) {
    if (!sb->b) return strdup("");
    char *b = sb->b;
    sb->b = NULL;
    sb->len = sb->cap = 0;
    return b;
}

/* Encode `s` as a JSON string literal (including the surrounding quotes). */
static void sb_puts_json(StrBuf *sb, const char *s) {
    sb_putc(sb, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '"':  sb_puts(sb, "\\\""); break;
            case '\\': sb_puts(sb, "\\\\"); break;
            case '\n': sb_puts(sb, "\\n"); break;
            case '\r': sb_puts(sb, "\\r"); break;
            case '\t': sb_puts(sb, "\\t"); break;
            case '\b': sb_puts(sb, "\\b"); break;
            case '\f': sb_puts(sb, "\\f"); break;
            default:
                if (*p < 0x20) {
                    char esc[8];
                    snprintf(esc, sizeof(esc), "\\u%04x", *p);
                    sb_puts(sb, esc);
                } else {
                    sb_putc(sb, (char)*p);
                }
        }
    }
    sb_putc(sb, '"');
}

/* ------------------------------ tiny JSON parser ------------------------ */

enum { JNULL, JBOOL, JNUM, JSTR, JARR, JOBJ };

typedef struct Json {
    int type;
    double num;
    int boolean;
    char *str;
    struct Json **arr;
    int alen, acap;
    char **keys;
    struct Json **vals;
    int olen, ocap;
} Json;

static Json *jnew(int type) {
    Json *j = calloc(1, sizeof(Json));
    if (j) j->type = type;
    return j;
}

static void jfree(Json *j) {
    if (!j) return;
    if (j->type == JSTR) free(j->str);
    if (j->type == JARR) {
        for (int i = 0; i < j->alen; i++) jfree(j->arr[i]);
        free(j->arr);
    }
    if (j->type == JOBJ) {
        for (int i = 0; i < j->olen; i++) {
            free(j->keys[i]);
            jfree(j->vals[i]);
        }
        free(j->keys);
        free(j->vals);
    }
    free(j);
}

static Json *jparse_value(const char **p);

static void jskip(const char **p) {
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') (*p)++;
}

static void sb_put_utf8(StrBuf *sb, unsigned cp) {
    if (cp < 0x80) {
        sb_putc(sb, (char)cp);
    } else if (cp < 0x800) {
        sb_putc(sb, (char)(0xC0 | (cp >> 6)));
        sb_putc(sb, (char)(0x80 | (cp & 0x3F)));
    } else {
        sb_putc(sb, (char)(0xE0 | (cp >> 12)));
        sb_putc(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_putc(sb, (char)(0x80 | (cp & 0x3F)));
    }
}

static char *jparse_string(const char **p) {
    if (**p != '"') return NULL;
    (*p)++;
    StrBuf sb = {0};
    while (**p && **p != '"') {
        if (**p == '\\') {
            (*p)++;
            char e = **p;
            (*p)++;
            switch (e) {
                case '"':  sb_putc(&sb, '"'); break;
                case '\\': sb_putc(&sb, '\\'); break;
                case '/':  sb_putc(&sb, '/'); break;
                case 'b':  sb_putc(&sb, '\b'); break;
                case 'f':  sb_putc(&sb, '\f'); break;
                case 'n':  sb_putc(&sb, '\n'); break;
                case 'r':  sb_putc(&sb, '\r'); break;
                case 't':  sb_putc(&sb, '\t'); break;
                case 'u': {
                    unsigned cp = 0;
                    int ok = 1;
                    for (int i = 0; i < 4; i++) {
                        char h = (*p)[i];
                        int d;
                        if (h >= '0' && h <= '9') d = h - '0';
                        else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
                        else { ok = 0; break; }
                        cp = cp * 16 + (unsigned)d;
                    }
                    if (ok) { *p += 4; sb_put_utf8(&sb, cp); }
                    else sb_putc(&sb, '?');
                    break;
                }
                default: sb_putc(&sb, e); break;
            }
        } else {
            sb_putc(&sb, **p);
            (*p)++;
        }
    }
    if (**p == '"') (*p)++;
    return sb_take(&sb);
}

static Json *jparse_array(const char **p) {
    Json *j = jnew(JARR);
    if (!j) return NULL;
    (*p)++; /* '[' */
    jskip(p);
    if (**p == ']') { (*p)++; return j; }
    for (;;) {
        Json *v = jparse_value(p);
        if (!v) break;
        if (j->alen == j->acap) {
            j->acap = j->acap ? j->acap * 2 : 8;
            Json **na = realloc(j->arr, sizeof(Json *) * (size_t)j->acap);
            if (!na) { jfree(v); break; }
            j->arr = na;
        }
        j->arr[j->alen++] = v;
        jskip(p);
        if (**p == ',') { (*p)++; jskip(p); continue; }
        if (**p == ']') { (*p)++; break; }
        break;
    }
    return j;
}

static Json *jparse_object(const char **p) {
    Json *j = jnew(JOBJ);
    if (!j) return NULL;
    (*p)++; /* '{' */
    jskip(p);
    if (**p == '}') { (*p)++; return j; }
    for (;;) {
        if (**p != '"') break;
        char *key = jparse_string(p);
        if (!key) break;
        jskip(p);
        if (**p == ':') (*p)++;
        jskip(p);
        Json *v = jparse_value(p);
        if (j->olen == j->ocap) {
            j->ocap = j->ocap ? j->ocap * 2 : 8;
            char **nk = realloc(j->keys, sizeof(char *) * (size_t)j->ocap);
            Json **nv = realloc(j->vals, sizeof(Json *) * (size_t)j->ocap);
            if (!nk || !nv) { free(key); jfree(v); break; }
            j->keys = nk;
            j->vals = nv;
        }
        j->keys[j->olen] = key;
        j->vals[j->olen] = v;
        j->olen++;
        jskip(p);
        if (**p == ',') { (*p)++; jskip(p); continue; }
        if (**p == '}') { (*p)++; break; }
        break;
    }
    return j;
}

static Json *jparse_value(const char **p) {
    jskip(p);
    switch (**p) {
        case 'n': if (!strncmp(*p, "null", 4)) { *p += 4; return jnew(JNULL); } break;
        case 't': if (!strncmp(*p, "true", 4)) { *p += 4; Json *j = jnew(JBOOL); if (j) j->boolean = 1; return j; } break;
        case 'f': if (!strncmp(*p, "false", 5)) { *p += 5; return jnew(JBOOL); } break;
        case '"': { Json *j = jnew(JSTR); if (j) j->str = jparse_string(p); return j; }
        case '[': return jparse_array(p);
        case '{': return jparse_object(p);
        default:
            if (**p == '-' || (**p >= '0' && **p <= '9')) {
                char *end = NULL;
                double d = strtod(*p, &end);
                if (end && end != *p) { *p = end; Json *j = jnew(JNUM); if (j) j->num = d; return j; }
            }
    }
    return NULL;
}

static Json *jparse(const char *s) {
    if (!s) return NULL;
    const char *p = s;
    return jparse_value(&p);
}

static Json *jget(const Json *o, const char *key) {
    if (!o || o->type != JOBJ) return NULL;
    for (int i = 0; i < o->olen; i++)
        if (!strcmp(o->keys[i], key)) return o->vals[i];
    return NULL;
}

/* ------------------------------ path helpers ---------------------------- */

static void absolute_path(const char *path, char *out, size_t n) {
    char resolved[PATH_MAX];
    if (realpath(path, resolved)) {
        snprintf(out, n, "%s", resolved);
        return;
    }
    if (path[0] == '/') {
        snprintf(out, n, "%s", path);
        return;
    }
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)))
        snprintf(out, n, "%s/%s", cwd, path);
    else
        snprintf(out, n, "%s", path);
}

static int is_uri_safe(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
           c == '~' || c == '/';
}

void lsp_uri_from_path(const char *path, char *out, size_t n) {
    char abs[PATH_MAX];
    absolute_path(path, abs, sizeof(abs));

    size_t o = 0;
    const char *prefix = "file://";
    for (const char *q = prefix; *q && o + 1 < n; q++) out[o++] = *q;
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)abs; *p && o + 4 < n; p++) {
        if (is_uri_safe(*p)) {
            out[o++] = (char)*p;
        } else {
            out[o++] = '%';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 0xF];
        }
    }
    out[o] = '\0';
}

void lsp_root_from_path(const char *path, char *out, size_t n) {
    char abs[PATH_MAX];
    absolute_path(path, abs, sizeof(abs));
    char *slash = strrchr(abs, '/');
    if (slash && slash != abs) *slash = '\0';
    else snprintf(abs, sizeof(abs), ".");
    snprintf(out, n, "%s", abs);
}

int lsp_language_for(const char *path, char *out, size_t n) {
    const char *dot = strrchr(path, '.');
    if (!dot) return 0;
    const char *e = dot + 1;
    struct { const char *ext; const char *lang; } table[] = {
        {"c", "c"}, {"h", "c"},
        {"cc", "cpp"}, {"cpp", "cpp"}, {"cxx", "cpp"}, {"c++", "cpp"},
        {"hh", "cpp"}, {"hpp", "cpp"}, {"hxx", "cpp"}, {"h++", "cpp"},
        {"m", "objective-c"}, {"mm", "objective-cpp"},
        {NULL, NULL}
    };
    for (int i = 0; table[i].ext; i++) {
        if (!strcasecmp(e, table[i].ext)) {
            snprintf(out, n, "%s", table[i].lang);
            return 1;
        }
    }
    return 0;
}

/* ------------------------------ diagnostics ----------------------------- */

void lsp_diags_clear(LspDiagnostics *d) {
    if (!d) return;
    for (int i = 0; i < d->count; i++) d->items[i].message[0] = '\0';
    d->count = 0;
}

void lsp_diags_free(LspDiagnostics *d) {
    if (!d) return;
    free(d->items);
    d->items = NULL;
    d->count = 0;
    d->cap = 0;
}

void lsp_completions_clear(LspCompletions *l) {
    if (!l) return;
    l->count = 0;
}

void lsp_completions_free(LspCompletions *l) {
    if (!l) return;
    free(l->items);
    l->items = NULL;
    l->count = 0;
    l->cap = 0;
}

static void diag_push(LspDiagnostics *d, const LspDiag *src) {
    if (d->count == d->cap) {
        int ncap = d->cap ? d->cap * 2 : 8;
        LspDiag *ni = realloc(d->items, sizeof(LspDiag) * (size_t)ncap);
        if (!ni) return;
        d->items = ni;
        d->cap = ncap;
    }
    d->items[d->count++] = *src;
}

/* ------------------------------ IPC sending ----------------------------- */

static int write_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n > 0) { off += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        return -1;
    }
    return 0;
}

static int lsp_send_raw(LspClient *c, const char *body) {
    if (!c || !c->running || c->out_fd < 0 || !body) return -1;
    char header[64];
    int hn = snprintf(header, sizeof(header), "Content-Length: %zu\r\n\r\n", strlen(body));
    if (hn < 0) return -1;
    if (write_all(c->out_fd, header, (size_t)hn) < 0) return -1;
    return write_all(c->out_fd, body, strlen(body));
}

static void send_initialize(LspClient *c, const char *root_uri) {
    StrBuf sb = {0};
    sb_puts(&sb, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{");
    sb_printf(&sb, "\"processId\":%d,", (int)getpid());
    sb_puts(&sb, "\"clientInfo\":{\"name\":\"pvim\"},\"rootUri\":");
    if (root_uri && root_uri[0]) sb_puts_json(&sb, root_uri);
    else sb_puts(&sb, "null");
    sb_puts(&sb, ",\"capabilities\":{\"textDocument\":{\"completion\":{"
                 "\"completionItem\":{\"labelDetailsSupport\":true}}}},"
                 "\"initializationOptions\":{\"clangd\":{\"headerInsertion\":\"never\"}}}}");
    char *body = sb_take(&sb);
    lsp_send_raw(c, body);
    free(body);
}

static void send_initialized(LspClient *c) {
    lsp_send_raw(c, "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}");
}

static void send_did_open(LspClient *c, const char *uri, const char *lang,
                          const char *text) {
    StrBuf sb = {0};
    sb_puts(&sb, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{");
    sb_puts(&sb, "\"uri\":"); sb_puts_json(&sb, uri);
    sb_puts(&sb, ",\"languageId\":"); sb_puts_json(&sb, lang);
    sb_printf(&sb, ",\"version\":%d,", c->doc_version);
    sb_puts(&sb, "\"text\":"); sb_puts_json(&sb, text ? text : "");
    sb_puts(&sb, "}}}");
    char *body = sb_take(&sb);
    lsp_send_raw(c, body);
    free(body);
}

static void send_did_change(LspClient *c, const char *uri, const char *text) {
    StrBuf sb = {0};
    sb_puts(&sb, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",\"params\":{");
    sb_puts(&sb, "\"textDocument\":{\"uri\":"); sb_puts_json(&sb, uri);
    sb_printf(&sb, ",\"version\":%d},", c->doc_version);
    sb_puts(&sb, "\"contentChanges\":[{\"text\":");
    sb_puts_json(&sb, text ? text : "");
    sb_puts(&sb, "}]}}");
    char *body = sb_take(&sb);
    lsp_send_raw(c, body);
    free(body);
}

/* ------------------------------ IPC receiving --------------------------- */

static void handle_diagnostics(LspClient *c, Json *params) {
    if (!params) return;
    Json *uri = jget(params, "uri");
    if (uri && uri->type == JSTR && c->doc_uri && strcmp(uri->str, c->doc_uri) != 0)
        return;

    Json *diags = jget(params, "diagnostics");
    if (!diags || diags->type != JARR) return;

    lsp_diags_clear(&c->diags);
    for (int i = 0; i < diags->alen; i++) {
        Json *d = diags->arr[i];
        if (!d || d->type != JOBJ) continue;

        LspDiag nd;
        nd.line = 0;
        nd.severity = LSP_SEV_ERROR;
        nd.message[0] = '\0';

        Json *sev = jget(d, "severity");
        if (sev && sev->type == JNUM) nd.severity = (int)sev->num;

        Json *msg = jget(d, "message");
        if (msg && msg->type == JSTR)
            snprintf(nd.message, sizeof(nd.message), "%s", msg->str);

        Json *range = jget(d, "range");
        Json *start = range ? jget(range, "start") : NULL;
        Json *line = start ? jget(start, "line") : NULL;
        if (line && line->type == JNUM) nd.line = (int)line->num;

        diag_push(&c->diags, &nd);
    }
}

/* ------------------------------ completion ------------------------------ */

/* Convert an LSP snippet (insertTextFormat 2) to plain text: `$1` / `${1}` are
 * dropped, `${1:default}` keeps `default`, `${1|a,b|}` keeps `a`, and an
 * escaped character (`\$`, `\}`) loses the backslash. clangd only sends
 * snippets when the client advertises snippetSupport, but stripping them keeps
 * insertion correct if that ever changes. */
static void snippet_to_plain(const char *in, char *out, size_t n) {
    size_t o = 0;
    for (const char *p = in; *p;) {
        if (*p == '\\' && p[1]) {          /* escaped literal */
            if (o + 1 < n) out[o++] = p[1];
            p += 2;
            continue;
        }
        if (*p == '$' && p[1] == '{') {
            const char *q = p + 2;
            while (*q && *q != ':' && *q != '}' && *q != '|') q++;
            if (*q == ':' || *q == '|') {  /* keep the default / first choice */
                const char *b = q + 1;
                while (*b && *b != '}' && *b != ',' && *b != '|') {
                    if (o + 1 < n) out[o++] = *b;
                    b++;
                }
            }
            while (*p && *p != '}') p++;
            if (*p == '}') p++;
            continue;
        }
        if (*p == '$' && p[1] >= '0' && p[1] <= '9') {  /* `$1`: drop */
            p++;
            while (*p >= '0' && *p <= '9') p++;
            continue;
        }
        if (o + 1 < n) out[o++] = *p;
        p++;
    }
    out[o] = '\0';
}

/* Strip leading and trailing ASCII whitespace in place. clangd pads some
 * completion strings (e.g. a label of " malloc"), which would otherwise leave
 * a stray space in the menu's columns. */
static void trim_ws(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = '\0';
}

static void comp_push(LspCompletions *l, const LspCompletion *src) {
    if (l->count == l->cap) {
        int ncap = l->cap ? l->cap * 2 : 16;
        LspCompletion *ni = realloc(l->items, sizeof(LspCompletion) * (size_t)ncap);
        if (!ni) return;
        l->items = ni;
        l->cap = ncap;
    }
    l->items[l->count++] = *src;
}

static void handle_completion(LspClient *c, Json *result) {
    lsp_completions_clear(&c->completions);

    Json *items = result;
    if (result && result->type == JOBJ) items = jget(result, "items");
    if (items && items->type == JARR) {
        for (int i = 0; i < items->alen && c->completions.count < LSP_COMPLETION_MAX; i++) {
            Json *it = items->arr[i];
            if (!it || it->type != JOBJ) continue;

            Json *label = jget(it, "label");
            if (!label || label->type != JSTR) continue;

            LspCompletion nc;
            memset(&nc, 0, sizeof nc);
            snprintf(nc.label, sizeof(nc.label), "%s", label->str);

            Json *fmt = jget(it, "insertTextFormat");
            int snippet = (fmt && fmt->type == JNUM && (int)fmt->num == 2);

            char raw[256] = "";
            Json *text = jget(it, "insertText");
            if (text && text->type == JSTR)
                snprintf(raw, sizeof(raw), "%s", text->str);
            Json *te = jget(it, "textEdit");
            if (te && te->type == JOBJ) {
                Json *nt = jget(te, "newText");
                if (nt && nt->type == JSTR)
                    snprintf(raw, sizeof(raw), "%s", nt->str);
            }
            if (!raw[0]) snprintf(raw, sizeof(raw), "%s", nc.label);
            if (snippet) snippet_to_plain(raw, nc.insert, sizeof(nc.insert));
            else snprintf(nc.insert, sizeof(nc.insert), "%s", raw);

            Json *f = jget(it, "filterText");
            if (f && f->type == JSTR)
                snprintf(nc.filter, sizeof(nc.filter), "%s", f->str);
            Json *det = jget(it, "detail");
            if (det && det->type == JSTR)
                snprintf(nc.detail, sizeof(nc.detail), "%s", det->str);
            Json *kind = jget(it, "kind");
            if (kind && kind->type == JNUM) nc.kind = (int)kind->num;

            /* clangd puts the parameter list in labelDetails.detail and the
             * return type / include hint in labelDetails.description. */
            Json *ld = jget(it, "labelDetails");
            if (ld && ld->type == JOBJ) {
                Json *ltd = jget(ld, "detail");
                if (ltd && ltd->type == JSTR)
                    snprintf(nc.label_detail, sizeof(nc.label_detail), "%s", ltd->str);
                Json *ltdesc = jget(ld, "description");
                if (ltdesc && ltdesc->type == JSTR)
                    snprintf(nc.label_desc, sizeof(nc.label_desc), "%s", ltdesc->str);
            }
            nc.source = LSP_SRC_LSP;
            nc.snippet = snippet;

            trim_ws(nc.label);
            trim_ws(nc.detail);
            trim_ws(nc.label_detail);
            trim_ws(nc.label_desc);

            /* clangd's include completions carry the closing delimiter in the
             * label (e.g. "stdio.h>"); drop it so the menu shows just the path.
             * The delimiter stays in `insert`, which completes the directive. */
            if (nc.kind == LSP_CK_FILE) {
                size_t L = strlen(nc.label);
                if (L > 0 && (nc.label[L - 1] == '>' || nc.label[L - 1] == '"'))
                    nc.label[L - 1] = '\0';
            }

            comp_push(&c->completions, &nc);
        }
    }
    c->completions_ready = 1;
}

static void handle_body(LspClient *c, const char *body) {
    Json *root = jparse(body);
    if (!root) return;

    Json *method = jget(root, "method");
    if (method && method->type == JSTR) {
        if (!strcmp(method->str, "textDocument/publishDiagnostics"))
            handle_diagnostics(c, jget(root, "params"));
        /* window/logMessage, $/progress, ... are intentionally ignored. */
    } else {
        Json *id = jget(root, "id");
        int rid = (id && id->type == JNUM) ? (int)id->num : 0;
        if (!c->initialized && rid == 1) {
            c->initialized = 1;
            send_initialized(c);
            if (c->pending_path) {
                char uri[PATH_MAX + 16];
                char lang[16] = "c";
                lsp_uri_from_path(c->pending_path, uri, sizeof(uri));
                lsp_language_for(c->pending_path, lang, sizeof(lang));
                free(c->doc_uri);
                c->doc_uri = strdup(uri);
                snprintf(c->language_id, sizeof(c->language_id), "%s", lang);
                c->doc_version = 1;
                send_did_open(c, uri, lang, c->pending_text ? c->pending_text : "");
                free(c->pending_path); c->pending_path = NULL;
                free(c->pending_text); c->pending_text = NULL;
            }
        } else if (rid != 0 && rid == c->completion_id) {
            handle_completion(c, jget(root, "result"));
            c->completion_id = 0;
        }
    }
    jfree(root);
}

static char *find_crlfcrlf(const char *s, size_t n) {
    for (size_t i = 0; i + 4 <= n; i++)
        if (s[i] == '\r' && s[i + 1] == '\n' && s[i + 2] == '\r' && s[i + 3] == '\n')
            return (char *)(s + i);
    return NULL;
}

static long parse_content_length(const char *hdr, size_t len) {
    static const char key[] = "content-length:";
    size_t klen = sizeof(key) - 1;
    for (size_t i = 0; i + klen <= len; i++) {
        size_t k = 0;
        while (k < klen && tolower((unsigned char)hdr[i + k]) == key[k]) k++;
        if (k == klen) {
            const char *p = hdr + i + klen;
            const char *end = hdr + len;
            while (p < end && (*p == ' ' || *p == '\t')) p++;
            return strtol(p, NULL, 10);
        }
    }
    return -1;
}

static void lsp_process(LspClient *c) {
    for (;;) {
        char *he = find_crlfcrlf(c->recv, c->recv_len);
        if (!he) break;

        size_t header_len = (size_t)(he - c->recv) + 4;
        long clen = parse_content_length(c->recv, (size_t)(he - c->recv));
        if (clen < 0) {
            memmove(c->recv, c->recv + header_len, c->recv_len - header_len);
            c->recv_len -= header_len;
            continue;
        }
        if (c->recv_len < header_len + (size_t)clen) break;

        char *body = malloc((size_t)clen + 1);
        if (!body) break;
        memcpy(body, c->recv + header_len, (size_t)clen);
        body[clen] = '\0';

        size_t rest = c->recv_len - header_len - (size_t)clen;
        memmove(c->recv, c->recv + header_len + (size_t)clen, rest);
        c->recv_len = rest;

        handle_body(c, body);
        free(body);
    }
}

/* ------------------------------ public API ------------------------------ */

void lsp_init(LspClient *c) {
    memset(c, 0, sizeof(*c));
    c->in_fd = -1;
    c->out_fd = -1;
    c->pid = -1;
}

int lsp_start(LspClient *c, const char *root_dir) {
    int inpipe[2], outpipe[2];
    if (pipe(inpipe) != 0) return -1;
    if (pipe(outpipe) != 0) {
        close(inpipe[0]); close(inpipe[1]);
        return -1;
    }
    /* Close-on-exec pipe: the child's end disappears when execvp succeeds, so a
     * read on our end returning data means the exec itself failed. */
    int execpipe[2];
    if (pipe(execpipe) != 0) {
        close(inpipe[0]); close(inpipe[1]);
        close(outpipe[0]); close(outpipe[1]);
        return -1;
    }
    fcntl(execpipe[1], F_SETFD, FD_CLOEXEC);

    /* A server that dies mid-write must not take the editor down. */
    signal(SIGPIPE, SIG_IGN);

    pid_t pid = fork();
    if (pid < 0) {
        close(inpipe[0]); close(inpipe[1]);
        close(outpipe[0]); close(outpipe[1]);
        close(execpipe[0]); close(execpipe[1]);
        return -1;
    }

    if (pid == 0) {
        /* Child: clangd's stdin <- inpipe, stdout -> outpipe; quiet stderr. */
        dup2(inpipe[0], STDIN_FILENO);
        dup2(outpipe[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        close(inpipe[0]); close(inpipe[1]);
        close(outpipe[0]); close(outpipe[1]);
        close(execpipe[0]);

        /* Start clangd in the file's directory so it locates the project's
         * compile_commands.json. */
        if (root_dir && root_dir[0]) {
            if (chdir(root_dir) != 0) { /* non-fatal: run from the cwd */ }
        }

        const char *argv[] = {
            "clangd", "--background-index", "--header-insertion=never", NULL
        };
        execvp(argv[0], (char *const *)argv);
        int err = errno;
        ssize_t ignored = write(execpipe[1], &err, sizeof(err));
        (void)ignored;
        _exit(127);
    }

    close(inpipe[0]);
    close(outpipe[1]);
    close(execpipe[1]);

    int exec_errno = 0;
    ssize_t nr;
    do {
        nr = read(execpipe[0], &exec_errno, sizeof(exec_errno));
    } while (nr < 0 && errno == EINTR);
    close(execpipe[0]);

    if (nr > 0) {
        /* execvp failed: reap the child and report that LSP is unavailable. */
        close(inpipe[1]);
        close(outpipe[0]);
        int status;
        waitpid(pid, &status, 0);
        return -1;
    }

    c->out_fd = inpipe[1];
    c->in_fd = outpipe[0];
    c->pid = pid;
    c->running = 1;
    /* Request ids start above the fixed initialize(1)/shutdown(2) ids so a
     * completion response can never be mistaken for the handshake. */
    c->next_id = 100;
    c->doc_version = 0;

    int fl = fcntl(c->in_fd, F_GETFL, 0);
    if (fl >= 0) fcntl(c->in_fd, F_SETFL, fl | O_NONBLOCK);

    char root_uri[PATH_MAX + 16];
    root_uri[0] = '\0';
    if (root_dir && root_dir[0])
        lsp_uri_from_path(root_dir, root_uri, sizeof(root_uri));
    send_initialize(c, root_uri);
    return 0;
}

void lsp_poll(LspClient *c) {
    if (!c || !c->running || c->in_fd < 0) return;

    for (;;) {
        if (c->recv_cap - c->recv_len < 4096) {
            size_t ncap = c->recv_cap ? c->recv_cap * 2 : 8192;
            char *nb = realloc(c->recv, ncap);
            if (!nb) break;
            c->recv = nb;
            c->recv_cap = ncap;
        }
        ssize_t n = read(c->in_fd, c->recv + c->recv_len,
                         c->recv_cap - c->recv_len - 1);
        if (n > 0) {
            c->recv_len += (size_t)n;
            c->recv[c->recv_len] = '\0';
            continue;
        }
        if (n == 0) { c->running = 0; break; }
        if (errno == EINTR) continue;
        break; /* EAGAIN / other */
    }

    if (c->recv_len > 0) lsp_process(c);
}

int lsp_fd(const LspClient *c) {
    if (c && c->running && c->in_fd >= 0) return c->in_fd;
    return -1;
}

void lsp_did_open(LspClient *c, const char *path, const char *text) {
    if (!c || !c->running) return;
    if (!c->initialized) {
        free(c->pending_path);
        c->pending_path = strdup(path);
        free(c->pending_text);
        c->pending_text = strdup(text ? text : "");
        return;
    }

    char uri[PATH_MAX + 16];
    char lang[16] = "c";
    lsp_uri_from_path(path, uri, sizeof(uri));
    lsp_language_for(path, lang, sizeof(lang));

    free(c->doc_uri);
    c->doc_uri = strdup(uri);
    snprintf(c->language_id, sizeof(c->language_id), "%s", lang);
    c->doc_version = 1;
    send_did_open(c, uri, lang, text);
}

void lsp_did_change(LspClient *c, const char *path, const char *text) {
    if (!c || !c->running) return;

    if (!c->initialized || !c->doc_uri) {
        /* Not open yet: keep the freshest text for the queued didOpen. */
        free(c->pending_text);
        c->pending_text = strdup(text ? text : "");
        if (!c->pending_path) c->pending_path = strdup(path);
        return;
    }

    c->doc_version++;
    send_did_change(c, c->doc_uri, text);
}

int lsp_request_completion(LspClient *c, int line, int character) {
    if (!c || !c->running || !c->initialized || !c->doc_uri) return 0;

    int id = c->next_id++;
    StrBuf sb = {0};
    sb_printf(&sb, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"textDocument/completion\",\"params\":{", id);
    sb_puts(&sb, "\"textDocument\":{\"uri\":");
    sb_puts_json(&sb, c->doc_uri);
    sb_printf(&sb, "},\"position\":{\"line\":%d,\"character\":%d}}", line, character);
    sb_puts(&sb, "}");
    char *body = sb_take(&sb);
    int rc = lsp_send_raw(c, body);
    free(body);
    if (rc != 0) return 0;

    c->completion_id = id;
    c->completions_ready = 0;
    return 1;
}

void lsp_stop(LspClient *c) {
    if (!c) return;

    if (c->running) {
        lsp_send_raw(c, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"shutdown\"}");
        lsp_send_raw(c, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");
    }
    if (c->out_fd >= 0) close(c->out_fd);
    c->out_fd = -1;
    if (c->in_fd >= 0) close(c->in_fd);
    c->in_fd = -1;

    /* Reap the child whether it is still running or already exited on its own. */
    if (c->pid > 0) {
        int status = 0;
        int reaped = 0;
        for (int i = 0; i < 20; i++) {
            pid_t r = waitpid(c->pid, &status, WNOHANG);
            if (r == c->pid || r < 0) { reaped = 1; break; }
            struct timespec ts = {0, 10000000}; /* 10 ms */
            nanosleep(&ts, NULL);
        }
        if (!reaped) {
            kill(c->pid, SIGTERM);
            waitpid(c->pid, &status, 0);
        }
        c->pid = -1;
    }
    c->running = 0;

    free(c->doc_uri); c->doc_uri = NULL;
    free(c->pending_path); c->pending_path = NULL;
    free(c->pending_text); c->pending_text = NULL;
    free(c->recv); c->recv = NULL;
    c->recv_len = c->recv_cap = 0;
    lsp_diags_free(&c->diags);
    lsp_completions_free(&c->completions);
    c->completion_id = 0;
    c->completions_ready = 0;
}
