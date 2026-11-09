#pragma once

#include "api-client.h"
#include "domain.h"
#include "native-cache-service.h"
#include "session.h"
#include "native-chat-key-vault.h"
#include "native-drafts.h"

#include <QHash>
#include <QSet>
#include <QQueue>
#include <QTimer>
#include <QVariantList>

namespace workspace {

class NativePreferences;
class RealtimeClient;
class FileClient;
class NativeChatKeyVault;
struct FileResult;

class NativeController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool authenticated READ authenticated NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(QVariantList projects READ projects NOTIFY stateChanged)
    Q_PROPERTY(QVariantList savedAccounts READ savedAccounts NOTIFY savedAccountsChanged)
    Q_PROPERTY(QVariantMap account READ account NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap ownStatus READ ownStatus NOTIFY stateChanged)
    Q_PROPERTY(QString ownStatusContextKey READ ownStatusContextKey NOTIFY stateChanged)
    Q_PROPERTY(QString viewerDisplayName READ viewerDisplayName NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap encryption READ encryption NOTIFY stateChanged)
    Q_PROPERTY(QVariantList directChatChoices READ directChatChoices NOTIFY stateChanged)
    Q_PROPERTY(bool directChatDirectoryVerified READ directChatDirectoryVerified NOTIFY stateChanged)
    // Like channel creation: genuine lifetime loss, never ordinary row counters
    // or directory display metadata. The form's attempt is not a server UUID.
    Q_PROPERTY(QString directChatContextKey READ directChatContextKey NOTIFY stateChanged)
    Q_PROPERTY(QString directChatPhase READ directChatPhase NOTIFY stateChanged)
    Q_PROPERTY(bool directChatBusy READ directChatBusy NOTIFY stateChanged)
    Q_PROPERTY(bool directChatUncertain READ directChatUncertain NOTIFY stateChanged)
    Q_PROPERTY(QString directChatError READ directChatError NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap directChatForm READ directChatForm NOTIFY stateChanged)
    Q_PROPERTY(bool canCheckDirectChat READ canCheckDirectChat NOTIFY stateChanged)
    Q_PROPERTY(bool canRetryDirectChat READ canRetryDirectChat NOTIFY stateChanged)
    Q_PROPERTY(bool isDirectConversation READ isDirectConversation NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap directPeer READ directPeer NOTIFY stateChanged)
    Q_PROPERTY(bool canCreateChannel READ canCreateChannel NOTIFY stateChanged)
    Q_PROPERTY(QVariantList channelMemberChoices READ channelMemberChoices NOTIFY stateChanged)
    Q_PROPERTY(bool channelMembersVerified READ channelMembersVerified NOTIFY stateChanged)
    // Ownership/API/route/cache/trust lifetime only. Ordinary row counters and
    // directory display updates do not change this key or discard a form.
    Q_PROPERTY(QString channelCreationContextKey READ channelCreationContextKey NOTIFY stateChanged)
    Q_PROPERTY(QString channelCreationPhase READ channelCreationPhase NOTIFY stateChanged)
    Q_PROPERTY(bool channelCreationBusy READ channelCreationBusy NOTIFY stateChanged)
    Q_PROPERTY(bool channelCreationUncertain READ channelCreationUncertain NOTIFY stateChanged)
    Q_PROPERTY(QString channelCreationError READ channelCreationError NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap channelCreationForm READ channelCreationForm NOTIFY stateChanged)
    Q_PROPERTY(bool canCheckChannelCreation READ canCheckChannelCreation NOTIFY stateChanged)
    Q_PROPERTY(bool canRetryChannelMembers READ canRetryChannelMembers NOTIFY stateChanged)
    Q_PROPERTY(bool topicCreationBusy READ topicCreationBusy NOTIFY stateChanged)
    Q_PROPERTY(bool topicCreationUncertain READ topicCreationUncertain NOTIFY stateChanged)
    Q_PROPERTY(bool canCheckTopicCreation READ canCheckTopicCreation NOTIFY stateChanged)
    Q_PROPERTY(QString topicCreationError READ topicCreationError NOTIFY stateChanged)
    Q_PROPERTY(QString topicCreationStream READ topicCreationStream NOTIFY stateChanged)
    Q_PROPERTY(QString topicCreationName READ topicCreationName NOTIFY stateChanged)
    Q_PROPERTY(QString topicCreationAttemptId READ topicCreationAttemptId NOTIFY stateChanged)
    // Current verified shell lifetime; target ACL has its own streams[].contextKey.
    // Ordinary row metadata and completed intent revisions do not change this key.
    Q_PROPERTY(QString topicCreationContextKey READ topicCreationContextKey NOTIFY stateChanged)
    Q_PROPERTY(QVariantList archivedChats READ archivedChats NOTIFY archivedChatsChanged)
    Q_PROPERTY(bool archiveLoading READ archiveLoading NOTIFY stateChanged)
    Q_PROPERTY(QString archiveError READ archiveError NOTIFY stateChanged)
    Q_PROPERTY(QVariantList streams READ streams NOTIFY streamsChanged)
    Q_PROPERTY(QVariantList topics READ topics NOTIFY topicsChanged)
    Q_PROPERTY(bool topicResolving READ topicResolving NOTIFY stateChanged)
    Q_PROPERTY(bool topicsComplete READ topicsComplete NOTIFY stateChanged)
    Q_PROPERTY(QVariantList messages READ messages NOTIFY messagesChanged)
    Q_PROPERTY(qulonglong historyCommitRevision READ historyCommitRevision NOTIFY messagesChanged)
    Q_PROPERTY(bool messageHistoryReady READ messageHistoryReady NOTIFY stateChanged)
    Q_PROPERTY(QVariantList reactionChoices READ reactionChoices CONSTANT)
    Q_PROPERTY(QString conversationKind READ conversationKind NOTIFY stateChanged)
    Q_PROPERTY(bool hasConversation READ hasConversation NOTIFY stateChanged)
    Q_PROPERTY(QString selectedStream READ selectedStream NOTIFY stateChanged)
    Q_PROPERTY(QString selectedTopic READ selectedTopic NOTIFY stateChanged)
    Q_PROPERTY(QString draft READ draft WRITE setDraft NOTIFY draftChanged)
    Q_PROPERTY(bool canCompose READ canCompose NOTIFY stateChanged)
    Q_PROPERTY(bool canSend READ canSend NOTIFY stateChanged)
    Q_PROPERTY(bool canLoadOlder READ canLoadOlder NOTIFY stateChanged)
    Q_PROPERTY(QString title READ title NOTIFY stateChanged)
    Q_PROPERTY(QString syncState READ syncState NOTIFY stateChanged)
    Q_PROPERTY(QString ownerKey READ ownerKey NOTIFY stateChanged)
    Q_PROPERTY(bool canAttach READ canAttach NOTIFY stateChanged)
    Q_PROPERTY(bool topicClosed READ topicClosed NOTIFY stateChanged)
    Q_PROPERTY(bool canReopenTopic READ canReopenTopic NOTIFY stateChanged)
    Q_PROPERTY(bool fileBusy READ fileBusy NOTIFY stateChanged)
    Q_PROPERTY(QString notice READ notice NOTIFY stateChanged)
    Q_PROPERTY(bool cacheStale READ cacheStale NOTIFY stateChanged)
public:
    explicit NativeController(ApiClient *api, Session *session, QObject *parent = nullptr);
    ~NativeController() override;
    void setRealtimeMode(bool enabled) { m_realtimeMode = enabled; if (enabled) m_poll.stop(); }
    void useRealtime(RealtimeClient *client);
    void useFiles(FileClient *client);
    void useCache(NativeCacheService *cache);
    void useDrafts(NativeDrafts *drafts);
    struct DraftMemory { int entries = 0, verifiedRoutes = 0; qsizetype textBytes = 0; };
    DraftMemory draftMemory() const;
    void reportDraftFlushFailure();
    void usePreferences(NativePreferences *preferences);
    QVariantMap ownStatus() const;
    QString ownStatusContextKey() const;
    Q_INVOKABLE void saveOwnStatus(const QString &text, const QString &emoji, bool away);
    Q_INVOKABLE void cancelOwnStatusUpdate();
    void useChatKeys(NativeChatKeyVault *vault);
    QVariantMap encryption() const;
    Q_INVOKABLE void requestChatKey(const QString &keyUuid = {});
    Q_INVOKABLE void checkChatKeyRequest(const QString &keyUuid);
    Q_INVOKABLE void retryChatKeyRequest(const QString &keyUuid);
    Q_INVOKABLE void retryChatKeyStore();
    Q_INVOKABLE void grantChatKey(const QString &request);
    Q_INVOKABLE void rejectChatKey(const QString &request);
    Q_INVOKABLE void resolveKeyServiceRequest(const QString &responseId);
    Q_INVOKABLE void resolveKeyServiceRequest(const QString &responseId, quint64 disclosureSerial);
    Q_INVOKABLE void checkChatKeyResponse(const QString &request);
    Q_INVOKABLE void retryChatKeyResponse(const QString &request);
    void setChatKeyStoreOpening(bool opening);
    bool cacheStale() const;
    void reloadSnapshot();
    bool applyRealtimeEvent(const QJsonObject &envelope);
    bool authenticated() const { return m_session->ready(); }
    bool busy() const { return m_busy || m_loading || m_sending || m_snapshotActive || m_topicChanging || m_cacheForgettingAccount; }
    QString error() const { return m_error; }
    QVariantList projects() const;
    QVariantList savedAccounts() const;
    QVariantMap account() const;
    QString viewerDisplayName() const;
    QVariantList directChatChoices() const { return channelMemberChoices(); }
    bool directChatDirectoryVerified() const { return channelMembersVerified(); }
    QString directChatContextKey() const;
    QString directChatPhase() const;
    bool directChatBusy() const;
    bool directChatUncertain() const;
    QString directChatError() const;
    QVariantMap directChatForm() const;
    bool canCheckDirectChat() const;
    bool canRetryDirectChat() const;
    bool isDirectConversation() const;
    QVariantMap directPeer() const;
    bool canCreateChannel() const;
    QVariantList channelMemberChoices() const;
    bool channelMembersVerified() const;
    QString channelCreationContextKey() const;
    QString channelCreationPhase() const;
    bool channelCreationBusy() const;
    bool channelCreationUncertain() const;
    QString channelCreationError() const;
    QVariantMap channelCreationForm() const;
    bool canCheckChannelCreation() const;
    bool canRetryChannelMembers() const;
    bool topicCreationBusy() const;
    bool topicCreationUncertain() const;
    bool canCheckTopicCreation() const;
    QString topicCreationError() const;
    QString topicCreationStream() const;
    QString topicCreationName() const;
    QString topicCreationAttemptId() const;
    QString topicCreationContextKey() const;
    QVariantList archivedChats() const;
    bool archiveLoading() const;
    QString archiveError() const;
    Q_INVOKABLE bool canUnarchiveChat(const QString &id) const;
    Q_INVOKABLE void openArchivedChat(const QString &id);
    Q_INVOKABLE void unarchiveChat(const QString &id);
    QVariantList streams() const;
    QVariantList topics() const;
    bool topicResolving() const { return m_topicOpening.active; }
    bool topicsComplete() const { return m_topicsVerified; }
    QVariantList messages() const;
    qulonglong historyCommitRevision() const { return m_historyCommitRevision; }
    bool messageHistoryReady() const;
    QVariantList reactionChoices() const { return knownReactionEmojis(); }
    bool hasConversation() const { return !m_stream.isEmpty(); }
    QString conversationKind() const { return !hasConversation() ? QStringLiteral("none")
        : m_topic.isEmpty() ? QStringLiteral("stream") : QStringLiteral("topic"); }
    QString selectedStream() const { return m_stream; }
    QString selectedTopic() const { return m_topic; }
    QString draft() const;
    void setDraft(const QString &text);
    bool canCompose() const;
    bool canSend() const;
    bool canLoadOlder() const { return !m_before.isEmpty() && !m_loading && !m_snapshotActive
        && (!m_cacheProtection || m_historyVerified); }
    QString title() const;
    QString syncState() const { return m_syncState; }
    QString ownerKey() const { return authenticated()
        ? SessionVault::accountId(m_api->server(), m_session->project(), m_session->user()) : QString{}; }
    bool canAttach() const;
    bool topicClosed() const;
    bool canReopenTopic() const;
    bool fileBusy() const { return m_uploads > 0 || m_downloads > 0; }
    QString notice() const;

    Q_INVOKABLE void login(const QString &server, const QString &user,
                           const QString &password, const QString &otp);
    Q_INVOKABLE void openProject(const QString &id);
    Q_INVOKABLE void resumeSavedAccount(const QString &id);
    Q_INVOKABLE void forgetSavedAccount(const QString &id);
    Q_INVOKABLE void logout();
    Q_INVOKABLE void selectStream(const QString &id);
    Q_INVOKABLE void selectTopic(const QString &id);
    Q_INVOKABLE void openTopic(const QString &id, const QString &streamHint = {});
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void loadOlder();
    Q_INVOKABLE void reopenTopic();
    Q_INVOKABLE bool canStartDirectChatWith(const QString &peer) const;
    Q_INVOKABLE void startDirectChat(const QString &peer);
    Q_INVOKABLE void checkDirectChat();
    Q_INVOKABLE void retryDirectChat();
    Q_INVOKABLE void createChannel(const QString &name, const QString &description,
        bool inviteOnly, bool announce, const QVariantList &memberUuids);
    Q_INVOKABLE void checkChannelCreation();
    Q_INVOKABLE void retryChannelMembers();
    Q_INVOKABLE bool canCreateTopicIn(const QString &stream) const;
    Q_INVOKABLE void createTopic(const QString &stream, const QString &name);
    Q_INVOKABLE void checkTopicCreation();
    Q_INVOKABLE void send();
    Q_INVOKABLE void checkPendingSend();
    Q_INVOKABLE void retryPendingSend();
    Q_INVOKABLE void copyMessage(const QString &id);
    Q_INVOKABLE void starMessage(const QString &id);
    Q_INVOKABLE void deleteMessage(const QString &id);
    Q_INVOKABLE void editMessage(const QString &id, const QString &text);
    Q_INVOKABLE void quoteMessage(const QString &id);
    Q_INVOKABLE void markMessagesReadUpTo(const QString &id);
    Q_INVOKABLE void markVisibleMessagesRead(const QVariantList &ids);
    Q_INVOKABLE void toggleReaction(const QString &id, const QString &emoji);
    Q_INVOKABLE void attachFiles(const QVariantList &localUrls);
    Q_INVOKABLE void downloadFile(const QString &id, const QUrl &destination);

signals:
    void chatKeyStoreOpenRequested();
    void archivedChatsChanged();
    void stateChanged();
    void savedAccountsChanged();
    void draftChanged();
    void streamsChanged();
    void topicsChanged();
    void messagesChanged();
    void topicCreated(QString stream, QString topic);
    void channelCreated(QString stream, QString defaultTopic);
    void directChatOpened(QString stream, QString peer, QString defaultTopic);
    void messageSent(QString id, QString stream, QString topic);
    void reactionAdded(QString messageId, QString canonicalEmoji, QString ownerKey, QString stream, QString topic);
    void snapshotReloaded();
    void snapshotFailed();
    void resynchronizeRequested();

private:
    struct ProjectionContext;
    struct ChatAuthority {
        QString owner, stream, defaultTopic, currentKey, role, issuerOwner;
        QHash<QString, QString> roles;
        quint64 epoch = 0;
        bool ready = false;
        QSet<QString> topics;
    };
    enum class ChatKeyAvailability { Unknown, Missing, Present, Error };
    struct PreparedChatMessage {
        QByteArray hash;
        QString content;
        quint64 deletion = 0;
        ChatKeyAvailability state = ChatKeyAvailability::Present;
    };
    void resetChatRuntime(bool preserveInitialOrder = false);
    void checkChatLifetime();
    bool chatVaultMatches() const;
    bool chatReadAllowed() const;
    bool revokeChatAccess(const QString &stream);
    void restoreChatAccess(const QJsonArray &rows, quint64 revision, const QString &owner,
                          quint64 epoch, const ProjectionContext &context);
    bool chatPublicContext(quint64 epoch, quint64 selection) const;
    bool chatContext(quint64 epoch, quint64 selection) const;
    bool currentChatOperation(quint64 operation, const QString &key) const;
    using ChatAuthorityCallback = std::function<void(bool, const QString &)>;
    void ensureChatAuthority(ChatAuthorityCallback done);
    void finishChatAuthority(quint64 epoch, quint64 selection, const QString &error = {});
    void chatCollection(const QString &path, QUrlQuery query, quint64 epoch, quint64 selection,
        std::function<void(const QJsonArray &, const QString &)> done, QJsonArray rows = {}, QSet<QByteArray> markers = {},
        int pages = 0, qint64 bytes = 0);
    void finishChatOperation(quint64 operation, const QString &key, const QString &phase, const QString &error = {});
    void startChatRequest(const QString &key, bool checking, bool retrying);
    void postChatRequest(const QString &key, const NativeChatKeyVault::PendingRequest &pending, quint64 epoch, quint64 selection, quint64 operation);
    bool matchesChatRequest(const Message &message, const QString &key,
                            const NativeChatKeyVault::PendingRequest &pending, bool observed) const;
    bool bindChatRequest(const Message &message, const QString &key,
                         const NativeChatKeyVault::PendingRequest &pending, bool observed = false);
    void observeChatRequest(const Message &message);
    bool currentChatRequest(const Message &response, const NativeChatKeyVault::PendingRequest &pending) const;
    bool receiveChatControl(const Message &message);
    void updateChatPendingState(const QString &key, bool known, bool requested);
    void prepareChatAnnouncement(const Message &message);
    void drainChatResponses(const QString &request);
    void retainChatControl(const Message &message);
    enum class ChatOutcomeRetention { Clear, OrderOnly, PublicFacts };
    void resetChatControls(bool preserveDeferred = false, ChatOutcomeRetention outcomes = ChatOutcomeRetention::Clear);
    void scheduleChatControls();
    void processChatAccountRequests(quint64 epoch, quint64 selection, quint64 work);
    void processChatControls(quint64 epoch, quint64 selection, quint64 work);
    void scheduleChatPreparation();
    void prepareChatSlice(quint64 epoch, quint64 selection, quint64 preparation, qsizetype offset);
    ChatAuthority m_chatAuthority;
    NativeChatKeyVault::Lease m_chatVaultLease;
    QPointer<NativeCacheService> m_chatCache;
    NativeCache::Lease m_chatCacheLease;
    QString m_chatOwner, m_chatStream, m_chatTopic, m_chatSecurity, m_chatPhase, m_chatError, m_chatRequestKey;
    QString m_chatLocalRequest, m_chatRevocationOwner;
    quint64 m_chatEpoch = 0, m_chatApiGeneration = 0, m_chatCatalog = 0;
    quint64 m_chatOperation = 0, m_chatAccessRevision = 0;
    QSet<QString> m_chatReadableStreams;
    bool m_chatAccessKnown = false;
    bool m_chatTrust = false, m_chatBusy = false, m_chatPrepareQueued = false;
    bool m_chatKeyStoreOpening = false;
    qsizetype m_chatPreparedUnits = 0;
    quint64 m_chatPreparation = 0;
    QByteArray m_chatPreparingWindow, m_chatRejectedWindow;
    QHash<QString, PreparedChatMessage> m_chatPrepared;
    ChatKeyAvailability m_chatCurrentKeyState = ChatKeyAvailability::Unknown;
    QString m_chatSendKey;
    quint64 m_chatSendDeletion = 0;
    QVector<Message> m_chatPrepareRows;
    QHash<QString, Message> m_chatResponses;
    struct ChatRequestFact {
        QString stream, key, author, device, name, publicKey;
        qsizetype units = 0;
        enum class LookupRetry { None, Failed, Omitted, LiveDeleted };
        bool deleted = false, unresolved = false, accountMissing = false;
        LookupRetry lookupRetry = LookupRetry::None;
        qsizetype lookupUnits = 0;
        quint64 revision = 0, disclosureSerial = 0;
        QString outcome, response;
    };
    struct ChatRequestLookup {
        enum class Phase { Queued, Cache, Network, WaitingCache, LateCache };
        QString response, owner, project, user, stream, topic, security;
        QByteArray responseHash;
        QPointer<NativeCacheService> cache;
        NativeCache::Lease lease;
        ApiClient::RequestId handle;
        quint64 ticket = 0, selection = 0, generation = 0, catalog = 0, access = 0, revision = 0;
        qsizetype units = 0;
        Phase phase = Phase::Queued;
        bool awaitCache = false, savedOnly = false;
    };
    bool chatRequestLookupContext(const QString &id, const ChatRequestLookup &lookup, bool allowDeleted = false) const;
    void interruptChatRequestLookups();
    void cancelChatRequestLookup(const QString &id);
    void finishChatRequestLookup(const QString &id, quint64 ticket, bool waitForCache);
    void pumpChatRequestLookups();
    void hydrateChatRequestLookups();
    void applyResolvedChatRequest(const QString &id, quint64 ticket, const QJsonObject &raw, bool persist);
    void fetchChatRequestLookup(const QString &id, quint64 ticket);
    QHash<QString, ChatRequestLookup> m_chatRequestLookups;
    quint64 m_nextChatRequestLookup = 0;
    bool m_chatRequestLookupPumpQueued = false;
    struct ChatIssueKey {
        ChatKeyAvailability state = ChatKeyAvailability::Unknown;
        quint64 deletion = 0;
        bool pendingKnown = false, requested = false;
    };
    struct ChatOutcomeFact {
        QString id, stream, request, key, author, device, outcome;
        qsizetype units = 0;
        bool valid = false, deleted = false;
    };
    struct ChatKeyResponse {
        QString uuid, publicId, owner, project, user, stream, topic, routeTopic, key, currentKey;
        QString request, requester, deviceName, publicKey, senderDevice, recipientDevice, action, phase, error, security;
        QJsonObject payload;
        QPointer<NativeChatKeyVault> vault;
        NativeChatKeyVault::Lease vaultLease;
        QPointer<NativeCacheService> cache;
        NativeCache::Lease cacheLease;
        quint64 ticket = 0, epoch = 0, selection = 0, catalog = 0, generation = 0, access = 0;
        quint64 requestRevision = 0, deletion = 0;
        bool busy = false, dispatched = false, materialLost = false, statusOnly = false, deleted = false, holdsField = false;
    };
    bool chatIssuerAllowed() const;
    bool chatResponseAllowed(const ChatRequestFact &fact) const;
    bool chatRequestMatches(const ChatRequestFact &fact, const ChatKeyResponse &intent) const;
    QVariantList chatKeyRequestsMetadata() const;
    QVariantMap chatKeyResponseMetadata() const;
    void observeChatKeyOutcome(const Message &message);
    void rememberChatOutcomeOrder(const QVector<Message> &rows);
    void reduceChatKeyOutcomes();
    bool chatKeyResponseOwned(bool writing) const;
    bool chatKeyResponseContext(const QString &id, quint64 ticket) const;
    void captureChatKeyResponse(bool statusOnly);
    void interruptChatKeyResponse();
    void startChatKeyResponse(const QString &request, bool grant);
    void recoverChatKeyResponse(const QString &request, bool retry);
    void finishChatKeyResponse(const QString &id, quint64 ticket, const QString &error);
    void postChatKeyResponse(const QString &id, quint64 ticket);
    void readChatKeyResponse(const QString &id, quint64 ticket, bool retry);
    bool matchesChatKeyResponse(const QJsonObject &raw, bool realtime) const;
    bool confirmChatKeyResponse(const QJsonObject &raw, const QString &id, quint64 ticket, bool realtime = false);
    std::optional<ChatKeyResponse> m_chatKeyResponse;
    QHash<QString, ChatIssueKey> m_chatIssueKeys;
    quint64 m_nextChatKeyResponse = 0, m_nextChatRequestFact = 0;
    bool m_chatIssueChanged = false;
    QHash<QString, ChatRequestFact> m_chatRequestFacts;
    QVector<ChatOutcomeFact> m_chatOutcomeFacts;
    enum class ChatInitialVault { Unattached, Waiting, Complete };
    ChatInitialVault m_chatInitialVault = ChatInitialVault::Unattached;
    qsizetype m_chatRequestFactUnits = 0;
    bool m_chatRequestFactsLimited = false;
    QVector<ChatAuthorityCallback> m_chatAuthorityWaiters;
    bool m_chatAuthorityLoading = false;
    bool m_chatControlsQueued = false, m_chatControlAuthorityFailed = false;
    quint64 m_chatControlWork = 0;
    qsizetype m_chatControlOffset = 0, m_chatAccountOffset = 0;
    QByteArray m_chatControlWindow;
    QVector<Message> m_chatControlRows;
    QHash<QString, Message> m_chatDeferredControls;
    qsizetype m_chatDeferredUnits = 0;
    bool m_chatDeferredLimited = false;

    struct DirectChat {
        QString attempt, peer, uuid, phase = QStringLiteral("idle"), error;
        QString routeStream, routeTopic, contextKey, defaultTopic, acl, collectionPath;
        QJsonArray topics;
        quint64 generation = 0, selection = 0, catalog = 0, ticket = 0;
        quint64 revision = 0, unsafeRevision = 0, topicsUnsafeRevision = 0, topicRevision = 0, topicsRevision = 0, collectionStreamRevision = 0;
        int readReconciliations = 0, collectionRequests = 0;
        qint64 collectionRows = 0, collectionBytes = 0;
        QPointer<NativeCacheService> cache;
        NativeCache::Lease lease;
        bool busy = false, dispatched = false, confirmed = false, allowCreate = false, accessLost = false, publishing = false;
    };
    const DirectChat *directChat() const;
    bool directChatAllowed() const;
    void captureDirectContext(DirectChat &intent);
    bool directChatContext(const QString &owner, const QString &attempt, quint64 ticket);
    bool directPhase(const QString &owner, const QString &attempt, quint64 ticket, const QString &phase);
    void finishDirectChat(const QString &owner, const QString &attempt, quint64 ticket,
                          const QString &error, const QString &phase = QStringLiteral("uncertain"));
    void interruptDirectChats(const QString &preservePublishingAttempt = {});
    void noteDirectChatEvent(const QString &stream, const QString &topic = {}, bool deleted = false,
                             bool matchingOwn = false, bool accessLost = false);
    bool reconcileDirectRead(const QString &owner, const QString &attempt, quint64 ticket,
                             quint64 unsafeRevision, std::function<void()> retry);
    void lookupDirectChat(const QString &owner, const QString &attempt, quint64 ticket);
    void postDirectChat(const QString &owner, const QString &attempt, quint64 ticket);
    void loadDirectDefault(const QString &owner, const QString &attempt, quint64 ticket);
    void loadDirectCatalog(const QString &owner, const QString &attempt, quint64 ticket);
    void directCollection(const QString &owner, const QString &attempt, quint64 ticket,
        const QString &path, QUrlQuery query, std::function<void(const QJsonArray &)> done,
        QJsonArray rows = {}, QSet<QByteArray> markers = {});
    struct ChannelCreation {
        QString uuid, name, description, phase = QStringLiteral("idle"), error;
        QString routeStream, routeTopic, contextKey, defaultTopic, acl;
        QStringList members;
        QJsonArray topics;
        quint64 generation = 0, selection = 0, catalog = 0, revision = 0, unsafeRevision = 0, ticket = 0;
        quint64 collectionStreamRevision = 0, topicsUnsafeRevision = 0;
        QString collectionPath;
        int readReconciliations = 0, collectionRequests = 0;
        qint64 collectionRows = 0, collectionBytes = 0;
        QPointer<NativeCacheService> cache;
        NativeCache::Lease lease;
        bool inviteOnly = false, announce = false, busy = false;
        bool dispatched = false, verifiedRequest = false, confirmed = false, deleted = false, accessLost = false;
        bool membersAttempted = false, membersConfirmed = false, allowMembers = false, checkBindings = false;
    };
    const ChannelCreation *channelCreation() const;
    bool channelCreationAllowed() const;
    bool channelCreationContext(const QString &owner, const QString &id, quint64 ticket);
    void captureChannelContext(ChannelCreation &intent);
    bool channelPhase(const QString &owner, const QString &id, quint64 ticket, const QString &phase);
    void finishChannelCreation(const QString &owner, const QString &id, quint64 ticket,
                               const QString &error, const QString &phase = QStringLiteral("uncertain"));
    void interruptChannelCreations();
    void noteChannelCreationEvent(const QString &stream, const QString &topic = {}, bool deleted = false,
                                  bool matchingOwn = false);
    void loseChannelCreationAccess(const QString &stream);
    bool reconcileChannelRead(const QString &owner, const QString &id, quint64 ticket,
                              quint64 unsafeRevision, std::function<void()> retry);
    void updateChannelDirectory(const QJsonArray &rows, bool authoritative);
    void confirmChannelCreation(const QString &owner, const QString &id, quint64 ticket);
    void loadChannelDefault(const QString &owner, const QString &id, quint64 ticket);
    void checkChannelMembers(const QString &owner, const QString &id, quint64 ticket);
    void addChannelMembers(const QString &owner, const QString &id, quint64 ticket);
    void loadChannelCatalog(const QString &owner, const QString &id, quint64 ticket);
    void channelCollection(const QString &owner, const QString &id, quint64 ticket,
        const QString &path, QUrlQuery query, std::function<void(const QJsonArray &)> done,
        QJsonArray rows = {}, QSet<QByteArray> markers = {});
    struct TopicCreation {
        QString uuid, stream, name, error, routeStream, routeTopic, acl;
        quint64 generation = 0, selection = 0, catalog = 0, revision = 0, ticket = 0;
        QPointer<NativeCacheService> cache;
        NativeCache::Lease lease;
        bool busy = false, uncertain = false, confirmed = false, deleted = false;
    };
    QString creationOwnerKey() const;
    const TopicCreation *topicCreation() const;
    bool topicCreationAllowed(const QString &stream) const;
    bool topicCreationContext(const QString &owner, const QString &id, quint64 ticket);
    void interruptTopicCreations();
    void noteTopicCreationEvent(const QString &stream, const QString &topic = {}, bool deleted = false);
    void confirmTopicCreation(const QString &owner, const QString &id, quint64 ticket);
    void loadCreatedTopicCatalog(const QString &owner, const QString &id, quint64 ticket,
                                 quint64 revision, QByteArray marker = {}, QJsonArray rows = {},
                                 QSet<QByteArray> markers = {}, qint64 bytes = 0);
    void finishTopicCreation(const QString &owner, const QString &id, quint64 ticket,
                             const QString &error, bool uncertain = true);
    struct TopicOpening {
        QString requested, owner, routeStream, routeTopic, routeSecurity, targetSecurity;
        quint64 ticket = 0, generation = 0, selection = 0, catalog = 0;
        QPointer<ApiClient> api;
        QPointer<NativeCacheService> cache;
        NativeCache::Lease lease;
        ApiClient::RequestId request;
        std::optional<Topic> canonical;
        bool active = false, catalogVerified = false, snapshotActive = false;
        bool syncBlocked = false, forgetting = false;
    };
    void interruptTopicOpening();
    bool topicOpeningContext(quint64 ticket) const;
    void beginTopicOpening(const QString &requested, std::optional<Topic> canonical = {});
    void resolveTopicOpeningStream(quint64 ticket);
    void publishTopicOpening(quint64 ticket);
    void finishTopicOpening(quint64 ticket, const QString &error = {});
    void noteTopicOpeningStream(const Stream &stream);
    void activateStream(const QString &id, const QVector<Topic> *topics = nullptr,
                        const QString &directPublishingAttempt = {},
                        const Topic *initialTopic = nullptr, quint64 openingTicket = 0);
    struct ArchiveOperation {
        QString owner, acl, error;
        quint64 generation = 0, catalog = 0, ticket = 0;
        QPointer<NativeCacheService> cache;
        NativeCache::Lease lease;
        bool pending = false, uncertain = false, dispatched = false, invalidated = false;
    };
    bool archiveContext(const QString &id, quint64 ticket) const;
    bool archiveAvailable() const;
    void postUnarchive(const QString &id, quint64 ticket);
    void readUnarchive(const QString &id, quint64 ticket, bool mayPost);
    void finishUnarchive(const QString &id, quint64 ticket, const QString &error, bool uncertain);
    void applyUnarchive(const QString &id, quint64 ticket, const QJsonObject &canonical);
    void persistStreams();
    QHash<QString, ArchiveOperation> m_archiveOperations;
    quint64 m_nextArchiveOperation = 0;
    using OwnReactions = QMap<QString, ReactionFact>;
    struct ReactionIntent {
        QString nonce, emoji, stream, topic, owner, error;
        quint64 selection = 0;
        quint64 generation = 0;
        bool desired = false;
        bool mutated = false;
        bool checking = true;
        bool uncertain = false;
        bool reportAddition = false;
        int attempts = 0;
    };
    bool reactionContext(const QString &id, const QString &nonce);
    void ownReactions(const QString &id, std::function<void(bool, const OwnReactions &)> callback,
                      QByteArray marker = {}, OwnReactions accumulated = {}, QSet<QByteArray> seen = {});
    void confirmReaction(const QString &id, const QString &nonce);
    void retryReactionConfirmation(const QString &id, const QString &nonce);
    void refreshReactions(const QString &id);
    void resetReactions(bool retainUncertain = false);
    void reportReaction(const QString &message, const QString &id = {});
    bool authoritativeRoute() const;
    void notifyTrustChanged();
    void openCache();
    void closeCache();
    void markCacheStale();
    void invalidateCache(NativeCache::Kind kind, const QString &stream = {}, const QString &topic = {});
    void invalidateConversation(const QString &stream, const QString &topic, bool counters = true);
    void staleCache(NativeCache::Kind kind, const QString &stream = {}, const QString &topic = {});
    void staleConversation(const QString &stream, const QString &topic, bool counters = true);
    struct ProjectionContext {
        QPointer<NativeCacheService> cache;
        NativeCache::Lease lease;
        quint64 generation = 0;
    };
    ProjectionContext projectionContext() const;
    void projectCanonical(const QJsonObject &raw, NativeCache::ProjectionFields fields,
                          const ProjectionContext &context);
    struct MessageFields {
        QByteArray payloadHash;
        quint64 payload = 0, star = 0;
        bool starred = false, initialized = false, deleted = false;
        int pending = 0;
    };
    void observeMessage(const QJsonObject &raw);
    void pruneMessageFields();
    void finishMessageIntent(const QString &id);
    void staleReadConversation(const QString &stream, const QString &topic);
    void cachedCollection(const NativeCache::Key &key, const QString &path, const QUrlQuery &query,
                          std::function<bool(const QJsonArray &, bool)> done, bool forceNetwork = false);
    struct UploadTask {
        quint64 intent = 0;
        QString path;
        QString stream;
        QString conversation;
        NativeDrafts::Token draftResidency{};
    };
    struct FileOperation {
        QPointer<FileClient> service;
        quint64 operation = 0;
    };
    void cancelFileTransfers(const QString &reason = {});
    void drainUploads();
    void finishUpload(quint64 intent, quint64 generation, quint64 epoch,
                      const QString &conversation, const FileResult &result);
    void resetReadState();
    void sendReadRequest(bool confirmation);
    void finishReadRequest(bool success);
    void refreshReadCounters(const Message &boundary);
    void refreshReadStreamCounter(const Message &boundary);
    struct SendIntent {
        QString uuid, text; // Existing ordinary-send representation remains first.
        bool encrypted = false, busy = false, dispatched = false, materialLost = false, statusOnly = false;
        QString owner, user, project, stream, topic, routeTopic, key, currentKey, publicId, phase, error;
        QJsonObject payload;
        QPointer<NativeChatKeyVault> vault;
        NativeChatKeyVault::Lease vaultLease;
        ProjectionContext projection;
        quint64 deletion = 0, selection = 0, catalog = 0, accessRevision = 0, ticket = 0;
        NativeDrafts::Token draftToken{};
        bool draftCaptured = false;
        quint64 draftRevision = 0;
    };
    QString encryptedSendTarget() const;
    bool encryptedSendOwned(const SendIntent &intent, bool writing) const;
    bool encryptedSendContext(const QString &conversation, const QString &id, quint64 ticket) const;
    void captureEncryptedSend(SendIntent &intent);
    void interruptEncryptedSends();
    void sendEncryptedText();
    void recoverEncryptedSend(bool retry);
    void postEncryptedSend(const QString &conversation, const QString &id, quint64 ticket);
    void readEncryptedSend(const QString &conversation, const QString &id, quint64 ticket, bool retry);
    bool matchesEncryptedSend(const QJsonObject &raw, const SendIntent &intent, bool realtime) const;
    bool confirmEncryptedSend(const QJsonObject &raw, const QString &conversation, const QString &id,
                              quint64 ticket, bool realtime = false);
    void finishEncryptedSend(const QString &conversation, const QString &id, quint64 ticket, const QString &error);
    QVariantMap pendingSendMetadata() const;
    QString m_activeEncryptedSend;
    quint64 m_nextEncryptedSend = 0;
    ApiClient::RequestId request(const QByteArray &method, const QString &path, const QJsonValue &body,
                 const QUrlQuery &query, ApiClient::Callback callback);
    void collection(const QString &path, QUrlQuery query,
                    std::function<void(const QJsonArray &)> done,
                    QJsonArray accumulated = {}, QSet<QByteArray> markers = {});
    void loadTopics();
    void loadMessages(bool older = false);
    void historyPage(bool older, quint64 selection, quint64 revision,
                     std::optional<Message> boundary, QByteArray marker,
                     QVector<Message> accumulated, QSet<QByteArray> seen, int pages = 0, qint64 bytes = 0);
    void verifyMutation(const QString &id, quint64 selection, NativeCache::Projection field);
    void reset();
    void report(const QString &message);
    void finishSnapshotPart(unsigned part);
    void cancelSnapshot();
    const Stream *currentStream() const;
    QString peerDisplayName(const QString &id, const QString &fallback = {}) const;
    const Message *findMessage(const QString &id) const;
    bool unresolvedTopicRoute() const;
    bool inConversation(const Message &message) const;
    QString conversationKey() const { return m_stream + "/" + m_topic; }

    ApiClient *m_api;
    QPointer<NativeChatKeyVault> m_chatKeys;
    Session *m_session;
    QPointer<NativeCacheService> m_cache;
    QMetaObject::Connection m_cacheDestroyed;
    QMetaObject::Connection m_cacheStatusChanged;
    QHash<int, std::function<void()>> m_cacheHydrations;
    bool m_cacheWasAvailable = false;
    bool m_cacheForgettingAccount = false;
    NativeCache::Lease m_cacheLease;
    quint64 m_cacheRevision = 0;
    bool m_cacheProtection = false;
    bool m_cacheHydrated = false;
    bool m_catalogVerified = false;
    bool m_usersVerified = false;
    bool m_topicsVerified = false;
    bool m_topicSeedUnverified = false;
    TopicOpening m_topicOpening;
    quint64 m_nextTopicOpening = 0;
    bool m_historyVerified = false;
    // Counts accepted cache/network publications, never route clears or live rows.
    quint64 m_historyCommitRevision = 0;
    bool m_syncBlocked = false;
    QPointer<FileClient> m_files;
    QMetaObject::Connection m_filesDestroyed;
    QQueue<UploadTask> m_uploadQueue;
    QHash<quint64, FileOperation> m_fileOperations;
    quint64 m_nextFileOperation = 0;
    quint64 m_fileEpoch = 0;
    quint64 m_fileServiceSerial = 0;
    int m_activeUploads = 0;
    bool m_drainingUploads = false;
    QTimer m_poll;
    QVector<Stream> m_streams;
    QVector<Topic> m_topics;
    QVector<Message> m_messages;
    struct UserPresentation {
        QString name, avatar, presence, statusLabel;
        std::optional<qint64> presenceUpdatedAt;
        QString email;
        QString status, statusText, statusEmoji;
    };
    static UserPresentation makeUserPresentation(const QString &name, const QJsonObject &row,
        bool authoritative, const UserPresentation &previous);
    QString userDisplayName(const QString &id, const QString &fallback = {}) const;
    QString userAvatar(const QString &id) const;
    QString userPresence(const QString &id) const;
    QString userStatusLabel(const QString &id) const;
    QHash<QString, UserPresentation> m_names;
    QString m_viewerDirectoryName, m_viewerDirectoryOwner;
    QPointer<NativePreferences> m_statusPreferences;
    QString m_conversationPreferenceOwner;
    QStringList m_conversationRestore;
    quint64 m_conversationRestoreTicket = 0;
    bool m_conversationRestoreScheduled = false;
    QString m_statusPreferenceOwner, m_ownStatusOwner, m_ownStatusError;
    bool m_statusManualAway = false, m_ownStatusPending = false;
    quint64 m_ownStatusTicket = 0, m_ownStatusGeneration = 0;
    ApiClient::RequestId m_ownStatusRequest;
    void prepareOwnStatusPreferences();
    void prepareConversationRestore();
    void cancelConversationRestore();
    void maybeRestoreConversation();
    void rememberConversation();
    void interruptOwnStatusUpdate();
    QMap<QString, QVariantMap> m_channelChoices;
    QString m_channelDirectoryOwner;
    bool m_channelDirectoryValid = false;
    QHash<QString, DirectChat> m_directChats;
    quint64 m_nextDirectChat = 0, m_directTrust = 0;
    QHash<QString, ChannelCreation> m_channelCreations;
    quint64 m_nextChannelCreation = 0;
    quint64 m_channelTrust = 0;
    QHash<QString, TopicCreation> m_topicCreations;
    quint64 m_nextTopicCreation = 0;
    QHash<QString, QString> m_drafts;
    QHash<QString, quint64> m_draftRevisions;
    QPointer<NativeDrafts> m_draftStore;
    QString m_draftOwner;
    QString m_draftFallbackOwner, m_draftFallbackRoute, m_draftFallback;
    QSet<QString> m_draftRoutes;
    bool m_syncingDrafts = false, m_draftFlushFailed = false;
    QMetaObject::Connection m_draftChanges, m_draftStates, m_draftDestroyed, m_draftSelection;
    bool durableDraftRoute() const;
    void synchronizeDrafts();
    QHash<QString, SendIntent> m_pendingSends;
    QHash<QString, int> m_pendingUploads;
    QHash<QString, QString> m_attachmentNotices;
    QString m_stream;
    QString m_topic;
    QString m_error;
    QString m_syncState;
    QString m_notice;
    int m_uploads = 0;
    int m_downloads = 0;
    QByteArray m_before;
    quint64 m_selection = 0;
    quint64 m_catalog = 0;
    quint64 m_topicRequest = 0;
    quint64 m_topicRevision = 0;
    quint64 m_topicMutation = 0;
    bool m_topicChanging = false;
    quint64 m_messageRevision = 0;
    quint64 m_streamRevision = 0;
    quint64 m_readTicket = 0;
    quint64 m_readCounterRequest = 0;
    bool m_readPending = false;
    int m_readPostRetries = 0;
    int m_readConfirmAttempts = 0;
    std::optional<Message> m_readBoundary;
    std::optional<Message> m_readStreamBoundary;
    QHash<QString, Message> m_queuedReadBoundaries;
    QQueue<QString> m_readTopics;
    QHash<QString, quint64> m_readCounterRequests;
    quint64 m_readCounterInvalidation = 0;
    QSet<QString> m_failedReadAnchors;
    QSet<QString> m_readLoadedIds;
    QSet<QString> m_confirmedReadIds;
    QHash<QString, ReactionIntent> m_reactionIntents;
    QHash<QString, OwnReactions> m_ownReactions;
    QHash<QString, quint64> m_reactionRevisions;
    QHash<QString, MessageFields> m_messageFields;
    quint64 m_nextMessageField = 0;
    QHash<QString, quint64> m_reactionRefreshes;
    quint64 m_nextReactionRefresh = 0;
    bool m_recoveringMarker = false;
    bool m_resumeOlder = false;
    bool m_busy = false;
    bool m_loading = false;
    bool m_sending = false;
    bool m_realtimeMode = false;
    bool m_snapshotActive = false;
    unsigned m_snapshotParts = 0;
    quint64 m_snapshotSelection = 0;
    quint64 m_snapshotCatalog = 0;
};

}
