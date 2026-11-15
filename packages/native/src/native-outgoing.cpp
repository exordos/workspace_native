#include "native-controller.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QPointer>
#include <QTimer>
#include <QScopeGuard>
#include <QUuid>
#include <algorithm>

namespace workspace {
namespace {
bool sameLease(const NativeDraftService::Lease &a, const NativeDraftService::Lease &b)
{ return a.owner == b.owner && a.issuer == b.issuer && a.generation == b.generation; }
bool sameCache(const NativeCache::Lease &a, const NativeCache::Lease &b)
{ return a.owner == b.owner && a.generation == b.generation; }
}

NativeDraftService *NativeController::outgoingStorage() const
{ return m_draftStore ? m_draftStore->storage() : nullptr; }

bool NativeController::outgoingReady() const
{
    auto *storage = outgoingStorage();
    return !storage || (storage == m_outgoingService && storage->available() && m_outgoingLoaded
        && creationOwnerKey() == m_outgoingOwner && sameLease(storage->lease(), m_outgoingLease));
}

bool NativeController::outgoingOwned(const SendIntent &intent, bool writing) const
{
    if (intent.encrypted) return encryptedSendOwned(intent, writing);
    if (!authenticated() || intent.owner != creationOwnerKey() || intent.project != m_session->project()
        || intent.user != m_session->user() || !m_catalogVerified || m_snapshotActive || m_syncBlocked
        || m_cacheForgettingAccount) return false;
    const auto row = std::find_if(m_streams.cbegin(), m_streams.cend(), [&](const Stream &s) { return s.uuid == intent.stream; });
    if (row == m_streams.cend() || row->encrypted) return false;
    if (!writing) return true;
    return authoritativeRoute() && m_stream == intent.stream && m_topic == intent.topic && !topicClosed()
        && (!m_draftStore || outgoingReady());
}

bool NativeController::outgoingContext(const QString &conversation, const QString &id, quint64 ticket) const
{
    const auto found = m_pendingSends.constFind(conversation);
    if (found == m_pendingSends.cend() || found->uuid != id || found->ticket != ticket) return false;
    if (found->encrypted) return encryptedSendContext(conversation, id, ticket);
    return outgoingOwned(*found, false) && found->projection.generation == m_api->generation()
        && found->catalog == m_catalog && found->projection.cache == m_cache
        && sameCache(found->projection.lease, m_cacheLease)
        && (!m_cache || sameCache(m_cache->lease(), found->projection.lease));
}

OutgoingRecord NativeController::outgoingRecord(const SendIntent &intent) const
{
    OutgoingRecord record;
    record.uuid = intent.uuid; record.stream = intent.stream; record.topic = intent.topic;
    record.routeTopic = intent.routeTopic; record.publicId = intent.publicId;
    record.payload = intent.payload; record.createdAt = intent.createdAt;
    if (intent.encrypted) record.deletionRevision = qint64(intent.deletion);
    else {
        record.draftUuid = intent.draftToken.uuid; record.draftSnapshot = intent.draftToken.snapshot;
        record.draftText = intent.text; record.replyActive = intent.replyActive; record.replySnapshot = intent.replySnapshot;
    }
    return record;
}

void NativeController::synchronizeOutgoing()
{
    if (m_outgoingSyncing) return;
    m_outgoingSyncing = true;
    const QPointer<NativeController> guard(this);
    const auto leave = qScopeGuard([guard] { if (guard) guard->m_outgoingSyncing = false; });
    auto *storage = outgoingStorage();
    const auto owner = creationOwnerKey();
    if (!storage || owner.isEmpty()) {
        m_outgoingOwner.clear(); m_outgoingService = storage;
        m_outgoingLoaded = m_outgoingLoading = false; return;
    }
    const auto lease = storage->lease();
    if (owner != lease.owner || !storage->available()) return;
    if (m_outgoingOwner != owner || m_outgoingService != storage || !sameLease(lease, m_outgoingLease)) {
        m_outgoingOwner = owner; m_outgoingService = storage; m_outgoingLease = lease;
        m_outgoingLoaded = m_outgoingLoading = false; m_outgoingError.clear();
    }
    if (!m_outgoingLoaded && !m_outgoingLoading && m_outgoingError.isEmpty()) {
        m_outgoingLoading = true;
        storage->outgoing(lease, this, [this, owner, lease, storage = QPointer<NativeDraftService>(storage)](NativeDraftStore::OutgoingPage page) {
            if (!storage || storage != outgoingStorage() || owner != creationOwnerKey() || !sameLease(lease, storage->lease())) return;
            m_outgoingLoading = false;
            if (page.failure != NativeDraftService::Failure::None) {
                m_outgoingError = tr("Pending messages could not be loaded. Sending is paused."); emit stateChanged(); return;
            }
            for (const auto &record : page.rows) {
                const auto conversation = record.stream + '/' + record.routeTopic;
                if (m_pendingSends.contains(conversation)) continue;
                SendIntent intent; intent.uuid = record.uuid; intent.publicId = record.publicId;
                intent.stream = record.stream; intent.topic = record.topic; intent.routeTopic = record.routeTopic;
                intent.payload = record.payload; intent.encrypted = record.payload.value("kind") == QJsonValue("e2ee");
                intent.owner = intent.encrypted ? ownerKey() : owner;
                intent.project = m_session->project(); intent.user = m_session->user();
                intent.key = record.payload.value("key_uuid").toString(); intent.deletion = quint64(record.deletionRevision);
                intent.createdAt = record.createdAt; intent.durable = true; intent.restored = true; intent.admitted = true;
                intent.phase = "uncertain"; intent.error = tr("A pending message was restored. Check its status before retrying.");
                if (!intent.encrypted) {
                    intent.text = record.draftText; intent.wire = record.payload.value("content").toString();
                    intent.draftToken = {owner, record.stream, record.topic, record.draftUuid, record.draftSnapshot, record.draftText};
                    intent.draftCaptured = !record.draftUuid.isEmpty();
                    intent.replyActive = record.replyActive; intent.replySnapshot = record.replySnapshot;
                }
                m_pendingSends.insert(conversation, intent);
                ++m_messageFields[intent.publicId].pending;
            }
            m_outgoingLoaded = true;
            emit stateChanged();
        });
        return;
    }
    if (!m_outgoingLoaded) return;
    for (auto it = m_pendingSends.begin(); it != m_pendingSends.end(); ++it) {
        // Discard is an owner-scoped storage operation. Cache/transport
        // replacement must not re-enable Retry before its deletion commits.
        if (!it->durable || !it->busy || it->phase == "discarding" || outgoingContext(it.key(), it->uuid, it->ticket)) continue;
        it->busy = false; it->phase = "uncertain"; ++it->ticket; m_sending = false;
        it->error = tr("Sending was interrupted. Check the saved message status before retrying.");
    }
    auto found = m_pendingSends.find(conversationKey());
    if (found == m_pendingSends.end() || !found->restored || found->automaticChecked || found->busy || m_sending
        || !outgoingOwned(*found, false) || !currentStream() || !m_topicsVerified
        || (!found->encrypted && m_draftStore && !m_draftStore->ready(found->stream, found->topic))) return;
    found->automaticChecked = true;
    // Reconciliation is GET-only on startup, and only after the original
    // conversation's current authority and draft have finished loading.
    const auto conversation = conversationKey();
    QTimer::singleShot(0, this, [this, conversation] {
        if (conversation == conversationKey()) checkPendingSend();
    });
}

void NativeController::admitOutgoing(const QString &conversation, std::function<void()> dispatch)
{
    const auto intent = m_pendingSends.value(conversation);
    if (!intent.durable) { dispatch(); return; }
    const auto id = intent.uuid; const auto ticket = intent.ticket;
    auto *storage = outgoingStorage();
    if (!storage || !outgoingReady()) { failOutgoing(conversation, id, ticket, tr("The pending message could not be saved. Your text is preserved.")); return; }
    const auto lease = m_outgoingLease;
    const auto persist = [this, conversation, id, ticket, lease, storage = QPointer<NativeDraftService>(storage), dispatch = std::move(dispatch)](bool flushed) {
        if (!outgoingContext(conversation, id, ticket)) return;
        if (!flushed || !storage || storage != outgoingStorage() || !sameLease(lease, storage->lease())) {
            failOutgoing(conversation, id, ticket, tr("The pending message could not be saved. Your text is preserved.")); return;
        }
        storage->putOutgoing(lease, outgoingRecord(m_pendingSends.value(conversation)), this,
            [this, conversation, id, ticket, dispatch](NativeDraftService::Failure failure) {
            if (!outgoingContext(conversation, id, ticket)) return;
            if (failure != NativeDraftService::Failure::None) {
                failOutgoing(conversation, id, ticket, tr("The pending message could not be saved. Your text is preserved.")); return;
            }
            m_pendingSends[conversation].admitted = true;
            dispatch();
        });
    };
    // An accepted but debounced draft write must not resurrect the sent draft
    // after the outgoing completion transaction has consumed its snapshot.
    if (!intent.encrypted && m_draftStore) m_draftStore->flush(this, persist);
    else persist(true);
}

void NativeController::retireOutgoing(const SendIntent &intent, std::function<void()> complete)
{
    if (!intent.durable || intent.retired) { complete(); return; }
    const auto conversation = intent.stream + '/' + intent.routeTopic;
    auto *storage = outgoingStorage();
    if (!storage || !outgoingReady()) { failOutgoing(conversation, intent.uuid, intent.ticket, tr("Message confirmation could not be saved. Check its status again.")); return; }
    storage->finishOutgoing(m_outgoingLease, intent.uuid, true, this,
        [this, conversation, id = intent.uuid, ticket = intent.ticket, complete = std::move(complete)](NativeDraftService::Failure failure) {
        if (!outgoingContext(conversation, id, ticket)) return;
        if (failure != NativeDraftService::Failure::None) {
            failOutgoing(conversation, id, ticket, tr("Message confirmation could not be saved. Check its status again.")); return;
        }
        complete();
    });
}

void NativeController::failOutgoing(const QString &conversation, const QString &id, quint64 ticket, const QString &error)
{
    auto found = m_pendingSends.find(conversation);
    if (found == m_pendingSends.end() || found->uuid != id || found->ticket != ticket) return;
    if (found->encrypted) { finishEncryptedSend(conversation, id, ticket, error); return; }
    found->busy = false; found->phase = "uncertain"; found->error = error; m_sending = false;
    if (!found->durable) finishMessageIntent(found->publicId);
    if (conversation == conversationKey()) report(error); else emit stateChanged();
}

void NativeController::recoverOrdinarySend(bool retry)
{
    const auto conversation = conversationKey(); auto found = m_pendingSends.find(conversation);
    if (found == m_pendingSends.end() || found->encrypted || found->busy || m_sending || !outgoingOwned(*found, retry)) return;
    found->projection = projectionContext(); found->catalog = m_catalog; found->ticket = ++m_nextOutgoing;
    found->busy = true; found->phase = "checking"; found->error.clear(); m_sending = true;
    const auto id = found->uuid; const auto ticket = found->ticket;
    const bool admitted = found->admitted;
    const QPointer<NativeController> guard(this); emit stateChanged();
    if (!guard || !outgoingContext(conversation, id, ticket)) return;
    const auto readBack = [this, conversation, id, ticket, retry] { readOrdinarySend(conversation, id, ticket, retry); };
    if (!admitted && retry) admitOutgoing(conversation, readBack); else readBack();
}

QVariantMap NativeController::outgoingSend() const
{
    if (!m_outgoingError.isEmpty()) return {{"error", m_outgoingError}, {"busy", false}, {"canCheck", true}};
    const auto found = m_pendingSends.constFind(conversationKey());
    if (found == m_pendingSends.cend() || !found->durable || !outgoingOwned(*found, false)) return {};
    const bool check = !found->busy && !m_sending;
    bool retry = check && outgoingOwned(*found, true);
    if (found->encrypted && found->restored && check && chatVaultMatches()) {
        // Availability is a capability hint; the explicit Retry action reads
        // and verifies the original key/deletion revision before dispatch.
        retry = authoritativeRoute() && m_stream == found->stream && m_topic == found->routeTopic;
    }
    return {{"id", found->publicId}, {"phase", found->phase}, {"busy", found->busy},
        {"error", found->error}, {"canCheck", check}, {"canRetry", retry}, {"canDiscard", check}};
}

void NativeController::discardPendingSend()
{
    const auto conversation = conversationKey(); const auto found = m_pendingSends.constFind(conversation);
    if (found == m_pendingSends.cend() || !found->durable || found->busy || m_sending || !outgoingOwned(*found, false)) return;
    const auto intent = *found; auto *storage = outgoingStorage(); if (!storage || !outgoingReady()) return;
    m_pendingSends[conversation].busy = true; m_pendingSends[conversation].phase = "discarding";
    storage->finishOutgoing(m_outgoingLease, intent.uuid, false, this,
        [this, conversation, intent](NativeDraftService::Failure failure) {
        const auto found = m_pendingSends.constFind(conversation);
        if (found == m_pendingSends.cend() || found->uuid != intent.uuid || found->ticket != intent.ticket
            || found->phase != "discarding" || found->owner != intent.owner
            || (intent.encrypted ? ownerKey() : creationOwnerKey()) != intent.owner) return;
        if (failure != NativeDraftService::Failure::None) {
            auto &pending = m_pendingSends[conversation];
            pending.busy = false; pending.phase = "uncertain";
            pending.error = tr("The pending message could not be removed.");
            if (conversation == conversationKey()) report(pending.error); else emit stateChanged();
            return;
        }
        m_pendingSends.remove(conversation);
        finishMessageIntent(intent.publicId); emit stateChanged();
    });
    emit stateChanged();
}

void NativeController::forgetSavedAccountWithFollowUp(const QString &id, std::function<void()> done)
{
    if (busy()) return;
    auto *storage = outgoingStorage();
    if (storage) for (const auto &account : m_session->savedAccounts()) {
        if (account.id != id) continue;
        const auto identity = QJsonDocument(QJsonArray{account.server.toString(QUrl::FullyEncoded), account.project, account.user})
            .toJson(QJsonDocument::Compact);
        const auto owner = QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
        const auto generation = m_api->generation();
        m_cacheForgettingAccount = true;
        storage->clearOutgoing(owner, this, [this, owner, id, generation, done = std::move(done)](NativeDraftService::Failure failure) {
            if (generation != m_api->generation()) return;
            m_cacheForgettingAccount = false;
            if (failure != NativeDraftService::Failure::None) {
                report(tr("Pending messages could not be removed. The saved account was kept.")); return;
            }
            if (owner == creationOwnerKey()) {
                for (auto it = m_pendingSends.begin(); it != m_pendingSends.end();) {
                    if (!it->durable) { ++it; continue; }
                    finishMessageIntent(it->publicId);
                    it = m_pendingSends.erase(it);
                }
            }
            finishForgetSavedAccount(id, done);
        });
        emit stateChanged(); return;
    }
    finishForgetSavedAccount(id, std::move(done));
}

} // namespace workspace
