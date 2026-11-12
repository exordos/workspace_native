// Throwaway spike for docs/NATIVE_DESKTOP_CALL_STACK.md (#160).
// Proves: QtWebEngine initializes + renders in a QML window, a CSP-safe
// app-scheme loader path, getUserMedia permission plumbing through
// featurePermissionRequested, local MediaStream video rendering, and the
// getDisplayMedia picker insertion point. NOT production code; never built
// unless WORKSPACE_NATIVE_CALL_SPIKE=ON; registered in no test suite.

#include <QtWebEngineQuick>
#include <QGuiApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QQmlApplicationEngine>
#include <QTimer>
#include <QUrl>
#include <QWebEngineProfile>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineUrlScheme>
#include <QWebEngineUrlSchemeHandler>

// Serves spike content under app:// — a registered *secure* local scheme, so
// getUserMedia (which requires a secure context, unlike plain qrc://) can run
// from "our own base url", mirroring the reference vendored-script loader
// (workspace_ui jitsi-external-api.loader.ts:9-27).
class SpikeSchemeHandler : public QWebEngineUrlSchemeHandler {
public:
    void requestStarted(QWebEngineUrlRequestJob *job) override {
        QString path = job->requestUrl().path();
        if (path == QLatin1String("/"))
            path = QStringLiteral("/probe.html");
        QFile *file = new QFile(QStringLiteral(":/spike/call%1").arg(path), job);
        if (!file->open(QIODevice::ReadOnly)) {
            job->fail(QWebEngineUrlRequestJob::UrlNotFound);
            return;
        }
        job->reply(file->fileName().endsWith(QLatin1String(".js"))
                       ? QByteArrayLiteral("application/javascript")
                       : QByteArrayLiteral("text/html"),
                   file);
    }
};

int main(int argc, char *argv[])
{
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS",
            (qEnvironmentVariable("QTWEBENGINE_CHROMIUM_FLAGS")
             + QStringLiteral(" --no-sandbox --disable-gpu --disable-dev-shm-usage")).toUtf8());
    QWebEngineUrlScheme scheme(QByteArrayLiteral("app"));
    scheme.setSyntax(QWebEngineUrlScheme::Syntax::HostAndPort);
    scheme.setDefaultPort(443);
    scheme.setFlags(QWebEngineUrlScheme::LocalScheme | QWebEngineUrlScheme::SecureScheme
                    | QWebEngineUrlScheme::CorsEnabled | QWebEngineUrlScheme::LocalAccessAllowed);
    QWebEngineUrlScheme::registerScheme(scheme);
    QtWebEngineQuick::initialize();

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("workspace-native-call-spike"));
    QCommandLineParser parser;
    parser.addOption({QStringLiteral("page"), QStringLiteral("spike page file name"),
                      QStringLiteral("name"), QStringLiteral("probe.html")});
    parser.addOption({QStringLiteral("duration"), QStringLiteral("ms to live"),
                      QStringLiteral("ms"), QStringLiteral("15000")});
    parser.addOption({QStringLiteral("show"), QStringLiteral("keep window on screen (no auto-quit)")});
    parser.process(app);

    auto *spikeHandler = new SpikeSchemeHandler;
    spikeHandler->setParent(&app);
    QWebEngineProfile::defaultProfile()->installUrlSchemeHandler(
        QByteArrayLiteral("app"), spikeHandler);

    QQmlApplicationEngine engine;
    const QString page = parser.value(QStringLiteral("page"));
    const QUrl startUrl = page.startsWith(QLatin1String("https:"))
        ? QUrl(page)
        : QUrl(QStringLiteral("app://spike/%1").arg(page));
    engine.rootContext()->setContextProperty(QStringLiteral("spikeStartUrl"), startUrl);
    engine.load(QUrl(QStringLiteral("qrc:/spike/call/CallSpike.qml")));
    if (engine.rootObjects().isEmpty())
        return 2;

    if (!parser.isSet(QStringLiteral("show"))) {
        bool ok = false;
        const int ms = parser.value(QStringLiteral("duration")).toInt(&ok);
        QTimer::singleShot(ok ? ms : 15000, &app, &QCoreApplication::quit);
    }
    return app.exec();
}
