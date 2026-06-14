#ifndef WTOP_WIDGETS_H
#define WTOP_WIDGETS_H

#include "ui.h"
#include "terminal.h"

#include <stdint.h>
#include <stddef.h>

/* Reusable drawing helpers and the clickable bars. Widgets are dumb: they take
 * a TermSize and the data to draw, emit into the current frame, and (for the
 * bars) record click hitboxes; they never touch AppContext. */

/* One entry in a function-key bar. `action` is what hitmap_at() returns when
 * the button is clicked; each view defines its own action ids. */
typedef struct {
    const char *key;
    const char *label;
    int         action;
} KeyBind;

/* Human-readable byte size, e.g. "1.5G". */
void fmt_bytes(uint64_t bytes, char *out, size_t n);

/* "12.3M/s" style throughput from a bytes-per-second rate. */
void fmt_rate(double bps, char *out, size_t n);

/* Display width of a UTF-8 string (counts each sequence as one column). */
int display_width(const char *s);

/* htop-style meter: label[|||||      text], no trailing newline. */
void draw_meter(const char *label, double fraction, const char *text, int width);

/* Process-screen layout for the current window and CPU/meter settings. */
Layout compute_layout(TermSize ts, uint32_t num_cpus, bool per_core);

/* Bottom key bar; records each button's column span into *hm (may be NULL). */
void render_keybar(TermSize ts, const KeyBind *binds, int count, HitMap *hm);

/* Top tab bar [Processes][I/O]; records tab hitboxes (action = AppView). */
void render_tabbar(TermSize ts, AppView active, HitMap *hm);

/* Map a click to a recorded action id, or -1 if it hit nothing. */
int hitmap_at(const HitMap *hm, int x, int y);

#endif /* WTOP_WIDGETS_H */
