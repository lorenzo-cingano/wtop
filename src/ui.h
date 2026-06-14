#ifndef WTOP_UI_H
#define WTOP_UI_H

#include "wtop.h"
#include "terminal.h"
#include "sysinfo.h"
#include "proclist.h"
#include "iostat.h"
#include "config.h"
#include "input.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Shared TUI state and the view-dispatch contract.
 *
 * Everything the views need rides on a single AppContext passed by pointer;
 * there are no UI globals (the only global is the Ctrl-handler quit flag in
 * main.c). Each screen is a ViewVTable {render, handle}; ui.c's run loop polls
 * input, dispatches to the active view, and drives the sample cadence. */

typedef enum {
    VIEW_PROCESSES,
    VIEW_IO,
    VIEW_SETUP
} AppView;

/* Navigation: we follow the selected process by PID so the highlight stays on
 * the same process as the sort order shifts between samples. */
typedef struct {
    size_t   sel;        /* selected row index into the visible list */
    size_t   scroll;     /* index of the first visible row */
    uint32_t sel_pid;    /* PID under the selection, for re-anchoring */
} NavState;

/* Per-frame layout for the process screen. */
typedef struct {
    int core_cols;       /* per-core meters per row */
    int core_rows;       /* rows of per-core meters */
    int cpu_lines;       /* lines the CPU section occupies (grid or 1 bar) */
    int header_lines;    /* everything above the process rows */
    int visible_rows;    /* process rows that fit */
} Layout;

/* Click-target geometry a widget records while rendering, so a later click can
 * be mapped back to a logical action without the renderer and the click handler
 * drifting out of sync. */
#define WTOP_MAX_HITBOXES 32
typedef struct {
    int x0, x1;          /* inclusive column span */
    int action;          /* widget-defined action id */
} Hitbox;
typedef struct {
    Hitbox items[WTOP_MAX_HITBOXES];
    int    count;
    int    row;          /* the screen row this map applies to */
} HitMap;

/* Set of tagged PIDs. Tags survive refreshes (kept here, not in ProcList) and
 * can drive a bulk kill. */
#define WTOP_MAX_TAGS 1024
typedef struct {
    uint32_t pids[WTOP_MAX_TAGS];
    size_t   count;
} TagSet;

/* Transient UI state: active screen, kill confirmation, a short-lived status
 * line shown in place of the key bar, the filter text field, and the help
 * overlay flag. */
typedef struct {
    AppView  view;
    int      setup_sel;                    /* selected row in the setup screen */

    bool     confirm_kill;                 /* showing "kill? [y/N]" prompt */
    bool     kill_bulk;                    /* the prompt targets the tag set */
    uint32_t kill_pid;                     /* single-PID target when !kill_bulk */
    char     kill_name[WTOP_MAX_PROC_NAME];
    size_t   kill_count;                   /* tagged count shown when kill_bulk */

    char     status[256];                  /* result message, "" when none */
    uint64_t status_until;                 /* tick after which to clear it */

    bool     filter_input;                 /* editing the filter text field */
    char     filter[128];                  /* active name filter, "" = none */

    bool     help_open;                    /* F1/? overlay; any event closes it */
} UiState;

/* The whole application state, threaded to every render/handle function. */
typedef struct AppContext {
    /* Owned by main(); borrowed here. */
    Config   *cfg;
    ProcList *pl;
    SysInfo  *sys;
    IoStat   *io;

    /* Owned by the UI. */
    NavState  nav;
    UiState   ui;
    TagSet    tags;
    Layout    L;
    TermSize  ts;
    uint64_t  last_sample;

    /* Filled by the active view's render, consulted on the next click. */
    HitMap    keybar_hits;
    HitMap    tab_hits;
} AppContext;

/* One screen. handle() returns false to request quit. */
typedef struct {
    void (*render)(AppContext *ctx);
    bool (*handle)(AppContext *ctx, const InputEvent *ev, uint64_t now);
} ViewVTable;

/* Dispatch table lookup (implemented in ui.c over the per-view vtables). */
const ViewVTable *views_get(AppView v);

/* The run loop: input poll -> pre-dispatch -> active view, plus the sample
 * cadence and render. Owns input_init()/input_shutdown(). */
void ui_run(AppContext *ctx);

/* Shared helpers used by both the loop and the views. */
void ui_clamp_view(NavState *nav, size_t count, int visible_rows);
void ui_reanchor(NavState *nav, ProcList *pl);
void ui_set_status(AppContext *ctx, uint64_t now, const char *fmt, ...);

/* Tag-set operations. */
bool tagset_contains(const TagSet *t, uint32_t pid);
void tagset_toggle(TagSet *t, uint32_t pid);
void tagset_clear(TagSet *t);

/* Sort-field display names, shared by the process and setup screens. */
extern const char *const SORT_NAMES[PROC_SORT_COUNT];

#endif /* WTOP_UI_H */
