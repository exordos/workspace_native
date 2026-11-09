#include "native-draft-service.h"
#include "domain.h"

#include <QJsonDocument>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <deque>

namespace workspace {
namespace {
using Failure = NativeDraftStore::Failure;
std::atomic<quint64> Issuer{0};
enum class Op { Open, Close, Read, Latest, List, Apply, Flush, Stop };
struct Result {
    Failure failure = Failure::Unavailable;
    NativeDraftStore::Read read;
    NativeDraftStore::Page page;
};
struct Job {
    Op op = Op::Flush;
    NativeDraftService::Lease lease;
    QString id, stream, topic;
    QVector<DraftRecord> writes;
    QStringList removals;
    qsizetype bytes = 0;
    QPointer<QObject> receiver;
    std::function<void(Result)> done;
    bool data() const { return op == Op::Read || op == Op::Latest || op == Op::List || op == Op::Apply; }
    bool optional() const { return op == Op::Read || op == Op::Latest || op == Op::List || op == Op::Open || op == Op::Close; }
};
class DraftWorker final : public QObject {
public:
    explicit DraftWorker(QString directory) : directory(std::move(directory)) {}
    Result run(const Job &job)
    {
        Result result;
        if (job.op == Op::Stop || job.op == Op::Close) {
            store.reset(); owner.clear(); result.failure = Failure::None;
        } else if (job.op == Op::Flush) {
            result.failure = Failure::None; // FIFO barrier after committed Apply jobs.
        } else {
            if (!store || owner != job.lease.owner || !store->available()) {
                store.reset(); store = std::make_unique<NativeDraftStore>(directory);
                result.failure = store->open(job.lease.owner);
                owner = result.failure == Failure::None ? job.lease.owner : QString{};
            } else result.failure = Failure::None;
            if (result.failure == Failure::None) {
                switch (job.op) {
                case Op::Read: result.read = store->read(job.id); result.failure = result.read.failure; break;
                case Op::Latest: result.read = store->latest(job.stream, job.topic); result.failure = result.read.failure; break;
                case Op::List: result.page = store->list(job.id); result.failure = result.page.failure; break;
                case Op::Apply: result.failure = store->apply(job.writes, job.removals); break;
                default: break;
                }
            }
        }
        result.read.failure = result.page.failure = result.failure;
        return result;
    }
    QString directory, owner;
    std::unique_ptr<NativeDraftStore> store;
};
bool validOwner(const QString &owner)
{
    static const QRegularExpression pattern("\\A[0-9a-f]{64}\\z");
    return pattern.match(owner).hasMatch();
}
}

struct NativeDraftService::Private {
    NativeDraftService *q;
    const quint64 issuer = ++Issuer;
    quint64 generation = 0;
    QString owner;
    Failure failure = Failure::Unavailable;
    bool available = false, closing = false, stopped = false;
    QThread thread;
    DraftWorker *worker;
    std::deque<Job> queue;
    std::optional<Job> active;
    int jobs = 0, controls = 0;
    qsizetype bytes = 0;

    Private(NativeDraftService *q, QString directory) : q(q), worker(new DraftWorker(std::move(directory)))
    {
        worker->moveToThread(&thread);
        QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
        thread.start();
    }
    bool current(const Lease &lease) const
    {
        return !closing && lease.issuer == issuer && lease.generation == generation && !owner.isEmpty() && lease.owner == owner;
    }
    void deliver(Job job, Result result)
    {
        if (job.receiver && job.done) job.done(std::move(result));
    }
    void reject(Job job, Failure reason)
    {
        if (!job.receiver || !job.done) return;
        // A rejected input must not retain large caller-owned record/address
        // buffers until its small asynchronous failure receipt is delivered.
        job.writes.clear(); job.removals.clear(); job.id.clear(); job.stream.clear(); job.topic.clear();
        Result result; result.failure = result.read.failure = result.page.failure = reason;
        const QPointer<NativeDraftService> guard(q);
        QTimer::singleShot(0, q, [guard, job = std::move(job), result]() mutable {
            if (guard) guard->d->deliver(std::move(job), result);
        });
    }
    bool enqueue(Job job, bool control = false)
    {
        if (closing || (!control && (jobs >= MaximumDataJobs || job.bytes > MaximumPendingBytes - bytes))
            || (control && job.op != Op::Stop && controls >= MaximumControlJobs)) {
            reject(std::move(job), closing ? Failure::Unavailable : Failure::Capacity); return false;
        }
        if (job.data()) { ++jobs; bytes += job.bytes; }
        else ++controls;
        queue.push_back(std::move(job)); dispatch();
        return true;
    }
    void cancelOptional()
    {
        for (auto it = queue.begin(); it != queue.end();) {
            if (!it->optional()) { ++it; continue; }
            Job job = std::move(*it); it = queue.erase(it);
            if (job.data()) { --jobs; bytes -= job.bytes; }
            else --controls;
            reject(std::move(job), Failure::Unavailable);
        }
    }
    void dispatch()
    {
        if (active || queue.empty()) return;
        active = std::move(queue.front()); queue.pop_front();
        const Job job = *active;
        const QPointer<NativeDraftService> guard(q);
        QMetaObject::invokeMethod(worker, [guard, worker = worker, job] {
            Result result = worker->run(job);
            if (guard) QMetaObject::invokeMethod(guard, [guard, job, result]() mutable {
                if (guard) guard->d->complete(std::move(job), std::move(result));
            }, Qt::QueuedConnection);
        }, Qt::QueuedConnection);
    }
    void complete(Job job, Result result)
    {
        if (job.data()) { --jobs; bytes -= job.bytes; }
        else --controls;
        active.reset();
        if (job.op == Op::Open && current(job.lease)) {
            failure = result.failure; available = failure == Failure::None;
        }
        if ((job.op == Op::Read || job.op == Op::Latest || job.op == Op::List) && !current(job.lease)) {
            result = {}; result.failure = result.read.failure = result.page.failure = Failure::Unavailable;
        }
        const bool finished = job.op == Op::Stop;
        if (finished) { stopped = true; available = false; thread.quit(); }
        else dispatch();
        const QPointer<NativeDraftService> guard(q);
        emit q->stateChanged();
        if (!guard) return;
        deliver(std::move(job), std::move(result));
        if (guard && finished) emit q->shutdownFinished();
    }
};

NativeDraftService::NativeDraftService(QString directory, QObject *parent)
    : QObject(parent), d(std::make_unique<Private>(this, std::move(directory))) {}
NativeDraftService::~NativeDraftService()
{
    d->closing = true;
    // The active worker invocation is already queued before this blocking
    // barrier. Drain accepted writes; optional reads need no teardown callback.
    auto pending = std::move(d->queue);
    if (d->thread.isRunning() && !d->stopped) {
        QMetaObject::invokeMethod(d->worker, [worker = d->worker, pending = std::move(pending)] {
            for (const auto &job : pending) if (job.op == Op::Apply) worker->run(job);
            Job stop; stop.op = Op::Stop; worker->run(stop);
        }, Qt::BlockingQueuedConnection);
    }
    d->thread.quit(); d->thread.wait();
}

bool NativeDraftService::open(const QString &owner)
{
    if (d->closing) return false;
    ++d->generation; d->owner = validOwner(owner) ? owner : QString{};
    d->available = false; d->failure = d->owner.isEmpty() ? Failure::InvalidOwner : Failure::Unavailable;
    d->cancelOptional();
    if (d->owner.isEmpty()) { emit stateChanged(); return false; }
    Job job; job.op = Op::Open; job.lease = lease();
    const bool accepted = d->enqueue(std::move(job), true);
    emit stateChanged(); return accepted;
}
void NativeDraftService::close()
{
    if (d->closing) return;
    ++d->generation; d->owner.clear(); d->available = false; d->cancelOptional();
    Job job; job.op = Op::Close; d->enqueue(std::move(job), true); emit stateChanged();
}
NativeDraftService::Lease NativeDraftService::lease() const { return {d->owner, d->issuer, d->generation}; }
bool NativeDraftService::available() const { return d->available && !d->closing; }
NativeDraftService::Failure NativeDraftService::failure() const { return d->failure; }
bool NativeDraftService::busy() const { return d->active.has_value() || !d->queue.empty(); }
int NativeDraftService::pendingDataJobs() const { return d->jobs; }
qsizetype NativeDraftService::pendingBytes() const { return d->bytes; }
bool NativeDraftService::stopped() const { return d->stopped; }

bool NativeDraftService::read(const Lease &lease, const QString &uuid, QObject *receiver, ReadDone done)
{
    Job job; job.op = Op::Read; job.lease = lease; job.id = uuid; job.receiver = receiver ? receiver : this;
    job.done = [done = std::move(done)](Result r) { if (done) done(std::move(r.read)); };
    if (!d->current(lease) || !d->available) { d->reject(std::move(job), Failure::Unavailable); return false; }
    if (!isUuid(uuid) || uuid != uuid.toLower()) { d->reject(std::move(job), Failure::InvalidRecord); return false; }
    return d->enqueue(std::move(job));
}
bool NativeDraftService::latest(const Lease &lease, const QString &stream, const QString &topic, QObject *receiver, ReadDone done)
{
    Job job; job.op = Op::Latest; job.lease = lease; job.stream = stream; job.topic = topic; job.receiver = receiver ? receiver : this;
    job.done = [done = std::move(done)](Result r) { if (done) done(std::move(r.read)); };
    if (!d->current(lease) || !d->available) { d->reject(std::move(job), Failure::Unavailable); return false; }
    if (!isUuid(stream) || !isUuid(topic)) { d->reject(std::move(job), Failure::InvalidRecord); return false; }
    return d->enqueue(std::move(job));
}
bool NativeDraftService::list(const Lease &lease, const QString &afterUuid, QObject *receiver, PageDone done)
{
    Job job; job.op = Op::List; job.lease = lease; job.id = afterUuid; job.receiver = receiver ? receiver : this;
    job.done = [done = std::move(done)](Result r) { if (done) done(std::move(r.page)); };
    if (!d->current(lease) || !d->available) { d->reject(std::move(job), Failure::Unavailable); return false; }
    if (!afterUuid.isEmpty() && (!isUuid(afterUuid) || afterUuid != afterUuid.toLower())) {
        d->reject(std::move(job), Failure::InvalidRecord); return false;
    }
    return d->enqueue(std::move(job));
}
bool NativeDraftService::apply(const Lease &lease, const QVector<DraftRecord> &writes, const QStringList &removals, QObject *receiver, Done done)
{
    Job job; job.op = Op::Apply; job.lease = lease; job.writes = writes; job.removals = removals;
    job.receiver = receiver ? receiver : this; job.done = [done = std::move(done)](Result r) { if (done) done(r.failure); };
    if (!d->current(lease)) { d->reject(std::move(job), Failure::Unavailable); return false; }
    if (writes.size() + removals.size() > NativeDraftStore::MaximumPageRows) { d->reject(std::move(job), Failure::Capacity); return false; }
    QSet<QString> ids;
    for (const auto &r : writes) {
        if (!NativeDraftStore::valid(r) || ids.contains(r.uuid)) { d->reject(std::move(job), Failure::InvalidRecord); return false; }
        ids.insert(r.uuid);
        job.bytes += NativeDraftStore::serializedSize(r);
        if (job.bytes > NativeDraftStore::MaximumBatchBytes) { d->reject(std::move(job), Failure::Capacity); return false; }
    }
    for (const auto &id : removals) {
        if (!isUuid(id) || id != id.toLower() || ids.contains(id)) { d->reject(std::move(job), Failure::InvalidRecord); return false; }
        ids.insert(id);
    }
    if (job.bytes > NativeDraftStore::MaximumBatchBytes) { d->reject(std::move(job), Failure::Capacity); return false; }
    return d->enqueue(std::move(job));
}
bool NativeDraftService::flush(QObject *receiver, Done done)
{
    Job job; job.op = Op::Flush; job.receiver = receiver ? receiver : this;
    job.done = [done = std::move(done)](Result r) { if (done) done(r.failure); };
    return d->enqueue(std::move(job), true);
}
void NativeDraftService::shutdown()
{
    if (d->closing) return;
    d->cancelOptional();
    Job job; job.op = Op::Stop;
    d->enqueue(std::move(job), true); d->closing = true;
}

} // namespace workspace
