#include "native-realtime-cursor-store.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSqlQuery>
#include <QSqlError>
#include <QStandardPaths>
#include <QUuid>

namespace workspace {
namespace {
// The JavaScript-safe integer range realtime epoch values must stay inside,
// matching the epoch checks in src/realtime-client.cpp.
constexpr quint64 MaxEpoch = 9007199254740991ULL;
constexpr qsizetype MaxGenerationBytes = 1024;
constexpr QFileDevice::Permissions PrivateFile(
    QFileDevice::ReadOwner | QFileDevice::WriteOwner);
const QRegularExpression &keyPattern()
{
    static const QRegularExpression pattern("\\A[0-9a-f]{64}\\z");
    return pattern;
}
bool validCursor(const RealtimeCursorStore::Cursor &cursor)
{
    const auto generation = cursor.generation.toUtf8();
    return !cursor.generation.trimmed().isEmpty() && !generation.isEmpty()
        && generation.size() <= MaxGenerationBytes && cursor.epoch <= MaxEpoch;
}
bool validOwnerKey(const QString &key) { return keyPattern().match(key).hasMatch(); }
}

QString RealtimeCursorStore::ownerKey(const QUrl &server, const QString &project, const QString &user)
{
    const auto identity = QJsonDocument(QJsonArray{server.toString(QUrl::FullyEncoded), project, user})
        .toJson(QJsonDocument::Compact);
    return QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
}

RealtimeCursorStore::RealtimeCursorStore(QString directory)
    : m_directory(directory.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                                      + QStringLiteral("/drafts")
                                      : std::move(directory))
{
    ensureOpen();
}

RealtimeCursorStore::~RealtimeCursorStore() { close(); }

void RealtimeCursorStore::close()
{
    if (m_database.isOpen()) m_database.close();
    m_database = {};
    if (!m_connection.isEmpty()) QSqlDatabase::removeDatabase(m_connection);
    m_connection.clear(); m_path.clear(); m_lock.reset();
}

bool RealtimeCursorStore::available() const { return m_database.isOpen(); }

bool RealtimeCursorStore::ensureOpen()
{
    if (m_database.isOpen()) return true;
    if (QFileInfo(m_directory).isSymLink() || !QDir().mkpath(m_directory)
        || !QFile::setPermissions(m_directory, PrivateFile | QFileDevice::ExeOwner)) return false;
    m_path = QDir(m_directory).filePath(QStringLiteral("realtime_cursors.sqlite3"));
    for (const auto &suffix : {QString{}, QStringLiteral("-wal"), QStringLiteral("-shm"), QStringLiteral(".lock")})
        if (QFileInfo(m_path + suffix).isSymLink()) { close(); return false; }
    m_lock = std::make_unique<QLockFile>(m_path + QStringLiteral(".lock"));
    if (!m_lock->tryLock(0)) { close(); return false; }
    m_connection = QStringLiteral("cassi-native-cursors-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connection);
    m_database.setDatabaseName(m_path);
    if (!m_database.open()) { close(); return false; }
    bool ok = true;
    {
        QSqlQuery query(m_database);
        for (const auto *sql : {"PRAGMA journal_mode=WAL", "PRAGMA synchronous=FULL", "PRAGMA secure_delete=ON"})
            if (!query.exec(QLatin1String(sql))) { ok = false; break; }
        int version = -1;
        if (ok) ok = query.exec(QStringLiteral("PRAGMA user_version")) && query.next();
        if (ok) version = query.value(0).toInt(); else ok = false;
        if (ok && version == 0) {
            // Fresh database: refuse anything but an empty file, then commit the
            // schema in one transaction (same fail-closed discipline as
            // NativeDraftStore::open()).
            ok = query.exec(QStringLiteral(
                "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'"))
                && !query.next();
            if (ok && m_database.transaction()) {
                ok = query.exec(QStringLiteral(
                    "CREATE TABLE realtime_cursors (owner_key TEXT PRIMARY KEY NOT NULL, "
                    "epoch_generation TEXT NOT NULL, epoch_version INTEGER NOT NULL)"))
                    && query.exec(QStringLiteral("PRAGMA user_version=1"));
                if (!ok || !m_database.commit()) { m_database.rollback(); ok = false; }
            } else ok = false;
        }
        if (ok && version != 0 && version != 1) ok = false;
        if (ok) ok = query.exec(QStringLiteral(
            "SELECT owner_key, epoch_generation, epoch_version FROM realtime_cursors LIMIT 0"));
    }
    if (ok) ok = QFile::setPermissions(m_path, PrivateFile);
    if (!ok) close();
    return ok;
}

bool RealtimeCursorStore::save(const QString &key, const Cursor &cursor)
{
    if (!validOwnerKey(key) || !validCursor(cursor)) return false;
    if (!ensureOpen()) return false;
    if (!m_database.transaction()) return false;
    {
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral("SELECT epoch_generation, typeof(epoch_generation), "
            "epoch_version, typeof(epoch_version) FROM realtime_cursors WHERE owner_key = ?"));
        query.addBindValue(key);
        if (!query.exec()) { m_database.rollback(); return false; }
        if (query.next()) {
            const auto stored = query.value(0).toString();
            const auto storedEpoch = query.value(2).toULongLong();
            const bool sane = query.value(1).toString() == QLatin1String("text") && !stored.isEmpty()
                && query.value(3).toString() == QLatin1String("integer") && storedEpoch <= MaxEpoch;
            if (sane && stored == cursor.generation && storedEpoch >= cursor.epoch) {
                // The cursor only grows inside a generation: a late write from a
                // replaced socket must not roll catch-up backwards.
                m_database.rollback();
                return true;
            }
        }
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("INSERT OR REPLACE INTO realtime_cursors "
        "(owner_key, epoch_generation, epoch_version) VALUES (?, ?, ?)"));
    query.addBindValue(key);
    query.addBindValue(cursor.generation);
    query.addBindValue(qint64(cursor.epoch));
    if (!query.exec() || !m_database.commit()) { m_database.rollback(); return false; }
    return true;
}

std::optional<RealtimeCursorStore::Cursor> RealtimeCursorStore::read(const QString &key)
{
    if (!validOwnerKey(key)) return {};
    if (!ensureOpen()) return {};
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT epoch_generation, typeof(epoch_generation), "
        "epoch_version, typeof(epoch_version) FROM realtime_cursors WHERE owner_key = ?"));
    query.addBindValue(key);
    if (!query.exec() || !query.next()) return {};
    const auto generation = query.value(0).toString();
    const auto epoch = query.value(2).toULongLong();
    const auto generationBytes = generation.toUtf8();
    const bool valid = query.value(1).toString() == QLatin1String("text")
        && !generation.trimmed().isEmpty() && !generationBytes.isEmpty()
        && generationBytes.size() <= MaxGenerationBytes
        && query.value(3).toString() == QLatin1String("integer") && epoch <= MaxEpoch;
    if (valid) return Cursor{generation, epoch};
    // Discard invalid rows on read so a later connect cannot resurrect them.
    QSqlQuery purge(m_database);
    purge.prepare(QStringLiteral("DELETE FROM realtime_cursors WHERE owner_key = ?"));
    purge.addBindValue(key);
    purge.exec();
    return {};
}

bool RealtimeCursorStore::clear(const QString &key)
{
    if (!validOwnerKey(key)) return false;
    if (!ensureOpen()) return false;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM realtime_cursors WHERE owner_key = ?"));
    query.addBindValue(key);
    return query.exec();
}

} // namespace workspace
