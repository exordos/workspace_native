#pragma once

#include "native-test-harness.h"

// Recording fake for the desktop-notification transport (issue #3). It tracks
// "one active notification per tag" the same way the Electron shell does: a
// new show for an active tag replaces the previous holder.
class RecordingNotifications final : public workspace::NotificationTransport {
public:
    struct Show { QString tag, title, body; };
    QList<Show> shows;
    QStringList closed;
    int attentionRequests = 0;
    int attentionClears = 0;

    bool show(const QString &tag, const QString &title, const QString &body) override
    {
        shows.push_back({tag, title, body});
        return true;
    }
    void closeByTag(const QString &tag) override { closed << tag; }
    void requestAttention() override { ++attentionRequests; }
    void clearAttention() override { ++attentionClears; }
    QString lastTag() const { return shows.isEmpty() ? QString{} : shows.last().tag; }
};

// Minimal session-bus stand-in for org.freedesktop.Notifications so the wire
// signature of Notify/CloseNotification is verifiable without a real daemon.
class FakeFreedesktopNotifications final : public QObject
{
    Q_OBJECT
public:
    struct NotifyCall
    {
        QString app;
        uint replacesId = 0;
        QString icon;
        QString summary;
        QString body;
        QStringList actions;
        QVariantMap hints;
        int timeout = 0;
    };
    QList<NotifyCall> notifyCalls;
    QList<uint> closedIds;
};

class FakeFreedesktopNotificationsAdaptor final : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")
public:
    explicit FakeFreedesktopNotificationsAdaptor(FakeFreedesktopNotifications *owner)
        : QDBusAbstractAdaptor(owner), m_owner(owner) {}

public slots:
    uint Notify(const QString &app, uint replacesId, const QString &icon,
        const QString &summary, const QString &body, const QStringList &actions,
        const QVariantMap &hints, int timeout)
    {
        m_owner->notifyCalls.push_back(
            {app, replacesId, icon, summary, body, actions, hints, timeout});
        return 100u + quint32(m_owner->notifyCalls.size());
    }
    void CloseNotification(uint id) { m_owner->closedIds.append(id); }

private:
    FakeFreedesktopNotifications *m_owner;
};

inline QJsonObject notificationStreamDto(const QString &mode = QStringLiteral("all_messages"))
{
    return {{QStringLiteral("uuid"), streamId}, {QStringLiteral("project_id"), projectId},
            {QStringLiteral("name"), QStringLiteral("Native channel")},
            {QStringLiteral("encryption"), false}, {QStringLiteral("notification_mode"), mode}};
}

inline QJsonObject notificationTopicDto(const QString &id = topicId,
                                        const QString &name = QStringLiteral("Native topic"),
                                        const QString &mode = QStringLiteral("default"))
{
    return {{QStringLiteral("uuid"), id}, {QStringLiteral("project_id"), projectId},
            {QStringLiteral("stream_uuid"), streamId}, {QStringLiteral("name"), name},
            {QStringLiteral("notification_mode"), mode}};
}

// Marks a history DTO as a foreign unread arrival (notification candidate).
inline QJsonObject incomingDto(QJsonObject row, const QString &author)
{
    row[QStringLiteral("read")] = false;
    row[QStringLiteral("is_own")] = false;
    row[QStringLiteral("author_uuid")] = author;
    return row;
}

inline const QString notificationAuthor = QStringLiteral("55555555-5555-4555-8555-555555555555");
