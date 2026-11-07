#include "native-chat-key-vault.h"
#include "native-chat-crypto.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QSqlQuery>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUuid>
#include <deque>

using namespace workspace;
using Vault = NativeChatKeyVault;
using Failure = Vault::Failure;
namespace {
const QString Project("10000000-0000-4000-8000-000000000001");
const QString User("20000000-0000-4000-8000-000000000002");
const QString OtherUser("20000000-0000-4000-8000-000000000003");
const QUrl Server("https://workspace.example.test");
const Vault::Address Address{"30000000-0000-4000-8000-000000000003", "40000000-0000-4000-8000-000000000004"};
class Secrets final : public SessionSecretStore {
public:
    QMap<QString, QByteArray> values;
    std::deque<std::function<void()>> writes;
    bool holdWrites = false;
    Error failure = Error::None;
    int reads = 0, writeCount = 0;
    void read(const QString &key, Loaded done) override {
        ++reads;
        const auto error = failure != Error::None ? failure : values.contains(key) ? Error::None : Error::NotFound;
        const auto bytes = values.value(key);
        QTimer::singleShot(0, this, [done, error, bytes] { done(bytes, error); });
    }
    void write(const QString &key, const QByteArray &bytes, Completion done) override {
        ++writeCount;
        auto action = [this, key, bytes, done] { if (failure == Error::None) values.insert(key, bytes); done(failure); };
        if (holdWrites) writes.push_back(action);
        else QTimer::singleShot(0, this, std::move(action));
    }
    void remove(const QString &key, Completion done) override { values.remove(key); done(Error::None); }
    void finishWrite() { auto action = std::move(writes.front()); writes.pop_front(); action(); }
};

QString databasePath(const QString &directory, const QString &user = User)
{
    return directory + '/' + SessionVault::accountId(Server, Project, user) + ".sqlite3";
}

bool modify(const QString &path, const QString &sql)
{
    const auto connection = QUuid::createUuid().toString(); bool result = false;
    {
        auto database = QSqlDatabase::addDatabase("QSQLITE", connection); database.setDatabaseName(path);
        if (database.open()) { QSqlQuery query(database); result = query.exec(sql); }
        database.close();
    }
    QSqlDatabase::removeDatabase(connection); return result;
}

Vault::PendingRequest request()
{
    const auto pair = NativeChatCrypto::createRequestKeyPair();
    return {QUuid::createUuid().toString(QUuid::WithoutBraces), {}, "Frozen synthetic device", pair.publicKey,
        pair.privateKey, 123456789, 0, {}};
}

QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

bool replaceGroup(const QString &directory, Secrets &backend, const Vault &vault,
                  const Vault::Address &address, const QJsonObject &object)
{
    const auto record = QString::fromUtf8(QJsonDocument(QJsonArray{"group", vault.device().id,
        address.stream, address.key}).toJson(QJsonDocument::Compact));
    const auto owner = SessionVault::accountId(Server, Project, User);
    auto sealed = NativeChatCrypto::encryptMessage(backend.values.value("chat-master-v1:" + owner),
        QJsonDocument(object).toJson(QJsonDocument::Compact), {"workspace-native-key-vault-v1:" + owner, record});
    return sealed && modify(databasePath(directory), "UPDATE secrets SET payload=X'"
        + QString::fromLatin1((QByteArray("WSKV\x01", 5) + sealed.bytes).toHex()) + "' WHERE address='" + record + "'");
}

QByteArray storedGroup(const QString &directory, const Vault &vault, const Vault::Address &address)
{
    const auto connection = uuid(); QByteArray bytes;
    const auto record = QString::fromUtf8(QJsonDocument(QJsonArray{"group", vault.device().id,
        address.stream, address.key}).toJson(QJsonDocument::Compact));
    {
        auto database = QSqlDatabase::addDatabase("QSQLITE", connection); database.setDatabaseName(databasePath(directory));
        if (database.open()) {
            QSqlQuery query(database); query.prepare("SELECT payload FROM secrets WHERE address=?"); query.addBindValue(record);
            if (query.exec() && query.next()) bytes = query.value(0).toByteArray();
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(connection); return bytes;
}

bool insertGenerationFixture(const QString &directory, Secrets &backend, const Vault &vault, int first, int count)
{
    const auto connection = uuid(); bool okay = false;
    const auto owner = SessionVault::accountId(Server, Project, User);
    {
        auto database = QSqlDatabase::addDatabase("QSQLITE", connection); database.setDatabaseName(databasePath(directory));
        okay = database.open() && database.transaction();
        {
            QSqlQuery query(database); query.prepare("INSERT INTO secrets(address,payload) VALUES(?,?)");
            for (int i = first; okay && i < first + count; ++i) {
                const auto id = QString("a0000000-0000-4000-8000-%1").arg(i, 12, 16, QLatin1Char('0'));
                const auto record = QString::fromUtf8(QJsonDocument(QJsonArray{"group", vault.device().id,
                    Address.stream, id}).toJson(QJsonDocument::Compact));
                auto sealed = NativeChatCrypto::encryptMessage(backend.values.value("chat-master-v1:" + owner),
                    QJsonDocument(QJsonObject{{"key", QString::fromLatin1(QByteArray(32, 'k').toBase64())},
                        {"receivedAt", 123}, {"deletionRevision", 0}, {"sendOrder", 0}}).toJson(QJsonDocument::Compact),
                    {"workspace-native-key-vault-v1:" + owner, record});
                if (!sealed) { okay = false; break; }
                query.bindValue(0, record); query.bindValue(1, QByteArray("WSKV\x01", 5) + sealed.bytes);
                okay = query.exec();
            }
        }
        if (okay) okay = database.commit(); else database.rollback();
        database.close();
    }
    QSqlDatabase::removeDatabase(connection); return okay;
}

Vault::Completion granted(const Vault::PendingRequest &pending)
{
    return {pending.localId, pending.messageId, uuid(), Vault::Outcome::Granted};
}
} // namespace

class NativeChatKeyVaultTest final : public QObject {
    Q_OBJECT
private:
    void open(Vault &vault, Failure expected = Failure::None, const QString &user = User) {
        bool done = false; Failure actual = Failure::Unavailable;
        vault.open(Server, Project, user, "Synthetic Linux device", [&](Failure error) { actual = error; done = true; });
        QTRY_VERIFY(done); QCOMPARE(actual, expected);
    }
private slots:
    void loadedPendingCopyScrubsPrivateAndRecoveryWithoutChangingDurableMetadata()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend); open(vault);
        auto saved = request(); saved.targetTopic = "50000000-0000-4000-8000-000000000005";
        saved.recovery = Vault::Recovery{QByteArray(128, 'e'), QByteArray(32, 'p')};
        QCOMPARE(vault.putPendingRequest(vault.lease(), Address, saved), Failure::None);
        auto loaded = vault.pendingRequests(vault.lease(), Address); QCOMPARE(loaded.values.size(), 1);
        auto &copy = loaded.values.first(); copy.clearSecrets();
        QVERIFY(copy.privateKey.isEmpty()); QVERIFY(copy.recovery->previousKey.isEmpty());
        QCOMPARE(copy.localId, saved.localId); QCOMPARE(copy.targetTopic, saved.targetTopic); QCOMPARE(copy.publicKey, saved.publicKey);
        const auto durable = vault.pendingRequests(vault.lease(), Address); QCOMPARE(durable.values.size(), 1);
        QVERIFY(durable.values.first().privateKey == saved.privateKey);
        QVERIFY(durable.values.first().recovery->previousKey == saved.recovery->previousKey);
    }
    void rolledBackWritesNeverPublishMaterial()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend); open(vault);
        QSignalSpy changes(&vault, &Vault::keyMaterialChanged);
        auto pending = request(); pending.messageId = uuid();
        QCOMPARE(vault.putPendingRequest(vault.lease(), Address, pending), Failure::None);
        QVERIFY(modify(databasePath(directory.path()), "CREATE TRIGGER fail_write BEFORE INSERT ON secrets BEGIN SELECT RAISE(ABORT,'synthetic write failure'); END"));
        QCOMPARE(vault.put(vault.lease(), Address, QByteArray(32, 'a'), 0), Failure::Unavailable);
        QCOMPARE(vault.remove(vault.lease(), Address), Failure::Unavailable);
        QCOMPARE(vault.completeRequest(vault.lease(), Address, granted(pending), QByteArray(32, 'g')), Failure::Unavailable);
        QVERIFY(changes.isEmpty()); QVERIFY(vault.read(vault.lease(), Address).value.bytes.isEmpty());
        QCOMPARE(vault.pendingRequests(vault.lease(), Address).values.size(), 1);
        QVERIFY(!vault.requestCompletion(vault.lease(), Address, pending.localId).value);
    }

    void committedMaterialNotifiesWithoutOrderOrNoopNoise()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend); open(vault);
        QSignalSpy changes(&vault, &Vault::keyMaterialChanged);
        int committed = 0;
        connect(&vault, &Vault::keyMaterialChanged, &vault, [&](const QString &stream, const QString &key) {
            QCOMPARE(stream, Address.stream); QCOMPARE(key, Address.key);
            const auto read = vault.read(vault.lease(), Address); QCOMPARE(read.failure, Failure::None);
            if (++committed == 1) QVERIFY(read.value.bytes == QByteArray(32, 'a'));
            else { QVERIFY(read.value.bytes.isEmpty()); QCOMPARE(read.value.deletionRevision, quint64(1)); }
        });
        QCOMPARE(vault.put(vault.lease(), Address, QByteArray(32, 'a'), 0), Failure::None);
        QCOMPARE(changes.size(), 1);
        QCOMPARE(vault.put(vault.lease(), Address, QByteArray(32, 'a'), 0), Failure::None);
        QCOMPARE(vault.markForSending(vault.lease(), Address, 0), Failure::None);
        QCOMPARE(vault.put(vault.lease(), Address, QByteArray(32, 'b'), 0), Failure::Conflict);
        QCOMPARE(changes.size(), 1);
        QCOMPARE(vault.remove(vault.lease(), Address), Failure::None); QCOMPARE(changes.size(), 2);
        QCOMPARE(vault.put(vault.lease(), Address, QByteArray(32, 'a'), 0), Failure::Superseded);
        QCOMPARE(changes.size(), 2);
    }

    void completionNotifiesOnlyForNewCommittedMaterial()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend); open(vault);
        QSignalSpy changes(&vault, &Vault::keyMaterialChanged);
        auto pending = request(); pending.messageId = uuid();
        QCOMPARE(vault.putPendingRequest(vault.lease(), Address, pending), Failure::None);
        const auto completion = granted(pending);
        bool receiptVisible = false;
        connect(&vault, &Vault::keyMaterialChanged, &vault, [&](const QString &, const QString &) {
            receiptVisible = vault.requestCompletion(vault.lease(), Address, pending.localId).value.has_value()
                && vault.pendingRequests(vault.lease(), Address).values.isEmpty();
        });
        QCOMPARE(vault.completeRequest(vault.lease(), Address, completion, QByteArray(32, 'g')), Failure::None);
        QCOMPARE(changes.size(), 1); QVERIFY(receiptVisible);
        QCOMPARE(vault.completeRequest(vault.lease(), Address, completion, QByteArray(32, 'g')), Failure::None);
        QCOMPARE(changes.size(), 1);
        pending = request(); pending.messageId = uuid();
        QCOMPARE(vault.putPendingRequest(vault.lease(), Address, pending), Failure::None);
        Vault::Completion rejected{pending.localId, pending.messageId, uuid(), Vault::Outcome::Rejected};
        QCOMPARE(vault.completeRequest(vault.lease(), Address, rejected), Failure::None);
        QCOMPARE(changes.size(), 1); QVERIFY(vault.read(vault.lease(), Address).value.bytes == QByteArray(32, 'g'));
    }

    void lifetimeNotificationsExposeCoherentOpenAndClose()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend);
        QSignalSpy changes(&vault, &Vault::stateChanged);
        bool readySeen = false, closedSeen = false;
        connect(&vault, &Vault::stateChanged, &vault, [&] {
            if (vault.available()) { readySeen = true; QVERIFY(!vault.lease().owner.isEmpty()); QVERIFY(!vault.device().id.isEmpty()); }
            else { closedSeen = true; QVERIFY(vault.lease().owner.isEmpty()); QVERIFY(vault.device().id.isEmpty()); }
        });
        open(vault); QVERIFY(readySeen); QVERIFY(closedSeen); QVERIFY(changes.size() >= 2);
        const auto before = changes.size(); const auto lease = vault.lease();
        vault.close(); QCOMPARE(changes.size(), before + 1); QVERIFY(!vault.available());
        QCOMPARE(vault.read(lease, Address).failure, Failure::Superseded);
    }

    void materialSubscriberCanRemoveThenDestroyVault()
    {
        QTemporaryDir directory; Secrets backend; auto *vault = new Vault(directory.path(), &backend); open(*vault);
        QPointer<Vault> guard(vault); const auto lease = vault->lease(); int calls = 0;
        connect(vault, &Vault::keyMaterialChanged, this, [&](const QString &, const QString &) {
            if (++calls == 1) QCOMPARE(vault->remove(lease, Address), Failure::None);
            else { delete vault; vault = nullptr; }
        });
        const auto result = vault->put(lease, Address, QByteArray(32, 'a'), 0);
        QCOMPARE(result, Failure::None); QCOMPARE(calls, 2); QVERIFY(!guard);
        Vault reopened(directory.path(), &backend); open(reopened);
        const auto read = reopened.read(reopened.lease(), Address); QVERIFY(read.value.bytes.isEmpty());
        QCOMPARE(read.value.deletionRevision, quint64(1));
    }

    void lifetimeSubscriberCanDestroyOrReplaceOpeningOwner()
    {
        QTemporaryDir directory; Secrets backend; auto *vault = new Vault(directory.path(), &backend);
        QPointer<Vault> guard(vault); bool done = false;
        connect(vault, &Vault::stateChanged, this, [&] { if (vault->available()) { delete vault; vault = nullptr; } });
        vault->open(Server, Project, User, "Synthetic", [&](Failure) { done = true; });
        QTRY_VERIFY(!guard); QVERIFY(!done);
        Vault next(directory.path(), &backend); open(next);
        bool switched = false, attempted = false, staleDone = false;
        connect(&next, &Vault::stateChanged, &next, [&] {
            if (!next.available() && !attempted) {
                attempted = true;
                next.open(Server, Project, OtherUser, "Other", [&](Failure status) { QCOMPARE(status, Failure::None); switched = true; });
            }
        });
        next.open(Server, Project, User, "Superseded", [&](Failure status) { QCOMPARE(status, Failure::Superseded); staleDone = true; });
        QTRY_VERIFY(switched); QTRY_VERIFY(staleDone);
        QCOMPARE(next.lease().owner, SessionVault::accountId(Server, Project, OtherUser));
    }

    void legacyRequestWithoutTargetDoesNotInventOne()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend); bool opened = false;
        vault.open(Server, Project, User, "Legacy device", [&](Failure result) { opened = result == Failure::None; }); QTRY_VERIFY(opened);
        auto pending = request(); QCOMPARE(vault.putPendingRequest(vault.lease(), Address, pending), Failure::None);
        const auto loaded = vault.pendingRequests(vault.lease(), Address); QCOMPARE(loaded.failure, Failure::None); QCOMPARE(loaded.values.size(), 1);
        QVERIFY(loaded.values.first().targetTopic.isEmpty());
        pending.targetTopic = "50000000-0000-4000-8000-000000000005";
        QCOMPARE(vault.putPendingRequest(vault.lease(), Address, pending), Failure::Conflict);
    }

    void originalRequestTargetTopicSurvivesBindingAndReopen()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend);
        bool opened = false; Failure status = Failure::Unavailable;
        vault.open(Server, Project, User, "Target topic device", [&](Failure result) { status = result; opened = true; });
        QTRY_VERIFY(opened); QCOMPARE(status, Failure::None);
        auto pending = request(); pending.targetTopic = "50000000-0000-4000-8000-000000000005";
        QCOMPARE(vault.putPendingRequest(vault.lease(), Address, pending), Failure::None);
        auto values = vault.pendingRequests(vault.lease(), Address); QCOMPARE(values.failure, Failure::None); QCOMPARE(values.values.size(), 1);
        QCOMPARE(values.values.first().targetTopic, pending.targetTopic);
        const auto publicId = uuid(); QCOMPARE(vault.bindPendingRequest(vault.lease(), Address, pending.localId, publicId), Failure::None);
        vault.close(); opened = false;
        vault.open(Server, Project, User, "Another label", [&](Failure result) { status = result; opened = true; });
        QTRY_VERIFY(opened); QCOMPARE(status, Failure::None);
        values = vault.pendingRequests(vault.lease(), Address); QCOMPARE(values.values.size(), 1);
        QCOMPARE(values.values.first().targetTopic, pending.targetTopic); QCOMPARE(values.values.first().messageId, publicId);
    }

    void originalRequestTargetTopicIsImmutableAndRejectsMalformedUuid()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend); bool opened = false;
        vault.open(Server, Project, User, "Target device", [&](Failure result) { opened = result == Failure::None; }); QTRY_VERIFY(opened);
        auto pending = request(); pending.targetTopic = "50000000-0000-4000-8000-000000000005";
        QCOMPARE(vault.putPendingRequest(vault.lease(), Address, pending), Failure::None);
        pending.targetTopic = "60000000-0000-4000-8000-000000000006";
        QCOMPARE(vault.putPendingRequest(vault.lease(), Address, pending), Failure::Conflict);
        pending = request(); pending.targetTopic = "not-a-uuid";
        QCOMPARE(vault.putPendingRequest(vault.lease(), Address, pending), Failure::InvalidKey);
    }

    void init() { QTest::failOnWarning(); }

    void generationMetadataAndSendingSelectionSurviveRestartAndGrantArrival()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend); open(vault);
        const Vault::Address older{Address.stream, "10000000-0000-4000-8000-000000000001"};
        const Vault::Address newer{Address.stream, "20000000-0000-4000-8000-000000000002"};
        const QByteArray a(32, 'a'), b(32, 'b'), c(32, 'c');
        QCOMPARE(vault.listKeys(vault.lease(), Address.stream).failure, Failure::None);
        QVERIFY(vault.listKeys(vault.lease(), Address.stream).values.isEmpty());
        QCOMPARE(vault.put(vault.lease(), older, a, 0), Failure::None);
        QCOMPARE(vault.put(vault.lease(), newer, b, 0), Failure::None);
        const auto initial = vault.listKeys(vault.lease(), Address.stream);
        QCOMPARE(initial.failure, Failure::None); QCOMPARE(initial.values.size(), 2);
        for (int i = 1; i < initial.values.size(); ++i)
            QVERIFY(initial.values[i - 1].receivedAt > initial.values[i].receivedAt
                || (initial.values[i - 1].receivedAt == initial.values[i].receivedAt
                    && initial.values[i - 1].key < initial.values[i].key));
        QCOMPARE(vault.lastAvailableKey(vault.lease(), Address.stream).key, initial.values.first().key);
        const auto received = vault.read(vault.lease(), older).value.receivedAt;
        QCOMPARE(vault.markForSending(vault.lease(), older, 0), Failure::None);
        QCOMPARE(vault.lastAvailableKey(vault.lease(), Address.stream).key, older.key);
        auto pending = request(); pending.messageId = uuid();
        QCOMPARE(vault.putPendingRequest(vault.lease(), Address, pending), Failure::None);
        QCOMPARE(vault.completeRequest(vault.lease(), Address, granted(pending), c), Failure::None);
        QCOMPARE(vault.put(vault.lease(), older, a, 0), Failure::None);
        QCOMPARE(vault.lastAvailableKey(vault.lease(), Address.stream).key, older.key);
        QCOMPARE(vault.read(vault.lease(), older).value.receivedAt, received);
        QCOMPARE(vault.read(vault.lease(), older).value.sendOrder, quint64(1));
        vault.close(); open(vault);
        const auto latest = vault.lastAvailableKey(vault.lease(), Address.stream);
        QCOMPARE(latest.failure, Failure::None); QCOMPARE(latest.key, older.key); QVERIFY(latest.value.bytes == a);
        QCOMPARE(vault.markForSending(vault.lease(), newer, 0), Failure::None);
        QCOMPARE(vault.read(vault.lease(), newer).value.sendOrder, quint64(2));
        QCOMPARE(vault.lastAvailableKey(vault.lease(), Address.stream).key, newer.key);
    }

    void sendingMetadataCannotRestoreDeletedSecretOrCrossOwnerAndStream()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend); open(vault);
        const QByteArray secret(32, 'k');
        QCOMPARE(vault.put(vault.lease(), Address, secret, 0), Failure::None);
        QCOMPARE(vault.markForSending(vault.lease(), Address, 0), Failure::None);
        const auto old = vault.lease();
        QCOMPARE(vault.remove(old, Address), Failure::None);
        QCOMPARE(vault.markForSending(old, Address, 0), Failure::Superseded);
        QCOMPARE(vault.markForSending(old, Address, 1), Failure::MissingKey);
        QCOMPARE(vault.read(old, Address).value.sendOrder, quint64(0));
        QVERIFY(vault.listKeys(old, Address.stream).values.isEmpty());
        const auto missing = vault.lastAvailableKey(old, Address.stream);
        QCOMPARE(missing.failure, Failure::None); QVERIFY(missing.key.isEmpty()); QVERIFY(missing.value.bytes.isEmpty());
        QCOMPARE(vault.put(old, Address, secret, 1), Failure::None);
        const Vault::Address otherStream{uuid(), Address.key};
        QCOMPARE(vault.put(old, otherStream, QByteArray(32, 'o'), 0), Failure::None);
        QCOMPARE(vault.markForSending(old, otherStream, 0), Failure::None);
        QCOMPARE(vault.read(old, otherStream).value.sendOrder, quint64(1));
        open(vault, Failure::None, OtherUser);
        QCOMPARE(vault.markForSending(old, Address, 1), Failure::Superseded);
        QCOMPARE(vault.listKeys(old, Address.stream).failure, Failure::Superseded);
        QCOMPARE(vault.lastAvailableKey(old, Address.stream).failure, Failure::Superseded);
        QVERIFY(vault.listKeys(vault.lease(), Address.stream).values.isEmpty());
        QCOMPARE(vault.listKeys(vault.lease(), "bad").failure, Failure::InvalidAddress);
        QCOMPARE(vault.lastAvailableKey(vault.lease(), "bad").failure, Failure::InvalidAddress);
        open(vault); QCOMPARE(vault.listKeys(vault.lease(), Address.stream).values.size(), 1);
    }

    void failedSendingMetadataWritePreservesPreviouslySelectedGeneration()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend); open(vault);
        const Vault::Address second{Address.stream, uuid()};
        QCOMPARE(vault.put(vault.lease(), Address, QByteArray(32, 'a'), 0), Failure::None);
        QCOMPARE(vault.put(vault.lease(), second, QByteArray(32, 'b'), 0), Failure::None);
        QCOMPARE(vault.markForSending(vault.lease(), Address, 0), Failure::None);
        QVERIFY(modify(databasePath(directory.path()), "CREATE TRIGGER fail_send BEFORE INSERT ON secrets WHEN NEW.address LIKE '[\"group\"%' BEGIN SELECT RAISE(ABORT,'injected'); END"));
        QCOMPARE(vault.markForSending(vault.lease(), second, 0), Failure::Unavailable);
        QCOMPARE(vault.lastAvailableKey(vault.lease(), Address.stream).key, Address.key);
        QCOMPARE(vault.read(vault.lease(), second).value.sendOrder, quint64(0));
        QVERIFY(modify(databasePath(directory.path()), "DROP TRIGGER fail_send"));
        QCOMPARE(vault.markForSending(vault.lease(), second, 0), Failure::None);
        QCOMPARE(vault.read(vault.lease(), second).value.sendOrder, quint64(2));
    }

    void invalidSendingMetadataFailsWithoutPartialGenerationLists_data()
    {
        QTest::addColumn<QJsonValue>("order");
        QTest::newRow("negative") << QJsonValue(-1);
        QTest::newRow("fractional") << QJsonValue(1.5);
        QTest::newRow("above-safe-integer") << QJsonValue(9007199254740992.0);
        QTest::newRow("string") << QJsonValue("1");
        QTest::newRow("null") << QJsonValue(QJsonValue::Null);
        QTest::newRow("boolean") << QJsonValue(true);
    }
    void invalidSendingMetadataFailsWithoutPartialGenerationLists()
    {
        QFETCH(QJsonValue, order);
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend); open(vault);
        const Vault::Address second{Address.stream, uuid()}; const QByteArray key(32, 'k');
        QCOMPARE(vault.put(vault.lease(), Address, key, 0), Failure::None);
        QCOMPARE(vault.put(vault.lease(), second, key, 0), Failure::None);
        QVERIFY(replaceGroup(directory.path(), backend, vault, second,
            {{"key", QString::fromLatin1(key.toBase64())}, {"receivedAt", 123}, {"deletionRevision", 0}, {"sendOrder", order}}));
        const auto metadata = vault.listKeys(vault.lease(), Address.stream);
        QCOMPARE(metadata.failure, Failure::Corrupt); QVERIFY(metadata.values.isEmpty());
        const auto latest = vault.lastAvailableKey(vault.lease(), Address.stream);
        QCOMPARE(latest.failure, Failure::Corrupt); QVERIFY(latest.key.isEmpty()); QVERIFY(latest.value.bytes.isEmpty());
        QCOMPARE(vault.markForSending(vault.lease(), Address, 0), Failure::Corrupt);
        QCOMPARE(vault.read(vault.lease(), Address).value.sendOrder, quint64(0));
    }

    void maximumSendOrderCannotOverflowOrChangeAnExistingSelection()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend); open(vault);
        const Vault::Address second{Address.stream, uuid()}; const QByteArray key(32, 'k');
        QCOMPARE(vault.put(vault.lease(), Address, key, 0), Failure::None);
        QCOMPARE(vault.put(vault.lease(), second, key, 0), Failure::None);
        QVERIFY(replaceGroup(directory.path(), backend, vault, Address,
            {{"key", QString::fromLatin1(key.toBase64())}, {"receivedAt", 123}, {"deletionRevision", 0}, {"sendOrder", 9007199254740991.0}}));
        QCOMPARE(vault.markForSending(vault.lease(), second, 0), Failure::Capacity);
        QCOMPARE(vault.read(vault.lease(), second).value.sendOrder, quint64(0));
        QCOMPARE(vault.lastAvailableKey(vault.lease(), Address.stream).key, Address.key);
        QCOMPARE(vault.markForSending(vault.lease(), Address, 0), Failure::Capacity);
    }

    void legacyVersionTwoGroupWithoutSendingMetadataMigratesWithoutRewritingCiphertext()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend); open(vault);
        const QByteArray key(32, 'k');
        QCOMPARE(vault.put(vault.lease(), Address, key, 0), Failure::None);
        QVERIFY(replaceGroup(directory.path(), backend, vault, Address,
            {{"key", QString::fromLatin1(key.toBase64())}, {"receivedAt", 123}, {"deletionRevision", 0}}));
        const auto before = storedGroup(directory.path(), vault, Address); QVERIFY(!before.isEmpty());
        vault.close(); QVERIFY(modify(databasePath(directory.path()), "PRAGMA user_version=2")); open(vault);
        QCOMPARE(storedGroup(directory.path(), vault, Address), before);
        const auto metadata = vault.listKeys(vault.lease(), Address.stream);
        QCOMPARE(metadata.failure, Failure::None); QCOMPARE(metadata.values.size(), 1);
        QCOMPARE(metadata.values.first().key, Address.key); QCOMPARE(metadata.values.first().receivedAt, qint64(123));
        QCOMPARE(vault.read(vault.lease(), Address).value.sendOrder, quint64(0));
        QCOMPARE(vault.lastAvailableKey(vault.lease(), Address.stream).key, Address.key);
        QCOMPARE(vault.markForSending(vault.lease(), Address, 0), Failure::None);
        vault.close(); open(vault); QCOMPARE(vault.read(vault.lease(), Address).value.sendOrder, quint64(1));
    }

    void nonzeroSendingOrderOnATombstoneAndSwappedGenerationAddressesAreCorrupt()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend); open(vault);
        QCOMPARE(vault.put(vault.lease(), Address, QByteArray(32, 'k'), 0), Failure::None);
        QCOMPARE(vault.remove(vault.lease(), Address), Failure::None);
        QVERIFY(replaceGroup(directory.path(), backend, vault, Address,
            {{"key", QJsonValue(QJsonValue::Null)}, {"receivedAt", 0}, {"deletionRevision", 1}, {"sendOrder", 1}}));
        QCOMPARE(vault.listKeys(vault.lease(), Address.stream).failure, Failure::Corrupt);
        QCOMPARE(vault.lastAvailableKey(vault.lease(), Address.stream).failure, Failure::Corrupt);
        QVERIFY(replaceGroup(directory.path(), backend, vault, Address,
            {{"key", QString::fromLatin1(QByteArray(32, 'k').toBase64())}, {"receivedAt", 123}, {"deletionRevision", 1}, {"sendOrder", 1}}));
        QVERIFY(modify(databasePath(directory.path()), "UPDATE secrets SET address=replace(address,'" + Address.key + "','" + uuid() + "') WHERE address LIKE '[\"group\"%'"));
        const auto result = vault.listKeys(vault.lease(), Address.stream);
        QCOMPARE(result.failure, Failure::Corrupt); QVERIFY(result.values.isEmpty());
        const auto latest = vault.lastAvailableKey(vault.lease(), Address.stream);
        QCOMPARE(latest.failure, Failure::Corrupt); QVERIFY(latest.value.bytes.isEmpty());
    }

    void moreThan4096GenerationsRemainEnumerableAndTombstonesDoNotBlockOtherKeys()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend); open(vault);
        QVERIFY(insertGenerationFixture(directory.path(), backend, vault, 0, 4096));
        const Vault::Address first{Address.stream, "a0000000-0000-4000-8000-000000000000"};
        const auto accepted = vault.listKeys(vault.lease(), Address.stream);
        QCOMPARE(accepted.failure, Failure::None); QCOMPARE(accepted.values.size(), 4096);
        QCOMPARE(accepted.values.first().key, first.key);
        QCOMPARE(vault.markForSending(vault.lease(), first, 0), Failure::None);
        QCOMPARE(vault.lastAvailableKey(vault.lease(), Address.stream).key, first.key);
        QVERIFY(insertGenerationFixture(directory.path(), backend, vault, 4096, 1));
        const auto expanded = vault.listKeys(vault.lease(), Address.stream);
        QCOMPARE(expanded.failure, Failure::None); QCOMPARE(expanded.values.size(), 4097);
        const auto latest = vault.lastAvailableKey(vault.lease(), Address.stream);
        QCOMPARE(latest.failure, Failure::None); QCOMPARE(latest.key, first.key); QVERIFY(latest.value.bytes == QByteArray(32, 'k'));
        QCOMPARE(vault.markForSending(vault.lease(), first, 0), Failure::None);
        QCOMPARE(vault.read(vault.lease(), first).value.sendOrder, quint64(2));
        const Vault::Address extra{Address.stream, "a0000000-0000-4000-8000-000000001000"};
        QCOMPARE(vault.remove(vault.lease(), extra), Failure::None);
        const auto remaining = vault.listKeys(vault.lease(), Address.stream);
        QCOMPARE(remaining.failure, Failure::None); QCOMPARE(remaining.values.size(), 4096);
        QCOMPARE(vault.markForSending(vault.lease(), first, 0), Failure::None);
        QCOMPARE(vault.read(vault.lease(), first).value.sendOrder, quint64(3));
        QVERIFY(vault.read(vault.lease(), first).value.bytes == QByteArray(32, 'k'));
    }

    void pendingRequestRestoresFrozenWireFieldsAndRepairEvidence()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend); open(vault);
        auto pending = request(); pending.messageId = uuid();
        pending.recovery = Vault::Recovery{QByteArray(4096, 'p'), QByteArray(32, 'k')};
        QCOMPARE(vault.putPendingRequest(vault.lease(), Address, pending), Failure::None);
        vault.close(); open(vault);
        const auto rows = vault.pendingRequests(vault.lease(), Address);
        QCOMPARE(rows.failure, Failure::None); QCOMPARE(rows.values.size(), 1);
        const auto restored = rows.values.first(); QCOMPARE(restored.localId, pending.localId);
        QCOMPARE(restored.messageId, pending.messageId); QCOMPARE(restored.deviceName, pending.deviceName);
        QVERIFY(restored.publicKey == pending.publicKey); QVERIFY(restored.privateKey == pending.privateKey);
        QCOMPARE(restored.createdAt, pending.createdAt); QVERIFY(restored.recovery);
        QVERIFY(restored.recovery->envelope == pending.recovery->envelope);
        QVERIFY(restored.recovery->previousKey == pending.recovery->previousKey);
        QFile file(databasePath(directory.path())); QVERIFY(file.open(QIODevice::ReadOnly)); const auto disk = file.readAll();
        QVERIFY(!disk.contains(pending.privateKey)); QVERIFY(!disk.contains(pending.privateKey.toBase64()));
        QVERIFY(!disk.contains(pending.recovery->previousKey)); QVERIFY(!disk.contains(pending.recovery->envelope.toBase64()));
        QVERIFY(!disk.contains(pending.deviceName.toUtf8()));
    }

    void bindingIsStableAndRepairsOnlyLegacyCanonicalPlacement()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend); open(vault);
        auto pending = request(); const auto placement = uuid();
        QCOMPARE(vault.putPendingRequest(vault.lease(), Address, pending), Failure::None);
        QCOMPARE(vault.bindPendingRequest(vault.lease(), Address, pending.localId, placement), Failure::None);
        QCOMPARE(vault.bindPendingRequest(vault.lease(), Address, pending.localId, placement), Failure::None);
        QCOMPARE(vault.bindPendingRequest(vault.lease(), Address, pending.localId, uuid()), Failure::Conflict);
        QCOMPARE(vault.putPendingRequest(vault.lease(), Address, pending), Failure::Conflict);
        QCOMPARE(vault.bindPendingRequest(vault.lease(), Address, uuid(), placement), Failure::MissingRequest);
        auto legacy = request(); legacy.messageId = legacy.localId;
        QCOMPARE(vault.putPendingRequest(vault.lease(), Address, legacy), Failure::None);
        const auto corrected = uuid(); QCOMPARE(vault.bindPendingRequest(vault.lease(), Address, legacy.localId, corrected), Failure::None);
        vault.close(); open(vault); const auto rows = vault.pendingRequests(vault.lease(), Address).values;
        QCOMPARE(rows.size(), 2);
        for (const auto &row : rows) QCOMPARE(row.messageId, row.localId == pending.localId ? placement : corrected);
    }

    void grantAtomicallyStoresKeyAndBothReceiptsAndReplayCannotRestoreDeletion()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend); open(vault);
        auto pending = request(); pending.messageId = uuid(); const auto completion = granted(pending);
        const auto key = NativeChatCrypto::createGroupKey(); QVERIFY(key);
        QCOMPARE(vault.putPendingRequest(vault.lease(), Address, pending), Failure::None);
        QCOMPARE(vault.completeRequest(vault.lease(), Address, completion, key.bytes), Failure::None);
        QCOMPARE(vault.pendingRequests(vault.lease(), Address).values.size(), 0);
        QVERIFY(vault.read(vault.lease(), Address).value.bytes == key.bytes);
        const auto receivedAt = vault.read(vault.lease(), Address).value.receivedAt; QVERIFY(receivedAt > 0);
        const auto byLocal = vault.requestCompletion(vault.lease(), Address, pending.localId);
        const auto byMessage = vault.requestCompletionByMessage(vault.lease(), Address, pending.messageId);
        QCOMPARE(byLocal.failure, Failure::None); QVERIFY(byLocal.value); QVERIFY(byMessage.value);
        QCOMPARE(byLocal.value->responseMessage, completion.responseMessage);
        QCOMPARE(byMessage.value->localId, pending.localId); QCOMPARE(byMessage.value->outcome, Vault::Outcome::Granted);
        vault.close(); open(vault);
        QCOMPARE(vault.completeRequest(vault.lease(), Address, completion, key.bytes), Failure::None);
        QCOMPARE(vault.read(vault.lease(), Address).value.receivedAt, receivedAt);
        auto changed = completion; changed.outcome = Vault::Outcome::Rejected;
        QCOMPARE(vault.completeRequest(vault.lease(), Address, changed), Failure::Conflict);
        changed = completion; changed.responseMessage = uuid();
        QCOMPARE(vault.completeRequest(vault.lease(), Address, changed, key.bytes), Failure::Conflict);
        changed = completion; changed.requestMessage = uuid();
        QCOMPARE(vault.completeRequest(vault.lease(), Address, changed, key.bytes), Failure::Conflict);
        QCOMPARE(vault.remove(vault.lease(), Address), Failure::None);
        QCOMPARE(vault.completeRequest(vault.lease(), Address, completion, key.bytes), Failure::None);
        QVERIFY(vault.read(vault.lease(), Address).value.bytes.isEmpty());
        QCOMPARE(vault.read(vault.lease(), Address).value.deletionRevision, quint64(1));
        pending.deletionRevision = 1;
        QCOMPARE(vault.putPendingRequest(vault.lease(), Address, pending), Failure::Conflict);
    }

    void rejectRemovesOnlyTheMatchingPrivateRequestAndPreservesExistingKey()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend); open(vault);
        const QByteArray key(32, 'k'); QCOMPARE(vault.put(vault.lease(), Address, key, 0), Failure::None);
        auto first = request(), second = request(); first.messageId = uuid();
        QCOMPARE(vault.putPendingRequest(vault.lease(), Address, first), Failure::None);
        QCOMPARE(vault.putPendingRequest(vault.lease(), Address, second), Failure::None);
        auto completion = granted(first); completion.outcome = Vault::Outcome::Rejected;
        QCOMPARE(vault.completeRequest(vault.lease(), Address, completion), Failure::None);
        const auto rows = vault.pendingRequests(vault.lease(), Address); QCOMPARE(rows.values.size(), 1);
        QCOMPARE(rows.values.first().localId, second.localId);
        QVERIFY(vault.read(vault.lease(), Address).value.bytes == key);
        QCOMPARE(vault.completeRequest(vault.lease(), Address, completion), Failure::None);
        QCOMPARE(vault.requestCompletionByMessage(vault.lease(), Address, first.messageId).value->outcome, Vault::Outcome::Rejected);
    }

    void failedSecondReceiptWriteRollsBackKeyFirstReceiptAndPrivateRequestDeletion()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend); open(vault);
        auto pending = request(); pending.messageId = uuid(); const auto completion = granted(pending);
        QCOMPARE(vault.putPendingRequest(vault.lease(), Address, pending), Failure::None);
        const auto path = databasePath(directory.path());
        QVERIFY(modify(path, "CREATE TRIGGER fail_receipt BEFORE INSERT ON secrets WHEN NEW.address LIKE '[\"receipt-message\"%' BEGIN SELECT RAISE(ABORT,'injected'); END"));
        const QByteArray key(32, 'g');
        QCOMPARE(vault.completeRequest(vault.lease(), Address, completion, key), Failure::Unavailable);
        QVERIFY(vault.read(vault.lease(), Address).value.bytes.isEmpty());
        QVERIFY(!vault.requestCompletion(vault.lease(), Address, pending.localId).value);
        QVERIFY(!vault.requestCompletionByMessage(vault.lease(), Address, pending.messageId).value);
        QCOMPARE(vault.pendingRequests(vault.lease(), Address).values.size(), 1);
        QVERIFY(modify(path, "DROP TRIGGER fail_receipt"));
        QCOMPARE(vault.completeRequest(vault.lease(), Address, completion, key), Failure::None);
    }

    void generationDeletionRemovesPendingRepairCopiesAndFencesOldResults()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend); open(vault);
        auto pending = request(); pending.messageId = uuid(); pending.recovery = Vault::Recovery{QByteArray(48, 'p'), QByteArray(32, 'k')};
        const auto other = Vault::Address{Address.stream, uuid()};
        QCOMPARE(vault.putPendingRequest(vault.lease(), Address, pending), Failure::None);
        auto additional = request(); QCOMPARE(vault.putPendingRequest(vault.lease(), Address, additional), Failure::None);
        QCOMPARE(vault.putPendingRequest(vault.lease(), other, additional), Failure::None);
        QCOMPARE(vault.remove(vault.lease(), Address), Failure::None);
        QCOMPARE(vault.pendingRequests(vault.lease(), Address).values.size(), 0);
        QCOMPARE(vault.pendingRequests(vault.lease(), other).values.size(), 1);
        QCOMPARE(vault.completeRequest(vault.lease(), Address, granted(pending), QByteArray(32, 'n'), true), Failure::MissingRequest);
        QCOMPARE(vault.putPendingRequest(vault.lease(), Address, pending), Failure::Superseded);
        pending.deletionRevision = 1;
        QCOMPARE(vault.putPendingRequest(vault.lease(), Address, pending), Failure::None);
    }

    void repairRequiresTheSavedPriorKeyAndPreservesFirstReceivedDate()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend); open(vault);
        const QByteArray oldKey(32, 'a'), repaired(32, 'b');
        QCOMPARE(vault.put(vault.lease(), Address, oldKey, 0), Failure::None);
        const auto received = vault.read(vault.lease(), Address).value.receivedAt;
        QCOMPARE(vault.markForSending(vault.lease(), Address, 0), Failure::None);
        auto first = request(), second = request(); first.messageId = uuid(); second.messageId = uuid();
        first.recovery = second.recovery = Vault::Recovery{QByteArray(48, 'p'), oldKey};
        QCOMPARE(vault.putPendingRequest(vault.lease(), Address, first), Failure::None);
        QCOMPARE(vault.putPendingRequest(vault.lease(), Address, second), Failure::None);
        QCOMPARE(vault.completeRequest(vault.lease(), Address, granted(first), repaired), Failure::Conflict);
        QCOMPARE(vault.completeRequest(vault.lease(), Address, granted(first), repaired, true), Failure::None);
        QCOMPARE(vault.completeRequest(vault.lease(), Address, granted(second), repaired, true), Failure::None);
        QVERIFY(vault.read(vault.lease(), Address).value.bytes == repaired);
        QCOMPARE(vault.read(vault.lease(), Address).value.receivedAt, received);
        QCOMPARE(vault.read(vault.lease(), Address).value.sendOrder, quint64(1));
        QCOMPARE(vault.lastAvailableKey(vault.lease(), Address.stream).key, Address.key);
        auto wrong = request(); wrong.messageId = uuid(); wrong.recovery = Vault::Recovery{QByteArray(48, 'p'), QByteArray(32, 'w')};
        QCOMPARE(vault.putPendingRequest(vault.lease(), Address, wrong), Failure::None);
        QCOMPARE(vault.completeRequest(vault.lease(), Address, granted(wrong), oldKey, true), Failure::Conflict);
        QCOMPARE(vault.pendingRequests(vault.lease(), Address).values.size(), 1);
    }

    void failedPendingDeletionRollsBackTheKeyDeletionRevision()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend); open(vault);
        const QByteArray key(32, 'k'); QCOMPARE(vault.put(vault.lease(), Address, key, 0), Failure::None);
        QCOMPARE(vault.putPendingRequest(vault.lease(), Address, request()), Failure::None);
        const auto path = databasePath(directory.path());
        QVERIFY(modify(path, "CREATE TRIGGER fail_delete BEFORE DELETE ON secrets WHEN OLD.address LIKE '[\"request\"%' BEGIN SELECT RAISE(ABORT,'injected'); END"));
        QCOMPARE(vault.remove(vault.lease(), Address), Failure::Unavailable);
        const auto retained = vault.read(vault.lease(), Address); QVERIFY(retained.value.bytes == key);
        QCOMPARE(retained.value.deletionRevision, quint64(0));
        QCOMPARE(vault.pendingRequests(vault.lease(), Address).values.size(), 1);
        QVERIFY(modify(path, "DROP TRIGGER fail_delete"));
        QCOMPARE(vault.remove(vault.lease(), Address), Failure::None);
        QCOMPARE(vault.pendingRequests(vault.lease(), Address).values.size(), 0);
        QCOMPARE(vault.read(vault.lease(), Address).value.deletionRevision, quint64(1));
    }

    void pendingCountIsBoundedWithoutEvictingOrChangingAnExistingIntent()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend); open(vault);
        QVector<Vault::PendingRequest> saved;
        for (qsizetype i = 0; i < Vault::MaximumPendingRequests; ++i) {
            auto pending = request(); QCOMPARE(vault.putPendingRequest(vault.lease(), Address, pending), Failure::None); saved.append(pending);
        }
        auto extra = request(); QCOMPARE(vault.putPendingRequest(vault.lease(), Address, extra), Failure::Capacity);
        QCOMPARE(vault.putPendingRequest(vault.lease(), Address, saved.first()), Failure::None);
        const auto placement = uuid(); QCOMPARE(vault.bindPendingRequest(vault.lease(), Address, saved.first().localId, placement), Failure::None);
        auto first = saved.first(); first.messageId = placement; auto completion = granted(first); completion.outcome = Vault::Outcome::Rejected;
        QCOMPARE(vault.completeRequest(vault.lease(), Address, completion), Failure::None);
        QCOMPARE(vault.putPendingRequest(vault.lease(), Address, extra), Failure::None);
        vault.close(); open(vault); QCOMPARE(vault.pendingRequests(vault.lease(), Address).values.size(), Vault::MaximumPendingRequests);
    }

    void pendingBytesAndIndividualRecoveryEvidenceAreBounded()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend); open(vault);
        auto tooLarge = request(); tooLarge.recovery = Vault::Recovery{QByteArray(Vault::MaximumRecoveryEnvelopeBytes + 1, 'p'), QByteArray(32, 'a')};
        QCOMPARE(vault.putPendingRequest(vault.lease(), Address, tooLarge), Failure::Capacity);
        int accepted = 0;
        for (; accepted < Vault::MaximumPendingRequests; ++accepted) {
            auto pending = request(); pending.recovery = Vault::Recovery{QByteArray(Vault::MaximumRecoveryEnvelopeBytes, 'p'), QByteArray(32, 'a')};
            const auto result = vault.putPendingRequest(vault.lease(), Address, pending);
            if (result == Failure::Capacity) break;
            QCOMPARE(result, Failure::None);
        }
        QVERIFY(accepted > 0); QVERIFY(accepted < Vault::MaximumPendingRequests);
        const auto rows = vault.pendingRequests(vault.lease(), Address);
        QCOMPARE(rows.failure, Failure::None); QCOMPARE(rows.values.size(), accepted);
    }

    void completionCannotClaimAnotherRequestsPublicPlacement()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend); open(vault);
        auto first = request(), second = request(); first.messageId = uuid(); second.messageId = first.messageId;
        QCOMPARE(vault.putPendingRequest(vault.lease(), Address, first), Failure::None);
        QCOMPARE(vault.putPendingRequest(vault.lease(), Address, second), Failure::None);
        const QByteArray key(32, 'g'); QCOMPARE(vault.completeRequest(vault.lease(), Address, granted(first), key), Failure::None);
        QCOMPARE(vault.completeRequest(vault.lease(), Address, granted(second), key), Failure::Conflict);
        QCOMPARE(vault.pendingRequests(vault.lease(), Address).values.size(), 1);
        QVERIFY(!vault.requestCompletion(vault.lease(), Address, second.localId).value);
    }

    void pendingAddressTamperingAndOversizedRowsFailClosed()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend); open(vault);
        const auto pending = request(); QCOMPARE(vault.putPendingRequest(vault.lease(), Address, pending), Failure::None);
        const auto path = databasePath(directory.path()); vault.close();
        QVERIFY(modify(path, "UPDATE secrets SET address=replace(address,'" + pending.localId + "','" + uuid() + "') WHERE address LIKE '[\"request\"%'"));
        open(vault); QCOMPARE(vault.pendingRequests(vault.lease(), Address).failure, Failure::Corrupt); vault.close();
        QVERIFY(modify(path, "UPDATE secrets SET payload=zeroblob(" + QString::number(Vault::MaximumRequestRecordBytes + 64) + ") WHERE address LIKE '[\"request\"%'"));
        open(vault); QCOMPARE(vault.pendingRequests(vault.lease(), Address).failure, Failure::Corrupt);
    }

    void pendingOperationsRespectOwnerLeaseAndMalformedInputs()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend); open(vault);
        auto pending = request(); pending.messageId = uuid(); const auto old = vault.lease();
        QCOMPARE(vault.putPendingRequest(old, Address, pending), Failure::None);
        auto changed = pending; changed.deviceName += 'x';
        QCOMPARE(vault.putPendingRequest(old, Address, changed), Failure::Conflict);
        changed = pending; changed.createdAt = -1;
        QCOMPARE(vault.putPendingRequest(old, Address, changed), Failure::InvalidKey);
        QCOMPARE(vault.bindPendingRequest(old, Address, pending.localId, "not-a-uuid"), Failure::InvalidAddress);
        open(vault, Failure::None, OtherUser);
        QCOMPARE(vault.pendingRequests(old, Address).failure, Failure::Superseded);
        QCOMPARE(vault.putPendingRequest(old, Address, pending), Failure::Superseded);
        QCOMPARE(vault.bindPendingRequest(old, Address, pending.localId, pending.messageId), Failure::Superseded);
        QCOMPARE(vault.completeRequest(old, Address, granted(pending), QByteArray(32, 'g')), Failure::Superseded);
        QCOMPARE(vault.requestCompletion(old, Address, pending.localId).failure, Failure::Superseded);
        QCOMPARE(vault.pendingRequests(vault.lease(), Address).values.size(), 0);
        open(vault); QCOMPARE(vault.pendingRequests(vault.lease(), Address).values.size(), 1);
    }

    void validatedVersionOneMigratesWithoutRewritingExistingEncryptedKeys()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend); open(vault);
        const QByteArray key(32, 'k'); QCOMPARE(vault.put(vault.lease(), Address, key, 0), Failure::None);
        const auto device = vault.device(); vault.close();
        QVERIFY(modify(databasePath(directory.path()), "PRAGMA user_version=1"));
        open(vault); QCOMPARE(vault.device().id, device.id); QVERIFY(vault.read(vault.lease(), Address).value.bytes == key);
        QCOMPARE(backend.writeCount, 1);
        QCOMPARE(vault.putPendingRequest(vault.lease(), Address, request()), Failure::None);
    }

    void encryptsKeysAndDeviceNameOutsideTheMessageCache()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend); open(vault);
        QVERIFY(vault.available()); QVERIFY(!vault.device().id.isEmpty());
        QCOMPARE(vault.device().name, QString("Synthetic Linux device"));
        const QByteArray secret(32, 'k');
        QCOMPARE(vault.put(vault.lease(), Address, secret, 0), Failure::None);
        const auto saved = vault.read(vault.lease(), Address); QCOMPARE(saved.failure, Failure::None);
        QCOMPARE(saved.value.bytes, secret); QVERIFY(saved.value.receivedAt > 0);
        QCOMPARE(backend.values.size(), 1); QCOMPARE(backend.values.first().size(), 32);
        QFile file(databasePath(directory.path())); QVERIFY(file.open(QIODevice::ReadOnly)); const auto bytes = file.readAll();
        QVERIFY(!bytes.contains(secret)); QVERIFY(!bytes.contains(secret.toBase64()));
        QVERIFY(!bytes.contains(backend.values.first())); QVERIFY(!bytes.contains("Synthetic Linux device"));
        QVERIFY(!(file.permissions() & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ReadOther | QFileDevice::WriteOther)));
    }

    void reopenPreservesDeviceKeyAndFirstReceivedDate()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend); open(vault);
        const auto device = vault.device(); const auto key = NativeChatCrypto::createGroupKey(); QVERIFY(key);
        QCOMPARE(vault.put(vault.lease(), Address, key.bytes, 0), Failure::None);
        const auto first = vault.read(vault.lease(), Address).value;
        QCOMPARE(vault.put(vault.lease(), Address, key.bytes, 0), Failure::None);
        vault.close(); open(vault); QCOMPARE(vault.device().id, device.id); QCOMPARE(vault.device().name, device.name);
        const auto saved = vault.read(vault.lease(), Address); QCOMPARE(saved.failure, Failure::None);
        QCOMPARE(saved.value.bytes, key.bytes); QCOMPARE(saved.value.receivedAt, first.receivedAt);
        QCOMPARE(backend.writeCount, 1);
    }

    void generationAndOwnerLeasesNeverCrossPartitions()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend); open(vault);
        const auto old = vault.lease(); QCOMPARE(vault.put(old, Address, QByteArray(32, 'a'), 0), Failure::None);
        open(vault, Failure::None, OtherUser); QVERIFY(vault.read(vault.lease(), Address).value.bytes.isEmpty());
        QCOMPARE(vault.put(old, Address, QByteArray(32, 'x'), 0), Failure::Superseded);
        QCOMPARE(vault.read(old, Address).failure, Failure::Superseded);
        QCOMPARE(vault.remove(old, Address), Failure::Superseded);
        open(vault); QCOMPARE(vault.read(vault.lease(), Address).value.bytes, QByteArray(32, 'a'));
        QCOMPARE(vault.read(old, Address).failure, Failure::Superseded);
    }

    void deletionRevisionPreventsLateRestorationAndPersists()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend); open(vault);
        QCOMPARE(vault.put(vault.lease(), Address, QByteArray(32, 'a'), 0), Failure::None);
        QCOMPARE(vault.put(vault.lease(), Address, QByteArray(32, 'b'), 0), Failure::Conflict);
        QCOMPARE(vault.read(vault.lease(), Address).value.bytes, QByteArray(32, 'a'));
        QCOMPARE(vault.remove(vault.lease(), Address), Failure::None);
        const auto removed = vault.read(vault.lease(), Address); QCOMPARE(removed.failure, Failure::None);
        QVERIFY(removed.value.bytes.isEmpty()); QCOMPARE(removed.value.deletionRevision, quint64(1));
        QCOMPARE(removed.value.receivedAt, qint64(0));
        vault.close(); open(vault);
        QCOMPARE(vault.put(vault.lease(), Address, QByteArray(32, 'a'), 0), Failure::Superseded);
        QCOMPARE(vault.put(vault.lease(), Address, QByteArray(32, 'b'), 1), Failure::None);
        QCOMPARE(vault.read(vault.lease(), Address).value.bytes, QByteArray(32, 'b'));
        QCOMPARE(vault.remove(vault.lease(), Address), Failure::None);
        QCOMPARE(vault.read(vault.lease(), Address).value.deletionRevision, quint64(2));
    }

    void missingOrWrongMasterNeverReplacesExistingSecrets()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend); open(vault);
        QCOMPARE(vault.put(vault.lease(), Address, QByteArray(32, 'a'), 0), Failure::None); vault.close();
        const auto key = backend.values.firstKey(); const auto original = backend.values.first(); backend.values.clear();
        open(vault, Failure::MissingMasterKey); QVERIFY(!vault.available()); QCOMPARE(backend.writeCount, 1);
        backend.values.insert(key, QByteArray(32, 'x')); open(vault, Failure::Corrupt); QVERIFY(!vault.available());
        backend.values[key] = original; open(vault); QCOMPARE(vault.read(vault.lease(), Address).value.bytes, QByteArray(32, 'a'));
    }

    void unavailableBackendNeverCreatesPlaintextFallback()
    {
        QTemporaryDir directory; Secrets backend; backend.failure = SessionSecretStore::Error::Unavailable;
        Vault vault(directory.path(), &backend); open(vault, Failure::Unavailable);
        QVERIFY(!vault.available()); QVERIFY(!QFileInfo::exists(databasePath(directory.path())));
        backend.failure = SessionSecretStore::Error::Denied; open(vault, Failure::Denied);
        QVERIFY(!vault.available()); QCOMPARE(backend.writeCount, 0);
    }

    void malformedInputsHaveNoPersistentEffects()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend);
        bool done = false;
        vault.open(QUrl("http://workspace.example.test"), Project, User, "Synthetic", [&](Failure error) {
            QCOMPARE(error, Failure::InvalidOwner); done = true;
        }); QTRY_VERIFY(done); QCOMPARE(backend.reads, 0); open(vault);
        QCOMPARE(vault.put(vault.lease(), {"invalid", Address.key}, QByteArray(32, 'x'), 0), Failure::InvalidAddress);
        QCOMPARE(vault.put(vault.lease(), Address, QByteArray(31, 'x'), 0), Failure::InvalidKey);
        QCOMPARE(vault.read(vault.lease(), Address).value.bytes.size(), 0);
    }

    void ciphertextTamperingAndAddressSwapsFailClosed()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend); open(vault);
        QCOMPARE(vault.put(vault.lease(), Address, QByteArray(32, 'a'), 0), Failure::None);
        const auto path = databasePath(directory.path()); vault.close();
        QVERIFY(modify(path, "UPDATE secrets SET payload=CAST(substr(payload,1,5)||CASE hex(substr(payload,6,1)) WHEN '00' THEN X'01' ELSE X'00' END||substr(payload,7) AS BLOB) WHERE address!='device'"));
        open(vault); QCOMPARE(vault.read(vault.lease(), Address).failure, Failure::Corrupt);
        QCOMPARE(vault.put(vault.lease(), Address, QByteArray(32, 'x'), 0), Failure::Corrupt);
    }

    void openingAgainDuringMasterWriteMustNotLoseTheMaster()
    {
        QTemporaryDir directory; Secrets backend; backend.holdWrites = true; Vault vault(directory.path(), &backend);
        bool first = false, second = false; Failure secondResult = Failure::Unavailable;
        vault.open(Server, Project, User, "Synthetic", [&](Failure error) { QCOMPARE(error, Failure::Superseded); first = true; });
        QTRY_COMPARE(backend.writes.size(), size_t(1));
        vault.open(Server, Project, User, "Synthetic", [&](Failure error) { secondResult = error; second = true; });
        // A second read before the outstanding master write completes observes
        // NotFound and can produce a different master for the same durable file.
        QTest::qWait(20); QCOMPARE(backend.writeCount, 1);
        backend.holdWrites = false; backend.finishWrite();
        QTRY_VERIFY(first); QTRY_VERIFY(second); QCOMPARE(secondResult, Failure::None);
        QCOMPARE(vault.put(vault.lease(), Address, QByteArray(32, 'a'), 0), Failure::None);
        vault.close(); open(vault); QCOMPARE(vault.read(vault.lease(), Address).value.bytes, QByteArray(32, 'a'));
    }

    void concurrentVaultsMustNotRaceMasterCreation()
    {
        QTemporaryDir directory; Secrets backend; backend.holdWrites = true;
        Vault first(directory.path(), &backend), second(directory.path(), &backend);
        bool firstDone = false;
        first.open(Server, Project, User, "Synthetic", [&](Failure error) { QCOMPARE(error, Failure::None); firstDone = true; });
        QTRY_COMPARE(backend.writes.size(), size_t(1));
        open(second, Failure::Unavailable); QCOMPARE(backend.writeCount, 1);
        backend.holdWrites = false; backend.finishWrite(); QTRY_VERIFY(firstDone);
        first.close(); open(second); QVERIFY(second.available());
    }

    void destroyedInitializerCannotCauseAReplacementMaster()
    {
        QTemporaryDir directory; Secrets backend; backend.holdWrites = true;
        auto *first = new Vault(directory.path(), &backend);
        first->open(Server, Project, User, "Synthetic", [](Failure) {});
        QTRY_COMPARE(backend.writes.size(), size_t(1)); delete first;
        Vault replacement(directory.path(), &backend);
        open(replacement, Failure::MissingMasterKey); QCOMPARE(backend.writeCount, 1);
        backend.holdWrites = false; backend.finishWrite();
        open(replacement); QVERIFY(replacement.available()); QCOMPARE(backend.writeCount, 1);
    }

    void encryptedRecordIsBoundToItsDeviceStreamAndGenerationAddress()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend); open(vault);
        QCOMPARE(vault.put(vault.lease(), Address, QByteArray(32, 'a'), 0), Failure::None);
        const auto path = databasePath(directory.path()); vault.close();
        const auto otherKey = QString("40000000-0000-4000-8000-000000000005");
        QVERIFY(modify(path, "UPDATE secrets SET address=replace(address,'" + Address.key + "','" + otherKey + "') WHERE address!='device'"));
        open(vault);
        QCOMPARE(vault.read(vault.lease(), {Address.stream, otherKey}).failure, Failure::Corrupt);
        QCOMPARE(vault.read(vault.lease(), Address).failure, Failure::None);
        QVERIFY(vault.read(vault.lease(), Address).value.bytes.isEmpty());
    }

    void symlinkTargetIsNotOpenedAndDoesNotWriteAMaster()
    {
        QTemporaryDir directory; Secrets backend; const auto target = directory.filePath("foreign");
        QFile file(target); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write("preserve"), qint64(8)); file.close();
        QVERIFY(QFile::link(target, databasePath(directory.path())));
        Vault vault(directory.path(), &backend); open(vault, Failure::Unavailable);
        QCOMPARE(backend.reads, 0); QCOMPARE(backend.writeCount, 0);
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("preserve"));
    }

    void losingAnInjectedBackendCompletesThePendingOpenAndReleasesItsLock()
    {
        QTemporaryDir directory; auto *backend = new Secrets; backend->holdWrites = true;
        Vault vault(directory.path(), backend); bool completed = false;
        vault.open(Server, Project, User, "Synthetic", [&](Failure error) {
            QCOMPARE(error, Failure::Unavailable); completed = true;
        });
        QTRY_COMPARE(backend->writes.size(), size_t(1)); delete backend;
        QTRY_VERIFY(completed); QVERIFY(!vault.available());
        QVERIFY(!QFileInfo::exists(databasePath(directory.path()) + ".lock"));
        open(vault, Failure::Unavailable);
    }

    void deletionBeforeFirstSaveAndReplacementObjectFenceOldResults()
    {
        QTemporaryDir directory; Secrets backend; Vault first(directory.path(), &backend); open(first);
        const auto old = first.lease();
        QCOMPARE(first.remove(old, Address), Failure::None);
        QCOMPARE(first.put(old, Address, QByteArray(32, 'a'), 0), Failure::Superseded);
        first.close(); Vault replacement(directory.path(), &backend); open(replacement);
        QCOMPARE(replacement.put(old, Address, QByteArray(32, 'a'), 1), Failure::Superseded);
        QCOMPARE(replacement.read(replacement.lease(), Address).value.deletionRevision, quint64(1));
        QCOMPARE(replacement.put(replacement.lease(), Address, QByteArray(32, 'a'), 1), Failure::None);
    }

    void oversizedEncryptedRowFailsClosedWithoutReplacement()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend); open(vault);
        QCOMPARE(vault.put(vault.lease(), Address, QByteArray(32, 'a'), 0), Failure::None);
        const auto path = databasePath(directory.path()); vault.close();
        QVERIFY(modify(path, "UPDATE secrets SET payload=zeroblob(3145728) WHERE address!='device'"));
        open(vault); QCOMPARE(vault.read(vault.lease(), Address).failure, Failure::Corrupt);
        QCOMPARE(vault.remove(vault.lease(), Address), Failure::Corrupt);
    }

    void futureDatabaseVersionIsPreservedAndRejected()
    {
        QTemporaryDir directory; Secrets backend; Vault vault(directory.path(), &backend); open(vault);
        const auto path = databasePath(directory.path()); vault.close();
        QVERIFY(modify(path, "PRAGMA user_version=4"));
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); const auto before = file.readAll(); file.close();
        open(vault, Failure::Corrupt); QVERIFY(!vault.available());
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), before);
    }

    void failedMasterWriteKeepsAnUnresolvedReservationAndNeverCreatesAnotherMaster()
    {
        QTemporaryDir directory; Secrets backend; backend.holdWrites = true; Vault vault(directory.path(), &backend);
        bool completed = false;
        vault.open(Server, Project, User, "Synthetic", [&](Failure error) {
            QCOMPARE(error, Failure::Denied); completed = true;
        });
        QTRY_COMPARE(backend.writes.size(), size_t(1)); backend.failure = SessionSecretStore::Error::Denied;
        backend.finishWrite(); QTRY_VERIFY(completed); QVERIFY(!vault.available());
        QCOMPARE(QFileInfo(databasePath(directory.path())).size(), qint64(0));
        backend.failure = SessionSecretStore::Error::None; backend.holdWrites = false;
        open(vault, Failure::MissingMasterKey); QCOMPARE(backend.writeCount, 1);
    }

    void completionCanDestroyTheVaultOrReenterWithAnotherOwner()
    {
        QTemporaryDir directory; Secrets backend; auto *temporary = new Vault(directory.path(), &backend);
        bool destroyed = false;
        temporary->open(Server, Project, User, "Synthetic", [&](Failure error) {
            QCOMPARE(error, Failure::None); delete temporary; temporary = nullptr; destroyed = true;
        });
        QTRY_VERIFY(destroyed); QVERIFY(!QFileInfo::exists(databasePath(directory.path()) + ".lock"));
        Vault vault(directory.path(), &backend); bool switched = false; Vault::Lease old;
        vault.open(Server, Project, User, "Synthetic", [&](Failure error) {
            QCOMPARE(error, Failure::None); old = vault.lease();
            QCOMPARE(vault.put(old, Address, QByteArray(32, 'a'), 0), Failure::None);
            vault.open(Server, Project, OtherUser, "Other synthetic device", [&](Failure next) {
                QCOMPARE(next, Failure::None); switched = true;
            });
        });
        QTRY_VERIFY(switched); QCOMPARE(vault.read(old, Address).failure, Failure::Superseded);
        QVERIFY(vault.read(vault.lease(), Address).value.bytes.isEmpty());
    }
};

QTEST_GUILESS_MAIN(NativeChatKeyVaultTest)
#include "native-chat-key-vault-test.moc"
