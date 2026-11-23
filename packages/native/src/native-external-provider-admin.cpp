#include "native-external-provider-admin.h"
#include "domain.h"
#include "session.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

namespace workspace
{
namespace
{
    // Reference caps (manage-external-provider-form.ui.tsx).
    constexpr qint64 maxAccounts = 100000;
    constexpr qint64 maxChatsPerAccount = 1000000;
    constexpr qint64 maxFileBytes = 5120LL * 1048576LL;

    QString policyUrl()
    {
        return QStringLiteral("/api/workspace/v1/messenger/external_provider_policies/zulip");
    }
    QString healthUrl()
    {
        return QStringLiteral("/api/workspace/v1/messenger/external_provider_health/zulip");
    }

    qint64 limitValue(const QJsonObject& limits, const QString& key)
    {
        const auto value = limits.value(key);
        return value.isDouble() ? static_cast<qint64>(value.toDouble()) : -1;
    }

    bool validLimit(const QString& key, qint64 value)
    {
        if (key == "max_accounts")
            return value >= 0 && value <= maxAccounts;
        if (key == "max_selected_chats_per_account")
            return value >= 0 && value <= maxChatsPerAccount;
        if (key == "max_file_bytes")
            return value >= 0 && value <= maxFileBytes;
        return false;
    }

    QString errorType(const ApiResult& result)
    {
        const auto object = result.data.toObject();
        return object.value("type").toString(object.value("error_type").toString());
    }

    QVariantMap caProjection(const QJsonValue& bundle)
    {
        if (bundle.isNull() || !bundle.isObject())
            return { { "present", false } };
        const auto object = bundle.toObject();
        const auto sha = object.value("sha256").toString();
        const auto count = object.value("certificate_count").toInt(-1);
        const auto generation = object.value("generation").toInt(-1);
        static const QRegularExpression shaPattern(QStringLiteral("^[0-9a-fA-F]{64}$"));
        if (!isUuid(object.value("uuid").toString()) || sha.isNull()
            || !shaPattern.match(sha).hasMatch() || count < 1 || count > 32 || generation < 1)
            return { { "present", false } };
        return { { "present", true }, { "uuid", object.value("uuid").toString() },
            { "generation", generation }, { "sha256", sha.toLower() }, { "certificateCount", count } };
    }
}

NativeExternalProviderAdmin::NativeExternalProviderAdmin(ApiClient* api, Session* session, QObject* parent)
    : QObject(parent)
    , m_api(api)
    , m_session(session)
{
    const auto invalidate = [this] {
        if (!m_owner.isEmpty() || m_dialogOpen || m_accessStatus != QStringLiteral("unknown"))
            reset();
    };
    connect(api, &ApiClient::sessionChanged, this, invalidate);
    connect(session, &Session::signedOut, this, invalidate);
}

NativeExternalProviderAdmin::~NativeExternalProviderAdmin() { cancelAll(); }

QString NativeExternalProviderAdmin::owner() const
{
    if (!m_session->ready() || !isUuid(m_session->project()) || !isUuid(m_session->user()))
        return { };
    return m_api->server().toString() + '/' + m_session->project() + '/' + m_session->user();
}

bool NativeExternalProviderAdmin::current(quint64 ticket) const
{
    return ticket == m_ticket && !m_owner.isEmpty() && m_owner == owner()
        && m_generation == m_api->generation();
}

void NativeExternalProviderAdmin::cancelAll()
{
    ++m_ticket;
    for (auto* slot : { &m_policyRequest, &m_healthRequest, &m_saveRequest, &m_actionRequest }) {
        const auto request = std::exchange(*slot, { });
        if (request.isValid())
            m_api->cancelRequest(request);
    }
}

void NativeExternalProviderAdmin::reset()
{
    cancelAll();
    m_owner.clear();
    m_dialogOpen = false;
    m_accessStatus = QStringLiteral("unknown");
    m_policyStatus = QStringLiteral("idle");
    m_healthStatus = QStringLiteral("idle");
    m_actionStatus = QStringLiteral("none");
    m_policy = { };
    m_draft = { };
    m_health = { };
    m_etag.clear();
    m_conflict = m_saveBusy = m_saveFailed = m_actionFailed = m_mutationBusy = false;
    m_refreshDeferred = false;
    emit changed();
}

void NativeExternalProviderAdmin::forbidAccess()
{
    m_accessStatus = QStringLiteral("denied");
    m_dialogOpen = false;
    m_policyStatus = QStringLiteral("idle");
    m_refreshDeferred = false; // A denied session must not queue refreshes.
    emit changed();
}

void NativeExternalProviderAdmin::adoptDeferredRefresh()
{
    if (!m_refreshDeferred)
        return;
    m_refreshDeferred = false;
    refreshPolicy();
}

bool NativeExternalProviderAdmin::parsePolicy(const QJsonObject& dto, const QByteArray& etag)
{
    if (dto.value("provider").toString() != "zulip" || !isUuid(dto.value("uuid").toString()))
        return false;
    const auto revision = dto.value("revision").toInt(0);
    if (revision < 1)
        return false;
    const auto limits = dto.value("limits").toObject();
    const qint64 accounts = limitValue(limits, "max_accounts");
    const qint64 chats = limitValue(limits, "max_selected_chats_per_account");
    const qint64 bytes = limitValue(limits, "max_file_bytes");
    if (!validLimit("max_accounts", accounts) || !validLimit("max_selected_chats_per_account", chats)
        || !validLimit("max_file_bytes", bytes))
        return false;
    QVariantMap limitsMap { { "max_accounts", accounts }, { "max_selected_chats_per_account", chats },
        { "max_file_bytes", bytes } };
    QVariantMap parsed { { "uuid", dto.value("uuid").toString() },
        { "updatedAt", dto.value("updated_at").toString() }, { "revision", revision },
        { "enabled", dto.value("enabled").toBool() },
        { "emergencySuspended", dto.value("emergency_suspended").toBool() },
        { "limits", limitsMap }, { "ca", caProjection(dto.value("custom_ca_bundle")) } };
    m_policy = parsed;
    m_etag = etag;
    m_policyStatus = QStringLiteral("ready");
    return true;
}

QVariantMap NativeExternalProviderAdmin::snapshot(const QVariantMap& policy) const
{
    const auto limits = policy.value("limits").toMap();
    return { { "enabled", policy.value("enabled") },
        { "max_accounts", limits.value("max_accounts") },
        { "max_selected_chats_per_account", limits.value("max_selected_chats_per_account") },
        { "max_file_bytes", limits.value("max_file_bytes") } };
}

void NativeExternalProviderAdmin::rebaseDraft(const QVariantMap& previousBase, const QVariantMap& incomingBase)
{
    // Three-way rebase (lib.ts rebaseExternalProviderPolicyDraft): a field
    // stays local iff it differs from the base it was edited against.
    QVariantMap rebased;
    for (auto it = incomingBase.cbegin(); it != incomingBase.cend(); ++it) {
        const auto local = m_draft.value(it.key());
        rebased.insert(it.key(), local.isValid() && local != previousBase.value(it.key()) ? local : it.value());
    }
    m_draft = rebased;
}

bool NativeExternalProviderAdmin::dirty() const
{
    return m_policyStatus == QStringLiteral("ready") && m_draft != snapshot(m_policy);
}

bool NativeExternalProviderAdmin::fieldsValid() const
{
    for (const auto key : { "max_accounts", "max_selected_chats_per_account", "max_file_bytes" }) {
        const auto value = m_draft.value(QLatin1String(key));
        if (!value.canConvert<double>())
            return false;
        if (!validLimit(QLatin1String(key), static_cast<qint64>(value.toDouble())))
            return false;
    }
    return true;
}

bool NativeExternalProviderAdmin::caBlocked() const
{
    return m_policy.value("ca").toMap().value("present").toBool();
}

bool NativeExternalProviderAdmin::canSave() const
{
    return m_policyStatus == QStringLiteral("ready") && !m_mutationBusy && !m_saveBusy
        && m_actionStatus == QStringLiteral("none") && dirty() && fieldsValid() && !caBlocked();
}

void NativeExternalProviderAdmin::probe()
{
    const auto nextOwner = owner();
    if (nextOwner.isEmpty())
        return;
    if (m_owner != nextOwner) {
        cancelAll();
        m_owner = nextOwner;
        m_generation = m_api->generation();
        m_policyStatus = QStringLiteral("idle");
        m_policy = { };
        m_draft = { };
        m_etag.clear();
        m_conflict = false;
    }
    if (m_policyStatus == QStringLiteral("loading") || m_policyStatus == QStringLiteral("ready"))
        return;
    const auto ticket = m_ticket;
    m_policyStatus = QStringLiteral("loading");
    emit changed();
    const auto slot = ++m_policySerial;
    m_policyRequest = m_api->request("GET", policyUrl(), { }, { },
        [this, ticket, slot](const ApiResult& result) {
            if (!current(ticket) || slot != m_policySerial)
                return;
            m_policyRequest = { };
            if (result.ok()) {
                m_accessStatus = QStringLiteral("granted");
                if (!parsePolicy(result.data.toObject(), result.etag)) {
                    m_policyStatus = QStringLiteral("error");
                    emit changed();
                    return;
                }
                m_draft = snapshot(m_policy);
                emit changed();
                return;
            }
            const auto forbidden = result.status == 403 && errorType(result) == "ExternalResourceForbiddenError";
            if (result.status == 401 || forbidden) {
                forbidAccess();
                return;
            }
            m_policyStatus = QStringLiteral("error");
            emit changed();
        });
}

void NativeExternalProviderAdmin::close() { reset(); }

void NativeExternalProviderAdmin::openDialog()
{
    if (m_accessStatus != QStringLiteral("granted"))
        return;
    m_dialogOpen = true;
    emit changed();
    if (m_healthStatus == QStringLiteral("idle"))
        loadHealth(true);
}

void NativeExternalProviderAdmin::closeDialog()
{
    if (!m_dialogOpen)
        return;
    m_dialogOpen = false;
    emit changed();
}

void NativeExternalProviderAdmin::refreshPolicy()
{
    if (m_owner.isEmpty() || m_mutationBusy)
        return;
    const auto ticket = m_ticket;
    // The last parsed policy stays the rebase base even when policyStatus has
    // moved to error (a malformed 412 marks the error without adopting a
    // snapshot, then reissues the dropped refresh from that path): the retry
    // must rebase the user's draft, not replace it wholesale
    // (#735 finding 4670483902). Presence of a parsed policy, not its status,
    // decides whether a draft exists to protect.
    const auto previous = m_policy.isEmpty() ? QVariantMap { } : snapshot(m_policy);
    const auto slot = ++m_policySerial;
    const auto generation = m_policyGeneration;
    m_policyRequest = m_api->request("GET", policyUrl(), { }, { },
        [this, ticket, slot, previous, generation](const ApiResult& result) {
            if (!current(ticket) || slot != m_policySerial)
                return;
            m_policyRequest = { };
            if (!result.ok()) {
                const auto forbidden = result.status == 401
                    || (result.status == 403 && errorType(result) == "ExternalResourceForbiddenError");
                // A revocation reply is never stale: it must land even while a
                // mutation is in flight, otherwise a failing mutation leaves
                // the dialog open with access already revoked server-side
                // (#735 review finding 4670132627).
                if (forbidden) {
                    forbidAccess();
                    return;
                }
            }
            if (m_mutationBusy) {
                // A save/action was dispatched while this refresh was in
                // flight. Adopting the late reply now would move the rebase
                // base out from under the pending mutation: a local edit that
                // matches the refreshed value would then read as "untouched"
                // at the 412 and be discarded. Drop the reply; the mutation's
                // own reply (200 echo or 412 snapshot) is authoritative. If
                // that mutation later fails without adopting a policy, the
                // requested refresh is reissued then (#735 finding 4670132621).
                m_refreshDeferred = true;
                return;
            }
            if (generation != m_policyGeneration)
                return; // #740: a confirmed mutation landed after this GET was
                        // dispatched - its reply, whether a snapshot or a
                        // failure, predates our write; discard it whole
                        // (finding 4672617551: a late FAILED reply must not
                        // mark the confirmed policy back into the error state).
            if (!result.ok()) {
                m_policyStatus = QStringLiteral("error");
                emit changed();
                return;
            }
            m_accessStatus = QStringLiteral("granted");
            if (!parsePolicy(result.data.toObject(), result.etag)) {
                m_policyStatus = QStringLiteral("error");
                emit changed();
                return;
            }
            if (previous.isEmpty())
                m_draft = snapshot(m_policy);
            else
                rebaseDraft(previous, snapshot(m_policy));
            m_conflict = false;
            emit changed();
        });
}

void NativeExternalProviderAdmin::loadHealth(bool initial)
{
    if (m_owner.isEmpty())
        return;
    const auto ticket = m_ticket;
    m_healthStatus = initial ? QStringLiteral("loading") : QStringLiteral("refreshing");
    emit changed();
    const auto slot = ++m_healthSerial;
    m_healthRequest = m_api->request("GET", healthUrl(), { }, { },
        [this, ticket, slot, initial](const ApiResult& result) {
            if (!current(ticket) || slot != m_healthSerial)
                return;
            m_healthRequest = { };
            if (result.ok() && result.data.isObject()) {
                const auto dto = result.data.toObject();
                if (dto.value("provider").toString() == "zulip") {
                    m_health = { { "provider", "zulip" },
                        { "status", dto.value("status").toString() },
                        { "accountCounts", dto.value("account_counts").toObject().toVariantMap() },
                        { "chatCounts", dto.value("chat_counts").toObject().toVariantMap() },
                        { "bridgeCounts", dto.value("bridge_counts").toObject().toVariantMap() },
                        { "operationCounts", dto.value("operation_counts").toObject().toVariantMap() },
                        { "metrics", dto.value("metrics").toObject().toVariantMap() },
                        { "updatedAt", dto.value("updated_at").toString() } };
                    m_healthStatus = QStringLiteral("ready");
                    emit changed();
                    return;
                }
            }
            m_healthStatus = initial ? QStringLiteral("loadError") : QStringLiteral("refreshError");
            emit changed();
        });
}

void NativeExternalProviderAdmin::refreshHealth()
{
    if (m_healthStatus != QStringLiteral("refreshing") && m_healthStatus != QStringLiteral("loading"))
        loadHealth(m_healthStatus == QStringLiteral("idle") || m_healthStatus == QStringLiteral("loadError"));
}

void NativeExternalProviderAdmin::setEnabled(bool enabled)
{
    if (m_policyStatus != QStringLiteral("ready") || m_saveBusy)
        return;
    m_draft["enabled"] = enabled;
    emit changed();
}

void NativeExternalProviderAdmin::setLimit(const QString& key, double value)
{
    if (m_policyStatus != QStringLiteral("ready") || m_saveBusy
        || !m_draft.contains(key))
        return;
    m_draft[key] = static_cast<qint64>(value);
    emit changed();
}

void NativeExternalProviderAdmin::resetDraft()
{
    if (m_policyStatus != QStringLiteral("ready"))
        return;
    m_conflict = false;
    m_saveFailed = false;
    m_draft = snapshot(m_policy);
    emit changed();
}

void NativeExternalProviderAdmin::keepDraft()
{
    if (!m_conflict)
        return;
    // The draft was preserved through the notice; the etag is already the
    // refreshed one from the conflict reply, so the next Save retries cleanly.
    m_conflict = false;
    emit changed();
}

void NativeExternalProviderAdmin::save()
{
    if (!canSave())
        return;
    QJsonObject limits { { "max_accounts", static_cast<double>(m_draft.value("max_accounts").toLongLong()) },
        { "max_selected_chats_per_account",
            static_cast<double>(m_draft.value("max_selected_chats_per_account").toLongLong()) },
        { "max_file_bytes", static_cast<double>(m_draft.value("max_file_bytes").toLongLong()) } };
    QJsonObject settings { { "kind", "zulip" }, { "enabled", m_draft.value("enabled").toBool() },
        { "limits", limits }, { "custom_ca_bundle", QJsonValue::Null } };
    Headers headers;
    if (!m_etag.isEmpty())
        headers.insert("If-Match", m_etag);
    const auto ticket = m_ticket;
    m_mutationBusy = true;
    m_saveBusy = true;
    m_saveFailed = false;
    emit changed();
    const auto slot = ++m_saveSerial;
    QJsonObject saveBody { { "settings", settings } };
    m_saveRequest = m_api->request("PUT", policyUrl(), saveBody, { },
        [this, ticket, slot](const ApiResult& result) {
            if (!current(ticket) || slot != m_saveSerial)
                return;
            m_saveRequest = { };
            m_mutationBusy = m_saveBusy = false;
            if (result.ok() && parsePolicy(result.data.toObject(), result.etag)) {
                m_refreshDeferred = false; // The echo is the newer snapshot.
                ++m_policyGeneration; // #740: confirmed mutation - late refresh replies die.
                m_draft = snapshot(m_policy);
                m_conflict = false;
                m_saveFailed = false;
                emit changed();
                return;
            }
            if (result.status == 412 && errorType(result) == "ExternalPreconditionFailedError") {
                m_conflict = true;
                // Keep the fresh server snapshot for the conflict notice path.
                const auto fresh = result.data.toObject().value("policy").toObject().isEmpty()
                    ? result.data.toObject()
                    : result.data.toObject().value("policy").toObject();
                const auto had = m_policyStatus == QStringLiteral("ready");
                const auto previous = had ? snapshot(m_policy) : QVariantMap { };
                if (!fresh.isEmpty() && parsePolicy(fresh, result.etag)) {
                    m_refreshDeferred = false; // The 412 snapshot is the newer base.
                    ++m_policyGeneration; // #740: adopted server state is a new baseline.
                    if (!had || previous.isEmpty())
                        m_draft = snapshot(m_policy);
                    else
                        // Three-way rebase (design: conflict keeps per-field
                        // local edits, untouched fields adopt the fresh policy).
                        rebaseDraft(previous, snapshot(m_policy));
                } else {
                    m_policyStatus = QStringLiteral("error");
                    // Nothing newer was adopted: honor the dropped refresh.
                    adoptDeferredRefresh();
                }
                emit changed();
                return;
            }
            m_saveFailed = true;
            adoptDeferredRefresh(); // The failed save did not adopt a policy.
            emit changed();
        },
        false, headers);
}

void NativeExternalProviderAdmin::suspend()
{
    if (m_policyStatus != QStringLiteral("ready") || m_mutationBusy || m_saveBusy
        || m_actionStatus != QStringLiteral("none")
        || m_policy.value("emergencySuspended").toBool())
        return;
    const auto ticket = m_ticket;
    const auto previous = snapshot(m_policy);
    const auto pristine = m_draft == previous;
    m_mutationBusy = true;
    m_actionStatus = QStringLiteral("suspending");
    m_actionFailed = false;
    emit changed();
    const auto slot = ++m_actionSerial;
    m_actionRequest = m_api->request("POST", policyUrl() + QStringLiteral("/actions/suspend/invoke"), { }, { },
        [this, ticket, slot, previous, pristine](const ApiResult& result) {
            if (!current(ticket) || slot != m_actionSerial)
                return;
            m_actionRequest = { };
            m_mutationBusy = false;
            m_actionStatus = QStringLiteral("none");
            if (result.ok() && parsePolicy(result.data.toObject(), result.etag)) {
                ++m_policyGeneration; // #740: confirmed mutation - late refresh replies die.
                m_actionFailed = false;
                m_refreshDeferred = false; // The action reply carries the newer policy.
                if (pristine)
                    m_draft = snapshot(m_policy);
                else
                    rebaseDraft(previous, snapshot(m_policy));
                emit changed();
                return;
            }
            m_actionFailed = true;
            adoptDeferredRefresh(); // The failed action did not adopt a policy.
            emit changed();
        });
}

void NativeExternalProviderAdmin::resume()
{
    if (m_policyStatus != QStringLiteral("ready") || m_mutationBusy || m_saveBusy
        || m_actionStatus != QStringLiteral("none")
        || !m_policy.value("emergencySuspended").toBool())
        return;
    const auto ticket = m_ticket;
    const auto previous = snapshot(m_policy);
    const auto pristine = m_draft == previous;
    m_mutationBusy = true;
    m_actionStatus = QStringLiteral("resuming");
    m_actionFailed = false;
    emit changed();
    const auto slot = ++m_actionSerial;
    m_actionRequest = m_api->request("POST", policyUrl() + QStringLiteral("/actions/resume/invoke"), { }, { },
        [this, ticket, slot, previous, pristine](const ApiResult& result) {
            if (!current(ticket) || slot != m_actionSerial)
                return;
            m_actionRequest = { };
            m_mutationBusy = false;
            m_actionStatus = QStringLiteral("none");
            if (result.ok() && parsePolicy(result.data.toObject(), result.etag)) {
                ++m_policyGeneration; // #740: confirmed mutation - late refresh replies die.
                m_actionFailed = false;
                m_refreshDeferred = false; // The action reply carries the newer policy.
                if (pristine)
                    m_draft = snapshot(m_policy);
                else
                    rebaseDraft(previous, snapshot(m_policy));
                emit changed();
                return;
            }
            m_actionFailed = true;
            adoptDeferredRefresh(); // The failed action did not adopt a policy.
            emit changed();
        });
}
} // namespace workspace
