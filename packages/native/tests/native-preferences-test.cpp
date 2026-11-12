#include "native-preferences.h"
#include "domain.h"

#include <QGuiApplication>
#include <QSignalSpy>
#include <QStyleHints>
#include <QScreen>
#include <QTemporaryDir>
#include <QTest>
#include <QWindow>
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>
#include <limits>

using namespace workspace;

class NativePreferencesTest final : public QObject {
    Q_OBJECT
    Qt::ColorScheme originalScheme = Qt::ColorScheme::Unknown;
private slots:
    void init() { originalScheme = QGuiApplication::styleHints()->colorScheme(); }
    void cleanup() { QGuiApplication::styleHints()->setColorScheme(originalScheme); }

    void lastConversationPersistsAcrossRestartWithExactOwnerIsolation()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("preferences.ini");
        const QString ownerA(64, u'a'), ownerB(64, u'b');
        const QString stream = "33333333-3333-4333-8333-333333333333";
        const QString topic = "44444444-4444-4444-8444-444444444444";
        {
            NativePreferences preferences(path);
            preferences.recordLastConversation(ownerA, stream, topic);
            QCOMPARE(preferences.loadLastConversation(ownerA), (QStringList{stream, topic}));
            QVERIFY(preferences.loadLastConversation(ownerB).isEmpty());
            QSettings before(path, QSettings::IniFormat);
            QVERIFY(!before.contains("navigation/owners/" + ownerA + "/lastConversation"));
            preferences.recordLastConversation(ownerB, stream, {});
            QCOMPARE(preferences.loadLastConversation(ownerB), (QStringList{stream, QString{}}));
        } // Flush the last pending route on normal shutdown, before its debounce expires.
        NativePreferences reopened(path);
        QCOMPARE(reopened.loadLastConversation(ownerA), (QStringList{stream, topic}));
        QCOMPARE(reopened.loadLastConversation(ownerB), (QStringList{stream, QString{}}));
        reopened.recordLastConversation(ownerA, {}, {});
        QCOMPARE(reopened.loadLastConversation(ownerA), (QStringList{QString{}, QString{}}));
        QCOMPARE(reopened.loadLastConversation(ownerB), (QStringList{stream, QString{}}));
    }

    void lastConversationCoalescesNavigationBeforeWritingTheFinalRoute()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("preferences.ini");
        const QString owner(64, u'a'), stream = "33333333-3333-4333-8333-333333333333";
        const QString first = "44444444-4444-4444-8444-444444444444", last = "55555555-5555-4555-8555-555555555555";
        NativePreferences preferences(path);
        preferences.recordLastConversation(owner, stream, first);
        preferences.recordLastConversation(owner, stream, last);
        QSettings disk(path, QSettings::IniFormat);
        const auto key = "navigation/owners/" + owner + "/lastConversation";
        QVERIFY(!disk.contains(key));
        QTRY_VERIFY_WITH_TIMEOUT(([&] { disk.sync(); return disk.value(key).toStringList() == QStringList{stream, last}; })(), 1500);
        QCOMPARE(preferences.loadLastConversation(owner), (QStringList{stream, last}));
    }

    void malformedLastConversationAndOwnerCannotBecomeARoute()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("preferences.ini");
        const QString owner(64, u'a'), stream = "33333333-3333-4333-8333-333333333333";
        for (const auto &route : {QStringList{}, QStringList{stream}, QStringList{stream, "bad-topic"},
                 QStringList{"", stream}, QStringList{stream, "", "extra"}}) {
            { QSettings disk(path, QSettings::IniFormat); disk.setValue("navigation/owners/" + owner + "/lastConversation", route); disk.sync(); }
            NativePreferences preferences(path);
            QVERIFY(preferences.loadLastConversation(owner).isEmpty());
            preferences.recordLastConversation("../owner", stream, {});
            preferences.recordLastConversation(owner, {}, stream);
            QVERIFY(preferences.loadLastConversation("../owner").isEmpty());
            QVERIFY(preferences.loadLastConversation(owner).isEmpty());
        }
    }

    void defaultsFollowTheSystemAndToggleUsesResolvedAppearance()
    {
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("preferences.ini"));
        QCOMPARE(preferences.paletteId(), QString("blue-cold"));
        QCOMPARE(preferences.mode(), QString("system"));
        QCOMPARE(preferences.sidebarPreferredWidth(), 299);
        auto *style = QGuiApplication::styleHints();
        style->setColorScheme(Qt::ColorScheme::Dark);
        QTRY_VERIFY(preferences.dark());
        preferences.toggleMode();
        QCOMPARE(preferences.mode(), QString("light"));
        QVERIFY(!preferences.dark());
        style->setColorScheme(Qt::ColorScheme::Light);
        style->setColorScheme(Qt::ColorScheme::Dark);
        QVERIFY(!preferences.dark());
        preferences.setMode("system");
        QVERIFY(preferences.dark());
        style->setColorScheme(Qt::ColorScheme::Light);
        QTRY_VERIFY(!preferences.dark());
    }

    void diskCacheDefaultsToTenDecimalGigabytesAndIsGlobalAcrossOwners()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("preferences.ini");
        {
            NativePreferences preferences(path);
            QCOMPARE(preferences.cacheLimitGb(), 10);
            QSignalSpy changes(&preferences, &NativePreferences::cacheLimitGbChanged);
            preferences.setOwner(QString(64, u'a'));
            preferences.setCacheLimitGb(25);
            QCOMPARE(changes.count(), 1);
            preferences.setOwner(QString(64, u'b'));
            QCOMPARE(preferences.cacheLimitGb(), 25);
            for (const auto invalid : {-1, 0, 1001, std::numeric_limits<int>::max()})
                preferences.setCacheLimitGb(invalid);
            QCOMPARE(preferences.cacheLimitGb(), 25);
            QCOMPARE(changes.count(), 1);
            preferences.setCacheLimitGb(25);
            QCOMPARE(changes.count(), 1);
        }
        NativePreferences restored(path);
        QCOMPARE(restored.cacheLimitGb(), 25);
        restored.setOwner({});
        QCOMPARE(restored.cacheLimitGb(), 25);
        QSignalSpy errors(&restored, &NativePreferences::cacheErrorChanged);
        restored.reportCacheError("unavailable");
        QCOMPARE(restored.cacheError(), QString("unavailable"));
        restored.reportCacheError("unavailable");
        QCOMPARE(errors.count(), 1);
        restored.reportCacheError({});
        QCOMPARE(errors.count(), 2);
    }

    void malformedDiskCacheLimitsCannotIncreaseDiskUse()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("preferences.ini");
        for (const auto *invalid : {"0", "1001", "-1", "NaN", "100.5", "1e6", "25\n", "999999999999"}) {
            {
                QSettings settings(path, QSettings::IniFormat);
                settings.setValue("cache/limitGb", invalid);
            }
            NativePreferences preferences(path);
            QCOMPARE(preferences.cacheLimitGb(), 10);
        }
        for (const int bound : {1, 1000}) {
            { NativePreferences preferences(path); preferences.setCacheLimitGb(bound); }
            NativePreferences restored(path);
            QCOMPARE(restored.cacheLimitGb(), bound);
        }
    }

    void suggestionsPersistFrequencyWithRecencyTiesAndFourteenItemBound()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("preferences.ini");
        const QString owner(64, u'a');
        {
            NativePreferences preferences(path);
            preferences.setOwner(owner);
            preferences.recordSuggestedReaction("heart", owner);
            preferences.recordSuggestedReaction("joy", owner);
            QCOMPARE(preferences.suggestedReactions().first().toMap().value("name").toString(), QString("joy"));
            preferences.recordSuggestedReaction("heart", owner);
            preferences.recordSuggestedReaction("clap", owner);
            const auto suggestions = preferences.suggestedReactions();
            QCOMPARE(suggestions.size(), 3);
            QCOMPARE(suggestions.at(0).toMap().value("name").toString(), QString("heart"));
            QCOMPARE(suggestions.at(0).toMap().value("count").toLongLong(), 2);
            QCOMPARE(suggestions.at(1).toMap().value("name").toString(), QString("clap"));
        }
        NativePreferences restored(path);
        restored.setOwner(owner);
        QCOMPARE(restored.suggestedReactions().size(), 3);
        QCOMPARE(restored.suggestedReactions().first().toMap().value("count").toLongLong(), 2);
        for (const auto &choice : knownReactionEmojis().mid(0, 20))
            restored.recordSuggestedReaction(choice.toMap().value("emojiName").toString(), owner);
        QCOMPARE(restored.suggestedReactions().size(), 14);
        QSettings settings(path, QSettings::IniFormat);
        const auto rows = QJsonDocument::fromJson(settings.value("reactions/owners/" + owner + "/suggested").toByteArray()).array();
        QCOMPARE(rows.size(), 14);
    }

    void reactionHistoryRequiresExactCurrentOwnerAndCanonicalKnownNames()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("preferences.ini");
        const QString ownerA(64, u'a'), ownerB(64, u'b');
        NativePreferences preferences(path);
        preferences.recordSuggestedReaction("heart", {});
        preferences.recordQuickReactionUse("heart", {});
        QVERIFY(preferences.suggestedReactions().isEmpty());
        QVERIFY(preferences.quickReactionUsage().isEmpty());
        preferences.setOwner(ownerA);
        QSignalSpy changes(&preferences, &NativePreferences::reactionHistoryChanged);
        for (const auto *name : {"unknown_provider_emoji", ":heart:", "❤", "Heart", "heart\n"}) {
            preferences.recordSuggestedReaction(QString::fromUtf8(name), ownerA);
            preferences.recordQuickReactionUse(QString::fromUtf8(name), ownerA);
        }
        preferences.recordSuggestedReaction("heart", ownerB);
        preferences.recordQuickReactionUse("heart", ownerB);
        QCOMPARE(changes.count(), 0);
        preferences.recordSuggestedReaction("heart", ownerA);
        preferences.recordQuickReactionUse("joy", ownerA);
        QCOMPARE(changes.count(), 2);
        preferences.setOwner(ownerB);
        QVERIFY(preferences.suggestedReactions().isEmpty());
        QVERIFY(preferences.quickReactionUsage().isEmpty());
        preferences.recordSuggestedReaction("clap", ownerA); // Late previous-owner event.
        preferences.recordQuickReactionUse("clap", ownerA);
        QVERIFY(preferences.quickReactionUsage().isEmpty());
        preferences.setOwner(ownerA);
        QCOMPARE(preferences.suggestedReactions().size(), 1);
        QCOMPARE(preferences.quickReactionUsage().value("joy").toLongLong(), 1);
        QVERIFY(!preferences.quickReactionUsage().contains("clap"));
    }

    void appearanceNotificationsCannotExposePreviousOwnerReactionHistory()
    {
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("preferences.ini"));
        const QString ownerA(64, u'a'), ownerB(64, u'b');
        preferences.setOwner(ownerA);
        preferences.setMode("dark");
        preferences.recordSuggestedReaction("heart", ownerA);
        preferences.recordQuickReactionUse("joy", ownerA);
        bool notified = false;
        bool exposedPreviousOwner = false;
        connect(&preferences, &NativePreferences::modeChanged, &preferences, [&] {
            notified = true;
            exposedPreviousOwner = !preferences.suggestedReactions().isEmpty()
                || !preferences.quickReactionUsage().isEmpty();
        });
        preferences.setOwner(ownerB);
        QVERIFY(notified);
        QVERIFY(!exposedPreviousOwner);
        QVERIFY(preferences.suggestedReactions().isEmpty());
        QVERIFY(preferences.quickReactionUsage().isEmpty());
    }

    void quickReactionCountsUseSevenUtcDaysAndDiscardMalformedOrFutureData()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("preferences.ini");
        const QString owner(64, u'a');
        QDate today(2026, 11, 6);
        {
            QSettings settings(path, QSettings::IniFormat);
            QJsonObject days{
                {"2026-10-30", QJsonObject{{"heart", 100}}},
                {"2026-10-31", QJsonObject{{"heart", 2}}},
                {"2026-11-06", QJsonObject{{"heart", 3}, {"joy", 1.5}, {"clap", -1},
                    {"unknown", 4}, {"thumbs_up", 1000000001}, {"cry", "4"}}},
                {"2026-11-07", QJsonObject{{"heart", 1000}}},
                {"2026-11-06\n", QJsonObject{{"heart", 1000}}},
                {"2026-02-30", QJsonObject{{"heart", 1000}}},
                {"not-a-date", QJsonObject{{"heart", 1000}}}
            };
            settings.setValue("reactions/owners/" + owner + "/usage", QJsonDocument(days).toJson(QJsonDocument::Compact));
        }
        NativePreferences preferences(path, nullptr, [&] { return today; });
        preferences.setOwner(owner);
        QCOMPARE(preferences.quickReactionUsage().size(), 1);
        QCOMPARE(preferences.quickReactionUsage().value("heart").toLongLong(), 5);
        preferences.recordQuickReactionUse("heart", owner);
        QCOMPARE(preferences.quickReactionUsage().value("heart").toLongLong(), 6);
        today = today.addDays(1);
        QCOMPARE(preferences.quickReactionUsage().value("heart").toLongLong(), 4);
        preferences.recordQuickReactionUse("joy", owner);
        QCOMPARE(preferences.quickReactionUsage().value("joy").toLongLong(), 1);
        today = today.addDays(7);
        QVERIFY(preferences.quickReactionUsage().isEmpty());
        NativePreferences restored(path, nullptr, [&] { return QDate(2026, 11, 7); });
        restored.setOwner(owner);
        QCOMPARE(restored.quickReactionUsage().value("heart").toLongLong(), 4);
        QCOMPARE(restored.quickReactionUsage().value("joy").toLongLong(), 1);
    }

    void malformedSuggestedHistoryIsFilteredAndCountersSaturate()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("preferences.ini");
        const QString owner(64, u'a');
        {
            QSettings settings(path, QSettings::IniFormat);
            QJsonArray rows{QJsonObject{{"name", "heart"}, {"count", 1000000000}},
                QJsonObject{{"name", "heart"}, {"count", 2}},
                QJsonObject{{"name", "unknown"}, {"count", 1}},
                QJsonObject{{"name", "joy"}, {"count", 0.5}},
                QJsonObject{{"name", "clap"}, {"count", "2"}},
                QJsonObject{{"name", "cry"}, {"count", -1}}};
            settings.setValue("reactions/owners/" + owner + "/suggested", QJsonDocument(rows).toJson(QJsonDocument::Compact));
            const QJsonObject days{{"2026-11-06", QJsonObject{{"heart", 1000000000}}}};
            settings.setValue("reactions/owners/" + owner + "/usage", QJsonDocument(days).toJson(QJsonDocument::Compact));
        }
        NativePreferences preferences(path, nullptr, [] { return QDate(2026, 11, 6); });
        preferences.setOwner(owner);
        QCOMPARE(preferences.suggestedReactions().size(), 1);
        preferences.recordSuggestedReaction("heart", owner);
        preferences.recordQuickReactionUse("heart", owner);
        QCOMPARE(preferences.suggestedReactions().first().toMap().value("count").toLongLong(), 1000000000);
        QCOMPARE(preferences.quickReactionUsage().value("heart").toLongLong(), 1000000000);
    }

    void restoresSeparateOpaqueOwnersAndOneGlobalSidebarWidth()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("preferences.ini");
        const QString ownerA(64, u'a'), ownerB(64, u'b');
        {
            NativePreferences preferences(path);
            preferences.setOwner(ownerA);
            preferences.setPaletteId("emerald-chat");
            preferences.setMode("dark");
            preferences.setSidebarPreferredWidth(417.4);
            preferences.setOwner(ownerB);
            QCOMPARE(preferences.paletteId(), QString("blue-cold"));
            QCOMPARE(preferences.mode(), QString("system"));
            QCOMPARE(preferences.sidebarPreferredWidth(), 417);
            preferences.setPaletteId("orange-warm");
            preferences.setMode("light");
            preferences.setOwner(ownerA);
            QCOMPARE(preferences.paletteId(), QString("emerald-chat"));
            QCOMPARE(preferences.mode(), QString("dark"));
        }
        NativePreferences restored(path);
        QCOMPARE(restored.paletteId(), QString("blue-cold"));
        QCOMPARE(restored.mode(), QString("system"));
        QCOMPARE(restored.sidebarPreferredWidth(), 417);
        restored.setOwner(ownerA);
        QCOMPARE(restored.paletteId(), QString("emerald-chat"));
        QCOMPARE(restored.mode(), QString("dark"));
        restored.setOwner(ownerB);
        QCOMPARE(restored.paletteId(), QString("orange-warm"));
        QCOMPARE(restored.mode(), QString("light"));
        QSettings settings(path, QSettings::IniFormat);
        for (const auto &key : settings.allKeys()) {
            QVERIFY(key == "layout/sidebarWidth" || key.startsWith("appearance/owners/" + ownerA + '/')
                    || key.startsWith("appearance/owners/" + ownerB + '/'));
            QVERIFY(!key.contains("user") && !key.contains("server") && !key.contains("token"));
        }
    }

    void malformedStoredValuesAndInvalidOwnersFallBackSafely()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("preferences.ini");
        {
            QSettings settings(path, QSettings::IniFormat);
            settings.setValue("appearance/anonymous/palette", "missing-palette");
            settings.setValue("appearance/anonymous/mode", "unsupported-mode");
            settings.setValue("layout/sidebarWidth", "not-a-number");
        }
        NativePreferences preferences(path);
        QCOMPARE(preferences.paletteId(), QString("blue-cold"));
        QCOMPARE(preferences.mode(), QString("system"));
        QCOMPARE(preferences.sidebarPreferredWidth(), 299);
        const QString owner(64, u'c');
        preferences.setOwner(owner);
        preferences.setPaletteId("blue-mist");
        preferences.setMode("dark");
        preferences.setOwner("../not-an-owner");
        QCOMPARE(preferences.paletteId(), QString("blue-cold"));
        QCOMPARE(preferences.mode(), QString("system"));
        QVERIFY(!preferences.error().isEmpty());
        preferences.setOwner(owner);
        QCOMPARE(preferences.paletteId(), QString("blue-mist"));
        QCOMPARE(preferences.mode(), QString("dark"));
        QSettings settings(path, QSettings::IniFormat);
        QVERIFY(!settings.childGroups().contains("not-an-owner"));
    }

    void setterBoundsAndEverySupportedPaletteSurviveRecreation()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("preferences.ini");
        for (const auto *palette : {"orange-warm", "blue-cold", "blue-mist", "emerald-chat"}) {
            {
                NativePreferences preferences(path);
                preferences.setPaletteId(QString::fromLatin1(palette));
                preferences.setSidebarPreferredWidth(1000);
                QCOMPARE(preferences.sidebarPreferredWidth(), 720);
                preferences.setSidebarPreferredWidth(100);
                QCOMPARE(preferences.sidebarPreferredWidth(), 240);
                preferences.setSidebarPreferredWidth(std::numeric_limits<double>::quiet_NaN());
                QCOMPARE(preferences.sidebarPreferredWidth(), 299);
            }
            NativePreferences restored(path);
            QCOMPARE(restored.paletteId(), QString::fromLatin1(palette));
            QCOMPARE(restored.sidebarPreferredWidth(), 299);
        }
        NativePreferences invalid(path);
        invalid.setPaletteId("unknown");
        invalid.setMode("wrong");
        QCOMPARE(invalid.paletteId(), QString("blue-cold"));
        QCOMPARE(invalid.mode(), QString("system"));
    }

    void ownerHashMustBeExactlyLowerHexWithoutTrailingNewlines()
    {
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("preferences.ini"));
        const QString owner(64, u'd');
        for (const auto &invalid : {owner + '\n', owner.toUpper(), owner.left(63), owner + 'd'}) {
            preferences.setOwner(owner);
            preferences.setMode("dark");
            preferences.setOwner(invalid);
            QCOMPARE(preferences.mode(), QString("system"));
            QVERIFY(!preferences.error().isEmpty());
        }
    }

    void normalWindowGeometryPersistsSeparatelyFromAppearanceOwners()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("preferences.ini");
        const auto screen = QGuiApplication::primaryScreen()->availableGeometry();
        const QRect rectangle(screen.x() + 20, screen.y() + 30, 820, 640);
        {
            NativePreferences preferences(path);
            QWindow window;
            preferences.attachWindow(&window);
            window.setGeometry(rectangle);
            preferences.attachWindow(nullptr); // Flush, as normal application exit does.
        }
        NativePreferences restored(path);
        restored.setOwner(QString(64, u'a'));
        QWindow window;
        restored.attachWindow(&window);
        const auto expectedX = std::clamp(rectangle.x(), screen.left(), qMax(screen.left(), screen.right() - rectangle.width() + 1));
        const auto expectedY = std::clamp(rectangle.y(), screen.top(), qMax(screen.top(), screen.bottom() - rectangle.height() + 1));
        QCOMPARE(window.geometry(), QRect(expectedX, expectedY, 820, 640));
        restored.setOwner(QString(64, u'b'));
        QCOMPARE(window.geometry(), QRect(expectedX, expectedY, 820, 640));
    }

    void minimizedAndMaximizedGeometryNeverOverwriteNormalBounds()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("preferences.ini");
        NativePreferences preferences(path);
        QWindow window;
        preferences.attachWindow(&window);
        const auto normal = QRect(QGuiApplication::primaryScreen()->availableGeometry().topLeft(), QSize(900, 650));
        window.setGeometry(normal);
        window.setWindowState(Qt::WindowMaximized);
        window.setGeometry(0, 0, 2000, 1200);
        window.setWindowState(Qt::WindowMinimized);
        window.setGeometry(0, 0, 100, 100);
        QVERIFY(preferences.windowMaximized());
        preferences.attachWindow(nullptr);
        QSettings saved(path, QSettings::IniFormat);
        QCOMPARE(saved.value("window/width").toInt(), 900);
        QCOMPARE(saved.value("window/height").toInt(), 650);
        QVERIFY(saved.value("window/maximized").toBool());
        NativePreferences restored(path);
        QWindow second;
        restored.attachWindow(&second);
        QCOMPARE(second.windowState(), Qt::WindowMaximized);
    }

    void removedMonitorAndMalformedBoundsRestoreOntoAnAvailableScreen()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("preferences.ini");
        {
            QSettings settings(path, QSettings::IniFormat);
            settings.setValue("window/x", std::numeric_limits<int>::max());
            settings.setValue("window/y", std::numeric_limits<int>::min());
            settings.setValue("window/width", "invalid");
            settings.setValue("window/height", -1);
        }
        NativePreferences preferences(path);
        QWindow window;
        preferences.attachWindow(&window);
        const auto available = QGuiApplication::primaryScreen()->availableGeometry();
        QCOMPARE(window.width(), qMin(1440, qMax(700, available.width())));
        QCOMPARE(window.height(), qMin(900, qMax(600, available.height())));
        QVERIFY(window.x() >= available.left()); QVERIFY(window.y() >= available.top());
        if (available.width() >= 700 && available.height() >= 600) QVERIFY(available.contains(window.geometry()));
        preferences.attachWindow(nullptr);
    }

    void visibleActivationRetainsMaximizationAfterMinimize_data()
    {
        QTest::addColumn<bool>("hidden");
        QTest::newRow("minimized") << false;
        QTest::newRow("hidden") << true;
    }

    void visibleActivationRetainsMaximizationAfterMinimize()
    {
        QFETCH(bool, hidden);
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("preferences.ini"));
        QWindow window;
        window.setTitle("Cassi native window-state regression");
        preferences.attachWindow(&window);
        window.show(); QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.showMaximized(); QTRY_COMPARE(window.windowState(), Qt::WindowMaximized);
        if (hidden) { window.hide(); QVERIFY(!window.isVisible()); }
        else { window.showMinimized(); QTRY_COMPARE(window.visibility(), QWindow::Minimized); }
        QVERIFY(preferences.windowMaximized());
        preferences.activateWindow();
        QTRY_COMPARE(window.windowState(), Qt::WindowMaximized);
        QTRY_COMPARE(window.visibility(), QWindow::Maximized);
        window.close();
    }
    void organizationHistoryRoundTrip()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto store = directory.filePath("prefs.ini");
        {
            NativePreferences preferences(store);
            QVERIFY(preferences.organizationHistory().isEmpty());
            preferences.recordOrganizationOrigin("https://acme.example/some/path");
            preferences.recordOrganizationOrigin("http://127.0.0.1:8080/");
            preferences.recordOrganizationOrigin("not a url");
            preferences.recordOrganizationOrigin("ftp://nope.example");
            // Most-recent-first: suggestions lead with the latest origin.
            QCOMPARE(preferences.organizationHistory(),
                     (QStringList{"http://127.0.0.1:8080", "https://acme.example"}));
            // Re-recording an origin moves it to the front without duplication.
            preferences.recordOrganizationOrigin("https://acme.example");
            QCOMPARE(preferences.organizationHistory(),
                     (QStringList{"https://acme.example", "http://127.0.0.1:8080"}));
        }
        NativePreferences reopened(store);
        QCOMPARE(reopened.organizationHistory(),
                 (QStringList{"https://acme.example", "http://127.0.0.1:8080"}));
    }

    void composerToolbarExpansionRoundTripsAcrossRecreatedPreferences()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("composer-toolbar.ini");
        NativePreferences preferences(path);
        // Native counterpart of settings.model.ts composerToolbarExpanded,
        // stored with the global layout keys like the sidebar width.
        const auto index = preferences.metaObject()->indexOfProperty("composerToolbarExpanded");
        QVERIFY(index >= 0);
        const QMetaProperty property = preferences.metaObject()->property(index);
        QVERIFY(property.isValid());
        QVERIFY(property.isWritable());
        // settings.model.ts:74 default: composerToolbarExpanded false.
        QVERIFY(!property.read(&preferences).toBool());
        QSignalSpy changed(&preferences, SIGNAL(composerToolbarExpandedChanged()));
        QVERIFY(changed.isValid());
        QVERIFY(property.write(&preferences, true));
        QCOMPARE(changed.count(), 1);
        QVERIFY(property.read(&preferences).toBool());
        // Reopening the same settings file restores the expansion choice,
        // mirroring the toolbar.ui.test "keeps toolbar expansion across
        // composer instances" contract.
        NativePreferences reopened(path);
        const auto reopenedIndex = reopened.metaObject()->indexOfProperty("composerToolbarExpanded");
        QVERIFY(reopenedIndex >= 0);
        const QMetaProperty reopenedProperty = reopened.metaObject()->property(reopenedIndex);
        QVERIFY(reopenedProperty.read(&reopened).toBool());
        QVERIFY(reopenedProperty.write(&reopened, false));
        NativePreferences restarted(path);
        const auto restartedIndex = restarted.metaObject()->indexOfProperty("composerToolbarExpanded");
        QVERIFY(restartedIndex >= 0);
        QVERIFY(!restarted.metaObject()->property(restartedIndex).read(&restarted).toBool());
    }
};

QTEST_MAIN(NativePreferencesTest)
#include "native-preferences-test.moc"
