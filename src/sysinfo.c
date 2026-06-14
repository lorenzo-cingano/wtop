#include "sysinfo.h"
#include "wtop.h"

/* Per-core CPU times come from NtQuerySystemInformation, which lives in
 * ntdll. We declare the minimal pieces ourselves and resolve the function at
 * runtime so we need no DDK headers and no extra link libraries. */

#define SystemProcessorPerformanceInformation 8

typedef struct {
    LARGE_INTEGER IdleTime;
    LARGE_INTEGER KernelTime;   /* includes idle */
    LARGE_INTEGER UserTime;
    LARGE_INTEGER DpcTime;
    LARGE_INTEGER InterruptTime;
    ULONG         InterruptCount;
} SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION;

typedef LONG (WINAPI *NtQuerySystemInformation_t)(
    ULONG SystemInformationClass,
    PVOID SystemInformation,
    ULONG SystemInformationLength,
    PULONG ReturnLength);

static NtQuerySystemInformation_t g_NtQSI = NULL;

static uint64_t g_prev_idle = 0;
static uint64_t g_prev_kernel = 0;
static uint64_t g_prev_user = 0;
static uint32_t g_num_cpus = 1;

/* Per-core previous counters. */
static uint64_t g_core_prev_idle[WTOP_MAX_CORES];
static uint64_t g_core_prev_busy[WTOP_MAX_CORES]; /* kernel+user (kernel incl. idle) */

void sysinfo_init(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    g_num_cpus = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
    if (g_num_cpus > WTOP_MAX_CORES)
        g_num_cpus = WTOP_MAX_CORES;

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        /* Cast via uintptr_t to avoid -Wcast-function-type on the
         * FARPROC -> typed function pointer conversion. */
        FARPROC proc = GetProcAddress(ntdll, "NtQuerySystemInformation");
        g_NtQSI = (NtQuerySystemInformation_t)(uintptr_t)proc;
    }

    /* Seed overall-CPU baseline. */
    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        g_prev_idle = filetime_to_u64(&idle);
        g_prev_kernel = filetime_to_u64(&kernel);
        g_prev_user = filetime_to_u64(&user);
    }

    /* Seed per-core baseline. */
    if (g_NtQSI) {
        SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION info[WTOP_MAX_CORES];
        ULONG ret = 0;
        if (g_NtQSI(SystemProcessorPerformanceInformation, info,
                    sizeof(info[0]) * g_num_cpus, &ret) == 0) {
            for (uint32_t c = 0; c < g_num_cpus; c++) {
                uint64_t idle_t = (uint64_t)info[c].IdleTime.QuadPart;
                uint64_t busy_t = (uint64_t)info[c].KernelTime.QuadPart
                                + (uint64_t)info[c].UserTime.QuadPart;
                g_core_prev_idle[c] = idle_t;
                g_core_prev_busy[c] = busy_t;
            }
        }
    }
}

void sysinfo_sample(SysInfo *out)
{
    out->num_cpus = g_num_cpus;
    for (uint32_t c = 0; c < WTOP_MAX_CORES; c++)
        out->core_usage[c] = 0.0;

    /* --- Overall CPU ---
     * "kernel" already includes idle, so busy = (kernel - idle) + user. */
    FILETIME idle, kernel, user;
    out->cpu_usage = 0.0;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        uint64_t i = filetime_to_u64(&idle);
        uint64_t k = filetime_to_u64(&kernel);
        uint64_t u = filetime_to_u64(&user);

        uint64_t d_idle = i - g_prev_idle;
        uint64_t d_total = (k - g_prev_kernel) + (u - g_prev_user);
        if (d_total > 0)
            out->cpu_usage = (double)(d_total - d_idle) / (double)d_total;

        g_prev_idle = i;
        g_prev_kernel = k;
        g_prev_user = u;
    }

    /* --- Per-core CPU --- */
    if (g_NtQSI) {
        SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION info[WTOP_MAX_CORES];
        ULONG ret = 0;
        if (g_NtQSI(SystemProcessorPerformanceInformation, info,
                    sizeof(info[0]) * g_num_cpus, &ret) == 0) {
            for (uint32_t c = 0; c < g_num_cpus; c++) {
                uint64_t idle_t = (uint64_t)info[c].IdleTime.QuadPart;
                uint64_t busy_t = (uint64_t)info[c].KernelTime.QuadPart
                                + (uint64_t)info[c].UserTime.QuadPart;

                uint64_t d_idle = idle_t - g_core_prev_idle[c];
                uint64_t d_total = busy_t - g_core_prev_busy[c];
                if (d_total > 0) {
                    double busy = (double)(d_total - d_idle) / (double)d_total;
                    if (busy < 0) busy = 0;
                    if (busy > 1) busy = 1;
                    out->core_usage[c] = busy;
                }
                g_core_prev_idle[c] = idle_t;
                g_core_prev_busy[c] = busy_t;
            }
        }
    }

    /* --- Memory --- */
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        out->mem_total = ms.ullTotalPhys;
        out->mem_used = ms.ullTotalPhys - ms.ullAvailPhys;

        uint64_t page_total = (ms.ullTotalPageFile > ms.ullTotalPhys)
                                  ? ms.ullTotalPageFile - ms.ullTotalPhys : 0;
        uint64_t avail_page = (ms.ullAvailPageFile > ms.ullAvailPhys)
                                  ? ms.ullAvailPageFile - ms.ullAvailPhys : 0;
        out->swap_total = page_total;
        out->swap_used = (page_total > avail_page) ? page_total - avail_page : 0;
    } else {
        out->mem_total = out->mem_used = 0;
        out->swap_total = out->swap_used = 0;
    }
}
