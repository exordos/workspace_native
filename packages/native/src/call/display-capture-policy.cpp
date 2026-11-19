#include "call/display-capture-policy.h"

namespace workspace {

QString displayCaptureOrigin(const QUrl &url)
{
    if (!url.isValid())
        return {};
    const QString scheme = url.scheme().toLower();
    const QString host = url.host().toLower();
    if (scheme.isEmpty() || host.isEmpty())
        return {}; // no authority: the reference getUrlOrigin yields null here
    // Keep IPv6 hosts bracketed the way URL origin serialization does.
    const QString bracketed = host.contains(QLatin1Char(':')) && !host.startsWith(QLatin1Char('['))
        ? QLatin1Char('[') + host + QLatin1Char(']')
        : host;
    const int port = url.port(-1);
    const bool defaultPort = port == -1
        || (scheme == QLatin1String("https") && port == 443)
        || (scheme == QLatin1String("http") && port == 80);
    QString origin = scheme + QLatin1String("://") + bracketed;
    if (!defaultPort)
        origin += QLatin1Char(':') + QString::number(port);
    return origin;
}

bool isAllowedDisplayCapture(bool isMainFrame, const QUrl &requestingUrl,
    const QList<DisplayCaptureFrame> &frames)
{
    // 1. The shell's own main frame never obtains desktop capture
    // (main.ts:1003). This holds even for a request carrying the meeting origin.
    if (isMainFrame)
        return false;
    // 2 and 3. Parse failure or a non-https scheme denies (main.ts:1007-1009).
    if (requestingUrl.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) != 0)
        return false;
    const QString origin = displayCaptureOrigin(requestingUrl);
    if (origin.isEmpty())
        return false;
    // 4. A live (non-detached) frame in the subtree must carry that exact
    // origin (main.ts:1011-1016); a detached frame is not live.
    for (const auto &frame : frames) {
        if (frame.detached)
            continue;
        if (displayCaptureOrigin(frame.url) == origin)
            return true;
    }
    return false;
}

} // namespace workspace
