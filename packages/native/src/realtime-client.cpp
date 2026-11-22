#include "realtime-client.h"

#include "domain.h"
#include "native-realtime-cursor-store.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QWebSocket>
#include <algorithm>
#include <cmath>

namespace workspace {
namespace {
constexpr quint64 MaxEpoch = 9007199254740991ULL;
constexpr int MaxFrameBytes = 1024 * 1024;
// Coalescing window for durable cursor writes. Catch-up delivers up to 100
// events per page, so one SQLite transaction per acknowledged event would
// serialize the GUI thread through every burst. Under-writing is always safe
// (catch-up re-delivers events above the persisted epoch), so a lost trailing
// window replays at most this much work. See
// docs/NATIVE_DESKTOP_DURABLE_REALTIME_CURSORS.md.
constexpr int CursorPersistDelayMs = 500;

bool epochValue(const QJsonValue &value, quint64 *epoch)
{
    if (!value.isDouble())
        return false;
    const auto number = value.toDouble(-1);
    if (!std::isfinite(number) || number < 0 || number > double(MaxEpoch) || std::floor(number) != number)
        return false;
    *epoch = quint64(number);
    return true;
}

bool nonempty(const QJsonValue &value)
{
    return value.isString() && !value.toString().trimmed().isEmpty();
}

bool expiredCursor(const QJsonObject &json)
{
    quint64 current = 0, minimum = 0;
    const auto reason = json.value("reason").toString();
    return json.value("type") == "EventsCursorExpiredError" && json.value("code") == 410
        && json.value("error") == "epoch_pruned" && json.value("message").isString()
        && nonempty(json.value("epoch_generation"))
        && epochValue(json.value("current_epoch_version"), &current)
        && epochValue(json.value("minimum_epoch_version"), &minimum) && minimum >= 1
        && (reason == "epoch_generation_required" || reason == "epoch_generation_changed"
            || reason == "future_epoch" || reason == "epoch_pruned");
}
} // namespace

RealtimeClient::RealtimeClient(ApiClient *api, Session *session, QObject *parent)
    : QObject(parent), m_api(api), m_session(session)
{
    m_reconnect.setSingleShot(true);
    m_handshake.setSingleShot(true);
    m_handshake.setInterval(30000);
    m_probe.setInterval(15000);
    m_cursorFlush.setSingleShot(true);
    connect(&m_cursorFlush, &QTimer::timeout, this, [this] {
        // While suspended the deferred write is kept on purpose: shutdown
        // and the next resume retry a failed persist (finding 4584028540).
        if (m_active)
            flushCursorPersist();
    });
    connect(&m_reconnect, &QTimer::timeout, this, [this] {
        if (!m_active)
            return;
        if (m_epochGeneration.isEmpty())
            baseline();
        else
            nudge();
    });
    connect(&m_handshake, &QTimer::timeout, this, [this] { retry("retrying"); });
    connect(&m_probe, &QTimer::timeout, this, &RealtimeClient::nudge);
    connect(api, &ApiClient::sessionChanged, this, [this] {
        suspend();
        if (m_wanted)
            setState("waiting_for_session");
    });
    connect(session, &Session::authenticated, this, [this] {
        if (m_wanted)
            resume();
    });
    connect(session, &Session::signedOut, this, [this] {
        suspend();
        // A signed-out owner's stream position must not survive anywhere:
        // the next connect re-baselines instead of resuming it.
        clearDurableCursor(m_durableKey);
        m_durableKey.clear();
        m_cursors.clear();
        m_hydratedOwners.clear();
        m_owner.clear();
        m_epochGeneration.clear();
        m_epoch = 0;
        m_refreshRequested = false;
        setState(m_wanted ? "waiting_for_session" : "stopped");
    });
}

RealtimeClient::~RealtimeClient() { suspend(); }

void RealtimeClient::setState(const QString &state)
{
    if (m_state == state)
        return;
    m_state = state;
    // Issue #668: every transition lands in the diagnostics ledger before the
    // signal fans out, so a snapshot taken from a connectionChanged handler
    // already sees the transition it reacted to.
    m_activity.recordStateChange(state, QDateTime::currentMSecsSinceEpoch());
    emit connectionChanged(state);
}

bool RealtimeClient::current(quint64 run, quint64 generation) const
{
    return m_wanted && m_active && m_run == run && m_generation == generation
        && m_api->generation() == generation && m_session->ready();
}

void RealtimeClient::start()
{
    m_wanted = true;
    resume();
}

void RealtimeClient::stop()
{
    m_wanted = false;
    suspend();
    setState("stopped");
}

void RealtimeClient::suspend()
{
    // Flush the acknowledged cursor before every teardown (retry included)
    // so a crash never loses more than the coalescing window.
    m_cursorFlush.stop();
    if (m_cursorDirty)
        flushCursorPersist();
    ++m_run;
    m_active = false;
    m_reconnect.stop();
    m_handshake.stop();
    m_probe.stop();
    if (m_socket) {
        disconnect(m_socket, nullptr, this, nullptr);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    m_events.clear();
    m_pending = {};
    m_nextMarker.clear();
    m_markers.clear();
    m_fetching = m_waitingSnapshot = m_nudged = m_socketReady = false;
    m_socketEpoch = 0;
}

void RealtimeClient::resume()
{
    if (!m_wanted || !m_session->ready()) {
        setState("waiting_for_session");
        return;
    }
    const auto owner = m_api->server().toString() + '/' + m_session->project() + '/' + m_session->user();
    if (m_active && owner == m_owner && m_generation == m_api->generation())
        return;
    suspend();
    if (owner != m_owner) {
        m_refreshRequested = false;
        // Owner replacement fence: the departed owner's durable cursor is
        // deleted so it can never resume another owner's socket, and any
        // pending throttled write is discarded rather than re-persisted.
        // The departed owner's in-memory entry must leave with its durable
        // row, otherwise switching back selects it and silently resumes the
        // cursor that was just wiped (finding 4584028556).
        clearDurableCursor(m_durableKey);
        m_cursors.remove(m_owner);
        m_hydratedOwners.remove(m_owner);
        discardCursorPersist();
    }
    m_owner = owner;
    m_generation = m_api->generation();
    m_active = true;
    m_retries = 0;
    // The durable key must be recomputed while the session identity is live
    // (sign-out clears project/user before this client can react).
    m_durableKey = m_cursorStore
        ? RealtimeCursorStore::ownerKey(m_api->server(), m_session->project(), m_session->user())
        : QString();
    // A delete still owed from an earlier failed clear is retried here; while
    // it has not landed, the invalidated row stays fenced from restore so a
    // readable-but-write-rejecting store cannot resurrect it and keep the
    // 410 resynchronization spinning (findings 4584028530/4584028572).
    retryDurableClear();
    // A persist that failed before the suspend kept its dirty cursor, and
    // the coalescing timer is inert while suspended, so resume has to
    // re-arm it: without this the retained cursor strands until the next
    // acknowledged event even though storage may already be healthy
    // (finding 4584028540).
    if (m_cursorDirty && !m_cursorFlush.isActive() && m_cursorStore
        && !m_durableKey.isEmpty() && !m_epochGeneration.isEmpty())
        m_cursorFlush.start(CursorPersistDelayMs);
    auto saved = m_cursors.constFind(owner);
    if (saved == m_cursors.cend() && m_cursorStore && !m_durableKey.isEmpty()
        && !m_pendingClearKeys.contains(m_durableKey)) {
        // Restore-on-connect: a valid persisted row continues through the
        // same catch-up path the in-memory reconnect uses. A missing, invalid
        // or unreachable row keeps the untouched baseline path.
        if (const auto restored = m_cursorStore->read(m_durableKey)) {
            m_cursors.insert(owner, {restored->generation, restored->epoch});
            saved = m_cursors.constFind(owner);
        }
    }
    if (saved == m_cursors.cend()) {
        m_epochGeneration.clear();
        m_epoch = 0;
        baseline();
    } else if (!m_hydratedOwners.contains(owner)) {
        // Issue #376: transport position and application-model hydration are
        // separate state. A row read back from the durable store proves the
        // previous process acknowledged a snapshot, never that this process
        // published one: the consumer (NativeController) starts every fresh
        // process with empty catalogs, and catch-up events alone cannot
        // reconstruct channels that existed before the cursor. Keep the
        // restored position as the transport resume point (no epoch endpoint
        // round-trip, issue #15 intact), but require one authorized snapshot
        // through the same waiting gate the baseline path uses. While the
        // gate is closed catchUp/nudge/connectSocket stay blocked, so catch-up
        // cannot deliver events ahead of the catalogs; after the
        // acknowledgement, catchUp resumes above the restored epoch and
        // re-delivers every newer event, so events that arrived during the
        // initial loading are reconciled, never lost. In-memory reconnects
        // (same live process) skip the snapshot exactly as before, but only
        // once this process has acknowledged one: m_hydratedOwners tracks
        // that independently of the connection state, so a suspend()/resume()
        // during an unacknowledged restore-time snapshot re-arms this gate
        // instead of treating the restored cursor as hydrated (findings
        // 4599009714/4599009723).
        m_epochGeneration = saved->first;
        m_epoch = saved->second;
        m_waitingSnapshot = true;
        setState("snapshot_required");
        emit snapshotRequired();
    } else {
        m_epochGeneration = saved->first;
        m_epoch = saved->second;
        catchUp();
    }
}

void RealtimeClient::baseline()
{
    if (!m_active || m_fetching)
        return;
    m_fetching = true;
    setState("catching_up");
    const auto run = m_run, generation = m_generation;
    QPointer<RealtimeClient> self(this);
    m_api->request("GET", "/api/workspace/v1/epoch/", {}, {}, [self, run, generation](const ApiResult &result) {
        if (!self || !self->current(run, generation))
            return;
        self->m_fetching = false;
        if (result.status == 401) {
            self->authenticationFailure();
            return;
        }
        const auto json = result.data.toObject();
        quint64 epoch = 0, currentEpoch = 0, minimum = 0;
        if (!result.ok() || !epochValue(json.value("epoch_version"), &epoch)
            || !epochValue(json.value("current_epoch_version"), &currentEpoch)
            || !epochValue(json.value("minimum_epoch_version"), &minimum)
            || minimum < 1 || minimum > currentEpoch + 1 || epoch != currentEpoch
            || !nonempty(json.value("epoch_generation"))) {
            self->retry("retrying");
            return;
        }
        self->m_epoch = epoch;
        self->m_epochGeneration = json.value("epoch_generation").toString();
        self->m_waitingSnapshot = true;
        self->setState("snapshot_required");
        emit self->snapshotRequired();
    });
}

void RealtimeClient::acknowledgeSnapshot()
{
    if (!current(m_run, m_generation) || !m_waitingSnapshot)
        return;
    m_waitingSnapshot = false;
    m_hydratedOwners.insert(m_owner);
    m_cursors.insert(m_owner, {m_epochGeneration, m_epoch});
    scheduleCursorPersist();
    catchUp();
}

void RealtimeClient::retrySnapshot()
{
    if (m_active && m_waitingSnapshot)
        emit snapshotRequired();
}

void RealtimeClient::resynchronize()
{
    if (!m_wanted || !m_session->ready())
        return;
    m_cursors.remove(m_owner);
    // A resynchronization invalidates the catalog basis too: the fresh
    // baseline after it must re-ask for a snapshot in this process.
    m_hydratedOwners.remove(m_owner);
    // A pruned cursor must not survive either: discard the pending durable
    // write and delete the row before re-baselining, so a restart between
    // here and the next snapshot ack cannot resurrect the pruned position.
    // The post-resync cursor is persisted only after the snapshot or the
    // first event after it is acknowledged.
    discardCursorPersist();
    clearDurableCursor(m_durableKey);
    suspend();
    resume();
}

void RealtimeClient::reconnectNow(const QString &reason)
{
    Q_UNUSED(reason);
    if (!m_wanted)
        return;
    // Resume path: re-establish the link with no backoff delay, keeping the
    // saved cursors so catch-up resumes where it left off. This is the native
    // counterpart of the reference manager.reconnectAll("power_resume").
    suspend();
    resume();
}

void RealtimeClient::cursorExpired() { resynchronize(); }

void RealtimeClient::nudge()
{
    if (!current(m_run, m_generation) || m_waitingSnapshot || m_epochGeneration.isEmpty())
        return;
    if (m_fetching || !m_pending.isEmpty() || m_reconnect.isActive()) {
        m_nudged = true;
        return;
    }
    m_nudged = false;
    m_markers.clear();
    catchUp();
}

bool RealtimeClient::validEvent(const QJsonObject &event) const
{
    quint64 epoch = 0, schema = 0;
    return epochValue(event.value("schema_version"), &schema) && schema >= 1
        && epochValue(event.value("epoch_version"), &epoch) && epoch > 0
        && isUuid(event.value("uuid").toString())
        && event.value("project_id").toString() == m_session->project()
        && event.value("user_uuid").toString() == m_session->user()
        && nonempty(event.value("object_type")) && nonempty(event.value("action"))
        && event.value("payload").isObject() && nonempty(event.value("payload").toObject().value("kind"))
        && QDateTime::fromString(event.value("created_at").toString(), Qt::ISODateWithMs).isValid()
        && QDateTime::fromString(event.value("updated_at").toString(), Qt::ISODateWithMs).isValid();
}

void RealtimeClient::catchUp(QByteArray marker)
{
    if (!m_active || m_waitingSnapshot)
        return;
    m_fetching = true;
    setState("catching_up");
    QUrlQuery query;
    query.addQueryItem("epoch_version>", QString::number(m_epoch));
    query.addQueryItem("epoch_generation", m_epochGeneration);
    query.addQueryItem("page_limit", "100");
    if (!marker.isEmpty())
        query.addQueryItem("page_marker", QString::fromLatin1(marker));
    const auto run = m_run, generation = m_generation;
    QPointer<RealtimeClient> self(this);
    m_api->request("GET", "/api/workspace/v1/events/", {}, query, [self, run, generation](const ApiResult &result) {
        if (!self || !self->current(run, generation))
            return;
        if (result.status == 410 && expiredCursor(result.data.toObject())) {
            self->cursorExpired();
            return;
        }
        if (result.status == 401) {
            self->authenticationFailure();
            return;
        }
        if (!result.ok() || !result.data.isArray() || result.data.toArray().size() > 100) {
            self->retry("retrying");
            return;
        }
        QVector<QJsonObject> events;
        for (const auto &value : result.data.toArray()) {
            if (!value.isObject() || !self->validEvent(value.toObject())) {
                self->retry("invalid_data");
                return;
            }
            events.append(value.toObject());
        }
        std::sort(events.begin(), events.end(), [](const QJsonObject &a, const QJsonObject &b) {
            return a.value("epoch_version").toInteger() < b.value("epoch_version").toInteger();
        });
        quint64 previous = 0;
        QString previousUuid;
        for (const auto &event : events) {
            const auto epoch = quint64(event.value("epoch_version").toInteger());
            const auto uuid = event.value("uuid").toString();
            if (epoch == previous) {
                if (uuid != previousUuid) { self->retry("invalid_data"); return; }
                continue;
            }
            previous = epoch;
            previousUuid = uuid;
            if (epoch > self->m_epoch)
                self->m_events.enqueue(event);
        }
        if (!result.nextMarker.isEmpty()) {
            bool valid = false;
            const auto markerEpoch = result.nextMarker.toULongLong(&valid);
            if (!valid || markerEpoch != previous || markerEpoch <= self->m_epoch
                || self->m_markers.contains(result.nextMarker)) {
                self->retry("invalid_data");
                return;
            }
            self->m_markers.insert(result.nextMarker);
            if (self->m_markers.size() > 4096)
                self->m_markers.clear();
        }
        self->m_nextMarker = result.nextMarker;
        self->dispatch();
    });
}

void RealtimeClient::dispatch()
{
    if (!m_active || !m_pending.isEmpty() || m_waitingSnapshot)
        return;
    if (!m_events.isEmpty()) {
        m_pending = m_events.dequeue();
        // A synchronous consumer may acknowledge and clear m_pending before the next
        // subscriber sees this signal. Keep this delivery stable for all subscribers.
        const auto envelope = m_pending;
        // Issue #668: traffic accounting keys on the envelope type when the
        // frame carries one (error frames such as EventsCursorExpiredError),
        // else on object_type.action for regular events.
        QString eventType = envelope.value(QLatin1String("type")).toString();
        if (eventType.isEmpty()) {
            eventType = envelope.value(QLatin1String("object_type")).toString();
            const auto action = envelope.value(QLatin1String("action")).toString();
            if (!action.isEmpty()) {
                if (!eventType.isEmpty())
                    eventType += QLatin1Char('.');
                eventType += action;
            }
        }
        if (eventType.isEmpty())
            eventType = QStringLiteral("unknown");
        m_activity.recordReceived(eventType, QDateTime::currentMSecsSinceEpoch());
        emit eventReceived(envelope);
        return;
    }
    if (!m_nextMarker.isEmpty()) {
        const auto marker = m_nextMarker;
        m_nextMarker.clear();
        catchUp(marker);
        return;
    }
    m_fetching = false;
    if (m_socketEpoch > m_epoch) {
        retry("retrying");
        return;
    }
    if (m_nudged) {
        m_nudged = false;
        nudge();
        return;
    }
    connectSocket();
}

bool RealtimeClient::acknowledge(const QString &eventUuid)
{
    if (!current(m_run, m_generation) || m_pending.isEmpty()
        || m_pending.value("uuid").toString() != eventUuid)
        return false;
    m_epoch = quint64(m_pending.value("epoch_version").toInteger());
    m_cursors.insert(m_owner, {m_epochGeneration, m_epoch});
    scheduleCursorPersist();
    m_pending = {};
    const auto run = m_run, generation = m_generation;
    QTimer::singleShot(0, this, [this, run, generation] {
        if (current(run, generation))
            dispatch();
    });
    return true;
}

void RealtimeClient::scheduleCursorPersist()
{
    if (!m_cursorStore || m_durableKey.isEmpty() || m_epochGeneration.isEmpty())
        return;
    m_cursorDirty = true;
    if (!m_cursorFlush.isActive())
        m_cursorFlush.start(CursorPersistDelayMs);
}

void RealtimeClient::flushCursorPersist()
{
    m_cursorFlush.stop();
    retryDurableClear();
    if (!m_cursorDirty)
        return;
    if (!m_cursorStore || m_durableKey.isEmpty() || m_epochGeneration.isEmpty()) {
        m_cursorDirty = false;
        return;
    }
    if (m_cursorStore->save(m_durableKey, {m_epochGeneration, m_epoch})) {
        bool replacementLanded = !m_pendingClearKeys.contains(m_durableKey);
        if (!replacementLanded) {
            // save() also returns true for a same-generation monotonic
            // no-op, where the invalidated row survives untouched. Only
            // release the fence once the key verifiably holds the
            // replacement cursor; until then the delete keeps retrying
            // (finding 4585327330). The replacement also stays dirty: a
            // no-op save wrote nothing, so dropping it here would let the
            // later successful delete remove the stale row while the
            // replacement never reaches disk (finding 4585693719). The
            // next flush, after the delete lands, writes it to the
            // now-empty key.
            const auto written = m_cursorStore->read(m_durableKey);
            if (written.has_value() && written->generation == m_epochGeneration
                && written->epoch == m_epoch) {
                m_pendingClearKeys.remove(m_durableKey);
                replacementLanded = true;
            }
        }
        m_cursorDirty = !replacementLanded;
        if (m_cursorDirty && m_active)
            m_cursorFlush.start(CursorPersistDelayMs);
    } else if (m_active) {
        // A transient save failure must not drop the pending cursor until
        // the next event: keep it dirty and retry while this owner is still
        // current (finding 4584028540).
        m_cursorFlush.start(CursorPersistDelayMs);
    }
}

bool RealtimeClient::clearDurableCursor(const QString &key)
{
    if (!m_cursorStore || key.isEmpty())
        return true;
    if (m_cursorStore->clear(key)) {
        m_pendingClearKeys.remove(key);
        return true;
    }
    // The invalidated row stays readable: fence it out of restore and keep
    // the delete pending until it lands (findings 4584028530/4584028572).
    // Each owed owner's delete is tracked on its own so a second failed
    // clear cannot overwrite the first one's retry (finding 4585327340).
    m_pendingClearKeys.insert(key);
    return false;
}

void RealtimeClient::retryDurableClear()
{
    if (m_pendingClearKeys.isEmpty() || !m_cursorStore)
        return;
    const auto owed = m_pendingClearKeys;
    for (const auto &key : owed) {
        if (m_cursorStore->clear(key))
            m_pendingClearKeys.remove(key);
    }
    if (m_active && !m_pendingClearKeys.isEmpty() && !m_cursorFlush.isActive())
        m_cursorFlush.start(CursorPersistDelayMs);
}

void RealtimeClient::discardCursorPersist()
{
    m_cursorFlush.stop();
    m_cursorDirty = false;
}

void RealtimeClient::connectSocket()
{
    if (!m_active || m_waitingSnapshot)
        return;
    if (m_socket) {
        if (m_socketReady)
            setState("live");
        return;
    }
    auto *socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    m_socket = socket;
    socket->setMaxAllowedIncomingFrameSize(MaxFrameBytes);
    socket->setMaxAllowedIncomingMessageSize(MaxFrameBytes);
    socket->setReadBufferSize(MaxFrameBytes);
    const auto run = m_run, generation = m_generation;
    connect(socket, &QWebSocket::textMessageReceived, this, [this, socket, run, generation](const QString &frame) {
        if (current(run, generation) && m_socket == socket)
            socketFrame(frame);
    });
    connect(socket, &QWebSocket::binaryMessageReceived, this, [this, run, generation] {
        if (current(run, generation))
            retry("invalid_data");
    });
    connect(socket, &QWebSocket::connected, this, [this, socket, run, generation] {
        if (!current(run, generation))
            return;
        if (socket->subprotocol() != "workspace.events.v1")
            retry("invalid_data");
    });
    connect(socket, &QWebSocket::disconnected, this, [this, socket, run, generation] {
        if (!current(run, generation) || m_socket != socket)
            return;
        // Issue #668: record the drop with its real close code before the
        // semantic branch; 4401/4410 are failures the user must be able to
        // see in diagnostics just as much as plain transport drops.
        m_activity.recordFailure(QDateTime::currentMSecsSinceEpoch(),
            QStringLiteral("socket_closed"), int(socket->closeCode()));
        if (int(socket->closeCode()) == 4401) {
            authenticationFailure();
        } else if (int(socket->closeCode()) == 4410) {
            cursorExpired();
        } else {
            retry("retrying");
        }
    });
    connect(socket, &QWebSocket::errorOccurred, this, [this, run, generation](QAbstractSocket::SocketError error) {
        if (current(run, generation)) {
            // Only the enum value is recorded: errorString() can echo the
            // request URL and must never reach a diagnostics surface.
            m_activity.recordFailure(QDateTime::currentMSecsSinceEpoch(),
                QStringLiteral("socket_error"), int(error));
            retry("retrying");
        }
    });
    QUrlQuery cursor;
    cursor.addQueryItem("last_epoch_version", QString::number(m_epoch));
    cursor.addQueryItem("epoch_generation", m_epochGeneration);
    setState("connecting");
    m_handshake.start();
    if (!m_api->openRealtimeSocket(socket, cursor))
        retry("authentication_required");
}

void RealtimeClient::socketFrame(const QString &frame)
{
    if (frame.toUtf8().size() > MaxFrameBytes) { retry("invalid_data"); return; }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(frame.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        retry("invalid_data");
        return;
    }
    const auto json = document.object();
    if (expiredCursor(json)) { cursorExpired(); return; }
    if (json.value("type") == "ready") {
        quint64 epoch = 0;
        if (!epochValue(json.value("epoch_version"), &epoch)
            || json.value("epoch_generation").toString() != m_epochGeneration) {
            cursorExpired();
            return;
        }
        m_socketReady = true;
        m_retries = 0;
        m_refreshRequested = false;
        m_handshake.stop();
        m_probe.start();
        if (epoch > m_epoch)
            nudge();
        else if (!m_fetching)
            setState("live");
        return;
    }
    if (!validEvent(json)) { retry("invalid_data"); return; }
    const auto epoch = quint64(json.value("epoch_version").toInteger());
    if (epoch <= m_epoch)
        return;
    m_socketEpoch = std::max(m_socketEpoch, epoch);
    nudge();
}

void RealtimeClient::retry(const QString &state)
{
    if (!m_active)
        return;
    ++m_run;
    m_fetching = false;
    m_events.clear();
    m_pending = {};
    m_nextMarker.clear();
    m_markers.clear();
    m_nudged = false;
    m_handshake.stop();
    m_probe.stop();
    if (m_socket) {
        disconnect(m_socket, nullptr, this, nullptr);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    m_socketReady = false;
    setState(state);
    if (!m_reconnect.isActive()) {
        const auto delay = std::min(30000U, 1000U << std::min(m_retries, 5U));
        ++m_retries;
        m_reconnect.start(int(delay));
    }
}

void RealtimeClient::authenticationFailure()
{
    if (m_refreshRequested) {
        retry("authentication_required");
        return;
    }
    m_refreshRequested = true;
    suspend();
    setState("authentication_required");
    m_session->requestRefresh();
}

QVariantMap RealtimeClient::websocketSummary() const
{
    auto summary = m_activity.summary(QDateTime::currentMSecsSinceEpoch());
    summary.insert(QStringLiteral("state"),
        m_state.isEmpty() ? QVariant(QStringLiteral("stopped")) : QVariant(m_state));
    // The endpoint is printed without query parameters; the bearer token
    // travels in the subprotocol (api-client.cpp openRealtimeSocket), never
    // in the URL, so the printed endpoint is credential-free by construction.
    auto endpoint = m_api->server();
    endpoint.setScheme(endpoint.scheme() == QLatin1String("https")
        ? QStringLiteral("wss") : QStringLiteral("ws"));
    endpoint.setPath(QStringLiteral("/api/workspace/v1/events/ws"));
    summary.insert(QStringLiteral("endpoint"), endpoint.toString(QUrl::RemoveQuery));
    return summary;
}

} // namespace workspace
