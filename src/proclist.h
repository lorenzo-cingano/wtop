#ifndef WTOP_PROCLIST_H
#define WTOP_PROCLIST_H

#include "wtop.h"
#include <stdint.h>
#include <stdbool.h>

/* Enumerates processes and computes per-process CPU% across samples. */

typedef enum {
    PROC_SORT_CPU,
    PROC_SORT_MEM,
    PROC_SORT_PID,
    PROC_SORT_THREADS,
    PROC_SORT_NAME,
    PROC_SORT_COUNT
} ProcSortField;

typedef struct {
    uint32_t pid;
    uint32_t ppid;
    uint32_t threads;
    uint64_t mem_bytes;     /* working set size */
    double   cpu_percent;   /* % of one core (htop-style, can exceed 100) */
    char     name[WTOP_MAX_PROC_NAME];
} ProcInfo;

typedef struct ProcList ProcList;

ProcList *proclist_create(void);
void proclist_destroy(ProcList *pl);

/* Set the sort column/direction. Re-sorts the current sample immediately and
 * applies to subsequent samples. */
void proclist_set_sort(ProcList *pl, ProcSortField field, bool descending);

/* Set a case-insensitive substring filter on the process name. NULL or ""
 * clears it. proclist_count()/proclist_get() then expose only matches. */
void proclist_set_filter(ProcList *pl, const char *substr);

/* Toggle tree view. When on, the visible set is ordered as a parent/child
 * forest (children sorted by the active sort key under each parent) and
 * proclist_get_prefix() returns each row's indentation/connector glyphs. */
void proclist_set_tree(ProcList *pl, bool on);

/* Re-enumerate processes and recompute CPU% relative to the previous sample.
 * Results are owned by the ProcList and valid until the next sample/destroy. */
void proclist_sample(ProcList *pl);

/* Number of processes matching the current filter (the visible set). */
size_t proclist_count(const ProcList *pl);

/* Total number of processes in the last sample, ignoring the filter. */
size_t proclist_total(const ProcList *pl);

/* Access the filtered, sorted sample. Index is into the visible set. */
const ProcInfo *proclist_get(const ProcList *pl, size_t index);

/* Tree-drawing prefix for a visible row (e.g. "├─ "), to be printed before the
 * process name. Returns "" in flat mode or for an out-of-range index. */
const char *proclist_get_prefix(const ProcList *pl, size_t index);

#endif /* WTOP_PROCLIST_H */
