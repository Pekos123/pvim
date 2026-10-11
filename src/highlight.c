#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../include/config.h"
#include "../include/highlight.h"

/* ---------------------------------------------------------------------------
 * Configuration format ("*.syntax", one directive per line):
 *
 *   # comment line
 *   filetype = c                 name shown in the status bar (required)
 *   ext = .c .h .cpp .hpp        filename extensions (repeatable, list)
 *   keywords = if for return     control-flow keywords (repeatable, list)
 *   types = int char void        type keywords -> coloured like a type
 *   single_comment = //          single-line comment marker
 *   multi_comment_start / multi_comment_end = the block-comment delimiters
 *                             (see syntax/c.syntax for a concrete example)
 *   shebang = bash sh            `#!` line substrings (repeatable, list)
 *   flags = numbers strings      which literals to colour (repeatable, list)
 *
 * `types` are stored as regular keywords with a trailing '|'; the scanner's
 * existing keyword-separator logic treats a '|' suffix as a "type" keyword, so
 * the rest of the syntax engine is unchanged. Lists are whitespace/comma
 * separated; a token starting with '#' ends a list entry run (inline comment).
 * ------------------------------------------------------------------------ */

#define HL_HIGHLIGHT_NUMBERS (1 << 0)
#define HL_HIGHLIGHT_STRINGS (1 << 1)

/* One parsed syntax rule file. This structure is the interface the rest of the
 * engine relies on; the loader fills one instance per `*.syntax` file and every
 * string / string-array it points to is heap-allocated. */
typedef struct {
    const char *filetype;   /* name shown in the status bar             */
    const char **ext;       /* filename extensions, NULL-terminated     */
    const char **keywords;  /* keywords + types, NULL-terminated        */
    const char **shebangs;  /* `#!` line substrings, NULL-terminated    */
    const char *scs;        /* single-line comment start                */
    const char *mcs, *mce;  /* multi-line comment start / end           */
    int flags;
} HLDBEntry;

static HLDBEntry **g_db = NULL;
static int g_db_count = 0;
static int g_loaded = 0;
static HLDBEntry *syntax = NULL;

/* ---------------------------------------------------------------------------
 * Colour themes. Each highlight role maps to an XTerm-256 palette index, or -1
 * for the terminal's default foreground. The built-in values below are the
 * fallback; a theme file may override any subset of them:
 *
 *   # themes/<name>.theme
 *   comment   = 245
 *   keyword1  = orange
 *   string    = pink
 *
 * A value is a palette index (0-255), a colour name (red, green, orange, ...),
 * or `default` / `none` / -1 for the terminal default. The role names accept a
 * few aliases (keyword = keyword1, type = types = keyword2, mlcomment, ...).
 *
 * A plain config file `pvim.conf` selects the theme and may override colours:
 *   theme = default
 *   string = pink          # wins over the theme
 * ------------------------------------------------------------------------ */

#define HL_ROLE_COUNT (HL_MENU_BORDER + 1)

/* Fallback palette, copied into g_theme before each theme load so `:reload`
 * can undo a theme's overrides instead of layering a new theme on top of the
 * previous one. */
static const int g_theme_defaults[HL_ROLE_COUNT] = {
    [HL_NORMAL]     = -1,    /* default      */
    [HL_COMMENT]    = 245,   /* grey         */
    [HL_MLCOMMENT]  = 245,   /* grey         */
    [HL_KEYWORD1]   = 214,   /* orange       */
    [HL_KEYWORD2]   = 40,    /* green        */
    [HL_STRING]     = 204,   /* pink         */
    [HL_NUMBER]     = 203,   /* red-orange   */
    [HL_MACRO]      = 33,    /* blue         */
    [HL_VARIABLE]   = 81,    /* steel blue   */
    [HL_FUNCTION]   = 228,   /* pale yellow  */
    [HL_MATCH]      = 227,   /* search: light yellow block        */
    [HL_SELECTION]  = -1,    /* selection: -1 -> reuse syntax colour */
    [HL_ERROR]      = 196,   /* red          */
    [HL_WARNING]    = 226,   /* yellow       */
    [HL_INFO]       = 39,    /* blue         */
    [HL_HINT]       = 245,   /* grey         */
    [HL_MENU]        = 231,  /* completion menu: bright white label   */
    [HL_MENU_DETAIL] = 245,  /* completion menu: muted signature      */
    [HL_MENU_SOURCE] = 45,   /* completion menu: cyan badge / tag     */
    [HL_MENU_SELECT] = 237,  /* completion menu: selected row block   */
    [HL_MENU_BG]     = 236,  /* completion menu: popup background     */
    [HL_MENU_BORDER] = 45    /* completion menu: popup border         */
};

static int g_theme[HL_ROLE_COUNT];

/* Brightness (bold) for the two reverse-video blocks. */
static int g_selection_bold = 0;
static int g_search_bold = 0;

static char g_theme_name[64] = "default";

/* --------------------------- small helpers ------------------------------ */

static const char **arr_push(const char ***arr, int *count, int *cap, char *s) {
    if (*count + 1 >= *cap) {
        int ncap = *cap ? *cap * 2 : 8;
        const char **na = realloc((char **)*arr, sizeof(char *) * (size_t)ncap);
        if (!na) { free(s); return NULL; }
        *arr = na;
        *cap = ncap;
    }
    (*arr)[(*count)++] = s;
    (*arr)[*count] = NULL;   /* keep a NULL sentinel */
    return *arr;
}

static void arr_free(const char **arr) {
    if (!arr) return;
    for (int i = 0; arr[i]; i++) free((char *)arr[i]);
    free((char **)arr);
}

static char *trim_ws(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = '\0';
    return s;
}

static char *aprintf(const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return NULL; }
    char *s = malloc((size_t)n + 1);
    if (s) vsnprintf(s, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return s;
}

static HLDBEntry *entry_new(void) {
    return calloc(1, sizeof(HLDBEntry));
}

static void entry_free(HLDBEntry *e) {
    if (!e) return;
    free((char *)e->filetype);
    arr_free(e->ext);
    arr_free(e->keywords);
    arr_free(e->shebangs);
    free((char *)e->scs);
    free((char *)e->mcs);
    free((char *)e->mce);
    free(e);
}

void highlight_free_syntax(void) {
    for (int i = 0; i < g_db_count; i++) entry_free(g_db[i]);
    free(g_db);
    g_db = NULL;
    g_db_count = 0;
    g_loaded = 0;
    syntax = NULL;
    pvim_config_free();
}

/* Insert `e`, replacing any earlier entry with the same filetype so user files
 * can override the bundled ones. */
static void db_upsert(HLDBEntry *e) {
    for (int i = 0; i < g_db_count; i++) {
        if (strcmp(g_db[i]->filetype, e->filetype) == 0) {
            if (syntax == g_db[i]) syntax = e;
            entry_free(g_db[i]);
            g_db[i] = e;
            return;
        }
    }
    HLDBEntry **ndb = realloc(g_db, sizeof(HLDBEntry *) * (size_t)(g_db_count + 1));
    if (!ndb) { entry_free(e); return; }
    g_db = ndb;
    g_db[g_db_count++] = e;
}

static void set_scalar(const char **dst, const char *value) {
    free((char *)*dst);
    *dst = strdup(value);
}

/* Split `value` on whitespace/commas, appending one token (a type keyword if
 * `with_pipe`, suffixed with '|') per token. */
static void parse_list(const char *value, int with_pipe,
                       const char ***arr, int *count, int *cap) {
    char *copy = strdup(value);
    if (!copy) return;
    char *save = NULL;
    for (char *tok = strtok_r(copy, " \t\r\n,", &save); tok;
         tok = strtok_r(NULL, " \t\r\n,", &save)) {
        if (tok[0] == '#') break;   /* inline comment */
        size_t n = strlen(tok);
        char *s;
        if (with_pipe) {
            s = malloc(n + 2);
            if (s) {
                memcpy(s, tok, n);
                s[n] = '|';
                s[n + 1] = '\0';
            }
        } else {
            s = strdup(tok);
        }
        if (s) arr_push(arr, count, cap, s);
    }
    free(copy);
}

typedef struct {
    int ecount, ecap;
    const char **ext;
    int kcount, kcap;
    const char **keywords;
    int scount, scap;
    const char **shebangs;
} ParseCtx;

static void apply_field(HLDBEntry *e, ParseCtx *ctx, char *line) {
    char *eq = strchr(line, '=');
    if (!eq) return;
    *eq = '\0';
    char *key = trim_ws(line);
    char *value = trim_ws(eq + 1);
    if (!*key) return;

    if (!strcmp(key, "filetype") || !strcmp(key, "name")) {
        set_scalar(&e->filetype, value);
    } else if (!strcmp(key, "ext")) {
        parse_list(value, 0, &ctx->ext, &ctx->ecount, &ctx->ecap);
    } else if (!strcmp(key, "shebang") || !strcmp(key, "shebangs")) {
        parse_list(value, 0, &ctx->shebangs, &ctx->scount, &ctx->scap);
    } else if (!strcmp(key, "keywords")) {
        parse_list(value, 0, &ctx->keywords, &ctx->kcount, &ctx->kcap);
    } else if (!strcmp(key, "types")) {
        parse_list(value, 1, &ctx->keywords, &ctx->kcount, &ctx->kcap);
    } else if (!strcmp(key, "single_comment")) {
        set_scalar(&e->scs, value);
    } else if (!strcmp(key, "multi_comment_start")) {
        set_scalar(&e->mcs, value);
    } else if (!strcmp(key, "multi_comment_end")) {
        set_scalar(&e->mce, value);
    } else if (!strcmp(key, "flags")) {
        char *copy = strdup(value);
        if (!copy) return;
        char *save = NULL;
        for (char *tok = strtok_r(copy, " \t\r\n,", &save); tok;
             tok = strtok_r(NULL, " \t\r\n,", &save)) {
            if (!strcmp(tok, "numbers")) e->flags |= HL_HIGHLIGHT_NUMBERS;
            else if (!strcmp(tok, "strings")) e->flags |= HL_HIGHLIGHT_STRINGS;
        }
        free(copy);
    }
}

static HLDBEntry *load_one_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    HLDBEntry *e = entry_new();
    if (!e) { fclose(f); return NULL; }
    ParseCtx ctx = {0};
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        char *l = trim_ws(line);
        if (!*l || *l == '#') continue;   /* blank or comment */
        apply_field(e, &ctx, l);
    }
    fclose(f);
    if (!e->filetype) {   /* a rule file needs a name to be useful */
        arr_free(ctx.ext);
        arr_free(ctx.keywords);
        arr_free(ctx.shebangs);
        entry_free(e);
        return NULL;
    }
    e->ext = ctx.ext;
    e->keywords = ctx.keywords;
    e->shebangs = ctx.shebangs;
    return e;
}

static void load_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        size_t n = strlen(name);
        if (n < 7 || strcmp(name + n - 7, ".syntax") != 0) continue;
        char path[PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path))
            continue;
        HLDBEntry *e = load_one_file(path);
        if (e) db_upsert(e);
    }
    closedir(d);
}

/* ------------------------------ theme loader ---------------------------- */

typedef struct { const char *name; int role; } HlRoleName;

static const HlRoleName g_role_names[] = {
    {"normal",    HL_NORMAL},
    {"comment",   HL_COMMENT},
    {"mlcomment", HL_MLCOMMENT},
    {"keyword",   HL_KEYWORD1},
    {"keyword1",  HL_KEYWORD1},
    {"type",      HL_KEYWORD2},
    {"types",     HL_KEYWORD2},
    {"keyword2",  HL_KEYWORD2},
    {"string",    HL_STRING},
    {"number",    HL_NUMBER},
    {"macro",     HL_MACRO},
    {"variable",  HL_VARIABLE},
    {"function",  HL_FUNCTION},
    {"match",     HL_MATCH},
    {"search",    HL_MATCH},
    {"selection", HL_SELECTION},
    {"select",    HL_SELECTION},
    {"visual",    HL_SELECTION},
    {"error",     HL_ERROR},
    {"err",       HL_ERROR},
    {"warning",   HL_WARNING},
    {"warn",      HL_WARNING},
    {"info",      HL_INFO},
    {"hint",      HL_HINT},
    {"completion_menu",        HL_MENU},
    {"completion_menu_label",  HL_MENU},
    {"completion_menu_detail", HL_MENU_DETAIL},
    {"completion_menu_source", HL_MENU_SOURCE},
    {"completion_menu_select", HL_MENU_SELECT},
    {"completion_menu_bg",     HL_MENU_BG},
    {"completion_menu_border", HL_MENU_BORDER},
    {"menu",        HL_MENU},
    {"menu_detail", HL_MENU_DETAIL},
    {"menu_source", HL_MENU_SOURCE},
    {"menu_select", HL_MENU_SELECT},
    {"menu_bg",     HL_MENU_BG},
    {"menu_border", HL_MENU_BORDER},
    {NULL, -1}
};

static int parse_color(const char *s) {
    if (!s || !*s) return -2;
    if (!strcmp(s, "default") || !strcmp(s, "none") ||
        !strcmp(s, "normal")  || !strcmp(s, "-1"))
        return -1;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end != s && *end == '\0' && v >= 0 && v <= 255) return (int)v;

    static const struct { const char *n; int c; } named[] = {
        {"black", 0}, {"red", 1}, {"green", 2}, {"yellow", 3},
        {"blue", 4}, {"magenta", 5}, {"cyan", 6}, {"white", 7},
        {"gray", 8}, {"grey", 8}, {"brightred", 9}, {"brightgreen", 10},
        {"brightyellow", 11}, {"brightblue", 12}, {"brightmagenta", 13},
        {"brightcyan", 14}, {"brightwhite", 15}, {"orange", 214},
        {"pink", 204}, {"purple", 135}, {"teal", 30}, {"lime", 118},
        {"olive", 142}, {"navy", 17}, {"gold", 220}, {NULL, 0}
    };
    for (int i = 0; named[i].n; i++)
        if (!strcmp(s, named[i].n)) return named[i].c;
    return -2;   /* unrecognised: leave the current value alone */
}

static int role_for_name(const char *name) {
    for (int i = 0; g_role_names[i].name; i++)
        if (!strcmp(name, g_role_names[i].name)) return g_role_names[i].role;
    return -1;
}

/* Parse on/off style values. Returns 0/1, or -1 when unrecognised. */
static int parse_bool(const char *s) {
    if (!s || !*s) return -1;
    if (!strcmp(s, "on")  || !strcmp(s, "true")  || !strcmp(s, "yes") ||
        !strcmp(s, "1")   || !strcmp(s, "bold")  || !strcmp(s, "bright"))
        return 1;
    if (!strcmp(s, "off") || !strcmp(s, "false") || !strcmp(s, "no") ||
        !strcmp(s, "0")   || !strcmp(s, "none")  || !strcmp(s, "normal"))
        return 0;
    return -1;
}

/* Apply one `key = value` line to the colour table; unknown keys and values
 * are ignored, so a theme and pvim.conf can share this parser. */
static void apply_color_line(const char *key, const char *value) {
    /* Brightness (bold) toggles for the two reverse-video blocks. */
    if (!strcmp(key, "selection_bold") || !strcmp(key, "select_bold") ||
        !strcmp(key, "visual_bold")) {
        int b = parse_bool(value);
        if (b >= 0) g_selection_bold = b;
        return;
    }
    if (!strcmp(key, "search_bold") || !strcmp(key, "match_bold")) {
        int b = parse_bool(value);
        if (b >= 0) g_search_bold = b;
        return;
    }

    int role = role_for_name(key);
    if (role < 0) return;
    int c = parse_color(value);
    if (c != -2) g_theme[role] = c;
}

/* Parse `path` as `key = value` lines, applying every recognised colour.
 * Inline `# ...` comments are allowed on a line. */
static int load_theme_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *l = trim_ws(line);
        if (!*l || *l == '#') continue;
        char *eq = strchr(l, '=');
        if (!eq) continue;
        *eq = '\0';
        char *val = trim_ws(eq + 1);
        char *hash = strchr(val, '#');
        if (hash) *hash = '\0';
        apply_color_line(trim_ws(l), trim_ws(val));
    }
    fclose(f);
    return 1;
}

/* Load `themes/<name>.theme`, preferring user files over bundled ones. */
static int load_theme_by_name(const char *name) {
    const char *dirs[6];
    int n = 0;

    const char *env = getenv("PVIM_THEME_DIR");
    if (env && *env) dirs[n++] = env;

    char *alloc[3] = {NULL, NULL, NULL};
    int a = 0;
    char *ud = pvim_user_config_dir();
    if (ud) {
        alloc[a] = aprintf("%s/themes", ud);
        free(ud);
        if (alloc[a]) dirs[n++] = alloc[a++];
    }

    const char *ed = pvim_exe_dir();
    const char *pd = pvim_exe_parent_dir();
    if (ed[0]) {
        alloc[a] = aprintf("%s/themes", ed);
        if (alloc[a]) dirs[n++] = alloc[a++];
    }
    if (pd[0]) {
        alloc[a] = aprintf("%s/themes", pd);
        if (alloc[a]) dirs[n++] = alloc[a++];
    }
    dirs[n++] = "./themes";

    int found = 0;
    for (int i = 0; i < n && !found; i++) {
        char *path = aprintf("%s/%s.theme", dirs[i], name);
        if (path) {
            found = load_theme_file(path);
            free(path);
        }
    }
    for (int i = 0; i < a; i++) free(alloc[i]);
    return found;
}

void highlight_load_theme(void) {
    /* Start from the built-in palette so a reloaded config that no longer sets
     * a role falls back to the default rather than the previous theme. */
    memcpy(g_theme, g_theme_defaults, sizeof(g_theme));
    g_selection_bold = 0;
    g_search_bold = 0;

    const char *env = getenv("PVIM_THEME");
    char *cfg = NULL;
    const char *name = "default";
    if (env && *env) name = env;
    else if ((cfg = pvim_config_get("theme")) != NULL) name = cfg;

    int found = load_theme_by_name(name);
    snprintf(g_theme_name, sizeof(g_theme_name), "%s", found ? name : "default");

    /* Colour overrides in pvim.conf win over the theme file. */
    const char *path = pvim_config_path();
    if (path) load_theme_file(path);

    free(cfg);
}

/* --------------------------- syntax directory --------------------------- */

static void load_syntax_dirs(void) {
    load_dir("./syntax");   /* bundled defaults (cwd)              */

    char *dirs[3];
    int n = 0;
    {
        const char *ed = pvim_exe_dir();
        const char *pd = pvim_exe_parent_dir();
        if (ed[0]) dirs[n++] = aprintf("%s/syntax", ed);
        if (pd[0]) dirs[n++] = aprintf("%s/syntax", pd);
    }
    {
        char *ud = pvim_user_config_dir();
        if (ud) {
            dirs[n] = aprintf("%s/syntax", ud);
            free(ud);
            if (dirs[n]) n++;
        }
    }
    for (int i = 0; i < n; i++) {
        load_dir(dirs[i]);
        free(dirs[i]);
    }
    const char *env = getenv("PVIM_SYNTAX_DIR");
    if (env && *env) load_dir(env);   /* highest priority */
}

int highlight_load_syntax(void) {
    if (g_loaded) return g_db_count > 0;
    g_loaded = 1;

    load_syntax_dirs();
    highlight_load_theme();
    atexit(highlight_free_syntax);
    return g_db_count > 0;
}

/* Re-read syntax rules, the theme and pvim.conf from disk. Backs the `:reload`
 * command so edits take effect without restarting the editor. The free routine
 * also drops the cached config path, and it is already registered with atexit,
 * so it is not re-registered here. */
void highlight_reload(void) {
    highlight_free_syntax();   /* frees the DB + config cache, clears g_loaded */
    load_syntax_dirs();
    highlight_load_theme();
    g_loaded = 1;
}

/* ------------------------------ rendering ------------------------------- */

int highlight_to_color(unsigned char hl) {
    if (hl >= HL_ROLE_COUNT) return -1;
    return g_theme[hl];
}

int highlight_diag_color(int severity) {
    switch (severity) {
        case 1: return highlight_to_color(HL_ERROR);
        case 2: return highlight_to_color(HL_WARNING);
        case 3: return highlight_to_color(HL_INFO);
        default: return highlight_to_color(HL_HINT);
    }
}

int highlight_selection_bold(void) { return g_selection_bold; }
int highlight_search_bold(void) { return g_search_bold; }

const char *highlight_syntax_name(void) {
    return syntax ? syntax->filetype : "none";
}

const char *highlight_theme_name(void) {
    return g_theme_name;
}

/* ------------------------------- scanner -------------------------------- */

static int is_separator(int c) {
    return c == ' ' || c == '\0' || c == ',' || c == '(' || c == ')' ||
           c == '[' || c == ']' || c == '{' || c == '}' || c == ';' ||
           c == ':' || c == '"' || c == '\'' || c == '.' || c == '/' ||
           c == '=' || c == '+' || c == '-' || c == '*' || c == '&' ||
           c == '|' || c == '<' || c == '>' || c == '!' || c == '?';
}

static int is_ident_char(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* Keywords after which the next identifier names a type (struct/union/enum/
 * class/typedef), so it can get the type colour. */
static int is_type_keyword(const char *kw) {
    return !strcmp(kw, "struct") || !strcmp(kw, "union") ||
           !strcmp(kw, "enum") || !strcmp(kw, "class") ||
           !strcmp(kw, "typedef");
}

/* Scan one row in isolation. `in_comment` is the multi-line-comment state
 * inherited from the previous row; the resulting end-of-row state is stored in
 * `row->hl_open_comment` so the next row can pick it up without touching the
 * whole file. */
static void highlight_scan_row(erow *row, int in_comment) {
    row->hl = realloc(row->hl, row->size > 0 ? (size_t)row->size : 1);
    memset(row->hl, HL_NORMAL, row->size > 0 ? (size_t)row->size : 1);

    if (syntax == NULL || row->size == 0) {
        row->hl_open_comment = in_comment;
        return;
    }

    const char **keywords = syntax->keywords;
    const char *scs = syntax->scs;
    const char *mcs = syntax->mcs;
    const char *mce = syntax->mce;
    int scs_len = scs ? (int)strlen(scs) : 0;
    int mcs_len = mcs ? (int)strlen(mcs) : 0;
    int mce_len = mce ? (int)strlen(mce) : 0;

    int prev_sep = 1;        /* pretend the row starts on a separator */
    int in_string = 0;
    int at_line_start = 1;   /* only the first token may start a #-directive */
    int expect_type = 0;     /* next plain identifier is a type name */
    int i = 0;

    while (i < row->size) {
        char c = row->chars[i];

        if (at_line_start && (c == ' ' || c == '\t')) {
            i++;
            continue;
        }

        /* Whole-line preprocessor directive -> macro colour. Coloring the
         * entire directive line (not just `#include`) also makes quoted
         * "local.h" and angle <global.h> headers render identically. */
        if (at_line_start && c == '#') {
            memset(&row->hl[i], HL_MACRO, (size_t)(row->size - i));
            break;
        }
        at_line_start = 0;

        unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : (unsigned char)HL_NORMAL;

        if (scs_len && !in_string && !in_comment) {
            if (!strncmp(&row->chars[i], scs, (size_t)scs_len)) {
                memset(&row->hl[i], HL_COMMENT, (size_t)(row->size - i));
                break;
            }
        }

        if (mcs_len && mce_len && !in_string) {
            if (in_comment) {
                row->hl[i] = HL_MLCOMMENT;
                if (!strncmp(&row->chars[i], mce, (size_t)mce_len)) {
                    memset(&row->hl[i], HL_MLCOMMENT, (size_t)mce_len);
                    i += mce_len;
                    in_comment = 0;
                    prev_sep = 1;
                    continue;
                } else {
                    i++;
                    continue;
                }
            } else if (!strncmp(&row->chars[i], mcs, (size_t)mcs_len)) {
                memset(&row->hl[i], HL_MLCOMMENT, (size_t)mcs_len);
                i += mcs_len;
                in_comment = 1;
                continue;
            }
        }

        if (syntax->flags & HL_HIGHLIGHT_STRINGS) {
            if (in_string) {
                row->hl[i] = HL_STRING;
                if (c == '\\' && i + 1 < row->size) {
                    row->hl[i + 1] = HL_STRING;
                    i += 2;
                    continue;
                }
                if (c == in_string) in_string = 0;
                i++;
                prev_sep = 1;
                continue;
            } else if (c == '"' || c == '\'') {
                in_string = c;
                row->hl[i] = HL_STRING;
                i++;
                continue;
            }
        }

        if (syntax->flags & HL_HIGHLIGHT_NUMBERS) {
            if ((isdigit((unsigned char)c) && (prev_sep || prev_hl == HL_NUMBER)) ||
                (c == '.' && prev_hl == HL_NUMBER)) {
                row->hl[i] = HL_NUMBER;
                i++;
                prev_sep = 0;
                continue;
            }
        }

        /* Identifiers: keywords, macros, types, functions, variables. */
        if (is_ident_char(c)) {
            int j = i;
            int has_upper = 0, has_lower = 0;
            while (j < row->size && is_ident_char(row->chars[j])) {
                char ch = row->chars[j];
                if (ch >= 'A' && ch <= 'Z') has_upper = 1;
                else if (ch >= 'a' && ch <= 'z') has_lower = 1;
                j++;
            }
            int klen = j - i;
            int sep_after = (j >= row->size) || is_separator(row->chars[j]);

            const char *kw = NULL;
            int matched_kw = 0, kw2 = 0;
            if (prev_sep && keywords) {
                for (int k = 0; keywords[k]; k++) {
                    int kl = (int)strlen(keywords[k]);
                    int two = (keywords[k][kl - 1] == '|');
                    if (two) kl--;
                    if (kl == klen && sep_after &&
                        !strncmp(&row->chars[i], keywords[k], (size_t)klen)) {
                        matched_kw = 1;
                        kw2 = two;
                        kw = keywords[k];
                        break;
                    }
                }
            }

            if (matched_kw) {
                memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, (size_t)klen);
                if (!kw2 && is_type_keyword(kw)) expect_type = 1;
            } else if (has_upper && !has_lower) {
                /* ALL_CAPS identifier -> macro */
                memset(&row->hl[i], HL_MACRO, (size_t)klen);
            } else if (expect_type) {
                /* name right after struct/union/enum/class/typedef */
                memset(&row->hl[i], HL_KEYWORD2, (size_t)klen);
                expect_type = 0;
            } else if (j < row->size && row->chars[j] == '(') {
                /* identifier immediately followed by a paren -> function */
                memset(&row->hl[i], HL_FUNCTION, (size_t)klen);
            } else {
                memset(&row->hl[i], HL_VARIABLE, (size_t)klen);
            }
            i += klen;
            prev_sep = sep_after;
            continue;
        }

        /* statement-y punctuation drops the pending "expect a type" state */
        if (c == ';' || c == '{' || c == '}' || c == '(' || c == ')' || c == '=')
            expect_type = 0;

        prev_sep = is_separator(c);
        i++;
    }

    row->hl_open_comment = in_comment;
}

void highlight_update_buffer(EditorBuffer *buf, int start) {
    if (!buf || start < 0) return;
    for (int i = start; i < buf->num_rows; i++) {
        erow *row = &buf->rows[i];
        int prev_state = row->hl_open_comment;
        int in_comment = (i > 0) ? buf->rows[i - 1].hl_open_comment : 0;
        highlight_scan_row(row, in_comment);
        /* If this row's comment state did not change, no later row is
         * affected, so the scan can stop early. */
        if (row->hl_open_comment == prev_state)
            break;
    }
}

void highlight_rescan_buffer(EditorBuffer *buf, int start) {
    if (!buf || start < 0) return;
    for (int i = start; i < buf->num_rows; i++) {
        int in_comment = (i > 0) ? buf->rows[i - 1].hl_open_comment : 0;
        highlight_scan_row(&buf->rows[i], in_comment);
    }
}

void highlight_select_for_buffer(EditorBuffer *buf) {
    highlight_load_syntax();
    syntax = NULL;
    if (!buf) return;

    if (buf->filename) {
        size_t flen = strlen(buf->filename);
        for (int j = 0; j < g_db_count; j++) {
            HLDBEntry *s = g_db[j];
            if (!s->ext) continue;
            for (const char **e = s->ext; *e; e++) {
                size_t elen = strlen(*e);
                if (flen >= elen && !strcmp(buf->filename + flen - elen, *e)) {
                    syntax = s;
                    goto done;
                }
            }
        }
    }

    /* Shebang detection for scripts without a matching extension. */
    if (buf->num_rows > 0 && buf->rows[0].size >= 2 &&
        buf->rows[0].chars[0] == '#' && buf->rows[0].chars[1] == '!') {
        const char *line = buf->rows[0].chars;
        for (int j = 0; j < g_db_count; j++) {
            HLDBEntry *s = g_db[j];
            if (!s->shebangs) continue;
            for (const char **sh = s->shebangs; *sh; sh++) {
                if (strstr(line, *sh)) {
                    syntax = s;
                    goto done;
                }
            }
        }
    }

done:
    /* Apply the chosen scheme (or clear it) across every row. */
    highlight_rescan_buffer(buf, 0);
}