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
};

QTEST_MAIN(NativeAuthGuardTest)
#include "native-auth-guard-test.moc"
