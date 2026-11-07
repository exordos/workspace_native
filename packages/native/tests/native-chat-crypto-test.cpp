#include "native-chat-crypto.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTest>
#include <cstdio>

using workspace::NativeChatCrypto;
using Crypto = NativeChatCrypto;
using Failure = Crypto::Failure;
namespace {
const Crypto::Context Context{"10000000-0000-4000-8000-000000000001", "20000000-0000-4000-8000-000000000002"};
const Crypto::GrantContext Grant{Context.streamUuid, Context.keyUuid, "30000000-0000-4000-8000-000000000003",
                                "sender-device", "recipient-device"};
QByteArray from64(const QJsonObject &value, const char *key) { return QByteArray::fromBase64(value.value(key).toString().toLatin1()); }
Crypto::Context context(const QJsonObject &value) { return {value.value("streamUuid").toString(), value.value("keyUuid").toString()}; }
Crypto::GrantContext grantContext(const QJsonObject &value)
{
    return {value.value("streamUuid").toString(), value.value("keyUuid").toString(), value.value("requestMessageUuid").toString(),
        value.value("senderDeviceId").toString(), value.value("recipientDeviceId").toString()};
}
int interop()
{
    QFile input; if (!input.open(stdin, QIODevice::ReadOnly)) return 2;
    const auto requests = QJsonDocument::fromJson(input.readAll()).array(); QJsonArray output;
    for (const auto &request : requests) {
        const auto value = request.toObject(); const auto operation = value.value("operation").toString();
        if (operation == "keypair") {
            const auto pair = Crypto::createRequestKeyPair();
            output.append(QJsonObject{{"failure", int(pair.failure)}, {"publicKey", QString::fromLatin1(pair.publicKey.toBase64())},
                                      {"privateKey", QString::fromLatin1(pair.privateKey.toBase64())}});
            continue;
        }
        Crypto::Result result;
        if (operation == "encryptMessage") result = Crypto::encryptMessage(from64(value, "groupKey"), from64(value, "plaintext"), context(value));
        else if (operation == "decryptMessage") result = Crypto::decryptMessage(from64(value, "groupKey"), from64(value, "envelope"), context(value));
        else if (operation == "encryptGrant") result = Crypto::encryptGroupKeyGrant(from64(value, "groupKey"), from64(value, "publicKey"), grantContext(value));
        else if (operation == "decryptGrant") result = Crypto::decryptGroupKeyGrant(from64(value, "privateKey"), from64(value, "publicKey"), from64(value, "envelope"), grantContext(value));
        output.append(QJsonObject{{"failure", int(result.failure)}, {"bytes", QString::fromLatin1(result.bytes.toBase64())}});
    }
    QFile out; if (!out.open(stdout, QIODevice::WriteOnly)) return 2;
    const auto encoded = QJsonDocument(output).toJson(QJsonDocument::Compact);
    if (out.write(encoded) != encoded.size()) return 2;
    return 0;
}
} // namespace

class NativeChatCryptoTest final : public QObject {
    Q_OBJECT
private slots:
    void randomKeysAndRequestPairsAreIndependent()
    {
        const auto first = Crypto::createGroupKey(), second = Crypto::createGroupKey();
        QVERIFY(first); QVERIFY(second); QCOMPARE(first.bytes.size(), 32); QCOMPARE(second.bytes.size(), 32);
        QVERIFY(first.bytes != second.bytes);
        const auto a = Crypto::createRequestKeyPair(), b = Crypto::createRequestKeyPair();
        QVERIFY(a); QVERIFY(b); QCOMPARE(a.privateKey.size(), 32); QCOMPARE(a.publicKey.size(), 32);
        QVERIFY(a.privateKey != b.privateKey); QVERIFY(a.publicKey != b.publicKey);
    }

    void binaryMessagesRoundTripWithIndependentSalt_data()
    {
        QTest::addColumn<QByteArray>("plaintext");
        QTest::newRow("empty") << QByteArray{};
        QTest::newRow("utf8") << QString::fromUtf8("Привет 👩🏽‍💻 — 日本語").toUtf8();
        QByteArray binary; for (int n = 0; n < 256; ++n) binary.append(char(n));
        QTest::newRow("all-octets") << binary;
        QTest::newRow("composer-bound") << QByteArray(160000, 'x');
    }
    void binaryMessagesRoundTripWithIndependentSalt()
    {
        QFETCH(QByteArray, plaintext); const auto key = Crypto::createGroupKey(); QVERIFY(key);
        QSet<QByteArray> salts;
        for (int n = 0; n < 8; ++n) {
            const auto sealed = Crypto::encryptMessage(key.bytes, plaintext, Context); QVERIFY(sealed);
            QCOMPARE(sealed.bytes.size(), plaintext.size() + 48); salts.insert(sealed.bytes.first(32));
            const auto opened = Crypto::decryptMessage(key.bytes, sealed.bytes, Context); QVERIFY(opened);
            QCOMPARE(opened.bytes, plaintext);
        }
        QCOMPARE(salts.size(), 8);
    }

    void messageTamperingNeverReturnsUnauthenticatedPlaintext_data()
    {
        QTest::addColumn<int>("offset");
        QTest::newRow("salt") << 0; QTest::newRow("ciphertext") << 32; QTest::newRow("tag") << 50;
    }
    void messageTamperingNeverReturnsUnauthenticatedPlaintext()
    {
        QFETCH(int, offset); const auto key = Crypto::createGroupKey(); QVERIFY(key);
        auto sealed = Crypto::encryptMessage(key.bytes, "abcdef", Context); QVERIFY(sealed);
        sealed.bytes[offset] = char(sealed.bytes.at(offset) ^ 1);
        const auto opened = Crypto::decryptMessage(key.bytes, sealed.bytes, Context);
        QCOMPARE(opened.failure, Failure::InvalidCiphertext); QVERIFY(opened.bytes.isEmpty());
    }

    void messageContextAndKeyAreAuthenticated()
    {
        const auto key = Crypto::createGroupKey(), other = Crypto::createGroupKey(); QVERIFY(key); QVERIFY(other);
        const auto sealed = Crypto::encryptMessage(key.bytes, "text", Context); QVERIFY(sealed);
        for (const auto &context : {Crypto::Context{"other-stream", Context.keyUuid}, Crypto::Context{Context.streamUuid, "other-key"}}) {
            const auto opened = Crypto::decryptMessage(key.bytes, sealed.bytes, context);
            QCOMPARE(opened.failure, Failure::InvalidCiphertext); QVERIFY(opened.bytes.isEmpty());
        }
        const auto wrong = Crypto::decryptMessage(other.bytes, sealed.bytes, Context);
        QCOMPARE(wrong.failure, Failure::InvalidCiphertext); QVERIFY(wrong.bytes.isEmpty());
        QCOMPARE(Crypto::encryptMessage(QByteArray(31, 'x'), "text", Context).failure, Failure::InvalidKey);
        QCOMPARE(Crypto::decryptMessage(key.bytes, QByteArray(47, 'x'), Context).failure, Failure::InvalidCiphertext);
        QCOMPARE(Crypto::encryptMessage(key.bytes, "text", {{}, Context.keyUuid}).failure, Failure::ContextMismatch);
        const QByteArray oversized(Crypto::MaximumMessageBytes + 1, 'x');
        QCOMPARE(Crypto::encryptMessage(key.bytes, oversized, Context).failure, Failure::InvalidCiphertext);
        QCOMPARE(Crypto::decryptMessage(key.bytes, oversized + QByteArray(48, 'x'), Context).failure, Failure::InvalidCiphertext);
    }

    void hpkeGrantRoundTripsForExactRequest()
    {
        const auto group = Crypto::createGroupKey(); const auto pair = Crypto::createRequestKeyPair(); QVERIFY(group); QVERIFY(pair);
        const auto grant = Crypto::encryptGroupKeyGrant(group.bytes, pair.publicKey, Grant); QVERIFY(grant);
        QCOMPARE(grant.bytes.size(), 85); QCOMPARE(grant.bytes.first(5).toHex(), QByteArray("5753434701"));
        const auto opened = Crypto::decryptGroupKeyGrant(pair.privateKey, pair.publicKey, grant.bytes, Grant);
        QVERIFY(opened); QCOMPARE(opened.bytes, group.bytes);
    }

    void grantBindsEveryContextField_data()
    {
        QTest::addColumn<int>("field");
        for (int n = 0; n < 5; ++n) QTest::newRow(qPrintable(QString::number(n))) << n;
    }
    void grantBindsEveryContextField()
    {
        QFETCH(int, field); const auto group = Crypto::createGroupKey(); const auto pair = Crypto::createRequestKeyPair();
        QVERIFY(group); QVERIFY(pair);
        const auto grant = Crypto::encryptGroupKeyGrant(group.bytes, pair.publicKey, Grant); QVERIFY(grant);
        auto changed = Grant;
        QString *fields[]{&changed.streamUuid, &changed.keyUuid, &changed.requestMessageUuid, &changed.senderDeviceId, &changed.recipientDeviceId};
        *fields[field] += "-changed";
        const auto opened = Crypto::decryptGroupKeyGrant(pair.privateKey, pair.publicKey, grant.bytes, changed);
        QCOMPARE(opened.failure, Failure::InvalidCiphertext); QVERIFY(opened.bytes.isEmpty());
    }

    void malformedGrantOrWrongRequestKeyNeverReturnsAKey()
    {
        const auto group = Crypto::createGroupKey(); const auto pair = Crypto::createRequestKeyPair(), other = Crypto::createRequestKeyPair();
        QVERIFY(group); QVERIFY(pair); QVERIFY(other);
        const auto grant = Crypto::encryptGroupKeyGrant(group.bytes, pair.publicKey, Grant); QVERIFY(grant);
        for (const int offset : {0, 4, 5, 36, 37, 84}) {
            auto bytes = grant.bytes; bytes[offset] = char(bytes.at(offset) ^ 1);
            const auto result = Crypto::decryptGroupKeyGrant(pair.privateKey, pair.publicKey, bytes, Grant);
            QCOMPARE(result.failure, Failure::InvalidCiphertext); QVERIFY(result.bytes.isEmpty());
        }
        for (const auto &bytes : {grant.bytes.first(84), grant.bytes + 'x', QByteArray{}}) {
            const auto result = Crypto::decryptGroupKeyGrant(pair.privateKey, pair.publicKey, bytes, Grant);
            QCOMPARE(result.failure, Failure::InvalidCiphertext); QVERIFY(result.bytes.isEmpty());
        }
        QCOMPARE(Crypto::decryptGroupKeyGrant(other.privateKey, pair.publicKey, grant.bytes, Grant).failure, Failure::InvalidCiphertext);
        QCOMPARE(Crypto::decryptGroupKeyGrant(pair.privateKey, other.publicKey, grant.bytes, Grant).failure, Failure::InvalidCiphertext);
        QCOMPARE(Crypto::encryptGroupKeyGrant(group.bytes, QByteArray(32, '\0'), Grant).failure, Failure::InvalidKey);
        QCOMPARE(Crypto::encryptGroupKeyGrant(group.bytes, QByteArray(31, 'x'), Grant).failure, Failure::InvalidKey);
        auto missing = Grant; missing.recipientDeviceId.clear();
        QCOMPARE(Crypto::encryptGroupKeyGrant(group.bytes, pair.publicKey, missing).failure, Failure::ContextMismatch);
        QCOMPARE(Crypto::decryptGroupKeyGrant(pair.privateKey, pair.publicKey, grant.bytes, missing).failure, Failure::InvalidKey);
    }

    void sourceProviderVectorsDecryptWithoutReencoding()
    {
        QFile file(QString::fromUtf8(WORKSPACE_NATIVE_CRYPTO_VECTORS)); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto root = QJsonDocument::fromJson(file.readAll()).object();
        QVERIFY(root.value("synthetic").toBool()); QCOMPARE(root.value("provider").toString(), QString("chat-real-crypto-provider.lib.ts"));
        int count = 0;
        for (const auto &value : root.value("messages").toArray()) {
            const auto item = value.toObject(); const auto opened = Crypto::decryptMessage(from64(item, "groupKey"), from64(item, "envelope"), context(item));
            QVERIFY(opened); QCOMPARE(opened.bytes, from64(item, "plaintext")); ++count;
        }
        QVERIFY(count >= 5); count = 0;
        for (const auto &value : root.value("grants").toArray()) {
            const auto item = value.toObject(); const auto opened = Crypto::decryptGroupKeyGrant(from64(item, "privateKey"), from64(item, "publicKey"), from64(item, "envelope"), grantContext(item));
            QVERIFY(opened); QCOMPARE(opened.bytes, from64(item, "groupKey")); ++count;
        }
        QVERIFY(count >= 2);
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().contains("--interop")) return interop();
    NativeChatCryptoTest tests; return QTest::qExec(&tests, argc, argv);
}
#include "native-chat-crypto-test.moc"
