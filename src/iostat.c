/*
 * wtop - an htop-style process viewer for Windows.
 * Copyright (c) 2026 Cingano Development. All rights reserved.
 *
 * Network and disk I/O throughput.
 *
 * Network counters come from GetIfTable2 (iphlpapi), resolved at runtime so we
 * need no extra link library - the same trick sysinfo.c uses for ntdll. Disk
 * counters come from IOCTL_DISK_PERFORMANCE on each \\.\PhysicalDriveN, which a
 * normal user can query by opening the drive with zero access rights.
 */

/* GetIfTable2 / MIB_IF_TABLE2 live in <netioapi.h>, which iphlpapi.h pulls in
 * only when NTDDI_VERSION (and _WIN32_WINNT) target Vista or later. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#elif _WIN32_WINNT < 0x0600
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x06010000
#elif NTDDI_VERSION < 0x06000000
#undef NTDDI_VERSION
#define NTDDI_VERSION 0x06010000
#endif

#include <winsock2.h>     /* must precede windows.h so winsock2 wins */
#include <ws2ipdef.h>     /* defines _WS2IPDEF_, which unlocks MIB_IF_*2 below */
#include <iphlpapi.h>
#include <windows.h>
#include <winioctl.h>

#include "iostat.h"
#include "wtop.h"

#include <stdio.h>
#include <string.h>

/* Floors so idle links/disks don't peg the auto-scaled meters at zero width,
 * and decay so a one-off spike doesn't shrink the bars forever. */
#define NET_SCALE_FLOOR  (128.0 * 1024.0)    /* 128 KB/s */
#define DISK_SCALE_FLOOR (1024.0 * 1024.0)   /* 1 MB/s   */
#define SCALE_DECAY      0.9

/* iphlpapi entry points we resolve by hand. */
typedef ULONG (WINAPI *GetIfTable2_t)(PMIB_IF_TABLE2 *);
typedef void  (WINAPI *FreeMibTable_t)(PVOID);

static GetIfTable2_t  p_GetIfTable2  = NULL;
static FreeMibTable_t p_FreeMibTable = NULL;

/* Previous per-interface byte totals, keyed by interface LUID so a rate is
 * tracked even as adapters come and go between samples. */
typedef struct {
    ULONG64  luid;
    uint64_t in;
    uint64_t out;
    bool     used;
} NicPrev;
static NicPrev g_nic_prev[64];

/* Previous per-physical-drive counters, indexed by drive number. */
static uint64_t g_disk_prev_read[IOSTAT_MAX_DISKS];
static uint64_t g_disk_prev_write[IOSTAT_MAX_DISKS];
static uint64_t g_disk_prev_idle[IOSTAT_MAX_DISKS];
static bool     g_disk_prev_valid[IOSTAT_MAX_DISKS];

static uint64_t g_prev_tick = 0;
static double   g_net_peak  = NET_SCALE_FLOOR;
static double   g_disk_peak = DISK_SCALE_FLOOR;

/* Locate the previous-counter slot for an interface, allocating one on first
 * sight. Returns NULL only if the (64-entry) table is full. */
static NicPrev *nic_slot(ULONG64 luid)
{
    NicPrev *free_slot = NULL;
    for (size_t i = 0; i < sizeof(g_nic_prev) / sizeof(g_nic_prev[0]); i++) {
        if (g_nic_prev[i].used && g_nic_prev[i].luid == luid)
            return &g_nic_prev[i];
        if (!free_slot && !g_nic_prev[i].used)
            free_slot = &g_nic_prev[i];
    }
    if (free_slot) {
        free_slot->used = true;
        free_slot->luid = luid;
        free_slot->in = free_slot->out = 0;
    }
    return free_slot;
}

void iostat_init(void)
{
    HMODULE iphlp = LoadLibraryA("iphlpapi.dll");
    if (iphlp) {
        p_GetIfTable2 =
            (GetIfTable2_t)(uintptr_t)GetProcAddress(iphlp, "GetIfTable2");
        p_FreeMibTable =
            (FreeMibTable_t)(uintptr_t)GetProcAddress(iphlp, "FreeMibTable");
    }

    /* Seed the baselines with a throwaway sample so the first sample the caller
     * sees reports a real delta rather than the whole cumulative total. That
     * seed sample divides huge cumulative totals by a near-zero interval, which
     * would poison the auto-scaling peaks - so reset them afterwards. */
    g_prev_tick = GetTickCount64();
    IoStat seed;
    iostat_sample(&seed);
    g_net_peak = NET_SCALE_FLOOR;
    g_disk_peak = DISK_SCALE_FLOOR;
}

/* ------------------------------- network --------------------------------- */

static void sample_network(IoStat *out, double secs)
{
    if (!p_GetIfTable2 || !p_FreeMibTable)
        return;

    PMIB_IF_TABLE2 table = NULL;
    if (p_GetIfTable2(&table) != NO_ERROR || !table)
        return;

    out->net_ok = true;
    double rx_total = 0, tx_total = 0;

    for (ULONG i = 0; i < table->NumEntries; i++) {
        const MIB_IF_ROW2 *row = &table->Table[i];

        /* Skip loopback and links that aren't up - they only add noise. */
        if (row->Type == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;
        if (row->OperStatus != IfOperStatusUp)
            continue;
        /* NDIS exposes each filter/LWF layer of an adapter as its own row with
         * identical byte counts; counting them would multiply the real traffic.
         * Keep only the underlying (non-filter) interface. */
        if (row->InterfaceAndOperStatusFlags.FilterInterface)
            continue;

        uint64_t in  = row->InOctets;
        uint64_t out_ = row->OutOctets;

        NicPrev *prev = nic_slot(row->InterfaceLuid.Value);
        double rx = 0, tx = 0;
        if (prev) {
            if (in  >= prev->in)  rx = (double)(in  - prev->in)  / secs;
            if (out_ >= prev->out) tx = (double)(out_ - prev->out) / secs;
            prev->in = in;
            prev->out = out_;
        }

        rx_total += rx;
        tx_total += tx;

        if (out->nic_count < IOSTAT_MAX_NICS) {
            NicStat *n = &out->nics[out->nic_count++];
            WideCharToMultiByte(CP_UTF8, 0, row->Alias, -1,
                                n->name, sizeof(n->name), NULL, NULL);
            n->name[sizeof(n->name) - 1] = '\0';
            n->rx_bps = rx;
            n->tx_bps = tx;
            n->rx_total = in;
            n->tx_total = out_;
        }
    }

    p_FreeMibTable(table);

    out->net_rx_bps = rx_total;
    out->net_tx_bps = tx_total;

    double cur = rx_total > tx_total ? rx_total : tx_total;
    g_net_peak *= SCALE_DECAY;
    if (g_net_peak < NET_SCALE_FLOOR) g_net_peak = NET_SCALE_FLOOR;
    if (cur > g_net_peak) g_net_peak = cur;
    out->net_scale = g_net_peak;
}

/* --------------------------------- disk ---------------------------------- */

static void sample_disk(IoStat *out, double secs)
{
    double read_total = 0, write_total = 0;
    double idle_100ns_window = secs * 1.0e7;   /* 100ns ticks in the interval */

    for (int n = 0; n < IOSTAT_MAX_DISKS; n++) {
        char path[32];
        snprintf(path, sizeof(path), "\\\\.\\PhysicalDrive%d", n);

        HANDLE h = CreateFileA(path, 0,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            g_disk_prev_valid[n] = false;   /* drive absent; reset baseline */
            continue;
        }

        DISK_PERFORMANCE dp;
        DWORD ret = 0;
        BOOL ok = DeviceIoControl(h, IOCTL_DISK_PERFORMANCE, NULL, 0,
                                  &dp, sizeof(dp), &ret, NULL);
        CloseHandle(h);
        if (!ok)
            continue;

        uint64_t rb   = (uint64_t)dp.BytesRead.QuadPart;
        uint64_t wb   = (uint64_t)dp.BytesWritten.QuadPart;
        uint64_t idle = (uint64_t)dp.IdleTime.QuadPart;

        if (g_disk_prev_valid[n]) {
            double rd = (rb >= g_disk_prev_read[n])
                            ? (double)(rb - g_disk_prev_read[n]) / secs : 0;
            double wr = (wb >= g_disk_prev_write[n])
                            ? (double)(wb - g_disk_prev_write[n]) / secs : 0;
            double busy = 0;
            if (idle >= g_disk_prev_idle[n] && idle_100ns_window > 0) {
                double d_idle = (double)(idle - g_disk_prev_idle[n]);
                busy = 1.0 - d_idle / idle_100ns_window;
                if (busy < 0) busy = 0;
                if (busy > 1) busy = 1;
            }

            out->disk_ok = true;
            read_total += rd;
            write_total += wr;

            if (out->disk_count < IOSTAT_MAX_DISKS) {
                DiskStat *d = &out->disks[out->disk_count++];
                d->index = n;
                d->read_bps = rd;
                d->write_bps = wr;
                d->busy = busy;
                d->read_total = rb;
                d->write_total = wb;
            }
        }

        g_disk_prev_read[n] = rb;
        g_disk_prev_write[n] = wb;
        g_disk_prev_idle[n] = idle;
        g_disk_prev_valid[n] = true;
    }

    out->disk_read_bps = read_total;
    out->disk_write_bps = write_total;

    double cur = read_total > write_total ? read_total : write_total;
    g_disk_peak *= SCALE_DECAY;
    if (g_disk_peak < DISK_SCALE_FLOOR) g_disk_peak = DISK_SCALE_FLOOR;
    if (cur > g_disk_peak) g_disk_peak = cur;
    out->disk_scale = g_disk_peak;
}

void iostat_sample(IoStat *out)
{
    memset(out, 0, sizeof(*out));
    out->net_scale = g_net_peak;
    out->disk_scale = g_disk_peak;

    uint64_t now = GetTickCount64();
    double secs = (now > g_prev_tick) ? (double)(now - g_prev_tick) / 1000.0 : 0;
    if (secs < 0.001) secs = 0.001;     /* guard the very first/odd interval */
    g_prev_tick = now;

    sample_network(out, secs);
    sample_disk(out, secs);
}
