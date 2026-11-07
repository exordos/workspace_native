#include "native-cache-service.h"
#include "domain.h"

#include <QCryptographicHash>
#include <QEventLoop>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <algorithm>
#include <deque>

namespace workspace
{
namespace
{
std::atomic<quint64> ServiceGeneration{0};
enum class Op { Open, Resize, Close, Get, GetBody, Put, Project, Invalidate, OmitBody, Stale, Read, Forget, Flush, Stop };
struct Identity {
    QUrl server;
    QString project, user, owner;
};
Identity identity(QUrl server, QString project, QString user, bool loopback)
{
    const bool test =
        loopback && server.scheme() == "http" &&
        (server.host() == "127.0.0.1" || server.host() == "::1" || server.host() == "localhost");
    if (!server.isValid() || server.isRelative() || (server.scheme() != "https" && !test) ||
        server.host().isEmpty() || server.authority().contains('@') || server.hasQuery() ||
        server.hasFragment() || (!server.path().isEmpty() && server.path() != "/") || !isUuid(project) ||
        !isUuid(user))
        return {};
    server.setHost(server.host().toLower());
    server.setPath({});
    if ((server.scheme() == "https" && server.port() == 443) || (test && server.port() == 80))
        server.setPort(-1);
    project = project.toLower();
    user = user.toLower();
    const auto bytes =
        QJsonDocument(QJsonArray{"workspace-cache-v1", server.toString(QUrl::FullyEncoded), project, user})
            .toJson(QJsonDocument::Compact);
    return {server, project, user,
            QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())};
}
struct Input {
    Op op = Op::Flush;
    Identity identity;
    NativeCache::Key key{NativeCache::Kind::Streams, {}, {}, {}};
    QByteArray payload, next;
    QSet<QString> ids;
    NativeCache::ProjectionFields fields;
    qint64 limit = NativeCache::DefaultDiskLimitBytes;
    quint64 epoch = 0;
    bool retainOnly = false;
};
struct Result {
    bool okay = false, cancelled = false;
    QString error, availableOwner;
    std::optional<NativeCache::Entry> entry;
    std::optional<NativeCache::MessageBody> body;
    NativeCache::Stats stats;
    qint64 usage = 0, applied = 0;
};
struct Shared {
    std::atomic<quint64> epoch{1};
    QMutex mutex;
    Result result;
};
struct Job {
    Input input;
    quint64 ownerGeneration = 0, revision = 0;
    bool required = false;
    QPointer<QObject> receiver;
    NativeCacheService::Completion done;
    NativeCacheService::ReadCallback read;
    NativeCacheService::BodyReadCallback readBody;
    qint64 bytes() const
    {
        return input.payload.size() + input.next.size() + input.key.marker.size() +
               qint64(input.ids.size()) * 80;
    }
};
class CacheWorker final : public QObject
{
    Q_OBJECT
  public:
    CacheWorker(QString directory, NativeCache::Policy policy, bool loopback, std::function<qint64()> clock,
                std::shared_ptr<Shared> shared)
        : directory(std::move(directory)), policy(policy), loopback(loopback), clock(std::move(clock)),
          shared(std::move(shared))
    {
    }
    void run(Input input)
    {
        Result result;
        const auto epoch = input.epoch;
        const auto cancelled = [state = shared, epoch] { return state->epoch.load() != epoch; };
        if (input.op == Op::Stop || input.op == Op::Close) {
            if (store)
                result.usage = store->diskUsageBytes();
            store.reset();
            current = {};
            result.okay = true;
        } else if (!cancelled()) {
            if (input.op == Op::Open || input.op == Op::Resize) {
                if (input.op == Op::Open || !store || !store->available() ||
                    current.owner != input.identity.owner) {
                    store.reset();
                    store = std::make_unique<NativeCache>(directory, policy, loopback, clock);
                    store->setCancellationCheck(cancelled);
                    current = input.identity;
                    result.okay = store->setDiskLimitBytes(input.limit) &&
                                  store->open(current.server, current.project, current.user);
                } else {
                    store->setCancellationCheck(cancelled);
                    result.okay = store->setDiskLimitBytes(input.limit);
                }
                if (result.okay)
                    applied = input.limit;
            } else if (input.op == Op::Forget || input.op == Op::Invalidate || input.op == Op::Read || input.op == Op::OmitBody) {
                // Required erasures retain their captured owner across sign-out
                // and account changes. Reopen with the last applied budget, not
                // an uncompleted lower target that would delay privacy cleanup.
                NativeCache *target = store.get();
                std::unique_ptr<NativeCache> cleanup;
                if (!target || !target->available() ||
                    (input.op != Op::Forget && current.owner != input.identity.owner)) {
                    cleanup = std::make_unique<NativeCache>(directory, policy, loopback, clock);
                    target = cleanup.get();
                    target->setCancellationCheck(cancelled);
                    if (!target->setDiskLimitBytes(NativeCache::MaximumDiskLimitBytes) ||
                        !target->open(input.identity.server, input.identity.project, input.identity.user))
                        target = nullptr;
                } else
                    target->setCancellationCheck(cancelled);
                if (target) {
                    if (input.op == Op::Forget)
                        result.okay = target->forgetAccount(input.identity.server, input.identity.project,
                                                            input.identity.user);
                    if (input.op == Op::Invalidate)
                        result.okay = target->invalidate(target->lease(), input.key.kind, input.key.stream,
                                                         input.key.topic);
                    if (input.op == Op::Read)
                        result.okay = target->markHistoryRead(target->lease(), input.key.stream,
                                                              input.key.topic, input.ids);
                    if (input.op == Op::OmitBody)
                        result.okay = target->removeMessageBodyAndHistory(target->lease(), QString::fromUtf8(input.key.marker));
                    result.error = target->error();
                    result.usage = target->diskUsageBytes();
                } else if (cleanup)
                    result.error = cleanup->error();
                if (!result.okay && result.error.isEmpty() && !cancelled())
                    result.error = NativeCacheService::tr("The local message cache is unavailable.");
            } else if (input.op == Op::Flush)
                result.okay = true;
            else {
                if ((input.op == Op::Put || input.op == Op::Project) &&
                    (!store || !store->available() || current.owner != input.identity.owner)) {
                    store.reset();
                    store = std::make_unique<NativeCache>(directory, policy, loopback, clock);
                    store->setCancellationCheck(cancelled);
                    current = input.identity;
                    if (!store->setDiskLimitBytes(applied > 0 ? applied
                                                              : NativeCache::MaximumDiskLimitBytes) ||
                        !store->open(current.server, current.project, current.user))
                        store.reset();
                }
                if (store && store->available() && current.owner == input.identity.owner) {
                    store->setCancellationCheck(cancelled);
                    if (input.op == Op::Get) {
                        result.entry = store->get(store->lease(), input.key);
                        result.okay = bool(result.entry);
                    }
                    if (input.op == Op::GetBody) {
                        result.body = store->getMessageBody(store->lease(), QString::fromUtf8(input.key.marker));
                        result.okay = bool(result.body);
                    }
                    if (input.op == Op::Put) {
                        const auto document = QJsonDocument::fromJson(input.payload);
                        result.okay = document.isArray() &&
                                      store->put(store->lease(), input.key, document.array(), input.next);
                        if (input.retainOnly) {
                            // Always remove this route's RAM freshness before
                            // completion or another worker job, even on failure.
                            const bool stale = store->markStale(store->lease(), input.key.kind,
                                                                input.key.stream, input.key.topic);
                            result.okay = stale && result.okay;
                        }
                    }
                    if (input.op == Op::Project) {
                        const auto document = QJsonDocument::fromJson(input.payload);
                        result.okay = document.isObject() && store->projectMessage(
                            store->lease(), document.object(), input.fields);
                    }
                    if (input.op == Op::Stale) {
                        if (input.key.kind == static_cast<NativeCache::Kind>(0)) {
                            store->markStale(store->lease());
                            result.okay = true;
                        } else
                            result.okay = store->markStale(store->lease(), input.key.kind, input.key.stream,
                                                           input.key.topic);
                    }
                }
            }
        }
        result.cancelled = cancelled();
        if (store) {
            if (result.cancelled) {
                store->close();
                result.entry.reset();
                result.body.reset();
            } else {
                result.stats = store->stats();
                result.usage = store->diskUsageBytes();
                if (store->available())
                    result.availableOwner = current.owner;
                if (result.error.isEmpty())
                    result.error = store->error();
            }
        }
        result.applied = applied;
        {
            QMutexLocker lock(&shared->mutex);
            shared->result = std::move(result);
        }
        emit completed();
        if (input.op == Op::Stop)
            emit finished();
    }
  signals:
    void completed();
    void finished();

  private:
    QString directory;
    NativeCache::Policy policy;
    bool loopback;
    std::function<qint64()> clock;
    std::shared_ptr<Shared> shared;
    std::unique_ptr<NativeCache> store;
    Identity current;
    qint64 applied = 0;
};
} // namespace

struct NativeCacheService::Private {
    NativeCacheService *q;
    std::shared_ptr<Shared> shared = std::make_shared<Shared>();
    QThread thread;
    CacheWorker *worker = nullptr;
    std::deque<Job> queue;
    std::optional<Job> active;
    Identity owner;
    Lease lease;
    Stats stats;
    qint64 usage = 0, requested = NativeCache::DefaultDiskLimitBytes, applied = 0;
    quint64 revision = 0;
    bool available = false, busy = false, shutting = false, stopped = false, destroying = false,
         loopback = false;
    QString error;
    explicit Private(NativeCacheService *q) : q(q) {}
    bool accepts(const Lease &value) const
    {
        return !shutting && !owner.owner.isEmpty() && value.owner == lease.owner &&
               value.generation == lease.generation;
    }
    bool setError(QString value)
    {
        if (error == value)
            return true;
        error = std::move(value);
        const QPointer<NativeCacheService> guard(q);
        if (!destroying)
            emit q->errorChanged();
        return bool(guard);
    }
    Job make(Op op, QObject *receiver = nullptr, Completion done = {})
    {
        Job job;
        job.input.op = op;
        job.input.identity = owner;
        job.input.limit = requested;
        job.ownerGeneration = lease.generation;
        job.revision = revision;
        job.receiver = receiver ? receiver : q;
        job.done = std::move(done);
        return job;
    }
    void notify(Job job, bool okay, std::optional<Entry> entry = {}, std::optional<MessageBody> body = {})
    {
        if (!job.receiver || destroying)
            return;
        QPointer<NativeCacheService> guard(q);
        const auto generation = job.ownerGeneration, rev = job.revision;
        const bool required = job.required;
        const auto receiver = job.receiver;
        QMetaObject::invokeMethod(
            q,
            [guard, receiver, job = std::move(job), okay, entry = std::move(entry), body = std::move(body), generation, rev,
             required]() mutable {
                if (!guard || !receiver || guard->d->destroying || receiver->thread() != guard->thread())
                    return;
                const bool valid =
                    required || (generation == guard->d->lease.generation && rev == guard->d->revision);
                if (job.read)
                    job.read(valid ? std::move(entry) : std::optional<Entry>{});
                if (job.readBody)
                    job.readBody(valid ? std::move(body) : std::optional<MessageBody>{});
                if (job.done)
                    job.done(okay && valid);
            },
            Qt::QueuedConnection);
    }
    void dropOptional(bool maintenance = true)
    {
        for (auto it = queue.begin(); it != queue.end();) {
            if (!it->required && (maintenance || (it->input.op != Op::Open && it->input.op != Op::Resize))) {
                auto old = std::move(*it);
                it = queue.erase(it);
                notify(std::move(old), false);
            } else
                ++it;
        }
    }
    bool enqueue(Job job)
    {
        if (shutting && job.input.op != Op::Stop && job.input.op != Op::Flush) {
            notify(std::move(job), false);
            return false;
        }
        // Freshness revocation has one coalesced, zero-payload reserve beyond
        // the 64 ordinary required controls. Keep the newest barrier AFTER all
        // previously queued snapshots; a rejected local projection must never
        // leave an older authoritative page behind. Only one such queued job
        // exists across owners; an old owner's RAM freshness dies on reopen.
        const bool globalStale = job.required && !job.done && job.input.op == Op::Stale &&
                                 job.input.key.kind == static_cast<Kind>(0) && job.bytes() == 0;
        if (globalStale) {
            for (auto it = queue.begin(); it != queue.end();) {
                if (it->required && !it->done && it->input.op == Op::Stale &&
                    it->input.key.kind == static_cast<Kind>(0))
                    it = queue.erase(it);
                else
                    ++it;
            }
        }
        if (job.input.op == Op::Put || job.input.op == Op::Project)
            for (auto it = queue.begin(); it != queue.end(); ++it)
                if (it->input.op == job.input.op && it->input.identity.owner == job.input.identity.owner &&
                    (job.input.op != Op::Project || (it->input.fields == job.input.fields &&
                        QJsonDocument::fromJson(it->input.payload).object().value("uuid") ==
                        QJsonDocument::fromJson(job.input.payload).object().value("uuid"))) &&
                    it->input.key.kind == job.input.key.kind &&
                    it->input.key.stream == job.input.key.stream &&
                    it->input.key.topic == job.input.key.topic &&
                    it->input.key.marker == job.input.key.marker) {
                    auto old = std::move(*it);
                    queue.erase(it);
                    notify(std::move(old), false);
                    break;
                }
        if (job.required && !job.done && (job.input.op == Op::Invalidate || job.input.op == Op::Stale)) {
            for (auto &queued : queue)
                if (queued.input.op == job.input.op && !queued.done &&
                    queued.input.identity.owner == job.input.identity.owner &&
                    queued.input.key.kind == job.input.key.kind &&
                    queued.input.key.stream == job.input.key.stream &&
                    queued.input.key.topic == job.input.key.topic)
                    return true;
        }
        const auto privacy = [](Op op) { return op == Op::Forget || op == Op::OmitBody || op == Op::Close || op == Op::Stop; };
        const auto optionalData = [](Op op) { return op == Op::Put || op == Op::Get || op == Op::GetBody || op == Op::Project; };
        int optional = active && optionalData(active->input.op) ? 1 : 0;
        int controls = active && active->required && !privacy(active->input.op) ? 1 : 0;
        qint64 bytes = active ? active->bytes() : 0;
        for (const auto &queued : queue) {
            if (queued.required && !privacy(queued.input.op))
                ++controls;
            if (optionalData(queued.input.op))
                ++optional;
            bytes += queued.bytes();
        }
        if (job.required && controls >= 64 && !job.done &&
            (job.input.op == Op::Invalidate || job.input.op == Op::Stale)) {
            for (auto it = queue.begin(); it != queue.end();) {
                if (it->input.op == job.input.op && !it->done &&
                    it->input.identity.owner == job.input.identity.owner &&
                    it->input.key.kind == job.input.key.kind) {
                    it = queue.erase(it);
                    --controls;
                } else
                    ++it;
            }
            job.input.key.stream.clear();
            job.input.key.topic.clear();
        }
        if ((optionalData(job.input.op) && optional >= MaximumQueuedJobs) ||
            (!privacy(job.input.op) && bytes + job.bytes() > MaximumQueuedBytes) ||
            (job.required && !privacy(job.input.op) && controls >= 64 && !globalStale)) {
            if (job.required && !setError(q->tr("The local cache cleanup queue is busy. Please try again.")))
                return false;
            notify(std::move(job), false);
            return false;
        }
        const bool required = job.required;
        const bool interruptMaintenance = required && job.input.op != Op::Stale;
        const bool priority =
            job.input.op == Op::Invalidate || job.input.op == Op::OmitBody || job.input.op == Op::Forget || job.input.op == Op::Close;
        if (priority)
            queue.push_front(std::move(job));
        else
            queue.push_back(std::move(job));
        if (interruptMaintenance && active &&
            (active->input.op == Op::Open || active->input.op == Op::Resize) && !shutting) {
            ++shared->epoch;
            bool scheduled = false;
            for (const auto &queued : queue)
                if (queued.input.op == Op::Open || queued.input.op == Op::Resize)
                    scheduled = true;
            if (!scheduled && !owner.owner.isEmpty())
                queue.push_back(make(Op::Resize));
        }
        pump();
        return true;
    }
    void pump()
    {
        if (active || stopped)
            return;
        if (queue.empty()) {
            if (shutting) {
                auto stop = make(Op::Stop);
                stop.required = true;
                queue.push_back(std::move(stop));
            } else
                return;
        }
        active = std::move(queue.front());
        queue.pop_front();
        active->input.epoch = shared->epoch.load();
        const auto input = active->input;
        QMetaObject::invokeMethod(
            worker, [worker = worker, input] { worker->run(input); }, Qt::QueuedConnection);
    }
    void completed()
    {
        Result result;
        {
            QMutexLocker lock(&shared->mutex);
            result = std::move(shared->result);
        }
        if (!active)
            return;
        auto job = std::move(*active);
        active.reset();
        if (result.cancelled && (job.required || (shutting && job.ownerGeneration == lease.generation
            && (job.input.op == Op::Put || job.input.op == Op::Project))) &&
            job.input.op != Op::Stop && job.input.op != Op::Close) {
            queue.push_front(std::move(job));
            pump();
            return;
        }
        const QPointer<NativeCacheService> guard(q);
        const bool valid = !result.cancelled && job.input.epoch == shared->epoch.load() &&
                           job.ownerGeneration == lease.generation;
        const auto previousUsage = usage;
        const auto previousApplied = applied;
        const auto previousEntries = stats.entries;
        const auto previousBytes = stats.bytes;
        bool changed = false;
        if (valid || job.required) {
            usage = result.usage;
            stats = result.stats;
            applied = result.applied;
            changed = usage != previousUsage || applied != previousApplied ||
                      stats.entries != previousEntries || stats.bytes != previousBytes;
        }
        if (valid && (job.input.op == Op::Open || job.input.op == Op::Resize)) {
            busy = false;
            available = result.okay;
            if (!setError(result.error))
                return;
            changed = true;
            if (result.okay && !destroying)
                emit q->limitApplied(applied);
            if (!guard)
                return;
        } else if ((valid || job.required) && !result.error.isEmpty()) {
            available = !owner.owner.isEmpty() && result.availableOwner == owner.owner;
            if (!setError(result.error))
                return;
            changed = true;
        }
        if (changed && !destroying)
            emit q->stateChanged();
        if (!guard)
            return;
        if (!valid && !job.required) {
            result.entry.reset(); result.body.reset();
        }
        const bool stop = job.input.op == Op::Stop;
        const bool okay = (valid || job.required) && result.okay;
        notify(std::move(job), okay, std::move(result.entry), std::move(result.body));
        if (!stop)
            pump();
    }
};

NativeCacheService::NativeCacheService(QString directory, QObject *parent)
    : NativeCacheService(std::move(directory), Policy{}, false, {}, parent)
{
}
NativeCacheService::NativeCacheService(QString directory, Policy policy, bool loopback,
                                       std::function<qint64()> clock, QObject *parent)
    : QObject(parent), d(std::make_unique<Private>(this))
{
    d->loopback = loopback;
    d->lease.generation = ++ServiceGeneration;
    d->worker = new CacheWorker(std::move(directory), policy, loopback, std::move(clock), d->shared);
    d->worker->moveToThread(&d->thread);
    connect(d->worker, &CacheWorker::completed, this, [this] { d->completed(); });
    connect(d->worker, &CacheWorker::finished, &d->thread, &QThread::quit, Qt::DirectConnection);
    connect(&d->thread, &QThread::finished, d->worker, &QObject::deleteLater);
    connect(&d->thread, &QThread::finished, this, [this] {
        d->stopped = true;
        if (!d->destroying)
            emit shutdownFinished();
    });
    d->thread.start();
}
NativeCacheService::~NativeCacheService()
{
    // Production waits for shutdownFinished before quitting the application.
    // Destruction during a test/service replacement still processes cancellation
    // and required erasures; it never waits on an uninterruptible whole-cache loop.
    d->destroying = true;
    shutdown();
    if (!d->stopped) {
        QEventLoop loop;
        connect(&d->thread, &QThread::finished, &loop, &QEventLoop::quit);
        if (d->thread.isRunning())
            loop.exec();
    }
    d->thread.wait();
}
NativeCacheService::Lease NativeCacheService::lease() const
{
    return d->lease;
}
bool NativeCacheService::available() const
{
    return d->available && !d->busy && !d->shutting;
}
QString NativeCacheService::error() const
{
    return d->error;
}
NativeCacheService::Stats NativeCacheService::stats() const
{
    return d->stats;
}
qint64 NativeCacheService::diskUsageBytes() const
{
    return d->usage;
}
qint64 NativeCacheService::requestedDiskLimitBytes() const
{
    return d->requested;
}
qint64 NativeCacheService::appliedDiskLimitBytes() const
{
    return d->applied;
}
bool NativeCacheService::maintenanceBusy() const
{
    return d->busy;
}
bool NativeCacheService::stopped() const
{
    return d->stopped;
}

bool NativeCacheService::open(const QUrl &server, const QString &project, const QString &user,
                              QObject *receiver, Completion done)
{
    auto next = identity(server, project, user, d->loopback);
    if (d->shutting || next.owner.isEmpty()) {
        d->setError(tr("The local message cache is unavailable."));
        return false;
    }
    ++d->shared->epoch;
    ++d->revision;
    d->dropOptional();
    d->owner = std::move(next);
    d->lease = {d->owner.owner, ++ServiceGeneration};
    d->available = false;
    d->busy = true;
    const QPointer<NativeCacheService> guard(this);
    const bool accepted = d->enqueue(d->make(Op::Open, receiver, std::move(done)));
    if (!guard)
        return false;
    emit stateChanged();
    return guard && accepted;
}
void NativeCacheService::close()
{
    if (d->shutting)
        return;
    ++d->shared->epoch;
    ++d->revision;
    d->dropOptional();
    d->owner = {};
    d->lease = {{}, ++ServiceGeneration};
    d->available = false;
    d->busy = false;
    auto job = d->make(Op::Close);
    job.required = true;
    d->enqueue(std::move(job));
    emit stateChanged();
}
bool NativeCacheService::setDiskLimitBytes(qint64 bytes)
{
    if (d->shutting || bytes < NativeCache::MinimumDiskLimitBytes ||
        bytes > NativeCache::MaximumDiskLimitBytes) {
        d->setError(tr("The cache limit must be between 1 MiB and 1000 GB."));
        return false;
    }
    d->requested = bytes;
    if (d->owner.owner.isEmpty()) {
        if (!d->setError({}))
            return false;
        const QPointer<NativeCacheService> guard(this);
        emit stateChanged();
        return bool(guard);
    }
    ++d->shared->epoch;
    ++d->revision;
    d->dropOptional();
    d->available = false;
    d->busy = true;
    if (!d->setError({}))
        return false;
    const QPointer<NativeCacheService> guard(this);
    auto job = d->make(Op::Resize);
    const bool accepted = d->enqueue(std::move(job));
    if (!guard)
        return false;
    emit stateChanged();
    return guard && accepted;
}
void NativeCacheService::get(const Lease &lease, const Key &key, QObject *receiver, ReadCallback done)
{
    auto job = d->make(Op::Get, receiver);
    job.input.key = key;
    job.read = std::move(done);
    if (!available() || !d->accepts(lease)) {
        d->notify(std::move(job), false);
        return;
    }
    d->enqueue(std::move(job));
}
void NativeCacheService::getMessageBody(const Lease &lease, const QString &uuid, QObject *receiver, BodyReadCallback done)
{
    auto job = d->make(Op::GetBody, receiver);
    job.input.key = {Kind::MessageBody, {}, {}, uuid.toLower().toUtf8()};
    job.readBody = std::move(done);
    if (!isUuid(uuid) || !available() || !d->accepts(lease)) {
        d->notify(std::move(job), false); return;
    }
    d->enqueue(std::move(job));
}

bool NativeCacheService::putMessageBody(const Lease &lease, const QJsonObject &canonical,
                                        QObject *receiver, Completion done)
{
    return put(lease, {Kind::MessageBody, canonical.value("stream_uuid").toString(),
        canonical.value("topic_uuid").toString(), canonical.value("uuid").toString().toLower().toUtf8()},
        {canonical}, {}, receiver, std::move(done));
}

bool NativeCacheService::removeMessageBodyAndHistory(const Lease &lease, const QString &uuid,
                                                     QObject *receiver, Completion done)
{
    auto job = d->make(Op::OmitBody, receiver, std::move(done));
    job.input.key = {Kind::MessageBody, {}, {}, uuid.toLower().toUtf8()};
    job.required = true;
    if (!d->accepts(lease) || !isUuid(uuid)) { d->notify(std::move(job), false); return false; }
    // As with Forget, at most64 captured-owner erasures reserve only bounded
    // identity metadata beyond the ordinary control/raw-JSON budget. A full
    // optional/control queue must not drop an accepted authoritative omission.
    const auto erasure = [](Op op) { return op == Op::Forget || op == Op::OmitBody; };
    int cleanups = d->active && erasure(d->active->input.op) ? 1 : 0;
    for (const auto &queued : d->queue) if (erasure(queued.input.op)) ++cleanups;
    if (cleanups >= 64) {
        if (d->setError(tr("The local cache cleanup queue is busy. Please try again.")))
            d->notify(std::move(job), false);
        return false;
    }
    ++d->revision; job.revision = d->revision;
    // UUID metadata is captured while the bounded raw arrays are admitted,
    // rather than reparsing32MiB of queued JSON on this GUI-thread barrier.
    const auto id = uuid.toLower();
    const QPointer<NativeCacheService> guard(this);
    for (auto it = d->queue.begin(); it != d->queue.end();) {
        const auto &input = it->input;
        const bool affected = input.identity.owner == job.input.identity.owner &&
            ((input.op == Op::GetBody && input.key.marker == job.input.key.marker) ||
             ((input.op == Op::Put || input.op == Op::Project) && input.ids.contains(id)));
        if (affected) {
            auto old = std::move(*it); it = d->queue.erase(it); d->notify(std::move(old), false);
        } else ++it;
    }
    return guard && d->enqueue(std::move(job));
}

bool NativeCacheService::put(const Lease &lease, const Key &key, const QJsonArray &rows,
                             const QByteArray &next, QObject *receiver, Completion done, bool retainOnly)
{
    auto job = d->make(Op::Put, receiver, std::move(done));
    job.input.key = key;
    job.input.next = next;
    if (!d->accepts(lease) || rows.size() > 10000) {
        d->notify(std::move(job), false);
        return false;
    }
    if (key.kind == Kind::MessageBody && (rows.size() != 1 || !next.isEmpty()
        || rows.first().toObject().value("user_uuid").toString().compare(d->owner.user, Qt::CaseInsensitive)
        || rows.first().toObject().value("uuid").toString().compare(QString::fromUtf8(key.marker), Qt::CaseInsensitive))) {
        d->notify(std::move(job), false); return false;
    }
    if (key.kind == Kind::History || key.kind == Kind::MessageBody)
        for (const auto &row : rows) {
            const auto id = row.toObject().value("uuid").toString();
            if (isUuid(id)) job.input.ids.insert(id.toLower());
        }
    job.input.payload = QJsonDocument(rows).toJson(QJsonDocument::Compact);
    if (job.input.payload.size() + next.size() + key.marker.size() > MaximumPageBytes) {
        d->notify(std::move(job), false);
        return false;
    }
    job.input.retainOnly = retainOnly;
    if (retainOnly) {
        ++d->revision;
        job.revision = d->revision;
    }
    const QPointer<NativeCacheService> guard(this);
    const bool accepted = d->enqueue(std::move(job));
    if (guard && !accepted && retainOnly && d->accepts(lease))
        markStale(lease);
    return guard && accepted;
}
bool NativeCacheService::invalidate(const Lease &lease, Kind kind, const QString &stream,
                                    const QString &topic, QObject *receiver, Completion done)
{
    auto job = d->make(Op::Invalidate, receiver, std::move(done));
    job.input.key = {kind, stream, topic, {}};
    job.required = true;
    if (!d->accepts(lease)) {
        d->notify(std::move(job), false);
        return false;
    }
    ++d->revision;
    // Deletion dominates already accepted snapshots/projections of this route,
    // while independent owners, routes and field masks retain their FIFO work.
    for (auto it = d->queue.begin(); it != d->queue.end();) {
        const auto &input = it->input;
        if ((input.op == Op::Get || input.op == Op::GetBody || input.op == Op::Put || input.op == Op::Project)
            && input.identity.owner == job.input.identity.owner
            && NativeCache::matchesScope(input.key, kind, stream, topic)) {
            auto old = std::move(*it); it = d->queue.erase(it); d->notify(std::move(old), false);
        } else ++it;
    }
    job.revision = d->revision;
    return d->enqueue(std::move(job));
}
void NativeCacheService::markStale(const Lease &lease)
{
    markStale(lease, static_cast<Kind>(0), {}, {});
}
bool NativeCacheService::markStale(const Lease &lease, Kind kind, const QString &stream, const QString &topic)
{
    if (!d->accepts(lease))
        return false;
    ++d->revision;
    auto job = d->make(Op::Stale);
    job.input.key = {kind, stream, topic, {}};
    job.required = true;
    return d->enqueue(std::move(job));
}
bool NativeCacheService::markHistoryRead(const Lease &lease, const QString &stream, const QString &topic,
                                         const QSet<QString> &ids, QObject *receiver, Completion done)
{
    auto job = d->make(Op::Read, receiver, std::move(done));
    job.input.key = {Kind::History, stream, topic, {}};
    job.input.ids = ids;
    job.required = true;
    if (!d->accepts(lease) || ids.isEmpty() || ids.size() > 100000) {
        d->notify(std::move(job), false);
        return false;
    }
    ++d->revision;
    job.revision = d->revision;
    return d->enqueue(std::move(job));
}
bool NativeCacheService::projectMessage(const Lease &lease, const QJsonObject &canonical,
                                        ProjectionFields fields, QObject *receiver, Completion done)
{
    auto job = d->make(Op::Project, receiver, std::move(done));
    job.input.key = {Kind::History,
                     canonical.value("stream_uuid").toString(),
                     canonical.value("topic_uuid").toString(),
                     {}};
    job.input.fields = fields;
    if (isUuid(canonical.value("uuid").toString())) job.input.ids.insert(canonical.value("uuid").toString().toLower());
    if (!d->accepts(lease) || fields.toInt() == 0 || (fields.toInt() & ~15)) {
        d->notify(std::move(job), false);
        return false;
    }
    job.input.payload = QJsonDocument(canonical).toJson(QJsonDocument::Compact);
    if (job.input.payload.size() > MaximumPageBytes) {
        d->notify(std::move(job), false);
        return false;
    }
    return d->enqueue(std::move(job));
}
bool NativeCacheService::clearOwner(const Lease &lease, QObject *receiver, Completion done)
{
    if (!d->accepts(lease))
        return false;
    return forgetAccount(d->owner.server, d->owner.project, d->owner.user, receiver, std::move(done));
}
bool NativeCacheService::forgetAccount(const QUrl &server, const QString &project, const QString &user,
                                       QObject *receiver, Completion done)
{
    auto target = identity(server, project, user, d->loopback);
    if (target.owner.isEmpty() || d->shutting)
        return false;
    int cleanups = d->active && (d->active->input.op == Op::Forget || d->active->input.op == Op::OmitBody) ? 1 : 0;
    for (const auto &job : d->queue)
        if (job.input.op == Op::Forget || job.input.op == Op::OmitBody)
            ++cleanups;
    if (cleanups >= 64) {
        d->setError(tr("The local cache cleanup queue is busy. Please try again."));
        return false;
    }
    if (target.owner == d->owner.owner) {
        ++d->shared->epoch;
        ++d->revision;
        d->dropOptional();
        d->available = false;
        d->busy = false;
        d->owner = {};
        d->lease = {{}, ++ServiceGeneration};
    }
    const QPointer<NativeCacheService> guard(this);
    auto job = d->make(Op::Forget, receiver, std::move(done));
    job.input.identity = std::move(target);
    job.required = true;
    const bool accepted = d->enqueue(std::move(job));
    if (!guard)
        return false;
    emit stateChanged();
    return guard && accepted;
}
void NativeCacheService::flush(QObject *receiver, Completion done)
{
    d->enqueue(d->make(Op::Flush, receiver, std::move(done)));
}
void NativeCacheService::shutdown()
{
    if (d->shutting || d->stopped)
        return;
    d->shutting = true;
    d->available = false;
    d->busy = false;
    ++d->shared->epoch;
    ++d->revision;
    // Superseded maintenance is cancelled; already accepted writes and required
    // erasures remain FIFO and run against the last completed storage budget.
    for (auto it = d->queue.begin(); it != d->queue.end();) {
        if (it->input.op == Op::Open || it->input.op == Op::Resize || it->input.op == Op::Get || it->input.op == Op::GetBody) {
            auto old = std::move(*it);
            it = d->queue.erase(it);
            d->notify(std::move(old), false);
        } else
            ++it;
    }
    d->pump();
    if (!d->destroying)
        emit stateChanged();
}

} // namespace workspace
#include "native-cache-service.moc"
