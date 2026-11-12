#include "native-controller.h"
#include "media-viewer.h"
#include "native-deeplink.h"
#include "workspace-reference.h"
#include "native-preferences.h"
#include "native-markdown.h"
#include "native-chat-key-vault.h"
#include "native-chat-crypto.h"
#include <openssl/crypto.h>
#include "realtime-client.h"
#include "file-client.h"

#include <QClipboard>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QGuiApplication>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <QFileInfo>
#include <QJsonArray>
#include <QPointer>
#include <QUuid>
#include <QLocale>
#include <algorithm>

namespace workspace {

static const QString messenger = "/api/workspace/v1/messenger";

namespace {
void wipeChatBytes(QByteArray &value)
{
    if (!value.isEmpty()) { value.detach(); OPENSSL_cleanse(value.data(), size_t(value.size())); }
    value.clear();
}
bool sameChatLease(const NativeChatKeyVault::Lease &a, const NativeChatKeyVault::Lease &b)
{
    return a.owner == b.owner && a.device == b.device && a.issuer == b.issuer && a.generation == b.generation;
}
bool sameChatCache(const NativeCache::Lease &a, const NativeCache::Lease &b)
{
    return a.owner == b.owner && a.generation == b.generation;
}
QByteArray strictChatBase64(const QString &value, qsizetype maximum)
{
    if (value.isEmpty() || value.size() > maximum || value.size() % 4) return {};
    const auto ascii = value.toLatin1();
    if (QString::fromLatin1(ascii) != value) return {};
    const auto decoded = QByteArray::fromBase64(ascii, QByteArray::AbortOnBase64DecodingErrors);
    return !decoded.isEmpty() && decoded.toBase64() == ascii ? decoded : QByteArray{};
}
QByteArray chatPayloadHash(const Message &message)
{
    if (!message.crypto) return {};
    return QCryptographicHash::hash(QJsonDocument(QJsonArray{message.uuid, message.stream, message.topic,
        message.crypto->keyUuid, message.crypto->encodedContent}).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
}
qsizetype chatControlUnits(const Message &message)
{
    if (!message.crypto) return 0;
    const auto &p = *message.crypto;
    return message.uuid.size() + message.stream.size() + message.topic.size() + message.project.size()
        + message.author.size() + message.kind.size() + p.keyUuid.size() + p.deviceId.size() + p.deviceName.size()
        + p.encodedPublicKey.size() + p.requestMessage.size() + p.senderDevice.size()
        + p.recipientDevice.size() + p.encodedContent.size();
}
QByteArray chatRequestResponseHash(const Message &message)
{
    if (!message.crypto) return {};
    const auto &p = *message.crypto;
    return QCryptographicHash::hash(QJsonDocument(QJsonArray{message.uuid, message.project, message.stream,
        message.topic, message.author, message.kind, p.keyUuid, p.requestMessage, p.senderDevice,
        p.recipientDevice, p.encodedContent}).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
}
QString chatStreamSecurity(const Stream *stream)
{
    if (!stream) return {};
    QJsonArray values{stream->uuid, stream->role, stream->encrypted};
    if (stream->security) {
        const auto &s = *stream->security;
        values.append(QJsonArray{s.owner, s.viewer, s.source, s.currentKey, s.defaultTopic, s.provider, s.providerIdentity});
    }
    return QString::fromUtf8(QJsonDocument(values).toJson(QJsonDocument::Compact));
}
}

void NativeController::resetChatRuntime(bool preserveInitialOrder)
{
    const QPointer<NativeController> lookupGuard(this);
    ++m_chatEpoch; ++m_chatOperation;
    resetChatControls(false, preserveInitialOrder ? ChatOutcomeRetention::OrderOnly : ChatOutcomeRetention::Clear);
    if (!lookupGuard) return;
    interruptChatKeyResponse();
    m_chatLocalRequest.clear();
    m_chatAuthority = {};
    m_chatBusy = false;
    m_chatPhase.clear(); m_chatError.clear(); m_chatRequestKey.clear();
    m_chatPrepared.clear(); m_chatPrepareRows.clear(); m_chatPreparedUnits = 0;
    m_chatCurrentKeyState = ChatKeyAvailability::Unknown;
    m_chatSendKey.clear(); m_chatSendDeletion = 0;
    ++m_chatPreparation; m_chatPreparingWindow.clear(); m_chatRejectedWindow.clear();
    m_chatResponses.clear();
    m_chatAuthorityLoading = false; m_chatAuthorityWaiters.clear();
}

bool NativeController::chatVaultMatches() const
{
    return authenticated() && m_chatKeys && m_chatKeys->available() && !ownerKey().isEmpty()
        && m_chatKeys->lease().owner == ownerKey();
}

bool NativeController::chatReadAllowed() const
{
    return chatVaultMatches() && (!m_chatAccessKnown || m_chatReadableStreams.contains(m_stream));
}

bool NativeController::revokeChatAccess(const QString &stream)
{
    if (ownerKey().isEmpty() || (stream != m_stream
        && std::none_of(m_streams.cbegin(), m_streams.cend(), [&](const Stream &row) { return row.uuid == stream; }))) return false;
    if (m_chatRevocationOwner != ownerKey()) {
        m_chatRevocationOwner = ownerKey(); m_chatReadableStreams.clear(); m_chatAccessKnown = false;
    }
    if (!m_chatAccessKnown) {
        // The set follows the existing bounded owned catalog, never arbitrary
        // event UUIDs. A too-large catalog is conservatively locked.
        m_chatAccessKnown = true;
        if (m_streams.size() <= 100000) for (const auto &row : m_streams) m_chatReadableStreams.insert(row.uuid);
    }
    m_chatReadableStreams.remove(stream); ++m_chatAccessRevision;
    if (stream == m_stream) {
        ++m_chatEpoch; ++m_chatOperation; ++m_chatPreparation;
        const QPointer<NativeController> lookupGuard(this);
        resetChatControls();
        if (!lookupGuard) return true;
        m_chatLocalRequest.clear(); m_chatAuthority = {}; m_chatAuthorityLoading = false;
        m_chatAuthorityWaiters.clear(); m_chatResponses.clear(); m_chatBusy = false;
        m_chatPrepared.clear(); m_chatPreparedUnits = 0; m_chatPrepareRows.clear();
        m_chatCurrentKeyState = ChatKeyAvailability::Unknown;
        m_chatSendKey.clear(); m_chatSendDeletion = 0;
        m_chatPreparingWindow.clear(); m_chatRejectedWindow.clear();
        m_chatPhase = "locked"; m_chatError = tr("Channel access has been revoked.");
        interruptChatKeyResponse();
    }
    const QPointer<NativeController> guard(this);
    invalidateCache(NativeCache::Kind::History, stream);
    if (!guard) return true;
    invalidateCache(NativeCache::Kind::Topics, stream);
    if (!guard) return true;
    invalidateCache(NativeCache::Kind::Streams);
    return true;
}

void NativeController::restoreChatAccess(const QJsonArray &rows, quint64 revision, const QString &owner,
    quint64 epoch, const ProjectionContext &context)
{
    if (owner.isEmpty() || owner != ownerKey() || owner != m_chatRevocationOwner || revision != m_chatAccessRevision
        || epoch != m_chatEpoch || context.generation != m_api->generation() || context.cache != m_cache
        || !sameChatCache(context.lease, m_cacheLease)
        || (context.cache && !sameChatCache(context.cache->lease(), context.lease))) return;
    if (rows.size() > 100000) { m_chatReadableStreams.clear(); m_chatAccessKnown = true; return; }
    QSet<QString> current, verified;
    bool completeOwnership = true;
    for (const auto &raw : rows) {
        const auto stream = parseStream(raw.toObject(), m_session->project());
        if (!stream) return; // Caller normally validates the complete catalog first.
        current.insert(stream->uuid);
        const bool owned = stream->security && stream->security->viewer == m_session->user()
            && QStringList{"guest", "member", "moderator", "administrator", "owner"}.contains(stream->role);
        completeOwnership = completeOwnership && owned;
        if (owned) verified.insert(stream->uuid);
    }
    const bool omitted = std::any_of(m_streams.cbegin(), m_streams.cend(),
        [&](const Stream &row) { return row.encrypted && !current.contains(row.uuid); });
    if (completeOwnership) {
        m_chatReadableStreams = std::move(verified); m_chatAccessKnown = true;
    } else if (m_chatAccessKnown || omitted) {
        if (!m_chatAccessKnown && m_streams.size() <= 100000)
            for (const auto &row : m_streams) m_chatReadableStreams.insert(row.uuid);
        m_chatReadableStreams.intersect(current); m_chatReadableStreams.unite(verified); m_chatAccessKnown = true;
    }
    // A fresh omission is itself access loss. It cannot be undone by a later
    // stale stream add; only a new complete owned result can extend the set.
    if (omitted) {
        ++m_chatAccessRevision;
        if (!m_stream.isEmpty() && !current.contains(m_stream)) revokeChatAccess(m_stream);
    }
}

void NativeController::checkChatLifetime()
{
    const QPointer<NativeController> lookupGuard(this);
    const auto owner = ownerKey();
    if (owner != m_chatRevocationOwner) {
        m_chatRevocationOwner = owner; m_chatReadableStreams.clear(); m_chatAccessKnown = false; ++m_chatAccessRevision;
    }
    const auto vault = m_chatKeys ? m_chatKeys->lease() : NativeChatKeyVault::Lease{};
    const auto security = chatStreamSecurity(currentStream());
    const bool trust = m_catalogVerified && !m_snapshotActive && !m_syncBlocked;
    const bool sameSendingLifetime = owner == m_chatOwner && m_stream == m_chatStream
        && sameChatLease(vault, m_chatVaultLease);
    const bool hard = owner != m_chatOwner || m_stream != m_chatStream || m_topic != m_chatTopic
        || !sameChatLease(vault, m_chatVaultLease) || security != m_chatSecurity;
    const bool lifetime = hard || m_api->generation() != m_chatApiGeneration || m_catalog != m_chatCatalog
        || m_cache != m_chatCache || !sameChatCache(m_cacheLease, m_chatCacheLease);
    const bool live = lifetime || trust != m_chatTrust;
    const bool firstOpen = m_chatInitialVault == ChatInitialVault::Waiting && m_chatVaultLease.owner.isEmpty()
        && vault.owner == owner && !owner.isEmpty() && owner == m_chatOwner && m_stream == m_chatStream
        && m_topic == m_chatTopic && security == m_chatSecurity && m_api->generation() == m_chatApiGeneration
        && m_catalog == m_chatCatalog && m_cache == m_chatCache && sameChatCache(m_cacheLease, m_chatCacheLease)
        && (!m_chatAccessKnown || m_chatReadableStreams.contains(m_stream));
    if (live) {
        resetChatControls(!lifetime, !lifetime ? ChatOutcomeRetention::PublicFacts
            : firstOpen ? ChatOutcomeRetention::OrderOnly : ChatOutcomeRetention::Clear);
        if (!lookupGuard) return;
        ++m_chatEpoch; ++m_chatOperation; m_chatLocalRequest.clear(); m_chatAuthority = {}; m_chatBusy = false; m_chatPhase.clear(); m_chatError.clear();
        m_chatResponses.clear();
        m_chatAuthorityLoading = false; m_chatAuthorityWaiters.clear();
        ++m_chatPreparation; m_chatPreparingWindow.clear();
    }
    if (hard) {
        m_chatPrepared.clear(); m_chatPrepareRows.clear(); m_chatPreparedUnits = 0;
        m_chatCurrentKeyState = ChatKeyAvailability::Unknown;
        if (!sameSendingLifetime) { m_chatSendKey.clear(); m_chatSendDeletion = 0; }
        ++m_chatPreparation; m_chatPreparingWindow.clear(); m_chatRejectedWindow.clear();
    }
    m_chatOwner = owner; m_chatStream = m_stream; m_chatTopic = m_topic; m_chatSecurity = security;
    m_chatVaultLease = vault; m_chatApiGeneration = m_api->generation(); m_chatCatalog = m_catalog;
    m_chatCache = m_cache; m_chatCacheLease = m_cacheLease; m_chatTrust = trust;
    if (!vault.owner.isEmpty()) m_chatInitialVault = ChatInitialVault::Complete;
    interruptEncryptedSends();
    interruptChatKeyResponse();
}

void NativeController::useChatKeys(NativeChatKeyVault *vault)
{
    const QPointer<NativeController> lookupGuard(this);
    const bool firstAttachment = vault && !m_chatKeys && m_chatInitialVault == ChatInitialVault::Unattached;
    const bool preserveOrder = firstAttachment && !ownerKey().isEmpty() && ownerKey() == m_chatOwner
        && m_stream == m_chatStream && m_topic == m_chatTopic && chatStreamSecurity(currentStream()) == m_chatSecurity
        && m_api->generation() == m_chatApiGeneration && m_catalog == m_chatCatalog && m_cache == m_chatCache
        && sameChatCache(m_cacheLease, m_chatCacheLease) && (!m_chatAccessKnown || m_chatReadableStreams.contains(m_stream));
    m_chatInitialVault = firstAttachment ? ChatInitialVault::Waiting : ChatInitialVault::Complete;
    if (m_chatKeys) disconnect(m_chatKeys, nullptr, this, nullptr);
    resetChatRuntime(preserveOrder);
    if (!lookupGuard) return;
    m_chatKeys = vault;
    if (vault) {
        connect(vault, &NativeChatKeyVault::keyMaterialChanged, this, [this](const QString &stream, const QString &key) {
            // Deletion ABA permanently invalidates an already frozen ciphertext.
            // A grant for another generation does not revoke its existing key.
            for (auto it = m_pendingSends.begin(); it != m_pendingSends.end(); ++it) {
                if (!it->encrypted || it->stream != stream || it->key != key || it->vault != m_chatKeys) continue;
                auto stored = m_chatKeys->read(it->vaultLease, {stream, key});
                it->materialLost = it->materialLost || stored.failure != NativeChatKeyVault::Failure::None
                    || stored.value.bytes.size() != 32 || stored.value.deletionRevision != it->deletion;
                wipeChatBytes(stored.value.bytes);
            }
            if (m_chatKeyResponse && m_chatKeyResponse->action == "grant" && m_chatKeyResponse->stream == stream
                && m_chatKeyResponse->key == key && m_chatKeyResponse->vault == m_chatKeys) {
                auto stored = m_chatKeys->read(m_chatKeyResponse->vaultLease, {stream, key});
                m_chatKeyResponse->materialLost = m_chatKeyResponse->materialLost
                    || stored.failure != NativeChatKeyVault::Failure::None || stored.value.bytes.size() != 32
                    || stored.value.deletionRevision != m_chatKeyResponse->deletion;
                wipeChatBytes(stored.value.bytes);
                interruptChatKeyResponse();
            }
            if (stream != m_stream) return;
            m_chatIssueKeys.remove(key); m_chatControlWindow.clear();
            m_chatPrepared.clear(); m_chatPreparedUnits = 0;
            m_chatCurrentKeyState = ChatKeyAvailability::Unknown;
            m_chatRejectedWindow.clear(); m_chatPreparingWindow.clear(); ++m_chatPreparation;
            if (key == m_chatRequestKey && chatVaultMatches()) {
                auto stored = m_chatKeys->read(m_chatKeys->lease(), {stream, key});
                if (stored.failure != NativeChatKeyVault::Failure::None || stored.value.bytes.isEmpty()) {
                    ++m_chatOperation; m_chatLocalRequest.clear(); m_chatPhase = "locked"; m_chatBusy = false;
                }
                wipeChatBytes(stored.value.bytes);
            }
            scheduleChatPreparation();
            const QPointer<NativeController> guard(this); emit stateChanged();
            if (guard) emit messagesChanged();
        });
        connect(vault, &NativeChatKeyVault::stateChanged, this, [this] {
            const QPointer<NativeController> lookupGuard(this);
            checkChatLifetime();
            if (!lookupGuard) return;
            scheduleChatPreparation();
            if (!lookupGuard) return;
            const QPointer<NativeController> guard(this); emit stateChanged();
            if (guard) emit messagesChanged();
        });
        connect(vault, &QObject::destroyed, this, [this] {
            const QPointer<NativeController> lookupGuard(this);
            resetChatRuntime();
            if (!lookupGuard) return;
            m_chatKeys = nullptr;
            const QPointer<NativeController> guard(this); emit stateChanged();
            if (guard) emit messagesChanged();
        });
    }
    checkChatLifetime();
    if (!lookupGuard) return;
    scheduleChatPreparation();
    if (!lookupGuard) return;
    const QPointer<NativeController> guard(this); emit stateChanged();
    if (guard) emit messagesChanged();
}

QVariantMap NativeController::encryption() const
{
    const auto *stream = currentStream();
    const bool ownedNative = stream && stream->encrypted && stream->security
        && stream->security->viewer == m_session->user() && stream->security->source == "native"
        && stream->security->provider.isEmpty() && stream->security->providerIdentity.isEmpty()
        && QStringList{"guest", "member", "moderator", "administrator", "owner"}.contains(stream->role);
    const bool verified = ownedNative && m_catalogVerified;
    const bool issuing = m_chatKeyResponse && m_chatKeyResponse->busy;
    const bool canRequest = verified && !m_chatBusy && !issuing && chatContext(m_chatEpoch, m_selection);
    return {{"available", chatVaultMatches()}, {"state", m_chatPhase.isEmpty() ? QStringLiteral("locked") : m_chatPhase},
        {"busy", m_chatBusy || issuing}, {"error", m_chatError}, {"keyUuid", m_chatRequestKey},
        {"keyRequests", chatKeyRequestsMetadata()}, {"keyResponse", chatKeyResponseMetadata()},
        {"storeOpening", m_chatKeyStoreOpening}, {"pendingSend", pendingSendMetadata()},
        {"currentKeyUuid", verified ? stream->security->currentKey : QString{}},
        {"sendingKeyUuid", verified && m_chatCurrentKeyState != ChatKeyAvailability::Unknown
            && chatContext(m_chatEpoch, m_selection) ? m_chatSendKey : QString{}},
        {"canRequest", canRequest},
        {"canRequestCurrentKey", canRequest && isUuid(stream->security->currentKey)
            && m_chatCurrentKeyState == ChatKeyAvailability::Missing}};
}

void NativeController::setChatKeyStoreOpening(bool opening)
{
    if (m_chatKeyStoreOpening == opening) return;
    m_chatKeyStoreOpening = opening;
    emit stateChanged();
}

void NativeController::retryChatKeyStore()
{
    if (!authenticated() || !m_chatKeys || chatVaultMatches() || m_chatKeyStoreOpening) return;
    emit chatKeyStoreOpenRequested();
}

bool NativeController::chatPublicContext(quint64 epoch, quint64 selection) const
{
    // Public owned body correlation does not depend on this device's Vault,
    // issuer rights, provider crypto support or possession of a generation key.
    return authenticated() && !ownerKey().isEmpty() && currentStream()
        && m_chatEpoch == epoch && m_selection == selection && m_chatOwner == ownerKey()
        && (!m_chatAccessKnown || m_chatReadableStreams.contains(m_stream))
        && m_chatApiGeneration == m_api->generation() && m_chatStream == m_stream && m_chatTopic == m_topic
        && m_chatCatalog == m_catalog && m_chatCache == m_cache && sameChatCache(m_chatCacheLease, m_cacheLease)
        && m_catalogVerified && !m_snapshotActive && !m_syncBlocked
        && m_chatSecurity == chatStreamSecurity(currentStream());
}

bool NativeController::chatContext(quint64 epoch, quint64 selection) const
{
    return chatPublicContext(epoch, selection) && chatReadAllowed()
        && sameChatLease(m_chatKeys->lease(), m_chatVaultLease);
}

bool NativeController::currentChatOperation(quint64 operation, const QString &key) const
{
    return operation == m_chatOperation && key == m_chatRequestKey;
}

void NativeController::finishChatOperation(quint64 operation, const QString &key,
    const QString &phase, const QString &error)
{
    if (!currentChatOperation(operation, key)) return;
    m_chatBusy = false; m_chatPhase = phase; m_chatError = error;
    const auto epoch = m_chatEpoch, selection = m_selection;
    const QPointer<NativeController> guard(this);
    emit stateChanged();
    if (!guard || !currentChatOperation(operation, key) || !chatContext(epoch, selection)) return;
    emit messagesChanged();
}

void NativeController::chatCollection(const QString &path, QUrlQuery query, quint64 epoch, quint64 selection,
    std::function<void(const QJsonArray &, const QString &)> done, QJsonArray rows,
    QSet<QByteArray> markers, int pages, qint64 bytes)
{
    if (!chatContext(epoch, selection)) return;
    if (pages >= 500) { done({}, tr("Encryption authority exceeds the request limit.")); return; }
    request("GET", path, {}, query, [this, path, query, epoch, selection, done, rows, markers, pages, bytes](const ApiResult &result) mutable {
        if (!chatContext(epoch, selection)) return;
        if (!result.ok() || !result.data.isArray()) { done({}, tr("Could not verify encryption authority.")); return; }
        const auto page = result.data.toArray();
        bytes += QJsonDocument(page).toJson(QJsonDocument::Compact).size();
        if (bytes > 32 * 1024 * 1024 || rows.size() + page.size() > 100000) {
            done({}, tr("Encryption authority exceeds the response limit.")); return;
        }
        for (const auto &row : page) rows.append(row);
        if (!result.nextMarker.isEmpty()) {
            if (markers.contains(result.nextMarker)) { done({}, tr("Encryption authority pagination repeated.")); return; }
            markers.insert(result.nextMarker); query.removeAllQueryItems("page_marker");
            query.addQueryItem("page_marker", QString::fromUtf8(result.nextMarker));
            chatCollection(path, query, epoch, selection, done, rows, markers, pages + 1, bytes);
        } else done(rows, {});
    });
}

void NativeController::finishChatAuthority(quint64 epoch, quint64 selection, const QString &error)
{
    if (!chatContext(epoch, selection)) return;
    m_chatAuthorityLoading = false; m_chatAuthority.ready = error.isEmpty();
    reduceChatKeyOutcomes();
    auto waiting = std::move(m_chatAuthorityWaiters); m_chatAuthorityWaiters.clear();
    const QPointer<NativeController> guard(this);
    for (const auto &callback : waiting) {
        if (!guard || !chatContext(epoch, selection)) return;
        callback(error.isEmpty(), error);
    }
    if (guard && error.isEmpty() && chatContext(epoch, selection)) scheduleChatControls();
}

void NativeController::ensureChatAuthority(ChatAuthorityCallback done)
{
    const QPointer<NativeController> lookupGuard(this);
    checkChatLifetime();
    if (!lookupGuard) return;
    const auto epoch = m_chatEpoch, selection = m_selection;
    if (!chatContext(epoch, selection)) { done(false, tr("A verified live channel is required to request a key.")); return; }
    if (m_chatAuthority.ready && m_chatAuthority.epoch == epoch) { done(true, {}); return; }
    if (m_chatAuthorityWaiters.size() >= 32) { done(false, tr("Encryption authority exceeds the request limit.")); return; }
    m_chatAuthorityWaiters.append(std::move(done));
    if (m_chatAuthorityLoading) return;
    m_chatAuthorityLoading = true;
    const auto stream = m_stream;
    request("GET", messenger + "/streams/" + stream, {}, {}, [this, stream, epoch, selection](const ApiResult &result) {
        if (!chatContext(epoch, selection)) return;
        if (result.status == 403 || result.status == 404) {
            const QPointer<NativeController> guard(this);
            revokeChatAccess(stream);
            if (!guard) return;
            emit messagesChanged();
            if (guard) emit stateChanged();
            return;
        }
        const auto parsed = parseStream(result.data.toObject(), m_session->project());
        if (!result.ok() || !parsed || parsed->uuid != stream || !parsed->security
            || !currentStream() || parsed->encrypted != currentStream()->encrypted
            || parsed->security->viewer != m_session->user() || parsed->security->source != "native"
            || !parsed->security->provider.isEmpty() || !parsed->security->providerIdentity.isEmpty()
            || (!parsed->security->defaultTopic.isEmpty() && !isUuid(parsed->security->defaultTopic))
            || !QStringList{"guest", "member", "moderator", "administrator", "owner"}.contains(parsed->role)) {
            finishChatAuthority(epoch, selection, tr("This channel's encryption authority is unavailable.")); return;
        }
        // currentKey is nullable. An explicit historical generation is independent
        // of a current generation; the live watermark still captures the null.
        m_chatAuthority = {ownerKey(), stream, parsed->security->defaultTopic, parsed->security->currentKey, parsed->role,
            parsed->security->owner, {}, epoch, false};
        QUrlQuery query; query.addQueryItem("stream_uuid", stream);
        chatCollection(messenger + "/stream_topics/", query, epoch, selection,
            [this, stream, epoch, selection, query](const QJsonArray &topics, const QString &error) {
            if (!chatContext(epoch, selection)) return;
            if (!error.isEmpty()) { finishChatAuthority(epoch, selection, error); return; }
            int defaults = 0; QSet<QString> ids;
            for (const auto &raw : topics) {
                const auto topic = parseTopic(raw.toObject(), m_session->project(), stream);
                if (!topic || ids.contains(topic->uuid) || !raw.toObject().value("is_default").isBool()) {
                    finishChatAuthority(epoch, selection, tr("The encryption request topic could not be verified.")); return;
                }
                ids.insert(topic->uuid);
                if (topic->isDefault) {
                    ++defaults;
                    if (!m_chatAuthority.defaultTopic.isEmpty() && topic->uuid != m_chatAuthority.defaultTopic) {
                        finishChatAuthority(epoch, selection, tr("The default topic changed.")); return;
                    }
                    m_chatAuthority.defaultTopic = topic->uuid;
                }
            }
            m_chatAuthority.topics = ids;
            if (defaults != 1) { finishChatAuthority(epoch, selection, tr("The encryption request topic could not be verified.")); return; }
            chatCollection(messenger + "/stream_bindings/", query, epoch, selection,
                [this, stream, epoch, selection](const QJsonArray &bindings, const QString &error) {
                if (!chatContext(epoch, selection)) return;
                if (!error.isEmpty()) { finishChatAuthority(epoch, selection, error); return; }
                QSet<QString> ids, users;
                for (const auto &value : bindings) {
                    const auto row = value.toObject(); const auto id = row.value("uuid").toString(); const auto user = row.value("user_uuid").toString();
                    const auto role = row.value("role").toString();
                    if (!value.isObject() || !isUuid(id) || ids.contains(id) || !isUuid(user) || users.contains(user)
                        || row.value("project_id").toString() != m_session->project() || row.value("stream_uuid").toString() != stream
                        || !QStringList{"guest", "member", "moderator", "administrator", "owner"}.contains(role)) {
                        finishChatAuthority(epoch, selection, tr("The encryption participants could not be verified.")); return;
                    }
                    ids.insert(id); users.insert(user); m_chatAuthority.roles.insert(user, role);
                }
                if (m_chatAuthority.roles.value(m_session->user()) != m_chatAuthority.role) {
                    finishChatAuthority(epoch, selection, tr("Current channel access could not be verified.")); return;
                }
                finishChatAuthority(epoch, selection);
            });
        });
    });
}

void NativeController::requestChatKey(const QString &keyUuid) { startChatRequest(keyUuid, false, false); }
void NativeController::checkChatKeyRequest(const QString &keyUuid) { startChatRequest(keyUuid, true, false); }
void NativeController::retryChatKeyRequest(const QString &keyUuid) { startChatRequest(keyUuid, true, true); }

void NativeController::startChatRequest(const QString &keyUuid, bool checking, bool retrying)
{
    const QPointer<NativeController> lookupGuard(this);
    checkChatLifetime();
    if (!lookupGuard) return;
    if (m_chatBusy || (m_chatKeyResponse && m_chatKeyResponse->busy)
        || !chatReadAllowed() || !currentStream() || !currentStream()->encrypted) return;
    const auto key = keyUuid.isEmpty() && currentStream()->security ? currentStream()->security->currentKey : keyUuid.toLower();
    if (!isUuid(key)) return;
    const NativeChatKeyVault::Address address{m_stream, key};
    auto existing = m_chatKeys->read(m_chatVaultLease, address);
    const bool usable = existing.failure == NativeChatKeyVault::Failure::None && existing.value.bytes.size() == 32;
    wipeChatBytes(existing.value.bytes);
    if (usable) { scheduleChatPreparation(); return; }
    const auto operation = ++m_chatOperation;
    const auto epoch = m_chatEpoch, selection = m_selection;
    m_chatRequestKey = key; m_chatLocalRequest.clear(); m_chatBusy = true; m_chatPhase = "verifying"; m_chatError.clear();
    {
        // Existing immutable metadata must not be rewritten to the device's new
        // label. Validate the wire bound only before admitting a NEW request.
        auto pending = m_chatKeys->pendingRequests(m_chatVaultLease, address);
        const auto cleanup = qScopeGuard([&] { for (auto &item : pending.values) item.clearSecrets(); });
        if (pending.failure == NativeChatKeyVault::Failure::None && pending.values.isEmpty()
            && m_chatKeys->device().name.toUcs4().size() > 255) {
            finishChatOperation(operation, key, "locked", tr("The device name exceeds 255 Unicode characters.")); return;
        }
    }
    const QPointer<NativeController> guard(this); emit stateChanged();
    if (!guard || !currentChatOperation(operation, key) || !m_chatBusy || !chatContext(epoch, selection)) return;
    emit messagesChanged();
    if (!guard || !currentChatOperation(operation, key) || !m_chatBusy || !chatContext(epoch, selection)) return;
    ensureChatAuthority([this, key, checking, retrying, operation, epoch, selection](bool okay, const QString &error) {
        if (!currentChatOperation(operation, key) || !m_chatBusy || !chatContext(epoch, selection)) return;
        if (!okay) { finishChatOperation(operation, key, "unresolved", error); return; }
        const NativeChatKeyVault::Address address{m_stream, key};
        auto pending = m_chatKeys->pendingRequests(m_chatVaultLease, address);
        const auto cleanup = qScopeGuard([&] { for (auto &item : pending.values) item.clearSecrets(); });
        if (pending.failure != NativeChatKeyVault::Failure::None || pending.values.size() > 1) {
            finishChatOperation(operation, key, "unresolved", tr("The saved key request could not be verified.")); return;
        }
        if (pending.values.isEmpty()) {
            if (checking) { finishChatOperation(operation, key, "unresolved", tr("There is no saved request to check.")); return; }
            auto pair = NativeChatCrypto::createRequestKeyPair();
            if (!pair) { finishChatOperation(operation, key, "locked", tr("A secure request key pair could not be created.")); return; }
            auto snapshot = m_chatKeys->read(m_chatVaultLease, address);
            NativeChatKeyVault::PendingRequest saved{QUuid::createUuid().toString(QUuid::WithoutBraces), {}, m_chatKeys->device().name,
                pair.publicKey, pair.privateKey, QDateTime::currentMSecsSinceEpoch(), snapshot.value.deletionRevision, {}, m_chatAuthority.defaultTopic};
            const auto scrub = qScopeGuard([&] { saved.clearSecrets(); });
            wipeChatBytes(snapshot.value.bytes); wipeChatBytes(pair.privateKey);
            if (m_chatKeys->putPendingRequest(m_chatVaultLease, address, saved) != NativeChatKeyVault::Failure::None) {
                finishChatOperation(operation, key, "locked", tr("The key request could not be saved securely.")); return;
            }
            updateChatPendingState(key, true, true);
            m_chatLocalRequest = saved.localId;
            postChatRequest(key, saved, epoch, selection, operation); return;
        }
        auto saved = pending.values.first();
        const auto topic = saved.targetTopic;
        const auto placement = !saved.messageId.isEmpty() ? saved.messageId : isUuid(topic)
            ? QUuid::createUuidV5(QUuid(topic), saved.localId.toUtf8()).toString(QUuid::WithoutBraces) : QString{};
        saved.clearSecrets();
        m_chatLocalRequest = saved.localId;
        if (placement.isEmpty()) { finishChatOperation(operation, key, "unresolved", tr("This legacy request needs history reconciliation.")); return; }
        request("GET", messenger + "/messages/" + placement, {}, {}, [this, key, saved, retrying, epoch, selection, operation](const ApiResult &result) {
            if (!currentChatOperation(operation, key) || !m_chatBusy || !chatContext(epoch, selection)) return;
            const auto parsed = parseMessage(result.data.toObject(), m_session->project());
            if (result.ok() && parsed && bindChatRequest(*parsed, key, saved, true)) {
                const QPointer<NativeController> guard(this);
                finishChatOperation(operation, key, "requested");
                if (guard && currentChatOperation(operation, key) && chatContext(epoch, selection)) drainChatResponses(parsed->uuid);
                return;
            }
            if (result.status == 404 && retrying && !saved.targetTopic.isEmpty() && saved.targetTopic == m_chatAuthority.defaultTopic) {
                postChatRequest(key, saved, epoch, selection, operation); return;
            }
            finishChatOperation(operation, key, "unresolved", tr("The key request has not been confirmed. Check does not send it again."));
        });
    });
}

bool NativeController::matchesChatRequest(const Message &message, const QString &key,
    const NativeChatKeyVault::PendingRequest &saved, bool observed) const
{
    if (!message.crypto || message.kind != "key_request" || message.project != m_session->project()
        || message.stream != m_stream || message.author != m_session->user()
        || (!observed && (!message.own || saved.targetTopic.isEmpty() || message.topic != saved.targetTopic))
        || message.crypto->keyUuid != key || message.crypto->deviceId != m_chatKeys->device().id
        || message.crypto->deviceName != saved.deviceName
        || strictChatBase64(message.crypto->encodedPublicKey, 2048) != saved.publicKey) return false;
    if (!saved.targetTopic.isEmpty())
        return message.uuid == QUuid::createUuidV5(QUuid(saved.targetTopic), saved.localId.toUtf8()).toString(QUuid::WithoutBraces);
    return observed && (saved.messageId.isEmpty() || saved.messageId == saved.localId || saved.messageId == message.uuid);
}

bool NativeController::bindChatRequest(const Message &message, const QString &key,
    const NativeChatKeyVault::PendingRequest &saved, bool observed)
{
    if (!matchesChatRequest(message, key, saved, observed)) return false;
    return m_chatKeys->bindPendingRequest(m_chatVaultLease, {m_stream, key}, saved.localId, message.uuid)
        == NativeChatKeyVault::Failure::None;
}

void NativeController::postChatRequest(const QString &key, const NativeChatKeyVault::PendingRequest &pending,
    quint64 epoch, quint64 selection, quint64 operation)
{
    if (!currentChatOperation(operation, key) || !m_chatBusy || !chatContext(epoch, selection) || pending.targetTopic != m_chatAuthority.defaultTopic) return;
    const NativeChatKeyVault::Address address{m_stream, key};
    auto snapshot = m_chatKeys->read(m_chatVaultLease, address);
    const bool current = snapshot.failure == NativeChatKeyVault::Failure::None
        && snapshot.value.deletionRevision == pending.deletionRevision && snapshot.value.bytes.isEmpty();
    wipeChatBytes(snapshot.value.bytes);
    if (!current) { finishChatOperation(operation, key, "unresolved", tr("The key request was superseded by a local change.")); return; }
    const QJsonObject payload{{"kind", "key_request"}, {"key_uuid", key}, {"device_id", m_chatKeys->device().id},
        {"device_name", pending.deviceName}, {"public_key", QString::fromLatin1(pending.publicKey.toBase64())}};
    const QJsonObject body{{"uuid", pending.localId}, {"stream_uuid", m_stream}, {"topic_uuid", pending.targetTopic}, {"payload", payload}};
    auto captured = pending; captured.clearSecrets();
    m_chatPhase = "sending";
    request("POST", messenger + "/messages/", body, {}, [this, key, captured, epoch, selection, operation](const ApiResult &result) {
        if (!currentChatOperation(operation, key) || !m_chatBusy || !chatContext(epoch, selection)) return;
        const auto parsed = parseMessage(result.data.toObject(), m_session->project());
        if (result.ok() && parsed && bindChatRequest(*parsed, key, captured)) {
            const QPointer<NativeController> guard(this);
            finishChatOperation(operation, key, "requested");
            if (guard && currentChatOperation(operation, key) && chatContext(epoch, selection)) drainChatResponses(parsed->uuid);
            return;
        }
        finishChatOperation(operation, key, "unresolved", tr("The key request has not been confirmed. Check does not send it again."));
    });
}

void NativeController::drainChatResponses(const QString &request)
{
    const auto responses = m_chatResponses;
    const auto epoch = m_chatEpoch, selection = m_selection;
    const QPointer<NativeController> guard(this);
    for (const auto &response : responses) {
        if (!guard || !chatContext(epoch, selection)) return;
        if (response.crypto && response.crypto->requestMessage == request) receiveChatControl(response);
    }
}

// Demand is supplied by an expanded loaded G/J or the ordinary/account
// prefilter of that same owned loaded row, never by a free R UUID.
void NativeController::resolveKeyServiceRequest(const QString &responseId)
{
    resolveKeyServiceRequest(responseId, 0);
}

void NativeController::resolveKeyServiceRequest(const QString &responseId, quint64 disclosureSerial)
{
    const QPointer<NativeController> admission(this);
    checkChatLifetime();
    if (!admission) return;
    const auto response = findMessage(responseId);
    const auto stream = currentStream();
    if (!authenticated() || ownerKey().isEmpty() || !stream || m_snapshotActive || m_syncBlocked
        || (m_chatAccessKnown && !m_chatReadableStreams.contains(m_stream)) || m_chatRequestFactsLimited
        || !response || !inConversation(*response) || !response->crypto
        || (response->kind != "key_grant" && response->kind != "key_reject")) return;
    if (stream->security && stream->security->viewer != m_session->user()) return;
    if (stream->encrypted && stream->security && (stream->security->source != "native"
        || !stream->security->provider.isEmpty() || !stream->security->providerIdentity.isEmpty())) return;
    const auto id = response->crypto->requestMessage;
    if (!isUuid(id)) return;
    auto fact = m_chatRequestFacts.find(id);
    const bool newlyDisclosed = fact == m_chatRequestFacts.end() || disclosureSerial > fact->disclosureSerial;
    if (fact != m_chatRequestFacts.end()) fact->disclosureSerial = std::max(fact->disclosureSerial, disclosureSerial);
    auto found = m_chatRequestLookups.find(id);
    if (found != m_chatRequestLookups.end()) {
        // All concurrently mounted consumers contribute to the same high-water
        // mark. A rerender of an older consumer is never another disclosure.
        if (found->phase != ChatRequestLookup::Phase::WaitingCache || !newlyDisclosed
            || fact == m_chatRequestFacts.end() || fact->lookupRetry == ChatRequestFact::LookupRetry::None) return;
        finishChatRequestLookup(id, found->ticket, false); // failed HTTP already settled; no active reply to abort
        fact = m_chatRequestFacts.find(id);
    }
    if (fact != m_chatRequestFacts.end() && !fact->unresolved
        && !(newlyDisclosed && fact->lookupRetry != ChatRequestFact::LookupRetry::None)) return; // typed/identity-negative facts stay sealed; snapshot omission needs a new disclosure
    ChatRequestLookup lookup;
    lookup.ticket = ++m_nextChatRequestLookup; lookup.response = responseId;
    lookup.owner = ownerKey(); lookup.project = m_session->project(); lookup.user = m_session->user();
    lookup.stream = m_stream; lookup.topic = m_topic; lookup.security = chatStreamSecurity(stream);
    lookup.selection = m_selection; lookup.generation = m_api->generation(); lookup.catalog = m_catalog;
    lookup.access = m_chatAccessRevision; lookup.cache = m_cache; lookup.lease = m_cacheLease;
    lookup.responseHash = chatRequestResponseHash(*response);
    lookup.units = id.size() + lookup.response.size() + lookup.owner.size() + lookup.project.size() + lookup.user.size()
        + lookup.stream.size() + lookup.topic.size() + lookup.security.size() + lookup.lease.owner.size() + lookup.responseHash.size();
    const auto reserve = fact == m_chatRequestFacts.end() ? id.size() + m_stream.size() : 0;
    if ((fact == m_chatRequestFacts.end() && m_chatRequestFacts.size() + m_chatOutcomeFacts.size() >= 500)
        || m_chatRequestFactUnits + reserve + lookup.units > 2000000) {
        m_chatRequestFactsLimited = true; m_chatIssueChanged = true;
        const QPointer<NativeController> guard(this); emit stateChanged(); if (guard) emit messagesChanged();
        return;
    }
    if (fact == m_chatRequestFacts.end()) {
        ChatRequestFact pending; pending.stream = m_stream; pending.unresolved = true;
        pending.units = reserve; m_chatRequestFactUnits += reserve;
        fact = m_chatRequestFacts.insert(id, std::move(pending));
    }
    fact->unresolved = true;
    if (fact->lookupRetry != ChatRequestFact::LookupRetry::Omitted
        && fact->lookupRetry != ChatRequestFact::LookupRetry::LiveDeleted) fact->lookupRetry = ChatRequestFact::LookupRetry::None;
    fact->disclosureSerial = std::max(fact->disclosureSerial, disclosureSerial);
    fact->revision = ++m_nextChatRequestFact; lookup.revision = fact->revision;
    fact->lookupUnits = lookup.units; fact->units += lookup.units; m_chatRequestFactUnits += lookup.units;
    m_chatRequestLookups.insert(id, std::move(lookup));
    if (const auto loaded = findMessage(id)) {
        const QPointer<NativeController> guard(this);
        const auto ticket = m_chatRequestLookups.value(id).ticket;
        observeChatRequest(*loaded); // current owned model wins without disk/network
        if (!guard) return;
        auto current = m_chatRequestLookups.find(id);
        if (current == m_chatRequestLookups.end() || current->ticket != ticket) return;
        auto publication = *current; publication.revision = m_chatRequestFacts.value(id).revision;
        current->revision = publication.revision;
        finishChatRequestLookup(id, ticket, false);
        if (!chatRequestLookupContext(id, publication)) return;
        emit stateChanged(); if (guard && chatRequestLookupContext(id, publication)) emit messagesChanged();
        return;
    }
    pumpChatRequestLookups();
}

bool NativeController::chatRequestLookupContext(const QString &id, const ChatRequestLookup &lookup, bool allowDeleted) const
{
    const auto fact = m_chatRequestFacts.constFind(id);
    const auto response = findMessage(lookup.response);
    return authenticated() && ownerKey() == lookup.owner && m_session->project() == lookup.project
        && m_session->user() == lookup.user && m_selection == lookup.selection && m_api->generation() == lookup.generation
        && m_catalog == lookup.catalog && m_chatAccessRevision == lookup.access && m_stream == lookup.stream && m_topic == lookup.topic
        && !m_snapshotActive && !m_syncBlocked && (!m_chatAccessKnown || m_chatReadableStreams.contains(m_stream))
        && lookup.security == chatStreamSecurity(currentStream()) && m_cache == lookup.cache
        && sameChatCache(m_cacheLease, lookup.lease) && (!lookup.cache || sameChatCache(lookup.cache->lease(), lookup.lease))
        && fact != m_chatRequestFacts.cend()
        && (allowDeleted || !fact->deleted || (fact->unresolved && (fact->lookupRetry == ChatRequestFact::LookupRetry::Omitted
            || fact->lookupRetry == ChatRequestFact::LookupRetry::LiveDeleted)))
        && fact->revision == lookup.revision
        && ((response && inConversation(*response) && response->crypto && response->crypto->requestMessage == id
                && chatRequestResponseHash(*response) == lookup.responseHash)
            || std::any_of(m_messages.cbegin(), m_messages.cend(), [&](const Message &row) {
                // The owned R body is shared by all current typed G/J consumers.
                // Retiring the first witness must not strand another loaded one.
                return inConversation(row) && row.crypto && (row.kind == "key_grant" || row.kind == "key_reject")
                    && row.crypto->requestMessage == id;
            }));
}

void NativeController::interruptChatRequestLookups()
{
    QVector<ApiClient::RequestId> handles;
    for (auto it = m_chatRequestLookups.cbegin(); it != m_chatRequestLookups.cend(); ++it) {
        if (it->handle.isValid()) handles.append(it->handle);
        auto fact = m_chatRequestFacts.find(it.key());
        if (fact != m_chatRequestFacts.end()) {
            fact->units -= fact->lookupUnits; m_chatRequestFactUnits -= fact->lookupUnits; fact->lookupUnits = 0;
        }
    }
    m_chatRequestLookups.clear(); // revoke exact tickets before abort can invoke a callback
    const QPointer<NativeController> guard(this);
    for (const auto handle : handles) { m_api->cancelRequest(handle); if (!guard) return; }
}

void NativeController::cancelChatRequestLookup(const QString &id)
{
    const auto found = m_chatRequestLookups.constFind(id);
    if (found == m_chatRequestLookups.cend()) return;
    const auto handle = found->handle; const auto ticket = found->ticket;
    finishChatRequestLookup(id, ticket, false); // erase identity before physical abort
    m_api->cancelRequest(handle);
}

void NativeController::finishChatRequestLookup(const QString &id, quint64 ticket, bool waitForCache)
{
    auto found = m_chatRequestLookups.find(id);
    if (found == m_chatRequestLookups.end() || found->ticket != ticket) return;
    if (waitForCache) {
        found->phase = ChatRequestLookup::Phase::WaitingCache; found->handle = {};
    } else {
        auto fact = m_chatRequestFacts.find(id);
        if (fact != m_chatRequestFacts.end()) {
            fact->units -= fact->lookupUnits; m_chatRequestFactUnits -= fact->lookupUnits; fact->lookupUnits = 0;
            fact->unresolved = false; // failed attempts do not loop on render/getter calls
        }
        m_chatRequestLookups.erase(found);
    }
    pumpChatRequestLookups();
}

void NativeController::pumpChatRequestLookups()
{
    if (m_chatRequestLookupPumpQueued) return;
    m_chatRequestLookupPumpQueued = true;
    QTimer::singleShot(0, this, [this] {
        m_chatRequestLookupPumpQueued = false;
        int active = 0;
        for (const auto &lookup : std::as_const(m_chatRequestLookups))
            if (lookup.phase == ChatRequestLookup::Phase::Cache || lookup.phase == ChatRequestLookup::Phase::Network
                || lookup.phase == ChatRequestLookup::Phase::LateCache) ++active;
        const auto ids = m_chatRequestLookups.keys();
        for (const auto &id : ids) {
            if (active >= 4) return;
            auto found = m_chatRequestLookups.find(id);
            if (found == m_chatRequestLookups.end() || found->phase != ChatRequestLookup::Phase::Queued) continue;
            const auto ticket = found->ticket;
            if (!chatRequestLookupContext(id, *found)) { finishChatRequestLookup(id, ticket, false); continue; }
            ++active;
            const bool savedOnly = found->savedOnly;
            const auto fact = m_chatRequestFacts.constFind(id);
            const bool omissionRecheck = fact != m_chatRequestFacts.cend()
                && (fact->lookupRetry == ChatRequestFact::LookupRetry::Omitted
                    || fact->lookupRetry == ChatRequestFact::LookupRetry::LiveDeleted);
            // A new disclosure captures a new revision, but old disk bytes are
            // not fresh evidence after an authoritative miss/failed cleanup.
            if (!omissionRecheck && found->cache && found->cache->available()) {
                found->phase = savedOnly ? ChatRequestLookup::Phase::LateCache : ChatRequestLookup::Phase::Cache;
                const auto cache = found->cache; const auto lease = found->lease;
                const QPointer<NativeController> guard(this);
                cache->getMessageBody(lease, id, this, [this, guard, id, ticket, savedOnly](std::optional<NativeCache::MessageBody> body) {
                    if (!guard) return;
                    auto current = m_chatRequestLookups.find(id);
                    if (current == m_chatRequestLookups.end() || current->ticket != ticket) return;
                    if (!chatRequestLookupContext(id, *current)) { finishChatRequestLookup(id, ticket, false); return; }
                    if (body) applyResolvedChatRequest(id, ticket, body->row, false);
                    if (!guard) return;
                    current = m_chatRequestLookups.find(id);
                    if (current == m_chatRequestLookups.end() || current->ticket != ticket) return;
                    if (savedOnly) finishChatRequestLookup(id, ticket, false);
                    else fetchChatRequestLookup(id, ticket);
                });
            } else if (savedOnly) finishChatRequestLookup(id, ticket, false);
            else {
                found->awaitCache = bool(found->cache) && !omissionRecheck;
                fetchChatRequestLookup(id, ticket);
            }
        }
    });
}

void NativeController::hydrateChatRequestLookups()
{
    for (auto it = m_chatRequestLookups.begin(); it != m_chatRequestLookups.end(); ++it) {
        if (it->phase != ChatRequestLookup::Phase::WaitingCache || !it->awaitCache
            || !it->cache || !it->cache->available() || !chatRequestLookupContext(it.key(), *it)) continue;
        it->savedOnly = true; it->awaitCache = false; it->phase = ChatRequestLookup::Phase::Queued;
    }
    pumpChatRequestLookups();
}

void NativeController::applyResolvedChatRequest(const QString &id, quint64 ticket, const QJsonObject &raw, bool persist)
{
    auto found = m_chatRequestLookups.find(id);
    if (found == m_chatRequestLookups.end() || found->ticket != ticket || !chatRequestLookupContext(id, *found)) return;
    const auto parsed = parseMessage(raw, found->project);
    if (!parsed || parsed->uuid != id || raw.value("user_uuid").toString().compare(found->user, Qt::CaseInsensitive) != 0) return;
    const QPointer<NativeController> guard(this);
    auto previous = m_chatRequestFacts.find(id);
    if (persist && previous != m_chatRequestFacts.end()
        && (previous->lookupRetry == ChatRequestFact::LookupRetry::Omitted
            || previous->lookupRetry == ChatRequestFact::LookupRetry::LiveDeleted)) {
        // Only this exact fresh canonical HTTP result can reopen this tombstoned R.
        // No old cache callback or pre-omission ticket gets this revision.
        previous->deleted = false; previous->unresolved = false;
        previous->lookupRetry = ChatRequestFact::LookupRetry::None;
        previous->revision = ++m_nextChatRequestFact; found->revision = previous->revision;
    }
    observeChatRequest(*parsed); // existing bounded fact, including full wrong-kind/stream contradictions
    if (!guard) return;
    found = m_chatRequestLookups.find(id);
    if (found == m_chatRequestLookups.end() || found->ticket != ticket) return;
    const auto fact = m_chatRequestFacts.constFind(id);
    if (fact == m_chatRequestFacts.cend() || fact->unresolved || fact->deleted || m_chatRequestFactsLimited) return;
    found->revision = fact->revision;
    if (!chatRequestLookupContext(id, *found)) return;
    const auto cache = found->cache; const auto lease = found->lease;
    if (parsed->stream == m_stream && parsed->kind == "key_request" && currentStream() && currentStream()->encrypted
        && chatContext(m_chatEpoch, m_selection))
        receiveChatControl(*parsed); // existing shared authority/recipient path, no new mutation
    if (!guard) return;
    found = m_chatRequestLookups.find(id);
    if (found == m_chatRequestLookups.end() || found->ticket != ticket || !chatRequestLookupContext(id, *found)) return;
    if (persist && cache) cache->putMessageBody(lease, raw);
    if (!guard) return;
    found = m_chatRequestLookups.find(id);
    if (found == m_chatRequestLookups.end() || found->ticket != ticket || !chatRequestLookupContext(id, *found)) return;
    emit stateChanged();
    if (!guard) return;
    found = m_chatRequestLookups.find(id);
    if (found != m_chatRequestLookups.end() && found->ticket == ticket && chatRequestLookupContext(id, *found)) emit messagesChanged();
}

void NativeController::fetchChatRequestLookup(const QString &id, quint64 ticket)
{
    auto found = m_chatRequestLookups.find(id);
    if (found == m_chatRequestLookups.end() || found->ticket != ticket) return;
    if (!chatRequestLookupContext(id, *found)) { finishChatRequestLookup(id, ticket, false); return; }
    found->phase = ChatRequestLookup::Phase::Network;
    const auto project = found->project;
    const QPointer<NativeController> guard(this);
    const auto handle = request("GET", messenger + "/messages/", {}, {{"project_id", project}, {"uuid", id}},
        [this, guard, id, ticket](const ApiResult &result) {
        if (!guard) return;
        auto current = m_chatRequestLookups.find(id);
        if (current == m_chatRequestLookups.end() || current->ticket != ticket) return;
        current->handle = {};
        if (!chatRequestLookupContext(id, *current)) { finishChatRequestLookup(id, ticket, false); return; }
        if (!result.ok() || !result.data.isArray() || !result.nextMarker.isEmpty()) {
            auto fact = m_chatRequestFacts.find(id);
            if (fact != m_chatRequestFacts.end() && fact->unresolved
                && fact->lookupRetry != ChatRequestFact::LookupRetry::Omitted
                && fact->lookupRetry != ChatRequestFact::LookupRetry::LiveDeleted) fact->lookupRetry = ChatRequestFact::LookupRetry::Failed;
            if (current->awaitCache && current->cache && current->cache->available()) {
                current->savedOnly = true; current->awaitCache = false; current->phase = ChatRequestLookup::Phase::Queued;
                pumpChatRequestLookups();
            } else finishChatRequestLookup(id, ticket, current->awaitCache && current->cache);
            return; // failure is not authoritative omission; no second HTTP attempt
        }
        const auto rows = result.data.toArray();
        if (rows.isEmpty()) {
            const auto cache = current->cache; const auto lease = current->lease;
            auto fact = m_chatRequestFacts.find(id);
            fact->deleted = true; fact->unresolved = false;
            fact->lookupRetry = ChatRequestFact::LookupRetry::Omitted; fact->revision = ++m_nextChatRequestFact;
            auto publication = *current; publication.revision = fact->revision;
            reduceChatKeyOutcomes();
            if (!guard || !chatRequestLookupContext(id, publication, true)) return;
            finishChatRequestLookup(id, ticket, false); // seal the attempt BEFORE required erase/publication
            if (cache && m_cache == cache && sameChatCache(m_cacheLease, lease)) {
                const auto erased = [this, guard, cache, id, publication](bool okay) {
                    if (!guard || okay || !chatRequestLookupContext(id, publication, true)) return;
                    const auto error = cache && !cache->error().isEmpty() ? cache->error()
                        : NativeCacheService::tr("The local message cache is unavailable.");
                    if (m_chatError == error) return;
                    m_chatError = error;
                    emit stateChanged();
                    if (guard && chatRequestLookupContext(id, publication, true)) emit messagesChanged();
                };
                const bool admitted = cache->removeMessageBodyAndHistory(lease, id, this, erased);
                if (!guard) return;
                if (!admitted) erased(false);
            }
            if (!guard || !chatRequestLookupContext(id, publication, true)) return;
            emit stateChanged();
            if (guard && chatRequestLookupContext(id, publication, true)) emit messagesChanged();
            return;
        }
        if (rows.size() != 1 || !rows.first().isObject()) {
            auto fact = m_chatRequestFacts.find(id);
            if (fact != m_chatRequestFacts.end() && fact->unresolved
                && fact->lookupRetry != ChatRequestFact::LookupRetry::Omitted
                && fact->lookupRetry != ChatRequestFact::LookupRetry::LiveDeleted) fact->lookupRetry = ChatRequestFact::LookupRetry::Failed;
            finishChatRequestLookup(id, ticket, false); return;
        }
        const auto raw = rows.first().toObject(); const auto parsed = parseMessage(raw, current->project);
        if (!parsed || parsed->uuid != id || raw.value("user_uuid").toString().compare(current->user, Qt::CaseInsensitive) != 0) {
            // Explicit wrong identity/shape is no deletion authority and cannot
            // be overwritten by an old late-cache callback in this attempt.
            auto fact = m_chatRequestFacts.find(id);
            const auto old = fact->units; const auto lookupUnits = fact->lookupUnits;
            const auto disclosure = fact->disclosureSerial; const auto accountMissing = fact->accountMissing;
            *fact = {}; fact->stream = m_stream; fact->lookupUnits = lookupUnits; fact->disclosureSerial = disclosure;
            fact->accountMissing = accountMissing;
            fact->revision = ++m_nextChatRequestFact; fact->units = id.size() + m_stream.size() + lookupUnits;
            m_chatRequestFactUnits += fact->units - old;
            auto publication = *current; publication.revision = fact->revision;
            reduceChatKeyOutcomes();
            if (!guard) return;
            finishChatRequestLookup(id, ticket, false);
            if (!chatRequestLookupContext(id, publication)) return;
            emit stateChanged();
            if (guard && chatRequestLookupContext(id, publication)) emit messagesChanged();
            return;
        }
        applyResolvedChatRequest(id, ticket, raw, true);
        if (guard) finishChatRequestLookup(id, ticket, false);
    });
    if (!guard) return;
    found = m_chatRequestLookups.find(id);
    if (found != m_chatRequestLookups.end() && found->ticket == ticket && chatRequestLookupContext(id, *found)) found->handle = handle;
    else m_api->cancelRequest(handle);
}

void NativeController::observeChatRequest(const Message &message)
{
    if (message.project != m_session->project()) return;
    const auto previous = m_chatRequestFacts.constFind(message.uuid);
    // A full canonical update owns placement. Preserve contradictions for an
    // already observed UUID even when it leaves this stream; never admit an
    // arbitrary off-route request into the bounded observation window.
    if (message.stream != m_stream && previous == m_chatRequestFacts.cend()) return;
    if (previous != m_chatRequestFacts.cend() && previous->deleted) return;
    if (message.kind != "key_request" && previous == m_chatRequestFacts.cend()) return;
    ChatRequestFact fact;
    fact.stream = message.stream; fact.author = message.author;
    if (message.kind == "key_request" && message.crypto) {
        fact.key = message.crypto->keyUuid; fact.device = message.crypto->deviceId;
        fact.name = message.crypto->deviceName; fact.publicKey = message.crypto->encodedPublicKey;
    }
    if (previous != m_chatRequestFacts.cend() && previous->stream == fact.stream && previous->key == fact.key
        && previous->author == fact.author && previous->device == fact.device && previous->name == fact.name
        && previous->publicKey == fact.publicKey) return;
    const auto previousKey = previous == m_chatRequestFacts.cend() ? QString{} : previous->key;
    fact.revision = ++m_nextChatRequestFact;
    fact.lookupUnits = previous == m_chatRequestFacts.cend() ? 0 : previous->lookupUnits;
    fact.disclosureSerial = previous == m_chatRequestFacts.cend() ? 0 : previous->disclosureSerial;
    fact.accountMissing = previous != m_chatRequestFacts.cend() && previous->accountMissing;
    fact.units = message.uuid.size() + fact.stream.size() + fact.key.size() + fact.author.size()
        + fact.device.size() + fact.name.size() + fact.publicKey.size() + fact.lookupUnits;
    const auto oldUnits = previous == m_chatRequestFacts.cend() ? 0 : previous->units;
    if ((previous == m_chatRequestFacts.cend() && m_chatRequestFacts.size() + m_chatOutcomeFacts.size() >= 500)
        || m_chatRequestFactUnits - oldUnits + fact.units > 2000000) {
        // Never evict contradictory/deleted observations and then accept an
        // older response. A new owned lifetime may rebuild the bounded window.
        m_chatRequestFactsLimited = true;
        return;
    }
    m_chatRequestFactUnits += fact.units - oldUnits;
    m_chatRequestFacts.insert(message.uuid, std::move(fact)); m_chatIssueChanged = true;
    if (!previousKey.isEmpty() && std::none_of(m_chatRequestFacts.cbegin(), m_chatRequestFacts.cend(),
        [&](const ChatRequestFact &row) { return row.key == previousKey; })) m_chatIssueKeys.remove(previousKey);
    reduceChatKeyOutcomes();
}

bool NativeController::currentChatRequest(const Message &response,
    const NativeChatKeyVault::PendingRequest &pending) const
{
    if (m_chatRequestFactsLimited || !response.crypto) return false;
    const auto found = m_chatRequestFacts.constFind(response.crypto->requestMessage);
    if (found == m_chatRequestFacts.cend()) return true; // durable public binding may predate this loaded window
    return !found->deleted && found->stream == response.stream && found->key == response.crypto->keyUuid
        && found->author == m_session->user() && found->device == m_chatKeys->device().id
        && found->name == pending.deviceName && strictChatBase64(found->publicKey, 2048) == pending.publicKey;
}

void NativeController::updateChatPendingState(const QString &key, bool known, bool requested)
{
    const auto found = m_chatIssueKeys.find(key);
    if (found == m_chatIssueKeys.end()) return;
    requested = known && requested;
    if (found->pendingKnown != known || found->requested != requested) m_chatIssueChanged = true;
    found->pendingKnown = known; found->requested = requested;
}

bool NativeController::receiveChatControl(const Message &message)
{
    if (!currentStream() || !currentStream()->encrypted
        || !message.crypto || message.stream != m_stream || message.project != m_session->project()
        || (message.kind != "key_request" && message.kind != "key_grant" && message.kind != "key_reject")
        || !chatVaultMatches()) return true;
    const QPointer<NativeController> lifetimeGuard(this);
    checkChatLifetime();
    if (!lifetimeGuard) return true;
    const auto epoch = m_chatEpoch, selection = m_selection;
    if (!chatContext(epoch, selection)) {
        // Catch-up events are already acknowledged by RealtimeClient, while
        // writes/authority remain blocked until its live watermark. Keep only
        // bounded typed data; no binding or key receipt is admitted here.
        if (m_syncBlocked && m_catalogVerified && !m_snapshotActive && chatReadAllowed()) {
            observeChatRequest(message);
            retainChatControl(message);
        }
        return true;
    }
    observeChatRequest(message);
    observeChatKeyOutcome(message);
    const auto &payload = *message.crypto;
    if (message.kind == "key_request" && !m_chatRequestFactsLimited && !m_chatIssueKeys.contains(payload.keyUuid)) {
        // The same bounded availability cache is shared with announcements.
        if (m_chatIssueKeys.size() >= 500) {
            m_chatRequestFactsLimited = true; m_chatIssueChanged = true; return true;
        }
        // At most one metadata read per loaded generation/material lifetime.
        // The existing control slices bound work; getters never touch VaultSQL.
        auto key = m_chatKeys->read(m_chatVaultLease, {message.stream, payload.keyUuid});
        ChatIssueKey available;
        available.deletion = key.value.deletionRevision;
        available.state = key.failure != NativeChatKeyVault::Failure::None ? ChatKeyAvailability::Error
            : key.value.bytes.size() == 32 ? ChatKeyAvailability::Present
            : key.value.bytes.isEmpty() ? ChatKeyAvailability::Missing : ChatKeyAvailability::Error;
        wipeChatBytes(key.value.bytes);
        if (m_chatRequestFacts.contains(message.uuid)) m_chatIssueKeys.insert(payload.keyUuid, available);
        m_chatIssueChanged = true;
    }
    auto known = m_chatKeys->pendingRequests(m_chatVaultLease, {message.stream, payload.keyUuid});
    const auto scrub = qScopeGuard([&] { for (auto &item : known.values) item.clearSecrets(); });
    updateChatPendingState(payload.keyUuid, known.failure == NativeChatKeyVault::Failure::None, !known.values.isEmpty());
    if (known.failure != NativeChatKeyVault::Failure::None) return true;
    if (message.kind == "key_request") {
        QVector<qsizetype> bound, candidates;
        for (qsizetype index = 0; index < known.values.size(); ++index) {
            const auto &pending = known.values[index];
            if (!matchesChatRequest(message, payload.keyUuid, pending, true)) continue;
            if (pending.messageId == message.uuid) bound.append(index);
            else if (pending.messageId.isEmpty() || pending.messageId == pending.localId) candidates.append(index);
        }
        const auto &matches = bound.isEmpty() ? candidates : bound;
        if (matches.size() != 1) return true;
        if (bound.isEmpty()) {
            const auto completed = m_chatKeys->requestCompletionByMessage(m_chatVaultLease,
                {message.stream, payload.keyUuid}, message.uuid);
            if (completed.failure != NativeChatKeyVault::Failure::None || completed.value) return true;
        }
        const QPointer<NativeController> guard(this);
        const auto result = m_chatKeys->bindPendingRequest(m_chatVaultLease, {message.stream, payload.keyUuid},
            known.values[matches.first()].localId, message.uuid);
        if (guard && result == NativeChatKeyVault::Failure::None && chatContext(epoch, selection))
            drainChatResponses(message.uuid);
        return true;
    }
    if (payload.recipientDevice != m_chatKeys->device().id) {
        if (!m_chatRequestFacts.contains(payload.requestMessage)) retainChatControl(message);
        return true;
    }
    bool matches = false;
    for (const auto &item : known.values) {
        const auto placement = !item.messageId.isEmpty() && item.messageId != item.localId ? item.messageId
            : isUuid(item.targetTopic) ? QUuid::createUuidV5(QUuid(item.targetTopic), item.localId.toUtf8())
                .toString(QUuid::WithoutBraces) : QString{};
        const bool legacy = item.targetTopic.isEmpty() && (item.messageId.isEmpty() || item.messageId == item.localId);
        matches = matches || placement == payload.requestMessage || legacy;
    }
    if (!matches) { m_chatResponses.remove(message.uuid); return true; }
    const auto responseKey = message.uuid;
    if (m_chatResponses.size() >= 32 && !m_chatResponses.contains(responseKey)) return false;
    m_chatResponses.insert(responseKey, message);
    ensureChatAuthority([this, responseKey, epoch, selection](bool okay, const QString &) {
        if (!okay || !chatContext(epoch, selection) || !m_chatResponses.contains(responseKey)) return;
        const QPointer<NativeController> wake(this);
        const auto resume = qScopeGuard([wake] { if (wake) wake->scheduleChatControls(); });
        const auto message = m_chatResponses.value(responseKey); const auto &payload = *message.crypto;
        const auto role = m_chatAuthority.roles.value(message.author);
        if ((message.author != m_chatAuthority.issuerOwner && role != "owner" && role != "administrator")
            || payload.recipientDevice != m_chatKeys->device().id) { m_chatResponses.remove(responseKey); return; }
        const NativeChatKeyVault::Address address{message.stream, payload.keyUuid};
        auto requests = m_chatKeys->pendingRequests(m_chatVaultLease, address);
        const auto cleanup = qScopeGuard([&] { for (auto &item : requests.values) item.clearSecrets(); });
        if (requests.failure != NativeChatKeyVault::Failure::None) return;
        for (auto &pending : requests.values) {
            if (pending.messageId != payload.requestMessage || pending.messageId.isEmpty()
                || !currentChatRequest(message, pending)) { pending.clearSecrets(); continue; }
            m_chatResponses.remove(responseKey);
            NativeChatKeyVault::Completion completion{pending.localId, pending.messageId, message.uuid,
                message.kind == "key_grant" ? NativeChatKeyVault::Outcome::Granted : NativeChatKeyVault::Outcome::Rejected};
            QByteArray group;
            if (message.kind == "key_grant") {
                const auto envelope = strictChatBase64(payload.encodedContent, 40000);
                if (envelope.isEmpty()) { pending.clearSecrets(); continue; }
                auto decrypted = NativeChatCrypto::decryptGroupKeyGrant(pending.privateKey, pending.publicKey, envelope,
                    {message.stream, payload.keyUuid, pending.messageId, payload.senderDevice, payload.recipientDevice});
                if (!decrypted) { pending.clearSecrets(); continue; }
                group = std::move(decrypted.bytes);
            }
            pending.clearSecrets();
            const auto operation = m_chatOperation;
            const auto localId = pending.localId;
            const QPointer<NativeController> guard(this);
            const auto done = m_chatKeys->completeRequest(m_chatVaultLease, address, completion, group);
            wipeChatBytes(group);
            if (!guard) return;
            if (!chatContext(epoch, selection)) return;
            if (done == NativeChatKeyVault::Failure::None) {
                // Completion may synchronously remove the availability entry
                // through a Grant material signal. Reacquire only afterwards.
                auto remaining = m_chatKeys->pendingRequests(m_chatVaultLease, address);
                const auto scrub = qScopeGuard([&] { for (auto &item : remaining.values) item.clearSecrets(); });
                updateChatPendingState(payload.keyUuid, remaining.failure == NativeChatKeyVault::Failure::None,
                    !remaining.values.isEmpty());
            }
            if (done == NativeChatKeyVault::Failure::None && currentChatOperation(operation, payload.keyUuid)
                && m_chatLocalRequest == localId) {
                auto committed = m_chatKeys->read(m_chatVaultLease, address);
                const bool current = committed.failure == NativeChatKeyVault::Failure::None
                    && committed.value.deletionRevision == pending.deletionRevision
                    && (message.kind != "key_grant" || committed.value.bytes.size() == 32);
                wipeChatBytes(committed.value.bytes);
                if (!current) { finishChatOperation(operation, payload.keyUuid, "locked", tr("The key response was superseded by a local change.")); return; }
                finishChatOperation(operation, payload.keyUuid, message.kind == "key_grant" ? "ready" : "rejected");
            }
            return;
        }
    });
    return true;
}

void NativeController::retainChatControl(const Message &message)
{
    const auto old = m_chatDeferredControls.constFind(message.uuid);
    const auto oldUnits = old == m_chatDeferredControls.cend() ? 0 : chatControlUnits(*old);
    const auto units = chatControlUnits(message);
    if ((old == m_chatDeferredControls.cend() && m_chatDeferredControls.size() >= 500)
        || m_chatDeferredUnits - oldUnits + units > 2000000) {
        m_chatDeferredLimited = true;
        m_chatRequestFactsLimited = true;
        return;
    }
    m_chatDeferredUnits += units - oldUnits;
    m_chatDeferredControls.insert(message.uuid, message);
    // Record order only; validation/reduction remains in bounded work slices.
    rememberChatOutcomeOrder({message});
}

void NativeController::resetChatControls(bool preserveDeferred, ChatOutcomeRetention outcomes)
{
    const QPointer<NativeController> lookupGuard(this);
    interruptChatRequestLookups();
    if (!lookupGuard) return;
    ++m_chatControlWork;
    m_chatControlRows.clear(); m_chatControlWindow.clear(); m_chatControlOffset = m_chatAccountOffset = 0;
    m_chatControlAuthorityFailed = false;
    if (outcomes == ChatOutcomeRetention::OrderOnly) {
        for (auto &fact : m_chatOutcomeFacts) {
            m_chatRequestFactUnits -= fact.units;
            auto id = std::move(fact.id), stream = std::move(fact.stream);
            fact = {}; fact.id = std::move(id); fact.stream = std::move(stream);
            fact.units = fact.id.size() + fact.stream.size(); m_chatRequestFactUnits += fact.units;
        }
    } else if (outcomes == ChatOutcomeRetention::Clear) {
        for (const auto &fact : std::as_const(m_chatOutcomeFacts)) m_chatRequestFactUnits -= fact.units;
        m_chatOutcomeFacts.clear();
    }
    if (preserveDeferred) {
        for (const auto &response : std::as_const(m_chatResponses)) retainChatControl(response);
    } else {
        m_chatRequestFacts.clear(); m_chatRequestFactUnits = 0;
        for (const auto &fact : std::as_const(m_chatOutcomeFacts)) m_chatRequestFactUnits += fact.units;
        if (outcomes == ChatOutcomeRetention::Clear) m_chatRequestFactsLimited = false;
        m_chatIssueKeys.clear(); m_chatIssueChanged = false;
        m_chatDeferredControls.clear(); m_chatDeferredUnits = 0; m_chatDeferredLimited = false;
    }
}

void NativeController::prepareChatAnnouncement(const Message &message)
{
    if ((message.kind != "key_announced" && (!currentStream() || currentStream()->encrypted))
        || !message.crypto || !isUuid(message.crypto->keyUuid)
        || !inConversation(message) || !chatContext(m_chatEpoch, m_selection)) return;
    const auto &key = message.crypto->keyUuid;
    if (m_chatIssueKeys.contains(key)) return;
    // The existing control queue bounds work to eight rows/turn. Generation
    // metadata is bounded too; no getter or per-render Vault access is added.
    if (m_chatIssueKeys.size() >= 500) {
        m_chatRequestFactsLimited = true; m_chatIssueChanged = true; return;
    }
    auto stored = m_chatKeys->read(m_chatVaultLease, {message.stream, key});
    ChatIssueKey cached;
    cached.deletion = stored.value.deletionRevision;
    cached.state = stored.failure != NativeChatKeyVault::Failure::None ? ChatKeyAvailability::Error
        : stored.value.bytes.size() == 32 ? ChatKeyAvailability::Present
        : stored.value.bytes.isEmpty() ? ChatKeyAvailability::Missing : ChatKeyAvailability::Error;
    wipeChatBytes(stored.value.bytes);
    auto pending = m_chatKeys->pendingRequests(m_chatVaultLease, {message.stream, key});
    const auto scrub = qScopeGuard([&] { for (auto &item : pending.values) item.clearSecrets(); });
    cached.pendingKnown = pending.failure == NativeChatKeyVault::Failure::None;
    cached.requested = cached.pendingKnown && !pending.values.isEmpty();
    m_chatIssueKeys.insert(key, cached); m_chatIssueChanged = true;
}

void NativeController::scheduleChatControls()
{
    const QPointer<NativeController> lookupGuard(this);
    checkChatLifetime();
    if (!lookupGuard) return;
    if (m_chatControlsQueued) return;
    m_chatControlsQueued = true;
    QTimer::singleShot(0, this, [this] {
        const QPointer<NativeController> lookupGuard(this);
        m_chatControlsQueued = false; checkChatLifetime();
        if (!lookupGuard) return;
        const auto epoch = m_chatEpoch, selection = m_selection;
        if (!chatPublicContext(epoch, selection)) return;
        QVector<Message> controls;
        QCryptographicHash fingerprint(QCryptographicHash::Sha256);
        qint64 units = 0;
        bool limited = m_chatDeferredLimited;
        QHash<QString, Message> deferred = m_chatDeferredControls;
        QSet<QString> accountReferences;
        for (const auto &row : m_messages) {
            if (!row.crypto || (row.kind != "key_request" && row.kind != "key_grant" && row.kind != "key_reject"
                && row.kind != "key_announced")) continue;
            // The loaded model has already passed the REST/RT revision fence;
            // a deferred copy of this UUID may predate that authoritative row.
            controls.append(row);
            if (inConversation(row) && (row.kind == "key_grant" || row.kind == "key_reject"))
                accountReferences.insert(row.crypto->requestMessage);
            deferred.remove(row.uuid);
            if (controls.size() > 500) { limited = true; break; }
        }
        // Accepted input order was captured before display sorting/deferred
        // merging. These rows only fill/replace each UUID's retained position.
        if (!limited) for (const auto &row : std::as_const(deferred)) {
            controls.append(row);
            if (controls.size() > 500) { limited = true; break; }
        }
        for (const auto &row : std::as_const(controls)) {
            const auto &p = *row.crypto;
            const QJsonArray fields{row.uuid, row.stream, row.topic, row.project, row.author, row.kind,
                p.keyUuid, p.deviceId, p.deviceName, p.encodedPublicKey, p.requestMessage,
                p.senderDevice, p.recipientDevice, p.encodedContent};
            for (const auto &field : fields) units += field.toString().size();
            fingerprint.addData(QJsonDocument(fields).toJson(QJsonDocument::Compact));
            if (controls.size() > 500 || units > 2000000) { limited = true; break; }
        }
        auto window = fingerprint.result();
        if (limited) {
            window.append("limited");
            if (!m_chatRequestFactsLimited) m_chatIssueChanged = true;
            m_chatRequestFactsLimited = true;
        }
        if (window != m_chatControlWindow) {
            ++m_chatControlWork; m_chatControlWindow = window; m_chatControlOffset = 0;
            m_chatControlRows = limited ? QVector<Message>{} : std::move(controls);
            std::stable_sort(m_chatControlRows.begin(), m_chatControlRows.end(), [](const Message &a, const Message &b) {
                return a.kind == "key_request" && b.kind != "key_request";
            });
        }
        // Source's account resolver is outside the filtered/collapsed list.
        // Its presence is per missing R, not per response/delegate revision.
        if (const auto stream = currentStream(); stream && !stream->encrypted) {
            for (auto it = m_chatRequestFacts.begin(); it != m_chatRequestFacts.end(); ++it) {
                const bool knownBody = !it->unresolved && !it->deleted && it->lookupRetry == ChatRequestFact::LookupRetry::None;
                if (!accountReferences.contains(it.key()) || knownBody) it->accountMissing = false;
            }
            m_chatAccountOffset = 0;
            processChatAccountRequests(epoch, selection, m_chatControlWork);
        } else processChatControls(epoch, selection, m_chatControlWork);
    });
}

void NativeController::processChatAccountRequests(quint64 epoch, quint64 selection, quint64 work)
{
    const auto current = [&] { return chatPublicContext(epoch, selection) && m_chatControlWork == work
        && currentStream() && !currentStream()->encrypted; };
    if (!current() || m_chatRequestFactsLimited) return;
    const QPointer<NativeController> guard(this);
    int processed = 0;
    while (m_chatAccountOffset < m_chatControlRows.size() && processed < 8) {
        const auto row = m_chatControlRows[m_chatAccountOffset++]; ++processed;
        if (!inConversation(row) || !row.crypto || !findMessage(row.uuid)) continue; // deferred-only rows are not list consumers
        if (row.kind == "key_request") {
            observeChatRequest(row);
            if (chatContext(epoch, selection) && m_chatAuthority.ready && m_chatAuthority.epoch == epoch)
                prepareChatAnnouncement(row); // same bounded generation cache; no key request/bind
        }
        else if (row.kind == "key_grant" || row.kind == "key_reject") {
            const auto id = row.crypto->requestMessage;
            const auto request = m_chatRequestFacts.constFind(id);
            if (request != m_chatRequestFacts.cend() && !request->deleted && request->stream == row.stream
                && request->key == row.crypto->keyUuid && request->device == row.crypto->recipientDevice
                && chatContext(epoch, selection) && m_chatAuthority.ready && m_chatAuthority.epoch == epoch)
                prepareChatAnnouncement(row);
            if (chatContext(epoch, selection) && m_chatAuthority.ready && m_chatAuthority.epoch == epoch)
                observeChatKeyOutcome(row);
            // A cached/loaded typed negative is a known body too. It cannot
            // produce a recipient, but its mere presence must not cause GETs.
            auto fact = m_chatRequestFacts.constFind(id);
            const bool knownBody = fact != m_chatRequestFacts.cend() && !fact->unresolved && !fact->deleted
                && fact->lookupRetry == ChatRequestFact::LookupRetry::None;
            if (const auto loaded = findMessage(id)) observeChatRequest(*loaded);
            else if (!knownBody && (fact == m_chatRequestFacts.cend() || !fact->accountMissing
                || (fact->unresolved && fact->lookupRetry != ChatRequestFact::LookupRetry::Failed
                    && !m_chatRequestLookups.contains(id)))) {
                // An unfinished capture canceled by a pure trust reset is not
                // a settled failure and not a new consumer disclosure.
                const auto serial = fact == m_chatRequestFacts.cend() ? 1
                    : fact->accountMissing ? fact->disclosureSerial : fact->disclosureSerial + 1;
                resolveKeyServiceRequest(row.uuid, serial);
                if (!guard || !current()) return;
                auto admitted = m_chatRequestFacts.find(id);
                if (admitted != m_chatRequestFacts.end()) admitted->accountMissing = true;
            }
        }
        if (!guard || !current()) return;
    }
    if (m_chatAccountOffset < m_chatControlRows.size()) {
        QTimer::singleShot(0, this, [this, epoch, selection, work] { processChatAccountRequests(epoch, selection, work); });
        return;
    }
    if (m_chatIssueChanged) {
        m_chatIssueChanged = false;
        emit stateChanged();
        if (!guard || !current()) return;
        emit messagesChanged();
        if (!guard || !current()) return;
    }
    // An unfinished owned response still needs fresh authority for explicit Retry.
    if (m_chatControlRows.isEmpty() && (!m_chatKeyResponse || m_chatKeyResponse->phase == "ready"
        || !chatKeyResponseOwned(false))) return;
    // Only ordinary native sessions have Source's holder/manager preparation.
    // Public account correlation above also works when no such session exists.
    const auto stream = currentStream();
    if (!chatContext(epoch, selection) || !stream || !stream->security
        || stream->security->viewer != m_session->user() || stream->security->source != "native"
        || !stream->security->provider.isEmpty() || !stream->security->providerIdentity.isEmpty()) return;
    if (!m_chatAuthority.ready && !m_chatAuthorityLoading && !m_chatControlAuthorityFailed) {
        ensureChatAuthority([this, epoch, selection](bool okay, const QString &) {
            if (!chatContext(epoch, selection)) return;
            if (okay) { m_chatIssueChanged = true; scheduleChatControls(); }
            else m_chatControlAuthorityFailed = true;
        });
    }
}

void NativeController::processChatControls(quint64 epoch, quint64 selection, quint64 work)
{
    if (!chatContext(epoch, selection) || m_chatControlWork != work
        || !currentStream() || !currentStream()->encrypted) return;
    const QPointer<NativeController> guard(this);
    int processed = 0;
    while (m_chatControlOffset < m_chatControlRows.size() && processed < 8) {
        const auto row = m_chatControlRows[m_chatControlOffset];
        if (!m_chatAuthority.ready || m_chatAuthority.epoch != epoch) {
            // Observe/bind owned requests before authority, as in the recipient
            // path. Only responses/issuer actions wait for shared authority.
            if (!m_chatAuthorityLoading && !m_chatControlAuthorityFailed) {
                ensureChatAuthority([this, epoch, selection](bool okay, const QString &) {
                    if (!chatContext(epoch, selection)) return;
                    if (okay) { m_chatIssueChanged = true; scheduleChatControls(); }
                    else m_chatControlAuthorityFailed = true; // explicit Check/new live lifetime may retry
                });
                if (!guard || !chatContext(epoch, selection) || m_chatControlWork != work) return;
            }
            if (row.kind != "key_request") return;
        }
        if (row.kind == "key_announced") prepareChatAnnouncement(row);
        else if (!receiveChatControl(row)) return; // capacity release schedules another slice
        if (!guard || !chatContext(epoch, selection) || m_chatControlWork != work) return;
        ++m_chatControlOffset; ++processed;
    }
    if (m_chatControlOffset < m_chatControlRows.size()) QTimer::singleShot(0, this, [this, epoch, selection, work] {
        processChatControls(epoch, selection, work);
    });
    else {
        for (auto it = m_chatDeferredControls.begin(); it != m_chatDeferredControls.end();) {
            const auto &row = *it;
            const bool awaitingRequest = row.crypto && (row.kind == "key_grant" || row.kind == "key_reject")
                && !m_chatRequestFacts.contains(row.crypto->requestMessage);
            if (awaitingRequest) ++it;
            else { m_chatDeferredUnits -= chatControlUnits(row); it = m_chatDeferredControls.erase(it); }
        }
        if (m_chatIssueChanged) {
            m_chatIssueChanged = false;
            emit stateChanged();
            if (!guard || !chatContext(epoch, selection) || m_chatControlWork != work) return;
            emit messagesChanged();
        }
    }
}

// Issuer-side decisions are separate from this device's recipient receipts.
// All row/getter data below is public metadata; key reads occur in work/actions.
bool NativeController::chatIssuerAllowed() const
{
    if (!currentStream() || !currentStream()->encrypted
        || !chatContext(m_chatEpoch, m_selection) || !m_chatAuthority.ready
        || m_chatAuthority.epoch != m_chatEpoch || m_chatAuthority.owner != ownerKey()
        || m_chatAuthority.stream != m_stream) return false;
    const auto role = m_chatAuthority.roles.value(m_session->user());
    return m_chatAuthority.issuerOwner == m_session->user() || role == "owner" || role == "administrator";
}

bool NativeController::chatResponseAllowed(const ChatRequestFact &fact) const
{
    const auto *stream = currentStream();
    if (!stream) return false;
    if (stream->encrypted) return chatIssuerAllowed();
    // Ordinary current participants may relay an exact locally held generation.
    // Managers retain Reject without A; Grant separately checks material/deletion.
    if (!stream->security || stream->security->viewer != m_session->user()
        || stream->security->source != "native" || !stream->security->provider.isEmpty()
        || !stream->security->providerIdentity.isEmpty() || !chatContext(m_chatEpoch, m_selection)
        || !m_chatAuthority.ready || m_chatAuthority.epoch != m_chatEpoch
        || m_chatAuthority.owner != ownerKey() || m_chatAuthority.stream != m_stream
        || fact.deleted || fact.stream != m_stream) return false;
    const auto role = m_chatAuthority.roles.value(m_session->user());
    const bool manager = m_chatAuthority.issuerOwner == m_session->user()
        || role == "owner" || role == "administrator";
    const bool participant = m_chatAuthority.issuerOwner == m_session->user()
        || m_chatAuthority.roles.contains(m_session->user());
    const bool holder = participant && m_chatIssueKeys.value(fact.key).state == ChatKeyAvailability::Present;
    return (manager || holder) && (m_chatAuthority.issuerOwner == fact.author || m_chatAuthority.roles.contains(fact.author));
}

bool NativeController::chatRequestMatches(const ChatRequestFact &fact, const ChatKeyResponse &intent) const
{
    return !fact.deleted && fact.stream == intent.stream && fact.key == intent.key && fact.author == intent.requester
        && fact.device == intent.recipientDevice && fact.name == intent.deviceName && fact.publicKey == intent.publicKey;
}

QVariantList NativeController::chatKeyRequestsMetadata() const
{
    QVariantList rows;
    const auto stream = currentStream();
    if (!chatReadAllowed() || !stream || m_chatRequestFactsLimited) return rows;
    const bool ordinary = !stream->encrypted;
    if (ordinary && (!stream->security || stream->security->source != "native"
        || !stream->security->provider.isEmpty() || !stream->security->providerIdentity.isEmpty()
        || !chatContext(m_chatEpoch, m_selection) || !m_chatAuthority.ready
        || m_chatAuthority.epoch != m_chatEpoch || m_chatAuthority.owner != ownerKey()
        || m_chatAuthority.stream != m_stream)) return rows;
    const bool busy = m_chatBusy || (m_chatKeyResponse && m_chatKeyResponse->busy);
    const bool retained = m_chatKeyResponse && m_chatKeyResponse->owner == ownerKey() && m_chatKeyResponse->phase != "ready";
    const bool manager = chatIssuerAllowed();
    const auto localDevice = m_chatKeys->device().id;
    QStringList ids = m_chatRequestFacts.keys(); std::sort(ids.begin(), ids.end());
    for (const auto &id : ids) {
        const auto &fact = m_chatRequestFacts[id];
        if (fact.deleted || fact.stream != m_stream || !isUuid(fact.key) || !isUuid(fact.author)) continue;
        const auto publicKeySize = strictChatBase64(fact.publicKey, 2048).size();
        const bool valid = !fact.device.isEmpty() && publicKeySize > 0;
        const auto key = m_chatIssueKeys.value(fact.key);
        if (ordinary && !m_chatIssueKeys.contains(fact.key)) continue; // a real bounded generation read has completed
        const bool participant = m_chatAuthority.issuerOwner == fact.author || m_chatAuthority.roles.contains(fact.author);
        const auto selfRole = m_chatAuthority.roles.value(m_session->user());
        const bool ordinaryEligible = ordinary && participant
            && (m_chatAuthority.issuerOwner == m_session->user() || selfRole == "owner" || selfRole == "administrator"
                || key.state == ChatKeyAvailability::Present);
        const bool responseAllowed = chatResponseAllowed(fact);
        const bool pending = m_chatKeyResponse && m_chatKeyResponse->request == id && m_chatKeyResponse->busy;
        const auto status = fact.outcome.isEmpty() ? QStringLiteral("pending") : fact.outcome;
        rows.append(QVariantMap{{"id", id}, {"authorId", fact.author},
            {"name", userDisplayName(fact.author, QStringLiteral("#") + fact.author.left(8))}, {"deviceName", fact.name},
            {"keyUuid", fact.key}, {"status", status}, {"pending", pending},
            {"ownCurrentDevice", fact.author == m_session->user() && fact.device == localDevice},
            {"canManageKeys", ordinary ? ordinaryEligible : manager}, {"hasRequestedKey", key.state == ChatKeyAvailability::Present},
            {"offerGrant", responseAllowed && valid && publicKeySize == 32 && status == "pending" && key.state == ChatKeyAvailability::Present},
            {"offerReject", responseAllowed && valid && status == "pending"},
            {"canGrant", responseAllowed && !busy && !retained && valid && publicKeySize == 32 && status == "pending" && key.state == ChatKeyAvailability::Present},
            {"canReject", responseAllowed && !busy && !retained && valid && status == "pending"},
            {"error", m_chatKeyResponse && m_chatKeyResponse->request == id ? m_chatKeyResponse->error : QString{}}});
    }
    return rows;
}

QVariantMap NativeController::chatKeyResponseMetadata() const
{
    if (!m_chatKeyResponse || m_chatKeyResponse->owner != ownerKey() || m_chatKeyResponse->stream != m_stream) return {};
    const auto &intent = *m_chatKeyResponse;
    const bool check = !intent.busy && !m_chatBusy && intent.phase == "uncertain" && chatKeyResponseOwned(false);
    return {{"requestId", intent.request}, {"action", intent.action}, {"responseId", intent.publicId},
        {"phase", intent.phase}, {"busy", intent.busy}, {"error", intent.error},
        {"canCheck", check}, {"canRetry", check && chatKeyResponseOwned(true)}};
}

void NativeController::rememberChatOutcomeOrder(const QVector<Message> &rows)
{
    const auto *stream = currentStream();
    if (!authenticated() || ownerKey().isEmpty() || !stream || !stream->security
        || stream->security->viewer != m_session->user() || stream->security->source != "native"
        || !stream->security->provider.isEmpty() || !stream->security->providerIdentity.isEmpty()
        || (m_chatAccessKnown && !m_chatReadableStreams.contains(m_stream)) || m_chatRequestFactsLimited) return;
    QSet<QString> known;
    for (const auto &fact : std::as_const(m_chatOutcomeFacts)) known.insert(fact.id);
    for (const auto &row : rows) {
        if (!row.crypto || row.project != m_session->project() || row.stream != m_stream
            || (row.kind != "key_grant" && row.kind != "key_reject") || known.contains(row.uuid)) continue;
        // Before display merge keep only public UUID/stream order. No payload
        // parsing/decryption/Vault/reduction is added to a history callback.
        ChatOutcomeFact fact; fact.id = row.uuid; fact.stream = row.stream;
        fact.units = fact.id.size() + fact.stream.size();
        if (m_chatRequestFacts.size() + m_chatOutcomeFacts.size() >= 500
            || m_chatRequestFactUnits + fact.units > 2000000) {
            m_chatRequestFactsLimited = true; m_chatIssueChanged = true; return;
        }
        known.insert(fact.id); m_chatRequestFactUnits += fact.units;
        m_chatOutcomeFacts.append(std::move(fact));
    }
}

void NativeController::observeChatKeyOutcome(const Message &message)
{
    if (message.project != m_session->project() || !chatReadAllowed()) return;
    auto previous = std::find_if(m_chatOutcomeFacts.begin(), m_chatOutcomeFacts.end(),
        [&](const ChatOutcomeFact &fact) { return fact.id == message.uuid; });
    const bool response = message.crypto && (message.kind == "key_grant" || message.kind == "key_reject");
    // Only a known UUID may replace its correlation after moving out/becoming
    // another kind. A deleted observation cannot be resurrected by late replay.
    if (previous == m_chatOutcomeFacts.end() && (!response || message.stream != m_stream)) return;
    if (previous != m_chatOutcomeFacts.end() && previous->deleted) return;
    ChatOutcomeFact fact;
    fact.id = message.uuid; fact.stream = message.stream; fact.author = message.author;
    if (response) {
        const auto &payload = *message.crypto;
        fact.request = payload.requestMessage; fact.key = payload.keyUuid; fact.device = payload.recipientDevice;
        fact.outcome = message.kind == "key_grant" ? QStringLiteral("granted") : QStringLiteral("rejected");
        fact.valid = message.kind == "key_reject" || !strictChatBase64(payload.encodedContent, 40000).isEmpty();
    }
    fact.units = fact.id.size() + fact.stream.size() + fact.author.size() + fact.request.size()
        + fact.key.size() + fact.device.size() + fact.outcome.size();
    if (previous != m_chatOutcomeFacts.end() && previous->stream == fact.stream && previous->author == fact.author
        && previous->request == fact.request && previous->key == fact.key && previous->device == fact.device
        && previous->outcome == fact.outcome && previous->valid == fact.valid) return;
    const auto oldUnits = previous == m_chatOutcomeFacts.end() ? 0 : previous->units;
    if ((previous == m_chatOutcomeFacts.end() && m_chatRequestFacts.size() + m_chatOutcomeFacts.size() >= 500)
        || m_chatRequestFactUnits - oldUnits + fact.units > 2000000) {
        m_chatRequestFactsLimited = true; m_chatIssueChanged = true; return;
    }
    m_chatRequestFactUnits += fact.units - oldUnits;
    if (previous == m_chatOutcomeFacts.end()) m_chatOutcomeFacts.append(std::move(fact));
    else *previous = std::move(fact); // UUID replacement retains its first-observation position.
    reduceChatKeyOutcomes();
}

void NativeController::reduceChatKeyOutcomes()
{
    // Cached public metadata only. No Vault/network/getter work or nested
    // request-by-response scan: both observations together are bounded to500.
    for (auto &request : m_chatRequestFacts) {
        const auto units = request.outcome.size() + request.response.size();
        m_chatRequestFactUnits -= units; request.units -= units;
        request.outcome.clear(); request.response.clear();
    }
    if (!m_chatRequestFactsLimited && m_chatAuthority.ready && m_chatAuthority.epoch == m_chatEpoch
        && chatContext(m_chatEpoch, m_selection)) {
        for (const auto &fact : std::as_const(m_chatOutcomeFacts)) {
            if (fact.deleted || !fact.valid || fact.stream != m_stream) continue;
            auto request = m_chatRequestFacts.find(fact.request);
            const auto role = m_chatAuthority.roles.value(fact.author);
            if (request == m_chatRequestFacts.end() || request->deleted || !request->outcome.isEmpty()
                || request->stream != fact.stream || request->key != fact.key || request->device != fact.device
                || (currentStream() && currentStream()->encrypted
                    ? fact.author != m_chatAuthority.issuerOwner && role != "owner" && role != "administrator"
                    : (fact.author != m_chatAuthority.issuerOwner && !m_chatAuthority.roles.contains(fact.author))
                        || (request->author != m_chatAuthority.issuerOwner && !m_chatAuthority.roles.contains(request->author)))) continue;
            const auto units = fact.outcome.size() + fact.id.size();
            if (m_chatRequestFactUnits + units > 2000000) { m_chatRequestFactsLimited = true; break; }
            request->outcome = fact.outcome; request->response = fact.id; request->units += units;
            m_chatRequestFactUnits += units;
        }
    }
    m_chatIssueChanged = true;
    interruptChatKeyResponse();
}

bool NativeController::chatKeyResponseOwned(bool writing) const
{
    if (!m_chatKeyResponse) return false;
    const auto &intent = *m_chatKeyResponse;
    const auto stream = std::find_if(m_streams.cbegin(), m_streams.cend(),
        [&](const Stream &row) { return row.uuid == intent.stream; });
    if (!authenticated() || intent.owner != ownerKey() || intent.project != m_session->project()
        || intent.user != m_session->user() || intent.stream != m_stream || intent.deleted
        || (m_chatAccessKnown && !m_chatReadableStreams.contains(intent.stream))
        || !m_catalogVerified || m_snapshotActive || m_syncBlocked || stream == m_streams.cend()
        || (!stream->encrypted && intent.action != "reject" && intent.action != "grant") || !stream->security || stream->security->viewer != intent.user
        || stream->security->source != "native" || !stream->security->provider.isEmpty()
        || !stream->security->providerIdentity.isEmpty()
        || !QStringList{"guest", "member", "moderator", "administrator", "owner"}.contains(stream->role)) return false;
    // GET status does not read or restore the group key, device or request pair.
    if (!writing) return true;
    if (intent.materialLost || !intent.vault || intent.vault != m_chatKeys
        || !sameChatLease(intent.vaultLease, m_chatKeys->lease()) || m_chatRequestFactsLimited
        || !m_chatAuthority.topics.contains(intent.topic)) return false;
    if (m_topicsVerified && std::none_of(m_topics.cbegin(), m_topics.cend(),
        [&](const Topic &topic) { return topic.uuid == intent.topic; })) return false;
    const auto fact = m_chatRequestFacts.constFind(intent.request);
    if (fact == m_chatRequestFacts.cend() || !chatRequestMatches(*fact, intent)
        || (!fact->outcome.isEmpty() && fact->response != intent.publicId)
        || !chatResponseAllowed(*fact)) return false;
    if (intent.action == "grant") {
        const auto key = m_chatIssueKeys.constFind(intent.key);
        if (key == m_chatIssueKeys.cend() || key->state != ChatKeyAvailability::Present || key->deletion != intent.deletion) return false;
    }
    return true;
}

bool NativeController::chatKeyResponseContext(const QString &id, quint64 ticket) const
{
    if (!m_chatKeyResponse || m_chatKeyResponse->uuid != id || m_chatKeyResponse->ticket != ticket
        || !chatKeyResponseOwned(false)) return false;
    const auto &intent = *m_chatKeyResponse;
    if (intent.generation != m_api->generation() || intent.catalog != m_catalog || intent.selection != m_selection
        || intent.routeTopic != m_topic || intent.access != m_chatAccessRevision
        || intent.cache != m_cache || !sameChatCache(intent.cacheLease, m_cacheLease)
        || (intent.cache && !sameChatCache(intent.cache->lease(), intent.cacheLease))
        || intent.security != chatStreamSecurity(currentStream())) return false;
    if (intent.statusOnly) return true;
    if (m_topicsVerified && std::none_of(m_topics.cbegin(), m_topics.cend(),
        [&](const Topic &topic) { return topic.uuid == intent.topic; })) return false;
    const auto fact = m_chatRequestFacts.constFind(intent.request);
    return chatContext(intent.epoch, intent.selection) && !intent.materialLost && intent.vault == m_chatKeys
        && intent.vault && sameChatLease(intent.vaultLease, m_chatKeys->lease())
        && fact != m_chatRequestFacts.cend() && fact->revision == intent.requestRevision && chatRequestMatches(*fact, intent)
        && (fact->outcome.isEmpty() || fact->response == intent.publicId);
}

void NativeController::captureChatKeyResponse(bool statusOnly)
{
    auto &intent = *m_chatKeyResponse;
    intent.ticket = ++m_nextChatKeyResponse; intent.busy = true; intent.statusOnly = statusOnly;
    intent.epoch = m_chatEpoch; intent.selection = m_selection; intent.catalog = m_catalog;
    intent.generation = m_api->generation(); intent.access = m_chatAccessRevision;
    intent.cache = m_cache; intent.cacheLease = m_cacheLease; intent.routeTopic = m_topic;
    intent.security = chatStreamSecurity(currentStream()); intent.error.clear();
    if (!statusOnly) intent.requestRevision = m_chatRequestFacts.value(intent.request).revision;
}

void NativeController::interruptChatKeyResponse()
{
    if (!m_chatKeyResponse) return;
    auto &intent = *m_chatKeyResponse;
    if (intent.owner != ownerKey()) {
        if (intent.holdsField) finishMessageIntent(intent.publicId);
        m_chatKeyResponse.reset(); return;
    }
    if (!intent.busy || chatKeyResponseContext(intent.uuid, intent.ticket)) return;
    intent.busy = false; intent.phase = "uncertain"; ++intent.ticket;
    intent.error = tr("The key response was interrupted. Check its saved status before retrying.");
}

void NativeController::grantChatKey(const QString &request) { startChatKeyResponse(request, true); }
void NativeController::rejectChatKey(const QString &request) { startChatKeyResponse(request, false); }

void NativeController::startChatKeyResponse(const QString &request, bool grant)
{
    const QPointer<NativeController> lookupGuard(this);
    checkChatLifetime();
    if (!lookupGuard) return;
    if (m_chatBusy || (m_chatKeyResponse && m_chatKeyResponse->phase != "ready")
        || m_chatRequestFactsLimited) return;
    const auto found = m_chatRequestFacts.constFind(request);
    if (found == m_chatRequestFacts.cend() || found->deleted || found->stream != m_stream || !found->outcome.isEmpty()
        || !isUuid(found->key) || found->device.isEmpty() || !isUuid(found->author)
        || !chatResponseAllowed(*found)) return;
    const auto publicKey = strictChatBase64(found->publicKey, 2048);
    if (publicKey.isEmpty() || (grant && publicKey.size() != 32) || !isUuid(m_chatAuthority.defaultTopic)) return;
    const auto fact = *found;
    ChatKeyResponse intent;
    intent.uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    intent.owner = ownerKey(); intent.project = m_session->project(); intent.user = m_session->user();
    intent.request = request; intent.requester = fact.author; intent.deviceName = fact.name; intent.publicKey = fact.publicKey;
    intent.stream = fact.stream; intent.key = fact.key; intent.topic = m_chatAuthority.defaultTopic;
    intent.currentKey = m_chatAuthority.currentKey; intent.recipientDevice = fact.device;
    intent.senderDevice = m_chatKeys->device().id; intent.action = grant ? QStringLiteral("grant") : QStringLiteral("reject");
    intent.vault = m_chatKeys; intent.vaultLease = m_chatKeys->lease(); intent.phase = "sending";
    intent.publicId = QUuid::createUuidV5(QUuid(intent.topic), intent.uuid.toUtf8()).toString(QUuid::WithoutBraces);
    QJsonObject payload{{"kind", grant ? "key_grant" : "key_reject"}, {"key_uuid", intent.key},
        {"request_message_uuid", request}, {"sender_device_id", intent.senderDevice}, {"recipient_device_id", intent.recipientDevice}};
    if (grant) {
        auto key = m_chatKeys->read(intent.vaultLease, {intent.stream, intent.key});
        const auto scrub = qScopeGuard([&] { wipeChatBytes(key.value.bytes); });
        if (key.failure != NativeChatKeyVault::Failure::None || key.value.bytes.size() != 32) return;
        intent.deletion = key.value.deletionRevision;
        auto envelope = NativeChatCrypto::encryptGroupKeyGrant(key.value.bytes, publicKey,
            {intent.stream, intent.key, request, intent.senderDevice, intent.recipientDevice});
        const auto scrubEnvelope = qScopeGuard([&] { wipeChatBytes(envelope.bytes); });
        if (!envelope || envelope.bytes.toBase64().size() > 40000) {
            report(tr("The requested key could not be encrypted for this device.")); return;
        }
        payload["content"] = QString::fromLatin1(envelope.bytes.toBase64());
    }
    intent.payload = payload;
    m_chatKeyResponse = std::move(intent); captureChatKeyResponse(false);
    auto &saved = *m_chatKeyResponse;
    if (!chatKeyResponseOwned(true)) { m_chatKeyResponse.reset(); return; }
    saved.holdsField = true; ++m_messageFields[saved.publicId].pending;
    const auto id = saved.uuid; const auto ticket = saved.ticket;
    const QPointer<NativeController> guard(this); emit stateChanged();
    if (!guard || !chatKeyResponseContext(id, ticket)) return;
    emit messagesChanged();
    if (guard && chatKeyResponseContext(id, ticket)) postChatKeyResponse(id, ticket);
}

void NativeController::finishChatKeyResponse(const QString &id, quint64 ticket, const QString &error)
{
    if (!m_chatKeyResponse || m_chatKeyResponse->uuid != id || m_chatKeyResponse->ticket != ticket) return;
    m_chatKeyResponse->busy = false; m_chatKeyResponse->phase = "uncertain"; m_chatKeyResponse->error = error;
    const auto epoch = m_chatEpoch, selection = m_selection;
    const QPointer<NativeController> guard(this);
    emit stateChanged();
    if (!guard || m_chatEpoch != epoch || m_selection != selection || !chatKeyResponseContext(id, ticket)) return;
    emit messagesChanged();
}

void NativeController::postChatKeyResponse(const QString &id, quint64 ticket)
{
    if (!chatKeyResponseContext(id, ticket)) return;
    if (!chatKeyResponseOwned(true)) {
        finishChatKeyResponse(id, ticket, tr("The key response was interrupted. Check its saved status before retrying."));
        return;
    }
    const auto intent = *m_chatKeyResponse;
    if (!intent.dispatched && m_topicsVerified) {
        QString target; int defaults = 0;
        for (const auto &topic : m_topics) if (topic.isDefault) { target = topic.uuid; ++defaults; }
        if (defaults != 1 || target != intent.topic) {
            finishChatKeyResponse(id, ticket, tr("The default topic changed. Check the saved key response.")); return;
        }
    }
    if (m_chatAuthority.currentKey != intent.currentKey) {
        finishChatKeyResponse(id, ticket, tr("Channel encryption changed. Check the saved key response.")); return;
    }
    if (intent.action == "grant") {
        auto key = m_chatKeys->read(intent.vaultLease, {intent.stream, intent.key});
        const bool present = key.failure == NativeChatKeyVault::Failure::None && key.value.bytes.size() == 32
            && key.value.deletionRevision == intent.deletion;
        wipeChatBytes(key.value.bytes);
        if (!present) { finishChatKeyResponse(id, ticket, tr("The requested key is no longer available.")); return; }
    }
    m_chatKeyResponse->dispatched = true;
    const QJsonObject body{{"uuid", intent.uuid}, {"stream_uuid", intent.stream}, {"topic_uuid", intent.topic}, {"payload", intent.payload}};
    request("POST", messenger + "/messages/", body, {}, [this, id, ticket](const ApiResult &result) {
        if (!chatKeyResponseContext(id, ticket) || !m_chatKeyResponse->busy) return;
        if (!result.ok() || !matchesChatKeyResponse(result.data.toObject(), false)) {
            finishChatKeyResponse(id, ticket, tr("The key response was not confirmed. Check does not send it again.")); return;
        }
        readChatKeyResponse(id, ticket, false);
    });
}

bool NativeController::matchesChatKeyResponse(const QJsonObject &raw, bool realtime) const
{
    if (!m_chatKeyResponse) return false;
    const auto &intent = *m_chatKeyResponse;
    const auto parsed = parseMessage(raw, intent.project);
    return parsed && parsed->uuid == intent.publicId && parsed->stream == intent.stream && parsed->topic == intent.topic
        && parsed->author == intent.user && parsed->own && raw.value("payload").toObject() == intent.payload
        && (realtime || raw.value("user_uuid").toString() == intent.user);
}

bool NativeController::confirmChatKeyResponse(const QJsonObject &raw, const QString &id, quint64 ticket, bool realtime)
{
    if (!chatKeyResponseContext(id, ticket) || !matchesChatKeyResponse(raw, realtime)) return false;
    const auto intent = *m_chatKeyResponse;
    if (m_messageFields.value(intent.publicId).deleted) return false;
    auto message = parseMessage(raw, intent.project);
    if (!message) return false;
    m_chatKeyResponse->busy = false; m_chatKeyResponse->phase = "ready"; m_chatKeyResponse->error.clear();
    m_chatKeyResponse->holdsField = false;
    observeChatKeyOutcome(*message);
    const bool visible = inConversation(*message);
    if (visible && !findMessage(message->uuid)) {
        observeMessage(raw); m_messages = mergeMessages(m_messages, {*message}); ++m_messageRevision;
    }
    // No pending SendIntent/draft is borrowed. Release tombstone bookkeeping
    // before any optional cache call or subscriber can reenter another action.
    if (intent.holdsField) finishMessageIntent(intent.publicId);
    const QPointer<NativeController> guard(this);
    projectCanonical(raw, NativeCache::Projection::Insert, {intent.cache, intent.cacheLease, intent.generation});
    if (!guard || !chatKeyResponseContext(id, ticket)) return true;
    if (visible) { emit messagesChanged(); if (!guard || !chatKeyResponseContext(id, ticket)) return true; }
    emit stateChanged();
    return true;
}

void NativeController::readChatKeyResponse(const QString &id, quint64 ticket, bool retry)
{
    if (!chatKeyResponseContext(id, ticket)) return;
    const auto intent = *m_chatKeyResponse;
    request("GET", messenger + "/messages/" + intent.publicId, {}, {}, [this, id, ticket, retry](const ApiResult &result) {
        if (!chatKeyResponseContext(id, ticket) || !m_chatKeyResponse->busy) return;
        if (result.ok() && confirmChatKeyResponse(result.data.toObject(), id, ticket)) return;
        if (!retry || result.status != 404) {
            finishChatKeyResponse(id, ticket, tr("The saved key response is unresolved. Check does not send it again.")); return;
        }
        // A verified404 permits only an explicit same-wire retry. Renew shared
        // authority and the actual request, retaining the original owned topic.
        m_chatAuthority.ready = false;
        ensureChatAuthority([this, id, ticket](bool okay, const QString &error) {
            if (!chatKeyResponseContext(id, ticket)) return;
            if (!okay || !chatKeyResponseOwned(true)) {
                finishChatKeyResponse(id, ticket, error.isEmpty() ? tr("Permission to retry the key response is unavailable.") : error); return;
            }
            m_chatKeyResponse->currentKey = m_chatAuthority.currentKey;
            const auto requestId = m_chatKeyResponse->request;
            request("GET", messenger + "/messages/" + requestId, {}, {}, [this, id, ticket](const ApiResult &requestResult) {
                if (!chatKeyResponseContext(id, ticket)) return;
                const auto message = parseMessage(requestResult.data.toObject(), m_session->project());
                if (!requestResult.ok() || !message || message->uuid != m_chatKeyResponse->request
                    || message->kind != "key_request" || !message->crypto || message->stream != m_chatKeyResponse->stream) {
                    finishChatKeyResponse(id, ticket, tr("The original key request could not be verified.")); return;
                }
                observeChatRequest(*message);
                if (!chatKeyResponseContext(id, ticket)) return;
                postChatKeyResponse(id, ticket);
            });
        });
    });
}

void NativeController::checkChatKeyResponse(const QString &request) { recoverChatKeyResponse(request, false); }
void NativeController::retryChatKeyResponse(const QString &request) { recoverChatKeyResponse(request, true); }

void NativeController::recoverChatKeyResponse(const QString &request, bool retry)
{
    const QPointer<NativeController> lookupGuard(this);
    checkChatLifetime();
    if (!lookupGuard) return;
    if (!m_chatKeyResponse || m_chatKeyResponse->request != request || m_chatKeyResponse->busy || m_chatBusy
        || m_chatKeyResponse->phase != "uncertain" || !chatKeyResponseOwned(retry)) return;
    captureChatKeyResponse(!retry); m_chatKeyResponse->phase = "checking";
    const auto id = m_chatKeyResponse->uuid; const auto ticket = m_chatKeyResponse->ticket;
    const QPointer<NativeController> guard(this); emit stateChanged();
    if (!guard || !chatKeyResponseContext(id, ticket)) return;
    emit messagesChanged();
    if (guard && chatKeyResponseContext(id, ticket)) readChatKeyResponse(id, ticket, retry);
}

void NativeController::scheduleChatPreparation()
{
    const QPointer<NativeController> lookupGuard(this);
    checkChatLifetime();
    if (!lookupGuard) return;
    if (m_chatPrepareQueued) return;
    m_chatPrepareQueued = true;
    QTimer::singleShot(0, this, [this] {
        const QPointer<NativeController> lookupGuard(this);
        m_chatPrepareQueued = false; checkChatLifetime();
        if (!lookupGuard) return;
        if (!chatReadAllowed() || !currentStream() || !currentStream()->encrypted) return;
        if (m_chatCurrentKeyState == ChatKeyAvailability::Unknown && currentStream()->security
            && isUuid(currentStream()->security->currentKey)) {
            const auto epoch = m_chatEpoch, selection = m_selection;
            auto key = m_chatKeys->read(m_chatVaultLease, {m_stream, currentStream()->security->currentKey});
            m_chatCurrentKeyState = key.failure != NativeChatKeyVault::Failure::None ? ChatKeyAvailability::Error
                : key.value.bytes.isEmpty() ? ChatKeyAvailability::Missing
                : key.value.bytes.size() == 32 ? ChatKeyAvailability::Present : ChatKeyAvailability::Error;
            const auto previousSelection = m_chatSendKey;
            const auto previousDeletion = m_chatSendDeletion;
            m_chatSendKey.clear(); m_chatSendDeletion = 0;
            if (m_chatCurrentKeyState == ChatKeyAvailability::Present) {
                m_chatSendKey = currentStream()->security->currentKey;
                m_chatSendDeletion = key.value.deletionRevision;
            } else if (m_chatCurrentKeyState == ChatKeyAvailability::Missing) {
                auto previous = m_chatKeys->lastAvailableKey(m_chatVaultLease, m_stream);
                if (previous.failure == NativeChatKeyVault::Failure::None && previous.value.bytes.size() == 32) {
                    m_chatSendKey = previous.key; m_chatSendDeletion = previous.value.deletionRevision;
                }
                wipeChatBytes(previous.value.bytes);
            }
            wipeChatBytes(key.value.bytes);
            if (!m_chatSendKey.isEmpty() && (m_chatSendKey != previousSelection || m_chatSendDeletion != previousDeletion)
                && m_chatKeys->markForSending(m_chatVaultLease, {m_stream, m_chatSendKey}, m_chatSendDeletion)
                    != NativeChatKeyVault::Failure::None) {
                m_chatSendKey.clear(); m_chatSendDeletion = 0;
            }
            const QPointer<NativeController> guard(this);
            emit stateChanged();
            if (!guard || m_chatEpoch != epoch || m_selection != selection || !chatReadAllowed()) return;
        }
        if (!m_chatPreparingWindow.isEmpty()) {
            qsizetype index = 0;
            bool same = true;
            for (const auto &row : m_messages) {
                if (row.kind != "e2ee" || !row.crypto) continue;
                if (index >= m_chatPrepareRows.size()) { same = false; break; }
                const auto &previous = m_chatPrepareRows[index++];
                if (!previous.crypto || row.uuid != previous.uuid || row.stream != previous.stream
                    || row.topic != previous.topic || row.crypto->keyUuid != previous.crypto->keyUuid
                    || row.crypto->encodedContent != previous.crypto->encodedContent) { same = false; break; }
            }
            // Progress notifications do not change the encrypted source window.
            // A cleared fingerprint still forces material/lifetime preparation.
            if (same && index == m_chatPrepareRows.size()) return;
        }
        m_chatPrepareRows.clear(); QSet<QString> active;
        QCryptographicHash fingerprint(QCryptographicHash::Sha256);
        for (const auto &row : m_messages) {
            if (row.kind != "e2ee" || !row.crypto) continue;
            fingerprint.addData(chatPayloadHash(row));
            active.insert(row.uuid); m_chatPrepareRows.append(row);
            if (m_chatPrepareRows.size() > 500) break;
        }
        const auto window = fingerprint.result();
        if (window == m_chatPreparingWindow) return;
        m_chatPreparingWindow = window;
        if (m_chatPreparingWindow == m_chatRejectedWindow) return;
        if (m_chatPrepareRows.size() > 500) {
            m_chatRejectedWindow = m_chatPreparingWindow;
            const bool changed = !m_chatPrepared.isEmpty();
            m_chatPrepared.clear(); m_chatPreparedUnits = 0; m_chatPrepareRows.clear(); ++m_chatPreparation;
            if (changed) emit messagesChanged();
            return;
        }
        for (auto it = m_chatPrepared.begin(); it != m_chatPrepared.end();) {
            if (!active.contains(it.key())) { m_chatPreparedUnits -= it.key().size() + it->content.size(); it = m_chatPrepared.erase(it); }
            else ++it;
        }
        prepareChatSlice(m_chatEpoch, m_selection, ++m_chatPreparation, 0);
    });
}

void NativeController::prepareChatSlice(quint64 epoch, quint64 selection, quint64 preparation, qsizetype offset)
{
    // Local exact-generation keys may prepare owned cached ciphertext while
    // offline. Live authority is deliberately not fetched by this read path.
    if (m_chatEpoch != epoch || m_selection != selection || m_chatPreparation != preparation || !chatReadAllowed()
        || !sameChatLease(m_chatKeys->lease(), m_chatVaultLease)) return;
    bool changed = false;
    QElapsedTimer work; work.start();
    const auto limit = std::min(offset + 4, m_chatPrepareRows.size());
    auto end = offset;
    // Leave room for synchronous model work and queued UI events. The budget
    // is checked between rows; an individual crypto/SQL call is not preempted.
    for (; end < limit && (end == offset || work.elapsed() < 4); ++end) {
        const auto &message = m_chatPrepareRows[end]; const auto hash = chatPayloadHash(message);
        const NativeChatKeyVault::Address address{message.stream, message.crypto->keyUuid};
        auto key = m_chatKeys->read(m_chatVaultLease, address);
        const auto old = m_chatPrepared.constFind(message.uuid);
        if (old != m_chatPrepared.cend() && old->state == ChatKeyAvailability::Present
            && old->hash == hash && old->deletion == key.value.deletionRevision
            && key.failure == NativeChatKeyVault::Failure::None && key.value.bytes.size() == 32) { wipeChatBytes(key.value.bytes); continue; }
        auto previous = m_chatPrepared.find(message.uuid);
        if (previous != m_chatPrepared.end()) {
            m_chatPreparedUnits -= previous.key().size() + previous->content.size(); m_chatPrepared.erase(previous); changed = true;
        }
        auto state = key.failure == NativeChatKeyVault::Failure::None && key.value.bytes.isEmpty()
            ? ChatKeyAvailability::Missing : ChatKeyAvailability::Error;
        QString text;
        const auto envelope = strictChatBase64(message.crypto->encodedContent, 213336);
        if (key.failure == NativeChatKeyVault::Failure::None && key.value.bytes.size() == 32 && !envelope.isEmpty()) {
            auto plaintext = NativeChatCrypto::decryptMessage(key.value.bytes, envelope, {message.stream, message.crypto->keyUuid});
            if (plaintext) {
                text = QString::fromUtf8(plaintext.bytes);
                if (text.toUtf8() == plaintext.bytes) state = ChatKeyAvailability::Present;
                else text.clear();
            }
            wipeChatBytes(plaintext.bytes);
        }
        wipeChatBytes(key.value.bytes);
        if (text.size() + message.uuid.size() > 2000000 - m_chatPreparedUnits) {
            m_chatRejectedWindow = m_chatPreparingWindow;
            m_chatPrepared.clear(); m_chatPreparedUnits = 0; m_chatPrepareRows.clear(); changed = true;
            break;
        }
        m_chatPrepared.insert(message.uuid, {hash, text, key.value.deletionRevision, state});
        m_chatPreparedUnits += text.size() + message.uuid.size(); changed = true;
    }
    const QPointer<NativeController> guard(this);
    if (changed) emit messagesChanged();
    if (!guard || m_chatEpoch != epoch || m_selection != selection || m_chatPreparation != preparation) return;
    if (end < m_chatPrepareRows.size()) QTimer::singleShot(0, this, [this, epoch, selection, preparation, end] {
        prepareChatSlice(epoch, selection, preparation, end);
    });
}

namespace {
bool boundedText(const QString &text, int maximum)
{
    if (text.size() > maximum * 2) return false;
    int points = 0;
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text[i].isHighSurrogate()) {
            if (++i == text.size() || !text[i].isLowSurrogate()) return false;
        } else if (text[i].isLowSurrogate()) return false;
        if (++points > maximum) return false;
    }
    return true;
}

QString directoryViewerName(const QJsonObject &row)
{
    QStringList parts;
    for (const auto *field : {"first_name", "last_name"}) {
        const auto value = row.value(field);
        if (!value.isUndefined() && !value.isNull() && (!value.isString() || !boundedText(value.toString(), 128))) return {};
        const auto part = sourceTrimmed(value.toString());
        if (!part.isEmpty()) parts.append(part);
    }
    if (!parts.isEmpty()) return parts.join(' ');
    const auto username = row.value("username");
    return username.isString() && boundedText(username.toString(), 128) ? sourceTrimmed(username.toString()) : QString{};
}

QString streamAcl(const Stream &stream)
{
    return QString::fromUtf8(QJsonDocument(QJsonArray{stream.role, stream.directUser, stream.encrypted, stream.archived, stream.privateStream})
        .toJson(QJsonDocument::Compact));
}

QString topicOpeningSecurity(const Stream &stream)
{
    QJsonArray fields{streamAcl(stream)};
    if (stream.security) {
        const auto &security = *stream.security;
        fields.append(QJsonArray{security.owner, security.viewer, security.source, security.currentKey,
            security.defaultTopic, security.provider, security.providerIdentity});
    }
    return QString::fromUtf8(QJsonDocument(fields).toJson(QJsonDocument::Compact));
}

std::optional<Topic> creationTopicDto(const QJsonObject &raw, const QString &project,
                                     const QString &stream, const QString &viewer)
{
    const auto parsed = parseTopic(raw, project, stream);
    if (!parsed || raw.value("user_uuid").toString() != viewer || !boundedText(parsed->name, 128)
        || !raw.value("is_done").isBool() || !raw.value("is_default").isBool()) return {};
    for (const auto *field : {"unread_count", "active_unread_count", "passive_unread_count"})
        if (!raw.value(field).isDouble()) return {};
    return parsed;
}

bool validCreationDate(const QJsonValue &value)
{
    return value.isString() && QDateTime::fromString(value.toString(), Qt::ISODateWithMs).isValid();
}

bool nullableCreationUuid(const QJsonValue &value)
{
    return value.isUndefined() || value.isNull() || (value.isString() && isUuid(value.toString()));
}

const QStringList channelRoles{"guest", "member", "moderator", "administrator", "owner"};
const QStringList channelModes{"all_messages", "mentions_only", "muted"};

std::optional<Stream> creationStreamDto(const QJsonObject &raw, const QString &project,
                                      const QString &viewer, bool ordinary)
{
    const auto parsed = parseStream(raw, project);
    if (!parsed || raw.value("user_uuid").toString() != viewer || !isUuid(raw.value("owner").toString())
        || !boundedText(parsed->name, 255) || sourceTrimmed(parsed->name).isEmpty()
        || !raw.value("description").isString() || !boundedText(raw.value("description").toString(), 10000)
        || !channelRoles.contains(parsed->role) || !channelModes.contains(parsed->notificationMode)
        || !QStringList{"native", "zulip"}.contains(raw.value("source_name").toString())
        || !validCreationDate(raw.value("created_at")) || !validCreationDate(raw.value("updated_at"))) return {};
    for (const auto *field : {"invite_only", "announce", "private", "is_archived", "encryption"})
        if (!raw.value(field).isBool()) return {};
    for (const auto *field : {"unread_count", "active_unread_count", "passive_unread_count"})
        if (!raw.value(field).isDouble()) return {};
    for (const auto *field : {"provider_uuid", "direct_user_uuid", "current_encryption_key_uuid", "default_topic_uuid"})
        if (!nullableCreationUuid(raw.value(field))) return {};
    const auto provider = raw.value("provider_identity");
    if (!provider.isNull() && !provider.isUndefined()
        && (!provider.isString() || provider.toString().isEmpty() || !boundedText(provider.toString(), 255))) return {};
    if (ordinary && (raw.value("owner").toString() != viewer || parsed->role != "owner"
        || raw.value("source_name").toString() != "native" || !raw.value("provider_uuid").toString().isEmpty()
        || !provider.toString().isEmpty() || raw.value("private").toBool() || !parsed->directUser.isEmpty()
        || parsed->encrypted || !raw.value("current_encryption_key_uuid").toString().isEmpty()
        || parsed->archived || !isUuid(raw.value("default_topic_uuid").toString()))) return {};
    return parsed;
}

bool creationBindings(const QJsonArray &rows, const QString &project, const QString &stream,
                      QSet<QString> *users)
{
    QSet<QString> ids;
    for (const auto &value : rows) {
        if (!value.isObject()) return false;
        const auto raw = value.toObject();
        const auto id = raw.value("uuid").toString().toLower();
        const auto user = raw.value("user_uuid").toString().toLower();
        if (!isUuid(id) || ids.contains(id) || !isUuid(user) || users->contains(user)
            || raw.value("project_id").toString() != project || raw.value("stream_uuid").toString() != stream
            || !isUuid(raw.value("who_uuid").toString()) || !channelRoles.contains(raw.value("role").toString())
            || !channelModes.contains(raw.value("notification_mode").toString())
            || !validCreationDate(raw.value("created_at")) || !validCreationDate(raw.value("updated_at"))) return false;
        ids.insert(id); users->insert(user);
    }
    return true;
}

std::optional<QVariantMap> channelDirectoryChoice(const QJsonObject &raw, const QString &project)
{
    const auto id = raw.value("uuid").toString().toLower();
    const auto username = raw.value("username");
    if (!isUuid(id) || !username.isString() || sourceTrimmed(username.toString()).isEmpty()
        || !boundedText(username.toString(), 128)
        || (raw.contains("project_id") && raw.value("project_id").toString() != project)) return {};
    for (const auto *field : {"first_name", "last_name", "email"}) {
        const auto value = raw.value(field);
        if (!value.isUndefined() && !value.isNull()
            && (!value.isString() || !boundedText(value.toString(), QString::fromLatin1(field) == "email" ? 256 : 128))) return {};
    }
    return QVariantMap{{"id", id}, {"displayName", directoryViewerName(raw)},
        {"username", sourceTrimmed(username.toString())}, {"email", sourceTrimmed(raw.value("email").toString())}};
}

QString canonicalDirectUuid(const QString &project, const QString &viewer, const QString &peer)
{
    if (!isUuid(project) || !isUuid(viewer) || !isUuid(peer)) return {};
    QStringList pair{viewer.toLower(), peer.toLower()};
    pair.sort();
    return QUuid::createUuidV5(QUuid("26f42131-4503-5f59-8aa6-32b20f551751"),
        (project.toLower() + ":" + pair.join(':')).toUtf8()).toString(QUuid::WithoutBraces);
}

std::optional<Stream> directStreamDto(const QJsonObject &raw, const QString &project,
                                      const QString &viewer, const QString &peer)
{
    const auto parsed = creationStreamDto(raw, project, viewer, false);
    if (!parsed || parsed->uuid != canonicalDirectUuid(project, viewer, peer)
        || !parsed->privateStream || parsed->directUser != peer || parsed->role != "owner"
        || (raw.value("owner").toString() != viewer && raw.value("owner").toString() != peer)
        || raw.value("source_name").toString() != "native" || !raw.value("provider_uuid").toString().isEmpty()
        || !raw.value("provider_identity").toString().isEmpty()
        || (!parsed->encrypted && isUuid(raw.value("current_encryption_key_uuid").toString()))) return {};
    return parsed;
}
}

QString NativeController::creationOwnerKey() const
{
    if (!authenticated()) return {};
    // ApiClient has already validated the exact origin. Keeping this identity
    // separate from the persisted HTTPS vault also supports loopback fixtures.
    const auto identity = QJsonDocument(QJsonArray{m_api->server().toString(QUrl::FullyEncoded),
        m_session->project(), m_session->user()}).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
}

QString NativeController::userDisplayName(const QString &id, const QString &fallback) const
{
    const auto found = m_names.constFind(id);
    return found == m_names.cend() ? fallback : found->name;
}

NativeController::UserPresentation NativeController::makeUserPresentation(
    const QString &name, const QJsonObject &row, bool authoritative,
    const UserPresentation &previous)
{
    UserPresentation result{name, imageAvatarReference(row.value("avatar")), {}, {}, {},
        sourceTrimmed(row.value("email").toString()), {}, {}, {}};
    if (!authoritative) {
        // Source persists profiles, never live presence/status decorations.
        result.presence = "offline";
        result.presenceUpdatedAt = 0;
        return result;
    }
    result.status = row.value("status").toString();
    result.statusText = sourceTrimmed(row.value("status_text").toString());
    result.statusEmoji = sourceTrimmed(row.value("status_emoji").toString());
    qint64 updatedAt = 0; // Source user.model getTimestamp: invalid/missing date is zero.
    if (row.value("updated_at").isString()) {
        const auto date = QDateTime::fromString(row.value("updated_at").toString(), Qt::ISODateWithMs);
        if (date.isValid()) updatedAt = date.toMSecsSinceEpoch();
    }
    // Only the added presence facts have this ordering witness. Existing name
    // and avatar replacement semantics deliberately remain independent.
    if (previous.presenceUpdatedAt && updatedAt < *previous.presenceUpdatedAt) {
        result.presence = previous.presence;
        result.statusLabel = previous.statusLabel;
        result.presenceUpdatedAt = previous.presenceUpdatedAt;
        result.status = previous.status;
        result.statusText = previous.statusText;
        result.statusEmoji = previous.statusEmoji;
        return result;
    }
    result.presenceUpdatedAt = updatedAt;
    const auto status = row.value("status").toString();
    if (status == "active" || status == "offline") result.presence = status;
    else if (status == "idle" || status == "do_not_disturb") result.presence = "idle";
    // Missing/invalid optional status remains unknown, not invented online/offline.
    auto emoji = sourceTrimmed(row.value("status_emoji").toString());
    static const QHash<QString, QString> legacyEmoji{
        {"speech_balloon", QString::fromUtf8("💬")}, {"house", QString::fromUtf8("🏠")},
        {"palm_tree", QString::fromUtf8("🌴")}, {"plate_with_cutlery", QString::fromUtf8("🍽️")},
        {"helmet_with_white_cross", QString::fromUtf8("⛑️")}, {"spiral_calendar_pad", QString::fromUtf8("🗓️")}};
    const auto symbol = legacyEmoji.constFind(emoji);
    // Source JS /i without /u is ASCII-caseless; Unicode case folding must not
    // turn Kelvin sign or long-s into a shortcode. This is not a Unicode regex.
    const bool shortcode = !emoji.isEmpty() && std::all_of(emoji.cbegin(), emoji.cend(), [](QChar c) {
        const auto u = c.unicode();
        return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9')
            || u == '_' || u == '+' || u == '-';
    });
    if (symbol != legacyEmoji.cend()) emoji = *symbol;
    else if (shortcode) emoji.clear();
    const auto text = sourceTrimmed(row.value("status_text").toString());
    result.statusLabel = text.isEmpty() ? emoji : emoji.isEmpty() ? text : emoji + ' ' + text;
    return result;
}

QString NativeController::userPresence(const QString &id) const
{
    if (!authenticated() || m_channelDirectoryOwner != creationOwnerKey()) return {};
    return m_names.value(id).presence;
}

QString NativeController::userStatusLabel(const QString &id) const
{
    if (!authenticated() || m_channelDirectoryOwner != creationOwnerKey()) return {};
    return m_names.value(id).statusLabel;
}

QString NativeController::userAvatar(const QString &id) const
{
    if (!authenticated() || m_channelDirectoryOwner != creationOwnerKey()) return {};
    return m_names.value(id).avatar;
}

QVariantMap NativeController::account() const
{
    auto profile = m_session->publicProfile();
    if (!profile.isEmpty()) {
        profile.insert("avatar", userAvatar(m_session->user()));
        profile.insert("presence", userPresence(m_session->user()));
        profile.insert("statusLabel", userStatusLabel(m_session->user()));
        auto displayEmail = sourceTrimmed(profile.value("email").toString());
        if (authenticated() && m_channelDirectoryOwner == creationOwnerKey()) {
            const auto user = m_names.constFind(m_session->user());
            if (user != m_names.cend() && !user->email.isEmpty()) displayEmail = user->email;
        }
        profile.insert("displayEmail", displayEmail);
    }
    return profile;
}

void NativeController::usePreferences(NativePreferences *preferences)
{
    if (m_statusPreferences == preferences) return;
    const QPointer<NativeController> guard(this);
    interruptOwnStatusUpdate();
    if (!guard) return;
    m_statusPreferences = preferences;
    m_statusPreferenceOwner.clear();
    m_conversationPreferenceOwner.clear();
    cancelConversationRestore();
    prepareOwnStatusPreferences();
    prepareConversationRestore();
    if (!m_stream.isEmpty()) rememberConversation();
    maybeRestoreConversation();
    emit stateChanged();
}

void NativeController::cancelConversationRestore()
{
    ++m_conversationRestoreTicket;
    m_conversationRestore.clear();
    m_conversationRestoreScheduled = false;
}

void NativeController::prepareConversationRestore()
{
    const auto owner = creationOwnerKey();
    if (owner == m_conversationPreferenceOwner) return;
    cancelConversationRestore();
    m_conversationPreferenceOwner = owner;
    if (m_statusPreferences && !owner.isEmpty() && m_stream.isEmpty() && m_topic.isEmpty())
        m_conversationRestore = m_statusPreferences->loadLastConversation(owner);
}

void NativeController::rememberConversation()
{
    if (m_statusPreferences && authenticated())
        m_statusPreferences->recordLastConversation(creationOwnerKey(), m_stream, m_topic);
}

void NativeController::maybeRestoreConversation()
{
    if (!m_statusPreferences || m_conversationRestore.size() != 2 || m_conversationRestoreScheduled
        || !authenticated() || !m_catalogVerified || m_snapshotActive || m_syncBlocked || m_cacheForgettingAccount) return;
    const auto ticket = m_conversationRestoreTicket;
    const auto owner = creationOwnerKey();
    const auto generation = m_api->generation(), catalog = m_catalog, selection = m_selection;
    const QPointer<NativePreferences> preferences = m_statusPreferences;
    m_conversationRestoreScheduled = true;
    QTimer::singleShot(0, this, [this, ticket, owner, generation, catalog, selection, preferences] {
        if (!preferences || ticket != m_conversationRestoreTicket || owner != creationOwnerKey()
            || preferences != m_statusPreferences) return;
        m_conversationRestoreScheduled = false;
        if (generation != m_api->generation() || catalog != m_catalog || selection != m_selection) {
            maybeRestoreConversation();
            return;
        }
        if (!authenticated() || !m_catalogVerified || m_snapshotActive || m_syncBlocked || m_cacheForgettingAccount) return;
        const auto route = m_conversationRestore;
        cancelConversationRestore(); // One automatic attempt; explicit navigation wins thereafter.
        if (route.size() != 2 || route[0].isEmpty()) return;
        if (!route[1].isEmpty()) {
            openTopic(route[1], route[0]); // Canonical resolver owns moved/merged placement.
            return;
        }
        const auto stream = std::find_if(m_streams.cbegin(), m_streams.cend(), [&](const Stream &value) {
            return value.uuid == route[0];
        });
        if (stream != m_streams.cend()) {
            const auto id = stream->uuid;
            selectStream(id);
        }
        else report(tr("The saved conversation is no longer available."));
    });
}

void NativeController::prepareOwnStatusPreferences()
{
    const auto owner = creationOwnerKey();
    if (owner == m_statusPreferenceOwner) return;
    m_statusPreferenceOwner = owner;
    m_statusManualAway = m_statusPreferences && !owner.isEmpty()
        ? m_statusPreferences->loadOwnStatusAway(owner) : false;
}

QString NativeController::ownStatusContextKey() const
{
    if (!authenticated()) return {};
    const auto context = QJsonDocument(QJsonArray{creationOwnerKey(),
        QString::number(m_api->generation())}).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(QCryptographicHash::hash(context, QCryptographicHash::Sha256).toHex());
}

QVariantMap NativeController::ownStatus() const
{
    if (!authenticated()) return {};
    const auto owner = creationOwnerKey();
    const auto user = m_channelDirectoryOwner == owner ? m_names.value(m_session->user()) : UserPresentation{};
    const bool current = m_ownStatusOwner == owner && m_ownStatusGeneration == m_api->generation();
    return {{"text", user.statusText}, {"emoji", user.statusEmoji},
        {"away", (m_statusPreferenceOwner == owner && m_statusManualAway) || user.status == "do_not_disturb"},
        {"pending", current && m_ownStatusPending}, {"error", current ? m_ownStatusError : QString{}}};
}

void NativeController::interruptOwnStatusUpdate()
{
    const auto handle = m_ownStatusRequest;
    ++m_ownStatusTicket;
    m_ownStatusRequest = {};
    m_ownStatusPending = false;
    m_ownStatusOwner.clear();
    m_ownStatusError.clear();
    // Invalidate first: cancelRequest may call an observer synchronously.
    if (handle.isValid()) m_api->cancelRequest(handle);
}

void NativeController::cancelOwnStatusUpdate()
{
    const QPointer<NativeController> guard(this);
    interruptOwnStatusUpdate();
    if (guard) emit stateChanged();
}

void NativeController::saveOwnStatus(const QString &text, const QString &emoji, bool away)
{
    if (!authenticated() || m_ownStatusPending) return;
    prepareOwnStatusPreferences();
    const auto owner = creationOwnerKey(), user = m_session->user();
    if (owner.isEmpty() || !isUuid(user)) return;
    const auto generation = m_api->generation(), ticket = ++m_ownStatusTicket;
    const auto statusText = sourceTrimmed(text).left(256), statusEmoji = sourceTrimmed(emoji).left(64);
    m_ownStatusOwner = owner;
    m_ownStatusGeneration = generation;
    m_ownStatusError.clear();
    if (!boundedText(statusText, 256) || !boundedText(statusEmoji, 64)) {
        m_ownStatusError = tr("Failed to update status");
        emit stateChanged();
        return;
    }
    const QJsonObject body{{"status", away ? "idle" : "active"},
        {"text", statusText.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(statusText)},
        {"emoji", statusEmoji.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(statusEmoji)}};
    const QPointer<NativeController> guard(this);
    const auto current = [this, guard, owner, generation, ticket] {
        return guard && authenticated() && owner == creationOwnerKey()
            && generation == m_api->generation() && ticket == m_ownStatusTicket;
    };
    m_ownStatusPending = true;
    emit stateChanged();
    if (!current()) return;
    const auto handle = request("POST", "/api/workspace/v1/users/" + user + "/actions/presence/invoke", body, {},
        [this, current, owner, user, ticket, away](const ApiResult &result) {
        if (!current() || !m_ownStatusPending) return;
        const auto row = result.data.toObject();
        bool valid = result.ok() && result.data.isObject() && row.value("uuid").toString() == user
            && channelDirectoryChoice(row, m_session->project()).has_value()
            && QStringList{"iam", "zulip"}.contains(row.value("source").toString())
            && QStringList{"active", "idle", "offline", "do_not_disturb"}.contains(row.value("status").toString())
            && (row.value("avatar").isNull() || row.value("avatar").isUndefined() || row.value("avatar").isString());
        for (const auto *field : {"status_text", "status_emoji"}) {
            const auto value = row.value(field);
            valid = valid && (value.isNull() || value.isUndefined() || (value.isString()
                && boundedText(value.toString(), QString::fromLatin1(field) == "status_text" ? 256 : 64)));
        }
        for (const auto *field : {"last_ping_at", "created_at", "updated_at"}) valid = valid && validCreationDate(row.value(field));
        // Settle bookkeeping before cache/Qt notifications can admit a new operation.
        m_ownStatusPending = false;
        m_ownStatusRequest = {};
        if (!valid) {
            m_ownStatusError = tr("Failed to update status");
            emit stateChanged();
            return;
        }
        m_statusPreferenceOwner = owner;
        m_statusManualAway = away;
        if (m_statusPreferences) m_statusPreferences->recordOwnStatusAway(owner, away);
        const auto name = directoryViewerName(row);
        // A single owned canonical response cannot adopt somebody else's directory.
        if (m_channelDirectoryOwner != owner) {
            m_names.clear(); m_channelChoices.clear(); m_channelDirectoryValid = false;
            m_channelDirectoryOwner = owner;
        }
        m_names.insert(user, makeUserPresentation(name, row, true, m_names.value(user)));
        if (m_usersVerified) {
            m_viewerDirectoryName = name;
            m_viewerDirectoryOwner = owner;
        }
        const bool selfDirect = std::any_of(m_streams.cbegin(), m_streams.cend(), [&user](const Stream &stream) {
            return stream.privateStream && stream.directUser == user;
        });
        // HTTP response is reduced directly, never wrapped in a fabricated RT event.
        staleCache(NativeCache::Kind::Users);
        if (!current()) return;
        if (selfDirect) emit streamsChanged();
        if (!current()) return;
        emit messagesChanged();
        if (!current()) return;
        emit stateChanged();
    });
    if (!guard) return;
    if (current() && m_ownStatusPending) m_ownStatusRequest = handle;
    else m_api->cancelRequest(handle);
}

QString NativeController::viewerDisplayName() const
{
    if (!authenticated()) return {};
    if (m_usersVerified && m_viewerDirectoryOwner == creationOwnerKey() && !m_viewerDirectoryName.isEmpty())
        return m_viewerDirectoryName;
    const auto profile = m_session->publicProfile();
    for (const auto *field : {"displayName", "username", "email"}) {
        const auto value = profile.value(field).toString();
        if (boundedText(value, 257) && !sourceTrimmed(value).isEmpty()) return sourceTrimmed(value);
    }
    return {};
}

const NativeController::DirectChat *NativeController::directChat() const
{
    const auto found = m_directChats.constFind(creationOwnerKey());
    return found == m_directChats.cend() ? nullptr : &found.value();
}
QString NativeController::directChatContextKey() const
{
    if (!authenticated()) return {};
    const auto context = QJsonDocument(QJsonArray{creationOwnerKey(), QString::number(m_selection),
        QString::number(m_catalog), QString::number(m_api->generation()),
        QString::number(reinterpret_cast<quintptr>(m_cache.data())), m_cacheLease.owner,
        QString::number(m_cacheLease.generation), m_catalogVerified, m_snapshotActive,
        m_syncBlocked, m_cacheForgettingAccount, QString::number(m_directTrust)}).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(QCryptographicHash::hash(context, QCryptographicHash::Sha256).toHex());
}
QString NativeController::directChatPhase() const { const auto *intent = directChat(); return intent ? intent->phase : QStringLiteral("idle"); }
bool NativeController::directChatBusy() const { const auto *intent = directChat(); return intent && intent->busy; }
bool NativeController::directChatUncertain() const { return directChatPhase() == "uncertain"; }
QString NativeController::directChatError() const { const auto *intent = directChat(); return intent ? intent->error : QString{}; }
QVariantMap NativeController::directChatForm() const
{
    const auto *intent = directChat();
    return intent ? QVariantMap{{"attemptId", intent->attempt}, {"peerUuid", intent->peer},
        {"expectedStreamId", intent->uuid}, {"confirmedStreamId", intent->confirmed ? intent->uuid : QString{}},
        {"defaultTopicId", intent->defaultTopic}, {"contextKey", intent->contextKey}} : QVariantMap{};
}
bool NativeController::directChatAllowed() const
{
    return authenticated() && m_catalogVerified && !m_snapshotActive && !m_syncBlocked
        && !m_busy && !m_cacheForgettingAccount && !channelCreationBusy();
}

bool NativeController::canStartDirectChatWith(const QString &peer) const
{
    const auto id = peer.toLower();
    return directChatAllowed() && !directChatBusy() && directChatDirectoryVerified()
        && isUuid(id) && id != m_session->user() && m_channelChoices.contains(id);
}

bool NativeController::canCheckDirectChat() const
{
    return directChatAllowed() && !directChatBusy() && directChatUncertain();
}

bool NativeController::canRetryDirectChat() const
{
    const auto *intent = directChat();
    return canCheckDirectChat() && intent && !intent->accessLost
        && directChatDirectoryVerified() && m_channelChoices.contains(intent->peer);
}

bool NativeController::isDirectConversation() const
{
    const auto *stream = currentStream();
    return stream && stream->privateStream && isUuid(stream->directUser) && stream->directUser != m_session->user();
}

QString NativeController::peerDisplayName(const QString &id, const QString &fallback) const
{
    const auto name = m_channelDirectoryOwner == creationOwnerKey()
        ? m_channelChoices.value(id).value("displayName").toString() : QString{};
    return name.isEmpty() ? userDisplayName(id, fallback.isEmpty() ? "#" + id.left(8) : fallback) : name;
}

QVariantMap NativeController::directPeer() const
{
    if (!isDirectConversation()) return {};
    const auto id = currentStream()->directUser;
    auto choice = m_channelDirectoryOwner == creationOwnerKey() ? m_channelChoices.value(id) : QVariantMap{};
    choice["id"] = id;
    choice["displayName"] = peerDisplayName(id);
    choice["avatar"] = userAvatar(id);
    choice["presence"] = userPresence(id);
    return choice;
}

void NativeController::captureDirectContext(DirectChat &intent)
{
    intent.generation = m_api->generation(); intent.selection = m_selection; intent.catalog = m_catalog;
    intent.routeStream = m_stream; intent.routeTopic = m_topic;
    intent.contextKey = directChatContextKey(); intent.cache = m_cache; intent.lease = m_cacheLease;
    intent.ticket = ++m_nextDirectChat; intent.busy = true; intent.error.clear();
    intent.accessLost = intent.publishing = false; intent.topics = {}; intent.readReconciliations = 0;
    intent.collectionPath.clear();
    intent.collectionRequests = 0; intent.collectionRows = intent.collectionBytes = 0;
}

bool NativeController::directChatContext(const QString &owner, const QString &attempt, quint64 ticket)
{
    const auto found = m_directChats.constFind(owner);
    if (found == m_directChats.cend() || found->attempt != attempt || found->ticket != ticket || !found->busy) return false;
    if (owner == creationOwnerKey() && found->generation == m_api->generation() && found->selection == m_selection
        && found->catalog == m_catalog && found->routeStream == m_stream && found->routeTopic == m_topic
        && found->cache == m_cache && found->lease.owner == m_cacheLease.owner
        && found->lease.generation == m_cacheLease.generation && directChatAllowed() && !found->accessLost) return true;
    finishDirectChat(owner, attempt, ticket, tr("The chat status is unknown. Check status before trying again."));
    return false;
}

bool NativeController::directPhase(const QString &owner, const QString &attempt, quint64 ticket, const QString &phase)
{
    if (!directChatContext(owner, attempt, ticket)) return false;
    m_directChats[owner].phase = phase;
    const QPointer<NativeController> guard(this);
    emit stateChanged();
    return guard && directChatContext(owner, attempt, ticket);
}

void NativeController::finishDirectChat(const QString &owner, const QString &attempt, quint64 ticket,
                                        const QString &error, const QString &phase)
{
    auto found = m_directChats.find(owner);
    if (found == m_directChats.end() || found->attempt != attempt || found->ticket != ticket) return;
    found->busy = found->publishing = false; found->phase = phase; found->error = error; found->topics = {};
    found->allowCreate = false;
    if (owner == creationOwnerKey()) emit stateChanged();
}

void NativeController::interruptDirectChats(const QString &preservePublishingAttempt)
{
    for (auto &intent : m_directChats) if (intent.busy || intent.publishing) {
        if (intent.publishing && intent.attempt == preservePublishingAttempt) continue;
        intent.busy = intent.publishing = false; intent.phase = QStringLiteral("uncertain"); intent.allowCreate = false;
        intent.error = tr("The chat status is unknown. Check status before trying again.");
        intent.ticket = ++m_nextDirectChat; intent.topics = {};
    }
}

void NativeController::noteDirectChatEvent(const QString &stream, const QString &topic, bool deleted,
                                          bool matchingOwn, bool accessLost)
{
    auto found = m_directChats.find(creationOwnerKey());
    if (found == m_directChats.end() || found->uuid != stream
        || (!found->busy && found->phase != "uncertain" && !found->publishing)) return;
    ++found->revision;
    if (!topic.isEmpty()) ++found->topicRevision;
    if (!matchingOwn) ++found->unsafeRevision;
    if (accessLost || (deleted && (topic.isEmpty() || found->defaultTopic == topic))) {
        found->busy = found->publishing = false; found->phase = QStringLiteral("uncertain"); found->allowCreate = false;
        found->accessLost = true; found->ticket = ++m_nextDirectChat; found->topics = {};
        found->error = tr("The chat changed or is no longer accessible. Check status.");
        ++m_directTrust;
    }
}

bool NativeController::reconcileDirectRead(const QString &owner, const QString &attempt, quint64 ticket,
                                          quint64 unsafeRevision, std::function<void()> retry)
{
    if (!directChatContext(owner, attempt, ticket)) return true;
    auto &intent = m_directChats[owner];
    if (intent.unsafeRevision != unsafeRevision || intent.readReconciliations >= 3) return false;
    ++intent.readReconciliations;
    retry();
    return true;
}

void NativeController::startDirectChat(const QString &peer)
{
    const auto id = peer.toLower();
    if (!canStartDirectChatWith(id)) return;
    if (const auto *previous = directChat(); previous && previous->peer == id && previous->phase == "uncertain") {
        checkDirectChat(); return;
    }
    const auto owner = creationOwnerKey();
    if (!m_directChats.contains(owner) && m_directChats.size() >= 32) {
        report(tr("Too many pending chat accounts. Restart the application before opening another chat.")); return;
    }
    DirectChat intent;
    intent.attempt = QUuid::createUuid().toString(QUuid::WithoutBraces);
    intent.peer = id; intent.uuid = canonicalDirectUuid(m_session->project(), m_session->user(), id);
    captureDirectContext(intent); intent.allowCreate = true;
    const auto attempt = intent.attempt; const auto ticket = intent.ticket;
    m_directChats.insert(owner, intent);
    lookupDirectChat(owner, attempt, ticket);
}

void NativeController::checkDirectChat()
{
    if (!canCheckDirectChat()) return;
    const auto owner = creationOwnerKey(); auto &intent = m_directChats[owner];
    captureDirectContext(intent); intent.allowCreate = false;
    const auto attempt = intent.attempt; const auto ticket = intent.ticket;
    lookupDirectChat(owner, attempt, ticket);
}

void NativeController::retryDirectChat()
{
    if (!canRetryDirectChat()) return;
    const auto owner = creationOwnerKey(); auto &intent = m_directChats[owner];
    captureDirectContext(intent); intent.allowCreate = true;
    const auto attempt = intent.attempt; const auto ticket = intent.ticket;
    lookupDirectChat(owner, attempt, ticket);
}

void NativeController::lookupDirectChat(const QString &owner, const QString &attempt, quint64 ticket)
{
    const bool creating = m_directChats.value(owner).dispatched;
    if (!directPhase(owner, attempt, ticket, creating ? QStringLiteral("verifying-stream") : QStringLiteral("looking-up"))) return;
    const auto id = m_directChats.value(owner).uuid;
    const auto revision = m_directChats.value(owner).revision, unsafeRevision = m_directChats.value(owner).unsafeRevision;
    request("GET", messenger + "/streams/" + id, {}, {}, [this, owner, attempt, ticket, revision, unsafeRevision](const ApiResult &result) {
        if (!directChatContext(owner, attempt, ticket)) return;
        auto &intent = m_directChats[owner];
        if (revision != intent.revision) {
            if (reconcileDirectRead(owner, attempt, ticket, unsafeRevision,
                [this, owner, attempt, ticket] { lookupDirectChat(owner, attempt, ticket); })) return;
            finishDirectChat(owner, attempt, ticket, tr("The chat changed. Check status to open it.")); return;
        }
        if (result.status == 404) {
            if (intent.allowCreate && directChatDirectoryVerified() && m_channelChoices.contains(intent.peer)) {
                postDirectChat(owner, attempt, ticket); return;
            }
            finishDirectChat(owner, attempt, ticket, tr("The chat could not yet be found. Check status, or explicitly retry Start chat.")); return;
        }
        const auto raw = result.data.toObject();
        const auto parsed = result.ok() && result.data.isObject()
            ? directStreamDto(raw, m_session->project(), m_session->user(), intent.peer) : std::optional<Stream>{};
        if (!parsed) {
            intent.allowCreate = false;
            const bool denied = result.status == 400 || result.status == 401 || result.status == 403;
            if (denied || result.ok()) intent.accessLost = true;
            finishDirectChat(owner, attempt, ticket, tr("The server did not verify this personal chat."),
                denied ? QStringLiteral("failed") : QStringLiteral("uncertain")); return;
        }
        intent.confirmed = true; intent.allowCreate = false;
        intent.defaultTopic = raw.value("default_topic_uuid").toString(); intent.acl = streamAcl(*parsed);
        if (parsed->archived) {
            finishDirectChat(owner, attempt, ticket,
                tr("This personal chat is archived. Opening archived chats is not supported yet."), QStringLiteral("unsupported")); return;
        }
        if (!isUuid(intent.defaultTopic)) {
            finishDirectChat(owner, attempt, ticket, tr("The server did not return a valid default topic for this chat.")); return;
        }
        loadDirectDefault(owner, attempt, ticket);
    });
}

void NativeController::postDirectChat(const QString &owner, const QString &attempt, quint64 ticket)
{
    if (!directPhase(owner, attempt, ticket, QStringLiteral("creating"))) return;
    auto &intent = m_directChats[owner];
    if (!intent.allowCreate || !directChatDirectoryVerified() || !m_channelChoices.contains(intent.peer)) {
        finishDirectChat(owner, attempt, ticket, tr("Choose a person from the updated member directory."), QStringLiteral("failed")); return;
    }
    intent.allowCreate = false; intent.dispatched = true;
    const auto peer = intent.peer;
    request("POST", messenger + "/streams/", QJsonObject{{"name", "Direct"}, {"description", "Private workspace"},
        {"source_name", "native"}, {"direct_user_uuid", peer}}, {}, [this, owner, attempt, ticket](const ApiResult &result) {
        if (!directChatContext(owner, attempt, ticket)) return;
        const auto &intent = m_directChats[owner];
        const auto parsed = result.ok() && result.data.isObject()
            ? directStreamDto(result.data.toObject(), m_session->project(), m_session->user(), intent.peer) : std::optional<Stream>{};
        if (!parsed || parsed->archived) {
            const bool failed = result.status == 400 || result.status == 401 || result.status == 403 || result.status == 404;
            finishDirectChat(owner, attempt, ticket, failed ? tr("Could not start the personal chat. %1").arg(result.error)
                : tr("The chat status is unknown. Check status before trying again."),
                failed ? QStringLiteral("failed") : QStringLiteral("uncertain")); return;
        }
        lookupDirectChat(owner, attempt, ticket);
    });
}

void NativeController::directCollection(const QString &owner, const QString &attempt, quint64 ticket,
    const QString &path, QUrlQuery query, std::function<void(const QJsonArray &)> done, QJsonArray rows, QSet<QByteArray> markers)
{
    if (!directChatContext(owner, attempt, ticket)) return;
    auto &intent = m_directChats[owner];
    if (intent.collectionPath != path) {
        intent.collectionPath = path;
        intent.collectionStreamRevision = m_streamRevision;
    }
    if (intent.collectionRequests >= 500) {
        finishDirectChat(owner, attempt, ticket, tr("The server returned an invalid personal chat list.")); return;
    }
    ++intent.collectionRequests;
    const auto revision = intent.revision, unsafeRevision = intent.unsafeRevision, streamRevision = intent.collectionStreamRevision;
    request("GET", path, {}, query, [this, owner, attempt, ticket, revision, unsafeRevision, streamRevision, path, query, done, rows, markers](const ApiResult &result) mutable {
        if (!directChatContext(owner, attempt, ticket)) return;
        if (!result.ok() || !result.data.isArray()) {
            finishDirectChat(owner, attempt, ticket, tr("The chat could not be verified. Check status.")); return;
        }
        auto &current = m_directChats[owner];
        current.collectionRows += result.data.toArray().size();
        current.collectionBytes += QJsonDocument(result.data.toArray()).toJson(QJsonDocument::Compact).size();
        if (current.collectionRows > 100000 || current.collectionBytes > NativeCacheService::MaximumQueuedBytes) {
            finishDirectChat(owner, attempt, ticket, tr("The server returned an invalid personal chat list.")); return;
        }
        if (revision != current.revision || (path == messenger + "/streams/" && streamRevision != m_streamRevision)) {
            if (reconcileDirectRead(owner, attempt, ticket, unsafeRevision, [this, owner, attempt, ticket, path, query, done]() mutable {
                query.removeAllQueryItems("page_marker");
                m_directChats[owner].collectionStreamRevision = m_streamRevision;
                directCollection(owner, attempt, ticket, path, query, done);
            })) return;
            finishDirectChat(owner, attempt, ticket, tr("The chat changed. Check status to open it.")); return;
        }
        for (const auto &value : result.data.toArray()) rows.append(value);
        if (result.nextMarker.isEmpty()) { done(rows); return; }
        if (markers.contains(result.nextMarker) || current.collectionRequests >= 500) {
            finishDirectChat(owner, attempt, ticket, tr("The server returned an invalid personal chat list.")); return;
        }
        markers.insert(result.nextMarker);
        query.removeAllQueryItems("page_marker"); query.addQueryItem("page_marker", QString::fromUtf8(result.nextMarker));
        directCollection(owner, attempt, ticket, path, query, done, rows, markers);
    });
}

void NativeController::loadDirectDefault(const QString &owner, const QString &attempt, quint64 ticket)
{
    if (!directPhase(owner, attempt, ticket, QStringLiteral("verifying-default"))) return;
    const auto id = m_directChats.value(owner).uuid;
    QUrlQuery query; query.addQueryItem("stream_uuid", id); query.addQueryItem("page_limit", "200");
    directCollection(owner, attempt, ticket, messenger + "/stream_topics/", query, [this, owner, attempt, ticket, id](const QJsonArray &rows) {
        if (!directChatContext(owner, attempt, ticket)) return;
        QSet<QString> ids; int defaults = 0; const auto defaultId = m_directChats.value(owner).defaultTopic;
        for (const auto &value : rows) {
            const auto raw = value.toObject();
            const auto parsed = creationTopicDto(raw, m_session->project(), id, m_session->user());
            if (!value.isObject() || !parsed || ids.contains(parsed->uuid)
                || !validCreationDate(raw.value("created_at")) || !validCreationDate(raw.value("updated_at"))
                || !raw.value("notification_mode").isString() || (parsed->isDefault && parsed->uuid != defaultId)) {
                finishDirectChat(owner, attempt, ticket, tr("The server did not return a valid default topic for this chat.")); return;
            }
            ids.insert(parsed->uuid); defaults += parsed->isDefault;
        }
        if (defaults != 1 || !ids.contains(defaultId)) {
            finishDirectChat(owner, attempt, ticket, tr("The server did not return a valid default topic for this chat.")); return;
        }
        auto &intent = m_directChats[owner]; intent.topics = rows; intent.topicsUnsafeRevision = intent.unsafeRevision;
        intent.topicsRevision = intent.topicRevision;
        loadDirectCatalog(owner, attempt, ticket);
    });
}

void NativeController::loadDirectCatalog(const QString &owner, const QString &attempt, quint64 ticket)
{
    if (!directPhase(owner, attempt, ticket, QStringLiteral("refreshing-catalogs"))) return;
    m_directChats[owner].collectionStreamRevision = m_streamRevision;
    QUrlQuery query; query.addQueryItem("page_limit", "200");
    directCollection(owner, attempt, ticket, messenger + "/streams/", query, [this, owner, attempt, ticket](const QJsonArray &rows) {
        if (!directChatContext(owner, attempt, ticket)) return;
        const auto intent = m_directChats.value(owner);
        if (intent.topicsUnsafeRevision != intent.unsafeRevision) {
            finishDirectChat(owner, attempt, ticket, tr("The chat changed. Check status to open it.")); return;
        }
        if (intent.topicsRevision != intent.topicRevision) {
            if (reconcileDirectRead(owner, attempt, ticket, intent.topicsUnsafeRevision,
                [this, owner, attempt, ticket] { loadDirectDefault(owner, attempt, ticket); })) return;
            finishDirectChat(owner, attempt, ticket, tr("The chat changed. Check status to open it.")); return;
        }
        QVector<Stream> streams; QSet<QString> ids; bool found = false;
        for (const auto &value : rows) {
            const auto raw = value.toObject(); const bool target = raw.value("uuid").toString() == intent.uuid;
            const auto parsed = target ? directStreamDto(raw, m_session->project(), m_session->user(), intent.peer)
                : creationStreamDto(raw, m_session->project(), m_session->user(), false);
            if (!value.isObject() || !parsed || ids.contains(parsed->uuid) || (target
                && (parsed->archived || streamAcl(*parsed) != intent.acl
                    || raw.value("default_topic_uuid").toString() != intent.defaultTopic))) {
                finishDirectChat(owner, attempt, ticket, tr("The server returned an invalid personal chat list.")); return;
            }
            ids.insert(parsed->uuid); found |= target;
            streams.append(*parsed);
        }
        if (!found) { finishDirectChat(owner, attempt, ticket, tr("The chat is missing from the current chat list. Check status.")); return; }
        QVector<Topic> topics;
        for (const auto &value : intent.topics) topics.append(*creationTopicDto(value.toObject(), m_session->project(), intent.uuid, m_session->user()));
        const auto streamRevision = m_streamRevision;
        const QPointer<NativeController> guard(this);
        m_streams = streams; ++m_streamRevision; m_catalogVerified = true;
        emit streamsChanged();
        if (!guard || !directChatContext(owner, attempt, ticket)) return;
        if (m_streamRevision != streamRevision + 1 || m_directChats.value(owner).revision != intent.revision) {
            if (reconcileDirectRead(owner, attempt, ticket, intent.unsafeRevision,
                [this, owner, attempt, ticket] { loadDirectCatalog(owner, attempt, ticket); })) return;
            finishDirectChat(owner, attempt, ticket, tr("The chat changed. Check status to open it.")); return;
        }
        if (m_cache) {
            m_cache->put(m_cacheLease, {NativeCache::Kind::Streams, {}, {}, {}}, rows);
            if (!guard || !directChatContext(owner, attempt, ticket)) return;
            m_cache->put(m_cacheLease, {NativeCache::Kind::Topics, intent.uuid, {}, {}}, intent.topics);
            if (!guard || !directChatContext(owner, attempt, ticket)) return;
        }
        const auto selection = m_selection;
        const auto publishedContext = [this, owner, attempt, ticket, selection, intent] {
            const auto current = m_directChats.constFind(owner);
            const auto *stream = currentStream();
            return current != m_directChats.cend() && current->attempt == attempt && current->ticket == ticket && current->publishing
                && !current->accessLost && current->unsafeRevision == intent.unsafeRevision
                && current->topicRevision == intent.topicRevision
                && creationOwnerKey() == owner && m_api->generation() == intent.generation
                && m_catalog == intent.catalog && m_selection == selection + 1 && m_stream == intent.uuid && m_topic.isEmpty()
                && m_cache == intent.cache && m_cacheLease.owner == intent.lease.owner
                && m_cacheLease.generation == intent.lease.generation && directChatAllowed()
                && stream && streamAcl(*stream) == intent.acl;
        };
        m_directChats[owner].busy = false;
        m_directChats[owner].publishing = true;
        activateStream(intent.uuid, &topics, attempt);
        if (!guard) return;
        if (!publishedContext()) {
            finishDirectChat(owner, attempt, ticket, tr("The chat is ready. Check status to open it.")); return;
        }
        m_directChats[owner].phase = QStringLiteral("ready");
        emit stateChanged();
        if (!guard) return;
        if (!publishedContext()) {
            finishDirectChat(owner, attempt, ticket, tr("The chat is ready. Check status to open it.")); return;
        }
        emit directChatOpened(intent.uuid, intent.peer, intent.defaultTopic);
        if (!guard) return;
        auto completed = m_directChats.find(owner);
        if (completed != m_directChats.end() && completed->attempt == attempt && completed->ticket == ticket) {
            completed->publishing = false;
            completed->topics = {};
        }
    });
}

const NativeController::ChannelCreation *NativeController::channelCreation() const
{
    const auto found = m_channelCreations.constFind(creationOwnerKey());
    return found == m_channelCreations.cend() ? nullptr : &found.value();
}

bool NativeController::channelCreationAllowed() const
{
    return authenticated() && m_catalogVerified && !m_snapshotActive && !m_syncBlocked
        && !m_busy && !m_cacheForgettingAccount && !directChatBusy();
}

bool NativeController::canCreateChannel() const
{
    const auto *intent = channelCreation();
    return channelCreationAllowed() && (!intent || (!intent->busy
        && (intent->phase == "idle" || intent->phase == "failed" || intent->phase == "ready")));
}

QVariantList NativeController::channelMemberChoices() const
{
    QVariantList result;
    if (m_channelDirectoryOwner != creationOwnerKey() || !authenticated()) return result;
    for (const auto &choice : m_channelChoices) result.append(choice);
    return result;
}

bool NativeController::channelMembersVerified() const
{
    return authenticated() && m_usersVerified && m_channelDirectoryValid
        && m_channelDirectoryOwner == creationOwnerKey();
}
QString NativeController::channelCreationContextKey() const
{
    if (!authenticated()) return {};
    const auto context = QJsonDocument(QJsonArray{creationOwnerKey(), QString::number(m_selection),
        QString::number(m_catalog), QString::number(m_api->generation()),
        QString::number(reinterpret_cast<quintptr>(m_cache.data())), m_cacheLease.owner,
        QString::number(m_cacheLease.generation), m_catalogVerified, m_snapshotActive,
        m_syncBlocked, m_cacheForgettingAccount, QString::number(m_channelTrust)}).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(QCryptographicHash::hash(context, QCryptographicHash::Sha256).toHex());
}
QString NativeController::channelCreationPhase() const
{
    const auto *intent = channelCreation(); return intent ? intent->phase : QStringLiteral("idle");
}
bool NativeController::channelCreationBusy() const { const auto *intent = channelCreation(); return intent && intent->busy; }
bool NativeController::channelCreationUncertain() const { return channelCreationPhase() == "uncertain"; }
QString NativeController::channelCreationError() const { const auto *intent = channelCreation(); return intent ? intent->error : QString{}; }
QVariantMap NativeController::channelCreationForm() const
{
    const auto *intent = channelCreation();
    if (!intent) return {};
    QVariantList members;
    for (const auto &id : intent->members) members.append(id);
    return {{"attemptId", intent->uuid}, {"name", intent->name}, {"description", intent->description},
        {"inviteOnly", intent->inviteOnly}, {"announce", intent->announce}, {"memberUuids", members},
        {"confirmedStreamId", intent->confirmed ? intent->uuid : QString{}}, {"defaultTopicId", intent->defaultTopic},
        {"membersAttempted", intent->membersAttempted}, {"contextKey", intent->contextKey}};
}
bool NativeController::canCheckChannelCreation() const
{
    const auto *intent = channelCreation();
    return channelCreationAllowed() && intent && !intent->busy && !intent->deleted && intent->phase == "uncertain";
}
bool NativeController::canRetryChannelMembers() const
{
    const auto *intent = channelCreation();
    return channelCreationAllowed() && intent && !intent->busy && !intent->deleted && !intent->accessLost && intent->confirmed
        && !intent->members.isEmpty() && (intent->phase == "members-required" || intent->phase == "members-failed");
}

void NativeController::updateChannelDirectory(const QJsonArray &rows, bool authoritative)
{
    QMap<QString, QVariantMap> choices;
    bool valid = rows.size() <= 100000;
    for (const auto &value : rows) {
        const auto choice = value.isObject() ? channelDirectoryChoice(value.toObject(), m_session->project()) : std::optional<QVariantMap>{};
        if (!choice) { valid = false; break; }
        const auto id = choice->value("id").toString();
        if (id != m_session->user() && !choices.contains(id)) choices.insert(id, *choice);
    }
    m_channelChoices = valid ? choices : QMap<QString, QVariantMap>{};
    m_channelDirectoryOwner = creationOwnerKey();
    m_channelDirectoryValid = authoritative && valid;
}

void NativeController::captureChannelContext(ChannelCreation &intent)
{
    intent.generation = m_api->generation();
    intent.selection = m_selection;
    intent.catalog = m_catalog;
    intent.routeStream = m_stream;
    intent.routeTopic = m_topic;
    intent.contextKey = channelCreationContextKey();
    intent.cache = m_cache;
    intent.lease = m_cacheLease;
    intent.ticket = ++m_nextChannelCreation;
    intent.busy = true;
    intent.error.clear();
    intent.topics = {};
    intent.readReconciliations = 0;
    intent.collectionPath.clear();
}

bool NativeController::channelCreationContext(const QString &owner, const QString &id, quint64 ticket)
{
    const auto found = m_channelCreations.constFind(owner);
    if (found == m_channelCreations.cend() || found->uuid != id || found->ticket != ticket || !found->busy) return false;
    if (owner == creationOwnerKey() && found->generation == m_api->generation() && found->selection == m_selection
        && found->catalog == m_catalog && found->routeStream == m_stream && found->routeTopic == m_topic
        && found->cache == m_cache && found->lease.owner == m_cacheLease.owner
        && found->lease.generation == m_cacheLease.generation && channelCreationAllowed() && !found->deleted && !found->accessLost) return true;
    finishChannelCreation(owner, id, ticket, tr("The channel creation status is unknown. Check status before trying again."),
        found->dispatched ? QStringLiteral("uncertain") : QStringLiteral("failed"));
    return false;
}

bool NativeController::channelPhase(const QString &owner, const QString &id, quint64 ticket, const QString &phase)
{
    if (!channelCreationContext(owner, id, ticket)) return false;
    m_channelCreations[owner].phase = phase;
    const QPointer<NativeController> guard(this);
    emit stateChanged();
    return guard && channelCreationContext(owner, id, ticket);
}

void NativeController::finishChannelCreation(const QString &owner, const QString &id, quint64 ticket,
                                             const QString &error, const QString &phase)
{
    auto found = m_channelCreations.find(owner);
    if (found == m_channelCreations.end() || found->uuid != id || found->ticket != ticket) return;
    found->busy = false;
    found->phase = phase;
    found->error = error;
    found->topics = {};
    if (owner == creationOwnerKey()) emit stateChanged();
}

void NativeController::interruptChannelCreations()
{
    for (auto &intent : m_channelCreations) if (intent.busy) {
        intent.busy = false;
        intent.phase = intent.dispatched ? QStringLiteral("uncertain") : QStringLiteral("failed");
        intent.error = tr("The channel creation status is unknown. Check status before trying again.");
        intent.ticket = ++m_nextChannelCreation;
        intent.topics = {};
    }
}

void NativeController::noteChannelCreationEvent(const QString &stream, const QString &topic, bool deleted, bool matchingOwn)
{
    auto found = m_channelCreations.find(creationOwnerKey());
    if (found == m_channelCreations.end() || found->uuid != stream) return;
    if (!found->busy && (found->phase == "ready" || found->phase == "failed" || found->phase == "idle")) return;
    ++found->revision;
    if (!matchingOwn) ++found->unsafeRevision;
    if (deleted && (topic.isEmpty() || topic == found->defaultTopic)) {
        found->deleted = topic.isEmpty();
        found->busy = false;
        found->phase = topic.isEmpty() ? QStringLiteral("failed") : QStringLiteral("uncertain");
        found->error = topic.isEmpty() ? tr("The created channel was deleted.") : tr("The created default topic was deleted.");
        if (!topic.isEmpty()) { found->defaultTopic.clear(); found->accessLost = true; }
        found->ticket = ++m_nextChannelCreation;
        found->topics = {};
        ++m_channelTrust;
    }
}

bool NativeController::reconcileChannelRead(const QString &owner, const QString &id, quint64 ticket,
                                           quint64 unsafeRevision, std::function<void()> retry)
{
    if (!channelCreationContext(owner, id, ticket)) return true;
    auto &intent = m_channelCreations[owner];
    if (intent.unsafeRevision != unsafeRevision || intent.readReconciliations >= 3) return false;
    ++intent.readReconciliations;
    retry();
    return true;
}

void NativeController::loseChannelCreationAccess(const QString &stream)
{
    auto found = m_channelCreations.find(creationOwnerKey());
    if (found == m_channelCreations.end() || found->uuid != stream || !found->dispatched || found->accessLost
        || found->phase == "ready" || found->phase == "failed" || found->phase == "idle") return;
    found->accessLost = true;
    found->busy = false;
    found->phase = QStringLiteral("uncertain");
    found->error = tr("The channel creation status is unknown. Check status before trying again.");
    found->ticket = ++m_nextChannelCreation;
    found->topics = {};
    // This is an actual loss of the active intent's authority. Ordinary row
    // counters and display metadata never increment the form lifetime.
    ++m_channelTrust;
}

void NativeController::createChannel(const QString &name, const QString &description,
                                     bool inviteOnly, bool announce, const QVariantList &memberUuids)
{
    if (directChatBusy()) return;
    const auto owner = creationOwnerKey();
    if (owner.isEmpty()) return;
    const auto *previous = channelCreation();
    if (previous && (previous->busy || (previous->phase != "idle" && previous->phase != "failed" && previous->phase != "ready"))) return;
    ChannelCreation intent;
    const auto trimmedName = sourceTrimmed(name), trimmedDescription = sourceTrimmed(description);
    intent.name = boundedText(trimmedName, 256) ? trimmedName : QString{};
    intent.description = boundedText(trimmedDescription, 10001) ? trimmedDescription : QString{};
    intent.inviteOnly = inviteOnly; intent.announce = announce;
    intent.contextKey = channelCreationContextKey();
    if (trimmedName.isEmpty() || !boundedText(trimmedName, 255))
        intent.error = tr("Enter a channel name with 1 to 255 characters.");
    else if (!boundedText(trimmedDescription, 10000))
        intent.error = tr("Enter a description with no more than 10000 characters.");
    else if (!channelCreationAllowed())
        intent.error = tr("Wait for the channel list to update before creating a channel.");
    else if (memberUuids.size() > 100000)
        intent.error = tr("The selected channel members are invalid. Choose members from the current directory.");
    QSet<QString> seen;
    if (intent.error.isEmpty()) for (const auto &value : memberUuids) {
        const auto id = value.toString().toLower();
        if (value.metaType().id() != QMetaType::QString || !isUuid(id)) {
            intent.error = tr("The selected channel members are invalid. Choose members from the current directory."); break;
        }
        if (id == m_session->user() || seen.contains(id)) continue;
        seen.insert(id); intent.members.append(id);
    }
    if (intent.error.isEmpty() && !intent.members.isEmpty()) {
        if (!channelMembersVerified()) intent.error = tr("Wait for the member directory to update before creating this channel.");
        else for (const auto &id : intent.members) if (!m_channelChoices.contains(id)) {
            intent.error = tr("The selected channel members are invalid. Choose members from the current directory."); break;
        }
    }
    const auto formBytes = [](const ChannelCreation &entry) {
        return entry.name.toUtf8().size() + entry.description.toUtf8().size() + entry.members.size() * 40 + 512;
    };
    qint64 retained = formBytes(intent);
    for (auto it = m_channelCreations.begin(); it != m_channelCreations.end();) {
        if (it.key() != owner && !it->busy && (it->phase == "idle" || it->phase == "failed" || it->phase == "ready"))
            it = m_channelCreations.erase(it);
        else { if (it.key() != owner) retained += formBytes(*it); ++it; }
    }
    if ((!m_channelCreations.contains(owner) && m_channelCreations.size() >= 32)
        || retained > NativeCacheService::MaximumQueuedBytes) {
        report(tr("Resolve the earlier channel creation attempts before creating another channel.")); return;
    }
    m_channelCreations.insert(owner, intent);
    if (!intent.error.isEmpty()) { emit stateChanged(); return; }
    auto &accepted = m_channelCreations[owner];
    accepted.uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    accepted.allowMembers = true;
    captureChannelContext(accepted);
    const auto id = accepted.uuid;
    const auto ticket = accepted.ticket;
    const auto body = QJsonObject{{"uuid", id}, {"name", accepted.name}, {"description", accepted.description},
        {"source_name", "native"}, {"invite_only", inviteOnly}, {"announce", announce},
        {"encryption", false}, {"current_encryption_key_uuid", QJsonValue::Null}};
    if (!channelPhase(owner, id, ticket, QStringLiteral("creating"))) return;
    m_channelCreations[owner].dispatched = true;
    request("POST", messenger + "/streams/", body, {}, [this, owner, id, ticket](const ApiResult &result) {
        if (!channelCreationContext(owner, id, ticket)) return;
        auto &intent = m_channelCreations[owner];
        const auto raw = result.data.toObject();
        const auto parsed = creationStreamDto(raw, m_session->project(), m_session->user(), true);
        if (!result.ok() || !parsed || parsed->uuid != id || parsed->name != intent.name
            || parsed->description != intent.description || raw.value("invite_only").toBool() != intent.inviteOnly
            || raw.value("announce").toBool() != intent.announce) {
            const bool failed = result.status == 400 || result.status == 401 || result.status == 403 || result.status == 404;
            finishChannelCreation(owner, id, ticket, failed ? tr("Could not create the channel. %1").arg(result.error)
                : tr("The channel creation status is unknown. Check status before trying again."),
                failed ? QStringLiteral("failed") : QStringLiteral("uncertain")); return;
        }
        intent.verifiedRequest = true;
        intent.defaultTopic = raw.value("default_topic_uuid").toString();
        confirmChannelCreation(owner, id, ticket);
    });
}

void NativeController::checkChannelCreation()
{
    if (!canCheckChannelCreation()) return;
    const auto owner = creationOwnerKey();
    auto &intent = m_channelCreations[owner];
    intent.allowMembers = false; intent.checkBindings = true;
    intent.accessLost = false; // Explicit GET-only recovery may re-establish ownership.
    captureChannelContext(intent);
    const auto id = intent.uuid; const auto ticket = intent.ticket;
    confirmChannelCreation(owner, id, ticket);
}

void NativeController::retryChannelMembers()
{
    if (!canRetryChannelMembers()) return;
    const auto owner = creationOwnerKey();
    auto &intent = m_channelCreations[owner];
    intent.allowMembers = true; intent.checkBindings = true;
    captureChannelContext(intent);
    const auto id = intent.uuid; const auto ticket = intent.ticket;
    confirmChannelCreation(owner, id, ticket);
}

void NativeController::confirmChannelCreation(const QString &owner, const QString &id, quint64 ticket)
{
    if (!channelPhase(owner, id, ticket, QStringLiteral("verifying-stream"))) return;
    const auto revision = m_channelCreations.value(owner).revision;
    const auto unsafeRevision = m_channelCreations.value(owner).unsafeRevision;
    request("GET", messenger + "/streams/" + id, {}, {}, [this, owner, id, ticket, revision, unsafeRevision](const ApiResult &result) {
        if (!channelCreationContext(owner, id, ticket)) return;
        auto &intent = m_channelCreations[owner];
        const auto raw = result.data.toObject();
        const auto parsed = creationStreamDto(raw, m_session->project(), m_session->user(), true);
        if (!result.ok() || !parsed || parsed->uuid != id
            || (!intent.verifiedRequest && (parsed->name != intent.name || parsed->description != intent.description
                || raw.value("invite_only").toBool() != intent.inviteOnly || raw.value("announce").toBool() != intent.announce))) {
            finishChannelCreation(owner, id, ticket, tr("The channel creation status is unknown. Check status before trying again.")); return;
        }
        if (revision != intent.revision) {
            if (reconcileChannelRead(owner, id, ticket, unsafeRevision, [this, owner, id, ticket] { confirmChannelCreation(owner, id, ticket); })) return;
            finishChannelCreation(owner, id, ticket, tr("The channel creation status is unknown. Check status before trying again.")); return;
        }
        intent.verifiedRequest = intent.confirmed = true;
        intent.defaultTopic = raw.value("default_topic_uuid").toString();
        intent.acl = streamAcl(*parsed);
        loadChannelDefault(owner, id, ticket);
    });
}

void NativeController::channelCollection(const QString &owner, const QString &id, quint64 ticket,
    const QString &path, QUrlQuery query, std::function<void(const QJsonArray &)> done,
    QJsonArray rows, QSet<QByteArray> markers)
{
    if (!channelCreationContext(owner, id, ticket)) return;
    auto &intent = m_channelCreations[owner];
    if (intent.collectionPath != path) {
        intent.collectionPath = path; intent.collectionRequests = 0; intent.collectionRows = intent.collectionBytes = 0;
        intent.collectionStreamRevision = m_streamRevision;
    }
    if (intent.collectionRequests >= 500) {
        finishChannelCreation(owner, id, ticket, tr("The server returned an invalid channel creation response.")); return;
    }
    ++intent.collectionRequests;
    const auto revision = intent.revision, unsafeRevision = intent.unsafeRevision;
    const auto streamRevision = intent.collectionStreamRevision;
    request("GET", path, {}, query, [this, owner, id, ticket, revision, unsafeRevision, streamRevision, path, query, done, rows, markers](const ApiResult &result) mutable {
        if (!channelCreationContext(owner, id, ticket)) return;
        if (!result.ok() || !result.data.isArray()) {
            finishChannelCreation(owner, id, ticket, tr("The channel was created, but its status could not be verified. Check status.")); return;
        }
        auto &current = m_channelCreations[owner];
        current.collectionBytes += QJsonDocument(result.data.toArray()).toJson(QJsonDocument::Compact).size();
        current.collectionRows += result.data.toArray().size();
        if (current.collectionRows > 100000 || current.collectionBytes > NativeCacheService::MaximumQueuedBytes) {
            finishChannelCreation(owner, id, ticket, tr("The server returned an invalid channel creation response.")); return;
        }
        if (revision != current.revision || (path == messenger + "/streams/" && streamRevision != m_streamRevision)) {
            if (reconcileChannelRead(owner, id, ticket, unsafeRevision, [this, owner, id, ticket, path, query, done]() mutable {
                query.removeAllQueryItems("page_marker");
                m_channelCreations[owner].collectionStreamRevision = m_streamRevision;
                channelCollection(owner, id, ticket, path, query, done);
            })) return;
            finishChannelCreation(owner, id, ticket, tr("The channel was created, but its status could not be verified. Check status.")); return;
        }
        for (const auto &value : result.data.toArray()) rows.append(value);
        if (!result.nextMarker.isEmpty()) {
            if (markers.contains(result.nextMarker) || current.collectionRequests >= 500) {
                finishChannelCreation(owner, id, ticket, tr("The server returned an invalid channel creation response.")); return;
            }
            markers.insert(result.nextMarker);
            query.removeAllQueryItems("page_marker"); query.addQueryItem("page_marker", QString::fromUtf8(result.nextMarker));
            channelCollection(owner, id, ticket, path, query, done, rows, markers);
        } else done(rows);
    });
}

void NativeController::loadChannelDefault(const QString &owner, const QString &id, quint64 ticket)
{
    if (!channelPhase(owner, id, ticket, QStringLiteral("verifying-default"))) return;
    QUrlQuery query; query.addQueryItem("stream_uuid", id); query.addQueryItem("page_limit", "200");
    channelCollection(owner, id, ticket, messenger + "/stream_topics/", query, [this, owner, id, ticket](const QJsonArray &rows) {
        if (!channelCreationContext(owner, id, ticket)) return;
        const auto defaultId = m_channelCreations.value(owner).defaultTopic;
        QSet<QString> ids; int defaults = 0;
        for (const auto &value : rows) {
            const auto raw = value.toObject();
            const auto parsed = creationTopicDto(raw, m_session->project(), id, m_session->user());
            if (!value.isObject() || !parsed || ids.contains(parsed->uuid) || !validCreationDate(raw.value("created_at"))
                || !validCreationDate(raw.value("updated_at")) || !raw.value("notification_mode").isString()
                || (parsed->isDefault && parsed->uuid != defaultId)) {
                finishChannelCreation(owner, id, ticket, tr("The server returned an invalid default topic.")); return;
            }
            ids.insert(parsed->uuid); if (parsed->isDefault) ++defaults;
        }
        if (defaults != 1 || !ids.contains(defaultId)) {
            finishChannelCreation(owner, id, ticket, tr("The server returned an invalid default topic.")); return;
        }
        auto &intent = m_channelCreations[owner];
        intent.topics = rows;
        intent.topicsUnsafeRevision = intent.unsafeRevision;
        if (intent.members.isEmpty()) { intent.membersConfirmed = true; loadChannelCatalog(owner, id, ticket); }
        else if (intent.checkBindings) checkChannelMembers(owner, id, ticket);
        else if (intent.allowMembers) addChannelMembers(owner, id, ticket);
        else finishChannelCreation(owner, id, ticket, tr("The channel was created. Add the selected members to finish."), QStringLiteral("members-required"));
    });
}

void NativeController::checkChannelMembers(const QString &owner, const QString &id, quint64 ticket)
{
    if (!channelPhase(owner, id, ticket, QStringLiteral("checking-members"))) return;
    QUrlQuery query; query.addQueryItem("stream_uuid", id); query.addQueryItem("page_limit", "200");
    channelCollection(owner, id, ticket, messenger + "/stream_bindings/", query, [this, owner, id, ticket](const QJsonArray &rows) {
        if (!channelCreationContext(owner, id, ticket)) return;
        QSet<QString> users;
        if (!creationBindings(rows, m_session->project(), id, &users)) {
            finishChannelCreation(owner, id, ticket, tr("The server returned invalid channel memberships.")); return;
        }
        const auto intent = m_channelCreations.value(owner);
        const bool complete = std::all_of(intent.members.cbegin(), intent.members.cend(), [&users](const QString &user) { return users.contains(user); });
        if (complete) { m_channelCreations[owner].membersConfirmed = true; loadChannelCatalog(owner, id, ticket); }
        else if (intent.allowMembers) addChannelMembers(owner, id, ticket);
        else finishChannelCreation(owner, id, ticket, tr("The channel was created. Add the selected members to finish."), QStringLiteral("members-required"));
    });
}

void NativeController::addChannelMembers(const QString &owner, const QString &id, quint64 ticket)
{
    if (!channelPhase(owner, id, ticket, QStringLiteral("adding-members"))) return;
    auto &intent = m_channelCreations[owner];
    if (!intent.allowMembers) return;
    QJsonArray members; for (const auto &member : intent.members) members.append(member);
    intent.membersAttempted = true;
    intent.allowMembers = false; // One explicit authorization consumes one member POST.
    const auto revision = intent.revision;
    const auto unsafeRevision = intent.unsafeRevision;
    request("POST", messenger + "/streams/" + id + "/actions/add_users/invoke", QJsonObject{{"member", members}}, {},
        [this, owner, id, ticket, revision, unsafeRevision](const ApiResult &result) {
        if (!channelCreationContext(owner, id, ticket)) return;
        auto &intent = m_channelCreations[owner];
        QSet<QString> users;
        const bool valid = result.data.isArray() && creationBindings(result.data.toArray(), m_session->project(), id, &users)
            && users.size() == intent.members.size()
            && std::all_of(intent.members.cbegin(), intent.members.cend(), [&users](const QString &user) { return users.contains(user); });
        if (!result.ok() || !valid) {
            const bool failed = result.status == 400 || result.status == 401 || result.status == 403 || result.status == 404;
            finishChannelCreation(owner, id, ticket, failed ? tr("The channel was created, but members could not be added. %1").arg(result.error)
                : tr("The channel membership status is unknown. Check status before retrying members."),
                failed ? QStringLiteral("members-failed") : QStringLiteral("uncertain")); return;
        }
        if (revision != intent.revision) {
            if (reconcileChannelRead(owner, id, ticket, unsafeRevision, [this, owner, id, ticket] { checkChannelMembers(owner, id, ticket); })) return;
            finishChannelCreation(owner, id, ticket, tr("The channel membership status is unknown. Check status before retrying members.")); return;
        }
        intent.membersConfirmed = true;
        loadChannelCatalog(owner, id, ticket);
    });
}

void NativeController::loadChannelCatalog(const QString &owner, const QString &id, quint64 ticket)
{
    if (!channelPhase(owner, id, ticket, QStringLiteral("refreshing-catalogs"))) return;
    m_channelCreations[owner].collectionStreamRevision = m_streamRevision;
    QUrlQuery query; query.addQueryItem("page_limit", "200");
    channelCollection(owner, id, ticket, messenger + "/streams/", query, [this, owner, id, ticket](const QJsonArray &rows) {
        if (!channelCreationContext(owner, id, ticket)) return;
        // The target topics were read before the membership/catalog stages.
        // Capturing a newer revision for those requests cannot make an older
        // saved topic snapshot authoritative again.
        if (m_channelCreations.value(owner).topicsUnsafeRevision != m_channelCreations.value(owner).unsafeRevision) {
            finishChannelCreation(owner, id, ticket, tr("The channel was created, but the channel list changed. Check status.")); return;
        }
        const auto streamRevision = m_streamRevision;
        const auto creationRevision = m_channelCreations.value(owner).revision;
        const auto unsafeRevision = m_channelCreations.value(owner).unsafeRevision;
        QVector<Stream> streams; QSet<QString> ids; bool found = false;
        const auto expectedDefault = m_channelCreations.value(owner).defaultTopic;
        for (const auto &value : rows) {
            const auto raw = value.toObject(); const bool target = raw.value("uuid").toString() == id;
            const auto parsed = creationStreamDto(raw, m_session->project(), m_session->user(), target);
            if (!value.isObject() || !parsed || ids.contains(parsed->uuid)
                || (target && raw.value("default_topic_uuid").toString() != expectedDefault)) {
                finishChannelCreation(owner, id, ticket, tr("The server returned an invalid channel list.")); return;
            }
            ids.insert(parsed->uuid); found |= target;
            streams.append(*parsed);
        }
        if (!found) {
            finishChannelCreation(owner, id, ticket, tr("The channel was created, but it is missing from the channel list. Check status.")); return;
        }
        const auto intent = m_channelCreations.value(owner);
        QVector<Topic> topics;
        for (const auto &value : intent.topics) topics.append(*creationTopicDto(value.toObject(), m_session->project(), id, m_session->user()));
        const QPointer<NativeController> guard(this);
        m_streams = streams;
        ++m_streamRevision;
        m_catalogVerified = true;
        emit streamsChanged();
        if (!guard || !channelCreationContext(owner, id, ticket)) return;
        if (m_streamRevision != streamRevision + 1 || m_channelCreations.value(owner).revision != creationRevision) {
            if (reconcileChannelRead(owner, id, ticket, unsafeRevision, [this, owner, id, ticket] { loadChannelCatalog(owner, id, ticket); })) return;
            finishChannelCreation(owner, id, ticket, tr("The channel was created, but the channel list changed. Check status.")); return;
        }
        if (m_cache) {
            m_cache->put(m_cacheLease, {NativeCache::Kind::Streams, {}, {}, {}}, rows);
            if (!guard || !channelCreationContext(owner, id, ticket)) return;
            m_cache->put(m_cacheLease, {NativeCache::Kind::Topics, id, {}, {}}, intent.topics);
            if (!guard || !channelCreationContext(owner, id, ticket)) return;
        }
        const auto selection = m_selection;
        // Complete before changing our own route; external route changes during
        // signals below must still suppress the success/navigation credit.
        auto &completed = m_channelCreations[owner];
        completed.busy = false; completed.error.clear(); completed.topics = {};
        activateStream(id, &topics);
        if (!guard) return;
        if (creationOwnerKey() != owner || m_selection != selection + 1 || m_stream != id || !m_topic.isEmpty()) {
            finishChannelCreation(owner, id, ticket, tr("The channel was created. Check status to open it.")); return;
        }
        m_channelCreations[owner].phase = QStringLiteral("ready");
        emit stateChanged();
        if (guard && creationOwnerKey() == owner && m_selection == selection + 1 && m_stream == id && m_topic.isEmpty()
            && m_channelCreations.value(owner).uuid == id && m_channelCreations.value(owner).ticket == ticket)
            emit channelCreated(id, expectedDefault);
    });
}

const NativeController::TopicCreation *NativeController::topicCreation() const
{
    const auto found = m_topicCreations.constFind(creationOwnerKey());
    return found == m_topicCreations.cend() ? nullptr : &found.value();
}

bool NativeController::topicCreationBusy() const { const auto *intent = topicCreation(); return intent && intent->busy; }
bool NativeController::topicCreationUncertain() const { const auto *intent = topicCreation(); return intent && intent->uncertain; }
bool NativeController::canCheckTopicCreation() const
{
    const auto *intent = topicCreation();
    return intent && intent->uncertain && !intent->busy && !intent->deleted && topicCreationAllowed(intent->stream);
}
QString NativeController::topicCreationError() const { const auto *intent = topicCreation(); return intent ? intent->error : QString{}; }
QString NativeController::topicCreationStream() const { const auto *intent = topicCreation(); return intent ? intent->stream : QString{}; }
QString NativeController::topicCreationName() const { const auto *intent = topicCreation(); return intent ? intent->name : QString{}; }
QString NativeController::topicCreationAttemptId() const { const auto *intent = topicCreation(); return intent ? intent->uuid : QString{}; }
QString NativeController::topicCreationContextKey() const
{
    if (!authenticated()) return {};
    const auto context = QJsonDocument(QJsonArray{creationOwnerKey(), QString::number(m_selection),
        QString::number(m_catalog), QString::number(m_api->generation()),
        QString::number(reinterpret_cast<quintptr>(m_cache.data())), m_cacheLease.owner,
        QString::number(m_cacheLease.generation), m_catalogVerified, m_snapshotActive,
        m_syncBlocked, m_cacheForgettingAccount}).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(QCryptographicHash::hash(context, QCryptographicHash::Sha256).toHex());
}

bool NativeController::topicCreationAllowed(const QString &stream) const
{
    return authenticated() && m_catalogVerified && !m_snapshotActive && !m_syncBlocked && !m_busy
        && !m_topicChanging && !m_cacheForgettingAccount
        && std::any_of(m_streams.cbegin(), m_streams.cend(), [&stream](const Stream &item) {
            return item.uuid == stream && !item.archived;
        });
}

bool NativeController::canCreateTopicIn(const QString &stream) const
{
    return topicCreationAllowed(stream) && !topicCreationBusy() && !topicCreationUncertain();
}

void NativeController::interruptTopicCreations()
{
    // Preserve uncertain UUIDs across credentials/route lifetimes. They may
    // already exist on the server, so renewal must never mint a second POST.
    for (auto &intent : m_topicCreations) if (intent.busy) {
        intent.busy = false;
        intent.uncertain = true;
        intent.error = tr("The topic creation status is unknown. Check status before trying again.");
        intent.ticket = ++m_nextTopicCreation;
    }
}

bool NativeController::topicCreationContext(const QString &owner, const QString &id, quint64 ticket)
{
    auto found = m_topicCreations.find(owner);
    if (found == m_topicCreations.end() || found->uuid != id || found->ticket != ticket || !found->busy) return false;
    const auto stream = std::find_if(m_streams.cbegin(), m_streams.cend(), [&found](const Stream &item) {
        return item.uuid == found->stream;
    });
    if (owner == creationOwnerKey() && found->generation == m_api->generation() && found->selection == m_selection
        && found->catalog == m_catalog && found->routeStream == m_stream && found->routeTopic == m_topic
        && found->cache == m_cache && found->lease.owner == m_cacheLease.owner
        && found->lease.generation == m_cacheLease.generation && topicCreationAllowed(found->stream)
        && stream != m_streams.cend() && streamAcl(*stream) == found->acl && !found->deleted) return true;
    finishTopicCreation(owner, id, ticket, tr("The topic creation status is unknown. Check status before trying again."));
    return false;
}

void NativeController::finishTopicCreation(const QString &owner, const QString &id, quint64 ticket,
                                           const QString &error, bool uncertain)
{
    auto found = m_topicCreations.find(owner);
    if (found == m_topicCreations.end() || found->uuid != id || found->ticket != ticket) return;
    found->busy = false;
    found->uncertain = uncertain;
    found->error = error;
    if (owner == creationOwnerKey()) emit stateChanged();
}

void NativeController::noteTopicCreationEvent(const QString &stream, const QString &topic, bool deleted)
{
    auto found = m_topicCreations.find(creationOwnerKey());
    if (found == m_topicCreations.end() || found->stream != stream) return;
    ++found->revision;
    if (deleted && found->uuid == topic) {
        found->deleted = true;
        found->busy = found->uncertain = false;
        found->ticket = ++m_nextTopicCreation;
        found->error = tr("The created topic was deleted.");
    }
}

void NativeController::createTopic(const QString &stream, const QString &name)
{
    const auto owner = creationOwnerKey();
    if (owner.isEmpty() || topicCreationBusy() || topicCreationUncertain()) return;
    if (!m_topicCreations.contains(owner) && m_topicCreations.size() >= 32) {
        // Completed/failed entries can be discarded; unresolved identities cannot.
        for (auto it = m_topicCreations.begin(); it != m_topicCreations.end();) {
            if (!it->busy && !it->uncertain) it = m_topicCreations.erase(it); else ++it;
        }
        if (m_topicCreations.size() >= 32) {
            report(tr("Resolve the earlier topic creation attempts before creating another topic."));
            return;
        }
    }
    const auto trimmed = sourceTrimmed(name);
    auto &intent = m_topicCreations[owner];
    intent = {};
    intent.stream = isUuid(stream) ? stream : QString{};
    intent.name = boundedText(trimmed, 129) ? trimmed : QString{};
    if (trimmed.isEmpty() || !boundedText(trimmed, 128)) {
        intent.error = tr("Enter a topic name with 1 to 128 characters.");
        emit stateChanged();
        return;
    }
    if (!topicCreationAllowed(stream)) {
        intent.error = tr("Wait for the channel list to update before creating a topic.");
        emit stateChanged();
        return;
    }
    intent = {};
    intent.uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    intent.stream = stream;
    intent.name = trimmed;
    intent.generation = m_api->generation();
    intent.selection = m_selection;
    intent.catalog = m_catalog;
    intent.routeStream = m_stream;
    intent.routeTopic = m_topic;
    intent.cache = m_cache;
    intent.lease = m_cacheLease;
    const auto target = std::find_if(m_streams.cbegin(), m_streams.cend(), [&stream](const Stream &item) { return item.uuid == stream; });
    intent.acl = streamAcl(*target);
    intent.ticket = ++m_nextTopicCreation;
    intent.busy = true;
    const auto id = intent.uuid;
    const auto ticket = intent.ticket;
    const QPointer<NativeController> guard(this);
    emit stateChanged();
    if (!guard || !topicCreationContext(owner, id, ticket)) return;
    request("POST", messenger + "/stream_topics/", QJsonObject{{"uuid", id}, {"stream_uuid", stream}, {"name", trimmed}}, {},
        [this, owner, id, ticket](const ApiResult &result) {
        if (!topicCreationContext(owner, id, ticket)) return;
        auto &intent = m_topicCreations[owner];
        const auto parsed = creationTopicDto(result.data.toObject(), m_session->project(), intent.stream, m_session->user());
        if (!result.ok() || !parsed || parsed->uuid != id || parsed->name != intent.name) {
            const bool failed = result.status == 400 || result.status == 401 || result.status == 403 || result.status == 404;
            finishTopicCreation(owner, id, ticket, failed ? tr("Could not create the topic. %1").arg(result.error)
                : tr("The topic creation status is unknown. Check status before trying again."), !failed);
            return;
        }
        intent.confirmed = true;
        confirmTopicCreation(owner, id, ticket);
    });
}

void NativeController::checkTopicCreation()
{
    const auto owner = creationOwnerKey();
    auto found = m_topicCreations.find(owner);
    if (!canCheckTopicCreation() || found == m_topicCreations.end()) return;
    const auto target = std::find_if(m_streams.cbegin(), m_streams.cend(), [&found](const Stream &item) { return item.uuid == found->stream; });
    found->generation = m_api->generation();
    found->selection = m_selection;
    found->catalog = m_catalog;
    found->routeStream = m_stream;
    found->routeTopic = m_topic;
    found->cache = m_cache;
    found->lease = m_cacheLease;
    found->acl = streamAcl(*target);
    found->ticket = ++m_nextTopicCreation;
    found->busy = true;
    found->error.clear();
    const auto id = found->uuid;
    const auto ticket = found->ticket;
    const QPointer<NativeController> guard(this);
    emit stateChanged();
    if (guard && topicCreationContext(owner, id, ticket)) confirmTopicCreation(owner, id, ticket);
}

void NativeController::confirmTopicCreation(const QString &owner, const QString &id, quint64 ticket)
{
    const auto revision = m_topicCreations.value(owner).revision;
    request("GET", messenger + "/stream_topics/" + id, {}, {},
        [this, owner, id, ticket, revision](const ApiResult &result) {
        if (!topicCreationContext(owner, id, ticket)) return;
        auto &intent = m_topicCreations[owner];
        const auto parsed = creationTopicDto(result.data.toObject(), m_session->project(), intent.stream, m_session->user());
        if (!result.ok() || !parsed || parsed->uuid != id || revision != intent.revision) {
            finishTopicCreation(owner, id, ticket, tr("The topic creation status is unknown. Check status before trying again."));
            return;
        }
        intent.confirmed = true;
        const auto stream = intent.stream;
        const QPointer<NativeController> guard(this);
        staleCache(NativeCache::Kind::Topics, stream);
        if (!guard || !topicCreationContext(owner, id, ticket)) return;
        if (stream == m_stream) ++m_topicRequest; // Fence older catalog replies.
        loadCreatedTopicCatalog(owner, id, ticket, revision);
    });
}

void NativeController::loadCreatedTopicCatalog(const QString &owner, const QString &id, quint64 ticket,
                                              quint64 revision, QByteArray marker, QJsonArray rows,
                                              QSet<QByteArray> markers, qint64 bytes)
{
    if (!topicCreationContext(owner, id, ticket)) return;
    const auto stream = m_topicCreations.value(owner).stream;
    QUrlQuery query;
    query.addQueryItem("stream_uuid", stream);
    query.addQueryItem("page_limit", "200");
    if (!marker.isEmpty()) query.addQueryItem("page_marker", QString::fromUtf8(marker));
    request("GET", messenger + "/stream_topics/", {}, query,
        [this, owner, id, ticket, revision, stream, rows, markers, bytes](const ApiResult &result) mutable {
        if (!topicCreationContext(owner, id, ticket)) return;
        if (!result.ok() || !result.data.isArray() || revision != m_topicCreations.value(owner).revision) {
            finishTopicCreation(owner, id, ticket, tr("The topic was created, but the channel list could not be updated. Check status."));
            return;
        }
        bytes += QJsonDocument(result.data.toArray()).toJson(QJsonDocument::Compact).size();
        if (bytes > NativeCacheService::MaximumQueuedBytes) {
            finishTopicCreation(owner, id, ticket, tr("The server returned an invalid topic list."));
            return;
        }
        for (const auto &value : result.data.toArray()) rows.append(value);
        // Row/byte limits alone do not bound empty pages with unique cursors.
        constexpr qsizetype maximumPages = 500;
        if (rows.size() > 100000 || (!result.nextMarker.isEmpty()
            && (markers.contains(result.nextMarker) || markers.size() + 1 >= maximumPages))) {
            finishTopicCreation(owner, id, ticket, tr("The server returned an invalid topic list."));
            return;
        }
        if (!result.nextMarker.isEmpty()) {
            markers.insert(result.nextMarker);
            loadCreatedTopicCatalog(owner, id, ticket, revision, result.nextMarker, rows, markers, bytes);
            return;
        }
        QVector<Topic> topics;
        QSet<QString> ids;
        for (const auto &value : rows) {
            const auto parsed = creationTopicDto(value.toObject(), m_session->project(), stream, m_session->user());
            if (!parsed || ids.contains(parsed->uuid)) {
                finishTopicCreation(owner, id, ticket, tr("The server returned an invalid topic list."));
                return;
            }
            ids.insert(parsed->uuid);
            topics.append(*parsed);
        }
        if (!ids.contains(id)) {
            finishTopicCreation(owner, id, ticket, tr("The topic was created, but the channel list could not be updated. Check status."));
            return;
        }
        const QPointer<NativeController> guard(this);
        if (m_cache) m_cache->put(m_cacheLease, {NativeCache::Kind::Topics, stream, {}, {}}, rows);
        if (!guard || !topicCreationContext(owner, id, ticket)) return;
        const auto selection = m_selection;
        const auto intent = m_topicCreations.value(owner);
        const auto publishedContext = [this, owner, id, ticket, selection, intent](quint64 offset, bool busy) {
            const auto current = m_topicCreations.constFind(owner);
            const auto *target = currentStream();
            return current != m_topicCreations.cend() && current->uuid == id && current->ticket == ticket
                && current->busy == busy && !current->deleted && current->revision == intent.revision
                && creationOwnerKey() == owner && m_api->generation() == intent.generation
                && m_catalog == intent.catalog && m_selection == selection + offset
                && m_stream == intent.stream && m_topic == (offset == 1 ? QString{} : id)
                && m_cache == intent.cache && m_cacheLease.owner == intent.lease.owner
                && m_cacheLease.generation == intent.lease.generation && topicCreationAllowed(intent.stream)
                && target && streamAcl(*target) == intent.acl;
        };
        activateStream(stream, &topics);
        if (!guard) return;
        if (!publishedContext(1, true)) {
            finishTopicCreation(owner, id, ticket, tr("The topic was created. Check status to open it."));
            return;
        }
        selectTopic(id);
        if (!guard) return;
        if (!publishedContext(2, true)) {
            finishTopicCreation(owner, id, ticket, tr("The topic was created. Check status to open it."));
            return;
        }
        auto found = m_topicCreations.find(owner);
        if (found == m_topicCreations.end() || found->uuid != id || found->ticket != ticket) return;
        found->busy = found->uncertain = false;
        found->error.clear();
        emit stateChanged();
        if (!guard) return;
        if (!publishedContext(2, false)) {
            finishTopicCreation(owner, id, ticket, tr("The topic was created. Check status to open it."));
            return;
        }
        emit topicCreated(stream, id);
    });
}

bool NativeController::authoritativeRoute() const
{
    return authenticated() && !m_topicSeedUnverified && (!m_cacheProtection || (m_catalogVerified && m_topicsVerified
        && m_historyVerified && !m_snapshotActive && !m_syncBlocked));
}

void NativeController::notifyTrustChanged()
{
    const QPointer<NativeController> guard(this);
    if (m_topicOpening.active && !topicOpeningContext(m_topicOpening.ticket)) interruptTopicOpening();
    if (!guard) return;
    emit stateChanged();
    if (guard && m_cacheProtection && !m_messages.isEmpty()) emit messagesChanged();
    if (guard) maybeRestoreConversation();
}

bool NativeController::cacheStale() const
{
    return m_cacheProtection && ((m_syncBlocked && (!m_streams.isEmpty() || !m_messages.isEmpty()))
        || (m_cacheHydrated && ((!m_catalogVerified && !m_streams.isEmpty())
            || (!m_usersVerified && !m_names.isEmpty()) || (!m_topicsVerified && !m_topics.isEmpty())
            || (!m_historyVerified && !m_messages.isEmpty()))));
}

void NativeController::closeCache()
{
    const QPointer<NativeController> openingGuard(this);
    interruptTopicOpening();
    if (!openingGuard) return;
    ++m_cacheRevision;
    m_cacheHydrations.clear();
    m_cacheWasAvailable = false;
    // Row-preview lookups belong to the discarded lease, not to whatever opens next.
    m_rowPreviewPending.clear();
    m_rowPreviewInFlight.clear();
    const auto lease = m_cacheLease;
    m_cacheLease = {};
    if (m_cache && m_cache->lease().owner == lease.owner
        && m_cache->lease().generation == lease.generation) m_cache->close();
}

void NativeController::openCache()
{
    const QPointer<NativeController> openingGuard(this);
    interruptTopicOpening();
    if (!openingGuard) return;
    ++m_cacheRevision;
    m_cacheHydrations.clear();
    m_cacheWasAvailable = false;
    m_cacheLease = {};
    const QPointer<NativeController> guard(this);
    const QPointer<NativeCacheService> cache = m_cache;
    if (cache && authenticated() && cache->open(m_api->server(), m_session->project(), m_session->user())
        && guard && cache && m_cache == cache) {
        m_cacheLease = cache->lease();
        // Rows projected before the cache attached already registered their preview misses;
        // a status observer may destroy the controller inside open(), so this stays inside
        // the guard chain that `cache->open` is evaluated under.
        scheduleRowPreviewHydration();
    }
}

void NativeController::markCacheStale()
{
    const QPointer<NativeController> openingGuard(this);
    interruptTopicOpening();
    if (!openingGuard) return;
    ++m_cacheRevision;
    if (m_cache) m_cache->markStale(m_cacheLease);
    if (!m_streams.isEmpty() || !m_messages.isEmpty()) m_cacheHydrated = true;
}

void NativeController::invalidateCache(NativeCache::Kind kind, const QString &stream, const QString &topic)
{
    ++m_cacheRevision;
    if (m_cache) m_cache->invalidate(m_cacheLease, kind, stream, topic);
}

void NativeController::invalidateConversation(const QString &stream, const QString &topic, bool counters)
{
    invalidateCache(NativeCache::Kind::History, stream, topic);
    if (counters) {
        invalidateCache(NativeCache::Kind::Streams);
        invalidateCache(NativeCache::Kind::Topics, stream);
    }
}

void NativeController::staleCache(NativeCache::Kind kind, const QString &stream, const QString &topic)
{
    ++m_cacheRevision;
    if (m_cache) m_cache->markStale(m_cacheLease, kind, stream, topic);
}

void NativeController::staleConversation(const QString &stream, const QString &topic, bool counters)
{
    staleCache(NativeCache::Kind::History, stream, topic);
    if (counters) {
        staleCache(NativeCache::Kind::Streams);
        staleCache(NativeCache::Kind::Topics, stream);
    }
}

NativeController::ProjectionContext NativeController::projectionContext() const
{
    return {m_cache, m_cacheLease, m_api->generation()};
}

void NativeController::projectCanonical(const QJsonObject &raw, NativeCache::ProjectionFields fields,
                                        const ProjectionContext &context)
{
    if (context.cache && context.cache == m_cache && context.generation == m_api->generation() &&
        context.lease.owner == m_cacheLease.owner && context.lease.generation == m_cacheLease.generation)
        context.cache->projectMessage(context.lease, raw, fields);
}

void NativeController::observeMessage(const QJsonObject &raw)
{
    const auto id = raw.value("uuid").toString();
    auto &fields = m_messageFields[id];
    const auto hash = QCryptographicHash::hash(
        QJsonDocument(raw.value("payload").toObject()).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256);
    const bool starred = raw.value("starred").toBool();
    if (!fields.initialized || fields.payloadHash != hash)
        fields.payload = ++m_nextMessageField;
    if (!fields.initialized || fields.starred != starred)
        fields.star = ++m_nextMessageField;
    fields.payloadHash = hash;
    fields.starred = starred;
    fields.initialized = true;
}

void NativeController::pruneMessageFields()
{
    QSet<QString> loaded;
    for (const auto &message : m_messages) {
        loaded.insert(message.uuid);
        if (!m_messageFields.contains(message.uuid)) {
            QJsonObject payload{{"kind", message.kind}};
            if (message.kind == "markdown")
                payload["content"] = message.content;
            observeMessage({{"uuid", message.uuid}, {"payload", payload}, {"starred", message.starred}});
        }
    }
    for (auto it = m_messageFields.begin(); it != m_messageFields.end();)
        if (it->pending == 0 && !loaded.contains(it.key()))
            it = m_messageFields.erase(it);
        else
            ++it;
}

void NativeController::finishMessageIntent(const QString &id)
{
    auto it = m_messageFields.find(id);
    if (it != m_messageFields.end() && it->pending > 0)
        --it->pending;
    pruneMessageFields();
}

void NativeController::staleReadConversation(const QString &stream, const QString &topic)
{
    staleCache(NativeCache::Kind::History, stream, topic);
    staleCache(NativeCache::Kind::Streams);
    staleCache(NativeCache::Kind::Topics, stream);
}

bool NativeController::durableDraftRoute() const
{
    const auto *stream = currentStream();
    return m_draftStore && m_draftStore->owner() == creationOwnerKey()
        && !m_draftOwner.isEmpty() && stream && !stream->encrypted
        && m_draftRoutes.contains(conversationKey());
}

QString NativeController::draft() const
{
    // An open edit session owns the composer text; the stored draft waits underneath it.
    if (!m_editingMessage.isEmpty()) return m_editBuffer;
    if (durableDraftRoute()) return m_draftStore->text(m_stream, m_topic);
    const auto *stream = currentStream();
    if (stream && !stream->encrypted && m_draftFallbackOwner == creationOwnerKey()
        && m_draftFallbackRoute == conversationKey()) return m_draftFallback;
    return m_drafts.value(conversationKey());
}

NativeController::DraftMemory NativeController::draftMemory() const
{
    DraftMemory result{int(m_drafts.size()), int(m_draftRoutes.size()), 0};
    for (const auto &text : m_drafts) result.textBytes += text.size() * sizeof(QChar);
    if (!m_draftFallbackRoute.isEmpty()) { ++result.entries; result.textBytes += m_draftFallback.size() * sizeof(QChar); }
    return result;
}

QString NativeController::notice() const
{
    QStringList notices;
    const auto attachment = m_attachmentNotices.value(conversationKey());
    if (!attachment.isEmpty()) notices.append(attachment);
    if (!m_notice.isEmpty()) notices.append(m_notice);
    if (m_draftStore && (m_draftFlushFailed || m_draftStore->failure() != NativeDraftService::Failure::None))
        notices.append(tr("The draft could not be saved on this computer. Your input is still in memory. Retry before quitting."));
    return notices.join('\n');
}

QVariantMap NativeController::draftPage() const
{
    if (!m_draftStore || !authenticated() || m_draftStore->owner() != creationOwnerKey())
        return {{"rows", QVariantList{}}, {"loading", false}, {"error", QString{}}, {"canNext", false}, {"canPrevious", false}};
    auto page = m_draftStore->page(); QVariantList rows;
    static const QStringList phases{"local", "saving", "saved", "failed", "conflict", "deleting"};
    for (const auto &value : page.value("rows").toList()) {
        auto row = value.toMap(); const auto streamId = row.value("stream").toString(), topicId = row.value("topic").toString();
        const auto stream = std::find_if(m_streams.cbegin(), m_streams.cend(), [&](const Stream &s) { return s.uuid == streamId; });
        if (stream == m_streams.cend() || stream->encrypted) continue;
        const bool direct = stream->privateStream && isUuid(stream->directUser);
        const auto name = sourceTrimmed(stream->name).isEmpty() ? streamId : sourceTrimmed(stream->name);
        row["streamName"] = name; row["topicName"] = topicId; row["direct"] = direct; row["initial"] = name.left(1);
        if (streamId == m_stream) for (const auto &topic : m_topics) if (topic.uuid == topicId) row["topicName"] = topic.name;
        row["avatarUrl"] = direct ? userAvatar(stream->directUser) : QString{};
        row["avatarColor"] = !direct && stream->color ? QString("#%1").arg(*stream->color, 6, 16, QLatin1Char('0')) : QString{};
        const auto time = QDateTime::fromMSecsSinceEpoch(row.value("updatedAt").toLongLong()).toLocalTime();
        const auto today = QDate::currentDate();
        row["time"] = time.date() == today ? time.toString("HH:mm")
            : time.date() == today.addDays(-1) ? tr("Yesterday") + ' ' + time.toString("HH:mm")
            : QLocale::system().toString(time.date(), QLocale::system().language() == QLocale::Russian ? "d MMM" : "MMM d");
        row["phase"] = phases.value(row.value("phase").toInt()); rows.append(row);
    }
    page["rows"] = rows;
    const auto remote = m_draftStore->remoteStatus();
    page["loading"] = page.value("loading").toBool() || remote.busy;
    page["error"] = page.value("error").toBool() || remote.failure != NativeDrafts::RemoteFailure::None
        ? tr("Drafts could not be synchronized. Refresh to retry.") : QString{};
    return page;
}

// ---- Activity mentions page (#42, docs/NATIVE_DESKTOP_ACTIVITY_MENTIONS.md) ----
//
// Reference contract (workspace_ui 61dacef78dab2f5d0be76f283bebf49fe29b84af):
// queries and page sizes per activity-mentions.api.ts:20-21,60-134; collection
// and cursor semantics per activity-page.ui.tsx:178-247,411-516.

NativeController::MentionCollection &NativeController::mentionCollection(const QString &filter)
{
    return m_mentionsCollections[filter == QLatin1String("unread") ? QStringLiteral("unread")
                                                                   : QStringLiteral("all")];
}

void NativeController::closeOtherContentPages(ContentPage keep)
{
    if (keep != ContentPage::Drafts)
        closeDrafts();
    if (keep != ContentPage::Mentions)
        closeMentions();
    if (keep != ContentPage::Inbox)
        closeInbox();
}

void NativeController::showMentions()
{
    if (!authenticated())
        return;
    closeOtherContentPages(ContentPage::Mentions);
    m_mentionsVisible = true;
    auto &collection = mentionCollection(m_mentionsFilter);
    // Initial load blocks the list; a re-open keeps cached rows and refreshes
    // (activity-page.ui.tsx:283-297 hasCurrentMessages branch).
    if (collection.rows.isEmpty() && !collection.inflight)
        collection.loading = true;
    emit stateChanged();
    loadMentionCollection(m_mentionsFilter, false);
    if (!m_mentionIndexReady && !m_mentionIndexWalking)
        loadMentionIndex();
}

void NativeController::closeMentions()
{
    if (!m_mentionsVisible)
        return;
    m_mentionsVisible = false;
    emit stateChanged();
}

void NativeController::selectMentionsFilter(const QString &filter)
{
    const auto normalized = filter == QLatin1String("unread") ? QStringLiteral("unread")
                                                              : QStringLiteral("all");
    if (normalized == m_mentionsFilter)
        return;
    m_mentionsFilter = normalized;
    auto &collection = mentionCollection(normalized);
    if (collection.rows.isEmpty() && !collection.inflight)
        collection.loading = true;
    emit stateChanged();
    loadMentionCollection(normalized, false);
}

void NativeController::loadMoreMentions()
{
    const auto found = m_mentionsCollections.constFind(m_mentionsFilter);
    if (found == m_mentionsCollections.cend())
        return;
    const auto &collection = *found;
    if (!collection.hasMore || collection.inflight || collection.loading
        || collection.marker.isEmpty())
        return;
    loadMentionCollection(m_mentionsFilter, true);
}

void NativeController::retryMentions()
{
    auto &collection = mentionCollection(m_mentionsFilter);
    collection.error = false;
    collection.paginationError = false;
    if (collection.rows.isEmpty() && !collection.inflight)
        collection.loading = true;
    emit stateChanged();
    loadMentionCollection(m_mentionsFilter, false);
}

void NativeController::loadMentionCollection(const QString &filter, bool append)
{
    auto &collection = mentionCollection(filter);
    if (collection.inflight)
        return;
    collection.inflight = true;
    const quint64 version = ++collection.version;
    QUrlQuery query;
    query.addQueryItem("mentioned", "true");
    if (filter == QLatin1String("unread"))
        query.addQueryItem("read", "false");
    query.addQueryItem("sort_key", "created_at");
    query.addQueryItem("sort_dir", "desc");
    query.addQueryItem("page_limit", "50");
    const auto marker = append ? collection.marker : QByteArray();
    if (!marker.isEmpty())
        query.addQueryItem("page_marker", QString::fromUtf8(marker));
    request("GET", messenger + "/messages/", {}, query,
        [this, filter, append, version](const ApiResult &result) {
        const auto found = m_mentionsCollections.find(filter);
        if (found == m_mentionsCollections.end())
            return;
        auto &state = *found;
        if (state.version != version)
            return; // a newer refresh owns the state now
        state.inflight = false;
        if (!result.ok() || !result.data.isArray()) {
            state.loading = false;
            if (append)
                state.paginationError = true;
            else
                state.error = true;
            emit stateChanged();
            return;
        }
        QVector<Message> page;
        for (const auto &value : result.data.toArray()) {
            const auto parsed = parseMessage(value.toObject(), m_session->project());
            // A page generated before a confirmed deletion must not reinsert
            // the deleted card (issue #56).
            if (parsed && !m_deletedMentionIds.contains(parsed->uuid))
                page.append(*parsed);
        }
        if (append)
            state.rows += page; // dedupe happens at row build
        else
            state.rows = page;
        state.marker = result.nextMarker;
        state.hasMore = !result.nextMarker.isEmpty();
        state.loading = false;
        state.error = false;
        state.paginationError = false;
        emit stateChanged();
    });
}

void NativeController::loadMentionIndex()
{
    m_mentionIndexWalking = true;
    QUrlQuery query;
    query.addQueryItem("mentioned", "true");
    query.addQueryItem("read", "false");
    query.addQueryItem("sort_key", "created_at");
    query.addQueryItem("sort_dir", "desc");
    query.addQueryItem("page_limit", "100");
    if (!m_mentionIndexMarker.isEmpty())
        query.addQueryItem("page_marker", QString::fromUtf8(m_mentionIndexMarker));
    request("GET", messenger + "/messages/", {}, query, [this](const ApiResult &result) {
        if (!result.ok() || !result.data.isArray()) {
            // Fallback rule (activity-page.ui.tsx:236-237, test 556): while the
            // index is unavailable the row's own read flag decides the dot.
            m_mentionIndexWalking = false;
            m_mentionIndexMarker.clear();
            emit stateChanged();
            return;
        }
        for (const auto &value : result.data.toArray()) {
            const auto parsed = parseMessage(value.toObject(), m_session->project());
            if (parsed)
                m_unreadMentions.insert(parsed->uuid,
                    {parsed->stream, parsed->topic, parsed->createdMicros});
        }
        if (!result.nextMarker.isEmpty() && !m_mentionIndexMarkers.contains(result.nextMarker)) {
            m_mentionIndexMarkers.insert(result.nextMarker);
            m_mentionIndexMarker = result.nextMarker;
            loadMentionIndex();
            return;
        }
        m_mentionIndexMarker.clear();
        m_mentionIndexWalking = false;
        m_mentionIndexReady = true;
        emit stateChanged();
    });
}

bool NativeController::mentionUnread(const Message &message) const
{
    return m_mentionIndexReady ? m_unreadMentions.contains(message.uuid) : !message.read;
}

bool NativeController::mentionIndexRemove(const QString &uuid)
{
    return m_unreadMentions.remove(uuid) > 0;
}

bool NativeController::mentionIndexUpsert(const Message &message)
{
    const auto existing = m_unreadMentions.constFind(message.uuid);
    if (existing != m_unreadMentions.cend() && existing->stream == message.stream
        && existing->topic == message.topic && existing->createdMicros == message.createdMicros)
        return false;
    m_unreadMentions.insert(message.uuid, {message.stream, message.topic, message.createdMicros});
    return true;
}

// A read marker consumes the boundary message plus every earlier index entry in
// the same topic (read-boundary mutation, activity-realtime-applier.lib.ts:70-76
// applied via activity.model.ts applyUnreadMentionMutation).
bool NativeController::mentionIndexBoundary(const Message &message)
{
    bool changed = mentionIndexRemove(message.uuid);
    for (auto it = m_unreadMentions.begin(); it != m_unreadMentions.end();) {
        if (it->stream == message.stream && it->topic == message.topic
            && it->createdMicros <= message.createdMicros) {
            it = m_unreadMentions.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    return changed;
}

bool NativeController::mentionIndexClearTopic(const QString &stream, const QString &topic)
{
    bool changed = false;
    for (auto it = m_unreadMentions.begin(); it != m_unreadMentions.end();) {
        if (it->stream == stream && it->topic == topic) {
            it = m_unreadMentions.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    return changed;
}

bool NativeController::mentionIndexClearStream(const QString &stream)
{
    bool changed = false;
    for (auto it = m_unreadMentions.begin(); it != m_unreadMentions.end();) {
        if (it->stream == stream) {
            it = m_unreadMentions.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    return changed;
}

void NativeController::mentionLiveUpsert(const Message &message)
{
    for (auto &live : m_liveMentions) {
        if (live.uuid == message.uuid) {
            live = message;
            return;
        }
    }
    m_liveMentions.prepend(message);
    if (m_liveMentions.size() > 500)
        m_liveMentions.removeLast();
}

bool NativeController::mentionLiveRemove(const QString &uuid)
{
    const auto before = m_liveMentions.size();
    m_liveMentions.erase(std::remove_if(m_liveMentions.begin(), m_liveMentions.end(),
        [&](const Message &live) { return live.uuid == uuid; }), m_liveMentions.end());
    return m_liveMentions.size() != before;
}

// Issue #56: a confirmed deletion is final - it must not behave like a read.
// The uuid is tombstoned so an outstanding (or later) collection page cannot
// reinsert the card, and every already-fetched collection row is dropped in
// place so All and Unread shrink without waiting for a refetch. The tombstone
// set is bounded FIFO; message uuids are never reused, so dropping the oldest
// tombstone can only revive a row the server no longer serves anyway.
bool NativeController::mentionCollectionsPrune(const QString &uuid)
{
    static constexpr int kTombstoneCap = 4096;
    if (!m_deletedMentionIds.contains(uuid)) {
        m_deletedMentionIds.insert(uuid);
        m_deletedMentionOrder.append(uuid);
        while (m_deletedMentionOrder.size() > kTombstoneCap)
            m_deletedMentionIds.remove(m_deletedMentionOrder.takeFirst());
    }
    bool changed = false;
    for (auto &collection : m_mentionsCollections) {
        const auto before = collection.rows.size();
        collection.rows.erase(std::remove_if(collection.rows.begin(), collection.rows.end(),
            [&](const Message &row) { return row.uuid == uuid; }), collection.rows.end());
        changed = changed || collection.rows.size() != before;
    }
    return changed;
}

QString NativeController::mentionPreview(const Message &message) const
{
    if (message.kind != QLatin1String("markdown"))
        return tr("Encrypted message — key support is under development");
    // Activity cards summarize the body on one flow: whitespace collapsed and
    // clamped to 80 characters with a trailing ellipsis (activity-message-card
    // .ui.tsx:13-17 ACTIVITY_MESSAGE_SUMMARY_OPTIONS maxLength 80; the full
    // Markdown-AST summary is intentionally not duplicated here).
    auto text = message.content;
    text = text.simplified();
    if (text.size() > 80)
        text = text.left(77).trimmed() + QStringLiteral("...");
    return text;
}

QString NativeController::mentionTime(const QDateTime &created) const
{
    const auto local = created.toLocalTime();
    const auto today = QDate::currentDate();
    if (local.date() == today)
        return local.toString("HH:mm");
    if (local.date() == today.addDays(-1))
        return tr("Yesterday") + ' ' + local.toString("HH:mm");
    return QLocale::system().toString(local.date(),
        QLocale::system().language() == QLocale::Russian ? "d MMM" : "MMM d");
}

QVariantList NativeController::mentionRows(const QString &filter) const
{
    // Newest first with uuid descending tie break, deduped by uuid; a fetched
    // row supersedes a live row of the same uuid (activity-page.ui.tsx:91-101,
    // where the page list is inserted after live messages into the uuid map).
    QMap<QString, Message> byUuid;
    // Live mentions first: the uuid map is filled live-then-fetched so a
    // fetched row supersedes a live row of the same uuid
    // (activity-page.ui.tsx:232-247). In the Unread collection a live row is
    // shown per the unread rule; fetched rows are already server-filtered.
    const auto unreadFilter = filter == QLatin1String("unread");
    for (const auto &live : m_liveMentions) {
        if (unreadFilter && !mentionUnread(live))
            continue;
        byUuid.insert(live.uuid, live);
    }
    const auto found = m_mentionsCollections.constFind(filter);
    if (found != m_mentionsCollections.cend())
        for (const auto &message : found->rows)
            byUuid.insert(message.uuid, message);
    QVector<Message> merged;
    merged.reserve(byUuid.size());
    for (auto it = byUuid.cbegin(), end = byUuid.cend(); it != end; ++it)
        merged.append(*it);
    std::sort(merged.begin(), merged.end(), [](const Message &left, const Message &right) {
        if (left.createdMicros != right.createdMicros)
            return left.createdMicros > right.createdMicros;
        return left.uuid > right.uuid;
    });
    QVariantList rows;
    for (const auto &message : std::as_const(merged)) {
        const auto stream = std::find_if(m_streams.cbegin(), m_streams.cend(),
            [&](const Stream &candidate) { return candidate.uuid == message.stream; });
        QString topicName;
        for (const auto &topic : m_topics)
            if (topic.uuid == message.topic) topicName = topic.name;
        const bool direct = stream != m_streams.cend() && stream->privateStream
            && isUuid(stream->directUser);
        const auto streamName = stream != m_streams.cend()
            ? (sourceTrimmed(stream->name).isEmpty() ? stream->uuid : sourceTrimmed(stream->name))
            : QString{};
        rows.append(QVariantMap{
            {"id", message.uuid}, {"stream", message.stream}, {"topic", message.topic},
            {"streamName", streamName}, {"topicName", topicName}, {"direct", direct},
            {"author", userDisplayName(message.author,
                message.own ? tr("You") : QStringLiteral("#") + message.author.left(8))},
            {"unread", mentionUnread(message)}, {"time", mentionTime(message.created)},
            {"preview", mentionPreview(message)},
        });
    }
    return rows;
}

QVariantMap NativeController::mentionsPage() const
{
    QVariantMap page{{"rows", QVariantList{}}, {"filter", m_mentionsFilter},
        {"loading", false}, {"error", false}, {"paginationError", false},
        {"canLoadMore", false}};
    const auto found = m_mentionsCollections.constFind(m_mentionsFilter);
    if (found == m_mentionsCollections.cend())
        return page;
    const auto &collection = *found;
    page["rows"] = mentionRows(m_mentionsFilter);
    page["loading"] = collection.loading;
    page["error"] = collection.error;
    page["paginationError"] = collection.paginationError;
    page["canLoadMore"] = collection.hasMore && !collection.loading;
    return page;
}

void NativeController::resetMentions()
{
    m_mentionsVisible = false;
    m_mentionsFilter = QStringLiteral("all");
    m_mentionsCollections.clear();
    m_unreadMentions.clear();
    m_liveMentions.clear();
    m_deletedMentionIds.clear();
    m_deletedMentionOrder.clear();
    m_mentionIndexReady = false;
    m_mentionIndexWalking = false;
    m_mentionIndexMarker.clear();
    m_mentionIndexMarkers.clear();
}

// ---- Inbox page (issue #44, docs/NATIVE_DESKTOP_INBOX.md) ----

// Mirrors formatCount(count, UNREAD_COUNT_DISPLAY_LIMIT) in
// workspace_ui shared/lib/count-format.lib.ts: only counts ABOVE 1000
// compact into floor(count/1000) + "k+" (1000 -> "1000", 1001 -> "1k+").
static QString inboxCountText(qint64 count)
{
    return count > 1000 ? QString::number(count / 1000) + QStringLiteral("k+")
                        : QString::number(count);
}

static QDateTime inboxUpdated(qint64 microsEpoch)
{
    return QDateTime::fromMSecsSinceEpoch(microsEpoch / 1000, Qt::UTC);
}

void NativeController::showInbox()
{
    if (!authenticated())
        return;
    closeOtherContentPages(ContentPage::Inbox);
    m_inboxVisible = true;
    emit stateChanged();
    hydrateInboxTopics();
}

void NativeController::closeInbox()
{
    if (!m_inboxVisible)
        return;
    m_inboxVisible = false;
    emit stateChanged();
}

void NativeController::openInboxRow(const QString &stream, const QString &topic)
{
    // The reference row click navigates and the page unmounts
    // (inbox-page.ui.tsx:108-110). The fallback row carries no topic target
    // and routes to the stream instead.
    closeInbox();
    if (!topic.isEmpty() && isUuid(topic)) {
        // The inbox slot row is the verified owned DTO: hand it to the
        // topic-opening machinery directly (the owned-canonical path of
        // beginTopicOpening) instead of re-fetching topic metadata.
        const auto slot = m_inboxTopics.constFind(stream);
        if (slot != m_inboxTopics.cend())
            for (const auto &row : *slot)
                if (row.uuid == topic && row.stream == stream) {
                    beginTopicOpening(topic.toLower(), row);
                    return;
                }
        openTopic(topic, stream);
    } else {
        selectStream(stream);
    }
}

void NativeController::hydrateInboxTopics()
{
    if (!authenticated())
        return;
    for (const auto &stream : m_streams) {
        // Reference hydration sweeps every stream whose active counter says
        // there is unread traffic; passive-only streams never hydrate a slot.
        if (!stream.archived && stream.activeUnread > 0)
            hydrateInboxStream(stream.uuid);
    }
}

void NativeController::hydrateInboxStream(const QString &streamUuid)
{
    if (!authenticated() || !isUuid(streamUuid))
        return;
    if (m_inboxTopics.contains(streamUuid) || m_inboxHydrating.contains(streamUuid))
        return;
    // An unknown or archived stream is not a hydratable destination; the row
    // correctly stays out of the Inbox until the catalog knows the stream.
    if (!std::any_of(m_streams.cbegin(), m_streams.cend(),
            [&streamUuid](const Stream &stream) {
                return stream.uuid == streamUuid && !stream.archived; }))
        return;
    const auto owner = creationOwnerKey();
    const auto generation = m_api->generation();
    const auto catalog = m_catalog;
    // #64: every terminal outcome (delivery, stale drop, request failure)
    // releases only its OWN registration, identified by a per-attempt token.
    // A late completion from a previous attempt can never clear a newer
    // attempt's marker, so reopen can retry a failed hydration while an
    // in-flight successful one stays deduplicated.
    const auto token = ++m_inboxHydrationToken;
    m_inboxHydrating.insert(streamUuid, token);
    const auto settle = [this, streamUuid, token] {
        if (m_inboxHydrating.value(streamUuid) == token)
            m_inboxHydrating.remove(streamUuid);
    };
    QUrlQuery query;
    query.addQueryItem("stream_uuid", streamUuid);
    cachedCollection({NativeCache::Kind::Topics, streamUuid, {}, {}},
        messenger + "/stream_topics/", query,
        [this, streamUuid, owner, generation, catalog, settle](const QJsonArray &rows, bool) {
        // Stale-owner guard: an owner switch or catalog epoch change may
        // not write into the new inbox (same contract as mentions).
        if (!authenticated() || creationOwnerKey() != owner || m_api->generation() != generation
            || catalog != m_catalog) {
            settle();
            return false;
        }
        QVector<Topic> topics;
        QSet<QString> ids;
        for (const auto &value : rows) {
            const auto topic = parseTopic(value.toObject(), m_session->project(), streamUuid);
            // A malformed row cannot poison the projection; the sidebar
            // path for the same stream still reports it if selected.
            if (!topic || ids.contains(topic->uuid)) continue;
            // Reconcile the snapshot against authoritative events observed
            // while this fetch was in flight (#65): tombstones and moves to
            // another stream drop the row; a newer event DTO owns it.
            if (m_inboxDeletedTopics.contains(topic->uuid)) continue;
            const auto pending = m_inboxPlacementEvents.constFind(topic->uuid);
            if (pending != m_inboxPlacementEvents.cend()) {
                if (pending->stream != streamUuid) continue;
                ids.insert(topic->uuid);
                // Only a STRICTLY newer event DTO overrides the snapshot; on
                // a timestamp tie the fetched row reflects the later state
                // (reads do not bump updated_at), so the snapshot wins.
                topics.append(topic->updatedMicros >= pending->updatedMicros ? *topic : *pending);
                continue;
            }
            ids.insert(topic->uuid);
            topics.append(*topic);
        }
        // Moves INTO this stream recorded while the fetch was in flight: a
        // pre-move snapshot cannot contain those rows (#65). Entries are kept
        // until the session reset so a late source-side snapshot stays
        // filtered even after the destination slot has landed.
        for (auto it = m_inboxPlacementEvents.cbegin(); it != m_inboxPlacementEvents.cend(); ++it) {
            if (m_inboxDeletedTopics.contains(it.key())) continue;
            if (it.value().stream != streamUuid || ids.contains(it.key())) continue;
            ids.insert(it.key());
            topics.append(it.value());
        }
        m_inboxTopics.insert(streamUuid, topics);
        settle();
        emit stateChanged();
        return true;
    }, false, settle);
}

bool NativeController::inboxSlotTopicEvent(const QString &kind, const Topic &topic, bool ownsPlacement)
{
    if (kind == QLatin1String("topic.deleted")) {
        // A tombstone removes the entity from EVERY slot that still lists it:
        // the topic is gone, so an unreconciled stale placement must not
        // survive under a former stream (#65). The marker also keeps an
        // in-flight slot snapshot from resurrecting the row when it lands.
        m_inboxDeletedTopics.insert(topic.uuid);
        bool removed = false;
        for (auto slot = m_inboxTopics.begin(); slot != m_inboxTopics.end(); ++slot) {
            auto &rows = slot.value();
            const auto before = rows.size();
            rows.erase(std::remove_if(rows.begin(), rows.end(),
                [&topic](const Topic &row) { return row.uuid == topic.uuid; }), rows.end());
            removed |= rows.size() != before;
        }
        if (removed && m_inboxVisible) emit stateChanged();
        return removed;
    }
    if (!ownsPlacement) {
        // Non-authoritative events (counter updates, incomplete DTOs) only
        // touch the slot the payload names; they never move a row (#65).
        const auto slot = m_inboxTopics.find(topic.stream);
        if (slot == m_inboxTopics.end())
            return false;
        auto &rows = *slot;
        const auto found = std::find_if(rows.begin(), rows.end(),
            [&topic](const Topic &item) { return item.uuid == topic.uuid; });
        if (found != rows.end())
            *found = topic; // The DTO owns the counters (topic.read ships zeroes).
        else if (kind == QLatin1String("topic.created"))
            rows.append(topic);
        else
            return false;
        if (m_inboxVisible) emit stateChanged();
        return true;
    }
    // A complete owned event owns the row's placement across every slot: a
    // whole-topic move vacates the former stream so the stale card cannot
    // survive, and openInboxRow can never hand the obsolete DTO to
    // beginTopicOpening as verified canonical metadata (#65). The record
    // also reconciles slot snapshots that were fetched before this event.
    m_inboxPlacementEvents.insert(topic.uuid, topic);
    bool changed = false;
    for (auto slot = m_inboxTopics.begin(); slot != m_inboxTopics.end(); ++slot) {
        if (slot.key() == topic.stream) continue;
        auto &rows = slot.value();
        const auto before = rows.size();
        rows.erase(std::remove_if(rows.begin(), rows.end(),
            [&topic](const Topic &row) { return row.uuid == topic.uuid; }), rows.end());
        changed |= rows.size() != before;
    }
    const auto dest = m_inboxTopics.find(topic.stream);
    if (dest == m_inboxTopics.end()) {
        // The destination slot is not hydrated: one DTO cannot stand in for
        // the complete snapshot the slot contract promises, so refetch the
        // destination instead of fabricating a partial one. Only the open
        // page consumes slots, so a closed inbox just leaves the row
        // vacated; the showInbox sweep hydrates the missing destination.
        // A destination the catalog does not know leaves no row (invalidate).
        if (m_inboxVisible)
            hydrateInboxStream(topic.stream);
        if (changed && m_inboxVisible) emit stateChanged();
        return changed;
    }
    auto &rows = *dest;
    const auto found = std::find_if(rows.begin(), rows.end(),
        [&topic](const Topic &item) { return item.uuid == topic.uuid; });
    if (found != rows.end())
        *found = topic;
    else
        rows.append(topic);
    if (m_inboxVisible) emit stateChanged();
    return true;
}

bool NativeController::inboxSlotStreamRead(const QString &stream)
{
    const auto slot = m_inboxTopics.find(stream);
    if (slot == m_inboxTopics.end())
        return false;
    for (auto &topic : *slot) {
        topic.unread = topic.activeUnread = topic.passiveUnread = 0;
        topic.lastMessage.clear();
    }
    if (m_inboxVisible) emit stateChanged();
    return true;
}

void NativeController::resetInbox()
{
    m_inboxVisible = false;
    m_inboxTopics.clear();
    m_inboxHydrating.clear();
    m_inboxPlacementEvents.clear();
    m_inboxDeletedTopics.clear();
}

QVariantMap NativeController::inboxPage() const
{
    QVariantList direct, channels;
    if (authenticated()) {
        const auto user = m_session->user();
        for (const auto &stream : m_streams) {
            if (stream.archived) continue;
            // resolveWorkspaceActiveUnreadCount (notification-mode lib:37-41)
            // resolves active ?? unread; the native parser already normalizes
            // a missing active counter to 0 (design doc resolver note).
            const auto streamActive = stream.activeUnread;
            QVector<const Topic *> unreadTopics;
            const auto slot = m_inboxTopics.constFind(stream.uuid);
            if (slot != m_inboxTopics.cend()) {
                for (const auto &topic : *slot)
                    if (topic.activeUnread > 0) unreadTopics.append(&topic);
            } else if (stream.uuid == m_stream) {
                for (const auto &topic : m_topics)
                    if (topic.activeUnread > 0) unreadTopics.append(&topic);
            }
            // hasUnread = stream active OR any topic active
            // (inbox-page.ui.tsx:54-61); passive traffic never qualifies.
            if (streamActive <= 0 && unreadTopics.isEmpty()) continue;
            const bool directStream = stream.privateStream && isUuid(stream.directUser)
                && stream.directUser != user;
            // Same display-title rule as mentionRows(): peer name for DMs,
            // trimmed name with uuid fallback for channels.
            const auto streamTitle = directStream ? peerDisplayName(stream.directUser)
                : (sourceTrimmed(stream.name).isEmpty() ? stream.uuid : sourceTrimmed(stream.name));
            // Unread topics newest-first (uuid desc tie break), matching the
            // recency-based sidebar order the reference rows inherit.
            std::stable_sort(unreadTopics.begin(), unreadTopics.end(),
                [](const Topic *left, const Topic *right) {
                    if (left->updatedMicros != right->updatedMicros)
                        return left->updatedMicros > right->updatedMicros;
                    return left->uuid > right->uuid;
                });
            QVariantList rows;
            for (const auto *topic : unreadTopics) {
                // An empty topic title renders inbox.allMessages
                // (inbox-page.ui.tsx:64-73).
                const auto topicTitle = sourceTrimmed(topic->name).isEmpty()
                    ? tr("All messages") : sourceTrimmed(topic->name);
                rows.append(QVariantMap{
                    {"id", topic->uuid}, {"stream", stream.uuid}, {"topic", topic->uuid},
                    {"title", streamTitle + QStringLiteral(" · ") + topicTitle},
                    {"badge", inboxCountText(topic->activeUnread)},
                    {"time", topic->updatedMicros > 0
                        ? mentionTime(inboxUpdated(topic->updatedMicros)) : QString{}},
                    {"direct", directStream}});
            }
            if (rows.isEmpty() && streamActive > 0)
                // Stream-level unread with no unread topics renders ONE
                // fallback row without a topic target (inbox-page.ui.tsx:86-95).
                rows.append(QVariantMap{
                    {"id", stream.uuid}, {"stream", stream.uuid}, {"topic", QString{}},
                    {"title", streamTitle}, {"badge", inboxCountText(streamActive)},
                    {"time", stream.updatedMicros > 0
                        ? mentionTime(inboxUpdated(stream.updatedMicros)) : QString{}},
                    {"direct", directStream}});
            QVariantMap card{{"id", stream.uuid}, {"title", streamTitle},
                {"direct", directStream}, {"badge", inboxCountText(streamActive)}, {"rows", rows}};
            (directStream ? direct : channels).append(card);
        }
    }
    // Reference state derivation (inbox-page.ui.tsx:196-207): loading while
    // the catalog has not loaded and nothing errored; the error card once the
    // catalog request failed. m_catalogVerified is the native catalogLoaded.
    const bool catalogPending = authenticated() && !m_catalogVerified;
    return QVariantMap{{"direct", direct}, {"channels", channels},
        {"loading", catalogPending && m_error.isEmpty()},
        {"error", catalogPending && !m_error.isEmpty()}};
}

void NativeController::showDrafts()
{
    if (!authenticated() || !m_catalogVerified || !m_draftStore) return;
    closeOtherContentPages(ContentPage::Drafts);
    m_draftsVisible = true; const QPointer<NativeController> guard(this);
    m_draftStore->showPage();
    if (guard && m_draftStore) m_draftStore->refetchRemote();
    if (guard) emit stateChanged();
}
void NativeController::closeDrafts()
{
    if (!m_draftsVisible) return;
    m_draftsVisible = false; if (m_draftStore) m_draftStore->hidePage(); emit stateChanged();
}
void NativeController::refreshDrafts()
{
    if (!m_draftsVisible || !authenticated() || !m_draftStore) return;
    const QPointer<NativeController> guard(this); m_draftStore->showPage();
    if (guard && m_draftStore) m_draftStore->refetchRemote();
}
void NativeController::nextDraftPage() { if (m_draftsVisible && m_draftStore) m_draftStore->nextPage(); }
void NativeController::previousDraftPage() { if (m_draftsVisible && m_draftStore) m_draftStore->previousPage(); }
void NativeController::openDraft(const QString &id)
{
    if (!m_draftStore || !authenticated() || !m_catalogVerified) return;
    QVariantMap selected;
    for (const auto &value : m_draftStore->page().value("rows").toList())
        if (value.toMap().value("id").toString() == id) { selected = value.toMap(); break; }
    if (selected.isEmpty() || selected.value("consumed").toBool()) return;
    const auto stream = selected.value("stream").toString(), topic = selected.value("topic").toString();
    if (std::none_of(m_streams.cbegin(), m_streams.cend(), [&](const Stream &s) { return s.uuid == stream && !s.encrypted; })) return;
    m_draftsVisible = false; m_draftStore->hidePage();
    m_draftOpenId = id; m_draftOpenStream = stream; m_draftOpenTopic = topic;
    m_draftOpenOwner = creationOwnerKey(); m_draftOpenGeneration = m_api->generation();
    m_selectingDraftRoute = true; const QPointer<NativeController> guard(this);
    openTopic(topic, stream);
    if (!guard) return;
    m_selectingDraftRoute = false; synchronizeDrafts(); emit stateChanged();
}
void NativeController::deleteDraft(const QString &id)
{
    if (m_draftsVisible && authenticated() && m_draftStore) m_draftStore->actOnDraft(id, NativeDrafts::Action::Delete);
}
void NativeController::resolveDraft(const QString &id, const QString &action)
{
    if (!m_draftsVisible || !authenticated() || !m_draftStore) return;
    if (action == "server") m_draftStore->actOnDraft(id, NativeDrafts::Action::Server);
    else if (action == "local") m_draftStore->actOnDraft(id, NativeDrafts::Action::Local);
    else if (action == "delete") m_draftStore->actOnDraft(id, NativeDrafts::Action::Delete);
}

void NativeController::reportDraftFlushFailure()
{
    m_draftFlushFailed = true;
    emit stateChanged();
}

void NativeController::synchronizeDrafts()
{
    if (!m_draftStore || m_syncingDrafts) return;
    m_syncingDrafts = true;
    const QPointer<NativeController> guard(this);
    const auto syncing = qScopeGuard([guard] { if (guard) guard->m_syncingDrafts = false; });
    const auto owner = creationOwnerKey();
    if (owner != m_draftOwner) {
        m_draftsVisible = false; m_draftOpenId.clear();
        if (!m_draftOwner.isEmpty()) { m_drafts.clear(); m_draftRevisions.clear(); }
        m_draftFallbackOwner.clear(); m_draftFallbackRoute.clear(); m_draftFallback.clear();
        m_draftOwner = owner; m_draftRoutes.clear(); m_draftFlushFailed = false;
        m_draftStore->setOwner(owner);
        if (!guard || !m_draftStore) return;
    }
    for (const auto &route : m_draftRoutes.values())
        if (!m_draftStore->retained(route.section('/', 0, 0), route.section('/', 1, 1))) m_draftRoutes.remove(route);
    QSet<QString> ordinaryStreams, accessibleStreams;
    if (!owner.isEmpty() && m_catalogVerified)
        for (const auto &row : m_streams) {
            accessibleStreams.insert(row.uuid); if (!row.encrypted) ordinaryStreams.insert(row.uuid);
        }
    m_draftStore->useRemote(!owner.isEmpty() && m_catalogVerified ? m_api : nullptr,
        m_session ? m_session->project() : QString{}, m_session ? m_session->user() : QString{}, ordinaryStreams, accessibleStreams);
    if (!guard || !m_draftStore) return;
    const auto *stream = currentStream();
    const bool verified = !owner.isEmpty() && stream && !stream->encrypted && isUuid(m_topic)
        && m_catalogVerified && m_topicsVerified && !m_topicSeedUnverified
        && std::any_of(m_topics.cbegin(), m_topics.cend(), [this](const Topic &row) { return row.uuid == m_topic; });
    if (verified) {
        const auto route = conversationKey();
        m_draftRoutes.insert(route);
        m_draftStore->select(m_stream, m_topic);
        if (!guard || !m_draftStore) return;
        m_draftFallbackOwner = owner; m_draftFallbackRoute = route;
        m_draftFallback = m_draftStore->text(m_stream, m_topic);
        if (!m_draftOpenId.isEmpty() && m_draftOpenOwner == owner && m_draftOpenGeneration == m_api->generation()
            && m_draftOpenStream == m_stream && m_draftOpenTopic == m_topic) {
            const auto id = m_draftOpenId; m_draftOpenId.clear(); m_draftStore->actOnDraft(id, NativeDrafts::Action::Open);
        }
    } else if (stream && stream->encrypted) m_draftStore->select({}, {});
    if (!m_draftOpenId.isEmpty() && !m_selectingDraftRoute && !m_topicOpening.active && m_topicsVerified
        && (m_draftOpenOwner != owner || m_draftOpenGeneration != m_api->generation()
            || m_draftOpenStream != m_stream || m_draftOpenTopic != m_topic)) m_draftOpenId.clear();
}

void NativeController::useDrafts(NativeDrafts *drafts)
{
    if (m_draftStore == drafts) return;
    disconnect(m_draftChanges); disconnect(m_draftStates); disconnect(m_draftDestroyed); disconnect(m_draftSelection);
    m_draftStore = drafts; m_draftOwner.clear(); m_draftRoutes.clear();
    const QPointer<NativeController> guard(this);
    if (drafts) {
        m_draftChanges = connect(drafts, &NativeDrafts::changed, this,
            [this](const QString &owner, const QString &stream, const QString &topic) {
            if (!m_draftStore || owner != creationOwnerKey() || owner != m_draftOwner) return;
            const auto route = stream + '/' + topic;
            if (!m_draftRoutes.contains(route)) return;
            const QPointer<NativeController> guard(this);
            if (route == conversationKey() && currentStream() && !currentStream()->encrypted) {
                m_draftFallbackOwner = owner; m_draftFallbackRoute = route;
                m_draftFallback = m_draftStore->text(stream, topic);
                emit draftChanged();
            }
            if (guard) emit stateChanged();
        });
        m_draftStates = connect(drafts, &NativeDrafts::stateChanged, this, [this] { emit stateChanged(); });
        m_draftSelection = connect(this, &NativeController::stateChanged, this, &NativeController::synchronizeDrafts);
        m_draftDestroyed = connect(drafts, &QObject::destroyed, this, [this] {
            m_draftStore.clear(); m_draftRoutes.clear(); m_draftOwner.clear();
            emit stateChanged();
        });
        synchronizeDrafts();
    }
    if (!guard) return;
    emit draftChanged();
    if (guard) emit stateChanged();
}

void NativeController::useCache(NativeCacheService *cache)
{
    if (m_cache == cache) return;
    const QPointer<NativeController> lookupGuard(this);
    interruptChatRequestLookups();
    if (!lookupGuard) return;
    interruptChannelCreations();
    interruptDirectChats();
    const QPointer<NativeController> guard(this);
    disconnect(m_cacheDestroyed);
    disconnect(m_cacheStatusChanged);
    closeCache();
    if (!guard) return;
    if (m_cacheForgettingAccount) {
        m_cacheForgettingAccount = false;
        m_error = tr("The local cache could not be cleared. The saved account was kept.");
    }
    m_cache = cache;
    m_cacheProtection = m_cacheProtection || cache;
    m_catalogVerified = m_usersVerified = m_topicsVerified = m_historyVerified = false;
    if (cache) m_cacheDestroyed = connect(cache, &QObject::destroyed, this, [this] {
        const QPointer<NativeController> openingGuard(this);
        interruptTopicOpening();
        if (!openingGuard) return;
        interruptChannelCreations();
        interruptDirectChats();
        m_cache.clear(); m_cacheLease = {}; ++m_cacheRevision;
        m_cacheHydrations.clear(); m_cacheWasAvailable = false;
        m_loading = false;
        if (m_cacheForgettingAccount) {
            m_cacheForgettingAccount = false;
            m_error = tr("The local cache could not be cleared. The saved account was kept.");
        }
        // A discarded worker reply must not strand a catalog/history lookup.
        // Defer recovery so an immediate service replacement can supply it, and
        // never start messenger reads before token renewal has been verified.
        const QPointer<NativeController> guard(this);
        const auto generation = m_api->generation();
        const auto catalog = m_catalog;
        const auto recover = [this, guard, generation, catalog] {
            if (!guard || m_cache || generation != m_api->generation() || catalog != m_catalog
                || !authenticated()) return;
            refresh();
        };
        if (authenticated()) {
            if (m_busy) connect(m_session, &Session::busyChanged, this, [this, recover](bool busy) {
                if (!busy) QTimer::singleShot(0, this, recover);
            }, Qt::SingleShotConnection);
            else QTimer::singleShot(0, this, recover);
        }
        emit stateChanged();
    });
    if (cache) m_cacheStatusChanged = connect(cache, &NativeCacheService::stateChanged, this, [this, cache] {
        if (m_cache != cache) return;
        const bool ready = cache->available();
        const bool becameReady = ready && !m_cacheWasAvailable;
        m_cacheWasAvailable = ready;
        if (!becameReady) return;
        hydrateChatRequestLookups();
        auto pending = std::move(m_cacheHydrations);
        m_cacheHydrations.clear();
        const QPointer<NativeController> guard(this);
        for (const auto &hydrate : pending) { hydrate(); if (!guard) return; }
    });
    if (authenticated()) {
        openCache();
        if (!guard) return;
        refresh();
    }
    if (guard) emit stateChanged();
}

void NativeController::cachedCollection(const NativeCache::Key &key, const QString &path,
    const QUrlQuery &query, std::function<bool(const QJsonArray &, bool)> done, bool forceNetwork,
    std::function<void()> failed)
{
    const auto revision = m_cacheRevision;
    const auto generation = m_api->generation();
    const auto catalog = m_catalog;
    const auto selection = m_selection;
    const auto lease = m_cacheLease;
    const QPointer<NativeCacheService> cache = m_cache;
    const QPointer<NativeController> guard(this);
    const auto networkStarted = std::make_shared<bool>(false);
    const auto authoritativeDelivered = std::make_shared<bool>(false);
    const auto current = [guard, cache, lease, revision] {
        return guard && cache && guard->m_cache == cache && guard->m_cacheRevision == revision
            && guard->m_cacheLease.owner == lease.owner && guard->m_cacheLease.generation == lease.generation;
    };
    const auto network = [this, guard, cache, lease, key, revision, path, query, done, failed,
                          networkStarted, authoritativeDelivered, generation, catalog, selection] {
        if (!guard || *networkStarted) return;
        if (generation != m_api->generation() || catalog != m_catalog
            || (key.kind == NativeCache::Kind::Topics && selection != m_selection)) {
            // The captured epoch is gone and nothing will ever be dispatched:
            // deliver the terminal failure instead of dropping the request (#64).
            if (failed) failed();
            return;
        }
        *networkStarted = true;
        collection(path, query, [this, guard, cache, lease, key, revision, done, authoritativeDelivered](const QJsonArray &rows) {
            if (!guard) return;
            *authoritativeDelivered = true;
            if (!done(rows, true) || !guard) return;
            if (cache && m_cache == cache && m_cacheRevision == revision
                && m_cacheLease.owner == lease.owner && m_cacheLease.generation == lease.generation)
                cache->put(lease, key, rows);
        }, {}, {}, [guard, failed, authoritativeDelivered] {
            // A stale cache snapshot already settled the caller's contract:
            // do not double-report a failure after rows were delivered (#64).
            if (!guard || *authoritativeDelivered) return;
            if (failed) failed();
        });
    };
    const auto fetchCache = [this, guard, cache, lease, key, done, failed, forceNetwork,
                             authoritativeDelivered, current, network](bool savedOnly) {
        if (*authoritativeDelivered) return;
        if (!current()) {
            // Deferred hydration for a dead epoch can never deliver rows:
            // a savedOnly pass owes its caller the failure terminal (#64).
            if (savedOnly && failed) failed();
            return;
        }
        cache->get(lease, key, this, [this, guard, done, failed, forceNetwork, savedOnly,
                    authoritativeDelivered, current, network](std::optional<NativeCache::Entry> entry) {
            if (!guard || *authoritativeDelivered) return;
            if (!current()) {
                if (!savedOnly) network();
                else if (failed) failed();
                return;
            }
            if (entry) {
                const bool fresh = entry->fresh && !savedOnly && !forceNetwork && !m_snapshotActive && !m_syncBlocked;
                if (!fresh) m_cacheHydrated = true;
                if (fresh) *authoritativeDelivered = true;
                if (!done(entry->rows, fresh) || !guard) return;
                if (fresh) return;
            }
            if (!savedOnly) network();
            // A savedOnly deferred pass with no entry has no rows to deliver:
            // close the caller's contract with the failure terminal (#64).
            else if (failed) failed();
        });
    };
    if (!cache || !cache->available()) {
        if (cache) m_cacheHydrations[static_cast<int>(key.kind)] = [fetchCache] { fetchCache(true); };
        network();
        return;
    }
    if (forceNetwork) network();
    fetchCache(false);
}

void NativeController::reportReaction(const QString &message, const QString &id)
{
    m_error = message;
    if (m_reactionIntents.contains(id)) m_reactionIntents[id].error = message;
    const QPointer<NativeController> guard(this);
    emit messagesChanged();
    if (guard) emit stateChanged();
}

void NativeController::resetReactions(bool retainUncertain)
{
    if (!retainUncertain) m_reactionIntents.clear();
    else for (auto it = m_reactionIntents.begin(); it != m_reactionIntents.end();) {
        if (!it->mutated) it = m_reactionIntents.erase(it);
        else { it->checking = false; it->uncertain = true; it->reportAddition = false; ++it; }
    }
    m_ownReactions.clear();
    m_reactionRevisions.clear();
    m_reactionRefreshes.clear();
}

bool NativeController::reactionContext(const QString &id, const QString &nonce)
{
    auto intent = m_reactionIntents.find(id);
    if (intent == m_reactionIntents.end() || intent->nonce != nonce) return false;
    if (intent->generation != m_api->generation()) return false;
    if (intent->owner != ownerKey()) { m_reactionIntents.erase(intent); return false; }
    const auto *message = findMessage(id);
    if (!authenticated() || intent->selection != m_selection || intent->stream != m_stream
        || !message || message->topic != intent->topic || !inConversation(*message)) {
        // No new mutation can follow a stale preflight. An already sent command
        // keeps its identity until a later explicit, GET-only reconciliation.
        if (intent->mutated) { intent->checking = false; intent->uncertain = true; intent->reportAddition = false; }
        else m_reactionIntents.erase(intent);
        emit messagesChanged();
        return false;
    }
    return true;
}

void NativeController::ownReactions(const QString &id,
    std::function<void(bool, const OwnReactions &)> callback, QByteArray marker,
    OwnReactions accumulated, QSet<QByteArray> seen)
{
    QUrlQuery query;
    query.addQueryItem("message_uuid", id);
    query.addQueryItem("user_uuid", m_session->user());
    query.addQueryItem("page_limit", "200");
    if (!marker.isEmpty()) query.addQueryItem("page_marker", QString::fromUtf8(marker));
    request("GET", messenger + "/message_reactions/", {}, query,
        [this, id, callback, accumulated, seen](const ApiResult &result) mutable {
        if (!result.ok() || !result.data.isArray()) { callback(false, {}); return; }
        QSet<QString> ids;
        for (const auto &fact : accumulated) ids.insert(fact.uuid);
        for (const auto &value : result.data.toArray()) {
            const auto fact = parseReactionFact(value.toObject(), m_session->project());
            if (!fact || fact->message != id || fact->user != m_session->user()
                || ids.contains(fact->uuid) || accumulated.contains(fact->emoji)) { callback(false, {}); return; }
            ids.insert(fact->uuid); accumulated.insert(fact->emoji, *fact);
        }
        if (!result.nextMarker.isEmpty()) {
            if (!isUuid(QString::fromUtf8(result.nextMarker)) || seen.contains(result.nextMarker)
                || accumulated.size() > 10000) { callback(false, {}); return; }
            seen.insert(result.nextMarker);
            ownReactions(id, callback, result.nextMarker, accumulated, seen);
        } else callback(true, accumulated);
    });
}

void NativeController::toggleReaction(const QString &id, const QString &emoji)
{
    const auto *message = findMessage(id);
    if (!authoritativeRoute() || !message || !inConversation(*message)
        || m_busy || m_loading || m_snapshotActive || !validReactionName(emoji)) return;
    auto pending = m_reactionIntents.find(id);
    if (pending != m_reactionIntents.end()) {
        if (pending->checking || !pending->mutated) return;
        // An uncertain command is never inverted or retried. Only its read-back
        // can be retried, even if the user clicks another emoji while unresolved.
        pending->selection = m_selection;
        pending->generation = m_api->generation();
        pending->checking = true; pending->uncertain = false; pending->attempts = 0;
        pending->reportAddition = false;
        const auto nonce = pending->nonce;
        const QPointer<NativeController> guard(this);
        emit messagesChanged();
        if (guard) confirmReaction(id, nonce);
        return;
    }
    auto name = message->reactions.contains(emoji) ? emoji : canonicalReactionName(emoji);
    if (name.isEmpty()) {
        // A future provider emoji can be removed using its exact owned fact UUID;
        // it cannot be created from arbitrary names supplied by untrusted content.
        if (!message->reactions.contains(emoji)) return;
        name = emoji;
    }
    ReactionIntent intent;
    intent.nonce = QUuid::createUuid().toString(QUuid::WithoutBraces);
    intent.emoji = name; intent.stream = message->stream; intent.topic = message->topic; intent.selection = m_selection;
    intent.generation = m_api->generation(); intent.owner = ownerKey();
    m_reactionIntents.insert(id, intent);
    m_error.clear();
    const QPointer<NativeController> guard(this);
    emit messagesChanged();
    if (!guard) return;
    emit stateChanged();
    if (!guard || !reactionContext(id, intent.nonce)) return;
    ownReactions(id, [this, id, nonce = intent.nonce](bool ok, const OwnReactions &facts) {
        if (!reactionContext(id, nonce)) return;
        auto &current = m_reactionIntents[id];
        if (!ok || (!facts.contains(current.emoji) && canonicalReactionName(current.emoji).isEmpty())) {
            m_reactionIntents.remove(id);
            reportReaction(tr("Could not verify ownership of this reaction.")); return;
        }
        m_ownReactions.insert(id, facts);
        if (!facts.contains(current.emoji))
            current.emoji = canonicalReactionName(current.emoji);
        current.desired = !facts.contains(current.emoji);
        current.reportAddition = current.desired;
        current.mutated = true;
        const auto name = current.emoji;
        const auto desired = current.desired;
        const auto stream = current.stream, topic = current.topic;
        staleConversation(stream, topic, false);
        const auto path = messenger + "/message_reactions/" + (desired ? QString{} : facts.value(name).uuid);
        const auto body = desired ? QJsonObject{{"message_uuid", id}, {"emoji_name", name}} : QJsonObject{};
        request(desired ? "POST" : "DELETE", path, body, {}, [this, id, nonce, desired, stream, topic](const ApiResult &result) {
            staleConversation(stream, topic, false);
            if (!reactionContext(id, nonce)) return;
            // Network/5xx and accepted journal commands are ambiguous. Even a
            // malformed success may have committed; never repeat the mutation.
            if (result.status >= 400 && result.status < 500 && result.status != 404
                && result.status != 408 && result.status != 409 && result.status != 429) {
                m_reactionIntents.remove(id);
                reportReaction(tr("The reaction could not be changed. %1").arg(result.error)); return;
            }
            if (desired && result.ok()) {
                const auto fact = parseReactionFact(result.data.toObject(), m_session->project());
                if (fact && (fact->message != id || fact->user != m_session->user()
                    || fact->emoji != m_reactionIntents.value(id).emoji)) {
                    // Treat an unexpected response as unconfirmed, not as proof
                    // that the intended mutation was rejected by the server.
                    m_error = tr("The reaction response could not be verified.");
                    m_reactionIntents[id].error = m_error;
                }
            }
            confirmReaction(id, nonce);
        });
    });
}

void NativeController::retryReactionConfirmation(const QString &id, const QString &nonce)
{
    if (!reactionContext(id, nonce)) return;
    auto &intent = m_reactionIntents[id];
    if (intent.attempts >= 3) {
        intent.checking = false; intent.uncertain = true; intent.reportAddition = false;
        reportReaction(tr("The reaction has not been confirmed. Retry checks its status without sending it again."), id);
        return;
    }
    const auto generation = m_api->generation();
    QTimer::singleShot(intent.attempts * 200, this, [this, id, nonce, generation] {
        if (generation == m_api->generation() && reactionContext(id, nonce)) confirmReaction(id, nonce);
    });
}

void NativeController::confirmReaction(const QString &id, const QString &nonce)
{
    if (!reactionContext(id, nonce)) return;
    ++m_reactionIntents[id].attempts;
    const auto revision = m_reactionRevisions.value(id);
    ownReactions(id, [this, id, nonce, revision](bool ok, const OwnReactions &facts) {
        if (!reactionContext(id, nonce)) return;
        if (!ok) { retryReactionConfirmation(id, nonce); return; }
        const auto context = projectionContext();
        request("GET", messenger + "/messages/" + id, {}, {}, [this, id, nonce, revision, facts, context](const ApiResult &result) {
            if (!reactionContext(id, nonce)) return;
            const auto saved = parseMessage(result.data.toObject(), m_session->project());
            const auto intent = m_reactionIntents.value(id);
            if (!result.ok() || !saved || !saved->reactionsKnown || saved->uuid != id
                || saved->stream != intent.stream || saved->topic != intent.topic
                || revision != m_reactionRevisions.value(id)) { retryReactionConfirmation(id, nonce); return; }
            const bool exists = facts.contains(intent.emoji);
            const auto users = saved->reactionUsers.constFind(intent.emoji);
            const bool membershipAgrees = users == saved->reactionUsers.cend()
                || users->contains(m_session->user()) == exists;
            const bool countsAgree = !exists || saved->reactions.value(intent.emoji) > 0;
            // Own list and aggregate are separate projections. Require both to
            // agree before clearing the UUID-owned pending intent.
            if (exists != intent.desired || !membershipAgrees || !countsAgree) {
                retryReactionConfirmation(id, nonce); return;
            }
            for (auto &message : m_messages) if (message.uuid == id) {
                message.reactions = saved->reactions; message.reactionUsers = saved->reactionUsers; message.reactionsKnown = true;
            }
            m_ownReactions.insert(id, facts);
            staleConversation(intent.stream, intent.topic, false);
            projectCanonical(result.data.toObject(), NativeCache::Projection::Reactions, context);
            ++m_reactionRevisions[id]; ++m_messageRevision;
            m_reactionIntents.remove(id);
            if (!intent.error.isEmpty() && m_error == intent.error) m_error.clear();
            const QPointer<NativeController> guard(this);
            emit messagesChanged();
            if (guard) emit stateChanged();
            if (guard && intent.desired && intent.reportAddition && intent.owner == ownerKey()
                && intent.generation == m_api->generation() && intent.selection == m_selection
                && intent.stream == m_stream && (m_topic.isEmpty() || intent.topic == m_topic))
                emit reactionAdded(id, canonicalReactionName(intent.emoji), intent.owner, intent.stream, intent.topic);
        });
    });
}

void NativeController::refreshReactions(const QString &id)
{
    const auto *message = findMessage(id);
    if (!message || !inConversation(*message)) return;
    const auto selection = m_selection;
    const auto actualTopic = message->topic;
    const auto revision = m_reactionRevisions.value(id);
    if (m_reactionRefreshes.contains(id)) return;
    const auto ticket = ++m_nextReactionRefresh;
    m_reactionRefreshes.insert(id, ticket);
    const auto context = projectionContext();
    request("GET", messenger + "/messages/" + id, {}, {}, [this, id, selection, actualTopic, revision, ticket, context](const ApiResult &result) {
        if (m_reactionRefreshes.value(id) != ticket) return;
        if (selection != m_selection) { m_reactionRefreshes.remove(id); return; }
        m_reactionRefreshes.remove(id);
        if (revision != m_reactionRevisions.value(id)) { refreshReactions(id); return; }
        const auto saved = parseMessage(result.data.toObject(), m_session->project());
        if (!result.ok() || !saved || !saved->reactionsKnown || saved->uuid != id
            || !inConversation(*saved) || saved->topic != actualTopic) {
            report(tr("Could not refresh reaction metadata.")); return;
        }
        for (auto &message : m_messages) if (message.uuid == id) {
            message.reactions = saved->reactions; message.reactionUsers = saved->reactionUsers; message.reactionsKnown = true;
        }
        projectCanonical(result.data.toObject(), NativeCache::Projection::Reactions, context);
        ++m_messageRevision;
        const QPointer<NativeController> guard(this);
        emit messagesChanged();
        if (!guard || selection != m_selection) return;
        auto intent = m_reactionIntents.find(id);
        if (intent != m_reactionIntents.end() && !intent->checking && intent->mutated) {
            if (m_busy || intent->owner != ownerKey()) return;
            intent->selection = m_selection; intent->generation = m_api->generation();
            intent->checking = true; intent->uncertain = false; intent->attempts = 0;
            intent->reportAddition = false;
            confirmReaction(id, intent->nonce);
        }
    });
}

NativeController::NativeController(ApiClient *api, Session *session, QObject *parent)
    : QObject(parent), m_api(api), m_session(session)
{
    m_mediaViewer = new MediaViewerModel(this, this);
    // Issue #144: the top bar switcher re-reads its rows whenever the live
    // identity or the remembered account set changes.
    connect(m_session, &Session::authenticated, this, &NativeController::sessionSwitcherChanged);
    connect(m_session, &Session::signedOut, this, &NativeController::sessionSwitcherChanged);
    connect(m_session, &Session::savedAccountsChanged, this, &NativeController::sessionSwitcherChanged);
    // Issue #146: a failed bootstrap stays announced until the next snapshot
    // completes (reference layout-bootstrap-error-banner), and the rate-limit
    // countdown ticks so the banner label counts down in place.
    connect(this, &NativeController::snapshotFailed, this, [this] {
        if (!m_bootstrapError) { m_bootstrapError = true; emit stateChanged(); }
    });
    connect(this, &NativeController::snapshotReloaded, this, [this] {
        if (m_bootstrapError) { m_bootstrapError = false; emit stateChanged(); }
    });
    m_connectionCountdown.setInterval(1000);
    connect(&m_connectionCountdown, &QTimer::timeout, this, [this] {
        if (m_rateLimitedUntilMs <= QDateTime::currentMSecsSinceEpoch()) {
            m_rateLimitedUntilMs = 0;
            m_connectionCountdown.stop();
        }
        emit stateChanged();
    });
    m_notifications.setParent(this);
    // A notification click navigates to the conversation it reported, exactly
    // like the reference click route (layout-workspace-notifications.hook.ts).
    connect(&m_notifications, &NotificationService::routeRequested, this,
        [this](const QString &stream, const QString &topic) {
            if (!topic.isEmpty()) openTopic(topic, stream);
            else if (!stream.isEmpty()) selectStream(stream);
        });
    // Metadata resolution mirrors resolveCurrentCandidate in the reference
    // hook: the loaded catalogs are the native client's active store.
    m_notifications.setMetadataResolver([this](NotificationCandidate &candidate) {
        const auto stream = std::find_if(m_streams.cbegin(), m_streams.cend(),
            [&](const Stream &row) { return row.uuid == candidate.streamUuid; });
        if (stream != m_streams.cend()) {
            candidate.streamKnown = true;
            candidate.streamPrivate = stream->privateStream;
            candidate.streamName = stream->name;
            candidate.streamMode = stream->notificationMode;
        }
        if (const auto topic = std::find_if(m_topics.cbegin(), m_topics.cend(),
                [&](const Topic &row) { return row.uuid == candidate.topicUuid; });
            topic != m_topics.cend()) {
            candidate.topicKnown = true;
            candidate.topicName = topic->isDefault ? QString{} : topic->name;
            candidate.topicMode = topic->notificationMode;
        } else if (const auto backfill = m_topicNotificationBackfill.constFind(candidate.topicUuid);
            backfill != m_topicNotificationBackfill.cend()) {
            candidate.topicKnown = true;
            candidate.topicName = backfill->isDefault ? QString{} : backfill->name;
            candidate.topicMode = backfill->notificationMode;
        }
        const auto author = m_names.constFind(candidate.authorUuid);
        if (author != m_names.cend())
            candidate.senderName = author->name;
        if (!candidate.streamKnown)
            return false; // stream metadata is missing -> grace, then generic
        if (!candidate.streamPrivate && !candidate.topicKnown) {
            // Topic mute overrides the stream default. The catalog only covers
            // the selected stream, so resolve background topics on demand; the
            // grace window already bounds what an unreachable detail costs.
            requestTopicMetadata(candidate.topicUuid);
            return false;
        }
        return true;
    });
    // resolveGenericNotificationAccountLabel: username, separator, domain -
    // never a raw URL; an invalid or absent part is simply dropped.
    m_notifications.setGenericBody([this] {
        const QStringList parts{
            m_session->publicProfile().value(QStringLiteral("username")).toString().trimmed(),
            m_api->server().host()};
        QStringList kept;
        for (const auto &part : parts)
            if (!part.isEmpty()) kept << part;
        return kept.join(notificationSeparator());
    });
    // The default-organization shortcut exists only when the build (or the
    // process environment, for testability) defines the origin; the reference
    // gates the same block on DEFAULT_LOGIN_ORGANIZATION_URL.
    m_defaultOrganizationUrl = qEnvironmentVariable("WORKSPACE_NATIVE_DEFAULT_LOGIN_ORGANIZATION_URL",
#ifdef WORKSPACE_NATIVE_DEFAULT_LOGIN_ORGANIZATION_URL
        QStringLiteral(WORKSPACE_NATIVE_DEFAULT_LOGIN_ORGANIZATION_URL)
#else
        QString()
#endif
        ).trimmed();
    m_defaultOrganizationName = qEnvironmentVariable("WORKSPACE_NATIVE_DEFAULT_LOGIN_ORGANIZATION_NAME",
#ifdef WORKSPACE_NATIVE_DEFAULT_LOGIN_ORGANIZATION_NAME
        QStringLiteral(WORKSPACE_NATIVE_DEFAULT_LOGIN_ORGANIZATION_NAME)
#else
        tr("Exordos Core Public")
#endif
        ).trimmed();
    if (m_defaultOrganizationName.isEmpty())
        m_defaultOrganizationName = tr("Exordos Core Public");
    connect(this, &NativeController::stateChanged, this, &NativeController::scheduleChatPreparation);
    connect(this, &NativeController::messagesChanged, this, &NativeController::scheduleChatPreparation);
    connect(this, &NativeController::stateChanged, this, &NativeController::scheduleChatControls);
    connect(this, &NativeController::messagesChanged, this, &NativeController::scheduleChatControls);
    connect(this, &NativeController::streamsChanged, this, &NativeController::archivedChatsChanged);
    connect(this, &NativeController::stateChanged, this, &NativeController::archivedChatsChanged);
    connect(this, &NativeController::stateChanged, this, [this] { flushPendingDeepLink(); });
    connect(this, &NativeController::messagesChanged, this, [this] { reanchorFromWindow(); });
    connect(this, &NativeController::stateChanged, this, [this] { reanchorFromWindow(); });
    m_anchorHighlight.setSingleShot(true);
    connect(&m_anchorHighlight, &QTimer::timeout, this, [this] {
        if (m_anchorMessage.isEmpty()) return;
        m_anchorMessage.clear();
        emit anchorChanged();
    });

    connect(session, &Session::signedOut, this, &NativeController::reset);
    connect(session, &Session::error, this, [this](const QString &message) {
        // Remembering a verified session can fail independently of live sync.
        m_error = message;
        emit stateChanged();
    });
    connect(session, &Session::savedAccountsChanged, this, &NativeController::savedAccountsChanged);
    connect(this, &NativeController::savedAccountsChanged, this, &NativeController::sessionSwitcherChanged);
    connect(this, &NativeController::stateChanged, this, &NativeController::sessionSwitcherChanged);
    connect(session, &Session::authenticated, this, [this] {
        // The reference records the organization into history only after a
        // login committed its session (workspace-organization-history.lib.ts).
        if (m_preferences)
            m_preferences->recordOrganizationOrigin(m_api->server().toString());
    });
    connect(session, &Session::busyChanged, this, [this](bool busy) {
        const QPointer<NativeController> guard(this);
        if (!authenticated()) {
            interruptOwnStatusUpdate();
            if (!guard) return;
        }
        m_busy = busy;
        emit stateChanged();
    });
    connect(session, &Session::projectsAvailable, this, &NativeController::stateChanged);
    connect(api, &ApiClient::sessionChanged, this, [this] {
        const QPointer<NativeController> lookupGuard(this);
        interruptOwnStatusUpdate();
        if (!lookupGuard) return;
        m_statusPreferenceOwner.clear(); m_statusManualAway = false;
        interruptChatRequestLookups();
        if (!lookupGuard) return;
        const QPointer<NativeController> guard(this);
        interruptTopicOpening();
        if (!guard) return;
        m_cacheForgettingAccount = false;
        interruptTopicCreations();
        interruptChannelCreations();
        interruptDirectChats();
        m_messageFields.clear();
        // Same-token renewals retain their transfers. A changed generation fences
        // both active operations and queued paths before resetting their UI state.
        resetReadState();
        resetReactions(true);
        closeCache();
        if (!guard) return;
        m_catalogVerified = m_usersVerified = m_topicsVerified = m_historyVerified = false;
        cancelFileTransfers(tr("Attachment transfer interrupted by a session change. Choose the file again."));
        if (!guard) return;
        emit stateChanged();
        if (guard) emit messagesChanged();
    });
    connect(session, &Session::authenticated, this, [this] {
        const QPointer<NativeController> guard(this);
        prepareOwnStatusPreferences();
        interruptTopicOpening();
        if (!guard) return;
        interruptChannelCreations();
        interruptDirectChats();
        openCache();
        if (!guard) return;
        m_catalogVerified = m_usersVerified = m_topicsVerified = m_historyVerified = false;
        for (auto it = m_reactionIntents.begin(); it != m_reactionIntents.end();)
            if (it->owner != ownerKey()) it = m_reactionIntents.erase(it); else ++it;
        resetReadState();
        ++m_selection;
        prepareConversationRestore();
        m_loading = false;
        m_sending = false;
        m_topicChanging = false;
        ++m_topicMutation;
        m_recoveringMarker = m_resumeOlder = false;
        if (!m_realtimeMode) {
            refresh();
            m_poll.start();
        } else {
            // Token replacement cancels outstanding route reads. Re-establish
            // their snapshots behind a fresh epoch barrier while keeping drafts.
            m_snapshotActive = false;
            m_snapshotParts = 0;
            emit resynchronizeRequested();
        }
        if (guard) emit stateChanged();
    });
    m_poll.setInterval(15000);
    connect(&m_poll, &QTimer::timeout, this, [this] {
        if (QGuiApplication::applicationState() == Qt::ApplicationActive)
            loadMessages();
    });
}

NativeController::~NativeController()
{
    interruptOwnStatusUpdate();
    interruptChatRequestLookups();
    interruptTopicOpening();
    cancelFileTransfers();
}

void NativeController::cancelFileTransfers(const QString &reason)
{
    ++m_fileEpoch;
    const auto operations = m_fileOperations;
    m_fileOperations.clear();
    m_uploadQueue.clear();
    if (!reason.isEmpty()) {
        for (const auto &conversation : m_pendingUploads.keys())
            m_attachmentNotices[conversation] = reason;
        if (m_downloads > 0)
            m_notice = reason;
    }
    m_uploads = m_downloads = m_activeUploads = 0;
    m_pendingUploads.clear();
    // FileClient may be shared. Cancel only operations initiated by this controller.
    // The epoch was invalidated first because cancellation callbacks can be synchronous.
    for (const auto &operation : operations)
        if (operation.service && operation.operation != 0)
            operation.service->cancel(operation.operation);
}

void NativeController::useFiles(FileClient *client)
{
    if (m_files == client)
        return;
    disconnect(m_filesDestroyed);
    ++m_fileServiceSerial;
    cancelFileTransfers(tr("Attachment service changed. Choose the file again."));
    m_files = client;
    if (m_mediaViewer)
        m_mediaViewer->setFileClient(client);
    if (client) {
        const auto serial = m_fileServiceSerial;
        m_filesDestroyed = connect(client, &QObject::destroyed, this, [this, serial] {
            if (serial != m_fileServiceSerial)
                return;
            m_files.clear();
            if (m_mediaViewer)
                m_mediaViewer->setFileClient(nullptr);
            cancelFileTransfers(tr("Attachment service is unavailable. Choose the file again."));
            emit stateChanged();
        });
    }
    emit stateChanged();
}

bool NativeController::canAttach() const
{
    const auto *stream = currentStream();
    return m_files && authoritativeRoute() && stream && !stream->encrypted && !m_topic.isEmpty()
        && (!durableDraftRoute() || m_draftStore->ready(m_stream, m_topic))
        && !topicClosed() && !m_sending;
}

void NativeController::attachFiles(const QVariantList &localUrls)
{
    if (!canAttach())
        return;
    m_error.clear();
    const auto conversation = conversationKey();
    for (const auto &value : localUrls) {
        const auto url = value.toUrl();
        if (!url.isLocalFile() || url.hasQuery() || url.hasFragment()) {
            m_error = tr("Choose a local file to attach.");
            continue;
        }
        m_uploadQueue.enqueue({++m_nextFileOperation, url.toLocalFile(), m_stream, conversation,
            durableDraftRoute() ? m_draftStore->capture(m_stream, m_topic) : NativeDrafts::Token{}});
        ++m_uploads;
        ++m_pendingUploads[conversation];
        m_attachmentNotices[conversation] = tr("Uploading attachment…");
    }
    const QPointer<NativeController> guard(this);
    emit stateChanged();
    if (guard)
        drainUploads();
}

void NativeController::drainUploads()
{
    if (m_drainingUploads)
        return;
    m_drainingUploads = true;
    const QPointer<NativeController> self(this);
    while (m_files && m_activeUploads < 5 && !m_uploadQueue.isEmpty()) {
        const auto task = m_uploadQueue.dequeue();
        const auto generation = m_api->generation();
        const auto epoch = m_fileEpoch;
        const QPointer<FileClient> service = m_files;
        m_fileOperations.insert(task.intent, {service, 0});
        ++m_activeUploads;
        const auto operation = service->upload(task.path, task.stream, {},
            [self, generation, epoch, intent = task.intent, conversation = task.conversation,
             residency = task.draftResidency](const FileResult &result) {
                Q_UNUSED(residency);
                if (self)
                    self->finishUpload(intent, generation, epoch, conversation, result);
            });
        if (!self)
            return;
        // A local validation failure may already have completed synchronously.
        auto record = m_fileOperations.find(task.intent);
        if (record != m_fileOperations.end())
            record->operation = operation;
    }
    m_drainingUploads = false;
}

void NativeController::finishUpload(quint64 intent, quint64 generation, quint64 epoch,
                                    const QString &conversation, const FileResult &result)
{
    if (m_api->generation() != generation || m_fileEpoch != epoch || !m_fileOperations.remove(intent))
        return;
    --m_activeUploads;
    --m_uploads;
    if (--m_pendingUploads[conversation] <= 0)
        m_pendingUploads.remove(conversation);
    const QPointer<NativeController> guard(this);
    if (!result.ok()) {
        const auto error = tr("The attachment could not be uploaded. %1").arg(result.error);
        m_attachmentNotices[conversation] = error;
        if (conversation == conversationKey())
            m_error = error;
    } else {
        auto label = result.metadata.name;
        label.replace('\\', "\\\\");
        label.replace('[', "\\[");
        label.replace(']', "\\]");
        label.replace('\r', ' ');
        label.replace('\n', ' ');
        const auto prefix = result.metadata.kind() == FileKind::File ? QString{} : QStringLiteral("!");
        const auto attachment = prefix + '[' + label + "](<" + result.metadata.urn() + ">)";
        const auto stream = conversation.section('/', 0, 0), topic = conversation.section('/', 1, 1);
        const auto capturedStream = std::find_if(m_streams.cbegin(), m_streams.cend(),
            [&stream](const Stream &row) { return row.uuid == stream; });
        const bool persistent = m_draftStore && m_draftStore->owner() == creationOwnerKey()
            && capturedStream != m_streams.cend() && !capturedStream->encrypted
            && m_draftRoutes.contains(conversation);
        auto draft = persistent ? m_draftStore->text(stream, topic) : m_drafts.value(conversation);
        const bool imageParagraph = result.metadata.kind() == FileKind::Image;
        if (!draft.isEmpty()) {
            if (!draft.endsWith('\n'))
                draft += '\n';
            if (imageParagraph && !draft.endsWith("\n\n"))
                draft += '\n';
        }
        const auto text = draft + attachment + (imageParagraph ? "\n\n" : "\n");
        if (persistent) {
            m_draftStore->edit(stream, topic, text);
        } else {
            m_drafts.insert(conversation, text); ++m_draftRevisions[conversation];
        }
        if (!guard) return;
        m_attachmentNotices[conversation] = m_pendingUploads.value(conversation) > 0
            ? tr("Uploading attachments…") : tr("Attachment is ready to send.");
        if (conversation == conversationKey())
            emit draftChanged();
    }
    if (!guard)
        return;
    emit stateChanged();
    if (guard)
        drainUploads();
}

void NativeController::downloadFile(const QString &id, const QUrl &destination)
{
    if (!m_files || !authenticated() || !isUuid(id) || !destination.isLocalFile()
        || destination.hasQuery() || destination.hasFragment())
        return;
    const auto generation = m_api->generation();
    const auto epoch = m_fileEpoch;
    const auto intent = ++m_nextFileOperation;
    const QPointer<FileClient> service = m_files;
    ++m_downloads;
    m_error.clear();
    m_notice = tr("Downloading attachment…");
    m_fileOperations.insert(intent, {service, 0});
    const QPointer<NativeController> self(this);
    emit stateChanged();
    if (!self || m_fileEpoch != epoch || !service)
        return;
    const auto operation = service->download(id, destination.toLocalFile(), [self, generation, epoch, intent](const FileResult &result) {
        if (!self || self->m_api->generation() != generation || self->m_fileEpoch != epoch
            || !self->m_fileOperations.remove(intent))
            return;
        --self->m_downloads;
        if (!result.ok()) {
            self->m_error = tr("The attachment could not be saved. %1").arg(result.error);
            self->m_notice.clear();
        } else {
            self->m_notice = tr("Saved %1.").arg(QFileInfo(result.path).fileName());
        }
        emit self->stateChanged();
    });
    if (self) {
        auto record = m_fileOperations.find(intent);
        if (record != m_fileOperations.end())
            record->operation = operation;
    }
}

void NativeController::useRealtime(RealtimeClient *client)
{
    Q_ASSERT(client && !m_realtimeMode);
    setRealtimeMode(true);
    connect(client, &RealtimeClient::snapshotRequired, this, &NativeController::reloadSnapshot);
    connect(this, &NativeController::snapshotReloaded, client, &RealtimeClient::acknowledgeSnapshot);
    connect(this, &NativeController::snapshotFailed, client, [client] {
        QTimer::singleShot(1000, client, &RealtimeClient::retrySnapshot);
    });
    connect(this, &NativeController::resynchronizeRequested, client, &RealtimeClient::resynchronize);
    connect(client, &RealtimeClient::connectionChanged, this, [this](const QString &state) {
        const bool previous = m_syncBlocked;
        m_syncState = state;
        if (state == "live") {
            m_syncBlocked = false;
            // A healthy realtime link proves reachability on its own: a
            // stale transport-offline flag must not outlive the realtime
            // recovery and mask live sync state with the offline banner.
            m_transportOffline = false;
        }
        else if (state != "catching_up" && state != "connecting") m_syncBlocked = true;
        if (!previous && m_syncBlocked) {
            interruptChannelCreations();
            interruptDirectChats();
        }
        if (state == "retrying" || state == "invalid_data" || state == "authentication_required"
            || state == "waiting_for_session" || state == "stopped") markCacheStale();
        if (previous != m_syncBlocked) notifyTrustChanged(); else emit stateChanged();
    });
    connect(client, &RealtimeClient::eventReceived, this, [this, client](const QJsonObject &event) {
        if (applyRealtimeEvent(event)) {
            client->acknowledge(event.value("uuid").toString());
        } else {
            report(tr("A live update could not be applied. Reloading the current state."));
            client->resynchronize();
        }
    });
    client->start();
}

ApiClient::RequestId NativeController::request(const QByteArray &method, const QString &path,
                               const QJsonValue &body, const QUrlQuery &query,
                               ApiClient::Callback callback)
{
    QPointer<NativeController> self(this);
    const auto owner = m_api->generation();
    return m_api->request(method, path, body, query, [self, owner, callback](const ApiResult &result) {
        if (self && self->m_api->generation() == owner) {
            self->observeTransport(result);
            callback(result);
        }
    });
}

void NativeController::login(const QString &server, const QString &user,
                             const QString &password, const QString &otp)
{
    m_error.clear();
    m_notice.clear();
    m_uploads = m_downloads = 0;
    m_session->login(QUrl::fromUserInput(server), user, password, otp);
}

// Mirrors reference isValidRealmUrl (workspace_ui shared/lib/validation.ts):
// absolute URL with a safe scheme, hostname without spaces, non-empty dot
// labels. The transport origin rules (no path/query/fragment/userinfo) apply
// through ApiClient::canSetServer on the resolved origin.
static QUrl organizationOriginFor(const QString &text)
{
    const auto trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return {};
    const QUrl url(trimmed);
    if (!url.isValid() || url.isRelative() || url.host().isEmpty())
        return {};
    const auto host = url.host();
    if (host.contains(QLatin1Char(' ')) || host.startsWith(QLatin1Char('.'))
        || host.endsWith(QLatin1Char('.')) || host.contains(QLatin1String("..")))
        return {};
    // Mirror `new URL(input).origin`: credentials in the URL are stripped by
    // origin normalization, not rejected (workspace_ui validation.ts).
    QUrl origin;
    origin.setScheme(url.scheme());
    origin.setHost(host);
    if (url.port() != -1)
        origin.setPort(url.port());
    return origin;
}

// Mirrors resolveLoginIconUrl (workspace_ui login-page-icon-url.lib.ts):
// realm-relative icons resolve against the organization origin and absolute
// http(s) icons pass through unchanged. Cross-origin icons are accepted by the
// reference on purpose (CDN-hosted realm icons); the desktop (Electron) build
// never had the browser same-origin block, and the native client follows the
// same contract (see review thread on PR #80).
static QString resolveRealmIconUrl(const QString &origin, const QString &icon)
{
    const auto trimmed = icon.trimmed();
    if (trimmed.isEmpty())
        return {};
    const QUrl base(origin);
    if (!base.isValid() || base.host().isEmpty())
        return {};
    const auto resolved = base.resolved(QUrl(trimmed));
    if (!resolved.isValid() || resolved.host().isEmpty()
        || (resolved.scheme() != QLatin1String("https") && resolved.scheme() != QLatin1String("http")))
        return {};
    return resolved.toString();
}

void NativeController::setPreferences(NativePreferences *preferences)
{
    m_preferences = preferences;
}

QStringList NativeController::organizationHistory() const
{
    return m_preferences ? m_preferences->organizationHistory() : QStringList{};
}

bool NativeController::isOrganizationUrlValid(const QString &text) const
{
    const auto origin = organizationOriginFor(text);
    return !origin.isEmpty() && m_api->canSetServer(origin);
}

void NativeController::organizationInputChanged(const QString &text)
{
    if (authenticated())
        return;
    m_organizationRealm = text;
    // Reference handleRealmChange: any text whose origin is not the checked
    // one invalidates the check, the pending advance and the error slot.
    const auto origin = organizationOriginFor(text);
    const auto originKey = origin.isEmpty() ? QString{} : origin.toString();
    if (originKey == m_checkedOrganizationOrigin)
        return;
    ++m_organizationAttempt;
    m_organizationLoading = false;
    m_organizationSettings.clear();
    m_checkedOrganizationOrigin.clear();
    m_pendingOrganizationOrigin.clear();
    m_error.clear();
    if (m_loginStep != QLatin1String("organization"))
        m_loginStep = QStringLiteral("organization");
    emit stateChanged();
}

void NativeController::discoverOrganization(const QString &text)
{
    if (authenticated())
        return;
    m_organizationRealm = text;
    const auto origin = organizationOriginFor(text);
    if (origin.isEmpty() || !m_api->canSetServer(origin)) {
        // Reference fetchSettings returns false for an invalid realm after
        // clearing settings and the checked marker.
        if (!m_organizationSettings.isEmpty() || !m_checkedOrganizationOrigin.isEmpty()) {
            ++m_organizationAttempt;
            m_organizationLoading = false;
            m_organizationSettings.clear();
            m_checkedOrganizationOrigin.clear();
            emit stateChanged();
        }
        return;
    }
    const auto originKey = origin.toString();
    const auto attempt = ++m_organizationAttempt;
    m_organizationLoading = true;
    emit stateChanged();
    QPointer<NativeController> guard(this);
    m_session->fetchOrganizationSettings(origin, [guard, attempt, originKey, this](
            bool ok, QVariantMap raw, QString error) {
        if (!guard || attempt != m_organizationAttempt)
            return;
        m_organizationLoading = false;
        if (ok) {
            QVariantMap settings;
            settings.insert(QStringLiteral("organizationOrigin"), originKey);
            settings.insert(QStringLiteral("realmName"), raw.value(QLatin1String("realm_name")).toString());
            settings.insert(QStringLiteral("realmIconUrl"),
                resolveRealmIconUrl(originKey, raw.value(QLatin1String("realm_icon")).toString()));
            // Reference normalizeLoginRegistrationUrl: https-only.
            const auto registration = raw.value(QLatin1String("registration_url")).toString().trimmed();
            settings.insert(QStringLiteral("registrationUrl"),
                registration.startsWith(QLatin1String("https://")) ? registration : QString{});
            m_organizationSettings = settings;
            // The reference submits `new URL(realm).origin` after a check, so
            // downstream login sees exactly what was checked.
            m_organizationRealm = originKey;
        } else {
            m_organizationSettings.clear();
        }
        m_checkedOrganizationOrigin = originKey;
        if (!m_pendingOrganizationOrigin.isEmpty() && m_pendingOrganizationOrigin == originKey) {
            m_pendingOrganizationOrigin.clear();
            if (ok)
                m_loginStep = QStringLiteral("credentials");
            else
                m_error = error;
        }
        emit stateChanged();
    });
}

void NativeController::continueFromOrganization(const QString &text)
{
    if (authenticated())
        return;
    m_organizationRealm = text;
    m_error.clear();
    const auto origin = organizationOriginFor(text);
    if (origin.isEmpty() || !m_api->canSetServer(origin)) {
        m_error = tr("Server address must be a valid HTTPS URL");
        emit stateChanged();
        return;
    }
    const auto originKey = origin.toString();
    if (originKey == m_checkedOrganizationOrigin) {
        // Reference: a completed check either reveals credentials or repeats
        // the load error; it never refetches behind the button.
        m_pendingOrganizationOrigin.clear();
        if (!m_organizationSettings.isEmpty())
            m_loginStep = QStringLiteral("credentials");
        else
            m_error = tr("Could not load organization settings. Check the server address and try again.");
        emit stateChanged();
        return;
    }
    m_pendingOrganizationOrigin = originKey;
    if (!m_organizationLoading)
        discoverOrganization(text);
    else
        emit stateChanged();
}

void NativeController::backToOrganization()
{
    if (authenticated())
        return;
    m_pendingOrganizationOrigin.clear();
    m_error.clear();
    m_loginStep = QStringLiteral("organization");
    emit stateChanged();
}

void NativeController::selectDefaultOrganization()
{
    if (authenticated() || m_defaultOrganizationUrl.isEmpty())
        return;
    ++m_organizationAttempt;
    m_organizationLoading = false;
    m_organizationRealm = m_defaultOrganizationUrl;
    m_error.clear();
    m_checkedOrganizationOrigin.clear();
    m_organizationSettings.clear();
    m_pendingOrganizationOrigin.clear();
    m_loginStep = QStringLiteral("organization");
    emit stateChanged();
}

void NativeController::openProject(const QString &id)
{
    if (id != m_session->project())
        reset();
    m_session->selectProject(id);
}

void NativeController::resumeSavedAccount(const QString &id)
{
    if (busy()) return;
    m_error.clear();
    m_session->resumeSavedAccount(id);
}

void NativeController::forgetSavedAccount(const QString &id)
{
    forgetSavedAccountWithFollowUp(id, {});
}

void NativeController::forgetSavedAccountWithFollowUp(const QString &id, std::function<void()> done)
{
    if (busy()) return;
    m_error.clear();
    if (m_cache) for (const auto &account : m_session->savedAccounts()) {
        if (account.id != id) continue;
        ++m_cacheRevision;
        const QPointer<NativeCacheService> cache = m_cache;
        const QPointer<NativeController> guard(this);
        const auto generation = m_api->generation();
        m_cacheForgettingAccount = true;
        emit stateChanged();
        if (!guard || !cache || cache != m_cache || generation != m_api->generation()) return;
        const bool accepted = cache->forgetAccount(account.server, account.project, account.user, this,
            [this, guard, cache, id, generation, done = std::move(done)](bool erased) {
            if (!guard || cache != m_cache || generation != m_api->generation()) return;
            m_cacheForgettingAccount = false;
            if (!erased) {
                report(tr("The local cache could not be cleared. The saved account was kept."));
                return;
            }
            // Wait for the secure-store removal and only then hand over: the
            // successor resume advances the session attempt and its sign-out
            // reset would otherwise swallow (or pre-empt) the removal result.
            m_session->forgetSavedAccount(id, [this, guard, done = std::move(done)](const QString &removalError) {
                if (!guard) return;
                // The successor's reset clears m_error, so re-report the
                // removal failure afterwards to keep it visible to the user.
                if (done) done();
                if (!removalError.isEmpty())
                    report(tr("The saved credential could not be removed. %1").arg(removalError));
            });
            m_sessionProjections.remove(id);
            if (guard) emit stateChanged();
        });
        if (!guard) return;
        if (!accepted) {
            m_cacheForgettingAccount = false;
            report(tr("The local cache could not be cleared. The saved account was kept."));
        }
        return;
    }
    const QPointer<NativeController> tailGuard(this);
    m_session->forgetSavedAccount(id, [this, tailGuard, done = std::move(done)](const QString &removalError) {
        if (!tailGuard) return;
        // The successor's reset clears m_error, so re-report the removal
        // failure afterwards: the failure must outlive the handover, not be
        // discarded by it (review on PR #170).
        if (done) done();
        if (!removalError.isEmpty())
            report(tr("The saved credential could not be removed. %1").arg(removalError));
    });
    if (!tailGuard) return;
    m_sessionProjections.remove(id);
    emit stateChanged();
}

void NativeController::logout()
{
    // Banners must not outlive the account that produced them (the reference
    // closes every active owner notification on sign-out).
    m_notifications.onOwnerRetired(creationOwnerKey());
    m_topicNotificationBackfill.clear();
    m_topicMetadataInFlight.clear();
    const QPointer<NativeController> self(this);
    const QPointer<Session> session(m_session);
    bool accepted = true;
    if (m_cache) {
        const QPointer<NativeController> guard(this);
        const auto cleanup = [this, guard](bool erased) {
            if (guard && !erased && !authenticated())
                report(tr("Signed out, but the local message cache could not be cleared."));
        };
        if (authenticated()) accepted = m_cache->forgetAccount(m_api->server(), m_session->project(), m_session->user(), this, cleanup);
        else if (!m_cacheLease.owner.isEmpty()) accepted = m_cache->clearOwner(m_cacheLease, this, cleanup);
    }
    if (session) session->logout();
    if (self && !accepted) report(tr("Signed out, but the local message cache could not be cleared."));
}

void NativeController::reset()
{
    closeEditSession();
    closeReplySession();
    m_quotes.clear();
    m_quoteInFlight.clear();
    // A cold-start deep link is user intent captured before the session
    // settled; it survives login transitions and is consumed on apply.
    m_deepLinkFlushScheduled = false;
    ++m_organizationAttempt;
    m_loginStep = QStringLiteral("organization");
    m_organizationRealm.clear();
    m_checkedOrganizationOrigin.clear();
    m_pendingOrganizationOrigin.clear();
    m_organizationLoading = false;
    m_organizationSettings.clear();
    clearAnchor();
    m_draftsVisible = false; m_draftOpenId.clear();
    if (m_draftStore) m_draftStore->hidePage();
    resetMentions();
    resetInbox();
    const QPointer<NativeController> guard(this);
    interruptOwnStatusUpdate();
    if (!guard) return;
    m_statusPreferenceOwner.clear(); m_statusManualAway = false;
    m_conversationPreferenceOwner.clear();
    cancelConversationRestore();
    cancelFileTransfers();
    if (!guard) return;
    closeCache();
    if (!guard) return;
    m_cacheHydrated = false;
    m_catalogVerified = m_usersVerified = m_topicsVerified = m_historyVerified = false;
    m_topicSeedUnverified = false;
    resetReadState();
    resetReactions();
    ++m_selection;
    ++m_catalog;
    ++m_topicRequest;
    m_poll.stop();
    m_streams.clear();
    m_archiveOperations.clear();
    m_topics.clear();
    m_messages.clear();
    m_messageFields.clear();
    m_names.clear();
    // No row preview (positive or negative) survives an owner change, like the reference
    // previewTextCache that is dropped together with the session owner.
    m_rowPreviews.clear();
    m_rowPreviewOrder.clear();
    m_rowPreviewPending.clear();
    m_rowPreviewInFlight.clear();
    m_rowPreviewFence.clear();
    m_nextRowPreviewFence = 0;
    m_viewerDirectoryName.clear();
    m_viewerDirectoryOwner.clear();
    m_channelChoices.clear();
    m_channelDirectoryOwner.clear();
    m_channelDirectoryValid = false;
    interruptTopicCreations();
    interruptChannelCreations();
    interruptDirectChats();
    m_drafts.clear();
    m_draftRevisions.clear();
    m_draftFallbackOwner.clear(); m_draftFallbackRoute.clear(); m_draftFallback.clear();
    m_pendingSends.clear();
    m_pendingUploads.clear();
    m_attachmentNotices.clear();
    m_uploads = m_downloads = 0;
    m_notice.clear();
    m_stream.clear();
    m_topic.clear();
    m_before.clear();
    m_after.clear();
    m_error.clear();
    // Sign-out must not keep a banner alive across accounts (#146).
    m_bootstrapError = false;
    m_transportOffline = false;
    m_rateLimitedUntilMs = 0;
    m_connectionCountdown.stop();
    m_loading = m_sending = m_busy = false;
    m_topicChanging = false;
    ++m_topicMutation;
    m_recoveringMarker = m_resumeOlder = false;
    m_snapshotActive = false;
    m_snapshotParts = 0;
    emit draftChanged();
    emit streamsChanged();
    emit topicsChanged();
    emit messagesChanged();
    emit stateChanged();
}

void NativeController::report(const QString &message)
{
    cancelSnapshot();
    m_error = message;
    emit stateChanged();
}

void NativeController::cancelSnapshot()
{
    if (!m_snapshotActive)
        return;
    m_snapshotActive = false;
    m_snapshotParts = 0;
    emit snapshotFailed();
}

void NativeController::finishSnapshotPart(unsigned part)
{
    if (!m_snapshotActive || m_snapshotCatalog != m_catalog || m_snapshotSelection != m_selection)
        return;
    m_snapshotParts &= ~part;
    if (m_snapshotParts == 0) {
        m_snapshotActive = false;
        const QPointer<NativeController> guard(this);
        emit snapshotReloaded();
        if (guard) notifyTrustChanged();
    }
}

bool NativeController::applyRealtimeEvent(const QJsonObject &envelope)
{
    if (!authenticated() || envelope.value("project_id").toString() != m_session->project()
        || envelope.value("user_uuid").toString() != m_session->user()
        || !isUuid(envelope.value("uuid").toString()))
        return false;
    auto payload = envelope.value("payload").toObject();
    const auto kind = payload.take("kind").toString();
    const auto id = payload.value("uuid").toString();
    if (kind == "message.created" || kind == "message.updated" || kind == "message.read") {
        auto message = parseMessage(payload, m_session->project());
        if (!message) return false;
        // Unread-mentions index maintenance (#42): classification is the
        // server's `mentioned` tri-state on the DTO, per
        // activity-realtime-applier.lib.ts:55-127. Epoch/owner guards were
        // enforced by the envelope checks above.
        bool mentionChanged = false;
        if (kind == QLatin1String("message.read"))
            mentionChanged = mentionIndexBoundary(*message);
        else if (message->read)
            mentionChanged = mentionIndexRemove(message->uuid);
        else if (message->mentioned.value_or(false)) {
            mentionChanged = mentionIndexUpsert(*message);
            // Live merge feeds only created events (applier:113-124); updated
            // events move the index but not the live card.
            if (kind != QLatin1String("message.updated"))
                mentionLiveUpsert(*message);
        } else if (message->mentioned)
            mentionChanged = mentionIndexRemove(message->uuid);
        if (mentionChanged) emit stateChanged();
        const bool selected = inConversation(*message);
        // Background arrivals become notification candidates exactly where
        // the reference background projection would have recorded them.
        if (kind == QLatin1String("message.created") && !selected
            && !message->own && !message->read)
            offerNotificationCandidate(*message);
        if (kind == "message.read") {
            staleReadConversation(message->stream, message->topic);
            if (message->read && m_cache)
                m_cache->markHistoryRead(m_cacheLease, message->stream, message->topic, {message->uuid});
            if (message->read)
                m_notifications.onMessagesClosed(creationOwnerKey(), {message->uuid});
            // A read event owns only a monotonic flag, even if its envelope also
            // carries an older complete message DTO.
            if (selected && message->read) {
                for (auto &current : m_messages) if (current.uuid == message->uuid) current.read = true;
                ++m_messageRevision; emit messagesChanged();
            }
        } else {
            // A full canonical update can change placement while preserving UUID.
            // Capture the old row before cache notifications can replace its owner.
            const auto found = findMessage(message->uuid);
            const auto previous = found ? std::optional<Message>{*found} : std::optional<Message>{};
            const bool moved = previous && (previous->stream != message->stream || previous->topic != message->topic);
            const bool leftSelection = moved && !selected;
            const bool fits = previous || m_before.isEmpty() || (!m_messages.isEmpty()
                && (message->createdMicros > m_messages.first().createdMicros
                    || (message->createdMicros == m_messages.first().createdMicros && message->uuid >= m_messages.first().uuid)));
            const bool admitted = selected && fits;
            const QPointer<NativeController> guard(this);
            const auto context = projectionContext();
            const auto owner = creationOwnerKey();
            const auto selection = m_selection;
            const auto current = [&] {
                return guard && owner == creationOwnerKey() && selection == m_selection
                    && context.generation == m_api->generation() && context.cache == m_cache
                    && context.lease.owner == m_cacheLease.owner && context.lease.generation == m_cacheLease.generation;
            };
            const auto stale = [&](const QString &stream, const QString &topic) {
                for (const auto kind : {NativeCache::Kind::History, NativeCache::Kind::Streams, NativeCache::Kind::Topics}) {
                    ++m_cacheRevision;
                    if (context.cache) context.cache->markStale(context.lease, kind,
                        kind == NativeCache::Kind::Streams ? QString{} : stream,
                        kind == NativeCache::Kind::History ? topic : QString{});
                    if (!current()) return false;
                }
                return true;
            };
            if (!stale(message->stream, message->topic)) return true;
            if (moved && !stale(previous->stream, previous->topic)) return true;
            if (admitted || previous || m_messageFields.contains(message->uuid)) observeMessage(payload);
            const bool deleted = m_messageFields.value(message->uuid).deleted;
            if (!deleted) {
                cancelChatRequestLookup(message->uuid);
                if (!current()) return true;
                observeChatRequest(*message);
                observeChatKeyOutcome(*message);
                if (message->stream == m_stream) receiveChatControl(*message);
                if (!current()) return true;
            }
            NativeCache::ProjectionFields fields = NativeCache::Projection::Insert | NativeCache::Projection::Payload
                | NativeCache::Projection::Star;
            if (message->reactionsKnown) fields |= NativeCache::Projection::Reactions;
            if (!deleted) projectCanonical(payload, fields, context);
            if (!current()) return true;
            if (leftSelection && !deleted) {
                ++m_messageRevision;
                m_messages.erase(std::remove_if(m_messages.begin(), m_messages.end(),
                    [&](const Message &row) { return row.uuid == message->uuid; }), m_messages.end());
                pruneMessageFields();
                emit messagesChanged();
            } else if (admitted && !deleted) {
                if (!previous || previous->reactions != message->reactions || previous->reactionUsers != message->reactionUsers) {
                    ++m_reactionRevisions[message->uuid]; m_ownReactions.remove(message->uuid);
                }
                if (m_confirmedReadIds.contains(message->uuid) || (previous && previous->read)) message->read = true;
                ++m_messageRevision;
                m_messages = mergeMessages(m_messages, {*message});
                pruneMessageFields();
                emit messagesChanged();
            }
            if (!current()) return true;
            // Issue #116: a background (or in-place) edit/update moves the payload the memo
            // may have been derived from; a live tombstone ends it. Same lifecycle as the
            // confirmed edit verification and deleteMessage paths.
            if (deleted) markRowPreviewDeleted(message->uuid);
            else refreshRowPreviewFromPayload(*message);
        }
    } else if (kind == "message_reaction.created" || kind == "message_reaction.updated" || kind == "message_reaction.deleted") {
        const auto source = payload.value("source_name").toString();
        if (!isUuid(id) || payload.value("project_id").toString() != m_session->project()
            || !isUuid(payload.value("message_uuid").toString()) || !isUuid(payload.value("user_uuid").toString())
            || !validReactionName(payload.value("emoji_name").toString()) || (source != "native" && source != "zulip")) return false;
        for (const auto &key : {"provider_uuid", "old_message_uuid", "old_provider_uuid"}) {
            const auto value = payload.value(key);
            if (!value.isUndefined() && !value.isNull() && !isUuid(value.toString())) return false;
        }
        for (const auto &key : {"provider_identity", "old_provider_identity"}) {
            const auto value = payload.value(key);
            if (!value.isUndefined() && !value.isNull()
                && (!value.isString() || value.toString().isEmpty() || value.toString().toUcs4().size() > 255)) return false;
        }
        const auto oldName = payload.value("old_emoji_name"), oldSource = payload.value("old_source_name");
        if ((!oldName.isUndefined() && !oldName.isNull() && !validReactionName(oldName.toString()))
            || (!oldSource.isUndefined() && !oldSource.isNull() && oldSource != "native" && oldSource != "zulip")) return false;
        QSet<QString> affected{payload.value("message_uuid").toString()};
        if (!payload.value("old_message_uuid").toString().isEmpty()) affected.insert(payload.value("old_message_uuid").toString());
        for (const auto &messageId : affected) {
            const auto *message = findMessage(messageId);
            if (!message) { staleCache(NativeCache::Kind::History); continue; }
            staleConversation(message->stream, message->topic, false);
            ++m_reactionRevisions[messageId];
            m_ownReactions.remove(messageId);
            refreshReactions(messageId);
        }
    } else if (kind == "message.deleted") {
        if (!isUuid(id) || !isUuid(payload.value("stream_uuid").toString())
            || !isUuid(payload.value("topic_uuid").toString()))
            return false;
        // A confirmed deletion is final, not a read event (issue #56): the
        // uuid leaves the unread index, the live card and every fetched
        // collection in place (activity-realtime-applier.lib.ts:66-71 and
        // :118-124), and the tombstone keeps outstanding pages from putting
        // it back. QML is notified once, after all stores are updated.
        bool mentionsChanged = mentionIndexRemove(id);
        mentionsChanged = mentionLiveRemove(id) || mentionsChanged;
        mentionsChanged = mentionCollectionsPrune(id) || mentionsChanged;
        if (mentionsChanged)
            emit stateChanged();
        // Deletion closes notifications the same way a read does in the
        // reference closeReadMessageNotifications flow.
        m_notifications.onMessagesClosed(creationOwnerKey(), {id});
        const auto outcome = std::find_if(m_chatOutcomeFacts.begin(), m_chatOutcomeFacts.end(),
            [&](const ChatOutcomeFact &fact) { return fact.id == id; });
        const bool knownOutcome = outcome != m_chatOutcomeFacts.end()
            && outcome->stream == payload.value("stream_uuid").toString();
        if (payload.value("stream_uuid").toString() == m_stream || knownOutcome) {
            const QPointer<NativeController> lookupGuard(this);
            cancelChatRequestLookup(id);
            if (!lookupGuard) return true;
            if (m_chatRequestFacts.contains(id)) {
                m_chatRequestFacts[id].deleted = true;
                m_chatRequestFacts[id].lookupRetry = ChatRequestFact::LookupRetry::LiveDeleted;
                m_chatRequestFacts[id].revision = ++m_nextChatRequestFact;
            }
            if (outcome != m_chatOutcomeFacts.end()) outcome->deleted = true;
            reduceChatKeyOutcomes();
            if (m_chatKeyResponse && m_chatKeyResponse->publicId == id) m_chatKeyResponse->deleted = true;
            interruptChatKeyResponse();
            const auto deferred = m_chatDeferredControls.find(id);
            if (deferred != m_chatDeferredControls.end()) {
                m_chatDeferredUnits -= chatControlUnits(*deferred); m_chatDeferredControls.erase(deferred);
            }
            m_chatResponses.remove(id);
        }
        invalidateConversation(payload.value("stream_uuid").toString(), payload.value("topic_uuid").toString());
        if (m_messageFields.contains(id)) m_messageFields[id].deleted = true;
        if (payload.value("stream_uuid").toString() == m_stream
            && (m_topic.isEmpty() || payload.value("topic_uuid").toString() == m_topic)) {
            ++m_messageRevision;
            m_messages.erase(std::remove_if(m_messages.begin(), m_messages.end(),
                [&id](const Message &message) { return message.uuid == id; }), m_messages.end());
            pruneMessageFields();
            emit messagesChanged();
        }
    } else if (kind == "messages.read") {
        if (payload.value("project_id").toString() != m_session->project()
            || !payload.value("message_uuids").isArray())
            return false;
        QSet<QString> ids;
        for (const auto &value : payload.value("message_uuids").toArray()) {
            if (!isUuid(value.toString()))
                return false;
            ids.insert(value.toString());
        }
        // UUIDs in a verified owner-scoped read event can also refer to saved
        // off-route pages. The flag-only patch never creates a row or trusts an
        // old pagination boundary, and every retained page stays stale.
        staleCache(NativeCache::Kind::History);
        if (m_cache && !ids.isEmpty()) m_cache->markHistoryRead(m_cacheLease, {}, {}, ids);
        m_notifications.onMessagesClosed(creationOwnerKey(), QList<QString>(ids.cbegin(), ids.cend()));
        staleCache(NativeCache::Kind::Streams);
        staleCache(NativeCache::Kind::Topics);
        ++m_messageRevision;
        m_confirmedReadIds.unite(ids);
        // Exact-uuid read events clear exactly the listed dots
        // (activity-realtime-applier.lib.ts:60-66).
        bool mentionsRead = false;
        for (const auto &readId : ids)
            mentionsRead = mentionIndexRemove(readId) || mentionsRead;
        if (mentionsRead) emit stateChanged();
        for (auto &message : m_messages)
            if (ids.contains(message.uuid))
                message.read = true;
        emit messagesChanged();
    } else if (kind == "stream.created" || kind == "stream.updated" || kind == "stream.read") {
        ++m_streamRevision;
        const auto stream = parseStream(payload, m_session->project());
        if (!stream || (stream->archived && !creationStreamDto(payload, m_session->project(), m_session->user(), false)))
            return false;
        if (kind != "stream.read") {
            const QPointer<NativeController> openingGuard(this);
            noteTopicOpeningStream(*stream);
            if (!openingGuard) return true;
        }
        // Returning to an earlier ACL tuple does not restore the authority of
        // an already dispatched operation. Preserve its unknown outcome for
        // the next explicit same-UUID reconciliation instead.
        auto authority = *stream;
        authority.archived = false;
        if (auto op = m_archiveOperations.find(stream->uuid); op != m_archiveOperations.end()
            && op->acl != streamAcl(authority)) {
            op->invalidated = true;
            op->uncertain |= op->dispatched;
            op->pending = false;
        }
        noteTopicCreationEvent(stream->uuid);
        if (kind == QLatin1String("stream.read")) inboxSlotStreamRead(stream->uuid);
        bool matchingOwn = false;
        if (const auto *intent = channelCreation(); intent && intent->uuid == stream->uuid) {
            matchingOwn = kind == "stream.created" && creationStreamDto(payload, m_session->project(), m_session->user(), true)
                && stream->name == intent->name && stream->description == intent->description
                && payload.value("invite_only").toBool() == intent->inviteOnly && payload.value("announce").toBool() == intent->announce;
        }
        noteChannelCreationEvent(stream->uuid, {}, false, matchingOwn);
        if (const auto *intent = directChat(); intent && intent->uuid == stream->uuid) {
            const auto verified = directStreamDto(payload, m_session->project(), m_session->user(), intent->peer);
            const bool lost = !verified || stream->encrypted || stream->archived
                || (!intent->acl.isEmpty() && streamAcl(*stream) != intent->acl)
                || (!intent->defaultTopic.isEmpty() && payload.value("default_topic_uuid").toString() != intent->defaultTopic);
            noteDirectChatEvent(stream->uuid, {}, false, !lost && (kind == "stream.created" || kind == "stream.read"), lost);
        }
        if (const auto *intent = channelCreation(); intent && intent->uuid == stream->uuid && intent->dispatched) {
            const bool defaultChanged = !intent->defaultTopic.isEmpty()
                && intent->defaultTopic != payload.value("default_topic_uuid").toString();
            if (defaultChanged) m_channelCreations[creationOwnerKey()].defaultTopic.clear();
            if (!creationStreamDto(payload, m_session->project(), m_session->user(), true) || defaultChanged)
                loseChannelCreationAccess(stream->uuid);
        }
        if (kind == "stream.read") {
            // Reading a stream drops its mention index entries
            // (activity-realtime-applier.lib.ts:97-103).
            if (mentionIndexClearStream(stream->uuid)) emit stateChanged();
            staleCache(NativeCache::Kind::Streams);
            staleCache(NativeCache::Kind::Topics, stream->uuid);
            staleCache(NativeCache::Kind::History, stream->uuid);
        } else {
            const auto previous = std::find_if(m_streams.cbegin(), m_streams.cend(),
                [&id](const Stream &item) { return item.uuid == id; });
            const bool accessChanged = previous == m_streams.cend()
                || previous->encrypted != stream->encrypted || previous->role != stream->role
                || previous->directUser != stream->directUser || previous->privateStream != stream->privateStream;
            if (accessChanged) {
                invalidateCache(NativeCache::Kind::Streams);
                invalidateCache(NativeCache::Kind::Topics, stream->uuid);
                invalidateCache(NativeCache::Kind::History, stream->uuid);
            } else {
                staleCache(NativeCache::Kind::Streams);
                staleCache(NativeCache::Kind::Topics, stream->uuid);
                staleCache(NativeCache::Kind::History, stream->uuid);
            }
        }
        const auto found = std::find_if(m_streams.begin(), m_streams.end(),
            [&id](const Stream &item) { return item.uuid == id; });
        if (found != m_streams.end()) {
            *found = *stream;
        } else {
            m_streams.append(*stream);
        }
        const QPointer<NativeController> guard(this);
        persistStreams();
        if (!guard) return true;
        emit streamsChanged();
        // The open inbox projects the same counters live (design doc #44).
        if (m_inboxVisible) emit stateChanged();
    } else if (kind == "stream.deleted") {
        ++m_streamRevision;
        if (!isUuid(id))
            return false;
        if (m_topicOpening.active && (m_topicOpening.routeStream == id
            || (m_topicOpening.canonical && m_topicOpening.canonical->stream == id))) {
            const QPointer<NativeController> openingGuard(this);
            interruptTopicOpening();
            if (!openingGuard) return true;
        }
        if (auto op = m_archiveOperations.find(id); op != m_archiveOperations.end()) {
            op->invalidated = true;
            op->uncertain |= op->dispatched;
            op->pending = false;
        }
        noteTopicCreationEvent(id);
        noteChannelCreationEvent(id, {}, true);
        noteDirectChatEvent(id, {}, true);
        const QPointer<NativeController> chatGuard(this);
        revokeChatAccess(id);
        if (!chatGuard) return true;
        invalidateCache(NativeCache::Kind::Streams);
        invalidateCache(NativeCache::Kind::Topics, id);
        invalidateCache(NativeCache::Kind::History, id);
        m_streams.erase(std::remove_if(m_streams.begin(), m_streams.end(),
            [&id](const Stream &stream) { return stream.uuid == id; }), m_streams.end());
        m_inboxTopics.remove(id);
        if (m_stream == id)
            selectStream({});
        emit streamsChanged();
        if (m_inboxVisible) emit stateChanged();
    } else if (kind == "topic.created" || kind == "topic.updated" || kind == "topic.read") {
        const auto streamId = payload.value("stream_uuid").toString();
        const auto topic = parseTopic(payload, m_session->project(), streamId);
        if (!topic || !isUuid(streamId))
            return false;
        const QPointer<NativeController> openingGuard(this);
        const auto openingOwner = creationOwnerKey();
        const auto openingGeneration = m_api->generation();
        const auto openingSelection = m_selection;
        const auto previousOpening = m_nextTopicOpening;
        if (kind != "topic.read" && m_topicOpening.active
            && (m_topicOpening.requested == id || (m_topicOpening.canonical && m_topicOpening.canonical->uuid == id))) {
            interruptTopicOpening();
            if (!openingGuard) return true;
        }
        noteTopicCreationEvent(streamId);
        // Inbox topic slots follow the same DTO-driven counter contract
        // (docs/NATIVE_DESKTOP_INBOX.md). Only a complete owned update owns
        // the row's placement across slots, so a whole-topic move reconciles
        // the slots (#65); counter events never move a topic.
        const bool inboxOwnsPlacement = kind == QLatin1String("topic.created")
            || (kind == QLatin1String("topic.updated")
                && creationTopicDto(payload, m_session->project(), streamId, m_session->user()).has_value());
        inboxSlotTopicEvent(kind, *topic, inboxOwnsPlacement);
        bool matchingOwn = false;
        if (const auto *intent = channelCreation(); intent && intent->uuid == streamId) {
            matchingOwn = kind == "topic.created" && topic->uuid == intent->defaultTopic && topic->isDefault
                && creationTopicDto(payload, m_session->project(), streamId, m_session->user()).has_value();
            for (const auto &value : intent->topics) if (value.toObject().value("uuid").toString() == topic->uuid) {
                const auto previous = parseTopic(value.toObject(), m_session->project(), streamId);
                matchingOwn = matchingOwn && previous && previous->name == topic->name && previous->done == topic->done
                    && previous->isDefault == topic->isDefault && previous->notificationMode == topic->notificationMode
                    && previous->unread == topic->unread && previous->activeUnread == topic->activeUnread
                    && previous->passiveUnread == topic->passiveUnread && previous->lastMessage == topic->lastMessage
                    && previous->updatedMicros == topic->updatedMicros;
                break;
            }
        }
        noteChannelCreationEvent(streamId, id, false, matchingOwn);
        if (const auto *intent = directChat(); intent && intent->uuid == streamId) {
            bool matching = (kind == "topic.created" || kind == "topic.read") && topic->isDefault
                && (intent->defaultTopic.isEmpty() || topic->uuid == intent->defaultTopic)
                && creationTopicDto(payload, m_session->project(), streamId, m_session->user()).has_value();
            for (const auto &value : intent->topics) if (value.toObject().value("uuid").toString() == topic->uuid) {
                const auto previous = parseTopic(value.toObject(), m_session->project(), streamId);
                matching = matching && previous && previous->name == topic->name && previous->done == topic->done
                    && previous->isDefault == topic->isDefault && previous->notificationMode == topic->notificationMode
                    && previous->updatedMicros == topic->updatedMicros;
                break;
            }
            noteDirectChatEvent(streamId, id, false, matching);
        }
        if (kind == "topic.read") {
            // Reading a topic drops its mention index entries
            // (activity-realtime-applier.lib.ts:90-96).
            if (mentionIndexClearTopic(streamId, topic->uuid)) emit stateChanged();
            staleCache(NativeCache::Kind::Topics, streamId);
            staleCache(NativeCache::Kind::History, streamId, topic->uuid);
        } else staleCache(NativeCache::Kind::Topics, streamId);
        // Only a complete owned update owns placement. Remove a moved known
        // row even when it is not selected, so the local fast path cannot use
        // the old stream. Counter/read projections never move a topic.
        if (kind == "topic.updated" && streamId != m_stream
            && std::any_of(m_topics.cbegin(), m_topics.cend(), [&](const Topic &row) { return row.uuid == id; })) {
            const auto canonical = creationTopicDto(payload, m_session->project(), streamId, m_session->user());
            if (!canonical) return false;
            if (!openingGuard || openingOwner != creationOwnerKey() || openingGeneration != m_api->generation()
                || openingSelection != m_selection || previousOpening != m_nextTopicOpening) return true;
            const auto oldStream = m_stream;
            const bool follow = id == m_topic;
            ++m_topicRevision;
            ++m_topicRequest; // An old in-flight list cannot restore the former placement.
            ++m_readCounterInvalidation;
            m_topics.erase(std::remove_if(m_topics.begin(), m_topics.end(), [&](const Topic &row) { return row.uuid == id; }), m_topics.end());
            if (follow) { m_topicsVerified = false; m_topicSeedUnverified = true; }
            staleCache(NativeCache::Kind::Topics, oldStream);
            // The vacated conversation is an authoritative membership loss, not
            // an offline-replayable change: drop its history pages (topic and
            // former-stream aggregate) so the former route cannot hydrate
            // messages that no longer live there. The destination keeps its
            // saved pages stale below and revalidates normally.
            invalidateConversation(oldStream, id, false);
            staleCache(NativeCache::Kind::History, streamId, id);
            emit topicsChanged();
            if (!openingGuard || openingOwner != creationOwnerKey() || openingGeneration != m_api->generation()
                || openingSelection != m_selection || previousOpening != m_nextTopicOpening) return true;
            if (follow) beginTopicOpening(id, canonical);
            else {
                emit messagesChanged();
                if (openingGuard) emit stateChanged();
            }
            return true;
        }
        if (streamId == m_stream) {
            ++m_topicRevision;
            ++m_readCounterInvalidation;
            const auto found = std::find_if(m_topics.begin(), m_topics.end(),
                [&id](const Topic &item) { return item.uuid == id; });
            if (found == m_topics.end())
                m_topics.append(*topic);
            else
                *found = *topic;
            const QPointer<NativeController> guard(this);
            emit topicsChanged();
            if (!guard) return true;
            emit messagesChanged();
            if (!guard) return true;
        }
    } else if (kind == "topic.deleted") {
        if (!isUuid(id) || !isUuid(payload.value("stream_uuid").toString()))
            return false;
        const auto deletedStream = payload.value("stream_uuid").toString();
        if (mentionIndexClearTopic(deletedStream, id)) emit stateChanged();
        {
            Topic tombstone;
            tombstone.uuid = id;
            tombstone.stream = deletedStream;
            inboxSlotTopicEvent("topic.deleted", tombstone);
        }
        if (deletedStream == m_stream && id == m_topic) {
            // Keep the explicit A/T intent, but revoke its authority and retire
            // old history/list callbacks before cancellation or any notification.
            ++m_selection;
            ++m_topicRequest;
            ++m_topicRevision;
            ++m_messageRevision;
            ++m_topicMutation;
            m_topicChanging = false;
            m_topics.erase(std::remove_if(m_topics.begin(), m_topics.end(),
                [&id](const Topic &topic) { return topic.uuid == id; }), m_topics.end());
            m_topicsVerified = m_historyVerified = false;
            m_topicSeedUnverified = true;
            m_messages.clear();
            pruneMessageFields();
            m_before.clear();
            m_loading = false;
            m_recoveringMarker = m_resumeOlder = false;
            resetReadState();

            const QPointer<NativeController> guard(this);
            const auto owner = creationOwnerKey();
            const auto generation = m_api->generation();
            const auto catalog = m_catalog;
            const auto selection = m_selection;
            const auto previousOpening = m_nextTopicOpening;
            const auto cache = m_cache.data(); // Identity only; never dereferenced.
            const auto lease = m_cacheLease;
            const auto *stream = currentStream();
            const auto security = stream ? topicOpeningSecurity(*stream) : QString{};
            const auto catalogVerified = m_catalogVerified;
            const auto syncBlocked = m_syncBlocked;
            const auto forgetting = m_cacheForgettingAccount;
            const auto current = [&] {
                if (!guard || !authenticated() || owner != creationOwnerKey()
                    || generation != m_api->generation() || catalog != m_catalog
                    || selection != m_selection || previousOpening != m_nextTopicOpening
                    || deletedStream != m_stream || id != m_topic || !unresolvedTopicRoute()
                    || cache != m_cache.data() || lease.owner != m_cacheLease.owner
                    || lease.generation != m_cacheLease.generation
                    || catalogVerified != m_catalogVerified || syncBlocked != m_syncBlocked
                    || forgetting != m_cacheForgettingAccount) return false;
                const auto *selected = currentStream();
                return security == (selected ? topicOpeningSecurity(*selected) : QString{});
            };
            interruptTopicOpening();
            if (!current()) return true;
            interruptChatRequestLookups();
            if (!current()) return true;
            cancelSnapshot();
            if (!current()) return true;
            noteTopicCreationEvent(deletedStream, id, true);
            noteChannelCreationEvent(deletedStream, id, true);
            noteDirectChatEvent(deletedStream, id, true);
            invalidateCache(NativeCache::Kind::Topics, deletedStream);
            if (!current()) return true;
            invalidateCache(NativeCache::Kind::History, deletedStream, id);
            if (!current()) return true;
            emit topicsChanged();
            if (!current()) return true;
            emit messagesChanged();
            if (!current()) return true;
            // One existing resolver owns canonical identity, latest-selection
            // cancellation and owner/cache/lifetime fences. No aggregate hop.
            beginTopicOpening(id);
            return true;
        }
        if (m_topicOpening.active && (m_topicOpening.requested == id
            || (m_topicOpening.canonical && m_topicOpening.canonical->uuid == id))) {
            const QPointer<NativeController> openingGuard(this);
            interruptTopicOpening();
            if (!openingGuard) return true;
        }
        noteTopicCreationEvent(payload.value("stream_uuid").toString(), id, true);
        noteChannelCreationEvent(payload.value("stream_uuid").toString(), id, true);
        noteDirectChatEvent(payload.value("stream_uuid").toString(), id, true);
        invalidateCache(NativeCache::Kind::Topics, payload.value("stream_uuid").toString());
        invalidateCache(NativeCache::Kind::History, payload.value("stream_uuid").toString(), id);
        if (payload.value("stream_uuid").toString() == m_stream) {
            ++m_topicRevision;
            m_topics.erase(std::remove_if(m_topics.begin(), m_topics.end(),
                [&id](const Topic &topic) { return topic.uuid == id; }), m_topics.end());
            ++m_readCounterInvalidation;
            const QPointer<NativeController> guard(this);
            if (m_topic == id) selectTopic({});
            else if (m_topic.isEmpty()) {
                ++m_messageRevision;
                m_messages.erase(std::remove_if(m_messages.begin(), m_messages.end(),
                    [&id](const Message &row) { return row.topic == id; }), m_messages.end());
                pruneMessageFields();
                emit messagesChanged();
            }
            if (!guard) return true;
            emit topicsChanged();
        }
    } else if (kind == "stream_bindings.created" || kind == "stream_binding.updated" || kind == "stream_binding.deleted") {
        const auto stream = kind == "stream_bindings.created" ? id : payload.value("stream_uuid").toString();
        if (!isUuid(id) || !isUuid(stream)) return false;
        if (kind == "stream_binding.deleted") {
            if (!isUuid(payload.value("user_uuid").toString())
                || (payload.contains("project_id") && payload.value("project_id").toString() != m_session->project())) return false;
        } else {
            const auto rows = kind == "stream_bindings.created" ? payload.value("items") : QJsonValue(QJsonArray{payload});
            QSet<QString> users;
            if (!rows.isArray() || !creationBindings(rows.toArray(), m_session->project(), stream, &users)) return false;
        }
        const auto rows = kind == "stream_bindings.created" ? payload.value("items").toArray() : QJsonArray{payload};
        bool responseRowsChanged = false;
        if (m_topicOpening.active && (m_topicOpening.routeStream == stream
            || (m_topicOpening.canonical && m_topicOpening.canonical->stream == stream))) {
            const auto previous = std::find_if(m_streams.cbegin(), m_streams.cend(), [&](const Stream &row) { return row.uuid == stream; });
            for (const auto &value : rows) {
                const auto row = value.toObject();
                if (row.value("user_uuid").toString() == m_session->user()
                    && (kind == "stream_binding.deleted" || previous == m_streams.cend()
                        || previous->role != row.value("role").toString())) {
                    const QPointer<NativeController> openingGuard(this);
                    interruptTopicOpening();
                    if (!openingGuard) return true;
                    break;
                }
            }
        }
        if (kind == "stream_binding.deleted" && payload.value("user_uuid").toString() == m_session->user()) {
            const QPointer<NativeController> guard(this);
            const bool revoked = revokeChatAccess(stream);
            if (!guard) return true;
            if (revoked) emit messagesChanged();
            if (!guard) return true;
        }
        if (stream == m_chatStream && (m_chatAuthority.ready || m_chatAuthorityLoading)) {
            bool changed = m_chatAuthorityLoading;
            for (const auto &value : rows) {
                const auto row = value.toObject(); const auto user = row.value("user_uuid").toString();
                changed = changed || (kind == "stream_binding.deleted" ? m_chatAuthority.roles.contains(user)
                    : m_chatAuthority.roles.value(user) != row.value("role").toString());
            }
            if (changed) {
                // Public response order survives a role-only authority refresh.
                // Recipient callbacks/deferred work do not; access loss above
                // has already cleared everything through revokeChatAccess.
                resetChatControls(false, ChatOutcomeRetention::PublicFacts);
                ++m_chatEpoch; ++m_chatOperation; m_chatLocalRequest.clear(); m_chatAuthority = {}; m_chatAuthorityLoading = false;
                ++m_chatPreparation; m_chatPreparingWindow.clear();
                m_chatAuthorityWaiters.clear(); m_chatResponses.clear(); m_chatBusy = false;
                m_chatPhase = "unresolved";
                interruptChatKeyResponse();
                scheduleChatControls();
                responseRowsChanged = true;
            }
        }
        bool matchingOwn = kind == "stream_bindings.created";
        if (const auto *intent = channelCreation(); intent && intent->uuid == stream) {
            for (const auto &value : rows) matchingOwn = matchingOwn && intent->members.contains(value.toObject().value("user_uuid").toString());
        } else matchingOwn = false;
        noteChannelCreationEvent(stream, {}, false, matchingOwn);
        if (const auto *intent = directChat(); intent && intent->uuid == stream) {
            bool own = kind == "stream_bindings.created", lost = false;
            for (const auto &value : rows) {
                const auto row = value.toObject(); const auto user = row.value("user_uuid").toString();
                own = own && (user == m_session->user() || user == intent->peer) && row.value("role").toString() == "owner";
                lost = lost || (user == m_session->user() && (kind == "stream_binding.deleted" || row.value("role").toString() != "owner"));
            }
            noteDirectChatEvent(stream, {}, false, own, lost);
        }
        for (const auto &value : rows) {
            const auto row = value.toObject();
            if (row.value("user_uuid").toString() == m_session->user()
                && (kind == "stream_binding.deleted" || row.value("role").toString() != "owner"))
                loseChannelCreationAccess(stream);
        }
        if (responseRowsChanged) {
            // Waiting for fresh authority must not leave the old Request row
            // admitting input while the controlled detail response is pending.
            const auto epoch = m_chatEpoch, selection = m_selection, operation = m_chatOperation;
            const QPointer<NativeController> guard(this);
            emit messagesChanged();
            if (!guard || !chatContext(epoch, selection) || m_chatOperation != operation) return true;
        }
    } else if (kind == "user.updated") {
        const auto username = payload.value("username").toString();
        if (!isUuid(id) || username.isEmpty())
            return false;
        const QPointer<NativeController> guard(this);
        const auto owner = creationOwnerKey();
        const auto generation = m_api->generation(), catalog = m_catalog;
        const auto current = [&] { return guard && owner == creationOwnerKey()
            && generation == m_api->generation() && catalog == m_catalog; };
        const auto previousName = peerDisplayName(id), previousAvatar = userAvatar(id), previousPresence = userPresence(id);
        const bool directPeerVisible = std::any_of(m_streams.cbegin(), m_streams.cend(), [&id](const Stream &stream) {
            return stream.privateStream && stream.directUser == id;
        });
        const auto choice = channelDirectoryChoice(payload, m_session->project());
        if (!choice) m_channelDirectoryValid = false;
        else if (m_channelDirectoryOwner == creationOwnerKey() && m_channelDirectoryValid && id != m_session->user())
            m_channelChoices.insert(id.toLower(), *choice);
        staleCache(NativeCache::Kind::Users);
        if (!current()) return true;
        if (id == m_session->user() && m_usersVerified) {
            m_viewerDirectoryName = directoryViewerName(payload);
            m_viewerDirectoryOwner = creationOwnerKey();
        }
        auto name = (payload.value("first_name").toString() + " "
                     + payload.value("last_name").toString()).trimmed();
        m_names.insert(id, makeUserPresentation(name.isEmpty() ? username : name, payload, true,
            m_channelDirectoryOwner == owner ? m_names.value(id) : UserPresentation{}));
        if (directPeerVisible && (peerDisplayName(id) != previousName || userAvatar(id) != previousAvatar
            || userPresence(id) != previousPresence)) emit streamsChanged();
        if (!current()) return true;
        emit messagesChanged();
        if (!current()) return true;
    }
    if (kind == "message.created" || kind == "message.updated") {
        const QPointer<NativeController> guard(this);
        if (m_chatKeyResponse && m_chatKeyResponse->dispatched && m_chatKeyResponse->publicId == id) {
            const auto intent = *m_chatKeyResponse;
            confirmChatKeyResponse(payload, intent.uuid, intent.ticket, true);
        }
        if (!guard) return true;
        const auto pending = m_pendingSends;
        for (auto it = pending.cbegin(); it != pending.cend(); ++it) {
            if (it->encrypted && it->dispatched && it->publicId == id)
                confirmEncryptedSend(payload, it.key(), it->uuid, it->ticket, true);
            if (!guard) return true;
        }
    }
    // Unsupported domains can be deliberately skipped without blocking the common cursor.
    emit stateChanged();
    return !kind.isEmpty();
}

QVariantList NativeController::projects() const
{
    QVariantList result;
    for (const auto &project : m_session->projects())
        result.append(QVariantMap{{"id", project.uuid}, {"name", project.name}});
    return result;
}

QVariantList NativeController::savedAccounts() const
{
    QVariantList result;
    for (const auto &account : m_session->savedAccounts())
        result.append(QVariantMap{{"id", account.id}, {"server", account.server.toString()},
            {"project", account.project}, {"user", account.user},
            {"login", account.login}, {"projectName", account.projectName}});
    return result;
}

namespace {
QString workspaceSessionHost(const QUrl &server)
{
    // Mirrors the reference getHostLabel: the origin hostname, with the origin
    // text itself as the fallback when the stored value is not URL-shaped.
    const auto text = server.toString().trimmed();
    auto host = server.host();
    if (host.isEmpty())
        host = QUrl(text.startsWith(QLatin1String("http")) ? text
            : QLatin1String("https://") + text).host();
    return host.isEmpty() ? text : host;
}

bool backgroundCandidateActive(const QVariantMap &candidate, const QVariantMap &snapshotsById)
{
    // top-bar-workspace-session-switcher unread lib: a candidate counts when it
    // is not own/read and its current message snapshot is missing or still an
    // unread, non-deleted row.
    if (candidate.value(QStringLiteral("isOwn")).toBool() || candidate.value(QStringLiteral("read")).toBool())
        return false;
    const auto message = candidate.value(QStringLiteral("messageUuid")).toString();
    if (!snapshotsById.contains(message)) return true;
    const auto snapshot = snapshotsById.value(message).toMap();
    if (snapshot.value(QStringLiteral("read")).toBool()) return false;
    return !snapshot.contains(QStringLiteral("deletedAt"))
        || snapshot.value(QStringLiteral("deletedAt")).isNull();
}
}

int NativeController::backgroundProjectionUnread(const QVariantMap &projection)
{
    // Direct port of getBackgroundProjectionUnreadCount
    // (top-bar-workspace-session-unread.lib.ts:21-72 at the pinned revision):
    // all-folder count, then all-folder item sum, then folder sum, then
    // folder-item sum, then non-archived stream sum plus one pending
    // unknown-stream candidate, then candidate presence. No projection => 0.
    static const QLatin1String allFolder("00000000-0000-0000-0000-000000000000");
    if (projection.isEmpty()) return 0;
    const auto folderUnread = projection.value(QStringLiteral("unreadByFolderId")).toMap();
    const auto itemUnread = projection.value(QStringLiteral("unreadByFolderItemId")).toMap();
    if (folderUnread.contains(QString::fromLatin1(allFolder)))
        return qMax(0, folderUnread.value(QString::fromLatin1(allFolder)).toInt());
    const auto topology = projection.value(QStringLiteral("folderItemTopologyById")).toMap();
    bool hasAllFolderItems = false;
    int sum = 0;
    for (auto it = topology.cbegin(); it != topology.cend(); ++it) {
        if (it.value().toMap().value(QStringLiteral("folderUuid")).toString() != QString::fromLatin1(allFolder))
            continue;
        hasAllFolderItems = true;
        sum += qMax(0, itemUnread.value(it.key()).toInt());
    }
    if (hasAllFolderItems) return sum;
    if (!folderUnread.isEmpty()) {
        for (auto it = folderUnread.cbegin(); it != folderUnread.cend(); ++it) sum += qMax(0, it.value().toInt());
        return sum;
    }
    if (!itemUnread.isEmpty()) {
        for (auto it = itemUnread.cbegin(); it != itemUnread.cend(); ++it) sum += qMax(0, it.value().toInt());
        return sum;
    }
    const auto streams = projection.value(QStringLiteral("streamSnapshotsById")).toMap();
    const auto candidates = projection.value(QStringLiteral("notificationCandidates")).toList();
    const auto snapshotsById = projection.value(QStringLiteral("messageIdSnapshotsById")).toMap();
    if (!streams.isEmpty()) {
        QSet<QString> knownStreams;
        for (auto it = streams.cbegin(); it != streams.cend(); ++it) {
            const auto snapshot = it.value().toMap();
            knownStreams.insert(snapshot.value(QStringLiteral("streamUuid")).toString());
            if (snapshot.value(QStringLiteral("isArchived")).toBool()) continue;
            const auto active = snapshot.value(QStringLiteral("activeUnreadCount"));
            sum += qMax(0, active.isNull() || !active.isValid()
                ? snapshot.value(QStringLiteral("unreadCount")).toInt() : active.toInt());
        }
        bool pendingUnknownStream = false;
        for (const auto &value : candidates) {
            const auto candidate = value.toMap();
            if (knownStreams.contains(candidate.value(QStringLiteral("streamUuid")).toString())) continue;
            if (backgroundCandidateActive(candidate, snapshotsById)) { pendingUnknownStream = true; break; }
        }
        return sum + (pendingUnknownStream ? 1 : 0);
    }
    for (const auto &value : candidates)
        if (backgroundCandidateActive(value.toMap(), snapshotsById)) return 1;
    return 0;
}

QString NativeController::unreadBadgeLabel(int unread)
{
    // The dropdown Badge uses UNREAD_COUNT_DISPLAY_LIMIT = 1000. Issue #144
    // pins the native rendering to the exact cap label "1000+" above 1000.
    return unread > 1000 ? QStringLiteral("1000+") : QString::number(unread);
}

QVariantList NativeController::sessionSwitcherSessions() const
{
    QVariantList result;
    const auto activeId = m_session->currentSavedAccountId();
    const auto liveServer = m_api->server();
    const auto liveId = authenticated() ? SessionVault::accountId(liveServer,
        m_session->project(), m_session->user()) : QString();
    QSet<QString> seen;
    const auto appendRow = [&](const QString &id, const QUrl &server, const QString &project,
        const QString &login) {
        if (seen.contains(id)) return;
        seen.insert(id);
        const auto host = workspaceSessionHost(server);
        const auto secondary = QStringLiteral("%1 \xC2\xB7 %2").arg(host, project);
        const auto projection = m_sessionProjections.constFind(id);
        const auto currentViewer = viewerDisplayName();
        result.append(QVariantMap{{"id", id}, {"primaryLabel", login},
            {"secondaryLabel", secondary},
            {"title", QStringLiteral("%1 \xC2\xB7 %2").arg(login, secondary)},
            // The native client has no per-account profile store yet: the live
            // row keeps the verified viewer name, remembered rows keep the
            // saved login, mirroring the reference username fallback.
            {"displayName", (!activeId.isEmpty() && id == activeId && !currentViewer.isEmpty())
                ? currentViewer : login},
            {"server", server.toString()},
            {"isCurrent", activeId.isEmpty()
                ? (!liveId.isEmpty() && id == liveId) : id == activeId},
            {"unread", projection == m_sessionProjections.cend() ? 0
                : backgroundProjectionUnread(projection.value())}});
    };
    // The reference renders the live session first even when it is not (yet)
    // a remembered account (top-bar-session-store.ts accountsFromApiSession).
    if (authenticated()) {
        const auto liveLogin = m_session->loginName().trimmed();
        appendRow(liveId, liveServer, m_session->project(),
            liveLogin.isEmpty() ? m_session->user() : liveLogin);
    }
    for (const auto &account : m_session->savedAccounts()) {
        const auto login = account.login.trimmed();
        appendRow(account.id, account.server, account.project,
            login.isEmpty() ? account.user : login);
    }
    return result;
}

void NativeController::publishSessionProjection(const QString &accountId, const QVariantMap &projection)
{
    if (accountId.isEmpty()) return;
    if (projection.isEmpty()) m_sessionProjections.remove(accountId);
    else m_sessionProjections.insert(accountId, projection);
    emit sessionSwitcherChanged();
}

void NativeController::selectWorkspaceSession(const QString &id)
{
    if (busy() || id.isEmpty() || id == m_session->currentSavedAccountId()) return;
    bool remembered = false;
    for (const auto &account : m_session->savedAccounts()) remembered = remembered || account.id == id;
    if (!remembered) return;
    m_error.clear();
    // The live identity leaves memory (signedOut runs the standard reset) while
    // every remembered identity survives, then the target resumes like the
    // sign-in screen's Continue.
    m_session->detach();
    m_session->resumeSavedAccount(id);
}

void NativeController::logoutWorkspaceSession(const QString &id)
{
    if (busy() || id.isEmpty()) return;
    // The switcher renders the live identity as current even when the session
    // has no saved-account binding: sessionSwitcherSessions() synthesizes the
    // row id from server, project and user. Logging out that row must clear
    // the live session, not try to forget a row the vault never remembered.
    const auto liveId = authenticated() ? SessionVault::accountId(m_api->server(),
        m_session->project(), m_session->user()) : QString();
    const bool current = id == m_session->currentSavedAccountId()
        || (!liveId.isEmpty() && id == liveId);
    if (!current) { forgetSavedAccount(id); return; }
    QString nextId;
    bool remembered = false;
    for (const auto &account : m_session->savedAccounts()) {
        if (account.id == id) { remembered = true; continue; }
        if (nextId.isEmpty()) nextId = account.id;
    }
    if (!remembered) {
        // Synthesized live identity: there is no durable entry to erase, so
        // sign the live session out (standard teardown path) and hand the
        // window to the next remembered inbox.
        m_session->logout();
        if (!nextId.isEmpty()) m_session->resumeSavedAccount(nextId);
        return;
    }
    // Removing the active session detaches the live identity, removes the
    // remembered account through the same durable-cache path, then activates
    // the next remembered inbox (resolveWorkspacePostLogoutRoute). The
    // successor must be chosen before the removal, because the vault list
    // drops the entry synchronously.
    forgetSavedAccountWithFollowUp(id, [this, nextId] {
        // exo-review #170 round 2: the user may pick another account while the
        // removal is in flight. resumeSavedAccount binds its identity to the
        // session synchronously, so a bound saved account here means a newer
        // session choice already took over - that choice wins over the
        // successor captured when this logout was issued.
        if (nextId.isEmpty() || !m_session->currentSavedAccountId().isEmpty()) return;
        m_session->resumeSavedAccount(nextId);
    });
}

void NativeController::openAddServer()
{
    if (busy()) return;
    // The reference "Add server" navigates to /login with remembered sessions
    // intact; the native sign-in step opens once the live session is gone.
    m_session->detach();
}

namespace {
QString archiveAuthority(Stream stream)
{
    // Archive changes are the intended state transition, not a permission loss.
    stream.archived = false;
    return streamAcl(stream);
}
}

bool NativeController::archiveAvailable() const
{
    return authenticated() && m_catalogVerified && !m_snapshotActive && !m_syncBlocked && !m_cacheForgettingAccount;
}

bool NativeController::archiveLoading() const
{
    return authenticated() && (m_snapshotActive || (!m_catalogVerified && m_error.isEmpty()));
}

QString NativeController::archiveError() const
{
    return authenticated() && !m_catalogVerified && !m_snapshotActive ? m_error : QString{};
}

bool NativeController::archiveContext(const QString &id, quint64 ticket) const
{
    const auto op = m_archiveOperations.constFind(id);
    const auto row = std::find_if(m_streams.cbegin(), m_streams.cend(), [&id](const Stream &v) { return v.uuid == id; });
    return op != m_archiveOperations.cend() && op->ticket == ticket && !op->invalidated && op->owner == creationOwnerKey()
        && op->generation == m_api->generation() && op->catalog == m_catalog && archiveAvailable()
        && op->cache == m_cache && op->lease.owner == m_cacheLease.owner && op->lease.generation == m_cacheLease.generation
        && row != m_streams.cend() && archiveAuthority(*row) == op->acl;
}

bool NativeController::canUnarchiveChat(const QString &id) const
{
    const auto row = std::find_if(m_streams.cbegin(), m_streams.cend(), [&id](const Stream &v) { return v.uuid == id; });
    const auto op = m_archiveOperations.constFind(id);
    if (!archiveAvailable() || row == m_streams.cend() || !row->archived
        || (op != m_archiveOperations.cend() && archiveContext(id, op->ticket) && op->pending)) return false;
    int pending = 0;
    bool terminal = false;
    for (auto it = m_archiveOperations.cbegin(); it != m_archiveOperations.cend(); ++it) {
        pending += it->pending && archiveContext(it.key(), it->ticket);
        terminal |= !it->pending && !it->uncertain;
    }
    return pending < 32 && (op != m_archiveOperations.cend() || m_archiveOperations.size() < 128 || terminal);
}

QVariantList NativeController::archivedChats() const
{
    QVariantList result;
    int pending = 0;
    bool terminal = false;
    for (auto it = m_archiveOperations.cbegin(); it != m_archiveOperations.cend(); ++it) {
        pending += it->pending && archiveContext(it.key(), it->ticket);
        terminal |= !it->pending && !it->uncertain;
    }
    const bool room = m_archiveOperations.size() < 128 || terminal;
    for (const auto &stream : m_streams) {
        if (!stream.archived) continue;
        const auto op = m_archiveOperations.constFind(stream.uuid);
        const bool current = op != m_archiveOperations.cend() && archiveContext(stream.uuid, op->ticket);
        result.append(QVariantMap{{"id", stream.uuid},
            {"name", stream.directUser.isEmpty() ? stream.name : peerDisplayName(stream.directUser, stream.name)},
            {"isDirect", !stream.directUser.isEmpty()}, {"encrypted", stream.encrypted},
            {"pending", current && op->pending}, {"error", current ? op->error : QString{}},
            {"canUnarchive", archiveAvailable() && pending < 32 && (!current || !op->pending)
                && (op != m_archiveOperations.cend() || room)}});
    }
    return result;
}

void NativeController::openArchivedChat(const QString &id)
{
    if (!authenticated()) return;
    const auto row = std::find_if(m_streams.cbegin(), m_streams.cend(), [&id](const Stream &v) { return v.uuid == id && v.archived; });
    if (row != m_streams.cend()) activateStream(id);
}

void NativeController::unarchiveChat(const QString &id)
{
    if (!canUnarchiveChat(id)) return;
    const auto row = std::find_if(m_streams.cbegin(), m_streams.cend(), [&id](const Stream &v) { return v.uuid == id; });
    const auto old = m_archiveOperations.value(id);
    const bool reconcile = old.owner == creationOwnerKey() && (old.uncertain || (old.pending && old.dispatched));
    // Keep unknown server outcomes until an explicit same-UUID reconciliation.
    // Only settled entries can yield capacity within the 128-row bound.
    if (!m_archiveOperations.contains(id) && m_archiveOperations.size() >= 128) {
        auto oldest = m_archiveOperations.end();
        for (auto it = m_archiveOperations.begin(); it != m_archiveOperations.end(); ++it)
            if (!it->pending && !it->uncertain
                && (oldest == m_archiveOperations.end() || it->ticket < oldest->ticket)) oldest = it;
        if (oldest == m_archiveOperations.end()) return;
        m_archiveOperations.erase(oldest);
    }
    ArchiveOperation operation;
    operation.owner = creationOwnerKey(); operation.acl = archiveAuthority(*row);
    operation.generation = m_api->generation(); operation.catalog = m_catalog;
    operation.cache = m_cache; operation.lease = m_cacheLease; operation.ticket = ++m_nextArchiveOperation;
    operation.pending = true; operation.uncertain = reconcile;
    m_archiveOperations[id] = operation;
    const QPointer<NativeController> guard(this);
    emit streamsChanged();
    if (!guard || !archiveContext(id, operation.ticket)) return;
    if (reconcile) readUnarchive(id, operation.ticket, true);
    else postUnarchive(id, operation.ticket);
}

void NativeController::postUnarchive(const QString &id, quint64 ticket)
{
    if (!archiveContext(id, ticket)) return;
    m_archiveOperations[id].dispatched = true;
    request("POST", messenger + "/streams/" + id + "/actions/unarchive/invoke", {}, {},
        [this, id, ticket](const ApiResult &result) {
        if (!archiveContext(id, ticket)) return;
        const auto raw = result.data.toObject();
        const auto row = result.ok() ? creationStreamDto(raw, m_session->project(), m_session->user(), false) : std::optional<Stream>{};
        if (row && row->uuid == id && !row->archived) { applyUnarchive(id, ticket, raw); return; }
        // Known refusals do not imply a command was applied. Other outcomes are
        // reconciled by one exact read; never repeat a POST automatically.
        if (result.status >= 400 && result.status < 500 && result.status != 408 && result.status != 429) {
            finishUnarchive(id, ticket, tr("Could not unarchive the chat. Try again."), false); return;
        }
        readUnarchive(id, ticket, false);
    });
}

void NativeController::readUnarchive(const QString &id, quint64 ticket, bool mayPost)
{
    if (!archiveContext(id, ticket)) return;
    request("GET", messenger + "/streams/" + id, {}, {}, [this, id, ticket, mayPost](const ApiResult &result) {
        if (!archiveContext(id, ticket)) return;
        const auto raw = result.data.toObject();
        const auto row = result.ok() ? creationStreamDto(raw, m_session->project(), m_session->user(), false) : std::optional<Stream>{};
        if (row && row->uuid == id) {
            if (!row->archived) { applyUnarchive(id, ticket, raw); return; }
            const auto current = std::find_if(m_streams.cbegin(), m_streams.cend(), [&id](const Stream &v) { return v.uuid == id; });
            if (mayPost && current != m_streams.cend() && row->updatedMicros >= current->updatedMicros
                && archiveAuthority(*row) == m_archiveOperations.value(id).acl) {
                postUnarchive(id, ticket); return;
            }
        }
        if (result.status == 403 || result.status == 404) {
            const auto operation = m_archiveOperations.value(id);
            const auto selection = m_selection;
            const QPointer<NativeController> guard(this);
            const auto owned = [&] {
                return guard && operation.owner == creationOwnerKey() && operation.generation == m_api->generation()
                    && operation.catalog == m_catalog && operation.cache == m_cache
                    && operation.lease.owner == m_cacheLease.owner && operation.lease.generation == m_cacheLease.generation
                    && archiveAvailable();
            };
            const auto current = [&] { return owned() && selection == m_selection; };
            m_archiveOperations.remove(id);
            m_streams.erase(std::remove_if(m_streams.begin(), m_streams.end(), [&id](const Stream &v) { return v.uuid == id; }), m_streams.end());
            invalidateCache(NativeCache::Kind::Streams); if (!current()) return;
            invalidateCache(NativeCache::Kind::Topics, id); if (!current()) return;
            invalidateCache(NativeCache::Kind::History, id); if (!current()) return;
            if (m_stream == id) {
                selectStream({});
                if (!owned() || m_selection != selection + 1 || !m_stream.isEmpty()) return;
            }
            emit streamsChanged();
            return;
        }
        finishUnarchive(id, ticket, tr("Could not unarchive the chat. Try again."), true);
    });
}

void NativeController::finishUnarchive(const QString &id, quint64 ticket, const QString &error, bool uncertain)
{
    if (!archiveContext(id, ticket)) return;
    auto &op = m_archiveOperations[id]; op.pending = false; op.error = error; op.uncertain = uncertain;
    emit streamsChanged();
}

void NativeController::applyUnarchive(const QString &id, quint64 ticket, const QJsonObject &raw)
{
    if (!archiveContext(id, ticket)) return;
    const auto canonical = creationStreamDto(raw, m_session->project(), m_session->user(), false);
    const auto found = std::find_if(m_streams.begin(), m_streams.end(), [&id](const Stream &v) { return v.uuid == id; });
    if (!canonical || canonical->uuid != id || canonical->archived || found == m_streams.end()
        || archiveAuthority(*canonical) != m_archiveOperations.value(id).acl) {
        finishUnarchive(id, ticket, tr("Could not unarchive the chat. Try again."), true); return;
    }
    // Only archive state/timestamp are owned by this command. A newer event and
    // all concurrent counters/name/notification projections retain authority.
    if (canonical->updatedMicros >= found->updatedMicros) {
        found->archived = false; found->updatedMicros = canonical->updatedMicros;
    }
    auto &op = m_archiveOperations[id]; op.pending = op.uncertain = false; op.error.clear();
    ++m_streamRevision;
    const QPointer<NativeController> guard(this);
    persistStreams();
    if (!guard || !archiveContext(id, ticket)) return;
    emit streamsChanged();
}

void NativeController::persistStreams()
{
    ++m_cacheRevision;
    if (!m_cache || !authenticated()) return;
    QJsonArray rows;
    for (const auto &stream : m_streams) {
        QJsonObject row{{"uuid", stream.uuid}, {"project_id", m_session->project()}, {"name", stream.name},
            {"description", stream.description}, {"direct_user_uuid", stream.directUser.isEmpty() ? QJsonValue::Null : QJsonValue(stream.directUser)},
            {"role", stream.role}, {"encryption", stream.encrypted}, {"is_archived", stream.archived}, {"private", stream.privateStream},
            {"unread_count", stream.unread}, {"active_unread_count", stream.activeUnread}, {"passive_unread_count", stream.passiveUnread},
            {"last_message_uuid", stream.lastMessage.isEmpty() ? QJsonValue::Null : QJsonValue(stream.lastMessage)}};
        if (!stream.notificationMode.isEmpty()) row["notification_mode"] = stream.notificationMode;
        if (stream.color) row["color"] = int(*stream.color);
        if (stream.updatedMicros) row["updated_at"] = QDateTime::fromMSecsSinceEpoch(stream.updatedMicros / 1000, QTimeZone::UTC)
            .toString("yyyy-MM-dd'T'HH:mm:ss.") + QString("%1").arg(stream.updatedMicros % 1000000, 6, 10, QLatin1Char('0')) + 'Z';
        rows.append(row);
    }
    // A local projection is saved and marked stale in one optional worker job;
    // it must not depend on a second required-control queue admission.
    m_cache->put(m_cacheLease, {NativeCache::Kind::Streams, {}, {}, {}}, rows,
                 {}, nullptr, {}, true);
}

QVariantList NativeController::streams() const
{
    const bool avatarsOwned = authenticated() && m_channelDirectoryOwner == creationOwnerKey();
    QVector<const Stream *> ordered;
    for (const auto &stream : m_streams)
        if (!stream.archived) ordered.append(&stream);
    // workspace_ui 61dacef entities/messenger/messenger-sidebar.lib.ts:236-290: the default
    // sort mode is last_message, so the order is group rank first and then last-message
    // activity descending with unknown activity last. Ties keep the server order because the
    // native stream DTO carries neither orderIndex nor a pin flag (pin is issue #98).
    QHash<QString, qint64> activity;
    for (const auto *stream : ordered)
        activity.insert(stream->uuid, rowActivityMicros(stream->lastMessage, stream->updatedMicros));
    std::stable_sort(ordered.begin(), ordered.end(), [&](const Stream *a, const Stream *b) {
        const int rankA = streamGroupRank(*a), rankB = streamGroupRank(*b);
        if (rankA != rankB) return rankA < rankB;
        return activity.value(a->uuid) > activity.value(b->uuid);
    });
    QVariantList result;
    for (const auto *entry : ordered) {
        const auto &stream = *entry;
        auto row = QVariantMap{{"id", stream.uuid}, {"name", stream.name},
            {"description", stream.description}, {"unread", stream.unread}, {"activeUnread", stream.activeUnread},
            {"passiveUnread", stream.passiveUnread}, {"lastMessage", stream.lastMessage}, {"encrypted", stream.encrypted},
            {"isDirect", stream.privateStream && isUuid(stream.directUser) && stream.directUser != m_session->user()},
            {"peerId", stream.privateStream ? stream.directUser : QString{}},
            {"displayName", stream.privateStream && isUuid(stream.directUser)
                ? peerDisplayName(stream.directUser) : stream.name},
            {"avatar", avatarsOwned && stream.privateStream && isUuid(stream.directUser) && stream.directUser != m_session->user()
                ? m_names.value(stream.directUser).avatar : QString{}},
            {"presence", avatarsOwned && stream.privateStream && isUuid(stream.directUser) && stream.directUser != m_session->user()
                ? m_names.value(stream.directUser).presence : QString{}},
            {"activityMicros", activity.value(stream.uuid)},
            {"contextKey", QString::fromLatin1(QCryptographicHash::hash(streamAcl(stream).toUtf8(), QCryptographicHash::Sha256).toHex())}};
        row.insert(rowPreviewFields(stream.lastMessage));
        result.append(row);
    }
    return result;
}

QVariantList NativeController::topics() const
{
    QHash<QString, qint64> timestamps;
    for (const auto &message : m_messages) timestamps.insert(message.uuid, message.createdMicros);
    const auto *stream = currentStream();
    const auto rank = [stream](const Topic *topic) {
        if (topic->done) return 2;
        return topic->notificationMode == "mute" || (topic->notificationMode == "default"
            && stream && stream->notificationMode == "muted") ? 1 : 0;
    };
    QVector<const Topic *> sorted;
    for (const auto &topic : m_topics) sorted.append(&topic);
    std::stable_sort(sorted.begin(), sorted.end(), [&](const Topic *a, const Topic *b) {
        if (rank(a) != rank(b)) return rank(a) < rank(b);
        return timestamps.value(a->lastMessage, a->updatedMicros) > timestamps.value(b->lastMessage, b->updatedMicros);
    });
    QVariantList result;
    for (const auto *topic : sorted) {
        auto row = QVariantMap{{"id", topic->uuid}, {"name", topic->name.trimmed()},
            {"unread", topic->unread}, {"activeUnread", topic->activeUnread}, {"passiveUnread", topic->passiveUnread},
            {"lastMessage", topic->lastMessage}, {"done", topic->done},
            {"activityMicros", timestamps.value(topic->lastMessage, topic->updatedMicros)}};
        row.insert(rowPreviewFields(topic->lastMessage));
        result.append(row);
    }
    return result;
}

namespace {
struct ParsedLeadingQuote { QString id, label, text, rest; };

QString unescapeQuoteLabel(const QString &value)
{
    // Undo workspace::escapeWorkspaceMarkdownInline label escaping.
    static const QString special = QStringLiteral("\\`*_{()[]#+.!|>~-");
    QString out;
    out.reserve(value.size());
    for (qsizetype i = 0; i < value.size(); ++i) {
        if (value[i] == QChar('\\') && i + 1 < value.size() && special.contains(value[i + 1])) {
            out += value[++i];
            continue;
        }
        out += value[i];
    }
    return out;
}

// Source serializes a reply as "[Name](urn:quote:uuid[?text=...])" + blank line +
// answer (workspace_ui 61dacef workspace-reply.lib.ts:190-212). This display parse
// accepts only a standalone leading reference as the quote frame; references deeper
// in the body keep rendering as inline links.
std::optional<ParsedLeadingQuote> leadingQuoteReference(const QString &content)
{
    static const QRegularExpression pattern(
        "^\\s*\\[((?:\\\\.|[^\\\\\\]\\n])*)\\]\\((urn:[^)]+)\\)[ \\t]*(?:\\r?\\n+([\\s\\S]*))?$");
    const auto match = pattern.match(content);
    if (!match.hasMatch()) return std::nullopt;
    const auto reference = workspace::parseWorkspaceReference(match.captured(2));
    if (!reference || reference->kind != workspace::WorkspaceReferenceKind::Quote) return std::nullopt;
    ParsedLeadingQuote parsed;
    parsed.id = reference->id;
    parsed.label = unescapeQuoteLabel(match.captured(1)).trimmed();
    if (reference->text) parsed.text = *reference->text;
    parsed.rest = match.captured(3).trimmed();
    return parsed;
}
} // namespace

QVariantList NativeController::messages() const
{
    const bool avatarsOwned = authenticated() && m_channelDirectoryOwner == creationOwnerKey();
    const bool canReadChat = chatReadAllowed();
    const auto chat = encryption();
    const bool canRequestKey = chat.value("canRequest").toBool();
    QHash<QString, QVariantMap> requestMetadata;
    for (const auto &value : chat.value("keyRequests").toList()) {
        const auto row = value.toMap(); requestMetadata.insert(row.value("id").toString(), row);
    }
    QHash<QString, QString> topicNames;
    for (const auto &topic : m_topics) topicNames.insert(topic.uuid, topic.name.trimmed());
    // Display correlation can use a loaded typed R even before the key store
    // opens. Existing negative observations take precedence over this index.
    QHash<QString, const Message *> loadedRequests;
    for (const auto &message : m_messages)
        if (message.kind == "key_request" && message.crypto && message.project == m_session->project()
            && message.stream == m_stream) loadedRequests.insert(message.uuid, &message);
    const bool authorityCurrent = chatContext(m_chatEpoch, m_selection) && m_chatAuthority.ready
        && m_chatAuthority.epoch == m_chatEpoch && m_chatAuthority.owner == ownerKey() && m_chatAuthority.stream == m_stream;
    const auto displayName = [this](const QString &id) {
        const auto name = userDisplayName(id);
        return name.trimmed().isEmpty() ? QStringLiteral("#") + id.left(8) : name;
    };
    const auto responseMetadata = [&](const Message &message) -> QVariantMap {
        if (!message.crypto || (message.kind != "key_grant" && message.kind != "key_reject")) return {};
        const auto &response = *message.crypto;
        QString recipient, deviceName;
        bool correlated = false;
        const auto observed = m_chatRequestFacts.constFind(response.requestMessage);
        if (observed != m_chatRequestFacts.cend()) {
            correlated = !observed->deleted && observed->stream == message.stream
                && observed->key == response.keyUuid && observed->device == response.recipientDevice
                && isUuid(observed->author);
            if (correlated) { recipient = observed->author; deviceName = observed->name; }
        } else if (const auto request = loadedRequests.value(response.requestMessage)) {
            correlated = request->project == message.project && request->stream == message.stream
                && request->crypto->keyUuid == response.keyUuid && request->crypto->deviceId == response.recipientDevice;
            if (correlated) { recipient = request->author; deviceName = request->crypto->deviceName; }
        }
        const bool ownCurrent = correlated && recipient == m_session->user() && canReadChat
            && response.recipientDevice == m_chatKeys->device().id;
        const auto key = m_chatIssueKeys.value(response.keyUuid);
        const auto issuerRole = m_chatAuthority.roles.value(message.author);
        const bool allowedIssuer = authorityCurrent && (message.author == m_chatAuthority.issuerOwner
            || issuerRole == "owner" || issuerRole == "administrator");
        const bool offer = ownCurrent && message.kind == "key_reject" && allowedIssuer
            && isUuid(response.keyUuid) && key.state == ChatKeyAvailability::Missing;
        return {{"requestId", response.requestMessage}, {"keyUuid", response.keyUuid},
            {"senderName", displayName(message.author)}, {"recipientKnown", correlated},
            {"recipientId", recipient}, {"recipientName", correlated ? displayName(recipient) : QString{}},
            {"deviceName", deviceName}, {"ownCurrentDevice", ownCurrent},
            {"hasKey", canReadChat && key.state == ChatKeyAvailability::Present},
            {"offerRequest", offer}, {"canRequest", offer && canRequestKey && key.pendingKnown && !key.requested}};
    };
    const auto announcementMetadata = [&](const Message &message) -> QVariantMap {
        if (message.kind != "key_announced" || !message.crypto) return {};
        const auto &key = message.crypto->keyUuid;
        const auto available = m_chatIssueKeys.value(key);
        const auto role = m_chatAuthority.roles.value(message.author);
        const bool allowedIssuer = authorityCurrent && (message.author == m_chatAuthority.issuerOwner
            || role == "owner" || role == "administrator");
        const bool offer = canReadChat && !m_chatRequestFactsLimited && allowedIssuer
            && available.state == ChatKeyAvailability::Missing;
        return {{"keyUuid", key}, {"offerRequest", offer},
            {"canRequest", offer && canRequestKey && available.pendingKnown && !available.requested}};
    };
    const auto serviceStatus = [&](const Message &message, const QVariantMap &response,
                                   const QVariantMap &announcement) -> QString {
        if (!message.crypto || message.kind == "e2ee" || message.kind == "key_grant") return {};
        if (message.kind == "key_request") {
            const auto row = requestMetadata.value(message.uuid);
            if (row.isEmpty()) return QStringLiteral("unavailable");
            if (row.value("status").toString() != "pending") return {};
            // This is Source's inline presentation rule, not Core admission.
            const bool inlineActions = !row.value("ownCurrentDevice").toBool();
            const bool offered = inlineActions && (row.value("offerGrant").toBool() || row.value("offerReject").toBool());
            const bool enabled = inlineActions && (row.value("canGrant").toBool() || row.value("canReject").toBool());
            if (enabled) return QStringLiteral("action-required");
            if (offered || row.value("status").toString() == "pending") return QStringLiteral("waiting");
            return QStringLiteral("unavailable");
        }
        if (message.kind != "key_reject" && message.kind != "key_announced") return {};
        if (!canReadChat || m_chatRequestFactsLimited) return QStringLiteral("unavailable");
        if (message.kind == "key_reject") {
            if (message.crypto->recipientDevice != m_chatKeys->device().id) return {};
            if (!response.value("recipientKnown").toBool()) return QStringLiteral("unavailable");
            if (!response.value("ownCurrentDevice").toBool()) return {};
        }
        const auto key = m_chatIssueKeys.value(message.crypto->keyUuid);
        if (key.state == ChatKeyAvailability::Present) return {};
        // Unknown/error are not proof of a missing key. This explicit absence
        // of Native facts is unavailable until the bounded work publishes them.
        if (key.state != ChatKeyAvailability::Missing)
            return QStringLiteral("unavailable");
        const auto &actions = message.kind == "key_reject" ? response : announcement;
        if (actions.value("canRequest").toBool()) return QStringLiteral("action-required");
        if (actions.value("offerRequest").toBool()) return QStringLiteral("waiting");
        return {};
    };
    QVariantList result;
    for (const auto &message : m_messages) {
        // Match the source desktop's ownership presentation. Keep the raw
        // server flag intact for independent mutation-confirmation checks.
        const bool ownMessage = message.own || message.author == m_session->user();
        const auto prepared = m_chatPrepared.constFind(message.uuid);
        const bool preparedCurrent = canReadChat && prepared != m_chatPrepared.cend() && prepared->hash == chatPayloadHash(message);
        const bool decrypted = preparedCurrent && prepared->state == ChatKeyAvailability::Present;
        const auto encryptionState = message.kind != "e2ee" ? QString{} : !canReadChat ? QStringLiteral("locked")
            : !preparedCurrent ? !m_chatRejectedWindow.isEmpty() && m_chatRejectedWindow == m_chatPreparingWindow
                ? QStringLiteral("unavailable") : QStringLiteral("preparing")
            : prepared->state == ChatKeyAvailability::Missing ? QStringLiteral("missing")
            : prepared->state == ChatKeyAvailability::Present ? QStringLiteral("ready") : QStringLiteral("error");
        const auto content = decrypted ? prepared->content : message.kind == "markdown" ? message.content
            : message.kind == "e2ee" ? tr("Encrypted message — key support is under development")
                                    : tr("System message (%1)").arg(message.kind);
        // Quote frame fields (docs/NATIVE_DESKTOP_REPLIES.md): a leading quote
        // reference resolves from the loaded window, else from the resolved-quote
        // store fed by GET /messages/?uuid=, mirroring messenger-quote-resolver
        // .hook.ts:17-58 (store hit -> ready, fetch -> loading, miss -> unavailable).
        QString quoteId, quoteLabel, quoteSelection, quoteStatus, quoteAuthorName, quoteContent;
        auto displayContent = content;
        if (const auto quote = leadingQuoteReference(content)) {
            quoteId = quote->id;
            quoteLabel = quote->label;
            quoteSelection = quote->text;
            displayContent = quote->rest;
            const auto fallback = quoteLabel.isEmpty() ? tr("Quote") : quoteLabel;
            if (const auto *origin = findMessage(quoteId); origin && origin->kind == "markdown") {
                quoteStatus = QStringLiteral("ready");
                quoteAuthorName = userDisplayName(origin->author, fallback);
                quoteContent = origin->content;
            } else if (const auto record = m_quotes.constFind(quoteId); record != m_quotes.cend()) {
                if (record->missing) {
                    quoteStatus = QStringLiteral("unavailable");
                } else {
                    quoteStatus = QStringLiteral("ready");
                    quoteAuthorName = userDisplayName(record->authorId, fallback);
                    quoteContent = record->content;
                }
            } else {
                quoteStatus = QStringLiteral("loading");
                scheduleQuoteFetch(quoteId);
            }
        }
        QVariantList reactions;
        auto names = message.reactions.keys();
        auto intent = m_reactionIntents.constFind(message.uuid);
        if (intent != m_reactionIntents.cend() && intent->owner != ownerKey()) intent = m_reactionIntents.cend();
        if (intent != m_reactionIntents.cend() && !names.contains(intent->emoji)) names.append(intent->emoji);
        for (const auto &name : names) {
            const auto users = message.reactionUsers.value(name);
            QVariantList participants;
            for (const auto &id : users) {
                auto displayName = userDisplayName(id);
                if (displayName.isEmpty()) displayName = '#' + id.left(8);
                participants.append(QVariantMap{{"id", id}, {"name", displayName}});
            }
            const bool ownKnown = message.reactionUsers.contains(name) || m_ownReactions.contains(message.uuid);
            const bool own = m_ownReactions.contains(message.uuid) ? m_ownReactions.value(message.uuid).contains(name)
                                                                  : users.contains(m_session->user());
            reactions.append(QVariantMap{{"emojiName", name}, {"unicode", reactionUnicode(name)},
                {"count", message.reactions.value(name)}, {"userUuids", users}, {"participants", participants},
                {"own", own}, {"ownKnown", ownKnown},
                {"pending", intent != m_reactionIntents.cend() && intent->emoji == name},
                {"uncertain", intent != m_reactionIntents.cend() && intent->emoji == name && intent->uncertain}});
        }
        const auto topicName = topicNames.value(message.topic);
        const auto response = responseMetadata(message);
        const auto announcement = announcementMetadata(message);
        result.append(QVariantMap{{"id", message.uuid}, {"authorId", message.author},
            {"topicId", message.topic}, {"topicName", topicName},
            {"author", userDisplayName(message.author, ownMessage ? tr("You") : QStringLiteral("#") + message.author.left(8))},
            {"authorAvatar", avatarsOwned ? m_names.value(message.author).avatar : QString{}},
            {"authorPresence", avatarsOwned ? m_names.value(message.author).presence : QString{}},
            {"content", content}, {"displayContent", displayContent},
            {"quoteId", quoteId}, {"quoteLabel", quoteLabel}, {"quoteText", quoteSelection},
            {"quoteStatus", quoteStatus}, {"quoteAuthorName", quoteAuthorName}, {"quoteContent", quoteContent},
            {"time", message.created.toLocalTime().toString("HH:mm")},
            {"createdAt", message.created.toUTC().toString(Qt::ISODateWithMs)},
            {"decrypted", decrypted},
            {"encryptionKeyUuid", message.kind == "e2ee" && message.crypto ? message.crypto->keyUuid : QString{}},
            {"encryptionState", encryptionState},
            {"canRequestKey", canRequestKey && preparedCurrent && prepared->state == ChatKeyAvailability::Missing},
            {"keyServiceResponse", response},
            {"keyServiceAnnouncement", announcement},
            {"serviceStatus", serviceStatus(message, response, announcement)},
            {"own", ownMessage}, {"kind", message.kind}, {"starred", message.starred}, {"read", message.read},
            {"reactions", reactions}, {"reactionBusy", intent != m_reactionIntents.cend()},
            {"reactionUncertain", intent != m_reactionIntents.cend() && intent->uncertain},
            {"canReact", authoritativeRoute() && inConversation(message) && !m_busy && !m_loading
                && !m_snapshotActive && (intent == m_reactionIntents.cend() || !intent->checking)}});
    }
    return result;
}

bool NativeController::messageHistoryReady() const
{
    return authenticated() && hasConversation() && m_historyVerified && !m_topicSeedUnverified
        && !m_snapshotActive && !m_syncBlocked
        && (!m_cacheProtection || (m_catalogVerified && m_topicsVerified));
}

const Stream *NativeController::currentStream() const
{
    const auto found = std::find_if(m_streams.begin(), m_streams.end(),
        [this](const Stream &stream) { return stream.uuid == m_stream; });
    return found == m_streams.end() ? nullptr : &*found;
}

bool NativeController::unresolvedTopicRoute() const
{
    // A canonical opening seed contains its Topic row and can read history.
    // A retired explicit topic has no row: keep poll/live bodies out even after
    // the resolver finishes with 404 or invalid/foreign metadata.
    return m_topicSeedUnverified && !m_topic.isEmpty()
        && std::none_of(m_topics.cbegin(), m_topics.cend(), [&](const Topic &topic) {
            return topic.uuid == m_topic && topic.stream == m_stream;
        });
}

void NativeController::offerNotificationCandidate(const Message &message)
{
    NotificationCandidate candidate;
    candidate.owner = creationOwnerKey();
    candidate.messageUuid = message.uuid;
    candidate.streamUuid = message.stream;
    candidate.topicUuid = message.topic;
    candidate.authorUuid = message.author;
    candidate.content = message.content;
    candidate.isOwn = message.own;
    candidate.read = message.read;
    candidate.mentioned = message.mentioned.value_or(false);
    candidate.createdMicros = message.createdMicros;
    // The caller only offers messages outside the on-screen conversation.
    candidate.viewport.windowFocused = m_windowFocused;
    candidate.viewport.conversationOnScreen = false;
    m_notifications.ingest(candidate);
}

void NativeController::setWindowFocused(bool focused)
{
    m_windowFocused = focused;
    m_notifications.setWindowFocused(focused);
}

bool NativeController::inConversation(const Message &message) const
{
    return hasConversation() && !unresolvedTopicRoute()
        && message.project == m_session->project() && message.stream == m_stream
        && (m_topic.isEmpty() || message.topic == m_topic);
}

const Message *NativeController::findMessage(const QString &id) const
{
    const auto found = std::find_if(m_messages.begin(), m_messages.end(),
        [&id](const Message &message) { return message.uuid == id; });
    return found == m_messages.end() ? nullptr : &*found;
}

QString NativeController::title() const
{
    const auto *stream = currentStream();
    if (isDirectConversation()) return directPeer().value("displayName").toString();
    const auto name = stream ? stream->name : tr("Workspace");
    for (const auto &topic : m_topics)
        if (topic.uuid == m_topic)
            return topic.name.isEmpty() ? name : topic.name + " · " + name;
    return name;
}

QString NativeController::encryptedSendTarget() const
{
    const auto *stream = currentStream();
    if (!stream || !stream->encrypted || !stream->security || !m_topicsVerified || m_topicSeedUnverified) return {};
    if (!m_topic.isEmpty()) {
        for (const auto &topic : m_topics) if (topic.uuid == m_topic && !topic.done) return topic.uuid;
        return {};
    }
    QString result;
    int defaults = 0;
    for (const auto &topic : m_topics) if (topic.isDefault) { result = topic.uuid; ++defaults; }
    return defaults == 1 && (stream->security->defaultTopic.isEmpty() || result == stream->security->defaultTopic) ? result : QString{};
}

bool NativeController::canCompose() const
{
    const auto *stream = currentStream();
    if (!stream || !authoritativeRoute()) return false;
    if (!stream->encrypted) return !m_topic.isEmpty() && !topicClosed();
    return chatContext(m_chatEpoch, m_selection) && m_chatCurrentKeyState != ChatKeyAvailability::Unknown
        && !m_chatSendKey.isEmpty()
        && stream->security && isUuid(stream->security->currentKey)
        && stream->security->viewer == m_session->user() && stream->security->source == "native"
        && stream->security->provider.isEmpty() && stream->security->providerIdentity.isEmpty()
        && QStringList{"guest", "member", "moderator", "administrator", "owner"}.contains(stream->role)
        && !encryptedSendTarget().isEmpty();
}

QVariantMap NativeController::pendingSendMetadata() const
{
    const auto found = m_pendingSends.constFind(conversationKey());
    if (found == m_pendingSends.cend() || !found->encrypted || found->owner != ownerKey()) return {};
    const bool check = !found->busy && !m_sending && encryptedSendOwned(*found, false);
    return {{"id", found->publicId}, {"targetTopic", found->topic}, {"phase", found->phase},
        {"error", found->error}, {"busy", found->busy}, {"canCheck", check},
        {"canRetry", check && encryptedSendOwned(*found, true)}};
}

bool NativeController::encryptedSendOwned(const SendIntent &intent, bool writing) const
{
    const auto stream = std::find_if(m_streams.cbegin(), m_streams.cend(),
        [&](const Stream &row) { return row.uuid == intent.stream; });
    // Public delivery status depends on the verified account and owned stream,
    // independently of a local key, device, visible topic or closed Vault.
    if (!intent.encrypted || !authenticated() || intent.owner != ownerKey() || intent.project != m_session->project()
        || intent.user != m_session->user() || (m_chatAccessKnown && !m_chatReadableStreams.contains(intent.stream))
        || !m_catalogVerified || m_snapshotActive || m_syncBlocked || stream == m_streams.cend()
        || !stream->encrypted || !stream->security || stream->security->viewer != intent.user
        || stream->security->source != "native" || !stream->security->provider.isEmpty()
        || !stream->security->providerIdentity.isEmpty()
        || !QStringList{"guest", "member", "moderator", "administrator", "owner"}.contains(stream->role)) return false;
    if (!writing) return true;
    if (!authoritativeRoute() || !m_topicsVerified || m_topicSeedUnverified || m_stream != intent.stream
        || m_topic != intent.routeTopic || !isUuid(stream->security->currentKey) || intent.materialLost
        || !intent.vault || intent.vault != m_chatKeys || !chatVaultMatches()
        || !sameChatLease(intent.vaultLease, m_chatKeys->lease())) return false;
    // A frozen aggregate intent retains its actual original topic if another
    // topic later becomes default. Retry never resolves or encrypts a new target.
    return std::any_of(m_topics.cbegin(), m_topics.cend(), [&](const Topic &topic) {
        return topic.uuid == intent.topic && (intent.routeTopic.isEmpty() || !topic.done);
    });
}

bool NativeController::encryptedSendContext(const QString &conversation, const QString &id, quint64 ticket) const
{
    const auto found = m_pendingSends.constFind(conversation);
    if (found == m_pendingSends.cend() || found->uuid != id || found->ticket != ticket
        || !encryptedSendOwned(*found, false) || found->catalog != m_catalog
        || found->accessRevision != m_chatAccessRevision || found->projection.generation != m_api->generation()
        || found->projection.cache != m_cache || !sameChatCache(found->projection.lease, m_cacheLease)
        || (m_cache && !sameChatCache(m_cache->lease(), found->projection.lease))) return false;
    // Automatic publication belongs to the original material/device attempt.
    // An explicit GET-only Check can rebind public delivery status after local
    // deletion/close without granting any authority to resend or restore keys.
    if (!found->statusOnly && (found->materialLost || !found->vault || found->vault != m_chatKeys
        || !chatVaultMatches() || !sameChatLease(found->vaultLease, m_chatKeys->lease()))) return false;
    const auto stream = std::find_if(m_streams.cbegin(), m_streams.cend(),
        [&](const Stream &row) { return row.uuid == found->stream; });
    return stream != m_streams.cend() && stream->security && stream->security->currentKey == found->currentKey;
}

void NativeController::captureEncryptedSend(SendIntent &intent)
{
    intent.selection = m_selection; intent.catalog = m_catalog; intent.accessRevision = m_chatAccessRevision;
    intent.projection = projectionContext(); intent.currentKey = currentStream()->security->currentKey;
    intent.ticket = ++m_nextEncryptedSend; intent.busy = true; intent.error.clear();
}

void NativeController::interruptEncryptedSends()
{
    for (auto it = m_pendingSends.begin(); it != m_pendingSends.end(); ++it) {
        if (!it->encrypted || !it->busy || encryptedSendContext(it.key(), it->uuid, it->ticket)) continue;
        const auto active = it.key() + '/' + it->uuid + '/' + QString::number(it->ticket);
        if (m_activeEncryptedSend == active) { m_activeEncryptedSend.clear(); m_sending = false; }
        it->busy = false; it->phase = "uncertain";
        it->error = tr("Sending was interrupted. Check the saved message status before retrying.");
        ++it->ticket;
    }
}

void NativeController::sendEncryptedText()
{
    if (!canSend()) return;
    SendIntent intent;
    intent.uuid = QUuid::createUuid().toString(QUuid::WithoutBraces); intent.text = draft(); intent.encrypted = true;
    intent.draftRevision = m_draftRevisions.value(conversationKey());
    intent.owner = ownerKey(); intent.user = m_session->user(); intent.project = m_session->project();
    intent.stream = m_stream; intent.routeTopic = m_topic; intent.topic = encryptedSendTarget();
    intent.key = m_chatSendKey; intent.deletion = m_chatSendDeletion; intent.vault = m_chatKeys;
    intent.vaultLease = m_chatKeys->lease();
    intent.publicId = QUuid::createUuidV5(QUuid(intent.topic), intent.uuid.toUtf8()).toString(QUuid::WithoutBraces);
    captureEncryptedSend(intent);
    auto stored = m_chatKeys->read(intent.vaultLease, {intent.stream, intent.key});
    const auto clear = qScopeGuard([&] { wipeChatBytes(stored.value.bytes); });
    if (stored.failure != NativeChatKeyVault::Failure::None || stored.value.bytes.size() != 32
        || stored.value.deletionRevision != intent.deletion || !encryptedSendOwned(intent, true)) return;
    auto plaintext = intent.text.toUtf8();
    const auto clearText = qScopeGuard([&] { wipeChatBytes(plaintext); });
    auto encoded = NativeChatCrypto::encryptMessage(stored.value.bytes, plaintext, {intent.stream, intent.key});
    const auto clearEncoded = qScopeGuard([&] { wipeChatBytes(encoded.bytes); });
    if (!encoded) { report(tr("The message could not be encrypted. Your text is preserved.")); return; }
    const auto wire = QString::fromLatin1(encoded.bytes.toBase64());
    if (wire.size() > 213336) { report(tr("The encrypted message exceeds the server size limit. Your text is preserved.")); return; }
    intent.payload = {{"kind", "e2ee"}, {"key_uuid", intent.key}, {"content", wire}};
    intent.phase = "sending";
    const auto conversation = conversationKey();
    m_pendingSends.insert(conversation, intent);
    ++m_messageFields[intent.publicId].pending;
    m_sending = true; m_activeEncryptedSend = conversation + '/' + intent.uuid + '/' + QString::number(intent.ticket);
    const QPointer<NativeController> guard(this);
    emit stateChanged();
    if (guard && encryptedSendContext(conversation, intent.uuid, intent.ticket))
        postEncryptedSend(conversation, intent.uuid, intent.ticket);
}

void NativeController::finishEncryptedSend(const QString &conversation, const QString &id, quint64 ticket, const QString &error)
{
    auto found = m_pendingSends.find(conversation);
    if (found == m_pendingSends.end() || found->uuid != id || found->ticket != ticket) return;
    if (m_activeEncryptedSend == conversation + '/' + id + '/' + QString::number(ticket)) {
        m_activeEncryptedSend.clear(); m_sending = false;
    }
    found->busy = false; found->phase = "uncertain"; found->error = error;
    if (found->owner == ownerKey() && conversation == conversationKey()) m_error = error;
    emit stateChanged();
}

void NativeController::postEncryptedSend(const QString &conversation, const QString &id, quint64 ticket)
{
    if (!encryptedSendContext(conversation, id, ticket)) return;
    auto intent = m_pendingSends.value(conversation);
    if (!encryptedSendOwned(intent, true)) {
        finishEncryptedSend(conversation, id, ticket, tr("Sending is unavailable. Check the saved message status.")); return;
    }
    m_pendingSends[conversation].dispatched = true;
    const QJsonObject body{{"uuid", intent.uuid}, {"stream_uuid", intent.stream}, {"topic_uuid", intent.topic}, {"payload", intent.payload}};
    request("POST", messenger + "/messages/", body, {}, [this, conversation, id, ticket](const ApiResult &result) {
        if (!encryptedSendContext(conversation, id, ticket)) return;
        const auto intent = m_pendingSends.value(conversation);
        if (!result.ok() || !matchesEncryptedSend(result.data.toObject(), intent, false)) {
            finishEncryptedSend(conversation, id, ticket, tr("Sending was not confirmed. Check the saved message status.")); return;
        }
        readEncryptedSend(conversation, id, ticket, false);
    });
}

bool NativeController::matchesEncryptedSend(const QJsonObject &raw, const SendIntent &intent, bool realtime) const
{
    const auto parsed = parseMessage(raw, intent.project);
    return parsed && parsed->uuid == intent.publicId && parsed->stream == intent.stream && parsed->topic == intent.topic
        && parsed->author == intent.user && parsed->own && parsed->kind == "e2ee" && parsed->crypto
        && parsed->crypto->keyUuid == intent.key && raw.value("payload").toObject() == intent.payload
        && (realtime || raw.value("user_uuid").toString() == intent.user);
}

void NativeController::readEncryptedSend(const QString &conversation, const QString &id, quint64 ticket, bool retry)
{
    if (!encryptedSendContext(conversation, id, ticket)) return;
    const auto intent = m_pendingSends.value(conversation);
    request("GET", messenger + "/messages/" + intent.publicId, {}, {}, [this, conversation, id, ticket, retry](const ApiResult &result) {
        if (!encryptedSendContext(conversation, id, ticket)) return;
        if (result.ok() && confirmEncryptedSend(result.data.toObject(), conversation, id, ticket)) return;
        const auto found = m_pendingSends.constFind(conversation);
        if (found == m_pendingSends.cend() || found->uuid != id || found->ticket != ticket) return;
        if (retry && result.status == 404 && encryptedSendOwned(*found, true)) {
            auto key = m_chatKeys->read(found->vaultLease, {found->stream, found->key});
            const bool usable = key.failure == NativeChatKeyVault::Failure::None && key.value.bytes.size() == 32
                && key.value.deletionRevision == found->deletion;
            wipeChatBytes(key.value.bytes);
            if (usable) { postEncryptedSend(conversation, id, ticket); return; }
        }
        finishEncryptedSend(conversation, id, ticket, tr("The saved message status is unresolved. Your text is preserved."));
    });
}

bool NativeController::confirmEncryptedSend(const QJsonObject &raw, const QString &conversation, const QString &id,
                                           quint64 ticket, bool realtime)
{
    if (!encryptedSendContext(conversation, id, ticket)) return false;
    const auto intent = m_pendingSends.value(conversation);
    if (!matchesEncryptedSend(raw, intent, realtime) || m_messageFields.value(intent.publicId).deleted) return false;
    auto saved = parseMessage(raw, intent.project);
    if (!saved) return false;
    const auto field = m_messageFields.value(intent.publicId);
    if (field.initialized && field.payloadHash != QCryptographicHash::hash(
        QJsonDocument(intent.payload).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256)) return false;
    m_pendingSends.remove(conversation);
    if (m_activeEncryptedSend == conversation + '/' + id + '/' + QString::number(ticket)) {
        m_activeEncryptedSend.clear(); m_sending = false;
    }
    if (m_drafts.value(conversation) == intent.text && m_draftRevisions.value(conversation) == intent.draftRevision)
        m_drafts.remove(conversation);
    m_attachmentNotices.remove(conversation);
    const QPointer<NativeController> guard(this);
    const auto owned = [&] {
        if (!guard || intent.owner != ownerKey() || intent.projection.generation != m_api->generation()
            || intent.catalog != m_catalog || intent.projection.cache != m_cache
            || !sameChatCache(intent.projection.lease, m_cacheLease)
            || (m_cache && !sameChatCache(m_cache->lease(), intent.projection.lease))
            || intent.accessRevision != m_chatAccessRevision || !encryptedSendOwned(intent, false)) return false;
        const auto stream = std::find_if(m_streams.cbegin(), m_streams.cend(),
            [&](const Stream &row) { return row.uuid == intent.stream; });
        return stream != m_streams.cend() && stream->security && stream->security->currentKey == intent.currentKey;
    };
    const auto visible = [&] {
        return owned() && intent.selection == m_selection && m_stream == intent.stream && m_topic == intent.routeTopic;
    };
    const bool show = visible();
    if (show && !findMessage(saved->uuid)) {
        if (m_confirmedReadIds.contains(saved->uuid)) saved->read = true;
        observeMessage(raw); m_messages = mergeMessages(m_messages, {*saved}); ++m_messageRevision;
    }
    // Release the held field/tombstone counter before optional cache work or
    // any publication can reenter, navigate or admit another conversation.
    finishMessageIntent(intent.publicId);
    if (show) m_error.clear();
    projectCanonical(raw, NativeCache::Projection::Insert, intent.projection);
    if (!owned()) return true;
    if (visible()) {
        emit draftChanged(); if (!owned()) return true;
        if (visible()) { emit messagesChanged(); if (!owned()) return true; }
    }
    emit stateChanged();
    if (visible()) emit messageSent(intent.publicId, intent.stream, intent.topic);
    return true;
}

void NativeController::checkPendingSend() { recoverEncryptedSend(false); }
void NativeController::retryPendingSend() { recoverEncryptedSend(true); }
void NativeController::recoverEncryptedSend(bool retry)
{
    const auto conversation = conversationKey();
    auto found = m_pendingSends.find(conversation);
    if (found == m_pendingSends.end() || found->busy || m_sending || !encryptedSendOwned(*found, retry)) return;
    found->statusOnly = !retry;
    captureEncryptedSend(*found); found->phase = "checking";
    const auto id = found->uuid; const auto ticket = found->ticket;
    m_sending = true; m_activeEncryptedSend = conversation + '/' + id + '/' + QString::number(ticket);
    const QPointer<NativeController> guard(this); emit stateChanged();
    if (guard && encryptedSendContext(conversation, id, ticket)) readEncryptedSend(conversation, id, ticket, retry);
}

bool NativeController::canSend() const
{
    // While an edit session is open the save control, not send, owns submission.
    if (!m_editingMessage.isEmpty()) return false;
    const auto *stream = currentStream();
    if (stream && stream->encrypted)
        return canCompose() && !m_sending && m_pendingSends.value(conversationKey()).uuid.isEmpty()
            && !draft().trimmed().isEmpty() && boundedText(draft(), 40000);
    return authoritativeRoute() && stream && !stream->encrypted && !m_topic.isEmpty()
        && (!durableDraftRoute() || m_draftStore->ready(m_stream, m_topic))
        && !draft().contains(":::")
        && !m_pendingSends.value(conversationKey()).encrypted
        && !topicClosed() && !m_sending && m_pendingUploads.value(conversationKey()) == 0
        && !draft().trimmed().isEmpty() && draft().size() <= 40000;
}

bool NativeController::topicClosed() const
{
    for (const auto &topic : m_topics)
        if (topic.uuid == m_topic) return topic.done;
    return false;
}

bool NativeController::canReopenTopic() const
{
    return authenticated() && m_catalogVerified && m_topicsVerified && !m_syncBlocked
        && !busy() && !m_stream.isEmpty() && !m_topic.isEmpty() && topicClosed();
}

void NativeController::reopenTopic()
{
    if (!canReopenTopic()) return;
    const auto stream = m_stream, topic = m_topic;
    const auto selection = m_selection, attempt = ++m_topicMutation;
    ++m_topicRevision;
    staleCache(NativeCache::Kind::Topics, stream);
    m_topicChanging = true;
    m_error.clear();
    const QPointer<NativeController> guard(this);
    emit stateChanged();
    if (!guard) return;
    request("POST", messenger + "/stream_topics/" + topic + "/actions/toggle_done/invoke", {}, {},
        [this, stream, topic, selection, attempt](const ApiResult &mutation) {
        if (attempt != m_topicMutation || selection != m_selection || stream != m_stream || topic != m_topic) return;
        const auto revision = m_topicRevision;
        // toggle_done is non-idempotent. Confirm even an uncertain POST through
        // a read; never repeat the command to discover whether it succeeded.
        request("GET", messenger + "/stream_topics/" + topic, {}, {},
            [this, stream, topic, selection, attempt, revision, mutationError = mutation.error](const ApiResult &result) {
            if (attempt != m_topicMutation || selection != m_selection || stream != m_stream || topic != m_topic) return;
            m_topicChanging = false;
            if (revision != m_topicRevision) {
                // A newer event wins. Obtain a fresh catalog before allowing a
                // second toggle, including when the event concerned a peer topic.
                m_topicsVerified = false;
                loadTopics();
                notifyTrustChanged();
                return;
            }
            const auto raw = result.data.toObject();
            const auto confirmed = result.ok() && raw.value("is_done").isBool()
                ? parseTopic(raw, m_session->project(), stream) : std::optional<Topic>{};
            if (!confirmed || confirmed->uuid != topic) {
                m_topicsVerified = false;
                report(tr("Could not confirm whether the topic reopened. Refresh before trying again."));
                return;
            }
            for (auto &item : m_topics)
                if (item.uuid == topic) { item = *confirmed; break; }
            ++m_topicRevision;
            m_topicsVerified = true;
            const QPointer<NativeController> self(this);
            emit topicsChanged();
            if (!self) return;
            if (confirmed->done)
                report(tr("Could not reopen the topic. %1").arg(mutationError));
            else
                notifyTrustChanged();
        });
    });
}

void NativeController::setDraft(const QString &text)
{
    // Reference parity: while editing, composer typing never reaches the draft store
    // (message-composer-draft.hook.ts skips onValueChange while isEditing).
    if (!m_editingMessage.isEmpty()) {
        if (m_editBuffer == text) return;
        m_editBuffer = text;
        ++m_editRevision;   // a save may not close past this revision (issue #115)
        emit draftChanged();
        emit stateChanged();
        return;
    }
    if ((m_topic.isEmpty() && (!currentStream() || !currentStream()->encrypted || !hasConversation())) || draft() == text)
        return;
    const QPointer<NativeController> guard(this);
    if (durableDraftRoute()) {
        m_draftStore->edit(m_stream, m_topic, text);
    } else {
        m_drafts.insert(conversationKey(), text); ++m_draftRevisions[conversationKey()];
    }
    if (!guard) return;
    emit draftChanged();
    emit stateChanged();
}

void NativeController::collection(const QString &path, QUrlQuery query,
                                 std::function<void(const QJsonArray &)> done,
                                 QJsonArray accumulated, QSet<QByteArray> markers,
                                 std::function<void()> failed)
{
    const auto catalog = m_catalog;
    query.removeAllQueryItems("page_limit");
    query.addQueryItem("page_limit", "200");
    request("GET", path, {}, query,
        [this, path, query, done, accumulated, markers, catalog, failed](const ApiResult &result) mutable {
        // Exactly one terminal outcome runs per collection walk (#64): the
        // rows delivery or, when a page fails or the epoch is gone, the
        // failure callback. Callers release their in-flight bookkeeping on
        // either terminal, never from a dropped reply.
        if (catalog != m_catalog) {
            if (failed) failed();
            return;
        }
        if (!result.ok() || !result.data.isArray()) {
            report(tr("Could not load the catalog. %1").arg(result.error));
            if (failed) failed();
            return;
        }
        for (const auto &value : result.data.toArray())
            accumulated.append(value);
        if (!result.nextMarker.isEmpty()) {
            if (markers.contains(result.nextMarker) || accumulated.size() > 100000) {
                report(tr("The server returned an invalid pagination cursor."));
                if (failed) failed();
                return;
            }
            markers.insert(result.nextMarker);
            query.removeAllQueryItems("page_marker");
            query.addQueryItem("page_marker", QString::fromUtf8(result.nextMarker));
            collection(path, query, done, accumulated, markers, failed);
        } else {
            done(accumulated);
        }
    });
}

void NativeController::refresh()
{
    if (m_draftStore) { m_draftFlushFailed = false; m_draftStore->retry(); }
    if (m_realtimeMode) {
        emit resynchronizeRequested();
        return;
    }
    reloadSnapshot();
}

void NativeController::retryBootstrap()
{
    // The reference Retry re-runs messenger bootstrap (bootstrapRetryNonce);
    // reloadSnapshot is the same path the realtime snapshot recovery uses.
    reloadSnapshot();
}

void NativeController::observeTransport(const ApiResult &result)
{
    bool changed = false;
    if (result.transportFailed) {
        if (!m_transportOffline) { m_transportOffline = true; changed = true; }
    } else if (result.status > 0) {
        // Any HTTP response proves the server is reachable again.
        if (m_transportOffline) { m_transportOffline = false; changed = true; }
        if (result.status == 429) {
            // The reference keeps the rate-limit countdown running until it
            // expires (connection-health retryAfterMs); a fresh 429 extends
            // it but never shortens it: a racing response with a smaller
            // Retry-After must not clear the banner before the deadline of
            // the earlier 429 expires.
            const auto seconds = result.retryAfterSeconds > 0 ? result.retryAfterSeconds : 30;
            const auto nextDeadline = QDateTime::currentMSecsSinceEpoch() + qint64(seconds) * 1000;
            m_rateLimitedUntilMs = qMax(m_rateLimitedUntilMs, nextDeadline);
            if (!m_connectionCountdown.isActive()) m_connectionCountdown.start();
            changed = true;
        }
    }
    if (changed)
        emit stateChanged();
}

QString NativeController::connectionPhase() const
{
    // Reference priority (layout-connection-banner.lib.ts:8-40): offline >
    // rate limit countdown > reconnecting > degraded.
    if (m_transportOffline)
        return QStringLiteral("offline");
    if (m_rateLimitedUntilMs > QDateTime::currentMSecsSinceEpoch())
        return QStringLiteral("rate_limited");
    if (m_syncState == QLatin1String("connecting") || m_syncState == QLatin1String("catching_up")
        || m_syncState == QLatin1String("retrying") || m_syncState == QLatin1String("waiting_for_session"))
        return QStringLiteral("reconnecting");
    if (m_syncState == QLatin1String("invalid_data") || m_syncState == QLatin1String("snapshot_required"))
        return QStringLiteral("degraded");
    return QStringLiteral("online");
}

QString NativeController::connectionSeverity() const
{
    // Critical for offline/blocked kinds, warning for everything else.
    return connectionPhase() == QLatin1String("offline")
        ? QStringLiteral("critical") : QStringLiteral("warning");
}

int NativeController::connectionRetryInSeconds() const
{
    const auto remaining = m_rateLimitedUntilMs - QDateTime::currentMSecsSinceEpoch();
    return remaining > 0 ? int((remaining + 999) / 1000) : 0;
}

void NativeController::reloadSnapshot()
{
    if (!authenticated())
        return;
    interruptChannelCreations();
    interruptDirectChats();
    m_error.clear();
    markCacheStale();
    if (m_realtimeMode) m_syncBlocked = true;
    m_catalogVerified = m_usersVerified = m_topicsVerified = m_historyVerified = false;
    const auto catalog = ++m_catalog;
    resetReadState();
    ++m_selection;
    m_loading = false;
    m_snapshotActive = true;
    m_snapshotParts = 3;
    m_snapshotCatalog = catalog;
    m_snapshotSelection = m_selection;
    m_recoveringMarker = m_resumeOlder = false;
    notifyTrustChanged();
    const auto chatAccess = m_chatAccessRevision;
    const auto chatOwner = ownerKey();
    const auto chatEpoch = m_chatEpoch;
    const auto chatProjection = projectionContext();
    cachedCollection({NativeCache::Kind::Streams, {}, {}, {}}, messenger + "/streams/", {},
        [this, catalog, chatAccess, chatOwner, chatEpoch, chatProjection](const QJsonArray &rows, bool authoritative) {
        if (catalog != m_catalog) return false;
        if (!authoritative && !m_streams.isEmpty()) return true;
        QVector<Stream> streams;
        QSet<QString> ids;
        for (const auto &value : rows) {
            const auto parsed = parseStream(value.toObject(), m_session->project());
            if (!parsed || ids.contains(parsed->uuid) || (authoritative && parsed->archived
                && !creationStreamDto(value.toObject(), m_session->project(), m_session->user(), false))) {
                report(tr("The server returned an invalid channel list."));
                return false;
            }
            ids.insert(parsed->uuid);
            streams.append(*parsed);
        }
        const QPointer<NativeController> chatGuard(this);
        if (authoritative) restoreChatAccess(rows, chatAccess, chatOwner, chatEpoch, chatProjection);
        if (!chatGuard) return false;
        m_streams = streams;
        m_catalogVerified = authoritative;
        emit streamsChanged();
        if (authoritative && !m_stream.isEmpty()) {
            if (!currentStream()) {
                m_snapshotActive = false;
                selectStream({});
                m_snapshotActive = true;
                m_snapshotSelection = m_selection;
            } else {
                m_snapshotParts |= 4;
                loadTopics();
                m_snapshotParts |= 8;
                loadMessages();
            }
        }
        if (authoritative) finishSnapshotPart(1);
        notifyTrustChanged();
        return true;
    }, true);
    cachedCollection({NativeCache::Kind::Users, {}, {}, {}}, "/api/workspace/v1/users/", {},
        [this, catalog](const QJsonArray &rows, bool authoritative) {
        if (catalog != m_catalog) return false;
        if (!authoritative && !m_names.isEmpty()) return true;
        QMap<QString, QStringList> previousDirectPresentation;
        for (const auto &stream : m_streams)
            if (stream.privateStream && isUuid(stream.directUser))
                previousDirectPresentation.insert(stream.directUser,
                    {peerDisplayName(stream.directUser), userAvatar(stream.directUser), userPresence(stream.directUser)});
        const auto previousPresentation = m_channelDirectoryOwner == creationOwnerKey()
            ? m_names : QHash<QString, UserPresentation>{};
        updateChannelDirectory(rows, authoritative);
        m_names.clear();
        if (authoritative) {
            m_viewerDirectoryName.clear();
            m_viewerDirectoryOwner = creationOwnerKey();
        }
        for (const auto &value : rows) {
            const auto json = value.toObject();
            const auto id = json.value("uuid").toString();
            if (authoritative && id == m_session->user()) m_viewerDirectoryName = directoryViewerName(json);
            QString name;
            if (json.contains("first_name") || json.contains("last_name")) {
                name = directoryViewerName(json);
            } else {
                name = json.value("name").toString();
                if (name.isEmpty())
                    name = json.value("display_name").toString();
                if (name.isEmpty())
                    name = json.value("username").toString();
            }
            if (isUuid(id) && !name.isEmpty())
                m_names.insert(id, makeUserPresentation(name, json, authoritative, previousPresentation.value(id)));
        }
        m_usersVerified = authoritative;
        bool labelsChanged = false;
        for (auto it = previousDirectPresentation.cbegin(); it != previousDirectPresentation.cend(); ++it)
            if (it.value() != QStringList{peerDisplayName(it.key()), userAvatar(it.key()), userPresence(it.key())}) {
                labelsChanged = true; break;
            }
        const QPointer<NativeController> guard(this);
        const auto owner = creationOwnerKey();
        const auto generation = m_api->generation();
        const auto current = [&] { return guard && catalog == m_catalog
            && owner == creationOwnerKey() && generation == m_api->generation(); };
        if (labelsChanged) emit streamsChanged();
        if (!current()) return false;
        emit messagesChanged();
        if (!current()) return false;
        if (authoritative) finishSnapshotPart(2);
        if (!current()) return false;
        emit stateChanged();
        return true;
    }, true);
}

void NativeController::selectStream(const QString &id)
{
    if (!m_selectingDraftRoute) m_draftOpenId.clear();
    m_draftsVisible = false; if (m_draftStore) m_draftStore->hidePage();
    closeMentions();
    activateStream(id);
}

void NativeController::activateStream(const QString &id, const QVector<Topic> *verifiedTopics,
                                      const QString &directPublishingAttempt,
                                      const Topic *initialTopic, quint64 openingTicket)
{
    if (!id.isEmpty() && std::none_of(m_streams.begin(), m_streams.end(),
            [&id](const Stream &stream) { return stream.uuid == id; }))
        return;
    cancelConversationRestore();
    const QPointer<NativeController> guard(this);
    const auto previousOpening = m_nextTopicOpening;
    if (openingTicket) {
        if (!initialTopic || !topicOpeningContext(openingTicket)) return;
        // Cancelling our old snapshot is part of this route commit. All other
        // owner/API/cache/selection witnesses remain unchanged and checked.
        m_topicOpening.snapshotActive = false;
    } else interruptTopicOpening();
    if (!guard || previousOpening != m_nextTopicOpening) return;
    interruptChannelCreations();
    interruptDirectChats(directPublishingAttempt);
    if (!guard) return;
    cancelSnapshot();
    if (!guard || previousOpening != m_nextTopicOpening
        || (openingTicket && !topicOpeningContext(openingTicket))) return;
    interruptChatRequestLookups();
    if (!guard) return;
    const bool seedUnverified = initialTopic || (!verifiedTopics && id == m_stream && m_topicSeedUnverified);
    resetReadState();
    m_topicChanging = false;
    ++m_topicMutation;
    ++m_selection;
    m_stream = id;
    m_topicsVerified = m_historyVerified = false;
    m_topic = initialTopic ? initialTopic->uuid : QString{};
    rememberConversation();
    settleEditRouteChange();
    m_topics = initialTopic ? QVector<Topic>{*initialTopic}
        : verifiedTopics ? *verifiedTopics : QVector<Topic>{};
    m_topicsVerified = verifiedTopics != nullptr && !initialTopic;
    m_topicSeedUnverified = seedUnverified;
    m_messages.clear();
    pruneMessageFields();
    m_before.clear();
    m_after.clear();
    m_loading = false;
    m_recoveringMarker = m_resumeOlder = false;
    const auto selection = m_selection;
    if (openingTicket) {
        // Only this synchronous, signal-free route assignment advances the
        // expected selection. Never rebind after a subscriber changes lifetime.
        m_topicOpening.selection = selection;
        m_topicOpening.routeStream = m_stream;
        m_topicOpening.routeTopic = m_topic;
        m_topicOpening.routeSecurity = topicOpeningSecurity(*currentStream());
    }
    const auto current = [&] {
        return guard && selection == m_selection
            && (!openingTicket || topicOpeningContext(openingTicket));
    };
    emit topicsChanged();
    if (!current()) return;
    emit messagesChanged();
    if (!current()) return;
    emit stateChanged();
    if (!current()) return;
    emit draftChanged();
    if (!current()) return;
    if (!id.isEmpty() && !verifiedTopics) loadTopics();
    if (current() && hasConversation()) loadMessages();
}

void NativeController::loadTopics()
{
    const auto selection = m_selection;
    const auto topicRequest = ++m_topicRequest;
    const auto revision = m_topicRevision;
    QUrlQuery query;
    query.addQueryItem("stream_uuid", m_stream);
    cachedCollection({NativeCache::Kind::Topics, m_stream, {}, {}}, messenger + "/stream_topics/", query,
        [this, selection, topicRequest, revision](const QJsonArray &rows, bool authoritative) {
        if (selection != m_selection || topicRequest != m_topicRequest) return false;
        if (!authoritative && !m_topics.isEmpty()) return true;
        if (revision != m_topicRevision) {
            loadTopics();
            return false;
        }
        QVector<Topic> topics;
        QSet<QString> ids;
        for (const auto &value : rows) {
            const auto topic = parseTopic(value.toObject(), m_session->project(), m_stream);
            if (!topic || ids.contains(topic->uuid)) {
                report(tr("The server returned an invalid topic list."));
                return false;
            }
            ids.insert(topic->uuid);
            topics.append(*topic);
        }
        m_topics = topics;
        ++m_readCounterInvalidation;
        m_topicsVerified = authoritative;
        if (authoritative) m_topicSeedUnverified = false;
        if (authoritative && !m_topic.isEmpty() && !ids.contains(m_topic))
            selectTopic({});
        const QPointer<NativeController> guard(this);
        emit topicsChanged();
        if (!guard) return true;
        emit messagesChanged();
        if (!guard) return true;
        if (authoritative) finishSnapshotPart(4);
        notifyTrustChanged();
        return true;
    }, m_topicSeedUnverified);
}

void NativeController::interruptTopicOpening()
{
    const auto api = m_topicOpening.api;
    const auto request = m_topicOpening.request;
    // Logical ownership is revoked before abort; cancellation callbacks may be
    // synchronous. They can neither publish nor clear a replacement intent.
    m_topicOpening = {};
    if (api && request.isValid()) api->cancelRequest(request);
}

bool NativeController::topicOpeningContext(quint64 ticket) const
{
    const auto &intent = m_topicOpening;
    if (!intent.active || intent.ticket != ticket || !intent.api || intent.api != m_api
        || intent.owner != creationOwnerKey() || intent.generation != m_api->generation()
        || intent.catalog != m_catalog || intent.selection != m_selection
        || intent.routeStream != m_stream || intent.routeTopic != m_topic
        || intent.cache != m_cache || intent.lease.owner != m_cacheLease.owner
        || intent.lease.generation != m_cacheLease.generation
        || intent.catalogVerified != m_catalogVerified || intent.snapshotActive != m_snapshotActive
        || intent.syncBlocked != m_syncBlocked || intent.forgetting != m_cacheForgettingAccount)
        return false;
    const auto *current = currentStream();
    if (intent.routeSecurity != (current ? topicOpeningSecurity(*current) : QString{})) return false;
    if (intent.canonical && !intent.targetSecurity.isEmpty()) {
        const auto target = std::find_if(m_streams.cbegin(), m_streams.cend(), [&](const Stream &stream) {
            return stream.uuid == intent.canonical->stream;
        });
        if (target == m_streams.cend() || topicOpeningSecurity(*target) != intent.targetSecurity) return false;
    }
    return true;
}

void NativeController::finishTopicOpening(quint64 ticket, const QString &error)
{
    if (!m_topicOpening.active || m_topicOpening.ticket != ticket) return;
    const bool current = topicOpeningContext(ticket);
    const QPointer<NativeController> guard(this);
    interruptTopicOpening();
    if (!guard) return;
    if (current && !error.isEmpty()) m_error = error;
    emit stateChanged();
}

void NativeController::openTopic(const QString &id, const QString &)
{
    if (!m_selectingDraftRoute) m_draftOpenId.clear();
    const auto target = id.toLower();
    if (m_topicOpening.active && m_topicOpening.requested == target
        && topicOpeningContext(m_topicOpening.ticket)) return;
    const QPointer<NativeController> guard(this);
    const auto previousOpening = m_nextTopicOpening;
    interruptTopicOpening();
    if (!guard || previousOpening != m_nextTopicOpening || !authenticated()) return;
    if (!isUuid(target)) {
        report(tr("Enter a valid topic identifier."));
        return;
    }
    cancelConversationRestore();
    const auto known = std::find_if(m_topics.cbegin(), m_topics.cend(), [&](const Topic &topic) {
        return topic.uuid == target && topic.stream == m_stream;
    });
    if (known != m_topics.cend()) {
        const auto topic = known->uuid; // Keep it alive across route publication signals.
        selectTopic(topic); // The catalog owns the route; never the old hint.
        return;
    }
    beginTopicOpening(target);
}

void NativeController::beginTopicOpening(const QString &requested, std::optional<Topic> canonical)
{
    const QPointer<NativeController> guard(this);
    const auto previousOpening = m_nextTopicOpening;
    interruptTopicOpening();
    if (!guard || previousOpening != m_nextTopicOpening || !authenticated()) return;
    TopicOpening intent;
    intent.active = true;
    intent.requested = requested;
    intent.canonical = std::move(canonical);
    intent.owner = creationOwnerKey();
    intent.api = m_api;
    intent.generation = m_api->generation();
    intent.selection = m_selection;
    intent.catalog = m_catalog;
    intent.routeStream = m_stream;
    intent.routeTopic = m_topic;
    const auto *stream = currentStream();
    intent.routeSecurity = stream ? topicOpeningSecurity(*stream) : QString{};
    intent.cache = m_cache;
    intent.lease = m_cacheLease;
    intent.catalogVerified = m_catalogVerified;
    intent.snapshotActive = m_snapshotActive;
    intent.syncBlocked = m_syncBlocked;
    intent.forgetting = m_cacheForgettingAccount;
    const auto ticket = intent.ticket = ++m_nextTopicOpening;
    m_topicOpening = std::move(intent);
    m_error.clear();
    emit stateChanged();
    if (!guard || !topicOpeningContext(ticket)) {
        if (guard) finishTopicOpening(ticket);
        return;
    }
    if (m_topicOpening.canonical) {
        resolveTopicOpeningStream(ticket);
        return;
    }
    m_topicOpening.request = request("GET", messenger + "/stream_topics/" + requested, {}, {},
        [this, ticket](const ApiResult &result) {
        if (!topicOpeningContext(ticket)) { finishTopicOpening(ticket); return; }
        m_topicOpening.request = {};
        const auto raw = result.data.toObject();
        const auto stream = raw.value("stream_uuid").toString();
        const auto topic = result.ok() && isUuid(stream) ? creationTopicDto(raw, m_session->project(),
            stream, m_session->user()) : std::optional<Topic>{};
        if (!topic) {
            finishTopicOpening(ticket, result.ok() ? tr("The server returned invalid topic metadata.") : result.error);
            return;
        }
        // The owned DTO is canonical; requested UUID and stream hint need not match.
        m_topicOpening.canonical = *topic;
        resolveTopicOpeningStream(ticket);
    });
}

void NativeController::resolveTopicOpeningStream(quint64 ticket)
{
    if (!topicOpeningContext(ticket) || !m_topicOpening.canonical) { finishTopicOpening(ticket); return; }
    const auto id = m_topicOpening.canonical->stream;
    const auto known = std::find_if(m_streams.cbegin(), m_streams.cend(), [&](const Stream &stream) { return stream.uuid == id; });
    if (known != m_streams.cend()) {
        m_topicOpening.targetSecurity = topicOpeningSecurity(*known);
        publishTopicOpening(ticket);
        return;
    }
    m_topicOpening.request = request("GET", messenger + "/streams/" + id, {}, {},
        [this, ticket, id](const ApiResult &result) {
        if (!topicOpeningContext(ticket)) { finishTopicOpening(ticket); return; }
        m_topicOpening.request = {};
        const auto stream = result.ok() ? creationStreamDto(result.data.toObject(), m_session->project(),
            m_session->user(), false) : std::optional<Stream>{};
        if (!stream || stream->uuid != id) {
            finishTopicOpening(ticket, result.ok() ? tr("The server returned invalid channel metadata.") : result.error);
            return;
        }
        const auto known = std::find_if(m_streams.cbegin(), m_streams.cend(), [&](const Stream &row) { return row.uuid == id; });
        const bool added = known == m_streams.cend();
        if (!added && topicOpeningSecurity(*known) != topicOpeningSecurity(*stream)) {
            finishTopicOpening(ticket);
            return;
        }
        // A read-counter event may have admitted the same owned stream while
        // this detail GET was pending. Keep its newer counters and one row.
        if (added) m_streams.append(*stream);
        m_topicOpening.targetSecurity = topicOpeningSecurity(added ? *stream : *known);
        // A detail row is not a complete/global fresh catalog projection.
        staleCache(NativeCache::Kind::Streams);
        const QPointer<NativeController> guard(this);
        if (added) emit streamsChanged();
        if (!guard) return;
        if (!topicOpeningContext(ticket)) { finishTopicOpening(ticket); return; }
        publishTopicOpening(ticket);
    });
}

void NativeController::publishTopicOpening(quint64 ticket)
{
    if (!topicOpeningContext(ticket) || !m_topicOpening.canonical) { finishTopicOpening(ticket); return; }
    const auto topic = *m_topicOpening.canonical;
    const QPointer<NativeController> guard(this);
    activateStream(topic.stream, nullptr, {}, &topic, ticket);
    if (!guard) return;
    if (m_topicOpening.active && m_topicOpening.ticket == ticket) finishTopicOpening(ticket);
}

void NativeController::noteTopicOpeningStream(const Stream &stream)
{
    if (!m_topicOpening.active || (stream.uuid != m_topicOpening.routeStream
        && (!m_topicOpening.canonical || stream.uuid != m_topicOpening.canonical->stream))) return;
    const auto previous = std::find_if(m_streams.cbegin(), m_streams.cend(), [&](const Stream &row) { return row.uuid == stream.uuid; });
    if (previous == m_streams.cend() || topicOpeningSecurity(*previous) != topicOpeningSecurity(stream))
        interruptTopicOpening();
}

void NativeController::requestTopicMetadata(const QString &topicUuid)
{
    if (topicUuid.isEmpty() || m_topicMetadataInFlight.contains(topicUuid)
        || m_topicMetadataFailed.contains(topicUuid))
        return;
    // Bound the backfill burst a topic flood can produce; the next grace tick
    // retries anything that did not get a slot.
    if (m_topicMetadataInFlight.size() >= 8)
        return;
    m_topicMetadataInFlight.insert(topicUuid);
    const auto owner = creationOwnerKey();
    request("GET", messenger + "/stream_topics/" + topicUuid, QJsonValue{}, QUrlQuery{},
        [this, topicUuid, owner](const ApiResult &result) {
            m_topicMetadataInFlight.remove(topicUuid);
            if (!authenticated() || creationOwnerKey() != owner)
                return;
            const auto json = result.error.isEmpty() ? result.data.toObject() : QJsonObject{};
            const auto topic = json.isEmpty()
                ? std::optional<Topic>{}
                : parseTopic(json, m_session->project(), json.value("stream_uuid").toString());
            if (!topic) {
                if (m_topicMetadataFailed.size() < 512)
                    m_topicMetadataFailed.insert(topicUuid);
                return;
            }
            // Stored in the notification backfill, never in m_topics: a
            // background topic must not reshape the active UI catalog.
            if (std::none_of(m_topics.cbegin(), m_topics.cend(),
                    [&](const Topic &row) { return row.uuid == topic->uuid; })) {
                // Cache semantics with a session bound: long-lived sessions
                // must not accumulate one entry per ever-seen background topic.
                if (m_topicNotificationBackfill.size() >= 256)
                    m_topicNotificationBackfill.erase(m_topicNotificationBackfill.begin());
                m_topicNotificationBackfill.insert(topic->uuid, *topic);
            }
        });
}

void NativeController::selectTopic(const QString &id)
{
    if (!m_selectingDraftRoute) m_draftOpenId.clear();
    m_draftsVisible = false; if (m_draftStore) m_draftStore->hidePage();
    closeMentions();
    closeInbox();
    if (!id.isEmpty() && std::none_of(m_topics.begin(), m_topics.end(),
            [&id](const Topic &topic) { return topic.uuid == id; }))
        return;
    cancelConversationRestore();
    const QPointer<NativeController> guard(this);
    const auto previousOpening = m_nextTopicOpening;
    interruptTopicOpening();
    if (!guard || previousOpening != m_nextTopicOpening) return;
    interruptChannelCreations();
    interruptDirectChats();
    cancelSnapshot();
    if (!guard || previousOpening != m_nextTopicOpening) return;
    interruptChatRequestLookups();
    if (!guard) return;
    resetReadState();
    m_topicChanging = false;
    ++m_topicMutation;
    ++m_selection;
    m_topic = id;
    rememberConversation();
    settleEditRouteChange();
    m_historyVerified = false;
    m_messages.clear();
    pruneMessageFields();
    m_before.clear();
    m_after.clear();
    m_loading = false;
    m_recoveringMarker = m_resumeOlder = false;
    const auto selection = m_selection;
    emit messagesChanged();
    if (!guard || selection != m_selection) return;
    emit stateChanged();
    if (!guard || selection != m_selection) return;
    emit draftChanged();
    if (guard && selection == m_selection && m_topicSeedUnverified) loadTopics();
    if (guard && selection == m_selection && hasConversation()) loadMessages();
}

void NativeController::loadOlder()
{
    if (m_loading || (m_cacheProtection && !m_historyVerified))
        return;
    m_recoveringMarker = m_resumeOlder = false;
    loadMessages(true);
}

void NativeController::loadMessages(bool older)
{
    if (!authenticated() || !hasConversation() || unresolvedTopicRoute()
        || m_loading || (older && m_before.isEmpty())) return;
    const NativeCache::Key key{NativeCache::Kind::History, m_stream, m_topic, older ? m_before : QByteArray{}};
    const auto selection = m_selection;
    const auto generation = m_api->generation();
    const auto revision = m_messageRevision;
    const auto cacheRevision = m_cacheRevision;
    const auto lease = m_cacheLease;
    const QPointer<NativeCacheService> cache = m_cache;
    const QPointer<NativeController> guard(this);
    const auto networkStarted = std::make_shared<bool>(false);
    m_loading = true;
    emit stateChanged();
    if (!guard) return;
    const auto network = [this, guard, older, selection, generation, revision, key, networkStarted] {
        if (!guard || generation != m_api->generation() || selection != m_selection || *networkStarted) return;
        *networkStarted = true;
        const auto boundary = m_messages.isEmpty() ? std::optional<Message>{} : std::optional<Message>{m_messages.first()};
        historyPage(older, selection, revision, boundary, older ? key.marker : QByteArray{}, {}, {});
    };
    const auto fetchCache = [this, guard, cache, lease, key, selection, generation, revision, cacheRevision, older, network](bool savedOnly) {
        if (!guard || !cache || cache != m_cache || selection != m_selection || generation != m_api->generation()) return;
        cache->get(lease, key, this, [this, guard, cache, lease, selection, generation, revision, cacheRevision, older, savedOnly, network](std::optional<NativeCache::Entry> entry) {
            if (!guard || selection != m_selection || cache != m_cache || generation != m_api->generation()) return;
            const bool current = cacheRevision == m_cacheRevision && lease.owner == m_cacheLease.owner
                && lease.generation == m_cacheLease.generation && revision == m_messageRevision;
            if (!current) {
                if (!savedOnly) { m_loading = false; loadMessages(); }
                return;
            }
            if (savedOnly && (m_historyVerified || !m_messages.isEmpty())) return;
            if (entry) {
                QVector<Message> rows;
                bool valid = true;
                for (const auto &value : entry->rows) {
                    const auto message = parseMessage(value.toObject(), m_session->project());
                    if (!message || !inConversation(*message)) { valid = false; break; }
                    rows.append(*message);
                }
                const bool fresh = valid && entry->fresh && !savedOnly && !m_snapshotActive && !m_syncBlocked
                    && m_catalogVerified && m_topicsVerified && (older || m_messages.isEmpty());
                if (fresh) {
                    for (const auto &raw : entry->rows) observeMessage(raw.toObject());
                    rememberChatOutcomeOrder(rows);
                    m_messages = mergeMessages(older ? m_messages : QVector<Message>{}, rows);
                    pruneMessageFields();
                    m_before = entry->nextMarker;
                    if (!older) m_after.clear();
                    m_historyVerified = true;
                    m_loading = false;
                    ++m_historyCommitRevision;
                    emit messagesChanged();
                    if (guard) emit topicsChanged();
                    if (guard) announceStreamRowPreviews(rows);
                    if (guard) emit stateChanged();
                    return;
                }
                if (valid && !older && m_messages.isEmpty()) {
                    for (const auto &raw : entry->rows) observeMessage(raw.toObject());
                    m_cacheHydrated = true;
                    rememberChatOutcomeOrder(rows);
                    m_messages = mergeMessages({}, rows);
                    pruneMessageFields();
                    m_before.clear();
                    m_after.clear();
                    ++m_historyCommitRevision;
                    emit messagesChanged();
                    if (!guard) return;
                    emit stateChanged();
                    if (!guard) return;
                    emit topicsChanged();
                    if (!guard) return;
                    announceStreamRowPreviews(rows);
                }
            }
            if (!savedOnly) network();
        });
    };
    if (!cache || !cache->available()) {
        if (cache && !older) m_cacheHydrations[static_cast<int>(NativeCache::Kind::History)] = [fetchCache] { fetchCache(true); };
        network();
        return;
    }
    fetchCache(false);
}

void NativeController::historyPage(bool older, quint64 selection, quint64 revision,
                                  std::optional<Message> boundary, QByteArray marker,
                                  QVector<Message> accumulated, QSet<QByteArray> seen, int pages, qint64 bytes)
{
    const NativeCache::Key key{NativeCache::Kind::History, m_stream, m_topic, marker};
    const auto lease = m_cacheLease;
    const auto cacheRevision = m_cacheRevision;
    const QPointer<NativeCacheService> cache = m_cache;
    QUrlQuery query;
    query.addQueryItem("stream_uuid", m_stream);
    if (!m_topic.isEmpty()) query.addQueryItem("topic_uuid", m_topic);
    query.addQueryItem("sort_key", "created_at");
    query.addQueryItem("sort_dir", "desc");
    query.addQueryItem("page_limit", "50");
    if (!marker.isEmpty())
        query.addQueryItem("page_marker", QString::fromUtf8(marker));
    request("GET", messenger + "/messages/", {}, query,
        [this, older, selection, revision, boundary, marker, accumulated, seen, pages, bytes, key, lease, cacheRevision, cache](const ApiResult &result) mutable {
        if (selection != m_selection)
            return;
        if (revision != m_messageRevision) {
            // A confirmed local mutation is newer than this snapshot. Keep the
            // verified rows while rebuilding from a fresh server transaction.
            m_loading = false;
            m_recoveringMarker = m_resumeOlder = false;
            loadMessages();
            return;
        }
        if (!result.ok() || !result.data.isArray()) {
            m_loading = false;
            if (result.status == 404 && !marker.isEmpty() && !m_recoveringMarker) {
                // UUID cursors disappear when their messages are deleted. One
                // fresh traversal obtains a valid boundary, then resumes Older.
                m_recoveringMarker = true;
                m_resumeOlder = older;
                loadMessages();
                return;
            }
            m_recoveringMarker = m_resumeOlder = false;
            report(tr("Could not load messages. %1").arg(result.error));
            return;
        }
        const auto page = result.data.toArray();
        bytes += QJsonDocument(page).toJson(QJsonDocument::Compact).size();
        if (++pages > 500 || accumulated.size() + page.size() > 100000 || bytes > 32 * 1024 * 1024) {
            m_loading = false;
            report(tr("The server returned an invalid history cursor."));
            return;
        }
        QSet<QString> ids;
        for (const auto &row : accumulated) ids.insert(row.uuid);
        bool reachedBoundary = false;
        for (const auto &value : page) {
            const auto message = parseMessage(value.toObject(), m_session->project());
            if (!message || !inConversation(*message) || ids.contains(message->uuid)) {
                m_loading = false;
                report(tr("The server returned an invalid conversation history."));
                return;
            }
            if (boundary && (message->createdMicros < boundary->createdMicros
                || (message->createdMicros == boundary->createdMicros && message->uuid <= boundary->uuid)))
                reachedBoundary = true;
            ids.insert(message->uuid);
            accumulated.append(*message);
            if (findMessage(message->uuid) || m_messageFields.contains(message->uuid)) observeMessage(value.toObject());
        }
        if (cache && m_cache == cache && cacheRevision == m_cacheRevision)
            cache->put(lease, key, result.data.toArray(), result.nextMarker);
        // Reconcile the whole already loaded window, including edits and
        // deletions in older pages, before publishing one atomic snapshot.
        if (!older && boundary && !reachedBoundary && !result.nextMarker.isEmpty()) {
            if (seen.contains(result.nextMarker) || pages >= 500 || accumulated.size() > 100000) {
                m_loading = false;
                report(tr("The server returned an invalid history cursor."));
                return;
            }
            seen.insert(result.nextMarker);
            historyPage(false, selection, revision, boundary, result.nextMarker, accumulated, seen, pages, bytes);
            return;
        }
        rememberChatOutcomeOrder(accumulated);
        if (older) {
            m_messages = mergeMessages(m_messages, accumulated);
        } else {
            m_messages = mergeMessages({}, accumulated);
        }
        pruneMessageFields();
        m_before = result.nextMarker;
        if (!older) m_after.clear();
        m_loading = false;
        m_historyVerified = true;
        const bool resumeOlder = m_resumeOlder && !m_before.isEmpty();
        m_resumeOlder = false;
        if (!resumeOlder)
            m_recoveringMarker = false;
        const QPointer<NativeController> guard(this);
        ++m_historyCommitRevision;
        emit messagesChanged();
        if (!guard) return;
        emit topicsChanged();
        if (!guard) return;
        announceStreamRowPreviews(accumulated);
        if (!guard) return;
        finishSnapshotPart(8);
        if (!guard) return;
        emit stateChanged();
        if (!guard) return;
        if (resumeOlder)
            loadMessages(true);
    });
}

void NativeController::send()
{
    if (!m_editingMessage.isEmpty()) return;
    if (currentStream() && currentStream()->encrypted) { sendEncryptedText(); return; }
    if (!canSend())
        return;
    const auto text = draft();
    // Reply composition contract (workspace_ui 61dacef workspace-reply.lib.ts:190-212):
    // "[Name](urn:quote:uuid)" + blank line + answer, trailing whitespace trimmed.
    const auto replyId = m_replyMessage;
    auto wire = text;
    if (!replyId.isEmpty()) {
        const auto quoteLine = workspace::buildWorkspaceQuoteReference(replyAuthor(), replyId);
        if (quoteLine.isEmpty()) {
            report(tr("This message can no longer be replied to."));
            return;
        }
        wire = (quoteLine + QStringLiteral("\n\n") + text).trimmed();
    }
    const auto conversation = conversationKey();
    auto intent = m_pendingSends.value(conversation);
    if (intent.uuid.isEmpty() || intent.text != text || intent.wire != wire) {
        intent = SendIntent{};
        intent.uuid = QUuid::createUuid().toString(QUuid::WithoutBraces); intent.text = text;
        intent.wire = wire;
        intent.draftRevision = m_draftRevisions.value(conversation);
        if (durableDraftRoute()) {
            intent.draftCaptured = true;
            intent.draftToken = m_draftStore->capture(m_stream, m_topic);
        }
        m_pendingSends.insert(conversation, intent);
    }
    ++m_messageFields[intent.uuid].pending;
    const auto context = projectionContext();
    const auto stream = m_stream;
    const auto topic = m_topic;
    staleConversation(stream, topic);
    m_sending = true;
    emit stateChanged();
    const QJsonObject body{{"uuid", intent.uuid}, {"stream_uuid", stream}, {"topic_uuid", topic},
        {"payload", QJsonObject{{"kind", "markdown"}, {"content", intent.wire}}}};
    request("POST", messenger + "/messages/", body, {},
        [this, stream, topic, text, conversation, intent, context, replyId](const ApiResult &result) {
        staleConversation(stream, topic);
        const auto sent = parseMessage(result.data.toObject(), m_session->project());
        if (!result.ok() || !sent || sent->stream != stream || sent->topic != topic
            || sent->kind != "markdown" || sent->content != intent.wire) {
            finishMessageIntent(intent.uuid);
            m_sending = false;
            report(tr("Sending was not confirmed. Your text is preserved. %1").arg(result.error));
            return;
        }
        const auto canonicalId = sent->uuid;
        if (canonicalId != intent.uuid) {
            ++m_messageFields[canonicalId].pending;
            finishMessageIntent(intent.uuid);
        }
        request("GET", messenger + "/messages/" + canonicalId, {}, {},
            [this, stream, topic, text, conversation, intent, context, canonicalId, replyId](const ApiResult &confirmation) {
            staleConversation(stream, topic);
            m_sending = false;
            const bool deleted = m_messageFields.value(canonicalId).deleted;
            const auto saved = parseMessage(confirmation.data.toObject(), m_session->project());
            if (!confirmation.ok() || !saved || saved->uuid != canonicalId || saved->stream != stream || saved->topic != topic
                || saved->kind != "markdown" || saved->content != intent.wire || !saved->own) {
                finishMessageIntent(canonicalId);
                report(tr("Message verification failed. Your text is preserved."));
                return;
            }
            if (deleted) { finishMessageIntent(canonicalId); emit stateChanged(); return; }
            projectCanonical(confirmation.data.toObject(), NativeCache::Projection::Insert, context);
            if (m_pendingSends.value(conversation).uuid == intent.uuid)
                m_pendingSends.remove(conversation);
            const QPointer<NativeController> draftGuard(this);
            if (intent.draftCaptured) {
                if (m_draftStore) m_draftStore->consume(intent.draftToken);
            } else if (m_drafts.value(conversation) == text
                       && m_draftRevisions.value(conversation) == intent.draftRevision) m_drafts.remove(conversation);
            if (!draftGuard) return;
            m_attachmentNotices.remove(conversation);
            if (conversation == conversationKey()) {
                ++m_messageRevision;
                if (!findMessage(saved->uuid)) {
                    observeMessage(confirmation.data.toObject());
                    m_messages = mergeMessages(m_messages, {*saved});
                }
            }
            finishMessageIntent(canonicalId);
            m_error.clear();
            if (!replyId.isEmpty() && m_replyMessage == replyId) closeReplySession();
            const QPointer<NativeController> guard(this);
            emit draftChanged();
            if (!guard) return;
            emit messagesChanged();
            if (!guard) return;
            emit stateChanged();
            if (guard && conversation == conversationKey())
                emit messageSent(saved->uuid, stream, topic);
        });
    });
}

void NativeController::copyMessage(const QString &id)
{
    const auto *message = findMessage(id);
    if (!message) return;
    if (message->kind == "markdown") QGuiApplication::clipboard()->setText(message->content);
    else if (message->kind == "e2ee" && chatReadAllowed()) {
        const auto prepared = m_chatPrepared.constFind(id);
        if (prepared != m_chatPrepared.cend() && prepared->state == ChatKeyAvailability::Present
            && prepared->hash == chatPayloadHash(*message)) {
            NativeMarkdown renderer;
            QGuiApplication::clipboard()->setText(renderer.render(prepared->content).value("plain").toString());
        }
    }
}

QString NativeController::messageLink(const QString &id) const
{
    if (!authenticated()) return {};
    const auto *message = findMessage(id);
    if (!message) return {};
    return workspace::messageUrl(m_api->server(), message->project, message->uuid);
}

void NativeController::copyMessageLink(const QString &id)
{
    const auto link = messageLink(id);
    if (!link.isEmpty()) QGuiApplication::clipboard()->setText(link);
}

void NativeController::openMessageLink(const QString &url)
{
    if (!authenticated() || busy()) {
        m_pendingDeepLink = url;
        return;
    }
    applyDeepLink(url);
}

void NativeController::applyDeepLink(const QString &url)
{
    const auto route = workspace::parseExternalDeepLink(url);
    if (!route.has_value()) return;
    // The reference navigates its inbox page for /inbox (inbox-page.ui.tsx
    // route, docs/NATIVE_DESKTOP_INBOX.md); the org/project guard matches
    // the one applyMessageRoute applies to message anchors.
    static const QRegularExpression inboxRoutePattern(
        "^/org/([^/]+)/project/([^/]+)/inbox/?$");
    if (const auto match = inboxRoutePattern.match(*route); match.hasMatch()) {
        if (match.captured(1).compare(workspace::orgRouteIdFromOrigin(m_api->server()), Qt::CaseInsensitive) == 0
            && match.captured(2).compare(m_session->project(), Qt::CaseInsensitive) == 0)
            showInbox();
        return;
    }
    const auto message = workspace::parseMessageRoute(*route);
    if (!message.has_value()) return; // Other whitelisted routes only activate the window.
    applyMessageRoute(message->org, message->project, message->message);
}

void NativeController::flushPendingDeepLink(bool settle)
{
    // Wait until the session and the default conversation restore have
    // settled; otherwise a selection storm would trample the anchor.
    if (m_pendingDeepLink.isEmpty() || !authenticated() || busy()
        || m_session->project().isEmpty() || streams().isEmpty() || m_stream.isEmpty()
        || m_topic.isEmpty() || !m_conversationRestore.isEmpty())
        return;
    if (settle && !m_deepLinkFlushScheduled) {
        // Settle first: cold-start arguments arrive long before the resume
        // flow finishes its own navigation bursts.
        m_deepLinkFlushScheduled = true;
        QTimer::singleShot(750, this, [this] {
            m_deepLinkFlushScheduled = false;
            flushPendingDeepLink(false);
        });
        return;
    }
    const auto url = m_pendingDeepLink;
    m_pendingDeepLink.clear();
    applyDeepLink(url);
}

void NativeController::applyMessageRoute(const QString &org, const QString &project,
                                         const QString &target)
{
    // The current preview binds one server origin and one project token; a link
    // for another organization or project lands safely without navigation.
    if (org.compare(workspace::orgRouteIdFromOrigin(m_api->server()), Qt::CaseInsensitive) != 0
        || project.compare(m_session->project(), Qt::CaseInsensitive) != 0) {
        clearAnchor(deepLinkUnavailableNotice());
        return;
    }
    m_pendingAnchorMessage = target;
    m_pendingAnchorStream = m_stream;
    m_pendingAnchorTopic = m_topic;
    m_anchorBudget = 8;
    m_anchorAroundTried = false;
    if (findMessage(target)) {
        reanchorFromWindow();
        return;
    }
    request("GET", messenger + "/messages/", {}, {{"project_id", project}, {"uuid", target}},
        [this, target](const ApiResult &result) {
        if (m_pendingAnchorMessage != target) return;
        const QJsonArray rows = result.ok() && result.data.isArray() ? result.data.toArray() : QJsonArray{};
        const auto parsed = rows.isEmpty() ? std::optional<Message>{}
            : parseMessage(rows.first().toObject(), m_session->project());
        if (!parsed || parsed->uuid != target) {
            clearAnchor(deepLinkUnavailableNotice());
            return;
        }
        if (m_pendingAnchorMessage != target) return;
        m_pendingAnchorParsed = parsed;
        m_pendingAnchorStream = parsed->stream;
        m_pendingAnchorTopic = parsed->topic;
        if (parsed->topic.isEmpty()) {
            if (parsed->stream == m_stream && m_topic.isEmpty()) reanchorFromWindow();
            else selectStream(parsed->stream);
        } else if (parsed->stream == m_stream && parsed->topic == m_topic) {
            reanchorFromWindow();
        } else {
            openTopic(parsed->topic);
        }
    });
}

void NativeController::setAnchorMessage(const QString &id)
{
    if (m_anchorMessage == id) return;
    m_anchorMessage = id;
    emit anchorChanged();
}

void NativeController::clearAnchor(const QString &notice)
{
    m_pendingAnchorMessage.clear();
    m_pendingAnchorStream.clear();
    m_pendingAnchorTopic.clear();
    m_pendingAnchorParsed.reset();
    m_anchorBudget = 0;
    m_anchorHighlight.stop();
    const bool hadAnchor = !m_anchorMessage.isEmpty();
    m_anchorMessage.clear();
    if (hadAnchor) emit anchorChanged();
    if (!notice.isEmpty()) {
        m_notice = notice;
        emit stateChanged();
    }
}

bool NativeController::topicOpeningOwnsPendingAnchor() const
{
    // True while the in-flight topic-opening operation is the one that was
    // started for the pending anchor's destination (issue #55). Its
    // intermediate pre-commit stateChanged emissions are owned transitions,
    // not the user navigating away, so they must not cancel the anchor.
    if (m_pendingAnchorTopic.isEmpty() || !m_topicOpening.active) return false;
    if (!topicOpeningContext(m_topicOpening.ticket)) return false;
    const auto destination = m_pendingAnchorTopic.toLower();
    return m_topicOpening.requested.toLower() == destination
        || (m_topicOpening.canonical && m_topicOpening.canonical->uuid.toLower() == destination);
}

void NativeController::reanchorFromWindow()
{
    if (m_pendingAnchorMessage.isEmpty()) return;
    if (m_stream != m_pendingAnchorStream || m_topic != m_pendingAnchorTopic) {
        // An owned deep-link transition to the anchor's own destination is in
        // progress: keep the anchor and let the committed route reanchor it.
        if (topicOpeningOwnsPendingAnchor()) return;
        clearAnchor(); // Navigation moved on; the anchor no longer has a home.
        return;
    }
    if (findMessage(m_pendingAnchorMessage)) {
        const auto target = m_pendingAnchorMessage;
        m_pendingAnchorMessage.clear();
        m_pendingAnchorStream.clear();
        m_pendingAnchorTopic.clear();
        m_anchorBudget = 0;
        m_pendingAnchorParsed.reset();
        setAnchorMessage(target);
        m_anchorHighlight.start(2500);
        return;
    }
    if (busy()) return; // A live page owns the next reanchor pass.
    // Reference parity (issue #71): a resolved anchor outside the loaded window opens
    // the around-anchor pages (workspace_ui 61dacef7 resolveOpeningMessageWindow in
    // entities/messenger/messenger-messages-loader.lib.ts) before the native bounded
    // walk, which stays the fallback when either around page fails.
    if (!m_anchorAroundTried && m_historyVerified && !m_loading && !m_snapshotActive) {
        m_anchorAroundTried = true;
        loadAroundAnchor();
        return;
    }
    if (m_anchorBudget > 0) {
        --m_anchorBudget;
        if (canLoadOlder()) {
            loadOlder();
            return;
        }
        // Topic switching commits its selection before the first history
        // page lands; give the in-flight page a short bounded grace.
        QTimer::singleShot(120, this, [this] { reanchorFromWindow(); });
        return;
    }
    clearAnchor(deepLinkUnavailableNotice());
}


void NativeController::loadAroundAnchor()
{
    // Reference contract: getMessagePagesAroundResolvedMessage (workspace_ui 61dacef7
    // shared/api/messenger-messages.api.ts, contract test :169-246) - two parallel
    // pages at page_marker=<anchor uuid>, sort_dir=desc for the older side and
    // sort_dir=asc for the newer side, page_limit 50 (DEFAULT_MESSAGES_PAGE_LIMIT).
    const QString target = m_pendingAnchorMessage;
    if (target.isEmpty() || m_loading) return;
    struct AroundPages {
        int done = 0;
        bool failed = false;
        QJsonArray before;
        QJsonArray after;
        QByteArray beforeMarker;
        QByteArray afterMarker;
    };
    const auto state = std::make_shared<AroundPages>();
    const auto selection = m_selection;
    const auto generation = m_api->generation();
    const auto revision = m_messageRevision;
    const QPointer<NativeController> guard(this);
    m_loading = true;
    emit stateChanged();
    if (!guard) return;
    const auto settle = [this, guard, state, selection, generation, revision, target] {
        if (!guard) return;
        if (selection != m_selection || generation != m_api->generation()
            || revision != m_messageRevision || m_pendingAnchorMessage != target) {
            m_loading = false;
            emit stateChanged();
            return;
        }
        if (state->failed) { // the bounded older walk remains the fallback (#55)
            m_loading = false;
            emit stateChanged();
            reanchorFromWindow();
            return;
        }
        QVector<Message> rows;
        bool invalid = false;
        const auto absorb = [this, &rows, &invalid, &target](const QJsonArray &page) {
            for (const auto &value : page) {
                const auto parsed = parseMessage(value.toObject(), m_session->project());
                // A page marker is only valid when every row on its page was accepted,
                // so a malformed row rejects the page (the older walk is the fallback).
                if (parsed && parsed->uuid == target) continue;
                if (!parsed || !inConversation(*parsed)) { invalid = true; return; }
                rows.append(*parsed);
                if (findMessage(parsed->uuid) || m_messageFields.contains(parsed->uuid))
                    observeMessage(value.toObject());
            }
        };
        absorb(state->before);
        if (!invalid) absorb(state->after);
        if (!invalid && m_pendingAnchorParsed && m_pendingAnchorParsed->uuid == target
            && inConversation(*m_pendingAnchorParsed))
            rows.append(*m_pendingAnchorParsed);
        if (invalid) {
            m_loading = false;
            report(tr("The server returned an invalid conversation history."));
            reanchorFromWindow();
            return;
        }
        m_messages = mergeMessages({}, rows);
        pruneMessageFields();
        m_before = state->beforeMarker;
        m_after = state->afterMarker;
        m_loading = false;
        m_historyVerified = true;
        ++m_historyCommitRevision;
        emit messagesChanged();
        if (!guard) return;
        emit topicsChanged();
        if (!guard) return;
        announceStreamRowPreviews(rows);
        if (!guard) return;
        emit stateChanged();
        if (!guard) return;
        reanchorFromWindow();
    };
    const auto page = [this, guard, state, settle, selection, generation](const char *dir) {
        if (!guard || selection != m_selection || generation != m_api->generation()) return;
        const bool newer = dir[0] == 'a';
        QUrlQuery query;
        query.addQueryItem("stream_uuid", m_stream);
        if (!m_topic.isEmpty()) query.addQueryItem("topic_uuid", m_topic);
        query.addQueryItem("sort_key", "created_at");
        query.addQueryItem("sort_dir", dir);
        query.addQueryItem("page_limit", "50");
        query.addQueryItem("page_marker", m_pendingAnchorMessage);
        request("GET", messenger + "/messages/", {}, query,
            [this, guard, state, settle, selection, generation, newer](const ApiResult &result) {
            if (!guard || selection != m_selection || generation != m_api->generation()) return;
            if (!result.ok() || !result.data.isArray()) state->failed = true;
            else if (newer) { state->after = result.data.toArray(); state->afterMarker = result.nextMarker; }
            else { state->before = result.data.toArray(); state->beforeMarker = result.nextMarker; }
            if (++state->done == 2) settle();
        });
    };
    page("desc");
    page("asc");
}

void NativeController::loadNewer()
{
    if (m_loading || m_after.isEmpty() || (m_cacheProtection && !m_historyVerified))
        return;
    loadNewerPage(m_after);
}

void NativeController::loadNewerPage(const QByteArray &marker)
{
    if (!authenticated() || !hasConversation() || unresolvedTopicRoute()
        || m_loading || marker.isEmpty()) return;
    const auto selection = m_selection;
    const auto generation = m_api->generation();
    const auto revision = m_messageRevision;
    const QPointer<NativeController> guard(this);
    m_loading = true;
    emit stateChanged();
    if (!guard) return;
    QUrlQuery query;
    query.addQueryItem("stream_uuid", m_stream);
    if (!m_topic.isEmpty()) query.addQueryItem("topic_uuid", m_topic);
    query.addQueryItem("sort_key", "created_at");
    query.addQueryItem("sort_dir", "asc");
    query.addQueryItem("page_limit", "50");
    query.addQueryItem("page_marker", QString::fromUtf8(marker));
    request("GET", messenger + "/messages/", {}, query,
        [this, guard, selection, generation, revision](const ApiResult &result) {
        if (!guard || selection != m_selection || generation != m_api->generation()) return;
        if (revision != m_messageRevision) {
            m_loading = false;
            loadMessages();
            return;
        }
        if (!result.ok() || !result.data.isArray()) {
            m_loading = false;
            report(tr("Could not load messages. %1").arg(result.error));
            return;
        }
        QVector<Message> rows;
        for (const auto &value : result.data.toArray()) {
            const auto parsed = parseMessage(value.toObject(), m_session->project());
            if (!parsed || !inConversation(*parsed)) {
                m_loading = false;
                report(tr("The server returned an invalid conversation history."));
                return;
            }
            rows.append(*parsed);
            if (findMessage(parsed->uuid) || m_messageFields.contains(parsed->uuid))
                observeMessage(value.toObject());
        }
        m_messages = mergeMessages(m_messages, rows);
        pruneMessageFields();
        m_after = result.nextMarker;
        m_loading = false;
        ++m_historyCommitRevision;
        emit messagesChanged();
        if (!guard) return;
        emit topicsChanged();
        if (!guard) return;
        announceStreamRowPreviews(rows);
        if (!guard) return;
        emit stateChanged();
    });
}

QString NativeController::deepLinkUnavailableNotice()

{
    return tr("The requested message is unavailable.");
}

QString NativeController::summarizeRowPreview(const QString &plainText)
{
    // workspace_ui 61dacef entities/messenger/messenger-topic-list.lib.ts:61-92 with
    // DEFAULT_WORKSPACE_MESSAGE_SUMMARY_OPTIONS: normalizePreviewText collapses every
    // whitespace run into one space and trims; truncatePreview keeps maxLength (120)
    // characters, trimming the tail, and appends the ASCII "..." marker.
    static constexpr int maximumCharacters = 120;
    static constexpr int markerLength = 3;
    const auto text = plainText.simplified();
    if (text.size() <= maximumCharacters) return text;
    return text.left(maximumCharacters - markerLength).trimmed() + QStringLiteral("...");
}

QString NativeController::formatRowTime(const QDateTime &created, const QLocale &locale,
    const QDate &today, const QString &yesterdayLabel)
{
    // workspace_ui 61dacef shared/lib/datetime.lib.ts:65-82 formatMessageTimeRelative:
    // the same local calendar day is a manual 24 h HH:mm (never Intl), the previous local
    // day is the chat.yesterday label with no clock time, anything older is the compact
    // day + short month style, which carries no year at all.
    if (!created.isValid()) return {};
    const auto local = created.toLocalTime();
    if (local.date() == today) return local.toString(QStringLiteral("HH:mm"));
    if (local.date() == today.addDays(-1)) return yesterdayLabel;
    return locale.toString(local.date(), locale.language() == QLocale::Russian
        ? QStringLiteral("d MMM") : QStringLiteral("MMM d"));
}

std::optional<NativeController::RowPreview> NativeController::rowPreviewOf(const Message &message) const
{
    // workspace_ui 61dacef messenger-topic-list.lib.ts:156-188 and
    // sidebar-workspace-message-preview.ui.tsx: the sender is common.you for own messages
    // and the resolved display name otherwise, omitted entirely for an unknown author.
    RowPreview preview;
    preview.created = message.created;
    // The sender label is resolved at projection time so a late-arriving display name or a
    // language change reaches the row; the cache only stores the author identity.
    preview.own = message.own;
    preview.author = message.author;
    QString body;
    if (message.kind == QLatin1String("markdown")) {
        body = message.content;
    } else {
        // A non-markdown payload previews as the decrypted body only when this very payload
        // has a key; otherwise the reference renders e2ee.lockedTitle and nothing else.
        const auto prepared = m_chatPrepared.constFind(message.uuid);
        if (prepared == m_chatPrepared.constEnd() || prepared->state != ChatKeyAvailability::Present
            || prepared->hash != chatPayloadHash(message)) {
            preview.locked = true;
            return preview;
        }
        body = prepared->content;
    }
    NativeMarkdown renderer;
    preview.text = summarizeRowPreview(renderer.render(body).value("plain").toString());
    return preview;
}

std::optional<NativeController::RowPreview> NativeController::rowPreview(const QString &uuid) const
{
    // Previews are best effort: a row whose last message the client does not hold keeps the
    // empty preview line, exactly like the reference for a message it has not hydrated.
    if (uuid.isEmpty()) return std::nullopt;
    for (const auto &message : m_messages)
        if (message.uuid == uuid) return rowPreviewOf(message);
    const auto cached = m_rowPreviews.constFind(uuid);
    if (cached != m_rowPreviews.constEnd()) return *cached;
    // The body may still exist on disk: record the miss and let the bounded pump resolve it
    // through the cache, then re-emit the affected rows.
    if (!m_rowPreviewPending.contains(uuid) && !m_rowPreviewInFlight.contains(uuid)) {
        m_rowPreviewPending.insert(uuid);
        scheduleRowPreviewHydration();
    }
    return std::nullopt;
}

qint64 NativeController::rowActivityMicros(const QString &lastMessage, qint64 updatedMicros) const
{
    if (!lastMessage.isEmpty())
        for (const auto &message : m_messages)
            if (message.uuid == lastMessage) return message.createdMicros;
    return updatedMicros;
}

int NativeController::streamGroupRank(const Stream &stream) const
{
    // workspace_ui 61dacef messenger-sidebar.lib.ts:291-299 with messenger-notification-mode
    // .lib.ts:22-29: archived is the last group and a stream is fully muted only when the
    // stream itself is muted and no topic explicitly un-muted it. Archived rows are already
    // filtered out of streams(), so only the muted rank is left to express here.
    if (stream.notificationMode != QLatin1String("muted")) return 0;
    for (const auto &topic : m_topics) {
        if (topic.stream != stream.uuid) continue;
        if (topic.notificationMode == QLatin1String("unmute")
            || topic.notificationMode == QLatin1String("follow")) return 0;
    }
    return 1;
}

QVariantMap NativeController::rowPreviewFields(const QString &lastMessage) const
{
    const auto preview = rowPreview(lastMessage);
    // An unresolved pointer (or a remembered negative lookup, whose created stays invalid)
    // keeps the reference empty preview line; a locked row is valid without any text.
    if (!preview || (!preview->locked && !preview->created.isValid()))
        return QVariantMap{{"previewSender", QString{}}, {"previewText", QString{}},
            {"previewLocked", false}, {"rowTime", QString{}}};
    const auto sender = preview->own ? tr("You") : userDisplayName(preview->author);
    const auto locale = m_preferences ? QLocale(m_preferences->locale()) : QLocale::system();
    return QVariantMap{{"previewSender", sender}, {"previewText", preview->text},
        {"previewLocked", preview->locked},
        {"rowTime", formatRowTime(preview->created, locale, QDate::currentDate(),
            tr("Yesterday"))}};
}

void NativeController::announceStreamRowPreviews(const QVector<Message> &rows)
{
    // A history commit can complete a row preview: the reference re-derives the sidebar row
    // from the messenger cache the moment its last message becomes known, and the native
    // activity comparator reads the same instants. Only streams actually pointed at one of
    // the new messages re-bind, so browsing a channel never reflows the whole list.
    if (rows.isEmpty()) return;
    QSet<QString> ids;
    for (const auto &row : rows) ids.insert(row.uuid);
    for (const auto &stream : std::as_const(m_streams)) {
        if (stream.archived || stream.lastMessage.isEmpty()) continue;
        if (ids.contains(stream.lastMessage)) { emit streamsChanged(); return; }
    }
}

void NativeController::storeRowPreview(const QString &uuid, const std::optional<RowPreview> &preview) const
{
    if (!preview) return;
    // The derived-text cache in the reference is bounded (512 rows, messenger-topic-list.lib.ts
    // previewTextCache); this evicts in insertion order at the same ceiling.
    if (!m_rowPreviews.contains(uuid)) m_rowPreviewOrder.enqueue(uuid);
    m_rowPreviews.insert(uuid, *preview);
    while (m_rowPreviews.size() > rowPreviewCacheLimit)
        m_rowPreviews.remove(m_rowPreviewOrder.dequeue());
}

quint64 NativeController::bumpRowPreviewFence(const QString &uuid)
{
    // Issue #116: every authoritative payload change invalidates previews derived from its
    // predecessor. The fence only needs to outlive an in-flight hydration read, so the map is
    // bounded by bulk reset instead of per-entry eviction: dropping old fences lets at most a
    // disk read through, and the disk row is written by the same canonical projections that
    // moved the payload, so it carries the new body.
    if (!m_rowPreviewFence.contains(uuid)
        && m_rowPreviewFence.size() >= rowPreviewCacheLimit * 4)
        m_rowPreviewFence.clear();
    return m_rowPreviewFence[uuid] = ++m_nextRowPreviewFence;
}

void NativeController::refreshRowPreviewFromPayload(const Message &message)
{
    // Issue #116: a confirmed or live payload change owns the memo the sidebar row may
    // already hold for this uuid: without this the memo keeps the pre-change body and shows
    // it again as soon as the message leaves m_messages (navigate away from the edited
    // conversation). Bumping the fence also discards hydration reads started before the
    // change, so an older disk body cannot land after this update and re-memoize itself.
    bumpRowPreviewFence(message.uuid);
    if (m_rowPreviews.contains(message.uuid))
        storeRowPreview(message.uuid, rowPreviewOf(message));
    announceRowPreviewRows(message.uuid);
}

void NativeController::markRowPreviewDeleted(const QString &uuid)
{
    // Issue #116: a deletion removes the payload behind the memo too. Re-derive from the
    // tombstone: a quiet (negative) preview, the same shape a cold-cache negative lookup
    // stores, while the fence drops in-flight reads of the now-deleted body.
    bumpRowPreviewFence(uuid);
    if (m_rowPreviews.contains(uuid))
        storeRowPreview(uuid, RowPreview{});
    announceRowPreviewRows(uuid);
}

void NativeController::announceRowPreviewRows(const QString &uuid)
{
    // Mirror of the hydration pump's notification rule: a row whose last-message pointer
    // references this uuid re-binds when its memo changes shape.
    for (const auto &stream : m_streams)
        if (stream.lastMessage == uuid) { emit streamsChanged(); emit topicsChanged(); return; }
    for (const auto &topic : m_topics)
        if (topic.lastMessage == uuid) { emit streamsChanged(); emit topicsChanged(); return; }
}

void NativeController::scheduleRowPreviewHydration() const
{
    if (m_rowPreviewPumpQueued) return;
    m_rowPreviewPumpQueued = true;
    QTimer::singleShot(0, this, [this] {
        m_rowPreviewPumpQueued = false;
        // The pump emits change signals and holds the QPointer receiver contract, so it runs
        // on the owning instance itself; the const projection may only have reached it
        // through the memoization bookkeeping above.
        const_cast<NativeController *>(this)->pumpRowPreviewHydration();
    });
}

void NativeController::pumpRowPreviewHydration()
{
    if (m_rowPreviewPending.isEmpty()) return;
    // Without an available cache there is nothing to read; the rows keep the reference empty
    // preview and the pending marks are released so a later cache attach can try again.
    if (!authenticated() || !m_cache || !m_cache->available()) { m_rowPreviewPending.clear(); return; }
    const auto owner = creationOwnerKey();
    const auto cache = m_cache;
    const auto lease = m_cacheLease;
    int started = 0;
    const auto pending = m_rowPreviewPending.values();
    for (const auto &uuid : pending) {
        if (started >= rowPreviewHydrationBatch) break;
        m_rowPreviewPending.remove(uuid);
        if (m_rowPreviews.contains(uuid) || m_rowPreviewInFlight.contains(uuid)) continue;
        m_rowPreviewInFlight.insert(uuid);
        ++started;
        const QPointer<NativeController> guard(this);
        // Issue #116: the read fence moves whenever the authoritative payload changes; a
        // body read that started before the move is older than the payload path's memo.
        const auto fence = m_rowPreviewFence.value(uuid);
        cache->getMessageBody(lease, uuid, this, [this, guard, uuid, owner, fence](std::optional<NativeCache::MessageBody> body) {
            if (!guard) return;
            m_rowPreviewInFlight.remove(uuid);
            // A body that answers after the session or its owner changed belongs to nobody.
            if (!authenticated() || creationOwnerKey() != owner) return;
            if (m_rowPreviewFence.value(uuid) != fence) {
                // Issue #116: the payload moved while this body was in flight, so the memo
                // already belongs to the newer payload (or the message is deleted). Storing
                // this stale body would resurrect the superseded preview; notify the rows
                // only, so they re-project from the fresh memo.
            } else if (const auto parsed = body ? parseMessage(body->row, m_session->project()) : std::nullopt;
                       parsed && parsed->uuid == uuid) {
                storeRowPreview(uuid, rowPreviewOf(*parsed));
            } else {
                storeRowPreview(uuid, RowPreview{}); // negative lookup: projections stay quiet
            }
            bool affectsRow = false;
            for (const auto &stream : m_streams)
                if (stream.lastMessage == uuid) { affectsRow = true; break; }
            if (!affectsRow)
                for (const auto &topic : m_topics)
                    if (topic.lastMessage == uuid) { affectsRow = true; break; }
            if (affectsRow) { emit streamsChanged(); emit topicsChanged(); }
        });
    }
    // A bounded pass leaves the remainder queued for the next tick, so every row of a large
    // sidebar still reaches its eventual preview without a burst of disk reads.
    if (!m_rowPreviewPending.isEmpty()) scheduleRowPreviewHydration();
}

bool NativeController::beginReply(const QString &id)
{
    const auto *stream = currentStream();
    const auto *message = findMessage(id);
    // Same conversation guard as editableMessage (docs/NATIVE_DESKTOP_REPLIES.md);
    // encrypted streams keep replies out of reach, as the shipped e2ee gate does.
    if (!authoritativeRoute() || !stream || stream->encrypted || !message
        || message->kind != "markdown"
        || message->stream != m_stream || message->topic != m_topic) {
        report(tr("This message can no longer be replied to."));
        return false;
    }
    if (!m_editingMessage.isEmpty()) closeEditSession();
    if (m_replyMessage != id) {
        m_replyMessage = id;
        m_replyRoute = conversationKey();
        emit replySessionChanged();
    }
    return true;
}

void NativeController::cancelReply()
{
    closeReplySession();
}

void NativeController::closeReplySession()
{
    if (m_replyMessage.isEmpty()) return;
    m_replyMessage.clear();
    m_replyRoute.clear();
    emit replySessionChanged();
}

QString NativeController::replyAuthor() const
{
    const auto *message = m_replyMessage.isEmpty() ? nullptr : findMessage(m_replyMessage);
    if (!message) return {};
    const bool own = message->own || message->author == m_session->user();
    return userDisplayName(message->author, own ? tr("You") : QStringLiteral("#") + message->author.left(8));
}

QString NativeController::replyExcerpt() const
{
    const auto *message = m_replyMessage.isEmpty() ? nullptr : findMessage(m_replyMessage);
    if (!message) return {};
    // QUOTE_PREVIEW_MAX = 80 (workspace_ui 61dacef message-composer-constants.lib.ts:3):
    // whitespace-collapsed summary truncated with the ASCII "..." marker.
    static constexpr int maximumCharacters = 80;
    static constexpr int markerLength = 3;
    static NativeMarkdown previewCache;
    const auto text = previewCache.render(message->content).value("plain").toString().simplified();
    if (text.size() <= maximumCharacters) return text;
    return text.left(maximumCharacters - markerLength).trimmed() + QStringLiteral("...");
}

void NativeController::openMessageInChat(const QString &id)
{
    // Quote frames and urn:quote/urn:message anchors jump through the shipped
    // message-link machinery: resolve GET, open the destination topic, bounded
    // older-page walk, highlight flash (docs/NATIVE_DESKTOP_MESSAGE_LINKS.md).
    if (!authenticated() || id.isEmpty()) return;
    if (id == m_anchorMessage) { m_anchorHighlight.start(2500); return; }
    applyMessageRoute(workspace::orgRouteIdFromOrigin(m_api->server()), m_session->project(), id);
}

void NativeController::scheduleQuoteFetch(const QString &id) const
{
    if (m_quotes.contains(id) || m_quoteInFlight.contains(id)) return;
    if (m_quoteRetryAfter.value(id, 0) > QDateTime::currentMSecsSinceEpoch()) return;
    m_quoteInFlight.insert(id);
    auto *self = const_cast<NativeController *>(this);
    QMetaObject::invokeMethod(self, [self, id] {
        const auto project = self->m_session->project();
        self->request("GET", messenger + "/messages/", {}, {{"project_id", project}, {"uuid", id}},
            [self, id](const ApiResult &result) {
            self->m_quoteInFlight.remove(id);
            if (!result.ok()) {
                // A transport failure is not an authoritative miss: keep the lookup
                // retriable with a short backoff (messenger-quote-loader.lib.ts keeps
                // "stale" distinct from "unavailable"), matching the design doc.
                // The timer re-issues the lookup on its own so the preview recovers
                // even when no projection happens after the connection returns.
                self->m_quoteRetryAfter.insert(id, QDateTime::currentMSecsSinceEpoch() + 3000);
                QTimer::singleShot(3200, self, [self, id] { self->scheduleQuoteFetch(id); });
                emit self->messagesChanged();
                return;
            }
            self->m_quoteRetryAfter.remove(id);
            QuoteRecord record;
            const QJsonArray rows = result.ok() && result.data.isArray() ? result.data.toArray() : QJsonArray{};
            const auto parsed = rows.isEmpty() ? std::optional<Message>{}
                : parseMessage(rows.first().toObject(), self->m_session->project());
            if (parsed && parsed->uuid == id && parsed->kind == "markdown") {
                record.authorId = parsed->author;
                record.content = parsed->content;
            } else {
                record.missing = true;
            }
            self->m_quotes.insert(id, record);
            emit self->messagesChanged();
        });
    }, Qt::QueuedConnection);
}

void NativeController::resetReadState()
{
    m_reactionRefreshes.clear();
    m_reactionRevisions.clear();
    m_ownReactions.clear();
    ++m_readTicket;
    ++m_readCounterRequest;
    m_readPending = false;
    m_readBoundary.reset();
    m_readStreamBoundary.reset();
    m_queuedReadBoundaries.clear();
    m_readTopics.clear();
    m_readCounterRequests.clear();
    ++m_readCounterInvalidation;
    m_failedReadAnchors.clear();
    m_readLoadedIds.clear();
    m_confirmedReadIds.clear();
    m_readPostRetries = m_readConfirmAttempts = 0;
}

static bool throughBoundary(const Message &message, const Message &boundary)
{
    return message.createdMicros < boundary.createdMicros
        || (message.createdMicros == boundary.createdMicros && message.uuid <= boundary.uuid);
}

void NativeController::markVisibleMessagesRead(const QVariantList &ids)
{
    if (ids.size() > 256 || !authoritativeRoute() || m_loading || m_snapshotActive) return;
    QMap<QString, Message> boundaries;
    for (const auto &value : ids) {
        if (value.metaType().id() != QMetaType::QString) return;
        const auto *message = findMessage(value.toString());
        if (!message || !inConversation(*message)) continue;
        const auto previous = boundaries.constFind(message->topic);
        if (previous == boundaries.cend() || !throughBoundary(*message, previous.value()))
            boundaries.insert(message->topic, *message);
    }
    const auto selection = m_selection;
    const QPointer<NativeController> guard(this);
    for (const auto &boundary : boundaries) {
        markMessagesReadUpTo(boundary.uuid);
        if (!guard || selection != m_selection) return;
    }
}

void NativeController::markMessagesReadUpTo(const QString &id)
{
    if (!authoritativeRoute() || m_loading || m_snapshotActive || m_failedReadAnchors.contains(id))
        return;
    const auto *anchor = findMessage(id);
    if (!anchor || !inConversation(*anchor))
        return;
    const auto unread = std::any_of(m_messages.cbegin(), m_messages.cend(), [anchor](const Message &message) {
        return !message.read && message.stream == anchor->stream && message.topic == anchor->topic && throughBoundary(message, *anchor);
    });
    if (!unread)
        return;
    if (m_readPending) {
        if (anchor->topic == m_readBoundary->topic && throughBoundary(*anchor, *m_readBoundary)) return;
        const auto queued = m_queuedReadBoundaries.constFind(anchor->topic);
        if (queued != m_queuedReadBoundaries.cend()) {
            if (!throughBoundary(*anchor, queued.value())) m_queuedReadBoundaries[anchor->topic] = *anchor;
        } else if (m_queuedReadBoundaries.size() < 256) {
            m_readTopics.enqueue(anchor->topic);
            m_queuedReadBoundaries.insert(anchor->topic, *anchor);
        }
        return;
    }
    m_readBoundary = *anchor;
    m_readLoadedIds.clear();
    for (const auto &message : m_messages)
        if (message.stream == anchor->stream && message.topic == anchor->topic && throughBoundary(message, *anchor))
            m_readLoadedIds.insert(message.uuid);
    m_readPending = true;
    m_readPostRetries = m_readConfirmAttempts = 0;
    ++m_readTicket;
    sendReadRequest(false);
}

void NativeController::sendReadRequest(bool confirmation)
{
    if (!m_readPending || !m_readBoundary)
        return;
    const auto boundary = *m_readBoundary;
    const auto selection = m_selection;
    const auto ticket = m_readTicket;
    const auto owner = m_session->user();
    const QPointer<NativeCacheService> cache = m_cache;
    const auto cacheLease = m_cacheLease;
    if (confirmation)
        ++m_readConfirmAttempts;
    if (!confirmation) staleReadConversation(boundary.stream, boundary.topic);
    request(confirmation ? "GET" : "POST", messenger + "/messages/" + boundary.uuid
        + (confirmation ? QString() : QStringLiteral("/actions/read_up_to/invoke")), {}, {},
        [this, boundary, selection, ticket, owner, confirmation, cache, cacheLease](const ApiResult &result) {
        staleReadConversation(boundary.stream, boundary.topic);
        if (!m_readPending || ticket != m_readTicket || selection != m_selection || !authenticated()
            || !inConversation(boundary)
            || owner != m_session->user())
            return;
        const auto retry = [this, ticket, selection](bool confirm, int delay) {
            QTimer::singleShot(delay, this, [this, ticket, selection, confirm] {
                if (m_readPending && m_readTicket == ticket && m_selection == selection)
                    sendReadRequest(confirm);
            });
        };
        if (!result.ok()) {
            const bool transient = result.status == 0 || result.status == 408 || result.status == 429 || result.status >= 500;
            if (transient && !confirmation && m_readPostRetries < 2) {
                retry(false, 250 << m_readPostRetries++);
                return;
            }
            if (transient && confirmation && m_readConfirmAttempts < 3) {
                retry(true, 250 << (m_readConfirmAttempts - 1));
                return;
            }
            finishReadRequest(false);
            return;
        }
        const auto json = result.data.toObject();
        const auto anchor = parseMessage(json, boundary.project);
        if (!anchor || anchor->uuid != boundary.uuid || anchor->stream != boundary.stream || anchor->topic != boundary.topic
            || anchor->author != boundary.author || anchor->createdMicros != boundary.createdMicros || !json.value("read").isBool()
            || (json.contains("user_uuid") && json.value("user_uuid").toString() != owner)) {
            finishReadRequest(false);
            return;
        }
        if (!anchor->read) {
            // Journal admission can precede application. Confirm the exact anchor
            // with bounded GETs instead of repeatedly admitting the same mutation.
            if (!confirmation) {
                sendReadRequest(true);
            } else if (m_readConfirmAttempts < 3) {
                retry(true, 250 << (m_readConfirmAttempts - 1));
            } else {
                finishReadRequest(false);
            }
            return;
        }
        ++m_messageRevision;
        m_confirmedReadIds.unite(m_readLoadedIds);
        if (cache && cache == m_cache && cacheLease.owner == m_cacheLease.owner
            && cacheLease.generation == m_cacheLease.generation)
            cache->markHistoryRead(cacheLease, boundary.stream, boundary.topic, m_readLoadedIds);
        for (auto &message : m_messages)
            if (m_readLoadedIds.contains(message.uuid) && message.project == boundary.project
                && message.stream == boundary.stream && message.topic == boundary.topic && throughBoundary(message, boundary))
                message.read = true;
        const QPointer<NativeController> guard(this);
        emit messagesChanged();
        if (!guard || m_readTicket != ticket || m_selection != selection)
            return;
        refreshReadCounters(boundary);
        finishReadRequest(true);
    });
}

void NativeController::finishReadRequest(bool success)
{
    if (!m_readBoundary)
        return;
    if (!success) {
        m_failedReadAnchors.insert(m_readBoundary->uuid);
        m_error = tr("The read marker could not be confirmed. Refresh the conversation to try again.");
    }
    m_readPending = false;
    m_readBoundary.reset();
    const auto selection = m_selection;
    const QPointer<NativeController> guard(this);
    emit stateChanged();
    if (!guard || selection != m_selection) return;
    while (!m_readTopics.isEmpty() && !m_readPending) {
        const auto topic = m_readTopics.dequeue();
        const auto next = m_queuedReadBoundaries.take(topic);
        markMessagesReadUpTo(next.uuid);
        if (!guard || selection != m_selection) return;
    }
    if (!m_readPending && m_readStreamBoundary) {
        const auto boundary = *m_readStreamBoundary;
        m_readStreamBoundary.reset();
        refreshReadStreamCounter(boundary);
    }
}

static bool completeReadCounterSnapshot(const QJsonObject &json)
{
    return json.contains("unread_count") && json.contains("active_unread_count")
        && json.contains("passive_unread_count") && json.contains("last_message_uuid");
}

void NativeController::refreshReadCounters(const Message &boundary)
{
    const auto selection = m_selection;
    const auto ticket = ++m_readCounterRequest;
    const auto counterRevision = m_readCounterInvalidation;
    m_readCounterRequests.insert(boundary.topic, ticket);
    const auto owner = m_session->user();
    m_readStreamBoundary = boundary;
    request("GET", messenger + "/stream_topics/" + boundary.topic, {}, {},
        [this, boundary, selection, ticket, counterRevision, owner](const ApiResult &result) {
        if (m_readCounterRequests.value(boundary.topic) != ticket) return;
        m_readCounterRequests.remove(boundary.topic);
        if (selection != m_selection
            || !inConversation(boundary) || counterRevision != m_readCounterInvalidation || owner != m_session->user())
            return;
        const auto json = result.data.toObject();
        const auto snapshot = parseTopic(json, boundary.project, boundary.stream);
        if (!result.ok() || !snapshot || !completeReadCounterSnapshot(json) || snapshot->uuid != boundary.topic
            || (json.contains("user_uuid") && json.value("user_uuid").toString() != owner)) {
            m_error = tr("The read marker was saved, but unread counters could not be refreshed.");
            emit stateChanged();
            return;
        }
        for (auto &topic : m_topics)
            if (topic.uuid == boundary.topic) {
                topic.unread = snapshot->unread; topic.activeUnread = snapshot->activeUnread;
                topic.passiveUnread = snapshot->passiveUnread; topic.lastMessage = snapshot->lastMessage;
            }
        ++m_topicRevision;
        emit topicsChanged();
    });
}

void NativeController::refreshReadStreamCounter(const Message &boundary)
{
    const auto selection = m_selection;
    const auto ticket = m_readCounterRequest;
    const auto streamRevision = m_streamRevision;
    const auto counterRevision = m_readCounterInvalidation;
    const auto owner = m_session->user();
    request("GET", messenger + "/streams/" + boundary.stream, {}, {},
        [this, boundary, selection, ticket, streamRevision, counterRevision, owner](const ApiResult &result) {
        if (selection != m_selection || ticket != m_readCounterRequest || streamRevision != m_streamRevision
            || !inConversation(boundary) || counterRevision != m_readCounterInvalidation || owner != m_session->user())
            return;
        const auto json = result.data.toObject();
        const auto snapshot = parseStream(json, boundary.project);
        if (!result.ok() || !snapshot || !completeReadCounterSnapshot(json) || snapshot->uuid != boundary.stream
            || (json.contains("user_uuid") && json.value("user_uuid").toString() != owner)) {
            m_error = tr("The read marker was saved, but unread counters could not be refreshed.");
            emit stateChanged();
            return;
        }
        for (auto &stream : m_streams)
            if (stream.uuid == boundary.stream) {
                stream.unread = snapshot->unread; stream.activeUnread = snapshot->activeUnread;
                stream.passiveUnread = snapshot->passiveUnread; stream.lastMessage = snapshot->lastMessage;
            }
        ++m_streamRevision;
        emit streamsChanged();
    });
}

void NativeController::verifyMutation(const QString &id, quint64 selection, NativeCache::Projection field,
                                      const std::function<void(bool)> &settled)
{
    const auto versions = m_messageFields.value(id);
    const auto context = projectionContext();
    request("GET", messenger + "/messages/" + id, {}, {},
            [this, id, selection, field, versions, context, settled](const ApiResult &result) {
                const auto currentVersions = m_messageFields.value(id);
                const bool unchanged = field == NativeCache::Projection::Payload
                                           ? versions.payload == currentVersions.payload
                                           : versions.star == currentVersions.star;
                const auto *current = findMessage(id);
                const auto message = parseMessage(result.data.toObject(), m_session->project());
                if (selection != m_selection || currentVersions.deleted || !current || !unchanged) {
                    finishMessageIntent(id);
                    if (settled) settled(false);
                    return;
                }
                if (!result.ok() || !message || message->uuid != id || !inConversation(*message) || message->topic != current->topic || message->author != current->author) {
                    finishMessageIntent(id);
                    if (settled) settled(false);
                    report(tr("The message change could not be verified."));
                    return;
                }
                auto updated = *current;
                if (field == NativeCache::Projection::Payload) {
                    updated.kind = message->kind;
                    updated.content = message->content;
                    // Confirmed payload edits flow into cached quote previews that
                    // reference this message from other conversations (review #119).
                    if (auto quoteIt = m_quotes.find(id); quoteIt != m_quotes.end() && !quoteIt->missing) {
                        quoteIt->content = message->content;
                        quoteIt->authorId = message->author;
                    }
                    auto &state = m_messageFields[id];
                    state.payloadHash = QCryptographicHash::hash(
                        QJsonDocument(result.data.toObject().value("payload").toObject())
                            .toJson(QJsonDocument::Compact),
                        QCryptographicHash::Sha256);
                    state.payload = ++m_nextMessageField;
                    // Issue #116: the verified body owns the memoized row preview too.
                    // Without this the sidebar keeps showing the pre-edit body as soon as
                    // the message leaves m_messages after navigating away.
                    refreshRowPreviewFromPayload(updated);
                } else {
                    updated.starred = message->starred;
                    m_messageFields[id].starred = message->starred;
                    m_messageFields[id].star = ++m_nextMessageField;
                }
                updated.read = updated.read || message->read || m_confirmedReadIds.contains(id);
                ++m_messageRevision;
                staleConversation(message->stream, message->topic, field == NativeCache::Projection::Payload);
                projectCanonical(result.data.toObject(), field, context);
                m_messages = mergeMessages(m_messages, {updated});
                finishMessageIntent(id);
                // An inline edit session closes only once the server body is confirmed here.
                if (settled) settled(true);
                emit messagesChanged();
                emit stateChanged();
            });
}

void NativeController::starMessage(const QString &id)
{
    if (!authoritativeRoute()) return;
    const auto *message = findMessage(id);
    if (!message)
        return;
    ++m_messageFields[id].pending;
    const auto selection = m_selection;
    const auto stream = message->stream, topic = message->topic;
    staleConversation(stream, topic, false);
    request("POST", messenger + "/messages/" + id + "/actions/"
        + (message->starred ? "unstar" : "star") + "/invoke", {}, {},
        [this, id, selection, stream, topic](const ApiResult &result) {
        staleConversation(stream, topic, false);
        if (selection != m_selection) { finishMessageIntent(id); return; }
        if (!result.ok()) {
            finishMessageIntent(id);
            report(tr("Could not change starred state. %1").arg(result.error));
        } else verifyMutation(id, selection, NativeCache::Projection::Star);
    });
}

void NativeController::editMessage(const QString &id, const QString &text)
{
    submitMessageEdit(id, text, {});
}

// The Source edits in the composer (docs/NATIVE_DESKTOP_INLINE_EDIT.md): the message body
// becomes the composer text, the conversation's stored draft is not written while the
// session is open, and the session ends on cancel, on a confirmed save or on a route change.
bool NativeController::beginEditMessage(const QString &id)
{
    const auto *message = editableMessage(id);
    if (!message) {
        report(tr("This message can no longer be edited"));
        return false;
    }
    // Reply and edit sessions are mutually exclusive (one composer, one mode).
    closeReplySession();
    m_editingMessage = id;
    m_editBuffer = message->content;
    m_editRoute = conversationKey();
    // Each open session gets a fresh identity and buffer revision (issue #115), so an
    // in-flight save from a previous session can never match the reopened one.
    ++m_editSession;
    ++m_editRevision;
    emit editSessionChanged();
    emit draftChanged();
    emit stateChanged();
    return true;
}

void NativeController::cancelEditMessage()
{
    closeEditSession();
}

void NativeController::submitEditMessage()
{
    if (m_editingMessage.isEmpty()) return;
    if (!canSaveEdit()) return;   // the disabled save control also means "do not submit"
    const auto id = m_editingMessage;
    const auto text = m_editBuffer;
    // Issue #115: the completion may only close the exact session and buffer revision it
    // submitted. Text typed after Save, or a session reopened for the same UUID, gets a
    // newer identity and survives this completion; B stays savable.
    const auto session = m_editSession, revision = m_editRevision;
    submitMessageEdit(id, text, [this, id, session, revision](bool confirmed) {
        if (confirmed && m_editingMessage == id
            && session == m_editSession && revision == m_editRevision)
            closeEditSession();
    });
}

void NativeController::editLastMessage()
{
    const auto *stream = currentStream();
    if (authoritativeRoute() && stream && !stream->encrypted) {
        // m_messages is sorted oldest first, so the tail is the newest message.
        for (auto it = m_messages.constEnd(); it != m_messages.constBegin();) {
            --it;
            const bool own = it->own || it->author == m_session->user();
            if (!own || it->kind != "markdown") continue;
            if (it->stream != m_stream || it->topic != m_topic) continue;
            beginEditMessage(it->uuid);
            return;
        }
    }
    report(tr("This message can no longer be edited"));
}

bool NativeController::canSaveEdit() const
{
    if (m_editingMessage.isEmpty() || !authoritativeRoute()) return false;
    if (conversationKey() != m_editRoute) return false;
    // While a save for this message is in flight a second save would race the first PUT
    // (issue #115); the save control waits for the pending completion.
    if (m_editSaveInFlight.contains(m_editingMessage)) return false;
    const auto *stream = currentStream();
    if (!stream || stream->encrypted) return false;
    return !m_editBuffer.trimmed().isEmpty() && m_editBuffer.size() <= 40000;
}

void NativeController::closeEditSession()
{
    if (m_editingMessage.isEmpty()) return;
    m_editingMessage.clear();
    m_editBuffer.clear();
    m_editRoute.clear();
    emit editSessionChanged();
    emit draftChanged();
    emit stateChanged();
}

void NativeController::settleEditRouteChange()
{
    if (!m_editingMessage.isEmpty() && conversationKey() != m_editRoute)
        closeEditSession();
    // The reply slot belongs to the conversation that started it
    // (docs/NATIVE_DESKTOP_REPLIES.md); leaving the conversation ends the session.
    if (!m_replyMessage.isEmpty() && conversationKey() != m_replyRoute)
        closeReplySession();
}

const Message *NativeController::editableMessage(const QString &id) const
{
    if (!authoritativeRoute()) return nullptr;
    const auto *stream = currentStream();
    const auto *message = findMessage(id);
    if (!message || !stream || stream->encrypted) return nullptr;
    if (!message->own && message->author != m_session->user()) return nullptr;
    if (message->kind != "markdown") return nullptr;
    if (message->stream != m_stream || message->topic != m_topic) return nullptr;
    return message;
}

void NativeController::submitMessageEdit(const QString &id, const QString &text,
                                         const std::function<void(bool)> &settled)
{
    if (!authoritativeRoute()) { if (settled) settled(false); return; }
    const auto *message = findMessage(id);
    const auto *stream = currentStream();
    if (!message || (!message->own && message->author != m_session->user())
        || message->kind != "markdown" || text.trimmed().isEmpty()
        || text.size() > 40000 || !stream || stream->encrypted) {
        if (settled) settled(false);
        return;
    }
    // Issue #115: only one edit save per message may be in flight; a second submit is
    // refused instead of racing the pending PUT. The flag clears on every completion
    // path before the caller's callback runs, so a later save is never locked out.
    // canSaveEdit reads the guard, so both transitions notify editSessionChanged
    // (its NOTIFY signal) or QML save controls keep a stale enabled state (review #130).
    if (m_editSaveInFlight.contains(id)) { if (settled) settled(false); return; }
    m_editSaveInFlight.insert(id);
    emit editSessionChanged();
    const auto done = [this, id, settled](bool confirmed) {
        m_editSaveInFlight.remove(id);
        emit editSessionChanged();
        if (settled) settled(confirmed);
    };
    ++m_messageFields[id].pending;
    const auto selection = m_selection;
    const auto streamId = message->stream, topic = message->topic;
    staleConversation(streamId, topic);
    request("PUT", messenger + "/messages/" + id,
        QJsonObject{{"payload", QJsonObject{{"kind", "markdown"}, {"content", text}}}}, {},
        [this, id, selection, streamId, topic, done](const ApiResult &result) {
        staleConversation(streamId, topic);
        if (selection != m_selection) { finishMessageIntent(id); done(false); return; }
        if (!result.ok()) {
            finishMessageIntent(id);
            done(false);
            report(tr("Could not edit the message. %1").arg(result.error));
        } else verifyMutation(id, selection, NativeCache::Projection::Payload, done);
    });
}

void NativeController::deleteMessage(const QString &id)
{
    if (!authoritativeRoute()) return;
    const auto *message = findMessage(id);
    if (!message || (!message->own && message->author != m_session->user()))
        return;
    const auto selection = m_selection;
    const auto stream = message->stream, topic = message->topic;
    invalidateConversation(stream, topic);
    request("DELETE", messenger + "/messages/" + id, {}, {},
        [this, id, selection, stream, topic](const ApiResult &result) {
        invalidateConversation(stream, topic);
        if (selection != m_selection)
            return;
        if (!result.ok()) {
            report(tr("Could not delete the message. %1").arg(result.error));
            return;
        }
        if (m_messageFields.contains(id)) m_messageFields[id].deleted = true;
        ++m_messageRevision;
        m_messages.erase(std::remove_if(m_messages.begin(), m_messages.end(),
            [&id](const Message &candidate) { return candidate.uuid == id; }), m_messages.end());
        // A deleted origin must not keep rendering from the quote cache; the next
        // projection re-resolves and lands on "unavailable" (review #119).
        m_quotes.remove(id);
        // Issue #116: the preview memo gets the same lifecycle as the quote cache: a row
        // still pointing at this uuid must not keep rendering the deleted body.
        if (m_rowPreviews.contains(id)) markRowPreviewDeleted(id);
        emit messagesChanged();
        emit stateChanged();
    });
}

}
