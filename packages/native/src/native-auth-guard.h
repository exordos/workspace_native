#pragma once

#include <QObject>
#include <QString>

#include <functional>

namespace workspace {

// Issue #165: native counterpart of the pinned web contract
// (packages/web/src/shared/lib/auth-guard.ts and
// packages/web/src/widgets/layout/layout-auth-guard.hook.ts at the sha pinned
// in docs/NATIVE_DESKTOP_AUTH_IDLE_TIMEOUT.md). Owns the inactivity deadline
// for the live session: a QTimer-driven check against an injectable clock,
// re-armed by user activity, paused while no window is visible/exposed, and an
// injected expiry action wired to the controller's regular sign-out path.
class NativeAuthGuard final : public QObject {
    Q_OBJECT
public:
    explicit NativeAuthGuard(QObject *parent = nullptr, std::function<qint64()> nowMs = {});
    ~NativeAuthGuard() override;

    // auth-idle-timeout.lib.ts:10-26: preset to milliseconds; a disabled or
    // unknown preset returns -1 (the reference maps `never` to null).
    static qint64 timeoutMsForPreset(const QString &preset);
    // resolveAuthIdleTimeout fallback (auth-idle-timeout.lib.ts:40-41): any
    // value outside the preset list resolves to the settings default "3d".
    static QString sanitizePreset(const QString &value);

    void setNowProvider(std::function<qint64()> nowMs);
    // Test/embedder seam for the deadline check cadence; production is 1000 ms.
    void setPollIntervalMs(int milliseconds);
    void setExpiryAction(std::function<void()> action);

    // Mirrors the layout hook effect: a preset change tears the previous
    // deadline down exactly once and re-arms immediately; "never" cancels only.
    void setTimeoutPreset(const QString &preset);
    // The guard is armed only while a live session exists (the reference starts
    // it only for a current instance with ready/degraded user status).
    void setSessionActive(bool active);
    // Explicit counterpart of document.hidden; production tracks top-level
    // window visibility itself and tests embedders drive this directly.
    void setWindowsVisible(bool visible);

    // Reference resetTimer: every activity event re-arms the full timeout.
    void noteActivity();

    bool active() const;
    // auth-guard.ts:217-227 read-only observers; 0 while the guard is inactive.
    qint64 expiresAtMs() const;
    qint64 remainingMs() const;

    bool eventFilter(QObject *watched, QEvent *event) override;

signals:
    void activeChanged();

private:
    std::function<qint64()> m_nowMs;
};

} // namespace workspace
