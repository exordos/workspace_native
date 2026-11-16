// Slow only the synthetic input used by the clipboard worker lifecycle test.
// This keeps the cancellation regression deterministic without large files.
#include <dlfcn.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>

extern "C" ssize_t read(int fd, void *buffer, size_t count)
{
    using Read = ssize_t (*)(int, void *, size_t);
    static const auto actual = reinterpret_cast<Read>(dlsym(RTLD_NEXT, "read"));
    char link[64]; char target[4096];
    std::snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    const auto length = readlink(link, target, sizeof(target) - 1);
    if (length > 0) {
        target[length] = '\0';
        if (std::strstr(target, "/cassi-slow-synthetic.bin")) usleep(150000);
    }
    return actual(fd, buffer, count);
}
