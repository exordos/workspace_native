// Linux-only test interposer. Act once, immediately before the real publication
// syscall, and only for the exact destination supplied by the isolated test.
#include <dlfcn.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
void substitute(const char *source, const char *destination)
{
    const auto *expected = std::getenv("WORKSPACE_TEST_PUBLICATION_DESTINATION");
    const auto *marker = std::getenv("WORKSPACE_TEST_PUBLICATION_MARKER");
    if (!expected || !marker || std::strcmp(destination, expected) != 0
        || ::access(marker, F_OK) == 0) return;
    std::string path(source);
    if (path.rfind("/proc/self/fd/", 0) == 0) {
        // O_TMPFILE descriptors retain their anonymous /proc name even after
        // commit links them. Find the one staging entry in this test's private
        // destination directory instead of assuming readlink names that entry.
        const std::string parent = std::string(destination).substr(0, std::string(destination).rfind('/'));
        auto *directory = ::opendir(parent.c_str());
        if (!directory) return;
        path.clear();
        while (const auto *entry = ::readdir(directory)) {
            if (std::strncmp(entry->d_name, ".workspace-download-", 20) != 0) continue;
            path = parent + '/' + entry->d_name;
            struct stat info {};
            if (::lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) path += "/payload";
            break;
        }
        ::closedir(directory);
        if (path.empty()) return;
    }
    const auto retained = path + ".retained";
    if (::rename(path.c_str(), retained.c_str()) != 0) return;
    const auto *kind = std::getenv("WORKSPACE_TEST_PUBLICATION_SUBSTITUTION");
    if (kind && std::strcmp(kind, "symlink") == 0) {
        if (::symlink("unverified-target", path.c_str()) != 0) return;
    } else {
        const int file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (file < 0) return;
        constexpr char bytes[] = "unverified replacement";
        const auto written = ::write(file, bytes, sizeof(bytes) - 1);
        ::close(file);
        if (written != sizeof(bytes) - 1) return;
    }
    const int proof = ::open(marker, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (proof >= 0) ::close(proof);
}
}

extern "C" int link(const char *source, const char *destination)
{
    const auto original = reinterpret_cast<int (*)(const char *, const char *)>(dlsym(RTLD_NEXT, "link"));
    substitute(source, destination);
    return original(source, destination);
}

extern "C" int linkat(int sourceDirectory, const char *source, int destinationDirectory,
                     const char *destination, int flags)
{
    const auto original = reinterpret_cast<int (*)(int, const char *, int, const char *, int)>(dlsym(RTLD_NEXT, "linkat"));
    substitute(source, destination);
    return original(sourceDirectory, source, destinationDirectory, destination, flags);
}
