#include "native-cache.h"
#include "domain.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>
#include <atomic>
#include <limits>

namespace workspace {
namespace {
constexpr auto PrivateFile = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
constexpr auto PrivateDirectory = PrivateFile | QFileDevice::ExeOwner;
constexpr qint64 PageSize = 4096;
constexpr qint64 SharedMemoryReserve = 32768;
std::atomic<quint64> LeaseSequence{0};
const QString EntryBytesSql = "(CASE typeof(payload) WHEN 'blob' THEN length(payload) ELSE length(CAST(payload AS BLOB)) END"
    "+CASE typeof(marker) WHEN 'blob' THEN length(marker) ELSE length(CAST(marker AS BLOB)) END"
    "+CASE typeof(next_marker) WHEN 'blob' THEN length(next_marker) ELSE length(CAST(next_marker AS BLOB)) END+256)";

bool validMarker(const QByteArray &marker)
{
    if (marker.size() > 4096 || QString::fromUtf8(marker).toUtf8() != marker) return false;
    for (const auto c : marker) if (static_cast<uchar>(c) < 0x20 || c == 0x7f) return false;
    return true;
}

QUrl origin(QUrl server, bool allowLoopback)
{
    const bool testHttp = allowLoopback && server.scheme() == "http"
        && (server.host() == "127.0.0.1" || server.host() == "::1" || server.host() == "localhost");
    if (!server.isValid() || server.isRelative() || (server.scheme() != "https" && !testHttp)
        || server.host().isEmpty() || server.authority().contains('@') || server.hasQuery()
        || server.hasFragment() || (!server.path().isEmpty() && server.path() != "/")) return {};
    server.setHost(server.host().toLower());
    server.setPath({});
    if ((server.scheme() == "https" && server.port() == 443)
        || (testHttp && server.port() == 80)) server.setPort(-1);
    return server;
}

QJsonObject select(const QJsonObject &input, const QStringList &fields)
{
    QJsonObject result;
    for (const auto &field : fields) if (input.contains(field)) result.insert(field, input.value(field));
    return result;
}

bool strings(const QJsonObject &object, const QStringList &fields)
{
    for (const auto &field : fields)
        if (object.contains(field) && !object.value(field).isString()) return false;
    return true;
}

bool booleans(const QJsonObject &object, const QStringList &fields)
{
    for (const auto &field : fields)
        if (object.contains(field) && !object.value(field).isBool()) return false;
    return true;
}
} // namespace

NativeCache::NativeCache(QString directory, QObject *parent)
    : NativeCache(std::move(directory), Policy{}, false, {}, parent) {}

NativeCache::NativeCache(QString directory, Policy policy, bool allowLoopbackForTests,
                         std::function<qint64()> clock, QObject *parent)
    : QObject(parent), m_directory(std::move(directory)), m_policy(policy),
      m_allowLoopback(allowLoopbackForTests), m_clock(std::move(clock))
{
    if (m_directory.isEmpty()) m_directory = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
        + "/messenger-v1";
    if (!m_clock) m_clock = [] { return QDateTime::currentMSecsSinceEpoch(); };
    m_policy.maxEntries = std::max(m_policy.maxEntries, 0);
    m_policy.maxBytes = std::clamp<qint64>(m_policy.maxBytes, 1024, MaximumDiskLimitBytes);
    m_policy.maxEntryBytes = std::clamp<qint64>(m_policy.maxEntryBytes, 512, std::min<qint64>(m_policy.maxBytes, 8 * 1024 * 1024));
    m_policy.ttlMs = std::clamp<qint64>(m_policy.ttlMs, 1, 24 * 60 * 60 * 1000);
}

NativeCache::~NativeCache() { close(); }

qint64 NativeCache::diskUsageBytes() const
{
    qint64 bytes = 0;
    const auto path = QDir(m_directory).filePath("cache.sqlite3");
    for (const auto &suffix : {QString{}, QString("-wal"), QString("-shm")})
        bytes += QFileInfo(path + suffix).size();
    return bytes;
}

void NativeCache::setError(const QString &message)
{
    if (m_error == message) return;
    m_error = message;
    emit errorChanged();
}

qint64 NativeCache::databaseLimitBytes() const
{
    // Keep room for one bounded write plus SQLite indices/WAL frames. The
    // database grows on demand; max_page_count never allocates these pages.
    const auto journal = std::min<qint64>(32 * 1024 * 1024, m_diskLimitBytes / 4);
    return (m_diskLimitBytes - journal - SharedMemoryReserve) / PageSize * PageSize;
}

qint64 NativeCache::entryLimitBytes() const
{
    const auto journal = std::min<qint64>(32 * 1024 * 1024, m_diskLimitBytes / 4);
    return std::min(m_policy.maxEntryBytes, journal / 4);
}

bool NativeCache::setDiskLimitBytes(qint64 bytes)
{
    if (shouldCancel()) return fail();
    if (bytes < MinimumDiskLimitBytes || bytes > MaximumDiskLimitBytes) {
        setError(tr("The cache limit must be between 1 MiB and 1000 GB."));
        return false;
    }
    m_diskLimitBytes = bytes;
    if (available() && !applyDiskLimit()) return fail();
    setError({});
    return true;
}

void NativeCache::close()
{
    // Leases must also differ across replacement cache service objects. A late
    // callback can outlive its original service without crossing an owner ID.
    m_generation = ++LeaseSequence;
    m_fresh.clear(); m_owner.clear(); m_project.clear(); m_user.clear(); m_stats = {};
    m_totalEntries = m_totalBytes = 0; m_dataVersion = -1;
    const auto connection = m_connection;
    m_connection.clear();
    if (m_database.isValid()) m_database.close();
    m_database = {};
    if (!connection.isEmpty()) QSqlDatabase::removeDatabase(connection);
}

bool NativeCache::fail()
{
    // A failed cache is optional. Never replace authoritative network behavior
    // with a partially read database or a stale successful mutation.
    const bool cancelled = shouldCancel();
    close();
    setError(cancelled ? QString{} : tr("The local message cache is unavailable."));
    return false;
}

bool NativeCache::open(const QUrl &server, const QString &project, const QString &user)
{
    close();
    const auto canonical = origin(server, m_allowLoopback);
    if (shouldCancel()) return fail();
    if (canonical.isEmpty() || !isUuid(project) || !isUuid(user)) return fail();
    QFileInfo directory(m_directory);
    if (!directory.isAbsolute() || directory.isSymLink()
        || !QDir().mkpath(m_directory) || !QFile::setPermissions(m_directory, PrivateDirectory)) return fail();
    const auto path = QDir(m_directory).filePath("cache.sqlite3");
    for (const auto &suffix : {QString{}, QString("-wal"), QString("-shm")}) {
        const QFileInfo info(path + suffix);
        if (info.isSymLink() || (info.exists() && (!info.isFile()
            || info.size() > MaximumDiskLimitBytes || !QFile::setPermissions(info.filePath(), PrivateFile)))) return fail();
    }
    QFile file(path);
    if (!file.exists()) {
        if (!file.open(QIODevice::ReadWrite | QIODevice::NewOnly, PrivateFile)) return fail();
        file.close();
    }
    m_connection = "workspace-cache-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_database = QSqlDatabase::addDatabase("QSQLITE", m_connection);
    m_database.setDatabaseName(path);
    m_database.setConnectOptions("QSQLITE_BUSY_TIMEOUT=100");
    if (!m_database.open()) return fail();
    bool okay = true;
    {
        QSqlQuery query(m_database);
        int version = -1;
        if (query.exec("PRAGMA user_version") && query.next()) version = query.value(0).toInt();
        const bool createSchema = version == 0;
        const bool migrate = version == 1;
        if (createSchema) {
            // Inspect identity before any pragma that changes an existing file.
            okay = query.exec("SELECT COUNT(*) FROM sqlite_master WHERE name NOT LIKE 'sqlite_%'")
                && query.next() && query.value(0).toInt() == 0;
        } else {
            okay = (version >= 1 && version <= 4) && query.exec("PRAGMA application_id") && query.next()
                && query.value(0).toInt() == 0x574e4331; // WNC1
        }
        query.finish();
        if (okay && !createSchema) {
            // Existing v1 files were created with INCREMENTAL before their DDL.
            // FULL can switch to INCREMENTAL using its existing pointer map;
            // NONE requires a full VACUUM, so reject it before any migration or
            // write pragmas rather than publishing an unshrinkable cache.
            okay = query.exec("PRAGMA auto_vacuum") && query.next()
                && (query.value(0).toInt() == 1 || query.value(0).toInt() == 2);
            query.finish();
        }
        for (const auto *sql : {"PRAGMA page_size=4096", "PRAGMA auto_vacuum=INCREMENTAL",
                "PRAGMA cache_size=-2048", "PRAGMA temp_store=MEMORY", "PRAGMA foreign_keys=ON",
                "PRAGMA journal_mode=WAL", "PRAGMA synchronous=NORMAL", "PRAGMA wal_autocheckpoint=256",
                "PRAGMA journal_size_limit=1048576"}) {
            if (!okay) break;
            if (!query.exec(QString::fromLatin1(sql))) { okay = false; break; }
        }
        if (okay && createSchema) {
            query.finish();
            okay = m_database.transaction();
            // Keep large response BLOBs in rowid leaves. The separate compact
            // identity index must not read unrelated bodies on a cache miss.
            if (okay) okay = query.exec("CREATE TABLE entries (owner TEXT NOT NULL, key BLOB NOT NULL,"
                " kind INTEGER NOT NULL, stream TEXT NOT NULL, topic TEXT NOT NULL, marker BLOB NOT NULL,"
                " payload BLOB NOT NULL, next_marker BLOB NOT NULL, saved_at INTEGER NOT NULL,"
                " expires_at INTEGER NOT NULL, last_used INTEGER NOT NULL, size INTEGER NOT NULL,"
                " PRIMARY KEY(owner,key))")
                && query.exec("CREATE INDEX cache_routes ON entries(owner,kind,stream,topic)")
                && query.exec("CREATE INDEX cache_lru ON entries(last_used)")
                && query.exec("PRAGMA application_id=1464746801")
                && query.exec("PRAGMA user_version=1");
            if (okay) okay = m_database.commit();
            else m_database.rollback();
        }
        if (okay && (createSchema || migrate)) {
            query.finish();
            okay = m_database.transaction();
            if (okay) okay = query.exec("CREATE TABLE history_rows (owner TEXT NOT NULL,key BLOB NOT NULL,"
                "message_uuid TEXT NOT NULL,PRIMARY KEY(owner,key,message_uuid),"
                "FOREIGN KEY(owner,key) REFERENCES entries(owner,key) ON DELETE CASCADE) WITHOUT ROWID")
                && query.exec("CREATE INDEX cache_message_lookup ON history_rows(owner,message_uuid,key)");
            if (okay && migrate) {
                QSqlQuery pages(m_database);
                okay = pages.exec("SELECT owner,key,payload FROM entries WHERE kind=4");
                while (okay && !shouldCancel() && pages.next()) {
                    const auto bytes = pages.value(2).toByteArray();
                    const auto doc = bytes.size() <= m_policy.maxEntryBytes ? QJsonDocument::fromJson(bytes) : QJsonDocument{};
                    okay = doc.isArray() && indexHistory(pages.value(0).toString(), pages.value(1).toByteArray(), doc.array());
                }
            }
            if (okay) okay = !shouldCancel() && query.exec("PRAGMA user_version=2");
            query.finish();
            if (okay) okay = m_database.commit();
            else m_database.rollback();
        }
        // v3 adds aggregate History keys with an empty exact topic. Older
        // clients must reject these semantics instead of leaving them fresh
        // during topic-scoped invalidation. Existing raw pages/indexes survive.
        if (okay && version < 3) okay = !shouldCancel() && query.exec("PRAGMA user_version=3");
        // v4 adds owner+UUID bodies. Old readers cannot project/invalidate those
        // semantics correctly and must reject the file. No old payload rewrite.
        if (okay && version < 4) okay = !shouldCancel() && query.exec("PRAGMA user_version=4");
        if (okay) okay = query.exec("PRAGMA page_size") && query.next() && query.value(0).toInt() == 4096;
        if (okay) okay = query.exec("SELECT COALESCE(MAX(last_used),0) FROM entries") && query.next();
        if (okay) {
            m_accessSequence = query.value(0).toLongLong();
            okay = m_accessSequence >= 0 && m_accessSequence < std::numeric_limits<qint64>::max() - 1000000;
        }
    }
    if (!okay) return fail();
    const auto identity = QJsonDocument(QJsonArray{"workspace-cache-v1", canonical.toString(QUrl::FullyEncoded),
        project.toLower(), user.toLower()}).toJson(QJsonDocument::Compact);
    m_owner = QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
    m_project = project.toLower();
    m_user = user.toLower();
    if (!applyDiskLimit()) return fail();
    setError({});
    return true;
}

bool NativeCache::accepts(const Lease &lease) const
{
    return available() && !lease.owner.isEmpty() && lease.owner == m_owner && lease.generation == m_generation;
}

QByteArray NativeCache::keyHash(const Key &key) const
{
    const auto kind = static_cast<int>(key.kind);
    if (kind < 1 || kind > 5 || !validMarker(key.marker)) return {};
    if (key.kind == Kind::MessageBody) {
        const auto uuid = QString::fromUtf8(key.marker);
        if (!isUuid(uuid) || (!key.stream.isEmpty() && !isUuid(key.stream))
            || (!key.topic.isEmpty() && !isUuid(key.topic))) return {};
        // Actual placement remains route metadata, not part of body identity.
        return QCryptographicHash::hash(QJsonDocument(QJsonArray{kind, uuid.toLower()})
            .toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
    }
    if ((key.kind == Kind::Streams || key.kind == Kind::Users) && (!key.stream.isEmpty() || !key.topic.isEmpty())) return {};
    if ((key.kind == Kind::Topics || key.kind == Kind::History) && !isUuid(key.stream)) return {};
    if (key.kind == Kind::Topics && !key.topic.isEmpty()) return {};
    if (key.kind == Kind::History && !key.topic.isEmpty() && !isUuid(key.topic)) return {};
    const auto data = QJsonDocument(QJsonArray{kind, key.stream.toLower(), key.topic.toLower(),
        QString::fromUtf8(key.marker)}).toJson(QJsonDocument::Compact);
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

std::optional<QJsonArray> NativeCache::sanitize(const Key &key, const QJsonArray &rows) const
{
    QJsonArray result;
    QSet<QString> ids;
    // A page is deliberately bounded before any per-row allocations.
    if (rows.size() > 10000 || (key.kind == Kind::MessageBody && rows.size() != 1)) return {};
    for (const auto &value : rows) {
        if (!value.isObject()) return {};
        const auto input = value.toObject();
        const auto uuid = input.value("uuid").toString();
        if (!isUuid(uuid) || ids.contains(uuid.toLower())) return {};
        ids.insert(uuid.toLower());
        QJsonObject row;
        switch (key.kind) {
        case Kind::Streams:
            if (!parseStream(input, m_project)
                || !strings(input, {"description", "role"})
                || !booleans(input, {"is_archived", "private"})) return {};
            if (input.contains("direct_user_uuid") && !input.value("direct_user_uuid").isNull()
                && !isUuid(input.value("direct_user_uuid").toString())) return {};
            row = select(input, {"uuid", "project_id", "name", "description", "direct_user_uuid", "role",
                "encryption", "is_archived", "private", "notification_mode", "unread_count", "active_unread_count", "passive_unread_count", "last_message_uuid", "updated_at"});
            break;
        case Kind::Users:
            if (!input.value("username").isString() || input.value("username").toString().isEmpty()
                || !strings(input, {"name", "display_name"})
                || (input.contains("project_id") && input.value("project_id").toString() != m_project)) return {};
            for (const auto *field : {"first_name", "last_name", "email"})
                if (input.contains(field) && !input.value(field).isNull() && !input.value(field).isString()) return {};
            row = select(input, {"uuid", "project_id", "username", "name", "first_name", "last_name", "display_name", "email"});
            if (const auto avatar = imageAvatarReference(input.value("avatar")); !avatar.isEmpty()) row.insert("avatar", avatar);
            break;
        case Kind::Topics:
            if (!parseTopic(input, m_project, key.stream)
                || !booleans(input, {"is_done", "is_default"})) return {};
            row = select(input, {"uuid", "project_id", "stream_uuid", "name", "is_done", "is_default",
                "unread_count", "active_unread_count", "passive_unread_count", "last_message_uuid", "notification_mode", "updated_at"});
            break;
        case Kind::MessageBody:
        case Kind::History: {
            const auto message = parseMessage(input, m_project);
            if (!message || (!key.stream.isEmpty() && message->stream != key.stream)
                || (!key.topic.isEmpty() && message->topic != key.topic)
                || !booleans(input, {"read", "is_own", "starred"})) return {};
            if (key.kind == Kind::MessageBody && (uuid.compare(QString::fromUtf8(key.marker), Qt::CaseInsensitive)
                || input.value("user_uuid").toString().compare(m_user, Qt::CaseInsensitive))) return {};
            row = select(input, {"uuid", "project_id", "user_uuid", "stream_uuid", "topic_uuid", "author_uuid", "created_at",
                "read", "is_own", "starred", "reactions", "reaction_users"});
            const auto payload = input.value("payload").toObject();
            const auto kind = payload.value("kind").toString();
            QStringList fields{"kind"};
            if (kind == "markdown") fields << "content";
            else if (kind == "e2ee") fields << "key_uuid" << "content";
            else if (kind == "key_request") fields << "key_uuid" << "device_id" << "device_name" << "public_key";
            else if (kind == "key_grant") fields << "key_uuid" << "request_message_uuid" << "sender_device_id" << "recipient_device_id" << "content";
            else if (kind == "key_announced") fields << "key_uuid";
            else if (kind == "key_reject") fields << "key_uuid" << "request_message_uuid" << "sender_device_id" << "recipient_device_id";
            if (!strings(payload, fields)) return {};
            row.insert("payload", select(payload, fields));
            break;
        }
        }
        result.append(row);
    }
    return result;
}

std::optional<NativeCache::Entry> NativeCache::get(const Lease &lease, const Key &key)
{
    if (shouldCancel()) { fail(); return {}; }
    const auto hash = keyHash(key);
    if (!accepts(lease) || hash.isEmpty()) { ++m_stats.misses; return {}; }
    if (!refreshTotals()) { fail(); return {}; }
    Entry result;
    bool found = false, okay = true;
    qint64 expires = 0;
    {
        QSqlQuery query(m_database);
        // Bound the driver result before copying an untrusted local BLOB.
        query.prepare("SELECT CASE WHEN LENGTH(payload)<=? THEN payload END,"
            "next_marker,saved_at,expires_at,kind,stream,topic FROM entries WHERE owner=? AND key=?");
        query.addBindValue(m_policy.maxEntryBytes);
        query.addBindValue(m_owner); query.addBindValue(hash);
        okay = query.exec();
        if (okay && query.next()) {
            const auto bytes = query.value(0).toByteArray();
            const auto marker = query.value(1).toByteArray();
            const auto document = bytes.size() <= m_policy.maxEntryBytes ? QJsonDocument::fromJson(bytes) : QJsonDocument{};
            const auto rows = document.isArray() ? sanitize(key, document.array()) : std::optional<QJsonArray>{};
            // Read back only data that still meets the typed contract. A private
            // cache remains untrusted input after a crash or local modification.
            found = rows.has_value() && validMarker(marker)
                && bytes.size() + marker.size() + key.marker.size() + 256 <= m_policy.maxEntryBytes;
            if (found && key.kind == Kind::MessageBody) {
                const auto row = rows->first().toObject();
                found = marker.isEmpty() && query.value(4).toInt() == int(Kind::MessageBody)
                    && query.value(5).toString() == row.value("stream_uuid").toString().toLower()
                    && query.value(6).toString() == row.value("topic_uuid").toString().toLower();
            }
            if (found) {
                result.rows = *rows; result.nextMarker = marker;
                result.savedAt = query.value(2).toLongLong(); expires = query.value(3).toLongLong();
            }
        }
    }
    if (!okay) { fail(); return {}; }
    if (!found) { ++m_stats.misses; return {}; }
    const auto now = m_clock();
    result.fresh = m_fresh.contains(hash) && result.savedAt >= 0 && result.savedAt <= now && expires > now
        && result.savedAt <= std::numeric_limits<qint64>::max() - m_policy.ttlMs
        && expires <= result.savedAt + m_policy.ttlMs;
    {
        QSqlQuery query(m_database);
        query.prepare("UPDATE entries SET last_used=? WHERE owner=? AND key=?");
        query.addBindValue(++m_accessSequence); query.addBindValue(m_owner); query.addBindValue(hash);
        okay = query.exec();
    }
    if (!okay) { fail(); return {}; }
    ++m_stats.hits;
    if (!result.fresh) ++m_stats.staleHits;
    return result;
}

bool NativeCache::put(const Lease &lease, const Key &key, const QJsonArray &rows, const QByteArray &nextMarker)
{
    const auto hash = keyHash(key);
    if (shouldCancel()) return fail();
    if (!accepts(lease) || hash.isEmpty() || !validMarker(nextMarker)
        || (key.kind == Kind::MessageBody && !nextMarker.isEmpty())) return false;
    const auto sanitized = sanitize(key, rows);
    if (!sanitized) return false;
    const auto payload = QJsonDocument(*sanitized).toJson(QJsonDocument::Compact);
    const auto size = payload.size() + nextMarker.size() + key.marker.size() + 256;
    if (size > entryLimitBytes() || size > m_policy.maxBytes) return false;
    const auto now = m_clock();
    if (now < 0 || now > std::numeric_limits<qint64>::max() - m_policy.ttlMs) return false;
    if (!trim(size)) return fail();
    bool existed = false; qint64 previousSize = 0;
    {
        QSqlQuery query(m_database); query.prepare("SELECT " + EntryBytesSql + " FROM entries WHERE owner=? AND key=?");
        query.addBindValue(m_owner); query.addBindValue(hash);
        if (!query.exec()) return false;
        if (query.next()) { existed = true; previousSize = query.value(0).toLongLong(); }
    }
    const auto body = key.kind == Kind::MessageBody ? sanitized->first().toObject() : QJsonObject{};
    const auto routeStream = body.isEmpty() ? key.stream.toLower() : body.value("stream_uuid").toString().toLower();
    const auto routeTopic = body.isEmpty() ? key.topic.toLower() : body.value("topic_uuid").toString().toLower();
    bool okay = m_database.transaction();
    if (okay) {
        QSqlQuery query(m_database);
        query.prepare("INSERT OR REPLACE INTO entries"
            "(owner,key,kind,stream,topic,marker,payload,next_marker,saved_at,expires_at,last_used,size)"
            " VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");
        for (const auto &value : QVariantList{m_owner, hash, static_cast<int>(key.kind),
                routeStream.isEmpty() ? QStringLiteral("") : routeStream,
                routeTopic.isEmpty() ? QStringLiteral("") : routeTopic,
                key.marker.isEmpty() ? QByteArray("") : key.marker, payload,
                nextMarker.isEmpty() ? QByteArray("") : nextMarker,
                now, now + m_policy.ttlMs, ++m_accessSequence, size}) query.addBindValue(value);
        okay = query.exec();
    }
    if (okay) { m_totalEntries += existed ? 0 : 1; m_totalBytes += size - previousSize; }
    if (okay && (key.kind == Kind::History || key.kind == Kind::MessageBody)) okay = indexHistory(m_owner, hash, *sanitized);
    if (okay) okay = trim();
    if (okay) okay = !shouldCancel() && m_database.commit();
    if (!okay) { m_database.rollback(); return fail(); }
    if (!m_fresh.contains(hash) && m_fresh.size() >= MaximumFreshPages) m_fresh.erase(m_fresh.begin());
    // Retain route metadata, not markers/payloads, for the bounded warm set.
    m_fresh.insert(hash, {key.kind, routeStream, routeTopic, {}});
    if (!checkpoint()) return fail();
    return true;
}

std::optional<NativeCache::MessageBody> NativeCache::getMessageBody(const Lease &lease, const QString &uuid)
{
    if (!accepts(lease) || !isUuid(uuid)) return {};
    const Key body{Kind::MessageBody, {}, {}, uuid.toLower().toUtf8()};
    if (const auto saved = get(lease, body))
        return MessageBody{saved->rows.first().toObject(), saved->savedAt};
    if (!accepts(lease)) return {};
    QList<Key> candidates;
    {
        QSqlQuery query(m_database);
        query.prepare("SELECT e.stream,e.topic,e.marker FROM history_rows h INDEXED BY cache_message_lookup"
            " JOIN entries e ON e.owner=h.owner AND e.key=h.key"
            " WHERE h.owner=? AND h.message_uuid=? AND e.kind=4 ORDER BY h.key LIMIT 4");
        query.addBindValue(m_owner); query.addBindValue(uuid.toLower());
        if (!query.exec()) { fail(); return {}; }
        while (query.next()) candidates.append({Kind::History, query.value(0).toString(),
            query.value(1).toString(), query.value(2).toByteArray()});
    }
    std::optional<MessageBody> newest;
    for (const auto &key : candidates) {
        if (shouldCancel()) { fail(); return {}; }
        const auto page = get(lease, key);
        if (!page) { if (!accepts(lease)) return {}; else continue; }
        for (const auto &value : page->rows) {
            if (value.toObject().value("uuid").toString().compare(uuid, Qt::CaseInsensitive)) continue;
            const auto owned = sanitize(body, {value});
            if (owned && (!newest || page->savedAt > newest->savedAt))
                newest = MessageBody{owned->first().toObject(), page->savedAt};
            break;
        }
    }
    return newest;
}

bool NativeCache::putMessageBody(const Lease &lease, const QJsonObject &canonical)
{
    return put(lease, {Kind::MessageBody, canonical.value("stream_uuid").toString(),
        canonical.value("topic_uuid").toString(), canonical.value("uuid").toString().toLower().toUtf8()}, {canonical});
}

bool NativeCache::removeMessageBodyAndHistory(const Lease &lease, const QString &uuid)
{
    if (!accepts(lease) || !isUuid(uuid)) return false;
    const auto hash = keyHash({Kind::MessageBody, {}, {}, uuid.toLower().toUtf8()});
    if (!eraseMatching("owner=? AND key=?", {m_owner, hash})) return fail();
    while (true) {
        if (shouldCancel()) return fail();
        QList<QByteArray> keys;
        {
            QSqlQuery query(m_database);
            query.prepare("SELECT key FROM history_rows INDEXED BY cache_message_lookup"
                " WHERE owner=? AND message_uuid=? LIMIT 64");
            query.addBindValue(m_owner); query.addBindValue(uuid.toLower());
            if (!query.exec()) return fail();
            while (query.next()) keys.append(query.value(0).toByteArray());
        }
        if (keys.isEmpty()) return true;
        for (const auto &key : keys)
            if (!eraseMatching("owner=? AND key=?", {m_owner, key})) return fail();
    }
}

bool NativeCache::indexHistory(const QString &owner, const QByteArray &key, const QJsonArray &rows)
{
    if (rows.size() > 10000) return false;
    QSqlQuery query(m_database);
    query.prepare("INSERT OR IGNORE INTO history_rows(owner,key,message_uuid) VALUES(?,?,?)");
    for (const auto &value : rows) {
        if (shouldCancel()) return false;
        const auto id = value.toObject().value("uuid").toString();
        if (!isUuid(id)) return false;
        query.bindValue(0, owner); query.bindValue(1, key); query.bindValue(2, id.toLower());
        if (!query.exec()) return false;
    }
    return true;
}

bool NativeCache::refreshTotals(bool force) const
{
    if (shouldCancel()) return false;
    QSqlQuery query(m_database);
    if (!query.exec("PRAGMA data_version") || !query.next()) return false;
    const auto version = query.value(0).toLongLong(); query.finish();
    if (!force && version == m_dataVersion) return true;
    if (!query.exec("SELECT COUNT(*),COALESCE(SUM(" + EntryBytesSql + "),0),COALESCE(MAX(last_used),0) FROM entries") || !query.next()) return false;
    const auto sequence = query.value(2).toLongLong();
    if (sequence < 0 || sequence > std::numeric_limits<qint64>::max() - 1000000) return false;
    if (m_dataVersion >= 0 && m_dataVersion != version) m_fresh.clear();
    m_totalEntries = query.value(0).toLongLong(); m_totalBytes = query.value(1).toLongLong(); m_dataVersion = version;
    m_accessSequence = std::max(m_accessSequence, sequence);
    ++m_stats.fullScans;
    return true;
}

bool NativeCache::trim(qint64 incomingBytes)
{
    if (!refreshTotals()) return false;
    QSqlQuery query(m_database);
    auto &count = m_totalEntries; auto &bytes = m_totalBytes;
    const auto logicalLimit = std::min(m_policy.maxBytes, databaseLimitBytes());
    const auto pageLimit = databaseLimitBytes() / PageSize;
    const auto reservePages = incomingBytes > 0 ? (2 * incomingBytes + 65536 + PageSize - 1) / PageSize : 0;
    const auto usedPages = [&]() -> qint64 {
        qint64 total = -1, free = -1;
        if (query.exec("PRAGMA page_count") && query.next()) total = query.value(0).toLongLong();
        query.finish();
        if (query.exec("PRAGMA freelist_count") && query.next()) free = query.value(0).toLongLong();
        query.finish(); return total >= free && free >= 0 ? total - free : -1;
    };
    auto used = usedPages();
    if (used < 0) return false;
    while ((m_policy.maxEntries > 0 && count > m_policy.maxEntries) || bytes > logicalLimit || used + reservePages > pageLimit) {
        if (shouldCancel()) return false;
        if (!query.exec("SELECT owner,key," + EntryBytesSql + " FROM entries ORDER BY last_used ASC LIMIT 64")) return false;
        struct Removal { QString owner; QByteArray key; qint64 size; };
        QList<Removal> removals;
        while (query.next()) removals.append({query.value(0).toString(), query.value(1).toByteArray(), query.value(2).toLongLong()});
        query.finish();
        if (removals.isEmpty()) return false;
        for (const auto &item : removals) {
            if (shouldCancel()) return false;
            if ((m_policy.maxEntries == 0 || count <= m_policy.maxEntries) && bytes <= logicalLimit && used + reservePages <= pageLimit) break;
            query.prepare("DELETE FROM entries WHERE owner=? AND key=?");
            query.addBindValue(item.owner); query.addBindValue(item.key);
            if (!query.exec()) return false;
            query.finish();
            if (item.owner == m_owner) m_fresh.remove(item.key);
            --count; bytes -= item.size;
            // Only the small-budget/physical-pressure path needs page probes per
            // eviction. Logical LRU limits otherwise use the bounded row batch.
            if (used + reservePages > pageLimit) { used = usedPages(); if (used < 0) return false; }
        }
    }
    return true;
}

bool NativeCache::checkpoint(bool enforceLimit)
{
    if (shouldCancel()) return false;
    m_stats.sampledPeakDiskBytes = std::max(m_stats.sampledPeakDiskBytes, diskUsageBytes());
    bool okay;
    {
        QSqlQuery query(m_database);
        okay = query.exec("PRAGMA wal_checkpoint(TRUNCATE)") && query.next() && query.value(0).toInt() == 0;
    }
    if (okay) ++m_stats.checkpoints;
    const auto path = QDir(m_directory).filePath("cache.sqlite3");
    for (const auto &suffix : {QString{}, QString("-wal"), QString("-shm")}) {
        const QFileInfo info(path + suffix);
        if (info.exists()) okay = okay && info.size() <= MaximumDiskLimitBytes && QFile::setPermissions(info.filePath(), PrivateFile);
    }
    return okay && (!enforceLimit || diskUsageBytes() <= m_diskLimitBytes);
}

bool NativeCache::applyDiskLimit()
{
    // A reduction starts with the old file size; it is accepted only after LRU
    // eviction, checkpoint and incremental vacuum reclaim that old allocation.
    if (!checkpoint(false)) return false;
    // Compact each bounded LRU DELETE before its free pages accumulate into a
    // whole-cache freelist. A pre-existing large freelist stays on the cancellable
    // incremental path below; switching FULL could compact that allocation in
    // one opaque commit. Normal writes retain their original incremental mode.
    bool compactingDeletes = false;
    if (QFileInfo(QDir(m_directory).filePath("cache.sqlite3")).size() > databaseLimitBytes()) {
        QSqlQuery mode(m_database);
        if (!mode.exec("PRAGMA freelist_count") || !mode.next()) return false;
        // Cap the first automatic compaction at one permitted response worth of free pages.
        compactingDeletes = mode.value(0).toLongLong() * PageSize <= entryLimitBytes();
        mode.finish();
        if (compactingDeletes && !mode.exec("PRAGMA auto_vacuum=FULL")) return false;
    }
    if (!trim()) return false;
    QSqlQuery query(m_database);
    if (compactingDeletes && !query.exec("PRAGMA auto_vacuum=INCREMENTAL")) return false;
    query.finish();
    qint64 previous = std::numeric_limits<qint64>::max();
    while (QFileInfo(QDir(m_directory).filePath("cache.sqlite3")).size() > databaseLimitBytes()) {
        if (shouldCancel()) return false;
        const auto size = QFileInfo(QDir(m_directory).filePath("cache.sqlite3")).size();
        if (size >= previous) return false;
        previous = size;
        const auto excessPages = (size - databaseLimitBytes() + PageSize - 1) / PageSize;
        const auto pages = std::max<qint64>(1, std::min({qint64(256), m_diskLimitBytes / 8 / PageSize, excessPages}));
        // SQLite yields zero-column rows while vacuuming. QSQLITE treats these
        // as non-select results, so next() cannot drain a multi-page PRAGMA.
        // Explicit single-page steps in one bounded transaction avoid a commit
        // and checkpoint for every 4KiB page, without borrowing native handles.
        bool okay = m_database.transaction();
        for (qint64 i = 0; okay && i < pages; ++i) {
            okay = !shouldCancel() && query.exec("PRAGMA incremental_vacuum(1)");
            while (okay && query.next()) {}
            okay = okay && !query.lastError().isValid();
            query.finish();
        }
        if (okay) okay = !shouldCancel() && m_database.commit();
        if (!okay) { m_database.rollback(); return false; }
        if (!checkpoint(false)) return false;
    }
    if (!query.exec("PRAGMA max_page_count=" + QString::number(databaseLimitBytes() / PageSize))
        || !query.next() || query.value(0).toLongLong() > databaseLimitBytes() / PageSize) return false;
    query.finish();
    const auto reserve = std::min<qint64>(32 * 1024 * 1024, m_diskLimitBytes / 4);
    // Hits update LRU metadata too. Their automatic checkpoints must respect a
    // small configured budget without checkpointing after every single read.
    const auto checkpointPages = std::max<qint64>(1, std::min<qint64>(256, reserve / (4 * PageSize)));
    if (!query.exec("PRAGMA wal_autocheckpoint=" + QString::number(checkpointPages))) return false;
    query.finish();
    if (!query.exec("PRAGMA journal_size_limit=" + QString::number(std::min<qint64>(1048576, reserve / 2)))) return false;
    query.finish();
    return checkpoint();
}

bool NativeCache::eraseMatching(const QString &where, const QVariantList &bindings)
{
    if (!refreshTotals()) return false;
    while (true) {
        if (shouldCancel()) return false;
        struct Removal { QString owner; QByteArray key; qint64 size; };
        QList<Removal> removals;
        {
            QSqlQuery query(m_database);
            query.prepare("SELECT owner,key," + EntryBytesSql + " FROM entries WHERE " + where + " LIMIT 64");
            for (const auto &value : bindings) query.addBindValue(value);
            if (!query.exec()) return false;
            while (query.next()) removals.append({query.value(0).toString(), query.value(1).toByteArray(), query.value(2).toLongLong()});
        }
        if (removals.isEmpty()) return true;
        for (const auto &item : removals) {
            if (shouldCancel()) return false;
            bool okay; qint64 removed = 0;
            {
                QSqlQuery query(m_database);
                query.prepare("DELETE FROM entries WHERE owner=? AND key=?");
                query.addBindValue(item.owner); query.addBindValue(item.key);
                okay = query.exec(); removed = query.numRowsAffected();
            }
            if (!okay) return false;
            if (removed > 0) { --m_totalEntries; m_totalBytes -= item.size; }
            if (item.owner == m_owner) m_fresh.remove(item.key);
            // No unbounded DELETE transaction or retained SQL reader may grow
            // the journal in proportion to a whole owner's 10GB cache.
            if (!checkpoint()) return false;
        }
        if (!refreshTotals()) return false;
    }
}

bool NativeCache::matchesScope(const Key &key, Kind kind, const QString &stream, const QString &topic)
{
    return (key.kind == kind || (kind == Kind::History && key.kind == Kind::MessageBody))
        && (stream.isEmpty() || key.stream.compare(stream, Qt::CaseInsensitive) == 0)
        && (topic.isEmpty() || key.topic.compare(topic, Qt::CaseInsensitive) == 0
            || (kind == Kind::History && key.topic.isEmpty()));
}

bool NativeCache::invalidate(const Lease &lease, Kind kind, const QString &stream, const QString &topic)
{
    if (!accepts(lease) || static_cast<int>(kind) < 1 || static_cast<int>(kind) > 5
        || (!stream.isEmpty() && !isUuid(stream)) || (!topic.isEmpty() && !isUuid(topic))) return false;
    QString where = kind == Kind::History ? "owner=? AND kind IN (4,5)" : "owner=? AND kind=?";
    QVariantList bindings{m_owner};
    if (kind != Kind::History) bindings.append(static_cast<int>(kind));
    if (!stream.isEmpty()) { where += " AND stream=?"; bindings.append(stream.toLower()); }
    if (!topic.isEmpty()) {
        where += kind == Kind::History ? " AND (topic=? OR topic='')" : " AND topic=?";
        bindings.append(topic.toLower());
    }
    return eraseMatching(where, bindings) || fail();
}

void NativeCache::markStale(const Lease &lease)
{
    if (accepts(lease)) m_fresh.clear();
}

bool NativeCache::markStale(const Lease &lease, Kind kind, const QString &stream, const QString &topic)
{
    if (!accepts(lease) || static_cast<int>(kind) < 1 || static_cast<int>(kind) > 5
        || (!stream.isEmpty() && !isUuid(stream)) || (!topic.isEmpty() && !isUuid(topic))) return false;
    const auto streamId = stream.toLower(), topicId = topic.toLower();
    // Disk pages can be numerous, while current-process freshness is bounded.
    // Staling changes only those RAM facts; saved response bytes stay untouched.
    for (auto it = m_fresh.begin(); it != m_fresh.end();) {
        const auto &key = it.value();
        if (matchesScope(key, kind, streamId, topicId)) it = m_fresh.erase(it);
        else ++it;
    }
    return true;
}

bool NativeCache::markHistoryRead(const Lease &lease, const QString &stream, const QString &topic,
                                const QSet<QString> &confirmedIds)
{
    if (!accepts(lease) || (!stream.isEmpty() && !isUuid(stream))
        || (!topic.isEmpty() && !isUuid(topic)) || confirmedIds.isEmpty() || confirmedIds.size() > 100000) return false;
    QSet<QString> ids;
    for (const auto &id : confirmedIds) {
        if (shouldCancel()) return fail();
        if (!isUuid(id)) return false;
        ids.insert(id.toLower());
    }
    {
        QSqlQuery query(m_database);
        if (!query.exec("CREATE TEMP TABLE IF NOT EXISTS read_ids(uuid TEXT PRIMARY KEY) WITHOUT ROWID")
            || !query.exec("DELETE FROM read_ids")) return fail();
        query.prepare("INSERT INTO read_ids(uuid) VALUES(?)");
        for (const auto &id : ids) { query.bindValue(0, id); if (!query.exec()) return fail(); }
    }
    QByteArray cursor("");
    while (true) {
        if (shouldCancel()) return fail();
        QList<QByteArray> keys;
        {
            QSqlQuery query(m_database);
            QString sql = "SELECT h.key FROM read_ids r JOIN history_rows h INDEXED BY cache_message_lookup"
                " ON h.message_uuid=r.uuid AND h.owner=? JOIN entries e ON e.owner=h.owner AND e.key=h.key WHERE h.key>?";
            if (!stream.isEmpty()) sql += " AND e.stream=?";
            if (!topic.isEmpty()) sql += " AND (e.topic=? OR e.topic='')";
            sql += " ORDER BY h.key LIMIT 128";
            query.prepare(sql); query.addBindValue(m_owner); query.addBindValue(cursor);
            if (!stream.isEmpty()) query.addBindValue(stream.toLower());
            if (!topic.isEmpty()) query.addBindValue(topic.toLower());
            if (!query.exec()) return fail();
            while (query.next()) {
                const auto key = query.value(0).toByteArray();
                if (keys.isEmpty() || keys.last() != key) keys.append(key);
            }
        }
        if (keys.isEmpty()) break;
        cursor = keys.last();
        for (const auto &hash : keys) {
            if (shouldCancel()) return fail();
            Key key{Kind::History, {}, {}, {}}; QByteArray bytes, next;
            {
                QSqlQuery query(m_database);
                query.prepare("SELECT stream,topic,marker,payload,next_marker,kind FROM entries WHERE owner=? AND key=? AND kind IN (4,5)");
                query.addBindValue(m_owner); query.addBindValue(hash);
                if (!query.exec()) return fail();
                if (!query.next()) continue;
                key.stream = query.value(0).toString(); key.topic = query.value(1).toString(); key.marker = query.value(2).toByteArray();
                bytes = query.value(3).toByteArray(); next = query.value(4).toByteArray();
                key.kind = static_cast<Kind>(query.value(5).toInt());
            }
            m_fresh.remove(hash);
            if (keyHash(key) != hash || bytes.size() + key.marker.size() + next.size() + 256 > m_policy.maxEntryBytes
                || !validMarker(next)) return false;
            const auto document = QJsonDocument::fromJson(bytes);
            const auto sanitized = document.isArray() ? sanitize(key, document.array()) : std::optional<QJsonArray>{};
            if (!sanitized) return false;
            auto rows = *sanitized; bool changed = false;
            for (qsizetype i = 0; i < rows.size(); ++i) {
                auto row = rows.at(i).toObject();
                if (ids.contains(row.value("uuid").toString().toLower()) && !row.value("read").toBool()) {
                    row.insert("read", true); rows.replace(i, row); changed = true;
                }
            }
            if (!changed) continue;
            const auto payload = QJsonDocument(rows).toJson(QJsonDocument::Compact);
            const auto size = payload.size() + key.marker.size() + next.size() + 256;
            if (size > entryLimitBytes() || size > m_policy.maxBytes) return false;
            if (!trim(size)) return fail();
            bool okay = m_database.transaction();
            {
                QSqlQuery query(m_database);
                query.prepare("UPDATE entries SET payload=?,size=? WHERE owner=? AND key=?");
                query.addBindValue(payload); query.addBindValue(size); query.addBindValue(m_owner); query.addBindValue(hash);
                okay = okay && query.exec();
                if (okay && query.numRowsAffected() > 0)
                    m_totalBytes += payload.size() - bytes.size();
            }
            if (okay) okay = trim();
            if (okay) okay = !shouldCancel() && m_database.commit();
            if (!okay) { m_database.rollback(); return fail(); }
            if (!checkpoint()) return fail();
        }
    }
    QSqlQuery cleanup(m_database);
    return cleanup.exec("DELETE FROM read_ids");
}

bool NativeCache::projectMessage(const Lease &lease, const QJsonObject &canonical, ProjectionFields fields)
{
    const int mask = fields.toInt();
    if (!accepts(lease) || mask == 0 || (mask & ~15))
        return false;
    const auto message = parseMessage(canonical, m_project);
    if (!message || (fields.testFlag(Projection::Star) && !canonical.value("starred").isBool()) ||
        (fields.testFlag(Projection::Reactions) && !message->reactionsKnown))
        return false;
    const bool viewerMismatch = canonical.value("user_uuid").toString().compare(m_user, Qt::CaseInsensitive) != 0;
    if (viewerMismatch && (canonical.contains("user_uuid") || fields.testFlag(Projection::Payload))) {
        // A partial mask inherits the saved body's viewer/placement. Explicit
        // foreign viewers and full payload replacements still require identity.
        QSqlQuery query(m_database);
        query.prepare("SELECT 1 FROM entries WHERE owner=? AND key=? AND kind=5");
        query.addBindValue(m_owner);
        query.addBindValue(keyHash({Kind::MessageBody, {}, {}, message->uuid.toLower().toUtf8()}));
        if (!query.exec()) return fail();
        if (query.next()) return false; // Never update an owned body from an unverified viewer DTO.
    }
    const Key head{Kind::History, message->stream, message->topic, {}};
    const auto validated = sanitize(head, {canonical});
    if (!validated)
        return false;
    auto raw = validated->first().toObject();
    const bool incomingRead = raw.value("read").toBool();
    bool confirmedRead = incomingRead;
    // Even an oversized/declined projection revokes warm reuse, while retaining
    // the last valid raw response for offline display.
    markStale(lease, Kind::History, head.stream, head.topic);
    if (shouldCancel())
        return fail();
    if (!refreshTotals())
        return fail();
    const auto projectedRow = [&](QJsonObject row, Kind kind) {
        if (fields.testFlag(Projection::Payload)) {
            row["stream_uuid"] = raw.value("stream_uuid");
            row["topic_uuid"] = raw.value("topic_uuid");
            row["payload"] = raw.value("payload");
            if (kind == Kind::MessageBody)
                for (const auto *field : {"user_uuid", "author_uuid", "created_at", "is_own"})
                    if (raw.contains(field)) row[field] = raw.value(field);
        }
        if (fields.testFlag(Projection::Star))
            row["starred"] = raw.value("starred");
        if (fields.testFlag(Projection::Reactions)) {
            row["reactions"] = raw.value("reactions");
            if (raw.contains("reaction_users"))
                row["reaction_users"] = raw.value("reaction_users");
            else
                row.remove("reaction_users");
        }
        if (confirmedRead)
            row["read"] = true;
        return row;
    };
    std::optional<MessageBody> removedHistory;
    const auto update = [&](const QByteArray &hash, bool insert) -> bool {
        if (shouldCancel())
            return fail();
        Key key{Kind::History, {}, {}, {}};
        QByteArray bytes, next;
        qint64 savedAt = 0;
        {
            QSqlQuery query(m_database);
            // Do not materialize an arbitrarily modified local blob.
            query.prepare(
                "SELECT stream,topic,marker,CASE WHEN LENGTH(payload)<=? THEN payload END,next_marker,kind,saved_at"
                " FROM entries WHERE owner=? AND key=? AND kind IN (4,5)");
            query.addBindValue(m_policy.maxEntryBytes);
            query.addBindValue(m_owner);
            query.addBindValue(hash);
            if (!query.exec())
                return fail();
            if (!query.next())
                return true; // Deletion wins; never manufacture a page.
            key.stream = query.value(0).toString();
            key.topic = query.value(1).toString();
            key.marker = query.value(2).toByteArray();
            bytes = query.value(3).toByteArray();
            next = query.value(4).toByteArray();
            key.kind = static_cast<Kind>(query.value(5).toInt());
            savedAt = query.value(6).toLongLong();
        }
        if (keyHash(key) != hash || !validMarker(next) ||
            bytes.size() + key.marker.size() + next.size() + 256 > m_policy.maxEntryBytes)
            return false;
        const bool belongs = key.stream == head.stream && (key.topic.isEmpty() || key.topic == head.topic);
        if (!belongs && !fields.testFlag(Projection::Payload))
            return true; // Partial field responses do not own message placement.
        const auto document = QJsonDocument::fromJson(bytes);
        const auto saved = document.isArray() ? sanitize(key, document.array()) : std::optional<QJsonArray>{};
        if (!saved)
            return false;
        auto rows = *saved;
        bool found = false, changed = false;
        for (qsizetype i = 0; i < rows.size(); ++i) {
            auto row = rows.at(i).toObject();
            if (row.value("uuid").toString().compare(message->uuid, Qt::CaseInsensitive))
                continue;
            found = true;
            confirmedRead |= row.value("read").toBool();
            if (!belongs && key.kind == Kind::History) {
                if (!viewerMismatch && row.value("user_uuid").toString().compare(m_user, Qt::CaseInsensitive) == 0
                    && (!removedHistory || savedAt > removedHistory->savedAt))
                    removedHistory = MessageBody{row, savedAt};
                rows.removeAt(i--);
                changed = true;
                continue;
            }
            row = projectedRow(row, key.kind);
            changed = changed || row != rows.at(i).toObject();
            rows.replace(i, row);
        }
        bool fits = key.kind == Kind::History && belongs && key.marker.isEmpty();
        if (fits && !next.isEmpty()) {
            const auto older = [](const Message &a, const Message &b) {
                return a.createdMicros != b.createdMicros ? a.createdMicros < b.createdMicros : a.uuid < b.uuid;
            };
            std::optional<Message> oldest;
            for (const auto &value : rows) {
                const auto candidate = parseMessage(value.toObject(), m_project);
                if (!oldest || older(*candidate, *oldest)) oldest = candidate;
            }
            fits = oldest && !older(*message, *oldest);
        }
        if (!found && insert && fields.testFlag(Projection::Insert) && fits) {
            if (confirmedRead) raw["read"] = true;
            rows.append(raw);
            // Only a saved head can accept a new confirmed row. Keep its original
            // page size/cursor: this is a stale projection, not a fresh snapshot.
            QList<QJsonObject> sorted;
            for (const auto &value : rows)
                sorted.append(value.toObject());
            std::sort(sorted.begin(), sorted.end(), [&](const QJsonObject &a, const QJsonObject &b) {
                const auto left = parseMessage(a, m_project), right = parseMessage(b, m_project);
                return left->createdMicros != right->createdMicros
                           ? left->createdMicros > right->createdMicros
                           : left->uuid > right->uuid;
            });
            rows = {};
            for (qsizetype i = 0; i < std::min<qsizetype>(50, sorted.size()); ++i)
                rows.append(sorted.at(i));
            changed = true;
        }
        if (!changed)
            return true;
        if (key.kind == Kind::MessageBody && fields.testFlag(Projection::Payload)) {
            key.stream = raw.value("stream_uuid").toString().toLower();
            key.topic = raw.value("topic_uuid").toString().toLower();
        }
        m_fresh.remove(hash);
        const auto payload = QJsonDocument(rows).toJson(QJsonDocument::Compact);
        const auto size = payload.size() + key.marker.size() + next.size() + 256;
        if (size > entryLimitBytes() || size > m_policy.maxBytes)
            return false;
        if (!trim(std::max<qint64>(0, payload.size() - bytes.size())))
            return fail();
        bool okay = m_database.transaction();
        bool affected = false;
        if (okay) {
            QSqlQuery query(m_database);
            query.prepare("UPDATE entries SET payload=?,size=?,stream=?,topic=? WHERE owner=? AND key=?");
            query.addBindValue(payload);
            query.addBindValue(size);
            query.addBindValue(key.stream);
            query.addBindValue(key.topic);
            query.addBindValue(m_owner);
            query.addBindValue(hash);
            okay = query.exec();
            affected = okay && query.numRowsAffected() > 0;
            if (affected)
                m_totalBytes += payload.size() - bytes.size();
        }
        if (okay && affected) {
            QSqlQuery query(m_database);
            query.prepare("DELETE FROM history_rows WHERE owner=? AND key=?");
            query.addBindValue(m_owner);
            query.addBindValue(hash);
            okay = query.exec() && indexHistory(m_owner, hash, rows);
        }
        if (okay)
            okay = trim();
        if (okay)
            okay = !shouldCancel() && m_database.commit();
        if (!okay) {
            m_database.rollback();
            return fail();
        }
        return checkpoint() || fail();
    };
    const QSet<QByteArray> heads{keyHash(head), keyHash({Kind::History, head.stream, {}, {}})};
    QByteArray cursor("");
    while (true) {
        if (shouldCancel())
            return fail();
        QList<QByteArray> keys;
        {
            QSqlQuery query(m_database);
            QString sql = "SELECT h.key FROM history_rows h INDEXED BY cache_message_lookup"
                          " JOIN entries e ON e.owner=h.owner AND e.key=h.key"
                          " WHERE h.owner=? AND h.message_uuid=?";
            if (!fields.testFlag(Projection::Payload)) sql += " AND e.stream=? AND (e.topic=? OR e.topic='')";
            sql += " AND h.key>? ORDER BY h.key LIMIT 128";
            query.prepare(sql);
            query.addBindValue(m_owner);
            query.addBindValue(message->uuid.toLower());
            if (!fields.testFlag(Projection::Payload)) {
                query.addBindValue(head.stream);
                query.addBindValue(head.topic);
            }
            query.addBindValue(cursor);
            if (!query.exec())
                return fail();
            while (query.next())
                keys.append(query.value(0).toByteArray());
        }
        if (keys.isEmpty())
            break;
        cursor = keys.last();
        for (const auto &hash : keys)
            if (!update(hash, false))
                return false;
    }
    if (fields.testFlag(Projection::Insert))
        for (const auto &hash : heads)
            if (!update(hash, true)) return false;
    // A full move may leave no saved destination window. Retain a body only
    // when an owned History row was actually removed; erasure still wins.
    const Key body{Kind::MessageBody, {}, {}, message->uuid.toLower().toUtf8()};
    if (removedHistory && !get(lease, body)) {
        if (!accepts(lease) || !putMessageBody(lease, projectedRow(removedHistory->row, Kind::MessageBody))) return false;
    }
    // A known read flag follows identity across placement. Earlier destination
    // pages may have been visited before the old source supplied that fact.
    if (confirmedRead && !incomingRead)
        return markHistoryRead(lease, head.stream, head.topic, {message->uuid});
    return true;
}

bool NativeCache::clearOwner(const Lease &lease)
{
    if (!accepts(lease)) return false;
    return eraseMatching("owner=?", {m_owner}) || fail();
}

bool NativeCache::forgetAccount(const QUrl &server, const QString &project, const QString &user)
{
    const auto canonical = origin(server, m_allowLoopback);
    if (shouldCancel()) return fail();
    if (canonical.isEmpty() || !isUuid(project) || !isUuid(user)) return false;
    if (!available()) {
        // A signed-out sign-in screen can remove a saved identity without
        // reading it, authenticating it, or publishing a cache owner to the UI.
        NativeCache cleanup(m_directory, m_policy, m_allowLoopback, m_clock);
        cleanup.setCancellationCheck(m_cancel);
        const bool okay = cleanup.setDiskLimitBytes(m_diskLimitBytes) && cleanup.open(canonical, project, user)
            && cleanup.clearOwner(cleanup.lease());
        if (!okay && !shouldCancel())
            setError(cleanup.error().isEmpty() ? tr("The local message cache is unavailable.") : cleanup.error());
        return okay;
    }
    const auto identity = QJsonDocument(QJsonArray{"workspace-cache-v1", canonical.toString(QUrl::FullyEncoded),
        project.toLower(), user.toLower()}).toJson(QJsonDocument::Compact);
    const auto owner = QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
    if (owner == m_owner) return clearOwner(lease());
    return eraseMatching("owner=?", {owner}) || fail();
}

NativeCache::Stats NativeCache::stats() const
{
    if (available()) refreshTotals();
    auto result = m_stats;
    result.freshPages = m_fresh.size();
    if (available()) { result.entries = m_totalEntries; result.bytes = m_totalBytes; }
    return result;
}

} // namespace workspace
