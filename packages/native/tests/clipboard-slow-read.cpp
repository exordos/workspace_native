// Slow only the synthetic input used by the clipboard worker lifecycle test.
// This keeps the cancellation regression deterministic without large files.
#include <dlfcn.h>
#include <fcntl.h>
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
        if (const auto suffix = std::strstr(target, "/cassi-preconfirm-synthetic.bin")) {
            char marker[4096]; const auto prefix = static_cast<size_t>(suffix - target);
            std::snprintf(marker, sizeof(marker), "%.*s/cassi-preconfirm-read.marker", int(prefix), target);
            const int markerFd = ::open(marker, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
            if (markerFd >= 0) ::close(markerFd);
        }
    }
    return actual(fd, buffer, count);
}
