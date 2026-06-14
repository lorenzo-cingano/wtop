#ifndef WTOP_INSTALL_H
#define WTOP_INSTALL_H

/* Self-installation: copy the running wtop.exe into %APPDATA%\wtop and add that
 * directory to the per-user PATH, so `wtop` runs from any console. Both routines
 * print progress to stdout/stderr and return 0 on success, non-zero on failure.
 *
 * wtop_install refuses to run if an installation is already present.
 * wtop_uninstall removes the executable and the PATH entry (the config file is
 * left in place). */

int wtop_install(void);
int wtop_uninstall(void);

#endif /* WTOP_INSTALL_H */
