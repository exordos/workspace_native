#include "native-auth-guard.h"

#include <QSignalSpy>
#include <QTest>

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
};

QTEST_MAIN(NativeAuthGuardTest)
#include "native-auth-guard-test.moc"
