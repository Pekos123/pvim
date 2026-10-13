# pvim — a tiny vim-like editor in C

A small educational editor in C99 with undo/redo, yank/paste/delete, bounded
scrolling, and a syntax highlighter whose rules and colours are **fully
external**: the C code contains no keyword lists, no extension tables, and no
hardcoded colours — everything comes from config files.

## Quick Install (Linux & macOS)

Install the latest prebuilt binary in one line — it detects the OS and
downloads the matching asset (`pvim-linux-x86_64` on Linux,
`pvim-macos-universal` on macOS), makes it executable, and installs it to
`/usr/local/bin/pvim` (using `sudo` only when that directory is not writable):

```sh
curl -sSL https://raw.githubusercontent.com/Pekos123/pvim/main/install.sh | bash
```

Prefer a self-contained script? Clone the repo and run `./install.sh` (or
`curl ... | bash` with the file next to you) — same result, no build step on
either platform:

```sh
git clone https://github.com/Pekos123/pvim
cd pvim
./install.sh
```

Prefer not to run a script at all? Fetch the binary yourself and put it on your
`PATH`:

```sh
# Linux:
curl -sL https://github.com/Pekos123/pvim/releases/latest/download/pvim-linux-x86_64 -o pvim && chmod +x pvim && sudo mv pvim /usr/local/bin/
# macOS:
curl -sL https://github.com/Pekos123/pvim/releases/latest/download/pvim-macos-universal -o pvim && chmod +x pvim && sudo mv pvim /usr/local/bin/
```

Either way `pvim` is then available from any directory. The Linux release
binary is a standalone 64-bit x86_64 ELF with no runtime dependencies; the
macOS one is a universal Mach-O that runs natively on both Intel and Apple
Silicon. To build from source instead, see [Build & run](#build--run).

## Build & run

```sh
make                        # -> bin/pvim (+ compile_commands.json)
./build.sh                  # build + install config and the global `pvim` link
pvim file.c                 # open a file (after ./build.sh)
bin/pvim                    # run straight from the repo build
```

`./build.sh` runs `make`, copies the bundled `pvim.conf`, `themes/` and
`syntax/` into `${XDG_CONFIG_HOME:-~/.config}/pvim` (never overwriting an
existing file unless you pass `--force`), and symlinks `bin/pvim` into
`/usr/local/bin/pvim` so `pvim` runs from any directory. Because it is a
symlink, every rebuild is picked up automatically; `sudo` is used only when the
target directory isn't writable, and only when the link actually needs
changing. Other options: `--clean` (full rebuild), `--bindir DIR` or
`$PVIM_BIN_DIR` (install the link elsewhere, e.g. `~/.local/bin`), and
`--no-config` / `--no-bin` to skip a step.

The build uses clang with strict C99 flags (`-Wall -Wextra -Wpedantic
-std=c99`). Open `.c`/`.h`/`.cpp` files to get live clangd diagnostics — see
[Language server](#language-server-clangd).

Editing is vim-flavoured: `i` insert, `Esc` back to normal mode, `d` + motion
to delete, `yy` to yank the current line, `p` to paste it below, `:` for
`w`/`q`/`x` commands, `u`/`Ctrl-r` undo/redo. `Ctrl-Left`/`Ctrl-Right` (or
`Shift-Left`/`Shift-Right`) move by word — a keyword run or a punctuation run,
flowing across line breaks — `Ctrl-Up`/`Ctrl-Down` jump to the previous/next
paragraph boundary (an empty line), and `Shift-Up`/`Shift-Down` (or
`PageUp`/`PageDown`) move a full page; all of them work in normal, insert and
visual mode. `v`/`V`/`Ctrl-v` start
character-, line- and block-wise visual selection; `y` yanks the selection and
`p` pastes the register over it, and in a block selection `I` inserts text on
every selected line (see [Visual block insert](#visual-block-insert)). `:diag`
(also `:problems` /
`:errors`) opens a floating panel listing every diagnostic in the file (see
[Floating windows](#floating-windows-popups)); `-` opens the floating file
explorer (see [File explorer](#file-explorer)). Completions from clangd, the
buffer and on-disk headers (inside an `#include`) pop up while you type (see
[Autocomplete](#autocomplete)).

Search with `/`: type a word or phrase (shown in the status bar, matches are
highlighted live as you type, drawn as reverse-video blocks). `Enter` jumps to
the first match at/after the cursor and keeps the highlight until you run `:noh`
(also `:nohlsearch`); `Esc` cancels the current search and restores the previous
one. Step between matches with **`Alt+Right`** / **`Alt+Left`**, or the
`searchnext` / `searchprev` commands (also `snext`/`sprev`, `findnext`/
`findprev`) — rebindable per key in `[bindings]`. Both directions wrap around the
buffer, and `:noh` only clears the highlight: the pattern is kept, so navigation
still works afterwards. The block colour and brightness come from
`search`/`search_bold` (see [Colours](#colours-themes)).

Copy and paste talk to the system clipboard: **`Super+y`** / **`Super+p`** by
default (see [Clipboard](#clipboard)).

Keys and combos can be rebound to editor commands from pvim.conf (see
[Key bindings](#key-bindings)).

## Highlighting: syntax rules

Rules live in `*.syntax` files in a `syntax/` directory (see
[`syntax/README.md`](syntax/README.md)):

```ini
filetype = c
ext = .c .h
keywords = if for return
types = int char void
single_comment = //
multi_comment_start = /*
multi_comment_end = */
shebang = bash sh
flags = numbers strings
```

Directories are searched in this order; a rule with the same `filetype`
replaces an earlier one, so your user files override the bundled ones:

1. `./syntax/` (bundled, cwd)
2. next to the executable and one level above it (so `bin/pvim` finds the
   repo's `syntax/` regardless of the working directory)
3. `~/.config/pvim/syntax/` (or `$XDG_CONFIG_HOME/pvim/syntax`)
4. `$PVIM_SYNTAX_DIR`

## Colours: themes

Every semantic colour is configurable. [`pvim.conf`](pvim.conf) selects the
theme and can override individual colours; each theme is a `themes/*.theme`
file:

| Role       | What it colours                    |
|------------|------------------------------------|
| `comment`  | single-line comments               |
| `mlcomment`| multi-line comments                |
| `keyword1` | control-flow keywords (`if`, ...)  |
| `keyword2` | type keywords and type names       |
| `string`   | `"..."` / `'...'` literals         |
| `number`   | numeric literals                   |
| `macro`    | `#`-directive lines, ALL_CAPS      |
| `variable` | plain identifiers                  |
| `function` | identifier immediately before `(`  |
| `match`    | search matches (alias `search`)    |
| `selection`| visual selection                   |
| `completion_menu`        | autocomplete candidate label      |
| `completion_menu_detail` | autocomplete signature / detail   |
| `completion_menu_source` | autocomplete kind tag and badge   |
| `completion_menu_select` | autocomplete selected-row block   |
| `completion_menu_bg`     | autocomplete popup background     |
| `completion_menu_border` | autocomplete popup border         |

The label role is also spelled `completion_menu_label`, and every role has a
short alias without the `completion_` prefix (`menu`, `menu_detail`,
`menu_source`, `menu_select`, `menu_bg`, `menu_border`).

Colour values are XTerm-256 indices (`0`-`255`), a name (`red`, `orange`,
`pink`, ...), or `default`/`none`/`-1` for the terminal default. See
`themes/default.theme` for the current palette.

Search matches and the visual selection are drawn as **reverse-video blocks**
(nothing is hardcoded — they look like a terminal selection). Their role
colour becomes the block's background, and two extra keys brighten them:

```ini
search         = 226   # alias: match
search_bold    = off   # on / off / true / false / 1 / 0
selection      = 240   # -1 reuses the syntax colour (multi-coloured block)
selection_bold = off
```

These live in `pvim.conf` (winning over the theme) or in a `themes/*.theme`
file.

**Starter config**: `./build.sh` installs [`pvim.conf`](pvim.conf) plus the
bundled `themes/` and `syntax/` into `~/.config/pvim/`, without overwriting
anything you have already edited (use `--force` to replace files). To do the
same by hand:

```sh
mkdir -p ~/.config/pvim
cp pvim.conf ~/.config/pvim/
cp -r themes syntax ~/.config/pvim/
```

Then change `theme = ...` or add colour overrides. To try a theme for a single
run without touching any files:

```sh
PVIM_THEME=solarized bin/pvim file.c
```

The active theme name is shown in the status bar.

## Line numbers

A gutter with line numbers sits to the left of the text. The mode is set in
`pvim.conf`:

```ini
linenumbers = absolute   # every line shows its own number (default)
linenumbers = relative   # each line shows its distance from the cursor
linenumbers = off        # no gutter
```

`PVIM_LINENUMBERS` overrides the config for a single run. In relative mode the
cursor line keeps its absolute number (shown bold); the current mode is shown in
the status bar (`Ln: abs` / `Ln: rel` / `Ln: off`). The gutter resizes
automatically with the file size, and horizontal scrolling never hides the
gutter.

## Clipboard

Copy and paste go through the system clipboard, bound to a configurable
modifier plus `y` / `p`:

```ini
clipboard_mod = super   # ctrl | alt | super (default) | none
```

- **`super+y`** copies: the visual selection in visual mode, or the current line
  (newline included) in normal mode.
- **`super+p`** pastes at the cursor; in visual mode it replaces the selection.

The internal register is separate and always available:

- **`yy`** yanks the current line (like `dd`, but without deleting).
- **`p`** pastes the register: after a `yy`/`dd` it inserts the line below the
  cursor; in visual mode it replaces the selection with the register contents.
- `d` + a motion (`dd`, or `d` with the arrow keys) also fills the register, so
  deleted text can be pasted back.

`ctrl` and `alt` work in any terminal. `super` (the Windows/Command key) relies
on the terminal forwarding it as a `CSI u` sequence — kitty does this out of the
box (e.g. `Super+y` arrives as `CSI 121;9u`), so no extra setup is needed there.
`PVIM_CLIPBOARD_MOD` overrides the config for a single run.

The clipboard itself is reached through the usual helpers — `wl-copy`/`wl-paste`
(Wayland), `xclip`, `xsel`, `pbcopy`/`pbpaste` — tried in that order. Copying
falls back to the terminal's **OSC 52** escape when no helper is installed, so
it keeps working over SSH.

## Visual block insert

In a block selection (`Ctrl-v`), press **`I`** to insert text on **every** line
of the block at its left column. You type on the first line as usual (the status
bar shows `-- VISUAL BLOCK INSERT --`); press `Esc` and the typed text is replayed
on all the other selected lines at the same column.

- `Backspace` edits the run before it is applied, and `Tab` inserts four spaces,
  just like insert mode.
- Lines shorter than the target column are padded with spaces so the inserted
  text lines up.
- The whole operation — typing plus the multi-line application — is a **single
  undo step**, so one `u` removes it everywhere.
- `I` outside a block selection does nothing.

## Word, paragraph and page motions

The arrow keys move the cursor one cell at a time. **Ctrl** and **Shift** add
word-wise and page-wise navigation, in normal, insert and visual mode alike:

| Key                       | Action                                                    |
|---------------------------|-----------------------------------------------------------|
| `Ctrl-Right` / `Shift-Right` | forward to the start of the next word                  |
| `Ctrl-Left` / `Shift-Left`   | back to the start of the previous word                 |
| `Ctrl-Down`               | down to the next paragraph boundary (the next empty line) |
| `Ctrl-Up`                 | up to the previous paragraph boundary                     |
| `Shift-Down` / `PageDown` | scroll down and move the cursor a full page               |
| `Shift-Up` / `PageUp`     | scroll up and move the cursor a full page                 |

A *word* follows vim's token logic: a run of keyword characters (`[A-Za-z0-9_]`)
or a run of punctuation, with whitespace skipped between. Moving forward at the
end of a line continues at the first word of the next line, and vice versa, so
the motions flow across the whole buffer. The paragraph jumps are vim's `{` /
`}`, landing in column 0 on the next/previous empty line. A *page* is the height
of the text viewport; the cursor moves by that many lines (clamped to the first
and last line of the file) and the window scrolls to keep it visible, so the
cursor never leaves the viewport. In a visual selection all of these simply
grow or shrink the highlight.

The sequences are decoded for standard terminals and for the kitty keyboard
protocol (`CSI 1;2C`, `CSI 1;5C`, `CSI 1;3C`, `CSI 6;3~`, …), plus the bare
`CSI 5C` form some terminals (rxvt) send. Modified arrows are reported as
distinct keys, so the table above is only the default layout — they can be
remapped (see [Key bindings](#key-bindings)), for example onto **Alt** when
your terminal already uses Shift. Note that Alt+Left/Alt+Right are the built-in
search next/previous keys; any *other* unbound Alt+arrow still moves one cell.

## Autocomplete

While typing, pvim offers completions from a few sources: **clangd** (when it
is running for the file, see [Language server](#language-server-clangd)), the
**words already in the buffer**, and — inside an `#include` directive — the
**headers found on disk**, so completion keeps working in a plain text file or
with no server installed. Candidates appear in a floating window anchored just
below the cursor.

```ini
completion = on                 # master switch (default on)
completion_auto = on            # also open while typing (default on)
completion_trigger = <C-Space>  # open on demand (default <C-Space>)
completion_accept = <CR> <Tab>  # keys that accept the highlighted item
```

- With `completion_auto = on`, the menu opens as soon as you type an identifier
  character (or one of `.`, `>`, `:`, `/`, `"`, `<`) and re-filters on each
  further key. The word you are currently editing is never offered back to
  itself, so typing the first letter of a word does not pop a menu.
- `Ctrl-Space` (the default trigger) opens the menu on demand, even with
  auto-completion off. In a legacy terminal `Ctrl-Space` arrives as a NUL byte
  and is matched too.
- `Ctrl-n` / `Down` and `Ctrl-p` / `Up` move the selection. The accept keys
  (`Enter` and `Tab` by default) replace the typed prefix with the highlighted
  candidate; `Esc` closes the menu (press it again to leave insert mode), and a
  key that cannot continue a completion (a space, say) closes it and is then
  processed normally.
- Each row is two columns: the clean candidate label on the left, and on the
  right (right-aligned) the signature plus a short kind tag (`fn`, `var`,
  `class`, …) and a source badge. For clangd functions the signature shows the
  return type, name and parameter list (for example
  `void *malloc(size_t size) fn [LSP]`).
- The badge tells you where the candidate came from: `[LSP]` (clangd),
  `[Snip]` (a snippet), `[Hdr]` (a header from the local scan) or `[Buffer]`
  (a word already in the file). On a narrow terminal the signature is dropped
  first and the label and badge are kept.
- Inside an `#include` (angle or quoted) the menu completes header paths. clangd
  supplies its include-path completions when it is running; otherwise pvim scans
  the system include directories (for `<...>`), or the file's own directory and
  any ancestor `include/` (for `"..."`). The text after the last `/` is the
  replaced slice — the same one clangd's edit covers — so `sys/ty` completes to
  `sys/types.h`, and the closing `>` or `"` is added (or reused) automatically.

The menu's colours come from the `completion_menu*` theme roles: the label, the
dimmed signature/detail, the source badge, the selected-row background, the popup
background and its border (see [Colours: themes](#colours-themes)). They also
work as flat `key = value` overrides in `pvim.conf`.

The trigger and accept keys use the same `<...>` notation as
[Key bindings](#key-bindings). All four settings are re-read by `:reload`.

## Key bindings

Keys and key combos can be bound to editor commands with a vim-style `map`
section in `pvim.conf`:

```ini
[bindings]
map <C-s> :w
map <C-q> :q!
map <D-y> :w
map <F2> :diag
map g :wq
```

pvim reads the first `pvim.conf` it finds, in this order: `$PVIM_CONFIG`, then
`${XDG_CONFIG_HOME:-~/.config}/pvim/pvim.conf`, then `./pvim.conf`, then next to
the executable and one level above it — so a `~/.config` copy takes precedence
over one in the working directory. Because maps are consulted **before** the
built-in keys, a map shadows a default. In particular, a config copied from the
older word-motion example (which bound `<A-Right>`/`<A-Left>`) masks the
built-in search next/previous keys; remove those lines or move the word motions
to another combo (e.g. `<A-S-Right>`/`<A-S-Left>`) and run `:reload`.

The `[bindings]` header is **required**: a `map` line anywhere else in the
file is ignored (that keeps the strict flat `key = value` parsing intact). The
section may be empty, and every line under it maps a key to a command; the `:`,
a trailing `<CR>`/`<Enter>`, and inline `#` comments are all optional
(`# foo`, `:w<CR>`, `:w # save` are the same three ways of writing the same
binding). `noremap` is accepted as a synonym for `map`. Anything else —
non-`map` lines, keys that don't parse, or empty commands — is ignored, so a
stray line can never break startup.

The key is written in standard vim notation:

| Form            | Key                                                     |
|-----------------|---------------------------------------------------------|
| `<C-x>`         | Ctrl+letter (`<C-p>`, `<C-s>`, …)                       |
| `<S-x>`         | Shift+letter — collapses onto the uppercase byte (`P`)  |
| `<A-x>` / `<M-x>` | Alt/Meta, combinable (`<C-A-x>`)                       |
| `<D-x>`         | Super (Windows/Command), as a kitty `CSI …;9u` combo    |
| `<F1>`–`<F12>`  | Function keys (SS3 + `CSI n~` + kitty functional codes) |
| `<CR>`/`<Enter>`,`<Esc>`,`<Tab>`,`<BS>`,`<Space>` | named keys |
| `<Up>`/`<Down>`/`<Left>`/`<Right>`, `<Home>`, `<End>`, `<Del>`, `<PageUp>`, `<PageDown>` | navigation, combinable with Ctrl/Shift/Alt/Super (`<S-Down>`, `<A-Up>`, `<C-S-Right>`, …) |
| `g` and any other single character | bare keys, bindable as-is    |

Bindings fire in **normal and visual modes only**; typing, `:`-commands and
search are never intercepted, and a plain key keeps its literal meaning in
insert mode. Bindings are consulted before the built-in commands and the
clipboard shortcuts, so `map <D-y> :w` rebinds copy, and `map w :q!` shadows
the (otherwise unbound) `w` key in normal mode. Visual mode drops to normal
before the command runs — except for the motion commands below, which keep the
mode so a bound key extends the selection.

Commands are the same set `:` mode accepts: `w`/`wq`/`x` (optionally with a
file argument), `q`/`q!`, `noh`, `diag`, `close`, `explore`, `reload`, and the
search motions `searchnext`/`searchprev` (also `snext`/`sprev`,
`findnext`/`findprev`). The
[word / paragraph / page motions](#word-paragraph-and-page-motions) are
commands too — `wordleft`/`wordprev`, `wordright`/`wordnext`,
`parup`/`parprev`, `pardown`/`parnext`, `pageup`/`pgup`, `pagedown`/`pgdn` — so
they can be rebound to any key that parses. Alt+Left/Alt+Right are the built-in
search navigation keys, so move the word motions to a free combo:

```ini
[bindings]
map <A-Down>    :pagedown
map <A-Up>      :pageup
map <A-S-Right> :wordright
map <A-S-Left>  :wordleft
```

Rebinding `searchnext`/`searchprev` works the same way, e.g. onto `n`/`N`:

```ini
map n :searchnext
map N :searchprev
```

`:q` refuses to throw away unsaved work: when the buffer has been modified it
is rejected with *No write since last change (add ! to override)*. `:q!` is the
explicit override that discards the changes; a successful `:w` clears the
guard, and `wq`/`x` only quit once the write succeeds (a failed save leaves the
buffer intact).

`:reload` (also `:reloadconfig` / `:source`) re-reads `pvim.conf`, the active
theme and the syntax rule files from disk, so config edits — including new
`[bindings]` maps — take effect without restarting the editor.

## Language server (clangd)

Opening a C/C++ file (`.c`, `.h`, `.cc`, `.cpp`, `.cxx`, `.hh`, `.hpp`,
`.hxx`) automatically starts `clangd --background-index --header-insertion=never`
and streams diagnostics back over JSON-RPC 2.0. The server is driven
non-blockingly: the editor polls it alongside the keyboard, so indexing never
stalls the UI.

- Errors and warnings are marked in the gutter (`E` / `W` / `I` / `H`) and
  counted in the status bar (`E1 W0`); the status bar itself never carries the
  message text, so a long message can't wrap it.
- The message for a diagnostic is appended **inline at the end of the code
  line**, after a single space of padding, coloured by severity (`Error` /
  `Warning` / `Info` / `Hint`). It is pure decoration: it is written after the
  row's characters and is never part of the buffer, so it cannot be edited and
  the cursor can never land in it. The message is cropped so the whole row still
  fits the text area; if there is no room (a very long line), it is simply not
  drawn, which keeps the message from wrapping and corrupting the frame.
- `:diag` (also `:problems` / `:errors` / `:diagnostics` / `:diags`) opens a
  panel listing every diagnostic in the file.
- Opening a different file (for example through the explorer) re-points clangd
  at the new document, so diagnostics always describe the buffer on screen.
- Edits are pushed to clangd as full-text `didChange` notifications, so the
  diagnostics refresh as you type.
- The terminal size is re-read on `SIGWINCH`, and the signal interrupts `poll()`
  so pvim repaints at the new size immediately (no need to press a key first).
  The status bar stays on the true bottom row, so resizing can't leave a stray
  blank line below it or stale content from the old frame.

`make` also writes `compile_commands.json`, so clangd evaluates the buffer with
exactly the flags pvim is built with. If `clangd` is not installed the editor
simply runs without diagnostics.

## Floating windows (popups)

`src/window.c` is a small overlay engine for popups that are composited on top
of the text grid during the normal redraw pass (no flicker, no separate buffer
to restore). A `FloatWindow` has a position, size, optional title, a box-drawing
border (`none` / `single` / `round` / `double`) and a z-order ("elevation");
higher-z windows are painted last, so they win where they overlap. Windows are
fitted to the text area and shifted in from the edge when they would fall
off-screen, so a popup can never corrupt the status line or draw out of bounds.

The engine never writes ANSI itself: it drives a `WinEmitter` sink
(cursor moves, attributes, text), which keeps the bounds checking, borders and
z-ordering fully unit-testable.

Right now the diagnostics list is the built-in consumer:

- `:diag` (also `:diagnostics`, `:diags`, `:problems`, `:errors`) opens a
  floating panel listing the current clangd diagnostics
  (`Error`/`Warning`/`Info`/`Hint`, line, message).
- `:close` (also `:c`, `:popup`) closes the panel; so does `Esc` in normal mode.
- With no language server running, the panel explains that instead.

The same primitive backs the completion menu (see
[Autocomplete](#autocomplete)) and the file explorer, and is the basis for
command palettes.

## File explorer

Press `-` (or run `:explore` / `:ex` / `:e`) to open a floating, Miller-columns
file manager (`src/explorer.c`) built on the same overlay engine. Three rounded
columns tile the text area:

- **Left** — the parent directory, with the current directory highlighted
  (empty and disabled at `/`).
- **Middle** — the working directory. Directories sort first and carry a
  `▸`/`/` marker. The highlighted entry is the selection.
- **Right** — a live preview: the selected file's first lines (or
  `<binary file>`) or a listing of a hovered directory.

Keys while the explorer is open:

| Key                       | Action                                            |
|---------------------------|---------------------------------------------------|
| `j` / `k` or arrows       | move the selection                                |
| `h` / left                | go to the parent directory                        |
| `l` / right / `Enter`     | enter a directory or open a file in the editor    |
| `a`                       | create a file (or a directory when the name ends in `/`) |
| `r`                       | rename the selected entry                         |
| `d`                       | delete the selected entry (`y` to confirm)        |
| `Esc`                     | close the explorer                                |

Editing is done inline: the prompt text appears in the status bar (the cursor
rides at the end of it), `Backspace` edits, `Enter` commits, `Esc` cancels.
Opening a file loads it into the buffer, clears undo/redo, snaps the cursor to
the top and leaves the explorer.

## Layout

```
src/        C sources (editor core, buffer, display, terminal, highlight, clipboard, config, lsp, window, explorer, headers)
include/    headers
syntax/     language rule files          (see syntax/README.md)
themes/     colour theme files
pvim.conf   starter configuration
build.sh    build; install config + global `pvim` symlink
Makefile    build rules (make -> bin/pvim)
```