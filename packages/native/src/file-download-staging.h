#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryDir>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#elif defined(Q_OS_WIN)
#include <qt_windows.h>
#include <io.h>
#include <cstring>
#include <limits>
#include <new>
#include <vector>
#endif

namespace workspace {

// Private implementation of a verified, no-replace download publication.
// The staging directory is on the destination filesystem, with no shared
// writer access. Retain its identity even if a writer of its parent renames it.
class FileDownloadStaging final {
public:
    explicit FileDownloadStaging(const QString &destination)
        : directory(QFileInfo(destination).dir().filePath(QStringLiteral(".workspace-download-XXXXXX")))
    {
#ifdef Q_OS_UNIX
        if (!directory.isValid()) return;
        descriptor = ::open(QFile::encodeName(directory.path()).constData(),
                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        struct stat info {};
        if (descriptor >= 0 && (::fstat(descriptor, &info) != 0
            || info.st_uid != ::geteuid() || (info.st_mode & 0777) != 0700)) {
            ::close(descriptor);
            descriptor = -1;
        }
#endif
    }

    ~FileDownloadStaging()
    {
#ifdef Q_OS_UNIX
        if (descriptor >= 0) ::close(descriptor);
#endif
    }

    bool isValid() const
    {
#ifdef Q_OS_UNIX
        return descriptor >= 0;
#else
        return directory.isValid();
#endif
    }

    QString fileName() const { return directory.filePath(QStringLiteral("payload")); }

#ifdef Q_OS_UNIX
    bool publishFromPrivateDirectory(const QString &destination) const
    {
        // Other UIDs may rename the directory through its parent, but cannot
        // replace this child. A same-UID process already owns the user's files.
        return descriptor >= 0 && ::linkat(descriptor, "payload", AT_FDCWD,
            QFile::encodeName(destination).constData(), 0) == 0;
    }
#endif

    bool commit(QSaveFile &output, const QString &destination) const
    {
#ifdef Q_OS_LINUX
        // QSaveFile can use O_TMPFILE. Keep that exact inode across commit;
        // reopening the resulting staging pathname would permit substitution.
        const int file = ::fcntl(output.handle(), F_DUPFD_CLOEXEC, 0);
        if (file < 0) return false;
        const bool committed = output.commit();
        const auto retained = QByteArray("/proc/self/fd/") + QByteArray::number(file);
        const bool published = committed && ::linkat(AT_FDCWD, retained.constData(), AT_FDCWD,
            QFile::encodeName(destination).constData(), AT_SYMLINK_FOLLOW) == 0;
        ::close(file);
        return published;
#elif defined(Q_OS_UNIX)
        return output.commit() && publishFromPrivateDirectory(destination);
#elif defined(Q_OS_WIN)
        // Qt 6.10 QSaveFile uses Win32NonShared: DELETE access, share mode 0,
        // and handle-based rename during commit. Duplicate that same object;
        // ReOpenFile would conflict with its exclusive sharing mode.
        const auto original = reinterpret_cast<HANDLE>(::_get_osfhandle(output.handle()));
        HANDLE retained = INVALID_HANDLE_VALUE;
        if (!::DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(), &retained,
                               0, FALSE, DUPLICATE_SAME_ACCESS)) return false;
        bool published = false;
        if (output.commit()) {
            const auto path = QDir::toNativeSeparators(QFileInfo(destination).absoluteFilePath());
            const auto size = static_cast<size_t>(path.size()) * sizeof(wchar_t);
            if (size <= std::numeric_limits<DWORD>::max() - sizeof(FILE_RENAME_INFO)) {
                std::vector<unsigned char> buffer(sizeof(FILE_RENAME_INFO) + size, 0);
                auto *rename = new (buffer.data()) FILE_RENAME_INFO{};
                rename->ReplaceIfExists = FALSE;
                rename->RootDirectory = nullptr;
                rename->FileNameLength = static_cast<DWORD>(size);
                std::memcpy(rename->FileName, path.utf16(), size);
                published = ::SetFileInformationByHandle(retained, FileRenameInfo, rename,
                    static_cast<DWORD>(buffer.size())) != 0;
            }
        }
        ::CloseHandle(retained);
        return published;
#else
        Q_UNUSED(output);
        Q_UNUSED(destination);
        return false;
#endif
    }

private:
    QTemporaryDir directory;
#ifdef Q_OS_UNIX
    int descriptor = -1;
#endif
};

} // namespace workspace
