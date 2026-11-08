#include "native-preferences.h"
#include "domain.h"

#include <QGuiApplication>
#include <QLocale>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStyleHints>
#include <QScreen>
#include <QWindow>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
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

QString validPalette(const QString &palette)
{
    for (const auto *id : {"orange-warm", "blue-cold", "blue-mist", "emerald-chat"})
        if (palette == QLatin1String(id)) return palette;
    return QStringLiteral("blue-cold");
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
} // namespace

NativePreferences::NativePreferences(QString settingsFile, QObject *parent, std::function<QDate()> utcToday)
    : QObject(parent), m_settings(preferencesPath(settingsFile), QSettings::IniFormat),
      m_utcToday(std::move(utcToday))
{
    if (!m_utcToday) m_utcToday = [] { return QDateTime::currentDateTimeUtc().date(); };
    m_settings.setFallbacksEnabled(false);
    m_windowSaveTimer.setSingleShot(true);
    m_windowSaveTimer.setInterval(350);
    connect(&m_windowSaveTimer, &QTimer::timeout, this, &NativePreferences::saveWindow);
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &NativePreferences::saveWindow);
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
    const auto cacheLimit = m_settings.value("cache/limitGb", 10).toString();
    static const QRegularExpression integer("^[0-9]{1,4}\\z");
    if (integer.match(cacheLimit).hasMatch()) {
        const auto storedLimit = cacheLimit.toInt();
        if (storedLimit >= 1 && storedLimit <= 1000) m_cacheLimitGb = storedLimit;
    }
    loadAppearance();
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
        // Appearance emits synchronously. Every observer must already see this
        // owner's history, including while the language/theme is changing.
        loadReactionHistory();
        loadAppearance();
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

void NativePreferences::persist()
{
    const auto group = appearanceGroup();
    m_settings.setValue(group + "/palette", m_palette);
    m_settings.setValue(group + "/mode", m_mode);
    m_settings.setValue(group + "/locale", m_locale);
    m_settings.setValue("layout/sidebarWidth", m_sidebarWidth);
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

void NativePreferences::setLocale(const QString &locale)
{
    if ((locale != "en" && locale != "ru") || locale == m_locale) return;
    m_locale = locale;
    persist();
    emit localeChanged();
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
    }
    m_windowSaveTimer.stop();
    m_window = window;
    if (!window) return;
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
    const auto geometryChanged = [this] {
        if (!m_window) return;
        if (m_window->windowState() == Qt::WindowNoState && m_window->width() >= 700 && m_window->height() >= 600)
            m_normalGeometry = m_window->geometry();
        m_windowSaveTimer.start();
    };
    connect(window, &QWindow::xChanged, this, geometryChanged);
    connect(window, &QWindow::yChanged, this, geometryChanged);
    connect(window, &QWindow::widthChanged, this, geometryChanged);
    connect(window, &QWindow::heightChanged, this, geometryChanged);
    connect(window, &QWindow::windowStateChanged, this, [this](Qt::WindowState state) {
        if (state == Qt::WindowMaximized) m_windowMaximized = true;
        else if (state == Qt::WindowNoState) m_windowMaximized = false;
        m_windowSaveTimer.start();
    });
}

void NativePreferences::saveWindow()
{
    if (!m_window || !m_normalGeometry.isValid()) return;
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

} // namespace workspace
