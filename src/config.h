#ifndef WTOP_CONFIG_H
#define WTOP_CONFIG_H

#include "proclist.h"
#include <stdint.h>
#include <stdbool.h>

/* User-tunable settings, persisted to %APPDATA%\wtop\wtoprc. */

typedef struct {
    uint32_t      refresh_ms;     /* sample/refresh interval */
    ProcSortField sort_field;     /* process table sort column */
    bool          sort_descending;
    bool          show_per_core;  /* per-core meter grid vs single CPU bar */
    bool          tree_view;      /* parent/child forest vs flat sorted list */
} Config;

/* Populate with built-in defaults. */
void config_defaults(Config *c);

/* Load defaults, then overlay any values found in the config file. */
void config_load(Config *c);

/* Write the config file (creating the directory if needed). */
void config_save(const Config *c);

#endif /* WTOP_CONFIG_H */
