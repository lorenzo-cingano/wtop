#include "view_setup.h"
#include "widgets.h"

#include <stdio.h>

/* Selectable refresh intervals, in milliseconds. */
static const uint32_t REFRESH_PRESETS[] = {
    250, 500, 1000, 1500, 2000, 3000, 5000, 10000
};
#define REFRESH_PRESET_COUNT \
    ((int)(sizeof(REFRESH_PRESETS) / sizeof(REFRESH_PRESETS[0])))

#define SETUP_OPT_COUNT 5
#define SETUP_FIRST_ROW 2          /* title + blank line precede the options */
#define SETUP_VALUE_COL 27         /* "  %-22s : " -> value starts here */

/* Keybar actions for the setup screen. */
enum { SS_NONE = 0, SS_BACK };

static const KeyBind SETUP_KEYS[] = {
    { "Up/Dn",      "Select", SS_NONE },
    { "Left/Right", "Change", SS_NONE },
    { "F2/Esc",     "Back",   SS_BACK },
};
#define SETUP_KEY_COUNT ((int)(sizeof(SETUP_KEYS) / sizeof(SETUP_KEYS[0])))

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

/* Apply a left/right adjustment (delta -1/+1) to the selected option. Sort
 * changes are pushed into the process list immediately. */
static void setup_adjust(AppContext *ctx, int opt, int delta)
{
    Config *c = ctx->cfg;
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
        proclist_set_sort(ctx->pl, c->sort_field, c->sort_descending);
        break;
    }
    case 2:
        c->sort_descending = !c->sort_descending;       /* two-state toggle */
        proclist_set_sort(ctx->pl, c->sort_field, c->sort_descending);
        break;
    case 3:
        c->show_per_core = !c->show_per_core;
        break;
    case 4:
        c->tree_view = !c->tree_view;
        proclist_set_tree(ctx->pl, c->tree_view);
        break;
    }
}

static void setup_render(AppContext *ctx)
{
    static const char *const labels[SETUP_OPT_COUNT] = {
        "Refresh interval",
        "Sort by",
        "Sort order",
        "Per-core CPU meters",
        "Tree view",
    };
    static const char *const help[SETUP_OPT_COUNT] = {
        "How often the display samples and refreshes.",
        "Which column orders the process list.",
        "Highest first (Descending) or lowest first (Ascending).",
        "Show one meter per CPU, or a single combined meter.",
        "Show processes as a parent/child tree, or a flat list.",
    };

    const Config *c = ctx->cfg;
    TermSize ts = ctx->ts;

    term_frame_begin();

    term_printf("\x1b[1;97;44m  wtop setup \x1b[K\x1b[0m\r\n");
    term_puts("\x1b[K\r\n");

    char val[32];
    for (int i = 0; i < SETUP_OPT_COUNT; i++) {
        setup_value(c, i, val, sizeof(val));
        if (i == ctx->ui.setup_sel)
            term_printf("\x1b[7m  %-22s : %-12s\x1b[K\x1b[0m\r\n",
                        labels[i], val);
        else
            term_printf("  %-22s : %s%-12s%s\x1b[K\r\n",
                        labels[i], VT_FG_CYAN, val, VT_RESET);
    }

    term_puts("\x1b[K\r\n");
    term_printf("%s  %s%s\x1b[K\r\n",
                VT_DIM, help[ctx->ui.setup_sel], VT_RESET);
    term_puts("\x1b[K\r\n");
    term_printf("%s  wtop \xc2\xa9 2026 Cingano Development%s\x1b[K\r\n",
                VT_DIM, VT_RESET);

    /* Pad to the footer. */
    int used = SETUP_FIRST_ROW + SETUP_OPT_COUNT
             + 2 /* blank+help */ + 2 /* blank+copyright */;
    for (int i = used; i < ts.height - 1; i++)
        term_puts("\x1b[K\r\n");

    render_keybar(ts, SETUP_KEYS, SETUP_KEY_COUNT, &ctx->keybar_hits);

    term_frame_end();
}

static bool setup_handle(AppContext *ctx, const InputEvent *ev, uint64_t now)
{
    (void)now;
    UiState *ui = &ctx->ui;

    if (ev->type == EV_MOUSE) {
        if (ev->button == MBTN_LEFT && ev->pressed) {
            int a = hitmap_at(&ctx->keybar_hits, ev->mx, ev->my);
            if (a == SS_BACK) { ui->view = VIEW_PROCESSES; return true; }
            int row = ev->my - SETUP_FIRST_ROW;
            if (row >= 0 && row < SETUP_OPT_COUNT) {
                if (row == ui->setup_sel && ev->mx >= SETUP_VALUE_COL)
                    setup_adjust(ctx, ui->setup_sel, +1);
                else
                    ui->setup_sel = row;
            }
        }
        return true;
    }

    if (ev->type != EV_KEY)
        return true;

    switch (ev->key) {
        case KEY_UP:
            if (ui->setup_sel > 0) ui->setup_sel--;
            break;
        case KEY_DOWN:
            if (ui->setup_sel + 1 < SETUP_OPT_COUNT) ui->setup_sel++;
            break;
        case KEY_LEFT:
            setup_adjust(ctx, ui->setup_sel, -1);
            break;
        case KEY_RIGHT:
            setup_adjust(ctx, ui->setup_sel, +1);
            break;
        case KEY_ENTER:
            setup_adjust(ctx, ui->setup_sel, +1);
            break;
        case KEY_F2:
        case KEY_ESC:
            ui->view = VIEW_PROCESSES;
            break;
        case KEY_F1:
            ui->help_open = true;
            break;
        case KEY_CHAR:
            switch (ev->ch) {
                case 'q': case 'Q': ui->view = VIEW_PROCESSES; break;
                case 'k': if (ui->setup_sel > 0) ui->setup_sel--; break;
                case 'j': if (ui->setup_sel + 1 < SETUP_OPT_COUNT) ui->setup_sel++;
                          break;
                case 'h': setup_adjust(ctx, ui->setup_sel, -1); break;
                case 'l': setup_adjust(ctx, ui->setup_sel, +1); break;
                case ' ': setup_adjust(ctx, ui->setup_sel, +1); break;
                case '?': ui->help_open = true; break;
                default: break;
            }
            break;
        default:
            break;
    }
    return true;
}

const ViewVTable VIEW_SETUP_VTABLE = { setup_render, setup_handle };
