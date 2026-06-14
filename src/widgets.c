#include "widgets.h"

#include <string.h>
#include <stdio.h>

void fmt_bytes(uint64_t bytes, char *out, size_t n)
{
    const char *units[] = { "B", "K", "M", "G", "T" };
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        u++;
    }
    if (u == 0)
        snprintf(out, n, "%.0f%s", v, units[u]);
    else
        snprintf(out, n, "%.1f%s", v, units[u]);
}

void fmt_rate(double bps, char *out, size_t n)
{
    char b[16];
    if (bps < 0) bps = 0;
    fmt_bytes((uint64_t)(bps + 0.5), b, sizeof(b));
    snprintf(out, n, "%s/s", b);
}

int display_width(const char *s)
{
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if ((*p & 0xC0) != 0x80) /* not a UTF-8 continuation byte */
            w++;
    return w;
}

void draw_meter(const char *label, double fraction, const char *text, int width)
{
    if (fraction < 0) fraction = 0;
    if (fraction > 1) fraction = 1;

    int inner = width - (int)strlen(label) - 2; /* minus the two brackets */
    if (inner < 4) inner = 4;

    int textlen = (int)strlen(text);
    if (textlen > inner) textlen = inner;
    int filled = (int)(fraction * inner + 0.5);

    const char *color = VT_FG_GREEN;
    if (fraction >= 0.8)      color = VT_FG_RED;
    else if (fraction >= 0.5) color = VT_FG_YELLOW;

    term_printf("%s%s%s[", VT_BOLD, label, VT_RESET);

    int text_start = inner - textlen;
    for (int i = 0; i < inner; i++) {
        bool in_text = (i >= text_start);
        char ch = in_text ? text[i - text_start] : (i < filled ? '|' : ' ');
        if (i < filled)
            term_printf("%s%c", color, ch);
        else
            term_printf("%s%c", VT_DIM, ch);
    }
    term_printf("%s]", VT_RESET);
}

Layout compute_layout(TermSize ts, uint32_t num_cpus, bool per_core)
{
    Layout L;
    int cols = ts.width / 20;
    if (cols < 1) cols = 1;
    if (cols > 8) cols = 8;
    if ((uint32_t)cols > num_cpus) cols = (int)num_cpus;
    if (cols < 1) cols = 1;

    L.core_cols = cols;
    L.core_rows = ((int)num_cpus + cols - 1) / cols;
    L.cpu_lines = per_core ? L.core_rows : 1;

    /* tab bar + CPU section + Mem meter + summary + column header. */
    L.header_lines = 1 + L.cpu_lines + 3;

    L.visible_rows = ts.height - L.header_lines - 1 /* footer */;
    if (L.visible_rows < 1) L.visible_rows = 1;
    return L;
}

void render_keybar(TermSize ts, const KeyBind *binds, int count, HitMap *hm)
{
    if (hm) {
        hm->count = 0;
        hm->row = ts.height - 1;
    }
    int used = 0;

    for (int i = 0; i < count; i++) {
        /* " key " + " label " plus the spaces around each. */
        int seg = display_width(binds[i].key)
                + display_width(binds[i].label) + 4;
        if (used + seg > ts.width)
            break;

        /* Key: bold black on a light button. Label: bold white on blue. */
        term_printf("\x1b[0m\x1b[1;30;107m %s \x1b[1;97;44m %s ",
                    binds[i].key, binds[i].label);

        if (hm && hm->count < WTOP_MAX_HITBOXES) {
            hm->items[hm->count].x0 = used;
            hm->items[hm->count].x1 = used + seg - 1;
            hm->items[hm->count].action = binds[i].action;
            hm->count++;
        }
        used += seg;
    }

    /* Extend the blue field to the end of the row, then reset. */
    term_printf("\x1b[44m\x1b[K\x1b[0m");
}

void render_tabbar(TermSize ts, AppView active, HitMap *hm)
{
    static const char *const names[] = { "Processes", "I/O" };
    static const AppView tabs[] = { VIEW_PROCESSES, VIEW_IO };
    const int ntabs = 2;

    (void)ts;
    if (hm) {
        hm->count = 0;
        hm->row = 0;
    }
    int used = 0;

    for (int i = 0; i < ntabs; i++) {
        int seg = display_width(names[i]) + 2; /* one space each side */
        const char *style = (tabs[i] == active)
            ? "\x1b[0m\x1b[1;30;107m"   /* active: black on bright white */
            : "\x1b[0m\x1b[1;97;44m";   /* inactive: white on blue */
        term_printf("%s %s ", style, names[i]);

        if (hm && hm->count < WTOP_MAX_HITBOXES) {
            hm->items[hm->count].x0 = used;
            hm->items[hm->count].x1 = used + seg - 1;
            hm->items[hm->count].action = (int)tabs[i];
            hm->count++;
        }
        used += seg;
    }

    term_printf("\x1b[0m\x1b[44m\x1b[K\x1b[0m");
}

int hitmap_at(const HitMap *hm, int x, int y)
{
    if (!hm || y != hm->row)
        return -1;
    for (int i = 0; i < hm->count; i++)
        if (x >= hm->items[i].x0 && x <= hm->items[i].x1)
            return hm->items[i].action;
    return -1;
}
