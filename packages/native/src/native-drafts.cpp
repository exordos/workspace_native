#include "native-drafts.h"
#include "domain.h"

#include <QDateTime>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQueue>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <limits>

namespace workspace {
namespace {
using Failure = NativeDraftService::Failure;
QString key(const QString &stream, const QString &topic) { return stream + '/' + topic; }
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
bool ownerValid(const QString &owner)
{
    static const QRegularExpression pattern("\\A[0-9a-f]{64}\\z");
    return pattern.match(owner).hasMatch();
}
struct Hold {
    QString uuid;
    std::function<void()> released;
    ~Hold() { if (released) released(); }
};
struct Work {
    DraftRecord record;
    quint64 version = 0, durable = 0;
    mutable qsizetype cachedBytes = 0;
    qsizetype bytes() const {
        if (!cachedBytes) cachedBytes = NativeDraftStore::serializedSize(record);
        return cachedBytes;
    }
};
struct Owner {
    QHash<QString, Work> records;
    QHash<QString, QString> active, shadow;
    QHash<QString, quint64> edits;
    QHash<QString, quint64> recent;
    QHash<QString, QVector<std::weak_ptr<const Hold>>> holds;
    QSet<QString> loaded, bootstrap, revisit, completed, dirty, clearPending, invalidShadow;
    std::optional<QSet<QString>> authorized;
    bool scopePending = false;
    Failure failure = Failure::None;
};
struct Waiter { QPointer<QObject> receiver; NativeDrafts::Done done; };
void promotePrivate(DraftRecord &record)
{
    const bool uncertain = !record.revision && (record.createPending
        || record.phase == DraftRecord::Phase::Saving || record.phase == DraftRecord::Phase::Failed
        || record.privateCreateUncertain);
    if (record.conflictRevision) record.revision = record.conflictRevision;
    record.privateDeletion = true; record.privateCreateUncertain = uncertain && !record.revision;
    record.text.clear(); record.replySession = {}; record.createPending = false; record.createText.clear();
    record.conflictText.clear(); record.conflictRevision = 0;
    record.consumed = true; record.phase = DraftRecord::Phase::Deleting;
}
}

struct NativeDrafts::Private {
    NativeDrafts *q;
    QPointer<NativeDraftService> storage;
    QHash<QString, Owner> owners;
    QString owner, stream, topic, workerOwner;
    QTimer saveTimer;
    bool scheduled = false, opening = false, running = false;
    quint64 version = 0, forceThrough = 0, use = 0;
    qint64 timestamp = 0;
    QVector<Waiter> waiters;
    struct Remote {
        enum class Stage { Fetch, Import, Scan, Idle, Failed };
        QPointer<ApiClient> api;
        QMetaObject::Connection generationChanged, destroyed;
        QString owner, project, user, after;
        QSet<QString> streams, accessible, seen, pending, blocked;
        QSet<QByteArray> markers;
        QByteArray marker, next;
        QVector<Draft> page;
        QVector<NativeDraftStore::Summary> summaries;
        QQueue<QString> queue;
        QHash<QString, qint64> due;
        Stage stage = Stage::Failed;
        quint64 epoch = 0, generation = 0;
        bool network = false, paused = false, scanEnd = false, rescan = false, reconcile = false, refetchRequested = false;
        QString job;
        std::shared_ptr<const Hold> hold;
        ApiClient::RequestId request;
        RemoteFailure failure = RemoteFailure::None;
        int attempts = 0;
        qint64 retryUntil = 0;
    } remote;
    QTimer remoteTimer;
    QElapsedTimer clock;
    bool pageVisible = false, pagePending = false, pageLoading = false, pageError = false;
    QString pageOwner, pageCursor, pageNext;
    QStringList pageHistory;
    QVector<NativeDraftStore::Preview> pageRows;
    quint64 pageEpoch = 0, actionEpoch = 0;
    QTimer pageTimer;
    struct ActionRequest {
        QString id, owner, route, snapshot;
        Action action = Action::Open;
        quint64 edits = 0;
    };
    std::optional<ActionRequest> action;

    Private(NativeDrafts *q, NativeDraftService *storage) : q(q), storage(storage)
    {
        saveTimer.setSingleShot(true); saveTimer.setInterval(400);
        clock.start(); remoteTimer.setSingleShot(true);
        pageTimer.setSingleShot(true); pageTimer.setInterval(200);
        QObject::connect(&pageTimer, &QTimer::timeout, q, [this] {
            if (pageVisible) { pagePending = true; schedule(); }
        });
        QObject::connect(&remoteTimer, &QTimer::timeout, q, [this] { schedule(); });
        QObject::connect(&saveTimer, &QTimer::timeout, q, [this] { schedule(); });
        if (storage) {
            QObject::connect(storage, &NativeDraftService::stateChanged, q, [this] { schedule(); });
            QObject::connect(storage, &QObject::destroyed, q, [this] {
                this->storage = nullptr; running = opening = false;
                if (!owner.isEmpty()) owners[owner].failure = Failure::Unavailable;
                const QPointer<NativeDrafts> guard(this->q);
                finish(false);
                if (guard) emit guard->stateChanged();
            });
        }
    }
    void schedule()
    {
        if (scheduled) return;
        scheduled = true;
        QTimer::singleShot(0, q, [this] { scheduled = false; pump(); });
    }
    QString text(const QString &stream, const QString &topic) const
    {
        const auto state = owners.constFind(owner);
        if (state == owners.cend()) return {};
        const auto route = key(stream, topic);
        if (state->shadow.contains(route)) return state->shadow.value(route);
        const auto row = state->records.constFind(state->active.value(route));
        return row == state->records.cend() || row->record.consumed ? QString{} : row->record.text;
    }
    void dirty(Owner &state, Work &work)
    {
        work.cachedBytes = 0;
        work.version = ++version;
        state.dirty.insert(work.record.uuid);
    }
    std::shared_ptr<const Hold> retain(Owner &state, const QString &route, const QString &id)
    {
        auto hold = std::make_shared<Hold>(); hold->uuid = id;
        const QPointer<NativeDrafts> guard(q);
        hold->released = [guard] {
            if (guard) QTimer::singleShot(0, guard, [guard] { if (guard) guard->d->schedule(); });
        };
        state.holds[route].append(hold); return hold;
    }
    void edited(DraftRecord &record)
    {
        record.snapshot = uuid();
        timestamp = qMax(timestamp + 1, QDateTime::currentMSecsSinceEpoch());
        record.updatedAt = timestamp;
    }
    bool hasDirty() const
    {
        for (auto it = owners.cbegin(); it != owners.cend(); ++it)
            if (!it->dirty.isEmpty() || !it->clearPending.isEmpty() || it->scopePending) return true;
        return false;
    }
    bool invalidInput() const
    {
        for (auto it = owners.cbegin(); it != owners.cend(); ++it)
            if (!it->invalidShadow.isEmpty()) return true;
        return false;
    }
    bool ready(const Work &work) const
    {
        return !waiters.isEmpty() || !saveTimer.isActive() || work.version <= forceThrough;
    }
    void finish(bool okay)
    {
        auto pending = std::move(waiters); waiters.clear();
        const QPointer<NativeDrafts> guard(q);
        for (const auto &waiter : pending) {
            if (waiter.receiver && waiter.done) waiter.done(okay);
            if (!guard) return;
        }
    }
    void notify(const QString &targetOwner, const QString &route)
    {
        const QString capturedOwner = targetOwner, stream = route.section('/', 0, 0), topic = route.section('/', 1, 1);
        emit q->changed(capturedOwner, stream, topic);
    }
    void touch(Owner &state, const QString &route) { state.recent.insert(route, ++use); }
    QSet<QString> routes(const Owner &state) const
    {
        auto result = state.loaded | state.bootstrap | state.completed | state.revisit | state.clearPending;
        for (const auto &route : state.recent.keys()) result.insert(route);
        for (const auto &route : state.active.keys()) result.insert(route);
        for (const auto &route : state.shadow.keys()) result.insert(route);
        for (const auto &route : state.holds.keys()) result.insert(route);
        for (const auto &row : state.records) result.insert(key(row.record.stream, row.record.topic));
        return result;
    }
    QSet<QString> protectedRoutes(const QString &identity, const Owner &state, bool bootstrap = true) const
    {
        auto result = state.clearPending | state.invalidShadow;
        if (bootstrap) result |= state.bootstrap;
        for (const auto &route : state.shadow.keys()) result.insert(route);
        if (identity == owner && !stream.isEmpty()) result.insert(key(stream, topic));
        for (auto row = state.records.cbegin(); row != state.records.cend(); ++row)
            if (row->version != row->durable || state.dirty.contains(row.key()))
                result.insert(key(row->record.stream, row->record.topic));
        for (auto it = state.holds.cbegin(); it != state.holds.cend(); ++it)
            if (std::any_of(it->cbegin(), it->cend(), [](const auto &hold) { return !hold.expired(); })) result.insert(it.key());
        return result;
    }
    bool protectedRow(const QString &identity, const Owner &state, const QString &id, const Work &work, const QSet<QString> &protectedRoutes) const
    {
        const auto route = key(work.record.stream, work.record.topic);
        if (work.version != work.durable || state.dirty.contains(id)
            || (state.active.value(route) == id && protectedRoutes.contains(route))) return true;
        if (work.record.phase == DraftRecord::Phase::Conflict && identity == owner
            && route == key(stream, topic)) return true;
        for (const auto &weak : state.holds.value(route))
            if (const auto hold = weak.lock(); hold && hold->uuid == id) return true;
        return false;
    }
    void unload(Owner &state, const QString &route)
    {
        state.active.remove(route); state.loaded.remove(route); state.edits.remove(route);
        state.completed.remove(route); state.revisit.remove(route);
    }
    void authorize(Owner &state, const QSet<QString> &streams)
    {
        if (state.authorized && *state.authorized == streams) return;
        state.authorized = streams; state.scopePending = true;
        for (const auto &route : routes(state)) {
            if (streams.contains(route.section('/', 0, 0))) continue;
            unload(state, route); state.shadow.remove(route); state.invalidShadow.remove(route);
            state.bootstrap.remove(route); state.clearPending.remove(route); state.recent.remove(route); state.holds.remove(route);
        }
        for (auto row = state.records.begin(); row != state.records.end();) {
            if (streams.contains(row->record.stream)) { ++row; continue; }
            state.dirty.remove(row.key()); row = state.records.erase(row);
        }
    }
    bool trim()
    {
        // No owner/map removal while an asynchronous operation captures it.
        if (running || opening) return false;
        bool changed = false;
        struct Candidate { QString owner, id, route; quint64 use; qsizetype bytes = 0; };
        const auto older = [](const Candidate &a, const Candidate &b) {
            if (a.use != b.use) return a.use < b.use;
            if (a.owner != b.owner) return a.owner < b.owner;
            return a.id < b.id;
        };
        QVector<Candidate> rows; qsizetype bytes = 0;
        for (auto state = owners.begin(); state != owners.end(); ++state) {
            for (auto it = state->holds.begin(); it != state->holds.end();) {
                auto &holds = it.value();
                holds.erase(std::remove_if(holds.begin(), holds.end(), [](const auto &hold) { return hold.expired(); }), holds.end());
                if (holds.isEmpty()) it = state->holds.erase(it); else ++it;
            }
            // Navigation does not queue reads of abandoned empty conversations.
            const auto required = protectedRoutes(state.key(), *state, false);
            for (const auto &route : state->bootstrap.values())
                if (!required.contains(route) && !state->edits.value(route)) state->bootstrap.remove(route);
            const auto protectedSet = protectedRoutes(state.key(), *state);
            for (auto row = state->records.cbegin(); row != state->records.cend(); ++row) {
                if (protectedRow(state.key(), *state, row.key(), *row, protectedSet)) continue;
                const auto route = key(row->record.stream, row->record.topic);
                rows.append({state.key(), row.key(), route, state->recent.value(route), row->bytes()});
                bytes += row->bytes();
            }
        }
        std::sort(rows.begin(), rows.end(), older);
        int count = rows.size();
        for (const auto &row : rows) {
            if (count <= NativeDrafts::MaximumCachedRecords && bytes <= NativeDrafts::MaximumCachedBytes) break;
            auto &state = owners[row.owner];
            if (state.active.value(row.route) == row.id) unload(state, row.route);
            state.records.remove(row.id); --count; bytes -= row.bytes; changed = true;
        }
        QVector<Candidate> metadata;
        for (auto state = owners.cbegin(); state != owners.cend(); ++state) {
            const auto protectedSet = protectedRoutes(state.key(), *state);
            for (const auto &route : routes(*state))
                if (!protectedSet.contains(route)) metadata.append({state.key(), route, route, state->recent.value(route)});
        }
        std::sort(metadata.begin(), metadata.end(), older);
        for (int i = 0; i < metadata.size() - NativeDrafts::MaximumCachedRoutes; ++i) {
            const auto &entry = metadata[i]; auto &state = owners[entry.owner];
            unload(state, entry.route); state.bootstrap.remove(entry.route); state.recent.remove(entry.route); state.holds.remove(entry.route);
            for (auto row = state.records.begin(); row != state.records.end();)
                if (key(row->record.stream, row->record.topic) == entry.route) row = state.records.erase(row); else ++row;
            changed = true;
        }
        for (auto state = owners.begin(); state != owners.end();)
            if (state.key() != owner && !state->scopePending && state->records.isEmpty() && routes(*state).isEmpty()) state = owners.erase(state);
            else ++state;
        return changed;
    }
    bool remoteLive() const
    {
        return remote.api && !remote.paused && remote.owner == owner && !owner.isEmpty()
            && remote.generation == remote.api->generation() && isUuid(remote.project) && isUuid(remote.user);
    }
    void cancelRemote()
    {
        ++remote.epoch; remoteTimer.stop();
        const auto request = remote.request; const auto api = remote.api;
        remote.request = {}; remote.network = false; remote.job.clear(); remote.hold.reset();
        if (api && request.isValid()) api->cancelRequest(request);
    }
    void failRemote(RemoteFailure failure)
    {
        remote.failure = failure; remote.stage = Remote::Stage::Failed;
        remote.page.clear(); remote.summaries.clear(); remote.seen.clear(); remote.markers.clear();
        remote.job.clear(); remote.hold.reset(); remoteTimer.stop();
    }
    void enqueueRemote(const QString &target, const QString &id, int delay = -1)
    {
        if (target != remote.owner || !remote.api || remote.blocked.contains(id)) return;
        if (!remote.pending.contains(id)) {
            if (remote.pending.size() >= 256) { remote.rescan = true; return; }
            remote.pending.insert(id); remote.queue.enqueue(id);
            remote.due[id] = clock.elapsed() + qMax(0, delay);
        }
        if (delay >= 0) remote.due[id] = clock.elapsed() + delay;
    }
    void requestRemote(const QByteArray &method, const QString &path, const QJsonValue &body,
                       const QUrlQuery &query, std::function<void(const ApiResult &)> done,
                       const QMap<QByteArray, QByteArray> &headers = {})
    {
        if (!remoteLive()) return;
        remote.network = true; const auto epoch = remote.epoch;
        const QPointer<NativeDrafts> guard(q);
        // The verified session token scopes this API to its project. The draft
        // endpoint rejects project_id as a collection filter. Validate each DTO.
        const auto request = remote.api->request(method, path, body, query,
            [guard, epoch, done = std::move(done)](const ApiResult &result) {
            if (!guard || guard->d->remote.epoch != epoch || !guard->d->remoteLive()) return;
            auto &d = *guard->d; d.remote.network = false; d.remote.request = {};
            done(result);
            if (guard) { guard->d->schedule(); emit guard->stateChanged(); }
        }, false, headers);
        if (guard && remote.epoch == epoch && remote.network) remote.request = request;
    }
    void readRemote(const QString &id, const NativeDraftService::Lease &lease,
                    std::function<void(std::optional<DraftRecord>)> done)
    {
        auto &state = owners[owner];
        if (state.records.contains(id)) {
            const QPointer<NativeDrafts> guard(q); done(state.records[id].record);
            if (guard) guard->d->schedule();
            return;
        }
        running = true; const auto epoch = remote.epoch;
        const QPointer<NativeDrafts> guard(q);
        storage->read(lease, id, q, [guard, epoch, done = std::move(done)](NativeDraftStore::Read result) {
            if (!guard) return;
            auto &d = *guard->d; d.running = false;
            if (d.remote.epoch != epoch || !d.remoteLive()) { d.schedule(); return; }
            if (result.failure != Failure::None) {
                d.owners[d.owner].failure = result.failure; d.failRemote(RemoteFailure::Unavailable);
            } else done(std::move(result.record));
            if (guard) { guard->d->schedule(); emit guard->stateChanged(); }
        });
    }
    void removeRemote(const DraftRecord &record, const NativeDraftService::Lease &lease)
    {
        auto &state = owners[owner];
        const auto before = state.records.value(record.uuid).version;
        const auto target = owner, route = key(record.stream, record.topic), id = record.uuid;
        running = true; const QPointer<NativeDrafts> guard(q);
        storage->apply(lease, {}, {id}, q, [guard, target, route, id, before](Failure result) {
            if (!guard) return;
            auto &d = *guard->d; d.running = false; auto &state = d.owners[target];
            if (result != Failure::None) state.failure = result;
            else {
                const auto row = state.records.constFind(id);
                if (row == state.records.cend() || row->version == before) {
                    state.records.remove(id); state.dirty.remove(id);
                    if (state.active.value(route) == id) state.active.remove(route);
                }
            }
            d.changedPage(); d.schedule(); d.notify(target, route);
            if (guard) emit guard->stateChanged();
        });
    }
    void importRemote(const Draft &server, const std::optional<DraftRecord> &local)
    {
        auto &state = owners[owner];
        if (local && local->phase != DraftRecord::Phase::Saved) return;
        DraftRecord record;
        if (local && sourceTrimmed(local->text) == server.content) record = *local;
        else {
            record.uuid = server.uuid; record.stream = server.stream; record.topic = server.topic;
            record.snapshot = uuid(); record.text = server.content;
            record.updatedAt = QDateTime::fromString(server.updatedAt, Qt::ISODateWithMs).toMSecsSinceEpoch();
        }
        if (local && local->revision > server.revision) return;
        record.revision = server.revision; record.serverUpdatedAt = server.updatedAt;
        record.phase = DraftRecord::Phase::Saved;
        if (!NativeDraftStore::valid(record)) { failRemote(RemoteFailure::InvalidResponse); return; }
        const auto route = key(record.stream, record.topic);
        auto &work = state.records[record.uuid]; work.record = record; dirty(state, work); forceThrough = version;
        timestamp = qMax(timestamp, record.updatedAt);
        if (state.loaded.contains(route) && !state.completed.contains(route) && !state.shadow.contains(route)) {
            const auto active = state.records.constFind(state.active.value(route));
            if (active == state.records.cend() || active->record.uuid == record.uuid
                || (active->record.phase == DraftRecord::Phase::Saved
                    && (active->record.updatedAt < record.updatedAt
                        || (active->record.updatedAt == record.updatedAt && active->record.uuid < record.uuid))))
                state.active[route] = record.uuid;
        }
        schedule(); notify(owner, route);
    }
    void finishRemoteJob()
    {
        remote.job.clear(); remote.hold.reset(); remote.attempts = 0;
        schedule();
    }
    void changedPage()
    {
        if (pageVisible && !pageTimer.isActive()) pageTimer.start();
    }
    bool pumpAction(const NativeDraftService::Lease &lease)
    {
        if (!action) return false;
        const auto request = *action; action.reset();
        const auto epoch = actionEpoch;
        if (request.owner != owner) return false;
        running = true; const QPointer<NativeDrafts> guard(q);
        storage->read(lease, request.id, q, [guard, request, epoch](NativeDraftStore::Read result) {
            if (!guard) return;
            auto &d = *guard->d; d.running = false;
            if (request.owner != d.owner || epoch != d.actionEpoch) { d.schedule(); return; }
            auto &state = d.owners[d.owner];
            auto record = state.records.contains(request.id) ? std::optional<DraftRecord>(state.records[request.id].record) : result.record;
            if (result.failure != Failure::None || !record || (state.authorized && !state.authorized->contains(record->stream))) {
                d.pageError = true; d.changedPage(); d.schedule(); emit guard->stateChanged(); return;
            }
            const auto route = key(record->stream, record->topic);
            if (request.action == Action::Open) {
                if (record->consumed || record->phase == DraftRecord::Phase::Deleting || route != request.route
                    || route != key(d.stream, d.topic) || state.edits.value(route) != request.edits || state.shadow.contains(route)) {
                    d.schedule(); return;
                }
                if (!state.records.contains(record->uuid)) state.records.insert(record->uuid, Work{*record, 0, 0});
                state.active[route] = record->uuid; state.completed.remove(route); state.revisit.remove(route); state.loaded.insert(route);
                state.bootstrap.remove(route); d.touch(state, route);
            } else {
                if ((!request.snapshot.isEmpty() && request.snapshot != record->snapshot)
                    || ((request.action == Action::Server || request.action == Action::Local) && record->phase != DraftRecord::Phase::Conflict)) {
                    d.changedPage(); d.schedule(); return;
                }
                if (record->phase == DraftRecord::Phase::Conflict) record->revision = record->conflictRevision;
                if (request.action == Action::Server) {
                    record->text = record->conflictText; record->replySession = {}; record->consumed = false;
                    record->phase = DraftRecord::Phase::Saved;
                } else if (request.action == Action::Local) {
                    record->consumed = false; record->phase = DraftRecord::Phase::Local;
                } else { record->consumed = true; record->phase = DraftRecord::Phase::Deleting; }
                record->conflictText.clear(); record->conflictRevision = 0;
                if (record->revision) { record->createPending = false; record->createText.clear(); }
                d.edited(*record);
                auto &work = state.records[record->uuid]; work.record = *record; d.dirty(state, work); d.forceThrough = d.version;
                if (record->consumed && state.active.value(route) == record->uuid) {
                    state.active.remove(route); state.completed.insert(route);
                } else if (!record->consumed && state.active.value(route).isEmpty() && !state.shadow.contains(route))
                    state.active[route] = record->uuid;
                d.remote.blocked.remove(record->uuid); d.enqueueRemote(d.owner, record->uuid);
                d.changedPage();
            }
            d.schedule(); d.notify(d.owner, route);
            if (guard) emit guard->stateChanged();
        }); return true;
    }
    bool pumpPage(const NativeDraftService::Lease &lease)
    {
        if (!pageVisible || !pagePending) return false;
        pagePending = false; pageLoading = true; running = true;
        const auto epoch = pageEpoch; const auto target = owner; const QPointer<NativeDrafts> guard(q);
        storage->previews(lease, pageCursor, q, [guard, epoch, target](NativeDraftStore::PreviewPage result) {
            if (!guard) return;
            auto &d = *guard->d; d.running = false;
            if (d.owner == target && d.pageEpoch == epoch && d.pageVisible) {
                d.pageLoading = false; d.pageError = result.failure != Failure::None;
                if (!d.pageError) { d.pageRows = std::move(result.rows); d.pageNext = result.next; d.pageOwner = target; }
            }
            d.schedule(); emit guard->stateChanged();
        }); return true;
    }
    void remoteJobResult(const DraftRecord &sent, const QByteArray &method, const QString &payload,
                         const ApiResult &result)
    {
        auto &state = owners[owner]; auto row = state.records.find(sent.uuid);
        if (row == state.records.end()) { finishRemoteJob(); return; }
        const auto route = key(sent.stream, sent.topic);
        if (method == "DELETE" && ((result.status == 204 && result.error.isEmpty()) || result.status == 404)) {
            // The durable removal is scheduled by the pump after this receipt.
            row->record.revision = 0; row->record.createPending = false; row->record.createText.clear();
            row->record.privateCreateUncertain = false;
            row->record.serverUpdatedAt.clear(); row->record.conflictRevision = 0; row->record.conflictText.clear();
            dirty(state, *row); forceThrough = version; return;
        }
        if ((result.ok() || result.status == 412) && result.data.isObject()) {
            const auto data = result.data.toObject();
            // The deployed stale-revision response wraps the owned DTO in current.
            // Older direct-DTO responses remain subject to the same strict checks.
            const auto snapshot = result.status == 412 && data.value("current").isObject()
                ? data.value("current").toObject() : data;
            const auto server = parseDraft(snapshot, remote.project, remote.user, result.etag);
            if (!server || server->uuid != sent.uuid || server->stream != sent.stream || server->topic != sent.topic
                || server->content.contains(":::")) {
                row->record.phase = DraftRecord::Phase::Failed;
                dirty(state, *row); remote.blocked.insert(sent.uuid);
                remote.failure = RemoteFailure::InvalidResponse; finishRemoteJob(); return;
            }
            const bool deleting = row->record.consumed;
            const bool matching = sourceTrimmed(row->record.text) == server->content;
            if (result.status == 412 && !row->record.privateDeletion && !(method != "POST" && matching)) {
                row->record.phase = DraftRecord::Phase::Conflict;
                row->record.conflictText = server->content; row->record.conflictRevision = server->revision;
                dirty(state, *row); forceThrough = version; finishRemoteJob(); notify(owner, route); return;
            }
            row->record.revision = server->revision; row->record.serverUpdatedAt = server->updatedAt;
            if (remote.reconcile) remote.seen.insert(server->uuid);
            row->record.createPending = false; row->record.createText.clear(); row->record.privateCreateUncertain = false;
            if (deleting) row->record.phase = DraftRecord::Phase::Deleting;
            else row->record.phase = sourceTrimmed(row->record.text) == sourceTrimmed(payload)
                || (result.status == 412 && matching) ? DraftRecord::Phase::Saved : DraftRecord::Phase::Local;
            dirty(state, *row); forceThrough = version; remote.failure = RemoteFailure::None;
            // Keep this serialized UUID until its new revision is durable, then
            // send a newer snapshot or its already-durable deletion intent.
            notify(owner, route); return;
        }
        const bool privateLookup = method == "GET" && row->record.privateDeletion && !row->record.revision;
        const bool retryable = result.status == 0 || result.status == 408 || result.status == 429 || result.status >= 500;
        if (retryable || privateLookup) {
            const int delay = privateLookup ? 30000 : 1000 * (1 << qMin(remote.attempts++, 5));
            remote.job.clear(); remote.hold.reset(); enqueueRemote(owner, sent.uuid, delay);
            remote.retryUntil = clock.elapsed() + delay;
            remote.failure = RemoteFailure::Unavailable;
            return;
        }
        row->record.phase = DraftRecord::Phase::Failed;
        remote.blocked.insert(sent.uuid);
        dirty(state, *row); forceThrough = version; remote.failure = RemoteFailure::Rejected;
        finishRemoteJob(); notify(owner, route);
    }
    void runRemoteJob(Owner &state, const NativeDraftService::Lease &lease)
    {
        auto row = state.records.find(remote.job);
        if (row == state.records.end()) { finishRemoteJob(); return; }
        if (row->version != row->durable) return;
        auto &record = row->record;
        if (!remote.streams.contains(record.stream)) {
            finishRemoteJob(); return;
        }
        if (record.phase == DraftRecord::Phase::Conflict || record.phase == DraftRecord::Phase::Saved
            || record.phase == DraftRecord::Phase::Failed) { finishRemoteJob(); return; }
        if (record.consumed && !record.revision && !record.createPending && !record.privateCreateUncertain) {
            const auto copy = record; finishRemoteJob(); removeRemote(copy, lease); return;
        }
        QByteArray method; QString payload;
        if (record.consumed && record.revision) method = "DELETE";
        else if (record.privateDeletion) method = "GET";
        else {
            if (sourceTrimmed(record.text).isEmpty()) { finishRemoteJob(); return; }
            method = record.revision ? "PUT" : "POST";
            if (method == "POST" && !record.createPending) {
                record.createPending = true; record.createText = sourceTrimmed(record.text);
                if (!record.consumed) record.phase = DraftRecord::Phase::Saving;
                dirty(state, *row); forceThrough = version; schedule(); return;
            }
            if (method == "PUT" && record.phase == DraftRecord::Phase::Local) {
                record.phase = DraftRecord::Phase::Saving; dirty(state, *row); forceThrough = version; schedule(); return;
            }
            payload = method == "POST" ? record.createText : sourceTrimmed(record.text);
        }
        const auto sent = record;
        QJsonValue body;
        if (method == "POST") body = QJsonObject{{"uuid", sent.uuid}, {"stream_uuid", sent.stream}, {"topic_uuid", sent.topic},
            {"payload", QJsonObject{{"kind", "markdown"}, {"content", payload}}}};
        else if (method == "PUT") body = QJsonObject{{"payload", QJsonObject{{"kind", "markdown"}, {"content", payload}}}};
        const auto path = QString("/api/workspace/v1/messenger/drafts/") + (method == "POST" ? QString{} : sent.uuid);
        QMap<QByteArray, QByteArray> headers;
        if (method == "PUT" || method == "DELETE") headers.insert("If-Match", '"' + QByteArray::number(sent.revision) + '"');
        requestRemote(method, path, body, {}, [this, sent, method, payload](const ApiResult &result) {
            remoteJobResult(sent, method, payload, result);
        }, headers);
    }
    void pumpRemote(Owner &state, const NativeDraftService::Lease &lease)
    {
        if (!remoteLive() || remote.network || remote.stage == Remote::Stage::Failed) return;
        if (remote.job.isEmpty() && remote.refetchRequested) { q->refetchRemote(); return; }
        if (remote.retryUntil > clock.elapsed()) { remoteTimer.start(int(remote.retryUntil - clock.elapsed())); return; }
        if (!remote.job.isEmpty()) {
            const QPointer<NativeDrafts> guard(q); runRemoteJob(state, lease);
            if (guard) emit guard->stateChanged();
            return;
        }
        if (remote.stage == Remote::Stage::Fetch) {
            QUrlQuery query; query.addQueryItem("page_limit", "100"); query.addQueryItem("sort_key", "updated_at");
            query.addQueryItem("sort_dir", "desc"); if (!remote.marker.isEmpty()) query.addQueryItem("page_marker", QString::fromLatin1(remote.marker));
            requestRemote("GET", "/api/workspace/v1/messenger/drafts/", {}, query, [this](const ApiResult &result) {
                if (!result.ok()) { failRemote(RemoteFailure::Unavailable); return; }
                if (!result.data.isArray() || result.data.toArray().size() > 100
                    || (!result.nextMarker.isEmpty() && remote.markers.contains(result.nextMarker))) {
                    failRemote(RemoteFailure::InvalidResponse); return;
                }
                QVector<Draft> rows;
                for (const auto &value : result.data.toArray()) {
                    const auto row = value.isObject() ? parseDraft(value.toObject(), remote.project, remote.user) : std::nullopt;
                    if (!row || remote.seen.contains(row->uuid) || row->content.contains(":::")) {
                        failRemote(RemoteFailure::InvalidResponse); return;
                    }
                    remote.seen.insert(row->uuid);
                    if (remote.streams.contains(row->stream)) rows.append(*row);
                }
                remote.page = std::move(rows); remote.next = result.nextMarker;
                if (!remote.next.isEmpty()) remote.markers.insert(remote.next);
                remote.stage = Remote::Stage::Import;
            }); return;
        }
        if (remote.stage == Remote::Stage::Import) {
            if (!remote.page.isEmpty()) {
                const auto server = remote.page.takeFirst();
                readRemote(server.uuid, lease, [this, server](std::optional<DraftRecord> local) { importRemote(server, local); });
                return;
            }
            if (!remote.next.isEmpty()) { remote.marker = remote.next; remote.stage = Remote::Stage::Fetch; schedule(); return; }
            remote.stage = Remote::Stage::Scan; remote.after.clear(); remote.scanEnd = false; remote.reconcile = true;
        }
        qint64 earliest = std::numeric_limits<qint64>::max();
        for (int i = 0, count = remote.queue.size(); i < count; ++i) {
            const auto id = remote.queue.dequeue(); const auto due = remote.due.value(id);
            if (due > clock.elapsed()) { earliest = qMin(earliest, due); remote.queue.enqueue(id); continue; }
            remote.pending.remove(id); remote.due.remove(id);
            readRemote(id, lease, [this, id](std::optional<DraftRecord> record) {
                if (!record) return;
                auto &state = owners[owner];
                if (!state.records.contains(id)) state.records.insert(id, Work{*record, 0, 0});
                remote.job = id; remote.hold = retain(state, key(record->stream, record->topic), id);
                schedule();
            }); return;
        }
        if (earliest != std::numeric_limits<qint64>::max())
            remoteTimer.start(int(qBound<qint64>(qint64(1), earliest - clock.elapsed(), qint64(32000))));
        if (remote.stage == Remote::Stage::Scan) {
            if (!remote.summaries.isEmpty()) {
                const auto summary = remote.summaries.takeFirst();
                readRemote(summary.uuid, lease, [this, lease](std::optional<DraftRecord> record) {
                    if (!record) return;
                    auto &state = owners[owner];
                    if (!state.records.contains(record->uuid)) state.records.insert(record->uuid, Work{*record, 0, 0});
                    if (!remote.streams.contains(record->stream)) { schedule(); return; }
                    if (remote.reconcile && record->phase == DraftRecord::Phase::Saved
                        && !remote.seen.contains(record->uuid)) removeRemote(*record, lease);
                    else if (record->phase == DraftRecord::Phase::Local || record->phase == DraftRecord::Phase::Saving
                             || record->phase == DraftRecord::Phase::Deleting) enqueueRemote(owner, record->uuid);
                    schedule();
                }); return;
            }
            if (remote.scanEnd) {
                remote.stage = Remote::Stage::Idle; remote.seen.clear(); remote.markers.clear(); remote.reconcile = false;
                schedule(); emit q->stateChanged(); return;
            } else {
                running = true; const auto epoch = remote.epoch; const QPointer<NativeDrafts> guard(q);
                storage->list(lease, remote.after, q, [guard, epoch](NativeDraftStore::Page result) {
                    if (!guard) return;
                    auto &d = *guard->d; d.running = false;
                    if (d.remote.epoch != epoch || !d.remoteLive()) { d.schedule(); return; }
                    if (result.failure != Failure::None) {
                        d.owners[d.owner].failure = result.failure; d.failRemote(RemoteFailure::Unavailable);
                    } else {
                        d.remote.summaries = std::move(result.rows); d.remote.after = result.next; d.remote.scanEnd = result.next.isEmpty();
                    }
                    d.schedule(); emit guard->stateChanged();
                }); return;
            }
        }
        if (remote.rescan && remote.pending.size() < 128) {
            remote.rescan = false; remote.stage = Remote::Stage::Scan; remote.after.clear(); remote.scanEnd = false; schedule();
        }
    }
    void pump()
    {
        if (!storage) { if (!waiters.isEmpty()) finish(false); return; }
        if (storage->stopped()) { if (!waiters.isEmpty()) finish(false); return; }
        if (running) return;
        if (opening) {
            if (storage->busy()) return;
            opening = false;
            if (!storage->available() || storage->lease().owner != workerOwner) {
                owners[workerOwner].failure = storage->failure();
                if (owners[workerOwner].failure == Failure::None) owners[workerOwner].failure = Failure::Unavailable;
                const QPointer<NativeDrafts> guard(q);
                emit q->stateChanged();
                if (!guard) return;
            }
        }
        const QPointer<NativeDrafts> trimGuard(q);
        if (trim()) emit q->stateChanged();
        if (!trimGuard) return;
        QString target;
        for (auto it = owners.cbegin(); it != owners.cend(); ++it) {
            if (it->failure != Failure::None) continue;
            if (it->scopePending || !it->bootstrap.isEmpty() || !it->clearPending.isEmpty()) { target = it.key(); break; }
            for (const auto &id : it->dirty)
                if (it->records.contains(id) && ready(it->records[id])) { target = it.key(); break; }
            if (!target.isEmpty()) break;
        }
        if (target.isEmpty()) target = owner;
        if (target.isEmpty()) {
            if (!workerOwner.isEmpty()) { workerOwner.clear(); storage->close(); }
            if (!waiters.isEmpty()) finish(!hasDirty() && !invalidInput());
            return;
        }
        auto &state = owners[target];
        if (state.failure != Failure::None) {
            if (!waiters.isEmpty()) finish(false);
            return;
        }
        if (workerOwner != target || !storage->available()) {
            workerOwner = target; opening = true;
            if (!storage->open(target)) {
                opening = false; state.failure = Failure::Unavailable;
                const QPointer<NativeDrafts> guard(q); emit q->stateChanged();
                if (guard && !waiters.isEmpty()) finish(false);
            }
            return;
        }
        const auto lease = storage->lease();
        if (state.scopePending && state.authorized) {
            state.scopePending = false; running = true; const QPointer<NativeDrafts> guard(q);
            storage->retainStreams(lease, *state.authorized, q, [guard, target](Failure result) {
                if (!guard) return;
                auto &d = *guard->d; d.running = false;
                if (result != Failure::None) { d.owners[target].scopePending = true; d.owners[target].failure = result; }
                d.schedule(); emit guard->stateChanged();
            }); return;
        }
        QString lookupRoute;
        if (!state.clearPending.isEmpty()) lookupRoute = *state.clearPending.cbegin();
        else if (!state.bootstrap.isEmpty()) lookupRoute = *state.bootstrap.cbegin();
        // A completed draft must reach disk before selecting an older distinct
        // draft on revisit. Initial hydration still precedes new input writes.
        const bool lookupPending = !lookupRoute.isEmpty()
            && !(state.revisit.contains(lookupRoute) && !state.dirty.isEmpty());
        if (!state.dirty.isEmpty() && !lookupPending) {
            QVector<DraftRecord> rows; QHash<QString, quint64> versions; qsizetype bytes = 0;
            auto ids = state.dirty.values(); std::sort(ids.begin(), ids.end());
            for (const auto &id : ids) {
                const auto row = state.records.constFind(id);
                if (row == state.records.cend() || !ready(*row)) continue;
                const auto size = NativeDraftStore::serializedSize(row->record);
                if (!rows.isEmpty() && (rows.size() == NativeDraftStore::MaximumPageRows
                    || bytes + size > NativeDraftStore::MaximumBatchBytes)) break;
                rows.append(row->record); versions.insert(id, row->version); bytes += size;
            }
            if (!rows.isEmpty()) {
            running = true;
            const QPointer<NativeDrafts> guard(q);
            storage->apply(lease, rows, {}, q, [guard, target, versions](Failure result) {
                if (!guard) return;
                auto &d = *guard->d; d.running = false;
                auto &state = d.owners[target];
                if (result != Failure::None) state.failure = result;
                else for (auto it = versions.cbegin(); it != versions.cend(); ++it) {
                    auto row = state.records.find(it.key());
                    if (row == state.records.end()) continue;
                    row->durable = qMax(row->durable, it.value());
                    if (row->version == it.value()) state.dirty.remove(it.key());
                    if (it.key() != d.remote.job && (row->record.phase == DraftRecord::Phase::Local
                        || row->record.phase == DraftRecord::Phase::Saving || row->record.phase == DraftRecord::Phase::Deleting))
                        d.enqueueRemote(target, it.key());
                }
                d.changedPage();
                d.schedule(); emit guard->stateChanged();
            });
            return;
            }
        }
        if (lookupPending) {
            const auto route = lookupRoute;
            const auto lookupStream = route.section('/', 0, 0), lookupTopic = route.section('/', 1, 1);
            const auto active = state.active.value(route);
                running = true;
                const auto beforeEdit = state.edits.value(route);
                const QPointer<NativeDrafts> guard(q);
                const auto apply = [guard, target, route, beforeEdit](NativeDraftStore::Read result) {
                    if (!guard) return;
                    auto &d = *guard->d; d.running = false;
                    auto &state = d.owners[target];
                    if (state.authorized && !state.authorized->contains(route.section('/', 0, 0))) {
                        state.bootstrap.remove(route); state.clearPending.remove(route); d.schedule(); return;
                    }
                    if (result.failure != Failure::None) state.failure = result.failure;
                    else if (result.record && key(result.record->stream, result.record->topic) != route)
                        state.failure = Failure::Corrupt;
                    else if (state.clearPending.remove(route)) {
                        state.loaded.insert(route); state.bootstrap.remove(route);
                        if (result.record && !result.record->consumed) {
                            auto record = *result.record; record.consumed = true; record.phase = DraftRecord::Phase::Deleting;
                            if (state.shadow.value(route).contains(":::")) promotePrivate(record);
                            d.edited(record); auto &work = state.records[record.uuid]; work.record = std::move(record);
                            d.dirty(state, work); d.forceThrough = d.version;
                        }
                    }
                    else if (beforeEdit == 0 && state.edits.value(route) == 0 && !state.completed.contains(route)) {
                        state.loaded.insert(route); state.bootstrap.remove(route);
                        if (result.record && !result.record->consumed && result.record->phase != DraftRecord::Phase::Deleting) {
                            const auto id = result.record->uuid;
                            if (!state.records.contains(id)) state.records.insert(id, Work{*result.record, 0, 0});
                            state.active.insert(route, id);
                        }
                    }
                    else if (result.failure == Failure::None) {
                        state.loaded.insert(route); state.bootstrap.remove(route);
                    }
                    if (state.loaded.contains(route)) state.revisit.remove(route);
                    if (result.record) {
                        d.timestamp = qMax(d.timestamp, result.record->updatedAt);
                        auto current = state.records.find(state.active.value(route));
                        if (current != state.records.end() && current->version > current->durable
                            && current->record.uuid != result.record->uuid && current->record.updatedAt <= result.record->updatedAt) {
                            current->record.updatedAt = ++d.timestamp;
                            d.dirty(state, *current);
                        }
                    }
            d.schedule(); d.notify(target, route);
                    if (guard) emit guard->stateChanged();
                };
                if (active.isEmpty() || state.records.contains(active)) storage->latest(lease, lookupStream, lookupTopic, q, apply);
                else storage->read(lease, active, q, apply);
                return;
        }
        const QPointer<NativeDrafts> guard(q);
        if (!waiters.isEmpty()) finish(!hasDirty() && !invalidInput());
        if (guard && !running && target == owner) {
            if (pumpAction(lease) || pumpPage(lease)) return;
            if (remoteLive()) pumpRemote(owners[target], lease);
        }
    }
};

NativeDrafts::NativeDrafts(NativeDraftService *storage, QObject *parent)
    : QObject(parent), d(std::make_unique<Private>(this, storage)) {}
NativeDrafts::~NativeDrafts() { d->cancelRemote(); }
QString NativeDrafts::owner() const { return d->owner; }
QString NativeDrafts::selectedStream() const { return d->stream; }
QString NativeDrafts::selectedTopic() const { return d->topic; }
QString NativeDrafts::text() const { return d->text(d->stream, d->topic); }
QString NativeDrafts::text(const QString &stream, const QString &topic) const { return d->text(stream, topic); }
NativeDrafts::Residency NativeDrafts::residency() const
{
    Residency result; result.owners = d->owners.size();
    for (auto state = d->owners.cbegin(); state != d->owners.cend(); ++state) {
        const auto protectedRoutes = d->protectedRoutes(state.key(), *state);
        const auto routes = d->routes(*state);
        result.routes += routes.size();
        for (const auto &route : routes) if (!protectedRoutes.contains(route)) ++result.cachedRoutes;
        for (auto row = state->records.cbegin(); row != state->records.cend(); ++row) {
            ++result.records;
            if (d->protectedRow(state.key(), *state, row.key(), *row, protectedRoutes)) continue;
            ++result.cachedRecords; result.cachedBytes += row->bytes();
        }
    }
    return result;
}
bool NativeDrafts::retained(const QString &stream, const QString &topic) const
{
    const auto state = d->owners.constFind(d->owner);
    return state != d->owners.cend() && state->recent.contains(key(stream, topic));
}
bool NativeDrafts::ready(const QString &stream, const QString &topic) const
{
    const auto state = d->owners.constFind(d->owner);
    return state != d->owners.cend() && (state->loaded.contains(key(stream, topic)) || state->failure != Failure::None);
}
NativeDraftService::Failure NativeDrafts::failure() const
{
    const auto state = d->owners.constFind(d->owner);
    return state == d->owners.cend() ? Failure::None
        : !state->invalidShadow.isEmpty() ? Failure::InvalidRecord : state->failure;
}

void NativeDrafts::setOwner(const QString &owner)
{
    const auto next = ownerValid(owner) ? owner : QString{};
    if (next == d->owner) return;
    d->cancelRemote();
    d->remote.owner.clear();
    ++d->actionEpoch; d->action.reset(); hidePage();
    d->owner = next; d->stream.clear(); d->topic.clear(); d->forceThrough = d->version;
    d->schedule(); emit stateChanged();
}
void NativeDrafts::select(const QString &stream, const QString &topic)
{
    const auto nextStream = isUuid(stream) && isUuid(topic) ? stream : QString{};
    const auto nextTopic = nextStream.isEmpty() ? QString{} : topic;
    if (nextStream == d->stream && nextTopic == d->topic) return;
    if (!d->owner.isEmpty()) {
        auto &state = d->owners[d->owner];
        const auto previous = key(d->stream, d->topic);
        if (!state.shadow.contains(previous) && state.completed.remove(previous)) {
            state.loaded.remove(previous); state.edits.remove(previous); state.revisit.insert(previous);
        }
    }
    d->stream = nextStream; d->topic = nextTopic; d->forceThrough = d->version;
    if (!d->owner.isEmpty() && !nextStream.isEmpty()) {
        auto &state = d->owners[d->owner]; const auto route = key(nextStream, nextTopic);
        d->touch(state, route);
        if (!state.loaded.contains(route)) state.bootstrap.insert(route);
    }
    d->schedule(); emit stateChanged();
}
bool NativeDrafts::edit(const QString &stream, const QString &topic, const QString &text)
{
    if (d->owner.isEmpty() || !isUuid(stream) || !isUuid(topic)) return false;
    auto &state = d->owners[d->owner]; const auto route = key(stream, topic);
    if (state.authorized && (!state.authorized->contains(stream)
        || (d->remote.owner == d->owner && !d->remote.streams.contains(stream)))) return false;
    d->touch(state, route);
    if (d->text(stream, topic) == text) { d->schedule(); return true; }
    ++state.edits[route];
    const bool loaded = state.loaded.contains(route);
    if (!loaded) state.bootstrap.insert(route);
    const auto active = state.active.value(route);
    auto found = state.records.find(active);
    const bool privateText = text.contains(":::");
    const bool empty = sourceTrimmed(text).isEmpty();
    if (privateText || empty) {
        state.invalidShadow.remove(route);
        // Keep indentation editable before the first non-space character. An
        // all-whitespace body is empty for persistence, not for the editor.
        if (privateText || !text.isEmpty()) state.shadow.insert(route, text); else state.shadow.remove(route);
        if (found != state.records.end() && !found->record.consumed) {
            if (privateText) promotePrivate(found->record);
            else { found->record.consumed = true; found->record.phase = DraftRecord::Phase::Deleting; }
            d->edited(found->record); d->dirty(state, *found);
        }
        // A new RAM row can coexist with an older disk row while bootstrap is
        // pending. Clearing must also dispose of that earlier selection.
        if (!loaded) state.clearPending.insert(route);
        state.active.remove(route); state.completed.insert(route);
    } else {
        DraftRecord record;
        if (found != state.records.end() && !found->record.consumed) record = found->record;
        else { record.uuid = uuid(); record.stream = stream; record.topic = topic; }
        const bool remoteTextUnchanged = record.phase == DraftRecord::Phase::Saved
            && sourceTrimmed(record.text) == sourceTrimmed(text);
        record.text = text; record.consumed = false;
        if (record.phase != DraftRecord::Phase::Conflict && !remoteTextUnchanged) record.phase = DraftRecord::Phase::Local;
        d->edited(record);
        if (!NativeDraftStore::valid(record)) {
            state.shadow.insert(route, text); state.invalidShadow.insert(route); state.failure = Failure::InvalidRecord;
            const QPointer<NativeDrafts> guard(this); d->notify(d->owner, route);
            if (guard) emit stateChanged();
            return false;
        }
        state.shadow.remove(route); state.invalidShadow.remove(route); state.completed.remove(route);
        state.clearPending.remove(route);
        auto &work = state.records[record.uuid]; work.record = std::move(record);
        state.active.insert(route, work.record.uuid); d->dirty(state, work);
        d->remote.blocked.remove(work.record.uuid);
        if (d->remote.job != work.record.uuid) d->enqueueRemote(d->owner, work.record.uuid, 1000);
        // New valid edits can retry an earlier local admission/storage failure.
        state.failure = Failure::None;
    }
    d->saveTimer.start(); d->schedule();
    const QPointer<NativeDrafts> guard(this); d->notify(d->owner, route);
    if (guard) emit stateChanged();
    return true;
}
NativeDrafts::Token NativeDrafts::capture(const QString &stream, const QString &topic) const
{
    auto state = d->owners.find(d->owner);
    if (state == d->owners.end() || !isUuid(stream) || !isUuid(topic)) return {};
    const auto route = key(stream, topic); d->touch(*state, route);
    Token result{d->owner, stream, topic, {}, {}, {}};
    const auto row = state->records.constFind(state->active.value(key(stream, topic)));
    if (!state->shadow.contains(route) && row != state->records.cend() && !row->record.consumed
        && row->record.stream == stream && row->record.topic == topic) {
        const auto &r = row->record; result.uuid = r.uuid; result.snapshot = r.snapshot; result.text = r.text;
    }
    result.residency = d->retain(*state, route, result.uuid);
    return result;
}
bool NativeDrafts::consume(const Token &token)
{
    if (token.owner.isEmpty() || token.owner != d->owner) return false;
    auto &state = d->owners[token.owner]; auto row = state.records.find(token.uuid);
    if (row == state.records.end() || row->record.snapshot != token.snapshot || row->record.text != token.text
        || row->record.consumed || row->record.stream != token.stream || row->record.topic != token.topic) return false;
    row->record.consumed = true; row->record.phase = DraftRecord::Phase::Deleting;
    d->edited(row->record); d->dirty(state, *row);
    const auto route = key(token.stream, token.topic);
    if (state.active.value(route) == token.uuid) state.active.remove(route);
    state.completed.insert(route); state.loaded.insert(route);
    d->forceThrough = d->version; d->schedule();
    const QPointer<NativeDrafts> guard(this); d->notify(token.owner, route);
    if (guard) emit stateChanged();
    return true;
}
void NativeDrafts::useRemote(ApiClient *api, const QString &project, const QString &user,
                            const QSet<QString> &ordinaryStreams, std::optional<QSet<QString>> accessibleStreams)
{
    const auto authorized = accessibleStreams.value_or(ordinaryStreams);
    const auto identity = api ? QJsonDocument(QJsonArray{api->server().toString(QUrl::FullyEncoded), project, user})
        .toJson(QJsonDocument::Compact) : QByteArray{};
    const auto expectedOwner = QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
    if (!api || !isUuid(project) || !isUuid(user) || expectedOwner != d->owner) api = nullptr;
    if (d->remote.api == api && ((!api && d->remote.owner.isEmpty()) || (api && d->remote.owner == d->owner && d->remote.project == project
        && d->remote.user == user && d->remote.streams == ordinaryStreams && d->remote.accessible == authorized
        && d->remote.generation == api->generation()))) return;
    d->cancelRemote(); disconnect(d->remote.generationChanged); disconnect(d->remote.destroyed);
    d->remote.api = api; d->remote.owner = api ? d->owner : QString{};
    d->remote.project = api ? project : QString{}; d->remote.user = api ? user : QString{};
    d->remote.streams = api ? ordinaryStreams : QSet<QString>{};
    d->remote.accessible = api ? authorized : QSet<QString>{};
    if (api) {
        d->authorize(d->owners[d->owner], authorized);
        d->remote.generation = api->generation();
        d->remote.generationChanged = connect(api, &ApiClient::sessionChanged, this, [this] {
            d->cancelRemote(); d->remote.stage = Private::Remote::Stage::Failed;
            d->remote.failure = RemoteFailure::Unavailable;
        });
        d->remote.destroyed = connect(api, &QObject::destroyed, this, [this] { useRemote(nullptr, {}, {}, {}); });
        refetchRemote();
    } else {
        d->remote.page.clear(); d->remote.summaries.clear(); d->remote.pending.clear(); d->remote.queue.clear();
        d->remote.due.clear(); d->remote.seen.clear(); d->remote.markers.clear(); d->remote.blocked.clear();
        d->remote.stage = Private::Remote::Stage::Failed; d->remote.failure = RemoteFailure::None;
        d->schedule(); emit stateChanged();
    }
}
void NativeDrafts::refetchRemote()
{
    if (!d->remoteLive()) return;
    if (!d->remote.job.isEmpty()) { d->remote.refetchRequested = true; return; }
    d->cancelRemote(); auto &r = d->remote; r.refetchRequested = false;
    r.page.clear(); r.summaries.clear(); r.pending.clear(); r.queue.clear(); r.due.clear(); r.blocked.clear();
    r.seen.clear(); r.markers.clear(); r.marker.clear(); r.next.clear(); r.after.clear();
    r.stage = Private::Remote::Stage::Fetch; r.failure = RemoteFailure::None; r.scanEnd = r.rescan = r.reconcile = false;
    r.attempts = 0; r.retryUntil = 0; d->schedule(); emit stateChanged();
}
void NativeDrafts::pauseRemote(bool paused)
{
    if (d->remote.paused == paused) return;
    d->remote.paused = paused;
    if (paused) d->cancelRemote(); else refetchRemote();
}
NativeDrafts::RemoteStatus NativeDrafts::remoteStatus() const
{
    RemoteStatus result; result.enabled = d->remoteLive(); result.failure = d->remote.failure;
    result.busy = result.enabled && (d->remote.network || !d->remote.job.isEmpty()
        || (d->remote.stage != Private::Remote::Stage::Idle && d->remote.stage != Private::Remote::Stage::Failed));
    const auto state = d->owners.constFind(d->owner);
    if (state == d->owners.cend()) return result;
    const auto active = state->records.constFind(state->active.value(key(d->stream, d->topic)));
    if (active != state->records.cend()) {
        result.phase = active->record.phase; result.conflict = active->record.phase == DraftRecord::Phase::Conflict;
        if (result.conflict) result.serverText = active->record.conflictText;
    } else for (const auto &work : state->records) {
        if (work.record.stream == d->stream && work.record.topic == d->topic && work.record.phase == DraftRecord::Phase::Conflict) {
            result.phase = work.record.phase; result.conflict = true; result.serverText = work.record.conflictText; break;
        }
    }
    return result;
}
bool NativeDrafts::resolveConflict(bool useServer)
{
    auto state = d->owners.find(d->owner); if (state == d->owners.end()) return false;
    const auto route = key(d->stream, d->topic);
    auto row = state->records.find(state->active.value(route));
    if (row == state->records.end() || row->record.phase != DraftRecord::Phase::Conflict) {
        row = std::find_if(state->records.begin(), state->records.end(), [&](const Work &work) {
            return key(work.record.stream, work.record.topic) == route && work.record.phase == DraftRecord::Phase::Conflict;
        });
    }
    if (row == state->records.end() || d->remote.job == row->record.uuid) return false;
    auto &record = row->record; record.revision = record.conflictRevision;
    if (useServer) {
        record.text = record.conflictText; record.replySession = {}; record.consumed = false; record.phase = DraftRecord::Phase::Saved;
    } else { record.consumed = false; record.phase = DraftRecord::Phase::Local; }
    record.createPending = false; record.createText.clear(); record.conflictText.clear(); record.conflictRevision = 0;
    d->edited(record); d->dirty(*state, *row); d->forceThrough = d->version;
    if (state->active.value(route).isEmpty() && !record.consumed && !state->shadow.contains(route)) state->active[route] = record.uuid;
    d->remote.blocked.remove(record.uuid); d->enqueueRemote(d->owner, record.uuid);
    d->schedule(); const QPointer<NativeDrafts> guard(this); d->notify(d->owner, route);
    if (guard) emit stateChanged();
    return true;
}
void NativeDrafts::showPage()
{
    ++d->pageEpoch; d->pageVisible = d->pagePending = true; d->pageLoading = d->pageError = false;
    d->pageCursor.clear(); d->pageNext.clear(); d->pageHistory.clear(); d->pageRows.clear(); d->pageOwner = d->owner;
    d->schedule(); emit stateChanged();
}
void NativeDrafts::hidePage()
{
    ++d->pageEpoch; d->pageTimer.stop(); d->pageVisible = d->pagePending = d->pageLoading = false;
    d->pageRows.clear(); d->pageNext.clear(); d->pageHistory.clear(); d->pageCursor.clear(); d->pageOwner.clear();
}
void NativeDrafts::nextPage()
{
    if (!d->pageVisible || d->pageNext.isEmpty() || d->pageLoading) return;
    d->pageHistory.append(d->pageCursor); d->pageCursor = d->pageNext; d->pagePending = true; ++d->pageEpoch;
    d->schedule(); emit stateChanged();
}
void NativeDrafts::previousPage()
{
    if (!d->pageVisible || d->pageHistory.isEmpty() || d->pageLoading) return;
    d->pageCursor = d->pageHistory.takeLast(); d->pagePending = true; ++d->pageEpoch;
    d->schedule(); emit stateChanged();
}
QVariantMap NativeDrafts::page() const
{
    QVariantList rows;
    if (d->pageOwner == d->owner) for (const auto &row : d->pageRows) {
        rows.append(QVariantMap{{"id", row.uuid}, {"stream", row.stream}, {"topic", row.topic}, {"preview", row.text},
            {"updatedAt", row.updatedAt}, {"phase", int(row.phase)}, {"consumed", row.consumed},
            {"conflict", row.phase == DraftRecord::Phase::Conflict}, {"busy", d->remote.job == row.uuid && d->remote.network}});
    }
    return {{"rows", rows}, {"loading", d->pagePending || d->pageLoading}, {"error", d->pageError},
        {"canNext", !d->pageNext.isEmpty()}, {"canPrevious", !d->pageHistory.isEmpty()}};
}
bool NativeDrafts::actOnDraft(const QString &id, Action action)
{
    if (!isUuid(id) || d->owner.isEmpty() || (action != Action::Open && d->remote.job == id && d->remote.network)) return false;
    auto state = d->owners.find(d->owner); if (state == d->owners.end()) return false;
    const auto row = state->records.constFind(id); QString snapshot;
    if (row != state->records.cend()) snapshot = row->record.snapshot;
    else for (const auto &preview : d->pageRows) if (preview.uuid == id) { snapshot = preview.snapshot; break; }
    if (action != Action::Open && snapshot.isEmpty()) return false;
    ++d->actionEpoch;
    d->action = Private::ActionRequest{id, d->owner, key(d->stream, d->topic), snapshot, action,
        state->edits.value(key(d->stream, d->topic))};
    d->schedule(); return true;
}
void NativeDrafts::flush(QObject *receiver, Done done)
{
    d->waiters.append({receiver ? receiver : this, std::move(done)});
    for (auto it = d->owners.begin(); it != d->owners.end(); ++it) it->failure = Failure::None;
    d->saveTimer.stop(); d->forceThrough = d->version; d->schedule();
}
void NativeDrafts::retry()
{
    if (!d->owner.isEmpty()) d->owners[d->owner].failure = Failure::None;
    d->forceThrough = d->version; d->schedule();
}

} // namespace workspace
