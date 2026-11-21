/* sys/wait.h -- minimal shim for MinGW-w64. */
#ifndef CASIOEMU_WIN32_SYS_WAIT_H
#define CASIOEMU_WIN32_SYS_WAIT_H

#include <sys/types.h>

#define WNOHANG 1
#define WUNTRACED 2

static int casioemu_win32_waitpid(pid_t pid, int* status, int options) {
    (void)pid;
    (void)status;
    (void)options;
    return -1;
}
#define waitpid casioemu_win32_waitpid

#endif /* CASIOEMU_WIN32_SYS_WAIT_H */
