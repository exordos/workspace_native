#include "session-vault.h"
#include "domain.h"

#include <qt6keychain/keychain.h>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

namespace workspace {
namespace {
const QString Service = QStringLiteral("com.exordos.workspace.native.sessions");

QUrl canonicalOrigin(const QUrl &origin)
{
    if (!origin.isValid() || origin.isRelative() || origin.scheme() != "https"
        || origin.host().isEmpty() || origin.authority().contains('@')
        || origin.hasQuery() || origin.hasFragment()
        || (!origin.path().isEmpty() && origin.path() != "/"))
        return {};
    QUrl normalized = origin;
    normalized.setHost(origin.host().toLower());
    normalized.setPath({});
    if (normalized.port() == 443) normalized.setPort(-1);
    return normalized;
}

SavedAccount normalizedAccount(SavedAccount account)
{
    account.server = canonicalOrigin(account.server);
    account.project = account.project.toLower();
    account.user = account.user.toLower();
    const auto id = SessionVault::accountId(account.server, account.project, account.user);
    if (id.isEmpty() || (!account.id.isEmpty() && account.id != id)) return {};
    account.id = id;
    return account;
}

bool validId(const QString &id)
{
    if (id.size() != 64) return false;
    for (const auto character : id)
        if (!((character >= u'0' && character <= u'9') || (character >= u'a' && character <= u'f')))
            return false;
    return true;
}

SessionSecretStore::Error secretError(QKeychain::Error error)
{
    using Error = SessionSecretStore::Error;
    switch (error) {
    case QKeychain::NoError: return Error::None;
    case QKeychain::EntryNotFound: return Error::NotFound;
    case QKeychain::NoBackendAvailable:
    case QKeychain::NotImplemented: return Error::Unavailable;
    case QKeychain::AccessDenied:
    case QKeychain::AccessDeniedByUser: return Error::Denied;
    default: return Error::Failed;
    }
}

QString secretErrorText(SessionSecretStore::Error error)
{
    using Error = SessionSecretStore::Error;
    switch (error) {
    case Error::None: return {};
    case Error::NotFound: return SessionVault::tr("The saved credential is no longer available.");
    case Error::Unavailable: return SessionVault::tr("The operating system's secure credential store is unavailable.");
    case Error::Denied: return SessionVault::tr("Access to the operating system's secure credential store was denied.");
    case Error::Failed: return SessionVault::tr("The secure credential operation failed.");
    }
    return SessionVault::tr("The secure credential operation failed.");
}
} // namespace

KeychainSessionSecretStore::KeychainSessionSecretStore(QObject *parent, JobStarter starter)
    : SessionSecretStore(parent), m_start(std::move(starter))
{
    if (!m_start) m_start = [](QKeychain::Job *job) { job->start(); };
}

void KeychainSessionSecretStore::write(const QString &key, const QByteArray &secret, Completion done)
{
    auto *job = new QKeychain::WritePasswordJob(Service, this);
    job->setInsecureFallback(false);
    job->setKey(key);
    job->setBinaryData(secret);
    connect(job, &QKeychain::Job::finished, this, [done = std::move(done)](QKeychain::Job *finished) {
        done(secretError(finished->error()));
    });
    m_start(job);
}

void KeychainSessionSecretStore::read(const QString &key, Loaded done)
{
    auto *job = new QKeychain::ReadPasswordJob(Service, this);
    job->setInsecureFallback(false);
    job->setKey(key);
    connect(job, &QKeychain::Job::finished, this, [job, done = std::move(done)](QKeychain::Job *) {
        const auto error = secretError(job->error());
        done(error == Error::None ? job->binaryData() : QByteArray{}, error);
    });
    m_start(job);
}

void KeychainSessionSecretStore::remove(const QString &key, Completion done)
{
    auto *job = new QKeychain::DeletePasswordJob(Service, this);
    job->setInsecureFallback(false);
    job->setKey(key);
    connect(job, &QKeychain::Job::finished, this, [done = std::move(done)](QKeychain::Job *finished) {
        done(secretError(finished->error()));
    });
    m_start(job);
}

SessionVault::SessionVault(QString metadataFile, SessionSecretStore *backend, QObject *parent)
    : QObject(parent), m_metadataFile(std::move(metadataFile)), m_backend(backend)
{
    if (!m_backend) m_backend = new KeychainSessionSecretStore(this);
    if (m_metadataFile.isEmpty())
        m_metadataFile = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
            + "/session-vault/accounts.ini";
    const QFileInfo info(m_metadataFile);
    if (info.isSymLink() || !QDir().mkpath(info.absolutePath())) {
        m_storageError = tr("Private saved-account metadata could not be opened.");
        return;
    }
    QFile file(m_metadataFile);
    if (!file.open(QIODevice::ReadWrite)
        || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        m_storageError = tr("Private saved-account metadata could not be opened.");
        return;
    }
    file.close();
    QSettings settings(m_metadataFile, QSettings::IniFormat);
    settings.setFallbacksEnabled(false);
    const int count = settings.beginReadArray("accounts");
    for (int index = 0; index < count; ++index) {
        settings.setArrayIndex(index);
        SavedAccount account{settings.value("id").toString(), QUrl(settings.value("server").toString()),
            settings.value("project").toString(), settings.value("user").toString(),
            settings.value("login").toString(), settings.value("projectName").toString()};
        // Never infer a missing ID from untrusted on-disk metadata.
        if (account.id.isEmpty()) continue;
        account = normalizedAccount(account);
        if (!account.id.isEmpty()) m_accounts.insert(account.id, account);
    }
    settings.endArray();
    if (settings.status() != QSettings::NoError) {
        m_accounts.clear();
        m_storageError = tr("Saved-account metadata could not be read.");
    }
}

QString SessionVault::accountId(const QUrl &server, const QString &project, const QString &user)
{
    const auto origin = canonicalOrigin(server);
    if (origin.isEmpty() || !isUuid(project) || !isUuid(user)) return {};
    const auto identity = QJsonDocument(QJsonArray{QStringLiteral("workspace-session-v1"),
        origin.toString(QUrl::FullyEncoded), project.toLower(), user.toLower()}).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
}

QVector<SavedAccount> SessionVault::accounts() const
{
    QVector<SavedAccount> visible;
    for (const auto &account : m_accounts)
        if (!m_forgetting.contains(account.id)) visible.append(account);
    return visible;
}

QString SessionVault::writeMetadata(const QMap<QString, SavedAccount> &accounts)
{
    if (!m_storageError.isEmpty()) return m_storageError;
    if (QFileInfo(m_metadataFile).isSymLink()) return tr("Saved-account metadata is not a regular private file.");
    QSettings settings(m_metadataFile, QSettings::IniFormat);
    settings.setFallbacksEnabled(false);
    settings.remove("accounts");
    settings.beginWriteArray("accounts", int(accounts.size()));
    int index = 0;
    for (const auto &account : accounts) {
        settings.setArrayIndex(index++);
        settings.setValue("id", account.id);
        settings.setValue("server", account.server.toString(QUrl::FullyEncoded));
        settings.setValue("project", account.project);
        settings.setValue("user", account.user);
        settings.setValue("login", account.login);
        settings.setValue("projectName", account.projectName);
    }
    settings.endArray();
    settings.sync();
    if (settings.status() != QSettings::NoError
        || !QFile::setPermissions(m_metadataFile, QFileDevice::ReadOwner | QFileDevice::WriteOwner))
        return tr("Saved-account metadata could not be updated.");
    return {};
}

void SessionVault::enqueue(std::function<void()> operation)
{
    m_queue.push_back(std::move(operation));
    startNext();
}

void SessionVault::startNext()
{
    if (m_running || m_queue.empty()) return;
    m_running = true;
    auto operation = std::move(m_queue.front());
    m_queue.pop_front();
    operation();
}

void SessionVault::complete(Completion done, const QString &error)
{
    m_running = false;
    // Schedule first: a consumer callback is allowed to destroy this vault.
    QTimer::singleShot(0, this, &SessionVault::startNext);
    if (done) done(error);
}

void SessionVault::save(SavedAccount account, QByteArray refreshToken, Completion done)
{
    account = normalizedAccount(std::move(account));
    if (account.id.isEmpty() || refreshToken.isEmpty() || !m_storageError.isEmpty()) {
        const auto error = m_storageError.isEmpty() ? tr("The saved account identity or credential is invalid.") : m_storageError;
        QTimer::singleShot(0, this, [done, error] { if (done) done(error); });
        return;
    }
    const auto revision = m_revisions.value(account.id);
    enqueue([this, account, refreshToken, done, revision] {
        if (m_revisions.value(account.id) != revision) {
            complete(done, tr("The saved-account operation was superseded."));
            return;
        }
        if (!m_backend) { complete(done, secretErrorText(SessionSecretStore::Error::Unavailable)); return; }
        const QPointer<SessionVault> guard(this);
        m_backend->write(account.id, refreshToken, [guard, account, done, revision](SessionSecretStore::Error error) {
            if (!guard) return;
            if (guard->m_revisions.value(account.id) != revision) {
                guard->complete(done, tr("The saved-account operation was superseded."));
                return;
            }
            if (error != SessionSecretStore::Error::None) { guard->complete(done, secretErrorText(error)); return; }
            auto updated = guard->m_accounts;
            updated.insert(account.id, account);
            const auto metadataError = guard->writeMetadata(updated);
            if (metadataError.isEmpty()) {
                guard->m_accounts = updated;
                guard->m_forgetting.remove(account.id);
                emit guard->accountsChanged();
            }
            if (!guard) return;
            if (!metadataError.isEmpty() && !guard->m_accounts.contains(account.id) && guard->m_backend) {
                guard->m_backend->remove(account.id, [guard, done, metadataError](SessionSecretStore::Error) {
                    if (guard) guard->complete(done, metadataError);
                });
                return;
            }
            guard->complete(done, metadataError);
        });
    });
}

void SessionVault::load(const QString &id, Loaded done)
{
    const auto revision = m_revisions.value(id);
    enqueue([this, id, done, revision] {
        const auto account = m_accounts.value(id);
        if (!done) { complete({}, {}); return; }
        if (account.id.isEmpty() || m_forgetting.contains(id) || normalizedAccount(account).id != id
            || revision != m_revisions.value(id)) {
            complete([done](QString error) { done({}, {}, error); }, tr("The saved account is no longer available."));
            return;
        }
        if (!m_backend) {
            complete([done](QString error) { done({}, {}, error); }, secretErrorText(SessionSecretStore::Error::Unavailable));
            return;
        }
        const QPointer<SessionVault> guard(this);
        m_backend->read(id, [guard, id, account, revision, done](QByteArray token, SessionSecretStore::Error error) {
            if (!guard) return;
            if (guard->m_revisions.value(id) != revision || guard->m_forgetting.contains(id)) {
                guard->complete([done](QString message) { done({}, {}, message); }, tr("The saved-account operation was superseded."));
                return;
            }
            if (error == SessionSecretStore::Error::NotFound) {
                guard->m_accounts.remove(id);
                guard->writeMetadata(guard->m_accounts);
                emit guard->accountsChanged();
            }
            if (!guard) return;
            if (error != SessionSecretStore::Error::None || token.isEmpty()) {
                guard->complete([done](QString message) { done({}, {}, message); },
                    secretErrorText(error == SessionSecretStore::Error::None ? SessionSecretStore::Error::NotFound : error));
                return;
            }
            guard->complete([done, account, token](QString message) { done(account, token, message); }, {});
        });
    });
}

void SessionVault::forget(const QString &id, Completion done)
{
    if (!validId(id)) {
        QTimer::singleShot(0, this, [done] { if (done) done(tr("The saved account identity is invalid.")); });
        return;
    }
    ++m_revisions[id];
    m_forgetting.insert(id);
    const QPointer<SessionVault> guard(this);
    emit accountsChanged();
    if (!guard) return;
    enqueue([this, id, done] {
        m_accounts.remove(id);
        const auto metadataError = writeMetadata(m_accounts);
        if (!m_backend) { complete(done, metadataError.isEmpty() ? secretErrorText(SessionSecretStore::Error::Unavailable) : metadataError); return; }
        const QPointer<SessionVault> guard(this);
        m_backend->remove(id, [guard, done, metadataError](SessionSecretStore::Error error) {
            if (!guard) return;
            guard->complete(done, !metadataError.isEmpty() ? metadataError
                : error == SessionSecretStore::Error::NotFound ? QString{} : secretErrorText(error));
        });
    });
}

} // namespace workspace
