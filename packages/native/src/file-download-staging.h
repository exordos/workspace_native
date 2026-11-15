#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#elif defined(Q_OS_WIN)
#include <qt_windows.h>
#include <io.h>
#include <fcntl.h>
#include <cstring>
#include <limits>
#include <new>
#include <vector>
#endif

namespace workspace {

// Write directly to an exclusively created file. Verification and publication
// use its retained identity; staging never reopens or replaces a pathname.
class FileDownloadStaging final {
public:
    explicit FileDownloadStaging(const QString &destination)
        : directory(QFileInfo(destination).dir().filePath(QStringLiteral(".workspace-download-XXXXXX")))
    {
        // A parent writer can substitute this pathname. Never recursively
        // delete through it, including constructor/error cleanup.
        directory.setAutoRemove(false);
        if (!directory.isValid()) return;
#ifdef Q_OS_UNIX
        descriptor = ::open(QFile::encodeName(directory.path()).constData(),
                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        struct stat info {};
        if (descriptor < 0) return;
        if (::fstat(descriptor, &info) != 0 || info.st_uid != ::geteuid()
            || (info.st_mode & 0777) != 0700) {
            ::close(descriptor);
            descriptor = -1;
            return;
        }
        const int output = ::openat(descriptor, "payload", O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (output < 0) return;
        created = true;
        if (!file.open(output, QIODevice::WriteOnly, QFileDevice::AutoCloseHandle)) ::close(output);
#elif defined(Q_OS_WIN)
        const auto path = QDir::toNativeSeparators(fileName());
        const auto output = ::CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
            GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (output == INVALID_HANDLE_VALUE) return;
        const int nativeDescriptor = ::_open_osfhandle(reinterpret_cast<intptr_t>(output), _O_BINARY | _O_RDWR);
        if (nativeDescriptor < 0 || !file.open(nativeDescriptor, QIODevice::WriteOnly, QFileDevice::AutoCloseHandle)) {
            FILE_DISPOSITION_INFO disposition {TRUE};
            ::SetFileInformationByHandle(output, FileDispositionInfo, &disposition, sizeof(disposition));
            if (nativeDescriptor < 0) ::CloseHandle(output); else ::_close(nativeDescriptor);
        }
#endif
    }

    ~FileDownloadStaging()
    {
#ifdef Q_OS_UNIX
        file.close();
        if (descriptor >= 0) {
            if (created) ::unlinkat(descriptor, "payload", 0);
            ::close(descriptor);
        }
#elif defined(Q_OS_WIN)
        if (file.isOpen() && !published) {
            FILE_DISPOSITION_INFO disposition {TRUE};
            ::SetFileInformationByHandle(handle(), FileDispositionInfo, &disposition, sizeof(disposition));
        }
        file.close();
#endif
        // Only remove an empty directory. A substituted directory's contents
        // and symlink targets are never traversed or recursively deleted.
        if (directory.isValid()) QDir().rmdir(directory.path());
    }

    bool isValid() const { return file.isOpen(); }
    QString fileName() const { return directory.filePath(QStringLiteral("payload")); }
    qint64 write(const QByteArray &bytes) { return file.write(bytes); }

#ifdef Q_OS_UNIX
    bool publishFromPrivateDirectory(const QString &destination)
    {
        // Another UID cannot replace the child in this owned 0700 directory.
        // The same UID already controls the user's files.
        return descriptor >= 0 && file.flush() && ::linkat(descriptor, "payload", AT_FDCWD,
            QFile::encodeName(destination).constData(), 0) == 0;
    }
#endif

    bool commit(const QString &destination)
    {
        if (!file.flush()) return false;
#ifdef Q_OS_UNIX
        // Match QSaveFile's best-effort sync before publication.
        ::fsync(file.handle());
#ifdef Q_OS_LINUX
        const auto retained = QByteArray("/proc/self/fd/") + QByteArray::number(file.handle());
        return ::linkat(AT_FDCWD, retained.constData(), AT_FDCWD,
            QFile::encodeName(destination).constData(), AT_SYMLINK_FOLLOW) == 0;
#else
        return publishFromPrivateDirectory(destination);
#endif
#elif defined(Q_OS_WIN)
        ::FlushFileBuffers(handle());
        const auto path = QDir::toNativeSeparators(QFileInfo(destination).absoluteFilePath());
        const auto size = static_cast<size_t>(path.size()) * sizeof(wchar_t);
        if (size > std::numeric_limits<DWORD>::max() - sizeof(FILE_RENAME_INFO)) return false;
        std::vector<unsigned char> buffer(sizeof(FILE_RENAME_INFO) + size, 0);
        auto *rename = new (buffer.data()) FILE_RENAME_INFO{};
        rename->ReplaceIfExists = FALSE;
        rename->RootDirectory = nullptr;
        rename->FileNameLength = static_cast<DWORD>(size);
        std::memcpy(rename->FileName, path.utf16(), size);
        published = ::SetFileInformationByHandle(handle(), FileRenameInfo, rename,
            static_cast<DWORD>(buffer.size())) != 0;
        if (published) file.close(); // Completion consumers must be able to open the saved file.
        return published;
#else
        Q_UNUSED(destination);
        return false;
#endif
    }

private:
    QTemporaryDir directory;
    QFile file;
#ifdef Q_OS_UNIX
    int descriptor = -1;
    bool created = false;
#elif defined(Q_OS_WIN)
    HANDLE handle() const { return reinterpret_cast<HANDLE>(::_get_osfhandle(file.handle())); }
    bool published = false;
#endif
};

} // namespace workspace
