#include "mock-server.h"
#include "native-app.h"
#include "native-chat-key-vault.h"
#include "native-controller.h"
#include "native-cache-service.h"
#include "native-preferences.h"

#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <deque>

using namespace workspace;
using namespace workspace::test;

namespace {
const QString OtherProject = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";

class MemorySecrets final : public SessionSecretStore {
public:
    QMap<QString, QByteArray> values;
    QStringList reads, writes;
    bool hold = false;
    Error error = Error::None;
    std::deque<std::function<void()>> pending;
    void read(const QString &key, Loaded done) override {
        reads.append(key);
        const auto operation = [this, key, done] {
            const auto value = values.value(key);
            done(error == Error::None ? value : QByteArray{}, error != Error::None ? error
                : value.isEmpty() ? Error::NotFound : Error::None);
        };
        if (hold) pending.push_back(operation);
        else QTimer::singleShot(0, this, operation);
    }
    void write(const QString &key, const QByteArray &value, Completion done) override {
        writes.append(key); values.insert(key, value);
        QTimer::singleShot(0, this, [done] { done(Error::None); });
    }
    void remove(const QString &key, Completion done) override {
        values.remove(key); QTimer::singleShot(0, this, [done] { done(Error::None); });
    }
    void release() {
        QVERIFY(!pending.empty());
        auto operation = std::move(pending.front()); pending.pop_front(); operation();
    }
};

struct IdentityTransport {
    QObject lifetime;
    QString selected = projectId;
    int calls = 0;
    int issuedTokens = 0;
    Session::RequestExecutor executor() {
        return [this](const QByteArray &, const QString &path, const QJsonValue &,
                      const QUrlQuery &, ApiClient::Callback done, bool,
                      const QMap<QByteArray, QByteArray> &) {
            ++calls;
            ApiResult result; result.status = 200;
            if (path.endsWith("/get_token/invoke"))
                result.data = QJsonObject{{"access_token", QString("synthetic-access-%1").arg(++issuedTokens)},
                    {"refresh_token", "synthetic-refresh"}, {"expires_in", 3600}};
            else if (path.endsWith("/introspect"))
                result.data = QJsonObject{{"project_id", selected}, {"user_info", QJsonObject{{"uuid", userId}}}};
            else result.data = QJsonArray{QJsonObject{{"uuid", projectId}, {"name", "First"}},
                QJsonObject{{"uuid", OtherProject}, {"name", "Second"}}};
            QTimer::singleShot(0, &lifetime, [done, result] { done(result); });
        };
    }
};

struct Fixture {
    QTemporaryDir directory;
    MemorySecrets secrets;
    IdentityTransport identity;
    ApiClient api;
    Session session{&api, nullptr, identity.executor()};
    NativeChatKeyVault keys{directory.filePath("keys"), &secrets};
    NativeController controller{&api, &session};
    Fixture() {
        // No realtime client is attached: the authenticated core emits its
        // resynchronization signal but sends no synthetic-host network request.
        controller.setRealtimeMode(true);
        bindNativeChatKeys(keys, session, api, controller, "Linux — Desktop");
    }
    bool login() {
        session.login(QUrl("https://runtime.example.invalid"), "synthetic-cassi", "synthetic-password");
        QElapsedTimer timer; timer.start();
        while (session.projects().size() != 2 && timer.elapsed() < 2000) QTest::qWait(1);
        if (session.projects().size() != 2) return false;
        identity.selected = projectId; session.selectProject(projectId);
        timer.restart();
        while (!session.ready() && timer.elapsed() < 2000) QTest::qWait(1);
        return session.ready();
    }
};
}

class NativeChatRuntimeTest final : public QObject {
    Q_OBJECT
private slots:
    void verifiedIdentityOpensOnePrivateStoreWithoutExposingSecrets() {
        Fixture f; QVERIFY(f.directory.isValid());
        QVERIFY(!f.keys.available()); QVERIFY(f.secrets.reads.isEmpty());
        bool observedOpening = false;
        connect(&f.controller, &NativeController::stateChanged, this, [&] {
            observedOpening |= f.controller.encryption().value("storeOpening").toBool();
        });
        QVERIFY(f.login()); QTRY_VERIFY(f.keys.available());
        QVERIFY(observedOpening); QVERIFY(!f.controller.encryption().value("storeOpening").toBool());
        QCOMPARE(f.keys.lease().owner, f.controller.ownerKey());
        QCOMPARE(f.keys.device().name, QString("Linux — Desktop"));
        QCOMPARE(f.secrets.reads.size(), 1); QCOMPARE(f.secrets.writes.size(), 1);
        const auto state = f.controller.encryption();
        QCOMPARE(state.keys(), (QStringList{"available", "busy", "canRequest", "canRequestCurrentKey", "currentKeyUuid", "error",
            "keyRequests", "keyResponse", "keyUuid", "pendingSend", "sendingKeyUuid", "state", "storeOpening"}));
        QVERIFY(state.value("keyRequests").toList().isEmpty());
        QVERIFY(state.value("keyResponse").toMap().isEmpty());
        QVERIFY(state.value("sendingKeyUuid").toString().isEmpty());
        QVERIFY(state.value("pendingSend").toMap().isEmpty());
        QVERIFY(state.value("available").toBool()); QVERIFY(!state.value("canRequest").toBool());
        QVERIFY(state.value("currentKeyUuid").toString().isEmpty());
        QVERIFY(!QJsonDocument(QJsonObject::fromVariantMap(state)).toJson().contains("synthetic-"));
    }

    void sameOwnerTokenRenewalKeepsTheExistingLeaseAndDevice() {
        Fixture f; QVERIFY(f.login()); QTRY_VERIFY(f.keys.available());
        const auto before = f.keys.lease(); const auto device = f.keys.device().id;
        const auto beforeApiGeneration = f.api.generation();
        const auto beforeRefreshLease = f.keys.lease();
        QSignalSpy ready(&f.session, &Session::authenticated);
        f.session.requestRefresh(); QTRY_COMPARE(ready.size(), 1);
        QVERIFY(f.api.generation() > beforeApiGeneration);
        QVERIFY(f.session.ready());
        QCOMPARE(f.keys.lease().owner, beforeRefreshLease.owner);
        QCOMPARE(f.keys.lease().device, beforeRefreshLease.device);
        QCOMPARE(f.keys.lease().issuer, beforeRefreshLease.issuer);
        QCOMPARE(f.keys.lease().generation, beforeRefreshLease.generation);
        QVERIFY(f.keys.available());
        QVERIFY(!f.controller.encryption().value("storeOpening").toBool());
        QCOMPARE(f.keys.lease().generation, before.generation); QCOMPARE(f.keys.device().id, device);
        QCOMPARE(f.secrets.reads.size(), 1); QCOMPARE(f.secrets.writes.size(), 1);
        f.controller.retryChatKeyStore(); QCOMPARE(f.secrets.reads.size(), 1);
    }

    void repeatedAuthenticationWhileTheOsReadIsPendingIsDeduplicated() {
        Fixture f; f.secrets.hold = true; QVERIFY(f.login());
        QTRY_COMPARE(f.secrets.pending.size(), size_t(1));
        QVERIFY(f.controller.encryption().value("storeOpening").toBool());
        const auto beforeApiGeneration = f.api.generation();
        const auto beforeRefreshLease = f.keys.lease();
        QSignalSpy ready(&f.session, &Session::authenticated);
        f.session.requestRefresh(); QTRY_COMPARE(ready.size(), 1);
        QVERIFY(f.api.generation() > beforeApiGeneration);
        QVERIFY(f.session.ready());
        QCOMPARE(f.keys.lease().owner, beforeRefreshLease.owner);
        QCOMPARE(f.keys.lease().device, beforeRefreshLease.device);
        QCOMPARE(f.keys.lease().issuer, beforeRefreshLease.issuer);
        QCOMPARE(f.keys.lease().generation, beforeRefreshLease.generation);
        QVERIFY(!f.keys.available());
        QVERIFY(f.controller.encryption().value("storeOpening").toBool());
        f.controller.retryChatKeyStore();
        QCOMPARE(f.secrets.reads.size(), 1); QCOMPARE(f.secrets.pending.size(), size_t(1));
        f.secrets.release(); QTRY_VERIFY(f.keys.available());
        QCOMPARE(f.secrets.writes.size(), 1);
    }

    void projectReplacementFencesTheOldOsCallbackBeforeNewIdentityIsReady() {
        Fixture f; f.secrets.hold = true; QVERIFY(f.login());
        QTRY_COMPARE(f.secrets.pending.size(), size_t(1));
        const auto oldOwner = f.controller.ownerKey();
        f.identity.selected = OtherProject; f.session.selectProject(OtherProject);
        QVERIFY(!f.keys.available()); QVERIFY(!f.controller.encryption().value("storeOpening").toBool());
        QTRY_VERIFY(f.session.ready());
        QVERIFY(f.controller.ownerKey() != oldOwner);
        QVERIFY(f.controller.encryption().value("storeOpening").toBool());
        f.secrets.release(); QTRY_COMPARE(f.secrets.pending.size(), size_t(1));
        QVERIFY(!f.keys.available()); QVERIFY(f.secrets.writes.isEmpty());
        QVERIFY(f.controller.encryption().value("storeOpening").toBool());
        f.secrets.release(); QTRY_VERIFY(f.keys.available());
        QCOMPARE(f.keys.lease().owner, f.controller.ownerKey());
        QVERIFY(f.keys.lease().owner != oldOwner); QCOMPARE(f.secrets.writes.size(), 1);
    }

    void deniedOsStorageNeedsAnExplicitStoreRetryAndDoesNotLoopOnRefresh() {
        Fixture f; f.secrets.error = SessionSecretStore::Error::Denied; QVERIFY(f.login());
        QTRY_VERIFY(!f.controller.encryption().value("storeOpening").toBool());
        QVERIFY(!f.keys.available()); QCOMPARE(f.secrets.reads.size(), 1);
        const auto beforeApiGeneration = f.api.generation();
        const auto beforeRefreshLease = f.keys.lease();
        QSignalSpy ready(&f.session, &Session::authenticated);
        f.session.requestRefresh(); QTRY_COMPARE(ready.size(), 1);
        QVERIFY(f.api.generation() > beforeApiGeneration);
        QVERIFY(f.session.ready());
        QCOMPARE(f.keys.lease().owner, beforeRefreshLease.owner);
        QCOMPARE(f.keys.lease().device, beforeRefreshLease.device);
        QCOMPARE(f.keys.lease().issuer, beforeRefreshLease.issuer);
        QCOMPARE(f.keys.lease().generation, beforeRefreshLease.generation);
        QVERIFY(!f.keys.available());
        QVERIFY(!f.controller.encryption().value("storeOpening").toBool());
        QCOMPARE(f.secrets.reads.size(), 1);
        f.secrets.error = SessionSecretStore::Error::None; f.secrets.hold = true;
        const auto authCalls = f.identity.calls;
        f.controller.retryChatKeyStore(); QTRY_COMPARE(f.secrets.pending.size(), size_t(1));
        f.controller.retryChatKeyStore(); QCOMPARE(f.secrets.reads.size(), 2);
        QCOMPARE(f.identity.calls, authCalls); // OS retry is not an API request.
        f.secrets.release(); QTRY_VERIFY(f.keys.available());
    }

    void logoutClosesMemoryAndReopeningKeepsTheDurableDevice() {
        Fixture f; QVERIFY(f.login()); QTRY_VERIFY(f.keys.available());
        const auto before = f.keys.lease(); const auto device = f.keys.device().id;
        f.session.logout(); QVERIFY(!f.keys.available());
        QVERIFY(!f.controller.encryption().value("available").toBool());
        QVERIFY(!f.controller.encryption().value("storeOpening").toBool());
        QVERIFY(f.keys.read(before, {streamId, returnedId}).value.bytes.isEmpty());
        QCOMPARE(QDir(f.directory.filePath("keys")).entryList({"*.sqlite3"}, QDir::Files).size(), 1);
        QVERIFY(f.login()); QTRY_VERIFY(f.keys.available());
        QCOMPARE(f.keys.device().id, device); QCOMPARE(f.secrets.writes.size(), 1);
        QCOMPARE(f.secrets.reads.size(), 2);
    }

    void cacheMaintenanceDoesNotReopenTheIndependentSecretStore() {
        Fixture f; QVERIFY(f.login()); QTRY_VERIFY(f.keys.available());
        const auto before = f.keys.lease();
        NativeCacheService cache(f.directory.filePath("cache"));
        NativePreferences preferences(f.directory.filePath("preferences.ini"));
        bindNativeCachePreferences(cache, preferences);
        QCOMPARE(preferences.cacheLimitGb(), 10);
        QVERIFY(cache.open(f.api.server(), f.session.project(), f.session.user())); QTRY_VERIFY(cache.available());
        preferences.setCacheLimitGb(1); QTRY_COMPARE(cache.appliedDiskLimitBytes(), 1000000000LL);
        QTRY_VERIFY(cache.available());
        QCOMPARE(f.keys.lease().generation, before.generation); QCOMPARE(f.secrets.reads.size(), 1);
        cache.shutdown(); QTRY_VERIFY(cache.stopped());
        QVERIFY(f.keys.available()); QCOMPARE(f.keys.lease().generation, before.generation);
    }

    void signoutReenteredFromOpeningPublicationCannotStartAnOldOwnerRead() {
        Fixture f; bool signedOut = false;
        connect(&f.controller, &NativeController::stateChanged, this, [&] {
            if (!signedOut && f.controller.encryption().value("storeOpening").toBool()) {
                signedOut = true; f.session.logout();
            }
        });
        f.session.login(QUrl("https://runtime.example.invalid"), "synthetic", "synthetic");
        QTRY_COMPARE(f.session.projects().size(), 2);
        f.session.selectProject(projectId); QTRY_VERIFY(signedOut);
        QVERIFY(!f.session.ready()); QVERIFY(!f.keys.available());
        QVERIFY(f.secrets.reads.isEmpty()); QVERIFY(f.secrets.writes.isEmpty());
        QVERIFY(!f.controller.encryption().value("storeOpening").toBool());
    }
};

QTEST_GUILESS_MAIN(NativeChatRuntimeTest)
#include "native-chat-runtime-test.moc"
