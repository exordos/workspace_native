#pragma once

#include <QSqlDatabase>
#include <QString>
#include <QUrl>
#include <memory>
#include <optional>

class QLockFile;

namespace workspace {

// Durable realtime cursors (issue #15), stored beside the durable outgoing
// queue in the same private directory and keyed by the same owner identity.
// Synchronous one-row transactions only, used from one thread at a time, so
// write cost stays inside RealtimeClient's throttled flush (see
// docs/NATIVE_DESKTOP_DURABLE_REALTIME_CURSORS.md). A missing, invalid,
// locked or corrupt row always reads back as "no cursor", so a client falls
// back to a clean baseline connect - never to a stale or cross-owner cursor.
class RealtimeCursorStore final {
public:
    struct Cursor { QString generation; quint64 epoch = 0; };
    // SHA256 hex of the compact JSON [server, project, user] identity, the
    // exact derivation NativeController::creationOwnerKey() and the account
    // forgetting purge use, so cursors can never cross owners.
    static QString ownerKey(const QUrl &server, const QString &project, const QString &user);

    explicit RealtimeCursorStore(QString directory = {});
    ~RealtimeCursorStore();
    RealtimeCursorStore(const RealtimeCursorStore &) = delete;
    RealtimeCursorStore &operator=(const RealtimeCursorStore &) = delete;

    bool available() const;
    // save only moves the cursor forward inside one generation; a different
    // generation replaces (a pruned generation is cleared before re-baselining).
    bool save(const QString &key, const Cursor &cursor);
    std::optional<Cursor> read(const QString &key);
    bool clear(const QString &key);

private:
    bool ensureOpen();
    void close();
    QString m_directory, m_path, m_connection;
    QSqlDatabase m_database;
    std::unique_ptr<QLockFile> m_lock;
};

} // namespace workspace
