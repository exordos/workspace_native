#pragma once

#include "api-client.h"
#include "session.h"

#include <QJsonObject>
#include <QPointer>
#include <QQueue>
#include <QSet>
#include <QTimer>

class QWebSocket;

namespace workspace {

class RealtimeCursorStore;

class RealtimeClient final : public QObject {
    Q_OBJECT
public:
    explicit RealtimeClient(ApiClient *api, Session *session, QObject *parent = nullptr);
    ~RealtimeClient() override;
    // Attaches the durable cursor store (issue #15). The store must outlive
    // this client. Without it cursors stay memory-only, as before the issue.
    void useCursorStore(RealtimeCursorStore *store) { m_cursorStore = store; }
    void start();
    void stop();
    bool acknowledge(const QString &eventUuid);
    void acknowledgeSnapshot();
    void retrySnapshot();
    void resynchronize();
    void reconnectNow(const QString &reason);
    void nudge();
    quint64 acknowledgedEpoch() const { return m_epoch; }
    QString epochGeneration() const { return m_epochGeneration; }

signals:
    void eventReceived(const QJsonObject &envelope);
    void snapshotRequired();
    void connectionChanged(const QString &state);

private:
    void resume();
    void suspend();
    void baseline();
    void catchUp(QByteArray marker = {});
    void dispatch();
    void connectSocket();
    void socketFrame(const QString &frame);
    void cursorExpired();
    void authenticationFailure();
    void retry(const QString &state);
    void setState(const QString &state);
    bool current(quint64 run, quint64 generation) const;
    bool validEvent(const QJsonObject &event) const;
    void scheduleCursorPersist();
    void flushCursorPersist();
    void discardCursorPersist();
    // Deletes a durable row, keeping a failed delete pending for retry and
    // fencing the invalidated key out of restore until the row is gone.
    bool clearDurableCursor(const QString &key);
    void retryDurableClear();

    ApiClient *m_api;
    Session *m_session;
    RealtimeCursorStore *m_cursorStore = nullptr;
    QPointer<QWebSocket> m_socket;
    QTimer m_reconnect;
    QTimer m_probe;
    QTimer m_handshake;
    QTimer m_cursorFlush;
    QHash<QString, QPair<QString, quint64>> m_cursors;
    // Owners whose snapshot this live process has acknowledged (#376). The
    // snapshot requirement is application-model hydration state, not
    // transport state: suspend() clears the transient m_waitingSnapshot
    // gate, so without this marker a reconnect during an unacknowledged
    // restore would treat the restored in-memory cursor as already
    // hydrated and start catch-up ahead of the catalogs (findings
    // 4599009714/4599009723). Only acknowledgeSnapshot() sets it, and only
    // the places that discard the owner's cursor clear it.
    QSet<QString> m_hydratedOwners;
    QString m_durableKey;
    // Durable keys whose delete has not landed yet: an invalidated row under
    // a pending key must not be restored, and every delete keeps being
    // retried independently until it lands (finding 4585327340).
    QSet<QString> m_pendingClearKeys;
    bool m_cursorDirty = false;
    QQueue<QJsonObject> m_events;
    QJsonObject m_pending;
    QSet<QByteArray> m_markers;
    QString m_owner;
    QString m_epochGeneration;
    QString m_state;
    QByteArray m_nextMarker;
    quint64 m_epoch = 0;
    quint64 m_run = 0;
    quint64 m_generation = 0;
    quint64 m_socketEpoch = 0;
    unsigned int m_retries = 0;
    bool m_wanted = false;
    bool m_active = false;
    bool m_fetching = false;
    bool m_waitingSnapshot = false;
    bool m_nudged = false;
    bool m_socketReady = false;
    bool m_refreshRequested = false;
};

} // namespace workspace
