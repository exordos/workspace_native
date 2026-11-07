#include "native-chat-key-vault.h"
#include "native-chat-crypto.h"

#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUuid>
#include <cstdio>

using namespace workspace;
namespace {
const QUrl Server("https://workspace.example.test");
const QString Project("10000000-0000-4000-8000-000000000001");
const NativeChatKeyVault::Address Address{"30000000-0000-4000-8000-000000000003", "40000000-0000-4000-8000-000000000004"};
const QString RequestId("50000000-0000-4000-8000-000000000005");
const QString PlacementId("60000000-0000-4000-8000-000000000006");
const QString ResponseId("70000000-0000-4000-8000-000000000007");

int worker(QCoreApplication &app, const QStringList &arguments)
{
    if (arguments.size() != 5 || !arguments[3].startsWith("cassi-native-vault-acceptance-")) return 2;
    const auto operation = arguments[2];
    const auto user = arguments[3].mid(QString("cassi-native-vault-acceptance-").size());
    const auto owner = SessionVault::accountId(Server, Project, user);
    if (owner.isEmpty()) return 2;
    QFile input; if (!input.open(stdin, QIODevice::ReadOnly)) return 2;
    auto expected = input.readAll();
    NativeChatKeyVault vault(arguments[4]); KeychainSessionSecretStore backend;
    QTimer deadline; deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &app, [&] { app.exit(3); }); deadline.start(15000);
    QTimer::singleShot(0, &vault, [&] {
        if (operation == "master-delete") {
            backend.remove("chat-master-v1:" + owner, [&](SessionSecretStore::Error error) {
                app.exit(error == SessionSecretStore::Error::None || error == SessionSecretStore::Error::NotFound ? 0 : 4);
            });
        } else if (operation == "master-missing") {
            backend.read("chat-master-v1:" + owner, [&](QByteArray bytes, SessionSecretStore::Error error) {
                const bool absent = error == SessionSecretStore::Error::NotFound && bytes.isEmpty();
                bytes.fill(0); app.exit(absent ? 0 : 4);
            });
        } else {
            vault.open(Server, Project, user, "cassi synthetic Linux acceptance", [&](NativeChatKeyVault::Failure error) {
                using Failure = NativeChatKeyVault::Failure;
                if (error != Failure::None) { app.exit(10 + int(error)); return; }
                if (operation == "write" && expected.size() == 32)
                    app.exit(vault.put(vault.lease(), Address, expected, 0) == Failure::None ? 0 : 6);
                else if (operation == "read" && expected.size() == 32) {
                    auto result = vault.read(vault.lease(), Address);
                    const bool matches = result.failure == Failure::None && result.value.bytes == expected;
                    result.value.bytes.fill(0); app.exit(matches ? 0 : 7);
                } else if (operation == "mark-sending" && expected.isEmpty()) {
                    app.exit(vault.markForSending(vault.lease(), Address, 0) == Failure::None ? 0 : 26);
                } else if (operation == "sending-read" && expected.size() == 32) {
                    auto latest = vault.lastAvailableKey(vault.lease(), Address.stream);
                    const auto metadata = vault.listKeys(vault.lease(), Address.stream);
                    const bool matches = latest.failure == Failure::None && latest.key == Address.key
                        && latest.value.bytes == expected && latest.value.sendOrder == 1
                        && metadata.failure == Failure::None && metadata.values.size() == 1
                        && metadata.values.first().key == Address.key
                        && metadata.values.first().receivedAt == latest.value.receivedAt;
                    latest.value.bytes.fill(0); app.exit(matches ? 0 : 27);
                } else if (operation == "remove" && expected.isEmpty())
                    app.exit(vault.remove(vault.lease(), Address) == Failure::None ? 0 : 8);
                else if ((operation == "removed" || operation == "removed-twice") && expected.isEmpty()) {
                    auto result = vault.read(vault.lease(), Address);
                    app.exit(result.failure == Failure::None && result.value.bytes.isEmpty()
                        && result.value.deletionRevision == (operation == "removed" ? 1 : 2) ? 0 : 9);
                } else if (operation == "request-write" && expected.size() == 64) {
                    NativeChatKeyVault::PendingRequest pending{RequestId, PlacementId, "Frozen synthetic request",
                        expected.first(32), expected.last(32), 123456789, 1, {}};
                    app.exit(vault.putPendingRequest(vault.lease(), Address, pending) == Failure::None ? 0 : 22);
                } else if (operation == "request-read" && expected.size() == 64) {
                    auto pending = vault.pendingRequests(vault.lease(), Address);
                    const bool matches = pending.failure == Failure::None && pending.values.size() == 1
                        && pending.values.first().publicKey == expected.first(32)
                        && pending.values.first().privateKey == expected.last(32)
                        && pending.values.first().messageId == PlacementId
                        && pending.values.first().deletionRevision == 1;
                    for (auto &request : pending.values) request.privateKey.fill(0);
                    app.exit(matches ? 0 : 23);
                } else if (operation == "request-complete" && expected.size() == 32) {
                    const NativeChatKeyVault::Completion completion{RequestId, PlacementId, ResponseId,
                        NativeChatKeyVault::Outcome::Granted};
                    app.exit(vault.completeRequest(vault.lease(), Address, completion, expected) == Failure::None ? 0 : 24);
                } else if (operation == "request-finished" && expected.size() == 32) {
                    const auto pending = vault.pendingRequests(vault.lease(), Address);
                    const auto completion = vault.requestCompletion(vault.lease(), Address, RequestId);
                    const auto byMessage = vault.requestCompletionByMessage(vault.lease(), Address, PlacementId);
                    auto key = vault.read(vault.lease(), Address);
                    const bool finished = pending.failure == Failure::None && pending.values.isEmpty()
                        && completion.failure == Failure::None && completion.value
                        && completion.value->responseMessage == ResponseId
                        && byMessage.failure == Failure::None && byMessage.value && byMessage.value->localId == RequestId
                        && key.failure == Failure::None && key.value.bytes == expected && key.value.deletionRevision == 1;
                    key.value.bytes.fill(0); app.exit(finished ? 0 : 25);
                } else app.exit(2);
            });
        }
    });
    const auto result = app.exec(); expected.fill(0); return result;
}
} // namespace

class NativeChatKeyVaultPlatformTest final : public QObject {
    Q_OBJECT
    QString m_identity;
    QTemporaryDir m_directory;
    bool m_attempted = false;
    bool run(const QString &operation, const QByteArray &material = {}) {
        QProcess child; child.setProgram(QCoreApplication::applicationFilePath());
        child.setArguments({"--worker", operation, m_identity, m_directory.path()}); child.start();
        if (!child.waitForStarted(5000)) return false;
        if (!material.isEmpty()) child.write(material);
        child.closeWriteChannel();
        if (!child.waitForFinished(20000)) { child.kill(); child.waitForFinished(5000); return false; }
        // Never forward backend diagnostics, expected keys or decrypted bytes.
        const bool success = child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0;
        if (!success) qWarning("Vault stage %s exited with status %d", qPrintable(operation), child.exitCode());
        return success;
    }
private slots:
    void realProviderRestoresTheEncryptedVaultAcrossProcesses() {
        QVERIFY(m_directory.isValid());
        m_identity = "cassi-native-vault-acceptance-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        QVERIFY2(run("master-missing"), "Secure-store preflight failed; no vault write was attempted");
        auto material = QUuid::createUuid().toRfc4122() + QUuid::createUuid().toRfc4122();
        m_attempted = true;
        QVERIFY2(run("write", material), "Vault write using the real OS provider failed");
        QVERIFY2(run("read", material), "A fresh process did not restore the exact vault key");
        QVERIFY2(run("mark-sending"), "Sending-key metadata update failed");
        QVERIFY2(run("sending-read", material), "A fresh process did not restore generation metadata and the sending key"); material.fill(0);
        QVERIFY2(run("remove"), "Vault key deletion failed");
        QVERIFY2(run("removed"), "A fresh process did not restore the deletion revision");
        auto pair = NativeChatCrypto::createRequestKeyPair(); QVERIFY(pair);
        auto requestMaterial = pair.publicKey + pair.privateKey; pair.privateKey.fill(0);
        QVERIFY2(run("request-write", requestMaterial), "Durable private-request write failed");
        QVERIFY2(run("request-read", requestMaterial), "A fresh process did not restore the exact private request"); requestMaterial.fill(0);
        // Grant completion restores a generation after its explicit deletion;
        // the pending request carries the current deletion revision.
        auto grantedKey = NativeChatCrypto::createGroupKey(); QVERIFY(grantedKey);
        QVERIFY2(run("request-complete", grantedKey.bytes), "Atomic grant completion failed");
        QVERIFY2(run("request-finished", grantedKey.bytes), "A fresh process did not restore the grant, receipts and removed private request");
        grantedKey.bytes.fill(0);
        QVERIFY2(run("remove"), "Granted-key cleanup failed");
        QVERIFY2(run("removed-twice"), "Granted-key deletion revision was not restored");
        QVERIFY2(run("master-delete"), "Test-owned master cleanup failed");
        QVERIFY2(run("master-missing"), "Test-owned master is still readable");
    }
    void cleanupTestCase() {
        if (!m_attempted) return;
        QVERIFY2(run("master-delete") && run("master-missing"), "Test-owned OS master cleanup is unconfirmed");
    }
};

int main(int argc, char **argv)
{
    QCoreApplication::setOrganizationName("Exordos");
    QCoreApplication::setApplicationName("Workspace Native Vault Acceptance");
    QCoreApplication app(argc, argv);
    if (app.arguments().value(1) == "--worker") return worker(app, app.arguments());
    NativeChatKeyVaultPlatformTest test; return QTest::qExec(&test, argc, argv);
}

#include "native-chat-key-vault-platform-test.moc"
