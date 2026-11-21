/* pty.h -- stub for MinGW-w64 (no POSIX pseudo-terminals).
 *
 * forkpty() fails with ENOSYS, so the in-app terminal reports an error at
 * runtime on Windows.  Build-only compatibility; no source changes needed.
 */
#ifndef CASIOEMU_WIN32_PTY_H
#define CASIOEMU_WIN32_PTY_H

#include <errno.h>
#include <sys/types.h>

static pid_t casioemu_win32_forkpty(int* master, char* name,
                                    const void* termp, const void* winp) {
    (void)master; (void)name; (void)termp; (void)winp;
    errno = ENOSYS;
    return (pid_t)-1;
}
#define forkpty casioemu_win32_forkpty

#endif /* CASIOEMU_WIN32_PTY_H */
