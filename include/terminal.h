#ifndef PVIM_TERMINAL_H
#define PVIM_TERMINAL_H

#include <stddef.h>

enum EditorKey {
    KEY_NULL = 0,
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN,
    KEY_F1,
    KEY_F2,
    KEY_F3,
    KEY_F4,
    KEY_F5,
    KEY_F6,
    KEY_F7,
    KEY_F8,
    KEY_F9,
    KEY_F10,
    KEY_F11,
    KEY_F12,
    KEY_ESC = 27,
    KEY_ENTER = '\r',
    KEY_TAB = '\t'
};

#define CTRL_KEY(k) ((k) & 0x1f)

/* Modifier bits, matching the kitty keyboard protocol. */
#define KEY_MOD_SHIFT 1
#define KEY_MOD_ALT   2
#define KEY_MOD_CTRL  4
#define KEY_MOD_SUPER 8

/* Modifier combos that cannot be expressed as a legacy byte (kitty sends e.g.
 * Super+y as "CSI 121;9u" even in its default mode) are packed into a single
 * int as KEY_MOD_BASE + (mods << 8) + key so keybindings stay comparable. */
#define KEY_MOD_BASE 100000

/* Pack a modifier mask and a base key into one comparable value. */
static inline int terminal_combo(int mods, int key) {
    return KEY_MOD_BASE + ((mods & 0xff) << 8) + (key & 0xff);
}

void terminal_enable_raw_mode(void);
void terminal_disable_raw_mode(void);
int terminal_read_key(void);
int terminal_get_window_size(int *rows, int *cols);
void terminal_enter_alternate_screen(void);
void terminal_leave_alternate_screen(void);

#endif /* PVIM_TERMINAL_H */
