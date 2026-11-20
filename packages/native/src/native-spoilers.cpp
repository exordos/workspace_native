#include "native-spoilers.h"

#include <QRegularExpression>
#include <QUuid>
#include <QTextDocumentFragment>
#include <md4c-html.h>

namespace workspace {
namespace {
constexpr unsigned Dialect = MD_DIALECT_GITHUB | MD_FLAG_NOHTML | MD_FLAG_HARD_SOFT_BREAKS;

struct Fence {
    bool spoiler = false;
    QString title;
    QByteArray content;
};

QList<Fence> codeFences(const QByteArray &source)
{
    struct Scan { QList<Fence> fences; bool inSpoiler = false; } scan;
    MD_PARSER parser{};
    parser.flags = Dialect;
    parser.enter_block = [](MD_BLOCKTYPE type, void *detail, void *data) {
        auto &scan = *static_cast<Scan *>(data);
        if (type == MD_BLOCK_CODE) {
            const auto &code = *static_cast<MD_BLOCK_CODE_DETAIL *>(detail);
            const auto label = QString::fromUtf8(code.info.text, code.info.size).simplified();
            const auto language = QString::fromUtf8(code.lang.text, code.lang.size);
            scan.inSpoiler = language.compare(QStringLiteral("spoiler"), Qt::CaseInsensitive) == 0;
            scan.fences.append({scan.inSpoiler, label.mid(language.size()).trimmed(), {}});
        }
        return 0;
    };
    parser.leave_block = [](MD_BLOCKTYPE type, void *, void *data) {
        if (type == MD_BLOCK_CODE) static_cast<Scan *>(data)->inSpoiler = false;
        return 0;
    };
    parser.enter_span = parser.leave_span = [](MD_SPANTYPE, void *, void *) { return 0; };
    parser.text = [](MD_TEXTTYPE, const MD_CHAR *text, MD_SIZE len, void *data) {
        auto &scan = *static_cast<Scan *>(data);
        if (scan.inSpoiler) scan.fences.last().content.append(text, len);
        return 0;
    };
    if (md_parse(source.constData(), MD_SIZE(source.size()), &parser, &scan) != 0) return {};
    return scan.fences;
}

// md4c exposes escaped punctuation as its own normal-text event. Preserve the
// distinction from a delimiter through HTML export without touching code or URLs.
QString protectEscapedBars(const QByteArray &source, const QString &html, QByteArray &marker)
{
    if (!source.contains("\\|")) return html;
    struct Scan { const char *begin; QList<qsizetype> offsets; } scan{source.constData(), {}};
    MD_PARSER parser{};
    parser.flags = Dialect;
    parser.enter_block = parser.leave_block = [](MD_BLOCKTYPE, void *, void *) { return 0; };
    parser.enter_span = parser.leave_span = [](MD_SPANTYPE, void *, void *) { return 0; };
    parser.text = [](MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size, void *data) {
        auto &scan = *static_cast<Scan *>(data);
        if (type == MD_TEXT_NORMAL && size == 1 && *text == '|' && text > scan.begin && text[-1] == '\\') {
            qsizetype offset = text - scan.begin;
            qsizetype start = offset;
            while (start > 0 && scan.begin[start - 1] == '\\') --start;
            if ((offset - start) % 2) scan.offsets.append(offset - 1);
        }
        return 0;
    };
    if (md_parse(source.constData(), MD_SIZE(source.size()), &parser, &scan) != 0 || scan.offsets.isEmpty())
        return html;
    marker = QUuid::createUuid().toByteArray(QUuid::Id128);
    QByteArray protectedSource;
    qsizetype pos = 0;
    for (const auto offset : std::as_const(scan.offsets)) {
        protectedSource.append(source.constData() + pos, offset - pos);
        protectedSource += marker;
        pos = offset + 2;
    }
    protectedSource.append(source.constData() + pos, source.size() - pos);
    QByteArray generated;
    const auto append = [](const MD_CHAR *text, MD_SIZE size, void *out) {
        static_cast<QByteArray *>(out)->append(text, size);
    };
    if (md_html(protectedSource.constData(), MD_SIZE(protectedSource.size()), append, &generated, Dialect, 0) != 0)
        return html;
    return QString::fromUtf8(generated);
}

class Renderer {
public:
    const QSet<QString> &opened;
    bool expandAll;
    const QString &defaultLabel;
    SpoilerLinks &links;

    QString anchor(const QString &id, const QString &label, bool masked)
    {
        const auto href = links.prefix + id;
        links.ids.insert(href, id);
        const QString style = masked ? QStringLiteral("color:#84909c;background-color:#84909c;") : QString();
        return QStringLiteral("<a href=\"%1\" style=\"text-decoration:none;%2\">%3</a>").arg(href, style, label);
    }

    QString render(const QByteArray &source, const QString &html, const QString &parent = {}, int depth = 0)
    {
        // Only renderer-generated tags appear here: user HTML is escaped by md4c.
        static const QRegularExpression tokens(QStringLiteral(
            R"(<pre><code(?: [^>]*)?>[\s\S]*?</code></pre>|<[^>]+>|[^<]+)"));
        static const QRegularExpression bars(QStringLiteral(R"(\|\|(.*?)\|\|)"),
            QRegularExpression::DotMatchesEverythingOption);
        const auto fences = source.toLower().contains("spoiler") ? codeFences(source) : QList<Fence>{};
        QString out;
        int fenceIndex = 0, occurrence = 0;
        int literalDepth = 0;
        QByteArray escapedBar;
        const QString parsed = protectEscapedBars(source, html, escapedBar);
        for (auto it = tokens.globalMatch(parsed); it.hasNext();) {
            const auto token = it.next().captured();
            if (token.startsWith(QLatin1String("<pre><code"))) {
                const auto fence = fences.value(fenceIndex++);
                if (!fence.spoiler || depth >= 16) { out += token; continue; }
                const auto id = parent + QString::number(++occurrence);
                const bool open = expandAll || opened.contains(id);
                links.headers.insert(links.prefix + id);
                const auto title = (fence.title.isEmpty() ? defaultLabel : fence.title).toHtmlEscaped();
                out += QStringLiteral("<p><strong>") + anchor(id,
                    (open ? QStringLiteral("▼ ") : QStringLiteral("▶ ")) + title, false) + QStringLiteral("</strong></p>");
                if (open) {
                    QByteArray body;
                    const auto append = [](const MD_CHAR *t, MD_SIZE n, void *dst) {
                        static_cast<QByteArray *>(dst)->append(t, n);
                    };
                    if (md_html(fence.content.constData(), MD_SIZE(fence.content.size()), append, &body, Dialect, 0) == 0)
                        out += render(fence.content, QString::fromUtf8(body), id + QLatin1Char('.'), depth + 1);
                }
            } else if (token.startsWith(QLatin1Char('<'))) {
                if (token.startsWith(QLatin1String("<code>")) || token.startsWith(QLatin1String("<a "))) ++literalDepth;
                if (token == QLatin1String("</code>") || token == QLatin1String("</a>")) --literalDepth;
                out += token;
            } else if (literalDepth == 0 && token.contains(QLatin1String("||"))) {
                qsizetype pos = 0;
                for (auto spans = bars.globalMatch(token); spans.hasNext();) {
                    const auto span = spans.next();
                    out += token.mid(pos, span.capturedStart() - pos);
                    auto content = span.captured(1);
                    if (!escapedBar.isEmpty()) content.replace(QString::fromLatin1(escapedBar), QLatin1String("|"));
                    if (content.isEmpty()) {
                        out += span.captured();
                    } else {
                        const auto id = parent + QString::number(++occurrence);
                        const bool open = expandAll || opened.contains(id);
                        const auto length = QTextDocumentFragment::fromHtml(content).toPlainText().size();
                        out += anchor(id, open ? content : QString(length, QChar(0x2588)), !open);
                    }
                    pos = span.capturedEnd();
                }
                out += token.mid(pos);
            } else {
                out += token;
            }
        }
        if (!escapedBar.isEmpty()) out.replace(QString::fromLatin1(escapedBar), QLatin1String("|"));
        return out;
    }
};
}

QByteArray renderSpoilers(const QString &markdown, const QByteArray &html,
    const QSet<QString> &opened, bool expandAll, const QString &defaultLabel, SpoilerLinks &links)
{
    return Renderer{opened, expandAll, defaultLabel, links}
        .render(markdown.toUtf8(), QString::fromUtf8(html)).toUtf8();
}
}
