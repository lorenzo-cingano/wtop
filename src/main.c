/*
 * wtop - an htop-style process viewer for Windows.
 * Copyright (c) 2026 Cingano Development. All rights reserved.
 */

#include "terminal.h"
#include "sysinfo.h"
#include "proclist.h"
#include "iostat.h"
#include "config.h"
#include "install.h"

#include <windows.h>
#include <conio.h>
#include <stdio.h>
#include <string.h>

#define POLL_MS 30

/* Which screen is showing. */
typedef enum {
    MODE_PROCESSES,
    MODE_IO,
    MODE_SETUP,
} AppMode;

static volatile bool g_running = true;

static BOOL WINAPI ctrl_handler(DWORD type)
{
    (void)type;
    g_running = false;
    return TRUE;
}

/* Navigation state. We follow the selected process by PID so the highlight
 * stays on the same process even as the sort order shifts between samples. */
typedef struct {
    size_t   sel;        /* selected row index into the sorted list */
    size_t   scroll;     /* index of the first visible row */
    uint32_t sel_pid;    /* PID under the selection, for re-anchoring */
} NavState;

/* Transient UI state: the current screen, the kill confirmation prompt, and a
 * short-lived status line shown in place of the key bar. */
typedef struct {
    AppMode  mode;
    int      setup_sel;                    /* selected row in the setup screen */
    bool     confirm_kill;                 /* showing "kill? [y/N]" prompt */
    uint32_t kill_pid;                     /* PID captured when prompting */
    char     kill_name[WTOP_MAX_PROC_NAME];
    char     status[256];                  /* result message, "" when none */
    uint64_t status_until;                 /* tick after which to clear it */
    bool     filter_input;                 /* editing the filter text field */
    char     filter[128];                  /* active name filter, "" = none */
} UiState;

/* Computed layout for the current frame. */
typedef struct {
    int core_cols;       /* per-core meters per row */
    int core_rows;       /* rows of per-core meters */
    int cpu_lines;       /* lines the CPU section occupies (grid or 1 bar) */
    int header_lines;    /* everything above the process rows */
    int visible_rows;    /* process rows that fit */
} Layout;

static Layout compute_layout(TermSize ts, uint32_t num_cpus, bool per_core)
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

    /* CPU section + Mem meter + summary + column header. */
    L.header_lines = L.cpu_lines + 3;

    L.visible_rows = ts.height - L.header_lines - 1 /* footer */;
    if (L.visible_rows < 1) L.visible_rows = 1;
    return L;
}

/* Human-readable byte size. */
static void fmt_bytes(uint64_t bytes, char *out, size_t n)
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

/* "12.3M/s" style throughput from a bytes-per-second rate. */
static void fmt_rate(double bps, char *out, size_t n)
{
    char b[16];
    if (bps < 0) bps = 0;
    fmt_bytes((uint64_t)(bps + 0.5), b, sizeof(b));
    snprintf(out, n, "%s/s", b);
}

/* Draw an htop-style meter: label[|||||      val] across `width` columns,
 * emitting no trailing newline so meters can be tiled side by side. */
static void draw_meter(const char *label, double fraction,
                       const char *text, int width)
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

/* One entry in the bottom function-key bar. */
typedef struct {
    const char *key;
    const char *label;
} KeyBind;

static const KeyBind KEYBINDS[] = {
    { "Up/Dn", "Select"  },
    { "PgUp/PgDn", "Page" },
    { "Home/End", "Jump"  },
    { "F2",    "Setup"   },
    { "F4",    "Filter"  },
    { "F5",    "Tree"    },
    { "F9",    "Kill"    },
    { "Tab",   "I/O"     },
    { "q",     "Quit"    },
};

#define STATUS_MS 4000

/* Terminate the process captured in the kill prompt and report the outcome
 * into ui->status. Windows has no signals, so this is a hard TerminateProcess. */
static void kill_process(UiState *ui, uint64_t now)
{
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, ui->kill_pid);
    if (!h) {
        snprintf(ui->status, sizeof(ui->status),
                 "Cannot open PID %u (%.64s) to terminate (error %lu)",
                 ui->kill_pid, ui->kill_name, GetLastError());
    } else {
        if (TerminateProcess(h, 1))
            snprintf(ui->status, sizeof(ui->status),
                     "Terminated PID %u (%.64s)", ui->kill_pid, ui->kill_name);
        else
            snprintf(ui->status, sizeof(ui->status),
                     "Failed to terminate PID %u (%.64s): error %lu",
                     ui->kill_pid, ui->kill_name, GetLastError());
        CloseHandle(h);
    }
    ui->status_until = now + STATUS_MS;
}

/* Display width of a UTF-8 string, counting each multi-byte sequence as a
 * single column. Good enough for the ASCII + arrow glyphs we use here. */
static int display_width(const char *s)
{
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if ((*p & 0xC0) != 0x80) /* not a UTF-8 continuation byte */
            w++;
    return w;
}

/* htop-style key bar: each binding is a highlighted key glyph followed by a
 * label on a cyan field, dropped from the right if the row is too narrow. */
static void render_keybar(TermSize ts)
{
    int n = (int)(sizeof(KEYBINDS) / sizeof(KEYBINDS[0]));
    int used = 0;

    for (int i = 0; i < n; i++) {
        /* " key " + " label " plus the spaces we add around each. */
        int seg = display_width(KEYBINDS[i].key)
                + display_width(KEYBINDS[i].label) + 4;
        if (used + seg > ts.width)
            break;

        /* Key: bold black on a light "button" (bright white bg).
         * Label: bold bright-white on blue, for strong contrast. */
        term_printf("\x1b[0m\x1b[1;30;107m %s \x1b[1;97;44m %s ",
                    KEYBINDS[i].key, KEYBINDS[i].label);
        used += seg;
    }

    /* Extend the blue field to the end of the row, then reset. */
    term_printf("\x1b[44m\x1b[K\x1b[0m");
}

/* The bottom row: a kill confirmation, a transient status line, or the
 * normal key bar - in that priority order. */
static void render_footer(TermSize ts, const UiState *ui)
{
    if (ui->confirm_kill) {
        /* Attention colours: bold white on red, filled to EOL. */
        term_printf("\x1b[1;97;41m Kill process %u (%s)?  [y/N] \x1b[K\x1b[0m",
                    ui->kill_pid, ui->kill_name);
        return;
    }
    if (ui->filter_input) {
        /* Editable text field: bold black on green, with a block cursor,
         * then a hint on the default background. */
        term_printf("\x1b[1;30;42m Filter: %s\xe2\x96\x88 \x1b[0m"
                    "%s  Enter apply   Esc clear%s\x1b[K",
                    ui->filter, VT_DIM, VT_RESET);
        return;
    }
    if (ui->status[0]) {
        term_printf("\x1b[1;97;44m %s \x1b[K\x1b[0m", ui->status);
        return;
    }
    render_keybar(ts);
}

/* Column header names mapped to their sort field, for setup + the table. */
static const char *SORT_NAMES[PROC_SORT_COUNT] = {
    "CPU%", "Memory", "PID", "Threads", "Name"
};

static void render(const SysInfo *sys, ProcList *pl, TermSize ts,
                   const NavState *nav, const UiState *ui,
                   const Config *cfg, Layout L)
{
    term_frame_begin();

    char label[16], val[16];

    if (cfg->show_per_core) {
        /* --- Per-core CPU meter grid --- */
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
        /* --- Single aggregate CPU meter --- */
        snprintf(val, sizeof(val), "%.1f%%", sys->cpu_usage * 100.0);
        draw_meter("CPU", sys->cpu_usage, val, ts.width);
        term_puts("\x1b[K\r\n");
    }

    /* --- Memory meter --- */
    double mem_frac = sys->mem_total ? (double)sys->mem_used / (double)sys->mem_total : 0;
    char used[24], total[24], buf[64];
    fmt_bytes(sys->mem_used, used, sizeof(used));
    fmt_bytes(sys->mem_total, total, sizeof(total));
    snprintf(buf, sizeof(buf), "%s/%s", used, total);
    draw_meter("Mem", mem_frac, buf, ts.width);
    term_puts("\x1b[K\r\n");

    /* --- Summary --- */
    term_printf("%s%u CPUs   avg %.1f%%   ",
                VT_DIM, sys->num_cpus, sys->cpu_usage * 100.0);
    if (ui->filter[0])
        term_printf("%zu/%zu match \"%s\"   ",
                    proclist_count(pl), proclist_total(pl), ui->filter);
    else
        term_printf("%zu processes   ", proclist_count(pl));
    term_printf("sorted by %s%s%s\x1b[K\r\n",
                SORT_NAMES[cfg->sort_field],
                cfg->tree_view ? "   [tree]" : "", VT_RESET);

    /* --- Column header; the active sort column is highlighted. --- */
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

    /* --- Process rows --- */
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

        if (idx == nav->sel) {
            /* Reverse-video highlight; \x1b[K fills the row to its end. */
            term_printf("\x1b[7m%-7u %6.1f %9s %5u  %s%s\x1b[K\x1b[0m\r\n",
                        p->pid, p->cpu_percent, mem, p->threads, pfx, p->name);
        } else {
            const char *cpu_color = "";
            if (p->cpu_percent >= 50.0)      cpu_color = VT_FG_RED;
            else if (p->cpu_percent >= 10.0) cpu_color = VT_FG_YELLOW;
            term_printf("%-7u %s%6.1f%s %9s %5u  %s%s%s%s\x1b[K\r\n",
                        p->pid, cpu_color, p->cpu_percent, VT_RESET,
                        mem, p->threads, VT_DIM, pfx, VT_RESET, p->name);
        }
    }

    /* --- Footer: prompt / status / key bar --- */
    render_footer(ts, ui);

    term_frame_end();
}

/* ====================== Setup / configuration screen ====================== */

/* Selectable refresh intervals, in milliseconds. */
static const uint32_t REFRESH_PRESETS[] = {
    250, 500, 1000, 1500, 2000, 3000, 5000, 10000
};
#define REFRESH_PRESET_COUNT \
    ((int)(sizeof(REFRESH_PRESETS) / sizeof(REFRESH_PRESETS[0])))

#define SETUP_OPT_COUNT 5

/* Index of the preset nearest to the current refresh value. */
static int refresh_preset_index(uint32_t ms)
{
    int best = 0;
    uint32_t best_diff = 0xFFFFFFFFu;
    for (int i = 0; i < REFRESH_PRESET_COUNT; i++) {
        uint32_t d = (REFRESH_PRESETS[i] > ms)
                         ? REFRESH_PRESETS[i] - ms : ms - REFRESH_PRESETS[i];
        if (d < best_diff) { best_diff = d; best = i; }
    }
    return best;
}

/* Render the current value of a setup option into buf. */
static void setup_value(const Config *c, int opt, char *buf, size_t n)
{
    switch (opt) {
    case 0:
        if (c->refresh_ms < 1000)
            snprintf(buf, n, "%u ms", c->refresh_ms);
        else
            snprintf(buf, n, "%.1f s", c->refresh_ms / 1000.0);
        break;
    case 1: snprintf(buf, n, "%s", SORT_NAMES[c->sort_field]); break;
    case 2: snprintf(buf, n, "%s",
                     c->sort_descending ? "Descending" : "Ascending"); break;
    case 3: snprintf(buf, n, "%s", c->show_per_core ? "On" : "Off"); break;
    case 4: snprintf(buf, n, "%s", c->tree_view ? "On" : "Off"); break;
    default: buf[0] = '\0'; break;
    }
}

/* Apply a left/right adjustment (delta -1/+1) to the selected option.
 * Sort changes are pushed into the process list immediately. */
static void setup_adjust(Config *c, ProcList *pl, int opt, int delta)
{
    switch (opt) {
    case 0: {
        int i = refresh_preset_index(c->refresh_ms) + delta;
        if (i < 0) i = 0;
        if (i >= REFRESH_PRESET_COUNT) i = REFRESH_PRESET_COUNT - 1;
        c->refresh_ms = REFRESH_PRESETS[i];
        break;
    }
    case 1: {
        int f = (int)c->sort_field + delta;
        f = (f % PROC_SORT_COUNT + PROC_SORT_COUNT) % PROC_SORT_COUNT;
        c->sort_field = (ProcSortField)f;
        proclist_set_sort(pl, c->sort_field, c->sort_descending);
        break;
    }
    case 2:
        c->sort_descending = !c->sort_descending;       /* two-state toggle */
        proclist_set_sort(pl, c->sort_field, c->sort_descending);
        break;
    case 3:
        c->show_per_core = !c->show_per_core;
        break;
    case 4:
        c->tree_view = !c->tree_view;
        proclist_set_tree(pl, c->tree_view);
        break;
    }
}

static void render_setup(const Config *c, const UiState *ui, TermSize ts)
{
    static const char *labels[SETUP_OPT_COUNT] = {
        "Refresh interval",
        "Sort by",
        "Sort order",
        "Per-core CPU meters",
        "Tree view",
    };
    static const char *help[SETUP_OPT_COUNT] = {
        "How often the display samples and refreshes.",
        "Which column orders the process list.",
        "Highest first (Descending) or lowest first (Ascending).",
        "Show one meter per CPU, or a single combined meter.",
        "Show processes as a parent/child tree, or a flat list.",
    };

    term_frame_begin();

    term_printf("\x1b[1;97;44m  wtop setup \x1b[K\x1b[0m\r\n");
    term_puts("\x1b[K\r\n");

    char val[32];
    for (int i = 0; i < SETUP_OPT_COUNT; i++) {
        setup_value(c, i, val, sizeof(val));
        if (i == ui->setup_sel)
            term_printf("\x1b[7m  %-22s : %-12s\x1b[K\x1b[0m\r\n",
                        labels[i], val);
        else
            term_printf("  %-22s : %s%-12s%s\x1b[K\r\n",
                        labels[i], VT_FG_CYAN, val, VT_RESET);
    }

    term_puts("\x1b[K\r\n");
    term_printf("%s  %s%s\x1b[K\r\n",
                VT_DIM, help[ui->setup_sel], VT_RESET);
    term_puts("\x1b[K\r\n");
    term_printf("%s  wtop \xc2\xa9 2026 Cingano Development%s\x1b[K\r\n",
                VT_DIM, VT_RESET);

    /* Pad to the footer. */
    int used = 2 /* title+blank */ + SETUP_OPT_COUNT
             + 2 /* blank+help */ + 2 /* blank+copyright */;
    for (int i = used; i < ts.height - 1; i++)
        term_puts("\x1b[K\r\n");

    term_printf("\x1b[1;97;44m"
                " Up/Dn \x1b[0m\x1b[1;97;44mSelect "
                " Left/Right \x1b[0m\x1b[1;97;44mChange "
                " F2/Esc \x1b[0m\x1b[1;97;44mBack \x1b[K\x1b[0m");

    term_frame_end();
}

/* Handle a key while the setup screen is showing. Returns false to quit. */
static bool handle_setup_key(UiState *ui, Config *c, ProcList *pl)
{
    int ch = _getch();

    if (ch == 0 || ch == 224) {
        int k = _getch();
        switch (k) {
            case 72: if (ui->setup_sel > 0) ui->setup_sel--; break;   /* up */
            case 80: if (ui->setup_sel + 1 < SETUP_OPT_COUNT) ui->setup_sel++;
                     break;                                           /* down */
            case 75: setup_adjust(c, pl, ui->setup_sel, -1); break;   /* left */
            case 77: setup_adjust(c, pl, ui->setup_sel, +1); break;   /* right */
            case 60: ui->mode = MODE_PROCESSES; break;                /* F2 */
        }
    } else {
        switch (ch) {
            case 27:            /* Esc */
            case 'q': case 'Q':
                ui->mode = MODE_PROCESSES; break;
            case 'k': if (ui->setup_sel > 0) ui->setup_sel--; break;
            case 'j': if (ui->setup_sel + 1 < SETUP_OPT_COUNT) ui->setup_sel++;
                      break;
            case 'h': setup_adjust(c, pl, ui->setup_sel, -1); break;
            case 'l': setup_adjust(c, pl, ui->setup_sel, +1); break;
            case 13:  case ' ':  /* Enter / Space cycle forward */
                setup_adjust(c, pl, ui->setup_sel, +1); break;
        }
    }
    return true;
}

/* =============================== I/O screen =============================== */

/* Full-screen network + disk throughput view (the "I/O tab"). Aggregate
 * meters auto-scale against a decaying peak, with per-interface and
 * per-physical-disk breakdowns underneath. */
static void render_io(const IoStat *io, TermSize ts)
{
    term_frame_begin();

    term_printf("\x1b[1;97;44m  wtop - I/O \x1b[K\x1b[0m\r\n");
    term_puts("\x1b[K\r\n");
    int used = 2;

    char r1[24], r2[24];

    /* --- Network --- */
    term_printf("%sNetwork%s\x1b[K\r\n", VT_BOLD, VT_RESET);
    used++;
    if (io->net_ok) {
        fmt_rate(io->net_rx_bps, r1, sizeof(r1));
        draw_meter("Rx ", io->net_scale ? io->net_rx_bps / io->net_scale : 0,
                   r1, ts.width);
        term_puts("\x1b[K\r\n");
        fmt_rate(io->net_tx_bps, r2, sizeof(r2));
        draw_meter("Tx ", io->net_scale ? io->net_tx_bps / io->net_scale : 0,
                   r2, ts.width);
        term_puts("\x1b[K\r\n");
        used += 2;

        term_printf("%s  %-22s %12s %12s%s\x1b[K\r\n",
                    VT_DIM, "INTERFACE", "RX/s", "TX/s", VT_RESET);
        used++;
        for (int i = 0; i < io->nic_count; i++) {
            fmt_rate(io->nics[i].rx_bps, r1, sizeof(r1));
            fmt_rate(io->nics[i].tx_bps, r2, sizeof(r2));
            term_printf("  %-22.22s %12s %12s\x1b[K\r\n",
                        io->nics[i].name, r1, r2);
            used++;
        }
    } else {
        term_printf("%s  (network counters unavailable)%s\x1b[K\r\n",
                    VT_DIM, VT_RESET);
        used++;
    }

    term_puts("\x1b[K\r\n");
    used++;

    /* --- Disk --- */
    term_printf("%sDisk%s\x1b[K\r\n", VT_BOLD, VT_RESET);
    used++;
    if (io->disk_ok) {
        fmt_rate(io->disk_read_bps, r1, sizeof(r1));
        draw_meter("Rd ", io->disk_scale ? io->disk_read_bps / io->disk_scale : 0,
                   r1, ts.width);
        term_puts("\x1b[K\r\n");
        fmt_rate(io->disk_write_bps, r2, sizeof(r2));
        draw_meter("Wr ", io->disk_scale ? io->disk_write_bps / io->disk_scale : 0,
                   r2, ts.width);
        term_puts("\x1b[K\r\n");
        used += 2;

        term_printf("%s  %-10s %12s %12s %7s%s\x1b[K\r\n",
                    VT_DIM, "DISK", "READ/s", "WRITE/s", "BUSY", VT_RESET);
        used++;
        for (int i = 0; i < io->disk_count; i++) {
            char nm[16];
            snprintf(nm, sizeof(nm), "Disk %d", io->disks[i].index);
            fmt_rate(io->disks[i].read_bps, r1, sizeof(r1));
            fmt_rate(io->disks[i].write_bps, r2, sizeof(r2));
            term_printf("  %-10s %12s %12s %6.0f%%\x1b[K\r\n",
                        nm, r1, r2, io->disks[i].busy * 100.0);
            used++;
        }
    } else {
        term_printf("%s  (disk counters unavailable)%s\x1b[K\r\n",
                    VT_DIM, VT_RESET);
        used++;
    }

    /* Pad to the footer. */
    for (int i = used; i < ts.height - 1; i++)
        term_puts("\x1b[K\r\n");

    term_printf("\x1b[1;97;44m"
                " Tab \x1b[0m\x1b[1;97;44mProcesses "
                " Esc \x1b[0m\x1b[1;97;44mBack "
                " q \x1b[0m\x1b[1;97;44mQuit \x1b[K\x1b[0m");

    term_frame_end();
}

/* Handle a key while the I/O screen is showing. Returns false to quit. */
static bool handle_io_key(UiState *ui)
{
    int ch = _getch();
    if (ch == 0 || ch == 224) {
        int k = _getch();
        if (k == 64)                       /* F6: toggle back to processes */
            ui->mode = MODE_PROCESSES;
        return true;
    }
    switch (ch) {
        case 9:                            /* Tab */
        case 27:                           /* Esc */
            ui->mode = MODE_PROCESSES;
            break;
        case 'q': case 'Q':
            return false;
    }
    return true;
}

/* Keep scroll so the selected row stays within the visible window. */
static void clamp_view(NavState *nav, size_t count, int visible_rows)
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

/* After a fresh sample, re-find the selected PID in the new ordering. */
static void reanchor_selection(NavState *nav, ProcList *pl)
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

/* Begin the kill confirmation for the currently selected process. */
static void request_kill(NavState *nav, ProcList *pl, UiState *ui)
{
    if (proclist_count(pl) == 0)
        return;
    const ProcInfo *p = proclist_get(pl, nav->sel);
    ui->confirm_kill = true;
    ui->kill_pid = p->pid;
    snprintf(ui->kill_name, sizeof(ui->kill_name), "%s", p->name);
}

/* Toggle tree view, persist the choice, and re-anchor the selection. */
static void toggle_tree(NavState *nav, ProcList *pl, Config *cfg)
{
    cfg->tree_view = !cfg->tree_view;
    proclist_set_tree(pl, cfg->tree_view);
    reanchor_selection(nav, pl);
}

/* Returns false if the key was a quit request. */
static bool handle_key(NavState *nav, ProcList *pl, UiState *ui, Config *cfg,
                       int visible_rows, uint64_t now)
{
    int c = _getch();

    /* When a confirmation is up, the next key answers it and nothing else. */
    if (ui->confirm_kill) {
        ui->confirm_kill = false;
        if (c == 'y' || c == 'Y') {
            kill_process(ui, now);
        } else {
            snprintf(ui->status, sizeof(ui->status), "Kill cancelled");
            ui->status_until = now + STATUS_MS;
        }
        return true;
    }

    /* While editing the filter, keys edit the text rather than navigate. */
    if (ui->filter_input) {
        if (c == 13) {                       /* Enter: apply and leave edit */
            ui->filter_input = false;
        } else if (c == 27) {                /* Esc: clear the filter */
            ui->filter[0] = '\0';
            ui->filter_input = false;
            proclist_set_filter(pl, ui->filter);
        } else if (c == 8) {                 /* Backspace */
            size_t len = strlen(ui->filter);
            if (len) ui->filter[len - 1] = '\0';
            proclist_set_filter(pl, ui->filter);
        } else if (c >= 32 && c < 127) {     /* printable: append */
            size_t len = strlen(ui->filter);
            if (len + 1 < sizeof(ui->filter)) {
                ui->filter[len] = (char)c;
                ui->filter[len + 1] = '\0';
            }
            proclist_set_filter(pl, ui->filter);
        } else if (c == 0 || c == 224) {
            _getch();                        /* swallow extended-key payload */
        }
        reanchor_selection(nav, pl);
        clamp_view(nav, proclist_count(pl), visible_rows);
        return true;
    }

    size_t n = proclist_count(pl);

    if (c == 0 || c == 224) {
        /* Extended key: arrows, page, home/end, function keys. */
        int k = _getch();
        switch (k) {
            case 72: if (nav->sel > 0) nav->sel--; break;             /* up */
            case 80: if (nav->sel + 1 < n) nav->sel++; break;         /* down */
            case 73: /* PgUp */
                nav->sel = (nav->sel > (size_t)visible_rows)
                               ? nav->sel - (size_t)visible_rows : 0;
                break;
            case 81: /* PgDn */
                nav->sel += (size_t)visible_rows;
                if (n && nav->sel >= n) nav->sel = n - 1;
                break;
            case 71: nav->sel = 0; break;                             /* Home */
            case 79: if (n) nav->sel = n - 1; break;                  /* End */
            case 60: ui->mode = MODE_SETUP; ui->setup_sel = 0; break; /* F2 */
            case 62: ui->filter_input = true; break;                  /* F4 */
            case 63: toggle_tree(nav, pl, cfg); n = proclist_count(pl);
                     break;                                           /* F5 */
            case 64: ui->mode = MODE_IO; break;                       /* F6 */
            case 67: request_kill(nav, pl, ui); break;                /* F9 */
        }
    } else {
        switch (c) {
            case 'q': case 'Q': case 27: return false;
            case 9: ui->mode = MODE_IO; break;                        /* Tab */
            case '/': ui->filter_input = true; break;                 /* filter */
            case 't': case 'T':                                       /* tree */
                toggle_tree(nav, pl, cfg); n = proclist_count(pl); break;
            case 'k': if (nav->sel > 0) nav->sel--; break;            /* vim up */
            case 'j': if (nav->sel + 1 < n) nav->sel++; break;        /* vim down */
            case 'g': nav->sel = 0; break;
            case 'G': if (n) nav->sel = n - 1; break;
        }
    }

    clamp_view(nav, n, visible_rows);
    if (n)
        nav->sel_pid = proclist_get(pl, nav->sel)->pid;
    return true;
}

/* One-line usage, printed for --help or an unknown option. */
static void print_usage(FILE *out)
{
    fprintf(out,
        "wtop - an htop-style process viewer for Windows.\n"
        "\n"
        "Usage: wtop [option]\n"
        "  (no option)   run the interactive process viewer\n"
        "  --install     copy wtop into %%APPDATA%%\\wtop and add it to PATH\n"
        "  --uninstall   remove the installed copy and the PATH entry\n"
        "  -h, --help    show this help\n");
}

int main(int argc, char **argv)
{
    /* Command-line modes run and exit before any terminal setup. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--install") == 0)
            return wtop_install();
        if (strcmp(argv[i], "--uninstall") == 0)
            return wtop_uninstall();
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(stdout);
            return 0;
        }
        fprintf(stderr, "wtop: unknown option '%s'\n\n", argv[i]);
        print_usage(stderr);
        return 2;
    }

    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    if (!term_init()) {
        fprintf(stderr, "wtop: this console does not support VT processing.\n");
        return 1;
    }

    sysinfo_init();
    iostat_init();
    ProcList *pl = proclist_create();
    if (!pl) {
        term_shutdown();
        return 1;
    }

    Config cfg;
    config_load(&cfg);

    NavState nav = { 0, 0, 0 };
    UiState ui = { 0 };
    SysInfo sys;
    IoStat io;

    proclist_set_sort(pl, cfg.sort_field, cfg.sort_descending);
    proclist_set_tree(pl, cfg.tree_view);

    /* Prime the samples so the first frame shows real CPU deltas. */
    sysinfo_sample(&sys);
    proclist_sample(pl);
    iostat_sample(&io);
    Sleep(200);
    sysinfo_sample(&sys);
    proclist_sample(pl);
    iostat_sample(&io);
    reanchor_selection(&nav, pl);

    TermSize ts = term_size();
    Layout L = compute_layout(ts, sys.num_cpus, cfg.show_per_core);
    clamp_view(&nav, proclist_count(pl), L.visible_rows);
    render(&sys, pl, ts, &nav, &ui, &cfg, L);

    uint64_t last_sample = GetTickCount64();

    while (g_running) {
        bool need_render = false;
        uint64_t now = GetTickCount64();

        /* Drain pending input first for snappy navigation. */
        while (_kbhit()) {
            bool keep;
            if (ui.mode == MODE_SETUP)
                keep = handle_setup_key(&ui, &cfg, pl);
            else if (ui.mode == MODE_IO)
                keep = handle_io_key(&ui);
            else
                keep = handle_key(&nav, pl, &ui, &cfg, L.visible_rows, now);
            if (!keep) {
                g_running = false;
                break;
            }
            need_render = true;
        }
        if (!g_running)
            break;

        /* Expire the transient status line. */
        if (ui.status[0] && now >= ui.status_until) {
            ui.status[0] = '\0';
            need_render = true;
        }

        if (now - last_sample >= cfg.refresh_ms) {
            sysinfo_sample(&sys);
            proclist_sample(pl);
            iostat_sample(&io);
            reanchor_selection(&nav, pl);
            last_sample = now;
            need_render = true;
        }

        if (need_render) {
            ts = term_size();
            L = compute_layout(ts, sys.num_cpus, cfg.show_per_core);
            if (ui.mode == MODE_SETUP) {
                render_setup(&cfg, &ui, ts);
            } else if (ui.mode == MODE_IO) {
                render_io(&io, ts);
            } else {
                clamp_view(&nav, proclist_count(pl), L.visible_rows);
                render(&sys, pl, ts, &nav, &ui, &cfg, L);
            }
        }

        Sleep(POLL_MS);
    }

    config_save(&cfg);
    proclist_destroy(pl);
    term_shutdown();
    return 0;
}
