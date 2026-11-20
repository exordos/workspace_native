#pragma once

#include "media-viewer.h"
#include "clipboard-file-identity.h"

#include "api-client.h"
#include "domain.h"
#include "native-cache-service.h"
#include "native-diagnostics.h"
#include "session.h"
#include "native-chat-key-vault.h"
#include "native-drafts.h"
#include "native-notifications.h"
#include "call/jitsi-call.h"
#include "call/native-call-window.h"
#include "call/native-call-store.h"
#include "call/incoming-call-lifecycle.h"
#include "native-call-bridge.h"

#include <QElapsedTimer>
#include <QHash>
#include <QLocale>
#include <QIODevice>
#include <QJsonArray>
#include <QSet>
#include <QStringList>
#include <QQueue>
#include <QTimer>
#include <QVariantList>
#include <QTemporaryFile>
#include <memory>
#include <atomic>

namespace workspace {

class MediaViewerModel;
class NativePreferences;
class NativeAuthGuard;
class RealtimeClient;
class PowerWatcher;
class FileClient;
class NativeChatKeyVault;
struct FileResult;

class NativeController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool authenticated READ authenticated NOTIFY stateChanged)
    Q_PROPERTY(bool profileLinkFailed READ profileLinkFailed NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QVariantList downloadEntries READ downloadEntries NOTIFY downloadsChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(QString loginStep READ loginStep NOTIFY stateChanged)
    // Issue #60 preselect (login-page.ui.tsx:316): the linked-message project
    // when the discovered list contains it, else the first project, else "".
    Q_PROPERTY(QString projectPreselection READ projectPreselection NOTIFY stateChanged)
    Q_PROPERTY(QString loginUsername READ loginUsername NOTIFY stateChanged)
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
    // Issue #221: uuids whose per-message re-decrypt is in flight. NOTIFY fires on every
    // insert and erase so the error-row Retry can bind its enabled state to this property.
    Q_PROPERTY(QStringList decryptRetryPendingUuids READ decryptRetryPendingUuids NOTIFY decryptRetryStateChanged)
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
    // Issue #89 (docs/NATIVE_DESKTOP_CHAT_HEADER_COUNTERS.md): the member +
    // online projection behind the chat header caption. Selection-scoped:
    // both reads report 0 unless the loaded bindings belong to the current
    // authentication and the selected (non-direct) stream.
    Q_PROPERTY(int chatHeaderMemberCount READ chatHeaderMemberCount NOTIFY stateChanged)
    Q_PROPERTY(int chatHeaderOnlineCount READ chatHeaderOnlineCount NOTIFY stateChanged)
    // Issue #438: the chat header publishes whether it is actually on screen;
    // only then may the member projection fetch pages for a new selection.
    Q_PROPERTY(bool chatHeaderVisible READ chatHeaderVisible WRITE setChatHeaderVisible NOTIFY stateChanged)
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
    // Issue #83: folder rail (docs/NATIVE_DESKTOP_FOLDERS_RAIL.md). The rail
    // rows, the effective folder selection and the folder-scoped sidebar
    // projection over the stream catalog.
    Q_PROPERTY(QVariantList folders READ folders NOTIFY foldersChanged)
    Q_PROPERTY(QVariantList sidebarStreams READ sidebarStreams NOTIFY sidebarStreamsChanged)
    // Issue #100: the reference sidebar state machine (loading card, blocking
    // error card, partial-refresh banner, five empty states) measured in
    // docs/NATIVE_DESKTOP_SIDEBAR_STATES.md.
    Q_PROPERTY(QVariantMap sidebarCatalogState READ sidebarCatalogState NOTIFY sidebarCatalogStateChanged)
    Q_PROPERTY(QStringList expandedSidebarStreams READ expandedSidebarStreams NOTIFY sidebarExpansionChanged)
    Q_PROPERTY(QString selectedFolder READ selectedFolder NOTIFY foldersChanged)
    // Issue #84: folder create/rename/delete (docs/NATIVE_DESKTOP_FOLDERS.md).
    // Single-flight mutation projection {kind, pending, error, ticket} over
    // the rail CRUD, guarded by owner + API generation + ticket exactly like
    // the ownStatus update above.
    Q_PROPERTY(QVariantMap folderMutation READ folderMutation NOTIFY folderMutationChanged)
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
    Q_PROPERTY(QVariantMap outgoingSend READ outgoingSend NOTIFY stateChanged)
    Q_PROPERTY(QString editingMessageId READ editingMessageId NOTIFY editSessionChanged)
    Q_PROPERTY(QString editSessionKey READ editSessionKey NOTIFY editSessionChanged)
    Q_PROPERTY(bool canSaveEdit READ canSaveEdit NOTIFY editSessionChanged)
    Q_PROPERTY(QVariantList editAttachments READ editAttachments NOTIFY editSessionChanged)
    Q_PROPERTY(QString replyMessageId READ replyMessageId NOTIFY replySessionChanged)
    Q_PROPERTY(QVariantList replyTabs READ replyTabs NOTIFY replySessionChanged)
    Q_PROPERTY(QString replyAuthor READ replyAuthor NOTIFY replySessionChanged)
    Q_PROPERTY(QString replyExcerpt READ replyExcerpt NOTIFY replySessionChanged)
    // Quote/reply media preview (issue #217, docs/NATIVE_DESKTOP_QUOTE_MEDIA_THUMBNAILS.md).
    Q_PROPERTY(QString replyMediaUrn READ replyMediaUrn NOTIFY replySessionChanged)
    Q_PROPERTY(QString replyMediaKind READ replyMediaKind NOTIFY replySessionChanged)
    Q_PROPERTY(QString replyMediaSummary READ replyMediaSummary NOTIFY replySessionChanged)
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
    // Issue #165: armed state of the auth idle guard (emitted on every
    // arm/cancel transition so QML bindings can never go stale).
    Q_PROPERTY(bool authGuardActive READ isAuthGuardActive NOTIFY authGuardActiveChanged)
    // Issue #134: verified session user for profile self/other decisions.
    Q_PROPERTY(QString currentUserId READ currentUserId NOTIFY stateChanged)
    Q_PROPERTY(bool canAttach READ canAttach NOTIFY stateChanged)
    Q_PROPERTY(MediaViewerModel *mediaViewer READ mediaViewer CONSTANT)
    Q_PROPERTY(bool topicClosed READ topicClosed NOTIFY stateChanged)
    Q_PROPERTY(bool canReopenTopic READ canReopenTopic NOTIFY stateChanged)
    Q_PROPERTY(bool fileBusy READ fileBusy NOTIFY stateChanged)
    Q_PROPERTY(QVariantList uploadEntries READ uploadEntries NOTIFY stateChanged)
    Q_PROPERTY(bool uploadPending READ uploadPending NOTIFY stateChanged)
    Q_PROPERTY(QString notice READ notice NOTIFY stateChanged)
    Q_PROPERTY(bool draftsVisible READ draftsVisible NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap draftPage READ draftPage NOTIFY stateChanged)
    Q_PROPERTY(bool mentionsVisible READ mentionsVisible NOTIFY stateChanged)
    Q_PROPERTY(QString mentionsFilter READ mentionsFilter NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap mentionsPage READ mentionsPage NOTIFY stateChanged)
    Q_PROPERTY(bool inboxVisible READ inboxVisible NOTIFY stateChanged)
    Q_PROPERTY(QString section READ section NOTIFY stateChanged)
    // Issue #46: the section that was active before the licenses page opened
    // (the reference page's "history back" surface; "chat" means no history).
    Q_PROPERTY(QString sectionBeforeLicenses READ sectionBeforeLicenses NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap inboxPage READ inboxPage NOTIFY stateChanged)
    Q_PROPERTY(bool feedVisible READ feedVisible NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap feedPage READ feedPage NOTIFY stateChanged)
    // Activity panel (#101, docs/NATIVE_DESKTOP_ACTIVITY_PANEL.md): open/closed
    // state persisted per workspace session, plus the compact-rail counts.
    // Counts use -1 for "not known yet" (the reference null): no badge renders.
    Q_PROPERTY(bool activityOpen READ activityOpen WRITE setActivityOpen NOTIFY activityOpenChanged)
    Q_PROPERTY(int activityInboxCount READ activityInboxCount NOTIFY foldersChanged)
    Q_PROPERTY(int activityMentionsCount READ activityMentionsCount NOTIFY stateChanged)
    // Activity starred page (#41, docs/NATIVE_DESKTOP_ACTIVITY_STARRED.md).
    Q_PROPERTY(bool starredVisible READ starredVisible NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap starredPage READ starredPage NOTIFY stateChanged)
    // Activity reactions unsupported state (#43, addendum to
    // docs/NATIVE_DESKTOP_ACTIVITY_STARRED.md): route-only, no data, no chip.
    Q_PROPERTY(bool reactionsVisible READ reactionsVisible NOTIFY stateChanged)
    // Favorites self-chat (#70, docs/NATIVE_DESKTOP_FAVORITES_SELF_CHAT.md):
    // the "Personal notes" stream of the verified user, opened only through
    // the Favorites chip; status is "" | "loading" | "ready" | "error".
    Q_PROPERTY(bool favoritesVisible READ favoritesVisible NOTIFY stateChanged)
    Q_PROPERTY(QString favoritesStatus READ favoritesStatus NOTIFY stateChanged)
    Q_PROPERTY(QString favoritesError READ favoritesError NOTIFY stateChanged)
    Q_PROPERTY(bool cacheStale READ cacheStale NOTIFY stateChanged)
    Q_PROPERTY(QString anchorMessageId READ anchorMessageId NOTIFY anchorChanged)
    // Issue #107 (docs/NATIVE_DESKTOP_ANCHOR_TRANSITION.md): visibility for the
    // reference anchor transition layer (workspace_ui 61dacef7
    // workspace-message-anchor-transition.ui.tsx). Pending drives the skeleton
    // preview zones; the error text drives the retryable failure alert.
    Q_PROPERTY(bool anchorTransitionPending READ anchorTransitionPending NOTIFY anchorChanged)
    Q_PROPERTY(QString anchorNavigationError READ anchorNavigationError NOTIFY anchorNavigationErrorChanged)
    Q_PROPERTY(QVariantMap conversationOpening READ conversationOpening NOTIFY stateChanged)
public:
    explicit NativeController(ApiClient *api, Session *session, QObject *parent = nullptr);
    ~NativeController() override;
    void setRealtimeMode(bool enabled) { m_realtimeMode = enabled; if (enabled) m_poll.stop(); }
    void useRealtime(RealtimeClient *client);
    // Power state (issue #78, docs/NATIVE_DESKTOP_POWER_STATE.md): resume
    // reconnects the realtime link immediately and a battery source widens
    // the presence heartbeat cadence.
    void setPowerWatcher(PowerWatcher *watcher);
    void setOnBattery(bool onBattery);
    bool isOnBattery() const { return m_onBattery; }
    int presenceHeartbeatIntervalMs() const;
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
    // Issue #244 (docs/NATIVE_DESKTOP_E2EE_KEY_MANAGEMENT.md): key screens of
    // the protected drawer. Saved-keys inventory over the durable vault with
    // the reference labels, reissue (announce a fresh generation) and delete
    // local key; the request-current-key recovery reuses requestChatKey().
    Q_PROPERTY(QVariantMap chatKeyInventory READ chatKeyInventory NOTIFY stateChanged)
    QVariantMap chatKeyInventory() const;
    Q_INVOKABLE void refreshChatLocalKeys();
    Q_INVOKABLE void reissueChatKey();
    Q_INVOKABLE void deleteChatLocalKey(const QString &keyUuid);
    // Issue #244 review 4577108887: recovery protocol for rotation stages
    // whose announcement never lands. A dispatch-attempted stage whose GET
    // rechecks keep answering "absent" saturates its durable absence
    // counter and becomes DORMANT: the staged key stays quarantined (never
    // destroyed) and keeps its stage-budget slot, because absence alone is
    // never proof that a dispatched POST can no longer land. What automatic
    // recovery may never decide, the user may: unresolvedRotationStages()
    // lists the device's pending stages (identity and bookkeeping only,
    // never key material); checkRotationStage() re-checks one and re-adopts
    // it when the announcement proves to exist (positive proof frees the
    // pin); discardRotationStage() is the informed explicit decision that
    // frees the pin without any proof, and requires confirmed == true.
    Q_INVOKABLE QJsonArray unresolvedRotationStages() const;
    Q_INVOKABLE void checkRotationStage(const QString &streamUuid, const QString &keyUuid);
    Q_INVOKABLE void discardRotationStage(const QString &streamUuid, const QString &keyUuid,
        bool confirmed);
    // Joined-style date label shared by the key screens (profileDetails'
    // joined rule: "MMM d, yyyy" en / "d MMM yyyy г." ru == formatDateJoined).
    Q_INVOKABLE static QString formatChatKeyDate(const QString &isoDate, const QString &localeName);
    // Issue #221: per-message decrypt Retry on the encrypted message error row.
    Q_INVOKABLE void retryMessageDecrypt(const QString &messageUuid);
    QStringList decryptRetryPendingUuids() const;
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
    // Issue #89: chat header caption counts (docs/NATIVE_DESKTOP_CHAT_HEADER_COUNTERS.md).
    int chatHeaderMemberCount() const;
    int chatHeaderOnlineCount() const;
    // Issue #438: visibility gate for the header's member-page fetches.
    bool chatHeaderVisible() const { return m_chatHeaderVisible; }
    void setChatHeaderVisible(bool visible);
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
    Q_INVOKABLE void archiveChat(const QString &id);
    Q_INVOKABLE void renameChat(const QString &id, const QString &name);
    Q_INVOKABLE void markChatRead(const QString &id);
    // Issue #97 (docs/NATIVE_DESKTOP_SIDEBAR_MUTE_INDICATORS.md): the stream menu
    // level switch for any stream, not just the open one; the write contract is
    // the chat-info switch's one (same endpoint, payload and canonical answer).
    Q_INVOKABLE void setStreamNotificationMode(const QString &id, const QString &mode);
    Q_INVOKABLE bool streamNotificationBusy(const QString &id) const;
    Q_INVOKABLE QVariantMap chatActionState(const QString &id) const;
    QVariantList streams() const;
    QVariantList folders() const;
    QVariantList sidebarStreams() const;
    QVariantMap sidebarCatalogState() const;
    QStringList expandedSidebarStreams() const { return m_expandedSidebarStreams; }
    Q_INVOKABLE QVariantMap sidebarTopicState(const QString &stream, bool includeRows) const;
    Q_INVOKABLE void requestSidebarTopics(const QString &stream);
    Q_INVOKABLE void toggleSidebarStream(const QString &stream);

    Q_INVOKABLE void setSidebarSearchQuery(const QString &query);
    QString selectedFolder() const;
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
    bool anchorTransitionPending() const
    { return m_anchorLinkPending && !m_pendingAnchorMessage.isEmpty(); }
    QString anchorNavigationError() const { return m_anchorNavigationError; }
    QVariantMap conversationOpening() const;
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
    // Activity panel (#101). Inbox count mirrors the reference All-folder
    // unread (selectMessengerSidebarActivityCounts); the mentions count is
    // the unread-mentions index size, null until the index is loaded
    // (sidebar-workspace-activity.ui.tsx owner-key gate).
    bool activityOpen() const { return m_activityOpen; }
    void setActivityOpen(bool open);
    int activityInboxCount() const;
    int activityMentionsCount() const;
    // count-format.lib.ts formatCount with UNREAD_COUNT_DISPLAY_LIMIT = 1000.
    Q_INVOKABLE QString activityBadgeLabel(int count) const;
    // Top-bar application section: "chat", "mail", "calendar" or "calls";
    // switching is exclusive and any conversation navigation returns to
    // "chat" (issues #35, #36, #37).
    QString section() const { return m_section; }
    QString sectionBeforeLicenses() const { return m_sectionBeforeLicenses; }
    QVariantMap inboxPage() const;
    // Project feed page (#38, docs/NATIVE_DESKTOP_FEED.md): cross-chat
    // "All messages" of the current project, marker-paginated.
    bool feedVisible() const { return m_feedVisible; }
    QVariantMap feedPage() const;
    // Activity starred page (#41, docs/NATIVE_DESKTOP_ACTIVITY_STARRED.md).
    bool starredVisible() const { return m_starredVisible; }
    QVariantMap starredPage() const;
    bool reactionsVisible() const { return m_reactionsVisible; }
    bool favoritesVisible() const { return m_favoritesVisible; }
    QString favoritesStatus() const { return m_favoritesStatus; }
    QString favoritesError() const { return m_favoritesError; }

    Q_INVOKABLE void login(const QString &server, const QString &user,
                           const QString &password, const QString &otp);
    // Login wizard organization step (docs/NATIVE_DESKTOP_LOGIN.md).
    QString loginStep() const { return m_loginStep; }
    QString projectPreselection() const;
    QString loginUsername() const { return m_session->loginName(); }
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
    Q_INVOKABLE void submitOtp(const QString &otp);
    Q_INVOKABLE void backToCredentials();
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
    // Issue #165: the inactivity guard is armed only while a live session
    // exists (docs/NATIVE_DESKTOP_AUTH_IDLE_TIMEOUT.md). The accessor is the
    // test seam for the injected clock and the check cadence.
    bool isAuthGuardActive() const;
    NativeAuthGuard *authGuard() const { return m_authGuard; }
    Q_INVOKABLE void selectStream(const QString &id);
    Q_INVOKABLE void selectTopic(const QString &id);
    // Issue #83: resolve a rail row by its stable key or UUID. Selecting the
    // effective folder is a no-op; a real change emits folderSelected() so the
    // view can clear the sidebar search exactly like the reference.
    Q_INVOKABLE void selectFolder(const QString &key);
    // Issue #84 will wire the creation dialog; the rail button only announces.
    Q_INVOKABLE void requestAddFolder();
    // Issue #84 (docs/NATIVE_DESKTOP_FOLDERS.md): rail CRUD following the
    // reference folder-rail.ui.tsx contract - POST /folders/
    // {title, background_color_value}, PUT /folders/<uuid> with the same
    // body, DELETE /folders/<uuid>. Only "created" rows are mutable; the
    // result updates the rail incrementally without a full /folders/ refetch.
    QVariantMap folderMutation() const;
    Q_INVOKABLE void createFolder(const QString &name, int backgroundColor);
    Q_INVOKABLE void renameFolder(const QString &folderUuid, const QString &name, int backgroundColor);
    Q_INVOKABLE void deleteFolder(const QString &folderUuid);
    Q_INVOKABLE void openTopic(const QString &id, const QString &streamHint = {});
    // Window focus mirror for desktop notifications (Electron sends
    // window:activity from the focus/blur handlers in main.ts).
    Q_INVOKABLE void setWindowFocused(bool focused);
    // Window visibility mirror (issue #78): hidden windows take the hidden
    // presence tier. The minimized seam exists because Qt keeps isVisible()
    // true for a minimized window while the reference ladder treats
    // minimized as hidden (fix-round-2, cid 4576526934).
    Q_INVOKABLE void setWindowVisible(bool visible);
    Q_INVOKABLE void setWindowMinimized(bool minimized);
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
    // Activity starred page (#41): one starred collection, bottom-edge
    // pagination and the message-route jump, per the reference contract.
    Q_INVOKABLE void showStarred();
    Q_INVOKABLE void closeStarred();
    Q_INVOKABLE void loadMoreStarred();
    Q_INVOKABLE void retryStarred();
    Q_INVOKABLE void openStarredMessage(const QString &id);
    // Favorites self-chat page (#70): ensure/find the "Personal notes"
    // direct stream with self and open it with the favorites presentation
    // (workspace-self-chat.hook.ts, chat-page.ui.tsx FavoritesPage).
    Q_INVOKABLE void showFavorites();
    Q_INVOKABLE void closeFavorites();
    Q_INVOKABLE void retryFavorites();
    Q_INVOKABLE void showInbox();
    Q_INVOKABLE void showSection(const QString &name);
    // Issue #46: the Licenses page dataset (docs/NATIVE_DESKTOP_LICENSES.md),
    // read from the build-time Qt resource as one JSON string.
    Q_INVOKABLE QString licensesDatasetJson() const;
    // Issue #50: the Diagnostics page surface (docs/NATIVE_DESKTOP_DIAGNOSTICS_PAGE.md):
    // the live dataset object, the pretty-printed snapshot JSON for the copy
    // action and the advanced block, the connection-report subset, the live
    // overall status (status pill) and one transport ping at a time
    // (pingFinished carries the latency).
    Q_INVOKABLE QVariantMap diagnosticsSnapshot() const;
    Q_INVOKABLE QString diagnosticsSnapshotJson() const;
    Q_INVOKABLE QString diagnosticsConnectionReportJson() const;
    // Issue #52: the memory panel poll source (3 s QTimer in the page calls
    // this while the section is visible). Same provider dataset that fills
    // the snapshot's memory key (docs/NATIVE_DESKTOP_DIAGNOSTICS_PAGE.md,
    // "Memory panel" section).
    Q_INVOKABLE QVariantMap diagnosticsMemorySnapshot(bool resetCpu = false) const;
    // Test seam: points the memory provider at a fixture proc root so window
    // tests drive exact values (mirrors NativeLogging::setProcRoot).
    void setDiagnosticsMemoryProcRoot(const QString &procRoot);
    Q_INVOKABLE QString diagnosticsOverallStatus() const;
    Q_INVOKABLE void pingServer();
    // Issue #51: the in-memory session log ring buffer feeding the log
    // viewer modal (docs/NATIVE_DESKTOP_DIAGNOSTICS_PAGE.md, modal section).
    // appendSessionLog is the logger.ts appendBufferedLog analogue; the
    // filtered/entries invokables answer the modal's list queries.
    Q_INVOKABLE void appendSessionLog(const QString &level, const QString &scope,
        const QString &message, const QVariantMap &data = QVariantMap());
    Q_INVOKABLE QVariantList diagnosticsSessionLogEntries() const;
    Q_INVOKABLE QVariantList diagnosticsSessionLogsFiltered(const QString &levelFilter,
        const QString &sourceFilter, const QString &scopeFilter,
        const QString &runtimeFilter, const QString &searchQuery) const;
    Q_INVOKABLE void clearSessionLog();
    Q_INVOKABLE QString diagnosticsLogsExportFilename() const;
    Q_INVOKABLE QString diagnosticsLogsExportJson() const;
    Q_INVOKABLE bool exportSessionLog(const QString &target) const;
    Q_INVOKABLE void closeInbox();
    Q_INVOKABLE void openInboxRow(const QString &stream, const QString &topic);
    Q_INVOKABLE void showReactions();
    Q_INVOKABLE void closeReactions();
    Q_INVOKABLE void showFeed();
    Q_INVOKABLE void closeFeed();
    Q_INVOKABLE void loadMoreFeed();
    Q_INVOKABLE void retryFeed();
    Q_INVOKABLE void openFeedRow(const QString &stream, const QString &topic);
    Q_INVOKABLE void nextDraftPage();
    Q_INVOKABLE void previousDraftPage();
    Q_INVOKABLE void openDraft(const QString &id);
    Q_INVOKABLE void deleteDraft(const QString &id);
    Q_INVOKABLE void resolveDraft(const QString &id, const QString &action);
    Q_INVOKABLE void loadOlder();
    Q_INVOKABLE void loadNewer();
    Q_INVOKABLE void prepareConversationOpening(const QString &savedAnchor = {});
    Q_INVOKABLE void cancelConversationOpening();
    Q_INVOKABLE void loadLatestMessages();
    // Issue #156 (docs/NATIVE_DESKTOP_HISTORY_PREFETCH.md): the view reports
    // user-attributed scroll geometry and the controller owns the reserve
    // policy for prefetching the next older page, so the trigger threshold,
    // the in-flight coalescing and the end-of-history guard are observable
    // and testable without a window. distanceFromTopPx is the reference
    // scrollTop analogue (loaded content above the viewport top),
    // viewportHeightPx the visible list height.
    Q_INVOKABLE void noteScrollReserve(qreal distanceFromTopPx, qreal viewportHeightPx);
    // Issue #212 review: re-arm-only variant of noteScrollReserve, called by
    // the view after a programmatic viewport restore (the positioning gate
    // suppresses regular notes during the swap). It can arm but never starts
    // a request, so programmatic movement alone keeps user attribution.
    Q_INVOKABLE void noteScrollRearm(qreal distanceFromTopPx, qreal viewportHeightPx);
    Q_INVOKABLE void reopenTopic();
    Q_INVOKABLE bool canStartDirectChatWith(const QString &peer) const;
    Q_INVOKABLE void startDirectChat(const QString &peer);
    Q_INVOKABLE void checkDirectChat();
    Q_INVOKABLE void retryDirectChat();
    Q_INVOKABLE void createChannel(const QString &name, const QString &description,
        bool inviteOnly, bool announce, const QVariantList &memberUuids, bool encrypted = false);
    Q_INVOKABLE void checkChannelCreation();
    Q_INVOKABLE void retryChannelMembers();
    // Review finding 4576823403 (medium, R7): dormant-quarantined create
    // stages keep their group key forever, but each one also holds a slot of
    // the vault's bounded 32-record create budget, and automatic recovery
    // may NEVER infer a terminal outcome from absence evidence alone — so
    // the budget must never be PERMANENTLY starved. Safe exit: a human is
    // the terminal authority. unresolvedChannelCreateStages() lists every
    // unresolved durable attempt (attempt uuid, staged key uuid, created
    // timestamp, dispatch phase, absence evidence, dormant flag — never key
    // material) so a UI can present them; resolveChannelCreateStage(attempt)
    // is the explicit user-invokable CHECK: a stream that now exists with a
    // verifying encrypted echo commits (re-adopts) the key and frees the
    // budget slot (terminal via positive proof), while a 404 is NOT terminal
    // (a delayed POST can still land) — the stage and its key are kept and
    // the answer is "not found yet".
    // Outcomes arrive via channelCreateStageResolved. Surfaced on the
    // controller so any UI can drive it; a full settings page is out of
    // scope for this slice.
    Q_INVOKABLE QJsonArray unresolvedChannelCreateStages() const;
    Q_INVOKABLE void resolveChannelCreateStage(const QString &attempt);
    // Review finding 4576938727 (critical, R8): because a check can NEVER
    // prove a terminal negative (a delayed POST may land arbitrarily late),
    // the only way for the user to stop awaiting an unresolved attempt and
    // free its create-budget slot is an informed explicit key-discard: the
    // channel may still appear later and would then be permanently
    // unreadable. discardChannelCreateStage only acts when the caller
    // carries the user's explicit confirmation (a UI dialog that names that
    // risk); unconfirmed calls change nothing. Outcomes arrive via
    // channelCreateStageResolved.
    Q_INVOKABLE void discardChannelCreateStage(const QString &attempt, bool confirmed);
    Q_INVOKABLE void refreshChannelInfo();
    // Issue #89: (re)loads the binding members of the currently selected
    // non-direct stream for the header caption; other selections clear the
    // projection instead.
    Q_INVOKABLE void refreshChatHeaderMembers();
    Q_INVOKABLE void setChannelInfoNotificationMode(const QString &mode);
    Q_INVOKABLE void removeChannelInfoMember(const QString &bindingUuid);
    Q_INVOKABLE void addChannelInfoMembers(const QStringList &userUuids);
    Q_INVOKABLE void resetChannelInfoAddState();
    // ---- Issue #134 profile drawer ----
    Q_INVOKABLE QVariantMap profileSnapshot(const QString &userUuid) const;
    Q_INVOKABLE QVariantList profileDetails(const QString &userUuid, const QString &localeName) const;
    Q_INVOKABLE QString buildProfileShareLink(const QString &userUuid) const;
    // Clipboard writes stay in C++ (message copy precedent).
    Q_INVOKABLE bool copyToClipboard(const QString &text) const;
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
    QVariantMap outgoingSend() const;
    Q_INVOKABLE void discardPendingSend();
    Q_INVOKABLE void checkPendingSend();
    Q_INVOKABLE void retryPendingSend();
    Q_INVOKABLE void copyMessage(const QString &id);
    Q_INVOKABLE QString messageLink(const QString &id) const;
    Q_INVOKABLE void copyMessageLink(const QString &id);
    // Issue #79 (docs/NATIVE_DESKTOP_CLIPBOARD_IMAGE.md): counterpart of the
    // Electron clipboard:writeImage handler (main.ts:1054-1067, cap main.ts:50).
    static constexpr qint64 ClipboardImageBytesLimit = 64 * 1024 * 1024;
    static bool clipboardImageBytes(const QByteArray &bytes);
    // Fix round R1 (cid=4577117364, cid=4577117373): both copy paths stage the
    // payload to a file first, so the write path reads it back through this
    // bounded helper - at most ClipboardImageBytesLimit + 1 bytes - and an
    // oversized file is rejected by clipboardImageBytes without ever being
    // materialised whole in memory.
    static QByteArray readClipboardBounded(QIODevice &source);
    Q_INVOKABLE bool copyMessageImage(const QString &id);
    Q_INVOKABLE void openMessageLink(const QString &url);
    // Issue #107: the failure alert's Retry re-issues the same lookup + bounded
    // walk; the inline scroll-to-bottom escapes to the conversation tail and
    // retires the failed presentation (reference onTailNavigationRequested).
    Q_INVOKABLE void retryAnchorNavigation();
    Q_INVOKABLE void dismissAnchorNavigation();
    bool profileLinkFailed() const {
        return m_profileLinkFailed && m_profileLinkFailureSelection == m_selection
            && m_profileLinkFailureInbox == m_inboxVisible && m_profileLinkFailureMentions == m_mentionsVisible
            && m_profileLinkFailureDrafts == m_draftsVisible;
    }
    Q_INVOKABLE void dismissProfileLinkFailure();
    Q_INVOKABLE void starMessage(const QString &id);
    Q_INVOKABLE void deleteMessage(const QString &id);
    Q_INVOKABLE void editMessage(const QString &id, const QString &text);
    // Issue #7 multi-forward (docs/NATIVE_DESKTOP_MULTI_FORWARD.md): forward a
    // batch of source uuids as one snapshot message into a target
    // conversation. Duplicates inside one batch collapse here; an
    // unconfirmed batch is confirmed GET-only with checkForwardStatus() and
    // never re-sent, mirroring the canonical Check pattern.
    Q_INVOKABLE bool canForwardMessages() const;
    Q_INVOKABLE bool forwardPending() const;
    Q_INVOKABLE void forwardMessages(const QStringList &messageUuids, const QString &streamUuid,
        const QString &topicUuid);
    Q_INVOKABLE void checkForwardStatus();
    // Reactive view for the forward modal (issue #7 slice 2): busy state, the
    // unconfirmed "Check never re-sends" state, and the last batch error.
    Q_PROPERTY(bool forwardPending READ forwardPending NOTIFY forwardChanged)
    Q_PROPERTY(bool forwardAwaitingCheck READ forwardAwaitingCheck NOTIFY forwardChanged)
    Q_PROPERTY(QString forwardError READ forwardError NOTIFY forwardChanged)
    bool forwardAwaitingCheck() const { return m_forwardIntent.pending && !m_forwardIntent.busy; }
    QString forwardError() const { return m_forwardError; }
    // Topic options for an arbitrary destination stream in the forward picker
    // (reference chat-page forward modal lists each channel's topics). GET
    // /stream_topics/ only; options cache per stream and reload on request.
    Q_INVOKABLE void fetchForwardTopics(const QString &streamUuid, bool refresh = false);
    Q_INVOKABLE QVariantList forwardTopicOptions(const QString &streamUuid) const;
    Q_INVOKABLE bool forwardTopicsPending(const QString &streamUuid) const;
    Q_INVOKABLE bool beginEditMessage(const QString &id);
    Q_INVOKABLE void cancelEditMessage();
    Q_INVOKABLE void submitEditMessage();
    Q_INVOKABLE void removeEditAttachment(const QString &id);
    QVariantList editAttachments() const;
    Q_INVOKABLE void editLastMessage();
    QString editingMessageId() const { return m_editingMessage; }
    QString editSessionKey() const { return m_editingMessage.isEmpty() ? QString{} : QString::number(m_editSession); }
    bool canSaveEdit() const;
    // Reply session (docs/NATIVE_DESKTOP_REPLIES.md + docs/NATIVE_DESKTOP_MULTI_REPLY_TABS.md):
    // a tab session of concurrent quote tabs; beginReply keeps the reference
    // replace-the-active-tab semantics, addReplyTab appends a new tab. The
    // composer draft mirrors the active tab's answer while the session is open,
    // and send() composes every "urn:quote" block plus answer in tab order.
    // selectedText carries the bubble-menu selection (issue #218); a selection
    // that is whitespace-only or taken from a body containing encrypted
    // fragments is dropped, falling back to the whole-message quote exactly
    // like chat-page-workspace.ui.tsx:2747-2751 at workspace_ui 230e02b8.
    Q_INVOKABLE bool beginReply(const QString &id, const QString &selectedText = {});
    Q_INVOKABLE bool addReplyTab(const QString &id, const QString &selectedText = {});
    Q_INVOKABLE bool selectReplyTab(const QString &tabId);
    Q_INVOKABLE void removeReplyTab(const QString &tabId);
    Q_INVOKABLE bool reorderReplyTab(const QString &tabId, int destinationIndex);
    Q_INVOKABLE void cancelReply();
    Q_INVOKABLE void openMessageInChat(const QString &id);
    QString replyMessageId() const;
    QString replyAuthor() const;
    QString replyExcerpt() const;
    // Media preview of the quoted body: first image wins, else first video
    // (issue #217). Empty when the quote carries no media reference.
    QString replyMediaUrn() const;
    QString replyMediaKind() const;
    QString replyMediaSummary() const;
    Q_INVOKABLE QVariantMap mediaPreviewReference(const QString &markdown) const;
    QVariantList replyTabs() const;
    Q_INVOKABLE void markMessagesReadUpTo(const QString &id);
    Q_INVOKABLE void markVisibleMessagesRead(const QVariantList &ids);
    Q_INVOKABLE void markTopicReadAtBottom();
    Q_INVOKABLE void toggleReaction(const QString &id, const QString &emoji);
    // ---- Issue #214 polls foundation (docs/NATIVE_DESKTOP_POLLS_FOUNDATION.md) ----
    // REST action surface of the poll contract; #215/#216 render them. Failures
    // only emit pollActionFailed with a technical detail - user-visible strings
    // arrive with the UI tickets.
    Q_INVOKABLE bool canCreatePoll() const;
    // Issue #215: reactive QML gate for the composer poll trigger; same rule as
    // the invokable, exposed so bindings re-evaluate with the selection.
    Q_PROPERTY(bool pollCreationAllowed READ canCreatePoll NOTIFY stateChanged)
    Q_PROPERTY(QString pollCreationOwnerKey READ creationOwnerKey NOTIFY stateChanged)
    // A UI caller supplies a token unique to this controller for each submission.
    Q_INVOKABLE void sendPoll(const QVariantList &questions, bool multipleAnswers, bool isAnonymous,
        bool allowRevote, const QString &resultsVisibility, bool allowAddingOptions,
        const QString &closesAt = QString(), const QString &operation = QString());
    Q_INVOKABLE void votePollOption(const QString &messageUuid, const QString &questionUuid,
        const QString &optionUuid, bool selected);
    Q_INVOKABLE void closePoll(const QString &messageUuid);
    Q_INVOKABLE void updatePollSettings(const QString &messageUuid, const QString &closesAt,
        bool allowAddingOptions);
    Q_INVOKABLE void addPollOption(const QString &messageUuid, const QString &questionUuid,
        const QString &optionUuid, const QString &optionText);
    Q_INVOKABLE void loadPollVoters(const QString &messageUuid, const QString &questionUuid,
        const QString &optionUuid, const QString &pageMarker = QString(), int pageLimit = 0);
    // Issue #216: locale rendering of poll deadline/closed dates
    // (workspace-message-poll.ui.tsx:47-49 in the reference).
    Q_INVOKABLE QString formatPollDate(const QString &isoDateTime) const;
    Q_INVOKABLE void attachFiles(const QVariantList &localUrls);
    QVariantList uploadEntries() const;
    bool uploadPending() const;
    Q_INVOKABLE bool pasteAttachments();
    Q_PROPERTY(bool clipboardFilesPreparing READ clipboardFilesPreparing NOTIFY stateChanged)
    Q_PROPERTY(QVariantList clipboardFilesPreview READ clipboardFilesPreview NOTIFY stateChanged)
    // 0 = no local files, 1 = preparing preview, 2 = files rejected with an error.
    Q_INVOKABLE int prepareClipboardFiles();
    Q_INVOKABLE void attachClipboardFiles();
    Q_INVOKABLE void cancelClipboardFiles();
    Q_INVOKABLE void dismissClipboardFiles();
    bool clipboardFilesPreparing() const { return m_clipboardFilesPreparing; }
    QVariantList clipboardFilesPreview() const { return m_clipboardFilesPreview; }
    Q_INVOKABLE void removeUpload(const QString &id);
    Q_INVOKABLE void removeUploadFromText(const QString &id);
    Q_INVOKABLE void retryUpload(const QString &id);
    Q_INVOKABLE void downloadFile(const QString &id, const QUrl &destination);
    QVariantList downloadEntries() const;
    Q_INVOKABLE void cancelDownload(const QString &id);
    Q_INVOKABLE void dismissDownload(const QString &id);
    Q_INVOKABLE void clearDownloads();
    Q_INVOKABLE void retryDownload(const QString &id);
    Q_INVOKABLE void openDownload(const QString &id);
    Q_INVOKABLE void revealDownload(const QString &id);

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
    Q_INVOKABLE QVariantList composerReferences(const QString &query) const;
    QString replaceComposerReferenceLinks(const QString &text) const;
    Q_INVOKABLE QString composerReferenceClipboardText() const;
    Q_INVOKABLE void openStreamReference(const QString &id);
    Q_INVOKABLE QVariantList mentionSuggestions(const QString &query) const;
    Q_INVOKABLE bool composerMentionsEnabled() const;
    Q_INVOKABLE void recordMentionPick(const QString &userUuid);

signals:
    void otpCodeRejected();
    void sidebarExpansionChanged();
    void activityOpenChanged();
    // Empty stream means a placement change may have affected several streams.
    void sidebarTopicsChanged(const QString &stream);
    void profileLinkStarted();
    void profileLinkOpened(const QString &userUuid);
    void downloadsChanged();
    void downloadDuplicateRequested();
    void chatKeyStoreOpenRequested();
    // Issue #244 review 4577108887: settles a user-invoked rotation-stage
    // check/discard with the outcome ("resolved", "released", "discarded"
    // or "unresolved") and a user-facing message naming what happened.
    void rotationStageResolved(const QString &streamUuid, const QString &keyUuid,
        const QString &outcome, const QString &message);
    void decryptRetryStateChanged();
    void archivedChatsChanged();
    // Issue #50: one finished Diagnostics ping (ok + latency ms); the page
    // mirrors the reference ping reset timer (3 s, logs-page.ui.tsx:238-242).
    void pingFinished(bool ok, int latencyMs);
    // Issue #51: the session log ring buffer changed (append or clear); the
    // log viewer modal re-reads its rows on this signal (reference
    // subscribeLogHistory).
    void sessionLogChanged();
    void stateChanged();
    void mentionMembershipChanged();
    void channelInfoAddSettledChanged();
    void ownProfileSettledChanged();
    void ownAvatarSettledChanged();
    // Issue #134: the profile page reopens the avatar dialog when a mutation
    // fails (reference edit-avatar-dialog contract); every mutation of
    // m_ownAvatarError must announce itself so the QML Connections handler
    // sees it (stateChanged alone does not name this transition).
    void ownAvatarErrorChanged();
    void anchorChanged();
    void anchorNavigationErrorChanged();
    void conversationMarkedRead(const QString &streamUuid, const QString &topicUuid);
    void savedAccountsChanged();
    void sessionSwitcherChanged();
    void authGuardActiveChanged();
    void draftChanged();
    void editSessionChanged();
    void replySessionChanged();
    void streamsChanged();
    void foldersChanged();
    void sidebarStreamsChanged();
    void sidebarCatalogStateChanged();
    void folderSelected();
    void folderAddRequested();
    void folderMutationChanged();
    void callCryptoStateChanged();
    void topicsChanged();
    void messagesChanged();
    void topicCreated(QString stream, QString topic);
    void channelCreated(QString stream, QString defaultTopic);
    // R7 (finding 4576823403) / R8 (finding 4576938727): terminal result of
    // one explicit resolveChannelCreateStage or discardChannelCreateStage
    // call: "committed" (the stream exists and the encrypted echo verified —
    // the staged key was adopted and the stage closed), "discarded" (an
    // informed explicit discard: the staged key record was removed by the
    // warned user's decision and its budget slot freed), "released" (a
    // provably never-dispatched stage retired without a network round trip),
    // "unresolved" (nothing conclusive — the stage and its key were kept
    // unchanged; a 404 is never terminal for a check).
    void channelCreateStageResolved(QString attempt, QString outcome, QString message);
    void directChatOpened(QString stream, QString peer, QString defaultTopic);
    void messageSent(QString id, QString stream, QString topic);
    void forwardChanged();
    void forwardTopicsChanged();
    void reactionAdded(QString messageId, QString canonicalEmoji, QString ownerKey, QString stream, QString topic);
    // ---- Issue #214 polls foundation signals ----
    void pollActionFailed(QString messageUuid, QString action, QString detail,
        QString questionUuid);
    // #216 round 3: the response of one action request was applied
    // (poll_state merged). Vote/add carry the affected question uuid so the
    // bubble releases only the lock its own request set - an unrelated poll
    // revision must not release in-flight question locks.
    void pollActionSucceeded(QString messageUuid, QString action, QString questionUuid);
    // Confirmed create notification for observers. The dialog uses the separate
    // operation completion below to distinguish different form lifetimes.
    void pollCreated(QString id, QString stream, QString topic);
    // One dialog submission, distinct from the canonical UUID reused on retry.
    // Empty detail confirms success; interrupted is an uncertain send.
    void pollCreationFinished(QString operation, QString detail);
    void pollVotersLoaded(QString messageUuid, QString questionUuid, QString optionUuid,
        QVariantList voters, QString nextMarker, QString error);
    void snapshotReloaded();
    void snapshotFailed();
    void resynchronizeRequested();

private:
    void applyDeepLink(const QString &url);
    void openProfileShareLink(const QString &user);
    void applyStreamRoute(const QString &org, const QString &project,
                          const QString &stream, const std::optional<QString> &topic);
    void resolveDeepLinkTopic();
    void flushPendingDeepLink(bool settle = true);
    // Mention suggestion sources for mentionSuggestions()/send serialization.
    QVariantList mentionCandidates() const;
    MentionRankingContext mentionRankingContext() const;
    QVector<ComposerMention> composerMentionEntries() const;
    NativePreferences *mentionPreferences() const;
    void applyMessageRoute(const QString &org, const QString &project, const QString &message);
    void setAnchorMessage(const QString &id);
    void clearAnchor(const QString &notice = {});
    void beginAnchorLinkJump(const QString &target);
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
    // Issue #244 review 4576750798: GET-only recheck pass over durable
    // in-flight rotation announcements (see native-chat-key-vault.h).
    void maybeResolveRotationStages();
    void resolveRotationStagePass(const NativeChatKeyVault::Lease &lease,
        QVector<NativeChatKeyVault::RotationStage> stages, quint64 pass);
    // Issue #294: an API-generation bump (token renewal, sign-out, server
    // switch) retires the reissue POST and rotation-resolution GETs without
    // their callbacks ever running; this releases the transient busy and
    // single-flight markers of those retired operations (durable pins stay).
    void interruptChatRotationOperations();
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
    PreparedChatMessage decryptChatMessageRow(const Message &message);
    QSet<QString> m_chatDecryptRetries;
    ChatAuthority m_chatAuthority;
    NativeChatKeyVault::Lease m_chatVaultLease;
    QPointer<NativeCacheService> m_chatCache;
    NativeCache::Lease m_chatCacheLease;
    QString m_chatOwner, m_chatStream, m_chatTopic, m_chatSecurity, m_chatPhase, m_chatError, m_chatRequestKey;
    QString m_chatLocalRequest, m_chatRevocationOwner;
    // Issue #244: cached local-key inventory projection + action pending/error.
    QVariantMap m_chatLocalKeysInventory{{{"state", QStringLiteral("loading")}}};
    bool m_chatReissueBusy = false;
    bool m_rotationResolveActive = false;
    QString m_chatKeyActionError;
    // Issue #244 review 4577108887: the key whose rotation POST is currently
    // in flight (excluded from the unresolved inventory, guarded against
    // user resolve/discard races) and the set of "stream|key" stage ids with
    // a user check in flight (one check per stage at a time).
    QString m_chatReissueKey;
    QSet<QString> m_rotationStageResolving;
    // Issue #294: operation identity for the transient rotation/reissue state,
    // mirroring the #293 create-stage tickets. A completion of a retired
    // operation must never settle away or re-set a newer operation's marker;
    // only the ticket-owning operation may clear it, and an interruption
    // retires the tickets so a dropped callback cannot settle anything later.
    quint64 m_chatReissueOperation = 0; // fences the reissue authority/POST callbacks
    quint64 m_rotationResolvePass = 0;  // fences the automatic resolution pass
    QHash<QString, quint64> m_rotationStageTickets;
    quint64 m_rotationStageTicketCounter = 0;
    // Definitive-absence observations that saturate a dispatched rotation
    // stage into the dormant quarantine (mirrors the accepted create-stage
    // protocol of #234: bounded, never key-destroying).
    static constexpr int maximumRotationStageMissingObservations = 5;
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
        QString encryptionKey;
        QByteArray groupKey;
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
        bool encrypted = false, keyStored = false;
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
    bool commitChannelKey(const QString &owner, const QString &id, quint64 ticket);
    // Review findings 4574928575/4574928581: after a restart the durable
    // pre-dispatch stage is the only copy of a new encrypted channel's key.
    // The first authenticated use of a vault scans the stages and resolves
    // each pending attempt by fetching the stream under its staged identity,
    // verifying the encrypted echo, committing the key, then clearing.
    struct PendingCreateStage { QString attempt, key; QByteArray groupKey; bool dispatchAttempted = true;
        qint64 createdAt = 0; int missingEvidence = 0; };
    void maybeResolveChannelCreateStages();
    void resolveChannelCreateStages(const QString &token);
    void interruptChannelStageOperations();
    void resolveNextChannelCreateStage(QVector<PendingCreateStage> stages, const QString &token,
                                       bool unresolved = false);
    void scheduleChannelStageRecoveryRetry(const QString &token);
    QString m_channelStageToken;
    int m_channelStageRecoveryRetries = 0;
    int m_channelStageRecoveryBackoffStep = 0;
    // R7 (review finding 4576823403): attempts whose user-invokable resolve
    // check is currently in flight — at most one resolve per attempt at a
    // time; a second call for the same attempt is ignored while one runs.
    QSet<QString> m_channelStageResolving;
    // Issue #293: operation tickets for the user-invokable checks. A completion
    // of an interrupted (generation-retired) check must never settle away a
    // newer check's marker; only the ticket-owning operation may clear it.
    QHash<QString, quint64> m_channelStageTickets;
    quint64 m_channelStageTicketCounter = 0;
    // Issue #293: true while the automatic recovery pass waits on a stage GET;
    // an API-generation bump that retires that GET re-arms the abandoned pass.
    bool m_channelStageRecoveryPassInFlight = false;
    static constexpr int maximumChannelStageRecoveryRetries = 5;
    static constexpr int channelStageRecoveryRetryIntervalMs = 1000;
    // Review finding 4575117269: after the fast window is exhausted recovery
    // re-arms with exponential backoff capped at this interval, so a failure
    // that outlives the window heals without a restart and without a tight loop.
    static constexpr qint64 channelStageRecoveryBackoffCapMs = 30000;
    // Review findings 4575672241/4575672242 as re-scoped by 4576535469
    // (critical): a definitive 404 proves absence NOW, never that a
    // dispatched POST can no longer land — the client observes no
    // server-side deadline or cancellation for the attempt — so absence
    // evidence alone is NOT a definitive terminal outcome and may NEVER
    // retire a dispatch-attempted stage: it can hold the only copy of a
    // group key of a creation whose POST is still able to commit. The
    // durable cross-cycle 404 counter instead SATURATES at this value: a
    // stage at saturation is DORMANT — its key stays quarantined inside
    // the vault's bounded MaximumCreateStages budget (never destroyed), the
    // scan keeps re-arming at the bounded backoff above, and if a later
    // scan sees the stream land the verified echo commits the key and
    // frees the slot (re-adoption). Only definitive terminal outcomes
    // retire material: the verified encrypted echo (commit), the live
    // POST's own definite rejection, or the provably-never-dispatched
    // 'prepared' phase reclaim (which never carried a live POST). The
    // counter saturating here also bounds the dormant bookkeeping itself:
    // dormant records never grow and never leave the 32-record budget.
    static constexpr int minimumChannelStageMissingObservations = 5;
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
        quint64 ticket = 0, generation = 0, selection = 0, catalog = 0, restoreTicket = 0;
        QPointer<ApiClient> api;
        QPointer<NativeCacheService> cache;
        NativeCache::Lease lease;
        ApiClient::RequestId request;
        std::optional<Topic> canonical;
        bool active = false, catalogVerified = false, snapshotActive = false;
        bool syncBlocked = false, forgetting = false, automaticRestore = false;
    };
    void interruptTopicOpening();
    bool topicOpeningContext(quint64 ticket) const;
    void beginTopicOpening(const QString &requested, std::optional<Topic> canonical = {}, bool automaticRestore = false);
    void resolveTopicOpeningStream(quint64 ticket);
    void publishTopicOpening(quint64 ticket);
    void finishTopicOpening(quint64 ticket, const QString &error = {}, bool unavailable = false);
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
        bool archived = false;
    };
    bool streamOperationContext(const QString &id, const ArchiveOperation &op) const;
    bool canSetChatArchived(const QString &id, bool archived) const;
    void setChatArchived(const QString &id, bool archived);
    void mutateChat(const QString &id, const QString &name, bool read);
    QHash<QString, ArchiveOperation> m_chatActions;
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
    // ---- Issue #214 polls foundation internals ----
    // Issue #215: one canonical poll creation attempt per owner/conversation. The
    // reference keeps the submitted attempt (chat-page-workspace pollAttemptRef):
    // retrying an unchanged draft must replay the identical canonical client
    // UUIDs, while any content change rotates the attempt.
    struct PollSendAttempt {
        QString signature; // canonical JSON of the submitted draft identity
        QString messageUuid; // client message uuid carried by the create body
        QStringList questionUuids;
        QList<QStringList> optionUuids;
    };
    void finishPollCreation(const QString &operation, const QString &detail);
    void invokePollAction(const QString &messageUuid, const QString &action, const QJsonObject &body);
    bool applyPollMutation(const QString &messageUuid, const QString &action,
        const QJsonObject &body, const ApiResult &result, const ProjectionContext &context);
    static QVariantMap pollRowSnapshot(const Message &message);
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
        quint64 editSession = 0; // zero belongs to the ordinary conversation draft
        QString editReplyTab;
        QString uploadName;
        std::shared_ptr<QTemporaryFile> temporary;
    };
    struct UploadEntry {
        UploadTask task;
        QString owner, status, error, markdown, fileUuid;
        qint64 size = 0, sent = 0, total = -1;
        bool retryable = true;
        bool serverProtected = false;
        quint64 generation = 0, epoch = 0;
        QString alias;
    };
    struct FileOperation {
        QPointer<FileClient> service;
        quint64 operation = 0;
        quint64 editSession = 0;
    };
    struct DownloadEntry {
        QString id, owner, fileUuid, fileName, requestedPath, path, status, error;
        qint64 received = 0, total = -1;
        quint64 intent = 0, retryService = 0;
        bool retrying = false;
        bool active() const { return status == "starting" || status == "downloading"; }
    };
    void startDownload(DownloadEntry entry);
    void finishDownloadRetries(bool success);
    QString uniqueDownloadPath(const QString &path) const;
    QString completedDownloadPath(const QString &id);
    void cancelFileTransfers(const QString &reason = {});
    void enqueueUpload(const QString &path, const QString &name = {}, std::shared_ptr<QTemporaryFile> temporary = {});
    QString clipboardUploadName(const QString &extension, bool image) const;
    void validateQueuedUpload(quint64 intent, quint64 epoch);
    QStringList readyUploadLinks(quint64 session) const;
    QString imageAlias(const UploadEntry &entry) const;
    QString replaceImageAliases(const QString &text, bool serialize) const;
    void cancelEditUploads();
    void removeUploadEntry(quint64 intent, bool notify);
    void discardUploads(const QList<quint64> &intents);
    void discardUploadsOutsideConversation();
    void drainUploads();
    void finishUpload(const UploadTask &task, quint64 generation, quint64 epoch,
                      const FileResult &result);
    void resetReadState();
    void enqueueReadBoundary(const QString &id, bool allowReadAnchor);
    void sendReadRequest(bool confirmation);
    void finishReadRequest(bool success);
    void refreshReadCounters(const Message &boundary);
    void refreshReadStreamCounter(const Message &boundary);
    struct SendIntent {
        QString uuid, text; // Existing ordinary-send representation remains first.
        bool durable = false, restored = false, retired = false, admitted = false, automaticChecked = false;
        qint64 createdAt = 0;
        std::shared_ptr<OutgoingRetirement> retirement;
        QString replyActive;
        QJsonArray replySnapshot;
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
    void synchronizeOutgoing();
    NativeDraftService *outgoingStorage() const;
    bool outgoingReady() const;
    bool outgoingOwned(const SendIntent &intent, bool writing) const;
    bool outgoingContext(const QString &conversation, const QString &id, quint64 ticket) const;
    OutgoingRecord outgoingRecord(const SendIntent &intent) const;
    void admitOutgoing(const QString &conversation, std::function<void()> dispatch);
    void retireOutgoing(const SendIntent &intent, std::function<void()> complete);
    void cancelOutgoingRetirement(const QString &publicId);
    void failOutgoing(const QString &conversation, const QString &id, quint64 ticket, const QString &error);
    void recoverOrdinarySend(bool retry);
    void postOrdinarySend(const QString &conversation, const QString &id, quint64 ticket);
    void readOrdinarySend(const QString &conversation, const QString &id, quint64 ticket, bool retry);
    void confirmOrdinarySend(const QJsonObject &raw, const QString &conversation, const QString &id, quint64 ticket);
    // Issue #7 multi-forward (docs/NATIVE_DESKTOP_MULTI_FORWARD.md). A batch
    // carries source uuids only: bodies load GET-only, one batch posts once,
    // and confirmation is the canonical GET-only read-back Check - an
    // unconfirmed batch is confirmed with a GET alone, never a second POST.
    struct ForwardIntent {
        QString uuid, publicId, owner, user, project, stream, topic, wire;
        QStringList sources;
        quint64 generation = 0, ticket = 0;
        bool busy = false, pending = false;
    };
    static QStringList uniqueForwardUuids(const QStringList &messageUuids);
    bool forwardOwned(const ForwardIntent &intent) const;
    void clearForwardIntent();
    void failForward(const QString &error);
    QString forwardDestinationRefusal(const QString &streamUuid) const;
    void fetchForwardSources(quint64 ticket);
    QString buildForwardSnapshot(const QStringList &sources, const QHash<QString, Message> &found) const;
    bool matchesForwardedPost(const QJsonObject &raw, const ForwardIntent &intent) const;
    void postForward(quint64 ticket);
    void readForwardStatus(quint64 ticket);
    bool confirmForward(const QJsonObject &raw, quint64 ticket);
    ForwardIntent m_forwardIntent;
    QString m_forwardError;
    QHash<QString, QVariantList> m_forwardTopicOptions;
    QHash<QString, quint64> m_forwardTopicRequests;
    QSet<QString> m_forwardTopicsLoading;
    void finishForgetSavedAccount(const QString &id, std::function<void()> done);
    QString m_outgoingOwner;
    NativeDraftService::Lease m_outgoingLease;
    QPointer<NativeDraftService> m_outgoingService;
    bool m_outgoingLoaded = false, m_outgoingLoading = false, m_outgoingSyncing = false;
    QString m_outgoingError;
    quint64 m_nextOutgoing = 0;
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
    // Issue #83: bootstrap GET /folders/ with the same catalog guard as the
    // other snapshot collections; failures keep (or install) the synthesized
    // system trio so the rail never renders empty.
    void loadFolders();
    // Notification metadata backfill: topics of background streams are not in
    // the active catalog, so mute policy fetches the detail on demand.
    void requestTopicMetadata(const QString &topicUuid);
    void loadMessages(bool older = false, bool latest = false);
    void retireConversationOpening();
    void resolveOpeningUnread(quint64 ticket, int attempt = 0);
    void resolveOpeningSaved(quint64 ticket, const QString &savedAnchor, int attempt = 0);
    void openConversationAnchor(const Message &message);
    void finishConversationOpening(const QString &id = {}, bool failed = false);
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
    void parkReplySession();
    void restoreParkedReplySession();
    QString replyAuthorFor(const QString &id) const;
    // The quoted message's markdown for a reply tab: the loaded row when present,
    // otherwise the resolved quote cache (issue #202 edit-time restore chips may
    // point outside the loaded window; docs/NATIVE_DESKTOP_EDIT_REPLY_RESTORE.md).
    QString replyQuotedContent(const QString &id) const;
    void scheduleQuoteFetch(const QString &id) const;
    void refreshQuoteOriginDecryptions();
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
    // Fix round R2 (PR #292 review, cid=4577609973 / cid=4577609975): the
    // creationOwnerKey alone cannot fence async completions - switching
    // projects away and back re-creates the identical owner key, so a retired
    // operation would pass an owner-only fence after the round trip. This
    // generation advances at every session retirement boundary: sign-out
    // clears it, and any re-authentication under a changed owner (a project
    // switch, including back to the original project) bumps it. Async copy
    // paths capture it at dispatch and discard completions from a retired
    // generation even when the owner key matches again.
    quint64 m_sessionGeneration = 0;
    QString m_sessionGenerationOwner;
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
    // Issue #100: catalog-scoped error text (reference messenger.error): the
    // failed streams walk feeds it, a snapshot reload or an authoritative
    // delivery clears it; it never carries unrelated action failures.
    QString m_sidebarError;
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
    QVector<DownloadEntry> m_downloadEntries;
    quint64 m_nextFileOperation = 0;
    quint64 m_fileEpoch = 0;
    quint64 m_fileServiceSerial = 0;
    int m_activeUploads = 0;
    bool m_drainingUploads = false;
    QTimer m_poll;
    // Presence heartbeat timer (issue #78): the reference cadence lives in
    // user-workspace-presence-reporter.lib.ts, mirrored below.
    void reportPresenceHeartbeat();
    QTimer m_presenceHeartbeat;
    QPointer<RealtimeClient> m_realtime;
    QPointer<PowerWatcher> m_powerWatcher;
    // Clock for the resume-reconnect floor (fix-round-1 finding 4): restarted
    // on every resume-triggered reconnectNow, resumes inside the floor ignored.
    QElapsedTimer m_resumeReconnectClock;
    // Set on PrepareForSleep(true): the next resume is a genuine sleep
    // cycle and bypasses the reconnect floor (fix-round-2, cid 4576829140).
    bool m_sawSleepNotification = false;
    bool m_onBattery = false;
    QVector<Stream> m_streams;
    // Issue #83: rail rows derived from GET /folders/ plus synthesized system
    // slots (reference fixed UUIDs 0..000/0..001/0..002).
    struct FolderRow
    {
        QString uuid;
        QString key;
        QString title;
        QString systemType; // all | personal | channels | created
        // 0xRRGGBB as masked by the reference display code; nullopt when the
        // DTO carries no background_color_value at all (review cid 4616033386:
        // unset is NOT black - the stored 0x00000000 stays a numeric 0).
        std::optional<quint32> color;
        int unread = 0;
        QStringList streamIds;
        // Issue #96 (docs/NATIVE_DESKTOP_SIDEBAR_ROW_STATES.md): stream ids whose
        // folder item carries a non-null pinned_at (adaptMessengerFolderItem).
        QSet<QString> pinnedStreams;
        bool synthesized = false;
    };
    QVector<FolderRow> m_folders;
    QString m_selectedFolder;
    // Issue #84: single-flight folder mutation state (ownStatus pattern):
    // kind "" | create | rename | delete, settled by owner + generation +
    // ticket so a session switch can never apply a stale result.
    QString m_folderMutationKind, m_folderMutationOwner, m_folderMutationError;
    bool m_folderMutationPending = false;
    quint64 m_folderMutationTicket = 0, m_folderMutationGeneration = 0;
    // Issue #423 review 4628637592: token for the single bounded follow-up
    // reconcile armed by a renewal interrupt; bumping it disarms any pending
    // follow-up (new interrupt, owner reset).
    quint64 m_folderMutationFollowup = 0;
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
    // ---- Issue #89 chat header member projection ----
    // docs/NATIVE_DESKTOP_CHAT_HEADER_COUNTERS.md. The member uuids belong
    // to m_chatHeaderMembersStream; reads with a different selection report
    // 0, so stale counts can never leak across conversations and late
    // pages are dropped by the load ticket (same discipline as #132).
    bool chatHeaderMembersContext(const QString &owner, quint64 ticket, const QString &stream) const;
    void loadChatHeaderMembersPage(const QString &marker, quint64 ticket, const QString &stream,
                                   QStringList accepted, QSet<QString> seenUsers);
    void applyChatHeaderMembersBindingEvent(const QString &kind, const QJsonArray &rows);
    // Issue #438: the automatic fetch trigger, scoped the same way #434 scoped
    // the binding-event NOTIFY: only a selection edge on an on-screen header
    // may load member pages; the same owner+stream never refetches on its own.
    void ensureChatHeaderMembers();
    QString m_chatHeaderMembersStream;
    QString m_chatHeaderMembersOwner;
    QStringList m_chatHeaderMemberUuids;
    bool m_chatHeaderVisible = false;
    // Issue #438 review (finding 4633233517): has the header surface ever
    // been on screen? Pre-mount selection skips arm nothing; only skips
    // after the first mount record a deferred recovery obligation, cleared
    // when the current selection claims its page (or is unselected).
    bool m_chatHeaderVisibilityMounted = false;
    bool m_chatHeaderDeferredEdge = false;
    quint64 m_chatHeaderMembersTicket = 0;
    // Issue #89 round 3 (PR #432 review finding 4628746243): a binding
    // membership event that lands while a member page load is in flight is
    // recorded here and replayed against the completed page, so the older
    // snapshot can never overwrite it. Reset with every refresh/load end.
    bool m_chatHeaderMembersLoading = false;
    bool m_chatHeaderMembersKnown = false;
    QList<QPair<QString, QString>> m_chatHeaderMembersPendingEvents;
    // ---- Issue #134 user profile drawer state ----
    void refreshOwnProfileProjection(const QString &user, quint64 ticket,
        const QVariantMap &savedProfile);
    bool profileContext(const QString &user, quint64 ticket, quint64 currentTicket) const;
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
    bool m_windowVisible = true;
    bool m_windowMinimized = false;
    QVector<Message> m_messages;
    struct UserPresentation {
        QString name, avatar, presence, statusLabel;
        std::optional<qint64> presenceUpdatedAt;
        QString email;
        QString status, statusText, statusEmoji;
        // Issue #88 (docs/NATIVE_DESKTOP_DM_HEADER_STATUS.md): the users-row
        // last-seen display string behind the direct header caption
        // (presence.lastSeen). Absent means contract null; like the presence
        // and status decorations it follows the updated_at ordering guard.
        QString lastSeen;
        // Issue #177: the directory's deactivated-account fact behind the
        // direct header badge (dm.partnerBlocked). Absent means not
        // deactivated, never invented; unlike presence it is account state,
        // so the updated_at presence guard does not keep it.
        bool accountDeactivated = false;
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
        // Issue #102 (docs/NATIVE_DESKTOP_CHAT_LIST_DENSITY.md): the topic the previewed
        // message lives in - the reference stream.preview?.route input for the
        // latestTopic click mode (sidebar-workspace.ui.tsx:455).
        QString topic;
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
    // Issue #177: mirrors userPresence gating for the deactivated-account fact.
    bool userAccountDeactivated(const QString &id) const;
    QString userStatusLabel(const QString &id) const;
    QString userLastSeen(const QString &id) const;
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
    QVariantMap m_mentionSelfChoice;
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
    QHash<QString, QVariantList> m_editAttachments; // empty key or restored reply tab id
    QString editAnswer(const QString &text, const QString &tabId = {}) const;
    // Save-race guards (issue #115): m_editSession names the open session (bumped on every
    // begin), m_editRevision tracks the edit buffer (bumped whenever the buffer is replaced).
    // A save completion may only close the exact (session, revision) it submitted.
    // m_editSaveInFlight lists messages whose edit PUT is still in flight, so a second
    // save for the same message is refused instead of racing the first.
    quint64 m_editSession = 0, m_editRevision = 0;
    QSet<QString> m_editSaveInFlight;
    // True when the open edit session restored its reply tabs from the stored body
    // (issue #202): the restored session is edit-local and closes with the edit.
    bool m_editRestoresReply = false;
    // Reply session (docs/NATIVE_DESKTOP_REPLIES.md + docs/NATIVE_DESKTOP_MULTI_REPLY_TABS.md):
    // concurrent quote tabs with per-tab answers; the conversationKey() that owns
    // the session is m_replyRoute. m_quotes caches resolved references.
    // The author/plain memos keep the restored banner readable while a parked
    // session's quoted messages are not yet in memory again after navigation
    // (docs/NATIVE_DESKTOP_REPLY_SESSION_PERSISTENCE.md); live resolution wins
    // whenever it succeeds.
    // ReplyTab.reference is the verbatim wire quote line captured by an edit-time
    // restore (docs/NATIVE_DESKTOP_EDIT_REPLY_RESTORE.md); ordinary tabs keep it
    // empty and serialize their quote line from the quoted message.
    struct ReplyTab {
        QString id, message, createdAt, answer, reference;
        // ReplyTab.selectedText is the fragment selected inside the quoted
        // bubble when the reply started (issue #218, workspace_ui 230e02b8
        // workspace-reply types: tab.selectedText). It wins the excerpt and
        // the wire quote carries it as ?text=; empty means "quote whole".
        QString selectedText;
        // ReplyTab.fallbackAuthor is the sender label parsed from the restored quote
        // line; it carries the chip author while the quoted origin is unresolved
        // (pending lookup) or when the lookup reports the origin is unavailable.
        QString fallbackAuthor;
        mutable QString authorMemo, plainMemo;
    };
    // A reply session parked under the conversationKey() that owns it while the
    // user is in another conversation (issue #201). The whole session travels:
    // tab set, order, active pointer and every answer.
    struct ParkedReplySession { QList<ReplyTab> tabs; QString activeTab; };
    QList<ReplyTab> m_replyTabs;
    QString m_replyActiveTab, m_replyRoute;
    QHash<QString, ParkedReplySession> m_replySessions;
    const ReplyTab *activeReplyTab() const;
    QString replyTabAuthor(const ReplyTab &tab) const;
    QString replyTabPlain(const ReplyTab &tab) const;
    // The getters' shared live-resolution sources, reused by the park path so
    // parked memos come from exactly the same data the banner renders from.
    std::optional<QString> replyPlainFor(const QString &id) const;
    void primeReplyTabMemos(QList<ReplyTab> &tabs) const;
    struct QuoteRecord {
        QString authorId, content;
        bool missing = false, decrypt = false;
        // Issue #222 review fix r2: a fetched encrypted origin keeps its
        // decryptable payload plus the last chat-prepared decrypt result, so
        // the quote frame reaches ready/error without the origin loading into
        // a conversation window.
        std::optional<Message> origin;
        PreparedChatMessage prepared;
        bool hasPrepared = false;
    };
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
    // Activity starred page (#41, docs/NATIVE_DESKTOP_ACTIVITY_STARRED.md):
    // one collection keyed `starred` (activity-page.ui.tsx:178-247), the
    // version counter is the collection-key guard and resetStarred() is the
    // runtime-owner guard (test 952: a stale page must never repopulate).
    struct StarredCollection {
        QVector<Message> rows;
        QByteArray marker;
        quint64 version = 0;
        bool inflight = false;
        // A mount (or retry) that arrived while a request was in flight: the
        // first-page refresh is re-issued once that response lands, so the
        // reopened page never settles on the pre-reopen snapshot.
        bool refreshPending = false;
        bool loading = false;
        bool error = false;
        bool paginationError = false;
        bool hasMore = false;
    };
    void loadStarredCollection(bool append);
    QVariantList starredRows() const;
    void resetStarred();
    // Issue #54: the sidebar destinations are mutually exclusive. Opening one
    // closes the others so exactly one page and one sidebar selection stay
    // active, and closing the active page returns to the conversation.
    enum class ContentPage { Drafts, Mentions, Inbox, Feed, Starred, Reactions, Mail, Calendar, Calls, Services, Licenses, Diagnostics, Favorites };

    // Issue #50: diagnostics collector helpers (docs/NATIVE_DESKTOP_DIAGNOSTICS_PAGE.md).
    void recordApiLatency(const QString &label, qint64 elapsedMs);

    void resetChatSection();
    void closeOtherContentPages(ContentPage keep);
    // Reference detection (messenger-self-chat.lib.ts:8-18): isPrivate with
    // direct_user_uuid equal to the verified session user; the archived row
    // still counts as self-chat for every ordinary list projection.
    std::optional<Stream> findSelfStream() const;
    void runFavoritesFlow();
    void ensureSelfStream();
    void openSelfStream(const QString &streamUuid, const QString &preferredTopic);
    void leaveFavorites();
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
    QString m_sidebarSearchQuery;
    QString m_sidebarExpansionOwner;
    QStringList m_expandedSidebarStreams;
    // Activity panel open state for the current session owner (#101);
    // default closed, like sidebar-config.model.ts DEFAULT_CONFIG.
    bool m_activityOpen = false;
    QStringList m_sidebarTopicPending;
    QSet<QString> m_sidebarTopicHydrating;
    bool m_restoringSidebarRoute = false;
    void prepareSidebarExpansion();
    void collapseSidebarExcept(const QString &stream);
    void hydrateExpandedSidebarStreams();
    void pumpSidebarTopicHydration();

    void hydrateSidebarSearchTopics();
    QVariantList topicRows(const QVector<Topic> &topics, const Stream *stream) const;
    bool m_inboxVisible = false;
    // Activity starred page (#41, docs/NATIVE_DESKTOP_ACTIVITY_STARRED.md).
    bool m_starredVisible = false;
    StarredCollection m_starred;
    // Favorites self-chat (#70). m_selfStreamEnsures deduplicates one
    // in-flight create per runtime generation (workspace-self-chat.hook.ts:36,
    // 48-56); the entry drops when the request settles (lines 63-68).
    bool m_favoritesVisible = false;
    QString m_favoritesStatus, m_favoritesError;
    QHash<QString, quint64> m_selfStreamEnsures;
    quint64 m_favoritesEnsureSeq = 0;
    quint64 m_favoritesWatch = 0;
    QString m_section = QStringLiteral("chat");
    QString m_sectionBeforeLicenses = QStringLiteral("chat");
    QHash<QString, QVector<Topic>> m_inboxTopics;
    QSet<QString> m_inboxStaleTopics;
    // Foreground authoritative catalogs fence older background completions.
    QHash<QString, quint64> m_inboxSelectedRevisions;
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
    // Project feed page (#38, docs/NATIVE_DESKTOP_FEED.md). Rows are stored
    // oldest-first exactly like the reference store (feed.model.ts:39-68:
    // created_at asc, uuid asc tie break, deduped by uuid); an older page
    // merges in front of the newer rows.
    struct FeedState {
        QVector<Message> rows;
        QByteArray marker;
        bool loading = false, loadingMore = false, error = false;
        bool inflight = false;
        quint64 version = 0;
        bool hasMore() const { return !marker.isEmpty(); }
    };
    bool m_feedVisible = false;
    bool m_reactionsVisible = false;
    FeedState m_feed;
    void loadFeedPage(bool append);
    void interruptFeedRequest();
    // Issue #423: settle a folder mutation retired by a generation bump.
    void interruptFolderMutation();
    void resetFeed();
    QVariantList feedRows() const;
    QString feedRowTime(const QDateTime &created) const;
    QString feedRowContext(const Message &message) const;
    QString feedRowPreview(const Message &message) const;
    QString summarizeFeedMarkdown(const QString &content) const;
    QString summarizeFeedInlineFlow(const QString &text, int depth, bool &leadingMedia) const;
    QString feedPreviewInlineLabel(const QString &bang, const QString &label,
        const QString &target, const QString &title, bool &isMedia, int depth = 0) const;
    bool durableDraftRoute() const;
    void synchronizeDrafts();
    QHash<QString, SendIntent> m_pendingSends;
    QHash<QString, PollSendAttempt> m_pollSendAttempts; // Owner + conversation retry identity
    QSet<QString> m_pendingPollCreations; // Dialog submission tokens, never canonical UUIDs
    QHash<QString, int> m_pendingUploads;
    struct ClipboardSnapshot {
        QString requested, canonical, name;
        ClipboardFileIdentity identity;
        std::shared_ptr<QTemporaryFile> file;
    };
    QList<UploadEntry> m_uploadEntries;
    QList<ClipboardSnapshot> m_clipboardSnapshots;
    QVariantList m_clipboardFilesPreview;
    std::shared_ptr<std::atomic_bool> m_clipboardSnapshotCancel;
    QString m_clipboardOwner, m_clipboardStream, m_clipboardTopic, m_clipboardEdit;
    bool m_clipboardFilesPreparing = false;
    bool m_clipboardFilesAccepted = false;
    QString m_inlineDraft, m_inlineDraftRoute, m_inlineDraftOwner;
    QHash<QString, QString> m_attachmentNotices;
    QString m_stream;
    QString m_topic;
    QString m_anchorMessage;
    // Issue #60: the parked message deep link's organization route id and
    // lowercased project uuid (reference messageTarget). Returns false when
    // no parked message link exists.
    bool linkedMessageTarget(QString *org, QString *project) const;
    QString m_pendingDeepLink;
    QMetaObject::Connection m_profileLinkCacheDestroyed;
    std::weak_ptr<quint64> m_profileLinkSelection;
    bool m_profileLinkFailed = false;
    quint64 m_profileLinkFailureSelection = 0;
    bool m_profileLinkFailureInbox = false, m_profileLinkFailureMentions = false, m_profileLinkFailureDrafts = false;
    struct DeepLinkTopic {
        QString stream, name, owner;
        quint64 selection = 0, generation = 0;
    } m_deepLinkTopic;
    quint64 m_deepLinkSerial = 0;
    bool m_deepLinkFlushScheduled = false;
    QString m_pendingAnchorMessage;
    // Issue #107: link-owned jump bookkeeping for the transition layer. Only
    // message-link anchors (applyMessageRoute / retry) set the flag, so the
    // service conversation-open automatic anchor keeps its old silent flow.
    bool m_anchorLinkPending = false;
    QString m_anchorNavigationError;
    QString m_failedAnchorLinkTarget;
    std::optional<Message> m_pendingAnchorParsed;
    QString m_pendingAnchorStream;
    QString m_pendingAnchorTopic;
    int m_anchorBudget = 0;
    bool m_anchorAroundTried = false;
    struct ConversationOpening {
        quint64 selection = 0, generation = 0, ticket = 0;
        bool prepared = false, pending = false, automaticAnchor = false, failed = false;
        QString target;
        int unreadAttempt = -1;
        QString savedAnchor;
        int savedAttempt = 0;
    } m_conversationOpening;
    quint64 m_nextConversationOpening = 0;
    QTimer m_anchorHighlight;
    QString m_error;
    bool m_startingWizardLogin = false;
    QString m_loginStep = QStringLiteral("organization");
    QString m_organizationRealm;
    QString m_checkedOrganizationOrigin;
    QString m_pendingOrganizationOrigin;
    QString m_defaultOrganizationUrl;
    QString m_defaultOrganizationName;
    QVariantMap m_organizationSettings;
    QPointer<NativePreferences> m_preferences;
    // Issue #165: inactivity guard and the preference scope it is bound to.
    NativeAuthGuard *m_authGuard = nullptr;
    QPointer<NativePreferences> m_authPrefsBound;
    bool m_organizationLoading = false;
    quint64 m_organizationAttempt = 0;
    QString m_syncState;
    QTimer m_connectionCountdown{this};
    qint64 m_rateLimitedUntilMs = 0;
    bool m_transportOffline = false;
    // Issue #50: diagnostics collector state (docs/NATIVE_DESKTOP_DIAGNOSTICS_PAGE.md).
    // The counters mirror the reference diagnostics-realtime.lib.ts stats; the
    // latency ring is the "API latency from HTTP response timing" source.
    // Issue #52: memory panel provider (procfs-backed, capability flags).
    workspace::diagnostics::NativeMemoryDiagnostics m_memoryDiagnostics;
    // Issue #51: in-memory session log ring buffer (reference logger.ts),
    // feeding the diagnostics log viewer modal.
    workspace::diagnostics::SessionLogBuffer m_sessionLog{this};
    int m_realtimeEventsReceived = 0;
    QString m_realtimeLastEventType;
    qint64 m_realtimeLastEventAtMs = 0;
    int m_realtimeReconnectAttempts = 0;
    QList<QPair<QString, qint64>> m_apiLatencySamples; // bounded to 200, insertion order
    bool m_pingInFlight = false;
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
    // Issue #156: prefetch arming state. Consumed only when a request starts;
    // re-armed by a note at or above the reserve, a fresh window commit, a
    // failed history request, and reset().
    bool m_olderPrefetchArmed = true;
    bool m_busy = false;
    bool m_loading = false;
    bool m_sending = false;
    bool m_realtimeMode = false;
    bool m_snapshotActive = false;
    unsigned m_snapshotParts = 0;
    quint64 m_snapshotSelection = 0;
    quint64 m_snapshotCatalog = 0;

public:
    // ---- Issue #161 call launch (docs/NATIVE_DESKTOP_CALL_LAUNCH.md) ----
    // Per-workspace meet host (server_settings.meet_url, origin-only, cleared
    // with the session), the single-active-call store, and the header start
    // flow that posts the meeting URL before opening the call.
    Q_PROPERTY(JitsiCallStore *calls READ calls CONSTANT)
    // The call window shell state machine (#162), opened/closed with the
    // active call: the QML overlay (expanded) and PiP window bind to it.
    Q_PROPERTY(JitsiCallWindowController *callWindow READ callWindow CONSTANT)
    // The call window's live participant count/roster source (#162 finding 5):
    // the embed transport reports count/roster reads here and the bridge feeds
    // the shell, so the reconcile timer always re-reads live embed data.
    Q_PROPERTY(JitsiEmbedBridge *embedBridge READ embedBridge CONSTANT)
    // Issue #163: the incoming invite lifecycle (ring schedule, 45 s
    // auto-decline, per-invite video preference, invite variant).
    Q_PROPERTY(IncomingCallLifecycle *incomingCall READ incomingCall CONSTANT)
    Q_PROPERTY(QString meetHost READ meetHost NOTIFY stateChanged)
    Q_PROPERTY(bool canStartCall READ canStartCall NOTIFY stateChanged)
    Q_PROPERTY(QString callError READ callError NOTIFY stateChanged)
    JitsiCallStore *calls() { return &m_callStore; }
    JitsiCallWindowController *callWindow() { return &m_callWindow; }
    JitsiEmbedBridge *embedBridge() { return &m_embedBridge; }
    IncomingCallLifecycle *incomingCall() { return &m_incomingCall; }
    QString meetHost() const { return m_meetHost; }
    QString callError() const { return m_callError; }
    // The reference renders the header call action only when the Workspace
    // meet host is configured (chat-page-workspace.ui.tsx:4100).
    bool canStartCall() const
    { return authenticated() && hasConversation() && !topicClosed() && !m_meetHost.isEmpty(); }
    Q_INVOKABLE void startCall();
    // Issue #164 DM call bridge (docs/NATIVE_DESKTOP_DM_CALL_BRIDGE.md): the
    // bridge store (pending partner + active-call descriptor), the profile Call
    // entry and the busy flag that mirrors the reference `callPending` chip gate.
    Q_PROPERTY(DmCallBridge *callBridge READ callBridge CONSTANT)
    Q_PROPERTY(bool profileCallPending READ profileCallPending NOTIFY stateChanged)
    // Encryption-availability mirror for the profile Call chip. The per-peer
    // answer below is an invokable whose stream and key inputs QML cannot
    // observe on its own; callCryptoStateChanged fires on every transition of
    // those inputs (stream catalog rows, the viewed conversation, the local
    // send key / chat read lifetime), and the chip binds through this property.
    Q_PROPERTY(bool callCryptoSessionReady READ callCryptoSessionReady NOTIFY callCryptoStateChanged)
    DmCallBridge *callBridge() { return &m_callBridge; }
    bool profileCallPending() const;
    bool callCryptoSessionReady() const
    { return chatContext(m_chatEpoch, m_selection) && !m_chatSendKey.isEmpty(); }
    // True when the partner's existing DM is an E2EE stream without a usable
    // local key: the Call chip is disabled and an invoke would only error.
    Q_INVOKABLE bool callTargetCryptoUnavailable(const QString &userUuid) const;
    // Profile panel Call: same navigation contract as the Message action, with
    // the pending-partner handoff into the shared header start flow.
    Q_INVOKABLE void callFromProfile(const QString &userUuid);
    // Card join: mirrors the reference join path - opening a different room
    // while a call is active is refused with the generic inline error.
    Q_INVOKABLE void joinCall(const QString &meetingUrl);
    // Bubble helpers: recognition uses the effective host (Workspace meet
    // host, else the build-time default) exactly like the reference card.
    Q_INVOKABLE QString callUrlInText(const QString &text) const;
    Q_INVOKABLE QString callRoomName(const QString &url) const;

private:
    void fetchCallMeetHost();
    void maybeOfferCallInvite(const Message &message);
    // Issue #164: descriptor sync + pending handoff (docs/NATIVE_DESKTOP_DM_CALL_BRIDGE.md).
    void syncCallBridge();
    void attemptPendingCallHandoff();
    void navigateProfileDirect(const ProfileDirectNavigation &navigation);
    JitsiCallStore m_callStore;
    JitsiCallWindowController m_callWindow;
    JitsiEmbedBridge m_embedBridge;
    IncomingCallLifecycle m_incomingCall;
    DmCallBridge m_callBridge;
    QString m_meetHost;
    QString m_callError;
    quint64 m_callMeetAttempt = 0;
    // Issue #164 stale-send guards: monotonic view epoch (bumped on every viewed
    // conversation change), the epoch owning the one in-flight guard, and the
    // peer whose Call-triggered direct-chat create is in flight.
    quint64 m_callViewEpoch = 1;
    quint64 m_callInflightEpoch = 0;
    quint64 m_callViewGeneration = 0;
    QString m_callViewOwner;
    QString m_callViewStream;
    QString m_callViewTopic;
    QString m_profileCallPeer;
    // Profile-direct navigation resolution shared by the Message and Call chips.
    ProfileDirectNavigation resolveProfileDirect(const QString &peer) const;
};

}
