#include "native-auth-guard.h"

namespace workspace {
namespace {
qint64 stubNowMs() { return 0; }
}

NativeAuthGuard::NativeAuthGuard(QObject *parent, std::function<qint64()> nowMs)
    : QObject(parent), m_nowMs(nowMs ? std::move(nowMs) : stubNowMs)
{
}

NativeAuthGuard::~NativeAuthGuard() = default;

qint64 NativeAuthGuard::timeoutMsForPreset(const QString &)
{
    return 0;
}

QString NativeAuthGuard::sanitizePreset(const QString &)
{
    return QStringLiteral("3d");
}

void NativeAuthGuard::setNowProvider(std::function<qint64()> nowMs)
{
    if (nowMs) m_nowMs = std::move(nowMs);
}

void NativeAuthGuard::setPollIntervalMs(int) {}
void NativeAuthGuard::setExpiryAction(std::function<void()>) {}
void NativeAuthGuard::setTimeoutPreset(const QString &) {}
void NativeAuthGuard::setSessionActive(bool) {}
void NativeAuthGuard::setWindowsVisible(bool) {}
void NativeAuthGuard::noteActivity() {}
bool NativeAuthGuard::active() const { return false; }
qint64 NativeAuthGuard::expiresAtMs() const { return 0; }
qint64 NativeAuthGuard::remainingMs() const { return 0; }

bool NativeAuthGuard::eventFilter(QObject *, QEvent *)
{
    return false;
}

} // namespace workspace
