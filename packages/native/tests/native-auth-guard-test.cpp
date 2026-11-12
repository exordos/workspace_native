#include "native-auth-guard.h"
#include "native-preferences.h"

#include <QCoreApplication>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QWindow>

#include <functional>

using namespace workspace;

namespace {
// The guard only reads time through its injected clock, so tests jump the
// deadline by hours without sleeping. The poll cadence is real time, kept tiny.
struct FakeClock {
    qint64 now = 1'000'000'000LL;
    qint64 ms() const { return now; }
};
} // namespace

class NativeAuthGuardTest final : public QObject {
    Q_OBJECT
private slots:
    // Acceptance (1): auth-idle-timeout.lib.ts:10-26 preset table.
    void presetConversionMatchesTheReferenceTable()
    {
        QCOMPARE(NativeAuthGuard::timeoutMsForPreset(QStringLiteral("6h")), 21600000LL);
        QCOMPARE(NativeAuthGuard::timeoutMsForPreset(QStringLiteral("12h")), 43200000LL);
        QCOMPARE(NativeAuthGuard::timeoutMsForPreset(QStringLiteral("24h")), 86400000LL);
        QCOMPARE(NativeAuthGuard::timeoutMsForPreset(QStringLiteral("3d")), 259200000LL);
        QCOMPARE(NativeAuthGuard::timeoutMsForPreset(QStringLiteral("7d")), 604800000LL);
        QCOMPARE(NativeAuthGuard::timeoutMsForPreset(QStringLiteral("never")), -1LL);
        QCOMPARE(NativeAuthGuard::sanitizePreset(QStringLiteral("24h")), QStringLiteral("24h"));
        QCOMPARE(NativeAuthGuard::sanitizePreset(QStringLiteral("never")), QStringLiteral("never"));
        QCOMPARE(NativeAuthGuard::sanitizePreset(QStringLiteral("13h")), QStringLiteral("3d"));
        QCOMPARE(NativeAuthGuard::sanitizePreset(QString{}), QStringLiteral("3d"));
    }

    // Acceptance (1): layout-auth-guard.hook.ts:22-23 - "never" starts no guard.
    void neverPresetKeepsTheGuardDisabled()
    {
        FakeClock clock;
        NativeAuthGuard guard(nullptr, [clockPtr = &clock] { return clockPtr->ms(); });
        int fired = 0;
        guard.setExpiryAction([&fired] { ++fired; });
        guard.setPollIntervalMs(5);
        guard.setWindowsVisible(true);
        guard.setSessionActive(true);
        guard.setTimeoutPreset(QStringLiteral("never"));
        QVERIFY(!guard.active());
        QVERIFY(guard.remainingMs() <= 0);
        clock.now += 8LL * 24 * 3600 * 1000;
        QTest::qWait(60);
        QCOMPARE(fired, 0);
    }

    // Acceptance (2): with no activity the sign-out fires exactly once at the
    // deadline (auth-guard.ts:194-201 scheduling, :132-169 firing).
    void idleDeadlineSignsOutExactlyOnceAtTheDeadline()
    {
        FakeClock clock;
        NativeAuthGuard guard(nullptr, [clockPtr = &clock] { return clockPtr->ms(); });
        QSignalSpy activeSpy(&guard, &NativeAuthGuard::activeChanged);
        int fired = 0;
        guard.setExpiryAction([&fired] { ++fired; });
        guard.setPollIntervalMs(5);
        guard.setWindowsVisible(true);
        guard.setSessionActive(true);
        QVERIFY(!guard.active()); // no preset configured yet
        guard.setTimeoutPreset(QStringLiteral("6h"));
        QVERIFY(guard.active());
        QCOMPARE(activeSpy.count(), 1);
        QCOMPARE(guard.expiresAtMs(), clock.now + 21600000LL);
        QCOMPARE(guard.remainingMs(), 21600000LL);
        clock.now += 21600000LL - 500;
        QTest::qWait(60);
        QCOMPARE(fired, 0); // 500 ms of fake time still short of the deadline
        clock.now += 600;
        QTRY_VERIFY_WITH_TIMEOUT(fired == 1, 2000);
        QVERIFY(!guard.active());
        QCOMPARE(activeSpy.count(), 2);
        clock.now += 7LL * 24 * 3600 * 1000;
        QTest::qWait(100);
        QCOMPARE(fired, 1); // never refires until a new login or preset
    }

    // Acceptance (3): activity at deadline-1s re-arms the full timeout
    // (auth-guard.ts:194-201 resetTimer).
    void activityBeforeTheDeadlineExtendsItByTheFullTimeout()
    {
        FakeClock clock;
        NativeAuthGuard guard(nullptr, [clockPtr = &clock] { return clockPtr->ms(); });
        int fired = 0;
        guard.setExpiryAction([&fired] { ++fired; });
        guard.setPollIntervalMs(5);
        guard.setWindowsVisible(true);
        guard.setSessionActive(true);
        guard.setTimeoutPreset(QStringLiteral("6h"));
        const auto originalDeadline = guard.expiresAtMs();
        clock.now += 21600000LL - 1000;
        guard.noteActivity();
        QCOMPARE(guard.expiresAtMs(), originalDeadline - 1000 + 21600000LL);
        clock.now += 2000; // past the original deadline
        QTest::qWait(60);
        QCOMPARE(fired, 0);
        clock.now += 21600000LL;
        QTRY_VERIFY_WITH_TIMEOUT(fired == 1, 2000);
    }

    // Acceptance (4): a preset change while armed tears down and re-arms
    // immediately with the new full timeout (layout-auth-guard.hook.ts:14-30).
    void presetChangeRearmsTheLiveDeadlineImmediately()
    {
        FakeClock clock;
        NativeAuthGuard guard(nullptr, [clockPtr = &clock] { return clockPtr->ms(); });
        int fired = 0;
        guard.setExpiryAction([&fired] { ++fired; });
        guard.setPollIntervalMs(5);
        guard.setWindowsVisible(true);
        guard.setSessionActive(true);
        guard.setTimeoutPreset(QStringLiteral("24h"));
        QCOMPARE(guard.expiresAtMs(), clock.now + 86400000LL);
        guard.setTimeoutPreset(QStringLiteral("6h"));
        QVERIFY(guard.active());
        QCOMPARE(guard.expiresAtMs(), clock.now + 21600000LL); // shortened, not added
        guard.setTimeoutPreset(QStringLiteral("24h"));
        QCOMPARE(guard.expiresAtMs(), clock.now + 86400000LL); // lengthened
    }

    // Acceptance (4) / layout-auth-guard.hook.test.ts:87-109: switching to
    // "never" is cleanup-only - cancel, never re-init, never fire.
    void switchingToNeverCancelsWithoutRefiring()
    {
        FakeClock clock;
        NativeAuthGuard guard(nullptr, [clockPtr = &clock] { return clockPtr->ms(); });
        QSignalSpy activeSpy(&guard, &NativeAuthGuard::activeChanged);
        int fired = 0;
        guard.setExpiryAction([&fired] { ++fired; });
        guard.setPollIntervalMs(5);
        guard.setWindowsVisible(true);
        guard.setSessionActive(true);
        guard.setTimeoutPreset(QStringLiteral("6h"));
        QVERIFY(guard.active());
        guard.setTimeoutPreset(QStringLiteral("never"));
        QVERIFY(!guard.active());
        QVERIFY(guard.remainingMs() <= 0);
        QCOMPARE(guard.expiresAtMs(), 0LL);
        clock.now += 7LL * 24 * 3600 * 1000;
        QTest::qWait(60);
        QCOMPARE(fired, 0);
        guard.noteActivity();
        QTest::qWait(60);
        QCOMPARE(fired, 0);
        QVERIFY(!guard.active());
        QCOMPARE(activeSpy.count(), 2); // armed once, cancelled once
    }

    // auth-guard.ts:171-214: the expiry timer is cleared while hidden and the
    // stored deadline reschedules (firing immediately if already elapsed) when
    // the surface becomes visible again; hidden time alone never signs out.
    void hiddenWindowsPauseTheDeadlineAndVisibleReturnResumesIt()
    {
        FakeClock clock;
        NativeAuthGuard guard(nullptr, [clockPtr = &clock] { return clockPtr->ms(); });
        int fired = 0;
        guard.setExpiryAction([&fired] { ++fired; });
        guard.setPollIntervalMs(5);
        guard.setWindowsVisible(true);
        guard.setSessionActive(true);
        guard.setTimeoutPreset(QStringLiteral("6h"));
        guard.setWindowsVisible(false);
        clock.now += 21600000LL + 1000; // far past the stored deadline
        QTest::qWait(60);
        QCOMPARE(fired, 0); // paused: hidden time cannot sign the user out
        QVERIFY(guard.active()); // the deadline itself is retained, not reset
        guard.setWindowsVisible(true);
        QTRY_VERIFY_WITH_TIMEOUT(fired == 1, 2000);
        QCOMPARE(fired, 1); // rescheduled from the stored deadline, already elapsed
        QVERIFY(!guard.active());
    }

    // Regression (PR #226 review, IDs 4555758272/4555758307): activity that
    // arrives after the deadline has already elapsed but before the pending
    // timer callback runs must enforce the expiry, not extend the session.
    // The reference's resetTimer cannot hit this because its timeout callback
    // is synchronous with the deadline; the poll-based native timer can.
    void activityAfterAnExpiredDeadlineSignsOutInsteadOfExtending()
    {
        FakeClock clock;
        NativeAuthGuard guard(nullptr, [clockPtr = &clock] { return clockPtr->ms(); });
        int fired = 0;
        guard.setExpiryAction([&fired] { ++fired; });
        guard.setPollIntervalMs(60000); // real-time poll far longer than the test
        guard.setWindowsVisible(true);
        guard.setSessionActive(true);
        guard.setTimeoutPreset(QStringLiteral("6h"));
        QVERIFY(guard.active());
        clock.now += 21600000LL + 10; // deadline passed; the timer callback is still pending
        QEvent mouseMove(QEvent::MouseMove);
        QVERIFY(!guard.eventFilter(&guard, &mouseMove)); // late input on the surface
        // The expired deadline is enforced instead of being re-armed away.
        QCOMPARE(fired, 1);
        QVERIFY(!guard.active());
        QTest::qWait(60);
        QCOMPARE(fired, 1); // exactly once, like every other expiry
    }

    // Regression (PR #226 review, ID 4555758286): a mapped window that is not
    // yet exposed (QWindow::isExposed() is false until the platform delivers
    // the Expose event - minimized X11 windows report the same) is not
    // "visible/exposed", so it must not resume the deadline. Exposing it must
    // resume from the retained deadline exactly like the hidden-window
    // contract (auth-guard.ts:171-214).
    void unexposedWindowsPauseTheDeadlineUntilExposedAgain()
    {
        FakeClock clock;
        NativeAuthGuard guard(nullptr, [clockPtr = &clock] { return clockPtr->ms(); });
        int fired = 0;
        guard.setExpiryAction([&fired] { ++fired; });
        guard.setPollIntervalMs(5);
        guard.setSessionActive(true); // installs the filter and auto-detects visibility
        guard.setTimeoutPreset(QStringLiteral("6h"));
        QVERIFY(guard.active());
        clock.now += 21600000LL + 1000; // far past the stored deadline while no window exists
        QTest::qWait(60);
        QCOMPARE(fired, 0); // paused: hidden time cannot sign the user out
        QWindow window;
        window.setVisible(true); // Show reaches the guard; Expose is still pending
        QVERIFY(window.isVisible());
        QVERIFY(!window.isExposed());
        // A visible-but-unexposed window must not resume the already-elapsed deadline.
        QCOMPARE(fired, 0);
        QVERIFY(guard.active()); // the deadline itself is retained, not reset
        // Flushing window-system events delivers Expose; the guard resumes and
        // the elapsed deadline fires immediately, once.
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTRY_VERIFY_WITH_TIMEOUT(fired == 1, 2000);
        QCOMPARE(fired, 1);
        // Hiding pauses again: the guard already fired and stays disarmed.
        window.setVisible(false);
        clock.now += 21600000LL;
        QTest::qWait(60);
        QCOMPARE(fired, 1); // exactly once, never refires
    }

    // The event filter maps the Qt activity events (MouseMove/KeyPress/
    // MouseButtonPress/Wheel - counterparts of auth-guard.ts:239) onto the
    // deadline reset without consuming events; other events must not extend it.
    void onlyActivityEventsReArmThroughTheEventFilter()
    {
        FakeClock clock;
        NativeAuthGuard guard(nullptr, [clockPtr = &clock] { return clockPtr->ms(); });
        guard.setPollIntervalMs(5);
        guard.setWindowsVisible(true);
        guard.setSessionActive(true);
        guard.setTimeoutPreset(QStringLiteral("6h"));
        clock.now += 3600000LL;
        QEvent mouseMove(QEvent::MouseMove);
        QVERIFY(!guard.eventFilter(&guard, &mouseMove));
        QCOMPARE(guard.expiresAtMs(), clock.now + 21600000LL);
        clock.now += 3600000LL;
        QEvent keyPress(QEvent::KeyPress);
        QVERIFY(!guard.eventFilter(&guard, &keyPress));
        QCOMPARE(guard.expiresAtMs(), clock.now + 21600000LL);
        clock.now += 3600000LL;
        QEvent mousePress(QEvent::MouseButtonPress);
        QVERIFY(!guard.eventFilter(&guard, &mousePress));
        QCOMPARE(guard.expiresAtMs(), clock.now + 21600000LL);
        clock.now += 3600000LL;
        QEvent wheel(QEvent::Wheel);
        QVERIFY(!guard.eventFilter(&guard, &wheel));
        QCOMPARE(guard.expiresAtMs(), clock.now + 21600000LL);
        const auto deadlineAfterActivity = guard.expiresAtMs();
        QEvent focusIn(QEvent::FocusIn);
        QVERIFY(!guard.eventFilter(&guard, &focusIn));
        QCOMPARE(guard.expiresAtMs(), deadlineAfterActivity); // focus is not activity
        QEvent showRequest(QEvent::Show);
        QVERIFY(!guard.eventFilter(&guard, &showRequest));
        QCOMPARE(guard.expiresAtMs(), deadlineAfterActivity);
    }

    // Acceptance (5): settings.model.ts:75,94 - absent preset means "3d".
    void authIdleTimeoutDefaultsToThreeDays()
    {
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("preferences.ini"));
        QCOMPARE(preferences.authIdleTimeout(), QStringLiteral("3d"));
    }

    // Acceptance (5): settings.model.ts:175-178 - invalid persisted values
    // sanitize to "3d" on load, in both anonymous and owner scopes.
    void invalidStoredAuthIdleTimeoutSanitizesToThreeDays()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("preferences.ini");
        {
            QSettings settings(path, QSettings::IniFormat);
            settings.setValue("settings/anonymous/authIdleTimeout", "13h");
            settings.setValue("settings/owners/" + QString(64, 'a') + "/authIdleTimeout", QString{});
            settings.sync();
        }
        NativePreferences preferences(path);
        QCOMPARE(preferences.authIdleTimeout(), QStringLiteral("3d"));
        preferences.setOwner(QString(64, 'a'));
        QCOMPARE(preferences.authIdleTimeout(), QStringLiteral("3d"));
    }

    // Acceptance (7): settings.model.ts:289-293 - the chosen preset persists
    // in the current owner's scope and survives a settings restart.
    void authIdleTimeoutSurvivesRestartPerOwner()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("preferences.ini");
        {
            NativePreferences preferences(path);
            preferences.setOwner(QString(64, 'a'));
            preferences.setAuthIdleTimeout(QStringLiteral("24h"));
            QCOMPARE(preferences.authIdleTimeout(), QStringLiteral("24h"));
        }
        NativePreferences restarted(path);
        QCOMPARE(restarted.authIdleTimeout(), QStringLiteral("3d")); // anonymous scope untouched
        restarted.setOwner(QString(64, 'a'));
        QCOMPARE(restarted.authIdleTimeout(), QStringLiteral("24h"));
        restarted.setOwner(QString(64, 'b'));
        QCOMPARE(restarted.authIdleTimeout(), QStringLiteral("3d")); // owners are isolated
        restarted.setOwner(QString(64, 'a'));
        QCOMPARE(restarted.authIdleTimeout(), QStringLiteral("24h"));
    }

    // The setter is the future #135 row surface: it stores the sanitized
    // preset immediately and notifies exactly once per effective change.
    void authIdleTimeoutSetterSanitizesPersistsAndNotifies()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("preferences.ini");
        NativePreferences preferences(path);
        QSignalSpy changed(&preferences, &NativePreferences::authIdleTimeoutChanged);
        preferences.setAuthIdleTimeout(QStringLiteral("6h"));
        QCOMPARE(preferences.authIdleTimeout(), QStringLiteral("6h"));
        QCOMPARE(changed.count(), 1);
        preferences.setAuthIdleTimeout(QStringLiteral("6h"));
        QCOMPARE(changed.count(), 1); // no duplicate notification
        preferences.setAuthIdleTimeout(QStringLiteral("8h")); // not a preset
        QCOMPARE(preferences.authIdleTimeout(), QStringLiteral("3d"));
        QCOMPARE(changed.count(), 2);
        NativePreferences restarted(path);
        restarted.setOwner(QString(64, 'c'));
        QCOMPARE(restarted.authIdleTimeout(), QStringLiteral("3d")); // invalid value was not stored
    }
};

QTEST_MAIN(NativeAuthGuardTest)
#include "native-auth-guard-test.moc"
