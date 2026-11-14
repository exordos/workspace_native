#include "native-chat-key-vault.h"
#include "domain.h"
#include "native-chat-crypto.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QScopeGuard>
#include <QTimer>
#include <QUuid>
#include <openssl/crypto.h>
#include <atomic>
#include <cmath>
#include <algorithm>

namespace workspace {
namespace {
std::atomic<quint64> Issuers{0};
constexpr quint64 MaximumRevision = 9007199254740991ULL;
constexpr auto PrivateFile = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
using Failure = NativeChatKeyVault::Failure;
const QByteArray Prefix("WSKV\x01", 5);

void cleanse(QByteArray &bytes)
{
    if (!bytes.isEmpty()) { bytes.detach(); OPENSSL_cleanse(bytes.data(), size_t(bytes.size())); }
    bytes.clear();
}

bool validText(const QString &text)
{
    return !text.trimmed().isEmpty() && text.size() <= 256
        && QString::fromUtf8(text.toUtf8()) == text && !text.contains(QChar::Null);
}

Failure backendFailure(SessionSecretStore::Error error)
{
    return error == SessionSecretStore::Error::Denied ? Failure::Denied : Failure::Unavailable;
}

QByteArray encode(const QJsonObject &value) { return QJsonDocument(value).toJson(QJsonDocument::Compact); }
} // namespace

void NativeChatKeyVault::PendingRequest::clearSecrets()
{
    cleanse(privateKey);
    if (recovery) cleanse(recovery->previousKey);
}

NativeChatKeyVault::NativeChatKeyVault(QString directory, SessionSecretStore *backend, QObject *parent)
    : QObject(parent), m_directory(std::move(directory)), m_backend(backend), m_issuer(++Issuers)
{
    if (m_directory.isEmpty())
        m_directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/chat-secrets-v1";
    if (!m_backend) m_backend = new KeychainSessionSecretStore(this);
    connect(m_backend, &QObject::destroyed, this, [this] {
        ++m_generation;
        if (m_openActive) finishOpen(m_activeDone, Failure::Unavailable);
        else { resetStorage(); m_lock.reset(); emit stateChanged(); }
    });
}

NativeChatKeyVault::~NativeChatKeyVault()
{
    if (m_backend) disconnect(m_backend, nullptr, this, nullptr);
    ++m_generation;
    resetStorage();
    m_lock.reset();
}

void NativeChatKeyVault::resetStorage()
{
    if (m_database.isValid()) {
        m_database.close(); m_database = {};
        QSqlDatabase::removeDatabase(m_connection);
    }
    m_connection.clear(); m_device = {}; cleanse(m_master);
}

void NativeChatKeyVault::close()
{
    ++m_generation; resetStorage(); m_owner.clear();
    if (!m_openActive) m_lock.reset();
    emit stateChanged();
}

void NativeChatKeyVault::startNextOpen()
{
    if (m_openActive || m_openQueue.empty()) return;
    m_openActive = true;
    auto next = std::move(m_openQueue.front()); m_openQueue.pop_front(); next();
}

void NativeChatKeyVault::finishOpen(Opened done, Failure failure)
{
    if (failure != Failure::None) { resetStorage(); m_lock.reset(); }
    m_openActive = false; m_activeDone = {};
    // A consumer may destroy this object or open another owner from its callback.
    QTimer::singleShot(0, this, &NativeChatKeyVault::startNextOpen);
    const QPointer<NativeChatKeyVault> guard(this);
    const auto generation = m_generation;
    emit stateChanged();
    if (!guard) return;
    if (failure == Failure::None && m_generation != generation) failure = Failure::Superseded;
    if (done) done(failure);
}

void NativeChatKeyVault::open(const QUrl &server, const QString &project, const QString &user,
                             const QString &deviceName, Opened done)
{
    const QPointer<NativeChatKeyVault> guard(this);
    const auto expectedGeneration = m_generation + 1;
    close();
    if (!guard) return;
    if (m_generation != expectedGeneration) {
        QTimer::singleShot(0, this, [done] { if (done) done(Failure::Superseded); });
        return;
    }
    m_owner = SessionVault::accountId(server, project, user);
    if (m_owner.isEmpty() || !validText(deviceName)) {
        m_owner.clear();
        QTimer::singleShot(0, this, [done] { if (done) done(Failure::InvalidOwner); }); return;
    }
    const auto generation = m_generation;
    const auto path = QDir(m_directory).filePath(m_owner + ".sqlite3");
    const auto secretAddress = "chat-master-v1:" + m_owner;
    if (m_openQueue.size() >= 32) {
        QTimer::singleShot(0, this, [done] { if (done) done(Failure::Capacity); }); return;
    }
    m_openQueue.push_back([this, generation, path, secretAddress, deviceName, done] {
        m_activeGeneration = generation; m_activeDone = done;
        if (m_generation != generation) { finishOpen(done, Failure::Superseded); return; }
        if (!m_backend) { finishOpen(done, Failure::Unavailable); return; }
        const auto lockPath = path + ".lock";
        const QFileInfo directory(m_directory), file(path);
        if (directory.isSymLink() || file.isSymLink() || QFileInfo(lockPath).isSymLink()
            || (file.exists() && (!file.isFile() || file.size() > MaximumDatabaseBytes))
            || !QDir().mkpath(m_directory)
            || !QFile::setPermissions(m_directory, PrivateFile | QFileDevice::ExeOwner)) {
            finishOpen(done, Failure::Unavailable); return;
        }
        m_lock = std::make_unique<QLockFile>(lockPath); m_lock->setStaleLockTime(0);
        if (!m_lock->tryLock(0)) { finishOpen(done, Failure::Unavailable); return; }
        const QPointer<NativeChatKeyVault> guard(this);
        m_backend->read(secretAddress, [guard, generation, path, secretAddress, deviceName, done](QByteArray secret, SessionSecretStore::Error error) mutable {
            if (!guard) { cleanse(secret); return; }
            if (!guard->m_openActive || guard->m_activeGeneration != generation) { cleanse(secret); return; }
            if (guard->m_generation != generation) { cleanse(secret); guard->finishOpen(done, Failure::Superseded); return; }
            if (error == SessionSecretStore::Error::NotFound) {
                cleanse(secret);
                // Existing files must never receive a newly generated master.
                if (QFileInfo::exists(path) || QFileInfo(path).isSymLink()) {
                    guard->finishOpen(done, Failure::MissingMasterKey); return;
                }
                auto generated = NativeChatCrypto::createGroupKey();
                if (!generated || !guard->m_backend) { guard->finishOpen(done, Failure::Unavailable); return; }
                // Reserve initialization durably before a possibly ambiguous OS
                // write. A later opener must not generate another master while
                // the first write can still complete after destruction/crash.
                QFile reservation(path);
                if (!reservation.open(QIODevice::WriteOnly | QIODevice::NewOnly)
                    || !reservation.setPermissions(PrivateFile)) {
                    cleanse(generated.bytes); guard->finishOpen(done, Failure::Unavailable); return;
                }
                reservation.close();
                guard->m_master = std::move(generated.bytes);
                guard->m_backend->write(secretAddress, guard->m_master,
                    [guard, generation, path, deviceName, done](SessionSecretStore::Error written) {
                        if (!guard) return;
                        if (!guard->m_openActive || guard->m_activeGeneration != generation) return;
                        if (guard->m_generation != generation) { guard->finishOpen(done, Failure::Superseded); return; }
                        if (written != SessionSecretStore::Error::None) { guard->finishOpen(done, backendFailure(written)); return; }
                        guard->finishOpen(done, guard->initialize(path, deviceName));
                    });
                return;
            }
            if (error != SessionSecretStore::Error::None) {
                cleanse(secret); guard->finishOpen(done, backendFailure(error)); return;
            }
            if (secret.size() != 32) { cleanse(secret); guard->finishOpen(done, Failure::Corrupt); return; }
            guard->m_master = std::move(secret);
            guard->finishOpen(done, guard->initialize(path, deviceName));
        });
    });
    startNextOpen();
}

NativeChatKeyVault::Lease NativeChatKeyVault::lease() const
{
    return available() ? Lease{m_owner, m_device.id, m_issuer, m_generation} : Lease{};
}

bool NativeChatKeyVault::accepts(const Lease &value) const
{
    return available() && value.issuer == m_issuer && value.generation == m_generation
        && value.owner == m_owner && value.device == m_device.id;
}

Failure NativeChatKeyVault::initialize(const QString &path, const QString &deviceName)
{
    const QFileInfo directory(m_directory), file(path);
    if (directory.isSymLink() || file.isSymLink() || (file.exists() && !file.isFile())
        || !QDir().mkpath(m_directory)
        || !QFile::setPermissions(m_directory, PrivateFile | QFileDevice::ExeOwner)) return Failure::Unavailable;
    // A zero-byte file is a reserved initialization with a confirmed OS master.
    const bool existing = file.exists() && file.size() != 0;
    QFile privateFile(path);
    if (!privateFile.open(QIODevice::ReadWrite) || !privateFile.setPermissions(PrivateFile)) return Failure::Unavailable;
    privateFile.close();
    m_connection = "workspace-chat-keys-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_database = QSqlDatabase::addDatabase("QSQLITE", m_connection);
    m_database.setDatabaseName(path);
    if (!m_database.open()) return Failure::Unavailable;
    QSqlQuery query(m_database);
    int version = 3;
    if (existing) {
        if (!query.exec("PRAGMA application_id") || !query.next() || query.value(0).toInt() != 1465076566)
            return Failure::Corrupt;
        query.finish();
        if (!query.exec("PRAGMA user_version") || !query.next()) return Failure::Corrupt;
        version = query.value(0).toInt();
        if (version < 1 || version > 3) return Failure::Corrupt;
        query.finish();
    }
    for (const auto *sql : {"PRAGMA page_size=4096", "PRAGMA cache_size=-2048", "PRAGMA journal_mode=DELETE",
            "PRAGMA synchronous=FULL", "PRAGMA secure_delete=ON", "PRAGMA max_page_count=16384"}) {
        if (!query.exec(QString::fromLatin1(sql))) return Failure::Unavailable;
        query.finish();
    }
    if (!existing) {
        if (!m_database.transaction()) return Failure::Unavailable;
        const bool okay = query.exec("CREATE TABLE secrets(address TEXT PRIMARY KEY NOT NULL,payload BLOB NOT NULL) WITHOUT ROWID")
            && query.exec("PRAGMA application_id=1465076566") && query.exec("PRAGMA user_version=3");
        query.finish();
        if (!okay || !m_database.commit()) { m_database.rollback(); return Failure::Unavailable; }
        m_device = {QUuid::createUuid().toString(QUuid::WithoutBraces), deviceName};
        const auto result = writeRecord("device", encode({{"id", m_device.id}, {"name", m_device.name}}));
        if (result != Failure::None) return result;
    } else {
        Failure error = Failure::None;
        auto bytes = readRecord("device", error);
        if (!bytes || error != Failure::None) return Failure::Corrupt;
        const auto object = QJsonDocument::fromJson(*bytes).object(); cleanse(*bytes);
        m_device = {object.value("id").toString(), object.value("name").toString()};
        if (!isUuid(m_device.id) || !validText(m_device.name)) return Failure::Corrupt;
        if (version < 3 && !query.exec("PRAGMA user_version=3")) return Failure::Unavailable;
    }
    return Failure::None;
}

Failure NativeChatKeyVault::writeRecord(const QString &address, const QByteArray &value, qsizetype limit)
{
    if (value.size() > limit) return Failure::Capacity;
    auto result = NativeChatCrypto::encryptMessage(m_master, value, {"workspace-native-key-vault-v1:" + m_owner, address});
    if (!result) return Failure::Unavailable;
    QSqlQuery query(m_database);
    query.prepare("INSERT OR REPLACE INTO secrets(address,payload) VALUES(?,?)");
    query.addBindValue(address); query.addBindValue(Prefix + result.bytes);
    if (query.exec()) return Failure::None;
    return query.lastError().nativeErrorCode().toInt() == 13 ? Failure::Capacity : Failure::Unavailable;
}

std::optional<QByteArray> NativeChatKeyVault::readRecord(const QString &address, Failure &failure, qsizetype limit)
{
    QSqlQuery query(m_database);
    query.prepare("SELECT CASE WHEN LENGTH(payload)<=? THEN payload END FROM secrets WHERE address=?");
    query.addBindValue(limit + 48 + Prefix.size()); query.addBindValue(address);
    if (!query.exec()) { failure = Failure::Unavailable; return {}; }
    if (!query.next()) return {};
    const auto envelope = query.value(0).toByteArray();
    if (!envelope.startsWith(Prefix)) { failure = Failure::Corrupt; return {}; }
    auto opened = NativeChatCrypto::decryptMessage(m_master, QByteArrayView(envelope).sliced(Prefix.size()),
        {"workspace-native-key-vault-v1:" + m_owner, address});
    if (!opened || opened.bytes.size() > limit) { failure = Failure::Corrupt; return {}; }
    return std::move(opened.bytes);
}

QString NativeChatKeyVault::addressKey(const Address &address) const
{
    if (!isUuid(address.stream) || !isUuid(address.key)) return {};
    return QString::fromUtf8(QJsonDocument(QJsonArray{"group", m_device.id, address.stream.toLower(), address.key.toLower()}).toJson(QJsonDocument::Compact));
}

NativeChatKeyVault::ReadResult NativeChatKeyVault::readKey(const Address &address)
{
    ReadResult result{Failure::None, {}};
    auto bytes = readRecord(addressKey(address), result.failure);
    if (!bytes || result.failure != Failure::None) return result;
    const auto object = QJsonDocument::fromJson(*bytes).object(); cleanse(*bytes);
    const auto revision = object.value("deletionRevision").toDouble(-1);
    const auto received = object.value("receivedAt").toDouble(-1);
    const auto order = object.value("sendOrder").isUndefined() ? 0 : object.value("sendOrder").toDouble(-1);
    const auto key = object.value("key");
    if (!std::isfinite(revision) || revision < 0 || revision > double(MaximumRevision) || std::floor(revision) != revision
        || !std::isfinite(received) || received < 0 || received > double(MaximumRevision) || std::floor(received) != received
        || !std::isfinite(order) || order < 0 || order > double(MaximumRevision) || std::floor(order) != order
        || (!key.isNull() && !key.isString())) return {Failure::Corrupt, {}};
    result.value.deletionRevision = quint64(revision); result.value.receivedAt = qint64(received);
    result.value.sendOrder = quint64(order);
    if (key.isString()) {
        const auto encoded = key.toString().toLatin1();
        result.value.bytes = QByteArray::fromBase64(encoded, QByteArray::AbortOnBase64DecodingErrors);
        if (result.value.bytes.size() != 32 || result.value.bytes.toBase64() != encoded) return {Failure::Corrupt, {}};
    } else if (received != 0 || order != 0) return {Failure::Corrupt, {}};
    return result;
}

NativeChatKeyVault::ReadResult NativeChatKeyVault::read(const Lease &value, const Address &address)
{
    if (!accepts(value)) return {Failure::Superseded, {}};
    if (addressKey(address).isEmpty()) return {Failure::InvalidAddress, {}};
    return readKey(address);
}

Failure NativeChatKeyVault::writeKey(const Address &address, const Key &key)
{
    return writeRecord(addressKey(address), encode({{"key", key.bytes.isEmpty() ? QJsonValue(QJsonValue::Null)
        : QJsonValue(QString::fromLatin1(key.bytes.toBase64()))}, {"deletionRevision", qint64(key.deletionRevision)},
        {"receivedAt", key.receivedAt}, {"sendOrder", qint64(key.sendOrder)}}));
}

Failure NativeChatKeyVault::put(const Lease &value, const Address &address, const QByteArray &key, quint64 expectedDeletionRevision)
{
    if (!accepts(value)) return Failure::Superseded;
    if (addressKey(address).isEmpty()) return Failure::InvalidAddress;
    if (key.size() != 32) return Failure::InvalidKey;
    if (!m_database.transaction()) return Failure::Unavailable;
    auto old = readKey(address); Failure result = old.failure;
    const bool materialChanged = old.failure == Failure::None && old.value.bytes.isEmpty();
    if (result == Failure::None && old.value.deletionRevision != expectedDeletionRevision) result = Failure::Superseded;
    if (result == Failure::None && !old.value.bytes.isEmpty() && old.value.bytes != key) result = Failure::Conflict;
    if (result == Failure::None) {
        if (old.value.bytes.isEmpty()) old.value.receivedAt = QDateTime::currentMSecsSinceEpoch();
        old.value.bytes = key; result = writeKey(address, old.value);
    }
    cleanse(old.value.bytes);
    if (result != Failure::None || !m_database.commit()) {
        m_database.rollback(); return result == Failure::None ? Failure::Unavailable : result;
    }
    if (materialChanged) emit keyMaterialChanged(address.stream.toLower(), address.key.toLower());
    return Failure::None;
}

Failure NativeChatKeyVault::remove(const Lease &value, const Address &address)
{
    if (!accepts(value)) return Failure::Superseded;
    if (addressKey(address).isEmpty()) return Failure::InvalidAddress;
    if (!m_database.transaction()) return Failure::Unavailable;
    auto old = readKey(address); Failure result = old.failure;
    if (result == Failure::None && old.value.deletionRevision >= MaximumRevision) result = Failure::Capacity;
    if (result == Failure::None) {
        cleanse(old.value.bytes); ++old.value.deletionRevision; old.value.receivedAt = 0; old.value.sendOrder = 0;
        result = writeKey(address, old.value);
    }
    if (result == Failure::None) {
        const auto prefix = requestPrefix("request", address);
        QSqlQuery query(m_database);
        query.prepare("DELETE FROM secrets WHERE address>=? AND address<?");
        query.addBindValue(prefix); query.addBindValue(prefix.chopped(1) + '-');
        if (!query.exec()) result = Failure::Unavailable;
    }
    cleanse(old.value.bytes);
    if (result != Failure::None || !m_database.commit()) {
        m_database.rollback(); return result == Failure::None ? Failure::Unavailable : result;
    }
    emit keyMaterialChanged(address.stream.toLower(), address.key.toLower());
    return Failure::None;
}

namespace {
using Request = NativeChatKeyVault::PendingRequest;
using Completion = NativeChatKeyVault::Completion;
using Outcome = NativeChatKeyVault::Outcome;

bool validInteger(double value)
{
    return std::isfinite(value) && value >= 0 && value <= double(MaximumRevision) && std::floor(value) == value;
}

QByteArray decodedBytes(const QJsonValue &value)
{
    if (!value.isString()) return {};
    const auto encoded = value.toString().toLatin1();
    auto bytes = QByteArray::fromBase64(encoded, QByteArray::AbortOnBase64DecodingErrors);
    return bytes.toBase64() == encoded ? bytes : QByteArray{};
}

bool validRequest(const Request &value)
{
    return isUuid(value.localId) && (value.messageId.isEmpty() || isUuid(value.messageId))
        && (value.targetTopic.isEmpty() || isUuid(value.targetTopic))
        && validText(value.deviceName) && value.publicKey.size() == 32 && value.privateKey.size() == 32
        && value.createdAt >= 0 && quint64(value.createdAt) <= MaximumRevision
        && value.deletionRevision <= MaximumRevision
        && (!value.recovery || (value.recovery->previousKey.size() == 32
            && value.recovery->envelope.size() >= 48
            && value.recovery->envelope.size() <= NativeChatKeyVault::MaximumRecoveryEnvelopeBytes));
}

QByteArray encodeRequest(const Request &value)
{
    QJsonObject object{{"localId", value.localId}, {"messageId", value.messageId}, {"deviceName", value.deviceName},
        {"publicKey", QString::fromLatin1(value.publicKey.toBase64())}, {"privateKey", QString::fromLatin1(value.privateKey.toBase64())},
        {"createdAt", value.createdAt}, {"deletionRevision", qint64(value.deletionRevision)}};
    if (!value.targetTopic.isEmpty()) object.insert("targetTopic", value.targetTopic);
    if (value.recovery) object.insert("recovery", QJsonObject{
        {"envelope", QString::fromLatin1(value.recovery->envelope.toBase64())},
        {"previousKey", QString::fromLatin1(value.recovery->previousKey.toBase64())}});
    return encode(object);
}

std::optional<Request> decodeRequest(const QByteArray &bytes)
{
    const auto object = QJsonDocument::fromJson(bytes).object();
    const auto created = object.value("createdAt").toDouble(-1), revision = object.value("deletionRevision").toDouble(-1);
    if (!validInteger(created) || !validInteger(revision) || !object.value("messageId").isString()) return {};
    Request request{object.value("localId").toString(), object.value("messageId").toString(), object.value("deviceName").toString(),
        decodedBytes(object.value("publicKey")), decodedBytes(object.value("privateKey")), qint64(created), quint64(revision), {}};
    if (object.contains("targetTopic")) {
        if (!object.value("targetTopic").isString()) return {};
        request.targetTopic = object.value("targetTopic").toString().toLower();
    }
    if (object.contains("recovery")) {
        if (!object.value("recovery").isObject()) return {};
        const auto recovery = object.value("recovery").toObject();
        request.recovery = NativeChatKeyVault::Recovery{decodedBytes(recovery.value("envelope")), decodedBytes(recovery.value("previousKey"))};
    }
    return validRequest(request) ? std::optional<Request>(std::move(request)) : std::nullopt;
}

bool sameCompletion(const Completion &left, const Completion &right)
{
    return left.localId == right.localId && left.requestMessage == right.requestMessage
        && left.responseMessage == right.responseMessage && left.outcome == right.outcome;
}

QByteArray encodeCompletion(const Completion &value)
{
    return encode({{"localId", value.localId}, {"requestMessage", value.requestMessage}, {"responseMessage", value.responseMessage},
        {"outcome", value.outcome == Outcome::Granted ? "granted" : "rejected"}});
}

void cleanseRequest(std::optional<Request> &request)
{
    if (!request) return;
    cleanse(request->privateKey);
    if (request->recovery) cleanse(request->recovery->previousKey);
    request.reset();
}
} // namespace

QString NativeChatKeyVault::requestPrefix(const char *type, const Address &address) const
{
    if (addressKey(address).isEmpty()) return {};
    auto value = QString::fromUtf8(QJsonDocument(QJsonArray{QLatin1String(type), m_device.id,
        address.stream.toLower(), address.key.toLower()}).toJson(QJsonDocument::Compact));
    value.chop(1); return value + ',';
}

QString NativeChatKeyVault::requestAddress(const char *type, const Address &address, const QString &id) const
{
    const auto prefix = requestPrefix(type, address);
    return !prefix.isEmpty() && isUuid(id) ? prefix + '"' + id.toLower() + "\"]" : QString{};
}

Failure NativeChatKeyVault::finishTransaction(Failure result)
{
    if (result == Failure::None && m_database.commit()) return Failure::None;
    m_database.rollback(); return result == Failure::None ? Failure::Unavailable : result;
}

std::optional<Request> NativeChatKeyVault::readPending(const Address &address, const QString &id, Failure &failure)
{
    auto bytes = readRecord(requestAddress("request", address, id), failure, MaximumRequestRecordBytes);
    if (!bytes) return {};
    auto value = decodeRequest(*bytes); cleanse(*bytes);
    if (!value || value->localId != id.toLower()) { failure = Failure::Corrupt; return {}; }
    return value;
}

Failure NativeChatKeyVault::requestCapacity(const Address &address, const QString &record, qsizetype encodedBytes)
{
    if (encodedBytes > MaximumRequestRecordBytes) return Failure::Capacity;
    const auto prefix = requestPrefix("request", address);
    QSqlQuery query(m_database);
    query.prepare("SELECT COUNT(*),COALESCE(SUM(LENGTH(payload)),0) FROM secrets WHERE address>=? AND address<? AND address!=?");
    query.addBindValue(prefix); query.addBindValue(prefix.chopped(1) + '-'); query.addBindValue(record);
    if (!query.exec() || !query.next()) return Failure::Unavailable;
    return query.value(0).toLongLong() < MaximumPendingRequests
        && query.value(1).toLongLong() + encodedBytes + 48 + Prefix.size() <= MaximumPendingBytes
        ? Failure::None : Failure::Capacity;
}

NativeChatKeyVault::PendingResult NativeChatKeyVault::pendingRequests(const Lease &value, const Address &address)
{
    if (!accepts(value)) return {Failure::Superseded, {}};
    if (addressKey(address).isEmpty()) return {Failure::InvalidAddress, {}};
    const auto prefix = requestPrefix("request", address);
    QSqlQuery query(m_database);
    query.prepare("SELECT address,LENGTH(payload) FROM secrets WHERE address>=? AND address<? ORDER BY address LIMIT ?");
    query.addBindValue(prefix); query.addBindValue(prefix.chopped(1) + '-'); query.addBindValue(MaximumPendingRequests + 1);
    if (!query.exec()) return {Failure::Unavailable, {}};
    QList<QString> records; qint64 total = 0;
    while (query.next()) {
        total += query.value(1).toLongLong(); records.append(query.value(0).toString());
        if (records.size() > MaximumPendingRequests || total > MaximumPendingBytes) return {Failure::Capacity, {}};
    }
    query.finish();
    PendingResult result{Failure::None, {}};
    for (const auto &record : records) {
        auto bytes = readRecord(record, result.failure, MaximumRequestRecordBytes);
        if (!bytes) return {result.failure == Failure::None ? Failure::Corrupt : result.failure, {}};
        auto request = decodeRequest(*bytes); cleanse(*bytes);
        if (!request || requestAddress("request", address, request->localId) != record) return {Failure::Corrupt, {}};
        result.values.append(std::move(*request));
    }
    return result;
}

Failure NativeChatKeyVault::putPendingRequest(const Lease &value, const Address &address, const PendingRequest &request)
{
    if (!accepts(value)) return Failure::Superseded;
    const auto record = requestAddress("request", address, request.localId);
    if (record.isEmpty()) return Failure::InvalidAddress;
    if (request.recovery && request.recovery->envelope.size() > MaximumRecoveryEnvelopeBytes) return Failure::Capacity;
    if (!validRequest(request)) return Failure::InvalidKey;
    auto captured = request; captured.localId = captured.localId.toLower(); captured.messageId = captured.messageId.toLower();
    captured.targetTopic = captured.targetTopic.toLower();
    auto encoded = encodeRequest(captured);
    if (encoded.size() > MaximumRequestRecordBytes) { cleanse(encoded); return Failure::Capacity; }
    if (!m_database.transaction()) { cleanse(encoded); return Failure::Unavailable; }
    auto key = readKey(address); auto result = key.failure;
    auto old = readPending(address, captured.localId, result);
    const auto terminal = readCompletion(requestAddress("receipt", address, captured.localId));
    if (result == Failure::None && terminal.failure != Failure::None) result = terminal.failure;
    if (result == Failure::None && terminal.value) result = Failure::Conflict;
    if (result == Failure::None && key.value.deletionRevision != captured.deletionRevision) result = Failure::Superseded;
    if (result == Failure::None && old && encodeRequest(*old) != encoded) result = Failure::Conflict;
    if (result == Failure::None) result = requestCapacity(address, record, encoded.size());
    if (result == Failure::None) result = writeRecord(record, encoded, MaximumRequestRecordBytes);
    cleanse(key.value.bytes); cleanseRequest(old); cleanse(encoded);
    return finishTransaction(result);
}

Failure NativeChatKeyVault::bindPendingRequest(const Lease &value, const Address &address,
                                              const QString &localId, const QString &messageId)
{
    if (!accepts(value)) return Failure::Superseded;
    if (requestAddress("request", address, localId).isEmpty() || !isUuid(messageId)) return Failure::InvalidAddress;
    if (!m_database.transaction()) return Failure::Unavailable;
    Failure result = Failure::None; auto pending = readPending(address, localId, result);
    auto key = readKey(address);
    if (result == Failure::None && key.failure != Failure::None) result = key.failure;
    if (result == Failure::None && !pending) result = Failure::MissingRequest;
    if (result == Failure::None && pending->deletionRevision != key.value.deletionRevision) result = Failure::Superseded;
    if (result == Failure::None && !pending->messageId.isEmpty() && pending->messageId != pending->localId
        && pending->messageId != messageId.toLower()) result = Failure::Conflict;
    if (result == Failure::None) {
        pending->messageId = messageId.toLower(); auto encoded = encodeRequest(*pending);
        const auto record = requestAddress("request", address, localId);
        result = requestCapacity(address, record, encoded.size());
        if (result == Failure::None) result = writeRecord(record, encoded, MaximumRequestRecordBytes);
        cleanse(encoded);
    }
    cleanse(key.value.bytes); cleanseRequest(pending); return finishTransaction(result);
}

NativeChatKeyVault::CompletionResult NativeChatKeyVault::readCompletion(const QString &record)
{
    Failure error = Failure::None; auto bytes = readRecord(record, error);
    if (!bytes) return {error, {}};
    const auto object = QJsonDocument::fromJson(*bytes).object(); cleanse(*bytes);
    Completion value{object.value("localId").toString(), object.value("requestMessage").toString(),
        object.value("responseMessage").toString(), Outcome::Rejected};
    const auto outcome = object.value("outcome").toString();
    if (!isUuid(value.localId) || !isUuid(value.requestMessage) || !isUuid(value.responseMessage)
        || (outcome != "granted" && outcome != "rejected")) return {Failure::Corrupt, {}};
    value.outcome = outcome == "granted" ? Outcome::Granted : Outcome::Rejected;
    return {Failure::None, std::move(value)};
}

NativeChatKeyVault::CompletionResult NativeChatKeyVault::requestCompletion(const Lease &value, const Address &address, const QString &localId)
{
    if (!accepts(value)) return {Failure::Superseded, {}};
    const auto record = requestAddress("receipt", address, localId);
    if (record.isEmpty()) return {Failure::InvalidAddress, {}};
    auto result = readCompletion(record);
    if (result.value && result.value->localId != localId.toLower()) return {Failure::Corrupt, {}};
    return result;
}

NativeChatKeyVault::CompletionResult NativeChatKeyVault::requestCompletionByMessage(const Lease &value, const Address &address, const QString &messageId)
{
    if (!accepts(value)) return {Failure::Superseded, {}};
    const auto record = requestAddress("receipt-message", address, messageId);
    if (record.isEmpty()) return {Failure::InvalidAddress, {}};
    auto result = readCompletion(record);
    if (result.value && result.value->requestMessage != messageId.toLower()) return {Failure::Corrupt, {}};
    return result;
}

Failure NativeChatKeyVault::completeRequest(const Lease &value, const Address &address, const Completion &completion,
                                          const QByteArray &groupKey, bool replaceExisting)
{
    if (!accepts(value)) return Failure::Superseded;
    if (addressKey(address).isEmpty() || !isUuid(completion.localId) || !isUuid(completion.requestMessage)
        || !isUuid(completion.responseMessage)) return Failure::InvalidAddress;
    if ((completion.outcome == Outcome::Granted && groupKey.size() != 32)
        || (completion.outcome == Outcome::Rejected && (!groupKey.isEmpty() || replaceExisting))
        || (completion.outcome != Outcome::Granted && completion.outcome != Outcome::Rejected)) return Failure::InvalidKey;
    auto captured = completion; captured.localId = captured.localId.toLower();
    captured.requestMessage = captured.requestMessage.toLower(); captured.responseMessage = captured.responseMessage.toLower();
    if (!m_database.transaction()) return Failure::Unavailable;
    Failure pendingError = Failure::None; auto pending = readPending(address, captured.localId, pendingError);
    auto key = readKey(address);
    const auto localRecord = requestAddress("receipt", address, captured.localId);
    const auto messageRecord = requestAddress("receipt-message", address, captured.requestMessage);
    const auto terminal = readCompletion(localRecord), byMessage = readCompletion(messageRecord);
    bool materialChanged = false;
    const auto result = [&]() -> Failure {
        if (terminal.failure != Failure::None) return terminal.failure;
        if (byMessage.failure != Failure::None) return byMessage.failure;
        if (terminal.value) {
            if (!sameCompletion(*terminal.value, captured)) return Failure::Conflict;
            if (!byMessage.value || !sameCompletion(*terminal.value, *byMessage.value)) return Failure::Corrupt;
            return Failure::None;
        }
        if (byMessage.value) return Failure::Conflict;
        if (pendingError != Failure::None) return pendingError;
        if (key.failure != Failure::None) return key.failure;
        if (!pending) return Failure::MissingRequest;
        if (pending->messageId != captured.requestMessage) return Failure::Conflict;
        if (pending->deletionRevision != key.value.deletionRevision) return Failure::Superseded;
        if (captured.outcome == Outcome::Granted) {
            if (replaceExisting) {
                if (!pending->recovery || key.value.bytes.isEmpty()
                    || (key.value.bytes != pending->recovery->previousKey && key.value.bytes != groupKey)) return Failure::Conflict;
            } else if (!key.value.bytes.isEmpty() && key.value.bytes != groupKey) return Failure::Conflict;
            materialChanged = key.value.bytes != groupKey;
            if (key.value.bytes.isEmpty()) key.value.receivedAt = QDateTime::currentMSecsSinceEpoch();
            key.value.bytes = groupKey;
            const auto written = writeKey(address, key.value);
            if (written != Failure::None) return written;
        }
        auto receipt = encodeCompletion(captured);
        const auto first = writeRecord(localRecord, receipt);
        if (first != Failure::None) return first;
        const auto second = writeRecord(messageRecord, receipt);
        if (second != Failure::None) return second;
        QSqlQuery query(m_database); query.prepare("DELETE FROM secrets WHERE address=?");
        query.addBindValue(requestAddress("request", address, captured.localId));
        return query.exec() ? Failure::None : Failure::Unavailable;
    }();
    cleanse(key.value.bytes); cleanseRequest(pending);
    const auto finished = finishTransaction(result);
    if (finished == Failure::None && materialChanged)
        emit keyMaterialChanged(address.stream.toLower(), address.key.toLower());
    return finished;
}

Failure NativeChatKeyVault::visitStreamKeys(const QString &stream,
    const std::function<Failure(const QString &, const Key &)> &visit)
{
    auto prefix = QString::fromUtf8(QJsonDocument(QJsonArray{"group", m_device.id, stream.toLower()}).toJson(QJsonDocument::Compact));
    prefix.chop(1); prefix += ',';
    QSqlQuery query(m_database);
    // The 64MiB database is already bounded. Stream addresses and decrypt only
    // one generation at a time; tombstones must not impose a lifetime count cap.
    query.setForwardOnly(true);
    query.prepare("SELECT address FROM secrets WHERE address>=? AND address<? ORDER BY address");
    query.addBindValue(prefix); query.addBindValue(prefix.chopped(1) + '-');
    if (!query.exec()) return Failure::Unavailable;
    while (query.next()) {
        const auto record = query.value(0).toString();
        const auto parts = QJsonDocument::fromJson(record.toUtf8()).array();
        if (parts.size() != 4 || !parts[3].isString()) return Failure::Corrupt;
        const Address address{stream.toLower(), parts[3].toString()};
        if (addressKey(address) != record) return Failure::Corrupt;
        auto key = readKey(address);
        const auto cleanup = qScopeGuard([&] { cleanse(key.value.bytes); });
        if (key.failure != Failure::None) return key.failure;
        const auto result = visit(address.key, key.value);
        if (result != Failure::None) return result;
    }
    return query.lastError().isValid() ? Failure::Unavailable : Failure::None;
}

NativeChatKeyVault::MetadataResult NativeChatKeyVault::listKeys(const Lease &value, const QString &stream)
{
    if (!accepts(value)) return {Failure::Superseded, {}};
    if (!isUuid(stream)) return {Failure::InvalidAddress, {}};
    if (!m_database.transaction()) return {Failure::Unavailable, {}};
    MetadataResult result{Failure::None, {}};
    auto failure = visitStreamKeys(stream, [&](const QString &id, const Key &key) {
        if (!key.bytes.isEmpty()) result.values.append({id, key.receivedAt});
        return Failure::None;
    });
    failure = finishTransaction(failure);
    if (failure != Failure::None) return {failure, {}};
    std::sort(result.values.begin(), result.values.end(), [](const KeyMetadata &a, const KeyMetadata &b) {
        return a.receivedAt != b.receivedAt ? a.receivedAt > b.receivedAt : a.key < b.key;
    });
    return result;
}

NativeChatKeyVault::AvailableKeyResult NativeChatKeyVault::lastAvailableKey(const Lease &value, const QString &stream)
{
    if (!accepts(value)) return {Failure::Superseded, {}, {}};
    if (!isUuid(stream)) return {Failure::InvalidAddress, {}, {}};
    if (!m_database.transaction()) return {Failure::Unavailable, {}, {}};
    AvailableKeyResult result{Failure::None, {}, {}};
    auto failure = visitStreamKeys(stream, [&](const QString &id, const Key &key) {
        if (!key.bytes.isEmpty() && (result.key.isEmpty() || key.sendOrder > result.value.sendOrder
            || (key.sendOrder == result.value.sendOrder && key.receivedAt > result.value.receivedAt))) {
            cleanse(result.value.bytes); result.key = id; result.value = key;
        }
        return Failure::None;
    });
    failure = finishTransaction(failure);
    if (failure != Failure::None) { cleanse(result.value.bytes); return {failure, {}, {}}; }
    return result;
}

Failure NativeChatKeyVault::markForSending(const Lease &value, const Address &address, quint64 expectedDeletionRevision)
{
    if (!accepts(value)) return Failure::Superseded;
    if (addressKey(address).isEmpty()) return Failure::InvalidAddress;
    if (!m_database.transaction()) return Failure::Unavailable;
    auto key = readKey(address); auto failure = key.failure;
    const auto cleanup = qScopeGuard([&] { cleanse(key.value.bytes); });
    if (failure == Failure::None && key.value.deletionRevision != expectedDeletionRevision) failure = Failure::Superseded;
    if (failure == Failure::None && key.value.bytes.isEmpty()) failure = Failure::MissingKey;
    quint64 maximum = 0;
    if (failure == Failure::None) failure = visitStreamKeys(address.stream, [&](const QString &, const Key &other) {
        maximum = std::max(maximum, other.sendOrder); return Failure::None;
    });
    if (failure == Failure::None && maximum >= MaximumRevision) failure = Failure::Capacity;
    if (failure == Failure::None) {
        key.value.sendOrder = maximum + 1;
        failure = writeKey(address, key.value);
    }
    return finishTransaction(failure);
}

void NativeChatKeyVault::CreateStage::clearSecrets()
{
    cleanse(groupKey);
}

QString NativeChatKeyVault::createStagePrefix() const
{
    auto value = QString::fromUtf8(QJsonDocument(QJsonArray{QLatin1String("create-stage"), m_device.id})
        .toJson(QJsonDocument::Compact));
    value.chop(1); return value + ',';
}

QString NativeChatKeyVault::createStageAddress(const QString &attempt) const
{
    return isUuid(attempt) ? createStagePrefix() + '"' + attempt.toLower() + "\"]" : QString{};
}

namespace {
QByteArray encodeCreateStage(const NativeChatKeyVault::CreateStage &value)
{
    return encode({{"attempt", value.attempt}, {"key", value.key},
        {"groupKey", QString::fromLatin1(value.groupKey.toBase64())}, {"createdAt", value.createdAt}});
}

std::optional<NativeChatKeyVault::CreateStage> decodeCreateStage(const QByteArray &bytes)
{
    const auto object = QJsonDocument::fromJson(bytes).object();
    const auto created = object.value("createdAt").toDouble(-1);
    if (!validInteger(created)) return {};
    NativeChatKeyVault::CreateStage stage{object.value("attempt").toString(), object.value("key").toString(),
        decodedBytes(object.value("groupKey")), qint64(created)};
    if (!isUuid(stage.attempt) || !isUuid(stage.key) || stage.groupKey.size() != 32) return {};
    return std::move(stage);
}
} // namespace

std::optional<NativeChatKeyVault::CreateStage> NativeChatKeyVault::readCreateStageRecord(const QString &attempt, Failure &failure)
{
    auto bytes = readRecord(createStageAddress(attempt), failure);
    if (!bytes) return {};
    auto stage = decodeCreateStage(*bytes); cleanse(*bytes);
    if (!stage || stage->attempt != attempt.toLower()) { failure = Failure::Corrupt; return {}; }
    return stage;
}

Failure NativeChatKeyVault::putCreateStage(const Lease &value, const CreateStage &stage)
{
    if (!accepts(value)) return Failure::Superseded;
    const auto record = createStageAddress(stage.attempt);
    if (record.isEmpty() || !isUuid(stage.key) || stage.groupKey.size() != 32) return Failure::InvalidKey;
    if (stage.createdAt < 0 || quint64(stage.createdAt) > MaximumRevision) return Failure::InvalidKey;
    auto captured = stage; captured.attempt = captured.attempt.toLower(); captured.key = captured.key.toLower();
    auto encoded = encodeCreateStage(captured);
    if (encoded.size() > MaximumRecordBytes) { cleanse(encoded); return Failure::Capacity; }
    if (!m_database.transaction()) { cleanse(encoded); return Failure::Unavailable; }
    Failure result = Failure::None;
    auto old = readCreateStageRecord(captured.attempt, result);
    // The staged attempt is immutable: a different key or identity for the
    // same attempt is a conflict, an identical rewrite is idempotent.
    if (result == Failure::None && old && encodeCreateStage(*old) != encoded) result = Failure::Conflict;
    if (result == Failure::None && !old) {
        const auto prefix = createStagePrefix();
        QSqlQuery query(m_database);
        query.prepare("SELECT COUNT(*) FROM secrets WHERE address>=? AND address<? AND address!=?");
        query.addBindValue(prefix); query.addBindValue(prefix.chopped(1) + '-'); query.addBindValue(record);
        if (!query.exec() || !query.next()) result = Failure::Unavailable;
        else if (query.value(0).toLongLong() >= MaximumCreateStages) result = Failure::Capacity;
    }
    if (result == Failure::None) result = writeRecord(record, encoded);
    cleanse(encoded);
    if (old) cleanse(old->groupKey);
    return finishTransaction(result);
}

NativeChatKeyVault::CreateStageResult NativeChatKeyVault::readCreateStage(const Lease &value, const QString &attempt)
{
    if (!accepts(value)) return {Failure::Superseded, {}};
    if (createStageAddress(attempt).isEmpty()) return {Failure::InvalidAddress, {}};
    Failure failure = Failure::None;
    auto stage = readCreateStageRecord(attempt.toLower(), failure);
    if (!stage && failure == Failure::None) failure = Failure::MissingKey;
    return {failure, std::move(stage)};
}

NativeChatKeyVault::CreateStageListResult NativeChatKeyVault::createStages(const Lease &value)
{
    if (!accepts(value)) return {Failure::Superseded, {}};
    const auto prefix = createStagePrefix();
    QSqlQuery query(m_database);
    query.prepare("SELECT address FROM secrets WHERE address>=? AND address<? ORDER BY address LIMIT ?");
    query.addBindValue(prefix); query.addBindValue(prefix.chopped(1) + '-'); query.addBindValue(MaximumCreateStages + 1);
    if (!query.exec()) return {Failure::Unavailable, {}};
    QList<QString> records;
    while (query.next()) {
        records.append(query.value(0).toString());
        if (records.size() > MaximumCreateStages) return {Failure::Capacity, {}};
    }
    query.finish();
    CreateStageListResult result{Failure::None, {}};
    for (const auto &record : records) {
        Failure failure = Failure::None;
        auto bytes = readRecord(record, failure);
        if (!bytes) return {failure == Failure::None ? Failure::Corrupt : failure, {}};
        auto stage = decodeCreateStage(*bytes); cleanse(*bytes);
        if (!stage || createStageAddress(stage->attempt) != record) return {Failure::Corrupt, {}};
        result.values.append(std::move(*stage));
    }
    return result;
}

Failure NativeChatKeyVault::clearCreateStage(const Lease &value, const QString &attempt)
{
    if (!accepts(value)) return Failure::Superseded;
    const auto record = createStageAddress(attempt);
    if (record.isEmpty()) return Failure::InvalidAddress;
    if (!m_database.transaction()) return Failure::Unavailable;
    QSqlQuery query(m_database);
    query.prepare("DELETE FROM secrets WHERE address=?");
    query.addBindValue(record);
    Failure result = query.exec() ? Failure::None : Failure::Unavailable;
    return finishTransaction(result);
}

} // namespace workspace
