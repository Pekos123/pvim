# Syntax rule files

pvim loads syntax rules from text files with a `.syntax` extension. Each file
defines one highlight scheme. No hardcoded language rules live in the C code
anymore — the engine only knows how to *read* these files.

Bundled schemes: `c`, `go`, `javascript`, `python`, `rust`, `shell`.
An editable copy of each lives in `~/.config/pvim/syntax/`.

## Where files are loaded from

In precedence order (a file with the same `filetype` replaces any earlier one):

1. `./syntax/` — the bundled defaults (this directory), relative to the
   current working directory.
2. A `syntax/` directory next to the pvim executable, or one level above it
   (this covers the repo layout `bin/pvim` → `../syntax`). This makes the
   bundled rules work no matter where you launch pvim from.
3. `~/.config/pvim/syntax/` — your personal rules.
   (`$XDG_CONFIG_HOME/pvim/syntax/` when `XDG_CONFIG_HOME` is set.)
4. The directory named by the `PVIM_SYNTAX_DIR` environment variable.

Copy a sample file into `~/.config/pvim/syntax/` and edit it to override the
built-in scheme. New files (e.g. `rust.syntax`) are picked up automatically on
the next launch.

## Format

`key = value` per line. Lines whose first non-space character is `#` are
comments. Values are stripped of surrounding whitespace; list values are split
on spaces and commas, and a token starting with `#` ends the list (inline
comment).

| Key                   | Meaning                                              | Example            |
|-----------------------|------------------------------------------------------|--------------------|
| `filetype`            | Name shown in the status bar (required)              | `filetype = c`     |
| `ext`                 | Filename extensions (repeatable, list)               | `ext = .c .h`      |
| `keywords`            | Control-flow keywords (repeatable, list)             | `keywords = if for`|
| `types`               | Type keywords, coloured like a type (list)           | `types = int char` |
| `shebang`             | Substrings matched against a `#!` first line (list)  | `shebang = python` |
| `single_comment`      | Single-line comment marker                           | `single_comment = //` |
| `multi_comment_start` | Multi-line comment opener                            | `multi_comment_start = /*` |
| `multi_comment_end`   | Multi-line comment closer                            | `multi_comment_end = */` |
| `flags`               | What to colour: `numbers`, `strings` (list)          | `flags = numbers strings` |

### Colours

The engine maps every token to one of these semantic colours:
comments/ML-comments grey, keywords orange, types (including struct/class/enum
names and `types` entries) green, strings pink, numbers red, **macros**
(preprocessor `#`-lines and ALL_CAPS identifiers) blue, **variables** steel
blue, **functions** (identifier immediately before `(`) pale yellow. Identifiers
that don't match anything become variables.

These colours are *defaults only* — every one of them can be restyled with a
theme or a pvim.conf override (see the top-level README and the `themes/`
directory). The theme does not affect *what* is highlighted, only *how each
role is coloured*.

## Notes

- `types` entries are stored internally with a trailing `|`, which is the flag
  the (otherwise unchanged) scanner treats as "type keyword".
- The whole `#include` / `#define` line is coloured as a macro, so both
  `#include "local.h"` and `#include <global.h>` render the same way.
- This scheme detection is heuristic: a type name is recognised where it is
  declared (`struct Point { ... }`, `class Foo { ... }`, `typedef ... Alias`);
  later uses of the name are coloured as variables.