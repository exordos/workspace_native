#include "native-updater.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaEnum>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslError>
#include <QSslSocket>
#include <QTimer>
#include <QTemporaryDir>
#include <openssl/ssl.h>
#include <cstdio>
#include <functional>
#include <memory>

static QByteArray read(const QString &path) {
    QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray{};
}
static QString sha(const QByteArray &bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
static void output(const QJsonObject &value) {
    const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Compact);
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
    std::fputc('\n', stdout); std::fflush(stdout);
}
static QJsonArray mappings() {
    QFile maps(QStringLiteral("/proc/self/maps")); QSet<QString> paths;
    if (maps.open(QIODevice::ReadOnly)) for (;;) {
        const auto line = maps.readLine(); if (line.isEmpty()) break;
        const auto slash = line.indexOf('/'); if (slash < 0) continue;
        const auto path = QString::fromUtf8(line.mid(slash).trimmed());
        if (path.contains("/libQt6Core.so") || path.contains("/libQt6Network.so")
            || path.contains("/libssl.so") || path.contains("/libcrypto.so")
            || path.endsWith("/libqopensslbackend.so")) paths.insert(path);
    }
    auto names = paths.values(); names.sort(); QJsonArray result;
    for (const auto &path : names) result.append(QJsonObject{{"path", path}, {"sha256", sha(read(path))}});
    return result;
}
// This is an oracle, not a product retry: only TWO public check() calls exist.
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv); QCoreApplication::setApplicationVersion("0.1.4");
    QCommandLineParser parser;
    parser.addOptions({
        QCommandLineOption(QStringLiteral("url"), QStringLiteral("Owned literal HTTPS loopback URL"), QStringLiteral("url")),
        QCommandLineOption(QStringLiteral("ca"), QStringLiteral("Owned ephemeral CA certificate"), QStringLiteral("file")),
        QCommandLineOption(QStringLiteral("expected"), QStringLiteral("Exact public synthetic JSON bytes"), QStringLiteral("file"))});
    parser.process(app);
    const QUrl url(parser.value("url")); const auto expected = read(parser.value("expected"));
    const auto ca = QSslCertificate::fromData(read(parser.value("ca")));
    QTemporaryDir cache;
    if (url.scheme() != "https" || url.host() != "127.0.0.1" || url.port() <= 0
        || url.path() != "/releases/latest" || url.hasQuery() || url.hasFragment() || !url.userInfo().isEmpty()
        || expected.isEmpty() || expected.size() > 4 * 1024 * 1024 || ca.size() != 1 || !cache.isValid()
        || OPENSSL_init_ssl(0, nullptr) != 1) return 2;
    // Dedicated fixture process only. Restored AFTER the real updater/manager dies.
    struct FixtureTrust {
        QSslConfiguration original = QSslConfiguration::defaultConfiguration();
        explicit FixtureTrust(const QSslCertificate &certificate) {
            auto local = original; local.setCaCertificates({certificate});
            local.setPeerVerifyMode(QSslSocket::VerifyPeer);
            QSslConfiguration::setDefaultConfiguration(local);
        }
        ~FixtureTrust() { QSslConfiguration::setDefaultConfiguration(original); }
    } trust(ca.front());
    workspace::NativeUpdater updater(url, QStringLiteral("0.1.4"), cache.path());
    auto *network = updater.findChild<QNetworkAccessManager *>(); if (!network) return 3;
    network->setProxy(QNetworkProxy::NoProxy); // Only owned loopback fixture process.
    QElapsedTimer time; time.start(); QJsonArray replies, states, sslErrors;
    int logicalChecks = 0, exitCode = 2; bool complete = false, warmup = false, queued = false;
    qint64 secondStarted = -1;
    QString outcome = QStringLiteral("UNEXPECTED_OR_INCOMPLETE");
    std::function<void()> submit, consider;
    QObject observer; // Disconnect before captured state, then updater, then trust restoration.
    QTimer overall; overall.setSingleShot(true);
    QObject::connect(&overall, &QTimer::timeout, &observer, [&] {
        output({{"event", "observer-deadline"}, {"atMs", double(time.elapsed())}}); app.quit();
    });
    overall.start(65000); // Two public operations, each existing product deadline <=30s.
    submit = [&] {
        ++logicalChecks;
        if (logicalChecks > 2) { app.quit(); return; }
        if (logicalChecks == 2) secondStarted = time.elapsed();
        output({{"event", "public-check"}, {"ordinal", logicalChecks}, {"atMs", double(time.elapsed())}});
        updater.check(); // Public product API; no direct request, retry, private completion or third call.
    };
    consider = [&] {
        if (complete || queued || logicalChecks < 1 || replies.isEmpty()) return;
        const auto state = updater.state();
        if (state != "current" && state != "error" && state != "available") return;
        if (logicalChecks == 1) {
            const auto first = replies.at(0).toObject();
            warmup = state == "current" && replies.size() == 1 && first.value("errorCode").toInt() == 0
                && first.value("status").toInt() == 200 && first.value("http2Used").toBool()
                && first.value("alpn").toString() == "h2" && first.value("contentLength").toInt() == expected.size()
                && sslErrors.isEmpty();
            if (!warmup) { outcome = "WARMUP_SETUP_FAILURE"; app.quit(); return; }
            queued = true;
            QTimer::singleShot(0, &observer, [&] { queued = false; submit(); });
            return;
        }
        if (replies.size() < 2) return;
        const auto fault = replies.at(1).toObject();
        const bool actualFault = fault.value("errorCode").toInt() == 399 && fault.value("status").isNull()
            && fault.value("http2Used").isNull() && fault.value("alpn").toString() == "h2"
            && fault.value("rawHeaderCount").toInt() == 0 && fault.value("unreadBytes").toInt() == 0
            && sslErrors.isEmpty();
        if (!actualFault) { outcome = "NO_MATCHING_REAL_WIRE_FAULT"; app.quit(); return; }
        complete = true;
        // Same unconditional user-facing oracle on future GREEN: second logical
        // check must return current without the test/user issuing a third check.
        if (state != "current") {
            outcome = "INTENDED_RECOVERY_ORACLE_RED"; exitCode = 10;
            output({{"event", "oracle-failure"}, {"message", "Second logical check must recover to current without a third manual Check"},
                {"actualState", state}, {"actualError", updater.error()}, {"publicChecks", logicalChecks}});
        } else {
            const auto recovered = replies.last().toObject();
            if (replies.size() != 3 || recovered.value("errorCode").toInt() != 0
                || recovered.value("status").toInt() != 200 || !recovered.value("http2Used").toBool()
                || recovered.value("alpn").toString() != "h2" || recovered.value("contentLength").toInt() != expected.size()) {
                outcome = "RECOVERY_WITNESS_MISMATCH"; exitCode = 2;
            } else { outcome = "RECOVERY_ORACLE_GREEN"; exitCode = 0; }
        }
        app.quit();
    };
    QObject::connect(&updater, &workspace::NativeUpdater::changed, &observer, [&] {
        const QJsonObject state{{"state", updater.state()}, {"error", updater.error()},
            {"logicalCheck", logicalChecks}, {"atMs", double(time.elapsed())}};
        states.append(state); output({{"event", "public-state"}, {"value", state}});
        // Queued observation only. This does not repair, drive or retry a request.
        QTimer::singleShot(0, &observer, consider);
    });
    QObject::connect(network, &QNetworkAccessManager::sslErrors, &observer, [&](QNetworkReply *, const QList<QSslError> &errors) {
        for (const auto &error : errors) sslErrors.append(int(error.error())); // Never ignore.
    });
    QObject::connect(network, &QNetworkAccessManager::finished, &observer, [&](QNetworkReply *reply) {
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        const auto h2 = reply->attribute(QNetworkRequest::Http2WasUsedAttribute);
        QJsonObject safeHeaders;
        for (const auto &key : {QByteArray("Content-Type"), QByteArray("Content-Length"), QByteArray("X-Fixture-Reply")})
            if (reply->hasRawHeader(key)) safeHeaders.insert(QString::fromLatin1(key), QString::fromLatin1(reply->rawHeader(key)));
        QJsonObject record{{"wireReplyOrdinal", int(replies.size())+1}, {"logicalCheck", logicalChecks}, {"atMs", double(time.elapsed())},
            {"errorCode", int(reply->error())}, {"errorString", reply->errorString()},
            {"status", status.isValid() ? QJsonValue(status.toInt()) : QJsonValue(QJsonValue::Null)},
            {"http2Used", h2.isValid() ? QJsonValue(h2.toBool()) : QJsonValue(QJsonValue::Null)},
            {"alpn", QString::fromLatin1(reply->sslConfiguration().nextNegotiatedProtocol())},
            {"rawHeaderCount", int(reply->rawHeaderPairs().size())}, {"unreadBytes", double(reply->bytesAvailable())},
            {"contentLength", reply->header(QNetworkRequest::ContentLengthHeader).isValid()
                ? QJsonValue(double(reply->header(QNetworkRequest::ContentLengthHeader).toLongLong())) : QJsonValue(QJsonValue::Null)},
            {"safeHeaders", safeHeaders}};
        // Never read/peek the live body: the real updater alone owns consumption/parse.
        replies.append(record); output({{"event", "real-reply-finished"}, {"value", record}});
        QTimer::singleShot(0, &observer, consider);
    });
    QTimer::singleShot(0, &observer, submit);
    app.exec();
    output({{"event", "summary"}, {"status", outcome}, {"oracleComplete", complete}, {"warmupCurrent", warmup},
        {"publicChecks", logicalChecks}, {"wireReplies", replies}, {"publicStates", states}, {"sslErrors", sslErrors},
        {"publicFinalState", updater.state()}, {"publicFinalError", updater.error()},
        {"secondLogicalElapsedMs", secondStarted >= 0 ? double(time.elapsed()-secondStarted) : -1.0},
        {"qtRuntimeVersion", qVersion()}, {"runtimeMappings", mappings()},
        {"expectedServerJsonSha256", sha(expected)}, {"bodyObserver", "No read/peek; server bytes/hash plus real updater parse are the witnesses"},
        {"manualThirdCheckIssued", false}, {"automaticRecoveryObserved", complete && exitCode == 0 && logicalChecks == 2 && replies.size() == 3}});
    return exitCode;
}
