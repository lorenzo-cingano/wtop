/*
 * wtop - an htop-style process viewer for Windows.
 * Copyright (c) 2026 Cingano Development. All rights reserved.
 *
 * Self-install / uninstall: place wtop.exe in %APPDATA%\wtop and register that
 * directory on the per-user PATH (HKCU\Environment), broadcasting the change so
 * already-open shells that re-read the environment pick it up. No admin rights
 * are needed because we only touch the current user's profile and registry.
 */

#include "install.h"
#include "wtop.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* %APPDATA%\wtop  (same directory the config lives in). */
static bool install_dir(char *buf, size_t n)
{
    const char *appdata = getenv("APPDATA");
    if (!appdata || !*appdata)
        return false;
    snprintf(buf, n, "%s\\wtop", appdata);
    return true;
}

/* Full path of the installed executable, %APPDATA%\wtop\wtop.exe. */
static bool install_exe_path(char *buf, size_t n)
{
    const char *appdata = getenv("APPDATA");
    if (!appdata || !*appdata)
        return false;
    snprintf(buf, n, "%s\\wtop\\wtop.exe", appdata);
    return true;
}

static bool file_exists(const char *path)
{
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

/* Two filesystem paths refer to the same file (case-insensitive). */
static bool same_path(const char *a, const char *b)
{
    return _stricmp(a, b) == 0;
}

/* ------------------------------ PATH editing ------------------------------ */

/* True if `dir` appears as a whole ';'-separated segment of `path` (the kind of
 * comparison Windows itself does: case-insensitive, segment-exact). */
static bool path_contains(const char *path, const char *dir)
{
    size_t dlen = strlen(dir);
    for (const char *p = path; *p; ) {
        const char *semi = strchr(p, ';');
        size_t seglen = semi ? (size_t)(semi - p) : strlen(p);
        if (seglen == dlen && _strnicmp(p, dir, dlen) == 0)
            return true;
        if (!semi)
            break;
        p = semi + 1;
    }
    return false;
}

/* Read HKCU\Environment\Path into a malloc'd, NUL-terminated string (caller
 * frees). *out_type receives the registry value type to preserve on write.
 * Returns "" (empty, type REG_EXPAND_SZ) when the value is absent. NULL only on
 * allocation failure. */
static char *path_read(HKEY key, DWORD *out_type)
{
    DWORD type = REG_EXPAND_SZ, size = 0;
    LONG rc = RegQueryValueExA(key, "Path", NULL, &type, NULL, &size);
    if (rc != ERROR_SUCCESS || size == 0) {
        char *empty = malloc(1);
        if (empty) empty[0] = '\0';
        *out_type = REG_EXPAND_SZ;
        return empty;
    }
    char *buf = malloc((size_t)size + 1);
    if (!buf)
        return NULL;
    rc = RegQueryValueExA(key, "Path", NULL, &type, (BYTE *)buf, &size);
    if (rc != ERROR_SUCCESS) {
        free(buf);
        return NULL;
    }
    buf[size] = '\0';                 /* registry strings may omit the NUL */
    *out_type = type;
    return buf;
}

/* Tell the shell and other listeners that the environment changed, so new
 * processes (and Explorer-launched ones) inherit the updated PATH. */
static void broadcast_env_change(void)
{
    DWORD_PTR result;
    SendMessageTimeoutA(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                        (LPARAM)"Environment", SMTO_ABORTIFHUNG, 5000, &result);
}

/* Append `dir` to the user PATH. Returns 1 if added, 0 if already present,
 * -1 on error. */
static int path_add(const char *dir)
{
    HKEY key;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Environment", 0,
                      KEY_READ | KEY_WRITE, &key) != ERROR_SUCCESS)
        return -1;

    DWORD type;
    char *cur = path_read(key, &type);
    if (!cur) { RegCloseKey(key); return -1; }

    if (path_contains(cur, dir)) {
        free(cur);
        RegCloseKey(key);
        return 0;
    }

    size_t curlen = strlen(cur);
    size_t need = curlen + 1 /* ';' */ + strlen(dir) + 1 /* NUL */;
    char *neu = malloc(need);
    if (!neu) { free(cur); RegCloseKey(key); return -1; }

    if (curlen == 0)
        snprintf(neu, need, "%s", dir);
    else if (cur[curlen - 1] == ';')
        snprintf(neu, need, "%s%s", cur, dir);
    else
        snprintf(neu, need, "%s;%s", cur, dir);

    LONG rc = RegSetValueExA(key, "Path", 0, type,
                             (const BYTE *)neu, (DWORD)(strlen(neu) + 1));
    free(cur);
    free(neu);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS)
        return -1;

    broadcast_env_change();
    return 1;
}

/* Remove `dir` from the user PATH. Returns 1 if removed, 0 if it wasn't there,
 * -1 on error. */
static int path_remove(const char *dir)
{
    HKEY key;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Environment", 0,
                      KEY_READ | KEY_WRITE, &key) != ERROR_SUCCESS)
        return -1;

    DWORD type;
    char *cur = path_read(key, &type);
    if (!cur) { RegCloseKey(key); return -1; }

    if (!path_contains(cur, dir)) {
        free(cur);
        RegCloseKey(key);
        return 0;
    }

    /* Rebuild the value, dropping only our own segment. Every other segment
     * (including empties) is preserved verbatim so the rest of PATH is left
     * byte-for-byte unchanged. */
    char *neu = malloc(strlen(cur) + 1);
    if (!neu) { free(cur); RegCloseKey(key); return -1; }

    size_t dlen = strlen(dir), nlen = 0;
    bool first = true;
    for (const char *p = cur; *p; ) {
        const char *semi = strchr(p, ';');
        size_t seglen = semi ? (size_t)(semi - p) : strlen(p);
        bool drop = seglen == dlen && _strnicmp(p, dir, dlen) == 0;
        if (!drop) {
            if (!first)
                neu[nlen++] = ';';
            memcpy(neu + nlen, p, seglen);
            nlen += seglen;
            first = false;
        }
        if (!semi)
            break;
        p = semi + 1;
    }
    neu[nlen] = '\0';

    LONG rc = RegSetValueExA(key, "Path", 0, type,
                             (const BYTE *)neu, (DWORD)(nlen + 1));
    free(cur);
    free(neu);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS)
        return -1;

    broadcast_env_change();
    return 1;
}

/* Query-only: is `dir` currently on the user PATH? */
static bool path_has(const char *dir)
{
    HKEY key;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Environment", 0,
                      KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    DWORD type;
    char *cur = path_read(key, &type);
    bool found = cur && path_contains(cur, dir);
    free(cur);
    RegCloseKey(key);
    return found;
}

/* A running executable cannot delete its own image, so when `wtop --uninstall`
 * is invoked from the installed copy we hand the deletion to a detached cmd that
 * waits a moment (until we have exited) then removes the file and, if empty, the
 * directory. */
static void schedule_self_delete(const char *exe, const char *dir)
{
    char cmd[2 * MAX_PATH + 128];
    snprintf(cmd, sizeof(cmd),
             "cmd.exe /C ping 127.0.0.1 -n 2 >nul & del /f /q \"%s\" "
             "& rmdir \"%s\" 2>nul",
             exe, dir);

    STARTUPINFOA si = { 0 };
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                       CREATE_NO_WINDOW | DETACHED_PROCESS,
                       NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

/* ------------------------------ public API ------------------------------- */

int wtop_install(void)
{
    char dir[MAX_PATH], dst[MAX_PATH], src[MAX_PATH];
    if (!install_dir(dir, sizeof(dir)) || !install_exe_path(dst, sizeof(dst))) {
        fprintf(stderr, "wtop: APPDATA is not set; cannot locate an install dir.\n");
        return 1;
    }

    /* Refuse to clobber an existing installation. */
    bool have_exe = file_exists(dst);
    bool have_path = path_has(dir);
    if (have_exe || have_path) {
        fprintf(stderr, "wtop is already installed:\n");
        if (have_exe)  fprintf(stderr, "  executable: %s\n", dst);
        if (have_path) fprintf(stderr, "  PATH entry: %s\n", dir);
        fprintf(stderr, "Run 'wtop --uninstall' first to remove it.\n");
        return 1;
    }

    if (!GetModuleFileNameA(NULL, src, sizeof(src))) {
        fprintf(stderr, "wtop: cannot determine the current executable path "
                        "(error %lu).\n", GetLastError());
        return 1;
    }

    if (!CreateDirectoryA(dir, NULL) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        fprintf(stderr, "wtop: cannot create %s (error %lu).\n",
                dir, GetLastError());
        return 1;
    }

    /* bFailIfExists=TRUE: we already checked, this just double-guards. */
    if (!CopyFileA(src, dst, TRUE)) {
        fprintf(stderr, "wtop: failed to copy executable to %s (error %lu).\n",
                dst, GetLastError());
        return 1;
    }
    printf("Installed wtop to %s\n", dst);

    int pr = path_add(dir);
    if (pr < 0) {
        fprintf(stderr, "wtop: copied the executable but could not update PATH.\n"
                        "Add this directory to PATH manually:\n  %s\n", dir);
        return 1;
    }
    if (pr == 1)
        printf("Added %s to your PATH.\n", dir);
    else
        printf("%s is already on your PATH.\n", dir);

    printf("Done. Open a NEW console and run: wtop\n");
    return 0;
}

int wtop_uninstall(void)
{
    char dir[MAX_PATH], dst[MAX_PATH], self[MAX_PATH];
    if (!install_dir(dir, sizeof(dir)) || !install_exe_path(dst, sizeof(dst))) {
        fprintf(stderr, "wtop: APPDATA is not set; cannot locate an install dir.\n");
        return 1;
    }

    bool have_exe = file_exists(dst);
    bool have_path = path_has(dir);
    if (!have_exe && !have_path) {
        fprintf(stderr, "wtop is not installed (nothing at %s).\n", dst);
        return 1;
    }

    int pr = path_remove(dir);
    if (pr == 1)
        printf("Removed %s from your PATH.\n", dir);

    if (have_exe) {
        bool running_installed =
            GetModuleFileNameA(NULL, self, sizeof(self)) && same_path(self, dst);
        if (running_installed) {
            schedule_self_delete(dst, dir);
            printf("Scheduled removal of the running executable %s.\n", dst);
        } else if (DeleteFileA(dst)) {
            printf("Deleted %s\n", dst);
            RemoveDirectoryA(dir);   /* succeeds only if empty (keeps wtoprc) */
        } else {
            fprintf(stderr, "wtop: could not delete %s (error %lu).\n",
                    dst, GetLastError());
        }
    }

    printf("Uninstalled. Your config (%s\\wtoprc) was kept.\n", dir);
    printf("Open a NEW console for the PATH change to take effect.\n");
    return 0;
}
