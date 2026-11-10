#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QVector>
#include <QVariantList>
#include <optional>

namespace workspace {

bool isUuid(const QString &value);
// ECMAScript String.trim whitespace, shared with existing Source name semantics.
QString sourceTrimmed(const QString &value);
// Display reference only: no URL/credential, no IO, no authority capability.
QString imageAvatarReference(const QJsonValue &value);

struct Project {
    QString uuid;
    QString name;
};

struct StreamCryptoMetadata {
    QString owner;
    QString viewer;
    QString source;
    QString currentKey;
    QString defaultTopic;
    QString provider;
    QString providerIdentity;
};

// Encoded wire data is never ordinary visible Message.content. The existing
// Message.kind discriminates the required fields; decoding belongs to crypto.
struct MessageCryptoPayload {
    QString keyUuid;
    QString encodedContent;
    QString deviceId;
    QString deviceName;
    QString encodedPublicKey;
    QString requestMessage;
    QString senderDevice;
    QString recipientDevice;
};

struct Stream {
    QString uuid;
    QString name;
    QString description;
    QString directUser;
    QString role;
    qint64 unread = 0;
    bool encrypted = false;
    bool archived = false;
    qint64 activeUnread = 0;
    qint64 passiveUnread = 0;
    QString lastMessage;
    QString notificationMode;
    bool privateStream = false;
    qint64 updatedMicros = 0;
    std::optional<StreamCryptoMetadata> security;
    std::optional<quint32> color{};
};

struct Topic {
    QString uuid;
    QString stream;
    QString name;
    qint64 unread = 0;
    bool done = false;
    bool isDefault = false;
    qint64 activeUnread = 0;
    qint64 passiveUnread = 0;
    QString lastMessage;
    QString notificationMode;
    qint64 updatedMicros = 0;
};

struct Message {
    QString uuid;
    QString project;
    QString stream;
    QString topic;
    QString author;
    QString kind;
    QString content;
    QDateTime created;
    qint64 createdMicros = 0;
    bool own = false;
    bool read = false;
    bool starred = false;
    // The server flags a mention of the current user on the message DTO itself;
    // the client never inspects mention entities (workspace_ui
    // activity-realtime-applier.lib.ts:113-127). Absent means unknown.
    std::optional<bool> mentioned;
    QMap<QString, qint64> reactions;
    QMap<QString, QStringList> reactionUsers;
    bool reactionsKnown = false;
    std::optional<MessageCryptoPayload> crypto;
};

struct ReactionFact {
    QString uuid;
    QString message;
    QString user;
    QString emoji;
};

// Server-owned ordinary Markdown draft, scoped to the verified project/user.
// Local editor state and pending synchronization never enter this wire DTO.
struct Draft {
    QString uuid, project, user, stream, topic, content;
    qint64 revision = 0;
    QByteArray etag;
    QString createdAt, updatedAt;
    qint64 updatedMicros = 0;
};

std::optional<Draft> parseDraft(const QJsonObject &json, const QString &project,
                               const QString &user, const QByteArray &etag = {});

bool validReactionName(const QString &name);
QString canonicalReactionName(const QString &input);
QString reactionUnicode(const QString &name);
QVariantList knownReactionEmojis();
std::optional<ReactionFact> parseReactionFact(const QJsonObject &json, const QString &project);

std::optional<Stream> parseStream(const QJsonObject &json, const QString &project);
std::optional<Topic> parseTopic(const QJsonObject &json, const QString &project,
                              const QString &stream);
std::optional<Message> parseMessage(const QJsonObject &json, const QString &project);
QVector<Message> mergeMessages(const QVector<Message> &existing,
                              const QVector<Message> &incoming);

}
