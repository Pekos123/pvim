#define _POSIX_C_SOURCE 200809L
#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include "../include/terminal.h"

static struct termios orig_termios;
/* Set once the raw tty settings / alternate screen are actually active, so the
 * exit and signal paths only undo what was really applied. */
static volatile sig_atomic_t g_raw_active = 0;
static volatile sig_atomic_t g_alt_active = 0;

void terminal_disable_raw_mode(void) {
    if (!g_raw_active) return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    g_raw_active = 0;
}

/* Async-signal-safe best effort: undo the alternate screen and the raw tty
 * settings so a crash or a `kill` never leaves the caller's shell in raw mode.
 * With OPOST off a newline no longer returns the cursor to column 0, so every
 * subsequent shell line starts where the previous one ended -- the "staircase"
 * that a `./build.sh` run would otherwise print. Also runs from atexit, so an
 * error-path `exit()` restores the screen too. */
static void terminal_restore_tty(void) {
    if (g_alt_active) {
        ssize_t r = write(STDOUT_FILENO, "\x1b[?1049l", 8);
        (void)r;
        g_alt_active = 0;
    }
    if (g_raw_active) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        g_raw_active = 0;
    }
}

static void terminal_fatal_signal(int sig) {
    terminal_restore_tty();
    /* SA_RESETHAND restored the default disposition, so re-raising makes the
     * process die from the original signal (preserving e.g. a core dump). */
    raise(sig);
}

static void terminal_install_signal_handlers(void) {
    static const int sigs[] = {
        SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGSEGV, SIGABRT, SIGBUS, SIGFPE
    };
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = terminal_fatal_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
        sigaction(sigs[i], &sa, NULL);
}

void terminal_enable_raw_mode(void) {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(terminal_restore_tty);
    terminal_install_signal_handlers();

    struct termios raw = orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0)
        g_raw_active = 1;
}

/* Read one byte, honouring the raw-mode VTIME timeout (VMIN=0). Returns 1 when
 * a byte arrived, 0 on timeout or EOF. */
static int read_byte_timeout(char *out) {
    int n;
    do {
        n = read(STDIN_FILENO, out, 1);
    } while (n == -1 && errno == EINTR);
    return n == 1;
}

int terminal_read_key(void) {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) exit(1);
    }

    if (c != '\x1b')
        return (unsigned char)c;

    /* After Esc, wait briefly for the rest of an escape sequence. If nothing
     * follows, this was a bare Esc. */
    char seq[16];
    int len = 0;
    if (!read_byte_timeout(&seq[0]))
        return KEY_ESC;

    if (seq[0] != '[' && seq[0] != 'O') {
        /* Legacy Alt+key: Esc followed by the key byte. */
        return terminal_combo(KEY_MOD_ALT, (unsigned char)seq[0]);
    }

    int is_csi = (seq[0] == '[');
    char final = 0;
    len = 0;
    while (len < (int)sizeof(seq)) {
        char b;
        if (!read_byte_timeout(&b)) break;
        seq[len++] = b;
        /* Parameter/intermediate bytes are 0x20..0x3f; a byte in 0x40..0x7e
         * is the final byte that ends the sequence. */
        if (b >= 0x40 && b <= 0x7e) { final = b; break; }
    }
    if (!final)
        return KEY_ESC;

    if (is_csi && final == 'u') {
        /* CSI key[:alt] ; mods[:event] ; ... u  (kitty keyboard protocol).
         * kitty uses this for modifier combos with no legacy encoding, which
         * includes Super+y ("CSI 121;9u"). */
        long params[4] = {0, 0, 0, 0};
        int np = 0;
        long cur = 0;
        int have = 0;
        for (int i = 0; i < len - 1 && np < 4; i++) {
            char b = seq[i];
            if (b >= '0' && b <= '9') {
                cur = cur * 10 + (b - '0');
                have = 1;
            } else if (b == ';') {
                params[np++] = have ? cur : 0;
                cur = 0;
                have = 0;
            } else if (b == ':') {
                /* Alternate-key / event-type sub-field: ignore it. */
                cur = 0;
                have = 0;
            } else {
                break;
            }
        }
        if (np < 4) params[np++] = have ? cur : 0;

        int code = (int)params[0];
        int mods = (np >= 2 && params[1] > 0) ? (int)params[1] : 1;
        int bits = (mods - 1) &
                   (KEY_MOD_SHIFT | KEY_MOD_ALT | KEY_MOD_CTRL | KEY_MOD_SUPER);

        if (code == KEY_ESC && bits == 0)
            return KEY_ESC;
        /* Normalise Ctrl+letter to the legacy control byte so existing
         * bindings (Ctrl-r redo, ...) keep working regardless of whether the
         * terminal disambiguates modified keys. */
        if (bits == KEY_MOD_CTRL && code >= 'a' && code <= 'z')
            return CTRL_KEY(code);
        if (bits == 0) {
            /* kitty protocol encodes F-keys as "functional" code points
             * (57358 = F1 ... 57369 = F12) when the keyboard state is pushed;
             * fold them onto the enum so bindings still match. */
            if (code >= 57358 && code <= 57369)
                return KEY_F1 + (code - 57358);
            return code;
        }
        return terminal_combo(bits, code);
    }

    /* Legacy functional keys: CSI number ~ and CSI [1] ; mods {letter}. */
    int i = 0;
    while (i < len - 1 && seq[i] >= '0' && seq[i] <= '9')
        i++;
    int num_end = i;
    long num = 0;
    for (int j = 0; j < num_end; j++)
        num = num * 10 + (seq[j] - '0');
    /* Skip an optional ";modifiers" parameter (ignored for these keys). */
    if (i < len - 1 && seq[i] == ';') {
        i++;
        while (i < len - 1 && ((seq[i] >= '0' && seq[i] <= '9') || seq[i] == ':'))
            i++;
    }

    if (final == '~') {
        if (num_end > 0) {
            switch ((int)num) {
                case 1: case 7: return HOME_KEY;
                case 3: return DEL_KEY;
                case 4: case 8: return END_KEY;
                case 5: return PAGE_UP;
                case 6: return PAGE_DOWN;
                case 15: return KEY_F5;
                case 17: return KEY_F6;
                case 18: return KEY_F7;
                case 19: return KEY_F8;
                case 20: return KEY_F9;
                case 21: return KEY_F10;
                case 23: return KEY_F11;
                case 24: return KEY_F12;
            }
        }
        return KEY_ESC;
    }
    /* F1-F4 arrive as the SS3 forms ESC O P / Q / R / S. */
    if (!is_csi && num_end == 0 && final >= 'P' && final <= 'S')
        return KEY_F1 + (final - 'P');
    switch (final) {
        case 'A': return ARROW_UP;
        case 'B': return ARROW_DOWN;
        case 'C': return ARROW_RIGHT;
        case 'D': return ARROW_LEFT;
        case 'H': return HOME_KEY;
        case 'F': return END_KEY;
    }
    return KEY_ESC;
}

int terminal_get_window_size(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        return -1;
    }
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
}

void terminal_enter_alternate_screen(void) {
    write(STDOUT_FILENO, "\x1b[?1049h", 8);
    g_alt_active = 1;
}

void terminal_leave_alternate_screen(void) {
    if (!g_alt_active) return;
    write(STDOUT_FILENO, "\x1b[?1049l", 8);
    g_alt_active = 0;
}
