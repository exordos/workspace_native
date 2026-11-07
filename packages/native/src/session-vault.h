#pragma once

#include <QByteArray>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QUrl>
#include <QVector>
#include <deque>
#include <functional>

namespace QKeychain { class Job; }

namespace workspace {

struct SavedAccount {
    QString id;
    QUrl server;
    QString project;
    QString user;
    QString login;
    QString projectName;
};

class SessionSecretStore : public QObject {
public:
    enum class Error { None, NotFound, Unavailable, Denied, Failed };
    using Completion = std::function<void(Error)>;
    using Loaded = std::function<void(QByteArray, Error)>;

    explicit SessionSecretStore(QObject *parent = nullptr) : QObject(parent) {}
    virtual void write(const QString &key, const QByteArray &secret, Completion done) = 0;
    virtual void read(const QString &key, Loaded done) = 0;
    virtual void remove(const QString &key, Completion done) = 0;
};

class KeychainSessionSecretStore final : public SessionSecretStore {
public:
    using JobStarter = std::function<void(QKeychain::Job *)>;
    explicit KeychainSessionSecretStore(QObject *parent = nullptr, JobStarter starter = {});
    void write(const QString &key, const QByteArray &secret, Completion done) override;
    void read(const QString &key, Loaded done) override;
    void remove(const QString &key, Completion done) override;

private:
    JobStarter m_start;
};

class SessionVault final : public QObject {
    Q_OBJECT
public:
    using Completion = std::function<void(QString)>;
    using Loaded = std::function<void(SavedAccount, QByteArray, QString)>;

    explicit SessionVault(QString metadataFile = {}, SessionSecretStore *backend = nullptr,
                          QObject *parent = nullptr);
    static QString accountId(const QUrl &server, const QString &project, const QString &user);
    QVector<SavedAccount> accounts() const;
    void save(SavedAccount account, QByteArray refreshToken, Completion done = {});
    void load(const QString &id, Loaded done);
    void forget(const QString &id, Completion done = {});

signals:
    void accountsChanged();

private:
    QString writeMetadata(const QMap<QString, SavedAccount> &accounts);
    void enqueue(std::function<void()> operation);
    void startNext();
    void complete(Completion done, const QString &error);

    QString m_metadataFile;
    QString m_storageError;
    QPointer<SessionSecretStore> m_backend;
    QMap<QString, SavedAccount> m_accounts;
    QMap<QString, quint64> m_revisions;
    QSet<QString> m_forgetting;
    std::deque<std::function<void()>> m_queue;
    bool m_running = false;
};

} // namespace workspace
