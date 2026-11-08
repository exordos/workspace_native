#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QPointer>
#include <QQuickItem>
#include <QQuickStyle>
#include <QRegularExpression>
#include <QResource>
#include <QSignalSpy>
#include <QStringList>
#include <QTest>
#include <QVariant>

#include <memory>

// This is a public QObject presentation adapter, not NativeUpdater/HTTP/install
// acceptance. The real leaf receives the same properties, invokables and signal.
class AboutPolicyUpdater final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString availableVersion READ availableVersion NOTIFY changed)
    Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT)
    Q_PROPERTY(qreal progress READ progress NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
public:
    QString state() const { return m_state; }
    QString availableVersion() const { return m_version; }
    QString currentVersion() const { return QStringLiteral("0.1.0"); }
    qreal progress() const { return m_progress; }
    QString error() const { return m_error; }

    int checks = 0;
    int downloads = 0;
    int installs = 0;
    int restarts = 0;
    int cancels = 0;
    bool reenterDownload = false;
    bool synchronousInstall = false;
    bool reenterRestart = false;
    QStringList calls;

    void publish(const QString &state, const QString &version = {},
                 qreal progress = 0, const QString &error = {})
    {
        m_state = state;
        m_version = version;
        m_progress = progress;
        m_error = error;
        emit changed();
    }
    void repeatChanged() { emit changed(); }

    Q_INVOKABLE void check()
    {
        ++checks; calls.append(QStringLiteral("check"));
        publish(QStringLiteral("checking"));
    }
    Q_INVOKABLE void download()
    {
        ++downloads; calls.append(QStringLiteral("download"));
        // Reenter once while state is still available: a missing admission
        // fence produces a second call rather than an unbounded recursion.
        if (reenterDownload && downloads == 1) emit changed();
        publish(QStringLiteral("downloading"), m_version);
    }
    Q_INVOKABLE void install()
    {
        ++installs; calls.append(QStringLiteral("install"));
        const auto version = m_version;
        publish(QStringLiteral("installing"), version);
        if (synchronousInstall) publish(QStringLiteral("ready"), version);
    }
    Q_INVOKABLE void restart()
    {
        ++restarts; calls.append(QStringLiteral("restart"));
        // Only one nested notification is needed to expose consumption after
        // the call; it cannot turn a failing test into an infinite recursion.
        if (reenterRestart && restarts == 1) emit changed();
    }
    Q_INVOKABLE void cancel()
    {
        ++cancels; calls.append(QStringLiteral("cancel"));
        publish(QStringLiteral("idle"));
    }
signals:
    void changed();
private:
    QString m_state = QStringLiteral("idle");
    QString m_version;
    qreal m_progress = 0;
    QString m_error;
};

class NativeAboutPolicyTest final : public QObject {
    Q_OBJECT
    std::unique_ptr<AboutPolicyUpdater> first;
    std::unique_ptr<AboutPolicyUpdater> second;
    std::unique_ptr<QQmlEngine> engine;
    std::unique_ptr<QQmlComponent> component;
    std::unique_ptr<QObject> root;
    QPointer<QQuickItem> drawer;

    bool createDrawer(AboutPolicyUpdater *updater)
    {
        root.reset(component->createWithInitialProperties({
            {QStringLiteral("injectedUpdater"), QVariant::fromValue(static_cast<QObject *>(updater))}
        }));
        if (!root) return false;
        drawer = root->findChild<QQuickItem *>(QStringLiteral("nativeAboutDrawer"));
        return drawer && drawer->property("policyReady").toBool();
    }
    bool activateAction()
    {
        // Invoke the real QML command. Physical Main input/HTTP/installer
        // acceptance belongs to the separate real-updater Window tests.
        return drawer && QMetaObject::invokeMethod(drawer, "activateUpdateAction");
    }
    void open(bool visible)
    {
        root->setProperty("drawerOpen", visible);
    }
private slots:
    void initTestCase() {}
    void cleanupTestCase() {}
    void init()
    {
        QTest::failOnWarning(QRegularExpression(QStringLiteral(".*")));
        first = std::make_unique<AboutPolicyUpdater>();
        second = std::make_unique<AboutPolicyUpdater>();
        engine = std::make_unique<QQmlEngine>();
        component = std::make_unique<QQmlComponent>(engine.get());
        component->setData(R"QML(
import QtQuick
Item {
    id: harness
    required property var injectedUpdater
    property bool drawerOpen: false
    width: 315
    height: 800
    NativeTheme { id: theme }
    NativeAboutDrawer {
        anchors.fill: parent
        updater: harness.injectedUpdater
        colors: theme.colors
        visible: harness.drawerOpen
        onCloseRequested: harness.drawerOpen = false
    }
}
)QML", QUrl(QStringLiteral("qrc:/workspace/qml/AboutPolicyHarness.qml")));
        QTRY_VERIFY2(component->isReady(), qPrintable(component->errorString()));
    }
    void cleanup()
    {
        // Neither adapter can disappear while the leaf/Connections exist.
        // No window or native surface is created by this policy executable.
        drawer.clear();
        root.reset();
        component.reset();
        engine.reset();
        second.reset();
        first.reset();
    }
    void hiddenAvailableDownloadsOnceAcrossReentryAndDisclosure()
    {
        first->reenterDownload = true;
        QVERIFY2(createDrawer(first.get()), qPrintable(component->errorString()));
        QVERIFY(!drawer->isVisible());
        first->publish(QStringLiteral("available"), QStringLiteral("0.1.1"));
        QCOMPARE(first->downloads, 1);
        QCOMPARE(first->state(), QStringLiteral("downloading"));
        first->publish(QStringLiteral("downloading"), QStringLiteral("0.1.1"), 0.4);
        first->repeatChanged();
        open(true);
        QVERIFY(drawer->isVisible());
        auto *closeAction = root->findChild<QQuickItem *>(QStringLiteral("nativeUpdateClose"));
        QVERIFY(closeAction && closeAction->isVisible() && closeAction->isEnabled());
        QSignalSpy closeRequested(drawer, SIGNAL(closeRequested()));
        QVERIFY(closeRequested.isValid());
        // Public clicked() dispatch exercises the real leaf handler; this
        // component test makes no physical input/visible-window claim.
        QVERIFY(QMetaObject::invokeMethod(closeAction, "clicked"));
        QCOMPARE(closeRequested.size(), 1);
        QVERIFY(!drawer->isVisible());
        open(true);
        QCOMPARE(first->calls, QStringList{QStringLiteral("download")});
        first->publish(QStringLiteral("downloaded"), QStringLiteral("0.1.1"));
        open(false);
        open(true);
        QCOMPARE(first->downloads, 1);
        QCOMPARE(first->checks, 0);
        QCOMPARE(first->installs, 0);
        QCOMPARE(first->restarts, 0);
    }
    void cancelOrErrorClearsExplicitInstallIntent_data()
    {
        QTest::addColumn<bool>("cancel");
        QTest::newRow("cancel-to-idle") << true;
        QTest::newRow("install-error") << false;
    }
    void cancelOrErrorClearsExplicitInstallIntent()
    {
        QFETCH(bool, cancel);
        first->publish(QStringLiteral("downloaded"), QStringLiteral("0.1.1"));
        QVERIFY2(createDrawer(first.get()), qPrintable(component->errorString()));
        QVERIFY(activateAction());
        QCOMPARE(first->installs, 1);
        QCOMPARE(first->state(), QStringLiteral("installing"));
        if (cancel) first->cancel();
        else first->publish(QStringLiteral("error"), QStringLiteral("0.1.1"), 0,
                            QStringLiteral("Synthetic install failure"));
        QVERIFY(drawer->property("canCheck").toBool());
        open(true); open(false); open(true);
        first->publish(QStringLiteral("ready"), QStringLiteral("0.1.1"));
        first->repeatChanged();
        QCOMPARE(first->restarts, 0);
        QCOMPARE(first->installs, 1);
        QCOMPARE(first->checks, 0);
        QCOMPARE(first->downloads, 0);
        QCOMPARE(first->cancels, cancel ? 1 : 0);
    }
    void targetReplacementClearsConsentPermanently()
    {
        first->publish(QStringLiteral("downloaded"), QStringLiteral("0.1.1"));
        QVERIFY2(createDrawer(first.get()), qPrintable(component->errorString()));
        QVERIFY(activateAction());
        QCOMPARE(first->installs, 1);
        first->publish(QStringLiteral("ready"), QStringLiteral("0.1.2"));
        QCOMPARE(first->restarts, 0);
        // Returning to the old target cannot revive discarded consent.
        first->publish(QStringLiteral("ready"), QStringLiteral("0.1.1"));
        QCOMPARE(first->restarts, 0);
        first->publish(QStringLiteral("downloaded"), QStringLiteral("0.1.2"));
        QVERIFY(activateAction());
        QCOMPARE(first->installs, 2);
        first->publish(QStringLiteral("ready"), QStringLiteral("0.1.2"));
        QCOMPARE(first->restarts, 1);
    }
    void updaterReplacementRequiresFreshConsent_data()
    {
        QTest::addColumn<bool>("replaceObject");
        QTest::newRow("different-updater-same-target") << true;
        QTest::newRow("null-then-same-updater") << false;
    }
    void updaterReplacementRequiresFreshConsent()
    {
        QFETCH(bool, replaceObject);
        first->publish(QStringLiteral("downloaded"), QStringLiteral("0.1.1"));
        QVERIFY2(createDrawer(first.get()), qPrintable(component->errorString()));
        QVERIFY(activateAction());
        QCOMPARE(first->installs, 1);
        AboutPolicyUpdater *current = first.get();
        if (replaceObject) {
            second->publish(QStringLiteral("ready"), QStringLiteral("0.1.1"));
            root->setProperty("injectedUpdater", QVariant::fromValue(static_cast<QObject *>(second.get())));
            current = second.get();
            first->publish(QStringLiteral("ready"), QStringLiteral("0.1.1"));
        } else {
            root->setProperty("injectedUpdater", QVariant::fromValue(static_cast<QObject *>(nullptr)));
            first->publish(QStringLiteral("ready"), QStringLiteral("0.1.1"));
            root->setProperty("injectedUpdater", QVariant::fromValue(static_cast<QObject *>(first.get())));
        }
        QCOMPARE(drawer->property("phase").toString(), current->state());
        current->repeatChanged();
        QCOMPARE(first->restarts, 0);
        QCOMPARE(second->restarts, 0);
        const int priorInstalls = current->installs;
        current->publish(QStringLiteral("downloaded"), QStringLiteral("0.1.1"));
        QVERIFY(activateAction());
        QCOMPARE(current->installs, priorInstalls + 1);
        current->publish(QStringLiteral("ready"), QStringLiteral("0.1.1"));
        QCOMPARE(current->restarts, 1);
        QCOMPARE((current == first.get() ? second.get() : first.get())->restarts, 0);
    }
    void readyAtMountAndDisclosureNeverCreatesConsent()
    {
        first->publish(QStringLiteral("ready"), QStringLiteral("0.1.1"));
        QVERIFY2(createDrawer(first.get()), qPrintable(component->errorString()));
        open(true); open(false); open(true);
        first->repeatChanged();
        QVERIFY(!drawer->property("canInstall").toBool());
        QVERIFY(!drawer->property("canCheck").toBool());
        QCOMPARE(first->installs, 0);
        QCOMPARE(first->restarts, 0);
        QCOMPARE(first->checks, 0);
        QCOMPARE(first->downloads, 0);
    }
    void matchingReadyConsumesIntentBeforeSynchronousReentry()
    {
        first->synchronousInstall = true;
        first->reenterRestart = true;
        first->publish(QStringLiteral("downloaded"), QStringLiteral("0.1.1"));
        QVERIFY2(createDrawer(first.get()), qPrintable(component->errorString()));
        QCOMPARE(first->installs, 0);
        QVERIFY(activateAction());
        QCOMPARE(first->installs, 1);
        QCOMPARE(first->restarts, 1);
        QCOMPARE(first->calls, (QStringList{QStringLiteral("install"), QStringLiteral("restart")}));
        first->repeatChanged();
        open(true); open(false); open(true);
        first->publish(QStringLiteral("ready"), QStringLiteral("0.1.1"));
        QCOMPARE(first->installs, 1);
        QCOMPARE(first->restarts, 1);
    }
};

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    Q_INIT_RESOURCE(native_icons);
    NativeAboutPolicyTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "native-about-policy-test.moc"
