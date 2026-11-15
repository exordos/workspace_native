#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <qt_windows.h>
#include <sddl.h>

// Interpose only this translation unit's production CreateFileW call. The
// substitution runs immediately before the real syscall, without a product
// test hook, sleep, or probabilistic race.
static HANDLE WINAPI stagingCreateFileW(LPCWSTR, DWORD, DWORD,
                                        LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
#define CreateFileW stagingCreateFileW
#include "../../src/file-download-staging.h"
#undef CreateFileW

namespace {
QString substitutionRoot;
bool substitutionAttempted = false;
bool substitutionCompleted = false;

LPCWSTR wide(const QString &path) { return reinterpret_cast<LPCWSTR>(path.utf16()); }

bool makePublicDirectory(const QString &path)
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;OICI;FA;;;WD)", SDDL_REVISION_1, &descriptor, nullptr)) return false;
    SECURITY_ATTRIBUTES attributes {sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    const bool result = ::CreateDirectoryW(wide(path), &attributes);
    ::LocalFree(descriptor);
    return result;
}

QByteArray readAsWorldRestrictedToken(const QString &path, DWORD *failure)
{
    // Two real OS access checks must pass: the current user's ordinary token
    // and a restricting Everyone SID. Owner/admin rights cannot satisfy the
    // second check. SeChangeNotifyPrivilege still permits directory traversal.
    HANDLE original = nullptr;
    HANDLE restricted = nullptr;
    QByteArray bytes;
    *failure = ERROR_SUCCESS;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY
                            | TOKEN_IMPERSONATE, &original)) {
        *failure = ::GetLastError();
        return bytes;
    }
    char world[SECURITY_MAX_SID_SIZE];
    DWORD size = sizeof(world);
    if (!::CreateWellKnownSid(WinWorldSid, nullptr, world, &size)) {
        *failure = ::GetLastError();
        ::CloseHandle(original);
        return bytes;
    }
    SID_AND_ATTRIBUTES restricting {world, 0};
    const bool tokenReady = ::CreateRestrictedToken(original, DISABLE_MAX_PRIVILEGE,
        0, nullptr, 0, nullptr, 1, &restricting, &restricted);
    ::CloseHandle(original);
    if (!tokenReady) {
        *failure = ::GetLastError();
        return bytes;
    }
    if (::ImpersonateLoggedOnUser(restricted)) {
        const auto file = ::CreateFileW(wide(path), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        *failure = file == INVALID_HANDLE_VALUE ? ::GetLastError() : ERROR_SUCCESS;
        if (file != INVALID_HANDLE_VALUE) {
            char buffer[128];
            DWORD count = 0;
            if (::ReadFile(file, buffer, sizeof(buffer), &count, nullptr))
                bytes = QByteArray(buffer, count);
            else *failure = ::GetLastError();
            ::CloseHandle(file);
        }
        const bool reverted = ::RevertToSelf();
        if (!reverted) qFatal("Could not restore the test thread's token");
    } else *failure = ::GetLastError();
    ::CloseHandle(restricted);
    return bytes;
}
}

static HANDLE WINAPI stagingCreateFileW(LPCWSTR name, DWORD access, DWORD sharing,
                                        LPSECURITY_ATTRIBUTES attributes, DWORD creation,
                                        DWORD flags, HANDLE templateFile)
{
    const QString path = QString::fromWCharArray(name);
    if (!substitutionRoot.isEmpty() && !substitutionAttempted
        && QFileInfo(path).fileName() == QStringLiteral("payload")) {
        substitutionAttempted = true;
        const auto parent = QFileInfo(path).absolutePath();
        const auto retained = parent + QStringLiteral(".retained");
        const auto target = QDir(substitutionRoot).filePath(QStringLiteral("public-target"));
        if (makePublicDirectory(target)
            && ::MoveFileExW(wide(parent), wide(retained), MOVEFILE_WRITE_THROUGH)) {
            substitutionCompleted = QProcess::execute(QStringLiteral("cmd.exe"),
                {QStringLiteral("/d"), QStringLiteral("/c"), QStringLiteral("mklink"),
                 QStringLiteral("/J"), QDir::toNativeSeparators(parent),
                 QDir::toNativeSeparators(target)}) == 0;
        }
    }
    return ::CreateFileW(name, access, sharing, attributes, creation, flags, templateFile);
}

class WindowsFileStagingTest final : public QObject {
    Q_OBJECT
private slots:
    void cleanup()
    {
        substitutionRoot.clear();
        substitutionAttempted = false;
        substitutionCompleted = false;
    }

    void publishesVerifiedBytes()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto destination = root.filePath(QStringLiteral("saved.bin"));
        workspace::FileDownloadStaging staging(destination);
        QVERIFY(staging.isValid());
        QCOMPARE(staging.write("verified attachment"), 19);
        QVERIFY(staging.commit(destination));
        QFile result(destination);
        QVERIFY(result.open(QIODevice::ReadOnly));
        QCOMPARE(result.readAll(), QByteArray("verified attachment"));
    }

    void competingDestinationIsNeverReplaced()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto destination = root.filePath(QStringLiteral("saved.bin"));
        workspace::FileDownloadStaging staging(destination);
        QVERIFY(staging.isValid());
        QCOMPARE(staging.write("verified attachment"), 19);
        QFile competitor(destination);
        QVERIFY(competitor.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        QCOMPARE(competitor.write("existing destination"), 20);
        competitor.close();
        QVERIFY(!staging.commit(destination));
        QVERIFY(competitor.open(QIODevice::ReadOnly));
        QCOMPARE(competitor.readAll(), QByteArray("existing destination"));
    }

    void substitutedJunctionCannotGrantAttachmentReadAccess()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        substitutionRoot = root.path();
        const auto destination = root.filePath(QStringLiteral("saved.bin"));
        workspace::FileDownloadStaging staging(destination);
        QVERIFY2(substitutionAttempted, "The actual production file-creation syscall must be intercepted");
        QVERIFY2(substitutionCompleted, "The native directory junction must replace the staging pathname");
        QVERIFY(staging.isValid());
        QCOMPARE(staging.write("verified attachment"), 19);
        QVERIFY(staging.commit(destination));

        // Prove the restricted reader can really read a file inheriting the
        // attacker's directory ACL, then check the published production file.
        const auto control = root.filePath(QStringLiteral("public-target/control.bin"));
        QFile publicFile(control);
        QVERIFY(publicFile.open(QIODevice::WriteOnly));
        QCOMPARE(publicFile.write("public control"), 14);
        publicFile.close();
        DWORD error = ERROR_SUCCESS;
        QCOMPARE(readAsWorldRestrictedToken(control, &error), QByteArray("public control"));
        QCOMPARE(error, DWORD(ERROR_SUCCESS));
        const auto exposed = readAsWorldRestrictedToken(destination, &error);
        QVERIFY2(exposed.isEmpty(), "A world-restricted token read the published attachment through inherited ACLs");
        QCOMPARE(error, DWORD(ERROR_ACCESS_DENIED));
        QFile ownFile(destination);
        QVERIFY(ownFile.open(QIODevice::ReadOnly));
        QCOMPARE(ownFile.readAll(), QByteArray("verified attachment"));
    }
};

QTEST_GUILESS_MAIN(WindowsFileStagingTest)
#include "windows-file-staging-test.moc"
