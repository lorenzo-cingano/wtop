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
#include "ui.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

/* Set false by the console control handler to unwind the run loop cleanly.
 * Read by ui_run() (see ui.c). */
volatile bool g_running = true;

static BOOL WINAPI ctrl_handler(DWORD type)
{
    (void)type;
    g_running = false;
    return TRUE;
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

    SysInfo sys;
    IoStat io;

    AppContext ctx = {
        .cfg = &cfg,
        .pl  = pl,
        .sys = &sys,
        .io  = &io,
    };

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
    ui_reanchor(&ctx.nav, pl);

    ui_run(&ctx);

    config_save(&cfg);
    proclist_destroy(pl);
    term_shutdown();
    return 0;
}
