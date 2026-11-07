#include "realtime-client.h"

#include "domain.h"

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
        m_cursors.clear();
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
    if (owner != m_owner)
        m_refreshRequested = false;
    m_owner = owner;
    m_generation = m_api->generation();
    m_active = true;
    m_retries = 0;
    const auto saved = m_cursors.constFind(owner);
    if (saved == m_cursors.cend()) {
        m_epochGeneration.clear();
        m_epoch = 0;
        baseline();
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
    m_cursors.insert(m_owner, {m_epochGeneration, m_epoch});
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
        emit eventReceived(m_pending);
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
    m_pending = {};
    const auto run = m_run, generation = m_generation;
    QTimer::singleShot(0, this, [this, run, generation] {
        if (current(run, generation))
            dispatch();
    });
    return true;
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
        if (int(socket->closeCode()) == 4401) {
            authenticationFailure();
        } else if (int(socket->closeCode()) == 4410) {
            cursorExpired();
        } else {
            retry("retrying");
        }
    });
    connect(socket, &QWebSocket::errorOccurred, this, [this, run, generation] {
        if (current(run, generation))
            retry("retrying");
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

} // namespace workspace
