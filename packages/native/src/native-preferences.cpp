#include "native-preferences.h"
#include "native-auth-guard.h"
#include "domain.h"

#include <QDebug>
#include <QGuiApplication>
#include <QLocale>
#include <QKeyEvent>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>
#include <QStyleHints>
#include <QScreen>
#include <QWindow>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>
#include <algorithm>
#include <cmath>

namespace workspace {
namespace {
QString preferencesPath(const QString &provided)
{
    return provided.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/preferences.ini"
        : provided;
}

// #62 round 2: fullscreen transition gate timing. The settle interval is how
// long the window must sit in the transition's target state without further
// WM movement before the gate closes and re-captures the live state; the cap
// closes the gate with the same re-capture if the WM never delivers a stable
// target state at all.
constexpr int FullscreenGateSettleMs = 300;
constexpr int FullscreenGateCapMs = 1500;

QString validPalette(const QString &palette)
{
    for (const auto *id : {"orange-warm", "blue-cold", "blue-mist", "emerald-chat"})
        if (palette == QLatin1String(id)) return palette;
    return QStringLiteral("blue-cold");
}

QString validNotificationSound(const QString &value)
{
    for (const auto *preset : {"subtle", "digital", "glass", "pulse", "none"})
        if (value == QLatin1String(preset)) return value;
    return QStringLiteral("default");
}

QString validSortMode(const QString &value)
{
    return value == "alphabetical" || value == "unread_first" ? value : QStringLiteral("last_message");
}

QString validMode(const QString &mode)
{
    return mode == "light" || mode == "dark" || mode == "system" ? mode : QStringLiteral("system");
}

constexpr qint64 MaxReactionCount = 1000000000;
constexpr qsizetype MaxReactionHistoryBytes = 2 * 1024 * 1024;

bool knownReaction(const QString &name)
{
    return !name.isEmpty() && canonicalReactionName(name) == name && !reactionUnicode(name).isEmpty();
}

bool validCount(const QJsonValue &value)
{
    const auto count = value.toDouble(-1);
    return value.isDouble() && std::isfinite(count) && count > 0
        && count <= MaxReactionCount && count == std::floor(count);
}

QString normalizeOrganizationOrigin(const QString &value)
{
    const QUrl url(value.trimmed());
    if (!url.isValid() || url.host().isEmpty())
        return {};
    const auto scheme = url.scheme();
    if (scheme != QLatin1String("https") && scheme != QLatin1String("http"))
        return {};
    QUrl origin;
    origin.setScheme(scheme);
    origin.setHost(url.host());
    if (url.port() != -1)
        origin.setPort(url.port());
    return origin.toString();
}
} // namespace

NativePreferences::NativePreferences(QString settingsFile, QObject *parent, std::function<QDate()> utcToday)
    : QObject(parent), m_settings(preferencesPath(settingsFile), QSettings::IniFormat), m_hotkeys(m_settings, this),
      m_utcToday(std::move(utcToday))
{
    if (!m_utcToday) m_utcToday = [] { return QDateTime::currentDateTimeUtc().date(); };
    m_settings.setFallbacksEnabled(false);
    m_hotkeys.load(settingsGroup());
    {
        // Persisted organization suggestions (login wizard). Entries that no
        // longer normalize to an origin are dropped instead of offered.
        const auto stored = m_settings.value("login/organizationHistory").toStringList();
        QStringList loaded;
        for (const auto &entry : stored) {
            const auto origin = normalizeOrganizationOrigin(entry);
            if (!origin.isEmpty() && !loaded.contains(origin))
                loaded.append(origin);
        }
        m_organizationHistory = loaded;
    }
    m_windowSaveTimer.setSingleShot(true);
    m_windowSaveTimer.setInterval(350);
    connect(&m_windowSaveTimer, &QTimer::timeout, this, &NativePreferences::saveWindow);
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &NativePreferences::saveWindow);
    // #62 round 2: both fullscreen gate timers release through the same
    // completion path so a gate that ends - early or capped - re-captures
    // the live window state instead of leaving the save stale.
    m_fullscreenGateSettle.setSingleShot(true);
    m_fullscreenGateSettle.setInterval(FullscreenGateSettleMs);
    connect(&m_fullscreenGateSettle, &QTimer::timeout, this, &NativePreferences::finishFullscreenGate);
    m_fullscreenGateCap.setSingleShot(true);
    m_fullscreenGateCap.setInterval(FullscreenGateCapMs);
    connect(&m_fullscreenGateCap, &QTimer::timeout, this, &NativePreferences::finishFullscreenGate);
    m_conversationSaveTimer.setSingleShot(true);
    m_conversationSaveTimer.setInterval(300); // Source last-messenger-route debounce.
    connect(&m_conversationSaveTimer, &QTimer::timeout, this, &NativePreferences::saveLastConversation);
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &NativePreferences::saveLastConversation);
    if (qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        auto *style = QGuiApplication::styleHints();
        m_systemDark = style->colorScheme() == Qt::ColorScheme::Dark;
        connect(style, &QStyleHints::colorSchemeChanged, this, [this](Qt::ColorScheme scheme) {
            const bool next = scheme == Qt::ColorScheme::Dark;
            if (m_systemDark == next) return;
            m_systemDark = next;
            if (m_mode == "system") emit darkChanged();
        });
    }
    bool valid = false;
    const auto storedWidth = m_settings.value("layout/sidebarWidth", 299).toDouble(&valid);
    if (valid && std::isfinite(storedWidth) && storedWidth >= 240 && storedWidth <= 720)
        m_sidebarWidth = std::round(storedWidth);
    // settings.model.ts:174 parses composerToolbarExpanded strictly (=== true).
    m_composerToolbarExpanded = m_settings.value("layout/composerToolbarExpanded", false).toBool();
    const auto cacheLimit = m_settings.value("cache/limitGb", 10).toString();
    static const QRegularExpression integer("^[0-9]{1,4}\\z");
    if (integer.match(cacheLimit).hasMatch()) {
        const auto storedLimit = cacheLimit.toInt();
        if (storedLimit >= 1 && storedLimit <= 1000) m_cacheLimitGb = storedLimit;
    }
    loadAppearance();
    loadAuthIdleTimeout();
    loadChatLayout();
    loadFolderRail();
    probePortalReducedMotion();
}

NativePreferences::~NativePreferences()
{
    saveLastConversation();
}

QStringList NativePreferences::loadLastConversation(const QString &owner)
{
    static const QRegularExpression hash("^[0-9a-f]{64}$");
    if (owner.size() != 64 || !hash.match(owner).hasMatch()) return {};
    if (owner == m_pendingConversationOwner) return m_pendingConversation;
    const auto route = m_settings.value("navigation/owners/" + owner + "/lastConversation").toStringList();
    if (route.size() != 2 || (!route[0].isEmpty() && !isUuid(route[0]))
        || (!route[1].isEmpty() && (!isUuid(route[1]) || route[0].isEmpty()))) return {};
    return route;
}

void NativePreferences::recordLastConversation(const QString &owner, const QString &stream, const QString &topic)
{
    static const QRegularExpression hash("^[0-9a-f]{64}$");
    if (owner.size() != 64 || !hash.match(owner).hasMatch()
        || (!stream.isEmpty() && !isUuid(stream)) || (!topic.isEmpty() && (!isUuid(topic) || stream.isEmpty()))) return;
    const QStringList route{stream.toLower(), topic.toLower()};
    if (loadLastConversation(owner) == route) return;
    if (!m_pendingConversationOwner.isEmpty() && owner != m_pendingConversationOwner) saveLastConversation();
    m_pendingConversationOwner = owner;
    m_pendingConversation = route;
    m_conversationSaveTimer.start();
}

QStringList NativePreferences::loadExpandedStreams(const QString &owner)
{
    static const QRegularExpression hash("^[0-9a-f]{64}$");
    if (!hash.match(owner).hasMatch()) return {};
    QStringList result;
    const auto stored = m_settings.value("navigation/owners/" + owner + "/expandedStreams").toStringList();
    for (const auto &id : stored) {
        const auto normalized = id.toLower();
        if (isUuid(normalized) && !result.contains(normalized)) result.append(normalized);
    }
    return result;
}

void NativePreferences::recordExpandedStreams(const QString &owner, const QStringList &streams)
{
    static const QRegularExpression hash("^[0-9a-f]{64}$");
    if (!hash.match(owner).hasMatch()) return;
    QStringList normalized;
    for (const auto &id : streams) {
        const auto value = id.toLower();
        if (isUuid(value) && !normalized.contains(value)) normalized.append(value);
    }
    m_settings.setValue("navigation/owners/" + owner + "/expandedStreams", normalized);
    m_settings.sync();
}

void NativePreferences::recordOrganizationOrigin(const QString &origin)
{
    const auto normalized = normalizeOrganizationOrigin(origin);
    if (normalized.isEmpty())
        return;
    QStringList history{normalized};
    for (const auto &previous : m_organizationHistory)
        if (previous != normalized && !history.contains(previous))
            history.append(previous);
    if (history == m_organizationHistory)
        return;
    m_organizationHistory = history;
    m_settings.setValue("login/organizationHistory", history);
    m_settings.sync();
    emit organizationHistoryChanged();
}

void NativePreferences::saveLastConversation()
{
    if (m_pendingConversationOwner.isEmpty()) return;
    m_conversationSaveTimer.stop();
    m_settings.setValue("navigation/owners/" + m_pendingConversationOwner + "/lastConversation", m_pendingConversation);
    m_settings.sync();
    m_pendingConversationOwner.clear();
    m_pendingConversation.clear();
}

bool NativePreferences::dark() const
{
    return m_mode == "dark" || (m_mode == "system" && m_systemDark);
}

// Issue #107: Qt 6.10 has no cross-platform reduced-motion hint
// (QStyleHints::accessibility() only carries contrast), so read the same
// preference the web's prefers-reduced-motion media query stands for on
// Linux: the XDG Settings portal's documented org.freedesktop.appearance
// reduced-motion key. The Settings interface is served by
// org.freedesktop.portal.Desktop at /org/freedesktop/portal/desktop - Read
// runs there with a short timeout so a live-but-slow portal never hangs
// startup, unknown values mean "no preference" per spec, and SettingChanged
// keeps the flag live when the user flips the toggle.
// Windows/macOS probes are deliberately out of scope (Linux-only product).
void NativePreferences::probePortalReducedMotion()
{
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return;
    auto read = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.portal.Desktop"),
        QStringLiteral("/org/freedesktop/portal/desktop"),
        QStringLiteral("org.freedesktop.portal.Settings"),
        QStringLiteral("Read"))
        << QStringLiteral("org.freedesktop.appearance") << QStringLiteral("reduced-motion");
    const auto reply = bus.call(read, QDBus::Block, 2000);
    if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
        auto value = reply.arguments().first().value<QDBusVariant>().variant();
        // Settings.Read wraps the setting in a second variant. Some older
        // backends return the uint32 in a single variant.
        if (value.metaType() == QMetaType::fromType<QDBusVariant>())
            value = value.value<QDBusVariant>().variant();
        updatePortalReducedMotion(value.metaType().id() == QMetaType::UInt && value.toUInt() == 1u);
    }
    // Connect last and ignore failure: without a portal announcing the key the
    // default "no preference" already holds.
    bus.connect(QStringLiteral("org.freedesktop.portal.Desktop"),
        QStringLiteral("/org/freedesktop/portal/desktop"),
        QStringLiteral("org.freedesktop.portal.Settings"),
        QStringLiteral("SettingChanged"),
        this, SLOT(onPortalSettingChanged(QString, QString, QDBusVariant)));
}

void NativePreferences::onPortalSettingChanged(const QString &ns, const QString &key,
                                               const QDBusVariant &value)
{
    if (ns == QLatin1String("org.freedesktop.appearance")
        && key == QLatin1String("reduced-motion"))
        updatePortalReducedMotion(value.variant().toUInt() == 1u);
}

void NativePreferences::updatePortalReducedMotion(bool reduced)
{
    const bool was = prefersReducedMotion();
    m_portalReducedMotion = reduced;
    if (was != prefersReducedMotion())
        emit prefersReducedMotionChanged();
}

void NativePreferences::setPrefersReducedMotionOverride(bool reduced)
{
    const bool was = prefersReducedMotion();
    m_reducedMotionOverride = reduced;
    if (was != prefersReducedMotion())
        emit prefersReducedMotionChanged();
}

QString NativePreferences::appearanceGroup() const
{
    return m_owner.isEmpty() ? QStringLiteral("appearance/anonymous")
                             : QStringLiteral("appearance/owners/") + m_owner;
}

void NativePreferences::setError(const QString &error)
{
    if (m_error == error) return;
    m_error = error;
    emit errorChanged();
}

void NativePreferences::loadAppearance()
{
    const auto oldPalette = m_palette;
    const auto oldMode = m_mode;
    const auto oldLocale = m_locale;
    const bool oldDark = dark();
    const auto group = appearanceGroup();
    m_palette = validPalette(m_settings.value(group + "/palette", "blue-cold").toString());
    m_mode = validMode(m_settings.value(group + "/mode", "system").toString());
    const auto storedLocale = m_settings.value(group + "/locale").toString();
    m_locale = storedLocale == "en" || storedLocale == "ru" ? storedLocale
        : QLocale::system().language() == QLocale::Russian ? QStringLiteral("ru") : QStringLiteral("en");
    if (m_settings.status() != QSettings::NoError) {
        m_palette = "blue-cold";
        m_mode = "system";
        setError(tr("Saved appearance preferences could not be read."));
    } else {
        setError({});
    }
    if (oldPalette != m_palette) emit paletteIdChanged();
    if (oldMode != m_mode) emit modeChanged();
    if (oldLocale != m_locale) emit localeChanged();
    if (oldDark != dark()) emit darkChanged();
}

void NativePreferences::setOwner(const QString &owner)
{
    static const QRegularExpression hash("^[0-9a-f]{64}$");
    const bool valid = owner.isEmpty() || (owner.size() == 64 && hash.match(owner).hasMatch());
    const auto next = valid ? owner : QString{};
    if (m_owner != next) {
        m_owner = next;
        m_hotkeys.load(settingsGroup());
        // Appearance emits synchronously. Every observer must already see this
        // owner's history, including while the language/theme is changing.
        loadReactionHistory();
        loadAppearance();
        loadAuthIdleTimeout();
    }
    if (!valid) setError(tr("The appearance preference scope is invalid."));
}

bool NativePreferences::loadOwnStatusAway(const QString &owner)
{
    static const QRegularExpression hash("^[0-9a-f]{64}$");
    if (owner.size() != 64 || !hash.match(owner).hasMatch()) return false;
    return m_settings.value("presence/owners/" + owner + "/manualAway", false).toBool();
}

void NativePreferences::recordOwnStatusAway(const QString &owner, bool away)
{
    static const QRegularExpression hash("^[0-9a-f]{64}$");
    if (owner.size() != 64 || !hash.match(owner).hasMatch()) return;
    const auto key = "presence/owners/" + owner + "/manualAway";
    if (away) m_settings.setValue(key, true); else m_settings.remove(key);
    m_settings.sync(); // Source writeManualStatus is best effort after a successful response.
}

bool NativePreferences::loadActivityOpen(const QString &owner)
{
    static const QRegularExpression hash("^[0-9a-f]{64}$");
    if (owner.size() != 64 || !hash.match(owner).hasMatch()) return false;
    // Absent means closed (sidebar-config.model.ts DEFAULT_CONFIG.activityOpen).
    return m_settings.value("sidebar/owners/" + owner + "/activityOpen", false).toBool();
}

void NativePreferences::recordActivityOpen(const QString &owner, bool open)
{
    static const QRegularExpression hash("^[0-9a-f]{64}$");
    if (owner.size() != 64 || !hash.match(owner).hasMatch()) return;
    const auto key = "sidebar/owners/" + owner + "/activityOpen";
    if (open) m_settings.setValue(key, true); else m_settings.remove(key);
    m_settings.sync(); // Same persistence contract as recordExpandedStreams.
}

QString NativePreferences::settingsGroup() const
{
    return m_owner.isEmpty() ? QStringLiteral("settings/anonymous")
                             : QStringLiteral("settings/owners/") + m_owner;
}

void NativePreferences::loadAuthIdleTimeout()
{
    const auto old = m_authIdleTimeout;
    // settings.model.ts:175-178: an invalid persisted value loads as "3d".
    m_authIdleTimeout = NativeAuthGuard::sanitizePreset(
        m_settings.value(settingsGroup() + "/authIdleTimeout", QStringLiteral("3d")).toString());
    if (m_authIdleTimeout != old)
        emit authIdleTimeoutChanged();
}

// Issue #102 (docs/NATIVE_DESKTOP_CHAT_LIST_DENSITY.md): the reference resolvers
// (workspace_ui 61dacef settings.model.ts:124,173) accept exactly one non-default
// token each and fall back to the default otherwise. The keys are global
// (`settings/app/...`) because settings.model.ts persistSettings keeps one global
// settings blob (deviation D2), unlike the per-owner authIdleTimeout scope.
void NativePreferences::loadChatLayout()
{
    const auto sound = validNotificationSound(m_settings.value("settings/app/notificationSound").toString());
    const auto sortMode = validSortMode(m_settings.value("settings/app/sidebarSortMode").toString());
    if (sound != m_notificationSound) { m_notificationSound = sound; emit notificationSoundChanged(); }
    if (sortMode != m_sidebarSortMode) { m_sidebarSortMode = sortMode; emit sidebarSortModeChanged(); }
    const auto density = m_settings.value("settings/app/chatListDensity").toString();
    const auto action = m_settings.value("settings/app/chatClickAction").toString();
    const auto nextDensity = density == QLatin1String("compact")
        ? density : QStringLiteral("general");
    const auto nextAction = action == QLatin1String("latestTopic")
        ? action : QStringLiteral("stream");
    if (nextDensity != m_chatListDensity) {
        m_chatListDensity = nextDensity;
        emit chatListDensityChanged();
    }
    if (nextAction != m_chatClickAction) {
        m_chatClickAction = nextAction;
        emit chatClickActionChanged();
    }
}

void NativePreferences::setNotificationSound(const QString &preset)
{
    const auto value = validNotificationSound(preset);
    if (m_notificationSound == value) return;
    m_notificationSound = value;
    m_settings.setValue("settings/app/notificationSound", value);
    m_settings.sync();
    emit notificationSoundChanged();
}

void NativePreferences::previewNotificationSound()
{
    if (m_notificationSound != "none") emit notificationSoundRequested(m_notificationSound);
}

void NativePreferences::setSidebarSortMode(const QString &mode)
{
    const auto value = validSortMode(mode);
    if (m_sidebarSortMode == value) return;
    m_sidebarSortMode = value;
    m_settings.setValue("settings/app/sidebarSortMode", value);
    m_settings.sync();
    emit sidebarSortModeChanged();
}

void NativePreferences::setChatListDensity(const QString &density)
{
    const auto next = density == QLatin1String("compact") ? density : QStringLiteral("general");
    if (next == m_chatListDensity) return;
    m_chatListDensity = next;
    m_settings.setValue("settings/app/chatListDensity", next);
    m_settings.sync();
    emit chatListDensityChanged();
}

void NativePreferences::setChatClickAction(const QString &action)
{
    const auto next = action == QLatin1String("latestTopic")
        ? action : QStringLiteral("stream");
    if (next == m_chatClickAction) return;
    m_chatClickAction = next;
    m_settings.setValue("settings/app/chatClickAction", next);
    m_settings.sync();
    emit chatClickActionChanged();
}

// Issue #85 (docs/NATIVE_DESKTOP_FOLDERS_RAIL_LAYOUT.md): the reference
// resolvers (workspace_ui 61dacef settings.model.ts resolveFolderRailLayout /
// resolveShowSystemFolders) are asymmetric: a virgin store starts on the
// default vertical/shown pair (createDefaultSettings), but once a value was
// stored the layout only stays "vertical" on the exact token - anything else
// resolves to "horizontal" - while the visibility flag is accepted only as a
// strict boolean and keeps the default true otherwise. The keys join the #102
// global settings/app scope (the reference settings blob is global too).
void NativePreferences::loadFolderRail()
{
    const auto layout = m_settings.value("settings/app/folderRailLayout");
    const auto nextLayout = !layout.isValid()
            || layout.toString() == QLatin1String("vertical")
        ? QStringLiteral("vertical") : QStringLiteral("horizontal");
    if (nextLayout != m_folderRailLayout) {
        m_folderRailLayout = nextLayout;
        emit folderRailLayoutChanged();
    }
    const auto shown = m_settings.value("settings/app/showSystemFolders");
    const auto shownText = shown.toString();
    const auto nextShown = shown.isValid() && shownText == QLatin1String("false")
        ? false : true;
    if (nextShown != m_showSystemFolders) {
        m_showSystemFolders = nextShown;
        emit showSystemFoldersChanged();
    }
}

void NativePreferences::setFolderRailLayout(const QString &layout)
{
    const auto next = layout == QLatin1String("vertical")
        ? layout : QStringLiteral("horizontal");
    if (next == m_folderRailLayout) return;
    m_folderRailLayout = next;
    m_settings.setValue("settings/app/folderRailLayout", next);
    m_settings.sync();
    emit folderRailLayoutChanged();
}

void NativePreferences::setShowSystemFolders(bool show)
{
    if (show == m_showSystemFolders) return;
    m_showSystemFolders = show;
    m_settings.setValue("settings/app/showSystemFolders", show);
    m_settings.sync();
    emit showSystemFoldersChanged();
}

void NativePreferences::setAuthIdleTimeout(const QString &preset)
{
    const auto next = NativeAuthGuard::sanitizePreset(preset);
    if (next == m_authIdleTimeout) return;
    m_authIdleTimeout = next;
    m_settings.setValue(settingsGroup() + "/authIdleTimeout", next);
    m_settings.sync();
    emit authIdleTimeoutChanged();
}

void NativePreferences::persist()
{
    const auto group = appearanceGroup();
    m_settings.setValue(group + "/palette", m_palette);
    m_settings.setValue(group + "/mode", m_mode);
    m_settings.setValue(group + "/locale", m_locale);
    m_settings.setValue("layout/sidebarWidth", m_sidebarWidth);
    m_settings.setValue("layout/composerToolbarExpanded", m_composerToolbarExpanded);
    m_settings.sync();
    setError(m_settings.status() == QSettings::NoError
        ? QString{} : tr("Appearance preferences could not be saved."));
}

void NativePreferences::setPaletteId(const QString &palette)
{
    const auto next = validPalette(palette);
    if (next == m_palette) return;
    m_palette = next;
    persist();
    emit paletteIdChanged();
}

void NativePreferences::setMode(const QString &mode)
{
    const auto next = validMode(mode);
    if (next == m_mode) return;
    const bool oldDark = dark();
    m_mode = next;
    persist();
    emit modeChanged();
    if (oldDark != dark()) emit darkChanged();
}

void NativePreferences::setSidebarPreferredWidth(qreal width)
{
    const auto next = std::isfinite(width) ? std::clamp(std::round(width), qreal(240), qreal(720)) : qreal(299);
    if (next == m_sidebarWidth) return;
    m_sidebarWidth = next;
    persist();
    emit sidebarPreferredWidthChanged();
}

void NativePreferences::setComposerToolbarExpanded(bool expanded)
{
    if (expanded == m_composerToolbarExpanded) return;
    m_composerToolbarExpanded = expanded;
    persist();
    emit composerToolbarExpandedChanged();
}

void NativePreferences::setLocale(const QString &locale)
{
    if ((locale != "en" && locale != "ru") || locale == m_locale) return;
    m_locale = locale;
    persist();
    emit localeChanged();
}

bool NativePreferences::resetLocalSettings()
{
    m_conversationSaveTimer.stop();
    m_pendingConversationOwner.clear();
    m_pendingConversation.clear();
    const QPointer<QWindow> window = m_window;
    attachWindow(nullptr);
    m_settings.clear();
    m_settings.setValue("login/organizationHistory", m_organizationHistory);
    m_settings.sync();
    if (window) {
        window->setWindowState(Qt::WindowNoState);
        attachWindow(window);
    }
    if (m_settings.status() != QSettings::NoError) {
        setError(tr("The local settings could not be cleared."));
        return false;
    }
    m_mentionStore = {};
    m_mentionStoreLoaded = false;
    m_mentionStoreOwner.clear();
    m_sidebarWidth = 299;
    m_cacheLimitGb = 10;
    m_composerToolbarExpanded = false;
    m_hotkeys.load(settingsGroup());
    loadAppearance();
    loadAuthIdleTimeout();
    loadChatLayout();
    loadFolderRail();
    loadReactionHistory();
    emit sidebarPreferredWidthChanged();
    emit cacheLimitGbChanged();
    emit composerToolbarExpandedChanged();
    return true;
}

void NativePreferences::setCacheLimitGb(int gigabytes)
{
    if (gigabytes < 1 || gigabytes > 1000 || gigabytes == m_cacheLimitGb) return;
    m_cacheLimitGb = gigabytes;
    m_settings.setValue("cache/limitGb", gigabytes);
    m_settings.sync();
    setError(m_settings.status() == QSettings::NoError ? QString{}
        : tr("The cache limit could not be saved."));
    emit cacheLimitGbChanged();
}

void NativePreferences::reportCacheError(const QString &error)
{
    if (m_cacheError == error) return;
    m_cacheError = error;
    emit cacheErrorChanged();
}

void NativePreferences::reportCacheStatus(bool busy, bool available, qint64 appliedLimitBytes, qint64 usageBytes)
{
    appliedLimitBytes = qMax<qint64>(0, appliedLimitBytes);
    usageBytes = qMax<qint64>(0, usageBytes);
    if (m_cacheBusy == busy && m_cacheAvailable == available
        && m_cacheAppliedLimitBytes == appliedLimitBytes && m_cacheUsageBytes == usageBytes) return;
    m_cacheBusy = busy;
    m_cacheAvailable = available;
    m_cacheAppliedLimitBytes = appliedLimitBytes;
    m_cacheUsageBytes = usageBytes;
    emit cacheStatusChanged();
}

QJsonObject NativePreferences::recentReactionUsage(const QJsonObject &days) const
{
    QJsonObject result;
    const auto today = m_utcToday();
    if (!today.isValid()) return result;
    for (auto it = days.begin(); it != days.end(); ++it) {
        const auto date = QDate::fromString(it.key(), Qt::ISODate);
        if (!date.isValid() || date.toString(Qt::ISODate) != it.key()
            || date > today || date < today.addDays(-6) || !it.value().isObject()) continue;
        QJsonObject counts;
        const auto raw = it.value().toObject();
        for (auto count = raw.begin(); count != raw.end(); ++count)
            if (knownReaction(count.key()) && validCount(count.value())) counts.insert(count.key(), count.value());
        if (!counts.isEmpty()) result.insert(it.key(), counts);
    }
    return result;
}

void NativePreferences::loadReactionHistory()
{
    m_suggested.clear();
    m_quickUsage = {};
    if (!m_owner.isEmpty()) {
        const auto group = QStringLiteral("reactions/owners/") + m_owner;
        const auto suggestions = m_settings.value(group + "/suggested").toByteArray();
        QSet<QString> seen;
        if (suggestions.size() <= MaxReactionHistoryBytes) {
            const auto rows = QJsonDocument::fromJson(suggestions).array();
            for (const auto &value : rows) {
                if (!value.isObject()) continue;
                const auto row = value.toObject();
                const auto name = row.value("name").toString();
                if (!knownReaction(name) || seen.contains(name) || !validCount(row.value("count"))) continue;
                seen.insert(name);
                m_suggested.append(QVariantMap{{"name", name}, {"count", row.value("count").toInteger()}});
                if (m_suggested.size() == 14) break;
            }
        }
        const auto usage = m_settings.value(group + "/usage").toByteArray();
        if (usage.size() <= MaxReactionHistoryBytes)
            m_quickUsage = recentReactionUsage(QJsonDocument::fromJson(usage).object());
    }
    emit reactionHistoryChanged();
}

QVariantList NativePreferences::suggestedReactions() const
{
    auto result = m_suggested;
    // Match emoji-picker-react FREQUENT mode: frequency, with MRU ties.
    std::stable_sort(result.begin(), result.end(), [](const QVariant &left, const QVariant &right) {
        return left.toMap().value("count").toLongLong() > right.toMap().value("count").toLongLong();
    });
    return result;
}

QVariantMap NativePreferences::quickReactionUsage() const
{
    QVariantMap counts;
    const auto days = recentReactionUsage(m_quickUsage);
    for (const auto &day : days) {
        const auto values = day.toObject();
        for (auto it = values.begin(); it != values.end(); ++it)
            counts[it.key()] = counts.value(it.key()).toLongLong() + it.value().toInteger();
    }
    return counts;
}

void NativePreferences::persistReactionHistory()
{
    if (m_owner.isEmpty()) return;
    const auto group = QStringLiteral("reactions/owners/") + m_owner;
    m_settings.setValue(group + "/suggested", QJsonDocument(QJsonArray::fromVariantList(m_suggested)).toJson(QJsonDocument::Compact));
    m_settings.setValue(group + "/usage", QJsonDocument(m_quickUsage).toJson(QJsonDocument::Compact));
    m_settings.sync();
    if (m_settings.status() != QSettings::NoError)
        setError(tr("Reaction history could not be saved."));
}

void NativePreferences::recordSuggestedReaction(const QString &name, const QString &expectedOwner)
{
    if (m_owner.isEmpty() || m_owner != expectedOwner || !knownReaction(name)) return;
    qint64 count = 0;
    for (auto it = m_suggested.begin(); it != m_suggested.end(); ++it) {
        if (it->toMap().value("name").toString() != name) continue;
        count = it->toMap().value("count").toLongLong();
        m_suggested.erase(it);
        break;
    }
    m_suggested.prepend(QVariantMap{{"name", name}, {"count", qMin(MaxReactionCount, count + 1)}});
    if (m_suggested.size() > 14) m_suggested.removeLast();
    persistReactionHistory();
    emit reactionHistoryChanged();
}

void NativePreferences::recordQuickReactionUse(const QString &name, const QString &expectedOwner)
{
    if (m_owner.isEmpty() || m_owner != expectedOwner || !knownReaction(name)) return;
    m_quickUsage = recentReactionUsage(m_quickUsage);
    const auto today = m_utcToday().toString(Qt::ISODate);
    if (today.isEmpty()) return;
    auto counts = m_quickUsage.value(today).toObject();
    counts.insert(name, qMin(MaxReactionCount, counts.value(name).toInteger() + 1));
    m_quickUsage.insert(today, counts);
    persistReactionHistory();
    emit reactionHistoryChanged();
}

void NativePreferences::toggleMode()
{
    setMode(dark() ? QStringLiteral("light") : QStringLiteral("dark"));
}

void NativePreferences::attachWindow(QWindow *window)
{
    if (m_window == window) return;
    if (m_window) {
        saveWindow();
        disconnect(m_window, nullptr, this, nullptr);
        m_window->removeEventFilter(this);
    }
    m_windowSaveTimer.stop();
    m_frameFitTimer.stop();
    // #62 round 2: an in-flight transition belongs to the window being
    // detached; never let its gate leak onto the next one.
    m_fullscreenGateSettle.stop();
    m_fullscreenGateCap.stop();
    m_fullscreenPending = false;
    m_window = window;
    if (!window) return;
    // #62: the F11 fullscreen toggle listens on the same window this
    // component already persists the state for (see eventFilter).
    window->installEventFilter(this);
    const auto screens = QGuiApplication::screens();
    if (screens.isEmpty()) return;
    const auto primary = QGuiApplication::primaryScreen()->availableGeometry();
    bool widthValid = false, heightValid = false, xValid = false, yValid = false;
    int width = m_settings.value("window/width", 1440).toInt(&widthValid);
    int height = m_settings.value("window/height", 900).toInt(&heightValid);
    const int x = m_settings.value("window/x").toInt(&xValid);
    const int y = m_settings.value("window/y").toInt(&yValid);
    if (!widthValid || width < 700 || width > 32768) width = 1440;
    if (!heightValid || height < 600 || height > 32768) height = 900;
    bool positionValid = xValid && yValid && x >= -1000000 && x <= 1000000 && y >= -1000000 && y <= 1000000;
    QRect saved(positionValid ? x : primary.x(), positionValid ? y : primary.y(), width, height);
    QRect available = primary;
    // Select the monitor with the largest visible intersection. A removed
    // monitor or malformed saved position falls back to the primary screen.
    qint64 bestArea = 0;
    if (positionValid) {
        for (const auto *screen : screens) {
            const auto intersection = screen->availableGeometry().intersected(saved);
            const qint64 area = qint64(intersection.width()) * intersection.height();
            if (area > bestArea) { bestArea = area; available = screen->availableGeometry(); }
        }
        if (bestArea == 0) positionValid = false;
    }
    width = std::clamp(width, 700, qMax(700, available.width()));
    height = std::clamp(height, 600, qMax(600, available.height()));
    QPoint topLeft = positionValid ? saved.topLeft()
        : QPoint(available.x() + (available.width() - width) / 2, available.y() + (available.height() - height) / 2);
    topLeft.setX(std::clamp(topLeft.x(), available.left(), qMax(available.left(), available.right() - width + 1)));
    topLeft.setY(std::clamp(topLeft.y(), available.top(), qMax(available.top(), available.bottom() - height + 1)));
    m_normalGeometry = QRect(topLeft, QSize(width, height));
    m_windowMaximized = m_settings.value("window/maximized", false).toBool();
    window->setGeometry(m_normalGeometry);
    if (m_windowMaximized) window->setWindowState(Qt::WindowMaximized);
    // #805: Qt's width/height are client-area sizes while the reference stores
    // frame bounds (main.ts getBounds/getNormalBounds of the pinned revision),
    // so a saved or default size that exactly fills the work area puts the
    // title bar's worth of pixels below the screen bottom under a decorating
    // WM - the composer row falls off screen. Once the WM reports its frame
    // extents, fit the client exactly once so the whole frame stays inside the
    // screen's available geometry. This is a startup-only adjustment: user
    // geometry is honoured afterwards, and a window that starts maximized is
    // left alone (the reference restores unmaximized bounds unfitted too).
    // Qt exposes no frameMarginsChanged notification, so a short bounded
    // poll watches for the WM to report its extents; it stops after the first
    // verdict (fit, undecorated, or deadline) until the next window attach.
    m_frameFitTimer.stop();
    m_frameFitTimer.setInterval(50);
    connect(&m_frameFitTimer, &QTimer::timeout, this,
        &NativePreferences::fitFrameToAvailableGeometry, Qt::UniqueConnection);
    m_frameFitDeadline.restart();
    fitFrameToAvailableGeometry(); // a re-attached window already has extents
    m_frameFitTimer.start();
    const auto geometryChanged = [this] {
        if (!m_window) return;
        if (m_fullscreenPending) { noteFullscreenGateSignal(); return; }
        if (m_window->windowState() == Qt::WindowMaximized)
            m_maximizedGeometry = m_window->geometry();
        else if (m_window->windowState() == Qt::WindowNoState
            && m_window->width() >= 700 && m_window->height() >= 600)
            m_normalGeometry = m_window->geometry();
        m_windowSaveTimer.start();
    };
    connect(window, &QWindow::xChanged, this, geometryChanged);
    connect(window, &QWindow::yChanged, this, geometryChanged);
    connect(window, &QWindow::widthChanged, this, geometryChanged);
    connect(window, &QWindow::heightChanged, this, geometryChanged);
    connect(window, &QWindow::windowStateChanged, this, [this](Qt::WindowState state) {
        if (m_fullscreenPending) { noteFullscreenGateSignal(); return; }
        if (state == Qt::WindowMaximized) m_windowMaximized = true;
        // A genuine user restore clears the flag; the unmaximize hop of a
        // fullscreen transition never reaches this handler because the gate
        // (see toggleWindowFullscreen) is open while it passes through.
        else if (state == Qt::WindowNoState) m_windowMaximized = false;
        m_windowSaveTimer.start();
    });
}

// #805 one-shot startup fit (see attachWindow): shrink and move the client so
// the decorated frame stays inside the screen's available geometry. Stops the
// poll on the first verdict: an undecorated window needs no fit, a deadline of
// 4 s covers a WM that never reports extents, and any state other than a
// settled normal window (maximized start, fullscreen pending) defers to the
// reference behaviour of restoring bounds unfitted.
void NativePreferences::fitFrameToAvailableGeometry()
{
    if (!m_window || m_fullscreenPending || m_windowMaximized) {
        m_frameFitTimer.stop();
        return;
    }
    if (m_window->windowState() != Qt::WindowNoState) return;
    const auto margins = m_window->frameMargins();
    if (margins.isNull()) { // undecorated: frame == client, nothing to fit
        if (m_frameFitDeadline.elapsed() > 4000) m_frameFitTimer.stop();
        return;
    }
    m_frameFitTimer.stop();
    const auto *screen = m_window->screen();
    if (!screen) return;
    const QRect available = screen->availableGeometry();
    const QRect client = m_window->geometry();
    QRect fitted = client;
    if (client.width() + margins.left() + margins.right() > available.width())
        fitted.setWidth(qMax(700, available.width() - margins.left() - margins.right()));
    if (client.height() + margins.top() + margins.bottom() > available.height())
        fitted.setHeight(qMax(600, available.height() - margins.top() - margins.bottom()));
    int x = fitted.x();
    if (x - margins.left() < available.x()) x = available.x() + margins.left();
    if (x + fitted.width() - 1 + margins.right() > available.right())
        x = available.right() + 1 - margins.right() - fitted.width();
    int y = fitted.y();
    if (y - margins.top() < available.y()) y = available.y() + margins.top();
    if (y + fitted.height() - 1 + margins.bottom() > available.bottom())
        y = available.bottom() + 1 - margins.bottom() - fitted.height();
    fitted.moveTopLeft(QPoint(x, y));
    if (fitted != client) m_window->setGeometry(fitted);
}

void NativePreferences::saveWindow()
{
    if (!m_window || !m_normalGeometry.isValid()) return;
    // #62 round 2 (review follow-up): this slot also fires immediately from
    // aboutToQuit and from the attachWindow(nullptr) detach, while a
    // fullscreen transition gate may still defer updates. The stored state
    // is then the pre-transition one, so re-capture the live window exactly
    // like the gate release does. Only the window's current state is read,
    // never the suppressed hop events, so the transient-hop protection
    // stays intact.
    if (m_fullscreenPending) recaptureLiveWindowState();
    m_windowSaveTimer.stop();
    m_settings.setValue("window/x", m_normalGeometry.x());
    m_settings.setValue("window/y", m_normalGeometry.y());
    m_settings.setValue("window/width", m_normalGeometry.width());
    m_settings.setValue("window/height", m_normalGeometry.height());
    m_settings.setValue("window/maximized", m_windowMaximized);
    m_settings.sync();
    if (m_settings.status() != QSettings::NoError) setError(tr("Window preferences could not be saved."));
}

void NativePreferences::activateWindow()
{
    if (!m_window) return;
    // QWindow::show() can normalize a hidden maximized window. Some window
    // managers hide it without reporting WindowMinimized to the application.
    if (m_window->windowState() == Qt::WindowFullScreen) m_window->showFullScreen();
    else if (m_windowMaximized) m_window->showMaximized();
    else m_window->showNormal();
    m_window->raise();
    m_window->requestActivate();
}

bool NativePreferences::eventFilter(QObject *watched, QEvent *event)
{
    // #62: F11 is the reference's only fullscreen surface on Linux/Windows -
    // the View menu's togglefullscreen role stays registered while the menu
    // bar is hidden (packages/electron/src/main.ts of the pinned revision).
    // Locale-independent: a key code, no new user-facing strings.
    if (watched == m_window && event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_F11 && key->modifiers() == Qt::NoModifier) {
            if (!key->isAutoRepeat()) toggleWindowFullscreen();
            return true;
        }
    }
    return false;
}

void NativePreferences::toggleWindowFullscreen()
{
    if (!m_window) return;
    // The WM hop between FullScreen and the previous state passes through
    // WindowNoState with transition geometry (unmaximize before entering,
    // restore before exiting). The gate keeps those transients out of the
    // saved normal geometry and the maximized flag - the reference
    // getNormalBounds rule - but it is directional: it closes the moment the
    // window has sat in the state this toggle asked for for the settle
    // interval (noteFullscreenGateSignal), and the cap only backstops a WM
    // that never settles. Either release re-captures the live state, so a
    // legitimate move or restore made right after the exit is persisted
    // instead of being dropped with the transition.
    const bool exiting = m_window->visibility() == QWindow::FullScreen;
    m_fullscreenPending = true;
    m_fullscreenAwaitingExit = exiting;
    m_fullscreenExitMaximized = exiting && m_windowMaximized;
    // #62 round 4 (review on the round-3 size guard): the release hop arrives
    // as WindowNoState still wearing the fullscreen shape. Snapshot that shape
    // now - the window is fullscreen at exactly this moment - and mark the
    // release restore in flight; recaptureLiveWindowState treats a
    // screen-filling frame as the hop only while that flag is set and the
    // frame equals the snapshot. A settled user window that happens to fill
    // the screen moves to a different shape and stays legitimate.
    m_fullscreenRestoreInFlight = exiting;
    m_fullscreenSnapshot = exiting ? m_window->geometry() : QRect();
    // #62 round 5 (review cid 4611449708): the enter side of the same rule.
    // Until the FullScreen state atom lands, geometry signals alone may be
    // the WM placing the window at the fullscreen shape, not the window
    // settling there; the enter flag keeps those frames out of the saved
    // normal geometry for normal windows too (recaptureLiveWindowState).
    m_fullscreenEnterInFlight = !exiting;
    m_fullscreenGateSettle.stop();
    m_fullscreenGateCap.start();
    if (exiting) {
        // The reference toggle returns the window to the state it had before
        // fullscreen: maximized returns maximized, normal returns at the
        // saved normal geometry.
        if (m_windowMaximized) m_window->showMaximized();
        else m_window->showNormal();
    } else {
        m_window->showFullScreen();
    }
}

void NativePreferences::noteFullscreenGateSignal()
{
    // Every state/geometry signal while the gate is open. The transition is
    // complete once the window reaches the state it was asked for and stops
    // moving for the settle interval; any further WM movement re-arms it.
    if (!m_window) { finishFullscreenGate(); return; }
    const auto state = m_window->windowState();
    // #62 round 6 (review cid 4612931182): the FullScreen state observation
    // is recorded the moment the signal arrives, not when recapture next
    // runs. Once the atom has been seen the enter is over: a later WM
    // restore is settled user geometry, so the enter flag must already be
    // retired when the immediate save recaptures it. A window leaving
    // fullscreen after the observation is then judged by the release rule,
    // so arm that rule from the first observation: Qt confirms the atom
    // locally before the fullscreen configure lands, so seed the shape with
    // the screen geometry and let live fullscreen-sized signals refine it.
    if (state == Qt::WindowFullScreen) {
        if (m_fullscreenEnterInFlight) m_fullscreenRestoreInFlight = true;
        m_fullscreenEnterInFlight = false;
        const auto *screen = m_window->screen();
        const auto geometry = m_window->geometry();
        const bool filled = screen
            && geometry.width() >= screen->geometry().width()
            && geometry.height() >= screen->geometry().height();
        if (filled) m_fullscreenSnapshot = geometry;
        else if (screen) m_fullscreenSnapshot = screen->geometry();
    }
    const bool reachedTarget = m_fullscreenAwaitingExit
        ? (m_fullscreenExitMaximized ? state == Qt::WindowMaximized : state == Qt::WindowNoState)
        : state == Qt::WindowFullScreen;
    if (reachedTarget) m_fullscreenGateSettle.start();
    else m_fullscreenGateSettle.stop();
}

void NativePreferences::finishFullscreenGate()
{
    m_fullscreenGateSettle.stop();
    m_fullscreenGateCap.stop();
    // #62 round 6 (review cid 4611449708): the gate release - the cap in
    // particular - must not clear the enter guard one statement before the
    // recapture below, or a fullscreen transition whose state transition
    // lags gets persisted as normal bounds by this very call. Only the
    // FullScreen state observation retires the guard (noteFullscreenGateSignal);
    // if that atom never arrived, the frame still lacks it and stays the
    // transition for the recapture below and for any later save on this gate.
    if (!m_fullscreenPending) return;
    m_fullscreenPending = false;
    if (!m_window) return;
    recaptureLiveWindowState();
    m_windowSaveTimer.start();
}

// #62 round 2: the one place that reads where the window actually is right
// now. Signals suppressed during the gate include legitimate user moves and
// restores that raced the transition; the live window is the only correct
// source, and a still-fullscreen or minimized window (cap expiry, or an
// immediate save mid-transition) keeps the saved normal geometry and
// maximized flag untouched. Used by the gate release and, through saveWindow,
// by immediate saves that land while the gate is still open.
void NativePreferences::recaptureLiveWindowState()
{
    if (!m_window) return;
    const auto state = m_window->windowState();
    if (state == Qt::WindowMaximized) {
        // Do not record geometry here: the state change can be confirmed
        // while the window still fills the screen; m_maximizedGeometry is
        // only ever fed by ungated geometryChanged signals on real bounds.
        m_windowMaximized = true;
        return;
    }
    if (state == Qt::WindowFullScreen) {
        // #62 round 4: keep the release snapshot fresh while the window is
        // actually fullscreen; the release hop frame equals this shape.
        m_fullscreenSnapshot = m_window->geometry();
        // #62 round 5: the fullscreen state atom has arrived; the enter is no
        // longer in flight, so later restores are judged as exits.
        m_fullscreenEnterInFlight = false;
        return;
    }
    if (state != Qt::WindowNoState) return;
    // #62 round 5 (review cid 4611449708): while an enter is in flight a
    // WindowNoState frame still lacks the FullScreen state atom - the WM may
    // deliver the fullscreen configure first - so it is the transition, not
    // settled normal bounds: persisting it would save the fullscreen rect as
    // the normal geometry (and dropping the maximized flag with it). This
    // covers normal windows; the maximized hop keeps its own guards below.
    if (m_fullscreenEnterInFlight) return;
    // #62 round 3 (review cid 4609403368) + round 4 (review on its size
    // guard): WindowNoState only describes a settled window when the window
    // is not still wearing a transition shape. The WM hop passes through
    // NoState in two shapes: entering fullscreen the WM unmaximizes onto the
    // very bounds the window had while maximized, and on release the state
    // change lands while the window still fills the screen, before the
    // restore configure arrives. Mirroring the reference getNormalBounds
    // rule, neither may enter the saved normal geometry or clear the
    // maximized flag - but a user-settled normal window may legitimately
    // fill the screen too, so size alone may not mark the hop. It is the hop
    // only while the release restore sequence is in flight AND the frame
    // still equals the fullscreen snapshot; a moved screen-filling frame is
    // the settled window, leaves the restore behind, and recaptures like
    // any other user change.
    const auto geometry = m_window->geometry();
    if (m_windowMaximized && m_maximizedGeometry.isValid() && geometry == m_maximizedGeometry)
        return;
    const auto *screen = m_window->screen();
    const bool screenFilling = screen
        && geometry.width() >= screen->geometry().width()
        && geometry.height() >= screen->availableGeometry().height();
    if (screenFilling) {
        // Release hop: still wearing the fullscreen snapshot while that
        // restore is in flight. Enter hop: a maximized window the WM is
        // unmaximizing onto the work area inside the enter gate.
        if (m_fullscreenRestoreInFlight && geometry == m_fullscreenSnapshot)
            return;
        if (m_windowMaximized && m_fullscreenPending && !m_fullscreenAwaitingExit)
            return;
    }
    m_fullscreenRestoreInFlight = false;
    if (geometry.width() >= 700 && geometry.height() >= 600) {
        m_windowMaximized = false;
        m_normalGeometry = geometry;
    }
}

namespace {
// mention-frecency.lib.ts: counts decay with a 14-day half-life; a decayed
// entry below 0.05 no longer changes any order, and at most 200 are kept.
constexpr double MentionFrecencyHalfLifeMs = 14.0 * 24 * 60 * 60 * 1000;
constexpr qsizetype MentionFrecencyMaxEntries = 200;
constexpr double MentionFrecencyMinKeptScore = 0.05;
constexpr qsizetype MentionFrecencyMaxBytes = 2 * 1024 * 1024;
const QRegularExpression validOwnerHash(QStringLiteral("^[0-9a-f]{64}$"));
const QRegularExpression mentionFrecencyUuidPattern(
    QStringLiteral("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"));

double mentionFrecencyDecay(double count, double ageMs)
{
    if (count <= 0) return 0;
    if (ageMs <= 0) return count;
    return count * std::pow(2.0, -ageMs / MentionFrecencyHalfLifeMs);
}

bool validMentionFrecencyEntry(const QJsonValue &value)
{
    if (!value.isObject()) return false;
    const auto entry = value.toObject();
    const auto count = entry.value("count");
    const auto lastUsedAt = entry.value("lastUsedAt");
    return count.isDouble() && count.toDouble() > 0 && lastUsedAt.isDouble();
}
} // namespace

qint64 NativePreferences::nowMs() const
{
    return m_nowMs ? m_nowMs() : QDateTime::currentMSecsSinceEpoch();
}

void NativePreferences::setNowProvider(std::function<qint64()> nowMs)
{
    m_nowMs = std::move(nowMs);
}

void NativePreferences::ensureMentionStore(const QString &owner)
{
    if (m_mentionStoreLoaded && m_mentionStoreOwner == owner) return;
    m_mentionStore = {};
    m_mentionStoreOwner = owner;
    m_mentionStoreLoaded = true;
    if (owner.isEmpty()) return;
    const auto raw = m_settings.value(QStringLiteral("mentions/owners/") + owner + "/frecency").toByteArray();
    if (raw.isEmpty() || raw.size() > MentionFrecencyMaxBytes) return;
    const auto parsed = QJsonDocument::fromJson(raw).object();
    for (auto it = parsed.begin(); it != parsed.end(); ++it)
    {
        if (!mentionFrecencyUuidPattern.match(it.key()).hasMatch()
            || !validMentionFrecencyEntry(it.value())) continue;
        m_mentionStore.insert(it.key(), it.value());
        if (m_mentionStore.size() >= MentionFrecencyMaxEntries) break;
    }
}

QVariantMap NativePreferences::mentionFrecencyFor(const QString &owner)
{
    // Decayed scores per user, ready to be fed into ranking; entries decayed
    // below the minimum drop out (loadMentionFrecency in the reference).
    ensureMentionStore(owner);
    const auto now = static_cast<double>(nowMs());
    QVariantMap scores;
    for (auto it = m_mentionStore.begin(); it != m_mentionStore.end(); ++it)
    {
        const auto entry = it.value().toObject();
        const auto score = mentionFrecencyDecay(entry.value("count").toDouble(),
            now - entry.value("lastUsedAt").toDouble());
        if (score >= MentionFrecencyMinKeptScore) scores.insert(it.key(), score);
    }
    return scores;
}

void NativePreferences::recordMentionPick(const QString &userUuid, const QString &owner)
{
    // The owner is the opaque per-account scope supplied by the caller
    // (NativeController::creationOwnerKey), a 64-hex hash on every transport.
    if (!validOwnerHash.match(owner).hasMatch()) return;
    if (!mentionFrecencyUuidPattern.match(userUuid).hasMatch()) return;
    ensureMentionStore(owner);
    const auto now = static_cast<double>(nowMs());
    QJsonObject entry;
    if (m_mentionStore.contains(userUuid))
    {
        const auto previous = m_mentionStore.value(userUuid).toObject();
        const auto decayed = mentionFrecencyDecay(previous.value("count").toDouble(),
            now - previous.value("lastUsedAt").toDouble());
        entry = QJsonObject{{"count", decayed + 1.0}, {"lastUsedAt", static_cast<qint64>(now)}};
    }
    else entry = QJsonObject{{"count", 1.0}, {"lastUsedAt", static_cast<qint64>(now)}};
    m_mentionStore.insert(userUuid, entry);
    persistMentionFrecency(owner);
    emit mentionFrecencyChanged();
}

void NativePreferences::persistMentionFrecency(const QString &owner)
{
    if (owner.isEmpty()) return;
    // Prune decayed entries and keep the strongest MentionFrecencyMaxEntries.
    const auto now = static_cast<double>(nowMs());
    QVector<QPair<QString, double>> scored;
    for (auto it = m_mentionStore.begin(); it != m_mentionStore.end(); ++it)
    {
        const auto entry = it.value().toObject();
        const auto score = mentionFrecencyDecay(entry.value("count").toDouble(),
            now - entry.value("lastUsedAt").toDouble());
        if (score >= MentionFrecencyMinKeptScore) scored.append({it.key(), score});
    }
    std::sort(scored.begin(), scored.end(), [](const auto &left, const auto &right) {
        return left.second > right.second;
    });
    if (scored.size() > MentionFrecencyMaxEntries) scored.resize(MentionFrecencyMaxEntries);
    QJsonObject store;
    for (const auto &kept : scored) store.insert(kept.first, m_mentionStore.value(kept.first));
    m_mentionStore = store;
    m_settings.setValue(QStringLiteral("mentions/owners/") + owner + "/frecency",
        QJsonDocument(m_mentionStore).toJson(QJsonDocument::Compact));
    m_settings.sync();
}

} // namespace workspace
