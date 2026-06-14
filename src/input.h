#ifndef WTOP_INPUT_H
#define WTOP_INPUT_H

#include <stdbool.h>
#include <stdint.h>

/* Win32 console input, normalized.
 *
 * Replaces the conio _getch/_kbhit model (which cannot report mouse events)
 * with ReadConsoleInput on STD_INPUT_HANDLE. Key, mouse, and resize records
 * are translated into a single InputEvent so the views never touch the raw
 * 0/224 extended-key dance or the Win32 INPUT_RECORD layout. Works on classic
 * conhost, not just Windows Terminal. */

typedef enum {
    EV_NONE,
    EV_KEY,
    EV_MOUSE,
    EV_RESIZE
} EventType;

typedef enum {
    KEY_NONE,
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_PGUP, KEY_PGDN, KEY_HOME, KEY_END,
    KEY_ENTER, KEY_ESC, KEY_BACKSPACE, KEY_TAB,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    KEY_CHAR        /* printable; Unicode codepoint in .ch */
} KeyCode;

typedef enum {
    MBTN_NONE,
    MBTN_LEFT,
    MBTN_RIGHT,
    MBTN_WHEEL_UP,
    MBTN_WHEEL_DOWN
} MouseButton;

typedef struct {
    EventType   type;
    /* EV_KEY */
    KeyCode     key;
    uint32_t    ch;       /* Unicode codepoint when key == KEY_CHAR */
    bool        ctrl;
    /* EV_MOUSE */
    int         mx, my;   /* window-relative cell, 0-based */
    MouseButton button;
    bool        pressed;  /* true on the press edge only */
    bool        dbl;      /* double-click */
} InputEvent;

/* Put the console into raw, mouse-aware input mode. Call after term_init().
 * Returns false if the input handle/mode could not be configured. */
bool input_init(void);

/* Restore the saved console input mode. Call before term_shutdown(). */
void input_shutdown(void);

/* Non-blocking. Drains noise records (mouse-move, key-up, button-release)
 * internally; fills *ev and returns true for the first real event, or false
 * when the input queue holds nothing actionable. */
bool input_poll(InputEvent *ev);

#endif /* WTOP_INPUT_H */
