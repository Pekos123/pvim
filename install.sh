#!/bin/sh
# pvim one-line installer.
#
# Downloads the latest prebuilt release binary (pvim-linux-x86_64) and installs
# it as /usr/local/bin/pvim so `pvim` runs from any directory. Pure POSIX sh;
# needs only curl or wget.
#
# Overridable environment variables:
#   PVIM_REPO     GitHub "owner/repo"       [Pekos123/pvim]
#   PVIM_VERSION  release tag, e.g. v0.2.0  [the latest release]
#   PVIM_BINDIR   install directory         [/usr/local/bin]
#   PVIM_URL      full download URL         [derived from the above]
set -eu

REPO="${PVIM_REPO:-Pekos123/pvim}"
BINDIR="${PVIM_BINDIR:-/usr/local/bin}"
ASSET="pvim-linux-x86_64"
NAME="pvim"

URL="${PVIM_URL:-}"
if [ -z "$URL" ]; then
    if [ -n "${PVIM_VERSION:-}" ]; then
        URL="https://github.com/${REPO}/releases/download/${PVIM_VERSION}/${ASSET}"
    else
        URL="https://github.com/${REPO}/releases/latest/download/${ASSET}"
    fi
fi

say()  { printf '%s\n' "$*"; }
fail() { printf 'pvim: %s\n' "$*" >&2; exit 1; }

# --- platform check ---------------------------------------------------------
os=$(uname -s 2>/dev/null || echo unknown)
arch=$(uname -m 2>/dev/null || echo unknown)
[ "$os" = "Linux" ] || fail "this installer supports Linux only (found: $os)."
case "$arch" in
    x86_64|amd64) : ;;
    *) fail "no prebuilt binary for '$arch'; only 64-bit x86_64 is published." ;;
esac

# --- downloader -------------------------------------------------------------
if command -v curl >/dev/null 2>&1; then
    download() { curl -fsSL "$1" -o "$2"; }
elif command -v wget >/dev/null 2>&1; then
    download() { wget -qO "$2" "$1"; }
else
    fail "curl or wget is required to download pvim."
fi

# --- download to a temporary file -------------------------------------------
tmp=$(mktemp 2>/dev/null || echo "/tmp/pvim-install.$$")
trap 'rm -f "$tmp"' 0 1 2 15

say "==> downloading ${ASSET} from ${REPO}"
download "$URL" "$tmp" || fail "download failed: $URL"
# mktemp creates a 0600 file; give it normal executable permissions (+x).
chmod 755 "$tmp"

# A 404 or captive portal can yield a valid file that is not a binary; make
# sure we got a real 64-bit ELF before touching the system.
if command -v file >/dev/null 2>&1; then
    case "$(file -b "$tmp")" in
        *ELF*64-bit*) : ;;
        *) fail "downloaded file is not a 64-bit ELF binary." ;;
    esac
fi

# --- install ----------------------------------------------------------------
target="${BINDIR}/${NAME}"
run_root() {
    if [ -n "${sudocmd:-}" ]; then sudo "$@"; else "$@"; fi
}

sudocmd=""
if [ ! -d "$BINDIR" ] || [ ! -w "$BINDIR" ]; then
    if command -v sudo >/dev/null 2>&1; then
        sudocmd="sudo"
        say "==> ${BINDIR} is not writable; using sudo"
    else
        fail "${BINDIR} is not writable and sudo is not available."
    fi
fi

run_root mkdir -p "$BINDIR"
run_root mv -f "$tmp" "$target"
run_root chmod 755 "$target"

say "==> installed ${target}"

# --- PATH check -------------------------------------------------------------
case ":${PATH}:" in
    *":${BINDIR}:"*)
        say "==> ${BINDIR} is already on your PATH"
        say "Success! Run 'pvim' or 'pvim path/to/file.c'."
        ;;
    *)
        say "warning: ${BINDIR} is not on your PATH."
        say "Add it with:  export PATH=\"${BINDIR}:\$PATH\""
        say "Success! pvim is installed at ${target} (then run: pvim file.c)."
        ;;
esac
