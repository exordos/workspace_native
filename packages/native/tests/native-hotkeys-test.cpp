#include "native-hotkeys.h"
#include <QKeyEvent>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace workspace;
class NativeHotkeysTest final : public QObject {
    Q_OBJECT
private slots:
    void catalogPersistenceAndCollisions()
    {
        QTemporaryDir dir;
        QSettings ini(dir.filePath("keys.ini"), QSettings::IniFormat);
        NativeHotkeys keys(ini);
        keys.load("settings/one");
        QMap<QString, int> groups;
        for (const auto& row : keys.actions())
            ++groups[row.toMap().value("category").toString()];
        QCOMPARE(keys.actions().size(), 18);
        QCOMPARE(groups.value("navigation"), 8);
        QCOMPARE(groups.value("interface"), 5);
        QCOMPARE(groups.value("composer"), 5);
        QCOMPARE(keys.fixed().size(), 5);
        QCOMPARE(keys.binding("composer.bold"), "Mod+B");
        QVERIFY(keys.assign("composer.bold", "Ctrl+Shift+K").isEmpty());
        QCOMPARE(keys.validate("composer.code", "Ctrl+Shift+K"), "conflict");
        QCOMPARE(keys.validate("interface.theme", "Meta+E"), "conflict");
        QVERIFY(keys.assign("navigation.calendar", "").isEmpty());
        QVERIFY(keys.setEnabled(false).isEmpty());
        NativeHotkeys restored(ini);
        restored.load("settings/one");
        QVERIFY(!restored.enabled());
        QCOMPARE(restored.binding("composer.bold"), "Ctrl+Shift+K");
        restored.load("settings/two");
        QVERIFY(restored.enabled());
        QCOMPARE(restored.binding("composer.bold"), "Mod+B");
        ini.setValue("settings/one/hotkeys/overrides/composer.bold", "not a chord");
        ini.setValue("settings/one/hotkeys/overrides/interface.theme", "Mod+B");
        restored.load("settings/one");
        QCOMPARE(restored.binding("composer.bold"), "Mod+B");
        QVERIFY(restored.binding("interface.theme").isEmpty());
    }
    void reservedAndPhysicalPositions()
    {
        QTemporaryDir dir;
        QSettings ini(dir.filePath("keys.ini"), QSettings::IniFormat);
        NativeHotkeys keys(ini);
        keys.load("scope");
        for (const QString& value :
            { "Escape", "Ctrl+C", "Alt+F4", "Mod+1", "Ctrl+Shift+R", "Shift+Insert", "Ctrl+L" })
            QCOMPARE(keys.validate("interface.theme", value), "reserved");
        QCOMPARE(keys.validate("interface.quoteMessage", "Ctrl+L"), QString());
        QCOMPARE(keys.validate("composer.code", "Control++"), "reserved");
        QCOMPARE(keys.validate("composer.code", "Shift+Tab"), "reserved");
        QCOMPARE(keys.validate("navigation.back", "Mod+E"), "conflict");
        QCOMPARE(keys.validate("composer.bold", "Ctrl+?"), QString());
        QVERIFY(keys.assign("composer.bold", "Ctrl+?").isEmpty());
        QCOMPARE(keys.validate("composer.code", "Ctrl+Shift+/"), "conflict");
#if defined(Q_OS_MACOS)
        const quint32 scan = 0, virtualKey = 11;
        const auto ctrl = Qt::MetaModifier;
#elif defined(Q_OS_WIN)
        const quint32 scan = 48, virtualKey = 0;
        const auto ctrl = Qt::ControlModifier;
#else
        const quint32 scan = 56, virtualKey = 0;
        const auto ctrl = Qt::ControlModifier;
#endif
        QKeyEvent press(
            QEvent::KeyPress, 0x418, ctrl | Qt::ShiftModifier, scan, virtualKey, 1, QString::fromUtf8("И"));
        QCOMPARE(NativeHotkeys::fromEvent(press), "Ctrl+Shift+B");
        QKeyEvent repeat(QEvent::KeyPress, Qt::Key_B, ctrl, "b", true);
        QVERIFY(NativeHotkeys::fromEvent(repeat).isEmpty());
        QKeyEvent altgr(QEvent::KeyPress, Qt::Key_B, Qt::GroupSwitchModifier);
        QVERIFY(NativeHotkeys::fromEvent(altgr).isEmpty());
    }
};
QTEST_GUILESS_MAIN(NativeHotkeysTest)
#include "native-hotkeys-test.moc"
