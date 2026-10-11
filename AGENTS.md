# AGENTS.md

## Summary of Completed Work - pvim Diagnostics Rework

1. **Per-severity highlight colors:**
   - Added `HL_ERROR`, `HL_WARNING`, `HL_INFO`, `HL_HINT` roles to the highlight system.
   - Mapped these to default colors (196/226/39/245) in `highlight.c`.
   - Provided `highlight_diag_color(severity)` (defined in `highlight.c` only) to map
     LSP severities to theme roles.

2. **Inline end-of-line diagnostics:**
   - The message for a diagnostic is appended at the **end of the same code line**
     by `display_append_row` in `src/display.c`, after one column of padding.
   - It is coloured by severity via `ab_set_attrs` + `highlight_diag_color`, and
     written after the row's characters, so it is decoration only: it is never
     part of the buffer and cannot be edited or have the cursor placed in it.
   - It is cropped with `ab_puts_crop` so the whole row still fits the text area
     (`screen_cols - code_width - 1`); when there is no room (a very long line),
     the message is skipped rather than allowed to wrap and corrupt the frame.
   - The gutter shows the severity letter (`E`/`W`/`I`/`H`) in the severity colour,
     and the status bar shows counts only (`E1 W0`), never the message text.

3. **Editor stability:**
   - `editor_scroll` remains line-based (no virtual rows); the cursor is always on
     a real buffer line.
   - The old floating "toast" (`editor_refresh_diag_toast`, drawn on the row above
     the error) and its `utf8_seq_len` / `utf8_cols` helpers were removed.
   - Control bytes in diagnostics/status text are sanitized (`sanitize_ctl_line`,
     `ab_puts_c` / `ab_puts_crop`) so untrusted server text can never move the
     terminal cursor mid-frame.
   - The frame uses the **full** terminal height: `editor_update_window_size`
     sets `screen_rows = ws_row - 1` (the bottom row is the status line) and
     `display_refresh_screen` does **not** emit a newline after the status line.
     Reserving a second row (`-= 2`) left a permanently blank row below the
     status bar, and a trailing `\r\n` on the bottom row would scroll the screen
     by one; both showed up as "a line below the status bar" after a resize.
   - `SIGWINCH` is installed with `sigaction` and **no** `SA_RESTART`, so a
     resize interrupts `poll()` (returns `EINTR`, the loop `continue`s and
     repaints immediately). With `signal()`'s BSD semantics `poll()` is restarted
     and the resize is only noticed on the next keystroke, leaving a stale,
     mis-sized frame on screen. `src/main.c` defines `_POSIX_C_SOURCE` for this.

## Build & install
- `make` -> `bin/pvim` (+ `compile_commands.json`). The target rule runs
  `mkdir -p bin`, so a fresh clone builds even though `/bin/` is gitignored.
- `./build.sh [--clean] [--force] [--bindir DIR] [--no-config] [--no-bin]`
  wraps `make`, installs the bundled `pvim.conf`, `themes/*.theme` and
  `syntax/*.syntax` into `${XDG_CONFIG_HOME:-~/.config}/pvim` (non-destructive:
  existing files are skipped unless `--force`), then symlinks `bin/pvim` into
  `$PVIM_BIN_DIR` (default `/usr/local/bin`) so `pvim` runs from any directory.
  `sudo` is used only for a non-writable target directory, and only when the
  link actually needs changing; a rebuild is picked up automatically through
  the symlink.
- The no-file scratch buffer in `src/main.c` inserts its starter line and then
  resets `buffer.dirty = 0`, so `:q` on a freshly opened scratch quits without
  needing `:q!` (the starter text is not an unsaved edit).
- `.gitignore` drops the build outputs (`/bin/`, `*.o`, `*.o.tmp`, `*.d`,
  `compile_commands.json`), the clangd `/.cache/` and the local `/.opencode/`.
- `.github/workflows/release.yml` runs on a `v*` tag: `make`, then a static link
  (`clang -static src/*.o -o pvim-linux-x86_64`) for a self-contained 64-bit
  x86_64 binary, uploaded as a workflow artifact and attached to a GitHub
  Release by `softprops/action-gh-release` (needs `contents: write`).
- `install.sh` (POSIX sh) downloads the latest `pvim-linux-x86_64` release
  asset with curl or wget, sanity-checks it is a 64-bit ELF, makes it executable
  (`chmod 755`) and moves it to `/usr/local/bin/pvim` (sudo only when the
  directory is not writable), then reports whether `/usr/local/bin` is on
  `PATH`. It backs the README "Quick Install (Linux)" section. Overridable:
  `PVIM_REPO`, `PVIM_VERSION`, `PVIM_BINDIR`, `PVIM_URL`.

## Commands
- `:diag` (also `:diagnostics`, `:diags`, `:problems`, `:errors`) opens a floating
  panel listing every diagnostic in the file (`Error`/`Warning`/`Info`/`Hint`,
  line number, message).
- `:reload` (also `:reloadconfig`, `:source`) re-reads `pvim.conf` at runtime:
  it calls `highlight_reload()` (syntax rules + theme + config cache) followed
  by `editor_apply_config()` (linenumbers, clipboard_mod, completion settings,
  `[bindings]`) and
  `highlight_select_for_buffer()`. Backed by `editor_run_cmd`, so it works both
  typed and as a map target.
- `:q` is guarded by `buffer.dirty`: with unsaved changes it refuses, showing
  "No write since last change (add ! to override)". `:q!` overrides, and
  `wq`/`x` only quit after a successful `buffer_save` (a failed write keeps the
  buffer and does not quit). `w` never quits.

## Vim-flavoured yank / paste
- Normal mode `yy` yanks the current line (newline included) into the internal
  register via `editor_yank_row` (shared with the line delete operators); `p`
  pastes it below the cursor.
- Visual mode `p` pastes the register over the selection (deletes the selection
  first, then inserts), and `clipboard_mod+p` does the same from the system
  clipboard. Both go through `editor_paste` / `editor_clipboard_paste`.
- `dd` yanks + deletes (unchanged); the register round-trips whole lines.

## Key bindings (`[bindings]` in pvim.conf)
- `src/config.c` gains `pvim_config_section("bindings", lines, max)`, a
  section-aware reader returning the trimmed, non-comment lines inside a
  `[name]` block (region ends at the next `[` header). The flat `key = value`
  parser used elsewhere is untouched.
- `include/terminal.h`/`src/terminal.c` add `KEY_F1`..`KEY_F12`, parsed from the
  SS3 forms (`ESC O P..S` = F1-F4), the CSI `15~`/`17~`..`24~` forms (F5-F12)
  and kitty's pushed-state functional code points 57358..57369.
- `editor_run_cmd(state, cmdline)` is the single command executor: both the
  `MODE_COMMAND` Enter handler and every binding dispatch call it, so a bound
  key and `:cmd` mean exactly the same thing.
- `include/editor.h` adds `Binding` (`key`, `cmd[128]`) and `bindings` /
  `binding_count` on `EditorState`, loaded by `bindings_load()` in `editor_init`
  and freed in `editor_free`.
- Key encoding mirrors `clipboard_key()`: Ctrl+letter -> legacy control byte,
  unmodified key -> its plain byte, alt/super combos -> `terminal_combo` value.
  `<S-p>` collapses onto the uppercase byte `P`.
- Dispatch is at the top of `editor_process_keypress`, before the clipboard
  shortcuts (so `map <D-y> ...` can rebind copy/paste), and fires only in
  MODE_NORMAL / MODE_VISUAL — COMMAND, SEARCH and INSERT are never intercepted.
- `editor_apply_config(state)` applies the editor-level settings (gutter,
  clipboard modifier, maps); `editor_init` and the `:reload` command both call
  it. `bindings_load()` frees the previous array first, so a reload can't leak.
  `highlight_reload()` resets `g_theme` from `g_theme_defaults` before applying
  the theme, so switching themes never layers colours.
- Lines that are not `map`/`noremap`, keys that fail to parse, and empty
  commands are silently skipped at load; inline `#` comments and a trailing
  `<CR>`/`<Enter>` are stripped.
- Loading is **section-strict on purpose**: a `map` line outside a `[bindings]`
  block is ignored (the flat `key = value` parser stays untouched). The bundled
  `pvim.conf` ships an explicit `[bindings]` header so copy-pasted examples
  can't silently miss it.

## Autocomplete (clangd + buffer words + headers)
- Sources: clangd results first (when it is running for the file), then unique
  buffer words, so completion works with no server at all. Inside an `#include`
  the buffer-word fallback is replaced by a local header scan (below).
  `editor_completion_open` rebuilds an editor-owned snapshot
  (`state->completion_items`) and re-filters it by the identifier prefix at the
  cursor; `main.c` never lets the editor see `LspClient`.
- The identifier the cursor currently sits in is never offered back to itself:
  `editor_completion_collect_words` computes that token's span on the cursor row
  and skips it, so typing the first letter of a word does not pop a menu (it
  would otherwise always match the `<letter><rest-of-word>` token it creates).
- Trigger: the configurable `completion_trigger` (default `<C-Space>`, also
  matched as the raw NUL legacy terminals send) plus auto while typing
  identifier / member punctuation. `editor_completion_note_request` asks main for
  one `textDocument/completion` (`state->completion_request`), coalesced by
  `completion_inflight` — no timers, at most one request in flight.
- Accept: `completion_accept` (default `<CR>` and `<Tab>`); `Ctrl-n`/`Down` and
  `Ctrl-p`/`Up` move the selection and `Esc` closes the menu. `completion` /
  `completion_auto` toggle the feature; all four are flat `key = value` entries
  parsed in `editor_apply_config` (so `:reload` picks them up) and reuse
  `parse_bind_key` for the `<...>` notation.
- `src/lsp.c` parses both `CompletionItem[]` and `CompletionList{items}`,
  prefers `textEdit.newText` over `insertText` over `label`, strips snippets to
  plain text, caps the list at `LSP_COMPLETION_MAX` (200) and converts the
  byte-based cursor to a UTF-16 position (`editor_cursor_utf16`). Request ids
  start at 100 so a completion reply can never be mistaken for the fixed
  initialize(1) / shutdown(2) ids.
- Include completion: `editor_completion_include_context` recognises the cursor
  inside `#include <...>` / `#include "..."` and yields the typed fragment plus
  the opening delimiter. `editor_completion_open` then uses the fragment's
  trailing component (after the last `/`) as the replacement prefix — the same
  slice clangd's `textEdit.range` covers — and matches the full fragment for
  local candidates: clangd items on the component (the server already resolved
  the directory), local headers on the full relative path. Accept settles the
  closing delimiter, stripping one when the item and the line both carry it and
  appending one when neither does; because `<` opens to `>` (not `<`), the
  closing char is not the opening one. `src/headers.c` (`headers_for`) lazily
  walks the system roots (`$CPATH`/`$C_INCLUDE_PATH`/`$CPLUS_INCLUDE_PATH`,
  `/usr/include`, `/usr/local/include`, and the `c++`/`-linux-gnu` subdirs) for
  angle includes, or the file's directory and ancestor `include/` directories for
  quoted ones, caching by (delimiter, directory); `:reload` calls
  `headers_invalidate`.
- The initialize request advertises `completionItem.labelDetailsSupport`, so
  clangd splits a function into `label` + `labelDetails.detail` (the parameter
  list) instead of returning the full signature as the label. `handle_completion`
  stores `labelDetails.detail` in `LspCompletion.label_detail` and
  `labelDetails.description` in `label_desc`, tags each item with its source
  (`LSP_SRC_LSP` / `LSP_SRC_BUFFER` / `LSP_SRC_HEADER` in `LSP_SRC_*`) and
  whether it is a snippet, strips a trailing include delimiter from a
  `LSP_CK_FILE` label, and `trim_ws` strips the padding clangd sometimes adds (a
  label of `" malloc"` becomes `"malloc"`).
- UI: the popup in `window.c` is anchored below the cursor, with a 10-row
  viewport and scroll; the snapshot (`state->completion_items`) is owned by the
  editor and freed in `editor_free`. Each row is two aligned columns: the clean
  label on the left and, right-aligned, the signature (return type + name +
  parameter list for functions), the kind tag and a source badge (`[LSP]`,
  `[Snip]`, `[Hdr]`, `[Buffer]`). `editor_completion_draw` computes the widths, builds the
  line and emits `WinSpan`s (per-column styles) through
  `window_set_line_spans`; the engine only knows abstract `WIN_STYLE_*` values
  (TEXT / MUTED / ACCENT / BORDER / TITLE) plus a per-row `selected` flag, and
  `display.c`'s `win_em_sgr` maps them to the `completion_menu*` theme roles. On
  a narrow terminal the signature is dropped first (then the kind tag), keeping
  the label and badge.
- For a file where clangd is not started, `setup_lsp_for_file` still records the
  attempted path in `doc_path`, so the main loop's "document changed" check does
  not re-run setup (and cancel an open menu) on every keystroke.

## Reliability
- `src/main.c` declares `LspClient lsp;` and **must** call `lsp_init(&lsp)` before
  any other LSP call. `setup_lsp_for_file()` begins with `lsp_stop(&lsp)`, which
  closes `in_fd`/`out_fd` and reaps `pid` from whatever the struct holds; on an
  uninitialized struct that garbage can close fd 0 (stdin). The symptom is
  `poll(STDIN_FILENO)` returning `POLLNVAL` every iteration, so the main loop
  repaints at full speed (~400k frames/s) and floods the terminal — a full-width
  busy loop, not a crash.
- `terminal_enable_raw_mode()` installs fatal-signal handlers (SIGINT, SIGTERM,
  SIGHUP, SIGQUIT, SIGSEGV, SIGABRT, SIGBUS, SIGFPE) that leave the alternate
  screen and restore the cooked tty before re-raising, so a crash or a `kill`
  can never leave the caller's shell in raw mode. `terminal.c` tracks
  `g_raw_active` / `g_alt_active` so the exit and signal paths only undo what was
  actually applied, and `build.sh` defensively runs `stty opost onlcr` so its
  transcript stays readable even if something else left the tty broken.

## Verification
- `make` with clang `-Wall -Wextra -Wpedantic -std=c99`: zero errors, zero warnings.
- pty end-to-end tests (in `/tmp/opencode/pvimtest/`):
  - `e2e_diag.py` — message is on the same row as the code, space-padded, severity
    colour, non-editable, cropped to width.
  - `e2e_diag_edge.py` — long lines and narrow terminals never render a row wider
    than the screen.
  - `e2e_diag_panel.py` — `:diag` lists diagnostics.
  - `e2e_yank_paste.py` — `yy` yanks a line, `p` pastes below, visual `p`
    replaces the selection.
  - `e2e_clipboard_visual.py` — `clipboard_mod+p` replaces the selection in
    visual mode (fake `wl-paste` on PATH).
  - `e2e_completion_words.py` — with no server (a `.txt` file), typing a prefix
    opens the menu from buffer words; `Ctrl-n` moves the selection and `Enter`
    replaces the prefix with the chosen word.
  - `e2e_completion_lsp.py` — for a `.c` file, `Ctrl-Space` opens the clangd
    menu and the item (`malloc`, absent from the buffer) is accepted and saved;
    skipped when clangd is not installed.
  - `e2e_completion_menu.py` — emulates the ANSI screen and checks the menu's
    two-column layout: the label sits one pad inside the left border, badges are
    right-aligned and line up across rows, the label / dimmed detail / source
    badge / selected-row block / background / border each use the configured
    `completion_menu*` role, and the selected row's block spans the full content
    width. An optional clangd phase (skipped when it is missing) checks that a
    function shows its return type and parameter list in the detail colour.
  - `e2e_completion_include.py` — `#include` completion: with clangd, typing
    inside `#include <...` offers headers and accepting yields
    `#include <stdio.h>`; with no server (a `.txt` file) the local scan backs the
    menu, its component filter narrows `foo.` to `foo.h` over an earlier header,
    a quoted include resolves relative to the file, a `sub/ty` fragment matches
    the full relative path, an existing closing `>` is never doubled, and (when
    the toolchain ships one) a C++ header is offered by its short name because
    the `c++/<version>` directory is indexed as its own root.
  - `e2e_bindings.py` — `<C-s>` writes the buffer to disk, `<C-q>` quits,
    super+d (`\x1b[100;9u`) opens the `:diag` panel, a bare `w` map fires in
    NORMAL but never while typing in `:`-mode, malformed map lines are ignored,
    and a `<F2>` map fires on the SS3 sequence (uses a temp `PVIM_CONFIG`).
  - `e2e_reload.py` — a `map` added to pvim.conf while pvim runs (and
    `linenumbers` / `theme` changes) take effect after `:reload` (uses a temp
    `PVIM_CONFIG` and `PVIM_THEME_DIR`).
  - `e2e_quit_guard.py` — a clean buffer quits with `:q`; a modified one is
    refused by `:q` (status asks for `!`) and quits with `:q!`; `:w` clears the
    guard; a failed `:wq` does not quit.
  - `e2e_resize.py` — emulates the frames on a simulated ANSI screen: the status
    bar is always on the last row (no blank row below it), and a bare
    `TIOCSWINSZ` + `SIGWINCH` (no keystroke) triggers an immediate repaint at the
    new size after both growing and shrinking.
  - `e2e_term_restore.py` — the tty is raw while pvim runs and cooked again after
    a normal `:q` and after each of SIGTERM / SIGINT / SIGHUP / SIGQUIT, so a
    killed editor can't leave the shell without `OPOST` (the staircase bug).
