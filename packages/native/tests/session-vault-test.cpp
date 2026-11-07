#include "session-vault.h"
#include "mock-server.h"

#include <qt6keychain/keychain.h>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace workspace;
using namespace workspace::test;

class MemorySecrets final : public SessionSecretStore {
public:
    QMap<QString, QByteArray> secrets;
    QStringList operations;
    std::deque<std::function<void()>> pending;
    bool hold = false;
    Error failure = Error::None;
    void dispatch(std::function<void()> operation) {
        if (hold) pending.push_back(std::move(operation));
        else QTimer::singleShot(0, this, std::move(operation));
    }
    void finish() {
        QVERIFY(!pending.empty());
        auto next = std::move(pending.front());
        pending.pop_front();
        next();
    }
    void write(const QString &key, const QByteArray &secret, Completion done) override {
        operations.append("write");
        dispatch([this, key, secret, done] {
            if (failure == Error::None) secrets.insert(key, secret);
            done(failure);
        });
    }
    void read(const QString &key, Loaded done) override {
        operations.append("read");
        dispatch([this, key, done] {
            done(failure == Error::None ? secrets.value(key) : QByteArray{},
                 failure != Error::None ? failure : secrets.contains(key) ? Error::None : Error::NotFound);
        });
    }
    void remove(const QString &key, Completion done) override {
        operations.append("remove");
        dispatch([this, key, done] {
            if (failure == Error::None) secrets.remove(key);
            done(failure);
        });
    }
};

static SavedAccount account(const QString &origin = "https://workspace.example")
{
    SavedAccount saved;
    saved.server = QUrl(origin);
    saved.project = projectId;
    saved.user = userId;
    saved.login = "synthetic-user";
    saved.projectName = "Synthetic project";
    return saved;
}

class SessionVaultTest final : public QObject {
    Q_OBJECT
private slots:
    void keyBindsCanonicalOriginProjectAndUser() {
        const auto key = SessionVault::accountId(QUrl("https://WORKSPACE.example:443/"), projectId, userId);
        QVERIFY(!key.isEmpty());
        QCOMPARE(key, SessionVault::accountId(QUrl("https://workspace.example"), projectId.toUpper(), userId));
        QVERIFY(key != SessionVault::accountId(QUrl("https://other.example"), projectId, userId));
        QVERIFY(key != SessionVault::accountId(QUrl("https://workspace.example:8443"), projectId, userId));
        QVERIFY(key != SessionVault::accountId(QUrl("https://workspace.example"), streamId, userId));
        QVERIFY(key != SessionVault::accountId(QUrl("https://workspace.example"), projectId, streamId));
        for (const auto &origin : {"http://workspace.example", "https://user:pass@workspace.example",
                                  "https://workspace.example/path", "https://workspace.example/?query=1"})
            QVERIFY(SessionVault::accountId(QUrl(origin), projectId, userId).isEmpty());
    }

    void savesOnlyMetadataToPrivateSettings() {
        QTemporaryDir directory;
        MemorySecrets backend;
        const auto path = directory.filePath("accounts.ini");
        SessionVault vault(path, &backend);
        bool saved = false;
        QString failure;
        vault.save(account(), "synthetic-refresh-only-in-keychain", [&](QString error) { failure = error; saved = true; });
        QTRY_VERIFY(saved);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QCOMPARE(vault.accounts().size(), 1);
        QFile metadata(path);
        QVERIFY(metadata.open(QIODevice::ReadOnly));
        const auto data = metadata.readAll();
        QVERIFY(data.contains("synthetic-user"));
        QVERIFY(!data.contains("synthetic-refresh-only-in-keychain"));
        QVERIFY(!data.contains("refresh_token"));
        QVERIFY(!(metadata.permissions() & (QFileDevice::ReadGroup | QFileDevice::WriteGroup
            | QFileDevice::ReadOther | QFileDevice::WriteOther)));
        bool loaded = false;
        vault.load(vault.accounts().first().id, [&](SavedAccount value, QByteArray token, QString error) {
            QVERIFY(error.isEmpty());
            QCOMPARE(value.server, QUrl("https://workspace.example"));
            QCOMPARE(value.user, userId);
            QCOMPARE(token, QByteArray("synthetic-refresh-only-in-keychain"));
            loaded = true;
        });
        QTRY_VERIFY(loaded);
    }

    void keychainJobsNeverEnablePlaintextFallback() {
        QTemporaryDir directory;
        int jobs = 0;
        KeychainSessionSecretStore backend(nullptr, [&](QKeychain::Job *job) {
            ++jobs;
            QVERIFY(!job->insecureFallback());
            QTimer::singleShot(0, job, [job] {
                job->emitFinishedWithError(QKeychain::NoBackendAvailable, "Synthetic unavailable backend");
            });
        });
        SessionVault vault(directory.filePath("accounts.ini"), &backend);
        bool saved = false;
        vault.save(account(), "synthetic-refresh-never-plaintext", [&](QString error) {
            QVERIFY(!error.isEmpty()); saved = true;
        });
        QTRY_VERIFY(saved);
        QVERIFY(vault.accounts().isEmpty());
        bool read = false;
        backend.read("synthetic-key", [&](QByteArray token, SessionSecretStore::Error error) {
            QVERIFY(token.isEmpty());
            QCOMPARE(error, SessionSecretStore::Error::Unavailable);
            read = true;
        });
        QTRY_VERIFY(read);
        bool removed = false;
        backend.remove("synthetic-key", [&](SessionSecretStore::Error error) {
            QCOMPARE(error, SessionSecretStore::Error::Unavailable); removed = true;
        });
        QTRY_VERIFY(removed);
        QCOMPARE(jobs, 3);
        QFile metadata(directory.filePath("accounts.ini"));
        if (metadata.open(QIODevice::ReadOnly))
            QVERIFY(!metadata.readAll().contains("synthetic-refresh-never-plaintext"));
    }

    void forgetFencesInFlightSaveAndSerializesDelete() {
        QTemporaryDir directory;
        MemorySecrets backend;
        backend.hold = true;
        SessionVault vault(directory.filePath("accounts.ini"), &backend);
        auto saved = account();
        saved.id = SessionVault::accountId(saved.server, saved.project, saved.user);
        bool writeDone = false, forgetDone = false;
        vault.save(saved, "synthetic-late-token", [&](QString) { writeDone = true; });
        QCOMPARE(backend.operations, QStringList{"write"});
        vault.forget(saved.id, [&](QString error) { QVERIFY(error.isEmpty()); forgetDone = true; });
        QCOMPARE(backend.operations.size(), 1);
        backend.finish();
        QTRY_VERIFY(writeDone);
        QTRY_COMPARE(backend.operations, (QStringList{"write", "remove"}));
        backend.finish();
        QTRY_VERIFY(forgetDone);
        QVERIFY(vault.accounts().isEmpty());
        QVERIFY(backend.secrets.isEmpty());
        SessionVault restarted(directory.filePath("accounts.ini"), &backend);
        QVERIFY(restarted.accounts().isEmpty());
    }

    void rejectsForgedOrTamperedMetadataOwnership() {
        QTemporaryDir directory;
        MemorySecrets backend;
        const auto path = directory.filePath("accounts.ini");
        SessionVault vault(path, &backend);
        auto forged = account();
        forged.id = SessionVault::accountId(QUrl("https://other.example"), projectId, userId);
        bool rejected = false;
        vault.save(forged, "synthetic-token", [&](QString error) { rejected = !error.isEmpty(); });
        QTRY_VERIFY(rejected);
        QVERIFY(backend.operations.isEmpty());
        bool saved = false;
        vault.save(account(), "synthetic-token", [&](QString error) { QVERIFY(error.isEmpty()); saved = true; });
        QTRY_VERIFY(saved);
        const auto id = vault.accounts().first().id;
        QSettings metadata(path, QSettings::IniFormat);
        metadata.beginWriteArray("accounts", 1);
        metadata.setArrayIndex(0);
        metadata.setValue("id", id);
        metadata.setValue("server", "https://other.example");
        metadata.setValue("project", projectId);
        metadata.setValue("user", userId);
        metadata.endArray();
        metadata.sync();
        SessionVault restarted(path, &backend);
        QVERIFY(restarted.accounts().isEmpty());
        const auto calls = backend.operations.size();
        bool loadRejected = false;
        restarted.load(id, [&](SavedAccount, QByteArray token, QString error) {
            QVERIFY(token.isEmpty()); loadRejected = !error.isEmpty();
        });
        QTRY_VERIFY(loadRejected);
        QCOMPARE(backend.operations.size(), calls);
    }

    void sequentialRotationKeepsNewestSecret() {
        QTemporaryDir directory;
        MemorySecrets backend;
        backend.hold = true;
        SessionVault vault(directory.filePath("accounts.ini"), &backend);
        int saves = 0;
        vault.save(account(), "synthetic-old-refresh", [&](QString error) { QVERIFY(error.isEmpty()); ++saves; });
        vault.save(account(), "synthetic-rotated-refresh", [&](QString error) { QVERIFY(error.isEmpty()); ++saves; });
        QCOMPARE(backend.operations.size(), 1);
        backend.finish();
        QTRY_COMPARE(backend.operations.size(), 2);
        backend.finish();
        QTRY_COMPARE(saves, 2);
        backend.hold = false;
        bool loaded = false;
        vault.load(vault.accounts().first().id, [&](SavedAccount, QByteArray token, QString error) {
            QVERIFY(error.isEmpty()); QCOMPARE(token, QByteArray("synthetic-rotated-refresh")); loaded = true;
        });
        QTRY_VERIFY(loaded);
    }
};

QTEST_GUILESS_MAIN(SessionVaultTest)
#include "session-vault-test.moc"
