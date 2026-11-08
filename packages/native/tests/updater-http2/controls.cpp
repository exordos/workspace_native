#include "native-updater.h"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QSet>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslError>
#include <QSslSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <openssl/ssl.h>
#include <cstdio>
#include <functional>
#include <memory>

static QByteArray read(const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
static QString sha(const QByteArray &bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); }
static void output(const QJsonObject &value) {
    const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Compact);
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout); std::fputc('\n', stdout); std::fflush(stdout);
}
static QJsonArray mappings() {
    QFile file(QStringLiteral("/proc/self/maps")); QSet<QString> paths;
    if (file.open(QIODevice::ReadOnly)) for (;;) {
        const auto line = file.readLine(); if (line.isEmpty()) break;
        const auto start = line.indexOf('/'); if (start < 0) continue;
        const auto path = QString::fromUtf8(line.mid(start).trimmed());
        if (path.contains("/libQt6Core.so") || path.contains("/libQt6Network.so")
            || path.contains("/libssl.so") || path.contains("/libcrypto.so") || path.endsWith("/libqopensslbackend.so")) paths.insert(path);
    }
    auto sorted = paths.values(); sorted.sort(); QJsonArray result;
    for (const auto &path : sorted) result.append(QJsonObject{{"path", path}, {"sha256", sha(read(path))}});
    return result;
}
static bool eligible(const QJsonObject &reply) {
    return reply.value("errorCode").toInt() == 399 && reply.value("status").isNull()
        && reply.value("http2Used").isNull() && reply.value("alpn").toString() == "h2"
        && reply.value("rawHeaderCount").toInt() == 0 && reply.value("unreadBytes").toInt() == 0;
}
static bool healthy(const QJsonObject &reply, qsizetype bytes) {
    return reply.value("errorCode").toInt() == 0 && reply.value("status").toInt() == 200
        && reply.value("http2Used").toBool() && reply.value("alpn").toString() == "h2"
        && reply.value("contentLength").toInteger() == bytes;
}

// QCore-only real transport controls. Never reads a reply body or calls private updater methods.
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv); QCoreApplication::setApplicationVersion("0.1.4");
    if (OPENSSL_init_ssl(0, nullptr) != 1) return 2;
    QCommandLineParser parser;
    for (const auto &name : {"case", "url", "ca", "body", "gate"})
        parser.addOption(QCommandLineOption(QString::fromLatin1(name), QString::fromLatin1(name), QStringLiteral("value")));
    parser.process(app);
    const auto mode = parser.value("case"); const QUrl url(parser.value("url"));
    QTemporaryDir cache; if (!cache.isValid()) return 2;
    if (mode == "endpoint-admission") {
        QJsonArray records;
        const QString production = QStringLiteral("https://api.github.com/repos/exordos/workspace_native/releases/latest");
        const QStringList negatives{production + "?override=1", production + "#fragment", production + "/extra",
            QStringLiteral("http://api.github.com/repos/exordos/workspace_native/releases/latest"),
            QStringLiteral("https://192.0.2.1:443/releases/latest"), QStringLiteral("https://localhost:443/releases/latest"),
            QStringLiteral("https://127.0.0.1.evil.invalid:443/releases/latest"),
            QStringLiteral("https://127.0.0.1/releases/latest"), QStringLiteral("https://[::1]/releases/latest"),
            QStringLiteral("https://user@127.0.0.1:443/releases/latest"), QStringLiteral("https://[::1]:443/releases/latest#x"),
            QStringLiteral("https://127.0.0.2:443/releases/latest"), QStringLiteral("https://[::2]:443/releases/latest")};
        bool okay = true;
        for (const auto &text : negatives) {
            workspace::NativeUpdater updater(QUrl(text), QStringLiteral("0.1.4"), cache.path());
            bool checking = false;
            QObject::connect(&updater, &workspace::NativeUpdater::changed, &updater, [&] { checking |= updater.state() == "checking"; });
            updater.check();
            const auto *network = updater.findChild<QNetworkAccessManager *>();
            const bool noReply = network && network->findChildren<QNetworkReply *>().isEmpty();
            okay &= updater.state() == "error" && !checking && noReply;
            records.append(QJsonObject{{"url", text}, {"state", updater.state()}, {"checkingObserved", checking}, {"zeroReplyObjects", noReply}});
        }
        // Positive exact production URL admission, cancelled synchronously BEFORE get(). No external request.
        workspace::NativeUpdater productionUpdater(QUrl(production), QStringLiteral("0.1.4"), cache.path());
        bool admitted = false;
        QObject::connect(&productionUpdater, &workspace::NativeUpdater::changed, &productionUpdater, [&] {
            if (productionUpdater.state() == "checking") { admitted = true; productionUpdater.cancel(); }
        });
        productionUpdater.check();
        const auto *network = productionUpdater.findChild<QNetworkAccessManager *>();
        const bool noReply = network && network->findChildren<QNetworkReply *>().isEmpty();
        okay &= admitted && productionUpdater.state() == "idle" && noReply;
        output({{"event", "summary"}, {"case", mode}, {"passed", okay}, {"negativeEndpoints", records},
            {"exactProductionUrlAdmitted", admitted}, {"productionCancelledBeforeRequest", noReply},
            {"productionFinalState", productionUpdater.state()}, {"externalRequests", 0}});
        return okay ? 0 : 10;
    }
    const QSet<QString> allowed{QStringLiteral("two-fail"), QStringLiteral("reset-cap"), QStringLiteral("http-403"),
        QStringLiteral("http-404"), QStringLiteral("http-429"), QStringLiteral("http-503"), QStringLiteral("schema"),
        QStringLiteral("postheaders"), QStringLiteral("tls"), QStringLiteral("http1"), QStringLiteral("cancel"),
        QStringLiteral("destroy"), QStringLiteral("replacement"), QStringLiteral("deadline"), QStringLiteral("ipv6"), QStringLiteral("archive-policy")};
    const auto expected = read(parser.value("body")); const auto ca = QSslCertificate::fromData(read(parser.value("ca")));
    if (!allowed.contains(mode) || url.scheme() != "https" || !QStringList{"127.0.0.1", "::1"}.contains(url.host())
        || url.port() <= 0 || url.path() != "/releases/latest" || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()
        || expected.isEmpty() || ca.size() != 1 || parser.value("gate").isEmpty()) return 2;
    struct FixtureTrust {
        QSslConfiguration original = QSslConfiguration::defaultConfiguration();
        explicit FixtureTrust(const QSslCertificate &ca) {
            auto config = original; config.setCaCertificates({ca}); config.setPeerVerifyMode(QSslSocket::VerifyPeer);
            QSslConfiguration::setDefaultConfiguration(config);
        }
        ~FixtureTrust() { QSslConfiguration::setDefaultConfiguration(original); }
    } trust(ca.front());
    auto updater = std::make_unique<workspace::NativeUpdater>(url, QStringLiteral("0.1.4"), cache.path());
    QPointer<QNetworkAccessManager> network = updater->findChild<QNetworkAccessManager *>(); if (!network) return 2;
    network->setProxy(QNetworkProxy::NoProxy); // Dedicated owned loopback child only.
    QElapsedTimer clock; clock.start();
    QJsonArray replies, states, sslErrors, actions;
    int checks = 0, exitCode = 10, readyReads = 0, activeAtCompletion = -1;
    bool complete = false, scheduled = false, warmup = false, capExhausted = false, actionQueued = false, actionDone = false;
    qint64 secondStarted = -1;
    QString reason = QStringLiteral("No complete oracle");
    QObject observer;
    QTimer overall; overall.setSingleShot(true);
    std::function<void()> submit, consider;
    auto activeReplies = [&] {
        int count = 0; if (network) for (auto *reply : network->findChildren<QNetworkReply *>()) count += !reply->isFinished(); return count;
    };
    auto finish = [&](bool pass, const QString &message) {
        if (complete) return; complete = true; exitCode = pass ? 0 : 10; reason = message;
        activeAtCompletion = activeReplies(); app.quit();
    };
    auto releaseGate = [&](const QString &receipt) {
        QSaveFile file(parser.value("gate")); const auto bytes = receipt.toUtf8() + '\n';
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
            finish(false, QStringLiteral("Cannot commit owned signal gate")); return;
        }
        output({{"event", "public-gate-released"}, {"receipt", receipt}, {"atMs", double(clock.elapsed())}});
    };
    QObject::connect(&overall, &QTimer::timeout, &observer, [&] { finish(false, QStringLiteral("Observer 65 s deadline")); });
    overall.start(65000);
    submit = [&] {
        if (complete || !updater) return;
        ++checks; if (checks > 3) { finish(false, QStringLiteral("Unexpected public check")); return; }
        if (checks == 2) secondStarted = clock.elapsed();
        output({{"event", "public-check"}, {"ordinal", checks}, {"atMs", double(clock.elapsed())}});
        updater->check();
        // Attach to the actual reply after public get() returned; product consumption stays first.
        if (mode == "postheaders" && checks == 2 && network) for (auto *reply : network->findChildren<QNetworkReply *>()) {
            if (reply->isFinished()) continue;
            QObject::connect(reply, &QNetworkReply::readyRead, &observer, [&, reply] {
                ++readyReads;
                if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200)
                    releaseGate(QStringLiteral("actual-status200-and-readyRead"));
            });
        }
        if (mode == "deadline" && checks == 2) QTimer::singleShot(26000, Qt::PreciseTimer, &observer, [&] {
            if (complete || !updater || updater->state() != "checking" || replies.size() != 1 || activeReplies() != 1) {
                finish(false, QStringLiteral("Original operation did not remain held at 26 s")); return;
            }
            releaseGate(QStringLiteral("second-check-still-checking-at-26s"));
        });
    };
    consider = [&] {
        if (complete || scheduled || checks == 0) return;
        if (actionDone && (mode == "cancel" || mode == "destroy")) {
            // Two queued turns after the public action drain an already queued recovery callback.
            scheduled = true;
            QTimer::singleShot(0, &observer, [&] { QTimer::singleShot(0, &observer, [&] {
                const bool stateOkay = mode == "destroy" ? !updater : updater && updater->state() == "idle";
                finish(stateOkay && replies.size() == 2 && checks == 2 && activeReplies() == 0
                    && eligible(replies.at(1).toObject()) && sslErrors.isEmpty(), QStringLiteral("Public cancellation/deletion invalidated queued recovery"));
            }); }); return;
        }
        if (!updater) return;
        const auto state = updater->state();
        if (state != "current" && state != "error" && state != "available") return;
        const bool single = mode == "tls" || mode == "http1" || mode == "ipv6" || mode == "archive-policy";
        if (!single && checks == 1) {
            if (replies.isEmpty()) return;
            warmup = replies.size() == 1 && state == "current" && healthy(replies.first().toObject(), expected.size()) && sslErrors.isEmpty();
            if (!warmup) { finish(false, QStringLiteral("Warmup failed")); return; }
            scheduled = true; QTimer::singleShot(0, &observer, [&] { scheduled = false; submit(); }); return;
        }
        if (mode == "tls") {
            if (replies.isEmpty()) return;
            finish(checks == 1 && replies.size() == 1 && state == "error" && !sslErrors.isEmpty()
                && replies.first().toObject().value("errorCode").toInt() == int(QNetworkReply::SslHandshakeFailedError)
                && replies.first().toObject().value("status").isNull(), QStringLiteral("Strict TLS hostname failure was not retried")); return;
        }
        if (mode == "http1") {
            if (replies.isEmpty()) return;
            const auto reply = replies.first().toObject();
            finish(checks == 1 && replies.size() == 1 && state == "error" && reply.value("errorCode").toInt() != 399
                && reply.value("errorCode").toInt() != 0 && reply.value("status").isNull()
                && reply.value("alpn").toString() == "http/1.1" && sslErrors.isEmpty(),
                QStringLiteral("Real HTTP/1 close did not create an application retry")); return;
        }
        if (mode == "ipv6" || mode == "archive-policy") {
            if (replies.isEmpty()) return;
            const auto reply = replies.first().toObject();
            const bool transport = checks == 1 && replies.size() == 1 && reply.value("status").toInt() == 200
                && reply.value("errorCode").toInt() == 0 && reply.value("alpn").toString() == "h2" && sslErrors.isEmpty();
            finish(transport && (mode == "ipv6" ? state == "current" && healthy(reply, expected.size()) : state == "error"),
                mode == "ipv6" ? QStringLiteral("Actual IPv6 HTTPS metadata admitted") : QStringLiteral("HTTPS metadata did not enable local archive policy")); return;
        }
        if (replies.size() < 2) return;
        if (mode.startsWith("http-")) {
            finish(checks == 2 && replies.size() == 2 && state == "error"
                && replies.at(1).toObject().value("status").toInt() == mode.mid(5).toInt() && sslErrors.isEmpty(),
                QStringLiteral("Real HTTP error status was not retried")); return;
        }
        if (mode == "schema") {
            const auto reply = replies.at(1).toObject();
            finish(checks == 2 && replies.size() == 2 && state == "error" && reply.value("status").toInt() == 200
                && reply.value("errorCode").toInt() == 0 && sslErrors.isEmpty(), QStringLiteral("Schema failure was not retried")); return;
        }
        if (mode == "postheaders") {
            const auto reply = replies.at(1).toObject();
            finish(checks == 2 && replies.size() == 2 && state == "error" && reply.value("status").toInt() == 200
                && reply.value("http2Used").toBool() && reply.value("alpn").toString() == "h2"
                && reply.value("errorCode").toInt() == 399 && readyReads > 0 && sslErrors.isEmpty(),
                QStringLiteral("Actual headers/body-prefix close was not retried")); return;
        }
        if (mode == "two-fail" || mode == "reset-cap") {
            if (replies.size() < 3) { finish(false, QStringLiteral("First eligible fault was not recovered")); return; }
            const bool capped = state == "error" && eligible(replies.at(1).toObject()) && eligible(replies.at(2).toObject()) && sslErrors.isEmpty();
            if (checks == 2 && mode == "reset-cap") {
                if (!capped || replies.size() != 3) { finish(false, QStringLiteral("First operation cap failed")); return; }
                capExhausted = true; scheduled = true;
                QTimer::singleShot(0, &observer, [&] { scheduled = false; submit(); }); return;
            }
            if (mode == "two-fail") {
                finish(checks == 2 && replies.size() == 3 && capped, QStringLiteral("Second eligible fault is terminal")); return;
            }
            finish(checks == 3 && capExhausted && replies.size() == 5 && state == "current"
                && eligible(replies.at(3).toObject()) && healthy(replies.at(4).toObject(), expected.size()) && sslErrors.isEmpty(),
                QStringLiteral("New explicit check reset the one-recovery budget")); return;
        }
        if (mode == "replacement") {
            if (!actionDone) return;
            finish(checks == 3 && replies.size() == 3 && state == "current" && eligible(replies.at(1).toObject())
                && healthy(replies.at(2).toObject(), expected.size()) && sslErrors.isEmpty(),
                QStringLiteral("Reentrant public replacement fenced the old queued request")); return;
        }
        if (mode == "deadline") {
            const auto elapsed = clock.elapsed() - secondStarted;
            finish(checks == 2 && replies.size() == 3 && state == "error" && updater->error() == "The update operation timed out."
                && eligible(replies.at(1).toObject()) && elapsed >= 26000 && elapsed <= 34000 && sslErrors.isEmpty(),
                QStringLiteral("Held retry ended at original 30 s total deadline, not 56 s")); return;
        }
        if (mode != "cancel" && mode != "destroy") finish(false, QStringLiteral("Unexpected terminal state"));
    };
    QObject::connect(updater.get(), &workspace::NativeUpdater::changed, &observer, [&] {
        if (!updater) return;
        states.append(QJsonObject{{"state", updater->state()}, {"error", updater->error()}, {"check", checks}, {"atMs", double(clock.elapsed())}});
        QTimer::singleShot(0, &observer, consider);
    });
    QObject::connect(network, &QNetworkAccessManager::sslErrors, &observer, [&](QNetworkReply *, const QList<QSslError> &errors) {
        for (const auto &error : errors) sslErrors.append(int(error.error())); // No ignoreSslErrors.
    });
    QObject::connect(network, &QNetworkAccessManager::finished, &observer, [&](QNetworkReply *reply) {
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        const auto h2 = reply->attribute(QNetworkRequest::Http2WasUsedAttribute);
        QJsonObject value{{"errorCode", int(reply->error())}, {"status", status.isValid() ? QJsonValue(status.toInt()) : QJsonValue(QJsonValue::Null)},
            {"http2Used", h2.isValid() ? QJsonValue(h2.toBool()) : QJsonValue(QJsonValue::Null)},
            {"alpn", QString::fromLatin1(reply->sslConfiguration().nextNegotiatedProtocol())},
            {"rawHeaderCount", int(reply->rawHeaderPairs().size())}, {"unreadBytes", double(reply->bytesAvailable())},
            {"contentLength", reply->header(QNetworkRequest::ContentLengthHeader).isValid()
                ? QJsonValue(double(reply->header(QNetworkRequest::ContentLengthHeader).toLongLong())) : QJsonValue(QJsonValue::Null)},
            {"check", checks}, {"atMs", double(clock.elapsed())}};
        replies.append(value); output({{"event", "reply-finished"}, {"value", value}});
        if (!actionQueued && checks == 2 && eligible(value) && (mode == "cancel" || mode == "destroy" || mode == "replacement")) {
            actionQueued = true;
            QTimer::singleShot(0, &observer, [&] {
                if (!updater || complete) { finish(false, QStringLiteral("Reentrant control lost updater")); return; }
                actions.append(QJsonObject{{"action", mode}, {"fromActual399Completion", true}, {"atMs", double(clock.elapsed())}});
                if (mode == "destroy") updater.reset();
                else { updater->cancel(); if (mode == "replacement") submit(); }
                actionDone = true; QTimer::singleShot(0, &observer, consider);
            });
        }
        QTimer::singleShot(0, &observer, consider);
    });
    QTimer::singleShot(0, &observer, submit);
    app.exec();
    output({{"event", "summary"}, {"case", mode}, {"passed", complete && exitCode == 0}, {"reason", reason},
        {"publicChecks", checks}, {"wireReplies", replies}, {"publicStates", states}, {"sslErrors", sslErrors}, {"actions", actions},
        {"warmupCurrent", warmup}, {"firstOperationCapExhausted", capExhausted}, {"readyReadReceipts", readyReads},
        {"activeRepliesAtCompletion", activeAtCompletion}, {"updaterDestroyed", !updater},
        {"publicFinalState", updater ? updater->state() : QStringLiteral("destroyed")},
        {"secondLogicalElapsedMs", secondStarted >= 0 ? double(clock.elapsed() - secondStarted) : -1.0},
        {"qtRuntimeVersion", qVersion()}, {"runtimeMappings", mappings()}, {"expectedServerJsonSha256", sha(expected)},
        {"observerReadsReplyBody", false}, {"automaticFallbackOrHttp1Forcing", false}});
    updater.reset(); // Destroy while observer and all captured storage still exist.
    return exitCode;
}
