#include "view_proc.h"
#include "widgets.h"

#include <string.h>
#include <stdio.h>

/* Keybar actions for the process screen. */
enum {
    PA_NONE = 0,
    PA_HELP, PA_SETUP, PA_FILTER, PA_TREE, PA_KILL, PA_IO, PA_QUIT
};

static const KeyBind PROC_KEYS[] = {
    { "F1",  "Help",   PA_HELP   },
    { "F2",  "Setup",  PA_SETUP  },
    { "F4",  "Filter", PA_FILTER },
    { "F5",  "Tree",   PA_TREE   },
    { "Spc", "Tag",    PA_NONE   },
    { "F9",  "Kill",   PA_KILL   },
    { "Tab", "I/O",    PA_IO     },
    { "q",   "Quit",   PA_QUIT   },
};
#define PROC_KEY_COUNT ((int)(sizeof(PROC_KEYS) / sizeof(PROC_KEYS[0])))

/* ----------------------------- sort / tree ------------------------------ */

static void apply_sort(AppContext *ctx)
{
    proclist_set_sort(ctx->pl, ctx->cfg->sort_field, ctx->cfg->sort_descending);
    ui_reanchor(&ctx->nav, ctx->pl);
}

static void cycle_sort(AppContext *ctx, int delta)
{
    int f = (int)ctx->cfg->sort_field + delta;
    f = (f % PROC_SORT_COUNT + PROC_SORT_COUNT) % PROC_SORT_COUNT;
    ctx->cfg->sort_field = (ProcSortField)f;
    apply_sort(ctx);
}

static void toggle_tree(AppContext *ctx)
{
    ctx->cfg->tree_view = !ctx->cfg->tree_view;
    proclist_set_tree(ctx->pl, ctx->cfg->tree_view);
    ui_reanchor(&ctx->nav, ctx->pl);
}

/* Map a click x in the column-header row to its sort field. Column spans follow
 * the header format specs in proc_render(): PID 0-7, CPU% 8-14, MEM 15-24,
 * THR 25-31, COMMAND 32+. */
static ProcSortField header_field_at(int x)
{
    if (x <= 7)  return PROC_SORT_PID;
    if (x <= 14) return PROC_SORT_CPU;
    if (x <= 24) return PROC_SORT_MEM;
    if (x <= 31) return PROC_SORT_THREADS;
    return PROC_SORT_NAME;
}

static void sort_by(AppContext *ctx, ProcSortField f)
{
    if (ctx->cfg->sort_field == f)
        ctx->cfg->sort_descending = !ctx->cfg->sort_descending;
    else
        ctx->cfg->sort_field = f;
    apply_sort(ctx);
}

/* -------------------------------- kill ---------------------------------- */

/* Begin a kill confirmation: the whole tag set if any are tagged, else the
 * selected process. */
static void request_kill(AppContext *ctx)
{
    if (ctx->tags.count > 0) {
        ctx->ui.confirm_kill = true;
        ctx->ui.kill_bulk = true;
        ctx->ui.kill_count = ctx->tags.count;
        return;
    }
    if (proclist_count(ctx->pl) == 0)
        return;
    const ProcInfo *p = proclist_get(ctx->pl, ctx->nav.sel);
    ctx->ui.confirm_kill = true;
    ctx->ui.kill_bulk = false;
    ctx->ui.kill_pid = p->pid;
    snprintf(ctx->ui.kill_name, sizeof(ctx->ui.kill_name), "%s", p->name);
}

/* Answer the kill prompt. Windows has no signals, so this is TerminateProcess. */
static void confirm_kill(AppContext *ctx, const InputEvent *ev, uint64_t now)
{
    ctx->ui.confirm_kill = false;

    bool yes = (ev->type == EV_KEY && ev->key == KEY_CHAR &&
                (ev->ch == 'y' || ev->ch == 'Y'));
    if (!yes) {
        ui_set_status(ctx, now, "Kill cancelled");
        return;
    }

    if (ctx->ui.kill_bulk) {
        size_t total = ctx->tags.count, killed = 0;
        for (size_t i = 0; i < ctx->tags.count; i++) {
            HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, ctx->tags.pids[i]);
            if (h) {
                if (TerminateProcess(h, 1))
                    killed++;
                CloseHandle(h);
            }
        }
        tagset_clear(&ctx->tags);
        ui_set_status(ctx, now, "Killed %zu of %zu tagged process%s",
                      killed, total, total == 1 ? "" : "es");
        return;
    }

    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, ctx->ui.kill_pid);
    if (!h) {
        ui_set_status(ctx, now,
                      "Cannot open PID %u (%.64s) to terminate (error %lu)",
                      ctx->ui.kill_pid, ctx->ui.kill_name, GetLastError());
    } else {
        if (TerminateProcess(h, 1))
            ui_set_status(ctx, now, "Terminated PID %u (%.64s)",
                          ctx->ui.kill_pid, ctx->ui.kill_name);
        else
            ui_set_status(ctx, now,
                          "Failed to terminate PID %u (%.64s): error %lu",
                          ctx->ui.kill_pid, ctx->ui.kill_name, GetLastError());
        CloseHandle(h);
    }
}

/* ------------------------------- filter --------------------------------- */

/* Encode a BMP codepoint as UTF-8. Returns byte count, or 0 for a surrogate. */
static int utf8_encode(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp >= 0xD800 && cp <= 0xDFFF)
        return 0;                 /* lone surrogate: skip */
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}

/* Delete the last whole UTF-8 sequence from s. */
static void utf8_chop(char *s)
{
    size_t len = strlen(s);
    if (len == 0)
        return;
    size_t i = len - 1;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80)
        i--;
    s[i] = '\0';
}

static void filter_event(AppContext *ctx, const InputEvent *ev)
{
    UiState *ui = &ctx->ui;
    if (ev->type != EV_KEY)
        return;

    switch (ev->key) {
        case KEY_ENTER:
            ui->filter_input = false;
            break;
        case KEY_ESC:
            ui->filter[0] = '\0';
            ui->filter_input = false;
            proclist_set_filter(ctx->pl, ui->filter);
            break;
        case KEY_BACKSPACE:
            utf8_chop(ui->filter);
            proclist_set_filter(ctx->pl, ui->filter);
            break;
        case KEY_CHAR: {
            char enc[4];
            int k = utf8_encode(ev->ch, enc);
            size_t len = strlen(ui->filter);
            if (k > 0 && len + (size_t)k < sizeof(ui->filter)) {
                memcpy(ui->filter + len, enc, (size_t)k);
                ui->filter[len + (size_t)k] = '\0';
            }
            proclist_set_filter(ctx->pl, ui->filter);
            break;
        }
        default:
            break;
    }
}

/* ------------------------------- actions -------------------------------- */

/* Run a keybar/menu action shared by key and click. Returns false to quit. */
static bool run_action(AppContext *ctx, int action)
{
    switch (action) {
        case PA_HELP:   ctx->ui.help_open = true; return true;
        case PA_SETUP:  ctx->ui.view = VIEW_SETUP; ctx->ui.setup_sel = 0; return true;
        case PA_FILTER: ctx->ui.filter_input = true; return true;
        case PA_TREE:   toggle_tree(ctx); return true;
        case PA_KILL:   request_kill(ctx); return true;
        case PA_IO:     ctx->ui.view = VIEW_IO; return true;
        case PA_QUIT:   return false;
        default:        return true;
    }
}

static void tag_selected(AppContext *ctx)
{
    size_t n = proclist_count(ctx->pl);
    if (n == 0)
        return;
    const ProcInfo *p = proclist_get(ctx->pl, ctx->nav.sel);
    tagset_toggle(&ctx->tags, p->pid);
    if (ctx->nav.sel + 1 < n)          /* advance, htop-style */
        ctx->nav.sel++;
}

/* -------------------------------- render -------------------------------- */

static void proc_footer(AppContext *ctx)
{
    UiState *ui = &ctx->ui;
    TermSize ts = ctx->ts;

    ctx->keybar_hits.count = 0;        /* stale-click guard when not drawn */

    if (ui->confirm_kill) {
        if (ui->kill_bulk)
            term_printf("\x1b[1;97;41m Kill %zu tagged process%s?  [y/N] "
                        "\x1b[K\x1b[0m",
                        ui->kill_count, ui->kill_count == 1 ? "" : "es");
        else
            term_printf("\x1b[1;97;41m Kill process %u (%s)?  [y/N] "
                        "\x1b[K\x1b[0m", ui->kill_pid, ui->kill_name);
        return;
    }
    if (ui->filter_input) {
        term_printf("\x1b[1;30;42m Filter: %s\xe2\x96\x88 \x1b[0m"
                    "%s  Enter apply   Esc clear%s\x1b[K",
                    ui->filter, VT_DIM, VT_RESET);
        return;
    }
    if (ui->status[0]) {
        term_printf("\x1b[1;97;44m %s \x1b[K\x1b[0m", ui->status);
        return;
    }
    render_keybar(ts, PROC_KEYS, PROC_KEY_COUNT, &ctx->keybar_hits);
}

static void proc_render(AppContext *ctx)
{
    const SysInfo *sys = ctx->sys;
    ProcList *pl = ctx->pl;
    const Config *cfg = ctx->cfg;
    const NavState *nav = &ctx->nav;
    Layout L = ctx->L;
    TermSize ts = ctx->ts;

    term_frame_begin();

    render_tabbar(ts, VIEW_PROCESSES, &ctx->tab_hits);
    term_puts("\r\n");

    char label[16], val[16];

    if (cfg->show_per_core) {
        int gap = 1;
        int meter_w = (ts.width - (L.core_cols - 1) * gap) / L.core_cols;
        if (meter_w < 8) meter_w = 8;

        for (int r = 0; r < L.core_rows; r++) {
            for (int c = 0; c < L.core_cols; c++) {
                int idx = r * L.core_cols + c;
                if ((uint32_t)idx >= sys->num_cpus)
                    break;
                snprintf(label, sizeof(label), "%d", idx);
                snprintf(val, sizeof(val), "%.0f%%", sys->core_usage[idx] * 100.0);
                draw_meter(label, sys->core_usage[idx], val, meter_w);
                if (c + 1 < L.core_cols && (uint32_t)(idx + 1) < sys->num_cpus)
                    term_puts(" ");
            }
            term_puts("\x1b[K\r\n");
        }
    } else {
        snprintf(val, sizeof(val), "%.1f%%", sys->cpu_usage * 100.0);
        draw_meter("CPU", sys->cpu_usage, val, ts.width);
        term_puts("\x1b[K\r\n");
    }

    double mem_frac = sys->mem_total
                        ? (double)sys->mem_used / (double)sys->mem_total : 0;
    char used[24], total[24], buf[64];
    fmt_bytes(sys->mem_used, used, sizeof(used));
    fmt_bytes(sys->mem_total, total, sizeof(total));
    snprintf(buf, sizeof(buf), "%s/%s", used, total);
    draw_meter("Mem", mem_frac, buf, ts.width);
    term_puts("\x1b[K\r\n");

    term_printf("%s%u CPUs   avg %.1f%%   ",
                VT_DIM, sys->num_cpus, sys->cpu_usage * 100.0);
    if (ctx->ui.filter[0])
        term_printf("%zu/%zu match \"%s\"   ",
                    proclist_count(pl), proclist_total(pl), ctx->ui.filter);
    else
        term_printf("%zu processes   ", proclist_count(pl));
    if (ctx->tags.count)
        term_printf("%zu tagged   ", ctx->tags.count);
    term_printf("sorted by %s%s%s\x1b[K\r\n",
                SORT_NAMES[cfg->sort_field],
                cfg->tree_view ? "   [tree]" : "", VT_RESET);

#define HDR_BASE "\x1b[1;97;44m"
#define HDR_HI   "\x1b[1;30;103m"
#define COL(f)   (cfg->sort_field == (f) ? HDR_HI : HDR_BASE)
    term_printf("%s%-7s%s ",  COL(PROC_SORT_PID),     "PID",     HDR_BASE);
    term_printf("%s%6s%s ",   COL(PROC_SORT_CPU),     "CPU%",    HDR_BASE);
    term_printf("%s%9s%s ",   COL(PROC_SORT_MEM),     "MEM",     HDR_BASE);
    term_printf("%s%5s%s  ",  COL(PROC_SORT_THREADS), "THR",     HDR_BASE);
    term_printf("%s%s%s",     COL(PROC_SORT_NAME),    "COMMAND", HDR_BASE);
    term_puts("\x1b[K\x1b[0m\r\n");
#undef COL
#undef HDR_HI
#undef HDR_BASE

    size_t n = proclist_count(pl);
    for (int i = 0; i < L.visible_rows; i++) {
        size_t idx = nav->scroll + (size_t)i;
        if (idx >= n) {
            term_puts("\x1b[K\r\n");
            continue;
        }
        const ProcInfo *p = proclist_get(pl, idx);
        const char *pfx = proclist_get_prefix(pl, idx);
        char mem[24];
        fmt_bytes(p->mem_bytes, mem, sizeof(mem));
        bool tagged = tagset_contains(&ctx->tags, p->pid);
        const char *mark = tagged ? "*" : "";

        if (idx == nav->sel) {
            term_printf("\x1b[7m%-7u %6.1f %9s %5u  %s%s%s\x1b[K\x1b[0m\r\n",
                        p->pid, p->cpu_percent, mem, p->threads, mark, pfx, p->name);
        } else if (tagged) {
            term_printf("\x1b[1;33m%-7u %6.1f %9s %5u  %s%s%s\x1b[K\x1b[0m\r\n",
                        p->pid, p->cpu_percent, mem, p->threads, mark, pfx, p->name);
        } else {
            const char *cpu_color = "";
            if (p->cpu_percent >= 50.0)      cpu_color = VT_FG_RED;
            else if (p->cpu_percent >= 10.0) cpu_color = VT_FG_YELLOW;
            term_printf("%-7u %s%6.1f%s %9s %5u  %s%s%s%s\x1b[K\r\n",
                        p->pid, cpu_color, p->cpu_percent, VT_RESET,
                        mem, p->threads, VT_DIM, pfx, VT_RESET, p->name);
        }
    }

    proc_footer(ctx);

    term_frame_end();
}

/* -------------------------------- input --------------------------------- */

static bool proc_mouse(AppContext *ctx, const InputEvent *ev)
{
    NavState *nav = &ctx->nav;
    Layout L = ctx->L;
    size_t n = proclist_count(ctx->pl);

    if (ev->button == MBTN_WHEEL_UP) {
        nav->sel = (nav->sel > 3) ? nav->sel - 3 : 0;
    } else if (ev->button == MBTN_WHEEL_DOWN) {
        nav->sel += 3;
        if (n && nav->sel >= n) nav->sel = n - 1;
    } else if (ev->button == MBTN_LEFT && ev->pressed) {
        int t = hitmap_at(&ctx->tab_hits, ev->mx, ev->my);
        if (t >= 0) { ctx->ui.view = (AppView)t; return true; }

        int a = hitmap_at(&ctx->keybar_hits, ev->mx, ev->my);
        if (a != -1) {
            if (a != PA_NONE)
                return run_action(ctx, a);
            return true;
        }

        if (ev->my == L.header_lines - 1) {
            sort_by(ctx, header_field_at(ev->mx));
        } else if (ev->my >= L.header_lines &&
                   ev->my < L.header_lines + L.visible_rows) {
            size_t idx = nav->scroll + (size_t)(ev->my - L.header_lines);
            if (idx < n) nav->sel = idx;
        }
    }

    ui_clamp_view(nav, n, L.visible_rows);
    if (n)
        nav->sel_pid = proclist_get(ctx->pl, nav->sel)->pid;
    return true;
}

static bool proc_handle(AppContext *ctx, const InputEvent *ev, uint64_t now)
{
    UiState *ui = &ctx->ui;
    ProcList *pl = ctx->pl;
    NavState *nav = &ctx->nav;
    int visible = ctx->L.visible_rows;

    /* A pending confirmation captures the next event and nothing else. */
    if (ui->confirm_kill) {
        confirm_kill(ctx, ev, now);
        return true;
    }

    /* While editing the filter, keys edit text rather than navigate. */
    if (ui->filter_input) {
        filter_event(ctx, ev);
        ui_reanchor(nav, pl);
        ui_clamp_view(nav, proclist_count(pl), visible);
        return true;
    }

    if (ev->type == EV_MOUSE)
        return proc_mouse(ctx, ev);

    if (ev->type != EV_KEY)
        return true;

    size_t n = proclist_count(pl);

    switch (ev->key) {
        case KEY_UP:   if (nav->sel > 0) nav->sel--; break;
        case KEY_DOWN: if (nav->sel + 1 < n) nav->sel++; break;
        case KEY_PGUP:
            nav->sel = (nav->sel > (size_t)visible) ? nav->sel - (size_t)visible : 0;
            break;
        case KEY_PGDN:
            nav->sel += (size_t)visible;
            if (n && nav->sel >= n) nav->sel = n - 1;
            break;
        case KEY_HOME: nav->sel = 0; break;
        case KEY_END:  if (n) nav->sel = n - 1; break;
        case KEY_F1:   ui->help_open = true; break;
        case KEY_F2:   ui->view = VIEW_SETUP; ui->setup_sel = 0; break;
        case KEY_F4:   ui->filter_input = true; break;
        case KEY_F5:   toggle_tree(ctx); n = proclist_count(pl); break;
        case KEY_F6:   ui->view = VIEW_IO; break;
        case KEY_F9:   request_kill(ctx); break;
        case KEY_TAB:  ui->view = VIEW_IO; break;
        case KEY_ESC:  return false;
        case KEY_CHAR:
            switch (ev->ch) {
                case 'q': case 'Q': return false;
                case '/': ui->filter_input = true; break;
                case 't': case 'T': toggle_tree(ctx); n = proclist_count(pl); break;
                case 'k': if (nav->sel > 0) nav->sel--; break;
                case 'j': if (nav->sel + 1 < n) nav->sel++; break;
                case 'g': nav->sel = 0; break;
                case 'G': if (n) nav->sel = n - 1; break;
                case '<': cycle_sort(ctx, -1); break;
                case '>': cycle_sort(ctx, +1); break;
                case '?': ui->help_open = true; break;
                case ' ': tag_selected(ctx); n = proclist_count(pl); break;
                default: break;
            }
            break;
        default:
            break;
    }

    ui_clamp_view(nav, n, visible);
    if (n)
        nav->sel_pid = proclist_get(pl, nav->sel)->pid;
    return true;
}

const ViewVTable VIEW_PROC_VTABLE = { proc_render, proc_handle };
