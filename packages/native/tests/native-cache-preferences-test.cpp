#include "native-app.h"
#include "native-cache-service.h"
#include "native-preferences.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace workspace;

class NativeCachePreferencesTest final : public QObject {
    Q_OBJECT
private slots:
    void requestedLimitIsPersistedBeforeTheWorkerConfirmsItsApplication()
    {
        QTemporaryDir directory;
        const auto settings = directory.filePath("preferences.ini");
        NativePreferences preferences(settings);
        QCOMPARE(preferences.cacheLimitGb(), 10);
        preferences.setCacheLimitGb(2);
        NativeCacheService service(directory.filePath("cache"));
        bindNativeCachePreferences(service, preferences);
        QCOMPARE(service.requestedDiskLimitBytes(), 2000000000LL);
        QVERIFY(!preferences.cacheAvailable());
        QVERIFY(service.open(QUrl("https://preferences.invalid"),
            "11111111-1111-4111-8111-111111111111", "22222222-2222-4222-8222-222222222222"));
        QVERIFY(preferences.cacheBusy());
        QTRY_VERIFY(preferences.cacheAvailable());
        QVERIFY(!preferences.cacheBusy());
        QCOMPARE(preferences.cacheAppliedLimitBytes(), 2000000000LL);

        preferences.setCacheLimitGb(1);
        QCOMPARE(service.requestedDiskLimitBytes(), 1000000000LL);
        QCOMPARE(preferences.cacheAppliedLimitBytes(), 2000000000LL);
        QVERIFY(preferences.cacheBusy());
        QVERIFY(!preferences.cacheAvailable());
        NativePreferences restarted(settings);
        QCOMPARE(restarted.cacheLimitGb(), 1);
        QTRY_VERIFY(preferences.cacheAvailable());
        QVERIFY(!preferences.cacheBusy());
        QCOMPARE(preferences.cacheAppliedLimitBytes(), 1000000000LL);
        QVERIFY(preferences.cacheUsageBytes() > 0);
        QVERIFY(preferences.cacheUsageBytes() <= 1000000000LL);
        QVERIFY(preferences.cacheError().isEmpty());
        service.shutdown();
        QVERIFY(!preferences.cacheAvailable());
        QTRY_VERIFY(service.stopped());
    }

    void storageFailureReachesTheActualSettingsBinding()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("not-a-directory");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        NativePreferences preferences(directory.filePath("preferences.ini"));
        NativeCacheService service(path);
        bindNativeCachePreferences(service, preferences);
        QVERIFY(service.open(QUrl("https://preferences.invalid"),
            "11111111-1111-4111-8111-111111111111", "22222222-2222-4222-8222-222222222222"));
        QTRY_VERIFY(!preferences.cacheBusy());
        QVERIFY(!preferences.cacheAvailable());
        QVERIFY(!preferences.cacheError().isEmpty());
        QCOMPARE(preferences.cacheLimitGb(), 10);
        service.shutdown();
        QTRY_VERIFY(service.stopped());
    }
};

QTEST_MAIN(NativeCachePreferencesTest)
#include "native-cache-preferences-test.moc"
