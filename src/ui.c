#include "ui.h"
#include "widgets.h"
#include "view_proc.h"
#include "view_io.h"
#include "view_setup.h"

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>

#define POLL_MS   30
#define STATUS_MS 4000

/* Set by main()'s console control handler on Ctrl+C / close. */
extern volatile bool g_running;

const char *const SORT_NAMES[PROC_SORT_COUNT] = {
    "CPU%", "Memory", "PID", "Threads", "Name"
};

const ViewVTable *views_get(AppView v)
{
    switch (v) {
        case VIEW_IO:    return &VIEW_IO_VTABLE;
        case VIEW_SETUP: return &VIEW_SETUP_VTABLE;
        case VIEW_PROCESSES:
        default:         return &VIEW_PROC_VTABLE;
    }
}

/* ----------------------------- shared helpers --------------------------- */

void ui_clamp_view(NavState *nav, size_t count, int visible_rows)
{
    if (count == 0) {
        nav->sel = nav->scroll = 0;
        return;
    }
    if (nav->sel >= count)
        nav->sel = count - 1;
    if (nav->sel < nav->scroll)
        nav->scroll = nav->sel;
    else if (nav->sel >= nav->scroll + (size_t)visible_rows)
        nav->scroll = nav->sel - (size_t)visible_rows + 1;
}

void ui_reanchor(NavState *nav, ProcList *pl)
{
    size_t n = proclist_count(pl);
    if (n == 0)
        return;
    if (nav->sel_pid) {
        for (size_t i = 0; i < n; i++) {
            if (proclist_get(pl, i)->pid == nav->sel_pid) {
                nav->sel = i;
                return;
            }
        }
    }
    if (nav->sel >= n)
        nav->sel = n - 1;
    nav->sel_pid = proclist_get(pl, nav->sel)->pid;
}

void ui_set_status(AppContext *ctx, uint64_t now, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ctx->ui.status, sizeof(ctx->ui.status), fmt, ap);
    va_end(ap);
    ctx->ui.status_until = now + STATUS_MS;
}

/* -------------------------------- tag set ------------------------------- */

bool tagset_contains(const TagSet *t, uint32_t pid)
{
    for (size_t i = 0; i < t->count; i++)
        if (t->pids[i] == pid)
            return true;
    return false;
}

void tagset_toggle(TagSet *t, uint32_t pid)
{
    for (size_t i = 0; i < t->count; i++) {
        if (t->pids[i] == pid) {
            t->pids[i] = t->pids[t->count - 1];  /* swap-remove */
            t->count--;
            return;
        }
    }
    if (t->count < WTOP_MAX_TAGS)
        t->pids[t->count++] = pid;
}

void tagset_clear(TagSet *t)
{
    t->count = 0;
}

/* ------------------------------ help overlay ---------------------------- */

static void render_help(AppContext *ctx)
{
    static const char *const lines[] = {
        "\x1b[1mNavigation\x1b[0m",
        "  Up/Down, j/k         move selection",
        "  PgUp/PgDn            page up / down",
        "  Home/End, g/G        jump to first / last",
        "  Mouse wheel          scroll the list",
        "  Click a row          select it",
        "",
        "\x1b[1mProcess list\x1b[0m",
        "  F4, /                filter by name",
        "  F5, t                toggle tree view",
        "  < / >                cycle sort column",
        "  Click a column       sort by it (click again reverses)",
        "  Space                tag / untag (then advance)",
        "  F9                   kill selected, or all tagged",
        "",
        "\x1b[1mScreens\x1b[0m",
        "  Tab, F6              switch Processes / I/O",
        "  Click a tab          switch screen",
        "  F2                   setup",
        "  F1, ?                this help",
        "  q, Esc, Ctrl+C       quit",
    };
    const int nlines = (int)(sizeof(lines) / sizeof(lines[0]));
    TermSize ts = ctx->ts;

    term_frame_begin();
    term_printf("\x1b[1;97;44m  wtop help \x1b[K\x1b[0m\r\n");
    term_puts("\x1b[K\r\n");
    int used = 2;
    for (int i = 0; i < nlines; i++) {
        term_printf("  %s\x1b[K\r\n", lines[i]);
        used++;
    }
    for (int i = used; i < ts.height - 1; i++)
        term_puts("\x1b[K\r\n");
    term_printf("\x1b[1;97;44m Any key / click \x1b[0m\x1b[1;97;44m Close "
                "\x1b[K\x1b[0m");
    term_frame_end();
}

/* -------------------------------- run loop ------------------------------ */

void ui_run(AppContext *ctx)
{
    /* Mouse-aware raw input. Keyboard still functions if this fails, so we
     * proceed regardless. */
    input_init();

    ctx->ts = term_size();
    ctx->L = compute_layout(ctx->ts, ctx->sys->num_cpus, ctx->cfg->show_per_core);
    ui_clamp_view(&ctx->nav, proclist_count(ctx->pl), ctx->L.visible_rows);
    views_get(ctx->ui.view)->render(ctx);

    ctx->last_sample = GetTickCount64();
    bool running = true;

    while (g_running && running) {
        bool need_render = false;
        uint64_t now = GetTickCount64();

        InputEvent ev;
        while (input_poll(&ev)) {
            if (ev.type == EV_RESIZE) {
                need_render = true;
                continue;
            }
            if (ev.type == EV_NONE)
                continue;

            /* The help overlay swallows the next event to close. */
            if (ctx->ui.help_open) {
                ctx->ui.help_open = false;
                need_render = true;
                continue;
            }

            bool keep = views_get(ctx->ui.view)->handle(ctx, &ev, now);
            if (!keep) {
                running = false;
                break;
            }
            need_render = true;
        }
        if (!g_running || !running)
            break;

        /* Expire the transient status line. */
        if (ctx->ui.status[0] && now >= ctx->ui.status_until) {
            ctx->ui.status[0] = '\0';
            need_render = true;
        }

        if (now - ctx->last_sample >= ctx->cfg->refresh_ms) {
            sysinfo_sample(ctx->sys);
            proclist_sample(ctx->pl);
            iostat_sample(ctx->io);
            ui_reanchor(&ctx->nav, ctx->pl);
            ctx->last_sample = now;
            need_render = true;
        }

        if (need_render) {
            ctx->ts = term_size();
            ctx->L = compute_layout(ctx->ts, ctx->sys->num_cpus,
                                    ctx->cfg->show_per_core);
            if (ctx->ui.help_open) {
                render_help(ctx);
            } else {
                if (ctx->ui.view == VIEW_PROCESSES)
                    ui_clamp_view(&ctx->nav, proclist_count(ctx->pl),
                                  ctx->L.visible_rows);
                views_get(ctx->ui.view)->render(ctx);
            }
        }

        Sleep(POLL_MS);
    }

    input_shutdown();
}
