#pragma once

#include "media-viewer.h"

#include "api-client.h"
#include "domain.h"
#include "native-cache-service.h"
#include "session.h"
#include "native-chat-key-vault.h"
#include "native-drafts.h"
#include "native-notifications.h"

#include <QHash>
#include <QLocale>
#include <QSet>
#include <QStringList>
#include <QQueue>
#include <QTimer>
#include <QVariantList>

namespace workspace {

class MediaViewerModel;
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
    Q_PROPERTY(QString loginStep READ loginStep NOTIFY stateChanged)
    Q_PROPERTY(QString organizationRealm READ organizationRealm NOTIFY stateChanged)
    Q_PROPERTY(bool organizationLoading READ organizationLoading NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap organizationSettings READ organizationSettings NOTIFY stateChanged)
    Q_PROPERTY(QString defaultOrganizationUrl READ defaultOrganizationUrl CONSTANT)
    Q_PROPERTY(QString defaultOrganizationName READ defaultOrganizationName CONSTANT)
    Q_PROPERTY(QVariantList projects READ projects NOTIFY stateChanged)
    Q_PROPERTY(QVariantList savedAccounts READ savedAccounts NOTIFY savedAccountsChanged)
    Q_PROPERTY(QVariantList sessionSwitcherSessions READ sessionSwitcherSessions NOTIFY sessionSwitcherChanged)
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
    // Issue #132 channel info drawer content for the selected stream
    // (docs/NATIVE_DESKTOP_CHANNEL_INFO.md): member projection, notification
    // store mode and the three member/notification mutations.
    Q_PROPERTY(QVariantMap channelInfoStream READ channelInfoStream NOTIFY stateChanged)
    Q_PROPERTY(QVariantList channelInfoMembers READ channelInfoMembers NOTIFY stateChanged)
    Q_PROPERTY(int channelInfoMemberCount READ channelInfoMemberCount NOTIFY stateChanged)
    Q_PROPERTY(int channelInfoOnlineCount READ channelInfoOnlineCount NOTIFY stateChanged)
    Q_PROPERTY(bool channelInfoIsOwner READ channelInfoIsOwner NOTIFY stateChanged)
    Q_PROPERTY(bool channelInfoLoading READ channelInfoLoading NOTIFY stateChanged)
    Q_PROPERTY(QString channelInfoNotificationMode READ channelInfoNotificationMode NOTIFY stateChanged)
    Q_PROPERTY(bool channelInfoNotificationBusy READ channelInfoNotificationBusy NOTIFY stateChanged)
    Q_PROPERTY(QString channelInfoNotificationError READ channelInfoNotificationError NOTIFY stateChanged)
    Q_PROPERTY(QStringList channelInfoRemovingMembers READ channelInfoRemovingMembers NOTIFY stateChanged)
    Q_PROPERTY(QString channelInfoMemberError READ channelInfoMemberError NOTIFY stateChanged)
    Q_PROPERTY(bool channelInfoAddBusy READ channelInfoAddBusy NOTIFY stateChanged)
    Q_PROPERTY(QString channelInfoAddError READ channelInfoAddError NOTIFY stateChanged)
    Q_PROPERTY(int channelInfoAddSettled READ channelInfoAddSettled NOTIFY channelInfoAddSettledChanged)
    // Issue #134 user profile drawer content (docs/NATIVE_DESKTOP_USER_PROFILE.md):
    // own IAM profile state for edit mode, own avatar mutations and the
    // profile Message-action direct-chat open.
    Q_PROPERTY(bool ownProfileBusy READ ownProfileBusy NOTIFY stateChanged)
    Q_PROPERTY(bool ownProfileSaving READ ownProfileSaving NOTIFY stateChanged)
    Q_PROPERTY(bool ownProfileLoaded READ ownProfileLoaded NOTIFY stateChanged)
    Q_PROPERTY(bool ownProfileRefreshPending READ ownProfileRefreshPending NOTIFY stateChanged)
    Q_PROPERTY(QString ownProfileError READ ownProfileError NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap ownProfile READ ownProfile NOTIFY stateChanged)
    Q_PROPERTY(int ownProfileSettled READ ownProfileSettled NOTIFY ownProfileSettledChanged)
    Q_PROPERTY(bool ownAvatarBusy READ ownAvatarBusy NOTIFY stateChanged)
    Q_PROPERTY(QString ownAvatarError READ ownAvatarError NOTIFY stateChanged)
    Q_PROPERTY(int ownAvatarSettled READ ownAvatarSettled NOTIFY ownAvatarSettledChanged)
    Q_PROPERTY(bool profileDirectPending READ profileDirectPending NOTIFY stateChanged)
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
    Q_PROPERTY(QString editingMessageId READ editingMessageId NOTIFY editSessionChanged)
    Q_PROPERTY(bool canSaveEdit READ canSaveEdit NOTIFY editSessionChanged)
    Q_PROPERTY(QString replyMessageId READ replyMessageId NOTIFY replySessionChanged)
    Q_PROPERTY(QString replyAuthor READ replyAuthor NOTIFY replySessionChanged)
    Q_PROPERTY(QString replyExcerpt READ replyExcerpt NOTIFY replySessionChanged)
    Q_PROPERTY(bool canCompose READ canCompose NOTIFY stateChanged)
    Q_PROPERTY(bool canSend READ canSend NOTIFY stateChanged)
    Q_PROPERTY(bool canLoadOlder READ canLoadOlder NOTIFY stateChanged)
    Q_PROPERTY(bool canLoadNewer READ canLoadNewer NOTIFY stateChanged)
    Q_PROPERTY(QString title READ title NOTIFY stateChanged)
    Q_PROPERTY(QString syncState READ syncState NOTIFY stateChanged)
    // Issue #146 top overlay banners: derived, read-only view of the existing
    // realtime/transport health, prioritized like the reference layout
    // (offline > rate_limited > reconnecting > degraded > online).
    Q_PROPERTY(QString connectionPhase READ connectionPhase NOTIFY stateChanged)
    Q_PROPERTY(QString connectionSeverity READ connectionSeverity NOTIFY stateChanged)
    Q_PROPERTY(int connectionRetryInSeconds READ connectionRetryInSeconds NOTIFY stateChanged)
    Q_PROPERTY(bool bootstrapError READ bootstrapError NOTIFY stateChanged)
    Q_PROPERTY(QString ownerKey READ ownerKey NOTIFY stateChanged)
    // Issue #134: verified session user for profile self/other decisions.
    Q_PROPERTY(QString currentUserId READ currentUserId NOTIFY stateChanged)
    Q_PROPERTY(bool canAttach READ canAttach NOTIFY stateChanged)
    Q_PROPERTY(MediaViewerModel *mediaViewer READ mediaViewer CONSTANT)
    Q_PROPERTY(bool topicClosed READ topicClosed NOTIFY stateChanged)
    Q_PROPERTY(bool canReopenTopic READ canReopenTopic NOTIFY stateChanged)
    Q_PROPERTY(bool fileBusy READ fileBusy NOTIFY stateChanged)
    Q_PROPERTY(QString notice READ notice NOTIFY stateChanged)
    Q_PROPERTY(bool draftsVisible READ draftsVisible NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap draftPage READ draftPage NOTIFY stateChanged)
    Q_PROPERTY(bool mentionsVisible READ mentionsVisible NOTIFY stateChanged)
    Q_PROPERTY(QString mentionsFilter READ mentionsFilter NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap mentionsPage READ mentionsPage NOTIFY stateChanged)
    Q_PROPERTY(bool inboxVisible READ inboxVisible NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap inboxPage READ inboxPage NOTIFY stateChanged)
    Q_PROPERTY(bool cacheStale READ cacheStale NOTIFY stateChanged)
    Q_PROPERTY(QString anchorMessageId READ anchorMessageId NOTIFY anchorChanged)
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
    // Sidebar row previews and the reference relative-time label, measured in
    // docs/NATIVE_DESKTOP_SIDEBAR_PREVIEWS.md; exposed for the QtTest contract cases.
    static QString summarizeRowPreview(const QString &plainText);
    static QString formatRowTime(const QDateTime &created, const QLocale &locale,
                                 const QDate &today, const QString &yesterdayLabel);
    bool busy() const { return m_busy || m_loading || m_sending || m_snapshotActive || m_topicChanging || m_cacheForgettingAccount; }
    QString error() const { return m_error; }
    QVariantList projects() const;
    QVariantList savedAccounts() const;
    // Rows for the top bar session switcher: remembered accounts with the
    // reference labels, projection-backed unread counts and the current mark
    // (docs/NATIVE_DESKTOP_SESSION_SWITCHER.md, issue #144).
    QVariantList sessionSwitcherSessions() const;
    static int backgroundProjectionUnread(const QVariantMap &projection);
    static QString unreadBadgeLabel(int unread);
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
    QVariantMap channelInfoStream() const;
    QVariantList channelInfoMembers() const;
    int channelInfoMemberCount() const { return channelInfoStream().isEmpty() ? 0 : int(m_channelInfoMembers.size()); }
    int channelInfoOnlineCount() const;
    bool channelInfoIsOwner() const;
    bool channelInfoLoading() const { return m_channelInfoLoading; }
    QString channelInfoNotificationMode() const;
    // Busy state is tracked per stream by the in-flight request token: each
    // request owns its stream's slot from issue time, and only that request's
    // own response may commit or release it, so neither an abandoned stream
    // nor a superseded response can disable or overwrite a newer request.
    bool channelInfoNotificationBusy() const
    { return m_channelInfoNotificationInFlight.contains(m_stream); }
    QString channelInfoNotificationError() const { return m_channelInfoNotificationError; }
    QStringList channelInfoRemovingMembers() const { return m_channelInfoRemoving; }
    QString channelInfoMemberError() const { return m_channelInfoMemberError; }
    bool channelInfoAddBusy() const
    { return m_channelInfoAddInFlight.contains(m_stream); }
    QString channelInfoAddError() const { return m_channelInfoAddError; }
    int channelInfoAddSettled() const { return m_channelInfoAddSettled; }
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
    QString anchorMessageId() const { return m_anchorMessage; }
    QString draft() const;
    void setDraft(const QString &text);
    bool canCompose() const;
    bool canSend() const;
    bool canLoadOlder() const { return !m_before.isEmpty() && !m_loading && !m_snapshotActive
        && (!m_cacheProtection || m_historyVerified); }
    bool canLoadNewer() const { return !m_after.isEmpty() && !m_loading && !m_snapshotActive
        && (!m_cacheProtection || m_historyVerified); }
    QString title() const;
    QString syncState() const { return m_syncState; }
    // docs/NATIVE_DESKTOP_OVERLAY_BANNERS.md: mapping of realtime sync state,
    // transport failures and HTTP 429 retries into the reference banner kinds.
    QString connectionPhase() const;
    QString connectionSeverity() const;
    int connectionRetryInSeconds() const;
    bool bootstrapError() const { return m_bootstrapError; }
    QString ownerKey() const { return authenticated()
        ? SessionVault::accountId(m_api->server(), m_session->project(), m_session->user()) : QString{}; }
    // Loopback-safe owner scope used by the mention frecency store and the
    // chat-member authority (tests pin preferences to this identity).
    QString creationOwnerKey() const;
    bool canAttach() const;
    MediaViewerModel *mediaViewer() const { return m_mediaViewer; }
    bool topicClosed() const;
    bool canReopenTopic() const;
    bool fileBusy() const { return m_uploads > 0 || m_downloads > 0; }
    QString notice() const;
    bool draftsVisible() const { return m_draftsVisible; }
    QVariantMap draftPage() const;
    bool mentionsVisible() const { return m_mentionsVisible; }
    QString mentionsFilter() const { return m_mentionsFilter; }
    QVariantMap mentionsPage() const;
    bool inboxVisible() const { return m_inboxVisible; }
    QVariantMap inboxPage() const;

    Q_INVOKABLE void login(const QString &server, const QString &user,
                           const QString &password, const QString &otp);
    // Login wizard organization step (docs/NATIVE_DESKTOP_LOGIN.md).
    QString loginStep() const { return m_loginStep; }
    QString organizationRealm() const { return m_organizationRealm; }
    bool organizationLoading() const { return m_organizationLoading; }
    QVariantMap organizationSettings() const { return m_organizationSettings; }
    QString defaultOrganizationUrl() const { return m_defaultOrganizationUrl; }
    QString defaultOrganizationName() const { return m_defaultOrganizationName; }
    void setPreferences(NativePreferences *preferences);
    Q_INVOKABLE QStringList organizationHistory() const;
    Q_INVOKABLE bool isOrganizationUrlValid(const QString &text) const;
    Q_INVOKABLE void organizationInputChanged(const QString &text);
    Q_INVOKABLE void discoverOrganization(const QString &text);
    Q_INVOKABLE void continueFromOrganization(const QString &text);
    Q_INVOKABLE void backToOrganization();
    Q_INVOKABLE void selectDefaultOrganization();
    Q_INVOKABLE void openProject(const QString &id);
    Q_INVOKABLE void resumeSavedAccount(const QString &id);
    Q_INVOKABLE void forgetSavedAccount(const QString &id);
    // Top bar workspace session switcher (issue #144).
    Q_INVOKABLE void selectWorkspaceSession(const QString &id);
    Q_INVOKABLE void logoutWorkspaceSession(const QString &id);
    Q_INVOKABLE void openAddServer();
    Q_INVOKABLE void publishSessionProjection(const QString &accountId, const QVariantMap &projection);
    Q_INVOKABLE void logout();
    Q_INVOKABLE void selectStream(const QString &id);
    Q_INVOKABLE void selectTopic(const QString &id);
    Q_INVOKABLE void openTopic(const QString &id, const QString &streamHint = {});
    // Window focus mirror for desktop notifications (Electron sends
    // window:activity from the focus/blur handlers in main.ts).
    Q_INVOKABLE void setWindowFocused(bool focused);
    NotificationService *notifications() { return &m_notifications; }
    Q_INVOKABLE void refresh();
    // Reference bootstrap error banner Retry (bootstrapRetryNonce analogue):
    // re-runs the existing snapshot bootstrap path.
    Q_INVOKABLE void retryBootstrap();
    Q_INVOKABLE void showDrafts();
    Q_INVOKABLE void closeDrafts();
    Q_INVOKABLE void refreshDrafts();
    Q_INVOKABLE void showMentions();
    Q_INVOKABLE void closeMentions();
    Q_INVOKABLE void selectMentionsFilter(const QString &filter);
    Q_INVOKABLE void loadMoreMentions();
    Q_INVOKABLE void retryMentions();
    Q_INVOKABLE void showInbox();
    Q_INVOKABLE void closeInbox();
    Q_INVOKABLE void openInboxRow(const QString &stream, const QString &topic);
    Q_INVOKABLE void nextDraftPage();
    Q_INVOKABLE void previousDraftPage();
    Q_INVOKABLE void openDraft(const QString &id);
    Q_INVOKABLE void deleteDraft(const QString &id);
    Q_INVOKABLE void resolveDraft(const QString &id, const QString &action);
    Q_INVOKABLE void loadOlder();
    Q_INVOKABLE void loadNewer();
    Q_INVOKABLE void reopenTopic();
    Q_INVOKABLE bool canStartDirectChatWith(const QString &peer) const;
    Q_INVOKABLE void startDirectChat(const QString &peer);
    Q_INVOKABLE void checkDirectChat();
    Q_INVOKABLE void retryDirectChat();
    Q_INVOKABLE void createChannel(const QString &name, const QString &description,
        bool inviteOnly, bool announce, const QVariantList &memberUuids);
    Q_INVOKABLE void checkChannelCreation();
    Q_INVOKABLE void retryChannelMembers();
    Q_INVOKABLE void refreshChannelInfo();
    Q_INVOKABLE void setChannelInfoNotificationMode(const QString &mode);
    Q_INVOKABLE void removeChannelInfoMember(const QString &bindingUuid);
    Q_INVOKABLE void addChannelInfoMembers(const QStringList &userUuids);
    Q_INVOKABLE void resetChannelInfoAddState();
    // ---- Issue #134 profile drawer ----
    Q_INVOKABLE QVariantMap profileSnapshot(const QString &userUuid) const;
    Q_INVOKABLE QVariantList profileDetails(const QString &userUuid, const QString &localeName) const;
    Q_INVOKABLE QString buildProfileShareLink(const QString &userUuid) const;
    // Clipboard writes stay in C++ (message copy precedent).
    Q_INVOKABLE void copyToClipboard(const QString &text) const;
    Q_INVOKABLE QVariantMap profileAvatarCapabilities() const;
    Q_INVOKABLE QVariantMap validateAvatarFile(const QString &path) const;
    Q_INVOKABLE void loadOwnProfile();
    // Save commits the identity patch first (only changed keys), then the
    // optional pending avatar mutation ("upload" with path, or "remove").
    Q_INVOKABLE void saveOwnProfile(const QVariantMap &draft, const QString &avatarAction = QString(),
        const QString &avatarPath = QString());
    Q_INVOKABLE void uploadOwnProfileAvatar(const QString &path);
    Q_INVOKABLE void removeOwnProfileAvatar();
    Q_INVOKABLE void openDirectChatFromProfile(const QString &userUuid);
    QString currentUserId() const { return authenticated() ? m_session->user() : QString(); }
    bool ownProfileBusy() const { return m_ownProfileLoading; }
    bool ownProfileSaving() const { return m_ownProfileSaving; }
    bool ownProfileLoaded() const { return !m_ownProfileBaseline.isEmpty(); }
    bool ownProfileRefreshPending() const { return m_ownProfileRefresh; }
    QString ownProfileError() const { return m_ownProfileError; }
    QVariantMap ownProfile() const { return m_ownProfileBaseline; }
    int ownProfileSettled() const { return m_ownProfileSettledCount; }
    bool ownAvatarBusy() const { return m_ownAvatarBusy; }
    QString ownAvatarError() const { return m_ownAvatarError; }
    int ownAvatarSettled() const { return m_ownAvatarSettledCount; }
    bool profileDirectPending() const { return m_profileDirectActive; }
    QString userAvatar(const QString &id) const;
    Q_INVOKABLE bool canCreateTopicIn(const QString &stream) const;
    Q_INVOKABLE void createTopic(const QString &stream, const QString &name);
    Q_INVOKABLE void checkTopicCreation();
    Q_INVOKABLE void send();
    Q_INVOKABLE void checkPendingSend();
    Q_INVOKABLE void retryPendingSend();
    Q_INVOKABLE void copyMessage(const QString &id);
    Q_INVOKABLE QString messageLink(const QString &id) const;
    Q_INVOKABLE void copyMessageLink(const QString &id);
    Q_INVOKABLE void openMessageLink(const QString &url);
    Q_INVOKABLE void starMessage(const QString &id);
    Q_INVOKABLE void deleteMessage(const QString &id);
    Q_INVOKABLE void editMessage(const QString &id, const QString &text);
    Q_INVOKABLE bool beginEditMessage(const QString &id);
    Q_INVOKABLE void cancelEditMessage();
    Q_INVOKABLE void submitEditMessage();
    Q_INVOKABLE void editLastMessage();
    QString editingMessageId() const { return m_editingMessage; }
    bool canSaveEdit() const;
    // Reply session (docs/NATIVE_DESKTOP_REPLIES.md): a single quote slot; the
    // composer draft stays the answer and send() composes the urn:quote line.
    Q_INVOKABLE bool beginReply(const QString &id);
    Q_INVOKABLE void cancelReply();
    Q_INVOKABLE void openMessageInChat(const QString &id);
    QString replyMessageId() const { return m_replyMessage; }
    QString replyAuthor() const;
    QString replyExcerpt() const;
    Q_INVOKABLE void markMessagesReadUpTo(const QString &id);
    Q_INVOKABLE void markVisibleMessagesRead(const QVariantList &ids);
    Q_INVOKABLE void toggleReaction(const QString &id, const QString &emoji);
    Q_INVOKABLE void attachFiles(const QVariantList &localUrls);
    Q_INVOKABLE void downloadFile(const QString &id, const QUrl &destination);

    // ---- Composer @mention suggestions (docs/NATIVE_DESKTOP_COMPOSER_MENTION.md) ----
    // Conversation context for mention ranking (MentionRankingContext in the
    // reference): self penalty, channel membership when known, recent authors,
    // DM partners and per-user mention frecency.
    struct MentionRankingContext
    {
        QString selfUserUuid;
        bool membersKnown = false;
        QSet<QString> channelMembers;
        QStringList recentAuthorUuids;
        QStringList dmPartnerUuids;
        QVariantMap frecencyByUserUuid;
    };

    // Pure ranking over candidate row maps (userUuid/displayName/username/email/
    // status/avatarUrl). Orders by match tier, then context score, then shorter
    // name, then locale order; membership marks carry "outsideChannel".
    static QVariantList rankMentionSuggestions(const QString &query, const QVariantList &candidates,
        const MentionRankingContext &context);
    Q_INVOKABLE QVariantList mentionSuggestions(const QString &query) const;
    Q_INVOKABLE bool composerMentionsEnabled() const;
    Q_INVOKABLE void recordMentionPick(const QString &userUuid);

signals:
    void chatKeyStoreOpenRequested();
    void archivedChatsChanged();
    void stateChanged();
    void channelInfoAddSettledChanged();
    void ownProfileSettledChanged();
    void ownAvatarSettledChanged();
    void anchorChanged();
    void savedAccountsChanged();
    void sessionSwitcherChanged();
    void draftChanged();
    void editSessionChanged();
    void replySessionChanged();
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
    void applyDeepLink(const QString &url);
    void flushPendingDeepLink(bool settle = true);
    // Mention suggestion sources for mentionSuggestions()/send serialization.
    QVariantList mentionCandidates() const;
    MentionRankingContext mentionRankingContext() const;
    QVector<ComposerMention> composerMentionEntries() const;
    NativePreferences *mentionPreferences() const;
    void applyMessageRoute(const QString &org, const QString &project, const QString &message);
    void setAnchorMessage(const QString &id);
    void clearAnchor(const QString &notice = {});
    bool topicOpeningOwnsPendingAnchor() const;
    void reanchorFromWindow();
    static QString deepLinkUnavailableNotice();
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
    void observeTransport(const ApiResult &result);
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
                          std::function<bool(const QJsonArray &, bool)> done, bool forceNetwork = false,
                          std::function<void()> failed = {});
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
        QString wire; // Wire markdown: the draft, or the urn:quote line plus the answer.
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
                    QJsonArray accumulated = {}, QSet<QByteArray> markers = {},
                    std::function<void()> failed = {});
    void loadTopics();
    // Notification metadata backfill: topics of background streams are not in
    // the active catalog, so mute policy fetches the detail on demand.
    void requestTopicMetadata(const QString &topicUuid);
    void loadMessages(bool older = false);
    void loadAroundAnchor();
    void loadNewerPage(const QByteArray &marker);
    void historyPage(bool older, quint64 selection, quint64 revision,
                     std::optional<Message> boundary, QByteArray marker,
                     QVector<Message> accumulated, QSet<QByteArray> seen, int pages = 0, qint64 bytes = 0);
    void verifyMutation(const QString &id, quint64 selection, NativeCache::Projection field,
                        const std::function<void(bool)> &settled = {});
    // Inline edit session (docs/NATIVE_DESKTOP_INLINE_EDIT.md): the buffer replaces the
    // composer draft and the conversation's stored draft is never written while it is open.
    void submitMessageEdit(const QString &id, const QString &text,
                           const std::function<void(bool)> &settled);
    void closeEditSession();
    void settleEditRouteChange();
    void closeReplySession();
    void scheduleQuoteFetch(const QString &id) const;
    const Message *editableMessage(const QString &id) const;
    void reset();
    void forgetSavedAccountWithFollowUp(const QString &id, std::function<void()> done);
    void report(const QString &message);
    void finishSnapshotPart(unsigned part);
    void cancelSnapshot();
    const Stream *currentStream() const;
    QString peerDisplayName(const QString &id, const QString &fallback = {}) const;
    const Message *findMessage(const QString &id) const;
    bool unresolvedTopicRoute() const;
    bool inConversation(const Message &message) const;
    void offerNotificationCandidate(const Message &message);
    QString conversationKey() const { return m_stream + "/" + m_topic; }

    ApiClient *m_api;
    // Per-saved-account background projection facts keyed by saved account id
    // (issue #144); unread badges for remembered sessions derive from these.
    QMap<QString, QVariantMap> m_sessionProjections;
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
    MediaViewerModel *m_mediaViewer = nullptr;
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
    // Issue #132 channel info drawer state for the selected stream. The
    // binding rows belong to m_channelInfoStreamId; a projection read with a
    // different selection stays empty, so stale pages can never leak across
    // conversations and late responses are dropped by the load ticket.
    struct ChannelInfoMember {
        QString bindingUuid, userUuid, role;
    };
    const Stream *channelInfoStreamRow() const;
    bool channelInfoContext(quint64 ticket, const QString &stream) const;
    void loadChannelInfoPage(const QString &marker, quint64 ticket, const QString &stream,
                             QVector<ChannelInfoMember> accepted, QSet<QString> seenUsers);
    QString m_channelInfoStreamId;
    QVector<ChannelInfoMember> m_channelInfoMembers;
    quint64 m_channelInfoTicket = 0;
    bool m_channelInfoLoading = false;
    QHash<QString, quint64> m_channelInfoNotificationInFlight;
    quint64 m_channelInfoNotificationToken = 0;
    QString m_channelInfoNotificationError;
    QStringList m_channelInfoRemoving;
    QString m_channelInfoMemberError;
    QHash<QString, quint64> m_channelInfoAddInFlight;
    quint64 m_channelInfoAddToken = 0;
    QString m_channelInfoAddError;
    int m_channelInfoAddSettled = 0;
    // ---- Issue #134 user profile drawer state ----
    void refreshOwnProfileProjection(const QString &user, quint64 ticket,
        const QVariantMap &savedProfile);
    bool profileContext(const QString &user, quint64 ticket) const;
    void captureProfileContext();
    QString m_profileOwner, m_profileUser, m_ownProfileOwner, m_ownProfileError, m_ownAvatarError;
    QString m_profileDirectStream, m_profileDirectTopic;
    QVariantMap m_ownProfileBaseline;
    quint64 m_profileApiGeneration = 0;
    quint64 m_ownProfileTicket = 0, m_ownAvatarTicket = 0;
    quint64 m_profileDirectSelection = 0;
    bool m_ownProfileLoading = false, m_ownProfileSaving = false, m_ownProfileRefresh = false;
    bool m_ownAvatarBusy = false, m_profileDirectActive = false;
    int m_ownProfileSettledCount = 0, m_ownAvatarSettledCount = 0;
    QVector<Topic> m_topics;
    NotificationService m_notifications;
    bool m_windowFocused = true;
    QVector<Message> m_messages;
    struct UserPresentation {
        QString name, avatar, presence, statusLabel;
        std::optional<qint64> presenceUpdatedAt;
        QString email;
        QString status, statusText, statusEmoji;
        // Issue #134: raw DTO facts the profile page renders (joined date,
        // username). Absent fields stay empty, never invented.
        QString createdAt;
        QString username;
    };
    static UserPresentation makeUserPresentation(const QString &name, const QJsonObject &row,
        bool authoritative, const UserPresentation &previous);
    QString userDisplayName(const QString &id, const QString &fallback = {}) const;
    // Sidebar row previews, docs/NATIVE_DESKTOP_SIDEBAR_PREVIEWS.md.
    struct RowPreview {
        QString text;
        QString author;
        bool own = false;
        bool locked = false;
        QDateTime created;
    };
    std::optional<RowPreview> rowPreview(const QString &uuid) const;
    std::optional<RowPreview> rowPreviewOf(const Message &message) const;
    void storeRowPreview(const QString &uuid, const std::optional<RowPreview> &preview) const;
    void scheduleRowPreviewHydration() const;
    void pumpRowPreviewHydration();
    void announceStreamRowPreviews(const QVector<Message> &rows);
    // Issue #116: a memo preview must not outlive the payload it was derived from. The
    // authoritative payload paths (confirmed edit verification, live message events and
    // deletions) route through these so the memo tracks the payload and stale in-flight
    // disk reads cannot resurrect an older body.
    void refreshRowPreviewFromPayload(const Message &message);
    void markRowPreviewDeleted(const QString &uuid);
    void announceRowPreviewRows(const QString &uuid);
    quint64 bumpRowPreviewFence(const QString &uuid);
    qint64 rowActivityMicros(const QString &lastMessage, qint64 updatedMicros) const;
    int streamGroupRank(const Stream &stream) const;
    QVariantMap rowPreviewFields(const QString &lastMessage) const;
    // Bounded cache warm for row previews (docs/NATIVE_DESKTOP_SIDEBAR_PREVIEWS.md section 2):
    // the projection is const and memoizes misses here; only the pump, the payload-change
    // paths and the sign-out reset mutate these fields, always on the GUI thread.
    static constexpr int rowPreviewCacheLimit = 512;
    static constexpr int rowPreviewHydrationBatch = 8;
    mutable QHash<QString, RowPreview> m_rowPreviews;
    mutable QQueue<QString> m_rowPreviewOrder;
    mutable QSet<QString> m_rowPreviewPending;
    mutable QSet<QString> m_rowPreviewInFlight;
    mutable bool m_rowPreviewPumpQueued = false;
    // Issue #116: per-uuid revision bumped whenever the authoritative payload moves (edit
    // verification, live update, deletion). A hydration read captures the revision when it
    // starts and discards its body when the revision has moved by completion, so an older
    // disk read can never overwrite the memo a newer payload already wrote. The map is only
    // live for the duration of a read; it is cleared on owner reset and wholesale on
    // overflow, and a lost fence only lets a disk read through, which projects the same
    // canonical body the payload path wrote.
    QHash<QString, quint64> m_rowPreviewFence;
    quint64 m_nextRowPreviewFence = 0;
    QString userPresence(const QString &id) const;
    QString userStatusLabel(const QString &id) const;
    QHash<QString, UserPresentation> m_names;
    QSet<QString> m_topicMetadataInFlight;
    QSet<QString> m_topicMetadataFailed;
    // Notification metadata for topics outside the active stream's catalog.
    // Kept separate from m_topics so background backfills never reshape the
    // conversation UI's topic list.
    QHash<QString, Topic> m_topicNotificationBackfill;
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
    // Inline edit session (docs/NATIVE_DESKTOP_INLINE_EDIT.md): the buffer stands in for
    // the composer draft; m_editRoute is the conversationKey() the session belongs to.
    QString m_editingMessage, m_editBuffer, m_editRoute;
    // Save-race guards (issue #115): m_editSession names the open session (bumped on every
    // begin), m_editRevision tracks the edit buffer (bumped whenever the buffer is replaced).
    // A save completion may only close the exact (session, revision) it submitted.
    // m_editSaveInFlight lists messages whose edit PUT is still in flight, so a second
    // save for the same message is refused instead of racing the first.
    quint64 m_editSession = 0, m_editRevision = 0;
    QSet<QString> m_editSaveInFlight;
    // Reply session (docs/NATIVE_DESKTOP_REPLIES.md): quoted message uuid plus
    // the conversationKey() that owns it; m_quotes caches resolved references.
    QString m_replyMessage, m_replyRoute;
    struct QuoteRecord { QString authorId, content; bool missing = false; };
    mutable QHash<QString, QuoteRecord> m_quotes;
    // Transport failures stay retriable (reference loader keeps "stale" out of
    // the resolved store); value = earliest epoch-ms to re-issue the lookup.
    mutable QHash<QString, qint64> m_quoteRetryAfter;
    mutable QSet<QString> m_quoteInFlight;
    QPointer<NativeDrafts> m_draftStore;
    QString m_draftOwner;
    QString m_draftFallbackOwner, m_draftFallbackRoute, m_draftFallback;
    QSet<QString> m_draftRoutes;
    bool m_syncingDrafts = false, m_draftFlushFailed = false;
    bool m_draftsVisible = false, m_selectingDraftRoute = false;
    QString m_draftOpenId, m_draftOpenStream, m_draftOpenTopic, m_draftOpenOwner;
    quint64 m_draftOpenGeneration = 0;
    QMetaObject::Connection m_draftChanges, m_draftStates, m_draftDestroyed, m_draftSelection;
    // Activity mentions page (#42, docs/NATIVE_DESKTOP_ACTIVITY_MENTIONS.md).
    struct MentionCollection {
        QVector<Message> rows;
        QByteArray marker;
        quint64 version = 0;
        bool inflight = false;
        bool loading = false;
        bool error = false;
        bool paginationError = false;
        bool hasMore = false;
    };
    MentionCollection &mentionCollection(const QString &filter);
    void loadMentionCollection(const QString &filter, bool append);
    void loadMentionIndex();
    QVariantList mentionRows(const QString &filter) const;
    bool mentionUnread(const Message &message) const;
    QString mentionPreview(const Message &message) const;
    QString mentionTime(const QDateTime &created) const;
    void resetMentions();
    // Issue #54: the sidebar destinations are mutually exclusive. Opening one
    // closes the others so exactly one page and one sidebar selection stay
    // active, and closing the active page returns to the conversation.
    enum class ContentPage { Drafts, Mentions, Inbox };
    void closeOtherContentPages(ContentPage keep);
    // Unread-mentions index entries (activity.model.ts ActivityUnreadMention):
    // stream/topic/created_at are needed for the read-boundary and scope clears
    // of activity-realtime-applier.lib.ts:55-105.
    struct MentionIndexEntry { QString stream, topic; qint64 createdMicros = 0; };
    bool mentionIndexRemove(const QString &uuid);
    bool mentionIndexUpsert(const Message &message);
    bool mentionIndexBoundary(const Message &message);
    bool mentionIndexClearTopic(const QString &stream, const QString &topic);
    bool mentionIndexClearStream(const QString &stream);
    void mentionLiveUpsert(const Message &message);
    bool mentionLiveRemove(const QString &uuid);
    // Issue #56: a confirmed message.deleted is final, not a read event. The
    // uuid gets tombstoned (bounded FIFO) so no later or in-flight collection
    // page can reinsert it, and every fetched collection is pruned in place.
    bool mentionCollectionsPrune(const QString &uuid);
    bool m_mentionsVisible = false;
    QString m_mentionsFilter = QStringLiteral("all");
    QHash<QString, MentionCollection> m_mentionsCollections;
    QHash<QString, MentionIndexEntry> m_unreadMentions;
    QVector<Message> m_liveMentions;
    QSet<QString> m_deletedMentionIds;
    QStringList m_deletedMentionOrder;
    bool m_mentionIndexReady = false;
    bool m_mentionIndexWalking = false;
    QByteArray m_mentionIndexMarker;
    QSet<QByteArray> m_mentionIndexMarkers;
    // Inbox page (#44, docs/NATIVE_DESKTOP_INBOX.md): the catalog already
    // carries the active/passive counters; the inbox keeps its own per-stream
    // topic snapshots so the reference topic rows exist for streams whose
    // topic lists the sidebar has not hydrated (design doc hydration note).
    bool m_inboxVisible = false;
    QHash<QString, QVector<Topic>> m_inboxTopics;
    QHash<QString, quint64> m_inboxHydrating; // stream -> hydration token (#64)
    quint64 m_inboxHydrationToken = 0;
    void hydrateInboxTopics();
    void hydrateInboxStream(const QString &stream);
    bool inboxSlotTopicEvent(const QString &kind, const Topic &topic, bool ownsPlacement = false);
    // #65: authoritative placement events observed while a slot fetch is in
    // flight. A newer event DTO overrides a stale snapshot row; a move-out or
    // deletion drops the row. Entries are applied whenever a slot lands and
    // kept until resetInbox(), bounding their count by distinct moved/deleted
    // topics of the session.
    QHash<QString, Topic> m_inboxPlacementEvents;
    QSet<QString> m_inboxDeletedTopics;
    bool inboxSlotStreamRead(const QString &stream);
    void resetInbox();
    bool durableDraftRoute() const;
    void synchronizeDrafts();
    QHash<QString, SendIntent> m_pendingSends;
    QHash<QString, int> m_pendingUploads;
    QHash<QString, QString> m_attachmentNotices;
    QString m_stream;
    QString m_topic;
    QString m_anchorMessage;
    QString m_pendingDeepLink;
    bool m_deepLinkFlushScheduled = false;
    QString m_pendingAnchorMessage;
    std::optional<Message> m_pendingAnchorParsed;
    QString m_pendingAnchorStream;
    QString m_pendingAnchorTopic;
    int m_anchorBudget = 0;
    bool m_anchorAroundTried = false;
    QTimer m_anchorHighlight;
    QString m_error;
    QString m_loginStep = QStringLiteral("organization");
    QString m_organizationRealm;
    QString m_checkedOrganizationOrigin;
    QString m_pendingOrganizationOrigin;
    QString m_defaultOrganizationUrl;
    QString m_defaultOrganizationName;
    QVariantMap m_organizationSettings;
    QPointer<NativePreferences> m_preferences;
    bool m_organizationLoading = false;
    quint64 m_organizationAttempt = 0;
    QString m_syncState;
    QTimer m_connectionCountdown{this};
    qint64 m_rateLimitedUntilMs = 0;
    bool m_transportOffline = false;
    bool m_bootstrapError = false;
    QString m_notice;
    int m_uploads = 0;
    int m_downloads = 0;
    QByteArray m_before;
    QByteArray m_after;
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
