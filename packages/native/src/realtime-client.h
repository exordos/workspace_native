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

class RealtimeClient final : public QObject {
    Q_OBJECT
public:
    explicit RealtimeClient(ApiClient *api, Session *session, QObject *parent = nullptr);
    ~RealtimeClient() override;
    void start();
    void stop();
    bool acknowledge(const QString &eventUuid);
    void acknowledgeSnapshot();
    void retrySnapshot();
    void resynchronize();
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

    ApiClient *m_api;
    Session *m_session;
    QPointer<QWebSocket> m_socket;
    QTimer m_reconnect;
    QTimer m_probe;
    QTimer m_handshake;
    QHash<QString, QPair<QString, quint64>> m_cursors;
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
