#!/bin/sh
# Build pvim, install its config into ~/.config/pvim, and put the binary on
# PATH (a symlink in /usr/local/bin by default) so `pvim` runs from anywhere.
#
# The config install copies the bundled pvim.conf, themes/*.theme and
# syntax/*.syntax into
#   ${XDG_CONFIG_HOME:-$HOME/.config}/pvim
# and is non-destructive: existing files are left untouched so a hand-edited
# config is never clobbered. Pass --force to overwrite.
#
# The binary install symlinks bin/pvim into the target directory (default
# /usr/local/bin, overridable with --bindir or $PVIM_BIN_DIR). Because it is a
# symlink, every rebuild is picked up automatically. It escalates with sudo
# only when the directory is not writable (and only when a change is actually
# needed). Use --no-bin / --no-config to skip either step.
set -eu

script_dir=$(CDPATH= cd "$(dirname "$0")" && pwd)
cd "$script_dir"

# A program killed while the terminal was in raw mode (e.g. a pvim that died on
# a signal) leaves output post-processing off, so our newlines don't return the
# cursor to column 0 and the whole transcript staircases. Restore the standard
# cooked output flags when attached to a terminal; a no-op otherwise.
if [ -t 1 ]; then
    stty opost onlcr 2>/dev/null || true
fi

clean=0
force=0
install_config=1
install_bin=1
bindir="${PVIM_BIN_DIR:-/usr/local/bin}"

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

Build pvim, install its config into
\${XDG_CONFIG_HOME:-\$HOME/.config}/pvim, and symlink the binary into
$bindir so \`pvim\` runs from any directory.

Options:
  -c, --clean        run 'make clean' first (full rebuild)
  -f, --force        overwrite existing config files
      --bindir DIR   install the binary symlink into DIR   [$bindir]
      --no-config    do not touch the config directory
      --no-bin       do not install the binary on PATH
  -h, --help         show this help
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        -c|--clean)   clean=1 ;;
        -f|--force)   force=1 ;;
        --bindir)     shift
                      [ $# -gt 0 ] || { echo "build.sh: --bindir needs an argument" >&2; exit 2; }
                      bindir=$1 ;;
        --no-config)  install_config=0 ;;
        --no-bin)     install_bin=0 ;;
        -h|--help)    usage; exit 0 ;;
        *) echo "build.sh: unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

# --- 1. build ---------------------------------------------------------------
if [ "$clean" -eq 1 ]; then
    echo "==> make clean"
    make clean
fi
echo "==> make"
make

# --- 2. install config ------------------------------------------------------
if [ "$install_config" -eq 1 ]; then
    cfg="${XDG_CONFIG_HOME:-$HOME/.config}/pvim"
    echo "==> installing config into $cfg"
    mkdir -p "$cfg/themes" "$cfg/syntax"

    copy() { # <src> <dst>
        if [ -e "$2" ] && [ "$force" -eq 0 ]; then
            echo "      skip    $2 (exists; use --force to overwrite)"
        else
            cp -f "$1" "$2"
            echo "      install $2"
        fi
    }

    copy pvim.conf "$cfg/pvim.conf"
    for f in themes/*.theme; do
        [ -e "$f" ] || continue
        copy "$f" "$cfg/themes/$(basename "$f")"
    done
    for f in syntax/*.syntax; do
        [ -e "$f" ] || continue
        copy "$f" "$cfg/syntax/$(basename "$f")"
    done
else
    echo "==> skipping config install (--no-config)"
fi

# --- 3. install the binary on PATH ------------------------------------------
if [ "$install_bin" -eq 1 ]; then
    exe="$script_dir/bin/pvim"
    link="$bindir/pvim"
    echo "==> installing the pvim binary into $bindir"

    if [ "$(readlink "$link" 2>/dev/null || true)" = "$exe" ]; then
        echo "      ok      $link already -> $exe"
    else
        # Escalate to sudo only when the directory is not writable. A plain
        # mkdir first keeps a fresh user-owned dir (e.g. ~/.local/bin) root-free.
        sudocmd=""
        if [ ! -d "$bindir" ]; then
            mkdir -p "$bindir" 2>/dev/null || true
        fi
        if [ ! -d "$bindir" ] || [ ! -w "$bindir" ]; then
            if command -v sudo >/dev/null 2>&1; then
                sudocmd="sudo"
                echo "      $bindir is not writable; using sudo"
            else
                echo "build.sh: $bindir is not writable and sudo is unavailable." >&2
                echo "build.sh: re-run as root, or use --bindir \"\$HOME/.local/bin\"." >&2
                exit 1
            fi
        fi
        $sudocmd mkdir -p "$bindir"
        $sudocmd ln -sfn "$exe" "$link"
        echo "      linked  $link -> $exe"
    fi

    case ":$PATH:" in
        *":$bindir:"*) : ;;
        *)
            echo "    note: $bindir is not on your PATH; add it with:"
            echo "          export PATH=\"$bindir:\$PATH\""
            ;;
    esac
else
    echo "==> skipping binary install (--no-bin)"
fi

echo "==> done"
