#pragma once

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QtGlobal>

#ifdef Q_OS_LINUX
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace workspace {

struct ClipboardFileIdentity {
    quint64 device = 0, inode = 0;
    qint64 size = 0, modifiedSeconds = 0, modifiedNanoseconds = 0;
    qint64 changedSeconds = 0, changedNanoseconds = 0;
};

#ifdef Q_OS_LINUX
inline bool clipboardIdentity(int descriptor, ClipboardFileIdentity &out)
{
    struct stat value{};
    if (::fstat(descriptor, &value) != 0 || !S_ISREG(value.st_mode)) return false;
    out = {quint64(value.st_dev), quint64(value.st_ino), qint64(value.st_size),
        qint64(value.st_mtim.tv_sec), qint64(value.st_mtim.tv_nsec),
        qint64(value.st_ctim.tv_sec), qint64(value.st_ctim.tv_nsec)};
    return true;
}

inline bool clipboardDescriptorMatchesPath(int descriptor, const QString &canonical)
{
    const auto link = QByteArray("/proc/self/fd/") + QByteArray::number(descriptor);
    char path[4096]; const auto length = ::readlink(link.constData(), path, sizeof(path));
    return length > 0 && length < qint64(sizeof(path)) && QFile::decodeName(QByteArray(path, length)) == canonical;
}

inline bool clipboardSameIdentity(const ClipboardFileIdentity &a, const ClipboardFileIdentity &b)
{
    return a.device == b.device && a.inode == b.inode && a.size == b.size
        && a.modifiedSeconds == b.modifiedSeconds && a.modifiedNanoseconds == b.modifiedNanoseconds
        && a.changedSeconds == b.changedSeconds && a.changedNanoseconds == b.changedNanoseconds;
}
#endif

} // namespace workspace
