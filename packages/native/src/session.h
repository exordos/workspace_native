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
    Q_PROPERTY(bool otpRequired READ otpRequired NOTIFY otpChallengeChanged)
    Q_PROPERTY(bool otpRejected READ otpRejected NOTIFY otpChallengeChanged)
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
    // Removing a saved identity is asynchronous. The optional completion is
    // invoked with the secure-store error (empty on success) once the vault
    // answers, independent of any newer session attempt, so callers can wait
    // for the removal result before they start the next session.
    void forgetSavedAccount(const QString &id, std::function<void(QString error)> done = {});
    void login(const QUrl &server, const QString &login, const QString &password,
               const QString &otp = {});
    bool otpRequired() const { return !m_otpBody.isEmpty(); }
    bool otpRejected() const { return m_otpRejected; }
    quint64 otpRevision() const { return m_attempt; }
    Q_INVOKABLE void submitOtp(const QString &otp);
    void cancelOtp();
    // Public organization discovery for the login wizard (no bearer auth). The
    // callback receives the parsed realm fields or a user-facing failure note.
    void fetchOrganizationSettings(const QUrl &origin,
        std::function<void(bool ok, QVariantMap settings, QString error)> done);
    void selectProject(const QString &uuid);
    void requestRefresh();
    // Signs out the live identity and removes its saved credential. The
    // optional completion receives the secure-store error (empty on success)
    // regardless of any session attempt advanced afterwards.
    void logout(std::function<void(QString error)> done = {});
    QString project() const { return m_project; }
    QString user() const { return m_user; }
    bool ready() const { return m_ready; }
    // Remaining access-credential lifetime in ms, -1 when nothing is verified
    // (issue #50 diagnostics session-remaining sub-line; same monotonic clock
    // as the refresh guard in session.cpp).
    qint64 accessRemainingMs() const;
    const QVector<Project> &projects() const { return m_projects; }
    bool canConfigureSummaryService() const { return m_ready && m_summaryServiceAllowed; }
    // Issue #635 split of the combined flag: the gates and endpoints tabs of the
    // topic-summary settings dialog are gated by two distinct capabilities
    // (workspace_ui entities/messenger/messenger-topic-summary-permissions.lib.ts).
    bool canManageSummaryGates() const { return m_ready && m_summaryGatesAllowed; }
    bool canManageSummaryEndpoints() const { return m_ready && m_summaryEndpointsAllowed; }
    QVariantMap publicProfile() const { return m_ready ? m_publicProfile : QVariantMap{}; }

signals:
    void otpChallengeChanged();
    void otpChallengeInvalidated();
    void projectsAvailable();
    void authenticated();
    // Issue #789: a saved account's identity (server/project/user) is fully
    // resolved from the vault without any network round; the cache may start
    // painting from it while the refresh chain is still in flight.
    void startupIdentityResolved();
    void signedOut();
    void error(const QString &message);
    void busyChanged(bool busy);
    void savedAccountsChanged();

private:
    void invalidateOtpContext();
    void requestLoginToken(const QJsonObject &body, const QString &otp);
    bool acceptToken(const ApiResult &result, qint64 requestStarted, bool sameOwnerRenewal = false);
    void discoverProjects();
    void failProjectDiscovery(const QString &message);
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
    bool m_summaryServiceAllowed = false;
    bool m_summaryGatesAllowed = false;
    bool m_summaryEndpointsAllowed = false;
    QElapsedTimer m_clock;
    QTimer m_refreshTimer;
    QTimer m_expiryTimer;
    QJsonObject m_otpBody; // transient, only while a login challenge is active
    QUrl m_otpServer;
    quint64 m_otpGeneration = 0;
    bool m_otpRejected = false;
    bool m_otpInFlight = false;
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
