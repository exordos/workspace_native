// Replace only the two synthetic files used by native-clipboard-path-race.
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

static void replaceSyntheticPath(const char *path, int flags)
{
    static std::atomic_bool swapped{false};
    if (!(flags & O_PATH) && (flags & O_ACCMODE) == O_RDONLY
        && std::strstr(path, "/cassi-race-selected.bin") && !swapped.exchange(true)) {
        char backup[4096]; std::snprintf(backup, sizeof(backup), "%s.saved", path);
        if (::rename(path, backup) == 0) ::symlink("cassi-race-secret.bin", path);
    }
}

extern "C" int open64(const char *path, int flags, ...)
{
    using Open = int (*)(const char *, int, ...);
    static const auto actual = reinterpret_cast<Open>(dlsym(RTLD_NEXT, "open64"));
    mode_t mode = 0;
    const bool hasMode = (flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE;
    if (hasMode) { va_list args; va_start(args, flags); mode = va_arg(args, int); va_end(args); }
    replaceSyntheticPath(path, flags);
    return hasMode ? actual(path, flags, mode) : actual(path, flags);
}

extern "C" int open(const char *path, int flags, ...)
{
    using Open = int (*)(const char *, int, ...);
    static const auto actual = reinterpret_cast<Open>(dlsym(RTLD_NEXT, "open"));
    mode_t mode = 0;
    const bool hasMode = (flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE;
    if (hasMode) { va_list args; va_start(args, flags); mode = va_arg(args, int); va_end(args); }
    replaceSyntheticPath(path, flags);
    return hasMode ? actual(path, flags, mode) : actual(path, flags);
}
