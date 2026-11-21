/* dlfcn.h -- minimal POSIX dlopen API shim for MinGW-w64 builds.
 *
 * Provided by the build system (cmake/compat-win is prepended to the include
 * path for Windows non-MSVC targets).  No project source files are modified.
 */
#ifndef CASIOEMU_WIN32_DLFCN_H
#define CASIOEMU_WIN32_DLFCN_H

#include <stdint.h>
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTLD_LAZY   0x0001
#define RTLD_NOW    0x0002
#define RTLD_GLOBAL 0x0100
#define RTLD_LOCAL  0x0000
#define RTLD_DEFAULT ((void*)(intptr_t)-1)

static inline void* casioemu_win32_dlopen(const char* path, int flags) {
    (void)flags;
    if (!path)
        return 0;
    HMODULE module = LoadLibraryA(path);
    return reinterpret_cast<void*>(module);
}

static inline void* casioemu_win32_dlsym(void* handle, const char* name) {
    if (!name)
        return 0;
    HMODULE module;
    if (handle == RTLD_DEFAULT)
        module = GetModuleHandle(0);
    else
        module = reinterpret_cast<HMODULE>(handle);
    if (!module)
        return 0;
    FARPROC symbol = GetProcAddress(module, name);
    return reinterpret_cast<void*>(symbol);
}

static inline int casioemu_win32_dlclose(void* handle) {
    if (!handle || handle == RTLD_DEFAULT)
        return 0;
    return FreeLibrary(reinterpret_cast<HMODULE>(handle)) ? 0 : -1;
}

static inline const char* casioemu_win32_dlerror(void) {
    static __thread char message[512];
    DWORD code = GetLastError();
    if (code == 0)
        return 0;
    DWORD written = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        0, code, 0, message, (DWORD)sizeof(message) - 1, 0);
    if (written == 0) {
        message[0] = 0;
        return "dlopen/dlsym failed";
    }
    message[written] = 0;
    return message;
}

#define dlopen  casioemu_win32_dlopen
#define dlsym   casioemu_win32_dlsym
#define dlclose casioemu_win32_dlclose
#define dlerror casioemu_win32_dlerror

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CASIOEMU_WIN32_DLFCN_H */
