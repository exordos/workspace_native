#pragma once
#include "native-external-accounts.h"
#include "native-external-provider-admin.h"

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

#include <QAbstractListModel>
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

class QImage;
template <class T> class QFutureWatcher;

namespace workspace {

class MediaViewerModel;
class NativePreferences;
class NativeAuthGuard;
class RealtimeClient;
class PowerWatcher;
class FileClient;
class NativeChatKeyVault;
struct FileResult;

// Issue #866 (finding 2): move-aware sidebar row model. The sidebar ListView
// used to bind the sidebarStreams QVariantList projection directly, so every
// re-rank (a hydrated preview stamp re-sorting its row) reassigned the model
// and reset the view: delegates under the user were re-created and lost focus.
// The reference keeps keyed DOM rows and MOVES them. This model keeps one
// instance for the app lifetime, exposes each projection row under the
// "modelData" role (the delegate contract is unchanged), and translates every
// re-projection into granular inserted/removed/moved/dataChanged deltas so
// surviving delegates keep their identity across a reorder. Lazily created
// on the first QML read: pure-C++ controller tests never instantiate it and
// their pinned projection budgets stay untouched.
class NativeSidebarStreamListModel final : public QAbstractListModel {
    Q_OBJECT
public:
    explicit NativeSidebarStreamListModel(NativeController *controller, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Sidebar scroll anchors (#660/#693) resolve a row index to its stream id;
    // with a real model the JS can no longer index model[row], so the id
    // lookup is part of the model contract.
    Q_INVOKABLE QString streamIdAt(int row) const;

public slots:
    // Re-diff the controller projection into the current rows. Idempotent:
    // connected to every notify that can move the projection; a settled state
    // emits nothing.
    void sync();

private:
    struct Row {
        QString key;
        QVariantMap map;
    };
    static QString keyForRow(const QVariant &value, int fallbackIndex);

    NativeController *m_controller;
    QVector<Row> m_rows;
};

class NativeController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool authenticated READ authenticated NOTIFY stateChanged)
    // Issue #789: true while the workspace is painted from the cached data of
    // the saved startup identity and the session refresh chain is still in
    // flight. Leaving it flips back to the authenticated projection or to the
    // sign-in screen; cached rows are never blanked by the handover.
    Q_PROPERTY(bool startupCacheMode READ startupCacheMode NOTIFY stateChanged)
    Q_PROPERTY(bool sessionRenewalUnavailable READ sessionRenewalUnavailable NOTIFY stateChanged)
    Q_PROPERTY(workspace::NativeExternalAccounts* externalAccounts READ externalAccounts CONSTANT)
    Q_PROPERTY(workspace::NativeExternalProviderAdmin* externalProviderAdmin READ externalProviderAdmin CONSTANT)
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
    Q_PROPERTY(QVariantList chatPickerChoices READ chatPickerChoices NOTIFY stateChanged)
    Q_PROPERTY(bool channelMembersVerified READ channelMembersVerified NOTIFY stateChanged)
    // Ownership/API/route/cache/trust lifetime only. Ordinary row counters and
    // directory display updates do not change this key or discard a form.
    Q_PROPERTY(QString channelCreationContextKey READ channelCreationContextKey NOTIFY stateChanged)
    Q_PROPERTY(QString channelCreationPhase READ channelCreationPhase NOTIFY stateChanged)
    Q_PROPERTY(bool channelCreationBusy READ channelCreationBusy NOTIFY stateChanged)
    Q_PROPERTY(bool channelCreationUncertain READ channelCreationUncertain NOTIFY stateChanged)
    Q_PROPERTY(QString channelCreationError READ channelCreationError NOTIFY stateChanged)
    Q_PROPERTY(bool channelCreateRequestFailed READ channelCreateRequestFailed NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap channelCreationForm READ channelCreationForm NOTIFY stateChanged)
    Q_PROPERTY(bool canCheckChannelCreation READ canCheckChannelCreation NOTIFY stateChanged)
    Q_PROPERTY(bool canRetryChannelMembers READ canRetryChannelMembers NOTIFY stateChanged)
    // Issue #132 channel info drawer content for the selected stream
    // (docs/NATIVE_DESKTOP_CHANNEL_INFO.md): member projection, notification
    // store mode and the three member/notification mutations.
    Q_PROPERTY(QVariantList channelInfoTopics READ channelInfoTopics NOTIFY channelInfoTopicsChanged)
    Q_PROPERTY(QVariantMap channelInfoStream READ channelInfoStream NOTIFY stateChanged)
    Q_PROPERTY(QVariantList channelInfoMembers READ channelInfoMembers NOTIFY stateChanged)
    Q_PROPERTY(int channelInfoMemberCount READ channelInfoMemberCount NOTIFY stateChanged)
    Q_PROPERTY(int channelInfoOnlineCount READ channelInfoOnlineCount NOTIFY stateChanged)
    // Issue #632 (docs/NATIVE_DESKTOP_KEY_SHARING_TOGGLE.md): the encrypted
    // channel owner-only key-sharing switch. The map carries {checked,
    // disabled} only while the row is editable (encrypted stream owned by the
    // viewer whose DTO confirms allow_key_sharing_by_holders); an empty map
    // means the row is hidden. Reuses stateChanged: no new NOTIFY signals.
    Q_PROPERTY(QVariantMap chatKeySharing READ chatKeySharing NOTIFY stateChanged)
    Q_PROPERTY(QString chatKeySharingError READ chatKeySharingError NOTIFY stateChanged)
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
    Q_PROPERTY(bool cachedSearchAvailable READ cachedSearchAvailable NOTIFY cachedSearchAvailabilityChanged)
    Q_PROPERTY(QVariantList cachedSearchResults READ cachedSearchResults NOTIFY cachedSearchChanged)
    Q_PROPERTY(QString cachedSearchState READ cachedSearchState NOTIFY cachedSearchChanged)
    Q_PROPERTY(QVariantList streams READ streams NOTIFY streamsChanged)
    // #866 review (finding 3, design round): preview CONTENT announces travel on a narrow
    // preview-only revision instead of streamsChanged, so a preview batch re-evaluates the
    // delegates' text bindings without rebuilding the sidebar row array under an in-flight
    // click or scroll. Delegates pass the revision into previewFields() as an explicit
    // binding dependency. Last-message POINTER moves still announce streamsChanged.
    Q_PROPERTY(int previewRevision READ previewRevision NOTIFY previewChanged)
    // Issue #867: projection-count seam for the regression tests (no NOTIFY).
    Q_PROPERTY(int streamsProjectionCountForTest READ streamsProjectionCountForTest)
    Q_PROPERTY(int topicsProjectionCountForTest READ topicsProjectionCountForTest)
    // Issue #83: folder rail (docs/NATIVE_DESKTOP_FOLDERS_RAIL.md). The rail
    // rows, the effective folder selection and the folder-scoped sidebar
    // projection over the stream catalog.
    Q_PROPERTY(QVariantList folders READ folders NOTIFY foldersChanged)
    Q_PROPERTY(QVariantList sidebarStreams READ sidebarStreams NOTIFY sidebarStreamsChanged)
    // Issue #866 (finding 2): stable sidebar model instance for the sidebar
    // ListView (created on first read, never reassigned - CONSTANT).
    Q_PROPERTY(NativeSidebarStreamListModel *sidebarStreamListModel READ sidebarStreamListModel CONSTANT)
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
    Q_PROPERTY(QStringList readRequestBoundaryIds READ readRequestBoundaryIds NOTIFY readRequestBoundariesChanged)
    Q_PROPERTY(QVariantList reactionChoices READ reactionChoices CONSTANT)
    Q_PROPERTY(QString conversationKind READ conversationKind NOTIFY stateChanged)
    Q_PROPERTY(bool hasConversation READ hasConversation NOTIFY stateChanged)
    Q_PROPERTY(QString selectedStream READ selectedStream NOTIFY stateChanged)
    Q_PROPERTY(QString selectedTopic READ selectedTopic NOTIFY stateChanged)
    Q_PROPERTY(QString draft READ draft WRITE setDraft NOTIFY draftChanged)
    Q_PROPERTY(QString composerPreviewBody READ composerPreviewBody NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap outgoingSend READ outgoingSend NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap encryptedOutgoingPresentation READ encryptedOutgoingPresentation NOTIFY stateChanged)
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
    // Calendar section page (#881, docs/NATIVE_DESKTOP_CALENDAR.md): events
    // for the visible window, fetched from GET /calendar/events?from&to.
    // Calendar state carries its own NOTIFY (issue #881, #89 lesson): the fetch
    // fires while Main.qml's `visible: section === "calendar"` binding is still
    // being evaluated (page onVisibleChanged -> refreshWindow -> request), and a
    // stateChanged emission here re-dirtied the section dependency mid-evaluation
    // - Qt raised "Binding loop detected for property visible" and the warning
    // poisoned every window case in the shard. Notify only what calendar changed.
    Q_PROPERTY(QVariantList calendarEvents READ calendarEvents NOTIFY calendarChanged)
    Q_PROPERTY(bool calendarLoading READ calendarLoading NOTIFY calendarChanged)
    Q_PROPERTY(bool calendarError READ calendarError NOTIFY calendarChanged)
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
    // Test-only seams for the upload preview worker (#680 fix round; no QML
    // consumer, used by native-controller-media-test): the barrier runs once
    // on the worker thread at job start, before the cooperative cancellation
    // check, so a test can stop the worker inside its flight window.
    // decodeStarts counts jobs that reached the decode stage at all.
    static void setUploadPreviewBarrierForTest(std::function<void()> barrier);
    static int uploadPreviewDecodeStartsForTest();
    // Issue #718 slice 2 seams (used by native-controller-sidebar-test):
    // hasPreparedTopicSlice reports whether the bounded preload has warmed a
    // small renderable slice for the topic (swap-on-ready source), and the
    // interval seam compresses the sweep pacing for deterministic waits.
    // The enable seam defaults ON (product behaviour); traffic-pinning test
    // fixtures turn the background sweep off so their request budgets stay
    // deterministic (swap-on-ready itself is unaffected - slices remembered
    // from real navigation keep working).
    bool hasPreparedTopicSlice(const QString &topicUuid) const;
    // Issue #893 (#855 subtask 1): GUI-thread phase attribution seam.
    // Recording is disabled by default (inert telemetry); reset enables it.
    Q_INVOKABLE void resetGuiPhaseTraceForTest();
    Q_INVOKABLE QVariantMap guiPhaseTraceForTest() const;
    // Begin/end pair used by QML instrumentation sites (no-op unless enabled).
    Q_INVOKABLE qint64 beginGuiPhaseSpan(const QString &name);
    Q_INVOKABLE void endGuiPhaseSpan(qint64 id);

    void setTopicPreloadIntervalMsForTest(int ms);
    void setTopicPreloadEnabledForTest(bool enabled);
    // #860: disables the network fallback stage of row-preview hydration (disk-only mode)
    // for traffic-pinned cases, mirroring setTopicPreloadEnabledForTest's per-case knob.
    void setRowPreviewNetworkEnabledForTest(bool enabled);
    // Test-only dispatch counter (#866 review): how many preview lookups the network
    // stage has actually dispatched. The mock's HTTP/1 connection pool cannot show an
    // over-dispatch at the server, so the cap is observed at the client seam.
    int rowPreviewNetworkIssuedForTest() const { return m_rowPreviewNetIssued; }
    // #888 witness: catalog-record inspections attributed to preview candidate
    // enumeration (streams + topics). Tests reset it, drive catalog/topic accepts (or
    // the scoped seam directly) and bound the GROWTH between catalog sizes - the old
    // quadratic enumeration cannot satisfy those bounds. Timing-independent by design.
    qint64 rowPreviewSeedWorkForTest() const { return m_rowPreviewSeedWork; }
    // #888 (exordy round 2): the reset also forces the reusable stream-uuid index
    // cold, so a bounded sweep measurement always starts from the same clean
    // state - the quadratic shape pays a scan per accept from cold too.
    void resetRowPreviewSeedWorkForTest() { m_rowPreviewSeedWork = 0; m_streamsRowTag = ~0ull; }
    // #888 witness: drive the scoped topic-accept seed exactly as the catalog parser
    // does after accepting one stream's topic rows.
    void seedStreamPreviewsForTest(const QString &streamUuid)
    {
        seedRowPreviewHydrationForStream(streamUuid);
    }
    // #866 witness (exordy finding 2): drive the network pump's queue exactly as a
    // catalog seed would (owner map + enqueue) and take candidates with the very
    // pick function the pump uses. Mock arrival order over the HTTP connection
    // pool is not a deterministic oracle, so the foreground-priority contract is
    // pinned at the state seam - the pick sequence below is fully deterministic.
    void rowPreviewSeedNetCandidateForTest(const QString &uuid, const QString &ownerStream)
    {
        m_rowPreviewStreamOf.insert(uuid, ownerStream);
        enqueueRowPreviewNet(uuid);
    }
    QString rowPreviewTakeNetForTest()
    {
        if (m_rowPreviewNetQueue.isEmpty())
            return {};
        const auto picked = takeRowPreviewCandidate(m_rowPreviewNetQueue);
        m_rowPreviewNetPending.remove(picked);
        return picked;
    }
    // #866 review (exordy finding 1): how many preview lookups are counted in-flight now.
    // The stale-session oracle: a reply from a retired session must leave the NEWER
    // request's marker in place, so a re-catalog can never re-dispatch the uuid.
    int rowPreviewNetworkInFlightForTest() const { return m_rowPreviewNetInFlight.size(); }
    // Issue #729 phase 2: the warm queue's throttle delay (429/5xx doubling,
    // bounded; cleared by a verified catalog commit).
    int topicPreloadBackoffMsForTest() const { return m_topicPreloadBackoffMs; }
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
    Q_INVOKABLE QVariantMap renderSecretMessage(const QString &messageId, const QStringList &ordinaryOpened,
        const QStringList &revealed);
    Q_INVOKABLE void requestSecretMessageKey(const QString &messageId, const QString &handle,
        const QStringList &ordinaryOpened);
    Q_PROPERTY(quint64 secretKeyRevision READ secretKeyRevision NOTIFY secretKeysChanged)
    quint64 secretKeyRevision() const { return m_secretKeyRevision; }
    Q_INVOKABLE void requestChatKey(const QString &keyUuid = {});
    // Issue #836: retries the session renewal after a transient failure on a
    // saved-account resume (no-op outside the degraded state).
    Q_INVOKABLE void requestSessionRenewal();
    Q_INVOKABLE void abandonStartupResume();
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
    Q_INVOKABLE void grantAllChatKeys();
    Q_INVOKABLE void grantChatKey(const QString &request, bool fromDrawer = false);
    Q_INVOKABLE void rejectChatKey(const QString &request, bool fromDrawer = false);
    Q_INVOKABLE void resolveKeyServiceRequest(const QString &responseId);
    Q_INVOKABLE void resolveKeyServiceRequest(const QString &responseId, quint64 disclosureSerial);
    Q_INVOKABLE void checkChatKeyResponse(const QString &request);
    Q_INVOKABLE void retryChatKeyResponse(const QString &request);
    void setChatKeyStoreOpening(bool opening);
    bool cacheStale() const;
    void reloadSnapshot();
    bool applyRealtimeEvent(const QJsonObject &envelope);
    bool authenticated() const { return m_session->ready(); }
    bool startupCacheMode() const { return m_startupCacheMode; }
    // Issue #836: forwards the session's degraded-renewal flag (transient
    // failure while resuming a saved account) so the cache shell can show a
    // recovery status and gate the manual retry.
    bool sessionRenewalUnavailable() const { return m_session->renewalUnavailable(); }
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
    QVariantList chatPickerChoices() const;
    Q_INVOKABLE QVariantList searchWorkspaceUsers(const QString &query) const;
    bool channelMembersVerified() const;
    QString channelCreationContextKey() const;
    QString channelCreationPhase() const;
    bool channelCreationBusy() const;
    bool channelCreationUncertain() const;
    QString channelCreationError() const;
    bool channelCreateRequestFailed() const;
    QVariantMap channelCreationForm() const;
    bool canCheckChannelCreation() const;
    bool canRetryChannelMembers() const;
    QVariantMap channelInfoStream() const;
    QVariantList channelInfoTopics() const;
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
    bool cachedSearchAvailable() const { return m_cache && m_cache->available() && !m_cacheLease.owner.isEmpty(); }
    QVariantList cachedSearchResults() const { return m_searchOwner == ownerKey() ? m_searchResults : QVariantList{}; }
    QString cachedSearchState() const { return m_searchOwner == ownerKey() ? m_searchState : QString{}; }
    Q_INVOKABLE void searchCachedMessages(const QString &query);
    Q_INVOKABLE void clearCachedSearch();
    Q_INVOKABLE bool openCachedSearchResult(const QString &id);
    QVariantList streams() const;
    // Issue #867 (subtask A): resolve one stream row by UUID without building
    // the computed catalog; QML single-ID lookups must not traverse the
    // reference sequence (each element access re-reads the backing property).
    // Returns an empty map for an unknown or archived id; the caller early-
    // returns on an empty target.
    Q_INVOKABLE QVariantMap streamById(const QString &id) const;
    // Issue #867 test-only oracle: how many full streams() projections ran.
    // Intentionally has no NOTIFY - nothing in QML may bind to it.
    int streamsProjectionCountForTest() const { return m_streamsProjectionCount; }
    // Issue #868 test-only oracle: how many full topics() projections ran.
    // Intentionally has no NOTIFY - nothing in QML may bind to it.
    int topicsProjectionCountForTest() const { return m_topicsProjectionCount; }
    // #866 review (finding 3): the preview-only revision counter (see the Q_PROPERTY note).
    int previewRevision() const { return m_previewRevision; }
    // #866 review (finding 3, design round): the QML seam for sidebar preview fields.
    // `revision` is intentionally unused by the lookup: delegates pass controller.previewRevision
    // so the QML binding re-evaluates on every coalesced preview announcement (QML var
    // identity is not stable across property reads, so the counter is the dependency edge).
    Q_INVOKABLE QVariantMap previewFields(const QString &lastMessageUuid, int revision) const;
    QVariantList folders() const;
    QVariantList sidebarStreams() const;
    // Lazily creates the sidebar model and wires its sync connections
    // (issue #866 finding 2); pure-C++ consumers never touch this seam.
    NativeSidebarStreamListModel *sidebarStreamListModel();
    QVariantMap sidebarCatalogState() const;
    QStringList expandedSidebarStreams() const { return m_expandedSidebarStreams; }
    Q_INVOKABLE QVariantMap sidebarTopicState(const QString &stream, bool includeRows) const;
    Q_INVOKABLE void requestSidebarTopics(const QString &stream);
    Q_INVOKABLE void toggleSidebarStream(const QString &stream);

    // Issue #635: topic-tab save of the topic summary settings dialog - dirty-only
    // patching of summary_enabled / summary_system_prompt / summary_reasoning_effort
    // over the reference set_summary_prompt surface. Empty prompt = app default (sent
    // as null), empty effort = provider default.
    Q_INVOKABLE void saveTopicSummaryTopic(const QString &topic, bool enabled,
        const QString &prompt, const QString &effort);
    // Issue #739: field-wise variant - a key present in the map is a candidate
    // edit by the current user (values: enabled bool, prompt/effort string or
    // null for the default); an absent key is never sent, so a newer remote
    // change to an untouched field survives the save.
    Q_INVOKABLE void saveTopicSummaryTopicPatch(const QString &topic, const QVariantMap &fields);
    // Issue #636 (docs/NATIVE_DESKTOP_TOPIC_CONTEXT_ACTIONS.md): topic context
    // menu actions, reference runWorkspaceTopicNotificationUpdate /
    // runWorkspaceTopicRead / runWorkspaceTopicRenameRequest /
    // runWorkspaceTopicDoneToggle (messenger-sidebar-actions.lib.ts:1083-1121).
    // Actions: notifications (mute|unmute|default|follow), read, rename,
    // toggle_done (and the drawer's summary) against stream/topic.
    Q_INVOKABLE void applyTopicAction(const QString &stream, const QString &topic,
        const QString &action, const QString &value);
    Q_INVOKABLE bool topicActionBusy(const QString &topic) const;
    Q_INVOKABLE QVariantMap topicActionState(const QString &stream, const QString &topic) const;

    Q_INVOKABLE void setSidebarSearchQuery(const QString &query);
    QString selectedFolder() const;
    QVariantList topics() const;
    bool topicResolving() const { return m_topicOpening.active; }
    bool topicsComplete() const { return m_topicsVerified; }
    QVariantList messages() const;
    qulonglong historyCommitRevision() const { return m_historyCommitRevision; }
    bool messageHistoryReady() const;
    QStringList readRequestBoundaryIds() const;
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
    QString composerPreviewBody() const;
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
    // Test seam: overrides the bounded escalation window (ms) after which a
    // stalled realtime reconnect surfaces the banner (issue #717).
    void setReconnectEscalationGraceMs(int ms);
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
    // Calendar section page (#881, docs/NATIVE_DESKTOP_CALENDAR.md): UI-phase
    // events for the last fetched window; the live adapter is #852.
    QVariantList calendarEvents() const { return m_calendar.events; }
    bool calendarLoading() const { return m_calendar.loading; }
    bool calendarError() const { return m_calendar.error; }
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
    QString loginUsername() const;
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
    Q_INVOKABLE void clearLocalCache();
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
    NativeExternalAccounts *externalAccounts() const { return m_externalAccounts; }
    NativeExternalProviderAdmin *externalProviderAdmin() const { return m_externalProviderAdmin; }
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
    // Calendar section page (#881): fetch the visible [from,to] window of
    // events (GET /calendar/events?from&to) and re-fetch it on retry.
    Q_INVOKABLE void requestCalendarWindow(const QString &from, const QString &to);
    Q_INVOKABLE void retryCalendar();
    // Issue #46: the Licenses page dataset (docs/NATIVE_DESKTOP_LICENSES.md),
    // read from the build-time Qt resource as one JSON string.
    Q_INVOKABLE static QString licensesDatasetJson();
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
    Q_INVOKABLE QVariantMap diagnosticsSupportMeasurements() const;
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
    Q_INVOKABLE void applyChannelTopicAction(const QString &id, const QString &action,
                                             const QString &value = {});
    Q_INVOKABLE void setChannelInfoNotificationMode(const QString &mode);
    Q_INVOKABLE void removeChannelInfoMember(const QString &bindingUuid);
    // Issue #632: owner-only encrypted key-sharing toggle (PUT with readback
    // verification, confirmed-value precedence over realtime snapshots).
    Q_INVOKABLE void setChatKeySharing(bool allow);
    QVariantMap chatKeySharing() const;
    QString chatKeySharingError() const { return m_chatKeySharingError; }
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
    QVariantMap encryptedOutgoingPresentation() const;
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
    Q_INVOKABLE bool replyToSelection(const QStringList &ids);
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
    Q_INVOKABLE bool canManagePoll(const QString &messageUuid) const;
    Q_INVOKABLE void closePoll(const QString &messageUuid);
    Q_INVOKABLE void updatePollSettings(const QString &messageUuid, const QString &closesAt,
        bool allowAddingOptions);
    Q_INVOKABLE void addPollOption(const QString &messageUuid, const QString &questionUuid,
        const QString &optionUuid, const QString &optionText);
    Q_INVOKABLE void loadPollVoters(const QString &messageUuid, const QString &questionUuid,
        const QString &optionUuid, const QString &pageMarker = QString(), int pageLimit = 0,
        const QString &requestTag = {});
    Q_INVOKABLE void cancelPollVoterRequest(const QString &requestTag);
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
    Q_INVOKABLE QVariantMap attachmentDownload(const QString &fileUuid) const;

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
    void cachedSearchAvailabilityChanged();
    void cachedSearchChanged();
    void channelInfoTopicsChanged();
    void channelTopicActionFinished(const QString &id, const QString &action, bool success);
    void topicActionFinished(const QString &streamUuid, const QString &topicUuid,
        const QString &action, bool success);
    void topicActionsChanged();
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
    void readRequestBoundariesChanged();
    void archivedChatsChanged();
    // Issue #50: one finished Diagnostics ping (ok + latency ms); the page
    // mirrors the reference ping reset timer (3 s, logs-page.ui.tsx:238-242).
    void pingFinished(bool ok, int latencyMs);
    // Issue #51: the session log ring buffer changed (append or clear); the
    // log viewer modal re-reads its rows on this signal (reference
    // subscribeLogHistory).
    void sessionLogChanged();
    void supportScopeChanged();
    void stateChanged();
    void calendarChanged();
    void secretKeysChanged();
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
    // #866 review (finding 3): preview-content announcements only; see previewRevision.
    void previewChanged();
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
        QVariantList voters, QString nextMarker, QString error, QString requestTag);
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
    void beginAnchorLinkJump(const QString &target, const QString &cachedStream = {}, const QString &cachedTopic = {});
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
        bool allowKeySharingByHolders = false;
        bool encrypted = false;
    };
    QVariantMap presentMessageSecrets(const QString &messageId, const QStringList &ordinaryOpened,
        const QStringList &revealed, const QString &requestHandle, QString *requestedStream, QString *requestedKey, bool validating = false);
    quint64 m_secretKeyRevision = 0;
    QString m_secretRequestStream;
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
    void fetchChatAuthority(const QString &stream, quint64 epoch, quint64 selection,
        std::function<void(const ChatAuthority &, const QString &)> done);
    void finishChatAuthority(quint64 epoch, quint64 selection, const QString &error = {});
    void chatCollection(const QString &path, QUrlQuery query, quint64 epoch, quint64 selection,
        std::function<void(const QJsonArray &, const QString &)> done, QJsonArray rows = {}, QSet<QByteArray> markers = {},
        int pages = 0, qint64 bytes = 0);
    void finishChatOperation(quint64 operation, const QString &key, const QString &phase, const QString &error = {});
    void startChatRequest(const QString &key, bool checking, bool retrying, const QString &secretStream = {}, std::function<bool()> stillNeeded = {});
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
    void postChatRequest(const NativeChatKeyVault::Address &address, const NativeChatKeyVault::PendingRequest &pending, quint64 epoch, quint64 selection, quint64 operation);
    bool matchesChatRequest(const Message &message, const QString &key,
                            const NativeChatKeyVault::PendingRequest &pending, bool observed, const QString &source = {}) const;
    bool bindChatRequest(const Message &message, const QString &key,
                         const NativeChatKeyVault::PendingRequest &pending, bool observed = false, const QString &source = {});
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
    const PreparedChatMessage *currentChatPresentation(const Message &message) const;
    QString chatPresentationState(bool allowed, const PreparedChatMessage *prepared) const;
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
        bool fromDrawer = false;
    };
    bool chatIssuerAllowed() const;
    bool chatGrantIssuerAllowed(const ChatAuthority &authority, const QString &issuer, const QString &requester) const;
    bool chatResponseAllowed(const ChatRequestFact &fact, bool grant) const;
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
    void startChatKeyResponse(const QString &request, bool grant, bool fromDrawer);
    void recoverChatKeyResponse(const QString &request, bool retry);
    void finishChatKeyResponse(const QString &id, quint64 ticket, const QString &error);
    void postChatKeyResponse(const QString &id, quint64 ticket);
    void readChatKeyResponse(const QString &id, quint64 ticket, bool retry);
    bool matchesChatKeyResponse(const QJsonObject &raw, bool realtime) const;
    bool confirmChatKeyResponse(const QJsonObject &raw, const QString &id, quint64 ticket, bool realtime = false);
    std::optional<ChatKeyResponse> m_chatKeyResponse;
    QStringList m_chatGrantQueue;
    QHash<QString, QString> m_chatGrantErrors;
    QSet<QString> m_failedBulkGrants;
    QString m_chatGrantInFlight;
    quint64 m_chatGrantEpoch = 0;
    void advanceChatGrants();
    void settleChatGrant(const QString &request, bool ambiguous);
    void resetChatGrantQueue();
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
                        const Topic *initialTopic = nullptr, quint64 openingTicket = 0,
                        bool openingFavorites = false);
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
    // Issue #789: opens the cache for the vault-resolved startup identity
    // before the session refresh chain completes; see startupCacheMode.
    void openCacheForStartupIdentity();
    // Issue #789: projects the saved channel list once that startup cache is
    // ready, so the first paint never waits on the token/introspect chain.
    void loadStartupCacheStreams();
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
                          std::function<void()> failed = {}, quint64 selectedTopicRequest = 0);
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
        // #680 review: the echoed upload metadata's HEIC verdict (-1 unknown
        // until the echo, 0 no, 1 yes). Preview selection and decode
        // scheduling must consult this one source, never the filename alone.
        int heicEcho = -1;
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
        // Review 4650261678 (issue #631): wall-clock instant of the transition
        // to "downloaded", so the card's confirmation window survives delegate
        // recreation (0 while the entry has not completed).
        qint64 completedAtMs = 0;
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
    // #680: staged decoded preview for a ready HEIC composer attachment.
    // The decode runs off the GUI thread through the bounded libheif decoder;
    // completion is fenced on the upload generation/epoch and on the row still
    // existing, so remove/sign-out/conversation churn drops it silently.
    void scheduleUploadPreview(quint64 intent, const QString &path, quint64 generation, quint64 epoch);
    void dropUploadPreview(quint64 intent);
    QString clipboardUploadName(const QString &extension, bool image) const;
    void validateQueuedUpload(quint64 intent, quint64 epoch);
    QStringList readyUploadLinks(quint64 session) const;
    std::optional<QString> composeSendBody() const;
    std::optional<QString> composeEditBody() const;
    void reserveImageAlias(UploadEntry &entry, const QString &name);
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
    void interruptForward(bool sameOwnerRenewal);
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
                 const QUrlQuery &query, ApiClient::Callback callback,
                 ApiClient::TokenRenewal renewal = ApiClient::TokenRenewal::Cancel);
    void collection(const QString &path, QUrlQuery query,
                    std::function<void(const QJsonArray &)> done,
                    QJsonArray accumulated = {}, QSet<QByteArray> markers = {},
                    std::function<void()> failed = {});
    void loadTopics();
    // Issue #83: bootstrap GET /folders/ with the same catalog guard as the
    // other snapshot collections; failures keep (or install) the synthesized
    // system trio so the rail never renders empty.
    void loadFolders();
    // Issue #719: a confirmed stream read decays the stored unread counts of the
    // folders that reference the stream (reference: the folder badge is derived
    // from the same unread ledger as the sidebar rows, so a read must move the
    // rail without waiting for the next /folders/ refetch).
    void decayFolderUnread(const QString &streamId, int cleared);
    // Notification metadata backfill: topics of background streams are not in
    // the active catalog, so mute policy fetches the detail on demand.
    void requestTopicMetadata(const QString &topicUuid);
    void loadMessages(bool older = false, bool latest = false, bool forceNetwork = false);
    void retryInterruptedHistory();
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
    void startAuthenticatedWorkspace();
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

    NativeExternalAccounts *m_externalAccounts;
    NativeExternalProviderAdmin *m_externalProviderAdmin;
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
    QString m_searchOwner, m_searchState, m_searchQuery;
    void refreshOpenCachedSearch(bool changeAccepted);
    QVariantList m_searchResults;
    quint64 m_searchTicket = 0;
    QPointer<NativeCacheService> m_cache;
    QMetaObject::Connection m_cacheDestroyed;
    QMetaObject::Connection m_cacheStatusChanged;
    QHash<int, std::function<void()>> m_cacheHydrations;
    bool m_cacheWasAvailable = false;
    bool m_cacheForgettingAccount = false;
    quint64 m_localResetGeneration = 0;
    NativeCache::Lease m_cacheLease;
    // Issue #789: startup cache-first state (see startupCacheMode). The
    // identity memo is the server|project|user tuple the bootstrap opened.
    bool m_startupCacheMode = false;
    QString m_startupIdentity;
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
    struct InterruptedHistory { quint64 selection; QString error; };
    std::optional<InterruptedHistory> m_interruptedHistory;
    quint64 m_realtimeRecovery = 0;
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
    void updateStreamNotificationMode(QString id, const QString &mode, bool fromDrawer);
    bool channelInfoContext(quint64 ticket, const QString &stream) const;
    void loadChannelInfoPage(const QString &marker, quint64 ticket, const QString &stream,
                             QVector<ChannelInfoMember> accepted, QSet<QString> seenUsers);
    QString m_channelInfoStreamId;
    // Issue #645: a topic action rides the generation that dispatched it; once that
    // generation is retired its wrapper callback can never fire, so keep the identity
    // needed to settle the UI operation from the renewal itself.
    struct ChannelTopicAction { QString id; QString action; quint64 generation; };
    QHash<QString, ChannelTopicAction> m_channelTopicRequests;
    // Issue #645 review (4653853398): keys of topic writes retired by a session
    // renewal. Their replies prove the server settled the write, so the drawer
    // rows are reconciled once more when such a reply arrives fenced.
    QSet<QString> m_channelTopicInterruptedWrites;
    // Issue #636: one in-flight action per topic (any action), matching the
    // drawer's arbitration that pinned cases rely on. Key: "<generation>/<topic>".
    QHash<QString, quint64> m_topicActionRequests;
    // Issue #645: action-lifetime ledger, key "<stream>|<topic>|<action>",
    // value the dispatching owner. Unlike the arbitration entry it survives
    // the retired callback (the wrapper can deliver a generation-fenced
    // answer that erases arbitration but never settles), so the session-
    // renewal sweep knows exactly which guards to settle as interrupted and
    // can fence foreign owners.
    QHash<QString, QString> m_topicActionLedger;
    // Guard/error projection per "<stream>|<topic>": <action>Pending and
    // <action>Error keys, consumed by the topic context menu.
    QHash<QString, QVariantMap> m_topicActionStates;
    quint64 m_topicActionToken = 0;
    void settleTopicAction(const QString &stream, const QString &topic,
        const QString &action, bool ok, const QString &errorMessage = QString());
    // Issue #635: shared transport of applyTopicAction (extraction seam) - per-topic
    // arbitration, renewal fencing and row merge with caller-supplied coordinates.
    void dispatchTopicAction(const QString &stream, const QString &topic, const QString &action,
        bool current, const QByteArray &method, const QString &path, const QJsonObject &body);
    void interruptTopicActions();
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
    // Issue #632: key-sharing toggle state, one in-flight request per stream
    // with a token guard mirroring the notification-mode mutation.
    QHash<QString, quint64> m_chatKeySharingInFlight;
    quint64 m_chatKeySharingToken = 0;
    QString m_chatKeySharingError;
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
    // Load missing membership on a visible channel, once per pending or successful snapshot.
    void ensureChatHeaderMembers();
    void publishChatHeaderMembers(QStringList accepted);
    QString m_chatHeaderMembersStream;
    QString m_chatHeaderMembersOwner;
    QStringList m_chatHeaderMemberUuids;
    bool m_chatHeaderVisible = false;
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
        QString state = QStringLiteral("ready");
        QString kind;
        QDateTime created;
        // Issue #102 (docs/NATIVE_DESKTOP_CHAT_LIST_DENSITY.md): the topic the previewed
        // message lives in - the reference stream.preview?.route input for the
        // latestTopic click mode (sidebar-workspace.ui.tsx:455).
        QString topic;
    };
    std::optional<RowPreview> rowPreview(const QString &uuid) const;
    std::optional<RowPreview> rowPreviewOf(const Message &message, bool active = false) const;
    void refreshEncryptedSidebarPreviews();
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
    // #866 review (finding 3): coalesced preview-revision bump - one previewChanged per
    // pump tick no matter how many memos that tick stored (replaces the streamsChanged
    // announcements the preview paths used to fire).
    void schedulePreviewRevisionBump();
    // exordy finding on PR #866 (medium): a hydration pump that learns the real created
    // stamp of a row's last message must let it participate in the activity comparator.
    // applyHydratedRowActivity records the stamp in the same ledger noteActivityMerge
    // feeds and reports whether the projected order actually moved; that report gates one
    // coalesced streamsChanged - the QML binding edge that re-reads the projection.
    // #861: topic rows own pointers too - a moved topic projection announces its own
    // coalesced topicsChanged from inside the function (the stream report stays with
    // the caller so the two projection edges stay independent).
    QVector<const Stream *> orderedStreamPointers() const;
    QVector<const Topic *> orderedTopicPointers(const QVector<Topic> &topics,
                                                const Stream *stream) const;
    bool applyHydratedRowActivity(const Message &message);
    void scheduleStreamsReorderAnnounce();
    void scheduleTopicsReorderAnnounce();
    quint64 bumpRowPreviewFence(const QString &uuid);
    qint64 rowActivityMicros(const QString &lastMessage, qint64 updatedMicros) const;
    // Issue #867 (subtask A): single-row projection shared by streams() and
    // streamById(); building one row must never cost the whole catalog.
    QVariantMap streamRowMap(const Stream &stream) const;
    // Issue #718 (docs/NATIVE_DESKTOP_PARITY.md): sidebar sort keys are recorded
    // when a message is first seen, so a row keeps its position when its
    // last-message pointer's row leaves the active window on a route change.
    QVector<Message> noteActivityMerge(const QVector<Message> &existing, const QVector<Message> &incoming);
    void noteActivityTimestamps(const QVector<Message> &rows);
    QHash<QString, qint64> m_activityTimestamps;
    // Issue #718 slice 2: bounded background preload of a small message-count
    // slice per populated topic + swap-on-ready projection in selectTopic.
    // Encrypted (e2ee) rows never become slices - the e2ee runtime owns those
    // windows end to end - and NativeCache/history budgets are untouched.
    struct TopicSlice { QString stream; QVector<Message> rows; QByteArray before; };
    void scheduleTopicPreload(int delayMs = 0);
    void runTopicPreloadBatch();
    void requestTopicPreload(const Topic &topic);
    void rememberRecentRoute(const QString &stream, const QString &topic);
    void rememberTopicSlice(const QString &topicUuid, const QString &stream,
                            const QVector<Message> &window, const QByteArray &before, int keepRows);
    void dropTopicSlices(const QString &stream, const QString &topic);
    QHash<QString, TopicSlice> m_topicSlices;
    QVector<QString> m_topicSliceOrder;
    QSet<QString> m_topicPreloadSkipped;
    QSet<QString> m_topicPreloadInFlight;
    // Independent of the bounded slice cache: once a topic's page has been
    // fetched successfully this session it is done, even if its slice was
    // evicted from the warm map - eviction must not re-arm the sweep
    // (issue #718 review round, exo-review findings 1/4).
    QSet<QString> m_topicPreloadDone;
    QTimer *m_topicPreloadTimer = nullptr;
    // Product default is ON; window-shaped test groups and debugging disable
    // the sweep with WORKSPACE_NATIVE_TOPIC_PRELOAD=0 (ctest ENVIRONMENT),
    // because fixtures that pin exact /messages/ traffic or delegate geometry
    // must not observe a background page landing mid-case.
    bool m_topicPreloadEnabled =
        qEnvironmentVariable("WORKSPACE_NATIVE_TOPIC_PRELOAD", "1") != "0";
    int m_topicPreloadIntervalMs = 1000;
    // Issue #729 phase 2: cross-route warm queue. The recency list is fed by
    // selection commits (front = newest, capped); the foreground sequence
    // fences warm replies that a newer foreground page already superseded;
    // the throttle delay doubles per 429/5xx and re-arms at the catalog seam.
    QVector<QPair<QString, QString>> m_recentRoutes;
    QHash<QString, quint64> m_topicForegroundSeq;
    // Issue #777: per-topic history invalidation counter. A warm page is a
    // dispatch-time snapshot, so a reply may only be remembered when the
    // topic's saved history was not invalidated while the request was in
    // flight; the counter is raised with every slice drop.
    QHash<QString, quint64> m_topicHistoryInvalidations;
    int m_topicPreloadBackoffMs = 0;
    // True while the open window shows a swap-on-ready slice: the next
    // verified page of this selection may overwrite that frame.
    bool m_preparedSwapActive = false;
    int streamGroupRank(const Stream &stream) const;
    QVariantMap rowPreviewFields(const QString &lastMessage) const;
    // Bounded cache warm for row previews (docs/NATIVE_DESKTOP_SIDEBAR_PREVIEWS.md section 2):
    // the projection is const and memoizes misses here; only the pump, the payload-change
    // paths and the sign-out reset mutate these fields, always on the GUI thread.
    static constexpr int rowPreviewCacheLimit = 512;
    static constexpr int rowPreviewHydrationBatch = 8;
    // #866 review (finding 3): preview-only revision counter + coalesce flag.
    int m_previewRevision = 0;
    bool m_previewRevisionBumpQueued = false;
    // exordy finding on PR #866: coalesce flag for the activity-reorder streamsChanged.
    bool m_streamsReorderQueued = false;
    bool m_topicsReorderQueued = false;
    // Issue #866 (finding 2): lazily created sidebar row model (parented here).
    NativeSidebarStreamListModel *m_sidebarStreamModel = nullptr;
    // #860: a disk miss is not an authoritative negative - the reference falls through to the
    // project-scoped messages lookup during bootstrap (messenger-last-messages-loader.lib.ts
    // L96-129). The network stage is bounded per tick like the disk pump, retries transient
    // failures a bounded number of times, and memoizes only authoritative outcomes.
    static constexpr int rowPreviewNetworkBatch = 8;
    static constexpr int rowPreviewNetworkRetries = 2;
    // #860: a server-side empty page is not the end of the story when a disk
    // body may still be on its way - the miss earns this many delayed
    // disk-stage re-reads before the quiet memo is finally stored.
    static constexpr int rowPreviewLateDiskRetries = 2;
    void seedRowPreviewHydrationFromCatalog();
    // #888: a single stream's topic catalog accept seeds only its own rows.
    void seedRowPreviewHydrationForStream(const QString &streamUuid);
    // #888: work oracle for the linear-enumeration contract - counts every catalog
    // record inspection (stream row or topic row) attributed to preview seeding.
    void recordRowPreviewSeedWork(int inspections) const { m_rowPreviewSeedWork += inspections; }
    void scheduleRowPreviewNetworkHydration();
    void pumpRowPreviewNetworkHydration();
    // #866 review (exordy finding 2): the pumps consume their pending queues in a
    // foreground-priority order - pointers belonging to the open route or to an
    // expanded sidebar stream are dispatched ahead of hidden catalog candidates.
    void enqueueRowPreviewDisk(const QString &uuid);
    void enqueueRowPreviewNet(const QString &uuid);
    bool isRowPreviewForeground(const QString &uuid) const;
    QString takeRowPreviewCandidate(QQueue<QString> &queue) const;
    // #866 review (exordy finding 1): every in-flight lookup carries a token; only the
    // request that still owns the uuid's marker may retire it, so a reply from a retired
    // session can never free a newer request's in-flight slot.
    quint64 m_rowPreviewDiskTokenSeq = 0;
    QHash<QString, quint64> m_rowPreviewDiskTokens;
    quint64 m_rowPreviewNetTokenSeq = 0;
    QHash<QString, quint64> m_rowPreviewNetTokens;
    mutable QQueue<QString> m_rowPreviewDiskQueue; // mutable: rowPreview() feeds it from a const read
    QQueue<QString> m_rowPreviewNetQueue;
    // uuid -> owning stream uuid, filled by the catalog seed; drives the foreground pick.
    QHash<QString, QString> m_rowPreviewStreamOf;
    QHash<QString, QString> m_visibleEncryptedPreviewStates;
    mutable QHash<QString, RowPreview> m_rowPreviews;
    mutable QQueue<QString> m_rowPreviewOrder;
    mutable QSet<QString> m_rowPreviewPending;
    mutable QSet<QString> m_rowPreviewInFlight;
    mutable bool m_rowPreviewPumpQueued = false;
    mutable bool m_rowPreviewNetworkEnabled = true;
    mutable bool m_rowPreviewNetworkPumpQueued = false;
    QSet<QString> m_rowPreviewNetPending;
    QSet<QString> m_rowPreviewNetInFlight;
    QHash<QString, int> m_rowPreviewNetRetries;
    int m_rowPreviewNetIssued = 0; // test-only dispatch counter (rowPreviewNetworkIssuedForTest)
    mutable qint64 m_rowPreviewSeedWork = 0; // #888 test-only inspection counter
    // #888 (exordy round 2, finding 4713852581): the scoped topic-accept seed runs
    // once per accepted stream; locating the owning record with a full scan made a
    // sweep of N accepts quadratic in the channel list. This index maps a stream
    // uuid to its row in m_streams and is rebuilt only after a structural change
    // of the streams collection (m_streamsMutation), so repeated accepts reuse one
    // linear pass instead of re-scanning per accept.
    int streamRowByUuid(const QString &uuid);
    quint64 m_streamsMutation = 0;   // bumped by every structural m_streams change
    quint64 m_streamsRowTag = ~0ull; // mutation tag m_streamsRow was built at
    QHash<QString, int> m_streamsRow;
    // #860: per-uuid budget for the late disk re-reads a network negative buys.
    QHash<QString, int> m_rowPreviewLateDiskRetries;
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
    QStringList m_directoryUserOrder;
    QVariantMap m_mentionSelfChoice;
    QString m_channelDirectoryOwner;
    // Issue #867: counts full streams() projection builds for the regression
    // tests; mutable because the counted getter is const.
    mutable int m_streamsProjectionCount = 0;
    // Issue #868: same oracle for the topics() projection (see above).
    mutable int m_topicsProjectionCount = 0;
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
    const Message *replyTarget(const QString &id) const;
    bool appendReplyTabs(const QStringList &ids, const QString &selectedText, bool keepRestoredEdit);
    const ReplyTab *activeReplyTab() const;
    QString replyTabAuthor(const ReplyTab &tab) const;
    QString replyTabPlain(const ReplyTab &tab) const;
    // The getters' shared live-resolution sources, reused by the park path so
    // parked memos come from exactly the same data the banner renders from.
    std::optional<QString> replyPlainFor(const QString &id) const;
    void primeReplyTabMemos(QList<ReplyTab> &tabs) const;
    QHash<QString, ApiClient::RequestId> m_pollVoterRequests;
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
    void resetMentionIndex();
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
    void openSelfStream(const QString &streamUuid);
    void settleFavoritesTopics(bool success);
    void leaveFavorites();
    // Unread-mentions index entries (activity.model.ts ActivityUnreadMention):
    // stream/topic/created_at are needed for the read-boundary and scope clears
    // of activity-realtime-applier.lib.ts:55-105.
    struct MentionIndexEntry { QString stream, topic; qint64 createdMicros = 0; };
    using MentionIndex = QHash<QString, MentionIndexEntry>;
    using MentionMutation = std::function<bool(MentionIndex &)>;
    bool mutateMentionIndex(MentionMutation mutation);
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
    MentionIndex m_unreadMentions;
    MentionIndex m_fetchedMentions;
    QList<MentionMutation> m_mentionWalkMutations;
    QVector<Message> m_liveMentions;
    QSet<QString> m_deletedMentionIds;
    QStringList m_deletedMentionOrder;
    bool m_mentionIndexReady = false;
    bool m_mentionIndexWalking = false;
    QTimer m_mentionIndexRetry;
    int m_mentionRetryCount = 0;
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
    void ensureSidebarStreamExpanded(const QString &stream);
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
    // Calendar section page (#881): fetch state for the visible date window.
    // The version fences late answers of a retired window (same monotonic
    // identity discipline as FeedState, issue #356); a re-show of the same
    // loaded window is not re-fetched, so section re-entry is traffic-free.
    struct CalendarState {
        QVariantList events;
        QString from, to;
        bool loading = false, error = false, inflight = false, hasLoaded = false;
        quint64 version = 0;
    };
    CalendarState m_calendar;
    void resetCalendar();
    void loadFeedPage(bool append);
    void interruptFeedRequest();
    void interruptCalendarRequest();
    // Issue #423: settle a folder mutation retired by a generation bump.
    void interruptFolderMutation();
    bool interruptChannelTopicActions();
    void resetFeed();
    QVariantList feedRows() const;
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
    // #680: intent -> staged decoded PNG (composer HEIC previews) and the
    // in-flight decode watchers owned by this controller.
    QHash<quint64, QString> m_uploadPreviews;
    QHash<quint64, QFutureWatcher<QImage>*> m_uploadPreviewJobs;
    // #680 fix round: per-attachment cooperative cancellation token, observed
    // at every stage boundary of the preview worker and handed to libheif's
    // cancel_decoding, so a removed attachment stops its decode in flight.
    QHash<quint64, std::shared_ptr<std::atomic_bool>> m_uploadPreviewCancel;
    std::unique_ptr<class QTemporaryDir> m_uploadPreviewDir;
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
    bool rejectMismatchedLoginOrganization(const QUrl &origin);
    void discoverInitialLinkedOrganization();
    void discoverLoginOrganization(const QString &text, quint64 linkSerial);
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
    bool m_anchorFromCachedSearch = false;
    QString m_anchorNavigationError;
    QString m_failedAnchorLinkTarget;
    QString m_failedAnchorConversation;
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
    void leaveLoginStep(const QString &step);
    bool m_preserveLoginWizard = false;
    QString m_previousLoginName;
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
    QMetaObject::Connection m_sidebarSortBinding, m_sidebarLocaleBinding;
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
    // Issue #717: backoff retries seen since the last live link. The
    // reconnecting banner stays quiet while a drop is self-recovering and
    // surfaces only once the first reconnect retry has also failed.
    int m_reconnectRetriesSinceLive = 0;
    // Issue #717 (exo-review): bounded escalation window. A realtime phase
    // that stays unstable without further retry transitions (a held catch-up
    // request) must still surface the banner once this window expires.
    int m_reconnectEscalationGraceMs = 10000;
    // Start of the current unstable realtime streak (0 while stable); the
    // banner escalates once the streak outlives the grace window.
    qint64 m_reconnectUnstableSinceMs = 0;
    qsizetype m_supportLatencyCount = 0;
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
    // Tail arrivals cannot overlap a page requested before the oldest loaded row.
    quint64 m_historyTailAppends = 0;
    quint64 historyPageRevision(bool older) const
    {
        return older ? m_messageRevision - m_historyTailAppends : m_messageRevision;
    }
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
    quint64 m_historyTimingGeneration = 0;
    quint64 m_scrollSampleSelection = 0;
    qint64 m_historyPageWaitMs = 0;
    QElapsedTimer m_scrollSampleClock;
    qreal m_previousTopDistance = 0;
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
    // Issue #857: a call is addressed to a concrete topic (the meeting-URL
    // message is a topic-scoped POST), and the reference only ever opens a
    // conversation on a concrete topic. The no-topic aggregate view has no
    // address to call into, so the gate stays closed there.
    bool canStartCall() const
    { return authenticated() && hasConversation() && !m_topic.isEmpty() && !topicClosed() && !m_meetHost.isEmpty(); }
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

public:
    // ---- Issue #635: AI topic-summary settings (docs/topic-summary-settings.md) ----
    // Controller surface of the reference dialog (workspace_ui
    // shared/api/messenger-topic-summary-management.api.ts at e0194ff); the QML
    // dialog consumes only this surface plus topicSummaryTabPermission.
    Q_PROPERTY(QVariantMap topicSummaryGates READ topicSummaryGates NOTIFY topicSummaryStateChanged)
    // True only while the gates DTO currently on screen was fetched successfully
    // for the open project (review #708 finding 8): a failed GET must leave the
    // dialog unable to edit or save unverified gate values. No extra emissions -
    // it rides the existing topicSummaryStateChanged NOTIFY.
    Q_PROPERTY(QVariantList topicSummaryEndpoints READ topicSummaryEndpoints NOTIFY topicSummaryStateChanged)
    Q_PROPERTY(bool topicSummarySettingsBusy READ topicSummarySettingsBusy NOTIFY topicSummaryStateChanged)
    Q_PROPERTY(bool topicSummaryGatesLoaded READ topicSummaryGatesLoaded NOTIFY topicSummaryStateChanged)
    Q_PROPERTY(QString topicSummarySettingsStatus READ topicSummarySettingsStatus NOTIFY topicSummaryStateChanged)
    Q_PROPERTY(QString topicSummarySettingsError READ topicSummarySettingsError NOTIFY topicSummaryStateChanged)
    // #795: which tab families ("gates"/"endpoints") produced the current error.
    // A shared status cannot tell, and a late reply can land while the other
    // tab is on screen, so each error band gates on its own producer. A single
    // interruption can retire in-flight ops of both families at once, hence a
    // list: every affected family stays attributed (finding 4687482019).
    Q_PROPERTY(QStringList topicSummarySettingsErrorOps READ topicSummarySettingsErrorOps NOTIFY topicSummaryStateChanged)

    QVariantMap topicSummaryGates() const { return m_topicSummaryGates; }
    bool topicSummaryGatesLoaded() const { return m_topicSummaryGatesLoaded; }
    QVariantList topicSummaryEndpoints() const { return m_topicSummaryEndpoints; }
    bool topicSummarySettingsBusy() const { return m_topicSummarySettingsBusy; }
    QString topicSummarySettingsStatus() const { return m_topicSummarySettingsStatus; }
    QString topicSummarySettingsError() const { return m_topicSummarySettingsError; }
    QStringList topicSummarySettingsErrorOps() const { return m_topicSummarySettingsErrorOps; }

    // "allowed" | "denied" | "unknown" for tab "topic" | "gates" | "endpoints",
    // mirroring messenger-topic-summary-permissions.lib.ts.
    Q_INVOKABLE QString topicSummaryTabPermission(const QString &tab) const;
    Q_INVOKABLE void loadTopicSummaryGates();
    Q_INVOKABLE void saveTopicSummaryGates(bool globalEnabled, bool projectEnabled);
    Q_INVOKABLE void loadTopicSummaryEndpoints();
    Q_INVOKABLE bool createTopicSummaryEndpoint(const QVariantMap &form);
    Q_INVOKABLE bool updateTopicSummaryEndpoint(const QString &uuid, const QVariantMap &form);
    Q_INVOKABLE void deleteTopicSummaryEndpoint(const QString &uuid);
    // Client-side rules of the reference endpoint form; returns offending field
    // names (empty when the form is valid), so Save can gate before dispatch.
    Q_INVOKABLE QStringList topicSummaryEndpointValidationErrors(const QVariantMap &form, bool isCreate) const;

signals:
    void topicSummaryStateChanged();

private:
    // At most one in-flight request per operation key ("gates", "endpoints",
    // "endpoint:<uuid>"); token 0 means the key was already busy.
    quint64 beginTopicSummaryOp(const QString &key);
    bool finishTopicSummaryOp(const QString &key, quint64 token);
    void interruptTopicSummaryOps();
    void setTopicSummaryStatus(const QString &status, const QString &error = {},
        const QString &errorOp = {});
    // Multi-producer variant used by the interruption sweep: every family whose
    // op was retired stays attributed (finding 4687482019).
    void setTopicSummaryStatusOps(const QString &status, const QString &error,
        const QStringList &errorOps);
    // Issue #840: a successful load of one family must not erase a live error
    // belonging to the other family (the bands filter on the ops ledger).
    void settleTopicSummaryLoadSuccess(const QString &family);
    QHash<QString, quint64> m_topicSummaryOps;
    quint64 m_topicSummaryToken = 0;
    QVariantMap m_topicSummaryGates;
    bool m_topicSummaryGatesLoaded = false;
    QVariantList m_topicSummaryEndpoints;
    bool m_topicSummaryCreateNeedsReconcile = false;
    bool m_topicSummarySettingsBusy = false;
    QString m_topicSummarySettingsStatus;
    QString m_topicSummarySettingsError;
    QStringList m_topicSummarySettingsErrorOps;
};

}
