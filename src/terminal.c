#include "terminal.h"
#include "wtop.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define ENABLE_VT_OUTPUT 0x0004  /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */

static HANDLE g_out = INVALID_HANDLE_VALUE;
static DWORD  g_saved_out_mode = 0;
static UINT   g_saved_cp = 0;

/* Growable byte buffer for one frame's worth of output. */
static char  *g_buf = NULL;
static size_t g_buf_len = 0;
static size_t g_buf_cap = 0;

static void buf_ensure(size_t extra)
{
    if (g_buf_len + extra + 1 <= g_buf_cap)
        return;
    size_t cap = g_buf_cap ? g_buf_cap : 8192;
    while (g_buf_len + extra + 1 > cap)
        cap *= 2;
    char *p = (char *)realloc(g_buf, cap);
    if (!p)
        return; /* drop output rather than crash; frame will be partial */
    g_buf = p;
    g_buf_cap = cap;
}

bool term_init(void)
{
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (g_out == INVALID_HANDLE_VALUE)
        return false;

    if (!GetConsoleMode(g_out, &g_saved_out_mode))
        return false;

    DWORD mode = g_saved_out_mode | ENABLE_VT_OUTPUT;
    if (!SetConsoleMode(g_out, mode))
        return false;

    /* Emit UTF-8 so box-drawing / bar glyphs render correctly. */
    g_saved_cp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);

    /* Alternate screen buffer + hide cursor + clear. */
    const char *setup = "\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H";
    DWORD written;
    WriteFile(g_out, setup, (DWORD)strlen(setup), &written, NULL);
    return true;
}

void term_shutdown(void)
{
    if (g_out == INVALID_HANDLE_VALUE)
        return;
    const char *teardown = "\x1b[0m\x1b[?25h\x1b[?1049l";
    DWORD written;
    WriteFile(g_out, teardown, (DWORD)strlen(teardown), &written, NULL);
    SetConsoleMode(g_out, g_saved_out_mode);
    if (g_saved_cp)
        SetConsoleOutputCP(g_saved_cp);
    free(g_buf);
    g_buf = NULL;
    g_buf_len = g_buf_cap = 0;
}

TermSize term_size(void)
{
    TermSize ts = { 80, 25 };
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(g_out, &info)) {
        ts.width = info.srWindow.Right - info.srWindow.Left + 1;
        ts.height = info.srWindow.Bottom - info.srWindow.Top + 1;
    }
    return ts;
}

void term_window_origin(int *x, int *y)
{
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(g_out, &info)) {
        *x = info.srWindow.Left;
        *y = info.srWindow.Top;
    } else {
        *x = 0;
        *y = 0;
    }
}

void term_frame_begin(void)
{
    g_buf_len = 0;
    /* Home the cursor; each line clears to EOL as it is drawn. */
    term_puts("\x1b[H");
}

void term_puts(const char *s)
{
    size_t n = strlen(s);
    buf_ensure(n);
    if (g_buf_cap == 0)
        return;
    memcpy(g_buf + g_buf_len, s, n);
    g_buf_len += n;
    g_buf[g_buf_len] = '\0';
}

void term_printf(const char *fmt, ...)
{
    char stack[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(stack, sizeof(stack), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n < sizeof(stack)) {
        term_puts(stack);
        return;
    }
    /* Rare large line: format into a heap buffer. */
    char *big = (char *)malloc((size_t)n + 1);
    if (!big)
        return;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    term_puts(big);
    free(big);
}

void term_frame_end(void)
{
    if (!g_buf || g_buf_len == 0)
        return;
    DWORD written;
    WriteFile(g_out, g_buf, (DWORD)g_buf_len, &written, NULL);
}
