#include "native-controller.h"
#include "native-deeplink.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#include <QRegularExpression>

namespace workspace {
namespace {
QString markdownReference(QString label, const QString &kind, const QString &id)
{
    label.replace('\\', QStringLiteral("\\\\"));
    label.replace('[', QStringLiteral("\\["));
    label.replace(']', QStringLiteral("\\]"));
    return QStringLiteral("[%1](urn:%2:%3)").arg(label, kind, id);
}
}

QVariantList NativeController::composerReferences(const QString &query) const
{
    QVariantList result;
    if (!composerMentionsEnabled()) return result;
    const auto filter = query.trimmed();
    for (const auto &stream : m_streams) {
        if (!stream.name.contains(filter, Qt::CaseInsensitive)) continue;
        result.append(QVariantMap{{"kind", "stream"}, {"stream", stream.uuid}, {"displayName", stream.name},
            {"markdown", markdownReference(stream.name, QStringLiteral("stream"), stream.uuid)}});
    }
    for (const auto &stream : m_streams) {
        const auto cached = m_inboxTopics.constFind(stream.uuid);
        if (stream.uuid != m_stream && (cached == m_inboxTopics.cend() || m_inboxStaleTopics.contains(stream.uuid))) continue;
        const auto &topics = stream.uuid == m_stream ? m_topics : cached.value();
        for (const auto &topic : topics) {
            if (topic.stream != stream.uuid || !(stream.name + ' ' + topic.name).contains(filter, Qt::CaseInsensitive)) continue;
            const auto label = stream.name + QStringLiteral(" › ") + topic.name;
            result.append(QVariantMap{{"kind", "topic"}, {"stream", stream.uuid}, {"topic", topic.uuid},
                {"displayName", label}, {"secondary", topic.name}, {"markdown", markdownReference(label, QStringLiteral("topic"), topic.uuid)}});
        }
    }
    return result;
}

QString NativeController::replaceComposerReferenceLinks(const QString &text) const
{
    static const QRegularExpression links(QStringLiteral(R"((?:https?://|ew://open/)[^\s<>()]+)"),
        QRegularExpression::CaseInsensitiveOption);
    auto matches = links.globalMatch(text);
    if (!matches.hasNext() || !composerMentionsEnabled()) return text;
    QHash<QString, QString> targets;
    for (const auto &entry : composerReferences({})) {
        const auto row = entry.toMap();
        auto path = QStringLiteral("/stream/") + row.value("stream").toString();
        if (row.contains("topic")) path += QStringLiteral("/topic/") + row.value("topic").toString();
        targets.insert(path, row.value("markdown").toString());
    }
    const auto origin = m_api->server();
    const auto prefix = QStringLiteral("/org/%1/project/%2").arg(orgRouteIdFromOrigin(origin), m_session->project());
    const auto port = [](const QUrl &url) { return url.port(url.scheme() == QLatin1String("https") ? 443 : 80); };
    QString result;
    qsizetype cursor = 0;
    while (matches.hasNext()) {
        const auto match = matches.next();
        auto candidate = match.capturedView();
        while (!candidate.isEmpty() && QStringLiteral(".,!?;:").contains(candidate.back())) candidate.chop(1);
        const QUrl url(candidate.toString(), QUrl::StrictMode);
        if (!url.isValid() || !url.userInfo().isEmpty()) continue;
        if (url.scheme() == QLatin1String("ew")) {
            if (!parseExternalDeepLink(candidate.toString())) continue;
        } else if (url.scheme() != origin.scheme() || url.host() != origin.host() || port(url) != port(origin)) continue;
        auto path = url.path(QUrl::FullyEncoded);
        if (!path.startsWith(prefix + QLatin1String("/stream/"))) continue;
        if (path.endsWith('/')) path.chop(1);
        const auto target = targets.constFind(path.mid(prefix.size()).toLower());
        if (target == targets.cend()) continue;
        result += QStringView(text).mid(cursor, match.capturedStart() - cursor);
        result += target.value();
        cursor = match.capturedStart() + candidate.size();
    }
    if (!cursor) return text;
    result += QStringView(text).mid(cursor);
    return result;
}

QString NativeController::composerReferenceClipboardText() const
{
    const auto *clipboard = QGuiApplication::clipboard();
    const auto *data = clipboard ? clipboard->mimeData() : nullptr;
    if (!data || !data->hasText()) return {};
    const auto text = data->text();
    const auto converted = replaceComposerReferenceLinks(text);
    return converted == text ? QString{} : converted;
}

void NativeController::openStreamReference(const QString &id)
{
    if (authenticated())
        applyStreamRoute(orgRouteIdFromOrigin(m_api->server()), m_session->project(), id, std::nullopt);
}
} // namespace workspace
