#ifndef WTOP_H
#define WTOP_H

#include <windows.h>
#include <stdint.h>
#include <stddef.h>

/* Shared limits and small helpers used across wtop modules. */

#define WTOP_MAX_PROC_NAME 260

/* Convert a FILETIME (two 32-bit halves, 100ns units) into a 64-bit count. */
static inline uint64_t filetime_to_u64(const FILETIME *ft)
{
    return ((uint64_t)ft->dwHighDateTime << 32) | (uint64_t)ft->dwLowDateTime;
}

#endif /* WTOP_H */
