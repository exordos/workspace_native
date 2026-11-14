#pragma once
// Portable-equivalent shims for the descriptor/console APIs the diagnostics
// test cases use (PR #289 review thread 4575693897).
//
// The raw POSIX headers (fcntl.h, poll.h, sys/stat.h, unistd.h) stay behind
// #if defined(Q_OS_UNIX) in the test source; on platforms without them these
// inline wrappers keep the console-capture cases compiling and behaviourally
// neutral: descriptors are plain handles (stderr is descriptor 2 everywhere),
// flushing/clearing the CRT stream is a no-op on Windows because its stderr
// writes are unbuffered and synchronous, and the poll-based console reader
// simply reports "no console bytes available" so timing assertions degrade to
// their documented no-op branch instead of hanging the suite.

#include <QtGlobal>

#if defined(Q_OS_UNIX)
#include <cstddef>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

inline constexpr int kNativeTestStderrFd = 2; // CRT descriptor for stderr on every platform

inline int nativeTestPollRead(int fd, char *buffer, int capacity, int timeoutMs)
{
#if defined(Q_OS_UNIX)
    struct pollfd pfd { fd, POLLIN, 0 };
    if (::poll(&pfd, 1, timeoutMs) <= 0)
        return 0;
    return static_cast<int>(::read(fd, buffer, static_cast<std::size_t>(capacity)));
#else
    Q_UNUSED(fd);
    Q_UNUSED(buffer);
    Q_UNUSED(capacity);
    Q_UNUSED(timeoutMs);
    return 0; // no pollable console pipe on non-POSIX platforms
#endif
}

inline void nativeTestFlushStderr()
{
#if defined(Q_OS_UNIX)
    ::fflush(stderr);
#endif
}

inline void nativeTestClearErr()
{
#if defined(Q_OS_UNIX)
    ::clearerr(stderr);
#endif
}
