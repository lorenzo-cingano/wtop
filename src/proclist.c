#include "proclist.h"
#include "wtop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* We enumerate processes with a single NtQuerySystemInformation call rather
 * than a Toolhelp snapshot plus an OpenProcess per process. The minimal pieces
 * of the (undocumented but stable) interface are declared here so we need no
 * DDK headers and no extra link libraries. */

#define SystemProcessInformation 5
#define WT_STATUS_INFO_LENGTH_MISMATCH ((LONG)0xC0000004L)

/* Tree-view indentation. Each level is 3 display columns; the box-drawing
 * glyphs are multi-byte UTF-8 but only the COMMAND column follows them, so the
 * byte/width mismatch does not disturb earlier columns. */
#define PREFIX_W 128
#define T_VERT   "\xe2\x94\x82  "          /* "|  " continuation under a parent */
#define T_BLANK  "   "                     /* spacing under a last child */
#define T_TEE    "\xe2\x94\x9c\xe2\x94\x80 " /* "|- " a non-last child */
#define T_ELBOW  "\xe2\x94\x94\xe2\x94\x80 " /* "`- " the last child */

typedef struct {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} WT_UNICODE_STRING;

/* Field layout matches the OS SYSTEM_PROCESS_INFORMATION up to WorkingSetSize,
 * which is all we read; entries are walked via NextEntryOffset. */
typedef struct {
    ULONG         NextEntryOffset;
    ULONG         NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG         HardFaultCount;
    ULONG         NumberOfThreadsHighWatermark;
    ULONGLONG     CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    WT_UNICODE_STRING ImageName;
    LONG          BasePriority;
    HANDLE        UniqueProcessId;
    HANDLE        InheritedFromUniqueProcessId;
    ULONG         HandleCount;
    ULONG         SessionId;
    ULONG_PTR     UniqueProcessKey;
    SIZE_T        PeakVirtualSize;
    SIZE_T        VirtualSize;
    ULONG         PageFaultCount;
    SIZE_T        PeakWorkingSetSize;
    SIZE_T        WorkingSetSize;
} WT_SYSTEM_PROCESS_INFORMATION;

typedef LONG (WINAPI *NtQSI_t)(ULONG, PVOID, ULONG, PULONG);

static NtQSI_t g_NtQSI = NULL;
static bool    g_NtQSI_loaded = false;

static void load_ntqsi(void)
{
    g_NtQSI_loaded = true;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        FARPROC proc = GetProcAddress(ntdll, "NtQuerySystemInformation");
        g_NtQSI = (NtQSI_t)(uintptr_t)proc;
    }
}

/* Per-PID CPU bookkeeping carried between samples. */
typedef struct {
    uint32_t pid;
    uint64_t prev_cpu;   /* kernel+user time, 100ns units */
} CpuTrack;

struct ProcList {
    ProcInfo  *items;
    size_t     count;
    size_t     cap;

    size_t    *view;        /* indices into items[] that pass the filter */
    size_t     view_count;
    size_t     view_cap;
    char       filter[128]; /* case-insensitive name substring, "" = none */

    char      *prefixes;    /* view_cap rows of PREFIX_W bytes, parallel to view */
    bool       tree;        /* tree view vs flat sorted list */

    CpuTrack  *track;
    size_t     track_count;
    size_t     track_cap;

    void      *infobuf;     /* reused NtQuerySystemInformation result buffer */
    size_t     infobuf_cap;

    uint64_t   prev_wall;   /* GetSystemTimeAsFileTime, 100ns units */

    ProcSortField sort_field;
    bool          sort_desc;
};

ProcList *proclist_create(void)
{
    ProcList *pl = (ProcList *)calloc(1, sizeof(*pl));
    if (pl) {
        pl->sort_field = PROC_SORT_CPU;
        pl->sort_desc = true;
    }
    return pl;
}

void proclist_destroy(ProcList *pl)
{
    if (!pl)
        return;
    free(pl->items);
    free(pl->view);
    free(pl->prefixes);
    free(pl->track);
    free(pl->infobuf);
    free(pl);
}

/* Built below; flat and tree rebuilds share the visible-set growth helper. */
static void rebuild_view(ProcList *pl);

/* Grow view[] and the parallel prefixes[] to hold at least `need` rows. */
static bool ensure_view_cap(ProcList *pl, size_t need)
{
    if (need == 0)
        need = 1;
    if (pl->view_cap >= need)
        return true;
    size_t *v = (size_t *)realloc(pl->view, need * sizeof(*v));
    if (!v)
        return false;
    pl->view = v;
    char *pf = (char *)realloc(pl->prefixes, need * PREFIX_W);
    if (!pf)
        return false;
    pl->prefixes = pf;
    pl->view_cap = need;
    return true;
}

/* True if the process name contains the filter substring (case-insensitive).
 * An empty filter matches everything. */
static bool name_matches(const ProcList *pl, size_t i)
{
    size_t flen = strlen(pl->filter);
    if (flen == 0)
        return true;
    for (const char *p = pl->items[i].name; *p; p++)
        if (_strnicmp(p, pl->filter, flen) == 0)
            return true;
    return false;
}

/* Flat (non-tree) visible set: every matching process, in the sorted order of
 * items[]. Prefixes are empty. */
static void rebuild_view_flat(ProcList *pl)
{
    pl->view_count = 0;
    for (size_t i = 0; i < pl->count; i++) {
        if (name_matches(pl, i)) {
            pl->view[pl->view_count] = i;
            pl->prefixes[pl->view_count * PREFIX_W] = '\0';
            pl->view_count++;
        }
    }
}

void proclist_set_filter(ProcList *pl, const char *substr)
{
    if (!substr)
        substr = "";
    snprintf(pl->filter, sizeof(pl->filter), "%s", substr);
    rebuild_view(pl);
}

static uint64_t lookup_prev_cpu(const ProcList *pl, uint32_t pid)
{
    for (size_t i = 0; i < pl->track_count; i++)
        if (pl->track[i].pid == pid)
            return pl->track[i].prev_cpu;
    return 0;
}

static void items_push(ProcList *pl, const ProcInfo *pi)
{
    if (pl->count == pl->cap) {
        size_t cap = pl->cap ? pl->cap * 2 : 256;
        ProcInfo *p = (ProcInfo *)realloc(pl->items, cap * sizeof(*p));
        if (!p)
            return;
        pl->items = p;
        pl->cap = cap;
    }
    pl->items[pl->count++] = *pi;
}

/* qsort has no context argument portably, so the active sort key is held in
 * file-scope state during the (single-threaded) sort. */
static ProcSortField g_sort_field = PROC_SORT_CPU;
static bool          g_sort_desc = true;

/* Order two processes by the active sort key, honouring direction. Used both to
 * sort the flat array and to order siblings within a tree node. */
static int compare_pi(const ProcInfo *pa, const ProcInfo *pb)
{
    int r = 0;

    switch (g_sort_field) {
    case PROC_SORT_CPU:
        r = (pa->cpu_percent > pb->cpu_percent) - (pa->cpu_percent < pb->cpu_percent);
        break;
    case PROC_SORT_MEM:
        r = (pa->mem_bytes > pb->mem_bytes) - (pa->mem_bytes < pb->mem_bytes);
        break;
    case PROC_SORT_PID:
        r = (pa->pid > pb->pid) - (pa->pid < pb->pid);
        break;
    case PROC_SORT_THREADS:
        r = (pa->threads > pb->threads) - (pa->threads < pb->threads);
        break;
    case PROC_SORT_NAME:
        r = _stricmp(pa->name, pb->name);
        break;
    default:
        break;
    }

    /* Deterministic tiebreak by PID so the order never jitters. */
    if (r == 0)
        r = (pa->pid > pb->pid) - (pa->pid < pb->pid);

    return g_sort_desc ? -r : r;
}

static int cmp_proc(const void *a, const void *b)
{
    return compare_pi((const ProcInfo *)a, (const ProcInfo *)b);
}

/* Index comparator for tree siblings: compares items[*a] vs items[*b]. */
static const ProcInfo *g_cmp_items = NULL;

static int cmp_index(const void *a, const void *b)
{
    return compare_pi(&g_cmp_items[*(const size_t *)a],
                      &g_cmp_items[*(const size_t *)b]);
}

static void proclist_sort(ProcList *pl)
{
    g_sort_field = pl->sort_field;
    g_sort_desc = pl->sort_desc;
    qsort(pl->items, pl->count, sizeof(ProcInfo), cmp_proc);
}

void proclist_set_sort(ProcList *pl, ProcSortField field, bool descending)
{
    if (field >= PROC_SORT_COUNT)
        field = PROC_SORT_CPU;
    pl->sort_field = field;
    pl->sort_desc = descending;
    proclist_sort(pl);
    rebuild_view(pl);
}

/* Emit `node` as a visible row with prefix `self_prefix`, then recurse into its
 * included children (sorted by the active key). `child_cont` is the prefix that
 * precedes each child's own connector. Recursion is guarded by visited[] so a
 * malformed parent cycle cannot loop forever. */
static void tree_walk(ProcList *pl, size_t node,
                      const long *parent, const bool *include, bool *visited,
                      const char *self_prefix, const char *child_cont)
{
    if (visited[node])
        return;
    visited[node] = true;

    if (pl->view_count < pl->view_cap) {
        pl->view[pl->view_count] = node;
        snprintf(&pl->prefixes[pl->view_count * PREFIX_W], PREFIX_W,
                 "%s", self_prefix);
        pl->view_count++;
    }

    /* Gather this node's included children. */
    size_t *kids = NULL, cap = 0, k = 0;
    for (size_t i = 0; i < pl->count; i++) {
        if (parent[i] == (long)node && include[i] && !visited[i]) {
            if (k == cap) {
                cap = cap ? cap * 2 : 8;
                size_t *t = (size_t *)realloc(kids, cap * sizeof(*t));
                if (!t) break;
                kids = t;
            }
            kids[k++] = i;
        }
    }
    if (k > 1)
        qsort(kids, k, sizeof(*kids), cmp_index);

    for (size_t c = 0; c < k; c++) {
        bool last = (c + 1 == k);
        char sp[PREFIX_W], cc[PREFIX_W];
        snprintf(sp, sizeof(sp), "%s%s", child_cont, last ? T_ELBOW : T_TEE);
        snprintf(cc, sizeof(cc), "%s%s", child_cont, last ? T_BLANK : T_VERT);
        tree_walk(pl, kids[c], parent, include, visited, sp, cc);
    }
    free(kids);
}

/* Tree visible set: a parent/child forest built from ppid links, depth-first,
 * children sorted by the active key. With a filter active, a node is kept if it
 * matches or has a matching descendant, so the path to each match stays visible. */
static void rebuild_view_tree(ProcList *pl)
{
    g_sort_field = pl->sort_field;
    g_sort_desc = pl->sort_desc;
    g_cmp_items = pl->items;

    size_t n = pl->count;
    pl->view_count = 0;
    if (n == 0)
        return;

    long *parent  = (long *)malloc(n * sizeof(*parent));
    bool *match   = (bool *)malloc(n * sizeof(*match));
    bool *include = (bool *)malloc(n * sizeof(*include));
    bool *visited = (bool *)calloc(n, sizeof(*visited));
    if (!parent || !match || !include || !visited) {
        free(parent); free(match); free(include); free(visited);
        rebuild_view_flat(pl);   /* degrade gracefully on OOM */
        return;
    }

    /* parent[i] = items[] slot whose pid == our ppid, or -1 if none (root). */
    for (size_t i = 0; i < n; i++) {
        parent[i] = -1;
        uint32_t ppid = pl->items[i].ppid;
        if (ppid != pl->items[i].pid) {          /* ignore self-parenting */
            for (size_t j = 0; j < n; j++)
                if (j != i && pl->items[j].pid == ppid) {
                    parent[i] = (long)j;
                    break;
                }
        }
    }

    /* Inclusion: matches plus all their ancestors (so context is preserved). */
    bool filtering = pl->filter[0] != '\0';
    for (size_t i = 0; i < n; i++) {
        match[i] = name_matches(pl, i);
        include[i] = !filtering;
    }
    if (filtering) {
        for (size_t i = 0; i < n; i++) {
            if (!match[i])
                continue;
            long c = (long)i, guard = 0;
            while (c >= 0 && guard++ <= (long)n) {
                include[c] = true;
                c = parent[c];
            }
        }
    }

    /* Roots: no parent (or an excluded parent), walked in sorted order. */
    size_t *roots = NULL, rcap = 0, rk = 0;
    for (size_t i = 0; i < n; i++) {
        bool is_root = (parent[i] < 0) || !include[parent[i]];
        if (is_root && include[i]) {
            if (rk == rcap) {
                rcap = rcap ? rcap * 2 : 32;
                size_t *t = (size_t *)realloc(roots, rcap * sizeof(*t));
                if (!t) break;
                roots = t;
            }
            roots[rk++] = i;
        }
    }
    if (rk > 1)
        qsort(roots, rk, sizeof(*roots), cmp_index);
    for (size_t r = 0; r < rk; r++)
        tree_walk(pl, roots[r], parent, include, visited, "", "");
    free(roots);

    /* Safety net: append any included node not reached (e.g. a parent cycle)
     * flat, so nothing is silently dropped from the list. */
    for (size_t i = 0; i < n; i++) {
        if (!visited[i] && include[i] && pl->view_count < pl->view_cap) {
            pl->view[pl->view_count] = i;
            pl->prefixes[pl->view_count * PREFIX_W] = '\0';
            pl->view_count++;
        }
    }

    free(parent); free(match); free(include); free(visited);
}

static void rebuild_view(ProcList *pl)
{
    if (!ensure_view_cap(pl, pl->count))
        return;
    if (pl->tree)
        rebuild_view_tree(pl);
    else
        rebuild_view_flat(pl);
}

void proclist_set_tree(ProcList *pl, bool on)
{
    pl->tree = on;
    rebuild_view(pl);
}

/* Query SystemProcessInformation into pl->infobuf, growing it as needed.
 * Returns the byte length written, or 0 on failure. */
static ULONG query_processes(ProcList *pl)
{
    if (!g_NtQSI_loaded)
        load_ntqsi();
    if (!g_NtQSI)
        return 0;

    ULONG need = 0;
    LONG st = g_NtQSI(SystemProcessInformation, pl->infobuf,
                      (ULONG)pl->infobuf_cap, &need);
    while (st == WT_STATUS_INFO_LENGTH_MISMATCH) {
        /* Add slack: the process set can grow between sizing and the read. */
        size_t cap = (size_t)need + 64 * 1024;
        void *nb = realloc(pl->infobuf, cap);
        if (!nb)
            return 0;
        pl->infobuf = nb;
        pl->infobuf_cap = cap;
        st = g_NtQSI(SystemProcessInformation, pl->infobuf,
                     (ULONG)pl->infobuf_cap, &need);
    }
    return (st < 0) ? 0 : need;
}

void proclist_sample(ProcList *pl)
{
    /* Wall-clock delta shared by every process this round. */
    FILETIME now_ft;
    GetSystemTimeAsFileTime(&now_ft);
    uint64_t now_wall = filetime_to_u64(&now_ft);
    uint64_t d_wall = (pl->prev_wall && now_wall > pl->prev_wall)
                          ? now_wall - pl->prev_wall : 0;

    ULONG total = query_processes(pl);
    if (total == 0)
        return;

    /* Fresh per-PID CPU table, grown as we walk the process list. */
    CpuTrack *new_track = NULL;
    size_t new_track_cap = 0;
    size_t new_track_count = 0;

    pl->count = 0;

    size_t off = 0;
    for (;;) {
        const WT_SYSTEM_PROCESS_INFORMATION *spi =
            (const WT_SYSTEM_PROCESS_INFORMATION *)((char *)pl->infobuf + off);

        /* PID 0 is the System Idle Process: a kernel accounting pseudo-process
         * whose CPU time is really per-core idle time (it reads up to
         * num_cpus * 100%). It is not a real process, so skip it entirely. */
        if ((uint32_t)(uintptr_t)spi->UniqueProcessId == 0) {
            if (spi->NextEntryOffset == 0)
                break;
            off += spi->NextEntryOffset;
            continue;
        }

        ProcInfo pi;
        memset(&pi, 0, sizeof(pi));
        pi.pid = (uint32_t)(uintptr_t)spi->UniqueProcessId;
        pi.ppid = (uint32_t)(uintptr_t)spi->InheritedFromUniqueProcessId;
        pi.threads = spi->NumberOfThreads;
        pi.mem_bytes = (uint64_t)spi->WorkingSetSize;

        /* Image base name (UTF-16, length in bytes) -> UTF-8. */
        if (spi->ImageName.Buffer && spi->ImageName.Length) {
            int wlen = spi->ImageName.Length / (int)sizeof(WCHAR);
            int n = WideCharToMultiByte(CP_UTF8, 0, spi->ImageName.Buffer, wlen,
                                        pi.name, (int)sizeof(pi.name) - 1,
                                        NULL, NULL);
            if (n < 0) n = 0;
            pi.name[n] = '\0';
        } else {
            /* The kernel process (PID 4) reports an empty image name. */
            snprintf(pi.name, sizeof(pi.name), "%s", "System");
        }

        uint64_t cur_cpu = (uint64_t)spi->UserTime.QuadPart
                         + (uint64_t)spi->KernelTime.QuadPart;
        if (d_wall > 0) {
            uint64_t prev = lookup_prev_cpu(pl, pi.pid);
            if (cur_cpu >= prev)
                pi.cpu_percent = (double)(cur_cpu - prev) / (double)d_wall * 100.0;
        }

        if (new_track_count == new_track_cap) {
            size_t cap = new_track_cap ? new_track_cap * 2 : 512;
            CpuTrack *t = (CpuTrack *)realloc(new_track, cap * sizeof(*t));
            if (t) { new_track = t; new_track_cap = cap; }
        }
        if (new_track_count < new_track_cap) {
            new_track[new_track_count].pid = pi.pid;
            new_track[new_track_count].prev_cpu = cur_cpu;
            new_track_count++;
        }

        items_push(pl, &pi);

        if (spi->NextEntryOffset == 0)
            break;
        off += spi->NextEntryOffset;
    }

    /* Swap in the new tracking table. */
    free(pl->track);
    pl->track = new_track;
    pl->track_cap = new_track_cap;
    pl->track_count = new_track_count;
    pl->prev_wall = now_wall;

    proclist_sort(pl);
    rebuild_view(pl);
}

size_t proclist_count(const ProcList *pl)
{
    return pl->view_count;
}

size_t proclist_total(const ProcList *pl)
{
    return pl->count;
}

const ProcInfo *proclist_get(const ProcList *pl, size_t index)
{
    if (index >= pl->view_count)
        return NULL;
    return &pl->items[pl->view[index]];
}

const char *proclist_get_prefix(const ProcList *pl, size_t index)
{
    if (!pl->prefixes || index >= pl->view_count)
        return "";
    return &pl->prefixes[index * PREFIX_W];
}
