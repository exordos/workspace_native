#pragma once

#include "api-client.h"
#include "domain.h"
#include "session-vault.h"

#include <QElapsedTimer>
#include <QTimer>
#include <QVariantMap>

namespace workspace {

class Session final : public QObject {
    Q_OBJECT
public:
    using RequestExecutor = std::function<void(const QByteArray &, const QString &,
        const QJsonValue &, const QUrlQuery &, ApiClient::Callback, bool,
        const QMap<QByteArray, QByteArray> &)>;
    explicit Session(ApiClient *api, QObject *parent = nullptr, RequestExecutor executor = {});
    void setVault(SessionVault *vault);
    QVector<SavedAccount> savedAccounts() const;
    // The saved identity backing the live session, empty while anonymous.
    QString currentSavedAccountId() const { return m_savedAccountId; }
    // The verified login of the live session, empty before sign-in.
    QString loginName() const { return m_login; }
    // Drops the live in-memory session like sign-out but keeps the saved
    // identity and its secure-store entry: the reference switcher retains
    // remembered sessions when the active one changes or the sign-in route
    // reopens for "Add server".
    void detach();
    void resumeSavedAccount(const QString &id);
    void resumeCurrentAccount();
    void forgetSavedAccount(const QString &id);
    void login(const QUrl &server, const QString &login, const QString &password,
               const QString &otp = {});
    // Public organization discovery for the login wizard (no bearer auth). The
    // callback receives the parsed realm fields or a user-facing failure note.
    void fetchOrganizationSettings(const QUrl &origin,
        std::function<void(bool ok, QVariantMap settings, QString error)> done);
    void selectProject(const QString &uuid);
    void requestRefresh();
    void logout();
    QString project() const { return m_project; }
    QString user() const { return m_user; }
    bool ready() const { return m_ready; }
    const QVector<Project> &projects() const { return m_projects; }
    QVariantMap publicProfile() const { return m_ready ? m_publicProfile : QVariantMap{}; }

signals:
    void projectsAvailable();
    void authenticated();
    void signedOut();
    void error(const QString &message);
    void busyChanged(bool busy);
    void savedAccountsChanged();

private:
    bool acceptToken(const ApiResult &result, qint64 requestStarted);
    void discoverProjects();
    void refreshProject();
    void fail(const QString &message);
    void clearMemory();
    void rememberVerifiedSession();
    void invalidateSavedAccount();
    void request(const QByteArray &method, const QString &path, const QJsonValue &body,
                 const QUrlQuery &query, ApiClient::Callback callback, bool isPublic = false,
                 const QMap<QByteArray, QByteArray> &headers = {});

    ApiClient *m_api;
    RequestExecutor m_request;
    QPointer<SessionVault> m_vault;
    QMetaObject::Connection m_vaultConnection;
    QString m_savedAccountId;
    QString m_login;
    QVariantMap m_publicProfile;
    QElapsedTimer m_clock;
    QTimer m_refreshTimer;
    QTimer m_expiryTimer;
    QByteArray m_refreshToken;
    QVector<Project> m_projects;
    QString m_project;
    QString m_user;
    quint64 m_attempt = 0;
    qint64 m_accessExpiresAt = 0;
    unsigned int m_refreshFailures = 0;
    bool m_refreshInFlight = false;
    bool m_ready = false;
};

}
