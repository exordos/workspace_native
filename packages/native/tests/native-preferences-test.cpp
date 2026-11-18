#include "native-preferences.h"
#include "domain.h"

#include <QGuiApplication>
#include <QSignalSpy>
#include <QStyleHints>
#include <QScreen>
#include <QTemporaryDir>
#include <QTest>
#include <QWindow>
#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDir>
#include <QFile>
#include <QDBusError>
#include <QDBusVariant>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>
#include <algorithm>
#include <limits>

using namespace workspace;

namespace {
// Stand-in for the XDG Desktop Portal Settings service at its real D-Bus
// addressing (service org.freedesktop.portal.Desktop, object path
// /org/freedesktop/portal/desktop, interface org.freedesktop.portal.Settings):
// Read(ns, key) answers org.freedesktop.appearance/reduced-motion with the
// pinned value, and SetReducedMotion mirrors a user flipping the system
// toggle by emitting SettingChanged the way the portal does.
class FakeSettingsPortal final : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Settings")
public:
    explicit FakeSettingsPortal(QObject *root) : QDBusAbstractAdaptor(root) {}
public slots:
    Q_SCRIPTABLE QDBusVariant Read(const QString &ns, const QString &key)
    {
        if (ns == QLatin1String("org.freedesktop.appearance")
            && key == QLatin1String("reduced-motion"))
            return QDBusVariant(QVariant(m_reducedMotion));
        return QDBusVariant(QVariant(QStringLiteral("unexpected-key")));
    }
    Q_SCRIPTABLE void SetReducedMotion(int value)
    {
        m_reducedMotion = value;
        emit SettingChanged(QStringLiteral("org.freedesktop.appearance"),
            QStringLiteral("reduced-motion"), QDBusVariant(QVariant(value)));
    }
signals:
    void SettingChanged(const QString &ns, const QString &key, const QDBusVariant &value);
private:
    int m_reducedMotion = 1;
};

// The reduced-motion probe talks to the session bus, so this binary brings up
// its own private bus before main(): a real session bus may already host the
// actual xdg-desktop-portal (the fake could never own its name) or none at
// all. The bus config is minimal - no <standard_session_servicedirs/>, so the
// daemon can never auto-activate the real portal behind a test's fake. If
// dbus-daemon is unavailable the bus stays whatever the environment provides
// and the portal case skips itself via the isConnected guard.
//
// exordy #387 round 3: the bus config and the bus socket live inside a private
// QTemporaryDir (0700, unpredictable mkdtemp name). A fixed name in the shared
// /tmp opened with WriteOnly|Truncate lets another local user pre-place a
// symlink there, and the truncate then clobbers whatever file the runner could
// write. Creation success is asserted by the portal case itself via
// busConfigWritten(), so a failed fixture can never pass silently.
class PortalProbeBus
{
public:
    PortalProbeBus()
    {
        if (!m_dir.isValid())
            return;
        const QString config = m_dir.filePath(QStringLiteral("bus.conf"));
        QFile configFile(config);
        if (!configFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return;
        }
        configFile.write(busConfigXml(m_dir.path()).toUtf8());
        configFile.close();
        m_busConfigWritten = true;
        m_daemon.start(QStringLiteral("dbus-daemon"),
            {QStringLiteral("--config-file=") + config, QStringLiteral("--print-address")});
        if (!m_daemon.waitForStarted(5000) || !m_daemon.waitForReadyRead(5000))
            return;
        const auto address = QString::fromUtf8(m_daemon.readAllStandardOutput())
            .trimmed().split(QLatin1Char(',')).first();
        if (address.startsWith(QLatin1String("unix:")) || address.startsWith(QLatin1String("tcp:")))
            qputenv("DBUS_SESSION_BUS_ADDRESS", address.toUtf8());
    }
    ~PortalProbeBus()
    {
        m_daemon.terminate();
        if (!m_daemon.waitForFinished(3000))
            m_daemon.kill();
    }
    bool busConfigWritten() const { return m_busConfigWritten; }
    // Builds the bus config XML for a socket directory. Two grammars guard the
    // interpolated path: (1) after the XML parses, the <listen> text must be a
    // valid D-Bus address, and dbus-daemon rejects reserved address characters
    // verbatim ("In D-Bus address, character '&' should have been escaped"),
    // so reserved characters are percent-encoded for the address grammar
    // first; (2) the value is then XML-escaped (& < > ") before being spliced
    // into the text node. Percent-encoding already strips every XML
    // metacharacter, so layer (2) is a no-op today; it stays because the two
    // layers guard different grammars. The config file's own path never
    // enters the XML (it reaches dbus-daemon via argv), so this is the only
    // interpolation point.
    static QString dbusEscapeAddressValue(const QString &value)
    {
        static const char hex[] = "0123456789ABCDEF";
        QByteArray escaped;
        const QByteArray utf8 = value.toUtf8();
        for (const char raw : utf8) {
            const bool safe = (raw >= 'a' && raw <= 'z') || (raw >= 'A' && raw <= 'Z')
                || (raw >= '0' && raw <= '9')
                || raw == '_' || raw == '.' || raw == '/' || raw == '-';
            if (safe) {
                escaped += raw;
            } else {
                const auto byte = static_cast<unsigned char>(raw);
                escaped += '%';
                escaped += hex[byte >> 4];
                escaped += hex[byte & 0x0F];
            }
        }
        return QString::fromUtf8(escaped);
    }
    static QString busConfigXml(const QString &tmpDirPath)
    {
        QString escapedPath = dbusEscapeAddressValue(tmpDirPath);
        escapedPath.replace(QLatin1Char('&'), QStringLiteral("&amp;"))
            .replace(QLatin1Char('<'), QStringLiteral("&lt;"))
            .replace(QLatin1Char('>'), QStringLiteral("&gt;"))
            .replace(QLatin1Char('"'), QStringLiteral("&quot;"));
        return QStringLiteral("<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN\""
                              " \"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">"
                              "<busconfig><type>session</type><listen>unix:tmpdir=")
            + escapedPath
            + QStringLiteral("</listen>"
                             "<policy context=\"default\">"
                             "<allow send_destination=\"*\" eavesdrop=\"true\"/>"
                             "<allow eavesdrop=\"true\"/><allow own=\"*\"/>"
                             "</policy></busconfig>");
    }
private:
    QTemporaryDir m_dir{QDir::tempPath() + QStringLiteral("/workspace-native-portal-test-XXXXXX")};
    bool m_busConfigWritten = false;
    QProcess m_daemon;
};
const PortalProbeBus portalProbeBus;
} // namespace

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
        // Global layout keys are exactly the sidebar width and the composer toolbar
        // expansion flag: settings.model.ts keeps composerToolbarExpanded in the same
        // global scope as the sidebar width, and NativePreferences::persist() flushes
        // both together (src/native-preferences.cpp:259-260). Any other key is a leak.
        for (const auto &key : settings.allKeys()) {
            QVERIFY(key == "layout/sidebarWidth" || key == "layout/composerToolbarExpanded"
                    || key.startsWith("appearance/owners/" + ownerA + '/')
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

    // docs/NATIVE_DESKTOP_CHAT_LIST_DENSITY.md (issue #102): the reference resolvers
    // (workspace_ui 61dacef settings.model.ts:124,173) accept exactly one non-default
    // token each and fall back otherwise; both keys are persisted in the global
    // settings scope (settings.model.ts persistSettings keeps one global blob).
    void chatListDensityAndChatClickActionPersistGloballyWithStrictDefaults()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto path = directory.filePath("chat-layout.ini");
        {
            NativePreferences preferences(path);
            QCOMPARE(preferences.chatListDensity(), QStringLiteral("general"));
            QCOMPARE(preferences.chatClickAction(), QStringLiteral("stream"));
            QSignalSpy densityChanged(&preferences, &NativePreferences::chatListDensityChanged);
            QSignalSpy actionChanged(&preferences, &NativePreferences::chatClickActionChanged);
            preferences.setChatListDensity("compact");
            preferences.setChatClickAction("latestTopic");
            QCOMPARE(densityChanged.count(), 1);
            QCOMPARE(actionChanged.count(), 1);
            QSettings disk(path, QSettings::IniFormat);
            QCOMPARE(disk.value("settings/app/chatListDensity").toString(), QStringLiteral("compact"));
            QCOMPARE(disk.value("settings/app/chatClickAction").toString(), QStringLiteral("latestTopic"));
        }
        {
            NativePreferences reopened(path);
            QCOMPARE(reopened.chatListDensity(), QStringLiteral("compact"));
            QCOMPARE(reopened.chatClickAction(), QStringLiteral("latestTopic"));
        }
        {
            QSettings disk(path, QSettings::IniFormat);
            disk.setValue("settings/app/chatListDensity", "standard");
            disk.setValue("settings/app/chatClickAction", "general");
            disk.sync();
        }
        NativePreferences sanitized(path);
        QCOMPARE(sanitized.chatListDensity(), QStringLiteral("general"));
        QCOMPARE(sanitized.chatClickAction(), QStringLiteral("stream"));
    }

    // docs/NATIVE_DESKTOP_FOLDERS_RAIL_LAYOUT.md (issue #85): the reference
    // resolvers (workspace_ui 61dacef settings.model.ts) keep folderRailLayout
    // on "vertical" only for the exact "vertical" token (anything else is
    // "horizontal") and showSystemFolders strictly on true/false (anything
    // else keeps the default true). A virgin store starts vertical/shown and
    // both keys live in the global settings/app scope, like the #102 chat
    // list layout keys. Access is via meta-object so a missing property is a
    // test failure, not a compile error.
    void folderRailLayoutAndSystemFoldersPersistGloballyWithStrictDefaults()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto path = directory.filePath("folder-rail.ini");
        {
            NativePreferences preferences(path);
            const auto layout = preferences.metaObject()->property(
                preferences.metaObject()->indexOfProperty("folderRailLayout"));
            const auto showSystem = preferences.metaObject()->property(
                preferences.metaObject()->indexOfProperty("showSystemFolders"));
            QVERIFY(layout.isValid() && layout.isWritable());
            QVERIFY(showSystem.isValid() && showSystem.isWritable());
            // settings.model.ts createDefaultSettings: vertical + shown.
            QCOMPARE(layout.read(&preferences).toString(), QStringLiteral("vertical"));
            QVERIFY(showSystem.read(&preferences).toBool());
            QSignalSpy layoutChanged(&preferences, SIGNAL(folderRailLayoutChanged()));
            QSignalSpy showSystemChanged(&preferences, SIGNAL(showSystemFoldersChanged()));
            QVERIFY(layoutChanged.isValid() && showSystemChanged.isValid());
            QVERIFY(layout.write(&preferences, QStringLiteral("horizontal")));
            QVERIFY(showSystem.write(&preferences, false));
            QCOMPARE(layoutChanged.count(), 1);
            QCOMPARE(showSystemChanged.count(), 1);
            QSettings disk(path, QSettings::IniFormat);
            QCOMPARE(disk.value("settings/app/folderRailLayout").toString(),
                QStringLiteral("horizontal"));
            QVERIFY(!disk.value("settings/app/showSystemFolders").toBool());
            // Re-writing the same value is a no-op without a re-emit.
            QVERIFY(layout.write(&preferences, QStringLiteral("horizontal")));
            QCOMPARE(layoutChanged.count(), 1);
        }
        {
            NativePreferences reopened(path);
            const auto layout = reopened.metaObject()->property(
                reopened.metaObject()->indexOfProperty("folderRailLayout"));
            const auto showSystem = reopened.metaObject()->property(
                reopened.metaObject()->indexOfProperty("showSystemFolders"));
            QCOMPARE(layout.read(&reopened).toString(), QStringLiteral("horizontal"));
            QVERIFY(!showSystem.read(&reopened).toBool());
        }
        {
            QSettings disk(path, QSettings::IniFormat);
            disk.setValue("settings/app/folderRailLayout", "diagonal");
            disk.setValue("settings/app/showSystemFolders", "sometimes");
            disk.sync();
        }
        NativePreferences sanitized(path);
        const auto layout = sanitized.metaObject()->property(
            sanitized.metaObject()->indexOfProperty("folderRailLayout"));
        const auto showSystem = sanitized.metaObject()->property(
            sanitized.metaObject()->indexOfProperty("showSystemFolders"));
        // resolveFolderRailLayout: a stored value that is not exactly
        // "vertical" lands on "horizontal"; resolveShowSystemFolders falls
        // back to the default show for a non-boolean.
        QCOMPARE(layout.read(&sanitized).toString(), QStringLiteral("horizontal"));
        QVERIFY(showSystem.read(&sanitized).toBool());
    }
    // Issue #107, exordy #387 round 2: the reduced-motion probe must address
    // the XDG Settings portal the way the portal spec does - D-Bus service
    // org.freedesktop.portal.Desktop, object path /org/freedesktop/portal/
    // desktop, interface org.freedesktop.portal.Settings, Read(namespace, key)
    // returning a variant - and keep the SettingChanged watch on that same
    // addressing. Namespace and key stay org.freedesktop.appearance /
    // reduced-motion, and an unknown value means "no preference".
    void prefersReducedMotionFollowsTheXDGSettingsPortal()
    {
        QVERIFY2(portalProbeBus.busConfigWritten(),
            "the portal probe fixture must have created its private bus config directory");
        auto bus = QDBusConnection::sessionBus();
        QVERIFY2(bus.isConnected(), "the portal probe needs a session bus");
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto path = directory.filePath("preferences.ini");
        // Claim the portal name before anything else can (the bus driver would
        // otherwise auto-activate the real portal behind the fake), then
        // answer Read there. The name must go through
        // QDBusConnection::registerService(), not a raw driver RequestName:
        // QtDBus only dispatches calls addressed to names this connection
        // registered itself.
        QObject root;
        auto *portal = new FakeSettingsPortal(&root);
        QVERIFY(bus.registerObject(QStringLiteral("/org/freedesktop/portal/desktop"),
            &root, QDBusConnection::ExportAdaptors));
        QVERIFY2(bus.registerService(QStringLiteral("org.freedesktop.portal.Desktop")),
            qPrintable(bus.lastError().message()));
        {
            NativePreferences reduced(path);
            QVERIFY2(reduced.prefersReducedMotion(),
                "the probe must read reduced-motion via Read on the portal's real D-Bus address");
            QSignalSpy changed(&reduced, &NativePreferences::prefersReducedMotionChanged);
            // Live watch: the portal announces the toggle back to 0 (no
            // preference), then to an unknown value that must not count as
            // reduced motion either.
            QMetaObject::invokeMethod(portal, "SetReducedMotion", Q_ARG(int, 0));
            QTRY_COMPARE_WITH_TIMEOUT(changed.count(), 1, 3000);
            QVERIFY(!reduced.prefersReducedMotion());
            QMetaObject::invokeMethod(portal, "SetReducedMotion", Q_ARG(int, 2));
            QTest::qWait(300);
            QCOMPARE(changed.count(), 1);
            QVERIFY(!reduced.prefersReducedMotion());
        }
        bus.unregisterService(QStringLiteral("org.freedesktop.portal.Desktop"));
        bus.unregisterObject(QStringLiteral("/org/freedesktop/portal/desktop"));
    }

    // Issue #107, exordy #387 round 4: QDir::tempPath() honours TMPDIR, and a
    // legitimate TMPDIR may contain XML metacharacters (an '&' in a checkout
    // path is the classic case). The bus config generator must encode the
    // socket directory so the <listen> value is a valid D-Bus address after
    // the XML parses: reserved address characters come back percent-encoded
    // (%26, %3C, %3E, %22), which is simultaneously XML-safe text.
    void busConfigXmlEscapesXmlMetacharactersInTemporaryPath()
    {
        const QString hostile = QStringLiteral("/tmp/a&b/<x>y\"z");
        QCOMPARE(PortalProbeBus::busConfigXml(hostile), QStringLiteral(
            "<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN\""
            " \"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">"
            "<busconfig><type>session</type>"
            "<listen>unix:tmpdir=/tmp/a%26b/%3Cx%3Ey%22z</listen>"
            "<policy context=\"default\">"
            "<allow send_destination=\"*\" eavesdrop=\"true\"/>"
            "<allow eavesdrop=\"true\"/><allow own=\"*\"/>"
            "</policy></busconfig>"));
    }

    // The real proof: point the private bus at a socket directory whose name
    // literally contains '&' and '<', and show a real dbus-daemon parses the
    // config and prints its address instead of exiting with a config error.
    //
    // exordy #387 round 5: the metacharacter directory is NOT created directly
    // under the shared TMPDIR under a predictable name, and nothing here ever
    // recursively removes a path this test does not exclusively own. The unique
    // private parent (QTemporaryDir: mkdtemp name, 0700) is created FIRST; the
    // 'probe&bus<x>' directory and the bus socket live strictly inside it, and
    // the only cleanup is that parent's own destructor.
    void dbusDaemonAcceptsTemporaryPathWithXmlMetacharacters()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon")).isEmpty())
            QSKIP("dbus-daemon is unavailable in this environment");
        // Keep the parent prefix short: the bus socket lands deep inside it
        // and the whole sun_path must stay under the 108-char unix limit.
        QTemporaryDir privateRoot{QDir::tempPath() + QStringLiteral("/wn107-XXXXXX")};
        QVERIFY(privateRoot.isValid());
        const QString hostileRoot = privateRoot.path() + QStringLiteral("/probe&bus<x>");
        QVERIFY(QDir().mkpath(hostileRoot));
        QVERIFY2(hostileRoot.contains(QLatin1Char('&')),
            "the fixture path itself must carry the metacharacter into the config");
        QVERIFY2(hostileRoot.startsWith(privateRoot.path() + QLatin1Char('/')),
            "every path the fixture creates must live inside its private parent");
        QTemporaryDir dir(hostileRoot + QStringLiteral("/bus-XXXXXX"));
        QVERIFY(dir.isValid());
        QVERIFY2(dir.path().contains(QLatin1Char('&')),
            "the socket directory itself must carry the metacharacter into the config");
        const QString config = dir.filePath(QStringLiteral("bus.conf"));
        QFile configFile(config);
        QVERIFY(configFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QVERIFY(configFile.write(PortalProbeBus::busConfigXml(dir.path()).toUtf8()));
        configFile.close();
        QProcess daemon;
        daemon.start(QStringLiteral("dbus-daemon"),
            {QStringLiteral("--config-file=") + config, QStringLiteral("--print-address")});
        QVERIFY(daemon.waitForStarted(5000));
        const bool printedAddress = daemon.waitForReadyRead(5000);
        const QString address = QString::fromUtf8(daemon.readAllStandardOutput())
            .trimmed().split(QLatin1Char(',')).first();
        const QString failure = QString::fromUtf8(daemon.readAllStandardError());
        daemon.terminate();
        if (!daemon.waitForFinished(3000))
            daemon.kill();
        // No manual cleanup: `privateRoot`'s destructor removes only the
        // unique directory it created, metacharacter subtree included.
        QVERIFY2(printedAddress && address.startsWith(QLatin1String("unix:")),
            qPrintable(QStringLiteral("dbus-daemon rejected the config: %1").arg(failure)));
    }
};

QTEST_MAIN(NativePreferencesTest)
#include "native-preferences-test.moc"
