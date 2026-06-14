#include "input.h"
#include "terminal.h"

#include <windows.h>

/* Some of these flags are missing from older MinGW headers; define what we use,
 * mirroring how terminal.c defines ENABLE_VIRTUAL_TERMINAL_PROCESSING. */
#ifndef ENABLE_MOUSE_INPUT
#define ENABLE_MOUSE_INPUT 0x0010
#endif
#ifndef ENABLE_WINDOW_INPUT
#define ENABLE_WINDOW_INPUT 0x0008
#endif
#ifndef ENABLE_QUICK_EDIT_MODE
#define ENABLE_QUICK_EDIT_MODE 0x0040
#endif
#ifndef ENABLE_EXTENDED_FLAGS
#define ENABLE_EXTENDED_FLAGS 0x0080
#endif

static HANDLE g_in = INVALID_HANDLE_VALUE;
static DWORD  g_saved_in_mode = 0;

/* Mouse button state from the previous record, for press-edge detection:
 * conhost sends a record on both the down and the up transition. */
static bool g_prev_left  = false;
static bool g_prev_right = false;

bool input_init(void)
{
    g_in = GetStdHandle(STD_INPUT_HANDLE);
    if (g_in == INVALID_HANDLE_VALUE)
        return false;
    if (!GetConsoleMode(g_in, &g_saved_in_mode))
        return false;

    DWORD mode = g_saved_in_mode;
    /* EXTENDED_FLAGS must be set in the SAME call that clears QUICK_EDIT_MODE,
     * or conhost ignores the clear and drags select text instead of producing
     * mouse events. PROCESSED_INPUT stays on so Ctrl+C still reaches the
     * console control handler. */
    mode |=  ENABLE_EXTENDED_FLAGS;
    mode |=  ENABLE_MOUSE_INPUT;
    mode |=  ENABLE_WINDOW_INPUT;
    mode |=  ENABLE_PROCESSED_INPUT;
    mode &= ~ENABLE_QUICK_EDIT_MODE;
    mode &= ~ENABLE_LINE_INPUT;
    mode &= ~ENABLE_ECHO_INPUT;
    if (!SetConsoleMode(g_in, mode))
        return false;

    FlushConsoleInputBuffer(g_in);
    g_prev_left = g_prev_right = false;
    return true;
}

void input_shutdown(void)
{
    if (g_in != INVALID_HANDLE_VALUE)
        SetConsoleMode(g_in, g_saved_in_mode);
}

static KeyCode vk_to_key(WORD vk)
{
    switch (vk) {
        case VK_UP:     return KEY_UP;
        case VK_DOWN:   return KEY_DOWN;
        case VK_LEFT:   return KEY_LEFT;
        case VK_RIGHT:  return KEY_RIGHT;
        case VK_PRIOR:  return KEY_PGUP;
        case VK_NEXT:   return KEY_PGDN;
        case VK_HOME:   return KEY_HOME;
        case VK_END:    return KEY_END;
        case VK_RETURN: return KEY_ENTER;
        case VK_ESCAPE: return KEY_ESC;
        case VK_BACK:   return KEY_BACKSPACE;
        case VK_TAB:    return KEY_TAB;
        case VK_F1:     return KEY_F1;
        case VK_F2:     return KEY_F2;
        case VK_F3:     return KEY_F3;
        case VK_F4:     return KEY_F4;
        case VK_F5:     return KEY_F5;
        case VK_F6:     return KEY_F6;
        case VK_F7:     return KEY_F7;
        case VK_F8:     return KEY_F8;
        case VK_F9:     return KEY_F9;
        case VK_F10:    return KEY_F10;
        case VK_F11:    return KEY_F11;
        case VK_F12:    return KEY_F12;
        default:        return KEY_NONE;
    }
}

/* Translate one console record into *ev. Returns true if it yielded an
 * actionable event; false for records we deliberately drop (key-up, mouse
 * move, button release, dead keys), so the caller keeps draining. */
static bool translate(const INPUT_RECORD *rec, InputEvent *ev)
{
    ev->type = EV_NONE;
    ev->key = KEY_NONE;
    ev->ch = 0;
    ev->ctrl = false;
    ev->mx = ev->my = 0;
    ev->button = MBTN_NONE;
    ev->pressed = false;
    ev->dbl = false;

    switch (rec->EventType) {
    case KEY_EVENT: {
        const KEY_EVENT_RECORD *k = &rec->Event.KeyEvent;
        if (!k->bKeyDown)
            return false;                 /* act on the down edge only */
        ev->ctrl = (k->dwControlKeyState &
                    (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
        KeyCode kc = vk_to_key(k->wVirtualKeyCode);
        if (kc != KEY_NONE) {
            ev->type = EV_KEY;
            ev->key = kc;
            return true;
        }
        WCHAR wc = k->uChar.UnicodeChar;
        if (wc >= 0x20 && wc != 0x7f) {   /* printable; skip control/dead keys */
            ev->type = EV_KEY;
            ev->key = KEY_CHAR;
            ev->ch = (uint32_t)wc;
            return true;
        }
        return false;
    }
    case MOUSE_EVENT: {
        const MOUSE_EVENT_RECORD *m = &rec->Event.MouseEvent;

        /* Moves flood the queue; drop them before any syscall for the origin. */
        if ((m->dwEventFlags & MOUSE_MOVED) && !(m->dwEventFlags & MOUSE_WHEELED)) {
            g_prev_left  = (m->dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;
            g_prev_right = (m->dwButtonState & RIGHTMOST_BUTTON_PRESSED) != 0;
            return false;
        }

        int ox = 0, oy = 0;
        term_window_origin(&ox, &oy);
        ev->mx = m->dwMousePosition.X - ox;
        ev->my = m->dwMousePosition.Y - oy;

        if (m->dwEventFlags & MOUSE_WHEELED) {
            SHORT delta = (SHORT)HIWORD(m->dwButtonState);
            ev->type = EV_MOUSE;
            ev->button = (delta > 0) ? MBTN_WHEEL_UP : MBTN_WHEEL_DOWN;
            ev->pressed = true;
            return true;
        }

        bool left  = (m->dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;
        bool right = (m->dwButtonState & RIGHTMOST_BUTTON_PRESSED) != 0;
        bool dbl   = (m->dwEventFlags & DOUBLE_CLICK) != 0;
        MouseButton b = MBTN_NONE;
        if (left && (!g_prev_left || dbl))
            b = MBTN_LEFT;
        else if (right && (!g_prev_right || dbl))
            b = MBTN_RIGHT;
        g_prev_left = left;
        g_prev_right = right;
        if (b == MBTN_NONE)
            return false;                 /* release edge: drop */
        ev->type = EV_MOUSE;
        ev->button = b;
        ev->pressed = true;
        ev->dbl = dbl;
        return true;
    }
    case WINDOW_BUFFER_SIZE_EVENT:
        ev->type = EV_RESIZE;
        return true;
    default:
        return false;
    }
}

bool input_poll(InputEvent *ev)
{
    if (g_in == INVALID_HANDLE_VALUE)
        return false;

    for (;;) {
        DWORD navail = 0;
        if (!GetNumberOfConsoleInputEvents(g_in, &navail) || navail == 0)
            return false;

        INPUT_RECORD rec;
        DWORD nread = 0;
        if (!ReadConsoleInputW(g_in, &rec, 1, &nread) || nread == 0)
            return false;

        if (translate(&rec, ev))
            return true;
        /* else: noise record consumed; keep draining the queue */
    }
}
