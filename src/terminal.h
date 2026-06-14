#ifndef WTOP_TERMINAL_H
#define WTOP_TERMINAL_H

#include <stdbool.h>

/* Thin wrapper over the Win32 console: VT escape sequences, the alternate
 * screen buffer, cursor visibility, and a frame-sized output buffer that is
 * flushed in one write to avoid flicker. */

typedef struct {
    int width;
    int height;
} TermSize;

/* Enable VT processing, switch to the alternate screen, hide the cursor.
 * Returns false if the console could not be put into VT mode. */
bool term_init(void);

/* Restore the original console state (primary screen, cursor, mode). */
void term_shutdown(void);

/* Current console window dimensions in character cells. */
TermSize term_size(void);

/* Frame buffer: accumulate output, then flush once per frame. */
void term_frame_begin(void);
void term_puts(const char *s);
void term_printf(const char *fmt, ...);
void term_frame_end(void);

/* Common VT colour codes (SGR). Use with term_printf("%s", ...). */
#define VT_RESET    "\x1b[0m"
#define VT_BOLD     "\x1b[1m"
#define VT_DIM      "\x1b[2m"
#define VT_FG_BLACK "\x1b[30m"
#define VT_FG_RED   "\x1b[31m"
#define VT_FG_GREEN "\x1b[32m"
#define VT_FG_YELLOW "\x1b[33m"
#define VT_FG_BLUE  "\x1b[34m"
#define VT_FG_CYAN  "\x1b[36m"
#define VT_FG_WHITE "\x1b[37m"
#define VT_BG_GREEN "\x1b[42m"
#define VT_BG_CYAN  "\x1b[46m"

#endif /* WTOP_TERMINAL_H */
