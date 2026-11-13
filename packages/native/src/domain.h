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

// Issue #214: wire-shaped poll snapshots mirroring the reference domain types
// (messenger.types.ts MessengerPollPayload/MessengerPollState). Empty
// closesAt/closedAt strings mean null. parseMessage fills them only for kind
// "poll", and only after the full reference guards accepted both objects.
struct PollOption {
    QString uuid;
    QString text;
};

struct PollQuestion {
    QString uuid;
    QString text;
    QVector<PollOption> options;
    QStringList correctOptionUuids;
};

struct PollPayload {
    QVector<PollQuestion> questions;
    bool multipleAnswers = false;
    bool isAnonymous = false;
    bool allowRevote = false;
    QString resultsVisibility; // "always" | "after_vote"
    bool allowAddingOptions = false;
    QString closesAt;
    QString closedAt;
};

struct PollQuestionState {
    QString questionUuid;
    qint64 voterCount = 0;
    QMap<QString, qint64> optionCounts;
    QMap<QString, QStringList> optionVoterPreviews;
    bool hasVoterPreviews = false;
};

struct PollMyVote {
    QString questionUuid;
    QStringList optionUuids;
};

struct PollState {
    qint64 revision = 0;
    bool isClosed = false;
    qint64 voterCount = 0;
    QVector<PollQuestionState> questions;
    QVector<PollMyVote> myVotes;
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
    // Issue #134: the stream DTO's optional default_topic_uuid, kept separate
    // from the crypto security witness. The profile Message action opens the
    // direct stream's default topic exactly as the reference resolver does.
    QString defaultTopic;
};

// Issue #134 profile Message/Call target resolution, a port of
// right-panel-user-profile-direct-navigation.lib.ts: already on this direct
// stream (any topic) -> stay; otherwise open the stream's default topic; a
// known stream without a valid default topic resolves as missing-default;
// no direct stream for the peer resolves as stream-missing.
struct ProfileDirectNavigation {
    enum class Status { AlreadyOpen, OpenDefaultTopic, MissingDefaultTopic, StreamMissing };
    Status status = Status::StreamMissing;
    QString streamUuid;
    QString topicUuid;
};
ProfileDirectNavigation resolveProfileDirectNavigation(
    const QString &directUserUuid,
    const QVector<QPair<QString, QString>> &directStreams,
    const QHash<QString, QString> &defaultTopics,
    const QString &currentStreamUuid);

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
    // Issue #214: present only for kind "poll". A poll DTO is accepted only
    // with a valid poll_state; other kinds must carry poll_state == null.
    std::optional<PollPayload> poll;
    std::optional<PollState> pollState;
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

// Issue #214 store merge (message.model.ts snapshot rule): a lower poll_state
// revision never overwrites - the incoming row keeps the previous payload and
// state and the call returns true so the caller can also skip cache payload
// projection. At equal revision an existing is_closed is never reverted; when
// that reopen ratchet applies, *reopenRatcheted (if requested) is set so the
// caller can project the guarded closed state instead of the incoming one.
bool applyPollRevisionGuard(Message &incoming, const Message &previous,
                            bool *reopenRatcheted = nullptr);

// Composer mention entries for the send body (message-composer-body.lib.ts
// WorkspaceComposerMention). visibleText is what the user sees in the draft;
// ambiguous display names disambiguate with the username before serialization.
struct ComposerMention {
    QString userUuid;
    QString displayName;
    QString visibleText;
};

// Converts visible "@DisplayName" texts to the canonical Workspace mention URNs
// at send time (message-composer-body.lib.ts serializeWorkspaceComposerMentions):
// email addresses and ambiguous bare names stay literal.
QString serializeComposerMentions(const QString &value, const QVector<ComposerMention> &mentions);

}
