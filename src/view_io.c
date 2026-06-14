#include "view_io.h"
#include "widgets.h"

#include <stdio.h>

/* Keybar actions for the I/O screen. */
enum { IA_NONE = 0, IA_PROC, IA_QUIT };

static const KeyBind IO_KEYS[] = {
    { "Tab", "Processes", IA_PROC },
    { "Esc", "Back",      IA_PROC },
    { "q",   "Quit",      IA_QUIT },
};
#define IO_KEY_COUNT ((int)(sizeof(IO_KEYS) / sizeof(IO_KEYS[0])))

static void io_render(AppContext *ctx)
{
    const IoStat *io = ctx->io;
    TermSize ts = ctx->ts;

    term_frame_begin();

    render_tabbar(ts, VIEW_IO, &ctx->tab_hits);
    term_puts("\r\n");
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

    render_keybar(ts, IO_KEYS, IO_KEY_COUNT, &ctx->keybar_hits);

    term_frame_end();
}

/* Run a keybar action shared by key and click. Returns false to quit. */
static bool io_action(AppContext *ctx, int action)
{
    switch (action) {
        case IA_PROC: ctx->ui.view = VIEW_PROCESSES; return true;
        case IA_QUIT: return false;
        default:      return true;
    }
}

static bool io_handle(AppContext *ctx, const InputEvent *ev, uint64_t now)
{
    (void)now;

    if (ev->type == EV_MOUSE) {
        if (ev->button == MBTN_LEFT && ev->pressed) {
            int t = hitmap_at(&ctx->tab_hits, ev->mx, ev->my);
            if (t >= 0) { ctx->ui.view = (AppView)t; return true; }
            int a = hitmap_at(&ctx->keybar_hits, ev->mx, ev->my);
            if (a > 0) return io_action(ctx, a);
        }
        return true;
    }

    if (ev->type != EV_KEY)
        return true;

    switch (ev->key) {
        case KEY_TAB:
        case KEY_ESC:
        case KEY_F6:
            ctx->ui.view = VIEW_PROCESSES;
            break;
        case KEY_F1:
            ctx->ui.help_open = true;
            break;
        case KEY_CHAR:
            if (ev->ch == 'q' || ev->ch == 'Q')
                return false;
            if (ev->ch == '?')
                ctx->ui.help_open = true;
            break;
        default:
            break;
    }
    return true;
}

const ViewVTable VIEW_IO_VTABLE = { io_render, io_handle };
