#ifndef WTOP_IOSTAT_H
#define WTOP_IOSTAT_H

#include <stdbool.h>
#include <stdint.h>

/* System-wide network and disk I/O throughput.
 *
 * Counters are cumulative byte totals read from the OS; iostat_sample() turns
 * the delta since the previous call into per-second rates, using the real
 * elapsed wall-clock time so the rates stay correct at any refresh interval. */

#define IOSTAT_MAX_NICS  16   /* network interfaces shown in the breakdown */
#define IOSTAT_MAX_DISKS 16   /* physical drives probed (\\.\PhysicalDriveN) */

typedef struct {
    char     name[64];        /* friendly adapter name (e.g. "Ethernet") */
    double   rx_bps;          /* bytes/s received */
    double   tx_bps;          /* bytes/s sent */
    uint64_t rx_total;        /* cumulative bytes received */
    uint64_t tx_total;        /* cumulative bytes sent */
} NicStat;

typedef struct {
    int      index;           /* physical drive number */
    double   read_bps;        /* bytes/s read */
    double   write_bps;       /* bytes/s written */
    double   busy;            /* fraction of the interval the disk was busy */
    uint64_t read_total;      /* cumulative bytes read */
    uint64_t write_total;     /* cumulative bytes written */
} DiskStat;

typedef struct {
    bool     net_ok;          /* network counters were available */
    bool     disk_ok;         /* at least one disk responded */

    double   net_rx_bps;      /* aggregate receive rate, bytes/s */
    double   net_tx_bps;      /* aggregate send rate, bytes/s */
    double   net_scale;       /* full-scale value for the network meters */

    double   disk_read_bps;   /* aggregate read rate, bytes/s */
    double   disk_write_bps;  /* aggregate write rate, bytes/s */
    double   disk_scale;      /* full-scale value for the disk meters */

    int      nic_count;
    NicStat  nics[IOSTAT_MAX_NICS];

    int      disk_count;
    DiskStat disks[IOSTAT_MAX_DISKS];
} IoStat;

/* Call once before the first iostat_sample(); resolves the network API and
 * seeds the baseline counters so the first real sample reports sane rates. */
void iostat_init(void);

/* Refresh network and disk rates from the delta since the previous call. */
void iostat_sample(IoStat *out);

#endif /* WTOP_IOSTAT_H */
