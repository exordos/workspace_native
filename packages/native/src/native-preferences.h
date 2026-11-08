#pragma once

#include <QObject>
#include <QSettings>
#include <QPointer>
#include <QRect>
#include <QTimer>
#include <QDate>
#include <QJsonObject>
#include <QVariantList>
#include <functional>

class QWindow;

namespace workspace {

class NativePreferences final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString paletteId READ paletteId WRITE setPaletteId NOTIFY paletteIdChanged)
    Q_PROPERTY(QString mode READ mode WRITE setMode NOTIFY modeChanged)
    Q_PROPERTY(QString locale READ locale WRITE setLocale NOTIFY localeChanged)
    Q_PROPERTY(bool dark READ dark NOTIFY darkChanged)
    Q_PROPERTY(qreal sidebarPreferredWidth READ sidebarPreferredWidth WRITE setSidebarPreferredWidth NOTIFY sidebarPreferredWidthChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(int cacheLimitGb READ cacheLimitGb WRITE setCacheLimitGb NOTIFY cacheLimitGbChanged)
    Q_PROPERTY(QString cacheError READ cacheError NOTIFY cacheErrorChanged)
    Q_PROPERTY(bool cacheBusy READ cacheBusy NOTIFY cacheStatusChanged)
    Q_PROPERTY(bool cacheAvailable READ cacheAvailable NOTIFY cacheStatusChanged)
    Q_PROPERTY(qint64 cacheAppliedLimitBytes READ cacheAppliedLimitBytes NOTIFY cacheStatusChanged)
    Q_PROPERTY(qint64 cacheUsageBytes READ cacheUsageBytes NOTIFY cacheStatusChanged)
    Q_PROPERTY(QVariantList suggestedReactions READ suggestedReactions NOTIFY reactionHistoryChanged)
    Q_PROPERTY(QVariantMap quickReactionUsage READ quickReactionUsage NOTIFY reactionHistoryChanged)
public:
    explicit NativePreferences(QString settingsFile = {}, QObject *parent = nullptr,
                               std::function<QDate()> utcToday = {});
    QString paletteId() const { return m_palette; }
    QString mode() const { return m_mode; }
    QString locale() const { return m_locale; }
    bool dark() const;
    qreal sidebarPreferredWidth() const { return m_sidebarWidth; }
    QString error() const { return m_error; }
    int cacheLimitGb() const { return m_cacheLimitGb; }
    QString cacheError() const { return m_cacheError; }
    bool cacheBusy() const { return m_cacheBusy; }
    bool cacheAvailable() const { return m_cacheAvailable; }
    qint64 cacheAppliedLimitBytes() const { return m_cacheAppliedLimitBytes; }
    qint64 cacheUsageBytes() const { return m_cacheUsageBytes; }
    QVariantList suggestedReactions() const;
    QVariantMap quickReactionUsage() const;
    void setOwner(const QString &owner);
    // Explicit settings I/O actions; never called from a QML/read-only getter.
    bool loadOwnStatusAway(const QString &owner);
    void recordOwnStatusAway(const QString &owner, bool away);
    void setPaletteId(const QString &palette);
    void setMode(const QString &mode);
    void setLocale(const QString &locale);
    void setCacheLimitGb(int gigabytes);
    void reportCacheError(const QString &error);
    void reportCacheStatus(bool busy, bool available, qint64 appliedLimitBytes, qint64 usageBytes);
    void setSidebarPreferredWidth(qreal width);
    Q_INVOKABLE void toggleMode();
    Q_INVOKABLE void recordSuggestedReaction(const QString &name, const QString &expectedOwner);
    Q_INVOKABLE void recordQuickReactionUse(const QString &name, const QString &expectedOwner);
    void attachWindow(QWindow *window);
    bool windowMaximized() const { return m_windowMaximized; }
    void activateWindow();

signals:
    void paletteIdChanged();
    void modeChanged();
    void localeChanged();
    void darkChanged();
    void sidebarPreferredWidthChanged();
    void errorChanged();
    void cacheLimitGbChanged();
    void cacheErrorChanged();
    void cacheStatusChanged();
    void reactionHistoryChanged();

private:
    QString appearanceGroup() const;
    void loadAppearance();
    void persist();
    void setError(const QString &error);
    void loadReactionHistory();
    void persistReactionHistory();
    QJsonObject recentReactionUsage(const QJsonObject &days) const;
    void saveWindow();
    QSettings m_settings;
    QString m_owner;
    QString m_palette = QStringLiteral("blue-cold");
    QString m_mode = QStringLiteral("system");
    QString m_locale = QStringLiteral("en");
    QString m_error;
    QString m_cacheError;
    bool m_cacheBusy = false;
    bool m_cacheAvailable = false;
    qint64 m_cacheAppliedLimitBytes = 0;
    qint64 m_cacheUsageBytes = 0;
    int m_cacheLimitGb = 10;
    QVariantList m_suggested;
    QJsonObject m_quickUsage;
    std::function<QDate()> m_utcToday;
    qreal m_sidebarWidth = 299;
    bool m_systemDark = false;
    QPointer<QWindow> m_window;
    QTimer m_windowSaveTimer;
    QRect m_normalGeometry;
    bool m_windowMaximized = false;
};

} // namespace workspace
