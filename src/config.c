#include "config.h"
#include "wtop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REFRESH_MIN 250u
#define REFRESH_MAX 10000u

void config_defaults(Config *c)
{
    c->refresh_ms = 1500;
    c->sort_field = PROC_SORT_CPU;
    c->sort_descending = true;
    c->show_per_core = true;
    c->tree_view = false;
}

/* Build "%APPDATA%\wtop\wtoprc" into buf. Returns false if APPDATA is unset. */
static bool config_path(char *buf, size_t n)
{
    const char *appdata = getenv("APPDATA");
    if (!appdata || !*appdata)
        return false;
    snprintf(buf, n, "%s\\wtop\\wtoprc", appdata);
    return true;
}

static void clamp_config(Config *c)
{
    if (c->refresh_ms < REFRESH_MIN) c->refresh_ms = REFRESH_MIN;
    if (c->refresh_ms > REFRESH_MAX) c->refresh_ms = REFRESH_MAX;
    if (c->sort_field >= PROC_SORT_COUNT) c->sort_field = PROC_SORT_CPU;
}

void config_load(Config *c)
{
    config_defaults(c);

    char path[MAX_PATH];
    if (!config_path(path, sizeof(path)))
        return;

    FILE *f = fopen(path, "r");
    if (!f)
        return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64];
        long val;
        if (sscanf(line, "%63s %ld", key, &val) != 2)
            continue;
        if (strcmp(key, "refresh_ms") == 0)
            c->refresh_ms = (uint32_t)val;
        else if (strcmp(key, "sort_field") == 0)
            c->sort_field = (ProcSortField)val;
        else if (strcmp(key, "sort_descending") == 0)
            c->sort_descending = (val != 0);
        else if (strcmp(key, "show_per_core") == 0)
            c->show_per_core = (val != 0);
        else if (strcmp(key, "tree_view") == 0)
            c->tree_view = (val != 0);
    }
    fclose(f);
    clamp_config(c);
}

void config_save(const Config *c)
{
    const char *appdata = getenv("APPDATA");
    if (!appdata || !*appdata)
        return;

    /* Ensure the directory exists (ignore "already exists"). */
    char dir[MAX_PATH];
    snprintf(dir, sizeof(dir), "%s\\wtop", appdata);
    CreateDirectoryA(dir, NULL);

    char path[MAX_PATH];
    if (!config_path(path, sizeof(path)))
        return;

    FILE *f = fopen(path, "w");
    if (!f)
        return;

    fprintf(f, "# wtop configuration\n");
    fprintf(f, "refresh_ms %u\n", c->refresh_ms);
    fprintf(f, "sort_field %d\n", (int)c->sort_field);
    fprintf(f, "sort_descending %d\n", c->sort_descending ? 1 : 0);
    fprintf(f, "show_per_core %d\n", c->show_per_core ? 1 : 0);
    fprintf(f, "tree_view %d\n", c->tree_view ? 1 : 0);
    fclose(f);
}
