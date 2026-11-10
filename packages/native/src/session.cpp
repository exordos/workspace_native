#include "session.h"

#include <QJsonArray>
#include <QSet>
#include <algorithm>

namespace workspace {

static const QString tokenPath = "/api/core/v1/iam/clients/default/actions/get_token/invoke";

static QVariantMap publicIdentityProfile(const QJsonObject &userInfo, const QString &uuid,
                                         const QString &login)
{
    // Copy only Core's public user fields, never the complete introspection or
    // credential envelope. Keep Unicode names intact; the view elides them.
    const auto text = [&userInfo](const QString &key) {
        return userInfo.value(key).toString().trimmed();
    };
    const auto first = text("first_name");
    const auto last = text("last_name");
    const auto email = text("email");
    auto username = text("username");
    if (username.isEmpty()) username = text("name"); // Deprecated Core alias.
    if (username.isEmpty()) username = login.trimmed();
    auto displayName = (first + ' ' + last).trimmed();
    if (displayName.isEmpty()) displayName = username;
    if (displayName.isEmpty()) displayName = email;
    if (displayName.isEmpty()) displayName = uuid;
    return {{"uuid", uuid}, {"displayName", displayName}, {"username", username},
            {"firstName", first}, {"lastName", last}, {"email", email}};
}

Session::Session(ApiClient *api, QObject *parent, RequestExecutor executor)
    : QObject(parent), m_api(api), m_request(std::move(executor))
{
    if (!m_request) {
        m_request = [api](const QByteArray &method, const QString &path, const QJsonValue &body,
                          const QUrlQuery &query, ApiClient::Callback callback, bool isPublic,
                          const QMap<QByteArray, QByteArray> &headers) {
            api->request(method, path, body, query, std::move(callback), isPublic, headers);
        };
    }
    m_clock.start();
    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setTimerType(Qt::PreciseTimer);
    m_expiryTimer.setSingleShot(true);
    m_expiryTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_refreshTimer, &QTimer::timeout, this, &Session::refreshProject);
    connect(&m_expiryTimer, &QTimer::timeout, this, [this] {
        if (m_accessExpiresAt > 0 && m_clock.elapsed() >= m_accessExpiresAt)
            fail(tr("Your session has expired. Sign in again."));
    });
}

void Session::setVault(SessionVault *vault)
{
    if (m_vault == vault) return;
    disconnect(m_vaultConnection);
    m_vault = vault;
    m_savedAccountId.clear();
    if (vault)
        m_vaultConnection = connect(vault, &SessionVault::accountsChanged,
                                    this, &Session::savedAccountsChanged);
    emit savedAccountsChanged();
}

QVector<SavedAccount> Session::savedAccounts() const
{
    return m_vault ? m_vault->accounts() : QVector<SavedAccount>{};
}

void Session::resumeCurrentAccount()
{
    // Startup is the only automatic choice. A manual login, account selection
    // or sign-out has already advanced the attempt and must win the race.
    if (!m_vault || m_attempt != 0) return;
    const auto id = m_vault->currentAccountId();
    if (!id.isEmpty()) resumeSavedAccount(id);
}

void Session::resumeSavedAccount(const QString &id)
{
    clearMemory();
    if (!m_vault) {
        emit error(tr("Saved sessions are unavailable."));
        return;
    }
    // Bind sign-out to this identity even while its secure-store read is pending.
    m_savedAccountId = id;
    emit busyChanged(true);
    const auto attempt = m_attempt;
    const QPointer<Session> guard(this);
    m_vault->load(id, [guard, attempt, id](SavedAccount account, QByteArray token, QString error) {
        if (!guard || attempt != guard->m_attempt) return;
        if (!error.isEmpty()) {
            guard->fail(error);
            return;
        }
        QString serverError;
        if (account.id != id || id != SessionVault::accountId(account.server, account.project, account.user)
            || !guard->m_api->setServer(account.server, &serverError)) {
            guard->fail(tr("The saved account identity is invalid."));
            return;
        }
        guard->m_project = account.project;
        guard->m_user = account.user;
        guard->m_login = account.login;
        guard->m_projects = {{account.project, account.projectName}};
        guard->m_refreshToken = token;
        guard->refreshProject();
    });
}

void Session::forgetSavedAccount(const QString &id)
{
    if (!m_vault) {
        emit error(tr("Saved sessions are unavailable."));
        return;
    }
    if (id == m_savedAccountId) {
        logout();
        return;
    }
    const QPointer<Session> guard(this);
    m_vault->forget(id, [guard](QString error) {
        if (guard && !error.isEmpty())
            emit guard->error(tr("The saved credential could not be removed. %1").arg(error));
    });
}

void Session::request(const QByteArray &method, const QString &path, const QJsonValue &body,
                      const QUrlQuery &query, ApiClient::Callback callback, bool isPublic,
                      const QMap<QByteArray, QByteArray> &headers)
{
    const QPointer<Session> guard(this);
    m_request(method, path, body, query, [guard, callback = std::move(callback)](const ApiResult &result) {
        if (guard) callback(result);
    }, isPublic, headers);
}

void Session::clearMemory()
{
    ++m_attempt;
    m_refreshTimer.stop();
    m_expiryTimer.stop();
    m_accessExpiresAt = 0;
    m_refreshFailures = 0;
    m_refreshInFlight = false;
    m_ready = false;
    m_refreshToken.fill('\0');
    m_refreshToken.clear();
    m_savedAccountId.clear();
    m_login.clear();
    m_publicProfile.clear();
    m_project.clear();
    m_user.clear();
    m_projects.clear();
    m_api->clearSession();
    emit busyChanged(false);
    emit signedOut();
}

void Session::logout()
{
    const auto id = m_savedAccountId;
    const auto vault = m_vault;
    clearMemory();
    if (!vault || id.isEmpty()) return;
    const auto attempt = m_attempt;
    const QPointer<Session> guard(this);
    vault->forget(id, [guard, attempt](QString error) {
        if (guard && attempt == guard->m_attempt && !error.isEmpty())
            emit guard->error(tr("Signed out, but the saved credential could not be removed. %1").arg(error));
    });
}

void Session::fail(const QString &message)
{
    clearMemory();
    emit error(message);
}

void Session::rememberVerifiedSession()
{
    if (!m_vault || !m_ready || m_refreshToken.isEmpty()) return;
    QString projectName;
    for (const auto &project : m_projects)
        if (project.uuid == m_project) { projectName = project.name; break; }
    SavedAccount account{{}, m_api->server(), m_project, m_user, m_login, projectName};
    account.id = SessionVault::accountId(account.server, account.project, account.user);
    // HTTP is limited to the existing transport test seam and is never persisted.
    if (account.id.isEmpty()) return;
    m_savedAccountId = account.id;
    const auto attempt = m_attempt;
    const QPointer<Session> guard(this);
    m_vault->save(account, m_refreshToken, [guard, attempt, id = account.id](QString error) {
        if (!guard || attempt != guard->m_attempt || id != guard->m_savedAccountId) return;
        // A late save of another account may keep that account available, but
        // only this still-current, verified session selects the startup identity.
        if (error.isEmpty() && guard->m_vault)
            error = guard->m_vault->setCurrentAccountId(id);
        if (!error.isEmpty())
            emit guard->error(tr("Signed in, but this session could not be remembered. %1").arg(error));
    });
}

void Session::invalidateSavedAccount()
{
    if (m_vault && !m_savedAccountId.isEmpty()) m_vault->forget(m_savedAccountId);
    m_savedAccountId.clear();
}

void Session::login(const QUrl &server, const QString &login, const QString &password,
                    const QString &otp)
{
    clearMemory();
    QString serverError;
    if (!m_api->setServer(server, &serverError)) {
        emit error(serverError);
        return;
    }
    m_login = login.trimmed();
    if (login.trimmed().isEmpty() || password.isEmpty()
        || (!otp.isEmpty() && (otp.size() != 6 || !std::all_of(otp.begin(), otp.end(),
            [](QChar c) { return c >= u'0' && c <= u'9'; })))) {
        emit error(tr("Enter your username, password and, if required, a six-digit code."));
        return;
    }
    emit busyChanged(true);
    const auto attempt = m_attempt;
    const auto requestStarted = m_clock.elapsed();
    const QJsonObject body{{"grant_type", "login+password"}, {"login", login.trimmed()},
        {"password", password}, {"scope", "openid email profile"},
        {"ttl", 3600}, {"refresh_ttl", 172800}};
    QMap<QByteArray, QByteArray> headers;
    if (!otp.isEmpty())
        headers.insert("X-OTP", otp.toUtf8());
    request("POST", tokenPath, body, {}, [this, attempt, requestStarted](const ApiResult &result) {
        if (attempt != m_attempt)
            return;
        if (!result.ok()) {
            fail(result.status == 401
                ? tr("Sign-in was rejected. Check your credentials and one-time code.")
                : tr("Sign-in failed. %1").arg(result.error));
            return;
        }
        if (acceptToken(result, requestStarted))
            discoverProjects();
    }, true, headers);
}

void Session::fetchOrganizationSettings(const QUrl &origin,
    std::function<void(bool ok, QVariantMap settings, QString error)> done)
{
    QString serverError;
    if (!m_api->setServer(origin, &serverError)) {
        done(false, {}, serverError);
        return;
    }
    request("GET", QStringLiteral("/api/workspace/v1/messenger/server_settings"), {}, {},
        [done = std::move(done)](const ApiResult &result) {
        // The native login surface renders realm name, icon and registration
        // entry only, so a 2xx JSON object with any subset of these strings is
        // an acceptable discovery result (docs/NATIVE_DESKTOP_LOGIN.md).
        if (!result.ok() || !result.data.isObject()) {
            done(false, {}, tr("Could not load organization settings. Check the server address and try again."));
            return;
        }
        const auto json = result.data.toObject();
        QVariantMap settings;
        for (auto it = json.begin(); it != json.end(); ++it)
            if (it.key() == QLatin1String("realm_name") || it.key() == QLatin1String("realm_icon")
                || it.key() == QLatin1String("registration_url"))
                settings.insert(it.key(), it.value().isString() ? it.value().toString() : QString{});
        done(true, settings, QString{});
    }, true);
}

bool Session::acceptToken(const ApiResult &result, qint64 requestStarted)
{
    const auto json = result.data.toObject();
    const auto token = json.value("access_token").toString().toUtf8();
    const auto refresh = json.value("refresh_token").toString().toUtf8();
    const auto expires = json.value("expires_in");
    const int seconds = expires.isUndefined() ? 3600 : expires.toInt(0);
    // Count TTL from dispatch using a monotonic clock. Time spent waiting for
    // the token response must not extend the credential's actual lifetime.
    const auto expiresAt = requestStarted + qint64(std::min(seconds, 86400)) * 1000;
    const auto remaining = expiresAt - m_clock.elapsed();
    if (token.isEmpty() || (refresh.isEmpty() && m_refreshToken.isEmpty()) || seconds <= 0
        || (!expires.isUndefined() && (!expires.isDouble() || expires.toDouble() != seconds))
        || remaining <= 0) {
        fail(tr("The server returned an invalid token response."));
        return false;
    }
    if (!refresh.isEmpty())
        m_refreshToken = refresh;
    m_accessExpiresAt = expiresAt;
    m_refreshFailures = 0;
    m_api->setAccessToken(token);
    m_expiryTimer.start(int(remaining));
    m_refreshTimer.start(int(std::max(qint64(1), remaining - std::min(qint64(60000), remaining / 4))));
    return true;
}

void Session::discoverProjects()
{
    const auto attempt = m_attempt;
    request("GET", "/api/core/v1/iam/projects/", {}, {},
        [this, attempt](const ApiResult &result) {
        if (attempt != m_attempt)
            return;
        if (!result.ok()) {
            fail(tr("Could not load your projects. %1").arg(result.error));
            return;
        }
        auto collection = result.data;
        if (collection.isObject()) {
            for (const auto &key : {"items", "data", "results", "objects"}) {
                if (collection.toObject().value(key).isArray()) {
                    collection = collection.toObject().value(key);
                    break;
                }
            }
        }
        if (!collection.isArray()) {
            fail(tr("The server returned an invalid project list."));
            return;
        }
        QSet<QString> seen;
        for (const auto &value : collection.toArray()) {
            const auto json = value.toObject();
            const auto id = json.value("uuid").toString().toLower();
            const auto name = json.value("name").toString();
            if (isUuid(id) && !name.isEmpty() && !seen.contains(id)) {
                seen.insert(id);
                m_projects.append({id, name});
            }
        }
        if (m_projects.isEmpty()) {
            fail(tr("No accessible Workspace projects were returned."));
            return;
        }
        emit busyChanged(false);
        emit projectsAvailable();
    });
}

void Session::selectProject(const QString &uuid)
{
    if (m_refreshToken.isEmpty() || std::none_of(m_projects.begin(), m_projects.end(),
            [&uuid](const Project &project) { return project.uuid == uuid; })) {
        emit error(tr("Select an accessible project from the list."));
        return;
    }
    m_project = uuid;
    m_savedAccountId.clear();
    m_publicProfile.clear();
    m_ready = false;
    m_refreshInFlight = false;
    m_refreshFailures = 0;
    ++m_attempt;
    refreshProject();
}

void Session::refreshProject()
{
    if (m_project.isEmpty() || m_refreshToken.isEmpty() || m_refreshInFlight)
        return;
    if (m_ready && m_clock.elapsed() >= m_accessExpiresAt) {
        fail(tr("Your session has expired. Sign in again."));
        return;
    }
    m_refreshTimer.stop();
    m_refreshInFlight = true;
    emit busyChanged(true);
    const auto attempt = m_attempt;
    const auto requestStarted = m_clock.elapsed();
    const QJsonObject body{{"grant_type", "refresh_token"},
        {"refresh_token", QString::fromUtf8(m_refreshToken)},
        {"scope", "openid email profile project:" + m_project}};
    request("POST", tokenPath, body, {}, [this, attempt, requestStarted](const ApiResult &result) {
        if (attempt != m_attempt)
            return;
        if (!result.ok()) {
            m_refreshInFlight = false;
            const auto remaining = m_accessExpiresAt - m_clock.elapsed();
            const bool transient = result.status == 0 || result.status == 408
                || result.status == 429 || result.status >= 500;
            if (m_ready && transient && remaining > 0) {
                const auto delay = std::min(qint64(30000), qint64(1000) << std::min(m_refreshFailures, 5U));
                ++m_refreshFailures;
                m_refreshTimer.start(int(std::min(delay, remaining)));
                emit busyChanged(false);
                emit error(tr("The connection was interrupted while renewing your session. Retrying shortly."));
                return;
            }
            const auto error = result.data.toObject().value("error").toString();
            if (result.status == 401 || result.status == 403 || error == "invalid_grant")
                invalidateSavedAccount();
            fail(tr("Your session could not be renewed. Sign in again."));
            return;
        }
        if (!acceptToken(result, requestStarted))
            return;
        request("GET", "/api/core/v1/iam/clients/default/actions/introspect", {}, {},
            [this, attempt](const ApiResult &identity) {
            if (attempt != m_attempt)
                return;
            const auto json = identity.data.toObject();
            const auto user = json.value("user_info").toObject().value("uuid").toString().toLower();
            if (!identity.ok() || !isUuid(user)
                || json.value("project_id").toString().toLower() != m_project
                || (!m_user.isEmpty() && m_user != user)) {
                if (identity.ok() || identity.status == 401 || identity.status == 403)
                    invalidateSavedAccount();
                fail(tr("The server did not confirm the selected user and project."));
                return;
            }
            m_user = user;
            m_publicProfile = publicIdentityProfile(json.value("user_info").toObject(), user, m_login);
            m_ready = true;
            m_refreshInFlight = false;
            rememberVerifiedSession();
            emit busyChanged(false);
            emit authenticated();
        });
    }, true);
}

void Session::requestRefresh()
{
    refreshProject();
}

}
