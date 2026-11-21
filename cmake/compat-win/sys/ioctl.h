/* sys/ioctl.h -- minimal shim for MinGW-w64. */
#ifndef CASIOEMU_WIN32_SYS_IOCTL_H
#define CASIOEMU_WIN32_SYS_IOCTL_H

#define TIOCGWINSZ 0x5413u
#define TIOCSWINSZ 0x5414u

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

static int casioemu_win32_ioctl(int fd, int request, void* argument) {
    (void)fd;
    (void)request;
    (void)argument;
    return -1;
}
#define ioctl casioemu_win32_ioctl

#endif /* CASIOEMU_WIN32_SYS_IOCTL_H */
