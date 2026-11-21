/* win32-compat.h -- POSIX setenv() shim for MinGW-w64 cross builds.
 *
 * Injected with -include by CasioEmuMsvc/CMakeLists.txt when building for
 * Windows with a non-MSVC compiler.  Keeps platform portability fixes in the
 * build system instead of editing source files owned by other work streams.
 */
#ifndef CASIOEMU_WIN32_COMPAT_H
#define CASIOEMU_WIN32_COMPAT_H

#if defined(_WIN32) && !defined(_MSC_VER)
#include <stdlib.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#ifndef F_GETFL
#  define F_GETFL 3
#endif
#ifndef F_SETFL
#  define F_SETFL 4
#endif
#ifndef O_NONBLOCK
#  define O_NONBLOCK 0x1000
#endif
#ifndef SIGHUP
#  define SIGHUP 1
#endif
#ifndef SIGKILL
#  define SIGKILL 9
#endif

/* The in-app terminal is POSIX-PTY based; stub the calls it makes so the
 * Windows build still links (forkpty() already fails gracefully). */
static int casioemu_win32_fcntl(int fd, int command, ...) {
    (void)fd; (void)command;
    return -1;
}
#define fcntl casioemu_win32_fcntl

static int casioemu_win32_kill(pid_t pid, int signal_number) {
    (void)pid; (void)signal_number;
    return -1;
}
#define kill casioemu_win32_kill

/* localtime_r() -> MSVC/MinGW localtime_s() argument order. */
static struct tm* casioemu_win32_localtime_r(const time_t* when, struct tm* result) {
    return localtime_s(result, when) == 0 ? result : 0;
}
#define localtime_r casioemu_win32_localtime_r

#ifndef S_ISLNK
#  ifndef S_IFLNK
#    define S_IFLNK 0xA000
#  endif
#  define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#endif

/* MinGW-w64 has no lstat(); there are no POSIX symlinks to follow here. */
static int casioemu_win32_lstat(const char* path, struct stat* info) {
    return stat(path, info);
}
#define lstat casioemu_win32_lstat

static int casioemu_win32_setenv(const char* name, const char* value, int overwrite) {
    if (name == 0 || value == 0)
        return -1;
    if (!overwrite && getenv(name) != 0)
        return 0;
    return _putenv_s(name, value) == 0 ? 0 : -1;
}

#define setenv casioemu_win32_setenv
#endif

#endif /* CASIOEMU_WIN32_COMPAT_H */
