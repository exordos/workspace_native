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
