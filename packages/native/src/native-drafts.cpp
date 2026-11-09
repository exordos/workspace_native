#include "native-drafts.h"
#include "domain.h"

#include <QDateTime>
#include <QHash>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QUuid>
#include <algorithm>

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
    Failure failure = Failure::None;
};
struct Waiter { QPointer<QObject> receiver; NativeDrafts::Done done; };
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

    Private(NativeDrafts *q, NativeDraftService *storage) : q(q), storage(storage)
    {
        saveTimer.setSingleShot(true); saveTimer.setInterval(400);
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
    void edited(DraftRecord &record)
    {
        record.snapshot = uuid();
        timestamp = qMax(timestamp + 1, QDateTime::currentMSecsSinceEpoch());
        record.updatedAt = timestamp;
    }
    bool hasDirty() const
    {
        for (auto it = owners.cbegin(); it != owners.cend(); ++it)
            if (!it->dirty.isEmpty() || !it->clearPending.isEmpty()) return true;
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
    bool protectedRow(const Owner &state, const QString &id, const Work &work, const QSet<QString> &protectedRoutes) const
    {
        const auto route = key(work.record.stream, work.record.topic);
        if (work.version != work.durable || state.dirty.contains(id)
            || (state.active.value(route) == id && protectedRoutes.contains(route))) return true;
        for (const auto &weak : state.holds.value(route))
            if (const auto hold = weak.lock(); hold && hold->uuid == id) return true;
        return false;
    }
    void unload(Owner &state, const QString &route)
    {
        state.active.remove(route); state.loaded.remove(route); state.edits.remove(route);
        state.completed.remove(route); state.revisit.remove(route);
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
                if (protectedRow(*state, row.key(), *row, protectedSet)) continue;
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
            if (state.key() != owner && state->records.isEmpty() && routes(*state).isEmpty()) state = owners.erase(state);
            else ++state;
        return changed;
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
            if (!it->bootstrap.isEmpty() || !it->clearPending.isEmpty()) { target = it.key(); break; }
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
                }
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
                    if (result.failure != Failure::None) state.failure = result.failure;
                    else if (result.record && key(result.record->stream, result.record->topic) != route)
                        state.failure = Failure::Corrupt;
                    else if (state.clearPending.remove(route)) {
                        state.loaded.insert(route); state.bootstrap.remove(route);
                        if (result.record && !result.record->consumed) {
                            auto record = *result.record; record.consumed = true; record.phase = DraftRecord::Phase::Deleting;
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
        if (!waiters.isEmpty()) finish(!hasDirty() && !invalidInput());
    }
};

NativeDrafts::NativeDrafts(NativeDraftService *storage, QObject *parent)
    : QObject(parent), d(std::make_unique<Private>(this, storage)) {}
NativeDrafts::~NativeDrafts() = default;
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
            if (d->protectedRow(*state, row.key(), *row, protectedRoutes)) continue;
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
            found->record.consumed = true; found->record.phase = DraftRecord::Phase::Deleting;
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
    auto hold = std::make_shared<Hold>(); hold->uuid = result.uuid;
    const QPointer<NativeDrafts> guard(d->q);
    hold->released = [guard] {
        if (guard) QTimer::singleShot(0, guard, [guard] { if (guard) guard->d->schedule(); });
    };
    state->holds[route].append(hold); result.residency = std::move(hold);
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
