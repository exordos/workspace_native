#include "native-draft-store.h"
#include "domain.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLockFile>
#include <QRegularExpression>
#include <QSet>
#include <QSqlQuery>
#include <QSqlError>
#include <QStandardPaths>
#include <QThread>
#include <QUuid>
#include <QVariant>
#include <cmath>

namespace workspace {
namespace {
using Failure = NativeDraftStore::Failure;
constexpr auto PrivateFile = QFileDevice::ReadOwner | QFileDevice::WriteOwner;

bool draftText(const QString &text)
{
    if (text.size() > 80000 || text.contains(":::")) return false;
    int points = 0;
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text[i].isHighSurrogate()) {
            if (++i == text.size() || !text[i].isLowSurrogate()) return false;
        } else if (text[i].isLowSurrogate()) return false;
        if (++points > 40000) return false;
    }
    return true;
}

bool privateFreeContext(const QJsonValue &value, int depth = 0)
{
    if (depth > 16) return false;
    if (value.isString()) return !value.toString().contains(":::");
    if (value.isArray()) {
        for (const auto &child : value.toArray())
            if (!privateFreeContext(child, depth + 1)) return false;
    } else if (value.isObject()) {
        const auto object = value.toObject();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it)
            if (!privateFreeContext(it.value(), depth + 1)) return false;
    }
    return true;
}

QByteArray encode(const DraftRecord &r)
{
    return QJsonDocument(QJsonObject{{"format", 1}, {"uuid", r.uuid}, {"stream", r.stream}, {"topic", r.topic},
        {"snapshot", r.snapshot}, {"text", r.text}, {"replySession", r.replySession}, {"updatedAt", r.updatedAt},
        {"revision", r.revision}, {"phase", static_cast<int>(r.phase)}, {"consumed", r.consumed},
        {"createPending", r.createPending}, {"createText", r.createText},
        {"conflictText", r.conflictText}, {"conflictRevision", r.conflictRevision}}).toJson(QJsonDocument::Compact);
}

bool integer(const QJsonObject &json, const char *key, qint64 &target)
{
    const auto value = json.value(QLatin1String(key));
    const double number = value.toDouble(-1);
    if (!value.isDouble() || !std::isfinite(number) || number < 0 || number > 9007199254740991.0
        || std::floor(number) != number) return false;
    target = static_cast<qint64>(number);
    return true;
}

std::optional<NativeDraftStore::Summary> summary(const QSqlQuery &query)
{
    NativeDraftStore::Summary row{query.value(0).toString(), query.value(1).toString(), query.value(2).toString(),
        query.value(3).toLongLong(), static_cast<DraftRecord::Phase>(query.value(4).toInt()), query.value(5).toBool()};
    if (!isUuid(row.uuid) || row.uuid != row.uuid.toLower() || !isUuid(row.stream) || !isUuid(row.topic)
        || row.updatedAt < 0 || row.updatedAt > 9007199254740991LL || query.value(4).toInt() < 0
        || query.value(4).toInt() > static_cast<int>(DraftRecord::Phase::Deleting)
        || (query.value(5).toInt() != 0 && query.value(5).toInt() != 1)
        || query.value(6).toString() != "integer" || query.value(7).toString() != "integer"
        || query.value(8).toString() != "integer") return {};
    return row;
}

std::optional<DraftRecord> decode(const QByteArray &bytes)
{
    if (bytes.size() > NativeDraftStore::MaximumRecordBytes) return {};
    QJsonParseError error;
    const auto json = QJsonDocument::fromJson(bytes, &error).object();
    if (error.error != QJsonParseError::NoError || json.value("format") != QJsonValue(1)) return {};
    DraftRecord r;
    for (const auto *field : {"uuid", "stream", "topic", "snapshot", "text", "createText", "conflictText"})
        if (!json.value(QLatin1String(field)).isString()) return {};
    if (!json.value("replySession").isObject() || !json.value("consumed").isBool()
        || !json.value("createPending").isBool()) return {};
    qint64 phase = 0;
    if (!integer(json, "updatedAt", r.updatedAt) || !integer(json, "revision", r.revision)
        || !integer(json, "conflictRevision", r.conflictRevision) || !integer(json, "phase", phase)
        || phase > static_cast<int>(DraftRecord::Phase::Deleting)) return {};
    r.phase = static_cast<DraftRecord::Phase>(phase);
    r.uuid = json.value("uuid").toString(); r.stream = json.value("stream").toString(); r.topic = json.value("topic").toString();
    r.snapshot = json.value("snapshot").toString(); r.text = json.value("text").toString();
    r.replySession = json.value("replySession").toObject(); r.consumed = json.value("consumed").toBool();
    r.createPending = json.value("createPending").toBool(); r.createText = json.value("createText").toString();
    r.conflictText = json.value("conflictText").toString();
    return NativeDraftStore::valid(r) ? std::optional<DraftRecord>(r) : std::nullopt;
}
} // namespace

qsizetype NativeDraftStore::serializedSize(const DraftRecord &r) { return encode(r).size(); }

bool NativeDraftStore::valid(const DraftRecord &r)
{
    const auto number = [](qint64 n) { return n >= 0 && n <= 9007199254740991LL; };
    const int phase = static_cast<int>(r.phase);
    return isUuid(r.uuid) && r.uuid == r.uuid.toLower() && isUuid(r.stream) && isUuid(r.topic) && isUuid(r.snapshot)
        && number(r.updatedAt) && number(r.revision) && number(r.conflictRevision)
        && phase >= 0 && phase <= static_cast<int>(DraftRecord::Phase::Deleting)
        && (r.phase != DraftRecord::Phase::Saved || r.revision > 0)
        && (r.phase != DraftRecord::Phase::Conflict || r.conflictRevision > 0)
        && draftText(r.text) && draftText(r.createText) && draftText(r.conflictText)
        && privateFreeContext(r.replySession) && serializedSize(r) <= MaximumRecordBytes;
}

NativeDraftStore::NativeDraftStore(QString directory)
    : m_directory(directory.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/drafts"
                                      : std::move(directory)), m_thread(QThread::currentThread()) {}
NativeDraftStore::~NativeDraftStore() { close(); }
bool NativeDraftStore::sameThread() const { return m_thread == QThread::currentThread(); }
bool NativeDraftStore::available() const { return sameThread() && m_database.isOpen(); }

void NativeDraftStore::close()
{
    Q_ASSERT(sameThread());
    if (m_database.isOpen()) m_database.close();
    m_database = {};
    if (!m_connection.isEmpty()) QSqlDatabase::removeDatabase(m_connection);
    m_connection.clear(); m_path.clear(); m_lock.reset();
}

bool NativeDraftStore::protectFiles()
{
    for (const auto &suffix : {QString{}, QStringLiteral("-wal"), QStringLiteral("-shm")}) {
        const QFileInfo info(m_path + suffix);
        if (info.isSymLink() || (info.exists() && (!info.isFile() || !QFile::setPermissions(info.filePath(), PrivateFile)))) return false;
    }
    return true;
}

NativeDraftStore::Failure NativeDraftStore::open(const QString &owner)
{
    if (!sameThread()) return Failure::Unavailable;
    close();
    static const QRegularExpression hash("\\A[0-9a-f]{64}\\z");
    if (!hash.match(owner).hasMatch()) return Failure::InvalidOwner;
    if (QFileInfo(m_directory).isSymLink() || !QDir().mkpath(m_directory)
        || !QFile::setPermissions(m_directory, PrivateFile | QFileDevice::ExeOwner)) return Failure::Unavailable;
    m_path = QDir(m_directory).filePath(owner + ".sqlite3");
    if (QFileInfo(m_path).isSymLink() || QFileInfo(m_path + ".lock").isSymLink()) { close(); return Failure::Unavailable; }
    m_lock = std::make_unique<QLockFile>(m_path + ".lock");
    if (!m_lock->tryLock(0)) { close(); return Failure::Locked; }
    m_connection = "cassi-native-drafts-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_database = QSqlDatabase::addDatabase("QSQLITE", m_connection);
    m_database.setDatabaseName(m_path);
    if (!protectFiles() || !m_database.open() || !protectFiles()) { close(); return Failure::Unavailable; }
    Failure failure = Failure::None;
    {
        QSqlQuery query(m_database);
        int version = -1;
        if (!query.exec("PRAGMA user_version") || !query.next()) failure = Failure::Corrupt;
        else version = query.value(0).toInt();
        if (failure == Failure::None && version != 0 && version != 1) failure = Failure::Schema;
        if (failure == Failure::None) {
            for (const auto *sql : {"PRAGMA journal_mode=WAL", "PRAGMA synchronous=FULL", "PRAGMA foreign_keys=ON",
                    "PRAGMA cache_size=-2048", "PRAGMA secure_delete=ON"})
                if (!query.exec(QLatin1String(sql))) { failure = Failure::Unavailable; break; }
        }
        if (failure == Failure::None && version == 0) {
            if (!query.exec("SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'")) failure = Failure::Corrupt;
            else if (query.next()) failure = Failure::Schema;
            else if (!m_database.transaction()) failure = Failure::Unavailable;
            else {
                bool ok = query.exec("CREATE TABLE draft_owner (owner TEXT NOT NULL)");
                if (ok) {
                    query.prepare("INSERT INTO draft_owner VALUES (?)"); query.addBindValue(owner); ok = query.exec();
                }
                if (ok) ok = query.exec("CREATE TABLE draft_records (uuid TEXT PRIMARY KEY NOT NULL, stream TEXT NOT NULL, topic TEXT NOT NULL, "
                    "updated_at INTEGER NOT NULL, phase INTEGER NOT NULL, consumed INTEGER NOT NULL, payload BLOB NOT NULL)");
                if (ok) ok = query.exec("PRAGMA user_version=1");
                if (!ok || !m_database.commit()) { m_database.rollback(); failure = Failure::Unavailable; }
            }
        }
        if (failure == Failure::None) {
            if (!query.exec("SELECT owner FROM draft_owner") || !query.next() || query.value(0).toString() != owner || query.next())
                failure = Failure::Corrupt;
        }
        if (failure == Failure::None
            && !query.exec("SELECT uuid, stream, topic, updated_at, phase, consumed, payload FROM draft_records LIMIT 0"))
            failure = Failure::Corrupt;
        if (failure == Failure::None && !query.exec("CREATE INDEX IF NOT EXISTS draft_conversation_order "
                "ON draft_records (stream, topic, updated_at DESC, uuid DESC)"))
            failure = Failure::Unavailable;
    }
    if (failure == Failure::None && !protectFiles()) failure = Failure::Unavailable;
    if (failure != Failure::None) close();
    return failure;
}

NativeDraftStore::Page NativeDraftStore::list(const QString &afterUuid, int limit)
{
    if (!available()) return {};
    if ((!afterUuid.isEmpty() && (!isUuid(afterUuid) || afterUuid != afterUuid.toLower())) || limit < 1 || limit > MaximumPageRows)
        return {Failure::InvalidRecord, {}, {}};
    QSqlQuery query(m_database);
    query.prepare("SELECT uuid, stream, topic, updated_at, phase, consumed, typeof(updated_at), typeof(phase), typeof(consumed) "
        "FROM draft_records WHERE uuid > ? ORDER BY uuid LIMIT ?");
    // A default QString is NULL to QSQLITE; uuid > NULL admits no rows.
    query.addBindValue(afterUuid.isNull() ? QStringLiteral("") : afterUuid); query.addBindValue(limit + 1);
    if (!query.exec()) return {};
    Page result{Failure::None, {}, {}};
    while (query.next()) {
        auto row = summary(query);
        if (!row) return {Failure::Corrupt, {}, {}};
        if (result.rows.size() == limit) { result.next = result.rows.last().uuid; break; }
        result.rows.append(std::move(*row));
    }
    return query.lastError().isValid() ? Page{Failure::Corrupt, {}, {}} : result;
}

NativeDraftStore::Read NativeDraftStore::read(const QString &uuid)
{
    if (!available()) return {};
    if (!isUuid(uuid) || uuid != uuid.toLower()) return {Failure::InvalidRecord, {}};
    QSqlQuery query(m_database);
    query.prepare("SELECT substr(payload, 1, ?), stream, topic, updated_at, phase, consumed, typeof(payload), "
        "typeof(updated_at), typeof(phase), typeof(consumed) FROM draft_records WHERE uuid = ?");
    query.addBindValue(MaximumRecordBytes + 1); query.addBindValue(uuid);
    if (!query.exec()) return {};
    if (!query.next()) return {query.lastError().isValid() ? Failure::Corrupt : Failure::None, {}};
    if (query.value(6).toString() != "blob" || query.value(7).toString() != "integer" || query.value(8).toString() != "integer"
        || query.value(9).toString() != "integer" || (query.value(5).toInt() != 0 && query.value(5).toInt() != 1))
        return {Failure::Corrupt, {}};
    auto record = decode(query.value(0).toByteArray());
    if (!record || record->uuid != uuid || record->stream != query.value(1).toString() || record->topic != query.value(2).toString()
        || record->updatedAt != query.value(3).toLongLong() || static_cast<int>(record->phase) != query.value(4).toInt()
        || record->consumed != query.value(5).toBool()) return {Failure::Corrupt, {}};
    return {Failure::None, std::move(record)};
}

NativeDraftStore::Failure NativeDraftStore::put(const DraftRecord &record) { return apply({record}, {}); }
NativeDraftStore::Failure NativeDraftStore::remove(const QString &uuid) { return apply({}, {uuid}); }

NativeDraftStore::Read NativeDraftStore::latest(const QString &stream, const QString &topic)
{
    if (!available()) return {};
    if (!isUuid(stream) || !isUuid(topic)) return {Failure::InvalidRecord, {}};
    QString id;
    {
        QSqlQuery query(m_database);
        query.setForwardOnly(true);
        query.prepare("SELECT uuid, stream, topic, updated_at, phase, consumed, typeof(updated_at), typeof(phase), typeof(consumed) "
            "FROM draft_records WHERE stream = ? AND topic = ? "
            "ORDER BY updated_at DESC, uuid DESC");
        query.addBindValue(stream); query.addBindValue(topic);
        if (!query.exec()) return {};
        while (query.next()) {
            const auto row = summary(query);
            if (!row) return {Failure::Corrupt, {}};
            if (!row->consumed && row->phase != DraftRecord::Phase::Deleting) { id = row->uuid; break; }
        }
        if (query.lastError().isValid()) return {Failure::Corrupt, {}};
    }
    return id.isEmpty() ? Read{Failure::None, {}} : read(id);
}

NativeDraftStore::Failure NativeDraftStore::apply(const QVector<DraftRecord> &writes, const QStringList &removals)
{
    if (!available()) return Failure::Unavailable;
    if (writes.size() + removals.size() > MaximumPageRows) return Failure::Capacity;
    QSet<QString> ids;
    QVector<QByteArray> payloads;
    qsizetype bytes = 0;
    for (const auto &r : writes) {
        if (!valid(r) || ids.contains(r.uuid)) return Failure::InvalidRecord;
        ids.insert(r.uuid);
        auto payload = encode(r);
        bytes += payload.size();
        if (bytes > MaximumBatchBytes) return Failure::Capacity;
        payloads.append(std::move(payload));
    }
    for (const auto &id : removals) {
        if (!isUuid(id) || id != id.toLower() || ids.contains(id)) return Failure::InvalidRecord;
        ids.insert(id);
    }
    if (!m_database.transaction()) return Failure::Unavailable;
    bool ok = true;
    {
        QSqlQuery query(m_database);
        query.prepare("INSERT INTO draft_records VALUES (?, ?, ?, ?, ?, ?, ?) ON CONFLICT(uuid) DO UPDATE SET "
            "stream=excluded.stream, topic=excluded.topic, updated_at=excluded.updated_at, phase=excluded.phase, "
            "consumed=excluded.consumed, payload=excluded.payload");
        for (qsizetype i = 0; i < writes.size(); ++i) {
            const auto &r = writes[i];
            query.bindValue(0, r.uuid); query.bindValue(1, r.stream); query.bindValue(2, r.topic);
            query.bindValue(3, r.updatedAt); query.bindValue(4, static_cast<int>(r.phase)); query.bindValue(5, r.consumed);
            query.bindValue(6, payloads[i]);
            if (!query.exec()) { ok = false; break; }
        }
        if (ok) {
            query.prepare("DELETE FROM draft_records WHERE uuid = ?");
            for (const auto &id : removals) {
                query.bindValue(0, id);
                if (!query.exec()) { ok = false; break; }
            }
        }
    }
    if (!ok || !m_database.commit()) { m_database.rollback(); return Failure::Unavailable; }
    return protectFiles() ? Failure::None : Failure::Unavailable;
}

} // namespace workspace
