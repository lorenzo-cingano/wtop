#ifndef WTOP_SYSINFO_H
#define WTOP_SYSINFO_H

#include <stdint.h>

/* System-wide CPU and memory statistics. */

#define WTOP_MAX_CORES 64   /* one processor group's worth of logical CPUs */

typedef struct {
    double cpu_usage;       /* overall CPU busy fraction, 0.0 .. 1.0 */
    double core_usage[WTOP_MAX_CORES]; /* per-core busy fraction, 0.0 .. 1.0 */
    uint64_t mem_total;     /* bytes of physical RAM */
    uint64_t mem_used;      /* bytes in use (total - available) */
    uint64_t swap_total;    /* bytes of commit limit beyond RAM (page file) */
    uint64_t swap_used;     /* bytes of page file in use */
    uint32_t num_cpus;      /* logical processor count (capped at MAX_CORES) */
} SysInfo;

/* Call once before the first sysinfo_sample(). */
void sysinfo_init(void);

/* Refresh stats. CPU usage is computed from the delta since the previous
 * call, so the first sample reports 0 until a baseline exists. */
void sysinfo_sample(SysInfo *out);

#endif /* WTOP_SYSINFO_H */
